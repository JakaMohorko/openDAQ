#include <gtest/gtest.h>

#include <opendaq/input_port_factory.h>
#include <opendaq/input_slot.h>
#include <opendaq/packet_factory.h>
#include "reader_common.h"

#include <atomic>

// Records the semantic notifications an InputSlot forwards to its owner
struct RecordingSlotListener final : daq::IInputSlotListener
{
    std::atomic<int> acceptsCount{0};
    std::atomic<int> connectedCount{0};
    std::atomic<int> disconnectedCount{0};
    std::atomic<int> packetPendingCount{0};
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

    void slotPacketPending(daq::SizeT slotIndex) override
    {
        ++packetPendingCount;
        lastIndex = slotIndex;
    }
};

class InputSlotTest : public ReaderTest<>
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

    void createSlot(SizeT index, const InputPortConfigPtr& port, IInputSlotListener* listener, bool globalIdFromSignal = false)
    {
        slotObj = createWithImplementation<IInputPortNotifications, InputSlot>(
            index, port, SampleType::Float64, SampleType::Int64, ReadMode::Scaled, loggerComponent, listener, globalIdFromSignal);
        slot = static_cast<InputSlot*>(slotObj.getObject());
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
    ObjectPtr<IInputPortNotifications> slotObj;
    InputSlot* slot{};
};

TEST_F(InputSlotTest, InitialConnectedStateFromPort)
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

TEST_F(InputSlotTest, ConnectedNotificationUpdatesStateAndForwards)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(3, port, &listener);

    port.connect(signal);

    ASSERT_EQ(listener.connectedCount, 1);
    ASSERT_EQ(listener.lastIndex, 3u);
    ASSERT_TRUE(slot->isConnected());
}

TEST_F(InputSlotTest, DisconnectNotificationForwards)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    port.disconnect();

    ASSERT_EQ(listener.disconnectedCount, 1);
    ASSERT_FALSE(slot->isConnected());
}

TEST_F(InputSlotTest, PacketPendingFiresOncePerCycle)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);
    port.connect(signal);

    // The initial descriptor event may already have set the pending bit - start a fresh cycle
    slot->clearPacketPending();
    listener.packetPendingCount = 0;

    sendDataPacket(5, 100);
    sendDataPacket(5, 105);
    sendDataPacket(5, 110);

    ASSERT_EQ(listener.packetPendingCount, 1);
    ASSERT_TRUE(slot->isPacketPending());

    ASSERT_TRUE(slot->clearPacketPending());
    ASSERT_FALSE(slot->isPacketPending());

    sendDataPacket(5, 115);
    ASSERT_EQ(listener.packetPendingCount, 2);

    ASSERT_TRUE(slot->clearPacketPending());
    ASSERT_FALSE(slot->clearPacketPending());
}

TEST_F(InputSlotTest, LastPacketArrivalUpdates)
{
    RecordingSlotListener listener;
    auto port = createPort();
    createSlot(0, port, &listener);

    ASSERT_EQ(slot->getLastPacketArrival(), InputSlot::SteadyClock::time_point{});

    const auto beforeSend = InputSlot::SteadyClock::now();
    port.connect(signal);
    sendDataPacket(5, 100);

    ASSERT_GE(slot->getLastPacketArrival(), beforeSend);
}

TEST_F(InputSlotTest, AcceptsSignalForwardedAndDefaultAccept)
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

TEST_F(InputSlotTest, InputIdFromSignalOrPort)
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

TEST_F(InputSlotTest, DetachListenerStopsNotifications)
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

TEST_F(InputSlotTest, RebindConnectionDrainsQueue)
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

TEST_F(InputSlotTest, UsedFlagAndPortActive)
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
