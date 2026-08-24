#include <coreobjects/ownable_ptr.h>
#include <coreobjects/property_object_factory.h>
#include <coretypes/validation.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/event_packet_utils.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reader_errors.h>
#include <opendaq/reader_status_impl.h>
#include <opendaq/reader_utils.h>
#include <opendaq/tags_private_ptr.h>

#include <date/date.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <chrono>
#include <limits>

using namespace std::chrono;

BEGIN_NAMESPACE_OPENDAQ

// The multi reader internals (Input, QueueReader, SynchronizationManager, ReadCoordinator,
// NotificationCoordinator, DataLossMonitor) live in daq::multi_reader
using namespace multi_reader;

// --- Construction -----------------------------------------------------------------------------

namespace
{

// The legacy list factories delegate through here so the builder constructor is the single
// wiring path. The builder's defaults already match the legacy behavior (SameThread
// notifications, no main input, no sync distance, no data-loss timeout).
MultiReaderBuilderPtr builderFromLegacyArgs(const ListPtr<IComponent>& list,
                                            SampleType valueReadType,
                                            SampleType domainReadType,
                                            ReadMode mode,
                                            Int requiredCommonSampleRate,
                                            Bool startOnFullUnitOfDomain,
                                            SizeT minReadCount)
{
    if (!list.assigned())
        DAQ_THROW_EXCEPTION(NotAssignedException, "List of inputs is not assigned");

    auto builder = MultiReaderBuilder();
    for (const auto& component : list)
    {
        if (auto signal = component.asPtrOrNull<ISignal>(); signal.assigned())
            builder.addSignal(signal);
        else if (auto port = component.asPtrOrNull<IInputPort>(); port.assigned())
            builder.addInputPort(port);
        else
            DAQ_THROW_EXCEPTION(InvalidParameterException, "One of the elements of input list is not signal or input port");
    }

    builder.setValueReadType(valueReadType);
    builder.setDomainReadType(domainReadType);
    builder.setReadMode(mode);
    builder.setRequiredCommonSampleRate(requiredCommonSampleRate);
    builder.setStartOnFullUnitOfDomain(startOnFullUnitOfDomain);
    builder.setMinReadCount(minReadCount);
    return builder;
}

}  // namespace

MultiReaderImpl::MultiReaderImpl(const ListPtr<IComponent>& list,
                                 SampleType valueReadType,
                                 SampleType domainReadType,
                                 ReadMode mode,
                                 ReadTimeoutType /*timeoutType*/,  // only All is honored
                                 Int requiredCommonSampleRate,
                                 Bool startOnFullUnitOfDomain,
                                 SizeT minReadCount)
    : MultiReaderImpl(builderFromLegacyArgs(list, valueReadType, domainReadType, mode, requiredCommonSampleRate, startOnFullUnitOfDomain, minReadCount))
{
}

MultiReaderImpl::MultiReaderImpl(MultiReaderImpl* old, SampleType valueReadType, SampleType domainReadType)
    : valueReadType(valueReadType)
    , domainReadType(domainReadType)
{
    ListPtr<IInputPortConfig> ports = List<IInputPortConfig>();
    std::vector<bool> usedFlags;
    std::vector<DataDescriptorPtr> oldValueDescriptors;
    std::vector<DataDescriptorPtr> oldDomainDescriptors;

    {
        std::scoped_lock lock(old->mutex);
        old->invalid = true;
        // Keep the invariant invalid <=> Error: statuses derive their validity from the state
        old->setStateLocked(ReaderState::Error, "Reader was invalidated by MultiReaderFromExisting");

        loggerComponent = old->loggerComponent;
        readMode = old->readMode;
        typeOfInputs = old->typeOfInputs;
        portBinder = old->portBinder;
        startOnFullUnitOfDomain = old->startOnFullUnitOfDomain;
        isActive = old->isActive;
        minReadCount = old->minReadCount;
        tickOffsetTolerance = old->tickOffsetTolerance;
        mainInputId = old->mainInputId;
        maxSynchronizationDistance = old->maxSynchronizationDistance;
        dataLossTimeout = old->dataLossTimeout;
        requiredCommonSampleRate = old->requiredCommonSampleRate;
        allowDifferentRates = old->allowDifferentRates;
        notificationMethod = old->notificationMethod;
        notificationMethodsList = old->notificationMethodsList;
        context = old->context;
        externalListener = old->externalListener;
        readCallback = std::move(old->readCallback);

        for (auto* slot : old->slots)
        {
            ports.pushBack(slot->getPort());
            usedFlags.push_back(slot->isUsed());
            // The initial descriptor events were consumed by the old reader; the new
            // per-input readers adopt the active descriptors instead
            oldValueDescriptors.push_back(slot->getQueueReader().getValueDescriptor());
            oldDomainDescriptors.push_back(slot->getQueueReader().getDomainDescriptor());
        }
    }

    resolvedDomainReadType = domainReadType == SampleType::Undefined ? SampleType::Int64 : domainReadType;

    this->internalAddRef();
    try
    {
        syncManager = std::make_unique<SynchronizationManager>(loggerComponent);
        readCoordinator = std::make_unique<ReadCoordinator>(loggerComponent);
        notificationCoordinator = std::make_unique<NotificationCoordinator>(context.getScheduler(), loggerComponent);
        notificationCoordinator->setEvaluationCallback([this] { onCoalescedEvaluation(); });
        dataLossMonitor = std::make_unique<DataLossMonitor>();
        dataLossMonitor->setDeadlineCallback(
            [this]
            {
                // Deadlines enter the same coalesced evaluation path as packets
                notificationCoordinator->requestEvaluation();
                notifyCondition.notify_all();
            });
        applyConfigToSyncManager();

        createSlots(ports);

        {
            std::lock_guard lock(mutex);
            const auto ownerPass = notificationCoordinator->beginOwnerPass();
            for (SizeT i = 0; i < usedFlags.size() && i < slots.size(); ++i)
            {
                applySlotUsedLocked(slots[i], usedFlags[i]);
                slots[i]->getQueueReader().seedDescriptors(oldValueDescriptors[i], oldDomainDescriptors[i]);
            }
            applyDataLossTimeoutLocked();
            evaluateStateLocked();
        }
        replaySlotCallbacks();
    }
    catch (...)
    {
        this->releaseWeakRefOnException();
        throw;
    }
}

MultiReaderImpl::MultiReaderImpl(const MultiReaderBuilderPtr& builder)
    : tickOffsetTolerance(builder.getTickOffsetTolerance())
    , requiredCommonSampleRate(builder.getRequiredCommonSampleRate())
    , allowDifferentRates(builder.getAllowDifferentSamplingRates())
    , startOnFullUnitOfDomain(builder.getStartOnFullUnitOfDomain())
    , minReadCount(builder.getMinReadCount())
    , notificationMethod(builder.getInputPortNotificationMethod())
    , notificationMethodsList(builder.getInputPortNotificationMethods())
    , valueReadType(builder.getValueReadType())
    , domainReadType(builder.getDomainReadType())
    , readMode(builder.getReadMode())
{
    internalAddRef();
    try
    {
        auto sourceComponents = builder.getSourceComponents();
        normalizeSources(sourceComponents);

        loggerComponent = context.getLogger().getOrAddComponent("MultiReader");

        // Deprecated: the value is ignored; kept on the builder for compatibility
        if (tickOffsetTolerance.assigned() && tickOffsetTolerance.getNumerator() != 0)
        {
            LOG_W("MultiReaderBuilder::setTickOffsetTolerance is deprecated and its value is ignored; "
                  "use setMaxSynchronizationDistance instead");
        }

        mainInputId = builder.getMainInput();
        if (mainInputId.assigned() && mainInputId.getLength() == 0)
            mainInputId = nullptr;
        maxSynchronizationDistance = builder.getMaxSynchronizationDistance();
        dataLossTimeout = builder.getDataLossTimeout();

        resolvedDomainReadType = domainReadType == SampleType::Undefined ? SampleType::Int64 : domainReadType;

        syncManager = std::make_unique<SynchronizationManager>(loggerComponent);
        readCoordinator = std::make_unique<ReadCoordinator>(loggerComponent);
        notificationCoordinator = std::make_unique<NotificationCoordinator>(context.getScheduler(), loggerComponent);
        notificationCoordinator->setEvaluationCallback([this] { onCoalescedEvaluation(); });
        dataLossMonitor = std::make_unique<DataLossMonitor>();
        dataLossMonitor->setDeadlineCallback(
            [this]
            {
                // Deadlines enter the same coalesced evaluation path as packets
                notificationCoordinator->requestEvaluation();
                notifyCondition.notify_all();
            });
        applyConfigToSyncManager();

        auto ports = createOrAdoptPorts(sourceComponents);
        createSlots(ports);

        {
            std::lock_guard lock(mutex);
            const auto ownerPass = notificationCoordinator->beginOwnerPass();
            if (mainInputId.assigned() && findSlotByIdLocked(mainInputId) == notFound)
                DAQ_THROW_EXCEPTION(NotFoundException, "The selected main input does not match any source component");
            // Adopted ports may arrive deactivated (a previous owner parked them via
            // setInputUsed(false)); their active state belongs to this reader now
            setPortsActiveLocked(isActive);
            applyDataLossTimeoutLocked();
            evaluateStateLocked();
        }
        replaySlotCallbacks();
    }
    catch (...)
    {
        this->releaseWeakRefOnException();
        throw;
    }
}

MultiReaderImpl::~MultiReaderImpl()
{
    if (dataLossMonitor)
        dataLossMonitor->detach();
    if (notificationCoordinator)
        notificationCoordinator->detach();
    for (auto* slot : slots)
        slot->detachListener();

    if (!portBinder.assigned())
    {
        for (auto* slot : slots)
            slot->getPort().remove();
    }
}

