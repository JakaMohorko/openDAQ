#include <opendaq/context_internal_ptr.h>
#include <opendaq/instance_factory.h>
#include <opendaq/module_ptr.h>
#include <opendaq/opendaq.h>
#include <ref_fb_module/module_dll.h>
#include <testutils/memcheck_listener.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace daq;

// Test matrix from multi-reader-rework-docs/06_usage_examples.md section 5, scoped to the
// implemented modes (EqualRates, MultiRate). Not covered here:
// - Resampled-mode tests (ResampledSumExact, ResampledMainSwitchRuntime,
//   MainSelectionRepopulated, MainDisconnectFallback): the Resampled mode ships with the
//   resampling phase (Phase 5).
// - GapEventResyncContinues: the function block does not enable gap checking on its ports,
//   so no gap events are generated; in-band gap-event resynchronization is covered by the
//   reader-level tests.
// - UnrecoverableErrorReported: the reader is a private member of the function block, so
//   IReaderConfig::markAsInvalid cannot be reached through the public surface; Error-state
//   handling is covered by the reader-level tests.

static ModulePtr createModule(const ContextPtr& context)
{
    ModulePtr module;
    auto logger = Logger();
    createModule(&module, context);
    return module;
}

static ContextPtr createContext()
{
    const auto logger = Logger();
    return Context(Scheduler(logger), logger, TypeManager(), nullptr, nullptr);
}

class SumTest : public testing::Test
{
public:
    ModulePtr module;
    FunctionBlockPtr fb;
    ContextPtr context;

protected:
    struct TestSignal
    {
        SignalConfigPtr value;
        SignalConfigPtr domain;
        DataDescriptorPtr valueDescriptor;
        DataDescriptorPtr domainDescriptor;
        std::int64_t delta = 1;
        std::int64_t nextTick = 0;
    };

    void SetUp() override
    {
        context = createContext();
        module = createModule(context);

        auto config = module.getAvailableFunctionBlockTypes().get("RefFBModuleSumReader").createDefaultConfig();
        config.setPropertyValue("ReaderNotificationMode", static_cast<Int>(PacketReadyNotification::Scheduler));

        fb = module.createFunctionBlock("RefFBModuleSumReader", nullptr, "fb", config);
    }

    void TearDown() override
    {
        // The function block processes packets on scheduler threads; stop the scheduler
        // before the fixture releases its references so no background task outlives the
        // test - the leak listener runs right after and counts in-flight objects as leaks
        if (context.assigned())
        {
            const auto scheduler = context.getScheduler();
            if (scheduler.assigned())
                scheduler.stop();
        }
    }

    static DataDescriptorPtr makeDomainDescriptor(std::int64_t resolutionDen, std::int64_t delta)
    {
        return DataDescriptorBuilder()
            .setSampleType(SampleType::Int64)
            .setTickResolution(Ratio(1, resolutionDen))
            .setOrigin("1970-01-01T00:00:00")
            .setRule(LinearDataRule(delta, 0))
            .setUnit(Unit("s", -1, "seconds", "time"))
            .build();
    }

    // A signal on a linear time grid: resolution 1/resolutionDen seconds and `delta` ticks
    // between samples, i.e. rate = resolutionDen / delta Hz
    TestSignal makeSignal(const std::string& name, std::int64_t resolutionDen, std::int64_t delta, std::int64_t startTick = 0)
    {
        TestSignal signal;
        signal.delta = delta;
        signal.nextTick = startTick;
        signal.domainDescriptor = makeDomainDescriptor(resolutionDen, delta);
        signal.valueDescriptor = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
        signal.domain = SignalWithDescriptor(context, signal.domainDescriptor, nullptr, name + "_time");
        signal.value = SignalWithDescriptor(context, signal.valueDescriptor, nullptr, name);
        signal.value.setDomainSignal(signal.domain);
        return signal;
    }

