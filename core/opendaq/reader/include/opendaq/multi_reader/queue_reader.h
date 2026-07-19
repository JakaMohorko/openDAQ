/*
 * Copyright 2022-2026 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <opendaq/data_descriptor_ptr.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/enum_flags.h>
#include <opendaq/input_port_config_ptr.h>
#include <opendaq/logger_component_ptr.h>
#include <opendaq/sample_type.h>
#include <opendaq/sample_reader.h>
#include <opendaq/typed_reading_utils.h>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

enum class SignalEventType
{
    NoChange = 0,
    ValueChanged,
    DomainChanged,
    DomainAndValueChanged,
    Gap
};

class SignalEvent
{
public:
    SignalEvent(const EventPacketPtr& packet);

    bool merge(const SignalEvent& otherEvent);
    SignalEventType getType() const;
    const DataDescriptorPtr& getDomainDescriptor() const;
    const DataDescriptorPtr& getValueDescriptor() const;

    EventPacketPtr toEventPacket() const;

private:
    void updateType();
private:
    SignalEventType eventType;
    DataDescriptorPtr domainDescriptor;
    DataDescriptorPtr valueDescriptor;
    // A change may carry a null descriptor (descriptor removed); explicit flags keep
    // "changed to null" distinguishable from "unchanged".
    bool domainDescriptorChanged = false;
    bool valueDescriptorChanged = false;
    Int gapDiff;
};


enum class AdvanceResult
{
    Success = 0,
    NeedMoreData,
    DomainChanged,
    OvershotError,
    Error
};

/**
 * @brief Result of an advance operation. On Success, reachedValue holds the domain value of the
 * first sample actually reached (in the signal's own domain) - the owner verifies it against the
 * requested target in the common domain instead of trusting target rounding.
 */
struct AdvanceOutcome
{
    AdvanceResult result;
    std::unique_ptr<DomainValue> reachedValue;
};

enum class QueueReaderIssue : uint32_t
{
    None                            = 0,
    ValueTypesNotConvertible        = 1 << 0,
    DomainTypesNotConvertible       = 1 << 1,
    UnsupportedDomainRule           = 1 << 2,
    OriginParsingFailed             = 1 << 3,
    DomainUnitInvalid               = 1 << 4,
    DomainNotScalar           = 1 << 5  // domain descriptor has dimensions - a vector timestamp has no meaning
};

class QueueReader
{
public:
    explicit QueueReader(const InputPortConfigPtr& port, // Consider using Connection instead
                 SampleType valueReadType,
                 SampleType domainReadType,
                 ReadMode mode,
                 const LoggerComponentPtr& logger,
                 bool globalIdFromSignal);

public:
    /**
     * @brief Adopt everything currently queued on the connection into the local packet
     * deque, applying leading events. The owner calls this at its evaluation points; every
     * other accessor is a pure query over the already-adopted state (#10).
     */
    void drain();

    DomainInfo getDomainInfo() const;
    std::unique_ptr<DomainValue> getFirstSampleDomainValue() const;

    /**
     * @brief System-clock time of the first unread sample, for synchronization-distance diagnostics.
     * Empty when no data packet is at the front of the queue.
     */
    std::optional<std::chrono::system_clock::time_point> getFirstSampleAbsoluteTime() const;

    /**
     * @brief Advance the cursor to the first sample at or after domainValue (signal-domain target).
     * Pending events block advancing (returns Error); the owner must pop them first.
     */
    AdvanceOutcome advanceToDomainValue(const DomainValue* domainValue);
    Int getSampleRate() const;

    void dropOutdatedPacketSegments();

    /**
     * @brief Deactivation drop: discard queued data packets and gap events (raw and pending),
     * keeping descriptor-change events pending so type state stays consistent while inactive.
     */
    void dropForInactive();
    
    /**
     * @brief Get the Available Samples in common rate equivalent
     * 
     * @return SizeT Available samples multiplied by the sample rate divider.
     */
    SizeT getAvailableSamples() const;

    /**
     * @brief Available samples (common rate equivalent) from the cursor up to the next event packet
     * or queue end. Makes the until-event contract of the availability count explicit.
     */
    SizeT getAvailableSamplesUntilEvent() const;

    bool hasPendingEvents() const;
    EventPacketPtr popFrontEvent();
    
    bool isValid() const;

    /// Active (cached) descriptors - null until the first descriptor event has been consumed.
    const DataDescriptorPtr& getValueDescriptor() const;
    const DataDescriptorPtr& getDomainDescriptor() const;
    
