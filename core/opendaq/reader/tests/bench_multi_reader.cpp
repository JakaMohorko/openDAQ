/*
 * Multi reader performance benchmark - standalone, uses only the public openDAQ API that is
 * common to the pre-rework (main) and post-rework branches, so the SAME source compiles and
 * runs against both for an apples-to-apples comparison.
 *
 * Output: CSV lines "scenario,param,metric,value" on stdout. Build the `bench_multi_reader`
 * target in Release and run with no arguments (runs all scenarios).
 *
 * Scenarios:
 *   inputs        - throughput vs number of inputs (equal rate)
 *   packet        - throughput vs packet size
 *   rates         - throughput vs multi-rate spread (dividers)
 *   events        - throughput vs descriptor-event rate (resync every K packets)
 *   resync        - time per resync when every packet batch forces a re-synchronization
 *   convert       - typed-read conversion throughput (native copy vs type conversion)
 */
#include <opendaq/context_factory.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/logger_factory.h>
#include <opendaq/packet_factory.h>
#include <opendaq/reader_factory.h>
#include <opendaq/scheduler_factory.h>
#include <opendaq/signal_factory.h>
#include <coreobjects/unit_factory.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace daq;

namespace
{

using Clock = std::chrono::steady_clock;

ContextPtr makeContext()
{
    auto logger = Logger();
    return Context(Scheduler(logger, 1), logger, nullptr, nullptr, nullptr);
}

DataDescriptorPtr valueDescriptor(SampleType type)
{
    return DataDescriptorBuilder().setSampleType(type).build();
}

// Implicit (linear-rule) domain descriptor; delta sets the sample spacing => rate = resolution/delta.
DataDescriptorPtr domainDescriptor(std::int64_t delta)
{
    return DataDescriptorBuilder()
        .setSampleType(SampleType::Int64)
        .setTickResolution(Ratio(1, 1000000))
        .setRule(LinearDataRule(delta, 0))
        .setOrigin("1970-01-01T00:00:00+00:00")
        .setUnit(Unit("s", -1, "seconds", "time"))
        .build();
}

struct Bench
{
    ContextPtr context = makeContext();
    std::vector<SignalConfigPtr> signals;
    std::vector<SignalConfigPtr> domains;
    std::vector<std::int64_t> deltas;
    std::vector<std::int64_t> nextTick;
    MultiReaderPtr reader;
    SampleType valueType = SampleType::Float64;

    void build(SizeT inputCount, SampleType valueSampleType, const std::vector<std::int64_t>& perInputDelta)
    {
        valueType = valueSampleType;
        signals.clear();
        domains.clear();
        deltas.clear();
        nextTick.assign(inputCount, 0);

        auto list = List<ISignal>();
        for (SizeT i = 0; i < inputCount; ++i)
        {
            const auto delta = perInputDelta[i % perInputDelta.size()];
            auto domain = Signal(context, nullptr, "d" + std::to_string(i));
            domain.setDescriptor(domainDescriptor(delta));
            auto sig = Signal(context, nullptr, "s" + std::to_string(i));
            sig.setDescriptor(valueDescriptor(valueSampleType));
            sig.setDomainSignal(domain);
            signals.push_back(sig);
            domains.push_back(domain);
            deltas.push_back(delta);
            list.pushBack(sig);
        }

        reader = MultiReaderBuilder()
                     .setInputPortNotificationMethod(PacketReadyNotification::SameThread)
                     .setValueReadType(SampleType::Float64)
                     .setDomainReadType(SampleType::Int64)
                     .addSignals(list)
                     .build();

        // Consume the initial descriptor events so the reader can synchronize
        drain();
    }

    void sendPacket(SizeT idx, SizeT sampleCount)
    {
        const auto domainPacket = DataPacket(domains[idx].getDescriptor(), sampleCount, nextTick[idx]);
        const auto valuePacket = DataPacketWithDomain(domainPacket, signals[idx].getDescriptor(), sampleCount);

        // Fill according to the declared sample type so conversion (if any) has real work
        void* raw = valuePacket.getRawData();
        switch (valueType)
        {
            case SampleType::Float64: { auto* d = static_cast<double*>(raw); for (SizeT i = 0; i < sampleCount; ++i) d[i] = 1.0; break; }
            case SampleType::Int32:   { auto* d = static_cast<std::int32_t*>(raw); for (SizeT i = 0; i < sampleCount; ++i) d[i] = 1; break; }
            case SampleType::Int16:   { auto* d = static_cast<std::int16_t*>(raw); for (SizeT i = 0; i < sampleCount; ++i) d[i] = 1; break; }
            default: break;
        }

        nextTick[idx] += static_cast<std::int64_t>(sampleCount) * deltas[idx];
        signals[idx].sendPacket(valuePacket);
    }

    void sendAll(SizeT sampleCount)
    {
        for (SizeT i = 0; i < signals.size(); ++i)
            sendPacket(i, sampleCount);
    }

