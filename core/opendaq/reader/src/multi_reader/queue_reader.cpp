#include <opendaq/multi_reader/queue_reader.h>

#include <opendaq/custom_log.h>
#include <opendaq/event_packet_utils.h>

#include <algorithm>
#include <cassert>
#include <limits>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

// COMMENT: We should maybe revisit the IMultiReaderStatus and how this is reported to them. Using event packets
//          is likely not the most intuitive.
SignalEvent::SignalEvent(const EventPacketPtr& packet)
    : eventType(SignalEventType::NoChange)
    , domainDescriptor(nullptr)
    , valueDescriptor(nullptr)
    , gapDiff(0)
{
    if (packet.getEventId() == event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED)
    {
        eventType = SignalEventType::Gap;
        gapDiff = packet.getParameters().get(event_packet_param::GAP_DIFF);
    }
    else
    {
        // The parse distinguishes "changed to null" (explicit null marker) from "unchanged"
        // (parameter absent) - a removed descriptor must not be mistaken for no change.
        const auto [valueDescChanged, domainDescChanged, newValueDescriptor, newDomainDescriptor] = parseDataDescriptorEventPacket(packet);
        valueDescriptorChanged = valueDescChanged;
        domainDescriptorChanged = domainDescChanged;
        domainDescriptor = newDomainDescriptor;
        valueDescriptor = newValueDescriptor;
        updateType();
    }
}

void SignalEvent::updateType()
{
    if (eventType == SignalEventType::Gap)
        return;

    if (domainDescriptorChanged && valueDescriptorChanged)
    {
        eventType = SignalEventType::DomainAndValueChanged;
    }
    else if (domainDescriptorChanged)
    {
        eventType = SignalEventType::DomainChanged;
    }
    else if (valueDescriptorChanged)
    {
        eventType = SignalEventType::ValueChanged;
    }
    else
    {
        eventType = SignalEventType::NoChange;
    }
}

bool SignalEvent::merge(const SignalEvent& other)
{
    // Gap events never merge with anything, including other gaps - each gap is reported individually
    if (this->eventType == SignalEventType::Gap || other.eventType == SignalEventType::Gap)
        return false;

    if (other.domainDescriptorChanged)
    {
        domainDescriptorChanged = true;
        domainDescriptor = other.domainDescriptor;
    }
    if (other.valueDescriptorChanged)
    {
        valueDescriptorChanged = true;
        valueDescriptor = other.valueDescriptor;
    }
    updateType();
    return true;
}

SignalEventType SignalEvent::getType() const
{
    return eventType;
}

const DataDescriptorPtr& SignalEvent::getDomainDescriptor() const
{
    return domainDescriptor;
}

const DataDescriptorPtr& SignalEvent::getValueDescriptor() const
{
    return valueDescriptor;
}

EventPacketPtr SignalEvent::toEventPacket() const
{
    if (eventType == SignalEventType::Gap)
    {
        return ImplicitDomainGapDetectedEventPacket(gapDiff);
    }
    else
    {
        // Unchanged descriptors stay absent (null parameter); a changed descriptor uses the
        // explicit null marker when removed, so consumers can tell "removed" from "unchanged".
        return DataDescriptorChangedEventPacket(
            valueDescriptorChanged ? descriptorToEventPacketParam(valueDescriptor) : nullptr,
            domainDescriptorChanged ? descriptorToEventPacketParam(domainDescriptor) : nullptr);
    }
}

QueueReader::QueueReader(const InputPortConfigPtr& port,  // Consider using Connection instead
                         SampleType valueReadType,
                         SampleType domainReadType,
                         ReadMode mode,
                         const LoggerComponentPtr& logger,
                         bool globalIdFromSignal)  // TODO
    // Init order matches the member declaration order in the header (avoids C5038)
    : port(port)
    , connection(port.getConnection())
    , loggerComponent(logger)
    , readMode(mode)
{
    typeCtx.domainIn = SampleType::Undefined;
    typeCtx.domainOut = domainReadType;
    typeCtx.valueIn = SampleType::Undefined;
    // COMMENT: Read mode should be reworked or at least clarified. Seems like there's a weird correlation between read mode and read types.
    //          Also, unscaled is simply ignored. This should be clarified.
    typeCtx.valueOut = mode == ReadMode::RawValue ? SampleType::Undefined : valueReadType;
}

