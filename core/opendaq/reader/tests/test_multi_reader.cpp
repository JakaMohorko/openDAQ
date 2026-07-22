#include <opendaq/custom_log.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_exceptions.h>
#include <opendaq/reader_factory.h>
#include <opendaq/time_reader.h>
#include "reader_common.h"

#include <gmock/gmock-matchers.h>

#include <chrono>
#include <future>
#include <thread>
#include <utility>

using namespace daq;
using namespace testing;

struct ReadSignal;

static void zeroOutPacketData(const DataPacketPtr& packet);
static DataPacketPtr createPacket(daq::SizeT numSamples, daq::Int offset, const ReadSignal& read);

struct ReadSignal
{
    explicit ReadSignal(const SignalConfigPtr& signal, Int packetOffset, Int packetSize)
        : packetSize(packetSize)
        , packetOffset(packetOffset)
        , signal(signal)
        , valueDescriptor(signal.getDescriptor())
    {
    }

    void setPacketOffset(Int offset)
    {
        packetOffset = offset;
    }

    void setPacketSize(Int size)
    {
        packetSize = size;
    }

    [[nodiscard]] SignalConfigPtr getDomainSignal() const
    {
        return signal.getDomainSignal();
    }

    [[nodiscard]] auto getDomainDescriptor() const
    {
        return getDomainSignal().getDescriptor();
    }

    void setValueDescriptor(const DataDescriptorPtr& descriptor)
    {
        signal.setDescriptor(descriptor);
        valueDescriptor = descriptor;
    }

    template <typename RoundTo = std::chrono::system_clock::duration>
    [[nodiscard]] auto toSysTime(ClockTick value, const DataDescriptorPtr& domainDataDescriptor = nullptr) const
    {
        using namespace std::chrono;

        auto dataDescriptor = domainDataDescriptor.assigned() ? domainDataDescriptor : getDomainDescriptor();

        system_clock::time_point parsedEpoch{};
        std::istringstream epochString(reader::fixupIso8601(dataDescriptor.getOrigin()));
        date::from_stream(epochString, "%FT%T%z", parsedEpoch);

        return reader::toSysTime<decltype(value), RoundTo>(value, parsedEpoch, dataDescriptor.getTickResolution());
    }

    template <typename ValueType = double>
    DataPacketPtr createAndSendPacket(Int packetIndex, bool log = false) const
    {
        Int delta = getDomainDescriptor().getRule().getParameters()["delta"];

        auto offset = packetOffset + ((packetSize * delta) * packetIndex);
        if (log)
        {
            std::cout << "<" << packetIndex << "> " << "(off: " << offset << " pSize: " << packetSize << " pOffset: " << packetOffset << ")"
                      << std::endl;
        }

        auto packet = createPacket(static_cast<SizeT>(packetSize), offset, *this);
        zeroOutPacketData(packet);

        ValueType* data = static_cast<ValueType*>(packet.getData());
        for (auto i = 0; i < packetSize; ++i)
        {
            using Scalar = std::conditional_t<
                std::is_same_v<ValueType, Complex_Number<float>>, float,
                std::conditional_t<
                    std::is_same_v<ValueType, Complex_Number<double>>, double, ValueType
                >
            >;

            data[i] = ValueType(static_cast<Scalar>(offset + i));
        }

        if (log)
        {
            ClockTick* ticks = static_cast<ClockTick*>(packet.getDomainPacket().getData());
            for (int i = 0; i < packetSize; ++i)
            {
                std::cout << ticks[i] << std::endl;
            }
        }

        signal.sendPacket(packet);
        return packet;
    }

    Int packetSize;
    Int packetOffset;
    SignalConfigPtr signal;
    DataDescriptorPtr valueDescriptor;
};

static std::string loggerEnvVariable{"OPENDAQ_SINK_WINDEBUG_LOG_LEVEL"};

class MultiReaderTest : public ReaderTest<>
{
public:
    using Super = ReaderTest<>;

    ReadSignal& addSignal(Int packetOffset, Int packetSize, const SignalPtr& domain, SampleType valueType = SampleType::Float64)
    {
        auto newSignal = Signal(context, nullptr, fmt::format("sig{}", counter++));
        newSignal.setDescriptor(setupDescriptor(valueType));
        newSignal.setDomainSignal(domain);

        return readSignals.emplace_back(newSignal, packetOffset, packetSize);
    }

    [[nodiscard]] SignalConfigPtr createDomainSignal(std::string epoch = "",
                                                     const daq::RatioPtr& resolution = nullptr,
                                                     const daq::DataRulePtr& rule = nullptr,
                                                     const daq::ReferenceDomainInfoPtr& referenceDomainInfo = nullptr) const
    {
        auto domain = Signal(context, nullptr, "time");
        domain.setDescriptor(createDomainDescriptor(std::move(epoch), resolution, rule, referenceDomainInfo));

        return domain;
    }

    void sendPackets(Int index) const
    {
        for (const auto& signal : readSignals)
        {
            signal.createAndSendPacket(index);
        }
    }

    [[nodiscard]] ListPtr<ISignal> signalsToList() const
    {
        ListPtr<SignalConfigPtr> signals = List<ISignalConfig>();
        for (const auto& read : readSignals)
        {
            signals.pushBack(read.signal);
        }
        return signals;
    }

    [[nodiscard]] ListPtr<IInputPortConfig> portsList(bool enableGapDetection = false) const
    {
        ListPtr<IInputPortConfig> ports = List<IInputPortConfig>();
        size_t index = 0;
        for (const auto& read : readSignals)
        {
            std::string localId = "readsig" + std::to_string(index++);
            auto port = InputPort(read.signal.getContext(), nullptr, localId, enableGapDetection);
            ports.pushBack(port);
        }
        return ports;
    }

    template <typename RoundTimeTo, typename T, typename U, typename V>
    void printData(std::int64_t samples, T& times, U& values, V& domain) const
    {
        using namespace reader;

        int numSignals = static_cast<int>(readSignals.size());
        for (int sigIndex = 0; sigIndex < numSignals; ++sigIndex)
        {
            fmt::print("--------\n");
            fmt::print("Signal {}\n", sigIndex);

            for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
            {
                times[sigIndex][sampleIndex] = readSignals[sigIndex].toSysTime<RoundTimeTo>(domain[sigIndex][sampleIndex]);
                std::cout << times[sigIndex][sampleIndex];

                fmt::print(" |d: {} |v: {}\n", domain[sigIndex][sampleIndex], values[sigIndex][sampleIndex]);
            }
        }
    }

    template <typename T, typename U>
    void printData(std::int64_t samples, T& times, U& values) const
    {
        using namespace std::chrono;
        using namespace reader;

        int numSignals = static_cast<int>(readSignals.size());
        for (int sigIndex = 0; sigIndex < numSignals; ++sigIndex)
        {
            fmt::print("--------\n");
            fmt::print("Signal {}\n", sigIndex);

            for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
            {
                std::stringstream ss;
                ss << times[sigIndex][sampleIndex];

                fmt::print(" |d: {} |v: {}\n", ss.str(), values[sigIndex][sampleIndex]);
            }
        }
    }

    template <typename RoundTimeTo, typename T>
    void roundData(std::int64_t samples, T& times)
    {
        int numSignals = static_cast<int>(readSignals.size());
        for (int sigIndex = 0; sigIndex < numSignals; ++sigIndex)
        {
            for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
            {
                times[sigIndex][sampleIndex] = std::chrono::round<RoundTimeTo>(times[sigIndex][sampleIndex]);
            }
        }
    }

protected:
    int counter{};
    LogLevel prevLogLevel{LogLevel::Default};
    SignalConfigPtr domainSignal;
    std::vector<ReadSignal> readSignals;
};

static void zeroOutPacketData(const DataPacketPtr& packet)
{
    memset(packet.getRawData(), 0, packet.getRawDataSize());
}

static DataPacketPtr createPacket(daq::SizeT numSamples, daq::Int offset, const ReadSignal& read)
{
    auto domainPacket = daq::DataPacket(read.getDomainDescriptor(), numSamples, offset);

    return daq::DataPacketWithDomain(domainPacket, read.valueDescriptor, numSamples);
}

TEST_F(MultiReaderTest, SignalStartDomainFrom0)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SignalStartDomainFrom0SkipSamples)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    SizeT skip = 100;
    multi.skipSamples(&skip);
    ASSERT_EQ(skip, 100u);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 346u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
    ASSERT_EQ(domain[0][0], 1223);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 341u);

    skip = 1000;
    multi.skipSamples(&skip);
    ASSERT_EQ(skip, 341u);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);
}

TEST_F(MultiReaderTest, IsSynchronized)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();
    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    ASSERT_FALSE(multi.getIsSynchronized());

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    ASSERT_FALSE(multi.getIsSynchronized());

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    // Behavior change (spec 6.2): the reader synchronizes eagerly when data arrives - the
    // coalesced evaluation runs on the scheduler, so wait for it instead of calling an
    // accessor that would evaluate lazily (getIsSynchronized only inspects the state)
    context.getScheduler().waitAll();
    ASSERT_TRUE(multi.getIsSynchronized());

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    ASSERT_TRUE(multi.getIsSynchronized());

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 441u);

    ASSERT_TRUE(multi.getIsSynchronized());
}

TEST_F(MultiReaderTest, SignalStartDomainFrom0Raw)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"), SampleType::Float64);
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"), SampleType::Int64);
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"), SampleType::UInt32);

    auto multi = MultiReaderBuilder()
                     .addSignals(signalsToList())
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .setValueReadType(SampleType::Undefined)
                     .setDomainReadType(SampleTypeFromType<ClockTick>::SampleType)
                     .setReadTimeoutType(ReadTimeoutType::All)
                     .setReadMode(ReadMode::RawValue)
                     .build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket<double>(0);
    sig1.createAndSendPacket<int64_t>(0);
    sig2.createAndSendPacket<uint32_t>(0);

    sig0.createAndSendPacket<double>(1);
    sig1.createAndSendPacket<int64_t>(1);
    sig2.createAndSendPacket<uint32_t>(1);

    sig0.createAndSendPacket<double>(2);
    sig1.createAndSendPacket<int64_t>(2);
    sig2.createAndSendPacket<uint32_t>(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = 5u;

    double values0[SAMPLES]{};
    std::int64_t values1[SAMPLES]{};
    std::uint32_t values2[SAMPLES]{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values0, values1, values2};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    // printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SignalStartRelativeOffset0)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG0_PACKET_SIZE = 523u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, SIG0_PACKET_SIZE, createDomainSignal(" "));
    auto& sig1 = addSignal(0, 732, createDomainSignal(" "));
    auto& sig2 = addSignal(0, 843, createDomainSignal(" "));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, SIG0_PACKET_SIZE * 3);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SignalStartDomainFrom0Timeout)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG1_PACKET_SIZE = 523;

    // prevent vector from re-allocating so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, SIG1_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = SIG1_PACKET_SIZE + 1;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    std::thread thread(
        [sig0, sig1, sig2]
        {
            using namespace std::chrono_literals;

            std::this_thread::sleep_for(200ms);

            sig0.createAndSendPacket(3);
            sig1.createAndSendPacket(3);
            sig2.createAndSendPacket(3);
        });

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count, 1000u);

    if (thread.joinable())
        thread.join();

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SignalStartDomainFrom0TimeoutExceeded)
{
    SKIP_TEST_MAC_CI;

    using namespace std::chrono;
    using namespace std::chrono_literals;

    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG1_PACKET_SIZE = 523;

    // prevent vector from re-allocating so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, SIG1_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = SIG1_PACKET_SIZE * 2;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    std::thread thread(
        [sig0, sig1, sig2]
        {
            using namespace std::chrono_literals;

            std::this_thread::sleep_for(200ms);

            sig0.createAndSendPacket(3);
            sig1.createAndSendPacket(3);
            sig2.createAndSendPacket(3);
        });

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count, 300);

    if (thread.joinable())
        thread.join();

    ASSERT_EQ(count, SAMPLES - 77);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(static_cast<std::int64_t>(count), time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, WithPacketOffsetNot0)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 458u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, WithPacketOffsetNot0Relative)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG0_PACKET_SIZE = 523u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, SIG0_PACKET_SIZE, createDomainSignal(" "));
    auto& sig1 = addSignal(134, 732, createDomainSignal(" "));
    auto& sig2 = addSignal(111, 843, createDomainSignal(" "));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    // samples needed to sync sig0 (134 - 123 = 11)
    ASSERT_EQ(available, (SIG0_PACKET_SIZE * 3) - 11);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, MaxTimeIsNotOnSignalWithMaxEpoch)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG0_PACKET_SIZE = 523u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(1240, SIG0_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:03+00:00"));  // 03:4.24
    auto& sig1 = addSignal(134, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, SIG0_PACKET_SIZE * 3);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Clock10kHzDelta10)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Clock15MHzFromEpoch)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    Int clock = 15000000;
    Int startOffest = 1781269748ll * clock;
    auto& sig0 = addSignal(startOffest, 6093750, createDomainSignal("1970-01-01T00:00:00+00:00", Ratio(1, clock)));
    auto& sig1 = addSignal(startOffest, 2812500, createDomainSignal("1970-01-01T00:00:00+00:00", Ratio(1, clock)));
    auto& sig2 = addSignal(startOffest, 3750000, createDomainSignal("1970-01-01T00:00:00+00:00", Ratio(1, clock)));

    auto multi = MultiReaderBuilder()
                     .setStartOnFullUnitOfDomain(true)
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 2812500 * 3);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}


