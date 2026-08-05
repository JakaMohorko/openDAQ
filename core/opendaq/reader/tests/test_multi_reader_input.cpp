#include <gtest/gtest.h>

#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader/callback_gate.h>
#include <opendaq/multi_reader/input.h>
#include <opendaq/packet_factory.h>
#include "reader_common.h"

using namespace daq::multi_reader;

#include <atomic>
#include <memory>

// Records the semantic notifications an Input forwards to its owner
struct RecordingSlotListener final : daq::multi_reader::IInputListener
{
    std::atomic<int> acceptsCount{0};
    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};
    std::atomic<int> packetPendingCount{0};
    std::atomic<int> forcedCount{0};
    std::atomic<daq::SizeT> lastIndex{static_cast<daq::SizeT>(-1)};
    bool acceptSignals = true;

    bool slotAcceptsSignal(daq::SizeT slotIndex, const daq::SignalPtr& /*signal*/) override
    {
        ++acceptsCount;
        lastIndex = slotIndex;
        return acceptSignals;
    }

    void slotConnected(daq::SizeT slotIndex) override
    {
        ++connectedCount;
        lastIndex = slotIndex;
    }

    void slotDisconnected(daq::SizeT slotIndex) override
    {
        ++disconnectedCount;
        lastIndex = slotIndex;
    }

    void slotPacketReceived(daq::SizeT slotIndex, bool forceEvaluation) override
    {
        ++packetPendingCount;
        if (forceEvaluation)
            ++forcedCount;
        lastIndex = slotIndex;
    }
};

class MultiReaderInputTest : public ReaderTest<>
{
public:
    using Super = ReaderTest<>;

protected:
    void SetUp() override
    {
        Super::SetUp();

        domainSignal = Signal(context, nullptr, "timeSig");
        signal.setDomainSignal(domainSignal);

        domainSignal.setDescriptor(DataDescriptorBuilder()
                                       .setSampleType(SampleType::Int64)
                                       .setTickResolution(Ratio(1, 1000))
                                       .setOrigin("1970-01-01T00:00:00+00:00")
                                       .setRule(LinearDataRule(1, 0))
                                       .setUnit(Unit("s", -1, "second", "time"))
                                       .build());
        signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Float64).build());
    }

    InputPortConfigPtr createPort(const std::string& localId = "port")
    {
        auto port = InputPort(context, nullptr, localId);
        // Synchronous listener notification keeps the tests deterministic
        port.setNotificationMethod(PacketReadyNotification::SameThread);
        return port;
    }

    void createSlot(SizeT index, const InputPortConfigPtr& port, IInputListener* listener, bool globalIdFromSignal = false)
    {
        slotObj = createWithImplementation<IInputPortNotifications, Input>(
            index, port, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, listener, globalIdFromSignal, gate);
        slot = static_cast<Input*>(slotObj.getObject());
        port.setListener(slotObj);
    }

    void sendDataPacket(SizeT sampleCount, Int offset)
    {
        const auto domainPacket = DataPacket(domainSignal.getDescriptor(), sampleCount, offset);
        const auto valuePacket = DataPacketWithDomain(domainPacket, signal.getDescriptor(), sampleCount);
        auto* values = static_cast<double*>(valuePacket.getRawData());
        for (SizeT i = 0; i < sampleCount; ++i)
            values[i] = static_cast<double>(i);
        domainSignal.sendPacket(domainPacket);
        signal.sendPacket(valuePacket);
    }

protected:
    SignalConfigPtr domainSignal;
    std::shared_ptr<CallbackGate> gate{std::make_shared<CallbackGate>()};
    ObjectPtr<IInputPortNotifications> slotObj;
    Input* slot{};
};

TEST_F(MultiReaderInputTest, InitialConnectedStateFromPort)
{
    RecordingSlotListener listener;

    auto unconnectedPort = createPort("unconnected");
    createSlot(0, unconnectedPort, &listener);
    ASSERT_FALSE(slot->isConnected());

    auto connectedPort = createPort("connected");
    connectedPort.connect(signal);
    createSlot(1, connectedPort, &listener);
    ASSERT_TRUE(slot->isConnected());
}

TEST_F(MultiReaderInputTest, ConnectedNotificationUpdatesStateAndForwards)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(3, port, &listener);

    port.connect(signal);

    ASSERT_EQ(listener.connectedCount, 1);
    ASSERT_EQ(listener.lastIndex, 3u);
    ASSERT_TRUE(slot->isConnected());
}

TEST_F(MultiReaderInputTest, DisconnectNotificationForwards)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    port.disconnect();

    ASSERT_EQ(listener.disconnectedCount, 1);
    ASSERT_FALSE(slot->isConnected());
}

