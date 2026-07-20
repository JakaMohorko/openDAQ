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
                                 ReadTimeoutType /*timeoutType*/,  // only All is honored (spec section 7.2)
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
                // Deadlines enter the same coalesced evaluation path as packets (spec section 9.5)
                notificationCoordinator->requestEvaluation();
                notifyCondition.notify_all();
            });
        applyConfigToSyncManager();

        createSlots(ports);

        std::lock_guard lock(mutex);
        for (SizeT i = 0; i < usedFlags.size() && i < slots.size(); ++i)
        {
            slots[i]->setUsed(usedFlags[i]);
            notificationCoordinator->setUsed(i, usedFlags[i]);
            slots[i]->getQueueReader().seedDescriptors(oldValueDescriptors[i], oldDomainDescriptors[i]);
        }
        applyDataLossTimeoutLocked();
        evaluateStateLocked();
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

        // Deprecated (spec section 8.4): the value is ignored; kept on the builder for compatibility
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
                // Deadlines enter the same coalesced evaluation path as packets (spec section 9.5)
                notificationCoordinator->requestEvaluation();
                notifyCondition.notify_all();
            });
        applyConfigToSyncManager();

        auto ports = createOrAdoptPorts(sourceComponents);
        createSlots(ports);

        std::lock_guard lock(mutex);
        if (mainInputId.assigned() && findSlotByIdLocked(mainInputId) == notFound)
            DAQ_THROW_EXCEPTION(NotFoundException, "The selected main input does not match any source component");
        // Adopted ports may arrive deactivated (a previous owner parked them via
        // setInputUsed(false)); their active state belongs to this reader now
        setPortsActiveLocked(isActive);
        applyDataLossTimeoutLocked();
        evaluateStateLocked();
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
                                                                                       typeOfInputs == InputType::Signals);
        auto* slot = static_cast<Input*>(slotObject.getObject());
        port.setListener(slotObject);

        slotObjects.push_back(std::move(slotObject));
        slots.push_back(slot);
        ++position;
    }

    notificationCoordinator->resize(slots.size());
    dataLossMonitor->resize(slots.size());
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

// --- State machine ----------------------------------------------------------------------------

void MultiReaderImpl::setStateLocked(ReaderState newState, std::string message, std::vector<SizeT> affected)
{
    state = newState;
    stateMessage = std::move(message);
    stateAffectedInputs = std::move(affected);
}

void MultiReaderImpl::setStateWithAffectedLocked(ReaderState newState,
                                                 const char* messagePrefix,
                                                 const char* messageSuffix,
                                                 std::vector<SizeT> affected)
{
    auto message = fmt::format("{} [{}]{}", messagePrefix, fmt::join(affected, ", "), messageSuffix);
    setStateLocked(newState, std::move(message), std::move(affected));
}

void MultiReaderImpl::invalidateSynchronizationLocked()
{
    syncManager->clearSynchronization();
    readCoordinator->invalidate();
    nextReadTick.reset();
}

void MultiReaderImpl::invalidateModelLocked()
{
    invalidateSynchronizationLocked();
    syncManager->invalidateModel();

    // The cached status and the common-output-domain descriptor embed the model
    // (epoch/resolution/rate) - any input's descriptor change can move it
    cachedStatus = nullptr;
    cachedStatusFingerprint = {};
    cachedInputSnapshot = nullptr;
    cachedStatusMessage = nullptr;
    cachedCommonDomainDescriptor = nullptr;
    cachedMainDescriptorPacket = nullptr;
}

std::vector<QueueReader*> MultiReaderImpl::collectUsedReaders(std::vector<SizeT>& slotIndices) const
{
    std::vector<QueueReader*> readers;
    collectUsedReadersInto(readers, slotIndices);
    return readers;
}

void MultiReaderImpl::collectUsedReadersInto(std::vector<QueueReader*>& readers, std::vector<SizeT>& slotIndices) const
{
    readers.clear();
    slotIndices.clear();
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        if (!slots[i]->isUsed())
            continue;
        readers.push_back(&slots[i]->getQueueReader());
        slotIndices.push_back(i);
    }
}

// COMMENT: What does "main descriptors locked" mean?
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