TEST_F(MultiReaderTest, Clock10kHzDelta10Relative)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG0_PACKET_SIZE = 523u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, SIG0_PACKET_SIZE, createDomainSignal(" "));
    auto& sig1 = addSignal(0, 732, createDomainSignal(" ", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal(" "));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, SIG0_PACKET_SIZE * 3);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Clock10kHzDelta10WithAlignedOffset)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(1240, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(130, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 1569u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Clock10kHzDelta10WithAlignedOffsetRelative)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(1240, 523, createDomainSignal(" "));
    auto& sig1 = addSignal(130, 732, createDomainSignal(" ", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(111, 843, createDomainSignal(" "));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 969u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Clock10kHzDelta10WithIntersampleOffset)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG10_PACKET_SIZE = 523;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(1240, SIG10_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(131, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 3u * SIG10_PACKET_SIZE);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    std::array readDomain{time[0][0], time[1][0], time[2][0]};

    using Seconds = std::chrono::duration<double>;

    auto firstTime = time[0][0];
    ASSERT_THAT(readDomain, testing::ElementsAre(firstTime, firstTime + Seconds(0.0001), firstTime));
}

TEST_F(MultiReaderTest, EpochChanged)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG1_PACKET_SIZE = 732;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, SIG1_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();
    TimeReader timeReader(multi);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig1.getDomainSignal().setDescriptor(createDomainDescriptor("2022-09-27T00:02:04.1+00:00"));

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    sig0.createAndSendPacket(3);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 632u);

    // Read over the signal-descriptor change (it will stop on event so maximim read samples will be still 632)
    constexpr const SizeT SAMPLES = SIG1_PACKET_SIZE + 1;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{time[0], time[1], time[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    ASSERT_EQ(count, available);

    printData(count, time, values);
    roundData<std::chrono::microseconds>(count, time);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, EpochChangedBeforeFirstData)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig1.getDomainSignal().setDescriptor(createDomainDescriptor("2022-09-27T00:02:04.1+00:00"));

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{0};
    MultiReaderStatusPtr status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_TRUE(status.getEventPackets().assigned());
    ASSERT_EQ(status.getEventPackets().getCount(), 1u);
    // Event dict is keyed by the input id (C6): for a signal-built reader that is the signal's
    // global id, not the synthetic internal port's - the same id getInputStates/setInputUsed use
    ASSERT_TRUE(status.getEventPackets().hasKey("/sig1"));
    ASSERT_NE(status.getEventPackets().get("/sig1"), nullptr);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 458u);

    count = SAMPLES;
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, Signal2Invalidated)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG2_PACKET_SIZE = 843u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, SIG2_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();
    TimeReader timeReader(multi);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig2.setValueDescriptor(setupDescriptor(SampleType::ComplexFloat64));

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket<ComplexFloat64>(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket<ComplexFloat64>(2);

    sig0.createAndSendPacket(3);

    available = multi.getAvailableCount();

    // 1 packet available until descriptor changes
    ASSERT_EQ(available, SIG2_PACKET_SIZE);

    // Read over the signal-descriptor change
    constexpr const SizeT SAMPLES = SIG2_PACKET_SIZE + 1;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{time[0], time[1], time[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    ASSERT_EQ(count, SIG2_PACKET_SIZE);

    auto status = multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    // The read runs into the descriptor-change event; validity stays true - only
    // ReadStatus::Fail is unrecoverable (C5/Q1)
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    printData(SAMPLES, time, values);
    roundData<std::chrono::microseconds>(SAMPLES, time);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, ResolutionChanged)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG1_PACKET_SIZE = 732;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, SIG1_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();
    TimeReader timeReader(multi);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0, false);
    sig2.createAndSendPacket(0);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    // Increase the resolution at the same sample-rate
    sig1.getDomainSignal().setDescriptor(createDomainDescriptor("2022-09-27T00:02:04+00:00", Ratio(1, 10000), LinearDataRule(10, 0)));
    sig1.packetOffset *= 10;

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1, false);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2, false);
    sig2.createAndSendPacket(2);

    sig0.createAndSendPacket(3);
    sig1.createAndSendPacket(3, false);
    sig2.createAndSendPacket(3);

    available = multi.getAvailableCount();
    // 732 - 100 needed to sync before descriptor changed
    ASSERT_EQ(available, 632u);

    // Read over the signal-descriptor change. it will stops on event. so it will read 632 as getAvailableCount return synced samples until
    // event
    constexpr const SizeT SAMPLES = SIG1_PACKET_SIZE + 1;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{time[0], time[1], time[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    ASSERT_EQ(count, available);

    printData(available, time, values);
    roundData<std::chrono::microseconds>(available, time);

    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SampleRateChanged)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const auto SIG1_PACKET_SIZE = 732u;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(123, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(134, SIG1_PACKET_SIZE, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(111, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();
    TimeReader timeReader(multi);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0, false);
    sig2.createAndSendPacket(0);

    // Change the sample-rate (same resolution, different delta)
    sig1.getDomainSignal().setDescriptor(createDomainDescriptor("2022-09-27T00:02:04+00:00", nullptr, LinearDataRule(10, 0)));

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1, false);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2, false);
    sig2.createAndSendPacket(2);

    sig0.createAndSendPacket(3);
    sig1.createAndSendPacket(3, false);
    sig2.createAndSendPacket(3);

    available = multi.getAvailableCount();

    // 732 - 100 needed to sync until next descriptor change
    ASSERT_EQ(available, SIG1_PACKET_SIZE - 100);

    // Read over the signal-descriptor change
    constexpr const SizeT SAMPLES = SIG1_PACKET_SIZE + 1;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{time[0], time[1], time[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    ASSERT_EQ(count, 632u);

    // Behavior change (spec 6.1/8.5): a sample-rate change is a recoverable event, not a
    // sticky invalidation - the next read returns the descriptor-change event and the
    // reader resynchronizes with the new dividers
    count = SAMPLES;
    auto status = multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);
    ASSERT_TRUE(status.getValid());
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(count, 0u);

    printData(SAMPLES, time, values);
    roundData<std::chrono::microseconds>(SAMPLES, time);

    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
}

// The ReuseReader test was deleted (test plan Part C): MultiReaderFromExisting is removed by
// the rework (spec 8.4) - recovery happens in the same reader instance via the state machine.

TEST_F(MultiReaderTest, MultiReaderWithInputPort)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto ports = portsList();
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        ports[i].connect(signals[i]);

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    auto status = multi.read(nullptr, &available);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, MultiReaderWithNotConnectedInputPort)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto portList = List<IInputPortConfig>();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList.pushBack(InputPort(readSignals[i].signal.getContext(), nullptr, "readsig" + std::to_string(i)));

    auto ports = portsList();
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    for (size_t i = 0; i < NUM_SIGNALS; i++)
        ports[i].connect(signals[i]);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    auto status = multi.read(nullptr, &available);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 446u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, MultiReaderWithDifferentInputs)
{
    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto portList = portsList();
    auto componentList = List<IComponent>(portList[0], portList[1], sig2.signal);
    ASSERT_THROW(MultiReaderFromPort(componentList), InvalidParameterException);
}

TEST_F(MultiReaderTest, MultipleMultiReaderToInputPort)
{
    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(1);

    addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();

    auto reader1 = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    ASSERT_THROW(MultiReaderFromPort(portList), AlreadyExistsException);
}

TEST_F(MultiReaderTest, MultiReaderReuseInputPort)
{
    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(1);

    addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();

    {
        auto reader1 = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    }
    ASSERT_NO_THROW(MultiReaderFromPort(portList));
}

TEST_F(MultiReaderTest, MultiReaderOnReadCallback)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    std::promise<void> promise;
    std::future<void> future = promise.get_future();

    SizeT count{SAMPLES};
    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto reader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT tmpCount{0};
        auto status = reader.read(nullptr, &tmpCount);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    reader.setOnDataAvailable(
        [&, promise = &promise]() mutable
        {
            if (reader.getAvailableCount() < count)
                return;

            reader.readWithDomain(valuesPerSignal, domainPerSignal, &count);
            reader.setOnDataAvailable(nullptr);  // trigger callback only once
            promise->set_value();
        });

    auto available = reader.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    auto promiseStatus = future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(promiseStatus, std::future_status::ready);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, MultiReaderFromPortOnReadCallback)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    std::promise<void> promise;
    std::future<void> future = promise.get_future();

    SizeT count{SAMPLES};
    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto ports = portsList();
    auto signals = signalsToList();
    auto reader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        ports[i].connect(signals[i]);

    SizeT toRead = 0u;
    auto status = reader.read(nullptr, &toRead);

    reader.setOnDataAvailable(
        [&, promise = &promise]
        {
            if (reader.getAvailableCount() < count)
                return;

            reader.readWithDomain(valuesPerSignal, domainPerSignal, &count);
            reader.setOnDataAvailable(nullptr);  // trigger callback only once
            promise->set_value();
        });

    auto available = reader.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    auto promiseStatus = future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(promiseStatus, std::future_status::ready);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, StartOnFullUnitOfDomain)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00"));
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto multi = MultiReaderBuilder()
                     .addSignals(signalsToList())
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .setReadTimeoutType(ReadTimeoutType::All)
                     .setRequiredCommonSampleRate(-1)
                     .setStartOnFullUnitOfDomain(true)
                     .build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    for (Int i = 0; i < 5; i++)
    {
        sig0.createAndSendPacket(i);
        sig1.createAndSendPacket(i);
        sig2.createAndSendPacket(i);
    }

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 615u);

    constexpr const SizeT SAMPLES = 5u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    ASSERT_THAT(time[1], ElementsAreArray(time[0]));
    ASSERT_THAT(time[2], ElementsAreArray(time[0]));
}

TEST_F(MultiReaderTest, SampleRateDivider)
{
    constexpr const auto NUM_SIGNALS = 3;
    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    std::array<std::int32_t, NUM_SIGNALS> dividers = {1, 2, 5};

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00", nullptr, LinearDataRule(dividers[0], 0)));  // 1000 Hz
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", nullptr, LinearDataRule(dividers[1], 0)));  // 500 Hz
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.125+00:00", nullptr, LinearDataRule(dividers[2], 0)));  // 200 Hz

    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    ASSERT_EQ(multi.getCommonSampleRate(), 1000);

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    for (Int i = 0; i < 5; i++)
    {
        sig0.createAndSendPacket(i);
        sig1.createAndSendPacket(i);
        sig2.createAndSendPacket(i);
    }

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 1480u);

    constexpr const SizeT SAMPLES = 52u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES - 2);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    std::array<ClockTick, NUM_SIGNALS> lastTimeStamp;

    for (SizeT i = 0; i < dividers.size(); i++)
    {
        const SizeT numberOfSamples = count / dividers[i];

        ASSERT_EQ(time[i][0], time[0][0]);
        lastTimeStamp[i] = domain[i][numberOfSamples - 1];

        for (SizeT j = 1; j < numberOfSamples; j++)
            ASSERT_EQ(domain[i][j] - domain[i][j - 1], dividers[i]);
    }

    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    for (SizeT i = 0; i < dividers.size(); i++)
    {
        ASSERT_EQ(domain[i][0] - lastTimeStamp[i], dividers[i]);
    }
}