void QueueReader::adoptPackets()
{
    invalidateAvailable();
    // Take ownership of all packets
    PacketPtr packet = connection.dequeue();
    while (packet.assigned())
    {
        // Sticky marker for hasQueuedEventPackets: the fast read path (owner's steady
        // state) must learn about adopted events without scanning the queue per read
        if (packet.getType() == PacketType::Event)
            eventPacketAdopted = true;
        packets.push_back(std::move(packet));
        packet = connection.dequeue();
    }
}

bool QueueReader::hasQueuedEventPackets()
{
    // Conservative: set on adoption, re-verified (and cleared) by a scan only while set -
    // the no-events steady state costs a single bool check per call
    if (!eventPacketAdopted)
        return false;
    eventPacketAdopted = getNumberOfEventPacketsInQueue() != 0;
    return eventPacketAdopted;
}

void QueueReader::drain()
{
    checkConnection();
    drainConnection();
}

DomainInfo QueueReader::getDomainInfo() const
{
    return typeCtx.domainInfo;
}

std::unique_ptr<DomainValue> QueueReader::getFirstSampleDomainValue() const
{
    if (packets.empty() || packets.front().getType() != PacketType::Data)
    {
        return nullptr;
    }

    DataPacketPtr domainPacket = packets.front().asPtr<IDataPacket>(true).getDomainPacket();
    if (!domainPacket.assigned())
    {
        DAQ_THROW_EXCEPTION(InvalidStateException, "Packet must have a domain packet assigned!");
    }

    return TypedReadingUtils::readDomainValue(
        typeCtx.domainIn, typeCtx.domainOut, typeCtx.domainLayout, domainPacket, readingPosition, typeCtx.domainInfo);
}

AdvanceOutcome QueueReader::advanceToDomainValue(const DomainValue* domainValue)
{
    invalidateAvailable();
    // Pending events must be popped before advancing - the owner would otherwise
    // step over a reportable event boundary without handling it.
    if (!events.empty())
        return {AdvanceResult::Error, nullptr};

    SignalEventType signalChange = SignalEventType::NoChange;

    bool found = false;
    SizeT end = 0;
    for (auto& packet : packets)
    {
        if (packet.getType() == PacketType::Data)
        {
            DataPacketPtr domainPacket = packet.asPtr<IDataPacket>(true).getDomainPacket();

            SizeT index = TypedReadingUtils::findDomainValue(
                typeCtx.domainIn, typeCtx.domainOut, typeCtx.domainLayout, domainPacket, domainValue, nullptr);

            if (index != static_cast<SizeT>(-1))
            {
                if (index < readingPosition)
                {
                    return {AdvanceResult::OvershotError, nullptr};
                }
                readingPosition = index;
                found = true;
                break;
            }

            readingPosition = 0;
            ++end;
            continue;
        }
        else if (packet.getType() == PacketType::Event)
        {
            auto eventPacket = packet.asPtr<IEventPacket>(true);
            signalChange = addEncounteredEvent(eventPacket);
            ++end;

            if (signalChange == SignalEventType::DomainChanged || signalChange == SignalEventType::DomainAndValueChanged ||
                signalChange == SignalEventType::Gap)
            {
                break;
            }
            continue;
        }
        else
        {
            // Unexpected packet type encountered.
            // Packet should be removed and sync is not successful.
            signalChange = SignalEventType::DomainChanged;
            ++end;
            break;
        }
    }

    packets.erase(packets.begin(), packets.begin() + end);

    switch (signalChange)
    {
        case SignalEventType::DomainChanged:
        case SignalEventType::DomainAndValueChanged:
        case SignalEventType::Gap:
            return {AdvanceResult::DomainChanged, nullptr};
        default:
            break;
    }

    if (found)
    {
        // Report the first-sample value actually reached so the owner can verify it
        // against the requested target after converting to the common domain.
        return {AdvanceResult::Success, getFirstSampleDomainValue()};
    }
    return {AdvanceResult::NeedMoreData, nullptr};
}