    // Sends `count` ramp samples whose value equals the domain tick, so every mode's sums
    // can be asserted exactly against the output domain
    static void sendRamp(TestSignal& signal, SizeT count)
    {
        auto domainPacket = DataPacket(signal.domainDescriptor, count, signal.nextTick);
        auto valuePacket = DataPacketWithDomain(domainPacket, signal.valueDescriptor, count);
        double* data = static_cast<double*>(valuePacket.getRawData());
        for (SizeT i = 0; i < count; ++i)
            data[i] = static_cast<double>(signal.nextTick + static_cast<std::int64_t>(i) * signal.delta);
        signal.nextTick += static_cast<std::int64_t>(count) * signal.delta;

        signal.domain.sendPacket(domainPacket);
        signal.value.sendPacket(valuePacket);
    }

    ComponentStatus getStatus() const
    {
        return fb.getStatusContainer().getStatus("ComponentStatus");
    }

    std::string getStatusMessage() const
    {
        return fb.getStatusContainer().getStatusMessage("ComponentStatus").toStdString();
    }

    template <typename Predicate>
    bool waitFor(Predicate predicate, int timeoutMs = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate();
    }

    bool waitForComponentStatus(ComponentStatus status, int timeoutMs = 5000)
    {
        return waitFor([&] { return getStatus() == status; }, timeoutMs);
    }

    bool waitForStatusMessageContains(const std::string& text, int timeoutMs = 5000)
    {
        return waitFor([&] { return getStatusMessage().find(text) != std::string::npos; }, timeoutMs);
    }

    StreamReaderPtr createSumReader()
    {
        return StreamReaderBuilder()
            .setSignal(fb.getSignals()[0])
            .setValueReadType(SampleType::Float64)
            .setDomainReadType(SampleType::Int64)
            .setSkipEvents(true)
            .build();
    }

    // Collects exactly `count` output samples (values and domain ticks) within the timeout
    bool readSumOutput(const StreamReaderPtr& reader,
                       SizeT count,
                       std::vector<double>& values,
                       std::vector<std::int64_t>& ticks,
                       int timeoutMs = 5000)
    {
        values.clear();
        ticks.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (values.size() < count && std::chrono::steady_clock::now() < deadline)
        {
            SizeT available = reader.getAvailableCount();
            if (available == 0)
            {
                // Event packets at the queue head gate availability; a zero-count read
                // consumes them (skipEvents applies on read, not on getAvailableCount)
                SizeT zero = 0;
                reader.read(nullptr, &zero);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            available = std::min<SizeT>(available, count - values.size());
            std::vector<double> valueChunk(available);
            std::vector<std::int64_t> tickChunk(available);
            reader.readWithDomain(valueChunk.data(), tickChunk.data(), &available);
            values.insert(values.end(), valueChunk.begin(), valueChunk.begin() + available);
            ticks.insert(ticks.end(), tickChunk.begin(), tickChunk.begin() + available);
        }
        return values.size() == count;
    }

    // Reads until a sample with value == factor * tick arrives; every sample on the way must
    // itself be an exact sum (value == someFactor * tick) - used across recovery transitions
    // where the contributing-input count changes
    bool waitForSumFactor(const StreamReaderPtr& reader, double factor, int timeoutMs = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            SizeT available = reader.getAvailableCount();
            if (available == 0)
            {
                // See readSumOutput: leading events gate availability until read past
                SizeT zero = 0;
                reader.read(nullptr, &zero);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            std::vector<double> values(available);
            std::vector<std::int64_t> ticks(available);
            reader.readWithDomain(values.data(), ticks.data(), &available);
            for (SizeT i = 0; i < available; ++i)
            {
                if (ticks[i] > 0 && values[i] == factor * static_cast<double>(ticks[i]))
                    return true;
            }
        }
        return false;
    }
};

// --- Basic lifecycle --------------------------------------------------------------------------

TEST_F(SumTest, Create)
{
    ASSERT_TRUE(fb.assigned());
    ASSERT_EQ(getStatus(), ComponentStatus::Warning);
    ASSERT_EQ(fb.getInputPorts().getCount(), 1u);
}

TEST_F(SumTest, ConnectSignalReportsOk)
{
    auto signal = makeSignal("s1", 1000, 1);
    fb.getInputPorts()[0].connect(signal.value);

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
    ASSERT_EQ(fb.getInputPorts().getCount(), 2u);
}

TEST_F(SumTest, DisconnectSignalsRestoresWarning)
{
    std::vector<TestSignal> signals;
    for (int i = 0; i < 4; ++i)
    {
        signals.push_back(makeSignal(fmt::format("s{}", i), 1000, 1));
        fb.getInputPorts()[i].connect(signals.back().value);
    }
    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
    ASSERT_EQ(fb.getInputPorts().getCount(), 5u);

    for (const auto& port : fb.getInputPorts())
        port.disconnect();

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Warning));
    ASSERT_EQ(fb.getInputPorts().getCount(), 1u);
}