TEST_F(MultiReaderTest, SampleRateDividerRequiredRate)
{
    constexpr const auto NUM_SIGNALS = 3;
    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    std::array<std::int32_t, NUM_SIGNALS> dividers = {1, 2, 5};
    const std::int64_t requiredRate = 2000;

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00", nullptr, LinearDataRule(dividers[0], 0)));  // 1000 Hz
    auto& sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", nullptr, LinearDataRule(dividers[1], 0)));  // 500 Hz
    auto& sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.125+00:00", nullptr, LinearDataRule(dividers[2], 0)));  // 200 Hz

    auto multi = MultiReaderBuilder()
                     .addSignals(signalsToList())
                     .setValueReadType(SampleType::Float64)
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .setDomainReadType(SampleType::Int64)
                     .setReadMode(ReadMode::Scaled)
                     .setReadTimeoutType(ReadTimeoutType::All)
                     .setRequiredCommonSampleRate(requiredRate)
                     .setStartOnFullUnitOfDomain(false)
                     .build();
    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    ASSERT_EQ(multi.getCommonSampleRate(), requiredRate);

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    for (Int i = 0; i < 5; i++)
    {
        sig0.createAndSendPacket(i);
        sig1.createAndSendPacket(i);
        sig2.createAndSendPacket(i);
    }

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 2960u);

    constexpr const SizeT SAMPLES = 102u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1], domain[2]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES - 2);

    std::array<std::chrono::system_clock::time_point[SAMPLES], NUM_SIGNALS> time{};
    printData<std::chrono::microseconds>(SAMPLES, time, values, domain);

    std::array<ClockTick, NUM_SIGNALS> lastTimeStamp;

    for (SizeT i = 0; i < dividers.size(); i++)
    {
        const SizeT numberOfSamples = count / dividers[i] / 2;

        ASSERT_EQ(time[i][0], time[0][0]);
        lastTimeStamp[i] = domain[i][numberOfSamples - 1];

        for (SizeT j = 1; j < numberOfSamples; j++)
            ASSERT_EQ(domain[i][j] - domain[i][j - 1], dividers[i]);
    }

    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    for (SizeT i = 0; i < dividers.size(); i++)
    {
        ASSERT_EQ(domain[i][0] - lastTimeStamp[i], dividers[i]);
    }
}

TEST_F(MultiReaderTest, MultiReaderBuilderGetSet)
{
    SignalPtr sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00")).signal;
    SignalPtr sig1 =
        addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0))).signal;
    SignalPtr sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00")).signal;

    auto portList = portsList();

    auto builder = MultiReaderBuilder();
    builder.setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    builder.addInputPort(portList[0]);
    builder.addSignal(sig1);
    builder.addSignal(sig2);
    builder.setValueReadType(SampleType::Int16);
    builder.setDomainReadType(SampleType::Int16);
    builder.setReadMode(ReadMode::RawValue);
    builder.setReadTimeoutType(ReadTimeoutType::Any);
    builder.setRequiredCommonSampleRate(0);
    builder.setStartOnFullUnitOfDomain(true);

    ASSERT_EQ(builder.getSourceComponents().getCount(), 3u);
    ASSERT_EQ(builder.getSourceComponents()[0].asPtr<IInputPort>().getSignal(), nullptr);
    ASSERT_EQ(builder.getSourceComponents()[1], sig1);
    ASSERT_EQ(builder.getSourceComponents()[2], sig2);

    ASSERT_EQ(builder.getValueReadType(), SampleType::Int16);
    ASSERT_EQ(builder.getDomainReadType(), SampleType::Int16);
    ASSERT_EQ(builder.getReadMode(), ReadMode::RawValue);
    ASSERT_EQ(builder.getReadTimeoutType(), ReadTimeoutType::Any);
    ASSERT_EQ(builder.getRequiredCommonSampleRate(), 0);
    ASSERT_EQ(builder.getStartOnFullUnitOfDomain(), true);
}

TEST_F(MultiReaderTest, MultiReaderBuilderWithDifferentInputs)
{
    readSignals.reserve(3);

    auto sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:04+00:00", Ratio(1, 1000 * 10ll), LinearDataRule(10, 0)));
    auto sig2 = addSignal(0, 843, createDomainSignal("2022-09-27T00:02:04.123+00:00"));

    auto portList = portsList();

    SignalPtr signal1 = sig1.signal;
    SignalPtr signal2 = sig2.signal;

    MultiReaderBuilderPtr builder = MultiReaderBuilder().addInputPort(portList[0]).addSignal(signal1).addSignal(signal2);
    ASSERT_THROW(MultiReaderFromBuilder(builder), InvalidParameterException);
}

TEST_F(MultiReaderTest, MultiReaderBuilderFromSignalsTimeouts)
{
    using namespace std::chrono_literals;
    readSignals.reserve(3);

    auto sig0 = addSignal(0, 10, createDomainSignal());
    auto sig1 = addSignal(0, 10, createDomainSignal());
    auto sig2 = addSignal(0, 10, createDomainSignal());

    SignalPtr signal0 = sig0.signal;
    SignalPtr signal1 = sig1.signal;
    SignalPtr signal2 = sig2.signal;

    MultiReaderBuilderPtr builder = MultiReaderBuilder().addSignal(signal0).addSignal(signal1).addSignal(signal2)
                                                        .setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    auto multireader = builder.build();

    using Type = SampleTypeToType<SampleType::Float64>::Type;
    Type sig0Samples[10];
    Type sig1Samples[10];
    Type sig2Samples[10];

    void* samples[3] = {sig0Samples, sig1Samples, sig2Samples};
    auto count = SizeT{0};
    auto status = multireader.read(samples, &count, 1000);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    auto start = std::chrono::steady_clock::now();
    auto sendThread = std::thread([&sig0, &sig1, &sig2]() {
        std::this_thread::sleep_for(1s);
        sig0.createAndSendPacket(0);
        sig1.createAndSendPacket(0);
        sig2.createAndSendPacket(0);
    });
    count = SizeT(sig0.packetSize);
    status = multireader.read(samples, &count, 10000);
    auto end = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, 10u);
    ASSERT_LT(diff, 5000);

    sendThread.join();
}

TEST_F(MultiReaderTest, MultiReaderTimeoutWhenDataAvailable)
{
    using namespace std::chrono_literals;
    readSignals.reserve(3);

    auto sig0 = addSignal(0, 10, createDomainSignal());
    auto sig1 = addSignal(0, 10, createDomainSignal());
    auto sig2 = addSignal(0, 10, createDomainSignal());

    auto multireader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    using Type = SampleTypeToType<SampleType::Float64>::Type;
    Type sig0Samples[10];
    Type sig1Samples[10];
    Type sig2Samples[10];

    void* samples[3] = {sig0Samples, sig1Samples, sig2Samples};
    auto count = SizeT{0};
    auto start = std::chrono::steady_clock::now();
    auto status = multireader.read(samples, &count, 10000);
    auto end = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_LT(diff, 5000);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    std::this_thread::sleep_for(1s);
    ASSERT_EQ(multireader.getAvailableCount(), 10u);

    count = SizeT(sig0.packetSize);
    start = std::chrono::steady_clock::now();
    status = multireader.read(samples, &count, 10000);
    end = std::chrono::steady_clock::now();
    diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Diff = " << diff << std::endl;
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, 10u);
    ASSERT_LT(diff, 5000);
}

TEST_F(MultiReaderTest, MultiReaderExceptionOnConstructor)
{
    readSignals.reserve(1);

    auto& sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00", nullptr, LinearDataRule(1, 0)));  // 1000 Hz

    MultiReaderPtr mr;
    try
    {
        mr = MultiReaderBuilder()
             .addSignals(signalsToList())
             .setValueReadType(SampleType::Float64)
             .setDomainReadType(SampleType::Int64)
             .setReadMode(ReadMode::Scaled)
             .setReadTimeoutType(ReadTimeoutType::All)
             .setRequiredCommonSampleRate(500)
             .setStartOnFullUnitOfDomain(false)
             .build();
    }
    catch (...)
    {
    }

    for (Int i = 0; i < 5; i++)
    {
        sig0.createAndSendPacket(i);
    }
}

TEST_F(MultiReaderTest, MultiReaderTimeoutChecking)
{
    readSignals.reserve(2);

    auto sig0 = addSignal(0, 523, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto sig1 = addSignal(0, 732, createDomainSignal("2022-09-27T00:02:03+00:00"));

    const MultiReaderPtr multiReader = MultiReaderBuilder()
                                           .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                                           .addSignals(signalsToList())
                                           .setValueReadType(SampleType::Float64)
                                           .setDomainReadType(SampleType::Int64)
                                           .build();

    {
        SizeT count{0};
        auto status = multiReader.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    constexpr size_t numberOfSamplesToRead = 8;
    double dataFirstSignal[numberOfSamplesToRead];
    double dataSecondSignal[numberOfSamplesToRead];
    double* data[2]{dataFirstSignal, dataSecondSignal};

    size_t count = numberOfSamplesToRead;
    auto a1 = std::async(std::launch::async, [&] { multiReader.read(data, &count, 10000); });

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);

    a1.wait();

    ASSERT_EQ(count, numberOfSamplesToRead);
}

TEST_F(MultiReaderTest, DISABLED_MultiReaderGapDetection)
{
    constexpr const auto NUM_SIGNALS = 2;
    readSignals.reserve(NUM_SIGNALS);

    // time different between the two signals is 0.01s which is 10 samples
    auto& sig0 = addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00", nullptr, LinearDataRule(1, 0)));
    auto& sig1 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03.01+00:00", nullptr, LinearDataRule(1, 0)));

    auto ports = portsList(true);
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        ports[i].connect(signals[i]);

    SizeT toRead = 0u;
    MultiReaderStatusPtr status = multi.read(nullptr, &toRead);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    // for signal 0 writes first packet with 10 samples, and then second packet with 10 samples and offset of 5
    // in signal will be generated 3 packets
    // data packet with 10 samples
    // event packet with gap in 5 samples
    // data packet with 10 samples
    sig0.createAndSendPacket(0);
    sig0.packetOffset = 5;
    sig0.createAndSendPacket(1);

    // for signal 1 - write 20 samples
    // in signal will be generated 1 packet with 20 samples
    sig1.createAndSendPacket(0);
    sig1.signal.getContext().getScheduler().waitAll();

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    SizeT count{0};
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_TRUE(status.getEventPackets().assigned());
    ASSERT_EQ(status.getEventPackets().getCount(), 1u);
    ASSERT_TRUE(status.getEventPackets().hasKey("/readsig0"));

    auto event = status.getEventPackets().get("/readsig0");
    ASSERT_EQ(event.getEventId(), event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED);
    ASSERT_EQ(event.getParameters().get(event_packet_param::GAP_DIFF), 5);

    constexpr const SizeT SAMPLES = 10;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1]};

    count = SAMPLES;
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    // bacause the was no shifting in time
    ASSERT_EQ(count, 10u);
}

TEST_F(MultiReaderTest, ReadWhenOnePortIsNotConnected)
{
    constexpr const auto NUM_SIGNALS = 3;
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 40, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();

    portList[0].connect(sig0.signal);
    portList[1].connect(sig1.signal);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    constexpr const SizeT SAMPLES = 10u;
    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};

    // check that we read 0 samples as one of the ports is not connected (Preparing: nothing
    // is wrong, but no data can be expected until the port connects)
    SizeT count{SAMPLES};
    MultiReaderStatusPtr status = multi.read(valuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
    ASSERT_EQ(count, 0u);

    // check reading with timeout
    count = SAMPLES;
    status = multi.read(valuesPerSignal, &count, 100u);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
    ASSERT_EQ(count, 0u);

    // connect signal to the port
    portList[2].connect(sig2.signal);
    sig2.createAndSendPacket(0);

    // first packet on sig2 will be the event packet
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(status.getEventPackets().getCount(), 3u);

    // Behavior change (spec 6.2/8.5): data queued before/alongside the events is preserved
    // and readable right after the events are consumed - the old reader silently dropped it
    count = SAMPLES;
    status = multi.read(valuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, 10u);

    count = SAMPLES;
    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    status = multi.read(valuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, 10u);
}

TEST_F(MultiReaderTest, NotifyPortIsConnected)
{
    constexpr const auto NUM_SIGNALS = 3;
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 40, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    portList[0].connect(sig0.signal);
    portList[1].connect(sig1.signal);

    MultiReaderStatusPtr status;

    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    multi.setOnDataAvailable(
        [&]
        {
            SizeT count{0};
            status = multi.read(nullptr, &count);
            promise.set_value();
        });

    portList[2].connect(sig2.signal);

    auto promiseStatus = future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(promiseStatus, std::future_status::ready);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(status.getEventPackets().getCount(), 3u);
}