std::optional<std::chrono::system_clock::time_point> QueueReader::getFirstSampleAbsoluteTime() const
{
    const auto firstSample = getFirstSampleDomainValue();
    if (!firstSample)
        return std::nullopt;
    return firstSample->toAbsoluteTime();
}

Int QueueReader::getSampleRate() const
{
    return sampleRate;
}

void QueueReader::consumeLeadingEventPackets()
{
    size_t end = 0;
    for (const auto& packet : packets)
    {
        auto packetType = packet.getType();
        if (packetType == PacketType::Data)
        {
            break;
        }

        EventPacketPtr eventPacket = packet.asPtr<IEventPacket>(true);
        addEncounteredEvent(eventPacket);

        ++end;
    }
    // Removing leading events can expose a new leading data run behind them, changing the
    // available count; when nothing is removed (front is already data) the count is unchanged,
    // so the incrementally maintained cache stays valid (steady-stream fast path).
    if (end > 0)
    {
        packets.erase(packets.begin(), packets.begin() + end);
        invalidateAvailable();
    }
}

void QueueReader::checkConnection() const
{
    if (!connection.assigned())
        DAQ_THROW_EXCEPTION(InvalidOperationException, "Connection must be assigned for this operation.");
}

void QueueReader::dropForInactive()
{
    invalidateAvailable();
    if (connection.assigned())
        drainConnection();

    // Pending gap events are meaningless once the data flow is suspended
    events.erase(std::remove_if(events.begin(),
                                events.end(),
                                [](const SignalEvent& event) { return event.getType() == SignalEventType::Gap; }),
                 events.end());

    // Drop data packets and gap events up to the first descriptor event, which stays -
    // the reader's type state must not silently diverge from the signal's
    while (!packets.empty())
    {
        const auto& front = packets.front();
        if (front.getType() == PacketType::Event)
        {
            const EventPacketPtr eventPacket = front.asPtr<IEventPacket>(true);
            if (eventPacket.getEventId() != event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED)
                break;
        }
        packets.pop_front();
        readingPosition = 0;
    }
    consumeLeadingEventPackets();
}

void QueueReader::dropOutdatedPacketSegments()
{
    while (getNumberOfEventPacketsInQueue() >= 2)
    {
        [[maybe_unused]] auto foundEvent = dropUntilEvent();  // asserted only; NDEBUG drops the use
        assert(foundEvent && "Event should have been found.");
        consumeLeadingEventPackets();
    }
    dropUntilEvent();
    consumeLeadingEventPackets();
}

SizeT QueueReader::getAvailableSamplesNative() const
{
    if (availableNativeValid)
    {
        // Debug cross-check: any queue mutation that forgot to invalidateAvailable() would
        // leave a stale cache here, which the test suite then catches immediately.
        assert(availableNativeCache == recomputeAvailableNative() && "stale available-count cache");
        return availableNativeCache;
    }
    availableNativeCache = recomputeAvailableNative();
    availableNativeValid = true;
    return availableNativeCache;
}

SizeT QueueReader::recomputeAvailableNative() const
{
    SizeT count = 0;
    SizeT packetReadingPosition = readingPosition;
    for (const auto& packet : packets)
    {
        if (packet.getType() != PacketType::Data)
            break;

        DataPacketPtr dataPacket = packet.asPtr<IDataPacket>(true);
        count += dataPacket.getSampleCount() - packetReadingPosition;

        // Only first packet may have non-zero reading position
        packetReadingPosition = 0;
    }
    return count;
}

SizeT QueueReader::getAvailableSamples() const
{
    return getAvailableSamplesNative() * sampleRateDivider;
}

SizeT QueueReader::getAvailableSamplesUntilEvent() const
{
    // The native counter stops at the first non-data packet, so the available count
    // already ends at the next event boundary; this alias makes that contract explicit.
    // Counts are in the common-rate equivalent: the owner-facing unit (spec section 7.1).
    return getAvailableSamplesNative() * sampleRateDivider;
}

bool QueueReader::hasPendingEvents() const
{
    return !events.empty();
}

EventPacketPtr QueueReader::popFrontEvent()
{
    if (events.empty())
        return nullptr;

    auto eventPacket = events.front().toEventPacket();
    events.pop_front();
    return eventPacket;
}

