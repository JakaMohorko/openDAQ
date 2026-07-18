#include <gtest/gtest.h>

#include <opendaq/input_port_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reference_domain_info_factory.h>
#include <opendaq/synchronization_manager.h>
#include "reader_common.h"

#include <memory>
#include <vector>

class SyncManagerTest : public ReaderTest<>
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
    }

    static DataDescriptorPtr domainDescriptor(const RatioPtr& resolution,
                                              Int delta,
                                              const std::string& epoch = "1970-01-01T00:00:00+00:00",
                                              const ReferenceDomainInfoPtr& referenceDomainInfo = nullptr)
    {
        auto builder = DataDescriptorBuilder()
                           .setSampleType(SampleType::Int64)
                           .setTickResolution(resolution)
                           .setOrigin(epoch)
                           .setRule(LinearDataRule(delta, 0))
                           .setUnit(Unit("s", -1, "second", "time"));
        if (referenceDomainInfo.assigned())
            builder.setReferenceDomainInfo(referenceDomainInfo);
        return builder.build();
    }

    TestInput& addInput(const std::string& name, const DataDescriptorPtr& domainDesc, bool setDomainDescriptor = true)
    {
        auto& input = *inputs.emplace_back(std::make_unique<TestInput>());
        input.domainSignal = Signal(context, nullptr, name + "_time");
        input.signal = Signal(context, nullptr, name);
        input.signal.setDomainSignal(input.domainSignal);
        if (setDomainDescriptor)
            input.domainSignal.setDescriptor(domainDesc);
        input.signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());

        input.port = InputPort(context, nullptr, name + "_port");
        input.port.connect(input.signal);
        input.reader = std::make_unique<QueueReader>(
            input.port, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, false);

        // Consume the initial descriptor event so the cached descriptors are active
        if (input.reader->hasPendingEvents())
            input.reader->popFrontEvent();
        return input;
    }

    void send(TestInput& input, SizeT sampleCount, Int startTick)
    {
        const auto domainPacket = DataPacket(input.domainSignal.getDescriptor(), sampleCount, startTick);
        const auto valuePacket = DataPacketWithDomain(domainPacket, input.signal.getDescriptor(), sampleCount);
        auto* values = static_cast<double*>(valuePacket.getRawData());
        for (SizeT i = 0; i < sampleCount; ++i)
            values[i] = static_cast<double>(i);
        input.domainSignal.sendPacket(domainPacket);
        input.signal.sendPacket(valuePacket);
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

    static Int commonTick(const DomainValue* value)
    {
        const auto* typed = dynamic_cast<const DomainValueImpl<Int>*>(value);
        EXPECT_NE(typed, nullptr);
        return typed ? typed->getValue() : -1;
    }

    static Int firstTick(QueueReader& reader)
    {
        const auto first = reader.getFirstSampleDomainValue();
        EXPECT_TRUE(first != nullptr);
        return first ? dynamic_cast<DomainValueImpl<Int>*>(first.get())->getValue() : -1;
    }

protected:
    std::unique_ptr<SynchronizationManager> manager;
    std::vector<std::unique_ptr<TestInput>> inputs;
};

// --- Checked arithmetic (spec section 4.1) ---

TEST_F(SyncManagerTest, CheckedArithmetic)
{
    ASSERT_EQ(SynchronizationManager::checkedMultiply(3, 4), 12);
    ASSERT_EQ(SynchronizationManager::checkedMultiply(0, 5), std::nullopt);
    ASSERT_EQ(SynchronizationManager::checkedMultiply(-2, 5), std::nullopt);
    const auto big = std::numeric_limits<std::int64_t>::max() / 2 + 1;
    ASSERT_EQ(SynchronizationManager::checkedMultiply(big, 2), std::nullopt);

    ASSERT_EQ(SynchronizationManager::checkedLcm(4, 6), 12);
    ASSERT_EQ(SynchronizationManager::checkedLcm(1, 1), 1);
    ASSERT_EQ(SynchronizationManager::checkedLcm(big, 3), std::nullopt);
}