TEST_F(SumTest, MissingSignalSparePortInert)
{
    auto s1 = makeSignal("s1", 1000, 1);
    auto s2 = makeSignal("s2", 1000, 1);
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s2.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);
    sendRamp(s2, 100);

    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 100, values, ticks));

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
    ASSERT_EQ(fb.getInputPorts().getCount(), 3u);
    ASSERT_FALSE(fb.getInputPorts()[2].getConnection().assigned());
    ASSERT_EQ(getStatusMessage().find("SumPort_3"), std::string::npos);
}

// --- Modes ------------------------------------------------------------------------------------

TEST_F(SumTest, EqualRatesSumHappyPath)
{
    auto s1 = makeSignal("s1", 1000, 1);
    auto s2 = makeSignal("s2", 1000, 1);
    auto s3 = makeSignal("s3", 1000, 1);
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s2.value);
    fb.getInputPorts()[2].connect(s3.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);
    sendRamp(s2, 100);
    sendRamp(s3, 100);

    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 100, values, ticks));

    for (SizeT i = 0; i < values.size(); ++i)
    {
        ASSERT_DOUBLE_EQ(values[i], 3.0 * static_cast<double>(ticks[i]));
        if (i > 0)
            ASSERT_EQ(ticks[i] - ticks[i - 1], 1);
    }
    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
}

TEST_F(SumTest, MultiRateGcdOutput)
{
    fb.setPropertyValue("Mode", 1);  // MultiRate

    // 10 Hz and 15 Hz on a 1/30 s grid -> common rate 30 Hz, dividers {3, 2}, block 6 ticks
    // -> output 5 Hz, summing the samples that coincide every 6 ticks (200 ms)
    auto s10 = makeSignal("s10", 30, 3);
    auto s15 = makeSignal("s15", 30, 2);
    fb.getInputPorts()[0].connect(s10.value);
    fb.getInputPorts()[1].connect(s15.value);

    auto reader = createSumReader();
    sendRamp(s10, 20);  // ticks 0..57 step 3
    sendRamp(s15, 30);  // ticks 0..58 step 2

    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 10, values, ticks));

    for (SizeT i = 0; i < values.size(); ++i)
    {
        ASSERT_EQ(ticks[i] % 6, 0);
        ASSERT_DOUBLE_EQ(values[i], 2.0 * static_cast<double>(ticks[i]));
        if (i > 0)
            ASSERT_EQ(ticks[i] - ticks[i - 1], 6);
    }
    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
}

