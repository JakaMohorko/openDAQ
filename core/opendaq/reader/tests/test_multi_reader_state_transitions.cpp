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

// Characterization tests for the multi reader's state machine (docs/multi_reader_state_refactor.md
// phase 0). Each test drives a reader through one scenario and records a golden trace of
// (trigger -> substate[affected inputs] + observable detail). The refactor's hard invariant is that
// for identical observable inputs the substate computed afterwards equals the substate computed
// today, on every path - these traces are what makes that checkable instead of arguable.
//
// Two things make the traces deterministic:
//
//  * The substate is read through MultiReaderImpl::getStateForTest(), not inferred from ReadStatus.
//    The public status is a lossy projection (WaitingForConnections, WaitingForDescriptors,
//    WaitingForData and Synchronizing all report Preparing), so it cannot characterize a partition
//    of the ladder.
//  * Every trigger drains the scheduler before the observation, so no trace line depends on which
//    coalesced task won a race. Observation itself is one of two kinds, and the difference matters:
//    a *probe* forces a level-triggered re-derivation from ground truth (what checkTransitionCriteria
//    will be), while a *peek* records the state a trigger left behind without re-deriving. The two
//    can legitimately differ - an alignment attempt leaves Synchronizing behind, and re-deriving
//    from the state it left reports WaitingForData (see AlignmentIsIterativeAndSynchronizingIsTransient).
//
// Where a trace line is a deliberate property rather than an obvious consequence, the property is
// named in a comment next to it, with the regression-contract item it belongs to.

#include <opendaq/custom_log.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_factory.h>
#include "reader_common.h"

#include <gmock/gmock-matchers.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <chrono>
#include <deque>
#include <string>
#include <vector>

using namespace daq;
using namespace testing;

namespace
{

const char* stateName(ReaderState state)
{
    switch (state)
    {
        case ReaderState::Inactive:
            return "Inactive";
        case ReaderState::WaitingForConnections:
            return "WaitingForConnections";
        case ReaderState::WaitingForDescriptors:
            return "WaitingForDescriptors";
        case ReaderState::Incompatible:
            return "Incompatible";
        case ReaderState::WaitingForData:
            return "WaitingForData";
        case ReaderState::Synchronizing:
            return "Synchronizing";
        case ReaderState::Synchronized:
            return "Synchronized";
        case ReaderState::EventPending:
            return "EventPending";
        case ReaderState::SynchronizationFailed:
            return "SynchronizationFailed";
        case ReaderState::DataLost:
            return "DataLost";
        case ReaderState::Error:
            return "Error";
    }
    return "<unknown>";
}

const char* readStatusName(ReadStatus status)
{
    switch (status)
    {
        case ReadStatus::Ok:
            return "Ok";
        case ReadStatus::Event:
            return "Event";
        case ReadStatus::Fail:
            return "Fail";
        case ReadStatus::Preparing:
            return "Preparing";
        case ReadStatus::Inactive:
            return "Inactive";
        case ReadStatus::InputsFailed:
            return "InputsFailed";
        case ReadStatus::Unknown:
            return "Unknown";
    }
    return "<unknown>";
}

}  // namespace

/// Every scenario runs twice: once with each behaviour checking its own exit conditions, and once with
/// the exhaustive derivation forced (the reference). Identical traces are what verify the narrow checks.
class MultiReaderStateTest : public ReaderTest<>, public testing::WithParamInterface<bool>
{
public:
    using Super = ReaderTest<>;

protected:
    /// One value signal with its own domain signal, plus a running domain offset so successive
    /// packets are contiguous in the signal's own domain.
    struct TraceSignal
    {
        SignalConfigPtr signal;
        SignalConfigPtr domain;
        Int packetSize;
        Int delta;
        Int nextTick;
    };

    void TearDown() override
    {
        // The reader must die before the scheduler stops: it is a fixture member, so without this
        // it would outlive Super::TearDown()
        impl = nullptr;
        multi = nullptr;
        Super::TearDown();
    }