TEST_F(SyncManagerTest, RationalGcd)
{
    const auto gcd1 = SynchronizationManager::rationalGcd({Ratio(1, 10), Ratio(1, 15)});
    ASSERT_TRUE(gcd1.has_value());
    ASSERT_EQ((*gcd1).getNumerator(), 1);
    ASSERT_EQ((*gcd1).getDenominator(), 30);

    const auto gcd2 = SynchronizationManager::rationalGcd({Ratio(1, 1000), Ratio(1, 1000)});
    ASSERT_TRUE(gcd2.has_value());
    ASSERT_EQ((*gcd2).getNumerator(), 1);
    ASSERT_EQ((*gcd2).getDenominator(), 1000);

    // Unreduced input: 2/10 reduces to 1/5, gcd(1/5, 1/15) = 1/15
    const auto gcd3 = SynchronizationManager::rationalGcd({Ratio(2, 10), Ratio(1, 15)});
    ASSERT_TRUE(gcd3.has_value());
    ASSERT_EQ((*gcd3).getNumerator(), 1);
    ASSERT_EQ((*gcd3).getDenominator(), 15);

    ASSERT_EQ(SynchronizationManager::rationalGcd({}), std::nullopt);
    ASSERT_EQ(SynchronizationManager::rationalGcd({Ratio(0, 10)}), std::nullopt);
}

// --- Common model construction (spec section 4) ---

TEST_F(SyncManagerTest, ModelEqualRates)
{
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    addInput("b", domainDescriptor(Ratio(1, 1000), 1));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_TRUE(result.ok()) << result.message;

    const auto& model = manager->getModel();
    ASSERT_EQ(model.commonSampleRate, 1000);
    ASSERT_EQ(model.sampleRateDividers, (std::vector<SizeT>{1, 1}));
    ASSERT_EQ(model.blockLcm, 1u);
    ASSERT_EQ(model.commonDomain.resolution.getNumerator(), 1);
    ASSERT_EQ(model.commonDomain.resolution.getDenominator(), 1000);
    ASSERT_EQ(inputs[0]->reader->getSampleRateDivider(), 1u);
    ASSERT_EQ(inputs[1]->reader->getSampleRateDivider(), 1u);
}

TEST_F(SyncManagerTest, ModelMixedResolutions)
{
    // 1/10 s and 1/15 s tick resolutions require a 1/30 s common grid (spec section 4.1)
    addInput("a", domainDescriptor(Ratio(1, 10), 1));
    addInput("b", domainDescriptor(Ratio(1, 15), 1));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_TRUE(result.ok()) << result.message;

    const auto& model = manager->getModel();
    ASSERT_EQ(model.commonSampleRate, 30);
    ASSERT_EQ(model.sampleRateDividers, (std::vector<SizeT>{3, 2}));
    ASSERT_EQ(model.blockLcm, 6u);
    ASSERT_EQ(model.commonDomain.resolution.getNumerator(), 1);
    ASSERT_EQ(model.commonDomain.resolution.getDenominator(), 30);
}

TEST_F(SyncManagerTest, ModelDeltaBasedDividers)
{
    // Same resolution, delta 2 halves the rate: rates {1000, 500} -> dividers {1, 2}, block 2
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    addInput("b", domainDescriptor(Ratio(1, 1000), 2));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_TRUE(result.ok()) << result.message;

    const auto& model = manager->getModel();
    ASSERT_EQ(model.commonSampleRate, 1000);
    ASSERT_EQ(model.sampleRateDividers, (std::vector<SizeT>{1, 2}));
    ASSERT_EQ(model.blockLcm, 2u);
}

TEST_F(SyncManagerTest, ModelEarliestEpochWins)
{
    addInput("a", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:01+00:00"));
    addInput("b", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00"));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_TRUE(result.ok()) << result.message;

    ASSERT_EQ(manager->getModel().commonDomain.epoch, inputs[1]->reader->getDomainInfo().epoch);
}

TEST_F(SyncManagerTest, ModelRequiredRateRefinesResolution)
{
    // A required rate above every input rate refines the common resolution so one
    // output sample period is still a whole number of common ticks
    manager->setRequiredCommonSampleRate(2000);
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    addInput("b", domainDescriptor(Ratio(1, 1000), 2));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_TRUE(result.ok()) << result.message;

    const auto& model = manager->getModel();
    ASSERT_EQ(model.commonSampleRate, 2000);
    ASSERT_EQ(model.sampleRateDividers, (std::vector<SizeT>{2, 4}));
    ASSERT_EQ(model.blockLcm, 4u);
    ASSERT_EQ(model.commonDomain.resolution.getNumerator(), 1);
    ASSERT_EQ(model.commonDomain.resolution.getDenominator(), 2000);
}

TEST_F(SyncManagerTest, ModelRequiredRateNotDivisible)
{
    manager->setRequiredCommonSampleRate(999);
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_EQ(result.issue, SyncSetupIssue::RequiredRateNotDivisible);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{0}));
    ASSERT_FALSE(manager->hasModel());
}

