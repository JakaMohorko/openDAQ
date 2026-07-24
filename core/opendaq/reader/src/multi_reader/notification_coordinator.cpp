#include <opendaq/multi_reader/notification_coordinator.h>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

NotificationCoordinator::NotificationCoordinator(const SchedulerPtr& scheduler, const LoggerComponentPtr& logger)
    : taskState(std::make_shared<TaskState>())
    , gateState(std::make_shared<CallbackGate>())
    , loggerComponent(logger)
{
    executor = [scheduler](std::function<void()> work)
    {
        if (scheduler.assigned())
            scheduler.scheduleWork(Work(std::move(work)));
        else
            work();  // no scheduler: run inline - correctness never depends on deferral
    };
}

NotificationCoordinator::NotificationCoordinator(WorkExecutor executor, const LoggerComponentPtr& logger)
    : taskState(std::make_shared<TaskState>())
    , gateState(std::make_shared<CallbackGate>())
    , executor(std::move(executor))
    , loggerComponent(logger)
{
}

NotificationCoordinator::~NotificationCoordinator()
{
    detach();
}

void NotificationCoordinator::setEvaluationCallback(EvaluationCallback callback)
{
    std::lock_guard lock(taskState->mutex);
    taskState->callback = std::move(callback);
}

void NotificationCoordinator::requestEvaluation()
{
    if (!taskState->scheduled.exchange(true))
        scheduleTask();
}

void NotificationCoordinator::detach()
{
    std::lock_guard lock(taskState->mutex);
    taskState->callback = nullptr;
}

const std::shared_ptr<CallbackGate>& NotificationCoordinator::gate() const
{
    return gateState;
}

bool NotificationCoordinator::gateSatisfied() const
{
    return gateState->isSatisfied();
}

CallbackGate::PassGuard NotificationCoordinator::beginOwnerPass()
{
    return CallbackGate::PassGuard(*gateState);
}

void NotificationCoordinator::setStateChangeNotify(bool notify)
{
    gateState->setStateChangeNotify(notify);
}

bool NotificationCoordinator::getStateChangeNotify() const
{
    return gateState->getStateChangeNotify();
}

void NotificationCoordinator::scheduleTask()
{
    // The lambda holds the task state alive; a coordinator destroyed with a task still
    // queued leaves a harmless no-op behind (detach cleared the callback).
    executor(
        [state = taskState]
        {
            // Cleared before running: updates arriving during the evaluation schedule
            // exactly one follow-up task instead of being lost.
            state->scheduled = false;

            std::lock_guard lock(state->mutex);
            if (state->callback)
                state->callback();
        });
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