    TraceSignal& addSignal(Int packetSize = 10, const RatioPtr& resolution = nullptr, const DataRulePtr& domainRule = nullptr)
    {
        const auto index = signals.size();
        auto domain = Signal(context, nullptr, fmt::format("time{}", index));
        domain.setDescriptor(createDomainDescriptor("2022-09-27T00:02:03+00:00", resolution, domainRule));

        auto value = Signal(context, nullptr, fmt::format("sig{}", index));
        value.setDescriptor(setupDescriptor(SampleType::Float64));
        value.setDomainSignal(domain);

        const Int delta = domain.getDescriptor().getRule().getParameters().get("delta");
        return signals.emplace_back(TraceSignal{value, domain, packetSize, delta, 0});
    }

    ListPtr<ISignal> signalList() const
    {
        auto list = List<ISignal>();
        for (const auto& item : signals)
            list.pushBack(item.signal);
        return list;
    }

    /// SameThread notifications are a precondition of the state machine (refactor spec 2.1) and
    /// what every existing multi reader test uses.
    MultiReaderBuilderPtr baseBuilder() const
    {
        return MultiReaderBuilder().setInputPortNotificationMethod(PacketReadyNotification::SameThread);
    }

    void buildFromSignals(MultiReaderBuilderPtr builder = nullptr)
    {
        if (!builder.assigned())
            builder = baseBuilder();
        multi = builder.addSignals(signalList()).build();
        bind();
    }

    /// Unconnected ports, so a test can stagger the connects itself.
    void buildFromPorts(MultiReaderBuilderPtr builder = nullptr)
    {
        if (!builder.assigned())
            builder = baseBuilder();

        ports = List<IInputPortConfig>();
        for (SizeT i = 0; i < signals.size(); ++i)
            ports.pushBack(InputPort(context, nullptr, fmt::format("port{}", i)));

        multi = builder.addInputPorts(ports).build();
        bind();
    }

    /// Virtual time for the data-loss deadlines: no real sleeps, and the monitor's waiter thread
    /// stays dormant so the tests drive every evaluation themselves.
    void useVirtualClock()
    {
        virtualNow = std::chrono::steady_clock::now();
        impl->setDataLossClockForTest([this] { return virtualNow; });
    }

    void advanceClock(std::chrono::seconds by)
    {
        virtualNow += by;
    }

    // --- Triggers. Each one settles the scheduler so the following observation is deterministic. ---

    void connect(SizeT index)
    {
        ports[index].connect(signals[index].signal);
        settle();
    }

    void disconnect(SizeT index)
    {
        ports[index].disconnect();
        settle();
    }

    void send(SizeT index, Int samples = 0)
    {
        auto& item = signals[index];
        const Int count = samples > 0 ? samples : item.packetSize;

        auto domainPacket = DataPacket(item.domain.getDescriptor(), count, item.nextTick);
        auto packet = DataPacketWithDomain(domainPacket, item.signal.getDescriptor(), count);
        memset(packet.getRawData(), 0, packet.getRawDataSize());
        item.nextTick += count * item.delta;

        item.signal.sendPacket(packet);
        settle();
    }

    void sendAll(Int samples = 0)
    {
        for (SizeT i = 0; i < signals.size(); ++i)
            send(i, samples);
    }

    void setValueSampleType(SizeT index, SampleType type)
    {
        signals[index].signal.setDescriptor(setupDescriptor(type));
        settle();
    }

    void setActive(bool active)
    {
        multi.setActive(active);
        settle();
    }

    void setInputUsed(SizeT index, bool used)
    {
        multi.setInputUsed(inputId(index), used);
        settle();
    }

    StringPtr inputId(SizeT index) const
    {
        // A signal-built reader keys its inputs by the signal's global id; a port-built one by the
        // port's (Input::getInputId)
        return ports.assigned() ? ports[index].getGlobalId() : signals[index].signal.getGlobalId();
    }

    // --- Observations. Every one appends exactly one golden-trace line. ---

