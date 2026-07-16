#include <opendaq/queue_reader.h>

#include <opendaq/custom_log.h>
#include <opendaq/event_packet_utils.h>

#include <limits>

BEGIN_NAMESPACE_OPENDAQ

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
        const auto [valueDescChanged, domainDescChanged, newValueDescriptor, newDomainDescriptor] = parseDataDescriptorEventPacket(packet);
        domainDescriptor = newDomainDescriptor;
        valueDescriptor = newValueDescriptor;
        updateType();
    }
}

void SignalEvent::updateType()
{
    if (eventType == SignalEventType::Gap)
        return;

    if (domainDescriptor.assigned() && valueDescriptor.assigned())
    {
        eventType = SignalEventType::DomainAndValueChanged;
    }
    else if (domainDescriptor.assigned())
    {
        eventType = SignalEventType::DomainChanged;
    }
    else if (valueDescriptor.assigned())
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

    if (other.domainDescriptor.assigned())
        domainDescriptor = other.domainDescriptor;
    if (other.valueDescriptor.assigned())
        valueDescriptor = other.valueDescriptor;
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
        return DataDescriptorChangedEventPacket(descriptorToEventPacketParam(valueDescriptor), descriptorToEventPacketParam(domainDescriptor));
    }
}

QueueReader::QueueReader(const InputPortConfigPtr& port,  // Consider using Connection instead
                         SampleType valueReadType,
                         SampleType domainReadType,
                         ReadMode mode,
                         const LoggerComponentPtr& logger,
                         bool globalIdFromSignal)  // TODO
    : port(port)
    , connection(port.getConnection())
    , readMode(mode)
    , loggerComponent(logger)
{
    typeCtx.domainIn = SampleType::Undefined;
    typeCtx.domainOut = domainReadType;
    typeCtx.valueIn = SampleType::Undefined;
    typeCtx.valueOut = mode == ReadMode::RawValue ? SampleType::Undefined : valueReadType;
}

void QueueReader::adoptPackets()
{
    // Take ownership of all packets
    PacketPtr packet = connection.dequeue();
    while (packet.assigned())
    {
        packets.push_back(std::move(packet));
        packet = connection.dequeue();
    }
}

DomainInfo QueueReader::getDomainInfo()
{
    checkConnection();

    drainConnection();
    return typeCtx.domainInfo;
}

std::unique_ptr<DomainValue> QueueReader::getFirstSampleDomainValue()
{
    checkConnection();
    drainConnection();

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
    checkConnection();
    drainConnection();

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

std::optional<std::chrono::system_clock::time_point> QueueReader::getFirstSampleAbsoluteTime()
{
    const auto firstSample = getFirstSampleDomainValue();
    if (!firstSample)
        return std::nullopt;
    return firstSample->toAbsoluteTime();
}

Int QueueReader::getSampleRate()
{
    checkConnection();
    drainConnection();

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
    packets.erase(packets.begin(), packets.begin() + end);
}

void QueueReader::checkConnection() const
{
    if (!connection.assigned())
        DAQ_THROW_EXCEPTION(InvalidOperationException, "Connection must be assigned for this operation.");
}

void QueueReader::dropOutdatedPacketSegments()
{
    checkConnection();
    drainConnection();

    while (getNumberOfEventPacketsInQueue() >= 2)
    {
        auto foundEvent = dropUntilEvent();
        assert(foundEvent && "Event should have been found.");
        consumeLeadingEventPackets();
    }
    dropUntilEvent();
    consumeLeadingEventPackets();
}

SizeT QueueReader::getAvailableSamplesNative()
{
    checkConnection();
    drainConnection();

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

SizeT QueueReader::getAvailableSamples()
{
    return getAvailableSamplesNative() * sampleRateDivider;
}

SizeT QueueReader::getAvailableSamplesUntilEvent()
{
    // The native counter stops at the first non-data packet, so the available count
    // already ends at the next event boundary; this alias makes that contract explicit.
    return getAvailableSamplesNative() * sampleRateDivider;
}

bool QueueReader::hasPendingEvents()
{
    checkConnection();
    drainConnection();
    return !events.empty();
}

EventPacketPtr QueueReader::popFrontEvent()
{
    checkConnection();
    drainConnection();
    if (events.empty())
        return nullptr;

    auto eventPacket = events.front().toEventPacket();
    events.pop_front();
    return eventPacket;
}

bool QueueReader::isValid()
{
    if (!connection.assigned())
        return false;

    drainConnection();
    return issues.empty();
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

void QueueReader::updateConnection()
{
    connection = port.getConnection();
    drainConnection();
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
    if (count == nullptr)
        return AdvanceResult::Error;

    if (*count == 0)
        return AdvanceResult::Success;

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

    typeCtx.domainLayout.rawSampleSize = descriptor.getRawSampleSize();
    {
        SizeT valuesPerSample = 1;
        SizeT dimensionCount = 0;
        auto dimensions = descriptor.getDimensions();
        if (dimensions.assigned())
        {
            dimensionCount = dimensions.getCount();
            for (const auto& dimension : dimensions)
                valuesPerSample *= static_cast<SizeT>(dimension.getSize());
        }
        typeCtx.domainLayout.valuesPerSample = valuesPerSample;
        // Domain samples must be scalar - a vector timestamp has no meaning
        issues.set(QueueReaderIssue::UnsupportedDimensions, dimensionCount != 0);
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
    auto newOrigin = reader::tryParseEpoch(origin);
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

    auto postScaling = descriptor.getPostScaling();
    if (!postScaling.assigned() || readMode == ReadMode::Scaled)
    {
        typeCtx.valueIn = descriptor.getSampleType();
    }
    else
    {
        typeCtx.valueIn = postScaling.getInputSampleType();
    }

    {
        typeCtx.valueLayout.rawSampleSize = descriptor.getRawSampleSize();
        // Values of any rank are readable - one sample is a fixed-size block of product-of-dimensions values
        SizeT valuesPerSample = 1;
        auto dimensions = descriptor.getDimensions();
        if (dimensions.assigned())
        {
            for (const auto& dimension : dimensions)
                valuesPerSample *= static_cast<SizeT>(dimension.getSize());
        }
        typeCtx.valueLayout.valuesPerSample = valuesPerSample;
    }

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

END_NAMESPACE_OPENDAQ