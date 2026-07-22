#include <opendaq/multi_reader/data_loss_monitor.h>

#include <algorithm>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

DataLossMonitor::DataLossMonitor()
    : clock([] { return std::chrono::steady_clock::now(); })
{
}

DataLossMonitor::~DataLossMonitor()
{
    detach();
}

void DataLossMonitor::detach()
{
    std::thread toJoin;
    {
        std::unique_lock lock(mutex);
        stopping = true;
        callback = nullptr;
        toJoin = std::move(waiter);
    }
    cv.notify_all();
    if (toJoin.joinable())
        toJoin.join();
}

void DataLossMonitor::setClockForTest(Clock newClock)
{
    std::thread toJoin;
    {
        std::unique_lock lock(mutex);
        clock = std::move(newClock);
        // A real-time wait cannot observe virtual time - the waiter exits on its next wake
        // (its loop condition checks useRealClock) and tests drive the owner's evaluation
        // directly after advancing the clock. `stopping` stays untouched so a concurrent
        // detach cannot be undone.
        useRealClock = false;
        toJoin = std::move(waiter);
    }
    cv.notify_all();
    if (toJoin.joinable())
        toJoin.join();
}

void DataLossMonitor::setTimeout(std::chrono::nanoseconds newTimeout)
{
    std::unique_lock lock(mutex);
    timeout = newTimeout;
    const auto now = clock();
    // Re-arm from now under the new timeout: a monitored slot gets a fresh full deadline (arming
    // is not deferred to its next packet), and an arrival recorded before this timeout existed
    // must not count toward it. Disabling (0) disarms every slot.
    for (auto& slot : slots)
    {
        slot.reported = false;
        if (slot.monitored && newTimeout.count() > 0)
        {
            slot.armed = true;
            slot.lastArrival = now;
        }
        else
        {
            slot.armed = false;
        }
    }
    ensureWaiterLocked(lock);
    cv.notify_all();
}

std::chrono::nanoseconds DataLossMonitor::getTimeout() const
{
    std::unique_lock lock(mutex);
    return timeout;
}

void DataLossMonitor::setDeadlineCallback(DeadlineCallback newCallback)
{
    std::unique_lock lock(mutex);
    callback = std::move(newCallback);
    ensureWaiterLocked(lock);
    cv.notify_all();
}

void DataLossMonitor::resize(SizeT slotCount)
{
    std::unique_lock lock(mutex);
    slots.resize(slotCount);
    cv.notify_all();
}

void DataLossMonitor::erase(SizeT slot)
{
    {
        std::unique_lock lock(mutex);
        if (slot >= slots.size())
            return;
        // Removing one input must not disturb the remaining inputs' arming or deadlines.
        slots.erase(slots.begin() + slot);
    }
    cv.notify_all();
}

bool DataLossMonitor::hasLostSlots() const
{
    std::unique_lock lock(mutex);
    if (timeout.count() <= 0)
        return false;

    const auto now = clock();
    for (const auto& slot : slots)
    {
        if (slot.monitored && slot.armed && now - slot.lastArrival > timeout)
            return true;
    }
    return false;
}

void DataLossMonitor::onPacket(SizeT slot)
{
    bool wake = false;
    {
        std::unique_lock lock(mutex);
        if (slot >= slots.size())
            return;
        // Arming follows the contract "first packet after the slot becomes monitored" -
        // packets seen while unmonitored (unused input, inactive reader, disabled
        // monitoring) must not count toward a later deadline
        if (!slots[slot].monitored || timeout.count() <= 0)
            return;
        slots[slot].lastArrival = clock();
        slots[slot].armed = true;
        slots[slot].reported = false;
        wake = waiter.joinable();
    }
    if (wake)
        cv.notify_all();
}

void DataLossMonitor::setMonitored(SizeT slot, bool monitored)
{
    {
        std::unique_lock lock(mutex);
        if (slot >= slots.size())
            return;
        if (slots[slot].monitored == monitored)
            return;
        slots[slot].monitored = monitored;
        slots[slot].reported = false;
        if (monitored && timeout.count() > 0)
        {
            // Arm at the start of monitoring: the deadline runs one full timeout from now, so a
            // used + connected input of an active reader that never delivers a packet trips the
            // same deadline as one whose producer stops after delivering some. onPacket refreshes
            // the deadline on each arrival; turning monitoring off (below) disarms.
            slots[slot].lastArrival = clock();
            slots[slot].armed = true;
        }
        else
        {
            slots[slot].armed = false;
        }
        ensureWaiterLocked(lock);
    }
    cv.notify_all();
}

std::vector<SizeT> DataLossMonitor::lostSlots() const
{
    std::unique_lock lock(mutex);
    std::vector<SizeT> lost;
    if (timeout.count() <= 0)
        return lost;

    const auto now = clock();
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        const auto& slot = slots[i];
        if (slot.monitored && slot.armed && now - slot.lastArrival > timeout)
            lost.push_back(i);
    }
    return lost;
}

std::optional<std::chrono::steady_clock::time_point> DataLossMonitor::earliestUnreportedDeadlineLocked() const
{
    if (timeout.count() <= 0)
        return std::nullopt;

    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const auto& slot : slots)
    {
        if (!slot.monitored || !slot.armed || slot.reported)
            continue;
        const auto deadline = slot.lastArrival + timeout;
        if (!earliest || deadline < *earliest)
            earliest = deadline;
    }
    return earliest;
}

void DataLossMonitor::ensureWaiterLocked(std::unique_lock<std::mutex>&)
{
    // The waiter exists only while it can do useful work: real clock, a callback to fire
    // and monitoring actually enabled - a disabled monitor costs no thread
    if (waiter.joinable() || !useRealClock || stopping || !callback || timeout.count() <= 0)
        return;
    waiter = std::thread([this] { waiterLoop(); });
}

void DataLossMonitor::waiterLoop()
{
    std::unique_lock lock(mutex);
    while (!stopping && useRealClock)
    {
        const auto deadline = earliestUnreportedDeadlineLocked();
        if (!deadline)
        {
            cv.wait(lock);
            continue;
        }

        cv.wait_until(lock, *deadline);
        // Re-check the exit conditions after every wake: a wait whose deadline already
        // passed reports timeout even when woken by detach or a clock injection
        if (stopping || !useRealClock)
            break;

        // Mark every expired deadline reported so a crossing fires exactly once
        const auto now = clock();
        bool anyExpired = false;
        for (auto& slot : slots)
        {
            if (slot.monitored && slot.armed && !slot.reported && now - slot.lastArrival > timeout)
            {
                slot.reported = true;
                anyExpired = true;
            }
        }
        if (anyExpired && callback)
        {
            auto invoke = callback;
            lock.unlock();
            invoke();
            lock.lock();
        }
    }
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