    /// getAvailableCount() + a trace line. The query path re-derives the state from ground truth in
    /// every non-synchronized state (refreshDataPlaneLocked escalates), which is what makes this a
    /// level-triggered probe rather than a peek at whatever a notification left behind.
    SizeT probe(const std::string& label)
    {
        const auto available = multi.getAvailableCount();
        mark(label, fmt::format("avail={}", available));
        return available;
    }

    /// Records the state the last trigger left behind, WITHOUT re-deriving it. Needed for the
    /// transients: an alignment attempt sets Synchronizing and the next evaluation supersedes it, so
    /// a probe can never see it.
    void peek(const std::string& label)
    {
        mark(label, "(peek)");
    }

    /// The zero-count event handshake: reports (and consumes) pending events without touching data.
    MultiReaderStatusPtr readEvents(const std::string& label)
    {
        SizeT count = 0;
        const MultiReaderStatusPtr status = multi.read(nullptr, &count);
        mark(label,
             fmt::format("status={} events={}", readStatusName(status.getReadStatus()), status.getEventPackets().getCount()));
        return status;
    }

    MultiReaderStatusPtr readData(const std::string& label, SizeT requested)
    {
        // One buffer per slot, indexed by slot index like the reader expects
        std::vector<std::vector<double>> buffers(signals.size(), std::vector<double>(requested, 0.0));
        std::vector<void*> pointers;
        for (auto& buffer : buffers)
            pointers.push_back(buffer.data());

        SizeT count = requested;
        const MultiReaderStatusPtr status = multi.read(pointers.data(), &count);
        mark(label, fmt::format("status={} count={}", readStatusName(status.getReadStatus()), count));
        return status;
    }

    InputState inputState(const MultiReaderStatusPtr& status, SizeT index) const
    {
        return static_cast<InputState>(static_cast<Int>(status.getInputStates().get(inputId(index))));
    }

    void expectTrace(const std::vector<std::string>& expected) const
    {
        EXPECT_THAT(trace, ElementsAreArray(expected));
    }

    std::deque<TraceSignal> signals;  // deque: references handed out by addSignal stay valid
    ListPtr<IInputPortConfig> ports;
    MultiReaderPtr multi;
    MultiReaderImpl* impl{nullptr};
    std::vector<std::string> trace;
    std::chrono::steady_clock::time_point virtualNow{};

private:
    void bind()
    {
        impl = dynamic_cast<MultiReaderImpl*>(multi.asPtr<IReaderConfig>().getObject());
        ASSERT_NE(impl, nullptr);
        impl->setExhaustiveDerivationForTest(GetParam());
    }

    /// Lets any coalesced evaluation already scheduled by a producer finish, so an observation never
    /// races one. The evaluation is idempotent (same ladder, same ground truth), so this is about
    /// keeping the trace reproducible, not about correctness of the state.
    void settle() const
    {
        scheduler.waitAll();
    }

    void mark(const std::string& label, const std::string& detail)
    {
        const auto snapshot = impl->getStateForTest();
        trace.push_back(fmt::format("{} -> {}[{}] {}",
                                    label,
                                    stateName(snapshot.state),
                                    fmt::join(snapshot.affectedInputs, ","),
                                    detail));
    }
};

// --- Cold start ---------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, ColdStartStaggeredConnects)
{
    addSignal();
    addSignal();
    addSignal();
    buildFromPorts();

    probe("built");
    connect(0);
    probe("connect 0");
    connect(1);
    probe("connect 1");
    connect(2);
    probe("connect 2");
    readEvents("read initial events");
    sendAll();
    probe("data on all");
    readData("read a block", 10);

    expectTrace({
        // Contract item 2: connections are the second rung, so an unconnected used input outranks
        // the missing descriptors and the missing data underneath it
        "built -> WaitingForConnections[0,1,2] avail=0",
        "connect 0 -> WaitingForConnections[1,2] avail=0",
        "connect 1 -> WaitingForConnections[2] avail=0",
        // The last connect completes the set; every input's initial descriptor event is queued, so
        // events (rung 4/5) preempt the descriptors rung even though the descriptors are the reason
        "connect 2 -> EventPending[0,1,2] avail=0",
        "read initial events -> WaitingForData[0,1,2] status=Event events=3",
        "data on all -> Synchronized[] avail=10",
        "read a block -> Synchronized[] status=Ok count=10",
    });
}