TEST_F(SyncManagerTest, ModelEqualRatePolicyViolation)
{
    manager->setAllowDifferentRates(false);
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    addInput("b", domainDescriptor(Ratio(1, 1000), 2));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_EQ(result.issue, SyncSetupIssue::RatesNotEqual);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{1}));
}

TEST_F(SyncManagerTest, ModelMissingDomainDescriptor)
{
    addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    addInput("b", nullptr, false);  // no domain descriptor ever set

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_EQ(result.issue, SyncSetupIssue::MissingDomainDescriptor);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{1}));
}

TEST_F(SyncManagerTest, ModelReferenceDomainIncompatible)
{
    // Distinct assigned reference domain ids with no known time source cannot be related
    addInput("a", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00",
                                   ReferenceDomainInfoBuilder().setReferenceDomainId("A").build()));
    addInput("b", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00",
                                   ReferenceDomainInfoBuilder().setReferenceDomainId("B").build()));

    const auto result = manager->buildCommonModel(readers(), slots(), 0);
    ASSERT_EQ(result.issue, SyncSetupIssue::ReferenceDomainIncompatible);

    // Same id is fine
    inputs.clear();
    addInput("c", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00",
                                   ReferenceDomainInfoBuilder().setReferenceDomainId("A").build()));
    addInput("d", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00",
                                   ReferenceDomainInfoBuilder().setReferenceDomainId("A").build()));
    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
}

// --- Alignment (spec section 5) ---

TEST_F(SyncManagerTest, SyncEqualRatesAlignsToLatestStart)
{
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 30, 500);
    send(b, 30, 510);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_NE(manager->getCommonStart(), nullptr);
    ASSERT_EQ(commonTick(manager->getCommonStart()), 510);

    // Both cursors moved to the aligned start
    ASSERT_EQ(firstTick(*a.reader), 510);
    ASSERT_EQ(firstTick(*b.reader), 510);
}

TEST_F(SyncManagerTest, SyncMixedRatesRoundsToBlockGrid)
{
    // Rates 10 and 15, dividers {3, 2}, block 6 common ticks on a 1/30 grid.
    // Latest start: a at tick 7 (= 21 common ticks); rounded up to the 6-tick grid -> 24.
    auto& a = addInput("a", domainDescriptor(Ratio(1, 10), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 15), 1));
    send(a, 20, 7);
    send(b, 20, 9);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 24);
    ASSERT_EQ(firstTick(*a.reader), 8);   // 24 / divider 3
    ASSERT_EQ(firstTick(*b.reader), 12);  // 24 / divider 2
}

TEST_F(SyncManagerTest, SyncDifferentEpochsExact)
{
    // b's epoch is 1 s later; its local tick 400 is absolute 1400 ms. Latest start is a's
    // tick 1500. Common epoch is a's, so the common start lands on common tick 1500.
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:00+00:00"));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1, "1970-01-01T00:00:01+00:00"));
    send(a, 200, 1500);
    send(b, 200, 400);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 1500);
    ASSERT_EQ(firstTick(*a.reader), 1500);
    ASSERT_EQ(firstTick(*b.reader), 500);  // 1500 ms - 1000 ms epoch offset
}

TEST_F(SyncManagerTest, SyncNeedMoreDataThenSynchronized)
{
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 60, 540);
    send(b, 5, 500);  // ends at 504, well before the aligned start 540

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    auto result = manager->synchronize(readers(), slots());
    ASSERT_EQ(result.outcome, SyncOutcome::NeedMoreData);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{1}));
    ASSERT_EQ(manager->getCommonStart(), nullptr);

    send(b, 60, 505);  // now covers 540
    result = manager->synchronize(readers(), slots());
    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 540);
}

TEST_F(SyncManagerTest, SyncEventPendingBlocksAlignment)
{
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 30, 500);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());

    // The descriptor change lands ahead of b's data, so it becomes a pending event
    // that must be popped before b may advance
    b.domainSignal.setDescriptor(domainDescriptor(Ratio(1, 500), 1));
    send(b, 30, 500);

    const auto result = manager->synchronize(readers(), slots());
    ASSERT_EQ(result.outcome, SyncOutcome::EventPending);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{1}));
    ASSERT_EQ(manager->getCommonStart(), nullptr);
}