bool MultiReaderImpl::exposeBuriedEventsLocked(const std::vector<SizeT>& affected)
{
    bool exposed = false;
    for (const auto index : affected)
    {
        if (index >= slots.size())
            continue;

        auto& reader = slots[index]->getQueueReader();
        if (!reader.hasPendingEvents() && reader.hasQueuedEventPackets())
        {
            // The failing input has a corrective descriptor change queued behind data that
            // was produced under the old, failing descriptor. That data can never be read
            // while the input keeps failing, so drop it and let the event surface - the same
            // semantics the setInputUsed(false -> true) recovery applies (dropForInactive).
            // Without this, an actively producing input could never recover: the fix would
            // stay buried behind unreadable data forever.
            reader.dropForInactive();
            exposed |= reader.hasPendingEvents();
        }
    }
    return exposed;
}

void MultiReaderImpl::drainUnusedSlotsLocked()
{
    // C12/Q5: unused inputs stay observable. Their ports are inactive, so data is dropped at
    // the connection and only events can arrive; draining them makes pending events visible
    // in the per-input states and lets them fire the dataAvailable callback (the recovery
    // signal a consumer answers with setInputUsed(id, true)).
    for (auto* slot : slots)
    {
        if (slot->isUsed())
            continue;

        slot->syncConnection();
        if (!slot->isConnected())
        {
            notificationCoordinator->setEvent(slot->getIndex(), false);
            continue;
        }

        slot->clearPacketPending();
        auto& reader = slot->getQueueReader();
        reader.drain();
        notificationCoordinator->setEvent(slot->getIndex(), reader.hasPendingEvents());
    }
}

void MultiReaderImpl::refreshDataPlaneLocked()
{
    // Establishment phases (waiting/synchronizing/failed) still run the full evaluation -
    // progress toward synchronization is exactly what those states are doing. The fast path
    // below is the synchronized steady state of review N6: no checks until an event is
    // encountered or a deadline expires; data packets only ever add availability.
    if (invalid || !isActive || state != ReaderState::Synchronized)
    {
        // The full ladder decides everything here (and clears the availability cache).
        evaluateStateLocked();
        return;
    }

    // Nothing to do if nothing changed since the last pass: no packet arrived (dataPlaneDirty)
    // and no read consumed (dataPlaneConsumed), and no deadline is pending. A buried event can
    // only surface through an arrival or a consumption, so there is nothing to re-check - this
    // collapses the repeated refreshes of a poll-then-read loop to one real pass. Any cached
    // availability stays valid across this early return: with nothing arrived and nothing
    // consumed the counts cannot have moved.
    const bool arrived = dataPlaneDirty.exchange(false, std::memory_order_acquire);
    if (!arrived && !dataPlaneConsumed && !dataLossMonitor->hasLostSlots())
        return;
    dataPlaneConsumed = false;

    // This pass recomputes each used input's availability (below), so the read path can plan and
    // lower readiness from the cached counts instead of walking every input again in createPlan
    // and in the post-commit readiness update. The slot count is fixed after construction; size
    // the cache lazily.
    if (dataPlaneSlotAvailable.size() != slots.size())
        dataPlaneSlotAvailable.assign(slots.size(), 0);

    const bool haveModel = syncManager->hasModel();
    const SizeT block = haveModel ? syncManager->getModel().blockLcm : 0;

    bool escalate = false;
    SizeT availableCommon = std::numeric_limits<SizeT>::max();
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        auto* slot = slots[i];
        const bool packetsArrived = slot->clearPacketPending();

        if (!slot->isUsed())
        {
            // Only events can arrive on an unused input's inactive port (C12/Q5)
            if (packetsArrived)
            {
                slot->syncConnection();
                if (slot->isConnected())
                {
                    auto& reader = slot->getQueueReader();
                    reader.drain();
                    notificationCoordinator->setEvent(slot->getIndex(), reader.hasPendingEvents());
                }
            }
            continue;
        }

        auto& reader = slot->getQueueReader();

        // The drain (the expensive step - adopts queued packets) is gated on arrival: a slot
        // with no new packet has nothing new to adopt.
        if (packetsArrived)
            reader.drain();

        // The event check runs EVERY cycle, not only on arrival: a leading event must be
        // reported (EventPending), and a buried event needs the full evaluation so the
        // partial segment in front of it can be discarded and the event can surface
        // (discardLeftoverSegments). A buried event becomes reachable through the reader's
        // own consumption (a read advancing the frontier past the data ahead of it) with no
        // new packet arriving, so gating this on packetsArrived would miss it. Both queries
        // are O(1) (empty-check / sticky adoption flag), so per-cycle is cheap.
        if (reader.hasPendingEvents() || reader.hasQueuedEventPackets())
        {
            escalate = true;
            continue;
        }

        // Availability is O(1) here (the queue reader maintains it incrementally across drains
        // and reads), so recomputing it for every used slot each real pass is cheap - and it is
        // exactly the count createPlan needs, so caching it removes createPlan's separate walk.
        // Readiness is derived from the same value: a slot is ready with a full aligned block
        // buffered before its next event.
        if (haveModel)
        {
            const SizeT avail = reader.getAvailableSamplesUntilEvent();
            dataPlaneSlotAvailable[i] = avail;
            availableCommon = std::min(availableCommon, avail);
            notificationCoordinator->setReady(slot->getIndex(), avail >= block);
        }
    }

    // Deadlines are maintained by the monitor's waiter thread; an unresolved loss must
    // re-enter the full evaluation (it decides when the loss becomes visible, section 2.6)
    if (!escalate && dataLossMonitor->hasLostSlots())
        escalate = true;

    if (escalate)
    {
        // The full evaluation can drop partial segments or change state, so the availability
        // gathered above is not authoritative; it clears the cache and consumers fall back to a
        // direct walk.
        evaluateStateLocked();
        return;
    }

    // Steady synchronized state: publish the availability this pass computed. The sentinel
    // survives only when no input is used, which maps to nothing available.
    dataPlaneAvailableCommon = availableCommon == std::numeric_limits<SizeT>::max() ? 0 : availableCommon;
    dataPlaneAvailableValid = haveModel;
}