TEST_P(MultiReaderStateTest, NoCallbackUntilEveryUsedInputHasSignal)
{
    // Contract item 4, both halves. While a used input has no signal, every slot's event bit is
    // suppressed, so the events already queued on the connected inputs cannot fire the callback;
    // and while a connect handshake is in flight (the window between connected() and the descriptor
    // packet, which every connect passes through) the suppression holds for the same reason. The
    // consumer therefore sees one wake with every input's initial event at once, never a partial one.
    addSignal();
    addSignal();
    addSignal();
    buildFromPorts();

    std::atomic<int> callbacks{0};
    multi.setOnDataAvailable([&callbacks] { ++callbacks; });

    connect(0);
    EXPECT_EQ(callbacks.load(), 0);
    connect(1);
    EXPECT_EQ(callbacks.load(), 0);

    connect(2);
    // At least one: slotConnected requests an evaluation of its own on top of the descriptor
    // packet's, and both legitimately observe the now-open gate
    EXPECT_GE(callbacks.load(), 1);

    const auto status = readEvents("read initial events");
    EXPECT_EQ(status.getEventPackets().getCount(), 3u);

    expectTrace({
        "read initial events -> WaitingForData[0,1,2] status=Event events=3",
    });
}

// --- Events: leading vs buried ------------------------------------------------------------------