TEST_F(SumTest, MultiRateBufferSizing)
{
    fb.setPropertyValue("Mode", 1);  // MultiRate

    // Rates {10, 15, 30} -> dividers {3, 2, 1}, block 6: per-input read counts differ per
    // read (count/divider), and no read may over- or under-run any buffer
    auto s10 = makeSignal("s10", 30, 3);
    auto s15 = makeSignal("s15", 30, 2);
    auto s30 = makeSignal("s30", 30, 1);
    fb.getInputPorts()[0].connect(s10.value);
    fb.getInputPorts()[1].connect(s15.value);
    fb.getInputPorts()[2].connect(s30.value);

    auto reader = createSumReader();
    sendRamp(s10, 20);  // 60 ticks
    sendRamp(s15, 30);  // 60 ticks
    sendRamp(s30, 60);  // 60 ticks

    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 10, values, ticks));

    for (SizeT i = 0; i < values.size(); ++i)
    {
        ASSERT_EQ(ticks[i] % 6, 0);
        ASSERT_DOUBLE_EQ(values[i], 3.0 * static_cast<double>(ticks[i]));
        if (i > 0)
            ASSERT_EQ(ticks[i] - ticks[i - 1], 6);
    }
    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
}

TEST_F(SumTest, MultiRatePartialBlockAtEvent)
{
    fb.setPropertyValue("Mode", 1);  // MultiRate

    auto s10 = makeSignal("s10", 30, 3);
    auto s15 = makeSignal("s15", 30, 2);
    fb.getInputPorts()[0].connect(s10.value);
    fb.getInputPorts()[1].connect(s15.value);

    auto reader = createSumReader();
    sendRamp(s10, 10);  // ticks 0..27 -> 5 complete blocks
    sendRamp(s15, 15);  // ticks 0..28

    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 5, values, ticks));
    for (SizeT i = 0; i < 5; ++i)
    {
        ASSERT_EQ(ticks[i], static_cast<std::int64_t>(i) * 6);
        ASSERT_DOUBLE_EQ(values[i], 2.0 * static_cast<double>(ticks[i]));
    }

    // A partial block (one sample each at tick 30), then a descriptor change on one input:
    // the partial block is discarded silently and summing resumes on the block grid
    sendRamp(s10, 1);
    sendRamp(s15, 1);
    const auto changedDescriptor = DataDescriptorBuilder()
                                       .setSampleType(SampleType::Float64)
                                       .setValueRange(Range(-1000, 1000))
                                       .build();
    s15.valueDescriptor = changedDescriptor;
    s15.value.setDescriptor(changedDescriptor);

    sendRamp(s10, 10);  // ticks 33..60
    sendRamp(s15, 15);  // ticks 32..60

    ASSERT_TRUE(readSumOutput(reader, 4, values, ticks));
    for (SizeT i = 0; i < 4; ++i)
    {
        // The tick-30 block was incomplete at the event: dropped, resync from tick 36
        ASSERT_EQ(ticks[i], 36 + static_cast<std::int64_t>(i) * 6);
        ASSERT_DOUBLE_EQ(values[i], 2.0 * static_cast<double>(ticks[i]));
    }
}

TEST_F(SumTest, ModeSwitchRebuildsReader)
{
    // 10 Hz first (defines the equal-rates reference), then 15 Hz
    auto s10 = makeSignal("s10", 30, 3);
    auto s15 = makeSignal("s15", 30, 2);
    fb.getInputPorts()[0].connect(s10.value);
    fb.getInputPorts()[1].connect(s15.value);

    auto reader = createSumReader();

    // EqualRates: the 15 Hz input is parked, the 10 Hz input sums alone
    sendRamp(s10, 20);
    sendRamp(s15, 30);
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));

    // MultiRate: the parked set is cleared, both inputs contribute on the GCD grid
    fb.setPropertyValue("Mode", 1);
    sendRamp(s10, 20);
    sendRamp(s15, 30);
    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Ok));
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));

    // Back to EqualRates: the 15 Hz input is parked again
    fb.setPropertyValue("Mode", 0);
    sendRamp(s10, 20);
    sendRamp(s15, 30);
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));
}

// --- Error states: parking, warnings, recovery -------------------------------------------------

