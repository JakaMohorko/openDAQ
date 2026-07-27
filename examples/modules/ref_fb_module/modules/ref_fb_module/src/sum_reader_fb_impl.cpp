#include <ref_fb_module/sum_reader_fb_impl.h>
#include <opendaq/function_block_ptr.h>
#include <opendaq/data_descriptor_ptr.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/signal_factory.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/packet_factory.h>
#include <opendaq/sample_type_traits.h>
#include <opendaq/reader_factory.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_utils.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

BEGIN_NAMESPACE_REF_FB_MODULE

namespace SumReader
{

static bool descriptorNotNull(const DataDescriptorPtr& descriptor)
{
    return descriptor.assigned() && descriptor != NullDataDescriptor();
}

static bool getDomainDescriptor(const EventPacketPtr& eventPacket, DataDescriptorPtr& domainDesc)
{
    if (eventPacket.assigned() && eventPacket.getEventId() == event_packet_id::DATA_DESCRIPTOR_CHANGED)
    {
        domainDesc = eventPacket.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR);
        return true;
    }
    return false;
}

static RatioPtr secondsToRatio(double seconds)
{
    return Ratio(static_cast<Int>(std::llround(seconds * 1000.0)), 1000);
}

SumReaderFbImpl::SumReaderFbImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, const PropertyObjectPtr& config)
    : FunctionBlock(CreateType(), ctx, parent, localId)
{
    initComponentStatus();
    setComponentStatusWithMessage(ComponentStatus::Warning, "No signals connected!");

    if (config.assigned())
        notificationMode = static_cast<PacketReadyNotification>(config.getPropertyValue("ReaderNotificationMode"));
    else
        notificationMode = PacketReadyNotification::Scheduler;

    initProperties();
    createDisconnectedPort();
    createReaderLocked();
    createSignals();
}

FunctionBlockTypePtr SumReaderFbImpl::CreateType()
{
    auto config = PropertyObject();
    config.addProperty(SparseSelectionProperty(
        "ReaderNotificationMode",
        Dict<IInteger, IString>({
            {static_cast<Int>(PacketReadyNotification::SameThread), "SameThread"},
            {static_cast<Int>(PacketReadyNotification::Scheduler), "Scheduler"}}),
        2));

    return FunctionBlockType("RefFBModuleSumReader", "Sum with reader", "Calculates signal sum using multi reader", config);
}

void SumReaderFbImpl::initProperties()
{
    // Property-write handlers run under the property object's config lock (the same mutex the
    // acquisition lock wraps), so they act directly without re-locking
    objPtr.addProperty(SelectionProperty("Mode", List<IString>("EqualRates", "MultiRate"), 0));
    objPtr.getOnPropertyValueWrite("Mode") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { modeChanged(); };

    objPtr.addProperty(FloatProperty("DataLossTimeout", 5.0));
    objPtr.getOnPropertyValueWrite("DataLossTimeout") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { readerConfigChanged(); };

    objPtr.addProperty(FloatProperty("MaxSynchronizationDistance", 5.0));
    objPtr.getOnPropertyValueWrite("MaxSynchronizationDistance") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { readerConfigChanged(); };

    objPtr.addProperty(FloatProperty("RecoveryRetryInterval", 5.0));
    objPtr.getOnPropertyValueWrite("RecoveryRetryInterval") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { readerConfigChanged(); };

    // C6: true (default) = park failing inputs so the rest keep summing; false = never
    // exclude anything, report the failing inputs and wait for every input to work
    objPtr.addProperty(BoolProperty("IgnoreFaultyInputs", true));
    objPtr.getOnPropertyValueWrite("IgnoreFaultyInputs") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr&) { ignoreFaultyInputsChanged(); };
}

std::string SumReaderFbImpl::getNextPortID() const
{
    int maxId = 0;
    for (const auto& port : connectedPorts)
    {
        std::string portId = port.getLocalId();
        auto pos = portId.find_last_of('_');
        int curId = std::stoi(portId.substr(pos + 1));
        maxId = curId > maxId ? curId : maxId;
    }

    return fmt::format("SumPort_{}", maxId + 1);
}

