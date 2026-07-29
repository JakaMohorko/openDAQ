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

#include <optional>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief The state classes of the multi reader. Each groups the ReaderState substates that share a
 * class invariant, and owns the rungs that produce them (docs/multi_reader_state_refactor.md §3.1).
 */
enum class StateId
{
    Error,                  ///< Error. Terminal, latched, no exit
    Inactive,               ///< Inactive, EventPending. Deactivated: no data flow, events still surface
    WaitingForValidInputs,  ///< WaitingForConnections, WaitingForDescriptors, Incompatible, EventPending
    Synchronizing,          ///< WaitingForData, Synchronizing, SynchronizationFailed, DataLost
    Ready                   ///< Synchronized
};

/**
 * @brief The class a substate belongs to.
 *
 * The class is a pure function of the substate plus the active flag - the classes are defined as a
 * grouping of the substates, so there is nothing to store and nothing that can desynchronize. The
 * one substate reachable from two classes is EventPending, which the active flag disambiguates: an
 * inactive reader still surfaces events, but its vocabulary is only {Inactive, EventPending}.
 */
StateId stateClassOf(ReaderState substate, bool isActive);

const char* stateClassName(StateId id);

/**
 * @brief What one criteria check concluded: either the verdict for this evaluation, or a hand-off to
 * the class that owns the rungs still to be run.
 *
 * A hand-off carries no substate on purpose - the receiving class derives it, which is what keeps
 * one condition in one place. A verdict carries the substate, and the class it belongs to follows
 * from stateClassOf, so a verdict is final even when it names another class's substate.
 */
struct Transition
{
    static Transition settled(StateOutcome outcome)
    {
        Transition transition;
        transition.outcome = std::move(outcome);
        return transition;
    }

    static Transition settled(ReaderState substate, std::string message = {}, std::vector<SizeT> affected = {})
    {
        return settled(StateOutcome{substate, std::move(message), std::move(affected)});
    }

    static Transition handOff(StateId target)
    {
        Transition transition;
        transition.target = target;
        return transition;
    }

    bool isHandOff() const
    {
        return target.has_value();
    }

    std::optional<StateId> target;
    StateOutcome outcome;
};

/**
 * @brief One state class of the multi reader.
 *
 * Stateless flyweights: one instance per class (stateFor), all mutable data reached through the
 * StateContext. Owner thread only, with the facade's state mutex held.
 */
class MultiReaderState
{
public:
    virtual ~MultiReaderState() = default;

    virtual StateId id() const = 0;

    /**
     * @brief Level-triggered re-derivation from ground truth: does this class still apply, with
     * which substate, and if not, which class owns the rest?
     *
     * This is the authority on what state the reader is in. It is not an optimization over an
     * edge-triggered step - there is no edge-triggered step. See §2 of the refactor spec: producers
     * are lock-free, so arbitrary amounts of data and any number of events can arrive between two
     * evaluations with no ordered notification the owner observes, and the input set is mutable at
     * runtime.
     */
    virtual Transition checkTransitionCriteria(StateContext& ctx) const = 0;
};

/// The flyweight for a class; the same instance for every reader, since they hold no data.
const MultiReaderState& stateFor(StateId id);

/**
 * @brief Run the machine to a verdict and assign @p ctx.outcome exactly once.
 *
 * Starts from the class the reader's current substate belongs to and follows hand-offs until a class
 * settles. Bounded: a hand-off chain can only ever run downstream (Error/Inactive from anywhere,
 * WaitingForValidInputs -> Synchronizing -> Ready), so it cannot cycle; the bound is a backstop for
 * an invariant break, not a truncation.
 *
 * Owner thread, facade state mutex held; the caller applies the outcome and publishes the
 * producer-facing gate state afterwards.
 */
void runStateEvaluation(StateContext& ctx);

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