bool QueueReader::isValid() const
{
    // A convenience over the issue flags: connected and free of descriptor issues (#11) -
    // the owner consumes the per-slot issues for its Incompatible diagnostics
    return connection.assigned() && issues.empty();
}

void QueueReader::domainChangeHandled()
{
    domainChanged = false;
}

const DataDescriptorPtr& QueueReader::getValueDescriptor() const
{
    return typeCtx.valueLayout.descriptor;
}

const DataDescriptorPtr& QueueReader::getDomainDescriptor() const
{
    return typeCtx.domainLayout.descriptor;
}

void QueueReader::seedDescriptors(const DataDescriptorPtr& valueDescriptor, const DataDescriptorPtr& domainDescriptor)
{
    if (valueDescriptor.assigned())
        typeCtx.valueLayout.descriptor = valueDescriptor;
    if (domainDescriptor.assigned())
        typeCtx.domainLayout.descriptor = domainDescriptor;
    if (typeCtx.valueLayout.descriptor.assigned() || typeCtx.domainLayout.descriptor.assigned())
        parseCachedDescriptors();
}

SampleType QueueReader::getValueReadType() const
{
    return typeCtx.valueOut;
}

SampleType QueueReader::getDomainReadType() const
{
    return typeCtx.domainOut;
}

void QueueReader::setValueTransformFunction(const FunctionPtr& transform)
{
    typeCtx.valueTransform = transform;
}

void QueueReader::setDomainTransformFunction(const FunctionPtr& transform)
{
    typeCtx.domainTransform = transform;
}

const FunctionPtr& QueueReader::getValueTransformFunction() const
{
    return typeCtx.valueTransform;
}

const FunctionPtr& QueueReader::getDomainTransformFunction() const
{
    return typeCtx.domainTransform;
}

void QueueReader::updateConnection()
{
    connection = port.getConnection();
    drainConnection();
}

bool QueueReader::refreshConnection()
{
    // The port can hold a connection whose notifications have not reached the owner yet -
    // initial event packets are enqueued while the connection is still being constructed,
    // before the connected() notification fires.
    auto current = port.getConnection();
    if (current == connection)
        return false;

    connection = std::move(current);
    drainConnection();
    return true;
}

void QueueReader::setSampleRateDivider(SizeT divider)
{
    if (divider == 0)
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Sample rate divider must not be 0.");
    }
    sampleRateDivider = divider;
}

SizeT QueueReader::getSampleRateDivider() const
{
    return sampleRateDivider;
}

AdvanceResult QueueReader::read(void* valueBuffer, void* domainBuffer, SizeT* count)
{
    if (count == nullptr)
        return AdvanceResult::Error;

    if (*count % sampleRateDivider != 0)
    {
        // Enforce reading in units of common samples
        *count = 0;
        return AdvanceResult::Error;
    }
    SizeT nativeCount = *count / sampleRateDivider;
    AdvanceResult result = readNative(valueBuffer, domainBuffer, &nativeCount);
    *count = nativeCount * sampleRateDivider;
    return result;
}