void SumReaderFbImpl::createSignals()
{
    sumSignal = createAndAddSignal("Sum");
    sumSignal.setName("Sum");
    sumDomainSignal = createAndAddSignal("SumDomain", nullptr, false);
    sumDomainSignal.setName("SumDomain");
    sumSignal.setDomainSignal(sumDomainSignal);
}

void SumReaderFbImpl::createDisconnectedPort()
{
    std::string id = getNextPortID();
    auto inputPort = createAndAddInputPort(id, notificationMode);
    disconnectedPort = inputPort;
}

bool SumReaderFbImpl::updateInputPortsLocked()
{
    bool connectedPortsChanged = false;
    if (disconnectedPort.assigned() && disconnectedPort.getConnection().assigned())
    {
        const auto portId = disconnectedPort.getGlobalId().toStdString();
        connectedPorts.emplace_back(disconnectedPort);
        cachedValueDescriptors.insert(std::make_pair(portId, NullDataDescriptor()));
        cachedDomainDescriptors.insert(std::make_pair(portId, NullDataDescriptor()));

        // Activate the newly connected port
        reader.setInputUsed(portId, true);
        disconnectedPort.release();
        connectedPortsChanged = true;
    }

    for (auto it = connectedPorts.begin(); it != connectedPorts.end();)
    {
        if (!it->getConnection().assigned())
        {
            const auto portId = it->getGlobalId().toStdString();
            reader.removeInput(it->getGlobalId());

            cachedValueDescriptors.erase(portId);
            cachedDomainDescriptors.erase(portId);
            parkedPorts.erase(portId);
            if (probingPortId == portId)
                probingPortId.clear();
            readerPorts.erase(std::remove(readerPorts.begin(), readerPorts.end(), *it), readerPorts.end());

            this->inputPorts.removeItem(*it);
            it = connectedPorts.erase(it);
            connectedPortsChanged = true;
        }
        else
        {
            ++it;
        }
    }

    if (!disconnectedPort.assigned())
    {
        createDisconnectedPort();

        // Add the empty port to the multi reader and mark it unused
        reader.addInput(disconnectedPort);
        reader.setInputUsed(disconnectedPort.getGlobalId(), false);
        readerPorts.push_back(disconnectedPort);
    }

    if (connectedPortsChanged)
        rateModelDirty = true;

    updateComponentStatusLocked();
    return connectedPortsChanged;
}

void SumReaderFbImpl::createReaderLocked()
{
    if (!disconnectedPort.assigned())
        return;

    // Disposing the reader is necessary to release port ownership
    if (reader.assigned())
    {
        reader.dispose();
        reader.release();
    }

    refreshReaderConfigLocked();

    // No descriptor replay is needed here: adopting a connected port re-enqueues the last
    // descriptor event (setListener calls enqueueLastDescriptor), so the new reader learns
    // the current descriptors on adoption.
    auto builder = MultiReaderBuilder()
                       .setDomainReadType(SampleType::Int64)
                       .setValueReadType(SampleType::Float64)
                       .setAllowDifferentSamplingRates(mode != SumMode::EqualRates)
                       .setDataLossTimeout(secondsToRatio(dataLossTimeoutSeconds))
                       .setMaxSynchronizationDistance(secondsToRatio(maxSyncDistanceSeconds))
                       .setInputPortNotificationMethod(notificationMode);

    for (const auto& port : connectedPorts)
        builder.addInputPort(port);
    builder.addInputPort(disconnectedPort);

    reader = builder.build();
    reader.setInputUsed(disconnectedPort.getGlobalId(), false);

    readerPorts.assign(connectedPorts.begin(), connectedPorts.end());
    readerPorts.push_back(disconnectedPort);

    parkedPorts.clear();
    probingPortId.clear();
    failedInputsMessage.clear();
    readerErrored = false;
    rateModelDirty = true;

    // The external listener stays for topology notifications (connect/disconnect reach the FB
    // through the reader's forwarding - the slots own the ports' listener seats) and for
    // packet-paced probe/staleness checks; parked-port recovery itself is status-driven (Q5)
    reader.setExternalListener(this->thisPtr<InputPortNotificationsPtr>());
    auto thisWeakRef = this->template getWeakRefInternal<IFunctionBlock>();
    reader.setOnDataAvailable(
        [this, thisWeakRef = std::move(thisWeakRef)]
        {
            const auto thisFb = thisWeakRef.getRef();
            if (thisFb.assigned())
                this->onDataReceived();
        });
}