TEST_P(MultiReaderStateTest, BuriedEventSurfacesOnReadNotOnQuery)
{
    // Contract item 6: the query path records a buried event for the gate but does not run the
    // ladder for it, so the reader stays Synchronized and simply reports nothing available past the
    // event; only a read escalates and transitions to EventPending.
    addSignal();
    addSignal();
    buildFromSignals();

    probe("built");
    readEvents("read initial events");
    sendAll();
    probe("data on both");

    // A descriptor change on input 0 lands behind its ten buffered samples
    setValueSampleType(0, SampleType::Float32);
    probe("buried event");
    readData("read up to the event", 10);
    probe("event now leading");
    readEvents("read the event");
    sendAll();
    probe("resynchronized");

    expectTrace({
        // Construction adopts the initial descriptor events (Input::replayMissedPortCallbacks),
        // so a signal-built reader starts out with events pending, not WaitingForDescriptors
        "built -> EventPending[0,1] avail=0",
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        // Still Synchronized, and the ten samples in front of the event are still available
        "buried event -> Synchronized[] avail=10",
        "read up to the event -> Synchronized[] status=Ok count=10",
        // The event is leading now, but this is the query path: no ladder, no EventPending - just
        // nothing available
        "event now leading -> Synchronized[] avail=0",
        "read the event -> WaitingForData[0,1] status=Event events=1",
        "resynchronized -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, LeadingEventPreemptsEveryRungBelowIt)
{
    // The same descriptor change with nothing buffered: the event leads immediately, so even the
    // query path's escalation lands on EventPending.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    setValueSampleType(0, SampleType::Float32);
    probe("leading event");
    readEvents("read the event");
    sendAll();
    probe("synchronized");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "leading event -> EventPending[0] avail=0",
        "read the event -> WaitingForData[0,1] status=Event events=1",
        "synchronized -> Synchronized[] avail=10",
    });
}

// --- Incompatible descriptors -------------------------------------------------------------------

TEST_P(MultiReaderStateTest, IncompatibleDescriptorAndRecovery)
{
    // Rung 7 (local validity) with a value type that cannot be converted to the read type, and the
    // recovery a later convertible descriptor brings. Incompatible is recoverable: the reader
    // itself stays valid throughout.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    setValueSampleType(0, SampleType::ComplexFloat64);
    probe("bad descriptor queued");
    readEvents("apply bad descriptor");
    probe("failure persists");

    EXPECT_TRUE(multi.asPtr<IReaderConfig>().getIsValid());

    setValueSampleType(0, SampleType::Float64);
    probe("corrective descriptor queued");
    readEvents("apply corrective descriptor");
    sendAll();
    probe("recovered");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        // The descriptor change is an event first; the validity rung only sees it once it is applied
        "bad descriptor queued -> EventPending[0] avail=0",
        "apply bad descriptor -> Incompatible[0] status=Event events=1",
        // Level-triggered: re-deriving from unchanged ground truth reproduces the same verdict
        "failure persists -> Incompatible[0] avail=0",
        "corrective descriptor queued -> EventPending[0] avail=0",
        "apply corrective descriptor -> WaitingForData[0,1] status=Event events=1",
        "recovered -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, RequiredRateNotDivisibleIsIncompatible)
{
    // Rung 8 (the cross-input model) rather than rung 7: each input is individually readable, the
    // required common rate is what cannot be satisfied. Same substate, different producer - the
    // refactor must keep both reachable.
    addSignal();
    buildFromSignals(baseBuilder().setRequiredCommonSampleRate(3));

    readEvents("read initial events");
    probe("required rate rejected");

    expectTrace({
        // The status says Event because events were returned, but the state is already Incompatible:
        // readEventsLocked re-evaluates after popping, and the model rung (8) sits above the data
        // rung (10), so the failure is reached without ever passing through WaitingForData
        "read initial events -> Incompatible[0] status=Event events=1",
        "required rate rejected -> Incompatible[0] avail=0",
    });
}

// --- Alignment ----------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, AlignmentIsIterativeAndSynchronizingIsTransient)
{
    // Rung 11: the alignment step is iterative. The start tick is chosen from the latest first
    // sample, and an input that has not received the packets to reach it yet leaves the reader in
    // Synchronizing - not WaitingForData (it has data) and not a failure (nothing is wrong).
    //
    // Two properties worth pinning:
    //
    //  * Synchronizing cannot survive a re-derivation. A NeedMoreData attempt discards every packet
    //    of the lagging input that lies below the target, so the input is empty afterwards and the
    //    next evaluation stops one rung earlier, at WaitingForData. Only a peek at the state the
    //    attempt left behind can see it - which is why the substate is recorded here without
    //    re-deriving.
    //  * The target is latched across those rounds (SynchronizationManager::pendingCandidate). Were
    //    it recomputed, each round would derive a later start from the cursors the previous round
    //    already advanced, and the reader would synchronize late.
    addSignal();
    addSignal();
    signals[1].nextTick = 30;  // input 1 starts three packets ahead of input 0
    buildFromSignals();

    readEvents("read initial events");

    send(1);
    peek("input 1 delivers first");
    send(0);
    peek("first alignment attempt");
    probe("re-derived after the attempt");

    send(0);
    peek("second attempt");
    send(0);
    peek("third attempt");
    send(0);
    peek("start reached");
    probe("synchronized");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "input 1 delivers first -> WaitingForData[0] (peek)",
        // Both inputs have data now, so the attempt runs and picks tick 30; input 0 has nothing at
        // or past it, which is NeedMoreData, not a failure
        "first alignment attempt -> Synchronizing[0] (peek)",
        // The attempt consumed input 0's below-target packets, so re-deriving from ground truth now
        // stops at the data rung. Synchronizing is a transient the level-triggered machine cannot
        // observe twice
        "re-derived after the attempt -> WaitingForData[0] avail=0",
        "second attempt -> Synchronizing[0] (peek)",
        "third attempt -> Synchronizing[0] (peek)",
        // Tick 30 stayed the target throughout, so the aligned start is 30 and both inputs deliver
        // ten samples from there
        "start reached -> Synchronized[] (peek)",
        "synchronized -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, SynchronizationDistanceExceededAndRemedy)
{
    // Rung 11's failure exit: the inputs start ten seconds apart with a five second threshold. The
    // early input is named, the reader stays active and valid, and - unlike the Incompatible paths -
    // no data is dropped to surface a buried event, because each input's data is individually fine.
    // The documented remedy is to exclude an input, which is what recovers here.
    addSignal(12000);
    addSignal(2000);
    signals[1].nextTick = 10000;  // 10 s at the 1/1000 s resolution

    buildFromSignals(baseBuilder().setMaxSynchronizationDistance(Ratio(5, 1)));

    readEvents("read initial events");
    sendAll();
    probe("ten seconds apart");

    EXPECT_TRUE(multi.getActive());
    EXPECT_TRUE(multi.asPtr<IReaderConfig>().getIsValid());

    setInputUsed(0, false);
    probe("early input excluded");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "ten seconds apart -> SynchronizationFailed[0] avail=0",
        // One used input left, so there is no distance to exceed
        "early input excluded -> Synchronized[] avail=2000",
    });
}

// --- Data loss ----------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, DataLossOnlyOnceTheInputCannotContribute)
{
    // Contract item 12, first half: the loss is in-band. An input whose producer went silent after
    // delivering a readable block keeps the reader Synchronized until that block is consumed.
    addSignal();
    addSignal();
    buildFromSignals(baseBuilder().setDataLossTimeout(Ratio(10, 1)));
    useVirtualClock();

    readEvents("read initial events");
    sendAll();
    probe("data on both");

    advanceClock(std::chrono::seconds(12));
    probe("deadline missed, block buffered");
    readData("drain the block", 10);
    probe("deadline missed, nothing buffered");

    send(1);
    probe("input 1 recovers");
    send(0);
    probe("input 0 recovers");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        // Both deadlines have expired, but the buffered pre-loss samples are still readable
        "deadline missed, block buffered -> Synchronized[] avail=10",
        "drain the block -> Synchronized[] status=Ok count=10",
        "deadline missed, nothing buffered -> DataLost[0,1] avail=0",
        // Recovery is per input, on its next packet
        "input 1 recovers -> DataLost[0] avail=0",
        "input 0 recovers -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, DataLossWithSubBlockResidual)
{
    // Contract item 12, second half: "can no longer contribute" is < one aligned block, not empty.
    // Input 0 runs at 750 Hz (divider 2) and input 1 at 500 Hz (divider 3), so the aligned block is
    // six common-rate samples. After sending 4 and 3 native samples (8 and 9 common) and reading one
    // block, each input keeps one native sample - 2 and 3 common-rate samples, unreadable because a
    // block-aligned read floors to whole blocks. Gating the loss on an empty queue would stall the
    // reader in Synchronized forever, never surfacing it.
    addSignal(4, Ratio(1, 1500), LinearDataRule(2, 0));
    addSignal(3, Ratio(1, 1500), LinearDataRule(3, 0));
    buildFromSignals(baseBuilder().setDataLossTimeout(Ratio(10, 1)));
    useVirtualClock();

    readEvents("read initial events");
    sendAll();
    probe("data on both");
    readData("read one block", 6);

    advanceClock(std::chrono::seconds(12));
    probe("deadline missed, residual buffered");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=6",
        "read one block -> Synchronized[] status=Ok count=6",
        "deadline missed, residual buffered -> DataLost[0,1] avail=0",
    });
}