TEST_F(MultiReaderTest, ReadWhilePortIsNotConnected)
{
    constexpr const auto NUM_SIGNALS = 3;
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 40, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    portList[0].connect(sig0.signal);
    portList[1].connect(sig1.signal);

    MultiReaderStatusPtr status;

    std::future<void> future = std::async(std::launch::async,
                                          [&]
                                          {
                                              SizeT count{0};
                                              status = multi.read(nullptr, &count, 1000u);
                                          });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    portList[2].connect(sig2.signal);

    future.wait();
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(status.getEventPackets().getCount(), 3u);
}

TEST_F_UNSTABLE_SKIPPED(MultiReaderTest, ReconnectWhileReading)
{
    constexpr const auto NUM_SIGNALS = 3;
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList[i].connect(readSignals[i].signal);

    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    constexpr const SizeT SAMPLES = 20u;
    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};

    SizeT count{SAMPLES};
    MultiReaderStatusPtr status;
    std::future<void> future = std::async(std::launch::async,
                                          [&]
                                          {
                                              SizeT tmpCnt{0};
                                              status = multi.read(nullptr, &tmpCnt, 500);
                                              multi.read(valuesPerSignal, &count, 300);
                                          });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sig0.createAndSendPacket(0);
    sig0.signal.getContext().getScheduler().waitAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    portList[0].disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    portList[0].connect(sig0.signal);

    future.wait();
    ASSERT_EQ(count, 10u);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(status.getEventPackets().getCount(), 3u);
    ASSERT_TRUE(status.getEventPackets().hasKey("/readsig0"));
}

class MockSignal
{
public:
    MockSignal(const ContextPtr& context, const StringPtr& id, const StringPtr& epoch)
    {
        signal = daq::Signal(context, nullptr, id + "_valueSignal");
        domainSignal = daq::Signal(context, nullptr, id + "_domainSignal");

        auto valueDescriptor = daq::DataDescriptorBuilder()
                                   .setSampleType(daq::SampleType::Float64)
                                   .setUnit(Unit("V", -1, "volts", "voltage"))
                                   .setName(id + " values")
                                   .build();
        auto domainDescriptor = daq::DataDescriptorBuilder()
                                    .setSampleType(daq::SampleType::Int64)
                                    .setUnit(daq::Unit("s", -1, "seconds", "time"))
                                    .setTickResolution(daq::Ratio(1, 1000))
                                    .setRule(daq::LinearDataRule(1, 0))
                                    .setOrigin(epoch)
                                    .setName(id + " time")
                                    .build();

        signal->setDescriptor(valueDescriptor);
        domainSignal->setDescriptor(domainDescriptor);
        signal->setDomainSignal(domainSignal);
    }
    SignalConfigPtr signal;
    SignalConfigPtr domainSignal;
};

TEST_F(MultiReaderTest, UndefinedReadWithMockSignals)
{
    StringPtr epoch = "2022-09-27T00:02:03+00:00";
    auto sig1 = MockSignal(context, "sig1", epoch);
    auto sig2 = MockSignal(context, "sig2", epoch);

    auto readerBuilder = MultiReaderBuilder();
    readerBuilder.setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    readerBuilder.addSignal(sig1.signal);
    readerBuilder.addSignal(sig2.signal);
    readerBuilder.setValueReadType(SampleType::Undefined);
    readerBuilder.setDomainReadType(SampleType::Int64);
    ASSERT_NO_THROW(readerBuilder.build());

    auto signalList = List<SignalPtr>(sig1.signal, sig2.signal);
    ASSERT_NO_THROW(MultiReader(signalList, SampleType::Undefined, SampleType::Int64));
}

TEST_F(MultiReaderTest, MultiReaderActive)
{
    constexpr auto NUM_SIGNALS = SizeT{3};
    constexpr auto NUM_SAMPLES = SizeT{10};
    double values[NUM_SIGNALS][NUM_SAMPLES] = {};
    double* valuesPerSignal[NUM_SIGNALS] = {values[0], values[1], values[2]};
    int64_t domainValues[NUM_SIGNALS][NUM_SAMPLES] = {};
    int64_t* domainValuesPerSignal[NUM_SIGNALS] = {domainValues[0], domainValues[1], domainValues[2]};
    auto count = SizeT{0};
    auto packetIndex = SizeT{0};

    readSignals.reserve(NUM_SIGNALS);

    auto signalReader = addSignal(0, NUM_SAMPLES, createDomainSignal());
    addSignal(0, NUM_SAMPLES, createDomainSignal());
    addSignal(0, NUM_SAMPLES, createDomainSignal());

    auto portList = portsList();
    auto multiReader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    auto status = daq::MultiReaderStatusPtr();

    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList[i].connect(readSignals[i].signal);

    // check active status
    bool isActive = multiReader.getActive();
    ASSERT_TRUE(isActive);

    // send packets to active reader
    sendPackets(packetIndex++);  // 0

    // receive event packets
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);

    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Event);
    ASSERT_EQ(count, 0u);

    // receive data packets
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Ok);
    ASSERT_EQ(count, NUM_SAMPLES);

    multiReader.setActive(false);

    // send packets to inactive reader: the deliberate deactivation is reported as Inactive
    sendPackets(packetIndex++);  // 1
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);

    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Inactive);
    ASSERT_EQ(count, 0u);

    // send packets to inactive reader
    sendPackets(packetIndex++);  // 2
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);

    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Inactive);
    ASSERT_EQ(count, 0u);

    // send event packet
    auto signal = signalReader.signal;
    auto domainSignal = signal.getDomainSignal().asPtrOrNull<ISignalConfig>();

    ASSERT_TRUE(domainSignal.assigned());

    auto dataDescriptor = signal.getDescriptor();
    auto domainDescriptor = domainSignal.getDescriptor();
    auto newDomainDescriptor = DataDescriptorBuilderCopy(domainDescriptor).setSampleType(daq::SampleType::Int32).build();
    domainSignal.setDescriptor(newDomainDescriptor);

    // send packets to inactive reader
    sendPackets(packetIndex++);  // 3
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);

    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Event);
    ASSERT_EQ(count, 0u);

    // set multireader active again
    multiReader.setActive(true);

    sendPackets(packetIndex++);  // 4
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);

    ASSERT_EQ(status.getReadStatus(), daq::ReadStatus::Ok);
    ASSERT_EQ(count, NUM_SAMPLES);
}

// The MultiReaderActiveCopyInactive test was deleted (test plan Part C): MultiReaderFromExisting
// is removed by the rework (spec 8.4) - recovery happens in the same reader instance.

TEST_F(MultiReaderTest, MultiReaderActiveFromPorts)
{
    using namespace std::chrono_literals;

    constexpr auto NUM_SIGNALS = SizeT{3};
    constexpr auto NUM_SAMPLES = SizeT{10};

    readSignals.reserve(NUM_SIGNALS);

    auto signalReader = addSignal(0, NUM_SAMPLES, createDomainSignal());
    addSignal(0, NUM_SAMPLES, createDomainSignal());
    addSignal(0, NUM_SAMPLES, createDomainSignal());

    auto portList = portsList();
    auto multiReader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    auto status = daq::MultiReaderStatusPtr();

    multiReader.setActive(false);

    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList[i].connect(readSignals[i].signal);

    ASSERT_FALSE(multiReader.getActive());
    for (const auto& port : portList)
        ASSERT_FALSE(port.getActive());
}

TEST_F(MultiReaderTest, MultiReaderActiveGapPacket)
{
    using namespace std::chrono_literals;

    constexpr auto NUM_SIGNALS = SizeT{3};
    constexpr auto NUM_SAMPLES = SizeT{10};
    double values[NUM_SIGNALS][NUM_SAMPLES] = {};
    double* valuesPerSignal[NUM_SIGNALS] = {values[0], values[1], values[2]};
    int64_t domainValues[NUM_SIGNALS][NUM_SAMPLES] = {};
    int64_t* domainValuesPerSignal[NUM_SIGNALS] = {domainValues[0], domainValues[1], domainValues[2]};
    auto count = SizeT{0};
    auto packetIndex = SizeT{0};

    readSignals.reserve(NUM_SIGNALS);

    auto signalReader0 = addSignal(0, NUM_SAMPLES, createDomainSignal());
    auto signalReader1 = addSignal(0, NUM_SAMPLES, createDomainSignal());
    auto signalReader2 = addSignal(0, NUM_SAMPLES, createDomainSignal());

    auto portList = portsList(true);
    auto multiReader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();
    auto status = daq::MultiReaderStatusPtr();

    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList[i].connect(readSignals[i].signal);

    // 0
    signalReader0.createAndSendPacket(packetIndex);
    signalReader1.createAndSendPacket(packetIndex);
    signalReader2.createAndSendPacket(packetIndex);

    // 1
    packetIndex += 1;
    // signalReader0.createAndSendPacket(packetIndex); // gap
    signalReader1.createAndSendPacket(packetIndex);
    signalReader2.createAndSendPacket(packetIndex);

    // 2
    packetIndex += 1;
    signalReader0.createAndSendPacket(packetIndex);
    signalReader1.createAndSendPacket(packetIndex);
    signalReader2.createAndSendPacket(packetIndex);

    // read first event packets
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(count, 0u);

    // 10 samples until gap event
    ASSERT_EQ(multiReader.getAvailableCount(), 10u);

    multiReader.setActive(false);

    ASSERT_EQ(multiReader.getAvailableCount(), 0u);

    // read nothing, gap packet was dropped; the deactivated reader reports Inactive
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Inactive);
    ASSERT_EQ(count, 0u);

    // change descriptor
    auto signal = signalReader0.signal;
    auto domainSignal = signal.getDomainSignal().asPtrOrNull<ISignalConfig>();
    auto dataDescriptor = signal.getDescriptor();
    auto domainDescriptor = domainSignal.getDescriptor();
    auto newDomainDescriptor = DataDescriptorBuilderCopy(domainDescriptor).setSampleType(daq::SampleType::Int32).build();
    domainSignal.setDescriptor(newDomainDescriptor);

    // read descriptor changed event
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(count, 0u);

    // set active again
    multiReader.setActive(true);

    // send data
    packetIndex += 1;
    signalReader0.createAndSendPacket(packetIndex);
    signalReader1.createAndSendPacket(packetIndex);
    signalReader2.createAndSendPacket(packetIndex);

    // read data
    count = NUM_SAMPLES;
    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, NUM_SAMPLES);
}