void SumReaderFbImpl::refreshReaderConfigLocked()
{
    dataLossTimeoutSeconds = objPtr.getPropertyValue("DataLossTimeout");
    maxSyncDistanceSeconds = objPtr.getPropertyValue("MaxSynchronizationDistance");
    recoveryRetryIntervalSeconds = objPtr.getPropertyValue("RecoveryRetryInterval");
    ignoreFaultyInputs = objPtr.getPropertyValue("IgnoreFaultyInputs");
}

void SumReaderFbImpl::ignoreFaultyInputsChanged()
{
    const bool previous = ignoreFaultyInputs;
    ignoreFaultyInputs = objPtr.getPropertyValue("IgnoreFaultyInputs");
    if (ignoreFaultyInputs == previous || !reader.assigned())
        return;

    // Both directions get a clean slate: enabling parking re-derives the parked set from the
    // next failure report, disabling it re-includes every input
    createReaderLocked();
    updateComponentStatusLocked();
}

void SumReaderFbImpl::modeChanged()
{
    mode = static_cast<SumMode>(static_cast<Int>(objPtr.getPropertyValue("Mode")));

    // A mode change rebuilds the reader (allowDifferentSamplingRates is builder-time
    // configuration); the parked set is cleared and re-derived from the new mode's rules
    createReaderLocked();
    updateComponentStatusLocked();
}

void SumReaderFbImpl::readerConfigChanged()
{
    // The sync distance and data-loss timeout are builder-only reader configuration: changing
    // them rebuilds the reader. The recovery retry interval is FB-side probe pacing and needs
    // no rebuild.
    const auto previousDataLossTimeout = dataLossTimeoutSeconds;
    const auto previousSyncDistance = maxSyncDistanceSeconds;
    refreshReaderConfigLocked();

    if (reader.assigned() &&
        (dataLossTimeoutSeconds != previousDataLossTimeout || maxSyncDistanceSeconds != previousSyncDistance))
    {
        createReaderLocked();
        updateComponentStatusLocked();
    }
}

void SumReaderFbImpl::onConnected(const InputPortPtr& inputPort)
{
    auto lock = this->getAcquisitionLock2();

    LOG_D("Sum Reader FB: Input port {} connected", inputPort.getLocalId())

    updateInputPortsLocked();
}

void SumReaderFbImpl::onDisconnected(const InputPortPtr& inputPort)
{
    auto lock = this->getAcquisitionLock2();

    LOG_D("Sum Reader FB: Input port {} disconnected", inputPort.getLocalId())
    if (updateInputPortsLocked())
        configureValueDescriptorLocked();
}

void SumReaderFbImpl::onDataReceived()
{
    // The reader drives everything through this one callback. It fires when a block is ready,
    // when an input has a returnable event (including an unused/parked input's recovery event,
    // review Q5), and - since the reader's data-loss rework - when an input misses its packet
    // deadline (DataLost). A stalled or never-delivering input therefore surfaces here as a
    // status the read reports, so the FB needs no packet-received hook or liveness timer of its
    // own: it reacts only to what the reader tells it.
    auto lock = this->getAcquisitionLock2();
    processReaderLocked();
}