void MultiReaderImpl::evaluateStateLocked()
{
    // The full ladder can drain, drop segments or change state, so any availability the last
    // fast pass cached is no longer authoritative. Clearing it here (the single funnel every full
    // evaluation passes through) is what makes the cache safe to reuse across refreshDataPlaneLocked's
    // early return: the cache is valid ONLY between a non-escalating synchronized fast pass and the
    // next thing that runs, and that next thing is either another fast pass (which republishes it),
    // the early return (nothing changed, so it stays exact), or a full evaluation (this, which clears it).
    dataPlaneAvailableValid = false;

    // 1. Error is terminal; inactivity gates everything else
    if (invalid)
    {
        setStateLocked(ReaderState::Error, stateMessage.empty() ? "Reader is invalid" : stateMessage);
        return;
    }

    drainUnusedSlotsLocked();

    if (!isActive)
    {
        // Inactive readers are not monitored for data loss (spec section 3.6)
        for (SizeT i = 0; i < slots.size(); ++i)
            dataLossMonitor->setMonitored(i, false);

        // Terminology (C12): ACTIVE/INACTIVE is the reader/port level switch (setActive) -
        // "pause the whole reader". USED/UNUSED is per-input participation (setInputUsed) -
        // "exclude this input from reading". The unused mechanism reuses port deactivation
        // internally because that is what stops data while preserving events.
        //
        // Inactivity suspends data flow only: descriptor/gap events are enqueued regardless
        // of the active flag and must still surface through reads (and through the
        // dataAvailable callback, which is why the event bits are maintained here too).
        // Unused inputs' events were already drained above (drainUnusedSlotsLocked).
        std::vector<SizeT> inactiveEventInputs;
        std::vector<SizeT> inactiveSlotIndices;
        const auto inactiveReaders = collectUsedReaders(inactiveSlotIndices);
        for (SizeT position = 0; position < inactiveReaders.size(); ++position)
        {
            const auto slotIndex = inactiveSlotIndices[position];
            slots[slotIndex]->syncConnection();
            if (!slots[slotIndex]->isConnected())
            {
                notificationCoordinator->setEvent(slotIndex, false);
                continue;
            }

            slots[slotIndex]->clearPacketPending();
            const bool hasEvents = inactiveReaders[position]->hasPendingEvents();
            notificationCoordinator->setEvent(slotIndex, hasEvents);
            if (hasEvents)
                inactiveEventInputs.push_back(slotIndex);
        }
        if (!inactiveEventInputs.empty())
        {
            invalidateSynchronizationLocked();
            setStateWithAffectedLocked(ReaderState::EventPending, "Events pending on inputs", "", std::move(inactiveEventInputs));
        }
        else
        {
            setStateLocked(ReaderState::Inactive);
        }
        return;
    }

    // 2. Resolve the used set and the main input (spec section 4.4: the explicitly
    // selected main input is never silently replaced)
    std::vector<SizeT> slotIndices;
    const auto usedReaders = collectUsedReaders(slotIndices);
    if (usedReaders.empty())
    {
        invalidateSynchronizationLocked();
        setStateLocked(ReaderState::WaitingForConnections, "No used inputs");
        return;
    }

    SizeT mainPosition = 0;
    if (mainInputId.assigned())
    {
        const auto mainSlot = mainSlotIndexLocked();
        const auto position = std::find(slotIndices.begin(), slotIndices.end(), mainSlot);
        if (mainSlot == notFound || position == slotIndices.end())
        {
            invalidateModelLocked();
            setStateLocked(ReaderState::WaitingForConnections,
                           "The selected main input is not among the used inputs",
                           mainSlot == notFound ? std::vector<SizeT>{} : std::vector<SizeT>{mainSlot});
            return;
        }
        mainPosition = static_cast<SizeT>(position - slotIndices.begin());
    }

    // 3. Connections - resynced from the ports themselves: initial event packets arrive
    // (and packetReceived fires) while the connection is still being constructed, before
    // the connected() notification reaches the slot
    {
        std::vector<SizeT> unconnected;
        for (const auto index : slotIndices)
        {
            slots[index]->syncConnection();
            if (!slots[index]->isConnected())
                unconnected.push_back(index);
        }
        if (!unconnected.empty())
        {
            // Connections gate events (spec 6.2: step 3 precedes step 5): while a used input
            // has no signal, no event is returnable, so the callback must not fire on the
            // events already queued on the connected inputs
            for (const auto index : slotIndices)
                notificationCoordinator->setEvent(index, false);

            invalidateModelLocked();
            setStateWithAffectedLocked(ReaderState::WaitingForConnections, "Inputs", " have no signal connected", std::move(unconnected));
            return;
        }
    }

    // Data-loss monitoring covers exactly the used, connected inputs of an active reader
    // (spec section 3.6); everything else is unmonitored and disarmed
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        const bool monitored = slots[i]->isUsed() && slots[i]->isConnected();
        dataLossMonitor->setMonitored(i, monitored);
    }

    // While synchronized, partial blocks in front of an event are silently discarded so
    // the event can surface (spec section 3.1/3.4)
    if (syncManager->getCommonStart() != nullptr && syncManager->hasModel())
        readCoordinator->discardLeftoverSegments(usedReaders, syncManager->getModel(), minReadCount);

    // 4./5. Refresh queues; pending events preempt everything below
    {
        std::vector<SizeT> eventInputs;
        bool handshakeInFlight = false;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            slots[slotIndices[position]]->clearPacketPending();
            const bool hasEvents = usedReaders[position]->hasPendingEvents();
            if (hasEvents)
                eventInputs.push_back(slotIndices[position]);
            notificationCoordinator->setEvent(slotIndices[position], hasEvents);

            // A connected input with neither descriptors nor events is still completing its
            // connect handshake: the signal's initial descriptor event has not been enqueued
            // yet (connections are constructed in steps and evaluations can run in between)
            if (!hasEvents && !usedReaders[position]->getValueDescriptor().assigned() &&
                !usedReaders[position]->getDomainDescriptor().assigned())
            {
                handshakeInFlight = true;
            }
        }
        // While a connect handshake is in flight the reader is not yet event-ready: the
        // in-flight input's initial descriptor event arrives momentarily and re-triggers
        // evaluation, so both the dataAvailable callback and blocked reads see every
        // input's initial event at once. The evaluation falls through to step 6, which
        // truthfully reports the handshaking input as WaitingForDescriptors.
        if (handshakeInFlight)
        {
            for (const auto index : slotIndices)
                notificationCoordinator->setEvent(index, false);
        }
        else if (!eventInputs.empty())
        {
            // Descriptors apply when leading events are consumed, so the cross-input model
            // can be built opportunistically - accessors like getCommonSampleRate and
            // getTickResolution work right after construction, like they always have
            if (!syncManager->hasModel())
            {
                bool modelBuildable = true;
                for (auto* reader : usedReaders)
                {
                    if (!reader->getValueDescriptor().assigned() || !reader->getDomainDescriptor().assigned() || !reader->isValid())
                        modelBuildable = false;
                }
                if (modelBuildable)
                    syncManager->buildCommonModel(usedReaders, slotIndices, mainPosition);
            }

            invalidateSynchronizationLocked();
            setStateWithAffectedLocked(ReaderState::EventPending, "Events pending on inputs", "", std::move(eventInputs));
            return;
        }
    }

    // 6. Descriptors
    {
        std::vector<SizeT> missing;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            if (!usedReaders[position]->getValueDescriptor().assigned() || !usedReaders[position]->getDomainDescriptor().assigned())
                missing.push_back(slotIndices[position]);
        }
        if (!missing.empty())
        {
            setStateWithAffectedLocked(ReaderState::WaitingForDescriptors, "Inputs", " have no descriptors yet", std::move(missing));
            return;
        }
    }

    refreshMainInputDescriptorsLocked();

    // 7. Local validity
    {
        std::vector<SizeT> invalidInputs;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            if (!usedReaders[position]->isValid())
                invalidInputs.push_back(slotIndices[position]);
        }
        if (!invalidInputs.empty())
        {
            invalidateModelLocked();
            if (exposeBuriedEventsLocked(invalidInputs))
            {
                for (const auto index : invalidInputs)
                    notificationCoordinator->setEvent(index, slots[index]->getQueueReader().hasPendingEvents());
                setStateWithAffectedLocked(ReaderState::EventPending, "Events pending on inputs", "", std::move(invalidInputs));
                return;
            }
            setStateWithAffectedLocked(
                ReaderState::Incompatible, "Inputs", " are not readable with the current descriptors", std::move(invalidInputs));
            return;
        }
    }

    // 9. Data-loss deadlines (spec section 3.6). In-band per review section 2.6: an input's
    // buffered pre-loss data stays readable (the producer went silent AFTER producing it),
    // so the loss only becomes the reader state once the affected input can no longer
    // contribute. Recovery is per input on its next packet, after which synchronization is
    // re-established.
    {
        const auto lost = dataLossMonitor->lostSlots();
        if (!lost.empty())
        {
            // "Can no longer contribute" is < one aligned block, not empty: block-aligned
            // reads floor to whole blocks, so a residual sub-block (possible whenever an
            // input's divider != blockLcm) is unreadable and, with the producer dead, no
            // event will ever end its segment to let it drain. Gating on == 0 would stall
            // the reader in Synchronized forever, never surfacing the loss.
            const SizeT block = syncManager->hasModel() ? syncManager->getModel().blockLcm : 1;
            std::vector<SizeT> drainedLost;
            for (const auto index : lost)
            {
                if (slots[index]->getQueueReader().getAvailableSamples() < block)
                    drainedLost.push_back(index);
            }
            if (!drainedLost.empty())
            {
                invalidateSynchronizationLocked();
                setStateWithAffectedLocked(ReaderState::DataLost, "Inputs", " missed their packet deadline", std::move(drainedLost));
                return;
            }
            // Lost but a full block still buffered: keep reading - the loss surfaces once the
            // input can no longer fill a block
        }
    }

    // Already synchronized: nothing further to establish
    if (syncManager->getCommonStart() != nullptr)
    {
        setStateLocked(ReaderState::Synchronized);
    }
    else
    {
        // 8. Cross-input compatibility and the common model
        auto setup = syncManager->buildCommonModel(usedReaders, slotIndices, mainPosition);
        if (!setup.ok())
        {
            readCoordinator->invalidate();
            if (exposeBuriedEventsLocked(setup.affectedInputs))
            {
                for (const auto index : setup.affectedInputs)
                    notificationCoordinator->setEvent(index, slots[index]->getQueueReader().hasPendingEvents());
                setStateWithAffectedLocked(ReaderState::EventPending, "Events pending on inputs", "", std::move(setup.affectedInputs));
                return;
            }
            setStateLocked(ReaderState::Incompatible, std::move(setup.message), std::move(setup.affectedInputs));
            return;
        }

        // 9. Data loss deadlines - Phase 4 (DataLossMonitor)

        // 10. Data on every input
        {
            std::vector<SizeT> empty;
            for (SizeT position = 0; position < usedReaders.size(); ++position)
            {
                if (usedReaders[position]->getAvailableSamples() == 0)
                    empty.push_back(slotIndices[position]);
            }
            if (!empty.empty())
            {
                setStateWithAffectedLocked(ReaderState::WaitingForData, "Waiting for data on inputs", "", std::move(empty));
                return;
            }
        }

        // 11. Alignment
        auto result = syncManager->synchronize(usedReaders, slotIndices);
        switch (result.outcome)
        {
            case SyncOutcome::Synchronized:
                // 12. Configure the read pipelines
                readCoordinator->configure(usedReaders, syncManager->getModel());
                nextReadTick = currentReadOffsetLocked();
                setStateLocked(ReaderState::Synchronized);
                break;
            case SyncOutcome::NeedMoreData:
                setStateLocked(ReaderState::Synchronizing, std::move(result.message), std::move(result.affectedInputs));
                break;
            case SyncOutcome::EventPending:
                invalidateSynchronizationLocked();
                setStateLocked(ReaderState::EventPending, std::move(result.message), std::move(result.affectedInputs));
                for (const auto index : result.affectedInputs)
                    notificationCoordinator->setEvent(index, true);
                break;
            case SyncOutcome::Failed:
                // Synchronization failure no longer deactivates the reader (spec section 8.5).
                // Unlike the Incompatible paths, we do NOT drop buffered data to surface a
                // buried event here: on a sync failure each input's data is individually valid
                // and readable (only the cross-input alignment failed), so the consumer's
                // remedy is to exclude an input or pick a main input - not to lose that input's
                // samples. A queued corrective descriptor surfaces the normal way once the
                // offending input is excluded and re-enabled (dropForInactive on re-enable).
                setStateLocked(ReaderState::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs));
                break;
        }
    }

    // 13. Readiness for the callback gate: a full aligned block while synchronized,
    // the first sample while still synchronizing
    for (SizeT position = 0; position < usedReaders.size(); ++position)
    {
        bool ready = false;
        if (state == ReaderState::Synchronized && syncManager->hasModel())
            ready = usedReaders[position]->getAvailableSamplesUntilEvent() >= syncManager->getModel().blockLcm;
        else
            ready = usedReaders[position]->getAvailableSamples() > 0;
        notificationCoordinator->setReady(slotIndices[position], ready);
    }
}

