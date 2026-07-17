#include <coreobjects/ownable_ptr.h>
#include <coreobjects/property_object_factory.h>
#include <coretypes/validation.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/event_packet_utils.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reader_errors.h>
#include <opendaq/reader_utils.h>
#include <opendaq/tags_private_ptr.h>

#include <date/date.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <chrono>

using namespace std::chrono;

BEGIN_NAMESPACE_OPENDAQ

// --- Construction -----------------------------------------------------------------------------

MultiReaderImpl::MultiReaderImpl(const ListPtr<IComponent>& list,
                                 SampleType valueReadType,
                                 SampleType domainReadType,
                                 ReadMode mode,
                                 ReadTimeoutType /*timeoutType*/,  // only All is honored (spec section 7.2)
                                 Int requiredCommonSampleRate,
                                 Bool startOnFullUnitOfDomain,
                                 SizeT minReadCount)
    : requiredCommonSampleRate(requiredCommonSampleRate)
    , startOnFullUnitOfDomain(startOnFullUnitOfDomain)
    , minReadCount(minReadCount)
    , notificationMethodsList(List<PacketReadyNotification>())
    , valueReadType(valueReadType)
    , domainReadType(domainReadType)
    , readMode(mode)
{
    this->internalAddRef();
    try
    {
        checkListSizeAndCacheContext(list);
        loggerComponent = context.getLogger().getOrAddComponent("MultiReader");
        typeOfInputs = sourceComponentsType(list);

        // Bounded packetReceived makes SameThread safe as the default for both
        // construction types (behavior change, spec section 8.5)
        notificationMethod = PacketReadyNotification::SameThread;

        resolvedDomainReadType = domainReadType == SampleType::Undefined ? SampleType::Int64 : domainReadType;

        syncManager = std::make_unique<SynchronizationManager>(loggerComponent);
        readCoordinator = std::make_unique<ReadCoordinator>(loggerComponent);
        notificationCoordinator = std::make_unique<NotificationCoordinator>(context.getScheduler(), loggerComponent);
        notificationCoordinator->setEvaluationCallback([this] { onCoalescedEvaluation(); });
        applyConfigToSyncManager();

        auto ports = createOrAdoptPorts(list);
        createSlots(ports);

        std::lock_guard lock(mutex);
        evaluateStateLocked();
    }
    catch (...)
    {
        this->releaseWeakRefOnException();
        throw;
    }
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

        loggerComponent = old->loggerComponent;
        readMode = old->readMode;
        typeOfInputs = old->typeOfInputs;
        portBinder = old->portBinder;
        startOnFullUnitOfDomain = old->startOnFullUnitOfDomain;
        isActive = old->isActive;
        minReadCount = old->minReadCount;
        tickOffsetTolerance = old->tickOffsetTolerance;
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
        applyConfigToSyncManager();

        createSlots(ports);

        std::lock_guard lock(mutex);
        for (SizeT i = 0; i < usedFlags.size() && i < slots.size(); ++i)
        {
            slots[i]->setUsed(usedFlags[i]);
            notificationCoordinator->setUsed(i, usedFlags[i]);
            slots[i]->getQueueReader().seedDescriptors(oldValueDescriptors[i], oldDomainDescriptors[i]);
        }
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
        checkListSizeAndCacheContext(sourceComponents);

        loggerComponent = context.getLogger().getOrAddComponent("MultiReader");
        typeOfInputs = sourceComponentsType(sourceComponents);

        resolvedDomainReadType = domainReadType == SampleType::Undefined ? SampleType::Int64 : domainReadType;

        syncManager = std::make_unique<SynchronizationManager>(loggerComponent);
        readCoordinator = std::make_unique<ReadCoordinator>(loggerComponent);
        notificationCoordinator = std::make_unique<NotificationCoordinator>(context.getScheduler(), loggerComponent);
        notificationCoordinator->setEvaluationCallback([this] { onCoalescedEvaluation(); });
        applyConfigToSyncManager();

        auto ports = createOrAdoptPorts(sourceComponents);
        createSlots(ports);

        std::lock_guard lock(mutex);
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

void MultiReaderImpl::checkListSizeAndCacheContext(const ListPtr<IComponent>& list)
{
    if (!list.assigned())
        DAQ_THROW_EXCEPTION(NotAssignedException, "List of inputs is not assigned");
    if (list.getCount() == 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Need at least one signal.");
    context = list[0].getContext();
}

MultiReaderImpl::InputType MultiReaderImpl::sourceComponentsType(const ListPtr<IComponent>& sources) const
{
    if (sources.getCount() == 0)
        return InputType::Unknown;

    if (sources[0].supportsInterface(IInputPort::Id))
        return InputType::Ports;
    if (sources[0].supportsInterface(ISignal::Id))
        return InputType::Signals;
    DAQ_THROW_EXCEPTION(InvalidParameterException, "Invalid component type, only IInputPort and ISignal are supported.");
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

        auto slotObject = createWithImplementation<IInputPortNotifications, InputSlot>(position,
                                                                                       port,
                                                                                       valueReadType,
                                                                                       resolvedDomainReadType,
                                                                                       readMode,
                                                                                       loggerComponent,
                                                                                       static_cast<IInputSlotListener*>(this),
                                                                                       typeOfInputs == InputType::Signals);
        auto* slot = static_cast<InputSlot*>(slotObject.getObject());
        port.setListener(slotObject);

        slotObjects.push_back(std::move(slotObject));
        slots.push_back(slot);
        ++position;
    }

    notificationCoordinator->resize(slots.size());
}

void MultiReaderImpl::applyConfigToSyncManager()
{
    syncManager->setRequiredCommonSampleRate(requiredCommonSampleRate);
    syncManager->setAllowDifferentRates(allowDifferentRates);
    syncManager->setStartOnFullUnitOfDomain(startOnFullUnitOfDomain);
}

// --- State machine ----------------------------------------------------------------------------

void MultiReaderImpl::setStateLocked(MultiReaderState newState, std::string message, std::vector<SizeT> affected)
{
    state = newState;
    stateMessage = std::move(message);
    stateAffectedInputs = std::move(affected);
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
}

std::vector<QueueReader*> MultiReaderImpl::collectUsedReaders(std::vector<SizeT>& slotIndices) const
{
    std::vector<QueueReader*> readers;
    slotIndices.clear();
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        if (!slots[i]->isUsed())
            continue;
        readers.push_back(&slots[i]->getQueueReader());
        slotIndices.push_back(i);
    }
    return readers;
}