void MultiReaderImpl::normalizeSources(const ListPtr<IComponent>& list)
{
    if (!list.assigned())
        DAQ_THROW_EXCEPTION(NotAssignedException, "List of inputs is not assigned");
    if (list.getCount() == 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Need at least one signal.");

    if (!context.assigned())
        context = list[0].getContext();

    // The input type is determined once and only ever narrows from Unknown; per-element
    // homogeneity (no mixing of signals and ports) is enforced by createOrAdoptPorts
    if (typeOfInputs == InputType::Unknown)
    {
        if (list[0].supportsInterface(IInputPort::Id))
            typeOfInputs = InputType::Ports;
        else if (list[0].supportsInterface(ISignal::Id))
            typeOfInputs = InputType::Signals;
        else
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Invalid component type, only IInputPort and ISignal are supported.");
    }
}

ListPtr<IInputPortConfig> MultiReaderImpl::createOrAdoptPorts(const ListPtr<IComponent>& list) const
{
    auto portList = List<IInputPortConfig>();
    for (const auto& el : list)
    {
        if (auto signal = el.asPtrOrNull<ISignal>(); signal.assigned())
        {
            if (typeOfInputs == InputType::Ports)
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot pass both input ports and signals as items");

            auto port = InputPort(context, nullptr, fmt::format("multi_reader_signal_{}", signal.getLocalId()));
            port.getTags().asPtr<ITagsPrivate>().add("MultiReaderInternalPort");

            port.connect(signal);
            portList.pushBack(port);
        }
        else if (auto port = el.asPtrOrNull<IInputPortConfig>(); port.assigned())
        {
            if (typeOfInputs == InputType::Signals)
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot pass both input ports and signals as items");

            portList.pushBack(port);
        }
        else
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "One of the elements of input list is not signal or input port");
        }
    }

    return portList;
}

void MultiReaderImpl::createSlots(const ListPtr<IInputPortConfig>& inputPorts)
{
    if (notificationMethodsList.assigned() && notificationMethodsList.getCount() > 0 &&
        notificationMethodsList.getCount() != inputPorts.getCount())
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "The list of source components is not of same size than the list of notification methods.");
    }

    const SizeT firstPosition = slots.size();
    SizeT position = firstPosition;
    for (const auto& port : inputPorts)
    {
        if (!port.getTags().contains("MultiReaderInternalPort"))
        {
            if (!portBinder.assigned())
                portBinder = PropertyObject();
            port.asPtr<IOwnable>().setOwner(portBinder);
        }

        const auto portNotificationMethod = notificationMethodsList.assigned() && notificationMethodsList.getCount() > 0
                                                ? notificationMethodsList[position - firstPosition]
                                                : notificationMethod;

        if (portNotificationMethod != PacketReadyNotification::Unspecified)
        {
            port.setNotificationMethod(portNotificationMethod);
        }
        else if (typeOfInputs == InputType::Signals)
        {
            // A reader built from signals owns its ports and must pick a concrete method;
            // port-constructed readers may keep the port's existing setting.
            DAQ_THROW_EXCEPTION(InvalidParameterException,
                                "Multi reader created from signals cannot have an unspecified input port notification method.");
        }

        auto slotObject = createWithImplementation<IInputPortNotifications, Input>(position,
                                                                                       port,
                                                                                       valueReadType,
                                                                                       resolvedDomainReadType,
                                                                                       readMode,
                                                                                       loggerComponent,
                                                                                       static_cast<IInputListener*>(this),
                                                                                       typeOfInputs == InputType::Signals,
                                                                                       notificationCoordinator->gate());
        auto* slot = static_cast<Input*>(slotObject.getObject());
        // Slots default to used; the gate's used count follows the slot set
        notificationCoordinator->gate()->adjustUsed(1);
        // Listening must happen here, before any evaluation drains the connection: setListener
        // front-loads the connection's cached descriptor, which has to sit AHEAD of data already
        // queued behind it (see Input::listen). Note this also means an already-connected port
        // ends up with a SECOND initial descriptor event - SignalImpl::listenerConnected enqueued
        // one at connect time, which made ConnectionImpl::onPacketEnqueued cache the descriptors,
        // and enqueueLastDescriptor now front-loads those same descriptors again. Both are
        // identical and consuming a descriptor event is idempotent, so it is harmless, but the
        // reader does observe two leading events per input - which matters to anything counting
        // events rather than acting on them.
        slot->listen(slotObject);

        slotObjects.push_back(std::move(slotObject));
        slots.push_back(slot);
        ++position;
    }

    dataLossMonitor->resize(slots.size());
}

void MultiReaderImpl::replaySlotCallbacks(SizeT firstSlot)
{
    // Must run WITHOUT the state lock: the replayed connected()/packetReceived() callbacks re-enter
    // this reader through slotConnected/slotPacketReceived, which take the lock themselves.
    for (SizeT i = firstSlot; i < slots.size(); ++i)
        slots[i]->replayMissedPortCallbacks();
}

namespace
{

std::chrono::system_clock::duration ratioSecondsToDuration(const RatioPtr& seconds)
{
    if (!seconds.assigned() || seconds.getDenominator() == 0 || seconds.getNumerator() <= 0)
        return {};
    const auto secs = static_cast<double>(seconds.getNumerator()) / static_cast<double>(seconds.getDenominator());
    return std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(secs));
}

}  // namespace

void MultiReaderImpl::applyConfigToSyncManager()
{
    syncManager->setRequiredCommonSampleRate(requiredCommonSampleRate);
    syncManager->setAllowDifferentRates(allowDifferentRates);
    syncManager->setStartOnFullUnitOfDomain(startOnFullUnitOfDomain);
    syncManager->setMaxSynchronizationDistance(ratioSecondsToDuration(maxSynchronizationDistance));
}

void MultiReaderImpl::applyDataLossTimeoutLocked()
{
    const auto duration = ratioSecondsToDuration(dataLossTimeout);
    dataLossMonitor->setTimeout(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
}

void MultiReaderImpl::onExitLocked(ReaderBehavior state){}
void MultiReaderImpl::onEnterLocked(ReaderBehavior state){}

void MultiReaderImpl::applyTransitionLocked(const ReaderStateTransition& transition)
{
    onExitLocked(currentBehavior);

    currentBehavior = transition.target;
    lastTrigger = transition.trigger;
    currentFault = transition.fault;

    onEnterLocked(transition.target);
}

ReaderStateTransition MultiReaderImpl::nextStateLocked()
{
    switch (currentBehavior)
    {
    case ReaderBehavior::Establishing:
        return evaluateEstablishingLocked();
    case ReaderBehavior::Synchronizing:
        return evaluateSynchronizingLocked();
    case ReaderBehavior::Ready:
        return evaluateReadyLocked();
    case ReaderBehavior::Error:
        return evaluateErrorLocked();
    }
    return {ReaderBehavior::Error, TransitionTrigger::Settled, std::nullopt};
}

void MultiReaderImpl::settleStateLocked(const ReaderStateTransition& transition)
{
    ReaderStateTransition nextTransition = transition;
    uint8_t iterations = 0;
    constexpr uint8_t MAX_ITERATIONS = 8;

    while (nextTransition.trigger != TransitionTrigger::Settled)
    {
        ++iterations;
        if (iterations > MAX_ITERATIONS)
        {
            // Something must have went wrong for the number of iterations to have been exceeded.
            nextTransition = {ReaderBehavior::Error, TransitionTrigger::Settled, std::nullopt};
            applyTransitionLocked(nextTransition);
            break;
        }

        applyTransitionLocked(nextTransition);
        nextTransition = nextStateLocked();
    }

    // Override the fault with what we found in staying case, unless the previous
    // fault was specified and needed user interaction (couldn't have been cleared by this point)
    if (currentBehavior != ReaderBehavior::Error)
    {
        currentFault = nextTransition.fault;
    }
}

// --- Stage helpers ------------------------------------------------------------------------------

Fault MultiReaderImpl::faultWithCulprits(FaultType type,
                                         const char* detailPrefix,
                                         const char* detailSuffix,
                                         std::vector<SizeT> culprits)
{
    auto detail = fmt::format("{} [{}]{}", detailPrefix, fmt::join(culprits, ", "), detailSuffix);
    return {type, std::move(detail), std::move(culprits)};
}

void MultiReaderImpl::drainUnusedSlotsLocked()
{
    // Unused inputs stay observable. Their ports are inactive, so data is dropped at the
    // connection and only events can arrive; draining them makes pending events visible in the
    // per-input states and lets them fire the dataAvailable callback.
    for (auto* slot : slots)
    {
        if (slot->isUsed())
            continue;

        slot->adoptQueuedPackets();
        if (!slot->isConnected())
        {
            slot->publishAvailability(0, false);
            setSlotEventLocked(slot, false);
            continue;
        }

        slot->clearPacketPending();
        auto& reader = slot->getQueueReader();
        reader.drain();
        publishSlotAvailabilityLocked(slot);
        setSlotEventLocked(slot, reader.hasPendingEvents());
    }
}

bool MultiReaderImpl::exposeBuriedEventsLocked(const std::vector<SizeT>& culprits)
{
    bool exposed = false;
    for (const auto index : culprits)
    {
        if (index >= slots.size())
            continue;

        auto& reader = slots[index]->getQueueReader();
        if (!reader.hasPendingEvents() && reader.hasQueuedEventPackets())
        {
            // The failing input has a corrective descriptor change queued behind data that was
            // produced under the old, failing descriptor. That data can never be read while the
            // input keeps failing, so drop it and let the event surface - the same semantics the
            // setInputUsed(false -> true) recovery applies. Without this, an actively producing
            // input could never recover: the fix would stay buried behind unreadable data forever.
            reader.dropForInactive();
            exposed |= reader.hasPendingEvents();
        }
    }
    return exposed;
}

std::optional<std::int64_t> MultiReaderImpl::readOffsetLocked() const
{
    const auto* start = syncManager->getCommonStart();
    if (start == nullptr)
        return std::nullopt;

    switch (resolvedDomainReadType)
    {
        case SampleType::Int64:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int64_t>*>(start))
                return typed->getValue();
            break;
        case SampleType::UInt64:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint64_t>*>(start))
                return static_cast<std::int64_t>(typed->getValue());
            break;
        case SampleType::Int32:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int32_t>*>(start))
                return typed->getValue();
            break;
        case SampleType::UInt32:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint32_t>*>(start))
                return typed->getValue();
            break;
        default:
            break;
    }
    return std::nullopt;
}

