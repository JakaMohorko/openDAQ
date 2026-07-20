#include <gtest/gtest.h>

#include <opendaq/event_packet_ids.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/multi_reader/read_coordinator.h>
#include <opendaq/multi_reader/synchronization_manager.h>
#include "reader_common.h"

using namespace daq::multi_reader;

#include <array>
#include <memory>
#include <vector>

class ReadCoordinatorTest : public ReaderTest<>
{
public:
    using Super = ReaderTest<>;

    struct TestInput
    {
        SignalConfigPtr signal;
        SignalConfigPtr domainSignal;
        InputPortConfigPtr port;
        std::unique_ptr<QueueReader> reader;
    };

protected:
    void SetUp() override
    {
        Super::SetUp();
        manager = std::make_unique<SynchronizationManager>(loggerComponent);
        coordinator = std::make_unique<ReadCoordinator>(loggerComponent);
    }

    TestInput& addInput(const std::string& name, Int delta)
    {
        auto& input = *inputs.emplace_back(std::make_unique<TestInput>());
        input.domainSignal = Signal(context, nullptr, name + "_time");
        input.signal = Signal(context, nullptr, name);
        input.signal.setDomainSignal(input.domainSignal);
        input.domainSignal.setDescriptor(DataDescriptorBuilder()
                                             .setSampleType(SampleType::Int64)
                                             .setTickResolution(Ratio(1, 1000))
                                             .setOrigin("1970-01-01T00:00:00+00:00")
                                             .setRule(LinearDataRule(delta, 0))
                                             .setUnit(Unit("s", -1, "second", "time"))
                                             .build());
        input.signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

        input.port = InputPort(context, nullptr, name + "_port");
        input.port.connect(input.signal);
        input.reader = std::make_unique<QueueReader>(
            input.port, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);
        input.reader->drain();
        if (input.reader->hasPendingEvents())
            input.reader->popFrontEvent();
        return input;
    }

    void send(TestInput& input, SizeT sampleCount, Int startTick, double firstValue = 0.0)
    {
        const auto domainPacket = DataPacket(input.domainSignal.getDescriptor(), sampleCount, startTick);
        const auto valuePacket = DataPacketWithDomain(domainPacket, input.signal.getDescriptor(), sampleCount);
        auto* values = static_cast<double*>(valuePacket.getRawData());
        for (SizeT i = 0; i < sampleCount; ++i)
            values[i] = firstValue + static_cast<double>(i);
        input.domainSignal.sendPacket(domainPacket);
        input.signal.sendPacket(valuePacket);
        // Queues refresh only at explicit drain points (#10)
        input.reader->drain();
    }

    std::vector<QueueReader*> readers() const
    {
        std::vector<QueueReader*> result;
        for (const auto& input : inputs)
            result.push_back(input->reader.get());
        return result;
    }

    std::vector<SizeT> slots() const
    {
        std::vector<SizeT> result(inputs.size());
        for (SizeT i = 0; i < result.size(); ++i)
            result[i] = i;
        return result;
    }

    // Builds the model and aligns both inputs so reads can start
    void buildAndSync()
    {
        ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
        const auto result = manager->synchronize(readers(), slots());
        ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
        coordinator->configure(readers(), manager->getModel());
    }

protected:
    std::unique_ptr<SynchronizationManager> manager;
    std::unique_ptr<ReadCoordinator> coordinator;
    std::vector<std::unique_ptr<TestInput>> inputs;
};

TEST_F(ReadCoordinatorTest, AvailabilityFloorsToBlockLcm)
{
    // Dividers {1, 2}: block of 2 common samples. a delivers 21 common, b 20 -> floor(20/2)*2 = 20
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 2);
    send(a, 21, 500);
    send(b, 10, 500);
    buildAndSync();

    const auto available = coordinator->getAvailableCount(readers(), manager->getModel(), 1);
    ASSERT_EQ(available, 20u);
}

TEST_F(ReadCoordinatorTest, AvailabilityZeroBelowEffectiveMinimum)
{
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 2);
    send(a, 21, 500);
    send(b, 10, 500);
    buildAndSync();

    // 20 common samples available, but the caller requires at least 30
    ASSERT_EQ(coordinator->getAvailableCount(readers(), manager->getModel(), 30), 0u);
}