void SumReaderFbImpl::processReaderLocked()
{
    if (!reader.assigned() || readerErrored)
        return;

    for (int iteration = 0; iteration < 64; ++iteration)
    {
        SizeT count = reader.getAvailableCount();

        // Rate model underivable with data pending should not happen (data implies a built
        // model); fall back to a zero-count status read
        if (count > 0 && !ensureRateModelLocked())
            count = 0;

        std::vector<std::unique_ptr<double[]>> buffers;
        std::vector<double*> rawBuffers;
        std::vector<SizeT> strides;
        buffers.reserve(readerPorts.size());
        rawBuffers.reserve(readerPorts.size());
        strides.reserve(readerPorts.size());

        for (const auto& port : readerPorts)
        {
            const auto portId = port.getGlobalId().toStdString();
            SizeT divider = 0;
            if (count > 0 && isActivePortLocked(portId))
            {
                const auto it = portDividers.find(portId);
                divider = it != portDividers.end() ? it->second : 0;
            }

            if (divider > 0)
            {
                buffers.push_back(std::make_unique<double[]>(count / divider));
                rawBuffers.push_back(buffers.back().get());
                strides.push_back(blockLcm / divider);
            }
            else
            {
                // Unused inputs (spare and parked ports) contribute nothing; their buffer
                // pointer may be null per the read contract
                rawBuffers.push_back(nullptr);
                strides.push_back(0);
            }
        }

        const MultiReaderStatusPtr status = reader.read(rawBuffers.data(), &count);

        if (count > 0)
            emitSumLocked(rawBuffers, strides, count, status);

        bool acted = false;
        if (status.getReadStatus() == ReadStatus::Event)
        {
            handleEventsLocked(status);
            acted = true;
        }

        acted |= handleStateLocked(status);
        if (readerErrored)
            return;

        // Q5 recovery: a parked (unused) input whose per-input state reports Event fired
        // this wakeup - probe it immediately (unpaced; this may be the last wakeup while
        // every input is parked, and flapping is bounded by the event rate)
        acted |= probeEventfulParkedLocked(status);

        if (!acted && count == 0)
            break;
    }

    maybeProbeLocked();
}

void SumReaderFbImpl::emitSumLocked(const std::vector<double*>& buffers,
                                    const std::vector<SizeT>& strides,
                                    SizeT commonCount,
                                    const MultiReaderStatusPtr& status)
{
    if (!descriptorNotNull(sumDataDescriptor) || !descriptorNotNull(sumDomainDataDescriptor))
        return;

    const SizeT blocks = commonCount / blockLcm;
    if (blocks == 0)
        return;

    // The status offset is the common-domain tick of the first sample of this read; reads are
    // block-aligned, so it is also the first output sample's tick
    const auto sumDomainPacket = DataPacket(sumDomainDataDescriptor, blocks, status.getOffset());
    const auto sumValuePacket = DataPacketWithDomain(sumDomainPacket, sumDataDescriptor, blocks);
    double* sumValueData = static_cast<double*>(sumValuePacket.getRawData());
    std::fill_n(sumValueData, blocks, 0.0);

    for (SizeT slot = 0; slot < buffers.size(); ++slot)
    {
        const double* signalData = buffers[slot];
        if (!signalData || strides[slot] == 0)
            continue;

        // The sum is defined at ticks where every input has a sample - the block starts;
        // input `slot` carries `strides[slot] == blockLcm / divider` samples per block
        const SizeT stride = strides[slot];
        for (SizeT block = 0; block < blocks; ++block)
            sumValueData[block] += signalData[block * stride];
    }

    sumDomainSignal.sendPacket(sumDomainPacket);
    sumSignal.sendPacket(sumValuePacket);
}