    // COMMENT: According to the spec, these things will probably be removed at the end? 
    /**
     * @brief Adopt already-active descriptors from a previous reader over the same connection
     * (reader-from-existing migration). The originals were consumed from the shared connection
     * by the previous owner, so no pending event is created here.
     */
    void seedDescriptors(const DataDescriptorPtr& valueDescriptor, const DataDescriptorPtr& domainDescriptor);

    /// Effective read types; a dynamically resolved Undefined value type reflects the signal's type.
    SampleType getValueReadType() const;
    SampleType getDomainReadType() const;

    // COMMENT: Are these transform functions actually still needed?
    void setValueTransformFunction(const FunctionPtr& transform);
    void setDomainTransformFunction(const FunctionPtr& transform);
    const FunctionPtr& getValueTransformFunction() const;
    const FunctionPtr& getDomainTransformFunction() const;

    void domainChangeHandled();
    void updateConnection();

    /**
     * @brief Rebind to the port's current connection only if it differs from the cached one.
     * @return true if the connection changed and the queue was re-drained.
     */
    bool refreshConnection();
    
    void setSampleRateDivider(SizeT divider);
    SizeT getSampleRateDivider() const;
    
    // COMMENT: Do we need the domain buffer? We're anyhow just reading sync values. The domain buffer can be calculated  for the main rate at the end.
    //          It should probably just be in the main domain units/epoch/resolution...
    // COMMENT: It feels inconsistent that we're providing divided rate sized buffers, but putting in a non-divided count. Feels like the divider calculation
    //          is done in multiple locations.
    
    /**
     * @brief Read common rate equivalent samples into the buffer. There will be nativeSamples = count / sampleRateDivider
     * samples read from the packets into the buffer.
     * 
     * @param buffer Buffer that has capacity of at least count / sampleRateDivider
     * @param count Desired sample count in common rate equivalent. 
     * @return AdvanceResult 
     */
    AdvanceResult read(void* valueBuffer, void* domainBuffer, SizeT* count);
    AdvanceResult skip(SizeT* count);

    /**
     * @brief Silently discard the current data segment, if there are fewer than samplesInBlock samples available (common rate equivalent).
     *
     * For example: If there are 3 samples available before next event, samplesInBlock=10 (this is dividerLCM in terms of multireading)
     * and divider for the queue reader is 2, then 5 native samples are required as minimum aligned read. Since 3 < 5, the 3 samples
     * are discarded and the original event packets ending the segment become pending. No synthetic event is created and no dropped
     * count is reported; the discontinuity is observable from the next read's domain output.
     *
     * @param samplesInBlock Number of samples (common rate equivalent).
     * @return true If samples were discarded.
     * @return false If samples were not discarded - data segment is long enough or there is no event in the queue to end the segment.
     */
    bool discardLeftoverSegment(SizeT samplesInBlock);
    
private:
    void drainConnection();
    void adoptPackets();
    void consumeLeadingEventPackets();
    
    SizeT getAvailableSamplesNative() const;
    AdvanceResult readNative(void* valueBuffer, void* domainBuffer, SizeT* count);

    void checkConnection() const;
    
    SignalEventType addEncounteredEvent(const EventPacketPtr& packet);
    void addToEventQueue(SignalEvent&& event);
    void parseDomainDescriptor();
    void parseValueDescriptor();
    void parseCachedDescriptors();
    size_t getNumberOfEventPacketsInQueue();
    bool dropUntilEvent();

private:
    std::deque<PacketPtr> packets;
    std::deque<SignalEvent> events;

    SizeT readingPosition = 0;

    InputPortConfigPtr port;
    ConnectionPtr connection;

    LoggerComponentPtr loggerComponent;

    EnumFlags<QueueReaderIssue> issues;

    ReadMode readMode;

    // COMMENT: It probably does not make sense to have output domain values for every signal. They should be time aligned anyhow.
    //          Also, the domain type and transform don't really make sense. Some of these might make sense in the future for 
    //          Explicit rate (async) signals, but not for sync ones where all must have a common domain. These changes result in API
    //          usage changes, but I'd prioritize usability over full API behaviour retention. To preserve old behaviour, we should 
    //          potentially apply the domain transforms at the very end, but not in the queue reader. Ideally, we would not even need 
    //          them.
    struct TypedReadingContext
    {
        SampleType domainIn;
        SampleType domainOut;
        ReadLayout domainLayout;
        DomainInfo domainInfo;
        FunctionPtr domainTransform = nullptr;
        
        SampleType valueIn;
        SampleType valueOut;
        ReadLayout valueLayout;
        FunctionPtr valueTransform = nullptr;
    };
    TypedReadingContext typeCtx;

    Int sampleRate = -1;
    Int packetDelta{0};
    bool domainChanged = false;

    SizeT sampleRateDivider = 1;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
