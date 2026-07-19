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
#pragma once
#include <coretypes/common.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Per-input packet-liveness deadlines (spec section 3.6).
 *
 * Monitoring arms per slot on the first packet after the slot becomes monitored
 * (used + connected + reader active); a monitored, armed slot whose last arrival is older
 * than the timeout is lost. A slot recovers when its next packet arrives; the owner leaves
 * the DataLost state when no lost slots remain. Zero timeout disables monitoring (default).
 *
 * Deadlines fire without reads: a waiter thread wakes at the earliest unreported deadline
 * and invokes the deadline callback (once per crossing), which the owner routes into the
 * coalesced state evaluation. With a test clock injected the waiter stays dormant - virtual
 * time cannot wake a real-time wait - and tests drive the evaluation themselves
 * (test scaffolding section 2.7).
 *
 * Thread safety: all methods are safe from any thread; onPacket is bounded (one leaf mutex,
 * no outward calls). The callback is invoked without the monitor lock held.
 */
class DataLossMonitor
{
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using DeadlineCallback = std::function<void()>;

    DataLossMonitor();
    ~DataLossMonitor();

    DataLossMonitor(const DataLossMonitor&) = delete;
    DataLossMonitor& operator=(const DataLossMonitor&) = delete;

    /// Test hook: replaces the time source and disables the real-time waiter.
    void setClockForTest(Clock clock);

    /// Zero disables monitoring and clears all arming.
    void setTimeout(std::chrono::nanoseconds newTimeout);
    std::chrono::nanoseconds getTimeout() const;

    /// Invoked from the waiter thread when a deadline expires; never under the monitor lock.
    void setDeadlineCallback(DeadlineCallback newCallback);

    /// Stops the waiter and drops the callback; safe to call more than once.
    void detach();

    void resize(SizeT slotCount);

    /// S1 (stable slots): drops one slot's state, shifting the following slots down by one;
    /// the remaining slots keep their arming and deadlines.
    void erase(SizeT slot);

    /// Producer path: record a packet arrival; arms the slot and clears a lost condition.
    void onPacket(SizeT slot);

    /// Owner gate: monitoring applies only to used, connected slots of an active reader.
    /// Turning a slot off clears its arming; it re-arms on the first packet after turning on.
    void setMonitored(SizeT slot, bool monitored);

    /// Monitored, armed slots whose deadline has expired, in slot order.
    std::vector<SizeT> lostSlots() const;

    /// Cheap steady-state probe for the read fast path: any expired deadline at all?
    bool hasLostSlots() const;

private:
    struct SlotState
    {
        bool monitored = false;
        bool armed = false;
        bool reported = false;  // deadline crossing already fired the callback
        std::chrono::steady_clock::time_point lastArrival{};
    };

    void ensureWaiterLocked(std::unique_lock<std::mutex>& lock);
    void waiterLoop();
    std::optional<std::chrono::steady_clock::time_point> earliestUnreportedDeadlineLocked() const;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread waiter;
    bool stopping = false;
    bool useRealClock = true;
    Clock clock;
    std::chrono::nanoseconds timeout{0};
    DeadlineCallback callback;
    std::vector<SlotState> slots;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