    // Drain all currently-available samples into throwaway buffers; returns common samples read.
    SizeT drain()
    {
        const SizeT n = signals.size();
        std::vector<std::vector<double>> bufs(n);
        std::vector<void*> ptrs(n);
        SizeT total = 0;

        for (;;)
        {
            SizeT available = reader.getAvailableCount();
            if (available == 0)
            {
                // A head event gates availability; a zero-count read consumes it, then retry once.
                SizeT zero = 0;
                auto status = reader.read(nullptr, &zero);
                if (status.getReadStatus() == ReadStatus::Event)
                    continue;
                break;
            }
            for (SizeT i = 0; i < n; ++i)
            {
                if (bufs[i].size() < available)
                    bufs[i].resize(available);
                ptrs[i] = bufs[i].data();
            }
            SizeT count = available;
            reader.read(ptrs.data(), &count);
            total += count;
            if (count == 0)
                break;
        }
        return total;
    }
};

double megaSamplesPerSec(SizeT commonSamples, double seconds)
{
    if (seconds <= 0.0)
        return 0.0;
    return (static_cast<double>(commonSamples) / seconds) / 1e6;
}

void emit(const char* scenario, const std::string& param, const char* metric, double value)
{
    std::printf("%s,%s,%s,%.4f\n", scenario, param.c_str(), metric, value);
    std::fflush(stdout);
}

// Steady-state throughput: send `iters` batches of `packet` samples to every input, drain each.
void throughput(const char* scenario, const std::string& param, Bench& b, SizeT packet, SizeT iters)
{
    // Warm up to synchronized steady state
    b.sendAll(packet);
    b.drain();

    SizeT total = 0;
    const auto t0 = Clock::now();
    for (SizeT it = 0; it < iters; ++it)
    {
        b.sendAll(packet);
        total += b.drain();
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    emit(scenario, param, "Msamp_s", megaSamplesPerSec(total, secs));
    emit(scenario, param, "ns_per_common_sample", total ? (secs * 1e9 / total) : 0.0);
}

void scenarioInputs()
{
    for (SizeT n : {1u, 2u, 4u, 8u, 16u, 32u})
    {
        Bench b;
        b.build(n, SampleType::Float64, {1});
        throughput("inputs", std::to_string(n), b, 1024, 200);
    }
}

void scenarioPacket()
{
    for (SizeT p : {16u, 64u, 256u, 1024u, 4096u, 16384u})
    {
        Bench b;
        b.build(4, SampleType::Float64, {1});
        // Keep total samples per scenario roughly constant
        const SizeT iters = (1u << 21) / p;
        throughput("packet", std::to_string(p), b, p, iters ? iters : 1);
    }
}

void scenarioRates()
{
    // Multi-rate spreads: dividers determine blockLcm and per-input sample counts
    // Deltas must yield integer sample rates (rate = resolutionDen/delta) that share a common
    // rate, or synchronization legitimately fails and the row reads 0; powers of two do.
    const std::vector<std::pair<std::string, std::vector<std::int64_t>>> spreads = {
        {"equal_1_1_1_1", {1, 1, 1, 1}},
        {"gcd_1_2", {1, 2, 1, 2}},
        {"gcd_1_2_5", {1, 2, 5, 10}},
        {"wide_1_2_4_8", {1, 2, 4, 8}},
    };
    for (const auto& [name, deltas] : spreads)
    {
        Bench b;
        b.build(4, SampleType::Float64, deltas);
        throughput("rates", name, b, 2048, 200);
    }
}

void scenarioEvents()
{
    // Descriptor-change event every K batches -> resync. K = 0 means "every batch".
    for (SizeT k : {1u, 2u, 8u, 32u, 128u})
    {
        Bench b;
        b.build(4, SampleType::Float64, {1});
        b.sendAll(1024);
        b.drain();

        SizeT total = 0;
        const SizeT iters = 400;
        const auto t0 = Clock::now();
        for (SizeT it = 0; it < iters; ++it)
        {
            if (it % k == 0)
            {
                // Re-assert the same value descriptor: a descriptor-changed event the reader
                // must consume and resynchronize past
                b.signals[it % b.signals.size()].setDescriptor(valueDescriptor(SampleType::Float64));
            }
            b.sendAll(1024);
            total += b.drain();
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        emit("events", "every_" + std::to_string(k), "Msamp_s", megaSamplesPerSec(total, secs));
    }
}

void scenarioResync()
{
    // Worst case: force a resync on EVERY batch (descriptor change on one input each time),
    // measuring the per-resync cost rather than steady throughput.
    for (SizeT n : {2u, 4u, 8u, 16u})
    {
        Bench b;
        b.build(n, SampleType::Float64, {1});
        b.sendAll(256);
        b.drain();

        const SizeT resyncs = 500;
        const auto t0 = Clock::now();
        for (SizeT it = 0; it < resyncs; ++it)
        {
            b.signals[it % n].setDescriptor(valueDescriptor(SampleType::Float64));
            b.sendAll(256);
            b.drain();
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        emit("resync", std::to_string(n), "us_per_resync", (secs * 1e6) / resyncs);
    }
}

void scenarioConvert()
{
    // Same pipeline, different signal sample type read as Float64: Float64 (no conversion),
    // Int32 and Int16 (integer -> double conversion in the typed read path).
    const std::vector<std::pair<std::string, SampleType>> types = {
        {"f64_nocopy", SampleType::Float64},
        {"i32_to_f64", SampleType::Int32},
        {"i16_to_f64", SampleType::Int16},
    };
    for (const auto& [name, type] : types)
    {
        Bench b;
        b.build(4, type, {1});
        throughput("convert", name, b, 4096, 300);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::string only = argc > 1 ? argv[1] : "";
    std::printf("scenario,param,metric,value\n");

    if (only.empty() || only == "inputs")  scenarioInputs();
    if (only.empty() || only == "packet")  scenarioPacket();
    if (only.empty() || only == "rates")   scenarioRates();
    if (only.empty() || only == "events")  scenarioEvents();
    if (only.empty() || only == "resync")  scenarioResync();
    if (only.empty() || only == "convert") scenarioConvert();

    return 0;
}
