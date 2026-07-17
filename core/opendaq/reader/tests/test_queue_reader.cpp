#include <gtest/gtest.h>

#include <coreobjects/unit_factory.h>
#include <coretypes/exceptions.h>
#include <coretypes/ratio_factory.h>

#include <opendaq/data_descriptor_factory.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/dimension_factory.h>
#include <opendaq/dimension_rule_factory.h>
#include <opendaq/domain_value.h>
#include <opendaq/event_packet_utils.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/queue_reader.h>
#include <opendaq/reader_utils.h>
#include <opendaq/typed_reading_utils.h>
#include "reader_common.h"

#include <chrono>
#include <thread>
#include <iostream>

class QueueReaderTest : public ReaderTest<>
{
public:
    using Super = ReaderTest<>;

protected:
    void SetUp() override
    {
        Super::SetUp();

        domainSignal = Signal(context, nullptr, "timeSig");
        signal.setDomainSignal(domainSignal);
    }

    void setDomainDescriptor(const DataDescriptorPtr& descriptor)
    {
        domainSignal.setDescriptor(descriptor);
    }

    void setValueDescriptor(const DataDescriptorPtr& descriptor)
    {
        signal.setDescriptor(descriptor);
    }

    void setPacketSize(SizeT size)
    {
        packetSize = size;
    }

    void setOffsetDelta(Int newOffset, Int newDelta)
    {
        offset = newOffset;
        delta = newDelta;
        samples = 0;
    }

    Int getOffset() const
    {
        return offset;
    }

    void sendNextPacket()
    {
        const auto domainPacket = DataPacket(domainSignal.getDescriptor(), packetSize, offset);
        const auto valuePacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), packetSize);
        auto* d = static_cast<double*>(valuePacket.getRawData());
        for (size_t i = 0; i < packetSize; ++i)
        {
            d[i] = static_cast<double>(samples + i);
        }

        domainSignal.sendPacket(domainPacket);
        signal.sendPacket(valuePacket);

        samples += packetSize;
        offset += packetSize * delta;
    }

protected:
    Int offset = 0;
    Int delta = 1;
    SizeT packetSize = 1;

    SizeT samples = 0;

    SignalConfigPtr domainSignal;
};

void assertReaderAtDomainValue(QueueReader& reader, Int tick)
{
    auto start = reader.getFirstSampleDomainValue();
    auto* startP = dynamic_cast<DomainValueImpl<Int>*>(start.get());
    ASSERT_EQ(startP->getValue(), tick);
}

TEST_F(QueueReaderTest, AdvancePastEnd)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("2022-09-27T00:02:03+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);

    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    for (int c = 0; c < 3; ++c)
    {
        sendNextPacket();
    }

    ASSERT_TRUE(reader.hasPendingEvents());

    ASSERT_EQ(reader.getAvailableSamples(), 3u * packetSize);

    auto start = reader.getFirstSampleDomainValue();
    auto* startP = dynamic_cast<DomainValueImpl<Int>*>(start.get());

    ASSERT_TRUE(startP != nullptr);

    ASSERT_EQ(startP->getValue(), 500u);

    // Pending events block advancing until they are popped
    std::unique_ptr<DomainValue> domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 512);
    auto outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Error);

    auto event = reader.popFrontEvent();
    ASSERT_EQ(event.getType(), PacketType::Event);
    auto params = event.getParameters();
    const DataDescriptorPtr domainFromEvent = params[event_packet_param::DOMAIN_DATA_DESCRIPTOR];
    ASSERT_EQ(domainFromEvent.getTickResolution().getDenominator(), sampleRate);
    ASSERT_FALSE(reader.hasPendingEvents());

    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success);

    // The outcome reports the first-sample value actually reached
    ASSERT_TRUE(outcome.reachedValue != nullptr);
    ASSERT_EQ(*domainValue, *outcome.reachedValue);

    auto start2 = reader.getFirstSampleDomainValue();
    ASSERT_EQ(*domainValue, *start2);

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 100512);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::NeedMoreData);
    ASSERT_EQ(outcome.reachedValue.get(), nullptr);

    start2 = reader.getFirstSampleDomainValue();
    ASSERT_EQ(start2.get(), nullptr);
}