TEST_F(ReadCoordinatorTest, AvailabilityStopsAtEventBoundary)
{
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 1);
    send(a, 30, 500);
    send(b, 10, 500);
    buildAndSync();

    // A descriptor change on b after 10 samples bounds availability at 10
    b.domainSignal.setDescriptor(DataDescriptorBuilder()
                                     .setSampleType(SampleType::Int64)
                                     .setTickResolution(Ratio(1, 1000))
                                     .setOrigin("1970-01-01T00:00:00+00:00")
                                     .setRule(LinearDataRule(2, 0))
                                     .setUnit(Unit("s", -1, "second", "time"))
                                     .build());
    send(b, 10, 510);

    ASSERT_EQ(coordinator->getAvailableCount(readers(), manager->getModel(), 1), 10u);
}

TEST_F(ReadCoordinatorTest, ReadCommitsAllInputsAligned)
{
    auto& a = addInput("a", 1);  // divider 1: gets count samples
    auto& b = addInput("b", 2);  // divider 2: gets count / 2 samples
    send(a, 20, 500, 100.0);
    send(b, 10, 500, 200.0);
    buildAndSync();

    std::array<double, 20> valuesA{};
    std::array<double, 10> valuesB{};
    std::array<void*, 2> buffers{valuesA.data(), valuesB.data()};

    const auto plan = coordinator->createPlan(20, readers(), manager->getModel(), 1, buffers.data(), nullptr);
    ASSERT_EQ(plan.commonCount, 20u);

    std::string error;
    ASSERT_EQ(coordinator->commit(plan, readers(), error), CommitResult::Ok) << error;

    for (SizeT i = 0; i < 20; ++i)
        ASSERT_EQ(valuesA[i], 100.0 + static_cast<double>(i));
    for (SizeT i = 0; i < 10; ++i)
        ASSERT_EQ(valuesB[i], 200.0 + static_cast<double>(i));

    // Everything consumed
    ASSERT_EQ(coordinator->getAvailableCount(readers(), manager->getModel(), 1), 0u);
}

TEST_F(ReadCoordinatorTest, PlanRoundsRequestDownToBlocks)
{
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 2);
    send(a, 20, 500);
    send(b, 10, 500);
    buildAndSync();

    // Request 19 with block 2 -> 18
    const auto plan = coordinator->createPlan(19, readers(), manager->getModel(), 1, nullptr, nullptr);
    ASSERT_EQ(plan.commonCount, 18u);

    // Request below one block -> nothing
    const auto empty = coordinator->createPlan(1, readers(), manager->getModel(), 1, nullptr, nullptr);
    ASSERT_EQ(empty.commonCount, 0u);
}

TEST_F(ReadCoordinatorTest, SkipSharesAlignmentWithRead)
{
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 2);
    send(a, 20, 500);
    send(b, 10, 500);
    buildAndSync();

    const auto plan = coordinator->createPlan(19, readers(), manager->getModel(), 1, nullptr, nullptr);
    ASSERT_EQ(plan.commonCount, 18u);

    std::string error;
    ASSERT_EQ(coordinator->skip(plan, readers(), error), CommitResult::Ok) << error;

    // 2 common samples remain (1 native on a... 2 on a, 1 on b)
    ASSERT_EQ(coordinator->getAvailableCount(readers(), manager->getModel(), 1), 2u);
}

TEST_F(ReadCoordinatorTest, DiscardLeftoverSegmentsIsSilent)
{
    auto& a = addInput("a", 1);
    auto& b = addInput("b", 1);
    send(a, 30, 500);
    send(b, 3, 500);
    buildAndSync();

    // b: 3 samples then a descriptor change - fewer than one block of 10 remains
    b.domainSignal.setDescriptor(DataDescriptorBuilder()
                                     .setSampleType(SampleType::Int64)
                                     .setTickResolution(Ratio(1, 1000))
                                     .setOrigin("1970-01-01T00:00:00+00:00")
                                     .setRule(LinearDataRule(2, 0))
                                     .setUnit(Unit("s", -1, "second", "time"))
                                     .build());
    b.reader->drain();

    CommonModel blockTen;
    blockTen.blockLcm = 10;

    const auto discarded = coordinator->discardLeftoverSegments(readers(), blockTen, 1);
    ASSERT_EQ(discarded, (std::vector<SizeT>{1}));

    // The discard is silent: the pending event is the descriptor change, nothing synthetic
    ASSERT_TRUE(b.reader->hasPendingEvents());
    const auto event = b.reader->popFrontEvent();
    ASSERT_EQ(event.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
    ASSERT_FALSE(b.reader->hasPendingEvents());
}