void SumReaderFbImpl::handleEventsLocked(const MultiReaderStatusPtr& status)
{
    bool valueDescriptorsChanged = false;

    for (const auto& [portId, packet] : status.getEventPackets())
    {
        const EventPacketPtr event = packet;
        // Gap events need no action here: the reader resynchronizes in-band
        if (!event.assigned() || event.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
            continue;

        const DataDescriptorPtr valueDesc = event.getParameters().get(event_packet_param::DATA_DESCRIPTOR);
        const DataDescriptorPtr domainDesc = event.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR);
        const auto id = StringPtr(portId).toStdString();

        if (descriptorNotNull(valueDesc))
        {
            cachedValueDescriptors[id] = valueDesc;
            valueDescriptorsChanged = true;
        }
        if (descriptorNotNull(domainDesc))
        {
            cachedDomainDescriptors[id] = domainDesc;
            rateModelDirty = true;
        }
    }

    // The main descriptor's domain part is the common output domain - the grid the status
    // offset is expressed in
    DataDescriptorPtr commonDomain;
    if (getDomainDescriptor(status.getMainDescriptor(), commonDomain) && descriptorNotNull(commonDomain))
    {
        commonDomainDescriptor = commonDomain;
        rateModelDirty = true;
    }

    if (valueDescriptorsChanged)
        configureValueDescriptorLocked();
}

bool SumReaderFbImpl::handleStateLocked(const MultiReaderStatusPtr& status)
{
    const auto readStatus = status.getReadStatus();

    if (readStatus == ReadStatus::Fail)
    {
        // Unrecoverable: report and stop issuing reads. No silent reader re-creation - an
        // internal invariant broke, and surfacing it is the handling. A mode change or
        // reconnect still rebuilds the reader through the normal path (explicit user action).
        readerErrored = true;
        const StringPtr message = status.getStateMessage();
        setComponentStatusWithMessage(
            ComponentStatus::Error,
            fmt::format("Reader failed unrecoverably: {}", message.assigned() ? message.toStdString() : "unknown error"));
        return false;
    }

    switch (readStatus)
    {
        case ReadStatus::InputsFailed:
        {
            if (!ignoreFaultyInputs)
            {
                // Parking disabled by configuration: never exclude anything - report which
                // inputs are failing and wait for all of them to work
                const auto message = describeFailedInputsLocked(status);
                if (message != failedInputsMessage)
                {
                    failedInputsMessage = message;
                    updateComponentStatusLocked();
                }
                return false;
            }

            // The per-input states name the failing inputs directly (the FB constructs the
            // reader from ports, so the input ids are the ports' global ids)
            bool acted = false;
            std::unordered_set<std::string> affected;
            for (const auto& [inputId, stateValue] : status.getInputStates())
            {
                const auto inputState = static_cast<InputState>(static_cast<Int>(stateValue));
                std::string reason;
                switch (inputState)
                {
                    case InputState::Incompatible:
                    {
                        const StringPtr message = status.getStateMessage();
                        reason = fmt::format("incompatible: {}", message.assigned() ? message.toStdString() : "");
                        break;
                    }
                    case InputState::SynchronizationFailed:
                        reason = "cannot synchronize";
                        break;
                    case InputState::DataLost:
                        reason = "no data";
                        break;
                    default:
                        continue;
                }

                const auto portId = StringPtr(inputId).toStdString();
                const auto port = std::find_if(connectedPorts.begin(),
                                               connectedPorts.end(),
                                               [&portId](const InputPortPtr& candidate)
                                               { return candidate.getGlobalId().toStdString() == portId; });
                if (port == connectedPorts.end() || !port->getConnection().assigned())
                    continue;

                affected.insert(portId);
                if (parkedPorts.find(portId) == parkedPorts.end() || portId == probingPortId)
                {
                    parkPortLocked(*port, reason);
                    acted = true;
                }
            }

            // A pending probe not implicated in this failure has proven itself
            if (!probingPortId.empty() && affected.find(probingPortId) == affected.end())
            {
                unparkLocked(probingPortId);
                acted = true;
            }

            if (acted)
                configureValueDescriptorLocked();
            return acted;
        }
        case ReadStatus::Ok:
            // Recovered: clear a standing failure report from the non-parking mode
            if (!failedInputsMessage.empty())
            {
                failedInputsMessage.clear();
                updateComponentStatusLocked();
            }
            // Synchronized: a probe that made it into a synchronized read has proven itself
            if (!probingPortId.empty())
            {
                unparkLocked(probingPortId);
                configureValueDescriptorLocked();
                return true;
            }
            return false;
        default:
            // Preparing/Inactive/Event: nothing to park or probe here - be patient (Preparing
            // is never a reason to act), and events are handled by handleEventsLocked. But a
            // standing "inputs failing (not excluded)" report from IgnoreFaultyInputs=false is
            // now stale (the reader left InputsFailed), so clear it - otherwise it would
            // linger until a read finally returns Ok, misreporting a since-recovered input.
            if (!failedInputsMessage.empty())
            {
                failedInputsMessage.clear();
                updateComponentStatusLocked();
            }
            return false;
    }
}

