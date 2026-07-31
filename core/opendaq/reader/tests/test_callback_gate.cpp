#include <gtest/gtest.h>

#include <opendaq/multi_reader/callback_gate.h>
#include "reader_common.h"

#include <memory>
#include <vector>

using namespace daq;
using namespace daq::multi_reader;

class CallbackGateTest : public ReaderTest<>
{
protected:
    // A gate with `count` slots, all armed and marked used - the state a freshly constructed
    // multi reader publishes before any evaluation.
    void makeSlots(SizeT count)
    {
        gate = std::make_shared<CallbackGate>();
        gate->adjustUsed(static_cast<std::int64_t>(count));
        flags.clear();
        for (SizeT i = 0; i < count; ++i)
            flags.push_back(std::make_unique<SlotGateFlags>(gate));
    }

    std::shared_ptr<CallbackGate> gate;
    std::vector<std::unique_ptr<SlotGateFlags>> flags;
};

TEST_F(CallbackGateTest, EmptyGateClosed)
{
    gate = std::make_shared<CallbackGate>();
    ASSERT_FALSE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, AnyEventOpensGate)
{
    makeSlots(3);
    ASSERT_FALSE(gate->isSatisfied());

    flags[1]->setEvent(true);
    ASSERT_TRUE(gate->isSatisfied());

    // Even an event on an unused slot keeps the gate open (the recovery signal)
    gate->adjustUsed(-1);
    ASSERT_TRUE(gate->isSatisfied());

    flags[1]->setEvent(false);
    ASSERT_FALSE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, AllUsedReadyOpensGate)
{
    makeSlots(3);
    flags[0]->setReady(true);
    flags[1]->setReady(true);
    ASSERT_FALSE(gate->isSatisfied());  // slot 2 not ready

    flags[2]->setReady(true);
    ASSERT_TRUE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, ReadyToleratesStragglerOnLeavingUsedSet)
{
    // ready >= used, not ==: a slot leaving the used set with its ready flag still up is
    // tolerated (a scheduled evaluation reconciles it), so the gate never misses a wake.
    makeSlots(2);
    flags[0]->setReady(true);
    flags[1]->setReady(true);
    ASSERT_TRUE(gate->isSatisfied());

    // Slot 1 becomes unused but its ready flag has not been lowered yet: ready(2) >= used(1)
    gate->adjustUsed(-1);
    ASSERT_TRUE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, NoUsedSlotsMeansNotReady)
{
    makeSlots(2);
    flags[0]->setReady(true);
    flags[1]->setReady(true);
    ASSERT_TRUE(gate->isSatisfied());

    // With no used slots the readiness term cannot open the gate (usedCount > 0 is required),
    // even while the ready flags are still up - readiness of nothing is not a reason to wake.
    gate->adjustUsed(-2);
    ASSERT_FALSE(gate->isSatisfied());

    // An event, however, still opens it (unused-slot events are the recovery signal).
    flags[0]->setEvent(true);
    ASSERT_TRUE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, StateChangeNotifyOpensGate)
{
    makeSlots(2);
    ASSERT_FALSE(gate->isSatisfied());

    gate->setStateChangeNotify(true);
    ASSERT_TRUE(gate->isSatisfied());

    gate->setStateChangeNotify(false);
    ASSERT_FALSE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, RaiseIsIdempotentOnCounters)
{
    makeSlots(2);
    // Repeated raises must bump the shared counter exactly once (transition, not level)
    ASSERT_TRUE(flags[0]->raiseReady());
    ASSERT_FALSE(flags[0]->raiseReady());
    ASSERT_TRUE(flags[1]->raiseReady());
    ASSERT_TRUE(gate->isSatisfied());  // both ready -> gate open, counter == 2

    // Lowering one drops the counter so the gate closes again
    ASSERT_TRUE(flags[0]->setReady(false));
    ASSERT_FALSE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, DisarmRetiresContributions)
{
    makeSlots(2);
    flags[0]->setReady(true);
    flags[0]->setEvent(true);
    ASSERT_TRUE(gate->isSatisfied());

    // Removing the slot: disarm subtracts its ready and event contributions atomically
    flags[0]->disarm();
    gate->adjustUsed(-1);  // owner also drops the used count for the removed slot
    ASSERT_FALSE(gate->isSatisfied());

    // A late producer raise on the disarmed slot is a no-op and cannot reopen the gate
    ASSERT_FALSE(flags[0]->raiseReady());
    ASSERT_FALSE(gate->isSatisfied());
}

TEST_F(CallbackGateTest, PassGuardTogglesEpochParity)
{
    gate = std::make_shared<CallbackGate>();
    const auto before = gate->passEpoch();
    ASSERT_TRUE(CallbackGate::epochQuiet(before));
    {
        CallbackGate::PassGuard guard(*gate);
        ASSERT_FALSE(CallbackGate::epochQuiet(gate->passEpoch()));
    }
    ASSERT_TRUE(CallbackGate::epochQuiet(gate->passEpoch()));
    ASSERT_NE(gate->passEpoch(), before);  // a full pass advanced the epoch
}
