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
#include <opendaq/domain_value.h>
#include <opendaq/logger_component_ptr.h>
#include <opendaq/queue_reader.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Everything derived from more than one input (spec sections 3.3 and 4):
 * the common domain, common sample rate, per-input dividers, the minimum aligned
 * block and - while synchronized - the common start.
 *
 * The divider vector is parallel to the used-input vector the model was built from;
 * the model is rebuilt whenever the used set, descriptors or configuration change.
 */
struct CommonModel
{
    DomainInfo commonDomain;
    std::int64_t commonSampleRate = -1;
    std::vector<SizeT> sampleRateDividers;
    SizeT blockLcm = 1;
    std::unique_ptr<DomainValue> commonStart;  // assigned only while synchronized
    SizeT mainPosition = 0;                    // position of the main input within the used-input vector
};

enum class SyncSetupIssue
{
    None = 0,
    MissingDomainDescriptor,
    InvalidSampleRate,
    RatesNotEqual,
    RequiredRateNotDivisible,
    ReferenceDomainIncompatible,
    ArithmeticOverflow
};

struct SyncSetupResult
{
    SyncSetupIssue issue = SyncSetupIssue::None;
    std::vector<SizeT> affectedInputs;  // slot indices
    std::string message;

    bool ok() const
    {
        return issue == SyncSetupIssue::None;
    }
};

enum class SyncOutcome
{
    Synchronized = 0,
    NeedMoreData,
    EventPending,
    Failed
};

enum class SyncFailureReason
{
    None = 0,
    SyncDistanceExceeded,
    TargetNotRepresentable,
    NoCommonTick
};

struct SyncResult
{
    SyncOutcome outcome;
    SyncFailureReason reason = SyncFailureReason::None;
    std::vector<SizeT> affectedInputs;  // slot indices
    std::string message;
};

/**
 * @brief Owns the cross-input domain/rate model and the alignment algorithm (spec sections 4 and 5).
 *
 * The manager never owns queues, never consumes reportable events and never reads user buffers;
 * it drives the per-input QueueReaders it is handed (advance/divider assignment only). All inputs
 * of one reader must share a single integral domain read type so their DomainValues are comparable
 * after conversion to the common domain - the multi reader constructs its QueueReaders that way.
 *
 * Whether the reader is synchronized is the state machine's call, never "is commonStart assigned".
 */
class SynchronizationManager
{
public:
    explicit SynchronizationManager(const LoggerComponentPtr& logger);

    // --- Configuration ---
    void setRequiredCommonSampleRate(std::int64_t rate);  // -1 disables (default)
    void setAllowDifferentRates(bool allow);              // default true
    void setStartOnFullUnitOfDomain(bool enabled);        // default false
    /// Zero disables (default). Enforced during synchronize() with per-input diagnostics.
    void setMaxSynchronizationDistance(std::chrono::system_clock::duration distance);

    // --- Checked 64-bit arithmetic (spec section 4.1: overflow => Incompatible, never wraparound) ---
    /// Positive operands only; nullopt on overflow or non-positive input.
    static std::optional<std::int64_t> checkedMultiply(std::int64_t a, std::int64_t b);
    static std::optional<std::int64_t> checkedLcm(std::int64_t a, std::int64_t b);
    /**
     * @brief Rational GCD: gcd(numerators) / lcm(denominators) over simplified ratios -
     * the coarsest resolution every input resolution is an integer multiple of
     * (1/10 and 1/15 yield 1/30). nullopt on overflow or invalid ratio.
     */
    static std::optional<RatioPtr> rationalGcd(const std::vector<RatioPtr>& ratios);

    /**
     * @brief Evaluation step 8: cross-input checks and (re)construction of the CommonModel.
     * Assigns each reader its sample-rate divider on success. commonStart is cleared.
     *
     * The common resolution additionally folds in 1/commonSampleRate so one output sample
     * period is always a whole number of common ticks (a required rate above every input
     * rate refines the resolution; the plain LCM-rate case is unaffected).
     *
     * @param inputs Used inputs in slot order.
     * @param slotIndices Parallel slot indices, used for diagnostics.
     * @param mainPosition Position of the main input within @p inputs.
     */
    SyncSetupResult buildCommonModel(const std::vector<QueueReader*>& inputs,
                                     const std::vector<SizeT>& slotIndices,
                                     SizeT mainPosition);

    bool hasModel() const;
    const CommonModel& getModel() const;

    /**
     * @brief Evaluation step 11: the iterative alignment of spec section 5. Preconditions
     * (validity, no pending events, data on every input) are the state evaluation's job.
     * On Synchronized the model's commonStart is assigned; every other outcome leaves it null.
     */
    SyncResult synchronize(const std::vector<QueueReader*>& inputs, const std::vector<SizeT>& slotIndices);

    /// Spec section 5 invalidation: any returned event, disconnect, descriptor/config/used-set
    /// change clears the synchronized start (the read pipelines are the coordinator's to clear).
    void clearSynchronization();

    /// Topology or configuration change invalidating the cross-input model itself.
    void invalidateModel();

    /// Common-domain start of the synchronized output; null while not synchronized.
    const DomainValue* getCommonStart() const;

private:
    SyncSetupResult checkReferenceDomains(const std::vector<QueueReader*>& inputs, const std::vector<SizeT>& slotIndices) const;
    RatioPtr startInterval() const;

    std::int64_t requiredCommonSampleRate = -1;
    bool allowDifferentRates = true;
    bool startOnFullUnitOfDomain = false;
    std::chrono::system_clock::duration maxSynchronizationDistance{0};

    bool modelValid = false;
    CommonModel model;

    LoggerComponentPtr loggerComponent;
};

END_NAMESPACE_OPENDAQ