TEST_F(SyncManagerTest, SyncNoCommonTick)
{
    // Both signals sample every 2 ticks, phase-shifted by one tick - no shared grid point
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 2));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 2));
    send(a, 60, 500);
    send(b, 60, 501);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Failed);
    ASSERT_EQ(result.reason, SyncFailureReason::NoCommonTick);
    ASSERT_EQ(manager->getCommonStart(), nullptr);
}

TEST_F(SyncManagerTest, SyncIntersampleOffsetWithinHalfBlockAccepted)
{
    // b's grid is phase-shifted by one tick (a tenth of the sample period) - it can never
    // reach an aligned tick exactly. The offset is well below half the aligned block, so the
    // reached sample is unambiguous and synchronization succeeds with the offset preserved
    // in b's own domain (spec section 4.3, direct-path domain output section 7.3).
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 10));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 10));
    send(a, 30, 500);
    send(b, 30, 501);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 510);
    ASSERT_EQ(firstTick(*a.reader), 510);
    ASSERT_EQ(firstTick(*b.reader), 511);
}

TEST_F(SyncManagerTest, SyncHalfSamplePeriodOffsetWithinHalfBlockAccepted)
{
    // b is offset by half its own sample period, but the aligned block (two 500 Hz samples,
    // four ticks) is larger - the reached sample is still strictly nearest to the candidate,
    // so the pair synchronizes (the same shape as reading a 250 Hz and an odd-tick 500 Hz
    // signal together). Contrast with SyncNoCommonTick, where the offset equals half the block.
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 4));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 2));
    send(a, 30, 500);
    send(b, 30, 501);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    ASSERT_EQ(manager->getModel().blockLcm, 2u);
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 504);
    ASSERT_EQ(firstTick(*a.reader), 504);
    ASSERT_EQ(firstTick(*b.reader), 505);
}

TEST_F(SyncManagerTest, MainInputDefinesGridPhase)
{
    // SY-14: the main input supplies the output grid phase. Its samples sit at 502 + 4k -
    // two ticks off the absolute block grid - yet candidates follow ITS grid, so it reaches
    // the start exactly instead of diverging into NoCommonTick.
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 4));  // 250 Hz main
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 2));  // 500 Hz
    send(a, 30, 502);  // ticks 502, 506, ...
    send(b, 60, 500);  // ticks 500, 502, ...

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 502);
    ASSERT_EQ(firstTick(*a.reader), 502);
    ASSERT_EQ(firstTick(*b.reader), 502);
}

TEST_F(SyncManagerTest, SyncStartOnFullUnitOfDomain)
{
    manager->setStartOnFullUnitOfDomain(true);
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 600, 500);   // covers tick 1000
    send(b, 400, 700);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Synchronized) << result.message;
    ASSERT_EQ(commonTick(manager->getCommonStart()), 1000);  // next full second
    ASSERT_EQ(firstTick(*a.reader), 1000);
    ASSERT_EQ(firstTick(*b.reader), 1000);
}

TEST_F(SyncManagerTest, SyncDistanceExceeded)
{
    using namespace std::chrono_literals;
    manager->setMaxSynchronizationDistance(10ms);

    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 30, 500);
    send(b, 30, 700);  // 200 ms later than a

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    const auto result = manager->synchronize(readers(), slots());

    ASSERT_EQ(result.outcome, SyncOutcome::Failed);
    ASSERT_EQ(result.reason, SyncFailureReason::SyncDistanceExceeded);
    ASSERT_EQ(result.affectedInputs, (std::vector<SizeT>{0}));  // a is too far behind the latest start
}

TEST_F(SyncManagerTest, ClearAndInvalidate)
{
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    auto& b = addInput("b", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 30, 500);
    send(b, 30, 500);

    ASSERT_TRUE(manager->buildCommonModel(readers(), slots(), 0).ok());
    ASSERT_EQ(manager->synchronize(readers(), slots()).outcome, SyncOutcome::Synchronized);
    ASSERT_NE(manager->getCommonStart(), nullptr);

    manager->clearSynchronization();
    ASSERT_EQ(manager->getCommonStart(), nullptr);
    ASSERT_TRUE(manager->hasModel());

    manager->invalidateModel();
    ASSERT_FALSE(manager->hasModel());
}

TEST_F(SyncManagerTest, SynchronizeWithoutModelFails)
{
    auto& a = addInput("a", domainDescriptor(Ratio(1, 1000), 1));
    send(a, 10, 500);

    const auto result = manager->synchronize(readers(), slots());
    ASSERT_EQ(result.outcome, SyncOutcome::Failed);
}