TEST_F(MultiReaderInputTest, PacketNotificationPerPacketAndPendingBit)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    // The initial descriptor event may already have set the pending bit - start a fresh cycle
    slot->clearPacketPending();
    listener.packetPendingCount = 0;

    // Every packet notifies (the owner coalesces); the pending bit arms once per cycle
    sendDataPacket(5, 100);
    sendDataPacket(5, 105);
    sendDataPacket(5, 110);

    ASSERT_EQ(listener.packetPendingCount, 3);  // one per value packet on the connected port
    ASSERT_TRUE(slot->isPacketPending());

    ASSERT_TRUE(slot->clearPacketPending());
    ASSERT_FALSE(slot->isPacketPending());

    sendDataPacket(5, 115);
    ASSERT_TRUE(slot->isPacketPending());

    ASSERT_TRUE(slot->clearPacketPending());
    ASSERT_FALSE(slot->clearPacketPending());
}

TEST_F(MultiReaderInputTest, AcceptsSignalForwardedAndDefaultAccept)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);

    listener.acceptSignals = false;
    ASSERT_FALSE(port.acceptsSignal(signal));
    ASSERT_EQ(listener.acceptsCount, 1);

    listener.acceptSignals = true;
    ASSERT_TRUE(port.acceptsSignal(signal));
    ASSERT_EQ(listener.acceptsCount, 2);

    // Without a listener the slot accepts by default
    slot->detachListener();
    ASSERT_TRUE(port.acceptsSignal(signal));
    ASSERT_EQ(listener.acceptsCount, 2);
}

TEST_F(MultiReaderInputTest, InputIdFromSignalOrPort)
{
    RecordingSlotListener listener;

    auto portIdPort = createPort("fromPort");
    portIdPort.connect(signal);
    createSlot(0, portIdPort, &listener, false);
    ASSERT_EQ(slot->getInputId(), portIdPort.getGlobalId());

    auto signalIdPort = createPort("fromSignal");
    createSlot(1, signalIdPort, &listener, true);
    // No signal connected yet - falls back to the port id
    ASSERT_EQ(slot->getInputId(), signalIdPort.getGlobalId());

    signalIdPort.connect(signal);
    ASSERT_EQ(slot->getInputId(), signal.getGlobalId());
}

TEST_F(MultiReaderInputTest, DetachListenerStopsNotifications)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);

    slot->detachListener();

    port.connect(signal);
    sendDataPacket(5, 100);

    ASSERT_EQ(listener.connectedCount, 0);
    ASSERT_EQ(listener.packetPendingCount, 0);
    // State still tracks reality; only the forwarding stops
    ASSERT_TRUE(slot->isConnected());
    ASSERT_TRUE(slot->isPacketPending());
}

TEST_F(MultiReaderInputTest, RebindConnectionDrainsQueue)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);

    // Connected after slot construction: the QueueReader still points at the null connection
    port.connect(signal);
    sendDataPacket(5, 100);

    slot->rebindConnection();

    auto& queueReader = slot->getQueueReader();
    ASSERT_TRUE(queueReader.hasPendingEvents());  // initial descriptor event
    queueReader.popFrontEvent();
    ASSERT_TRUE(queueReader.isValid());
    ASSERT_EQ(queueReader.getAvailableSamples(), 5u);
}