void SumReaderFbImpl::configureValueDescriptorLocked()
{
    UnitPtr unit;
    bool unitSeen = false;
    double lowValue = 0;
    double highValue = 0;
    bool allKnown = true;
    std::vector<InputPortPtr> mismatched;

    for (const auto& port : connectedPorts)
    {
        const auto portId = port.getGlobalId().toStdString();
        if (!isActivePortLocked(portId))
            continue;

        const auto it = cachedValueDescriptors.find(portId);
        if (it == cachedValueDescriptors.end() || !descriptorNotNull(it->second))
        {
            // Still waiting for this input's descriptors; the reader reports the same via
            // WaitingForDescriptors
            allKnown = false;
            continue;
        }

        const auto& descriptor = it->second;
        if (!unitSeen)
        {
            unit = descriptor.getUnit();
            unitSeen = true;
        }
        else if (descriptor.getUnit() != unit)
        {
            // Unit compatibility is this function block's own rule, not the reader's;
            // parking the offender keeps the rest summing
            mismatched.push_back(port);
            continue;
        }

        const auto range = descriptor.getValueRange();
        if (range.assigned())
        {
            lowValue += range.getLowValue().getFloatValue();
            highValue += range.getHighValue().getFloatValue();
        }
    }

    for (const auto& port : mismatched)
        parkPortLocked(port, "unit mismatch");

    if (!allKnown || !unitSeen)
    {
        updateComponentStatusLocked();
        return;
    }

    RangePtr range;
    if (std::fabs(lowValue - highValue) > 1e-9)
        range = Range(lowValue, highValue);
    else
        range = Range(-10, 10);

    sumDataDescriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(unit).setValueRange(range).build();
    if (sumSignal.getDescriptor() != sumDataDescriptor)
        sumSignal.setDescriptor(sumDataDescriptor);

    updateComponentStatusLocked();
}

