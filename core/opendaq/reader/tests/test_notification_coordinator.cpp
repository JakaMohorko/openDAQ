#include <gtest/gtest.h>

#include <opendaq/multi_reader/notification_coordinator.h>
#include "reader_common.h"

using namespace daq::multi_reader;

#include <functional>
#include <memory>
#include <vector>

class NotificationCoordinatorTest : public ReaderTest<>
{
public:
    using Super = ReaderTest<>;

protected:
    // Deterministic executor: tasks run only when runAll() is called
    NotificationCoordinator::WorkExecutor manualExecutor()
    {
        return [this](std::function<void()> work) { taskQueue.push_back(std::move(work)); };
    }

    void runAll()
    {
        auto tasks = std::move(taskQueue);
        taskQueue.clear();
        for (auto& task : tasks)
            task();
    }

protected:
    std::vector<std::function<void()>> taskQueue;
};

TEST_F(NotificationCoordinatorTest, CoalescesRequests)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    int evaluations = 0;
    coordinator.setEvaluationCallback([&] { ++evaluations; });

    coordinator.requestEvaluation();
    coordinator.requestEvaluation();
    coordinator.requestEvaluation();

    ASSERT_EQ(taskQueue.size(), 1u);
    runAll();
    ASSERT_EQ(evaluations, 1);
}

TEST_F(NotificationCoordinatorTest, RequestDuringEvaluationSchedulesFollowUp)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    int evaluations = 0;
    coordinator.setEvaluationCallback(
        [&]
        {
            ++evaluations;
            // A packet arriving while the evaluation runs must produce one follow-up
            if (evaluations == 1)
                coordinator.requestEvaluation();
        });

    coordinator.requestEvaluation();
    runAll();
    ASSERT_EQ(evaluations, 1);
    ASSERT_EQ(taskQueue.size(), 1u);  // the follow-up

    runAll();
    ASSERT_EQ(evaluations, 2);
    ASSERT_TRUE(taskQueue.empty());
}

TEST_F(NotificationCoordinatorTest, DetachStopsCallbacks)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    int evaluations = 0;
    coordinator.setEvaluationCallback([&] { ++evaluations; });

    coordinator.requestEvaluation();
    coordinator.detach();
    runAll();

    ASSERT_EQ(evaluations, 0);
}

TEST_F(NotificationCoordinatorTest, QueuedTaskOutlivesCoordinator)
{
    int evaluations = 0;
    {
        NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
        coordinator.setEvaluationCallback([&] { ++evaluations; });
        coordinator.requestEvaluation();
    }
    // The coordinator died with a task still queued; running it must be a harmless no-op
    runAll();
    ASSERT_EQ(evaluations, 0);
}

TEST_F(NotificationCoordinatorTest, NoSchedulerRunsInline)
{
    NotificationCoordinator coordinator(SchedulerPtr(nullptr), loggerComponent);
    int evaluations = 0;
    coordinator.setEvaluationCallback([&] { ++evaluations; });

    coordinator.requestEvaluation();
    ASSERT_EQ(evaluations, 1);

    coordinator.requestEvaluation();
    ASSERT_EQ(evaluations, 2);
}

// The coordinator exposes the shared gate; the gate's own semantics are covered in
// test_callback_gate.cpp. Here we only check the coordinator wires the gate through so a
// producer query and the owner's reconciliation see the same state.
TEST_F(NotificationCoordinatorTest, GateSharedAndStateChangeNotify)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    ASSERT_TRUE(coordinator.gate() != nullptr);
    ASSERT_FALSE(coordinator.gateSatisfied());

    // A state-change latch opens the gate through the coordinator surface too
    coordinator.setStateChangeNotify(true);
    ASSERT_TRUE(coordinator.getStateChangeNotify());
    ASSERT_TRUE(coordinator.gateSatisfied());

    coordinator.setStateChangeNotify(false);
    ASSERT_FALSE(coordinator.gateSatisfied());
}

TEST_F(NotificationCoordinatorTest, BeginOwnerPassMakesEpochNoisy)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    const auto& gate = coordinator.gate();

    ASSERT_TRUE(CallbackGate::epochQuiet(gate->passEpoch()));
    {
        auto pass = coordinator.beginOwnerPass();
        ASSERT_FALSE(CallbackGate::epochQuiet(gate->passEpoch()));
    }
    ASSERT_TRUE(CallbackGate::epochQuiet(gate->passEpoch()));
}
