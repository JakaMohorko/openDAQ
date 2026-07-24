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

TEST_F(MultiReaderInputTest, LastPacketArrivalUpdates)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);

    ASSERT_EQ(slot->getLastPacketArrival(), Input::SteadyClock::time_point{});

    const auto beforeSend = Input::SteadyClock::now();
    port.connect(signal);
    sendDataPacket(5, 100);

    ASSERT_GE(slot->getLastPacketArrival(), beforeSend);
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

    // Simulate the owner publishing a steady Synchronized gate: no wake-on-any, ready at 10
    // native samples, empty adopted basis.
    slot->setWakeOnAnyPacket(false);
    slot->setReadyThresholdNative(10);
    slot->publishGateBasis(0, false);
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

TEST_F(MultiReaderInputTest, ProducerForcesEvaluationOnConnectionEventPacket)
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
    slot->setReadyThresholdNative(10);
    slot->publishGateBasis(0, false);
    slot->gateFlags().setReady(false);
    slot->gateFlags().setEvent(false);

    listener.forcedCount = 0;

    // Events are owner-managed: a producer that sees an event packet on the connection (via the
    // O(1) hasEventPacket counter) does NOT raise the event flag itself - that would race the
    // owner and risk a stale flag. It forces a full evaluation, which sets event flags under the
    // state lock. So the slot's own event flag stays down; the listener is forced instead.
    signal.setDescriptor(DataDescriptorBuilder().setSampleType(SampleType::Int32).build());

    ASSERT_FALSE(slot->gateFlags().event());
    ASSERT_GE(listener.forcedCount.load(), 1);
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
    slot->setReadyThresholdNative(10);
    slot->publishGateBasis(0, false);

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