void MultiReaderImpl::updateMainDescriptorsLocked()
{
    if (slots.empty())
        return;

    // The main input is the first slot (construction order); Phase 4 makes it selectable
    auto& reader = slots.front()->getQueueReader();
    if (reader.getValueDescriptor().assigned())
        mainValueDescriptor = reader.getValueDescriptor();
    if (reader.getDomainDescriptor().assigned())
        mainDomainDescriptor = reader.getDomainDescriptor();
}

void MultiReaderImpl::evaluateStateLocked()
{
    // 1. Error is terminal; inactivity gates everything else
    if (invalid)
    {
        setStateLocked(MultiReaderState::Error, stateMessage.empty() ? "Reader is invalid" : stateMessage);
        return;
    }
    if (!isActive)
    {
        // Inactivity suspends data flow only: descriptor/gap events are enqueued regardless
        // of the active flag and must still surface through reads (and through the
        // dataAvailable callback, which is why the event bits are maintained here too)
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
            setStateLocked(MultiReaderState::EventPending,
                           fmt::format("Events pending on inputs [{}]", fmt::join(inactiveEventInputs, ", ")),
                           std::move(inactiveEventInputs));
        }
        else
        {
            setStateLocked(MultiReaderState::Inactive);
        }
        return;
    }

    // 2. Resolve the used set
    std::vector<SizeT> slotIndices;
    const auto usedReaders = collectUsedReaders(slotIndices);
    if (usedReaders.empty())
    {
        invalidateSynchronizationLocked();
        setStateLocked(MultiReaderState::WaitingForConnections, "No used inputs");
        return;
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
            setStateLocked(MultiReaderState::WaitingForConnections,
                           fmt::format("Inputs [{}] have no signal connected", fmt::join(unconnected, ", ")),
                           std::move(unconnected));
            return;
        }
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
        if (!eventInputs.empty())
        {
            // Reads may still return the events already pending, but the dataAvailable
            // callback waits for the in-flight handshake - its event arrives momentarily and
            // re-triggers evaluation, so the callback sees every input's initial event at once
            if (handshakeInFlight)
            {
                for (const auto index : slotIndices)
                    notificationCoordinator->setEvent(index, false);
            }
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
                    syncManager->buildCommonModel(usedReaders, slotIndices, 0);
            }

            invalidateSynchronizationLocked();
            setStateLocked(MultiReaderState::EventPending,
                           fmt::format("Events pending on inputs [{}]", fmt::join(eventInputs, ", ")),
                           std::move(eventInputs));
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
            setStateLocked(MultiReaderState::WaitingForDescriptors,
                           fmt::format("Inputs [{}] have no descriptors yet", fmt::join(missing, ", ")),
                           std::move(missing));
            return;
        }
    }

    updateMainDescriptorsLocked();

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
            setStateLocked(MultiReaderState::Incompatible,
                           fmt::format("Inputs [{}] are not readable with the current descriptors", fmt::join(invalidInputs, ", ")),
                           std::move(invalidInputs));
            return;
        }
    }

    // Already synchronized: nothing further to establish
    if (syncManager->getCommonStart() != nullptr)
    {
        setStateLocked(MultiReaderState::Synchronized);
    }
    else
    {
        // 8. Cross-input compatibility and the common model
        auto setup = syncManager->buildCommonModel(usedReaders, slotIndices, 0);
        if (!setup.ok())
        {
            readCoordinator->invalidate();
            setStateLocked(MultiReaderState::Incompatible, std::move(setup.message), std::move(setup.affectedInputs));
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
                setStateLocked(MultiReaderState::WaitingForData,
                               fmt::format("Waiting for data on inputs [{}]", fmt::join(empty, ", ")),
                               std::move(empty));
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
                setStateLocked(MultiReaderState::Synchronized);
                break;
            case SyncOutcome::NeedMoreData:
                setStateLocked(MultiReaderState::Synchronizing, std::move(result.message), std::move(result.affectedInputs));
                break;
            case SyncOutcome::EventPending:
                invalidateSynchronizationLocked();
                setStateLocked(MultiReaderState::EventPending, std::move(result.message), std::move(result.affectedInputs));
                for (const auto index : result.affectedInputs)
                    notificationCoordinator->setEvent(index, true);
                break;
            case SyncOutcome::Failed:
                // Synchronization failure no longer deactivates the reader (spec section 8.5)
                setStateLocked(MultiReaderState::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs));
                break;
        }
    }

    // 13. Readiness for the callback gate: a full aligned block while synchronized,
    // the first sample while still synchronizing
    for (SizeT position = 0; position < usedReaders.size(); ++position)
    {
        bool ready = false;
        if (state == MultiReaderState::Synchronized && syncManager->hasModel())
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

        evaluateStateLocked();
        if (notificationCoordinator->shouldInvokeCallback())
            callback = readCallback;
    }

    notifyCondition.notify_all();

    if (callback.assigned())
        wrapHandler(callback);
}