// --- setActive: the second arm of the ladder ----------------------------------------------------

TEST_P(MultiReaderStateTest, InactiveArmHasItsOwnReducedVocabulary)
{
    // The inactive arm is the fork the refactor turns into a class boundary, and its vocabulary is
    // exactly {Inactive, EventPending}: it consumes and reports events, but it never validates
    // descriptors. A descriptor that makes an input unreadable is therefore applied silently while
    // inactive and only becomes Incompatible on reactivation.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    sendAll();
    probe("data on both");

    setActive(false);
    probe("deactivated");

    setValueSampleType(0, SampleType::ComplexFloat64);
    probe("event while inactive");
    readEvents("apply bad descriptor while inactive");
    probe("still inactive");

    setActive(true);
    probe("reactivated");

    setValueSampleType(0, SampleType::Float64);
    readEvents("apply corrective descriptor");
    sendAll();
    probe("recovered");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        // Deactivation drops the buffered data (it is meaningless once the stream pauses)
        "deactivated -> Inactive[] avail=0",
        // Descriptor events are enqueued regardless of the active flag and must still surface
        "event while inactive -> EventPending[0] avail=0",
        // The bad descriptor is now applied - and the inactive arm has no validity rung to notice
        "apply bad descriptor while inactive -> Inactive[] status=Event events=1",
        "still inactive -> Inactive[] avail=0",
        // Reactivation is what runs the validity rung over it
        "reactivated -> Incompatible[0] avail=0",
        "apply corrective descriptor -> WaitingForData[0,1] status=Event events=1",
        "recovered -> Synchronized[] avail=10",
    });
}

