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

TEST_F(NotificationCoordinatorTest, EventOnAnyInputGatesCallback)
{
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(3);

    ASSERT_FALSE(coordinator.shouldInvokeCallback());

    coordinator.setEvent(1, true);
    ASSERT_TRUE(coordinator.anyUsedEvent());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    // Events on unused inputs fire the callback too (review Q5): the notification is the
    // recovery API for consumers that parked the input
    coordinator.setUsed(1, false);
    ASSERT_FALSE(coordinator.anyUsedEvent());
    ASSERT_TRUE(coordinator.anyEvent());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    // Consuming the event clears the gate
    coordinator.setEvent(1, false);
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

TEST_F(NotificationCoordinatorTest, StateChangeNotifyGatesCallback)
{
    // Part 1 (spec 3.5): a latched state-change notification (the DataLost deadline) opens the
    // callback gate on its own, even with no events and no readiness, and is a one-shot.
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(2);

    // No events, no ready inputs -> gate closed
    ASSERT_FALSE(coordinator.shouldInvokeCallback());

    coordinator.setStateChangeNotify(true);
    ASSERT_TRUE(coordinator.getStateChangeNotify());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());

    // Consuming the latch closes the gate again
    coordinator.setStateChangeNotify(false);
    ASSERT_FALSE(coordinator.getStateChangeNotify());
    ASSERT_FALSE(coordinator.shouldInvokeCallback());
}

TEST_F(NotificationCoordinatorTest, ClearReadinessLeavesStateChangeNotify)
{
    // clearReadiness drops ready/event bits (sync invalidated) but the state-change latch is a
    // separate signal the owner consumes explicitly once the callback has fired.
    NotificationCoordinator coordinator(manualExecutor(), loggerComponent);
    coordinator.resize(2);
    coordinator.setStateChangeNotify(true);

    coordinator.clearReadiness();
    ASSERT_TRUE(coordinator.getStateChangeNotify());
    ASSERT_TRUE(coordinator.shouldInvokeCallback());
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