std::vector<SizeT> MultiReaderImpl::visibleLostSlotsLocked() const
{
    const auto lost = dataLossMonitor->lostSlots();
    if (lost.empty())
        return {};

    // "Can no longer contribute" is < one aligned block, not empty: block-aligned reads floor to
    // whole blocks, so a residual sub-block (possible whenever an input's divider != blockLcm) is
    // unreadable and, with the producer dead, no event will ever end its segment to let it drain.
    // Gating on == 0 would keep the reader serving forever without surfacing the loss.
    const SizeT block = syncManager->hasModel() ? syncManager->getModel().blockLcm : 1;
    std::vector<SizeT> drained;
    for (const auto index : lost)
    {
        if (slots[index]->getQueueReader().getAvailableSamples() < block)
            drained.push_back(index);
    }
    return drained;
}

// --- Establishing -------------------------------------------------------------------------------

std::optional<Fault> MultiReaderImpl::evaluateInactiveLocked()
{
    // Inactive readers are not monitored for data loss
    for (SizeT i = 0; i < slots.size(); ++i)
        dataLossMonitor->setMonitored(i, false);

    std::vector<QueueReader*> readers;
    std::vector<SizeT> slotIndices;
    collectUsedReadersInto(readers, slotIndices);

    std::vector<SizeT> eventInputs;
    for (SizeT position = 0; position < readers.size(); ++position)
    {
        const auto slotIndex = slotIndices[position];
        // Clear-then-drain (see the connection stage): clear before adopting so a concurrent
        // lock-free arrival re-arms the flag instead of being stranded.
        slots[slotIndex]->clearPacketPending();
        slots[slotIndex]->adoptQueuedPackets();
        if (!slots[slotIndex]->isConnected())
        {
            setSlotEventLocked(slots[slotIndex], false);
            continue;
        }

        const bool hasEvents = readers[position]->hasPendingEvents();
        setSlotEventLocked(slots[slotIndex], hasEvents);
        if (hasEvents)
            eventInputs.push_back(slotIndex);
    }

    if (!eventInputs.empty())
        return faultWithCulprits(FaultType::EventPending, "Events pending on inputs", "", std::move(eventInputs));
    return std::nullopt;
}

/**
 * @brief Everything that has to hold before any alignment question is worth asking, in precedence
 * order: the used set and the main input, connections, the data-loss monitoring refresh, leading
 * events, descriptors, per-input validity, deadlines, and the cross-input model.
 *
 * Each stage either settles here naming what blocks advancement, or hands a consumer-resolvable
 * fault to Error. Falling off the end means rank 1 is complete and alignment can start.
 */
ReaderStateTransition MultiReaderImpl::evaluateEstablishingLocked()
{
    const auto settled = [](std::optional<Fault> fault = std::nullopt) -> ReaderStateTransition
    { return {ReaderBehavior::Establishing, TransitionTrigger::Settled, std::move(fault)}; };
    const auto faulted = [](Fault fault) -> ReaderStateTransition
    { return {ReaderBehavior::Error, TransitionTrigger::Faulted, std::move(fault)}; };

    drainUnusedSlotsLocked();

    if (!isActive)
        return settled(evaluateInactiveLocked());

    std::vector<QueueReader*> readers;
    std::vector<SizeT> slotIndices;
    collectUsedReadersInto(readers, slotIndices);

    if (readers.empty())
    {
        invalidateSynchronizationLocked();
        return settled(Fault{FaultType::Unconnected, "No used inputs", {}});
    }

    // The explicitly selected main input is never silently replaced
    SizeT mainPosition = 0;
    if (mainInputId.assigned())
    {
        const auto mainSlot = mainSlotIndexLocked();
        const auto position = std::find(slotIndices.begin(), slotIndices.end(), mainSlot);
        if (mainSlot == notFound || position == slotIndices.end())
        {
            invalidateModelLocked();
            return settled(Fault{FaultType::Unconnected,
                                 "The selected main input is not among the used inputs",
                                 mainSlot == notFound ? std::vector<SizeT>{} : std::vector<SizeT>{mainSlot}});
        }
        mainPosition = static_cast<SizeT>(position - slotIndices.begin());
    }

    // Adopt what the producers enqueued. Connectivity itself is NOT polled here: every
    // connect/disconnect/reconnect reaches the slot as a port callback, and
    // Input::replayMissedPortCallbacks replays the two callbacks the port skips for a port that was
    // already connected when the listener was installed. So isConnected() is authoritative; only
    // the queue contents need collecting, because the lock-free producer path cannot hand them over.
    {
        std::vector<SizeT> unconnected;
        for (const auto index : slotIndices)
        {
            // Clear the arrival flag BEFORE draining (clear-then-drain). The producer path is
            // lock-free, so a packet enqueued after this clear re-arms the flag and is caught by the
            // next pass; clearing AFTER the drain would instead wipe the flag of a packet enqueued
            // in the drain->clear window without ever adopting it, stranding it on the connection
            // (the availability-undercount race).
            slots[index]->clearPacketPending();
            slots[index]->adoptQueuedPackets();
            if (!slots[index]->isConnected())
                unconnected.push_back(index);
        }
        if (!unconnected.empty())
        {
            // Connections gate events: while a used input has no signal no event is returnable, so
            // the callback must not fire on the events already queued on the connected inputs
            for (const auto index : slotIndices)
                setSlotEventLocked(slots[index], false);

            invalidateModelLocked();
            return settled(faultWithCulprits(FaultType::Unconnected, "Inputs", " have no signal connected", std::move(unconnected)));
        }
    }

    // Data-loss monitoring covers exactly the used, connected inputs of an active reader;
    // everything else is unmonitored and disarmed
    for (SizeT i = 0; i < slots.size(); ++i)
        dataLossMonitor->setMonitored(i, slots[i]->isUsed() && slots[i]->isConnected());

    // Leading events preempt everything below them
    {
        std::vector<SizeT> eventInputs;
        bool handshakeInFlight = false;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            // packetPending was already cleared before the drain above (clear-then-drain); clearing
            // again here would re-open the drain->clear race, so it is intentionally not cleared.
            const bool hasEvents = readers[position]->hasPendingEvents();
            if (hasEvents)
                eventInputs.push_back(slotIndices[position]);
            setSlotEventLocked(slots[slotIndices[position]], hasEvents);

            // A connected input with neither descriptors nor events is still completing its connect
            // handshake: the signal's initial descriptor event has not been enqueued yet
            // (connections are constructed in steps and evaluations can run in between)
            if (!hasEvents && !readers[position]->getValueDescriptor().assigned() &&
                !readers[position]->getDomainDescriptor().assigned())
            {
                handshakeInFlight = true;
            }
        }

        // While a connect handshake is in flight the reader is not yet event-ready: the in-flight
        // input's initial descriptor event arrives momentarily and re-triggers evaluation, so both
        // the dataAvailable callback and blocked reads see every input's initial event at once. The
        // evaluation falls through to the descriptor stage, which truthfully names the handshaking
        // input as missing its descriptors.
        if (handshakeInFlight)
        {
            for (const auto index : slotIndices)
                setSlotEventLocked(slots[index], false);
        }
        else if (!eventInputs.empty())
        {
            // Descriptors apply when leading events are consumed, so the cross-input model is built
            // opportunistically - accessors like getCommonSampleRate and getTickResolution work
            // right after construction, like they always have
            if (!syncManager->hasModel())
            {
                bool modelBuildable = true;
                for (auto* reader : readers)
                {
                    if (!reader->getValueDescriptor().assigned() || !reader->getDomainDescriptor().assigned() || !reader->isValid())
                        modelBuildable = false;
                }
                if (modelBuildable)
                    syncManager->buildCommonModel(readers, slotIndices, mainPosition);
            }
            return settled(faultWithCulprits(FaultType::EventPending, "Events pending on inputs", "", std::move(eventInputs)));
        }
    }

    // Descriptors
    {
        std::vector<SizeT> missing;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (!readers[position]->getValueDescriptor().assigned() || !readers[position]->getDomainDescriptor().assigned())
                missing.push_back(slotIndices[position]);
        }
        if (!missing.empty())
            return settled(faultWithCulprits(FaultType::MissingDescriptors, "Inputs", " have no descriptors yet", std::move(missing)));
    }

    // From here on the main-input descriptors are current. Deliberately above every stage that
    // invalidates the model: the refresh may itself drop the cached main-descriptor packet.
    refreshMainInputDescriptorsLocked();

    // Per-input validity
    {
        std::vector<SizeT> invalidInputs;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (!readers[position]->isValid())
                invalidInputs.push_back(slotIndices[position]);
        }
        if (!invalidInputs.empty())
        {
            invalidateModelLocked();
            if (exposeBuriedEventsLocked(invalidInputs))
            {
                for (const auto index : invalidInputs)
                    setSlotEventLocked(slots[index], slots[index]->getQueueReader().hasPendingEvents());
                return settled(faultWithCulprits(FaultType::EventPending, "Events pending on inputs", "", std::move(invalidInputs)));
            }
            return faulted(faultWithCulprits(
                FaultType::Incompatible, "Inputs", " are not readable with the current descriptors", std::move(invalidInputs)));
        }
    }

    // Deadlines. In-band: an input's buffered pre-loss data stays readable (the producer went
    // silent AFTER producing it), so the loss only ends the establishment once the affected input
    // can no longer contribute a whole block.
    if (auto lost = visibleLostSlotsLocked(); !lost.empty())
    {
        invalidateSynchronizationLocked();
        return faulted(faultWithCulprits(FaultType::DataLost, "Inputs", " missed their packet deadline", std::move(lost)));
    }

    // The cross-input model, built only when absent. buildCommonModel assigns the model wholesale,
    // which clears commonStart - so rebuilding an existing one would destroy a synchronization the
    // reader is entitled to keep. Absent is the honest trigger: everything that can move the model
    // (descriptors, the used set, configuration) arrives as an event or a trigger that invalidates
    // it first, and that is also what lets Synchronizing trust the model it is handed.
    if (!syncManager->hasModel())
    {
        auto setup = syncManager->buildCommonModel(readers, slotIndices, mainPosition);
        if (!setup.ok())
        {
            readCoordinator->invalidate();
            if (exposeBuriedEventsLocked(setup.affectedInputs))
            {
                for (const auto index : setup.affectedInputs)
                    setSlotEventLocked(slots[index], slots[index]->getQueueReader().hasPendingEvents());
                return settled(
                    faultWithCulprits(FaultType::EventPending, "Events pending on inputs", "", std::move(setup.affectedInputs)));
            }
            return faulted(Fault{FaultType::Incompatible, std::move(setup.message), std::move(setup.affectedInputs)});
        }
    }

    return {ReaderBehavior::Synchronizing, TransitionTrigger::InputsReady, std::nullopt};
}

