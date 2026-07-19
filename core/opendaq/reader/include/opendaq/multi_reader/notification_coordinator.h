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
#include <opendaq/logger_component_ptr.h>
#include <opendaq/scheduler_ptr.h>
#include <opendaq/work_factory.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Readiness tracking and callback coalescing for the multi reader (spec section 3.5).
 *
 * Two independent responsibilities:
 *
 * 1. Coalesced evaluation scheduling. requestEvaluation() is the bounded producer-thread
 *    entry point: it schedules at most one evaluation task on the scheduler. The task
 *    clears its scheduled flag before running, so updates arriving during an evaluation
 *    schedule exactly one follow-up. The task never runs after detach() - the shared
 *    task state outlives the coordinator and is checked under its own lock.
 *
 * 2. Used/ready/event masks deciding whether the public onDataAvailable callback fires:
 *    (event & used).any() || (used.any() && (ready & used) == used).
 *    The "ready" meaning is phase-dependent (first sample while synchronizing, one full
 *    block while synchronized) - the owner sets the bits during its state evaluation.
 *
 * Threading contract: requestEvaluation() and detach() are thread-safe. Everything else
 * (masks, callback queries) must be called with the owner's state lock held. The
 * evaluation callback itself runs on a scheduler thread without any coordinator lock
 * held - the owner takes its own lock inside and must invoke user callbacks only after
 * releasing it. detach() must be called without holding locks the evaluation takes.
 */
class NotificationCoordinator
{
public:
    using EvaluationCallback = std::function<void()>;
    /// Test seam: replaces the scheduler as the task executor.
    using WorkExecutor = std::function<void(std::function<void()>)>;

    NotificationCoordinator(const SchedulerPtr& scheduler, const LoggerComponentPtr& logger);
    NotificationCoordinator(WorkExecutor executor, const LoggerComponentPtr& logger);
    ~NotificationCoordinator();

    /// The owner's coalesced evaluation entry point. Set once during construction of the owner.
    void setEvaluationCallback(EvaluationCallback callback);

    /// Producer-thread safe; schedules at most one coalesced evaluation task.
    void requestEvaluation();

    /// No evaluation callback runs after this returns-except one already in flight on
    /// another thread, which detach() waits out via the task-state lock.
    void detach();

    // --- Masks (owner state lock held) ---
    void resize(SizeT slotCount);
    SizeT getSlotCount() const;

    void setUsed(SizeT index, bool used);
    void setReady(SizeT index, bool ready);
    void setEvent(SizeT index, bool hasEvent);
    bool isUsed(SizeT index) const;

    /// Clears ready and event bits (synchronization invalidated, topology changed, ...).
    void clearReadiness();

    /// (event & used).any()
    bool anyUsedEvent() const;
    /// used.any() && (ready & used) == used
    bool allUsedReady() const;
    /// The callback gate of spec section 3.5.
    bool shouldInvokeCallback() const;

private:
    struct TaskState
    {
        std::mutex mutex;
        EvaluationCallback callback;   // cleared by detach()
        std::atomic_bool scheduled{false};
    };

    void scheduleTask();

    std::shared_ptr<TaskState> taskState;
    WorkExecutor executor;
    LoggerComponentPtr loggerComponent;

    std::vector<bool> usedMask;
    std::vector<bool> readyMask;
    std::vector<bool> eventMask;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