// --- setInputUsed -------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, UnusedInputStaysObservableAndRecovers)
{
    // An unused input is excluded from reading but stays observable: its queued events are still
    // adopted and reported as its per-input state, which is the recovery signal a consumer answers
    // with setInputUsed(id, true). Re-enabling restarts it from the live stream: queued data is
    // dropped, descriptor changes are kept.
    //
    // Packets are sent to both signals throughout - the unused input's port is inactive, so its data
    // is dropped at the connection, which keeps the two signals' domains aligned for the resync at
    // the end without giving the unused input anything to read.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    sendAll();
    readData("read the block", 10);

    setInputUsed(1, false);
    sendAll();
    probe("input 1 unused");

    setValueSampleType(1, SampleType::Float32);
    const auto status = readData("read while input 1 is unused", 10);
    // The unused input's event is visible per input while the reader keeps delivering
    EXPECT_EQ(inputState(status, 1), InputState::Event);
    EXPECT_EQ(inputState(status, 0), InputState::Ok);

    setInputUsed(1, true);
    probe("input 1 used again");
    readEvents("read the kept event");
    sendAll();
    probe("resynchronized");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "read the block -> Synchronized[] status=Ok count=10",
        // One used input left, and its port keeps delivering
        "input 1 unused -> Synchronized[] avail=10",
        "read while input 1 is unused -> Synchronized[] status=Ok count=10",
        // The event queued while unused survives the re-enable and preempts everything below it
        "input 1 used again -> EventPending[1] avail=0",
        "read the kept event -> WaitingForData[0,1] status=Event events=1",
        "resynchronized -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, ExcludingAnInputDropsWhatItHasAdopted)
{
    // Why the exclusion drops immediately instead of on the way back in: InputState::Event is derived
    // from hasPendingEvents(), which is leading-only, so an event arriving behind the input's own
    // leftover data would be invisible for as long as that data sits in front of it - and the event
    // is the only thing an unused input still has to say. The data is unreadable either way (the
    // re-enable restarts from the live stream), so it goes at exclusion time and the event leads.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    sendAll();
    probe("data on both");

    // Input 1 is excluded with ten adopted samples still queued
    setInputUsed(1, false);
    setValueSampleType(1, SampleType::Float32);
    const auto status = readData("read while input 1 is unused", 10);
    EXPECT_EQ(inputState(status, 1), InputState::Event);

    setInputUsed(1, true);
    probe("input 1 used again");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        "read while input 1 is unused -> Synchronized[] status=Ok count=10",
        "input 1 used again -> EventPending[1] avail=0",
    });
}

TEST_P(MultiReaderStateTest, ConnectingAnUnusedInputLeavesTheReaderSynchronized)
{
    // A signal appearing on an input that is excluded from reading cannot change a model built from the
    // used inputs, so ReadyState answers this connect without invalidating anything
    // (ReadyState::slotConnected). What the consumer sees is the reader carrying on, with the new
    // input's descriptor event reported as that input's state - the recovery signal it would answer
    // with setInputUsed(id, true).
    addSignal();
    addSignal();
    addSignal();
    buildFromPorts();

    connect(0);
    connect(1);
    setInputUsed(2, false);
    readEvents("read initial events");

    send(0);
    send(1);
    probe("data on the used inputs");

    connect(2);
    probe("unused input connected");

    const auto status = readData("read the block", 10);
    EXPECT_EQ(inputState(status, 2), InputState::Event);
    EXPECT_EQ(inputState(status, 0), InputState::Ok);

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on the used inputs -> Synchronized[] avail=10",
        // Still synchronized, still the same ten samples: the connect did not disturb the alignment
        "unused input connected -> Synchronized[] avail=10",
        "read the block -> Synchronized[] status=Ok count=10",
    });
}

