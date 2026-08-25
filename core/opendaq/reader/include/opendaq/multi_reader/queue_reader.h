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
#include <opendaq/connection_internal.h>
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

/// Result of an advance operation; on Success, reachedValue holds the domain value of the
/// first sample actually reached (in the signal's own domain).
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
    DomainNotScalar                 = 1 << 5,  // domain descriptor has dimensions - a vector timestamp has no meaning
    ValueDescriptorNull             = 1 << 6,  // descriptor explicitly unset (the NullDataDescriptor marker, sample type Null)
    DomainDescriptorNull            = 1 << 7
};

class QueueReader
{
public:
    explicit QueueReader(const InputPortConfigPtr& port,
                 SampleType valueReadType,
                 SampleType domainReadType,
                 ReadMode mode,
                 const LoggerComponentPtr& logger,
                 bool globalIdFromSignal);

public:
    /// Adopt everything currently queued on the connection into the local packet deque,
    /// applying leading events; every other accessor is a pure query over adopted state.
    void drain();

    DomainInfo getDomainInfo() const;
    std::unique_ptr<DomainValue> getFirstSampleDomainValue() const;

    /// System-clock time of the first unread sample; empty when no data packet is at the front.
    std::optional<std::chrono::system_clock::time_point> getFirstSampleAbsoluteTime() const;

    /// Advance the cursor to the first sample at or after domainValue (signal-domain target);
    /// pending events block advancing (returns Error).
    AdvanceOutcome advanceToDomainValue(const DomainValue* domainValue);
    Int getSampleRate() const;

    void dropOutdatedPacketSegments();

    /// Deactivation drop: discard queued data packets and gap events, keeping descriptor-change
    /// events pending so type state stays consistent while inactive.
    void dropForInactive();

    /// Available samples from the cursor up to the next event packet or the queue end,
    /// in common rate equivalent.
    SizeT getAvailableSamples() const;

    bool hasPendingEvents() const;

    /// True while any event packet sits in the adopted queue, including behind data
    /// (hasPendingEvents covers only leading events). Conservative and cheap.
    bool hasQueuedEventPackets();

    EventPacketPtr popFrontEvent();
    
    bool isValid() const;

    /// Active (cached) descriptors - null until the first descriptor event has been consumed.
    const DataDescriptorPtr& getValueDescriptor() const;
    const DataDescriptorPtr& getDomainDescriptor() const;
    
    /// Effective read types; a dynamically resolved Undefined value type reflects the signal's type.
    SampleType getValueReadType() const;
    SampleType getDomainReadType() const;

    void setValueTransformFunction(const FunctionPtr& transform);
    void setDomainTransformFunction(const FunctionPtr& transform);
    const FunctionPtr& getValueTransformFunction() const;
    const FunctionPtr& getDomainTransformFunction() const;

    void domainChangeHandled();

    /// Re-query the port's connection and adopt whatever it already holds; a change of
    /// connection identity discards everything adopted from the previous one.
    void updateConnection();

    void setSampleRateDivider(SizeT divider);
    SizeT getSampleRateDivider() const;

    /// Read `count` common-rate-equivalent samples (count / sampleRateDivider native samples)
    /// from the packets into the buffers.
    AdvanceResult read(void* valueBuffer, void* domainBuffer, SizeT* count);
    AdvanceResult skip(SizeT* count);

    /// Silently discard the current data segment if fewer than samplesInBlock samples (common
    /// rate equivalent) are available before the next event. @return true if discarded.
    bool discardLeftoverSegment(SizeT samplesInBlock);
    
private:
    void drainConnection();
    /// Forget the previous connection's signal entirely: adopted packets, pending events, cached
    /// descriptors and the state derived from them. Only updateConnection calls this.
    void dropForConnectionChange();
    void adoptPackets();
    /// Re-query the IConnectionInternal view after `connection` changes (see connectionInternal).
    void refreshConnectionInternal();
    void consumeLeadingEventPackets();
    
    SizeT getAvailableSamplesNative() const;
    AdvanceResult readNative(void* valueBuffer, void* domainBuffer, SizeT* count);

    
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

    /// Reused batch buffer for adoptPackets (one connection lock per batch, not per packet).
    std::vector<IPacket*> adoptBuffer;

    SizeT readingPosition = 0;
    /// Sticky adoption-time marker backing hasQueuedEventPackets (conservative: may be true
    /// after the event left the queue; never false while one is in it)
    bool eventPacketAdopted = false;

    /// O(1) lazy cache of getAvailableSamplesNative, invalidated on any packets/cursor mutation.
    mutable bool availableNativeValid = false;
    mutable SizeT availableNativeCache = 0;
    void invalidateAvailable() { availableNativeValid = false; }
    SizeT recomputeAvailableNative() const;

    InputPortConfigPtr port;
    ConnectionPtr connection;
    /// Cached IConnectionInternal view of `connection` for batch dequeue; null if unsupported.
    ObjectPtr<IConnectionInternal> connectionInternal;

    LoggerComponentPtr loggerComponent;

    EnumFlags<QueueReaderIssue> issues;

    ReadMode readMode;

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

        // Copy/convert specializations resolved once per descriptor; null until a compatible
        // descriptor is parsed.
        TypedReadingUtils::ReadDataFn valueReadFn = nullptr;
        TypedReadingUtils::ReadDataFn domainReadFn = nullptr;
    };
    TypedReadingContext typeCtx;

    Int sampleRate = -1;
    Int packetDelta{0};
    bool domainChanged = false;

    SizeT sampleRateDivider = 1;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