bool SumReaderFbImpl::ensureRateModelLocked()
{
    if (!rateModelDirty)
        return true;

    if (!descriptorNotNull(commonDomainDescriptor))
        return false;

    const Int commonSampleRate = reader.getCommonSampleRate();
    const RatioPtr resolution = reader.getTickResolution();
    if (commonSampleRate <= 0 || !resolution.assigned())
        return false;

    std::unordered_map<std::string, SizeT> dividers;
    SizeT lcmOfDividers = 1;
    for (const auto& port : connectedPorts)
    {
        const auto portId = port.getGlobalId().toStdString();
        if (!isActivePortLocked(portId))
            continue;

        const auto it = cachedDomainDescriptors.find(portId);
        if (it == cachedDomainDescriptors.end() || !descriptorNotNull(it->second))
            return false;

        std::int64_t rate = 0;
        try
        {
            rate = reader::getSampleRate(it->second);
        }
        catch (const DaqException&)
        {
            return false;
        }
        if (rate <= 0 || commonSampleRate % rate != 0)
            return false;

        const auto divider = static_cast<SizeT>(commonSampleRate / rate);
        dividers[portId] = divider;
        lcmOfDividers = std::lcm(lcmOfDividers, divider);
    }

    if (dividers.empty())
        return false;

    portDividers = std::move(dividers);
    blockLcm = lcmOfDividers;

    // Output grid: one sample per aligned block, i.e. every blockLcm-th common-rate sample.
    // One common-rate sample period is a whole number of common-domain ticks by construction.
    const std::int64_t ticksPerCommonSample =
        resolution.getDenominator() / (resolution.getNumerator() * commonSampleRate);
    if (ticksPerCommonSample <= 0)
        return false;

    const auto commonRule = commonDomainDescriptor.getRule();
    const NumberPtr ruleStart = commonRule.assigned() ? commonRule.getParameters().get("start").asPtr<INumber>() : NumberPtr(0);
    sumDomainDataDescriptor =
        DataDescriptorBuilderCopy(commonDomainDescriptor)
            .setRule(LinearDataRule(static_cast<Int>(ticksPerCommonSample * static_cast<std::int64_t>(blockLcm)), ruleStart))
            .build();
    if (sumDomainSignal.getDescriptor() != sumDomainDataDescriptor)
        sumDomainSignal.setDescriptor(sumDomainDataDescriptor);

    rateModelDirty = false;
    return true;
}

void SumReaderFbImpl::parkPortLocked(const InputPortPtr& port, const std::string& reason)
{
    const auto portId = port.getGlobalId().toStdString();
    reader.setInputUsed(port.getGlobalId(), false);

    auto& info = parkedPorts[portId];
    info.reason = reason;
    info.since = std::chrono::steady_clock::now();
    if (probingPortId == portId)
        probingPortId.clear();

    rateModelDirty = true;
    LOG_D("Sum Reader FB: Parked input {} ({})", port.getLocalId(), reason)
    updateComponentStatusLocked();
}

void SumReaderFbImpl::unparkLocked(const std::string& portId)
{
    parkedPorts.erase(portId);
    if (probingPortId == portId)
        probingPortId.clear();
    rateModelDirty = true;

    const auto port = findPortByIdLocked(portId);
    if (port.assigned())
        LOG_D("Sum Reader FB: Recovered input {}", port.getLocalId())
    updateComponentStatusLocked();
}

void SumReaderFbImpl::probePortLocked(const std::string& portId)
{
    const auto parked = parkedPorts.find(portId);
    if (parked == parkedPorts.end())
        return;

    probingPortId = portId;
    lastProbeTime = std::chrono::steady_clock::now();

    // Re-testing = marking the input used again; the next status evaluation either clears it
    // (recovered) or re-reports the failure (re-parked)
    reader.setInputUsed(String(portId), true);
    rateModelDirty = true;
}

bool SumReaderFbImpl::probeEventfulParkedLocked(const MultiReaderStatusPtr& status)
{
    if (readerErrored || !probingPortId.empty() || parkedPorts.empty())
        return false;

    for (const auto& [inputId, stateValue] : status.getInputStates())
    {
        if (static_cast<InputState>(static_cast<Int>(stateValue)) != InputState::Event)
            continue;

        const auto portId = StringPtr(inputId).toStdString();
        if (parkedPorts.find(portId) == parkedPorts.end())
            continue;

        probePortLocked(portId);
        return true;
    }
    return false;
}

std::string SumReaderFbImpl::describeFailedInputsLocked(const MultiReaderStatusPtr& status) const
{
    std::string result;
    for (const auto& [inputId, stateValue] : status.getInputStates())
    {
        std::string reason;
        switch (static_cast<InputState>(static_cast<Int>(stateValue)))
        {
            case InputState::Incompatible:
                reason = "incompatible";
                break;
            case InputState::SynchronizationFailed:
                reason = "cannot synchronize";
                break;
            case InputState::DataLost:
                reason = "no data";
                break;
            default:
                continue;
        }

        const auto port = findPortByIdLocked(StringPtr(inputId).toStdString());
        if (!result.empty())
            result += ", ";
        result += fmt::format("{} ({})", port.assigned() ? port.getLocalId().toStdString() : StringPtr(inputId).toStdString(), reason);
    }
    return result;
}

