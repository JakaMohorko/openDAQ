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
#include <opendaq/multi_reader/data_loss_monitor.h>
#include <opendaq/multi_reader/input.h>
#include <opendaq/multi_reader/read_coordinator.h>
#include <opendaq/multi_reader/reader_state.h>
#include <opendaq/multi_reader/synchronization_manager.h>
#include <opendaq/sample_type.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief The single verdict of one state evaluation: the substate, its diagnostic message and the
 * inputs the message names.
 *
 * One evaluation produces exactly one outcome - every rung of the ladder assigns it and returns -
 * which is why the facade can apply it once, when the evaluation returns, instead of the evaluation
 * writing the facade's state directly.
 */
struct StateOutcome
{
    ReaderState state = ReaderState::WaitingForConnections;
    std::string message;
    std::vector<SizeT> affected;
};

/// Formats "<messagePrefix> [i, j, ...]<messageSuffix>" from the affected indices before moving them
/// into the outcome - never both format and move in one argument list (the evaluation order of
/// function arguments is unspecified).
StateOutcome outcomeWithAffected(ReaderState state,
                                 const char* messagePrefix,
                                 const char* messageSuffix,
                                 std::vector<SizeT> affected);

/// What the shared input guard concluded (see InputGuard).
enum class GuardOutcome
{
    Validated,  ///< the inputs are readable; the guard's usedReaders/slotIndices/mainPosition apply
    Blocked,    ///< some input blocks reading; the guard's blocker holds the substate to report
    NotValid,   ///< the reader is invalid - ErrorState owns that
    NotActive   ///< the reader is deactivated - InactiveState owns that
};

/**
 * @brief Per-evaluation memo of the shared input guard: the rungs every state class has to
 * re-derive before it can trust anything downstream (invalid, the unused-slot drain, active, the
 * used set and main input, connections, events, descriptors, per-input validity).
 *
 * Every class runs the guard - the machine is level-triggered, so any of those conditions can appear
 * at any time, whichever state the reader was in. It must run at most ONCE per evaluation though,
 * which is what this memo is for: it drains queues and clears arrival flags, so a second run would
 * be wasted work on the read path.
 */
struct InputGuard
{
    bool unusedSlotsDrained = false;
    bool checked = false;
    GuardOutcome outcome = GuardOutcome::Validated;

    /// Assigned when the outcome is Blocked.
    StateOutcome blocker;

    /// Valid when the outcome is Validated: the used inputs in slot order, their slot indices and
    /// the position of the main input among them.
    std::vector<QueueReader*> usedReaders;
    std::vector<SizeT> slotIndices;
    SizeT mainPosition = 0;
};

/**
 * @brief Everything one state evaluation works on: the collaborators it drives, the configuration
 * it reads, and where its verdict goes. A view - it owns nothing and is built per evaluation.
 *
 * Deliberately not a back-pointer to the reader. What the evaluation may touch is exactly what is
 * reachable from here, which is what will let the state classes of the refactor be exercised
 * without a live reader (docs/multi_reader_state_refactor.md).
 *
 * Threading: owner thread only, with the facade's state mutex held. Producers reach none of this.
 */
struct StateContext
{
    StateContext(const std::vector<Input*>& slots,
                 SynchronizationManager& syncManager,
                 ReadCoordinator& readCoordinator,
                 DataLossMonitor& dataLossMonitor,
                 std::optional<std::int64_t>& nextReadTick,
                 const std::string& currentMessage)
        : slots(slots)
        , syncManager(syncManager)
        , readCoordinator(readCoordinator)
        , dataLossMonitor(dataLossMonitor)
        , nextReadTick(nextReadTick)
        , currentMessage(currentMessage)
    {
    }

    // --- Collaborators ---
    const std::vector<Input*>& slots;
    SynchronizationManager& syncManager;
    ReadCoordinator& readCoordinator;
    DataLossMonitor& dataLossMonitor;

    /// Common-domain tick of the next unread output sample; assigned when the evaluation
    /// synchronizes, cleared whenever it invalidates the synchronization.
    std::optional<std::int64_t>& nextReadTick;

    /// The message the reader carries right now. Exactly one rung reads it: Error keeps whatever
    /// explains why the reader became invalid.
    const std::string& currentMessage;

    /// The substate the reader carries right now. Read for one purpose only: with isActive it says
    /// which state class the evaluation starts from (stateClassOf) - never as an input to a rung,
    /// which would make the derivation edge-triggered.
    ReaderState currentState = ReaderState::WaitingForConnections;

    // --- Configuration, snapshotted per evaluation ---
    bool invalid = false;
    bool isActive = true;
    SizeT minReadCount = 1;
    StringPtr mainInputId;  ///< explicitly selected main input; null -> first used input
    SampleType resolvedDomainReadType = SampleType::Int64;

    // --- Verdict ---
    StateOutcome outcome;

    /**
     * @brief Deferred facade effects. The status cache and the main-input descriptors are derived
     * from the model but belong to the facade, not to a collaborator, so the evaluation records
     * that they need attention and the facade applies both the moment the evaluation returns.
     *
     * Equivalent to doing it inline, which is what makes the extraction behaviour-preserving:
     * nothing between the rungs that set these flags and the end of the evaluation reads either
     * the caches or the main descriptors, and no status can be built in between (statuses are
     * built on the read and query paths, after the evaluation).
     */
    bool modelInvalidated = false;
    bool mainDescriptorsStale = false;

    /// Memo of the shared input guard for this evaluation.
    InputGuard guard;

    // --- Collaborator operations ---
    /// Used inputs in slot order plus their slot indices; main input is the first used slot.
    std::vector<QueueReader*> collectUsedReaders(std::vector<SizeT>& slotIndices) const;
    /// Slot index of the explicitly selected main input; slotNotFound when the default
    /// (first used input) applies or the selection is dangling.
    SizeT mainSlotIndex() const;

    void invalidateSynchronization();
    /// Topology or configuration change invalidating the cross-input model itself; also marks the
    /// facade's model-derived caches for invalidation.
    void invalidateModel();

    /// Adopts unused inputs' queued event packets so they surface in the per-input states and fire
    /// the callback gate.
    void drainUnusedSlots();
    /// Failure-state recovery: a failed input with a corrective descriptor change buried behind
    /// unreadable stale data drops that data (dropForInactive semantics) so the event can surface.
    /// Returns true when any event became pending.
    bool exposeBuriedEvents(const std::vector<SizeT>& affected);

    /// The synchronized start as a tick of the reader's resolved domain read type; empty while not
    /// synchronized or when the start does not fit that type.
    std::optional<std::int64_t> readOffset() const;
};

/// Drop the synchronized start and everything derived from it. Shared by the state evaluation and
/// by the facade's own configuration paths, so it is a free function over the collaborators.
void invalidateSynchronization(SynchronizationManager& syncManager,
                               ReadCoordinator& readCoordinator,
                               std::optional<std::int64_t>& nextReadTick);

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