TEST_F(QueueReaderTest, DomainChangeDetection)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);

    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket();

    ASSERT_TRUE(reader.hasPendingEvents());

    ASSERT_EQ(reader.getAvailableSamples(), packetSize);

    auto start = reader.getFirstSampleDomainValue();
    auto* startP = dynamic_cast<DomainValueImpl<Int>*>(start.get());

    ASSERT_TRUE(startP != nullptr);

    ASSERT_EQ(startP->getValue(), 500u);

    // The descriptor change event when the sig was connected to port; must be popped before advancing
    auto eventPacket = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());

    std::unique_ptr<DomainValue> domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 512);
    auto outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::NeedMoreData);

    auto start2 = reader.getFirstSampleDomainValue();
    ASSERT_TRUE(start2 == nullptr);

    sendNextPacket();
    sendNextPacket();

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 512);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success);
    ASSERT_TRUE(outcome.reachedValue != nullptr);
    ASSERT_EQ(*domainValue, *outcome.reachedValue);

    ASSERT_FALSE(reader.hasPendingEvents());
    ASSERT_EQ(reader.getSampleRate(), sampleRate);

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);

    ASSERT_EQ(reader.getAvailableSamples(), 3u);

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 525);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::DomainChanged);
    ASSERT_EQ(outcome.reachedValue.get(), nullptr);

    // Event from changing the descriptor mid operation
    ASSERT_TRUE(reader.hasPendingEvents());

    eventPacket = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());
    ASSERT_EQ(reader.getSampleRate(), sampleRate / 10);
    ASSERT_TRUE(reader.isValid());
}

TEST_F(QueueReaderTest, OriginParsing)
{
    std::string origin = "1970-01-01T00:01:00+0000";
    auto epoch = reader::tryParseEpoch(origin);
    ASSERT_TRUE(epoch.has_value());

    origin = "1970-01-01T00:01:00+00:00";
    epoch = reader::tryParseEpoch(origin);
    ASSERT_TRUE(epoch.has_value());

    origin = "abc";
    epoch = reader::tryParseEpoch(origin);
    ASSERT_FALSE(epoch.has_value());

    origin = "1970-01-01T00:01:00+00:00abc";
    epoch = reader::tryParseEpoch(origin);
    ASSERT_FALSE(epoch.has_value());
}

TEST_F(QueueReaderTest, CreateBeforeConnection)
{
    auto inputPort = InputPort(context, nullptr, "port", true);

    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);
    std::unique_ptr<DomainValue> domainValue =
        std::make_unique<DomainValueImpl<Int>>(DomainInfo{std::chrono::system_clock::time_point{}, Ratio(1, 1000)}, 512);

    bool valid;
    ASSERT_NO_THROW(valid = reader.isValid());
    ASSERT_FALSE(valid);

    ASSERT_THROW(reader.getDomainInfo(), InvalidOperationException);
    ASSERT_THROW(reader.getFirstSampleDomainValue(), InvalidOperationException);
    ASSERT_THROW(reader.advanceToDomainValue(domainValue.get()), InvalidOperationException);
    ASSERT_THROW(reader.getSampleRate(), InvalidOperationException);
    ASSERT_THROW(reader.dropOutdatedPacketSegments(), InvalidOperationException);
    ASSERT_THROW(reader.hasPendingEvents(), InvalidOperationException);
    ASSERT_THROW(reader.popFrontEvent(), InvalidOperationException);
}

TEST_F(QueueReaderTest, CreateBeforeConnectionRecovery)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);

    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    ASSERT_FALSE(reader.isValid());
    ASSERT_THROW(reader.getDomainInfo(), InvalidOperationException);

    inputPort.connect(signal);
    reader.updateConnection();

    std::unique_ptr<DomainValue> domainValue =
        std::make_unique<DomainValueImpl<Int>>(DomainInfo{std::chrono::system_clock::time_point{}, Ratio(1, 1000)}, 512);

    bool valid;
    ASSERT_NO_THROW(valid = reader.isValid());
    ASSERT_TRUE(valid);

    ASSERT_NO_THROW(reader.getDomainInfo());
    ASSERT_NO_THROW(reader.getFirstSampleDomainValue());
    ASSERT_NO_THROW(reader.advanceToDomainValue(domainValue.get()));
    Int sr;
    ASSERT_NO_THROW(sr = reader.getSampleRate());
    ASSERT_EQ(sr, sampleRate);
    ASSERT_NO_THROW(reader.dropOutdatedPacketSegments());
    ASSERT_NO_THROW(reader.hasPendingEvents());
    ASSERT_NO_THROW(reader.popFrontEvent());
}

