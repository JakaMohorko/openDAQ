#include <gtest/gtest.h>
#include <opendaq/gmock/input_port.h>
#include <opendaq/gmock/signal.h>
#include <opendaq/packet_factory.h>
#include <opendaq/context_factory.h>
#include <opendaq/connection_factory.h>
#include <opendaq/connection_internal.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/sample_type_traits.h>
#include <opendaq/event_packet_params.h>

#include <vector>

using namespace daq;
using namespace testing;

/**
 * @brief Drain up to `count` packets through IConnectionInternal::dequeueUpTo - the batch path the
 * multi reader's QueueReader adopts with, and the only one that recounts the connection's cached
 * counters instead of decrementing them per packet.
 * @return how many packets were actually handed over.
 */
static SizeT batchDequeue(const ConnectionPtr& connection, SizeT count)
{
    const auto connectionInternal = connection.asPtr<IConnectionInternal>(true);

    std::vector<IPacket*> buffer(count, nullptr);
    SizeT dequeued = count;
    connectionInternal->dequeueUpTo(buffer.data(), &dequeued);

    // dequeueUpTo detaches, so the caller owns a reference to every packet it returned.
    for (SizeT i = 0; i < dequeued; ++i)
        PacketPtr::Adopt(buffer[i]);

    return dequeued;
}

template <class DT>
class GapCheckTest : public Test
{
protected:
    using DataType = DT;

    std::pair<DataDescriptorPtr, DataDescriptorPtr> getDescriptors(bool explicitDomainRule = false)
    {
        const auto valueDesc = DataDescriptorBuilder().setSampleType(SampleType::Float64).build();
        const auto domainDescBuilder = DataDescriptorBuilder().setSampleType(SampleTypeFromType<DT>::SampleType);

        if (!explicitDomainRule)
            domainDescBuilder.setRule(LinearDataRule(10, 2));

        return {valueDesc, domainDescBuilder.build()};
    }

    /**
     * @brief Queue up [descriptor][data 10][gap][data 10] on a gap-checking connection: the second
     * data packet starts at 200 where 100 was expected, so the connection synthesizes a gap packet
     * in front of it. Leaves samplesCnt = 20, eventPacketsCnt = 1, gapPacketsCnt = 1.
     */
    void enqueueGapSequence(const ConnectionPtr& connection)
    {
        auto [valueDesc, domainDesc] = getDescriptors();

        connection.enqueue(DataDescriptorChangedEventPacket(valueDesc, domainDesc));

        const auto domainPacket1 = DataPacket(domainDesc, 10, 0);
        connection.enqueue(DataPacketWithDomain(domainPacket1, valueDesc, 10));

        const auto domainPacket2 = DataPacket(domainDesc, 10, 200);
        connection.enqueue(DataPacketWithDomain(domainPacket2, valueDesc, 10));
    }
};

using GapCheckTypes = Types<double, int64_t, uint64_t>;

TYPED_TEST_SUITE(GapCheckTest, GapCheckTypes);

TYPED_TEST(GapCheckTest, Disabled)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(False));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [ valueDesc, domainDesc ] = this->getDescriptors();
    const auto domainPacket = DataPacket(domainDesc, 10, 0);
    const auto valuePacket = DataPacketWithDomain(domainPacket, valueDesc, 10);

    connection.enqueue(valuePacket);

    const auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, valuePacket);
}

TYPED_TEST(GapCheckTest, NoEventPacket)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();
    const auto domainPacket = DataPacket(domainDesc, 10, 0);
    const auto valuePacket = DataPacketWithDomain(domainPacket, valueDesc, 10);

    ASSERT_THROW(connection.enqueue(valuePacket), InvalidStateException);
}