TEST_F(MultiReaderTest, MultiReaderActiveDataAvailableCallback)
{
    using namespace std::chrono_literals;

    constexpr auto NUM_SIGNALS = SizeT{3};
    constexpr auto NUM_SAMPLES = SizeT{10};
    double values[NUM_SIGNALS][NUM_SAMPLES] = {};
    double* valuesPerSignal[NUM_SIGNALS] = {values[0], values[1], values[2]};
    int64_t domainValues[NUM_SIGNALS][NUM_SAMPLES] = {};
    int64_t* domainValuesPerSignal[NUM_SIGNALS] = {domainValues[0], domainValues[1], domainValues[2]};

    readSignals.reserve(NUM_SIGNALS);

    auto signalReader0 = addSignal(0, NUM_SAMPLES, createDomainSignal());
    auto signalReader1 = addSignal(0, NUM_SAMPLES, createDomainSignal());
    auto signalReader2 = addSignal(0, NUM_SAMPLES, createDomainSignal());

    auto portList = portsList(true);
    auto multiReader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::Scheduler).addInputPorts(portList).build();
    auto status = daq::MultiReaderStatusPtr();

    auto changeDomainSampleType = [](const ReadSignal& signalReader, SampleType newSampleType)
    {
        auto signal = signalReader.signal;
        auto domainSignal = signal.getDomainSignal().asPtrOrNull<ISignalConfig>();
        auto dataDescriptor = signal.getDescriptor();
        auto domainDescriptor = domainSignal.getDescriptor();
        auto newDomainDescriptor = DataDescriptorBuilderCopy(domainDescriptor).setSampleType(newSampleType).build();
        domainSignal.setDescriptor(newDomainDescriptor);
    };

    auto notified = false;
    auto state = 0;
    auto cv = std::condition_variable{};
    auto m = std::mutex{};

    multiReader.setOnDataAvailable(
        [this,
         &multiReader,
         &cv,
         &m,
         &notified,
         &state,
         &valuesPerSignal,
         &domainValuesPerSignal,
         NUM_SAMPLES]
        {
            auto lck = std::unique_lock{m};
            ASSERT_FALSE(notified);

            switch (state)
            {
                case 0:
                {
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
                    ASSERT_EQ(count, 0u);
                    auto events = status.getEventPackets();
                    for (auto i = SizeT{0}; i < events.getCount(); ++i)
                    {
                        auto sigId = fmt::format("/readsig{}", i);
                        ASSERT_TRUE(events.hasKey(sigId));
                        auto eventPacket = events.get(sigId);
                        ASSERT_EQ(eventPacket.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
                        ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DATA_DESCRIPTOR));
                        ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DOMAIN_DATA_DESCRIPTOR));
                        auto domainDescriptor =
                            eventPacket.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR).asPtrOrNull<IDataDescriptor>();
                        ASSERT_TRUE(domainDescriptor.assigned());
                        ASSERT_EQ(domainDescriptor.getSampleType(), SampleTypeFromType<ClockTick>::SampleType);
                    }

                    state = 1;
                    break;
                }
                case 1:
                {
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
                    ASSERT_EQ(count, NUM_SAMPLES);

                    state = 2;
                    break;
                }
                case 2:
                {
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
                    ASSERT_EQ(count, 0u);
                    ASSERT_EQ(multiReader.getAvailableCount(), 10u);

                    auto events = status.getEventPackets();
                    ASSERT_GE(events.getCount(), 1u);

                    auto sigId = fmt::format("/readsig{}", 0);
                    ASSERT_TRUE(events.hasKey(sigId));

                    auto eventPacket = events.get(sigId);
                    ASSERT_EQ(eventPacket.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DATA_DESCRIPTOR));
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DOMAIN_DATA_DESCRIPTOR));

                    auto domainDescriptor =
                        eventPacket.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR).asPtrOrNull<IDataDescriptor>();
                    ASSERT_TRUE(domainDescriptor.assigned());
                    ASSERT_EQ(domainDescriptor.getSampleType(), SampleType::UInt64);

                    multiReader.setActive(false);

                    state = 3;
                    break;
                }
                case 3:
                {
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
                    ASSERT_EQ(count, 0u);
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);

                    auto events = status.getEventPackets();
                    ASSERT_GE(events.getCount(), 1u);

                    auto sigId = fmt::format("/readsig{}", 0);
                    ASSERT_TRUE(events.hasKey(sigId));

                    auto eventPacket = events.get(sigId);
                    ASSERT_EQ(eventPacket.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DATA_DESCRIPTOR));
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DOMAIN_DATA_DESCRIPTOR));

                    auto domainDescriptor =
                        eventPacket.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR).asPtrOrNull<IDataDescriptor>();
                    ASSERT_TRUE(domainDescriptor.assigned());
                    ASSERT_EQ(domainDescriptor.getSampleType(), SampleType::Float64);

                    state = 4;
                    break;
                }
                case 4:
                {
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
                    ASSERT_EQ(count, 0u);
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);

                    auto events = status.getEventPackets();
                    ASSERT_GE(events.getCount(), 1u);

                    auto sigId = fmt::format("/readsig{}", 0);
                    ASSERT_TRUE(events.hasKey(sigId));

                    auto eventPacket = events.get(sigId);
                    ASSERT_EQ(eventPacket.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DATA_DESCRIPTOR));
                    ASSERT_TRUE(eventPacket.getParameters().hasKey(event_packet_param::DOMAIN_DATA_DESCRIPTOR));

                    auto domainDescriptor =
                        eventPacket.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR).asPtrOrNull<IDataDescriptor>();
                    ASSERT_TRUE(domainDescriptor.assigned());
                    ASSERT_EQ(domainDescriptor.getSampleType(), SampleType::Int64);

                    multiReader.setActive(true);

                    state = 5;
                    break;
                }
                case 5:
                {
                    ASSERT_EQ(multiReader.getAvailableCount(), 0u);
                    auto count = NUM_SAMPLES;
                    auto status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
                    ASSERT_EQ(count, 0u);

                    count = NUM_SAMPLES;
                    status = multiReader.readWithDomain(valuesPerSignal, domainValuesPerSignal, &count);
                    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
                    ASSERT_EQ(count, NUM_SAMPLES);

                    state = 6;
                    break;
                }
                default:
                {
                    GTEST_FAIL();
                }
            }
            notified = true;
            cv.notify_all();
        });

    for (size_t i = 0; i < NUM_SIGNALS; i++)
        portList[i].connect(readSignals[i].signal);

    while (state != 6)
    {
        auto lck = std::unique_lock{m};
        auto cv_status = cv.wait_for(lck, 10s, [&notified] { return notified; });
        ASSERT_TRUE(cv_status);
        notified = false;

        switch (state)
        {
            case 1:
                signalReader0.createAndSendPacket(0);
                signalReader1.createAndSendPacket(0);
                signalReader2.createAndSendPacket(0);
                break;
            case 2:
                changeDomainSampleType(signalReader0, SampleType::UInt64);
                signalReader0.createAndSendPacket(1);
                signalReader1.createAndSendPacket(1);
                signalReader2.createAndSendPacket(1);
                break;
            case 3:
                changeDomainSampleType(signalReader0, SampleType::Float64);
                signalReader0.createAndSendPacket(2);
                signalReader1.createAndSendPacket(2);
                signalReader2.createAndSendPacket(2);
                break;
            case 4:
                changeDomainSampleType(signalReader0, SampleType::Int64);
                break;
            case 5:
                signalReader0.createAndSendPacket(3);
                signalReader1.createAndSendPacket(3);
                signalReader2.createAndSendPacket(3);
                break;
            case 6:
                // finish
                break;
            default:
                GTEST_FAIL();
                break;
        }
    }

    context.getScheduler().waitAll();

    ASSERT_EQ(state, 6);
}

TEST_F(MultiReaderTest, ExpectSR)
{
    const auto ctx = NullContext();

    auto valueDesc = DataDescriptorBuilder().setSampleType(SampleType::Int32).build();
    auto timeDesc = DataDescriptorBuilder()
                        .setSampleType(SampleType::Int64)
                        .setRule(LinearDataRule(0, 10))
                        .setTickResolution(Ratio(1, 1000))
                        .setUnit(Unit("s", -1, "seconds", "time"))
                        .build();

    const auto valueSignal = SignalWithDescriptor(ctx, valueDesc, nullptr, "value");
    const auto timeSignal = SignalWithDescriptor(ctx, timeDesc, nullptr, "time");
    valueSignal.setDomainSignal(timeSignal);

    const auto reader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignal(valueSignal).
                                             setDomainReadType(SampleType::Int64).setValueReadType(SampleType::Int32).
                                             setRequiredCommonSampleRate(10).build();

    size_t count = 0;
    auto status = reader.read(nullptr, &count, 0);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    count = 0;
    status = reader.read(nullptr, &count, 0);
    // A required-rate mismatch is a recoverable per-input failure, not a reader failure
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(status.getInputStates().get(valueSignal.getGlobalId()))),
              InputState::Incompatible);
}

TEST_F(MultiReaderTest, TestReaderWithConnectedPortConnectionEmpty)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const SizeT SAMPLES = 4u;

    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 40, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    portList[0].connect(sig0.signal);
    portList[1].connect(sig1.signal);
    portList[2].connect(sig2.signal);

    for (const auto & port : portList)
    {
        auto connection = port.getConnection();
        ASSERT_TRUE(connection.assigned());

        SizeT packetInConnection = 0;
        while (true)
        {
            if (connection.dequeue().assigned())
                packetInConnection++;
            else
                break;
        }

        // 1 event packet
        ASSERT_EQ(packetInConnection, 1u);
    }
    auto reader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();

    {
        SizeT count{0};
        auto status = reader.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_EQ(status.getEventPackets().getCount(), 3u);
    }

    for (const auto & port : portList)
    {
        auto signal = port.getSignal().asPtr<ISignalConfig>(true);
        ASSERT_TRUE(signal.assigned());

        auto domainPacket = DataPacket(setupDescriptor(SampleType::RangeInt64, LinearDataRule(1, 0), nullptr), SAMPLES, 1);
        auto dataPacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), SAMPLES);
        auto dataPtr = static_cast<double*>(dataPacket.getData());
        for (SizeT i = 0; i < SAMPLES; i++)
            dataPtr[i] = 111.1 * (i + 1);

        signal.sendPacket(dataPacket);
    }

    {
        std::array<double[SAMPLES], NUM_SIGNALS> values{};
        void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};

        SizeT count{SAMPLES};
        auto status = reader.read(&valuesPerSignal, &count, 500);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);

        ASSERT_EQ(count, SAMPLES);
        for (SizeT i = 0; i < SAMPLES; i++)
        {
            ASSERT_EQ(values[0][i], 111.1 * (i + 1));
            ASSERT_EQ(values[1][i], 111.1 * (i + 1));
            ASSERT_EQ(values[2][i], 111.1 * (i + 1));
        }
    }
}

TEST_F(MultiReaderTest, TestReaderWithConnectedPortConnectionNotEmpty)
{
    constexpr const auto NUM_SIGNALS = 3;
    constexpr const SizeT SAMPLES = 4u;

    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 20, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 30, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 40, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto portList = portsList();
    portList[0].connect(sig0.signal);
    portList[1].connect(sig1.signal);
    portList[2].connect(sig2.signal);

    for (const auto & port : portList)
    {
        auto connection = port.getConnection();
        ASSERT_TRUE(connection.assigned());

        SizeT packetInConnection = 0;
        while (true)
        {
            if (connection.dequeue().assigned())
                packetInConnection++;
            else
                break;
        }

        // 1 event packet
        ASSERT_EQ(packetInConnection, 1u);
    }

    for (const auto & port : portList)
    {
        auto signal = port.getSignal().asPtr<ISignalConfig>(true);
        ASSERT_TRUE(signal.assigned());

        auto domainPacket = DataPacket(setupDescriptor(SampleType::RangeInt64, LinearDataRule(1, 0), nullptr), SAMPLES, 1);
        auto dataPacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), SAMPLES);
        auto dataPtr = static_cast<double*>(dataPacket.getData());
        for (SizeT i = 0; i < SAMPLES; i++)
            dataPtr[i] = 111.1 * (i + 1);

        signal.sendPacket(dataPacket);
    }

    auto reader = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(portList).build();

    {
        SizeT count{0};
        auto status = reader.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_EQ(status.getEventPackets().getCount(), 3u);
    }

    {
        std::array<double[SAMPLES], NUM_SIGNALS> values{};
        void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};

        SizeT count{SAMPLES};
        auto status = reader.read(&valuesPerSignal, &count, 500);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);

        ASSERT_EQ(count, SAMPLES);
        for (SizeT i = 0; i < SAMPLES; i++)
        {
            ASSERT_EQ(values[0][i], 111.1 * (i + 1));
            ASSERT_EQ(values[1][i], 111.1 * (i + 1));
            ASSERT_EQ(values[2][i], 111.1 * (i + 1));
        }
    }
}

class MinReadCountTest : public MultiReaderTest, public testing::WithParamInterface<SizeT>
{
};

TEST_P(MinReadCountTest, MinReadCount)
{
    auto timeoutMs = GetParam();

    constexpr auto NUM_SIGNALS = 3;
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig1 = addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00"));
    auto& sig2 = addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00"));

    auto multi = MultiReaderBuilder().addSignal(sig0.signal).addSignal(sig1.signal).addSignal(sig2.signal).setMinReadCount(20).
                                      setInputPortNotificationMethod(PacketReadyNotification::SameThread).build();

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);
    sig2.createAndSendPacket(0);

    constexpr const SizeT SAMPLES = 20u;
    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1], values[2]};
    int64_t domain[SAMPLES];

    SizeT count{10};
    MultiReaderStatusPtr status;

    ASSERT_EQ(multi.getAvailableCount(), 0u);

    ASSERT_THROW(multi.read(valuesPerSignal, &count, timeoutMs), InvalidParameterException);
    count = 10;
    ASSERT_THROW(multi.skipSamples(&count), InvalidParameterException);
    count = 10;
    ASSERT_THROW(multi.readWithDomain(valuesPerSignal, domain, &count, timeoutMs), InvalidParameterException);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);
    sig2.createAndSendPacket(1);

    ASSERT_EQ(multi.getAvailableCount(), 0u);
    count = 0;
    status = multi.read(nullptr, &count, timeoutMs);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    ASSERT_EQ(multi.getAvailableCount(), 20u);

    count = 20;
    multi.read(valuesPerSignal, &count, timeoutMs);

    ASSERT_EQ(count, 20u);

    sig0.createAndSendPacket(2);
    sig1.createAndSendPacket(2);
    sig2.createAndSendPacket(2);

    ASSERT_EQ(multi.getAvailableCount(), 0u);

    sig0.setValueDescriptor(setupDescriptor(SampleType::Int32));

    ASSERT_EQ(multi.getAvailableCount(), 0u);

    // Behavior change (spec 6.2/3.1): the leftover segment shorter than minReadCount is
    // discarded during evaluation, so the descriptor event surfaces on the first read;
    // the old reader needed one extra read to drop the segment first
    count = 0;
    status = multi.read(nullptr, &count, timeoutMs);
    ASSERT_EQ(count, 0u);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    count = 0;
    status = multi.read(nullptr, &count, timeoutMs);
    // The event consumed the buffered data with it - the reader is waiting for fresh data
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
    ASSERT_EQ(count, 0u);
}