// --- IInputSlotListener -----------------------------------------------------------------------

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
    // Bounded producer path: no locks, no queue access (spec section 9)
    notificationCoordinator->requestEvaluation();
    notifyCondition.notify_all();

    if (externalListener.assigned())
    {
        if (auto listener = externalListener.getRef(); listener.assigned() && slotIndex < slots.size())
            listener->packetReceived(slots[slotIndex]->getPort());
    }
}

// --- Status and offset ------------------------------------------------------------------------

MultiReaderStatusPtr MultiReaderImpl::createStatusLocked(const DictPtr<IString, IEventPacket>& eventPackets,
                                                         const NumberPtr& offset) const
{
    auto mainDescriptor = DataDescriptorChangedEventPacket(descriptorToEventPacketParam(mainValueDescriptor),
                                                           descriptorToEventPacketParam(mainDomainDescriptor));
    // The status reports the stream condition (error contract section 3.3): reads in
    // Incompatible/SynchronizationFailed carry an invalid status (ReadStatus::Fail without
    // events) while the reader itself stays recoverable
    const bool statusValid =
        !invalid && state != MultiReaderState::Incompatible && state != MultiReaderState::SynchronizationFailed;
    return MultiReaderStatus(mainDescriptor, eventPackets, statusValid, offset);
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

        // One event per input per call (spec section 7.2); the pop applies descriptor changes
        auto packet = reader.popFrontEvent();
        if (packet.assigned())
            events.set(slot->getPort().getGlobalId(), packet);

        notificationCoordinator->setEvent(slot->getIndex(), reader.hasPendingEvents());
    }

    // Every returned event invalidates synchronization; descriptor changes may have
    // changed rates, so the whole model is rebuilt right away - accessors like
    // getCommonSampleRate must reflect the new descriptors as soon as the events are out
    invalidateModelLocked();
    updateMainDescriptorsLocked();
    evaluateStateLocked();

    return createStatusLocked(events.getCount() > 0 ? events : DictPtr<IString, IEventPacket>(nullptr));
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

    evaluateStateLocked();

    // Zero-count handshake: report events or the current state without consuming data.
    // With a timeout the call waits for events to arrive instead of returning immediately.
    if (*count == 0)
    {
        if (timeoutMs > 0 && state != MultiReaderState::EventPending)
        {
            notifyCondition.wait_for(lock,
                                     milliseconds(timeoutMs),
                                     [&]
                                     {
                                         if (invalid)
                                             return true;
                                         evaluateStateLocked();
                                         return state == MultiReaderState::EventPending;
                                     });
        }
        MultiReaderStatusPtr statusPtr =
            state == MultiReaderState::EventPending ? readEventsLocked() : createStatusLocked();
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
                                     evaluateStateLocked();
                                     if (state == MultiReaderState::EventPending)
                                         return true;
                                     if (state != MultiReaderState::Synchronized)
                                         return false;

                                     std::vector<SizeT> slotIndices;
                                     const auto used = collectUsedReaders(slotIndices);
                                     const auto available =
                                         readCoordinator->getAvailableCount(used, syncManager->getModel(), minReadCount);
                                     const SizeT block = syncManager->getModel().blockLcm;
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
        evaluateStateLocked();
    }

    if (state == MultiReaderState::EventPending)
    {
        auto statusPtr = readEventsLocked();
        if (status)
            *status = statusPtr.detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    if (state != MultiReaderState::Synchronized)
    {
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    // Plan against availability, then commit every input - a partial commit is impossible
    std::vector<SizeT> slotIndices;
    const auto used = collectUsedReaders(slotIndices);
    const auto& model = syncManager->getModel();

    std::vector<void*> usedValueBuffers(used.size(), nullptr);
    std::vector<void*> usedDomainBuffers(used.size(), nullptr);
    for (SizeT position = 0; position < used.size(); ++position)
    {
        if (valueBuffers)
            usedValueBuffers[position] = valueBuffers[slotIndices[position]];
        if (domainBuffers)
            usedDomainBuffers[position] = domainBuffers[slotIndices[position]];
    }

    const auto plan = readCoordinator->createPlan(requested,
                                                  used,
                                                  model,
                                                  minReadCount,
                                                  skip ? nullptr : usedValueBuffers.data(),
                                                  skip ? nullptr : usedDomainBuffers.data());

    const auto offsetTick = nextReadTick;

    std::string commitError;
    const auto commitResult =
        skip ? readCoordinator->skip(plan, used, commitError) : readCoordinator->commit(plan, used, commitError);
    if (commitResult != CommitResult::Ok)
    {
        invalid = true;
        setStateLocked(MultiReaderState::Error, std::move(commitError));
        if (status)
            *status = createStatusLocked().detach();
        *count = 0;
        return OPENDAQ_SUCCESS;
    }

    if (plan.commonCount > 0 && nextReadTick.has_value() && model.commonSampleRate > 0)
    {
        // One common-rate sample spans a whole number of common ticks (spec section 4.1)
        const auto ticksPerSample = model.commonDomain.resolution.getDenominator() /
                                    (model.commonDomain.resolution.getNumerator() * model.commonSampleRate);
        nextReadTick = *nextReadTick + static_cast<std::int64_t>(plan.commonCount) * ticksPerSample;
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

    evaluateStateLocked();
    if (state == MultiReaderState::Synchronized)
    {
        std::vector<SizeT> slotIndices;
        const auto used = collectUsedReaders(slotIndices);
        *count = readCoordinator->getAvailableCount(used, syncManager->getModel(), minReadCount);
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

    const auto originString = date::format("%FT%TZ", syncManager->getModel().commonDomain.epoch);
    *origin = String(originString).detach();
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
    *isSynchronized = state == MultiReaderState::Synchronized ? True : False;
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
        if (typeOfInputs == InputType::Unknown)
            typeOfInputs = sourceComponentsType(list);

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

    auto* slot = slots[position];
    slot->detachListener();
    if (!portBinder.assigned())
        slot->getPort().remove();

    slots.erase(slots.begin() + position);
    slotObjects.erase(slotObjects.begin() + position);
    reindexSlotsLocked();

    notificationCoordinator->resize(0);
    notificationCoordinator->resize(slots.size());
    for (SizeT i = 0; i < slots.size(); ++i)
        notificationCoordinator->setUsed(i, slots[i]->isUsed());

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

    if (isUsed)
    {
        // Re-enabled inputs restart from the live stream: revalidation and
        // resynchronization run on the next evaluation
        slot->setPortActive(this->isActive);
        slot->rebindConnection();
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

// --- IInputPortNotifications (compat pass-through; slots are the real listeners) ---------------

ErrCode MultiReaderImpl::acceptsSignal(IInputPort* port, ISignal* signal, Bool* accept)
{
    OPENDAQ_PARAM_NOT_NULL(port);
    OPENDAQ_PARAM_NOT_NULL(signal);
    OPENDAQ_PARAM_NOT_NULL(accept);

    if (externalListener.assigned() && externalListener.getRef().assigned())
        return externalListener.getRef()->acceptsSignal(port, signal, accept);

    *accept = true;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::connected(IInputPort* port)
{
    OPENDAQ_PARAM_NOT_NULL(port);

    if (externalListener.assigned() && externalListener.getRef().assigned())
        return externalListener.getRef()->connected(port);
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::disconnected(IInputPort* port)
{
    OPENDAQ_PARAM_NOT_NULL(port);

    if (externalListener.assigned() && externalListener.getRef().assigned())
        return externalListener.getRef()->disconnected(port);
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::packetReceived(IInputPort* inputPort)
{
    OPENDAQ_PARAM_NOT_NULL(inputPort);

    if (externalListener.assigned() && externalListener.getRef().assigned())
        return externalListener.getRef()->packetReceived(inputPort);
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
    setStateLocked(MultiReaderState::Error, "Reader was marked as invalid");
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
    if (notificationCoordinator)
        notificationCoordinator->detach();
    for (auto* slot : slots)
        slot->detachListener();

    portBinder = nullptr;
    externalListener = nullptr;
    readCallback = nullptr;
    invalid = true;
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