TEST_F(QueueReaderTest, InvalidDomainAndBack)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("abc")  // Invalid origin
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.setNotificationMethod(PacketReadyNotification::SameThread);

    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    ASSERT_FALSE(reader.isValid());
    ASSERT_THROW(reader.getDomainInfo(), InvalidOperationException);

    inputPort.connect(signal);
    reader.updateConnection();

    bool valid;
    ASSERT_NO_THROW(valid = reader.isValid());
    ASSERT_FALSE(valid);

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_TRUE(reader.isValid());

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(3.5, 0))  // Non-integer delta
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_FALSE(reader.isValid());

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_TRUE(reader.isValid());

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(3, 0))  // Non-integer sample rate
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_FALSE(reader.isValid());

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_TRUE(reader.isValid());

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(ExplicitDataRule())  // Explicit data rule - shall be removed when resampling is added
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    ASSERT_FALSE(reader.isValid());
}

TEST_F(QueueReaderTest, MergeDomainAndValueChange)
{
    constexpr Int sampleRate = 10000;
    auto domainDescriptor = DataDescriptorBuilder()
                                .setSampleType(SampleType::Int64)
                                .setTickResolution(Ratio(1, sampleRate))
                                .setOrigin("1970-01-01T00:00:00+00:00")
                                .setRule(LinearDataRule(1, 0))
                                .setUnit(Unit("s", -1, "second", "time"))
                                .build();

    auto valueDescriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit("V", -1, "volt", "voltage")).build();

    // Unchanged descriptors are absent (nullptr) - the explicit null marker means "changed to null"
    auto domainChangePacket = DataDescriptorChangedEventPacket(nullptr, descriptorToEventPacketParam(domainDescriptor));
    SignalEvent domainChange(domainChangePacket);

    ASSERT_EQ(domainChange.getType(), SignalEventType::DomainChanged);

    auto valueChangePacket = DataDescriptorChangedEventPacket(descriptorToEventPacketParam(valueDescriptor), nullptr);
    SignalEvent valueChange(valueChangePacket);

    ASSERT_EQ(valueChange.getType(), SignalEventType::ValueChanged);

    auto merged = domainChange.merge(valueChange);
    ASSERT_TRUE(merged);

    ASSERT_EQ(domainChange.getType(), SignalEventType::DomainAndValueChanged);

    ASSERT_EQ(domainChange.getDomainDescriptor().getTickResolution().getDenominator(), sampleRate);
    ASSERT_EQ(domainChange.getValueDescriptor().getUnit().getQuantity(), String("voltage"));
}

TEST_F(QueueReaderTest, LatestDescriptorPreserved)
{
    constexpr Int sampleRate = 10000;
    auto domainDescriptor = DataDescriptorBuilder()
                                .setSampleType(SampleType::Int64)
                                .setTickResolution(Ratio(1, sampleRate))
                                .setOrigin("1970-01-01T00:00:00+00:00")
                                .setRule(LinearDataRule(1, 0))
                                .setUnit(Unit("s", -1, "second", "time"))
                                .build();
    auto domainDescriptor2 = DataDescriptorBuilder()
                                 .setSampleType(SampleType::Int64)
                                 .setTickResolution(Ratio(1, sampleRate))
                                 .setOrigin("1970-01-01T00:00:00+00:00")
                                 .setRule(LinearDataRule(10, 0))
                                 .setUnit(Unit("s", -1, "second", "time"))
                                 .build();

    auto domainChangePacket = DataDescriptorChangedEventPacket(nullptr, descriptorToEventPacketParam(domainDescriptor));
    SignalEvent domainChange(domainChangePacket);
    ASSERT_EQ(domainChange.getType(), SignalEventType::DomainChanged);
    NumberPtr delta = domainChange.getDomainDescriptor().getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 1u);

    auto domainChangePacket2 = DataDescriptorChangedEventPacket(nullptr, descriptorToEventPacketParam(domainDescriptor2));
    SignalEvent domainChange2(domainChangePacket2);
    ASSERT_EQ(domainChange2.getType(), SignalEventType::DomainChanged);
    delta = domainChange2.getDomainDescriptor().getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 10u);

    bool merged = domainChange.merge(domainChange2);
    ASSERT_TRUE(merged);

    ASSERT_EQ(domainChange.getType(), SignalEventType::DomainChanged);
    delta = domainChange.getDomainDescriptor().getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 10u);
}

