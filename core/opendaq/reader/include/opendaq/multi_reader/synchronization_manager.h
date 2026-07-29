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
#include <opendaq/multi_reader/queue_reader.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Everything derived from more than one input: the common domain, common
 * sample rate, per-input dividers, the minimum aligned block and - while
 * synchronized - the common start.
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

    /// Common-domain ticks per common-rate sample; integral by construction because the
    /// common resolution folds in 1/commonSampleRate. Zero when the model is not usable.
    std::int64_t ticksPerCommonSample() const
    {
        // {0, 0} is TickResolution's unassigned sentinel
        if (commonDomain.resolution.den == 0 || commonSampleRate <= 0)
            return 0;
        return commonDomain.resolution.den / (commonDomain.resolution.num * commonSampleRate);
    }
};

enum class SyncSetupIssue
{
    None = 0,
    MissingDomainDescriptor,
    InvalidSampleRate,
    RatesNotEqual,
    RequiredRateNotDivisible,
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
 * @brief Owns the cross-input domain/rate model and the alignment algorithm.
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

    // --- Checked 64-bit arithmetic (overflow => Incompatible, never wraparound) ---
    /// Positive operands only; nullopt on overflow or non-positive input.
    static std::optional<std::int64_t> checkedMultiply(std::int64_t a, std::int64_t b);
    static std::optional<std::int64_t> checkedLcm(std::int64_t a, std::int64_t b);
    /**
     * @brief Rational GCD: gcd(numerators) / lcm(denominators) over simplified ratios -
     * the coarsest resolution every input resolution is an integer multiple of
     * (1/10 and 1/15 yield 1/30). nullopt on overflow or invalid ratio.
     */
    static std::optional<TickResolution> rationalGcd(const std::vector<TickResolution>& ratios);

    /**
     * @brief Cross-input checks and (re)construction of the CommonModel.
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

    SyncSetupResult buildCommonModelImpl(const std::vector<QueueReader*>& inputs,
                                         const std::vector<SizeT>& slotIndices,
                                         SizeT mainPosition);

    bool hasModel() const;
    const CommonModel& getModel() const;

    /**
     * @brief The iterative alignment step. Preconditions (validity, no pending events,
     * data on every input) are the state evaluation's job. On Synchronized the model's
     * commonStart is assigned; every other outcome leaves it null.
     *
     * Reached-value acceptance: an input whose sample grid is phase-offset from the aligned
     * start grid can never reach the candidate tick exactly. Its reached value is accepted
     * when it lies strictly within half the aligned block interval of the candidate - the
     * sample then unambiguously belongs to the candidate's block, and the offset stays visible
     * in the per-signal domain output. An offset of half the block interval or more is
     * ambiguous and keeps re-targeting until the iteration bound reports NoCommonTick.
     */
    SyncResult synchronize(const std::vector<QueueReader*>& inputs, const std::vector<SizeT>& slotIndices);

    /// Any returned event, disconnect, descriptor/config/used-set change clears the
    /// synchronized start (the read pipelines are the coordinator's to clear).
    void clearSynchronization();

    /// Topology or configuration change invalidating the cross-input model itself.
    void invalidateModel();

    /// Common-domain start of the synchronized output; null while not synchronized.
    const DomainValue* getCommonStart() const;

private:
    struct CandidatePick
    {
        std::unique_ptr<DomainValue> value;  // chosen start (common domain); null on failure
        std::optional<SyncResult> failure;   // set instead when no start tick exists
    };

    struct AdvanceOutcomes
    {
        std::vector<std::unique_ptr<DomainValue>> reached;  // per input (common domain); only filled on Success
        std::vector<SizeT> pendingEventInputs;              // slot indices blocked by an unconsumed event
        std::vector<SizeT> needMoreDataInputs;              // slot indices that ran out of data
        bool overshoot = false;                             // a cursor moved past the candidate
    };

    /// synchronize() step 1: every input's first unread sample converted to the common domain
    /// (exact by construction). Returns a NeedMoreData result when an input has nothing unread.
    std::optional<SyncResult> collectFirstSamples(const std::vector<QueueReader*>& inputs,
                                                  const std::vector<SizeT>& slotIndices,
                                                  std::vector<std::unique_ptr<DomainValue>>& firstSamples) const;

    /// synchronize() step 2: no input's start may lag the latest start by more than the
    /// configured maximum synchronization distance (zero disables the check).
    std::optional<SyncResult> checkSynchronizationDistance(const std::vector<std::unique_ptr<DomainValue>>& firstSamples,
                                                           const std::vector<SizeT>& slotIndices) const;

    /// synchronize() step 3: choose the tick every input should start on. The candidate is an
    /// independent copy, so @p firstSamples stays intact on every path (including the failure
    /// ones); the search itself is documented at the definition.
    CandidatePick pickStartCandidate(const std::vector<std::unique_ptr<DomainValue>>& firstSamples,
                                     const std::vector<SizeT>& slotIndices) const;

    /// synchronize() step 4: advance every input's cursor to the candidate and classify the
    /// outcomes. The candidate is non-const only because DomainValue::fromDomain is non-const;
    /// its value is not changed.
    AdvanceOutcomes advanceAllInputs(const std::vector<QueueReader*>& inputs,
                                     const std::vector<SizeT>& slotIndices,
                                     DomainValue& candidate) const;

    RatioPtr startInterval() const;

    /// Aligned block interval in common-domain ticks (blockLcm * ticks per common-rate sample).
    std::int64_t blockIntervalTicks() const;
    /// Reached-value acceptance rule documented on synchronize().
    bool reachedAcceptable(const DomainValue& reached, const DomainValue& candidate) const;

    std::int64_t requiredCommonSampleRate = -1;
    bool allowDifferentRates = true;
    bool startOnFullUnitOfDomain = false;
    std::chrono::system_clock::duration maxSynchronizationDistance{0};

    bool modelValid = false;
    CommonModel model;

    /**
     * @brief The start tick already decided on but not yet reached by every input - the latched
     * synchronization target. This is why synchronize() is stateful across calls.
     *
     * NeedMoreData means "this target is right, some input has not received the packets to reach it
     * yet", so the target must survive until it is reached. Recomputing it would let each retry
     * derive a LATER start: a failed attempt leaves the inputs that did reach the target sitting on
     * it, so the next round sees those advanced positions as its first samples and rounds up from
     * them, ratcheting the common start forward a block at a time.
     *
     * Deliberately NOT a member of CommonModel: buildCommonModelImpl assigns `model = CommonModel{}`
     * wholesale, and the state ladder rebuilds the model on every evaluation while commonStart is
     * null - which is exactly the window this latch has to survive.
     *
     * Lifetime: set only on the NeedMoreData exit of synchronize(); consumed when the target is
     * reached (moved into model.commonStart); dropped by everything that can move the grid or the
     * data underneath it - invalidateModel (descriptor change / model rebuild), clearSynchronization
     * (disconnect, used-set change, pending event, data loss, config change), an overshoot, or a
     * reached value the grid did not predict.
     */
    std::unique_ptr<DomainValue> pendingCandidate;

    LoggerComponentPtr loggerComponent;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
