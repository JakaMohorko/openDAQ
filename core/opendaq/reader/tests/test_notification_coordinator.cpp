#include <gtest/gtest.h>

#include <opendaq/notification_coordinator.h>
#include "reader_common.h"

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

TEST_F(NotificationCoordinatorTest, EventOnUsedInputGatesCallback)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(3);

    ASSERT_FALSE(coordinator.shouldInvokeCallback());

    coordinator.setEvent(1, true);
    ASSERT_TRUE(coordinator.anyUsedEvent());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    // Events on unused inputs do not count
    coordinator.setUsed(1, false);
    ASSERT_FALSE(coordinator.shouldInvokeCallback());
}

TEST_F(NotificationCoordinatorTest, AllUsedReadyGatesCallback)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(3);

    coordinator.setReady(0, true);
    coordinator.setReady(1, true);
    ASSERT_FALSE(coordinator.allUsedReady());  // input 2 not ready

    coordinator.setReady(2, true);
    ASSERT_TRUE(coordinator.allUsedReady());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    // An unused input is excluded from the readiness requirement
    coordinator.setReady(2, false);
    coordinator.setUsed(2, false);
    ASSERT_TRUE(coordinator.allUsedReady());

    // No used inputs at all means nothing is ready
    coordinator.setUsed(0, false);
    coordinator.setUsed(1, false);
    ASSERT_FALSE(coordinator.allUsedReady());
    ASSERT_FALSE(coordinator.shouldInvokeCallback());
}

TEST_F(NotificationCoordinatorTest, ClearReadinessKeepsUsedMask)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(2);
    coordinator.setUsed(1, false);
    coordinator.setReady(0, true);
    coordinator.setEvent(0, true);
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    coordinator.clearReadiness();
    ASSERT_FALSE(coordinator.shouldInvokeCallback());
    ASSERT_TRUE(coordinator.isUsed(0));
    ASSERT_FALSE(coordinator.isUsed(1));
}

TEST_F(NotificationCoordinatorTest, ResizePreservesExistingBits)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(2);
    coordinator.setUsed(1, false);

    coordinator.resize(4);
    ASSERT_EQ(coordinator.getSlotCount(), 4u);
    ASSERT_FALSE(coordinator.isUsed(1));
    ASSERT_TRUE(coordinator.isUsed(2));   // new slots default to used
    ASSERT_FALSE(coordinator.shouldInvokeCallback());  // and to not-ready
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