TEST_F(QueueReaderTest, GapEventsRefuseMerge)
{
    constexpr Int sampleRate = 10000;
    auto domainDescriptor = DataDescriptorBuilder()
                                .setSampleType(SampleType::Int64)
                                .setTickResolution(Ratio(1, sampleRate))
                                .setOrigin("1970-01-01T00:00:00+00:00")
                                .setRule(LinearDataRule(1, 0))
                                .setUnit(Unit("s", -1, "second", "time"))
                                .build();

    auto domainChangePacket = DataDescriptorChangedEventPacket(nullptr, descriptorToEventPacketParam(domainDescriptor));
    SignalEvent domainChange(domainChangePacket);
    ASSERT_EQ(domainChange.getType(), SignalEventType::DomainChanged);

    auto gapPacket = ImplicitDomainGapDetectedEventPacket(0);
    SignalEvent gapChange(gapPacket);
    ASSERT_EQ(gapChange.getType(), SignalEventType::Gap);

    auto merged = domainChange.merge(gapChange);
    ASSERT_FALSE(merged);
}

TEST_F(QueueReaderTest, CheckAdvanceDomainEdgeCases)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket();
    sendNextPacket(); // [510 - 514]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);
    sendNextPacket(); // [515 - 555]
    sendNextPacket(); // [565 - 605]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 2);

    sendNextPacket(); // [615 - 623]
    sendNextPacket(); // [625 - 633]
    // Establish a queue, now test queue handling
    
    // Initial data segment
    ASSERT_TRUE(reader.hasPendingEvents());
    ASSERT_EQ(reader.getAvailableSamples(), 3 * packetSize); // First three packets worth of samples
    
    auto event = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());

    DataDescriptorPtr descriptor = event.getParameters()[event_packet_param::DOMAIN_DATA_DESCRIPTOR];
    NumberPtr delta = descriptor.getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 1u);

    auto start = reader.getFirstSampleDomainValue();
    auto* startP = dynamic_cast<DomainValueImpl<Int>*>(start.get());
    ASSERT_EQ(startP->getValue(), 500u);
    // End Initial data segment

    // Second data segment
    std::unique_ptr<DomainValue> domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 515);
    auto outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::DomainChanged);
    assertReaderAtDomainValue(reader, 515); // First available sample is the first sample of the "unopened" data packet

    ASSERT_TRUE(reader.hasPendingEvents());

    event = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());

    descriptor = event.getParameters()[event_packet_param::DOMAIN_DATA_DESCRIPTOR];
    delta = descriptor.getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 10u); // Check that we got the second descriptor

    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success); // Advancing for the second time will result in a success
    assertReaderAtDomainValue(reader, 515);

    ASSERT_EQ(reader.getSampleRate(), sampleRate/10);

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 605);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success); // Can advance to the last sample in the data segment
    ASSERT_TRUE(outcome.reachedValue != nullptr);
    ASSERT_EQ(*domainValue, *outcome.reachedValue);
    assertReaderAtDomainValue(reader, 605);
    // End Second data segment

    // Third data segment
    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 615);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::DomainChanged); // The first sample in the next sample only through domain change
    assertReaderAtDomainValue(reader, 615);

    ASSERT_TRUE(reader.hasPendingEvents());

    event = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());

    descriptor = event.getParameters()[event_packet_param::DOMAIN_DATA_DESCRIPTOR];
    delta = descriptor.getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 2u); // Check that we got the third descriptor

    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success); // Advancing for the second time will result in a success
    assertReaderAtDomainValue(reader, 615);

    ASSERT_EQ(reader.getSampleRate(), sampleRate/2);

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 633);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success); // Can advance to the last sample in the data segment
    assertReaderAtDomainValue(reader, 633);

    domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 650);
    outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::NeedMoreData);

    // Empty queue
    ASSERT_EQ(reader.getAvailableSamples(), 0u);
    auto first = reader.getFirstSampleDomainValue();
    ASSERT_EQ(first.get(), nullptr);
    ASSERT_FALSE(reader.hasPendingEvents());
    event = reader.popFrontEvent();
    ASSERT_EQ(event.getObject(), nullptr);
}

TEST_F(QueueReaderTest, DropOutdatedPacketSegments)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket();
    sendNextPacket(); // [510 - 514]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);
    sendNextPacket(); // [515 - 555]
    sendNextPacket(); // [565 - 605]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 2);

    sendNextPacket(); // [615 - 623]
    sendNextPacket(); // [625 - 633]
    // Establish a queue, now test queue handling
    
    reader.dropOutdatedPacketSegments();

    ASSERT_TRUE(reader.hasPendingEvents());

    auto event = reader.popFrontEvent();
    ASSERT_FALSE(reader.hasPendingEvents());

    DataDescriptorPtr descriptor = event.getParameters()[event_packet_param::DOMAIN_DATA_DESCRIPTOR];
    NumberPtr delta = descriptor.getRule().getParameters()["delta"];
    ASSERT_EQ(delta.getIntValue(), 2u);

    assertReaderAtDomainValue(reader, 615);
}