AdvanceResult QueueReader::readNative(void* valueBuffer, void* domainBuffer, SizeT* count)
{
    // Availability is maintained incrementally, not invalidated: this read consumes exactly the
    // samples it copies (decremented below), and consumeLeadingEventPackets invalidates only if
    // it crosses an event boundary. This keeps the count O(1) on the steady read path instead of
    // an O(buffered-packets) rescan after every read (main gets it O(1) from the connection).
    if (count == nullptr)
        return AdvanceResult::Error;

    if (*count == 0)
        return AdvanceResult::Success;

    // The owner drains at its evaluation points; pending events block data operations
    if (hasPendingEvents())
    {
        *count = 0;
        return AdvanceResult::Error;
    }

    const SizeT requested = *count;
    SizeT remainingToRead = requested;
    void* valuePtr = valueBuffer;
    void* domainPtr = domainBuffer;

    bool returnError = false;
    SizeT end = 0;
    for (auto& packet : packets)
    {
        if (packet.getType() != PacketType::Data)
        {
            // The user of this class should read <= available samples, so event is not encountered
            returnError = true;
            break;
        }

        DataPacketPtr dataPacket = packet.asPtr<IDataPacket>(true);
        SizeT sampleCount = dataPacket.getSampleCount();

        if (readingPosition > sampleCount)
        {
            returnError = true;
            break;
        }

        SizeT remainingInPacket = sampleCount - readingPosition;
        SizeT toRead = std::min(remainingToRead, remainingInPacket);

        if (toRead == 0)
        {
            readingPosition = 0;
            ++end;
            continue;
        }

        if (valuePtr != nullptr)
        {
            void* valueData = nullptr;
            switch (readMode)
            {
                case ReadMode::RawValue:
                case ReadMode::Unscaled:
                    valueData = dataPacket.getRawData();
                    break;
                case ReadMode::Scaled:
                default:
                    valueData = dataPacket.getData();
                    break;
            }

            ErrCode errCode = TypedReadingUtils::readData(typeCtx.valueIn,
                                                          typeCtx.valueOut,
                                                          false,
                                                          typeCtx.valueLayout,
                                                          valueData,
                                                          readingPosition,
                                                          &valuePtr,
                                                          toRead,
                                                          typeCtx.valueTransform);
            if (!OPENDAQ_SUCCEEDED(errCode))
                throwExceptionFromErrorCode(errCode, getErrorInfoMessage(errCode, true));
        }

        if (domainPtr != nullptr)
        {
            auto domainPacket = dataPacket.getDomainPacket();
            if (!domainPacket.assigned())
                DAQ_THROW_EXCEPTION(NotSupportedException, "Domain packet must be assigned.");

            ErrCode errCode = TypedReadingUtils::readData(typeCtx.domainIn,
                                                          typeCtx.domainOut,
                                                          true,
                                                          typeCtx.domainLayout,
                                                          domainPacket.getData(),
                                                          readingPosition,
                                                          &domainPtr,
                                                          toRead,
                                                          typeCtx.domainTransform);

            if (!OPENDAQ_SUCCEEDED(errCode))
                throwExceptionFromErrorCode(errCode, getErrorInfoMessage(errCode, true));
        }

        remainingToRead -= toRead;

        if (remainingToRead == 0)
        {
            readingPosition += toRead;
            if (readingPosition == sampleCount)
            {
                readingPosition = 0;
                ++end;
            }
            break;
        }

        readingPosition = 0;
        ++end;
    }
    packets.erase(packets.begin(), packets.begin() + end);
    *count = requested - remainingToRead;

    // Maintain the native available-count cache: exactly *count native samples were consumed from
    // the leading data run. Guarded on validity so a lazy/invalid cache stays invalid (recomputed
    // on the next query). consumeLeadingEventPackets below re-invalidates if it crosses an event.
    if (availableNativeValid)
        availableNativeCache -= *count;

    if (returnError)
        return AdvanceResult::Error;

    // Parse trailing events
    if (readingPosition == 0)
        consumeLeadingEventPackets();

    return remainingToRead == 0 ? AdvanceResult::Success : AdvanceResult::NeedMoreData;
}

AdvanceResult QueueReader::skip(SizeT* count)
{
    return read(nullptr, nullptr, count);
}

void QueueReader::drainConnection()
{
    if (!connection.assigned())
        return;

    if (!connection.peek().assigned())
        return;

    adoptPackets();
    consumeLeadingEventPackets();
}

bool QueueReader::discardLeftoverSegment(SizeT samplesInBlock)
{
    if (samplesInBlock % sampleRateDivider != 0)
    {
        DAQ_THROW_EXCEPTION(InvalidStateException, "Aligned block size must be divisible by all signal dividers.");
    }

    if (hasPendingEvents())
    {
        DAQ_THROW_EXCEPTION(InvalidStateException, "Events must be handled before discarding leftover segments.");
    }

    // No events in the queue, this segment has not been ended - mustn't discard
    if (getNumberOfEventPacketsInQueue() == 0)
        return false;

    const SizeT requiredNativeSamples = samplesInBlock / sampleRateDivider;
    const size_t availableNativeSamples = getAvailableSamplesNative();

    if (availableNativeSamples >= requiredNativeSamples)
        return false;

    // Silent discard: the trailing partial block is dropped without a synthetic event or
    // dropped-sample count; the original event packets ending the segment become pending.
    dropUntilEvent();
    consumeLeadingEventPackets(); // Transition to new segment
    return true;
}

