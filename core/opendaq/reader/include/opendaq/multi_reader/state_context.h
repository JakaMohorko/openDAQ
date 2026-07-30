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

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief The single verdict of one state derivation: the substate, its diagnostic message and the
 * inputs the message names.
 *
 * One derivation produces exactly one outcome - every rung returns as soon as it decides - which is
 * why the facade applies it once, when the derivation returns, instead of the state code writing the
 * facade's state directly.
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

/**
 * @brief The synchronized fast path's state: what the last pass adopted and published, plus the
 * scratch it reuses so no availability query allocates.
 *
 * Owned by the facade because it has to survive across calls (the states maintain it, they do not own
 * it). `dirty` is the one field a producer touches - Input's lock-free notification sets it - so it is
 * atomic; everything else is owner-thread only under the state mutex.
 *
 * `availableCommon` and `slotAvailable` are valid ONLY while `availableValid`: a non-escalating
 * synchronized pass sets it, and any escalation, non-synchronized pass or consuming read clears it,
 * after which consumers fall back to a direct walk.
 */
struct DataPlane
{
    std::atomic_bool dirty{true};
    /// A read consumed since the last pass, so a buried event may now be leading.
    bool consumed = false;

    std::vector<SizeT> slotAvailable;
    SizeT availableCommon = 0;
    bool availableValid = false;

    /// Reused across calls; kept separate from the read path's own scratch so the two never alias.
    std::vector<QueueReader*> scratchReaders;
    std::vector<SizeT> scratchSlotIndices;
};

/**
 * @brief Everything the state code works on: the collaborators it drives, the configuration it
 * reads, and where its verdict goes. A view - it owns nothing and is built per operation.
 *
 * Deliberately not a back-pointer to the reader: what the state code may touch is exactly what is
 * reachable from here, which is what lets a behaviour be exercised without a live reader
 * (docs/multi_reader_state_refactor.md).
 *
 * Threading: owner thread only, with the facade's state mutex held. Producers reach none of this.
 */
struct StateContext
{
    // Every member is a reference, deliberately: the context is built on the read and query paths, so
    // construction has to be a handful of pointer stores. Nothing is snapshotted, which also means a
    // context stays correct across an escalation that changes the state under it.
    StateContext(const std::vector<Input*>& slots,
                 SynchronizationManager& syncManager,
                 ReadCoordinator& readCoordinator,
                 DataLossMonitor& dataLossMonitor,
                 DataPlane& dataPlane,
                 std::optional<std::int64_t>& nextReadTick,
                 const std::string& currentMessage,
                 const ReaderState& substate,
                 const StringPtr& mainInputId,
                 const bool& invalid,
                 const bool& isActive,
                 const SizeT& minReadCount,
                 const SampleType& resolvedDomainReadType)
        : slots(slots)
        , syncManager(syncManager)
        , readCoordinator(readCoordinator)
        , dataLossMonitor(dataLossMonitor)
        , dataPlane(dataPlane)
        , nextReadTick(nextReadTick)
        , currentMessage(currentMessage)
        , substate(substate)
        , mainInputId(mainInputId)
        , invalid(invalid)
        , isActive(isActive)
        , minReadCount(minReadCount)
        , resolvedDomainReadType(resolvedDomainReadType)
    {
    }

    // --- Collaborators ---
    const std::vector<Input*>& slots;
    SynchronizationManager& syncManager;
    ReadCoordinator& readCoordinator;
    DataLossMonitor& dataLossMonitor;
    DataPlane& dataPlane;

    /// Common-domain tick of the next unread output sample; assigned when the evaluation
    /// synchronizes, cleared whenever it invalidates the synchronization.
    std::optional<std::int64_t>& nextReadTick;

    /// The message the reader carries right now. Exactly one rung reads it: Error keeps whatever
    /// explains why the reader became invalid.
    const std::string& currentMessage;

    /// The substate the reader is currently reporting - its own published diagnosis, which a
    /// behaviour may act on (holding unconsumed events is the case that matters). Not an input to the
    /// derivation, which always starts from ground truth.
    const ReaderState& substate;

    /// Explicitly selected main input; null -> first used input. By reference: the context is built
    /// per operation, including on the read path, and a StringPtr copy is a refcount bump per read.
    const StringPtr& mainInputId;

    // --- Configuration ---
    const bool& invalid;
    const bool& isActive;
    const SizeT& minReadCount;
    const SampleType& resolvedDomainReadType;

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