TEST_F(SumTest, EqualRatesDifferentRateParked)
{
    auto s1 = makeSignal("s1", 1000, 1);   // 1 kHz - connected first, defines the reference rate
    auto s2 = makeSignal("s2", 1000, 1);
    auto s500 = makeSignal("s500", 1000, 2);  // 500 Hz -> parked
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s2.value);
    fb.getInputPorts()[2].connect(s500.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);
    sendRamp(s2, 100);
    sendRamp(s500, 50);

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Warning));
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_3"));
    ASSERT_TRUE(waitForStatusMessageContains("incompatible"));

    // The two healthy inputs keep summing
    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 100, values, ticks));
    for (SizeT i = 0; i < values.size(); ++i)
        ASSERT_DOUBLE_EQ(values[i], 2.0 * static_cast<double>(ticks[i]));
}

TEST_F(SumTest, IgnoreFaultyInputsDisabledReportsWithoutParking)
{
    // C6: with IgnoreFaultyInputs=false a failing input is never excluded - the FB reports
    // it and waits for every input, so no sum is emitted while the failure persists
    fb.setPropertyValue("IgnoreFaultyInputs", False);

    auto s1 = makeSignal("s1", 1000, 1);      // 1 kHz
    auto s500 = makeSignal("s500", 1000, 2);  // 500 Hz -> incompatible in EqualRates mode
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s500.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);
    sendRamp(s500, 50);

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Warning));
    ASSERT_TRUE(waitForStatusMessageContains("not excluded"));
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));

    // Nothing was parked, so nothing sums - the reader waits for the failing input
    SizeT zero = 0;
    reader.read(nullptr, &zero);
    ASSERT_EQ(reader.getAvailableCount(), 0u);

    // Fixing the failing input recovers the whole sum without any probing
    s500.domainDescriptor = makeDomainDescriptor(1000, 1);
    s500.delta = 1;
    s500.nextTick = s1.nextTick;
    s500.domain.setDescriptor(s500.domainDescriptor);

    const bool recovered = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            sendRamp(s500, 10);
            return getStatus() == ComponentStatus::Ok;
        });
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));
}

TEST_F(SumTest, EqualRatesParkedRecoversOnDescriptorFix)
{
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    auto s1 = makeSignal("s1", 1000, 1);
    auto sBad = makeSignal("sBad", 1000, 2);  // 500 Hz -> parked
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(sBad.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);
    sendRamp(sBad, 50);
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));

    // Fix the parked input's rate; the descriptor event reaches the function block through
    // the reader's external listener and triggers a probe
    sBad.domainDescriptor = makeDomainDescriptor(1000, 1);
    sBad.delta = 1;
    sBad.nextTick = s1.nextTick;
    sBad.domain.setDescriptor(sBad.domainDescriptor);

    const bool recovered = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            sendRamp(sBad, 10);
            return getStatus() == ComponentStatus::Ok;
        });
    ASSERT_TRUE(recovered);

    // Both inputs contribute again
    sendRamp(s1, 50);
    sendRamp(sBad, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));
}

TEST_F(SumTest, IncompatibleDescriptorParked)
{
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    auto s1 = makeSignal("s1", 1000, 1);
    auto sBad = makeSignal("sBad", 1000, 1);
    const auto complexDescriptor = DataDescriptorBuilder().setSampleType(SampleType::ComplexFloat32).build();
    sBad.value.setDescriptor(complexDescriptor);

    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(sBad.value);

    auto reader = createSumReader();
    sendRamp(s1, 100);

    ASSERT_TRUE(waitForComponentStatus(ComponentStatus::Warning));
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    ASSERT_TRUE(waitForStatusMessageContains("incompatible"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));

    // A valid descriptor recovers the port
    sBad.value.setDescriptor(sBad.valueDescriptor);
    sBad.nextTick = s1.nextTick;

    const bool recovered = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            sendRamp(sBad, 10);
            return getStatus() == ComponentStatus::Ok;
        });
    ASSERT_TRUE(recovered);

    sendRamp(s1, 50);
    sendRamp(sBad, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));
}