// --- Synchronizing ------------------------------------------------------------------------------

/**
 * @brief Rank 2: everything Establishing checks already holds, so the only question left is the
 * alignment itself.
 *
 * The model is trusted here rather than rebuilt. Descriptors move only through an event, and the
 * used set, the main input and the configuration move only through a caller carrying a trigger -
 * both routes go through Establishing, which is where the model is built. What this rank does
 * re-read on every pass is the part of Establishing's ground truth a producer can change with no
 * notification at all: connectivity and leading events.
 */
ReaderStateTransition MultiReaderImpl::evaluateSynchronizingLocked()
{
    const auto settled = [](std::optional<Fault> fault = std::nullopt) -> ReaderStateTransition
    { return {ReaderBehavior::Synchronizing, TransitionTrigger::Settled, std::move(fault)}; };
    const auto faulted = [](Fault fault) -> ReaderStateTransition
    { return {ReaderBehavior::Error, TransitionTrigger::Faulted, std::move(fault)}; };
    const auto reestablish = [](TransitionTrigger trigger) -> ReaderStateTransition
    { return {ReaderBehavior::Establishing, trigger, std::nullopt}; };

    if (!isActive)
        return reestablish(TransitionTrigger::ActiveChanged);

    drainUnusedSlotsLocked();

    std::vector<QueueReader*> readers;
    std::vector<SizeT> slotIndices;
    collectUsedReadersInto(readers, slotIndices);
    if (readers.empty())
        return reestablish(TransitionTrigger::UsedChanged);

    // Clear-then-drain, the same ordering rule as Establishing: the producer path is lock-free, so a
    // packet enqueued after the clear re-arms the flag instead of being stranded on the connection.
    for (const auto index : slotIndices)
    {
        slots[index]->clearPacketPending();
        slots[index]->adoptQueuedPackets();
        if (!slots[index]->isConnected())
            return reestablish(TransitionTrigger::ConnectionChanged);
    }

    // Leading events outrank the alignment: they have to be consumed before the data behind them
    // means anything. Reported from here rather than handed back to Establishing - every input is
    // connected and past its handshake by now, so neither of Establishing's cross-input event
    // suppressions applies and there is nothing for it to add. Consuming the events is what returns
    // the reader to Establishing (readEventsLocked's EventsConsumed trigger), since a descriptor
    // change invalidates the model.
    {
        std::vector<SizeT> eventInputs;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            const bool hasEvents = readers[position]->hasPendingEvents();
            setSlotEventLocked(slots[slotIndices[position]], hasEvents);
            if (hasEvents)
                eventInputs.push_back(slotIndices[position]);
        }
        if (!eventInputs.empty())
            return settled(faultWithCulprits(FaultType::EventPending, "Events pending on inputs", "", std::move(eventInputs)));
    }

    // Deadlines, by the shared in-band rule: a crossed deadline only counts once the input can no
    // longer contribute a whole aligned block.
    if (auto lost = visibleLostSlotsLocked(); !lost.empty())
    {
        invalidateSynchronizationLocked();
        return faulted(faultWithCulprits(FaultType::DataLost, "Inputs", " missed their packet deadline", std::move(lost)));
    }

    // Every input needs something unread before an alignment attempt means anything. Its own stage
    // rather than synchronize()'s NeedMoreData, because collectFirstSamples early-returns on the
    // first empty input and would name one culprit where the reader should name all of them.
    {
        std::vector<SizeT> emptyInputs;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (readers[position]->getAvailableSamples() == 0)
                emptyInputs.push_back(slotIndices[position]);
        }
        if (!emptyInputs.empty())
            return settled(faultWithCulprits(FaultType::NotAligned, "Waiting for data on inputs", "", std::move(emptyInputs)));
    }

    // The iterative alignment step, stateful across passes: a NeedMoreData attempt latches the
    // target (SynchronizationManager::pendingCandidate) so the inputs that did reach it are waited
    // on against THIS tick instead of a freshly derived, later one. Staying in this rank is what
    // makes that latch legible - being here IS "a target is chosen and not yet reached".
    auto result = syncManager->synchronize(readers, slotIndices);
    switch (result.outcome)
    {
        case SyncOutcome::Synchronized:
            readCoordinator->configure(readers, syncManager->getModel());
            nextReadTick = readOffsetLocked();
            return {ReaderBehavior::Ready, TransitionTrigger::SynchronizationSucceeded, std::nullopt};
        case SyncOutcome::NeedMoreData:
            return settled(Fault{FaultType::NotAligned, std::move(result.message), std::move(result.affectedInputs)});
        case SyncOutcome::EventPending:
            for (const auto index : result.affectedInputs)
                setSlotEventLocked(slots[index], true);
            return settled(Fault{FaultType::EventPending, std::move(result.message), std::move(result.affectedInputs)});
        case SyncOutcome::Failed:
            return faulted(Fault{FaultType::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs)});
    }
    return faulted(Fault{FaultType::SynchronizationFailed, "Unhandled synchronization outcome", {}});
}
ReaderStateTransition MultiReaderImpl::evaluateReadyLocked()
{
    return {ReaderBehavior::Ready, TransitionTrigger::Settled, std::nullopt};
}
ReaderStateTransition MultiReaderImpl::evaluateErrorLocked()
{
    return {ReaderBehavior::Error, TransitionTrigger::Settled, std::nullopt};
}

// --- State machine ----------------------------------------------------------------------------

void MultiReaderImpl::setStateLocked(ReaderState newState, std::string message, std::vector<SizeT> affected)
{
    // Entering an InputsFailed-family state (Incompatible, SynchronizationFailed, DataLost) - or
    // changing which inputs it affects while in one - is a condition the consumer must act on that
    // carries no returnable data and, once the descriptors that caused it are cached, no
    // returnable event either. The two cases that would otherwise wake nobody: a data-loss
    // deadline (no packet at all), and re-probing a persistently incompatible / unsynchronizable
    // input (its descriptor is already known, so no new event fires). Latch a one-shot callback
    // wake so the reader itself notifies the consumer, which then reads the naming status
    // (spec 3.5/3.6). Edge-triggered: an unchanged failure re-evaluation does not re-latch, so
    // healthy-input packets cannot re-fire it while the failure persists.
    const bool inputsFailed = newState == ReaderState::Incompatible ||
                              newState == ReaderState::SynchronizationFailed ||
                              newState == ReaderState::DataLost;
    if (inputsFailed && (newState != state || affected != stateAffectedInputs))
    {
        notificationCoordinator->setStateChangeNotify(true);
        // No scheduling here: setStateLocked runs under the state mutex, and the inline
        // (no-scheduler) executor would re-enter onCoalescedEvaluation and deadlock. The wake
        // is delivered without it: an InputsFailed state is non-Synchronized, so the reader sets
        // wakeOnAnyPacket for every slot (publishProducerGateLocked) and the next packet forces
        // an evaluation that observes the latch; the pure data-loss deadline (no packet at all)
        // schedules from the monitor's own callback, outside any lock. When the transition
        // happens inside a coalesced evaluation (an escalated ladder), that same
        // onCoalescedEvaluation observes the latch after the ladder returns.
    }

    state = newState;
    stateMessage = std::move(message);
    stateAffectedInputs = std::move(affected);
}

void MultiReaderImpl::invalidateSynchronizationLocked()
{
    multi_reader::invalidateSynchronization(*syncManager, *readCoordinator, nextReadTick);
}

void MultiReaderImpl::invalidateModelLocked()
{
    invalidateSynchronizationLocked();
    syncManager->invalidateModel();
    clearModelDerivedCachesLocked();
}

void MultiReaderImpl::clearModelDerivedCachesLocked()
{
    // The cached status and the common-output-domain descriptor embed the model
    // (epoch/resolution/rate) - any input's descriptor change can move it
    cachedStatus = nullptr;
    cachedStatusFingerprint = {};
    cachedInputSnapshot = nullptr;
    cachedStatusMessage = nullptr;
    cachedCommonDomainDescriptor = nullptr;
    cachedMainDescriptorPacket = nullptr;
}

void MultiReaderImpl::collectUsedReadersInto(std::vector<QueueReader*>& readers, std::vector<SizeT>& slotIndices) const
{
    multi_reader::collectUsedReaders(slots, readers, slotIndices);
}

void MultiReaderImpl::refreshMainInputDescriptorsLocked()
{
    if (slots.empty())
        return;

    // The explicitly selected main input, or the first used input in construction order;
    // the first slot as a last resort while nothing is used
    SizeT mainSlot = mainSlotIndexLocked();
    if (mainSlot == notFound)
    {
        mainSlot = 0;
        for (SizeT i = 0; i < slots.size(); ++i)
        {
            if (slots[i]->isUsed())
            {
                mainSlot = i;
                break;
            }
        }
    }

    auto& reader = slots[mainSlot]->getQueueReader();
    const auto prevValue = mainValueDescriptor;
    const auto prevDomain = mainDomainDescriptor;
    if (reader.getValueDescriptor().assigned())
        mainValueDescriptor = reader.getValueDescriptor();
    if (reader.getDomainDescriptor().assigned())
        mainDomainDescriptor = reader.getDomainDescriptor();

    // The cached main-descriptor packet embeds these; drop it if they changed
    if (mainValueDescriptor != prevValue || mainDomainDescriptor != prevDomain)
        cachedMainDescriptorPacket = nullptr;
}

