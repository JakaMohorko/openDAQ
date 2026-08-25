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

#include <atomic>
#include <cstdint>
#include <memory>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Lock-free callback-gate state shared between the multi reader (owner) and its per-input
 * slots (producer threads). Satisfied when: event > 0 || (used > 0 && ready >= used) ||
 * stateChangeNotify. Producers only ever RAISE flags; lowering is reserved to the owner.
 */
class CallbackGate
{
public:
    bool isSatisfied() const
    {
        if (event.load() > 0)
            return true;
        if (stateChangeNotify.load())
            return true;
        const SizeT usedCount = used.load();
        return usedCount > 0 && ready.load() >= usedCount;
    }

    /// Owner-maintained used-slot count (state lock held by the caller).
    void adjustUsed(std::int64_t delta)
    {
        if (delta > 0)
            used.fetch_add(static_cast<SizeT>(delta));
        else if (delta < 0)
            used.fetch_sub(static_cast<SizeT>(-delta));
    }

    /// One-shot latch for a state change with no returnable data or event (InputsFailed family).
    void setStateChangeNotify(bool notify)
    {
        stateChangeNotify.store(notify);
    }

    bool getStateChangeNotify() const
    {
        return stateChangeNotify.load();
    }

    std::uint64_t passEpoch() const
    {
        return epoch.load();
    }

    static bool epochQuiet(std::uint64_t value)
    {
        return (value & 1u) == 0;
    }

    /// RAII marker for an owner pass that may move or consume samples; producers observing an
    /// odd or changed epoch do not trust their arithmetic and request an evaluation instead.
    class PassGuard
    {
    public:
        explicit PassGuard(CallbackGate& gate)
            : gate(&gate)
        {
            gate.epoch.fetch_add(1);
        }

        PassGuard(PassGuard&& other) noexcept
            : gate(other.gate)
        {
            other.gate = nullptr;
        }

        PassGuard(const PassGuard&) = delete;
        PassGuard& operator=(const PassGuard&) = delete;
        PassGuard& operator=(PassGuard&&) = delete;

        ~PassGuard()
        {
            if (gate)
                gate->epoch.fetch_add(1);
        }

    private:
        CallbackGate* gate;
    };

private:
    friend class SlotGateFlags;

    std::atomic<SizeT> used{0};
    std::atomic<SizeT> ready{0};
    std::atomic<SizeT> event{0};
    std::atomic_bool stateChangeNotify{false};
    /// Odd while an owner pass is in flight; incremented on entry and exit.
    std::atomic<std::uint64_t> epoch{0};
};

/**
 * @brief One slot's ready/event gate flags, packed with an armed bit into a single atomic word so
 * flag transitions and the shared counters can never diverge. Producers use raiseReady/raiseEvent
 * only; the owner sets both in either direction under its state lock and remains the authority.
 */
class SlotGateFlags
{
public:
    explicit SlotGateFlags(std::shared_ptr<CallbackGate> sharedGate)
        : gate(std::move(sharedGate))
    {
    }

    bool ready() const
    {
        return (word.load() & ReadyBit) != 0;
    }

    bool event() const
    {
        return (word.load() & EventBit) != 0;
    }

    /// @return true if the flag transitioned (and the shared counter was bumped).
    bool raiseReady()
    {
        return setBit(ReadyBit, true, gate->ready);
    }

    bool raiseEvent()
    {
        return setBit(EventBit, true, gate->event);
    }

    bool setReady(bool value)
    {
        return setBit(ReadyBit, value, gate->ready);
    }

    bool setEvent(bool value)
    {
        return setBit(EventBit, value, gate->event);
    }

    /// Owner removal: retire this slot's counter contributions; all later raises are no-ops.
    void disarm()
    {
        const std::uint32_t old = word.exchange(0);
        if (old & ReadyBit)
            gate->ready.fetch_sub(1);
        if (old & EventBit)
            gate->event.fetch_sub(1);
    }

private:
    static constexpr std::uint32_t ArmedBit = 1;
    static constexpr std::uint32_t ReadyBit = 2;
    static constexpr std::uint32_t EventBit = 4;

    bool setBit(std::uint32_t bit, bool value, std::atomic<SizeT>& counter)
    {
        std::uint32_t current = word.load();
        for (;;)
        {
            if ((current & ArmedBit) == 0)
                return false;
            const std::uint32_t next = value ? (current | bit) : (current & ~bit);
            if (next == current)
                return false;
            if (word.compare_exchange_weak(current, next))
            {
                if (value)
                    counter.fetch_add(1);
                else
                    counter.fetch_sub(1);
                return true;
            }
        }
    }

    std::shared_ptr<CallbackGate> gate;
    std::atomic<std::uint32_t> word{ArmedBit};
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