TEST_F(MultiReaderInputTest, AvailabilityAndMinimumAcrossTheSlotLifecycle)
{
    // The slot's availability is the adopted queue plus whatever the connection still holds, and
    // hasDataToRead() is that against a settable minimum. This walks the surface: the default
    // minimum, the two halves of the count, the owner's publish carrying one into the other, the
    // divider conversion, the event boundary, and the never-readable sentinel.
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    ASSERT_TRUE(reader.hasPendingEvents());  // initial descriptor event
    reader.popFrontEvent();
    publishSlotAvailability(*slot);

    // At construction any single sample is enough to read.
    ASSERT_EQ(slot->getMinReadCount(), 1u);
    ASSERT_EQ(slot->getAvailableSamples(), 0u);
    ASSERT_FALSE(slot->hasDataToRead());

    // The connection half: samples count before anything adopts them, which is what lets the
    // producer decide whether an arrival is worth an evaluation without touching the queue.
    sendDataPacket(5, 100);
    ASSERT_EQ(reader.getAvailableSamples(), 0u);  // nothing adopted yet
    ASSERT_EQ(slot->getAvailableSamples(), 5u);
    ASSERT_TRUE(slot->hasDataToRead());

    // Adopting moves the same samples from one half to the other. The count only survives the move
    // because the owner republishes: the producer cannot read the queue, so an owner pass that
    // moves samples and forgets to publish would strand the slot reporting the pre-drain basis.
    reader.drain();
    publishSlotAvailability(*slot);
    ASSERT_EQ(reader.getAvailableSamples(), 5u);
    ASSERT_EQ(slot->getAvailableSamples(), 5u);

    // Raising the minimum is the whole difference between the establishing and synchronized gate
    // policies - one number, not a second code path.
    slot->setMinReadCount(10);
    ASSERT_EQ(slot->getMinReadCount(), 10u);
    ASSERT_FALSE(slot->hasDataToRead());

    // Crossing it from the connection side, without adopting. 5 adopted + 5 queued.
    sendDataPacket(5, 105);
    ASSERT_EQ(slot->getAvailableSamples(), 10u);
    ASSERT_TRUE(slot->hasDataToRead());

    // Counts and minimum are both common-rate, so a divider rescales both: the same 10 native
    // samples now read as 20, and the same minimum of 10 demands only 5 native ones.
    reader.setSampleRateDivider(2);
    publishSlotAvailability(*slot);
    slot->setMinReadCount(10);
    ASSERT_EQ(slot->getMinReadCount(), 10u);
    ASSERT_EQ(slot->getAvailableSamples(), 20u);
    ASSERT_TRUE(slot->hasDataToRead());

    // An event BURIED on the connection bounds the count at itself, but the data queued in front of
    // it stays readable and must still count. Connection::hasEventPacket() would say "yes" here -
    // it means "an event is queued somewhere", not "the queue starts with one" - so it cannot be
    // used to decide this.
    signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Int32).build());
    ASSERT_TRUE(port.getConnection().hasEventPacket());
    ASSERT_EQ(slot->getAvailableSamples(), 20u);  // unchanged: the event is behind all 10 samples

    // Adopting pulls the event into the adopted queue (still buried behind the 10 samples). Now the
    // basis is what bounds the count, and the whole connection queue is behind that event.
    reader.drain();
    publishSlotAvailability(*slot);
    ASSERT_EQ(slot->getAvailableSamples(), 20u);

    slot->setMinReadCount(40);
    ASSERT_FALSE(slot->hasDataToRead());

    // Nothing can meet NeverReadable, and it is a sentinel rather than a count, so the divider must
    // not rescale it.
    slot->setMinReadCount(Input::NeverReadable);
    ASSERT_EQ(slot->getMinReadCount(), Input::NeverReadable);
    ASSERT_FALSE(slot->hasDataToRead());
}

TEST_F(MultiReaderInputTest, MinimumConvertsToNativeRoundingUp)
{
    // A minimum of one common-rate sample on an input running at half the common rate is still one
    // native sample to wait for. Truncating the conversion would store zero, and a zero minimum
    // makes hasDataToRead() true on an empty slot - an input that is permanently, silently ready.
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    reader.popFrontEvent();  // initial descriptor event
    reader.setSampleRateDivider(2);
    publishSlotAvailability(*slot);

    slot->setMinReadCount(1);
    ASSERT_EQ(slot->getAvailableSamples(), 0u);
    ASSERT_FALSE(slot->hasDataToRead());

    sendDataPacket(1, 100);
    ASSERT_EQ(slot->getAvailableSamples(), 2u);  // one native sample, common-rate equivalent
    ASSERT_TRUE(slot->hasDataToRead());
}

TEST_F(MultiReaderInputTest, UnconnectedSlotHasNothingAvailable)
{
    RecordingSlotListener listener;
    auto port = createPort("unconnected");
    createSlot(0, port, &listener);

    ASSERT_FALSE(slot->isConnected());
    ASSERT_EQ(slot->getAvailableSamples(), 0u);
    ASSERT_FALSE(slot->hasDataToRead());
}

TEST_F(MultiReaderInputTest, ProducerForcesEvaluationInNonSteadyState)
{
    // Default (wakeOnAnyPacket == true, the pre-first-evaluation state): every packet forces
    // an evaluation regardless of the gate, preserving classic establishment liveness.
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->clearPacketPending();
    listener.packetPendingCount = 0;
    listener.forcedCount = 0;

    sendDataPacket(5, 100);
    ASSERT_EQ(listener.packetPendingCount, 1);
    ASSERT_EQ(listener.forcedCount, 1);  // forced because the owner has not gone steady yet
}

TEST_F(MultiReaderInputTest, ProducerRaisesReadyFromConnectionCountersWhenSteady)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    // Adopt the initial descriptor event so subsequent packets are pure data
    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    if (reader.hasPendingEvents())
        reader.popFrontEvent();

    // Simulate the owner publishing a steady Synchronized gate: no wake-on-any, a minimum of 10
    // samples (divider 1, so 10 native too), empty adopted basis.
    slot->setWakeOnAnyPacket(false);
    slot->setMinReadCount(10);
    slot->publishAvailability(0, false);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);

    slot->clearPacketPending();
    listener.forcedCount = 0;

    // Below threshold: 5 native samples on the connection, not adopted, ready must stay down
    sendDataPacket(5, 200);
    ASSERT_FALSE(slot->gateFlags().ready());
    ASSERT_EQ(listener.forcedCount, 0);  // steady state, gate closed -> not forced

    // Crossing the threshold: the producer raises ready from basis + connection counters
    sendDataPacket(5, 205);
    ASSERT_TRUE(slot->gateFlags().ready());
}