TYPED_TEST(GapCheckTest, NotAvailableNoDomainPacket)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [_, desc] = this->getDescriptors();

    const auto eventPacket = DataDescriptorChangedEventPacket(desc, nullptr);
    connection.enqueue(eventPacket);

    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket);

    const auto packet1 = DataPacket(desc, 10, 0);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto packet2 = DataPacket(desc, 10, 20);
    connection.enqueue(packet2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, NoGap)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();

    const auto eventPacket = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket);

    const auto domainPacket1 = DataPacket(domainDesc, 10, 0);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto domainPacket2 = DataPacket(domainDesc, 10, 100);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, Gap)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();

    const auto eventPacket = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket);

    const auto domainPacket1 = DataPacket(domainDesc, 10, 0);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto domainPacket2 = DataPacket(domainDesc, 10, 200);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);

    pkt = connection.dequeue();
    ASSERT_EQ(pkt.asPtrOrNull<IEventPacket>(true).getEventId(), event_packet_id::IMPLICIT_DOMAIN_GAP_DETECTED);
    ASSERT_EQ(pkt.asPtrOrNull<IEventPacket>(true).getParameters().get(event_packet_param::GAP_DIFF), 100);

    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, NotAvailableExplicitDomainRule)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors(true);

    const auto eventPacket = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket);

    const auto domainPacket1 = DataPacket(domainDesc, 10);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto domainPacket2 = DataPacket(domainDesc, 10);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, Overlap)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();

    const auto eventPacket = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket);

    const auto domainPacket1 = DataPacket(domainDesc, 10, 50);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto domainPacket2 = DataPacket(domainDesc, 10, 100);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);

    pkt = connection.dequeue();
    ASSERT_EQ(pkt.asPtrOrNull<IEventPacket>(true).getParameters().get("GapDiff"), -50);

    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, DataDescriptorChanged)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();

    const auto eventPacket1 = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket1);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket1);

    const auto domainPacket1 = DataPacket(domainDesc, 10, 50);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto eventPacket2 = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket2);

    const auto domainPacket2 = DataPacket(domainDesc, 10, 1000);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);
}

TYPED_TEST(GapCheckTest, MultiplePackets)
{
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    auto [valueDesc, domainDesc] = this->getDescriptors();

    const auto eventPacket1 = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket1);
    auto pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket1);

    const auto domainPacket1 = DataPacket(domainDesc, 10, 50);
    const auto packet1 = DataPacketWithDomain(domainPacket1, valueDesc, 10);
    connection.enqueue(packet1);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet1);

    const auto eventPacket2 = DataDescriptorChangedEventPacket(valueDesc, domainDesc);
    connection.enqueue(eventPacket2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, eventPacket2);

    const auto domainPacket2 = DataPacket(domainDesc, 10, 1000);
    const auto packet2 = DataPacketWithDomain(domainPacket2, valueDesc, 10);
    connection.enqueue(packet2);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet2);

    const auto domainPacket3 = DataPacket(domainDesc, 10, 1100);
    const auto packet3 = DataPacketWithDomain(domainPacket3, valueDesc, 10);
    connection.enqueue(packet3);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet3);

    const auto domainPacket4 = DataPacket(domainDesc, 10, 1500);
    const auto packet4 = DataPacketWithDomain(domainPacket4, valueDesc, 10);
    connection.enqueue(packet4);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt.asPtrOrNull<IEventPacket>(true).getParameters().get("GapDiff"), 300);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet4);

    const auto domainPacket5 = DataPacket(domainDesc, 10, 1600);
    const auto packet5 = DataPacketWithDomain(domainPacket5, valueDesc, 10);
    connection.enqueue(packet5);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet5);

    const auto domainPacket6 = DataPacket(domainDesc, 10, 1600);
    const auto packet6 = DataPacketWithDomain(domainPacket6, valueDesc, 10);
    connection.enqueue(packet6);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt.asPtrOrNull<IEventPacket>(true).getParameters().get("GapDiff"), -100);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet6);

    const auto domainPacket7 = DataPacket(domainDesc, 10, 1700);
    const auto packet7 = DataPacketWithDomain(domainPacket7, valueDesc, 10);
    connection.enqueue(packet7);
    pkt = connection.dequeue();
    ASSERT_EQ(pkt, packet7);
}