TEST_F(QueueReaderTest, DiscardLeftoverSegment)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket();
    sendNextPacket(); // [510 - 514]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);
    sendNextPacket(); // [515 - 555]
    sendNextPacket(); // [565 - 605]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 2);

    sendNextPacket(); // [615 - 623]
    sendNextPacket(); // [625 - 633]
    // Establish a queue, now test queue handling
    ASSERT_TRUE(reader.hasPendingEvents());
    auto event = reader.popFrontEvent();
    ASSERT_TRUE(event.assigned());
    ASSERT_FALSE(reader.hasPendingEvents());

    ASSERT_EQ(reader.getAvailableSamples(), 15);
    ASSERT_THROW(reader.setSampleRateDivider(0), InvalidParameterException);

    ASSERT_NO_THROW(reader.setSampleRateDivider(10));
    ASSERT_EQ(reader.getAvailableSamples(), 150u);
    ASSERT_THROW(reader.discardLeftoverSegment(13), InvalidStateException);

    ASSERT_FALSE(reader.discardLeftoverSegment(10));
    ASSERT_FALSE(reader.discardLeftoverSegment(150));
    ASSERT_TRUE(reader.discardLeftoverSegment(200));

    // Silent discard: no synthetic gap event, only the original descriptor-change event is pending
    ASSERT_TRUE(reader.hasPendingEvents());
    ASSERT_THROW(reader.discardLeftoverSegment(10), InvalidStateException); // Cannot discard with pending events
    event = reader.popFrontEvent();
    ASSERT_TRUE(event.assigned());
    ASSERT_EQ(event.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
    ASSERT_FALSE(reader.hasPendingEvents());

    ASSERT_EQ(reader.getAvailableSamples(), 100u);
    ASSERT_NO_THROW(reader.setSampleRateDivider(100));
    ASSERT_EQ(reader.getAvailableSamples(), 1000u);

    ASSERT_FALSE(reader.discardLeftoverSegment(100));
    ASSERT_FALSE(reader.discardLeftoverSegment(1000));
    ASSERT_THROW(reader.discardLeftoverSegment(1001), InvalidStateException);
    ASSERT_TRUE(reader.discardLeftoverSegment(1100));
    ASSERT_THROW(reader.discardLeftoverSegment(100), InvalidStateException); // pending events

    ASSERT_TRUE(reader.hasPendingEvents());
    event = reader.popFrontEvent();
    ASSERT_TRUE(event.assigned());
    ASSERT_EQ(event.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
    ASSERT_FALSE(reader.hasPendingEvents());

    reader.setSampleRateDivider(2);
    ASSERT_EQ(reader.getAvailableSamples(), 20u);
    ASSERT_FALSE(reader.discardLeftoverSegment(1000)); // Don't discard if unfinished segment
}

TEST_F(QueueReaderTest, LeftoverSegmentPartialPackets)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 10000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 200;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 699]
    sendNextPacket(); // [700 - 899]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());
    
    setOffsetDelta(getOffset(), 2);
    setPacketSize(packetSize / 2);

    sendNextPacket();

    ASSERT_TRUE(reader.hasPendingEvents());
    reader.popFrontEvent();

    std::unique_ptr<DomainValue> domainValue = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 870);
    auto outcome = reader.advanceToDomainValue(domainValue.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success);

    ASSERT_FALSE(reader.discardLeftoverSegment(10));
    ASSERT_FALSE(reader.discardLeftoverSegment(30));
    ASSERT_TRUE(reader.discardLeftoverSegment(31));
}

TEST_F(QueueReaderTest, GapEventsDoNotMerge)
{
    // Gap events must never merge with anything, including other gaps - each keeps its own diff.
    // Gap packets cannot be enqueued into a connection from outside, so the merge rule is tested
    // directly on SignalEvent.
    auto gap5 = SignalEvent(ImplicitDomainGapDetectedEventPacket(5));
    auto gap7 = SignalEvent(ImplicitDomainGapDetectedEventPacket(7));

    ASSERT_EQ(gap5.getType(), SignalEventType::Gap);
    ASSERT_FALSE(gap5.merge(gap7));

    // Both diffs stay intact after the rejected merge
    NumberPtr gapDiff = gap5.toEventPacket().getParameters().get(event_packet_param::GAP_DIFF);
    ASSERT_EQ(gapDiff.getIntValue(), 5);
    gapDiff = gap7.toEventPacket().getParameters().get(event_packet_param::GAP_DIFF);
    ASSERT_EQ(gapDiff.getIntValue(), 7);

    // Gaps don't merge with descriptor changes in either direction
    auto descChange = SignalEvent(DataDescriptorChangedEventPacket(setupDescriptor(SampleType::Float64), nullptr));
    ASSERT_FALSE(descChange.merge(gap5));
    ASSERT_FALSE(gap5.merge(descChange));

    // Consecutive descriptor changes still merge, newest descriptor wins
    auto descChange2 = SignalEvent(DataDescriptorChangedEventPacket(setupDescriptor(SampleType::Int32), nullptr));
    ASSERT_TRUE(descChange.merge(descChange2));
    ASSERT_EQ(descChange.getValueDescriptor().getSampleType(), SampleType::Int32);
}