TEST_F(SumTest, DataLossParkedAndRecovers)
{
    fb.setPropertyValue("DataLossTimeout", 0.2);
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    auto s1 = makeSignal("s1", 1000, 1);
    auto s2 = makeSignal("s2", 1000, 1);
    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s2.value);

    auto reader = createSumReader();
    sendRamp(s1, 50);
    sendRamp(s2, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));

    // s2 stops sending; s1 keeps the pipeline alive until the deadline parks s2
    const bool parked = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            return getStatusMessage().find("no data") != std::string::npos;
        });
    ASSERT_TRUE(parked);
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));

    // s2 resumes near the live position; the periodic probe re-enables it
    s2.nextTick = s1.nextTick;
    const bool recovered = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            sendRamp(s2, 10);
            return getStatus() == ComponentStatus::Ok;
        });
    ASSERT_TRUE(recovered);

    sendRamp(s1, 50);
    sendRamp(s2, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));
}

TEST_F(SumTest, SyncFailureParked)
{
    fb.setPropertyValue("MaxSynchronizationDistance", 0.1);
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    auto sBehind = makeSignal("sBehind", 1000, 1, 0);       // starts at t = 0
    auto sAhead = makeSignal("sAhead", 1000, 1, 10000);     // starts 10 s later
    fb.getInputPorts()[0].connect(sBehind.value);
    fb.getInputPorts()[1].connect(sAhead.value);

    auto reader = createSumReader();
    sendRamp(sBehind, 100);
    sendRamp(sAhead, 100);

    // The input too far behind the latest start is parked
    ASSERT_TRUE(waitForStatusMessageContains("cannot synchronize"));
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_1"));
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));

    // Aligned data arrives (the stale queue was dropped while parked): probe recovers it
    sBehind.nextTick = sAhead.nextTick;
    const bool recovered = waitFor(
        [&]
        {
            sendRamp(sBehind, 10);
            sendRamp(sAhead, 10);
            return getStatus() == ComponentStatus::Ok;
        });
    ASSERT_TRUE(recovered);

    sendRamp(sBehind, 50);
    sendRamp(sAhead, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 2.0));
}

TEST_F(SumTest, NoCommonTickParked)
{
    fb.setPropertyValue("Mode", 1);  // MultiRate

    // Two 500 Hz signals on odd/even ticks of a delta-2 grid share no common tick
    auto sEven = makeSignal("sEven", 1000, 2, 0);
    auto sOdd = makeSignal("sOdd", 1000, 2, 1);
    fb.getInputPorts()[0].connect(sEven.value);
    fb.getInputPorts()[1].connect(sOdd.value);

    auto reader = createSumReader();
    sendRamp(sEven, 100);
    sendRamp(sOdd, 100);

    ASSERT_TRUE(waitForStatusMessageContains("cannot synchronize"));
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));

    // The main-grid input keeps summing alone on even ticks
    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    ASSERT_TRUE(readSumOutput(reader, 50, values, ticks));
    for (SizeT i = 0; i < values.size(); ++i)
    {
        ASSERT_EQ(ticks[i] % 2, 0);
        ASSERT_DOUBLE_EQ(values[i], static_cast<double>(ticks[i]));
    }
}

TEST_F(SumTest, AllPortsParkedNoOutput)
{
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    const auto complexDescriptor = DataDescriptorBuilder().setSampleType(SampleType::ComplexFloat32).build();
    auto s1 = makeSignal("s1", 1000, 1);
    auto s2 = makeSignal("s2", 1000, 1);
    s1.value.setDescriptor(complexDescriptor);
    s2.value.setDescriptor(complexDescriptor);

    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(s2.value);

    auto reader = createSumReader();
    ASSERT_TRUE(waitForStatusMessageContains("No usable inputs"));
    ASSERT_EQ(reader.getAvailableCount(), 0u);

    // One input recovers: output resumes while the other stays parked
    s1.value.setDescriptor(s1.valueDescriptor);
    const bool recovered = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            return getStatusMessage().find("No usable inputs") == std::string::npos;
        });
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));

    sendRamp(s1, 50);
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));
}