SizeT MultiReaderImpl::mainSlotIndexLocked() const
{
    if (!mainInputId.assigned())
        return notFound;
    return findSlotByIdLocked(mainInputId);
}

void MultiReaderImpl::refreshDataPlaneLocked(StateContext& ctx, bool escalateOnEvent)
{
    if (currentState->refreshDataPlane(ctx, escalateOnEvent))
        evaluateStateLocked();
}

void MultiReaderImpl::evaluateStateLocked()
{
    // The behaviour checks its own exit conditions; the exhaustive derivation is the reference the
    // tests cross-check that against.
    settleLocked([this](StateContext& ctx)
                 { return exhaustiveDerivationForTest ? deriveState(ctx) : currentState->reassess(ctx); });
}

StateContext MultiReaderImpl::makeStateContextLocked()
{
    return StateContext(slots,
                        *syncManager,
                        *readCoordinator,
                        *dataLossMonitor,
                        dataPlane,
                        nextReadTick,
                        stateMessage,
                        state,
                        mainInputId,
                        invalid,
                        isActive,
                        minReadCount,
                        resolvedDomainReadType);
}

void MultiReaderImpl::applyStateOutcomeLocked(StateContext& ctx, StateOutcome outcome)
{
    // The evaluation cannot reach these, so it flagged them instead. The order is the order the
    // ladder used to run them in: the descriptor refresh sits above the rungs that invalidate the
    // model, and the refresh may itself drop the cached main-descriptor packet.
    if (ctx.mainDescriptorsStale)
        refreshMainInputDescriptorsLocked();
    if (ctx.modelInvalidated)
        clearModelDerivedCachesLocked();

    // One evaluation, one verdict - so the InputsFailed latch in setStateLocked still sees exactly
    // one transition per evaluation, and still compares against the state it is replacing.
    setStateLocked(outcome.state, std::move(outcome.message), std::move(outcome.affected));

    // The behaviour follows the substate (stateIdFor). Cached rather than recomputed per call because
    // every read and query dispatches through it; this is the single place it is written.
    currentState = &stateFor(stateIdFor(state, isActive));
}

void MultiReaderImpl::setSlotReadyLocked(Input* slot, bool ready)
{
    multi_reader::setSlotReady(*slot, ready);
}

void MultiReaderImpl::setSlotEventLocked(Input* slot, bool event)
{
    multi_reader::setSlotEvent(*slot, event);
}

void MultiReaderImpl::applySlotUsedLocked(Input* slot, bool used)
{
    if (slot->isUsed() == used)
        return;
    slot->setUsed(used);
    notificationCoordinator->gate()->adjustUsed(used ? 1 : -1);
    // An unused slot contributes only events to the gate (the recovery signal); a stale ready
    // flag would let `ready >= used` open the gate on data the read path will never touch.
    if (!used)
        setSlotReadyLocked(slot, false);
}

void MultiReaderImpl::publishSlotAvailabilityLocked(Input* slot)
{
    multi_reader::publishSlotAvailability(*slot);
}

void MultiReaderImpl::clearGateReadinessLocked()
{
    for (auto* slot : slots)
    {
        setSlotReadyLocked(slot, false);
        setSlotEventLocked(slot, false);
    }
}

void MultiReaderImpl::updateCallbackStateLocked()
{
    auto ctx = makeStateContextLocked();
    if (currentState->updateCallbackState(ctx))
        evaluateStateLocked();
}

void MultiReaderImpl::onCoalescedEvaluation()
{
    ProcedurePtr callback;
    {
        std::lock_guard lock(mutex);
        const auto ownerPass = notificationCoordinator->beginOwnerPass();
        if (invalid)
            return;

        // The coalesced task only decides whether onDataAvailable should fire; it maintains the
        // gate flags without running the state ladder for events (deferred to the read/query path)
        // and without walking slots that already satisfy the gate. Producer raises are advisory;
        // this reconciliation is what stands between a stale raise and a spurious user callback.
        updateCallbackStateLocked();
        if (notificationCoordinator->gateSatisfied())
            callback = readCallback;

        // One-shot: consume a latched state-change wake (an InputsFailed transition) once
        // observed, so a single failure fires the callback exactly once rather than on every
        // later evaluation. A blocked read is woken independently by notifyCondition below.
        notificationCoordinator->setStateChangeNotify(false);
    }

    notifyCondition.notify_all();

    if (callback.assigned())
        wrapHandler(callback);
}

// --- IInputListener -----------------------------------------------------------------------

bool MultiReaderImpl::slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal)
{
    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned())
        {
            Bool accept = True;
            if (slotIndex < slots.size())
                checkErrorInfo(listener->acceptsSignal(slots[slotIndex]->getPort(), signal, &accept));
            return accept;
        }
    }
    return true;
}

void MultiReaderImpl::slotConnected(SizeT slotIndex)
{
    {
        std::lock_guard lock(mutex);
        const auto ownerPass = notificationCoordinator->beginOwnerPass();
        if (slotIndex < slots.size())
        {
            slots[slotIndex]->rebindConnection();
            settleLocked([this, slotIndex](StateContext& ctx) { return currentState->slotConnected(ctx, slotIndex); });
            settleStateLocked({ReaderBehavior::Establishing, TransitionTrigger::ConnectionChanged, std::nullopt});
        }
    }
    notifyCondition.notify_all();

    // A coalesced task scheduled by the connection's first packets may have run while the
    // connection was still being constructed and found nothing returnable; guarantee one
    // evaluation (and callback gate check) after the connection is fully established
    notificationCoordinator->requestEvaluation();

    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned() && slotIndex < slots.size())
            listener->connected(slots[slotIndex]->getPort());
    }
}

void MultiReaderImpl::slotDisconnected(SizeT slotIndex)
{
    {
        std::lock_guard lock(mutex);
        const auto ownerPass = notificationCoordinator->beginOwnerPass();
        if (slotIndex < slots.size())
        {
            slots[slotIndex]->rebindConnection();
            settleLocked([this, slotIndex](StateContext& ctx) { return currentState->slotDisconnected(ctx, slotIndex); });
        }
    }
    notifyCondition.notify_all();

    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned() && slotIndex < slots.size())
            listener->disconnected(slots[slotIndex]->getPort());
    }
}

void MultiReaderImpl::slotPacketReceived(SizeT slotIndex, bool forceEvaluation)
{
    // Bounded producer path: no state mutex, no queue access, no mutex at all - the slot has
    // already raised its gate flags from a minimal connection introspection.
    // Mark the data plane changed before the notify below, so a consumer woken by it sees it.
    dataPlane.dirty.store(true, std::memory_order_release);
    dataLossMonitor->onPacket(slotIndex);

    // An evaluation task is scheduled only when the callback gate is open - any event flag,
    // every used slot ready, or a latched state-change wake - or when the slot could not trust
    // its snapshot / the reader is not in the steady Synchronized state (forceEvaluation).
    // A closed gate means this packet provably cannot fire onDataAvailable, so scheduling
    // would only burn a scheduler round-trip; blocked reads are woken by the notify below and
    // re-check availability themselves.
    if (forceEvaluation || notificationCoordinator->gateSatisfied())
        notificationCoordinator->requestEvaluation();
    notifyCondition.notify_all();

    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned() && slotIndex < slots.size())
            listener->packetReceived(slots[slotIndex]->getPort());
    }
}

// --- Status and offset ------------------------------------------------------------------------

EventPacketPtr MultiReaderImpl::mainDescriptorPacketLocked()
{
    // Cached: this packet only changes when the main value descriptor or the common output
    // domain changes, but the status path rebuilt it on every read. The cache is cleared by
    // invalidateModelLocked (model/common-domain change) and refreshMainInputDescriptorsLocked
    // (main descriptors change) - the same points that clear cachedCommonDomainDescriptor.
    if (cachedMainDescriptorPacket.assigned())
        return cachedMainDescriptorPacket;

    // The domain part is the common output domain - the domain the status offset is
    // expressed in - not the main input's own domain. It is
    // rebuilt lazily per model build (invalidateModelLocked clears it).
    DataDescriptorPtr domainDescriptor = mainDomainDescriptor;
    if (syncManager->hasModel() && mainDomainDescriptor.assigned())
    {
        const auto& model = syncManager->getModel();
        if (!cachedCommonDomainDescriptor.assigned() && model.ticksPerCommonSample() > 0)
        {
            cachedCommonDomainDescriptor = DataDescriptorBuilderCopy(mainDomainDescriptor)
                                               .setOrigin(reader::isoEpochString(model.commonDomain.epoch))
                                               .setTickResolution(Ratio(model.commonDomain.resolution.num, model.commonDomain.resolution.den))
                                               .setRule(LinearDataRule(static_cast<Int>(model.ticksPerCommonSample()), 0))
                                               .build();
        }
        if (cachedCommonDomainDescriptor.assigned())
            domainDescriptor = cachedCommonDomainDescriptor;
    }
    cachedMainDescriptorPacket = DataDescriptorChangedEventPacket(descriptorToEventPacketParam(mainValueDescriptor),
                                                                  descriptorToEventPacketParam(domainDescriptor));
    return cachedMainDescriptorPacket;
}

namespace
{

// The public surface is the extended ReadStatus; the internal states map onto it.
// A status carrying events always reports Event - the events are the thing to react to.
ReadStatus toReadStatus(ReaderState state, bool hasEvents)
{
    if (hasEvents)
        return ReadStatus::Event;

    switch (state)
    {
        case ReaderState::Synchronized:
            return ReadStatus::Ok;
        case ReaderState::Inactive:
            return ReadStatus::Inactive;
        case ReaderState::EventPending:
            // Events are pending but none were returned in this status (e.g. a wait timed
            // out before they could be popped): "read again" is exactly the reaction
            return ReadStatus::Event;
        case ReaderState::Incompatible:
        case ReaderState::SynchronizationFailed:
        case ReaderState::DataLost:
            return ReadStatus::InputsFailed;
        case ReaderState::Error:
            return ReadStatus::Fail;
        case ReaderState::WaitingForConnections:
        case ReaderState::WaitingForDescriptors:
        case ReaderState::WaitingForData:
        case ReaderState::Synchronizing:
        default:
            return ReadStatus::Preparing;
    }
}

}  // namespace