TEST_F(QueueReaderTest, AdvanceReachedValueBetweenTicks)
{
    // A target between the signal's ticks is reached at the next existing sample;
    // the outcome reports that later value instead of pretending the target was hit.
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 2);
    setPacketSize(10); // Ticks [500, 502, ... 518]

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket();
    reader.popFrontEvent();

    std::unique_ptr<DomainValue> target = std::make_unique<DomainValueImpl<Int>>(reader.getDomainInfo(), 511);
    auto outcome = reader.advanceToDomainValue(target.get());
    ASSERT_EQ(outcome.result, AdvanceResult::Success);
    ASSERT_TRUE(outcome.reachedValue != nullptr);

    auto* reached = dynamic_cast<DomainValueImpl<Int>*>(outcome.reachedValue.get());
    ASSERT_TRUE(reached != nullptr);
    ASSERT_EQ(reached->getValue(), 512);
    assertReaderAtDomainValue(reader, 512);
}

TEST_F(QueueReaderTest, FirstSampleAbsoluteTime)
{
    constexpr Int sampleRate = 1000;
    const std::string origin = "1970-01-01T00:00:00+00:00";
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin(origin)
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);
    setPacketSize(5);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    // No data yet - no absolute time
    ASSERT_FALSE(reader.getFirstSampleAbsoluteTime().has_value());

    sendNextPacket(); // [500 - 504] at 1 ms resolution

    const auto absoluteTime = reader.getFirstSampleAbsoluteTime();
    ASSERT_TRUE(absoluteTime.has_value());

    const auto expected = reader::parseEpoch(origin) + std::chrono::milliseconds(500);
    ASSERT_EQ(*absoluteTime, expected);
}

TEST_F(QueueReaderTest, AvailableUntilEventMatchesSegment)
{
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);
    setPacketSize(5);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket(); // [505 - 509]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());
    setOffsetDelta(getOffset(), 2);
    sendNextPacket(); // [510, 512, ...] behind the descriptor-change event

    reader.popFrontEvent(); // Initial descriptor event

    // Both counts stop at the event boundary, in common-rate equivalent
    ASSERT_EQ(reader.getAvailableSamplesUntilEvent(), 10u);
    ASSERT_EQ(reader.getAvailableSamples(), reader.getAvailableSamplesUntilEvent());

    reader.setSampleRateDivider(2);
    ASSERT_EQ(reader.getAvailableSamplesUntilEvent(), 20u);
}

TEST_F(QueueReaderTest, VectorValueSignalLayout)
{
    // 1-D vector values are supported: counts are in samples, buffers hold count * vectorSize values
    constexpr Int sampleRate = 1000;
    constexpr SizeT vectorSize = 3;
    constexpr SizeT sampleCount = 5;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder()
                           .setSampleType(SampleType::Float64)
                           .setDimensions(List<IDimension>(Dimension(LinearDimensionRule(1, 0, vectorSize))))
                           .build());

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    const auto domainPacket = DataPacket(domainSignal.getDescriptor(), sampleCount, 500);
    const auto valuePacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), sampleCount);
    auto* values = static_cast<double*>(valuePacket.getRawData());
    for (SizeT i = 0; i < sampleCount * vectorSize; ++i)
        values[i] = static_cast<double>(i);
    domainSignal.sendPacket(domainPacket);
    signal.sendPacket(valuePacket);

    reader.popFrontEvent();
    ASSERT_TRUE(reader.isValid());

    // Available counts samples, not values
    ASSERT_EQ(reader.getAvailableSamples(), sampleCount);

    std::array<double, sampleCount * vectorSize> buffer{};
    SizeT count = sampleCount;
    ASSERT_EQ(reader.read(buffer.data(), nullptr, &count), AdvanceResult::Success);
    ASSERT_EQ(count, sampleCount);
    for (SizeT i = 0; i < sampleCount * vectorSize; ++i)
        ASSERT_EQ(buffer[i], static_cast<double>(i));
}