// --- Disconnect ---------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, DisconnectDropsToWaitingForConnections)
{
    addSignal();
    addSignal();
    buildFromPorts();

    connect(0);
    connect(1);
    readEvents("read initial events");
    sendAll();
    readData("read the block", 10);

    disconnect(1);
    probe("input 1 disconnected");

    connect(1);
    probe("input 1 reconnected");
    readEvents("read the reconnect event");
    sendAll();
    probe("resynchronized");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "read the block -> Synchronized[] status=Ok count=10",
        "input 1 disconnected -> WaitingForConnections[1] avail=0",
        // The reconnect enqueues the signal's descriptor onto the new connection, so there is an
        // event to consume before data can flow again
        "input 1 reconnected -> EventPending[1] avail=0",
        "read the reconnect event -> WaitingForData[0,1] status=Event events=1",
        "resynchronized -> Synchronized[] avail=10",
    });
}

TEST_P(MultiReaderStateTest, DisconnectDiscardsWhatTheSlotHasAdopted)
{
    // A disconnect throws away everything the slot adopted from that connection - queued samples,
    // pending events and the cached descriptors (QueueReader::updateConnection, keyed on connection
    // identity). The port may be reconnected to a DIFFERENT signal, so nothing about the old one may
    // survive: the reconnect goes through the descriptor handshake again rather than silently
    // resuming on stale samples, which is what the trace below pins - "input 1 reconnected" is
    // EventPending, not Synchronized.
    addSignal();
    addSignal();
    buildFromPorts();

    connect(0);
    connect(1);
    readEvents("read initial events");
    sendAll();
    probe("data on both");

    disconnect(1);
    probe("input 1 disconnected");
    connect(1);
    probe("input 1 reconnected");
    readEvents("read the reconnect event");
    sendAll();
    probe("resynchronized");

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        "input 1 disconnected -> WaitingForConnections[1] avail=0",
        // The new connection's descriptor event, with nothing of the old signal left in front of it
        "input 1 reconnected -> EventPending[1] avail=0",
        // Input 0 kept its unread block; input 1 starts over from nothing
        "read the reconnect event -> WaitingForData[1] status=Event events=1",
        "resynchronized -> Synchronized[] avail=10",
    });
}

// --- Error --------------------------------------------------------------------------------------

TEST_P(MultiReaderStateTest, ErrorIsTerminalAndOutranksEverything)
{
    // Contract item 1: invalid <=> Error, checked before inactivity and everything else, and
    // terminal - no trigger can leave it.
    addSignal();
    addSignal();
    buildFromSignals();

    readEvents("read initial events");
    sendAll();
    probe("data on both");

    multi.asPtr<IReaderConfig>().markAsInvalid();
    probe("marked invalid");

    sendAll();
    probe("packets after error");
    readData("read after error", 10);
    setActive(false);
    probe("deactivated after error");
    setActive(true);
    probe("reactivated after error");

    EXPECT_FALSE(multi.asPtr<IReaderConfig>().getIsValid());

    expectTrace({
        "read initial events -> WaitingForData[0,1] status=Event events=2",
        "data on both -> Synchronized[] avail=10",
        "marked invalid -> Error[] avail=0",
        "packets after error -> Error[] avail=0",
        "read after error -> Error[] status=Fail count=0",
        // Inactivity does not outrank Error - the invalid check is the first rung
        "deactivated after error -> Error[] avail=0",
        "reactivated after error -> Error[] avail=0",
    });
}

INSTANTIATE_TEST_SUITE_P(StateDerivation,
                         MultiReaderStateTest,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info)
                         { return info.param ? "Exhaustive" : "PerBehaviour"; });