void MultiReaderImpl::buildInputStateSnapshotLocked(std::vector<std::pair<StringPtr, Int>>& out) const
{
    out.clear();
    out.reserve(slots.size());

    // Failure states name their affected inputs; everything else is derived per slot
    InputState failureState = InputState::Ok;
    switch (state)
    {
        case ReaderState::Incompatible:
            failureState = InputState::Incompatible;
            break;
        case ReaderState::SynchronizationFailed:
            failureState = InputState::SynchronizationFailed;
            break;
        case ReaderState::DataLost:
            failureState = InputState::DataLost;
            break;
        default:
            break;
    }

    for (auto* slot : slots)
    {
        InputState inputState;
        if (!slot->isUsed())
        {
            // An unused input with unconsumed events reports Event - that is the
            // recovery signal consumers react to with setInputUsed(id, true)
            inputState = slot->isConnected() && slot->getQueueReader().hasPendingEvents() ? InputState::Event : InputState::Unused;
        }
        else if (failureState != InputState::Ok &&
                 std::find(stateAffectedInputs.begin(), stateAffectedInputs.end(), slot->getIndex()) != stateAffectedInputs.end())
        {
            inputState = failureState;
        }
        else if (slot->isConnected() && slot->getQueueReader().hasPendingEvents())
        {
            inputState = InputState::Event;
        }
        else if (state == ReaderState::Synchronized)
        {
            inputState = InputState::Ok;
        }
        else
        {
            inputState = InputState::Pending;
        }

        // getInputId() returns the slot's cached id, so this is a refbump - no per-read allocation
        out.emplace_back(slot->getInputId(), static_cast<Int>(inputState));
    }
}

MultiReaderStatusPtr MultiReaderImpl::createStatusLocked(const DictPtr<IString, IEventPacket>& eventPackets,
                                                         const NumberPtr& offset)
{
    // The invalid flag maps to Error here as a safety net - every path setting it is also
    // expected to set the state
    const auto effectiveState = invalid && state != ReaderState::Error ? ReaderState::Error : state;

    const bool hasEvents = eventPackets.assigned() && eventPackets.getCount() > 0;

    // Cached-instance behavior: event-less statuses are re-issued while
    // their visible content is unchanged; statuses carrying events are always fresh
    StatusFingerprint fingerprint;
    fingerprint.state = effectiveState;
    fingerprint.message = stateMessage;
    fingerprint.affectedInputs = stateAffectedInputs;
    fingerprint.mainValue = mainValueDescriptor.getObject();
    fingerprint.mainDomain = mainDomainDescriptor.getObject();

    // The offset advances on every data read; the rest of the content stays constant while
    // synchronized. Build the current snapshot and compare the offset-independent content to the
    // cache separately from the offset (see StatusFingerprint above).
    buildInputStateSnapshotLocked(statusSnapshotScratch);
    const std::int64_t offsetInt = offset.assigned() ? static_cast<std::int64_t>(offset.getIntValue()) : 0;

    if (!hasEvents && cachedStatus.assigned() && fingerprint == cachedStatusFingerprint && cachedInputSnapshot &&
        statusSnapshotScratch == *cachedInputSnapshot)
    {
        // Content unchanged. Re-issue the same instance when the offset also matches; otherwise
        // share the cached content (snapshot, message, descriptor packet - all refbumps) into a
        // new status stamped with the advanced offset, rebuilding none of it.
        if (offsetInt == cachedStatusOffset)
            return cachedStatus;

        MultiReaderStatusPtr restamped = createWithImplementation<IMultiReaderStatus, MultiReaderStatusImpl>(
            mainDescriptorPacketLocked(), nullptr, offset, cachedReadStatus, cachedStatusMessage, cachedInputSnapshot);
        cachedStatus = restamped;
        cachedStatusOffset = offsetInt;
        return restamped;
    }

    // Content changed (or the first status, or an event): rebuild. Construct directly (not via
    // MultiReaderStatusBuilder): the read path hands the status a shared snapshot that it boxes
    // into the IDict only if getInputStates() is called, so a steady read never builds the dict.
    // The builder's eager-dict path stays for external callers.
    auto snapshot = std::make_shared<const std::vector<std::pair<StringPtr, Int>>>(std::move(statusSnapshotScratch));
    const auto readStatus = toReadStatus(effectiveState, hasEvents);
    const auto messageStr = String(stateMessage);
    MultiReaderStatusPtr status = createWithImplementation<IMultiReaderStatus, MultiReaderStatusImpl>(
        mainDescriptorPacketLocked(), eventPackets, offset, readStatus, messageStr, snapshot);
    if (!hasEvents)
    {
        cachedStatus = status;
        cachedStatusFingerprint = std::move(fingerprint);
        cachedInputSnapshot = snapshot;
        cachedStatusMessage = messageStr;
        cachedReadStatus = readStatus;
        cachedStatusOffset = offsetInt;
    }
    return status;
}

MultiReaderStatusPtr MultiReaderImpl::readEventsLocked()
{
    auto events = Dict<IString, EventPacketPtr>();
    for (auto* slot : slots)
    {
        if (!slot->isUsed())
            continue;

        auto& reader = slot->getQueueReader();
        if (!reader.hasPendingEvents())
            continue;

        // One event per input per call; the pop applies descriptor changes.
        // Keyed by getInputId (the same id getInputStates, setInputUsed and removeInput use) so
        // a consumer can correlate a returned event with its per-input state and act on it -
        // for a signal-built reader that id is the signal's global id, not the synthetic port's.
        auto packet = reader.popFrontEvent();
        if (packet.assigned())
            events.set(slot->getInputId(), packet);

        setSlotEventLocked(slot, reader.hasPendingEvents());
    }

    // Every returned event invalidates synchronization; descriptor changes may have
    // changed rates, so the whole model is rebuilt right away - accessors like
    // getCommonSampleRate must reflect the new descriptors as soon as the events are out
    invalidateModelLocked();
    refreshMainInputDescriptorsLocked();
    evaluateStateLocked();

    return createStatusLocked(events);
}

// --- Read path --------------------------------------------------------------------------------

ErrCode MultiReaderImpl::readInternal(void** valueBuffers,
                                      void** domainBuffers,
                                      SizeT* count,
                                      SizeT timeoutMs,
                                      IMultiReaderStatus** status,
                                      bool skip)
{
    std::unique_lock lock(mutex);
    // Spans the whole read, including the timed waits (their predicate refreshes the data
    // plane): producers treat the entire read as an owner pass and schedule conservatively.
    const auto ownerPass = notificationCoordinator->beginOwnerPass();

    if (invalid)
    {
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return skip ? OPENDAQ_IGNORED : OPENDAQ_SUCCESS;
    }

    auto ctx = makeStateContextLocked();
    refreshDataPlaneLocked(ctx, true);

    // Zero-count handshake: report events or the current state without consuming data.
    // With a timeout the call waits for events to arrive instead of returning immediately.
    if (*count == 0)
    {
        if (timeoutMs > 0 && !currentState->readWaitSatisfied(ctx, 0))
        {
            notifyCondition.wait_for(lock,
                                     milliseconds(timeoutMs),
                                     [&]
                                     {
                                         if (invalid)
                                             return true;
                                         refreshDataPlaneLocked(ctx, true);
                                         return currentState->readWaitSatisfied(ctx, 0);
                                     });
        }
        MultiReaderStatusPtr statusPtr =
            currentState->planRead(ctx) == ReadAction::ReturnEvents ? readEventsLocked() : createStatusLocked();
        if (status)
            *status = statusPtr.detach();
        return OPENDAQ_SUCCESS;
    }

    const SizeT requested = *count;

    if (timeoutMs > 0)
    {
        // ReadTimeoutType::All - wait until the whole request is servable or an event arrives
        notifyCondition.wait_for(lock,
                                 milliseconds(timeoutMs),
                                 [&]
                                 {
                                     if (invalid)
                                         return true;
                                     refreshDataPlaneLocked(ctx, true);
                                     return currentState->readWaitSatisfied(ctx, requested);
                                 });
        if (invalid)
        {
            if (status)
                *status = createStatusLocked().detach();
            *count = 0;
            return OPENDAQ_SUCCESS;
        }
        refreshDataPlaneLocked(ctx, true);
    }

    const auto action = currentState->planRead(ctx);
    if (action == ReadAction::ReturnEvents)
    {
        auto statusPtr = readEventsLocked();
        if (status)
            *status = statusPtr.detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    if (action == ReadAction::ReportState)
    {
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    // Plan against availability, then commit every input - a partial commit is impossible.
    // Reuse member scratch (retains capacity across reads) to avoid per-read heap allocation.
    collectUsedReadersInto(readScratchUsed, readScratchSlotIndices);
    auto& used = readScratchUsed;
    auto& slotIndices = readScratchSlotIndices;
    const auto& model = syncManager->getModel();

    // The refresh above published per-input availability on the synchronized fast path; capture
    // whether that cache is usable now so both the plan and the post-commit readiness update can
    // reuse it instead of re-walking every input (nothing between here and the commit mutates a
    // queue, so the cached counts stay exact under the held mutex).
    const bool availableCached = dataPlane.availableValid;

    readScratchValueBuffers.assign(used.size(), nullptr);
    readScratchDomainBuffers.assign(used.size(), nullptr);
    for (SizeT position = 0; position < used.size(); ++position)
    {
        if (valueBuffers)
            readScratchValueBuffers[position] = valueBuffers[slotIndices[position]];
        if (domainBuffers)
            readScratchDomainBuffers[position] = domainBuffers[slotIndices[position]];
    }

    const SizeT alignedAvailable = availableCached
        ? ReadCoordinator::alignAvailable(dataPlane.availableCommon, model, minReadCount)
        : readCoordinator->getAvailableCount(used, model, minReadCount);

    const auto plan = readCoordinator->createPlan(requested,
                                                  alignedAvailable,
                                                  model,
                                                  minReadCount,
                                                  skip ? nullptr : readScratchValueBuffers.data(),
                                                  skip ? nullptr : readScratchDomainBuffers.data());

    const auto offsetTick = nextReadTick;

    std::string commitError;
    const auto commitResult =
        skip ? readCoordinator->skip(plan, used, commitError) : readCoordinator->commit(plan, used, commitError);
    if (commitResult != CommitResult::Ok)
    {
        invalid = true;
        setStateLocked(ReaderState::Error, std::move(commitError));
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    if (plan.commonCount > 0 && nextReadTick.has_value() && model.ticksPerCommonSample() > 0)
        nextReadTick = *nextReadTick + static_cast<std::int64_t>(plan.commonCount) * model.ticksPerCommonSample();

    // The commit consumed exactly plan.commonCount from every used input (a whole number of blocks,
    // never crossing an event), so every input's availability has dropped and readiness has to be
    // re-derived against the frontier the read left behind. This is the "fall on read" half of
    // readiness maintenance.
    if (plan.commonCount > 0)
    {
        for (SizeT position = 0; position < used.size(); ++position)
        {
            auto* slot = slots[slotIndices[position]];
            // Republish the full producer-visible availability (samples AND adopted-event state) from
            // the consumed frontier first, so a late async packetReceived never self-gates against a
            // stale basis and raises a phantom ready/forces a needless evaluation - and so the slot's
            // own answer below is computed from the post-commit truth.
            publishSlotAvailabilityLocked(slot);
            const bool slotReady = slot->hasAdoptedDataToRead();
            setSlotReadyLocked(slot, slotReady);
            // Data-first, the same rule as publishProducerGate: the event bit was deliberately down
            // while a servable block sat in front of the event, so consuming down to the boundary
            // (or to a sub-minimum residual) is what turns the slot blocked - and this pass is the
            // only one guaranteed to run then. A producer that never sends another packet would
            // never re-raise it, leaving the gate silent on an event it owes the consumer.
            setSlotEventLocked(slot,
                               !slotReady && (used[position]->hasPendingEvents() || used[position]->hasQueuedEventPackets()));
        }

        // The read advanced the frontier, so the cached counts are now stale and a previously
        // buried event may be leading: invalidate the cache and force the next data-plane refresh
        // to run its full pass rather than skip (see refreshDataPlaneLocked).
        dataPlane.availableValid = false;
        dataPlane.consumed = true;
    }

    NumberPtr offsetNumber = offsetTick.has_value() ? NumberPtr(*offsetTick) : NumberPtr(0);
    if (status)
        *status = createStatusLocked(nullptr, offsetNumber).detach();
    *count = plan.commonCount;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::read(void* samples, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);
    if (*count != 0)
    {
        OPENDAQ_PARAM_NOT_NULL(samples);

        if (minReadCount > *count)
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, "Count parameter has to be either 0 or larger than minReadCount.");
    }

    return readInternal(static_cast<void**>(samples), nullptr, count, timeoutMs, status, false);
}

ErrCode MultiReaderImpl::readWithDomain(void* samples, void* domain, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);
    if (*count != 0)
    {
        OPENDAQ_PARAM_NOT_NULL(samples);
        OPENDAQ_PARAM_NOT_NULL(domain);

        if (minReadCount > *count)
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, "Count parameter has to be either 0 or larger than minReadCount.");
    }

    return readInternal(static_cast<void**>(samples), static_cast<void**>(domain), count, timeoutMs, status, false);
}