TEST_F(QueueReaderTest, MatrixValueSignalReadable)
{
    // Rank-2 (matrix) values read like any fixed-size block: counts are in samples,
    // buffers hold count * product-of-dimensions values
    constexpr Int sampleRate = 1000;
    constexpr SizeT rows = 2;
    constexpr SizeT cols = 2;
    constexpr SizeT sampleCount = 5;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder()
                           .setSampleType(SampleType::Float64)
                           .setDimensions(List<IDimension>(Dimension(LinearDimensionRule(1, 0, rows)),
                                                           Dimension(LinearDimensionRule(1, 0, cols))))
                           .build());

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    const auto domainPacket = DataPacket(domainSignal.getDescriptor(), sampleCount, 500);
    const auto valuePacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), sampleCount);
    auto* values = static_cast<double*>(valuePacket.getRawData());
    for (SizeT i = 0; i < sampleCount * rows * cols; ++i)
        values[i] = static_cast<double>(i);
    domainSignal.sendPacket(domainPacket);
    signal.sendPacket(valuePacket);

    reader.popFrontEvent();
    ASSERT_TRUE(reader.isValid());

    // Available counts samples, not values
    ASSERT_EQ(reader.getAvailableSamples(), sampleCount);

    std::array<double, sampleCount * rows * cols> buffer{};
    SizeT count = sampleCount;
    ASSERT_EQ(reader.read(buffer.data(), nullptr, &count), AdvanceResult::Success);
    ASSERT_EQ(count, sampleCount);
    for (SizeT i = 0; i < sampleCount * rows * cols; ++i)
        ASSERT_EQ(buffer[i], static_cast<double>(i));
}

#pragma pack(push, 1)
struct QueueReaderTestStructSample
{
    uint32_t id;
    double value;
};
#pragma pack(pop)

TEST_F(QueueReaderTest, StructValueSignalReadable)
{
    // Struct-type values read as raw fixed-size blocks - only the bytes per sample differ
    constexpr Int sampleRate = 1000;
    constexpr SizeT sampleCount = 5;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(
        DataDescriptorBuilder()
            .setName("StructSample")
            .setSampleType(SampleType::Struct)
            .setStructFields(List<IDataDescriptor>(
                DataDescriptorBuilder().setName("Id").setSampleType(SampleType::UInt32).setRule(ExplicitDataRule()).build(),
                DataDescriptorBuilder().setName("Value").setSampleType(SampleType::Float64).setRule(ExplicitDataRule()).build()))
            .build());

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    // Undefined read type resolves dynamically to the signal's type - structs read as raw bytes
    QueueReader reader = QueueReader(inputPort, SampleType::Undefined, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    const auto domainPacket = DataPacket(domainSignal.getDescriptor(), sampleCount, 500);
    const auto valuePacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), sampleCount);
    auto* samples = static_cast<QueueReaderTestStructSample*>(valuePacket.getRawData());
    for (SizeT i = 0; i < sampleCount; ++i)
    {
        samples[i].id = static_cast<uint32_t>(100 + i);
        samples[i].value = static_cast<double>(i) * 1.5;
    }
    domainSignal.sendPacket(domainPacket);
    signal.sendPacket(valuePacket);

    reader.popFrontEvent();
    ASSERT_TRUE(reader.isValid());

    ASSERT_EQ(reader.getAvailableSamples(), sampleCount);

    std::array<QueueReaderTestStructSample, sampleCount> buffer{};
    SizeT count = sampleCount;
    ASSERT_EQ(reader.read(buffer.data(), nullptr, &count), AdvanceResult::Success);
    ASSERT_EQ(count, sampleCount);
    for (SizeT i = 0; i < sampleCount; ++i)
    {
        ASSERT_EQ(buffer[i].id, static_cast<uint32_t>(100 + i));
        ASSERT_EQ(buffer[i].value, static_cast<double>(i) * 1.5);
    }
}

TEST_F(QueueReaderTest, DomainWithDimensionsRejected)
{
    // Domain samples must be scalar - a vector timestamp has no meaning
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .setDimensions(List<IDimension>(Dimension(LinearDimensionRule(1, 0, 2))))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    reader.popFrontEvent();
    ASSERT_FALSE(reader.isValid());
}