TEST_F(SumTest, WarningMessageContents)
{
    fb.setPropertyValue("DataLossTimeout", 0.2);
    fb.setPropertyValue("RecoveryRetryInterval", 60.0);  // no probes during the assertions

    auto s1 = makeSignal("s1", 1000, 1);
    auto sBad = makeSignal("sBad", 1000, 1);
    auto sDead = makeSignal("sDead", 1000, 1);
    const auto complexDescriptor = DataDescriptorBuilder().setSampleType(SampleType::ComplexFloat32).build();
    sBad.value.setDescriptor(complexDescriptor);

    fb.getInputPorts()[0].connect(s1.value);
    fb.getInputPorts()[1].connect(sBad.value);
    fb.getInputPorts()[2].connect(sDead.value);

    auto reader = createSumReader();
    sendRamp(s1, 50);
    sendRamp(sDead, 50);

    // sBad parks immediately (incompatible); sDead parks when its deadline passes
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));
    const bool bothParked = waitFor(
        [&]
        {
            sendRamp(s1, 10);
            const auto message = getStatusMessage();
            return message.find("incompatible") != std::string::npos && message.find("no data") != std::string::npos;
        });
    ASSERT_TRUE(bothParked);

    const auto message = getStatusMessage();
    ASSERT_NE(message.find("SumPort_2"), std::string::npos);
    ASSERT_NE(message.find("SumPort_3"), std::string::npos);
    ASSERT_TRUE(waitForSumFactor(reader, 1.0));
}

TEST_F(SumTest, ProbeDoesNotFlap)
{
    fb.setPropertyValue("RecoveryRetryInterval", 0.2);

    auto healthy = makeSignal("healthy", 1000, 1);
    auto bad = makeSignal("bad", 1000, 1);
    const auto complexDescriptor = DataDescriptorBuilder().setSampleType(SampleType::ComplexFloat32).build();
    bad.value.setDescriptor(complexDescriptor);

    fb.getInputPorts()[0].connect(healthy.value);
    fb.getInputPorts()[1].connect(bad.value);

    auto reader = createSumReader();
    ASSERT_TRUE(waitForStatusMessageContains("SumPort_2"));

    // Stream through several probe cycles of the persistently bad port: the healthy input's
    // output must stay gap-free (probes cost a bounded resync, never data)
    std::vector<double> values;
    std::vector<std::int64_t> ticks;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    SizeT sent = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        sendRamp(healthy, 10);
        sent += 10;

        SizeT available = reader.getAvailableCount();
        if (available > 0)
        {
            std::vector<double> valueChunk(available);
            std::vector<std::int64_t> tickChunk(available);
            reader.readWithDomain(valueChunk.data(), tickChunk.data(), &available);
            values.insert(values.end(), valueChunk.begin(), valueChunk.begin() + available);
            ticks.insert(ticks.end(), tickChunk.begin(), tickChunk.begin() + available);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Collect the tail
    std::vector<double> tailValues;
    std::vector<std::int64_t> tailTicks;
    if (readSumOutput(reader, sent - values.size(), tailValues, tailTicks, 2000))
    {
        values.insert(values.end(), tailValues.begin(), tailValues.end());
        ticks.insert(ticks.end(), tailTicks.begin(), tailTicks.end());
    }

    ASSERT_GT(values.size(), 100u);
    for (SizeT i = 0; i < values.size(); ++i)
    {
        ASSERT_DOUBLE_EQ(values[i], static_cast<double>(ticks[i]));
        if (i > 0)
            ASSERT_EQ(ticks[i] - ticks[i - 1], 1) << "gap in the healthy stream at index " << i;
    }

    // The bad port is still parked
    ASSERT_EQ(getStatus(), ComponentStatus::Warning);
    ASSERT_NE(getStatusMessage().find("SumPort_2"), std::string::npos);
}