INSTANTIATE_TEST_SUITE_P(MinReadCountSuite, MinReadCountTest, testing::Values(0, 1000));

TEST_F(MultiReaderTest, TestTickOffsetExceeded)
{
    using namespace std::chrono_literals;
    constexpr auto kPacketSize = SizeT{10};
    constexpr auto kSignalCount = SizeT{10};
    const auto resolution = Ratio(1, 10);

    const auto okTolerance = Ratio(10, 100);
    const auto failTolerance = Ratio(8, 100);
    const auto boundTolerance = Ratio(9, 100);

    auto epoch = std::chrono::system_clock::now();
    auto dataBuffers = std::vector<void*>(kSignalCount, nullptr);
    auto domainBuffers = std::vector<void*>(kSignalCount, nullptr);

    for (SizeT i = 0; i < kSignalCount; ++i)
    {
        // auto epochString = reader::timePointString(epoch);
        auto epochString = date::format("%FT%T%z", epoch);
        auto domainSignal = createDomainSignal(epochString, resolution, LinearDataRule(1, 0));
        auto domainSampleSize = domainSignal.getDescriptor().getSampleSize();
        auto dataSignal = addSignal(0, kPacketSize, domainSignal);
        auto dataSampleSize = dataSignal.signal.getDescriptor().getSampleSize();
        domainBuffers[i] = std::calloc(kPacketSize, domainSampleSize);
        dataBuffers[i] = std::calloc(kPacketSize, dataSampleSize);
        epoch += 10ms;
    }

    auto multiReaderBuilder = MultiReaderBuilder();
    multiReaderBuilder.setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    auto ports = portsList();
    auto signals = signalsToList();
    ASSERT_EQ(ports.getCount(), signals.getCount());
    for (SizeT i = 0; i < ports.getCount(); ++i)
    {
        ports[i].connect(signals[i]);
        multiReaderBuilder.addInputPort(ports[i]);
    }
    multiReaderBuilder.setTickOffsetTolerance(failTolerance);

    auto multiReader = multiReaderBuilder.build();

    for (auto& signal: readSignals)
        signal.createAndSendPacket(0);

    auto count = SizeT{0};
    auto status = multiReader.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    // Behavior change (spec 8.4/8.5): tickOffsetTolerance is deprecated and ignored - the
    // sub-tick epoch offsets no longer fail synchronization, and a sync failure would no
    // longer deactivate the reader either. The read succeeds and the reader stays active.
    count = 10;
    status = multiReader.readWithDomain(dataBuffers.data(), domainBuffers.data(), &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_EQ(count, 10u);
    ASSERT_TRUE(multiReader.getActive());

    for (SizeT i = 0; i < kSignalCount; ++i)
    {
        std::free(domainBuffers[i]);
        std::free(dataBuffers[i]);
    }
}

TEST_F(MultiReaderTest, TestTickOffsetExceededByOffset)
{
    using namespace std::chrono_literals;
    constexpr auto kPacketSize = SizeT{10};
    constexpr auto kSignalCount = SizeT{2};
    const auto resolution = Ratio(1, 10);

    const auto okTolerance = Ratio(10, 100);
    const auto failTolerance = Ratio(1, 30);
    const auto boundTolerance = Ratio(9, 100);

    auto epoch = std::chrono::system_clock::now();
    auto dataBuffers = std::vector<void*>(kSignalCount, nullptr);
    auto domainBuffers = std::vector<void*>(kSignalCount, nullptr);

    auto epochString = date::format("%FT%T%z", epoch);
    auto start = SizeT{0};
    for (SizeT i = 0; i < kSignalCount; ++i)
    {
        // auto epochString = reader::timePointString(epoch);
        auto domainSignal = createDomainSignal(epochString, resolution, LinearDataRule(2, start++));
        auto domainSampleSize = domainSignal.getDescriptor().getSampleSize();
        auto dataSignal = addSignal(0, kPacketSize, domainSignal);
        auto dataSampleSize = dataSignal.signal.getDescriptor().getSampleSize();
        domainBuffers[i] = std::calloc(kPacketSize, domainSampleSize);
        dataBuffers[i] = std::calloc(kPacketSize, dataSampleSize);
    }

    auto multiReaderBuilder = MultiReaderBuilder();
    multiReaderBuilder.setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    auto ports = portsList();
    auto signals = signalsToList();
    ASSERT_EQ(ports.getCount(), signals.getCount());
    for (SizeT i = 0; i < ports.getCount(); ++i)
    {
        ports[i].connect(signals[i]);
        multiReaderBuilder.addInputPort(ports[i]);
    }
    multiReaderBuilder.setTickOffsetTolerance(failTolerance);

    auto multiReader = multiReaderBuilder.build();

    for (auto& signal: readSignals)
        signal.createAndSendPacket(0);

    auto count = SizeT{0};
    auto status = multiReader.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    // Behavior change (spec 5.7/8.4/8.5): tickOffsetTolerance is deprecated and ignored.
    // These delta-2 grids are phase-shifted by one tick and share no common grid point, so
    // the reader never synchronizes and returns no data - but a synchronization failure no
    // longer deactivates the reader.
    count = 10;
    status = multiReader.readWithDomain(dataBuffers.data(), domainBuffers.data(), &count);
    ASSERT_EQ(count, 0u);
    ASSERT_EQ(multiReader.getActive(), true);

    for (SizeT i = 0; i < kSignalCount; ++i)
    {
        std::free(domainBuffers[i]);
        std::free(dataBuffers[i]);
    }
}

TEST_F(MultiReaderTest, BuilderNotificationMethodsUnspecified)
{
    ListPtr<IInputPort> ports = List<IInputPort>();
    ListPtr<PacketReadyNotification> notifications = List<PacketReadyNotification>();
    for (int i = 0; i < 10; ++i)
    {
        auto port = InputPort(context, nullptr, "ip");
        port.setNotificationMethod(PacketReadyNotification::SameThread);
        ports.pushBack(port);

        notifications.pushBack(static_cast<int>(PacketReadyNotification::Unspecified));
    }

    auto builder = MultiReaderBuilder().setInputPortNotificationMethods(notifications).addInputPorts(ports);

    ReaderConfigPtr reader = builder.build();

    for (const auto& port : reader.getInputPorts())
        ASSERT_EQ(port.getNotificationMethod(), PacketReadyNotification::SameThread);
}

TEST_F(MultiReaderTest, BuilderNotificationMethodDefault)
{
    ListPtr<IInputPort> ports = List<IInputPort>();
    for (int i = 0; i < 10; ++i)
    {
        auto port = InputPort(context, nullptr, "ip");
        ports.pushBack(port);
    }

    auto builder = MultiReaderBuilder().addInputPorts(ports);

    ReaderConfigPtr reader = builder.build();

    for (const auto& port : reader.getInputPorts())
        ASSERT_EQ(port.getNotificationMethod(), PacketReadyNotification::SameThread);
}


TEST_F(MultiReaderTest, BuilderNotificationMethodsOverride)
{
    ListPtr<IInputPort> ports = List<IInputPort>();
    ListPtr<PacketReadyNotification> notifications = List<PacketReadyNotification>();
    for (int i = 0; i < 10; ++i)
    {
        auto port = InputPort(context, nullptr, "ip");
        port.setNotificationMethod(PacketReadyNotification::SameThread);
        ports.pushBack(port);

        notifications.pushBack(static_cast<int>(PacketReadyNotification::Scheduler));
    }

    auto builder = MultiReaderBuilder().setInputPortNotificationMethods(notifications).addInputPorts(ports);

    ReaderConfigPtr reader = builder.build();

    for (const auto& port : reader.getInputPorts())
        ASSERT_EQ(port.getNotificationMethod(), PacketReadyNotification::Scheduler);
}

TEST_F(MultiReaderTest, OffsetToLinear)
{
    constexpr const auto NUM_SIGNALS = 2;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(3);

    std::string epochStr = "2022-09-27T00:02:04+00:00";
    // same resolution, try to achieve mismatch with rule
    auto nativeResolution = Ratio(1, 1000);
    auto nativeRule = LinearDataRule(4, 0);

    auto commonResolution = Ratio(1, 1000);
    auto commonRule = LinearDataRule(2, 0);

    auto& sig0 = addSignal(4, 4, createDomainSignal(epochStr, nativeResolution, nativeRule));
    auto& sig1 = addSignal(17, 2, createDomainSignal(epochStr, commonResolution, commonRule));

    auto ports = portsList();
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    for (size_t i = 0; i < NUM_SIGNALS; i++)
        ports[i].connect(signals[i]);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);  // 4, 8, 12, 16
    sig0.createAndSendPacket(1);  // 20, 24, 28, 32

    sig1.createAndSendPacket(0);  // 17, 19
    sig1.createAndSendPacket(1);  // 21, 23
    sig1.createAndSendPacket(2);  // 25, 27
    sig1.createAndSendPacket(3);  // 29, 31
    sig1.createAndSendPacket(4);  // 33, 35

    // Latest start is 17, which gets rounded to 20 as the first sample all sample rates have in common
    // (the fact they are misaligned is not taken into account).
    // For sig 0, tick=20 is found in the second packet with index 0 (exact match)
    // For sig 1, tick=20 is between packets, but is found as second packet, index 0 (>= match)

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 8u);

    constexpr const SizeT SAMPLES = 8u;

    std::array<double[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);

    ASSERT_EQ(domain[0][0], 20);
    ASSERT_EQ(domain[0][1], 24);
    ASSERT_EQ(domain[0][2], 28);

    ASSERT_EQ(domain[1][0], 21);
    ASSERT_EQ(domain[1][1], 23);
    ASSERT_EQ(domain[1][2], 25);
}

TEST_F(MultiReaderTest, UndefinedValueType)
{
    constexpr const auto NUM_SIGNALS = 2;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(NUM_SIGNALS);

    auto& sig0 = addSignal(0, 10, createDomainSignal("2022-09-27T00:00:00+00:00"), SampleType::Int64);
    auto& sig1 = addSignal(0, 10, createDomainSignal("2022-09-27T00:00:00+00:00"), SampleType::Int64);

    auto builder = MultiReaderBuilder();
    builder.setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    builder.addSignals(signalsToList());
    builder.setValueReadType(SampleType::Invalid);

    auto multi = builder.build();

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_EQ(status.getMainDescriptor().getEventId(), "DATA_DESCRIPTOR_CHANGED");
    }

    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);

    sig0.createAndSendPacket(1);
    sig1.createAndSendPacket(1);

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 20u);

    constexpr const SizeT SAMPLES = 10u;

    std::array<int64_t[SAMPLES], NUM_SIGNALS> values{};
    std::array<ClockTick[SAMPLES], NUM_SIGNALS> domain{};

    void* valuesPerSignal[NUM_SIGNALS]{values[0], values[1]};
    void* domainPerSignal[NUM_SIGNALS]{domain[0], domain[1]};

    SizeT count{SAMPLES};
    multi.readWithDomain(valuesPerSignal, domainPerSignal, &count);

    ASSERT_EQ(count, SAMPLES);
    ASSERT_EQ(multi.getValueReadType(), SampleType::Int64);
}

