#include <gtest/gtest.h>

#include <opendaq/data_loss_monitor.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace daq;
using namespace std::chrono;
using namespace std::chrono_literals;

// Unit tests for the per-input packet-liveness deadlines (spec section 3.6, test plan B.6).
// All tests run on virtual time via the injected clock - zero real sleeps.
class DataLossMonitorTest : public testing::Test
{
protected:
    void SetUp() override
    {
        monitor.setClockForTest([this] { return now; });
        monitor.resize(2);
        monitor.setMonitored(0, true);
        monitor.setMonitored(1, true);
    }

    void advance(steady_clock::duration delta)
    {
        now += delta;
    }

    steady_clock::time_point now{steady_clock::now()};
    DataLossMonitor monitor;
};

TEST_F(DataLossMonitorTest, ZeroTimeoutDisables)  // DL-4
{
    monitor.onPacket(0);
    monitor.onPacket(1);
    advance(24h);
    ASSERT_TRUE(monitor.lostSlots().empty());
}

TEST_F(DataLossMonitorTest, ArmsOnlyAfterFirstPacket)  // DL-6
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    advance(24h);
    // No packet since the slots became monitored - nothing is armed, nothing is lost
    ASSERT_TRUE(monitor.lostSlots().empty());
}

TEST_F(DataLossMonitorTest, DeadlineExpiryReportsLoss)  // DL-1 (deadline math)
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    monitor.onPacket(0);
    monitor.onPacket(1);

    advance(99ms);
    ASSERT_TRUE(monitor.lostSlots().empty());

    advance(2ms);
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{0, 1}));
}

TEST_F(DataLossMonitorTest, OnlyStaleSlotsListed)  // DL-2
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    monitor.onPacket(0);
    monitor.onPacket(1);

    advance(60ms);
    monitor.onPacket(0);  // slot 0 stays fresh

    advance(60ms);  // slot 1 is 120 ms stale, slot 0 only 60 ms
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{1}));
}

TEST_F(DataLossMonitorTest, PacketRecoversSlot)  // DL-3
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    monitor.onPacket(0);
    monitor.onPacket(1);

    advance(150ms);
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{0, 1}));

    monitor.onPacket(1);
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{0}));

    monitor.onPacket(0);
    ASSERT_TRUE(monitor.lostSlots().empty());
}

TEST_F(DataLossMonitorTest, UnmonitoredSlotNeverTrips)  // DL-5
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    monitor.setMonitored(1, false);
    monitor.onPacket(0);
    monitor.onPacket(1);

    advance(1h);
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{0}));
}

TEST_F(DataLossMonitorTest, TurningMonitoringOffDisarms)  // DL-5/DL-6
{
    monitor.setTimeout(duration_cast<nanoseconds>(100ms));
    monitor.onPacket(0);

    monitor.setMonitored(0, false);
    monitor.setMonitored(0, true);

    // Re-enabling does not resurrect the old arrival - the slot re-arms on its next packet
    advance(1h);
    ASSERT_TRUE(monitor.lostSlots().empty());

    monitor.onPacket(0);
    advance(150ms);
    ASSERT_EQ(monitor.lostSlots(), (std::vector<SizeT>{0}));
}

TEST_F(DataLossMonitorTest, RealDeadlineFiresCallbackWithoutReads)  // DL-1 (slow smoke test)
{
    // The only test on real time: the waiter thread must fire the deadline callback on its
    // own, with no external polling driving it
    DataLossMonitor realMonitor;
    std::atomic<int> fired{0};
    realMonitor.setDeadlineCallback([&fired] { ++fired; });
    realMonitor.resize(1);
    realMonitor.setMonitored(0, true);
    realMonitor.setTimeout(duration_cast<nanoseconds>(50ms));
    realMonitor.onPacket(0);

    const auto start = steady_clock::now();
    while (fired.load() == 0 && steady_clock::now() - start < 5s)
        std::this_thread::yield();

    ASSERT_EQ(fired.load(), 1);
    ASSERT_EQ(realMonitor.lostSlots(), (std::vector<SizeT>{0}));

    // One crossing fires exactly once; the next packet re-arms for another crossing
    realMonitor.onPacket(0);
    const auto restart = steady_clock::now();
    while (fired.load() == 1 && steady_clock::now() - restart < 5s)
        std::this_thread::yield();
    ASSERT_EQ(fired.load(), 2);
}