TEST_F(QueueReaderTest, TestReading)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket();
    sendNextPacket(); // [510 - 514]
    sendNextPacket(); // [515 - 519]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);
    sendNextPacket(); // [515 - 555]
    sendNextPacket(); // [565 - 605]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 2);

    sendNextPacket(); // [615 - 623]
    sendNextPacket(); // [625 - 633]
    // Establish a queue, now test queue handling
    
    SizeT count = 10;
    ASSERT_EQ(reader.read(nullptr, nullptr, &count), AdvanceResult::Error);
    ASSERT_TRUE(reader.hasPendingEvents());
    reader.popFrontEvent();
    ASSERT_EQ(reader.getAvailableSamples(), 20u);

    std::array<double, 20> buffer;
    count = 20;
    auto result = reader.read(buffer.data(), nullptr, &count);
    ASSERT_EQ(result, AdvanceResult::Success);
    ASSERT_EQ(count, 20u);
    for (size_t i = 0; i < 20; ++i)
    {
        ASSERT_EQ(buffer[i], static_cast<double>(i));
    }

    ASSERT_TRUE(reader.hasPendingEvents());
    auto event = reader.popFrontEvent();
    ASSERT_EQ(event.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);

    reader.setSampleRateDivider(10);
    ASSERT_EQ(reader.getAvailableSamples(), 100);

    count = 80;
    result = reader.read(buffer.data(), nullptr, &count);
    ASSERT_EQ(result, AdvanceResult::Success);
    ASSERT_EQ(count, 80u);
    ASSERT_FALSE(reader.hasPendingEvents());

    ASSERT_TRUE(reader.discardLeftoverSegment(40));
    // Silent discard: only the original descriptor-change event becomes pending
    ASSERT_TRUE(reader.hasPendingEvents());
    event = reader.popFrontEvent();
    ASSERT_EQ(event.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
    ASSERT_FALSE(reader.hasPendingEvents());

    reader.setSampleRateDivider(2);
    ASSERT_EQ(reader.getAvailableSamples(), 20u);
    ASSERT_FALSE(reader.discardLeftoverSegment(8));
    ASSERT_FALSE(reader.discardLeftoverSegment(5000)); // Non-delimited segment
}

TEST_F(QueueReaderTest, ReadingEdgeCases)
{
    // Domain (time) signal: Int64, linear rule.
    constexpr Int sampleRate = 1000;
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(1, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

    setOffsetDelta(500, 1);

    const size_t packetSize = 5;
    setPacketSize(packetSize);

    auto inputPort = InputPort(context, nullptr, "port", true);
    inputPort.connect(signal);
    QueueReader reader = QueueReader(inputPort, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

    sendNextPacket(); // [500 - 504]
    sendNextPacket();
    sendNextPacket(); // [510 - 514]
    sendNextPacket(); // [515 - 519]
    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(10, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 10);
    sendNextPacket(); // [515 - 555]
    sendNextPacket(); // [565 - 605]

    setDomainDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Int64)
                            .setTickResolution(Ratio(1, sampleRate))
                            .setOrigin("1970-01-01T00:00:00+00:00")
                            .setRule(LinearDataRule(2, 0))
                            .setUnit(Unit("s", -1, "second", "time"))
                            .build());

    setOffsetDelta(getOffset(), 2);

    sendNextPacket(); // [615 - 623]
    sendNextPacket(); // [625 - 633]
    // Establish a queue, now test queue handling

    std::array<double, 20> buffer;

    reader.popFrontEvent();
    ASSERT_EQ(reader.getAvailableSamples(), 20u);
    auto result = reader.read(nullptr, nullptr, nullptr);
    ASSERT_EQ(result, AdvanceResult::Error);
    SizeT count = 18;
    result = reader.read(nullptr, nullptr, &count);
    ASSERT_EQ(result, AdvanceResult::Success);
    ASSERT_EQ(count, 18u);

    ASSERT_EQ(reader.getAvailableSamples(), 2u);
    reader.setSampleRateDivider(10);
    count = 15;
    result = reader.read(buffer.data(), nullptr, &count);
    ASSERT_EQ(result, AdvanceResult::Error);
    ASSERT_EQ(count, 0u);
    count = 20;
    result = reader.read(buffer.data(), nullptr, &count);
    ASSERT_EQ(result, AdvanceResult::Success);
    ASSERT_EQ(count, 20u);

    reader.popFrontEvent();
    count = 0;
    ASSERT_EQ(reader.read(buffer.data(), nullptr, &count), AdvanceResult::Success);
    ASSERT_EQ(count, 0u);

    reader.dropOutdatedPacketSegments();
    reader.popFrontEvent();
    ASSERT_EQ(reader.getAvailableSamples(), 100u);
    count = 200;
    ASSERT_EQ(reader.read(nullptr, nullptr, &count), AdvanceResult::NeedMoreData);
}