SignalEventType QueueReader::addEncounteredEvent(const EventPacketPtr& packet)
{
    auto event = SignalEvent(packet);
    auto eventType = event.getType();

    switch (eventType)
    {
        case SignalEventType::DomainChanged:
            typeCtx.domainLayout.descriptor = event.getDomainDescriptor();
            break;
        case SignalEventType::ValueChanged:
            typeCtx.valueLayout.descriptor = event.getValueDescriptor();
            break;
        case SignalEventType::DomainAndValueChanged:
            typeCtx.domainLayout.descriptor = event.getDomainDescriptor();
            typeCtx.valueLayout.descriptor = event.getValueDescriptor();
            break;
        default:
            break;
    }
    parseCachedDescriptors();
    addToEventQueue(std::move(event));
    return eventType;
}

void QueueReader::addToEventQueue(SignalEvent&& event)
{
    bool eventMerged = false;
    if (!events.empty())
    {
        // Attempt merging with the last event and add to list if merge not possible
        eventMerged = events.back().merge(event);
    }
    if (!eventMerged)
        events.push_back(event);
}

void QueueReader::parseDomainDescriptor()
{
    auto& descriptor = typeCtx.domainLayout.descriptor;
    if (!descriptor.assigned())
        return;

    // Type conversion
    const auto postScaling = descriptor.getPostScaling();
    if (!postScaling.assigned() || readMode == ReadMode::Scaled)
    {
        typeCtx.domainIn = descriptor.getSampleType();
    }
    else
    {
        typeCtx.domainIn = postScaling.getInputSampleType();
    }

    // One layout builder for every reader (#17); the scalar check stays here because it is
    // a domain-specific constraint, not a layout property
    typeCtx.domainLayout = TypedReadingUtils::createReadLayout(descriptor);
    {
        const auto dimensions = descriptor.getDimensions();
        issues.set(QueueReaderIssue::DomainNotScalar, dimensions.assigned() && dimensions.getCount() != 0);
    }

    typeCtx.domainInfo = DomainInfo::fromDescriptor(descriptor);

    bool domainTypesConvertible = TypedReadingUtils::isSampleTypeConvertible(typeCtx.domainIn, typeCtx.domainOut, true);
    issues.set(QueueReaderIssue::DomainTypesNotConvertible, !domainTypesConvertible);
    // END Type Conversion

    // Resolution and origin
    auto newResolution = descriptor.getTickResolution();
    if (typeCtx.domainInfo.resolution != newResolution)
    {
        typeCtx.domainInfo.resolution = newResolution;
        domainChanged = true;
    }

    std::string origin = descriptor.getOrigin();
    // A blank origin is a legitimate relative domain: samples count from the epoch zero point.
    // Only a non-blank origin that fails to parse is an issue.
    const bool originBlank = origin.find_first_not_of(" \t") == std::string::npos;
    auto newOrigin = originBlank ? std::optional<std::chrono::system_clock::time_point>(std::chrono::system_clock::time_point{})
                                 : reader::tryParseEpoch(origin);
    if (newOrigin.has_value() && typeCtx.domainInfo.epoch != newOrigin.value())
    {
        typeCtx.domainInfo.epoch = newOrigin.value();
        domainChanged = true;
    }
    issues.set(QueueReaderIssue::OriginParsingFailed, !newOrigin.has_value());
    // END Resolution and origin

    // Sample rate and delta
    {
        std::int64_t newSampleRate = 0;

        NumberPtr delta = 1;
        auto rule = descriptor.getRule();
        const bool ruleIsLinear = rule.assigned() && rule.getType() == DataRuleType::Linear;

        if (ruleIsLinear)
        {
            delta = rule.getParameters()["delta"];
        }

        const bool resolutionValid =
            typeCtx.domainInfo.resolution.assigned() &&
            typeCtx.domainInfo.resolution.getNumerator() > 0 &&
            typeCtx.domainInfo.resolution.getDenominator() > 0;
        const bool deltaPositive = delta.getFloatValue() > 0.0;

        double sr = 0.0;
        if (resolutionValid && deltaPositive)
        {
            sr = static_cast<double>(typeCtx.domainInfo.resolution.getDenominator()) /
                 (static_cast<double>(typeCtx.domainInfo.resolution.getNumerator()) * delta.getFloatValue());
        }

        const bool deltaIsInteger = (delta.getFloatValue() == static_cast<double>(delta.getIntValue()));
        // A valid rate is a positive integer within the representable range - anything else
        // (fractional, zero, negative or overflowing) marks the domain rule unsupported.
        const bool sampleRateRepresentable =
            sr >= 1.0 && sr <= static_cast<double>(std::numeric_limits<std::int64_t>::max());
        const bool sampleRateIsInteger =
            sampleRateRepresentable && (sr == static_cast<double>(static_cast<std::int64_t>(sr)));

        newSampleRate = sampleRateIsInteger ? static_cast<std::int64_t>(sr) : -1;

        if (sampleRate != newSampleRate)
        {
            sampleRate = newSampleRate;
            domainChanged = true;
        }

        if (packetDelta != delta.getIntValue())
        {
            packetDelta = delta.getIntValue();
            domainChanged = true;
        }

        issues.set(QueueReaderIssue::UnsupportedDomainRule,
                   !ruleIsLinear || !deltaIsInteger || !resolutionValid || !deltaPositive || !sampleRateIsInteger);
    }
    // END Sample rate and delta

    // Unit and quantity
    bool domainIsTimeInSeconds = false;
    do
    {
        auto domainUnit = descriptor.getUnit();
        if (!domainUnit.assigned())
            break;
        
        const auto domainQuantity = domainUnit.getQuantity();
        if (!domainQuantity.assigned() || domainQuantity.getLength() == 0)
            break;
        if (domainQuantity != "time")
            break;
        
        const auto domainUnitSymbol = domainUnit.getSymbol();
        if (domainUnitSymbol != "s")
            break;

        domainIsTimeInSeconds = true;
    } while (false);
    issues.set(QueueReaderIssue::DomainUnitInvalid, !domainIsTimeInSeconds);
    // END Unit and quantity
}

