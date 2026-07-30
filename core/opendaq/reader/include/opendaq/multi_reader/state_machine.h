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
#include <opendaq/multi_reader/state_context.h>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief The reader's behaviours. Each names a set of circumstances in which the reader behaves one
 * way - how it reads, what it reports available, how it gates producers, how it reacts to the world
 * changing - and the ReaderState substates that share that behaviour.
 *
 * The partition is the public ReadStatus, which is not a coincidence: that enum is the consumer's
 * view of exactly this question.
 */
enum class StateId
{
    Error,         ///< Error. Every operation fails; terminal and latched
    Inactive,      ///< Inactive. No data flow; events are still adopted and surfaced
    Establishing,  ///< WaitingForConnections/Descriptors/Data, Synchronizing. Nothing readable yet
    InputsFailed,  ///< Incompatible, SynchronizationFailed, DataLost. Persistent, needs consumer action
    Ready          ///< Synchronized. Aligned blocks readable; the only state with a data-plane fast path
};

const char* stateName(StateId id);

/**
 * @brief The behaviour a substate belongs to.
 *
 * EventPending is deliberately absent from the table: holding unconsumed events is a condition every
 * behaviour answers from within (its read returns them, its availability is zero, its gate opens on
 * the event flag), not a behaviour of its own. Reporting it always clears the synchronization, so what
 * remains is re-establishment - or the inactive behaviour, if the reader is deactivated.
 */
StateId stateIdFor(ReaderState substate, bool isActive);

/// What kind of read this is, decided by the behaviour and executed by the facade (which owns the
/// status caches and the read pipelines).
enum class ReadAction
{
    ReportState,   ///< nothing to return but the current state and its diagnostics
    ReturnEvents,  ///< unconsumed events must be returned before any data
    ServeData      ///< plan and commit against the aligned availability
};

/**
 * @brief One behaviour of the multi reader.
 *
 * Stateless flyweights: one instance per class (stateFor), all mutable data reached through the
 * StateContext. Owner thread only, with the facade's state mutex held.
 */
class MultiReaderState
{
public:
    virtual ~MultiReaderState() = default;

    virtual StateId id() const = 0;

    // The base implementations below are the conservative ones: nothing is readable, every packet
    // forces a full derivation, and the gate uses establishment semantics. ReadyState is the only
    // behaviour that can do better, because it is the only one where a read can provably return
    // samples - so it is the only one that overrides them.

    /**
     * @brief Publish the producer-facing gate policy for every slot: each slot's basis (adopted
     * availability until its next event, plus whether any event packet is adopted), its ready
     * threshold, and whether every packet must force an evaluation.
     *
     * This is how the lock-free producer path becomes state-aware without calling into the state:
     * the policy is published to the slots, and Input::packetReceived reads it.
     */
    virtual void publishProducerGate(StateContext& ctx) const;

    /**
     * @brief The data-plane pass of the read and query paths: adopt what arrived, maintain the gate
     * flags, and publish the availability the read path plans from.
     *
     * @param escalateOnEvent the read path surfaces events (true); the query path only records them
     *        for the gate and leaves the surfacing to the next read.
     * @return true when the caller must run the full derivation afterwards.
     */
    virtual bool refreshDataPlane(StateContext& ctx, bool escalateOnEvent) const;

    /**
     * @brief The coalesced task's pass: decides only whether onDataAvailable should fire. Maintains
     * the gate flags without deriving the state, and skips any slot that already satisfies the gate.
     * @return true when the caller must run the full derivation instead.
     */
    virtual bool updateCallbackState(StateContext& ctx) const;

    /// Samples a read can return right now, in common-rate samples.
    virtual SizeT availableCount(StateContext& ctx) const;

    /**
     * @brief What a read gets from this state. Holding unconsumed events is answered here rather than
     * being a state of its own: every behaviour returns them before anything else.
     */
    virtual ReadAction planRead(StateContext& ctx) const;

    /**
     * @brief The timed read's wait predicate: can this state serve @p requested samples now? A
     * request of zero is the event handshake, which only events can satisfy.
     */
    virtual bool readWaitSatisfied(StateContext& ctx, SizeT requested) const;

    /**
     * @brief Is this still the right behaviour, and with which substate?
     *
     * Checks this behaviour's own exit conditions against ground truth - not the whole precedence
     * order, which is what makes it cheaper than deriving from scratch. Level-triggered all the same:
     * it re-reads ground truth, it never trusts what a notification said earlier.
     *
     * The base implementation is the exhaustive derivation, which is the correct answer for any
     * behaviour that has no cheaper way to prove it still applies - a state whose whole job is looking
     * for progress cannot shortcut the search. Leaving a behaviour is always a full derivation: the
     * conditions below the one that changed have not been looked at since.
     */
    virtual StateOutcome reassess(StateContext& ctx) const;

    // --- How this behaviour responds to the world changing --------------------------------------
    // Each returns the substate to report, like reassess. The base reactions invalidate what the
    // change can have invalidated and then derive from scratch, which is the right answer whenever
    // the change can move a cross-input conclusion.

    /// A signal was connected to a slot's port (which may also be a signal REPLACING another - the
    /// port reports no disconnect for that).
    virtual StateOutcome slotConnected(StateContext& ctx, SizeT slot) const;
    virtual StateOutcome slotDisconnected(StateContext& ctx, SizeT slot) const;
    /// setActive, after the facade has switched the ports and dropped what deactivation drops.
    virtual StateOutcome activeChanged(StateContext& ctx) const;
    /// addInput, removeInput, setInputUsed, setMainInput.
    virtual StateOutcome inputSetChanged(StateContext& ctx) const;
};

/// The flyweight for a behaviour; the same instance for every reader, since they hold no data.
const MultiReaderState& stateFor(StateId id);

/**
 * @brief Derive the reader's substate from ground truth and perform the side effects of reaching it.
 *
 * Exhaustive: it re-derives every condition in precedence order, whatever state the reader was in.
 * That makes it the reference answer, which is why it stays even once the behaviours narrow their own
 * reassessment to their own exit conditions - the characterization tests cross-check against it.
 *
 * Owner thread, facade state mutex held. The caller applies the outcome, updates the current
 * behaviour and publishes the producer gate.
 */
StateOutcome deriveState(StateContext& ctx);

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
