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
#include <opendaq/logger_component_ptr.h>
#include <opendaq/queue_reader.h>
#include <opendaq/synchronization_manager.h>

#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief One multi-input read or skip, planned before anything commits (spec section 3.4).
 * Counts are common-rate equivalents; input i delivers commonCount / divider_i samples.
 */
struct ReadPlan
{
    SizeT commonCount = 0;                  // blockLcm-aligned, 0 = nothing to do
    std::vector<void*> valueBuffers;        // per input; may hold nullptr entries (skip)
    std::vector<void*> domainBuffers;       // per input; may hold nullptr entries
};

enum class CommitResult
{
    Ok = 0,
    InternalError  // a queue failed to deliver a validated plan - reader enters Error state
};

/**
 * @brief The single merged read coordinator of spec section 3.4, direct path.
 *
 * Availability, planning and committing all use the same alignment rules: counts are
 * rounded down to whole blockLcm blocks, stop before the earliest event boundary and
 * respect the effective minimum (max(minReadCount, blockLcm) rounded up to blocks).
 * The resampling execution branch is added in Phase 5; pipeline selection then happens
 * in configure().
 *
 * Never mutates queues except in commit() and discardLeftoverSegments(). All methods
 * require the owner's state lock; only the owner thread touches the queue readers, so
 * a plan validated against availability cannot be invalidated before its commit -
 * a partial commit is impossible by construction (acceptance criterion 8).
 */
class ReadCoordinator
{
public:
    explicit ReadCoordinator(const LoggerComponentPtr& logger);

    /**
     * @brief Called after every successful synchronization. Direct path: nothing to build;
     * Phase 5 selects per-input pipelines (direct copy vs. freshly built resampler) here.
     */
    void configure(const std::vector<QueueReader*>& inputs, const CommonModel& model);

    /// Synchronization was invalidated; configured pipelines are dropped.
    void invalidate();
    bool isConfigured() const;

    /**
     * @brief Complete aligned blocks available on every used input before its earliest
     * event boundary (common-rate equivalent). 0 below the effective minimum.
     */
    SizeT getAvailableCount(const std::vector<QueueReader*>& inputs, const CommonModel& model, SizeT minReadCount) const;

    /**
     * @brief Round the request down to whole blocks, clamp to availability and bind buffers.
     * Buffer vectors must either be empty (skip) or hold one entry per input (nullptr
     * entries are allowed and read into nothing... only valid for skip).
     */
    ReadPlan createPlan(SizeT requestedCommonCount,
                        const std::vector<QueueReader*>& inputs,
                        const CommonModel& model,
                        SizeT minReadCount,
                        void* const* valueBuffers,
                        void* const* domainBuffers) const;

    /// Execute the plan on every input. Cursors advance input by input in slot order;
    /// by the availability guarantee every read must succeed, anything else is InternalError.
    CommitResult commit(const ReadPlan& plan, const std::vector<QueueReader*>& inputs, std::string& errorMessage);

    /// Skip shares the planner and all alignment rules with read (spec section 7.2).
    CommitResult skip(const ReadPlan& plan, const std::vector<QueueReader*>& inputs, std::string& errorMessage);

    /**
     * @brief Silent leftover-segment discard (spec section 3.1): on inputs where fewer than
     * one aligned block remains before an event, drop the partial segment so the event
     * becomes pending. Returns the slot positions that discarded.
     */
    std::vector<SizeT> discardLeftoverSegments(const std::vector<QueueReader*>& inputs, const CommonModel& model);

private:
    static SizeT effectiveMinimum(const CommonModel& model, SizeT minReadCount);

    bool configured = false;
    LoggerComponentPtr loggerComponent;
};

END_NAMESPACE_OPENDAQ
