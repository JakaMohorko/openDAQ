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
    // Both directions clear the arming: disabling stops monitoring outright, and enabling
    // must not count arrivals recorded before the deadline existed - each slot re-arms on
    // its first packet under the new timeout
    for (auto& slot : slots)
    {
        slot.armed = false;
        slot.reported = false;
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
        if (!monitored)
        {
            slots[slot].armed = false;
            slots[slot].reported = false;
        }
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