TEST_F(MultiReaderInputTest, ProducerRaisesEventOnAConnectionEventPacket)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    if (reader.hasPendingEvents())
        reader.popFrontEvent();

    slot->setWakeOnAnyPacket(false);
    slot->setMinReadCount(10);
    slot->publishAvailability(0, false);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);

    listener.forcedCount = 0;

    // An event arriving on the connection is something the consumer must be woken to collect. The
    // producer states that in the flag system rather than falling back to forcing an evaluation -
    // events are self-gated exactly like readiness.
    signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Int32).build());

    ASSERT_TRUE(slot->gateFlags().event());
    ASSERT_EQ(listener.forcedCount, 0);
    // One event on any input opens the shared gate - that is what wakes the consumer to collect it.
    ASSERT_TRUE(gate->isSatisfied());
}

TEST_F(MultiReaderInputTest, ProducerGateIsDataFirstAcrossAnEventsJourney)
{
    // Data-first, followed through one event's journey down the pipeline: while a servable block
    // sits in front of the event the producer raises only ready - the consumer sits at the common
    // cursor and receives events in stream order, once the data ahead of them is consumed. Only
    // when nothing servable remains before the boundary does the event become the only possible
    // wake and raise the event bit.
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    if (reader.hasPendingEvents())
        reader.popFrontEvent();

    slot->setWakeOnAnyPacket(false);
    slot->setMinReadCount(10);
    slot->publishAvailability(0, false);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);
    listener.forcedCount = 0;

    // Stage 1: a full block is readable, then the event arrives on the connection BEHIND it. The
    // block raised ready; the event stays quiet - it is in the consumer's future, and the raised
    // ready flag already holds this slot's gate contribution.
    sendDataPacket(10, 100);
    ASSERT_TRUE(slot->hasDataToRead());
    ASSERT_TRUE(slot->gateFlags().ready());

    // Same sample type, different unit: still a descriptor-change event, but the fixture's
    // sendDataPacket keeps writing Float64 samples into later packets.
    signal.setDescriptor(
        DataDescriptorBuilder().setSampleType(SampleType::Float64).setUnit(Unit("V", -1, "volt", "voltage")).build());

    ASSERT_FALSE(slot->gateFlags().event());
    ASSERT_EQ(listener.forcedCount, 0);

    // Stage 2: the owner adopted the queue and republished - the event is now in the adopted
    // basis with a servable block still in front of it. A further packet still raises only
    // ready: adopted or not, an event behind servable data is not a wake.
    slot->publishAvailability(50, true);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);

    sendDataPacket(5, 200);

    ASSERT_TRUE(slot->gateFlags().ready());
    ASSERT_FALSE(slot->gateFlags().event());
    ASSERT_EQ(listener.forcedCount, 0);

    // Stage 3: reads consumed down to a sub-minimum residual in front of the event. The residual
    // can never grow past the boundary (availability stops at it), so the event is the only
    // possible wake - now it raises, and it alone opens the shared gate.
    slot->publishAvailability(5, true);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);

    sendDataPacket(5, 300);

    ASSERT_TRUE(slot->gateFlags().event());
    ASSERT_FALSE(slot->gateFlags().ready());
    ASSERT_EQ(listener.forcedCount, 0);
    ASSERT_TRUE(gate->isSatisfied());
}

TEST_F(MultiReaderInputTest, ProducerFallsBackToForceDuringOwnerPass)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    slot->rebindConnection();
    auto& reader = slot->getQueueReader();
    if (reader.hasPendingEvents())
        reader.popFrontEvent();

    slot->setWakeOnAnyPacket(false);
    slot->setMinReadCount(10);
    slot->publishAvailability(0, false);

    slot->clearPacketPending();
    listener.forcedCount = 0;

    // An owner pass in flight makes the epoch noisy: the producer cannot trust its snapshot
    // and forces an evaluation instead of raising flags.
    {
        CallbackGate::PassGuard pass(*gate);
        sendDataPacket(5, 300);
        ASSERT_EQ(listener.forcedCount, 1);
    }
}

TEST_F(MultiReaderInputTest, UsedFlagAndPortActive)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    ASSERT_TRUE(slot->isUsed());
    slot->setUsed(false);
    ASSERT_FALSE(slot->isUsed());

    slot->setPortActive(false);
    ASSERT_FALSE(port.getActive());
    slot->setPortActive(true);
    ASSERT_TRUE(port.getActive());

    slot->setUsed(true);
    ASSERT_TRUE(slot->isUsed());
}