TEST_F(MultiReaderTest, AddRemoveInput)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(NUM_SIGNALS);

    std::string epochStr = "2022-09-27T00:02:04+00:00";
    auto resolution = Ratio(1, 1000);
    auto rule = LinearDataRule(4, 0);

    auto& sig0 = addSignal(0, 20, createDomainSignal(epochStr, resolution, rule));
    auto& sig1 = addSignal(0, 20, createDomainSignal(epochStr, resolution, rule));
    auto& sig2 = addSignal(0, 10, createDomainSignal(epochStr, resolution, rule));

    auto ports = portsList();
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder()
                     .setAllowDifferentSamplingRates(false)
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addInputPort(ports[0])
                     .addInputPort(ports[1])
                     .build();

    ports[0].connect(signals[0]);
    ports[1].connect(signals[1]);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_TRUE(status.getValid());
    }

    sig0.createAndSendPacket(0);  // 20 samples
    sig1.createAndSendPacket(0);  // 20 samples
    sig2.createAndSendPacket(0);  // 10 samples
    sig2.createAndSendPacket(1);  // 10 samples

    // First two are aligned and packet were received in the connection
    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 20u);

    ASSERT_TRUE(multi.asPtr<IReaderConfig>().getIsValid());

    // When a non-connected port is added, all unread packets in other connections get dropped.
    multi.addInput(ports[2]);
    ports[2].connect(signals[2]);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_TRUE(status.getValid());
    }

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(1);  // 20 samples
    sig1.createAndSendPacket(1);  // 20 samples
    sig2.createAndSendPacket(2);  // 10 samples

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 10u);  // samples with timestamps 20-30 are available in all connections

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
        ASSERT_TRUE(status.getValid());
    }

    // Remove the signal with fewer samples
    multi.removeInput(ports[2].getGlobalId());

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 20u);  // now the first two can be aligned for all 20 samples
}

TEST_F(MultiReaderTest, UsedUnusedInput)
{
    constexpr const auto NUM_SIGNALS = 3;

    // prevent vector from re-allocating, so we have "stable" pointers
    readSignals.reserve(NUM_SIGNALS);

    std::string epochStr = "2022-09-27T00:02:04+00:00";
    auto resolution = Ratio(1, 1000);
    auto rule = LinearDataRule(4, 0);

    auto& sig0 = addSignal(0, 20, createDomainSignal(epochStr, resolution, rule));
    auto& sig1 = addSignal(0, 20, createDomainSignal(epochStr, resolution, rule));
    auto& sig2 = addSignal(0, 10, createDomainSignal(epochStr, resolution, rule));

    auto ports = portsList();
    auto signals = signalsToList();
    auto multi = MultiReaderBuilder()
                     .setAllowDifferentSamplingRates(false)
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addInputPort(ports[0])
                     .addInputPort(ports[1])
                     .addInputPort(ports[2])
                     .build();

    ports[0].connect(signals[0]);
    ports[1].connect(signals[1]);

    // Recovers to active state (all used are connected)
    multi.setInputUsed(ports[2].getGlobalId(), false);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_TRUE(status.getValid());
    }

    sig0.createAndSendPacket(0);  // 20 samples
    sig1.createAndSendPacket(0);  // 20 samples
    sig2.createAndSendPacket(0);  // 10 samples

    // The third signal is ignored, the first two get synced
    auto available = multi.getAvailableCount();
    ASSERT_EQ(available, 20u);

    sig2.createAndSendPacket(1);  // 10 samples

    ASSERT_TRUE(multi.asPtr<IReaderConfig>().getIsValid());

    ports[2].connect(signals[2]);
    multi.setInputUsed(ports[2].getGlobalId(), true);

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_TRUE(status.getValid());
    }

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 0u);

    sig0.createAndSendPacket(1);  // 20 samples
    sig1.createAndSendPacket(1);  // 20 samples
    sig2.createAndSendPacket(2);  // 10 samples

    available = multi.getAvailableCount();
    ASSERT_EQ(available, 10u);  // samples with timestamps 20-30 are available in all connections

    {
        SizeT count{0};
        auto status = multi.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
        ASSERT_TRUE(status.getValid());
    }
}

TEST_F(MultiReaderTest, CheckSpecificCase)
{
    readSignals.reserve(2);

    auto sig0 = addSignal(0, 2, createDomainSignal("2022-09-27T00:02:03+00:00", Ratio(1, 1000), LinearDataRule(1, 0), nullptr)); // 1000 Hz
    auto sig1 = addSignal(0, 1, createDomainSignal("2022-09-27T00:02:03+00:00", Ratio(1, 1000), LinearDataRule(5, 0), nullptr)); // 200 Hz

    const MultiReaderPtr multiReader = MultiReaderBuilder()
                                           .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                                           .addSignals(signalsToList())
                                           .setValueReadType(SampleType::Float64)
                                           .setDomainReadType(SampleType::Int64)
                                           .build();

    {
        SizeT count{0};
        auto status = multiReader.read(nullptr, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    }

    constexpr size_t numberOfSamplesToRead = 12;
    double dataFirstSignal[2*numberOfSamplesToRead];
    double dataSecondSignal[2*numberOfSamplesToRead];
    double* data[2]{dataFirstSignal, dataSecondSignal};

    sig0.createAndSendPacket(0, true);
    sig0.createAndSendPacket(1, true);
    sig0.createAndSendPacket(2, true);
    sig0.createAndSendPacket(3, true);
    sig0.createAndSendPacket(4, true);
    sig0.createAndSendPacket(5, true);
    sig0.setValueDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit("A", -1, "ampere", "current")).build());
    sig0.createAndSendPacket(6, true);
    sig0.createAndSendPacket(7, true);
    sig0.createAndSendPacket(8, true);

    sig1.createAndSendPacket(0, true);
    sig1.createAndSendPacket(1, true);
    sig1.createAndSendPacket(2, true);
    sig1.createAndSendPacket(3, true);
    sig1.createAndSendPacket(4, true);
    sig1.createAndSendPacket(5, true);
    sig1.createAndSendPacket(6, true);
    sig1.createAndSendPacket(7, true);

    {
        auto available = multiReader.getAvailableCount();
        ASSERT_EQ(available, 10u);

        SizeT count = numberOfSamplesToRead;
        auto status = multiReader.read(data, &count);
        ASSERT_EQ(count, 10u);
    }

    {
        auto available = multiReader.getAvailableCount();
        ASSERT_EQ(available, 0);

        // Behavior change (spec 7.2/8.5): the two samples left in front of the descriptor
        // change are less than one aligned block and are silently discarded, so the pending
        // event surfaces on this read instead of staying buried behind an unreadable segment
        SizeT count{2};
        auto status = multiReader.read(data, &count);
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
        ASSERT_EQ(count, 0u);
    }

    {
        auto available = multiReader.getAvailableCount();
        ASSERT_EQ(available, 0);

        SizeT count{0};
        auto status = multiReader.read(data, &count);
        // Data past the event keeps the reader synchronized (a full block just is not
        // available yet), so this reports Ok rather than Preparing
        ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    }
}

// --- IMultiReaderStatus state and diagnostics extension (spec 8.2, test plan ST rows) ---

TEST_F(MultiReaderTest, StatusStateWaitingForConnections)
{
    // ST-3/ST-4/ST-5: an unconnected used input is reported with its construction-order index
    readSignals.reserve(3);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto ports = portsList();
    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    ports[0].connect(readSignals[0].signal);
    ports[2].connect(readSignals[2].signal);
    // port 1 stays unconnected

    SizeT count{0};
    MultiReaderStatusPtr status = multi.read(nullptr, &count);
    // Waiting for a connection is not an error - nothing is wrong, no data is expected yet
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
    auto states = status.getInputStates();
    ASSERT_EQ(states.getCount(), 3u);
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(states.get(ports[1].getGlobalId()))), InputState::Pending);
    ASSERT_EQ(status.getStateMessage().toStdString(), "Inputs [1] have no signal connected");
}

TEST_F(MultiReaderTest, StatusEventDictAndInputStates)
{
    // ST-1/ST-6/ST-8: the event dict carries one event per input per read, the compat
    // first-event accessor returns one of them, and the per-input states reflect the
    // post-consumption condition
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    MultiReaderStatusPtr status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    // The dict carries the events keyed by port global id (ST-1)
    auto dict = status.getEventPackets();
    ASSERT_EQ(dict.getCount(), 2u);

    // IReaderStatus::getEventPacket returns one of the returned events (ST-8)
    auto firstEvent = status.asPtr<IReaderStatus>().getEventPacket();
    ASSERT_TRUE(firstEvent.assigned());
    ASSERT_EQ(firstEvent.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);

    // The initial events were consumed by this read - every input is now waiting for data
    auto states = status.getInputStates();
    ASSERT_EQ(states.getCount(), 2u);
    for (const auto& signal : readSignals)
    {
        ASSERT_EQ(static_cast<InputState>(static_cast<Int>(states.get(signal.signal.getGlobalId()))), InputState::Pending);
    }
}

TEST_F(MultiReaderTest, StatusMainDescriptorCommonDomain)
{
    // ST-2: the domain part of getMainDescriptor is the common output domain - earliest
    // epoch as origin, rational-GCD resolution and one output sample per linear-rule delta
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal("2022-09-27T00:02:04+00:00"));
    addSignal(0, 10, createDomainSignal("2022-09-27T00:02:03+00:00", Ratio(1, 10000), LinearDataRule(10, 0)));

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    MultiReaderStatusPtr status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    count = 0;
    status = multi.read(nullptr, &count);

    auto mainDescriptor = status.getMainDescriptor();
    ASSERT_TRUE(mainDescriptor.assigned());
    ASSERT_EQ(mainDescriptor.getEventId(), event_packet_id::DATA_DESCRIPTOR_CHANGED);

    DataDescriptorPtr domainDescriptor =
        mainDescriptor.getParameters().get(event_packet_param::DOMAIN_DATA_DESCRIPTOR).asPtrOrNull<IDataDescriptor>();
    ASSERT_TRUE(domainDescriptor.assigned());

    // Origin and resolution equal the reader's common-domain accessors
    ASSERT_EQ(domainDescriptor.getOrigin(), multi.getOrigin());
    ASSERT_EQ(domainDescriptor.getTickResolution(), multi.getTickResolution());
    ASSERT_EQ(domainDescriptor.getTickResolution(), Ratio(1, 10000));

    // One 1000 Hz output sample spans ten 1/10000 s common ticks
    const auto rule = domainDescriptor.getRule();
    ASSERT_EQ(rule.getType(), DataRuleType::Linear);
    ASSERT_EQ(static_cast<Int>(rule.getParameters().get("delta")), 10);
}

TEST_F(MultiReaderTest, StatusCachedWhileUnchangedNewOnChange)
{
    // ST-7: the status instance is re-issued while its visible content is unchanged and
    // replaced when the state changes
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    MultiReaderStatusPtr eventStatus = multi.read(nullptr, &count);
    ASSERT_EQ(eventStatus.getReadStatus(), ReadStatus::Event);

    count = 0;
    MultiReaderStatusPtr first = multi.read(nullptr, &count);
    count = 0;
    MultiReaderStatusPtr second = multi.read(nullptr, &count);

    ASSERT_EQ(first.getReadStatus(), ReadStatus::Preparing);
    ASSERT_EQ(first.getObject(), second.getObject());

    sendPackets(0);

    count = 0;
    MultiReaderStatusPtr third = multi.read(nullptr, &count);
    ASSERT_EQ(third.getReadStatus(), ReadStatus::Ok);
    ASSERT_NE(third.getObject(), second.getObject());
}

TEST_F(MultiReaderTest, StatusStateIncompatibleRecoverable)
{
    // ST-3 + LC-10: a non-convertible value descriptor reports a recoverable Incompatible
    // state naming the input; a later convertible descriptor recovers in the same instance
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    MultiReaderStatusPtr status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    // Complex values cannot be converted to the double read type
    readSignals[0].setValueDescriptor(setupDescriptor(SampleType::ComplexFloat64));

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(status.getInputStates().get(readSignals[0].signal.getGlobalId()))),
              InputState::Incompatible);
    // Recoverable - only ReadStatus::Fail reads as invalid (C5/Q1)
    ASSERT_TRUE(status.getValid());

    // A follow-up read with no events left reports the persistent condition
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
    ASSERT_TRUE(status.getValid());

    // The reader itself stays valid - the condition is recoverable (spec 6.1/8.5)
    ASSERT_TRUE(multi.asPtr<IReaderConfig>().getIsValid());

    readSignals[0].setValueDescriptor(setupDescriptor(SampleType::Float64));

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    ASSERT_TRUE(status.getValid());

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
}

// --- Phase 4: main-input selection, synchronization distance, data-loss monitoring ---

TEST_F(MultiReaderTest, MainInputAccessors)
{
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    // Empty string means automatic selection (error contract 3.3)
    ASSERT_EQ(multi.getMainInput().getLength(), 0u);
    ASSERT_THROW(multi.setMainInput("no-such-input"), NotFoundException);

    const auto mainId = readSignals[1].signal.getGlobalId();
    multi.setMainInput(mainId);
    ASSERT_EQ(multi.getMainInput(), mainId);

    // An empty id reverts to the default (first used input)
    multi.setMainInput("");
    ASSERT_EQ(multi.getMainInput().getLength(), 0u);

    // Selecting an unused input is rejected (error contract 3.3)
    multi.setInputUsed(mainId, false);
    ASSERT_THROW(multi.setMainInput(mainId), InvalidParameterException);
}