ErrCode MultiReaderImpl::skipSamples(SizeT* count, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    {
        std::lock_guard lock(mutex);
        if (invalid)
        {
            if (status)
                *status = createStatusLocked().detach();
            *count = 0;
            return OPENDAQ_IGNORED;
        }
    }

    if (minReadCount > *count)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, "Count parameter has to be larger than minReadCount.");

    return readInternal(nullptr, nullptr, count, 0, status, true);
}

// --- IReader ----------------------------------------------------------------------------------

ErrCode MultiReaderImpl::getAvailableCount(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    std::lock_guard lock(mutex);
    const auto ownerPass = notificationCoordinator->beginOwnerPass();

    *count = 0;
    if (invalid)
        return OPENDAQ_SUCCESS;

    // The query does not surface events (escalateOnEvent = false); it drains, maintains the callback
    // flags, and lets the read path surface any event. It can change the state, so the count is asked
    // of whichever behaviour it leaves behind.
    auto ctx = makeStateContextLocked();
    refreshDataPlaneLocked(ctx, false);
    *count = currentState->availableCount(ctx);
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setOnDataAvailable(IProcedure* callback)
{
    std::lock_guard lock(mutex);
    readCallback = callback;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setExternalListener(IInputPortNotifications* listener)
{
    externalListener = listener;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getEmpty(Bool* empty)
{
    OPENDAQ_PARAM_NOT_NULL(empty);

    std::lock_guard lock(mutex);
    const auto ownerPass = notificationCoordinator->beginOwnerPass();

    bool allHaveData = !slots.empty();
    for (auto* slot : slots)
    {
        if (!slot->isUsed())
            continue;

        // Queues refresh only at explicit points
        slot->adoptQueuedPackets();
        if (!slot->isConnected())
        {
            allHaveData = false;
            continue;
        }

        auto& reader = slot->getQueueReader();
        if (reader.hasPendingEvents())
        {
            *empty = False;
            return OPENDAQ_SUCCESS;
        }
        allHaveData = allHaveData && reader.getAvailableSamples() > 0;
    }

    *empty = allHaveData ? False : True;
    return OPENDAQ_SUCCESS;
}

// --- ISampleReader ----------------------------------------------------------------------------

ErrCode MultiReaderImpl::getValueReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    std::lock_guard lock(mutex);
    *sampleType = slots.empty() ? valueReadType : slots.front()->getQueueReader().getValueReadType();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    *sampleType = resolvedDomainReadType;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setValueTransformFunction(IFunction* transform)
{
    std::lock_guard lock(mutex);
    for (auto* slot : slots)
        slot->getQueueReader().setValueTransformFunction(transform);
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setDomainTransformFunction(IFunction* transform)
{
    std::lock_guard lock(mutex);
    for (auto* slot : slots)
        slot->getQueueReader().setDomainTransformFunction(transform);
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadMode(ReadMode* mode)
{
    OPENDAQ_PARAM_NOT_NULL(mode);

    *mode = readMode;
    return OPENDAQ_SUCCESS;
}

// --- IMultiReader accessors -------------------------------------------------------------------

ErrCode MultiReaderImpl::getTickResolution(IRatio** resolution)
{
    OPENDAQ_PARAM_NOT_NULL(resolution);

    std::lock_guard lock(mutex);
    if (!syncManager->hasModel())
    {
        *resolution = nullptr;
        return OPENDAQ_IGNORED;
    }

    // The public API reports the resolution as a Ratio object; the model stores plain values
    const auto& res = syncManager->getModel().commonDomain.resolution;
    *resolution = Ratio(res.num, res.den).detach();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOrigin(IString** origin)
{
    OPENDAQ_PARAM_NOT_NULL(origin);

    std::lock_guard lock(mutex);
    if (!syncManager->hasModel())
    {
        *origin = nullptr;
        return OPENDAQ_IGNORED;
    }

    *origin = String(reader::isoEpochString(syncManager->getModel().commonDomain.epoch)).detach();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOffset(void* domainStart)
{
    OPENDAQ_PARAM_NOT_NULL(domainStart);

    std::lock_guard lock(mutex);

    const auto tick = nextReadTick;
    if (!tick.has_value())
        return OPENDAQ_IGNORED;

    switch (resolvedDomainReadType)
    {
        case SampleType::Int64:
            *static_cast<std::int64_t*>(domainStart) = *tick;
            return OPENDAQ_SUCCESS;
        case SampleType::UInt64:
            *static_cast<std::uint64_t*>(domainStart) = static_cast<std::uint64_t>(*tick);
            return OPENDAQ_SUCCESS;
        case SampleType::Int32:
            *static_cast<std::int32_t*>(domainStart) = static_cast<std::int32_t>(*tick);
            return OPENDAQ_SUCCESS;
        case SampleType::UInt32:
            *static_cast<std::uint32_t*>(domainStart) = static_cast<std::uint32_t>(*tick);
            return OPENDAQ_SUCCESS;
        default:
            return OPENDAQ_IGNORED;
    }
}

ErrCode MultiReaderImpl::getIsSynchronized(Bool* isSynchronized)
{
    OPENDAQ_PARAM_NOT_NULL(isSynchronized);

    std::lock_guard lock(mutex);
    *isSynchronized = state == ReaderState::Synchronized ? True : False;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getCommonSampleRate(Int* commonSampleRate)
{
    OPENDAQ_PARAM_NOT_NULL(commonSampleRate);

    std::lock_guard lock(mutex);
    *commonSampleRate = syncManager->hasModel() ? syncManager->getModel().commonSampleRate : -1;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setActive(Bool isActive)
{
    ProcedurePtr callback;
    {
        std::lock_guard lock(mutex);
        const auto ownerPass = notificationCoordinator->beginOwnerPass();

        const bool changed = this->isActive != static_cast<bool>(isActive);
        this->isActive = isActive;

        if (changed)
        {
            setPortsActiveLocked(isActive);
            invalidateSynchronizationLocked();
            clearGateReadinessLocked();

            // Deactivation suspends the data flow: queued data and gap events are dropped
            // (they are meaningless once the stream pauses), while descriptor changes stay
            // pending so the reader's type state cannot silently diverge
            if (!isActive)
            {
                for (auto* slot : slots)
                    slot->getQueueReader().dropForInactive();
            }
        }
        settleLocked([this](StateContext& ctx) { return currentState->activeChanged(ctx); });
    }
    notifyCondition.notify_all();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getActive(Bool* isActive)
{
    OPENDAQ_PARAM_NOT_NULL(isActive);

    std::lock_guard lock(mutex);
    *isActive = this->isActive;
    return OPENDAQ_SUCCESS;
}

void MultiReaderImpl::setPortsActiveLocked(bool active)
{
    for (auto* slot : slots)
    {
        if (!slot->isUsed())
            continue;
        slot->setPortActive(active);
    }
}

// --- Input management -------------------------------------------------------------------------

SizeT MultiReaderImpl::findSlotByIdLocked(const StringPtr& id) const
{
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        if (slots[i]->getInputId() == id)
            return i;
    }
    return notFound;
}

void MultiReaderImpl::reindexSlotsLocked()
{
    for (SizeT i = 0; i < slots.size(); ++i)
        slots[i]->setIndex(i);
}

ErrCode MultiReaderImpl::addInput(IComponent* input)
{
    OPENDAQ_PARAM_NOT_NULL(input);

    try
    {
        ListPtr<IComponent> list = List<IComponent>();
        list.pushBack(input);

        SizeT firstNewSlot = 0;
        {
            std::lock_guard lock(mutex);
            const auto ownerPass = notificationCoordinator->beginOwnerPass();
            normalizeSources(list);

            auto ports = createOrAdoptPorts(list);
            firstNewSlot = slots.size();
            createSlots(ports);

            settleLocked([this](StateContext& ctx) { return currentState->inputSetChanged(ctx); });
        }
        // Unlocked: the new slots install themselves as listeners and replay the port's missing
        // callbacks, which re-enter this reader through slotConnected/slotPacketReceived.
        replaySlotCallbacks(firstNewSlot);
    }
    catch (...)
    {
        return OPENDAQ_ERR_INVALIDPARAMETER;
    }

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::removeInput(IString* id)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);
    const auto ownerPass = notificationCoordinator->beginOwnerPass();

    const auto position = findSlotByIdLocked(StringPtr::Borrow(id));
    if (position == notFound)
        return OPENDAQ_NOTFOUND;

    if (mainInputId.assigned() && mainInputId == StringPtr::Borrow(id))
    {
        LOG_W("The selected main input was removed; reverting to the default (first used input)");
        mainInputId = nullptr;
    }

    auto* slot = slots[position];
    slot->detachListener();
    // Retire the slot's gate contribution atomically: disarm() subtracts whatever flags are
    // set and makes any in-flight producer raise a no-op, so the shared counters can never
    // drift when a packet races the removal.
    if (slot->isUsed())
        notificationCoordinator->gate()->adjustUsed(-1);
    slot->gateFlags().disarm();
    if (!portBinder.assigned())
        slot->getPort().remove();

    slots.erase(slots.begin() + position);
    slotObjects.erase(slotObjects.begin() + position);
    reindexSlotsLocked();

    // Only the removed input's per-slot state goes; the remaining
    // inputs keep their readiness/event flags and armed data-loss deadlines
    dataLossMonitor->erase(position);

    settleLocked([this](StateContext& ctx) { return currentState->inputSetChanged(ctx); });
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setInputUsed(IString* id, Bool isUsed)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);
    const auto ownerPass = notificationCoordinator->beginOwnerPass();

    const auto position = findSlotByIdLocked(StringPtr::Borrow(id));
    if (position == notFound)
        return OPENDAQ_ERR_NOTFOUND;

    auto* slot = slots[position];
    applySlotUsedLocked(slot, isUsed);
    if (!isUsed)
        dataLossMonitor->setMonitored(position, false);

    if (isUsed)
    {
        // Re-enabled inputs restart from the live stream: data and gaps queued while the
        // input was unused are dropped (descriptor changes are kept, so the type state
        // stays coherent); revalidation and resynchronization run on the next evaluation
        slot->setPortActive(this->isActive);
        slot->rebindConnection();
        slot->getQueueReader().dropForInactive();
    }
    else
    {
        slot->setPortActive(false);
        // Exclusion drops what the input has already adopted, at the moment of exclusion rather
        // than on the way back in: the data is unreadable either way (re-enabling restarts from the
        // live stream), and keeping it would bury the events that ARE the input's only remaining
        // job - the per-input Event state is the recovery signal a consumer answers with
        // setInputUsed(id, true), and it is derived from leading events only. dropForInactive keeps
        // descriptor changes pending, so the type state stays coherent.
        slot->getQueueReader().dropForInactive();
    }

    settleLocked([this](StateContext& ctx) { return currentState->inputSetChanged(ctx); });
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getInputUsed(IString* id, Bool* isUsed)
{
    OPENDAQ_PARAM_NOT_NULL(id);
    OPENDAQ_PARAM_NOT_NULL(isUsed);

    std::lock_guard lock(mutex);

    const auto position = findSlotByIdLocked(StringPtr::Borrow(id));
    if (position == notFound)
        return OPENDAQ_ERR_NOTFOUND;

    *isUsed = slots[position]->isUsed() ? True : False;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setMainInput(IString* id)
{
    {
        std::lock_guard lock(mutex);
        const auto ownerPass = notificationCoordinator->beginOwnerPass();

        StringPtr newId = StringPtr::Borrow(id);
        if (newId.assigned() && newId.getLength() == 0)
            newId = nullptr;

        if (newId.assigned())
        {
            const auto position = findSlotByIdLocked(newId);
            if (position == notFound)
                return OPENDAQ_ERR_NOTFOUND;
            if (!slots[position]->isUsed())
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, "The selected main input is not used.");
        }

        mainInputId = newId;

        // The main input defines the output grid identity, so this invalidates the model like any
        // other change to the used-input set; the next derivation realigns on the new grid
        settleLocked([this](StateContext& ctx) { return currentState->inputSetChanged(ctx); });
    }
    notifyCondition.notify_all();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getMainInput(IString** id)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);
    // Empty string means automatic selection - the first used input
    *id = (mainInputId.assigned() ? mainInputId : String("")).addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

// --- IReaderConfig ----------------------------------------------------------------------------

ErrCode MultiReaderImpl::getValueTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);
    std::lock_guard lock(mutex);

    if (slots.empty())
    {
        *transform = nullptr;
        return OPENDAQ_ERR_INVALIDSTATE;
    }

    *transform = slots.front()->getQueueReader().getValueTransformFunction().addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);
    std::lock_guard lock(mutex);

    if (slots.empty())
    {
        *transform = nullptr;
        return OPENDAQ_ERR_INVALIDSTATE;
    }

    *transform = slots.front()->getQueueReader().getDomainTransformFunction().addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getInputPorts(IList** ports)
{
    OPENDAQ_PARAM_NOT_NULL(ports);

    std::lock_guard lock(mutex);

    auto list = List<IInputPortConfig>();
    for (auto* slot : slots)
        list.pushBack(slot->getPort());

    *ports = list.detach();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadTimeoutType(ReadTimeoutType* timeoutType)
{
    OPENDAQ_PARAM_NOT_NULL(timeoutType);

    *timeoutType = ReadTimeoutType::All;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::markAsInvalid()
{
    std::lock_guard lock(mutex);
    invalid = true;
    setStateLocked(ReaderState::Error, "Reader was marked as invalid");
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getIsValid(Bool* isValid)
{
    OPENDAQ_PARAM_NOT_NULL(isValid);

    std::lock_guard lock(mutex);
    *isValid = !invalid;
    return OPENDAQ_SUCCESS;
}

void MultiReaderImpl::internalDispose(bool)
{
    if (dataLossMonitor)
        dataLossMonitor->detach();
    if (notificationCoordinator)
        notificationCoordinator->detach();
    for (auto* slot : slots)
        slot->detachListener();

    // portBinder is deliberately kept: it marks the ports as externally owned, and the
    // destructor must not remove() adopted ports of a disposed reader (dispose-and-rebuild
    // is the documented consumer pattern for reconfiguring on the same ports). The binder
    // itself dies with the reader, which releases port ownership for re-adoption.
    externalListener = nullptr;
    readCallback = nullptr;
    cachedStatus = nullptr;
    cachedInputSnapshot = nullptr;
    cachedStatusMessage = nullptr;
    cachedCommonDomainDescriptor = nullptr;
    invalid = true;
    setStateLocked(ReaderState::Error, "Reader was disposed");
    isActive = false;
}

// --- Factories --------------------------------------------------------------------------------

OPENDAQ_DEFINE_CLASS_FACTORY(LIBRARY_FACTORY,
                             MultiReader,
                             IList*,
                             signals,
                             SampleType,
                             valueReadType,
                             SampleType,
                             domainReadType,
                             ReadMode,
                             mode,
                             ReadTimeoutType,
                             timeoutType)

OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ(LIBRARY_FACTORY,
                                                               MultiReaderImpl,
                                                               IMultiReader,
                                                               createMultiReaderEx,
                                                               IList*,
                                                               signals,
                                                               SampleType,
                                                               valueReadType,
                                                               SampleType,
                                                               domainReadType,
                                                               ReadMode,
                                                               mode,
                                                               ReadTimeoutType,
                                                               timeoutType,
                                                               Int,
                                                               requiredCommonSampleRate,
                                                               Bool,
                                                               startOnFullUnitOfDomain,
                                                               SizeT,
                                                               minReadCount)

template <>
struct ObjectCreator<IMultiReader>
{
    static ErrCode Create(IMultiReader** out, IMultiReader* toCopy, SampleType valueReadType, SampleType domainReadType) noexcept
    {
        OPENDAQ_PARAM_NOT_NULL(out);

        if (toCopy == nullptr)
        {
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ARGUMENT_NULL, "Existing reader must not be null");
        }

        auto old = ReaderConfigPtr::Borrow(toCopy);
        auto impl = dynamic_cast<MultiReaderImpl*>(old.getObject());

        if (impl == nullptr)
        {
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER,
                                       "MultiReader from existing can only be used with the base multi reader implementation");
        }

        return createObject<IMultiReader, MultiReaderImpl>(out, impl, valueReadType, domainReadType);
    }
};

OPENDAQ_DEFINE_CUSTOM_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ(LIBRARY_FACTORY,
                                                                      IMultiReader,
                                                                      createMultiReaderFromExisting,
                                                                      IMultiReader*,
                                                                      invalidatedReader,
                                                                      SampleType,
                                                                      valueReadType,
                                                                      SampleType,
                                                                      domainReadType)

extern "C" daq::ErrCode PUBLIC_EXPORT createMultiReaderFromBuilder(IMultiReader** objTmp, IMultiReaderBuilder* builder)
{
    return daq::createObject<IMultiReader, MultiReaderImpl>(objTmp, builder);
}

END_NAMESPACE_OPENDAQ