void MultiReaderImpl::onCoalescedEvaluation()
{
    ProcedurePtr callback;
    {
        std::lock_guard lock(mutex);
        if (invalid)
            return;

        // The coalesced task is the readiness/gate maintainer: data-only arrivals take the
        // fast path (drain + readiness bits); events and deadlines escalate to the full
        // evaluation inside it
        refreshDataPlaneLocked();
        if (notificationCoordinator->shouldInvokeCallback())
            callback = readCallback;
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
        if (slotIndex < slots.size())
        {
            slots[slotIndex]->rebindConnection();
            invalidateModelLocked();
            evaluateStateLocked();
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
        if (slotIndex < slots.size())
        {
            // Disarm immediately - the state evaluation may return before its monitor
            // refresh while other inputs are unconnected, and a stale arrival must not
            // count toward a deadline after a reconnect
            dataLossMonitor->setMonitored(slotIndex, false);
            slots[slotIndex]->rebindConnection();
            invalidateModelLocked();
            evaluateStateLocked();
        }
    }
    notifyCondition.notify_all();

    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned() && slotIndex < slots.size())
            listener->disconnected(slots[slotIndex]->getPort());
    }
}

void MultiReaderImpl::slotPacketReceived(SizeT slotIndex)
{
    // Bounded producer path: no state mutex, no queue access (spec section 9)
    // Mark the data plane changed before the notify below, so a consumer woken by it sees it.
    dataPlaneDirty.store(true, std::memory_order_release);
    dataLossMonitor->onPacket(slotIndex);
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
    // expressed in (spec sections 4.4 and 8.2) - not the main input's own domain. It is
    // rebuilt lazily per model build (invalidateModelLocked clears it).
    DataDescriptorPtr domainDescriptor = mainDomainDescriptor;
    if (syncManager->hasModel() && mainDomainDescriptor.assigned())
    {
        const auto& model = syncManager->getModel();
        if (!cachedCommonDomainDescriptor.assigned() && model.ticksPerCommonSample() > 0)
        {
            cachedCommonDomainDescriptor = DataDescriptorBuilderCopy(mainDomainDescriptor)
                                               .setOrigin(reader::isoEpochString(model.commonDomain.epoch))
                                               .setTickResolution(model.commonDomain.resolution)
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

// C5/Q1: the public surface is the extended ReadStatus; the internal states map onto it.
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
            // C12/Q2: an unused input with unconsumed events reports Event - that is the
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

    // Cached-instance behavior (spec section 8.2): event-less statuses are re-issued while
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

std::optional<std::int64_t> MultiReaderImpl::currentReadOffsetLocked() const
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

        // One event per input per call (spec section 7.2); the pop applies descriptor changes.
        // Keyed by getInputId (the same id getInputStates, setInputUsed and removeInput use) so
        // a consumer can correlate a returned event with its per-input state and act on it -
        // for a signal-built reader that id is the signal's global id, not the synthetic port's
        // (review finding: the two dicts were previously keyed differently and uncorrelatable).
        auto packet = reader.popFrontEvent();
        if (packet.assigned())
            events.set(slot->getInputId(), packet);

        notificationCoordinator->setEvent(slot->getIndex(), reader.hasPendingEvents());
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

    if (invalid)
    {
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return skip ? OPENDAQ_IGNORED : OPENDAQ_SUCCESS;
    }

    refreshDataPlaneLocked();

    // Zero-count handshake: report events or the current state without consuming data.
    // With a timeout the call waits for events to arrive instead of returning immediately.
    if (*count == 0)
    {
        if (timeoutMs > 0 && state != ReaderState::EventPending)
        {
            notifyCondition.wait_for(lock,
                                     milliseconds(timeoutMs),
                                     [&]
                                     {
                                         if (invalid)
                                             return true;
                                         refreshDataPlaneLocked();
                                         return state == ReaderState::EventPending;
                                     });
        }
        MultiReaderStatusPtr statusPtr =
            state == ReaderState::EventPending ? readEventsLocked() : createStatusLocked();
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
                                     refreshDataPlaneLocked();
                                     if (state == ReaderState::EventPending)
                                         return true;
                                     if (state != ReaderState::Synchronized)
                                         return false;

                                     const auto& waitModel = syncManager->getModel();
                                     // The refresh above just published availability on the fast
                                     // path; reuse it rather than walking the inputs again.
                                     SizeT available;
                                     if (dataPlaneAvailableValid)
                                         available = ReadCoordinator::alignAvailable(dataPlaneAvailableCommon, waitModel, minReadCount);
                                     else
                                     {
                                         collectUsedReadersInto(availScratchUsed, availScratchSlotIndices);
                                         available = readCoordinator->getAvailableCount(availScratchUsed, waitModel, minReadCount);
                                     }
                                     const SizeT block = waitModel.blockLcm;
                                     const SizeT alignedRequest = requested / block * block;
                                     return alignedRequest > 0 && available >= alignedRequest;
                                 });
        if (invalid)
        {
            if (status)
                *status = createStatusLocked().detach();
            *count = 0;
            return OPENDAQ_SUCCESS;
        }
        refreshDataPlaneLocked();
    }

    if (state == ReaderState::EventPending)
    {
        auto statusPtr = readEventsLocked();
        if (status)
            *status = statusPtr.detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    if (state != ReaderState::Synchronized)
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
    const bool availableCached = dataPlaneAvailableValid;

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
        ? ReadCoordinator::alignAvailable(dataPlaneAvailableCommon, model, minReadCount)
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

    // The commit consumed exactly plan.commonCount from every used input (a whole number of
    // blocks, never crossing an event), so each input's availability-until-event simply drops by
    // that amount. When the fast pass cached the pre-commit counts we derive the new readiness by
    // subtraction; otherwise (cache not valid this cycle) we query the now-decremented count
    // directly. This is the "fall on read" half of readiness maintenance.
    if (plan.commonCount > 0)
    {
        const auto block = model.blockLcm;
        for (SizeT position = 0; position < used.size(); ++position)
        {
            const SizeT remaining = availableCached
                ? dataPlaneSlotAvailable[slotIndices[position]] - plan.commonCount
                : used[position]->getAvailableSamplesUntilEvent();
            notificationCoordinator->setReady(slotIndices[position], remaining >= block);
        }

        // The read advanced the frontier, so the cached counts are now stale and a previously
        // buried event may be leading: invalidate the cache and force the next data-plane refresh
        // to run its full pass rather than skip (see refreshDataPlaneLocked).
        dataPlaneAvailableValid = false;
        dataPlaneConsumed = true;
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

    *count = 0;
    if (invalid)
        return OPENDAQ_SUCCESS;

    refreshDataPlaneLocked();
    if (state == ReaderState::Synchronized)
    {
        // The refresh above published availability on the fast path; reuse it rather than
        // walking every input again. Fall back to a direct count only when it is not valid.
        if (dataPlaneAvailableValid)
        {
            *count = ReadCoordinator::alignAvailable(dataPlaneAvailableCommon, syncManager->getModel(), minReadCount);
        }
        else
        {
            collectUsedReadersInto(availScratchUsed, availScratchSlotIndices);
            *count = readCoordinator->getAvailableCount(availScratchUsed, syncManager->getModel(), minReadCount);
        }
    }
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

    bool allHaveData = !slots.empty();
    for (auto* slot : slots)
    {
        if (!slot->isUsed())
            continue;

        // Queues refresh only at explicit points (#10)
        slot->syncConnection();
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

    *resolution = syncManager->getModel().commonDomain.resolution.addRefAndReturn();
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

        const bool changed = this->isActive != static_cast<bool>(isActive);
        this->isActive = isActive;

        if (changed)
        {
            setPortsActiveLocked(isActive);
            invalidateSynchronizationLocked();
            notificationCoordinator->clearReadiness();

            // Deactivation suspends the data flow: queued data and gap events are dropped
            // (they are meaningless once the stream pauses), while descriptor changes stay
            // pending so the reader's type state cannot silently diverge
            if (!isActive)
            {
                for (auto* slot : slots)
                    slot->getQueueReader().dropForInactive();
            }
        }
        evaluateStateLocked();
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

        std::lock_guard lock(mutex);
        normalizeSources(list);

        auto ports = createOrAdoptPorts(list);
        createSlots(ports);

        invalidateModelLocked();
        evaluateStateLocked();
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
    if (!portBinder.assigned())
        slot->getPort().remove();

    slots.erase(slots.begin() + position);
    slotObjects.erase(slotObjects.begin() + position);
    reindexSlotsLocked();

    // S1 (stable slots): only the removed input's per-slot state goes; the remaining
    // inputs keep their readiness/event bits and armed data-loss deadlines
    notificationCoordinator->erase(position);
    dataLossMonitor->erase(position);

    invalidateModelLocked();
    evaluateStateLocked();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setInputUsed(IString* id, Bool isUsed)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);

    const auto position = findSlotByIdLocked(StringPtr::Borrow(id));
    if (position == notFound)
        return OPENDAQ_ERR_NOTFOUND;

    auto* slot = slots[position];
    slot->setUsed(isUsed);
    notificationCoordinator->setUsed(position, isUsed);
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
    }

    invalidateModelLocked();
    evaluateStateLocked();
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

        // The main input defines the output grid identity - changing it invalidates the
        // synchronization; the next evaluation realigns on the new grid (spec section 5)
        invalidateModelLocked();
        evaluateStateLocked();
    }
    notifyCondition.notify_all();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getMainInput(IString** id)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);
    // Empty string means automatic selection - the first used input (error contract 3.3)
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
