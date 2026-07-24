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
#include <opendaq/multi_reader/callback_gate.h>
#include <opendaq/scheduler_ptr.h>
#include <opendaq/work_factory.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Evaluation-task scheduling and the shared callback gate for the multi reader.
 *
 * Two responsibilities:
 *
 * 1. Coalesced evaluation scheduling. requestEvaluation() is the bounded, lock-free entry
 *    point: it schedules at most one evaluation task on the scheduler. The task clears its
 *    scheduled flag before running, so updates arriving during an evaluation schedule exactly
 *    one follow-up. The task never runs after detach() - the shared task state outlives the
 *    coordinator and is checked under its own lock (taken only inside the task, never on the
 *    request path).
 *
 * 2. The shared CallbackGate (see callback_gate.h). Producers raise per-slot flags and query
 *    gateSatisfied() locklessly; a task is scheduled from the packet path only when the gate
 *    is open (or a producer could not trust its snapshot). The owner reconciles the flags to
 *    ground truth under its state lock before letting the gate fire the user callback, so
 *    producer raises are advisory: they can cause a spurious task but never a spurious user
 *    callback, and they can never suppress one.
 *
 * Threading contract: requestEvaluation(), detach() and every gate query are thread-safe and
 * lock-free on the caller's side. The evaluation callback runs on a scheduler thread without
 * any coordinator lock held - the owner takes its own lock inside and must invoke user
 * callbacks only after releasing it. detach() must be called without holding locks the
 * evaluation takes.
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

    /// Producer-thread safe and lock-free; schedules at most one coalesced evaluation task.
    void requestEvaluation();

    /// No evaluation callback runs after this returns - except one already in flight on
    /// another thread, which detach() waits out via the task-state lock.
    void detach();

    /// Shared gate state; each Input holds a reference so producer raises and owner
    /// reconciliation adjust the same counters.
    const std::shared_ptr<CallbackGate>& gate() const;

    /// Lock-free: the callback gate (see CallbackGate::isSatisfied).
    bool gateSatisfied() const;

    /// Mark the current thread as an owner pass for producers' consistency checks.
    CallbackGate::PassGuard beginOwnerPass();

    /// One-shot latch: raise the callback gate for a state change that carries no returnable
    /// data or event (a transition into an InputsFailed state - Incompatible /
    /// SynchronizationFailed / DataLost). Set by the owner on the transition; the owner consumes
    /// it (sets false) once the callback has fired, so a single occurrence wakes the consumer
    /// exactly once and does not re-fire while it persists.
    void setStateChangeNotify(bool notify);
    bool getStateChangeNotify() const;

private:
    struct TaskState
    {
        std::mutex mutex;
        EvaluationCallback callback;   // cleared by detach()
        std::atomic_bool scheduled{false};
    };

    void scheduleTask();

    std::shared_ptr<TaskState> taskState;
    std::shared_ptr<CallbackGate> gateState;
    WorkExecutor executor;
    LoggerComponentPtr loggerComponent;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