void SumReaderFbImpl::maybeProbeLocked()
{
    if (readerErrored || recoveryRetryIntervalSeconds <= 0)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration<double>(recoveryRetryIntervalSeconds);

    if (!probingPortId.empty())
    {
        // A probe normally resolves in handleStateLocked: Ok unparks it, InputsFailed
        // re-parks it. A probe of a port whose producer has gone permanently silent resolves
        // as neither - the reader sits in Preparing (WaitingForData) with the probed port
        // used-but-empty, which blocks the healthy inputs from summing and grows their queues
        // unbounded. Abandon such a stuck probe after one interval (re-park it) so the healthy
        // inputs resume; a later cycle retries it, interval-paced.
        if (now - lastProbeTime >= interval)
        {
            const auto port = findPortByIdLocked(probingPortId);
            if (port.assigned())
            {
                parkPortLocked(port, "no data");
                lastProbeTime = now;  // re-pace so the retry waits a full interval
                configureValueDescriptorLocked();
            }
        }
        return;
    }

    if (parkedPorts.empty())
        return;
    if (now - lastProbeTime < interval)
        return;

    // Periodic fallback: probe the oldest parked port, one at a time, so a bad port cannot
    // repeatedly interrupt the healthy ones
    auto oldest = parkedPorts.begin();
    for (auto it = std::next(parkedPorts.begin()); it != parkedPorts.end(); ++it)
    {
        if (it->second.since < oldest->second.since)
            oldest = it;
    }
    probePortLocked(oldest->first);
}

void SumReaderFbImpl::updateComponentStatusLocked()
{
    // Error is latched until an explicit rebuild (mode change or reconnect)
    if (readerErrored)
        return;

    if (connectedPorts.empty())
    {
        setComponentStatusWithMessage(ComponentStatus::Warning, "No signals connected!");
        return;
    }

    if (!parkedPorts.empty())
    {
        std::string parkedList;
        for (const auto& [portId, info] : parkedPorts)
        {
            const auto port = findPortByIdLocked(portId);
            if (!parkedList.empty())
                parkedList += ", ";
            parkedList += fmt::format("{} ({})", port.assigned() ? port.getLocalId().toStdString() : portId, info.reason);
        }

        if (parkedPorts.size() >= connectedPorts.size())
            setComponentStatusWithMessage(ComponentStatus::Warning, fmt::format("No usable inputs - all excluded from sum: {}", parkedList));
        else
            setComponentStatusWithMessage(ComponentStatus::Warning, fmt::format("Inputs excluded from sum: {}", parkedList));
        return;
    }

    if (!failedInputsMessage.empty())
    {
        // IgnoreFaultyInputs=false: nothing is excluded; the sum waits for every input
        setComponentStatusWithMessage(ComponentStatus::Warning, fmt::format("Inputs failing (not excluded): {}", failedInputsMessage));
        return;
    }

    setComponentStatus(ComponentStatus::Ok);
}

bool SumReaderFbImpl::isActivePortLocked(const std::string& portId) const
{
    // A port under probe is marked used again - the reader delivers its samples, so the sum
    // must include them even though the port stays in the parked set until confirmed
    if (parkedPorts.find(portId) != parkedPorts.end() && portId != probingPortId)
        return false;
    for (const auto& port : connectedPorts)
    {
        if (port.getGlobalId().toStdString() == portId)
            return true;
    }
    return false;
}

InputPortPtr SumReaderFbImpl::findPortByIdLocked(const std::string& portId) const
{
    for (const auto& port : readerPorts)
    {
        if (port.getGlobalId().toStdString() == portId)
            return port;
    }
    return nullptr;
}
}

END_NAMESPACE_REF_FB_MODULE