TYPED_TEST(GapCheckTest, CountersDecrementOnDequeue)
{
    // Counter bookkeeping across the three drain paths
    const auto ctx = NullContext();

    MockInputPort::Strict inputPort;
    MockSignal::Strict signal;

    EXPECT_CALL(inputPort.mock(), getGapCheckingEnabled(testing::_)).WillOnce(GetBool(True));

    const auto connection = Connection(inputPort.ptr, signal.ptr, ctx);

    // Incremental dequeue
    this->enqueueGapSequence(connection);
    ASSERT_EQ(connection.getPacketCount(), 4u);
    ASSERT_EQ(connection.getAvailableSamples(), 20u);
    ASSERT_TRUE(connection.hasEventPacket());
    ASSERT_TRUE(connection.hasGapPacket());

    // The reference path: onPacketDequeued decrements the counter matching each packet's kind.
    while (connection.dequeue().assigned())
    {
    }

    ASSERT_EQ(connection.getPacketCount(), 0u);
    ASSERT_EQ(connection.getAvailableSamples(), 0u);
    ASSERT_FALSE(connection.hasEventPacket());
    ASSERT_FALSE(connection.hasGapPacket());

    this->enqueueGapSequence(connection);
    ASSERT_EQ(connection.getPacketCount(), 4u);

    // dequeueUpTo removes packets without onPacketDequeued and calls countPackets() to rebuild the
    // counters from what is left. The rebuild has to cover gapPacketsCnt too - a gap packet drained
    // through this path otherwise leaves it non-zero forever, and hasEventPacket() (which is
    // eventPacketsCnt != 0 || gapPacketsCnt != 0) then reports true on an empty queue.
    ASSERT_EQ(batchDequeue(connection, 8), 4u);

    ASSERT_EQ(connection.getPacketCount(), 0u);
    ASSERT_EQ(connection.getAvailableSamples(), 0u);
    ASSERT_FALSE(connection.hasEventPacket());
    ASSERT_FALSE(connection.hasGapPacket());

    this->enqueueGapSequence(connection);

    ASSERT_EQ(batchDequeue(connection, 2), 2u);

    ASSERT_EQ(connection.getPacketCount(), 2u);
    ASSERT_EQ(connection.getAvailableSamples(), 10u);
    ASSERT_TRUE(connection.hasGapPacket());
    ASSERT_TRUE(connection.hasEventPacket());
    // Both stop at the leading gap; the descriptor is gone, so nothing bounds that count.
    ASSERT_EQ(connection.getSamplesUntilNextEventPacket(), 0u);
    ASSERT_EQ(connection.getSamplesUntilNextGapPacket(), 0u);
    ASSERT_EQ(connection.getSamplesUntilNextDescriptor(), 10u);

    // Draining the remainder must clear everything the recount is responsible for.
    ASSERT_EQ(batchDequeue(connection, 8), 2u);

    ASSERT_EQ(connection.getPacketCount(), 0u);
    ASSERT_EQ(connection.getAvailableSamples(), 0u);
    ASSERT_FALSE(connection.hasEventPacket());
    ASSERT_FALSE(connection.hasGapPacket());

    this->enqueueGapSequence(connection);
    ASSERT_EQ(connection.getPacketCount(), 4u);

    // dequeueAll empties the queue in one go and zeroes the counters directly - gapPacketsCnt
    // included, or it leaks exactly the way the batch path used to.
    ASSERT_EQ(connection.dequeueAll().getCount(), 4u);

    ASSERT_EQ(connection.getPacketCount(), 0u);
    ASSERT_EQ(connection.getAvailableSamples(), 0u);
    ASSERT_FALSE(connection.hasEventPacket());
    ASSERT_FALSE(connection.hasGapPacket());

    // A fourth kind of packet: an event the connection does not recognize. Enqueue counts it as an
    // event (getSamplesUntilNextEventPacket stops at any event packet, so any event packet has to
    // disable its counter fast path), which means every drain path has to discount it again. Miss
    // it and eventPacketsCnt only ever grows, leaving hasEventPacket() true on an empty queue -
    // the same failure as the gap counter, on the other counter.
    connection.enqueue(EventPacket("test", Dict<IString, IString>()));
    ASSERT_TRUE(connection.hasEventPacket());

    ASSERT_TRUE(connection.dequeue().assigned());

    ASSERT_EQ(connection.getPacketCount(), 0u);
    ASSERT_FALSE(connection.hasEventPacket());
    ASSERT_FALSE(connection.hasGapPacket());
}
