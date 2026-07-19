#include <opendaq/multi_reader/notification_coordinator.h>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

NotificationCoordinator::NotificationCoordinator(const SchedulerPtr& scheduler, const LoggerComponentPtr& logger)
    : taskState(std::make_shared<TaskState>())
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

void NotificationCoordinator::scheduleTask()
{
    // The lambda holds the task state alive; a coordinator destroyed with a task still
    // queued leaves a harmless no-op behind (detach cleared the callback).
    executor(
        [state = taskState]
        {
            // Cleared before running: updates arriving during the evaluation schedule
            // exactly one follow-up task instead of being lost (spec section 9.4)
            state->scheduled = false;

            std::lock_guard lock(state->mutex);
            if (state->callback)
                state->callback();
        });
}

void NotificationCoordinator::resize(SizeT slotCount)
{
    usedMask.resize(slotCount, true);
    readyMask.resize(slotCount, false);
    eventMask.resize(slotCount, false);
}

SizeT NotificationCoordinator::getSlotCount() const
{
    return usedMask.size();
}

void NotificationCoordinator::setUsed(SizeT index, bool used)
{
    usedMask.at(index) = used;
}

void NotificationCoordinator::setReady(SizeT index, bool ready)
{
    readyMask.at(index) = ready;
}

void NotificationCoordinator::setEvent(SizeT index, bool hasEvent)
{
    eventMask.at(index) = hasEvent;
}

bool NotificationCoordinator::isUsed(SizeT index) const
{
    return usedMask.at(index);
}

void NotificationCoordinator::clearReadiness()
{
    readyMask.assign(readyMask.size(), false);
    eventMask.assign(eventMask.size(), false);
}

bool NotificationCoordinator::anyUsedEvent() const
{
    for (SizeT i = 0; i < usedMask.size(); ++i)
    {
        if (usedMask[i] && eventMask[i])
            return true;
    }
    return false;
}

bool NotificationCoordinator::allUsedReady() const
{
    bool anyUsed = false;
    for (SizeT i = 0; i < usedMask.size(); ++i)
    {
        if (!usedMask[i])
            continue;
        anyUsed = true;
        if (!readyMask[i])
            return false;
    }
    return anyUsed;
}

bool NotificationCoordinator::shouldInvokeCallback() const
{
    return anyUsedEvent() || allUsedReady();
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