TEST_F(MultiReaderTest, MainInputSelectionGrid)
{
    // SY-14: the main input defines the grid phase, and alignment only tolerates other
    // inputs starting a small FORWARD offset after it. Input 1's grid trails input 0's by
    // nine of ten ticks - unacceptable with input 0 as main (nine-tick offsets are
    // ambiguous), but with input 1 as main input 0 leads by just one tick and the pair
    // synchronizes on input 1's grid.
    readSignals.reserve(2);
    auto& sig0 = addSignal(500, 30, createDomainSignal("2022-09-27T00:02:03+00:00", Ratio(1, 1000), LinearDataRule(10, 0)));
    auto& sig1 = addSignal(509, 30, createDomainSignal("2022-09-27T00:02:03+00:00", Ratio(1, 1000), LinearDataRule(10, 0)));

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    sig0.createAndSendPacket(0);
    sig1.createAndSendPacket(0);

    ASSERT_EQ(multi.getAvailableCount(), 0u);
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);

    multi.setMainInput(readSignals[1].signal.getGlobalId());
    ASSERT_GT(multi.getAvailableCount(), 0u);
    ASSERT_TRUE(multi.getIsSynchronized());
}

TEST_F(MultiReaderTest, MainInputDisconnectedWaits)
{
    // SY-15: a disconnected selected main input is never silently replaced - the reader
    // waits for its connection instead of re-anchoring on the remaining inputs
    readSignals.reserve(2);
    auto& sig0 = addSignal(0, 10, createDomainSignal());
    auto& sig1 = addSignal(0, 10, createDomainSignal());

    auto ports = portsList();
    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addInputPorts(ports).build();
    ports[0].connect(sig0.signal);
    ports[1].connect(sig1.signal);

    multi.setMainInput(ports[0].getGlobalId());

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    sendPackets(0);
    ASSERT_GT(multi.getAvailableCount(), 0u);

    ports[0].disconnect();

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Preparing);
    ASSERT_EQ(multi.getAvailableCount(), 0u);
    ASSERT_EQ(multi.getMainInput(), ports[0].getGlobalId());
}

TEST_F(MultiReaderTest, MaxSyncDistanceFailsWithDiagnostics)
{
    // SY-11: inputs starting 10 s apart with a 5 s threshold fail the synchronization with
    // the early input named; the reader stays active (spec 8.5)
    readSignals.reserve(2);
    addSignal(0, 12000, createDomainSignal());
    addSignal(10000, 2000, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setMaxSynchronizationDistance(Ratio(5, 1))
                     .build();

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    sendPackets(0);

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
    // Recoverable - only ReadStatus::Fail reads as invalid (C5/Q1)
    ASSERT_TRUE(status.getValid());
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(status.getInputStates().get(readSignals[0].signal.getGlobalId()))),
              InputState::SynchronizationFailed);
    ASSERT_NE(status.getStateMessage().toStdString().find("maximum synchronization distance"), std::string::npos);
    ASSERT_TRUE(multi.getActive());
}

TEST_F(MultiReaderTest, MaxSyncDistanceZeroDisables)
{
    // SY-12: with the default (zero) threshold the same 10 s stagger synchronizes
    readSignals.reserve(2);
    addSignal(0, 12000, createDomainSignal());
    addSignal(10000, 2000, createDomainSignal());

    auto multi =
        MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread).addSignals(signalsToList()).build();

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    sendPackets(0);

    ASSERT_EQ(multi.getAvailableCount(), 2000u);
    ASSERT_TRUE(multi.getIsSynchronized());
}

TEST_F(MultiReaderTest, Phase4BuilderAccessors)
{
    auto builder = MultiReaderBuilder();

    ASSERT_FALSE(builder.getMainInput().assigned());
    // Zero means disabled (the default)
    ASSERT_EQ(builder.getMaxSynchronizationDistance(), Ratio(0, 1));
    ASSERT_EQ(builder.getDataLossTimeout(), Ratio(0, 1));

    ASSERT_THROW(builder.setMaxSynchronizationDistance(Ratio(-1, 1)), InvalidParameterException);
    ASSERT_THROW(builder.setDataLossTimeout(Ratio(-1, 1)), InvalidParameterException);
    ASSERT_THROW(builder.setMaxSynchronizationDistance(nullptr), ArgumentNullException);
    ASSERT_THROW(builder.setDataLossTimeout(nullptr), ArgumentNullException);

    builder.setMaxSynchronizationDistance(Ratio(5, 1));
    builder.setDataLossTimeout(Ratio(1, 2));
    ASSERT_EQ(builder.getMaxSynchronizationDistance(), Ratio(5, 1));
    ASSERT_EQ(builder.getDataLossTimeout(), Ratio(1, 2));

    // The built reader reflects the builder configuration
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());
    const auto mainId = readSignals[1].signal.getGlobalId();

    auto multi = builder.setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setMainInput(mainId)
                     .build();

    ASSERT_EQ(multi.getMainInput(), mainId);
    // C1/C2: the sync distance and data-loss timeout are builder-only configuration;
    // the reader exposes no accessors for them
}

TEST_F(MultiReaderTest, DataLossVirtualClock)
{
    // DL-1/DL-2/DL-3 through the public API on virtual time (test scaffolding 2.7)
    readSignals.reserve(2);
    auto& sig0 = addSignal(0, 10, createDomainSignal());
    auto& sig1 = addSignal(0, 10, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setDataLossTimeout(Ratio(10, 1))  // ten virtual seconds
                     .build();

    auto* impl = dynamic_cast<MultiReaderImpl*>(multi.asPtr<IReaderConfig>().getObject());
    ASSERT_NE(impl, nullptr);
    auto virtualNow = std::chrono::steady_clock::now();
    impl->setDataLossClockForTest([&virtualNow] { return virtualNow; });

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);

    sendPackets(0);
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);

    // Drain the buffered data: with in-band data loss (review 2.6) buffered pre-loss
    // samples stay readable, so the loss only surfaces once the queue runs dry
    {
        double values0[10]{};
        double values1[10]{};
        void* buffers[2]{values0, values1};
        count = 10;
        status = multi.read(buffers, &count);
        ASSERT_EQ(count, 10u);
    }

    // Input 1 goes stale while input 0 keeps delivering (DL-2)
    virtualNow += std::chrono::seconds(6);
    sig0.createAndSendPacket(1);
    virtualNow += std::chrono::seconds(6);

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
    // Recoverable - only ReadStatus::Fail reads as invalid (C5/Q1)
    ASSERT_TRUE(status.getValid());
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(status.getInputStates().get(sig1.signal.getGlobalId()))),
              InputState::DataLost);

    // The stale input recovers on its next packet (DL-3)
    sig1.createAndSendPacket(1);
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_NE(status.getReadStatus(), ReadStatus::InputsFailed);
    ASSERT_TRUE(status.getValid());
}

TEST_F(MultiReaderTest, DataLossInactiveAndUnusedNotMonitored)
{
    // DL-5: inactive readers and unused inputs never trip the deadline
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setDataLossTimeout(Ratio(10, 1))
                     .build();

    auto* impl = dynamic_cast<MultiReaderImpl*>(multi.asPtr<IReaderConfig>().getObject());
    ASSERT_NE(impl, nullptr);
    auto virtualNow = std::chrono::steady_clock::now();
    impl->setDataLossClockForTest([&virtualNow] { return virtualNow; });

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    sendPackets(0);

    // An unused input is not monitored even when stale
    multi.setInputUsed(readSignals[1].signal.getGlobalId(), false);
    virtualNow += std::chrono::seconds(60);
    readSignals[0].createAndSendPacket(1);  // input 0 stays fresh

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_NE(status.getReadStatus(), ReadStatus::InputsFailed);

    // An inactive reader is not monitored at all
    multi.setInputUsed(readSignals[1].signal.getGlobalId(), true);
    multi.setActive(false);
    virtualNow += std::chrono::seconds(60);

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Inactive);
}

TEST_F(MultiReaderTest, DataLossDeadlineFiresWithoutReads)
{
    // DL-1 smoke test on real time: the deadline transitions the reader out of the
    // synchronized state autonomously - getIsSynchronized only inspects the state and
    // never re-evaluates, so observing the flip proves the waiter-driven path
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setDataLossTimeout(Ratio(1, 20))  // 50 ms
                     .build();

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    sendPackets(0);

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    ASSERT_TRUE(multi.getIsSynchronized());

    // Drain the buffered data: with in-band data loss (review 2.6) buffered pre-loss
    // samples keep the reader synchronized until they are read
    {
        double values0[10]{};
        double values1[10]{};
        void* buffers[2]{values0, values1};
        count = 10;
        status = multi.read(buffers, &count);
        ASSERT_EQ(count, 10u);
        ASSERT_TRUE(multi.getIsSynchronized());
    }

    const auto start = std::chrono::steady_clock::now();
    while (multi.getIsSynchronized() && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::yield();

    ASSERT_FALSE(multi.getIsSynchronized());

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
}

TEST_F(MultiReaderTest, DataLossFiresCallback)
{
    // Part 1 (spec 3.5): the transition into DataLost raises onDataAvailable on its own, so the
    // consumer is woken and reads the loss with no polling. Real-time smoke test - only the
    // reader's data-loss waiter thread drives the callback here.
    readSignals.reserve(2);
    addSignal(0, 10, createDomainSignal());
    addSignal(0, 10, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setDataLossTimeout(Ratio(1, 5))  // 200 ms
                     .build();

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    sendPackets(0);
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);

    // Drain the buffered pre-loss samples so the loss can surface once the queues run dry
    {
        double values0[10]{};
        double values1[10]{};
        void* buffers[2]{values0, values1};
        count = 10;
        status = multi.read(buffers, &count);
        ASSERT_EQ(count, 10u);
    }

    // Arm the callback only now: no packet and no read drive it, so the wake we observe is the
    // data-loss deadline itself
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    MultiReaderStatusPtr cbStatus;
    multi.setOnDataAvailable(
        [&]
        {
            multi.setOnDataAvailable(nullptr);  // one-shot
            SizeT c{0};
            cbStatus = multi.read(nullptr, &c);
            promise.set_value();
        });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(cbStatus.assigned());
    ASSERT_EQ(cbStatus.getReadStatus(), ReadStatus::InputsFailed);
    ASSERT_TRUE(cbStatus.getValid());
}

TEST_F(MultiReaderTest, DataLossArmsAtStartForSilentReenabledInput)
{
    // Part 2 (spec 3.6): a used input arms the moment it becomes monitored, so an input
    // re-enabled onto a now-silent producer trips the deadline and surfaces as DataLost even
    // though it never delivers a packet - the case that lets a consumer drop its own liveness timer.
    readSignals.reserve(2);
    auto& sig0 = addSignal(0, 10, createDomainSignal());
    auto& sig1 = addSignal(0, 10, createDomainSignal());

    auto multi = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .addSignals(signalsToList())
                     .setDataLossTimeout(Ratio(10, 1))  // ten virtual seconds
                     .build();

    auto* impl = dynamic_cast<MultiReaderImpl*>(multi.asPtr<IReaderConfig>().getObject());
    ASSERT_NE(impl, nullptr);
    auto virtualNow = std::chrono::steady_clock::now();
    impl->setDataLossClockForTest([&virtualNow] { return virtualNow; });

    SizeT count{0};
    auto status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Event);
    sendPackets(0);
    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::Ok);
    {
        double values0[10]{};
        double values1[10]{};
        void* buffers[2]{values0, values1};
        count = 10;
        status = multi.read(buffers, &count);
        ASSERT_EQ(count, 10u);
    }

    // Exclude input 1, then re-enable it onto a silent producer: it arms at the moment monitoring
    // resumes, with no packet to refresh the deadline.
    multi.setInputUsed(sig1.signal.getGlobalId(), false);
    multi.setInputUsed(sig1.signal.getGlobalId(), true);

    // Input 0 keeps delivering so it stays fresh; input 1 never does and crosses its deadline.
    virtualNow += std::chrono::seconds(11);
    sig0.createAndSendPacket(1);

    count = 0;
    status = multi.read(nullptr, &count);
    ASSERT_EQ(status.getReadStatus(), ReadStatus::InputsFailed);
    ASSERT_TRUE(status.getValid());
    ASSERT_EQ(static_cast<InputState>(static_cast<Int>(status.getInputStates().get(sig1.signal.getGlobalId()))),
              InputState::DataLost);
}