void QueueReader::parseValueDescriptor()
{
    auto& descriptor = typeCtx.valueLayout.descriptor;
    if (!descriptor.assigned())
        return;

    auto postScaling = descriptor.getPostScaling();
    if (!postScaling.assigned() || readMode == ReadMode::Scaled)
    {
        typeCtx.valueIn = descriptor.getSampleType();
    }
    else
    {
        typeCtx.valueIn = postScaling.getInputSampleType();
    }

    // Values of any rank are readable - one sample is a fixed-size block of
    // product-of-dimensions values (the one layout builder computes that, #17)
    typeCtx.valueLayout = TypedReadingUtils::createReadLayout(descriptor);

    if (typeCtx.valueOut == SampleType::Undefined)  // Dynamically determine output type
    {
        typeCtx.valueOut = typeCtx.valueIn;
    }

    bool valueTypesConvertible = TypedReadingUtils::isSampleTypeConvertible(typeCtx.valueIn, typeCtx.valueOut, false);
    issues.set(QueueReaderIssue::ValueTypesNotConvertible, !valueTypesConvertible);
}

void QueueReader::parseCachedDescriptors()
{
    parseDomainDescriptor();
    parseValueDescriptor();
}

size_t QueueReader::getNumberOfEventPacketsInQueue()
{
    size_t numberOfEventPackets = 0;
    for (const auto& packet : packets)
    {
        if (packet.getType() == PacketType::Event)
            ++numberOfEventPackets;
    }
    return numberOfEventPackets;
}

bool QueueReader::dropUntilEvent()
{
    invalidateAvailable();
    // Queue: d1 d2 E d3 -> E d3
    bool foundEvent = false;
    size_t end = 0;
    for (const auto& packet : packets)
    {
        if (packet.getType() == PacketType::Event)
        {
            foundEvent = true;
            break;
        }
        ++end;
    }
    if (foundEvent)
    {
        packets.erase(packets.begin(), packets.begin() + end);
        readingPosition = 0;
    }
    return foundEvent;
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
