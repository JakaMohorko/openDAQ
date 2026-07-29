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
#include <opendaq/multi_reader/state_machine.h>

#include <fmt/format.h>

#include <algorithm>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

StateId stateClassOf(ReaderState substate, bool isActive)
{
    switch (substate)
    {
        case ReaderState::Error:
            return StateId::Error;
        case ReaderState::Inactive:
            return StateId::Inactive;
        case ReaderState::EventPending:
            // The one substate two classes share - the inactive arm surfaces events too, and that is
            // the whole of its vocabulary beyond Inactive itself
            return isActive ? StateId::WaitingForValidInputs : StateId::Inactive;
        case ReaderState::WaitingForConnections:
        case ReaderState::WaitingForDescriptors:
        case ReaderState::Incompatible:
            return StateId::WaitingForValidInputs;
        case ReaderState::WaitingForData:
        case ReaderState::Synchronizing:
        case ReaderState::SynchronizationFailed:
        case ReaderState::DataLost:
            return StateId::Synchronizing;
        case ReaderState::Synchronized:
            return StateId::Ready;
    }
    return StateId::WaitingForValidInputs;
}

const char* stateClassName(StateId id)
{
    switch (id)
    {
        case StateId::Error:
            return "ErrorState";
        case StateId::Inactive:
            return "InactiveState";
        case StateId::WaitingForValidInputs:
            return "WaitingForValidInputsState";
        case StateId::Synchronizing:
            return "SynchronizingState";
        case StateId::Ready:
            return "ReadyState";
    }
    return "<unknown>";
}

namespace
{

/**
 * @brief The effects that follow from the substate itself rather than from the rung that decided it.
 * Applied once, where the machine settles, so the rule holds on every path to that substate instead
 * of being restated at each of them.
 *
 * Level-triggered like everything else here: applied whenever the machine settles on the substate,
 * not only on the transition into it. That is exactly what the rungs did before, and it is
 * idempotent - clearing an already-cleared synchronization is a no-op.
 *
 * Only two substates have an effect that is genuinely theirs. A returnable event and a missed
 * deadline both mean the aligned stream is finished: the event has to be consumed before data can be
 * read again, and an input that went silent cannot contribute to the block being assembled. Neither
 * can leave a synchronized start behind, on any path.
 *
 * Everything else stays with the rung that decides it, because the effect differs per path even for
 * one substate - see the notes in the classes below and §5.1 of docs/multi_reader_state_refactor.md.
 */
void applyStateInvariants(StateContext& ctx, ReaderState substate)
{
    switch (substate)
    {
        case ReaderState::EventPending:
        case ReaderState::DataLost:
            ctx.invalidateSynchronization();
            break;
        default:
            break;
    }
}

// --- Rungs shared by more than one class -------------------------------------------------------

/// Unused inputs stay observable. Their ports are inactive, so data is dropped at the connection and
/// only events can arrive; draining them makes pending events visible in the per-input states and
/// lets them fire the dataAvailable callback (the recovery signal a consumer answers with
/// setInputUsed(id, true)). Runs before anything that depends on the active flag, and at most once
/// per evaluation.
void drainUnusedSlotsOnce(StateContext& ctx)
{
    if (ctx.guard.unusedSlotsDrained)
        return;
    ctx.guard.unusedSlotsDrained = true;
    ctx.drainUnusedSlots();
}

/**
 * @brief The shared input guard - everything that has to hold before any downstream class can trust
 * its own conclusions. In ladder terms: rung 1 (invalid), the unused-slot drain, the active flag,
 * rung 2 (used set and main input), rung 3 (connections), the data-loss monitoring refresh and the
 * leftover-segment discard, rung 4/5 (events), rung 6 (descriptors) and rung 7 (per-input validity).
 *
 * Fills ctx.guard: either a blocker to report, or the used inputs for the rungs after it.
 */
GuardOutcome computeInputGuard(StateContext& ctx)
{
    auto& guard = ctx.guard;

    // 1. Error is terminal; inactivity gates everything else
    if (ctx.invalid)
        return GuardOutcome::NotValid;

    drainUnusedSlotsOnce(ctx);

    if (!ctx.isActive)
        return GuardOutcome::NotActive;

    // 2. Resolve the used set and the main input (the explicitly selected main input is
    // never silently replaced)
    guard.usedReaders = ctx.collectUsedReaders(guard.slotIndices);
    guard.mainPosition = 0;
    const auto& usedReaders = guard.usedReaders;
    const auto& slotIndices = guard.slotIndices;

    if (usedReaders.empty())
    {
        ctx.invalidateSynchronization();
        guard.blocker = {ReaderState::WaitingForConnections, "No used inputs", {}};
        return GuardOutcome::Blocked;
    }

    if (ctx.mainInputId.assigned())
    {
        const auto mainSlot = ctx.mainSlotIndex();
        const auto position = std::find(slotIndices.begin(), slotIndices.end(), mainSlot);
        if (mainSlot == slotNotFound || position == slotIndices.end())
        {
            ctx.invalidateModel();
            guard.blocker = {ReaderState::WaitingForConnections,
                             "The selected main input is not among the used inputs",
                             mainSlot == slotNotFound ? std::vector<SizeT>{} : std::vector<SizeT>{mainSlot}};
            return GuardOutcome::Blocked;
        }
        guard.mainPosition = static_cast<SizeT>(position - slotIndices.begin());
    }

    // 3. Adopt what the producers enqueued. Connectivity itself is NOT polled here: every
    // connect/disconnect/reconnect reaches the slot as a port callback, and Input::attach replays
    // the two callbacks the port skips for a port that was already connected when the listener was
    // installed (adoption, and the reader's own createOrAdoptPorts connect). So isConnected() is
    // authoritative; only the queue contents need collecting, because the lock-free producer path
    // cannot hand them over itself.
    {
        std::vector<SizeT> unconnected;
        for (const auto index : slotIndices)
        {
            // Clear the arrival flag BEFORE draining (clear-then-drain). The producer path is
            // lock-free, so a packet enqueued after this clear re-arms the flag and is caught by
            // the next pass; clearing AFTER the drain would instead wipe the flag of a packet
            // enqueued in the drain->clear window without ever adopting it, stranding it on the
            // connection (the availability-undercount race).
            ctx.slots[index]->clearPacketPending();
            ctx.slots[index]->adoptQueuedPackets();
            if (!ctx.slots[index]->isConnected())
                unconnected.push_back(index);
        }
        if (!unconnected.empty())
        {
            // Connections gate events: while a used input has no signal, no event is
            // returnable, so the callback must not fire on the
            // events already queued on the connected inputs
            for (const auto index : slotIndices)
                setSlotEvent(*ctx.slots[index], false);

            ctx.invalidateModel();
            guard.blocker = outcomeWithAffected(ReaderState::WaitingForConnections, "Inputs", " have no signal connected", std::move(unconnected));
            return GuardOutcome::Blocked;
        }
    }

    // Data-loss monitoring covers exactly the used, connected inputs of an active reader;
    // everything else is unmonitored and disarmed
    for (SizeT i = 0; i < ctx.slots.size(); ++i)
    {
        const bool monitored = ctx.slots[i]->isUsed() && ctx.slots[i]->isConnected();
        ctx.dataLossMonitor.setMonitored(i, monitored);
    }

    // While synchronized, partial blocks in front of an event are silently discarded so
    // the event can surface
    if (ctx.syncManager.getCommonStart() != nullptr && ctx.syncManager.hasModel())
        ctx.readCoordinator.discardLeftoverSegments(usedReaders, ctx.syncManager.getModel(), ctx.minReadCount);

    // 4./5. Refresh queues; pending events preempt everything below
    {
        std::vector<SizeT> eventInputs;
        bool handshakeInFlight = false;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            // packetPending was already cleared before the step-3 drain (clear-then-drain);
            // clearing again here would re-open the drain->clear race, so it is intentionally
            // not cleared in this pass.
            const bool hasEvents = usedReaders[position]->hasPendingEvents();
            if (hasEvents)
                eventInputs.push_back(slotIndices[position]);
            setSlotEvent(*ctx.slots[slotIndices[position]], hasEvents);

            // A connected input with neither descriptors nor events is still completing its
            // connect handshake: the signal's initial descriptor event has not been enqueued
            // yet (connections are constructed in steps and evaluations can run in between)
            if (!hasEvents && !usedReaders[position]->getValueDescriptor().assigned() &&
                !usedReaders[position]->getDomainDescriptor().assigned())
            {
                handshakeInFlight = true;
            }
        }
        // While a connect handshake is in flight the reader is not yet event-ready: the
        // in-flight input's initial descriptor event arrives momentarily and re-triggers
        // evaluation, so both the dataAvailable callback and blocked reads see every
        // input's initial event at once. The evaluation falls through to step 6, which
        // truthfully reports the handshaking input as WaitingForDescriptors.
        if (handshakeInFlight)
        {
            for (const auto index : slotIndices)
                setSlotEvent(*ctx.slots[index], false);
        }
        else if (!eventInputs.empty())
        {
            // Descriptors apply when leading events are consumed, so the cross-input model
            // can be built opportunistically - accessors like getCommonSampleRate and
            // getTickResolution work right after construction, like they always have
            if (!ctx.syncManager.hasModel())
            {
                bool modelBuildable = true;
                for (auto* reader : usedReaders)
                {
                    if (!reader->getValueDescriptor().assigned() || !reader->getDomainDescriptor().assigned() || !reader->isValid())
                        modelBuildable = false;
                }
                if (modelBuildable)
                    ctx.syncManager.buildCommonModel(usedReaders, slotIndices, guard.mainPosition);
            }

            // The synchronization is dropped by applyStateInvariants - it holds for every path to
            // EventPending, not just this one. The model built above deliberately survives.
            guard.blocker = outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(eventInputs));
            return GuardOutcome::Blocked;
        }
    }

    // 6. Descriptors
    {
        std::vector<SizeT> missing;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            if (!usedReaders[position]->getValueDescriptor().assigned() || !usedReaders[position]->getDomainDescriptor().assigned())
                missing.push_back(slotIndices[position]);
        }
        if (!missing.empty())
        {
            guard.blocker = outcomeWithAffected(ReaderState::WaitingForDescriptors, "Inputs", " have no descriptors yet", std::move(missing));
            return GuardOutcome::Blocked;
        }
    }

    // The facade refreshes its main-input descriptors from here on (deferred to the end of the
    // evaluation; see StateContext::mainDescriptorsStale)
    ctx.mainDescriptorsStale = true;

    // 7. Local validity
    {
        std::vector<SizeT> invalidInputs;
        for (SizeT position = 0; position < usedReaders.size(); ++position)
        {
            if (!usedReaders[position]->isValid())
                invalidInputs.push_back(slotIndices[position]);
        }
        if (!invalidInputs.empty())
        {
            ctx.invalidateModel();
            if (ctx.exposeBuriedEvents(invalidInputs))
            {
                for (const auto index : invalidInputs)
                    setSlotEvent(*ctx.slots[index], ctx.slots[index]->getQueueReader().hasPendingEvents());
                guard.blocker = outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(invalidInputs));
                return GuardOutcome::Blocked;
            }
            guard.blocker = outcomeWithAffected(
                ReaderState::Incompatible, "Inputs", " are not readable with the current descriptors", std::move(invalidInputs));
            return GuardOutcome::Blocked;
        }
    }

    return GuardOutcome::Validated;
}

GuardOutcome runInputGuard(StateContext& ctx)
{
    if (!ctx.guard.checked)
    {
        ctx.guard.checked = true;
        ctx.guard.outcome = computeInputGuard(ctx);
    }
    return ctx.guard.outcome;
}

/// Route whatever the guard concluded, for the classes that live downstream of it. Only ever called
/// with a non-Validated outcome.
Transition handOffFromGuard(StateContext& ctx, GuardOutcome outcome)
{
    switch (outcome)
    {
        case GuardOutcome::NotValid:
            return Transition::handOff(StateId::Error);
        case GuardOutcome::NotActive:
            return Transition::handOff(StateId::Inactive);
        case GuardOutcome::Blocked:
            // The substate is already known, so this is final even when it belongs to another class
            return Transition::settled(std::move(ctx.guard.blocker));
        case GuardOutcome::Validated:
            break;
    }
    return Transition::handOff(StateId::WaitingForValidInputs);
}

/**
 * @brief 9. Data-loss deadlines. In-band: an input's buffered pre-loss data stays readable (the
 * producer went silent AFTER producing it), so the loss only becomes the reader state once the
 * affected input can no longer contribute. Recovery is per input on its next packet, after which
 * synchronization is re-established.
 *
 * Shared by ReadyState and SynchronizingState: it outranks both the synchronized fast path and the
 * alignment rungs.
 */
std::optional<StateOutcome> checkDataLoss(StateContext& ctx)
{
    const auto lost = ctx.dataLossMonitor.lostSlots();
    if (lost.empty())
        return std::nullopt;

    // "Can no longer contribute" is < one aligned block, not empty: block-aligned
    // reads floor to whole blocks, so a residual sub-block (possible whenever an
    // input's divider != blockLcm) is unreadable and, with the producer dead, no
    // event will ever end its segment to let it drain. Gating on == 0 would stall
    // the reader in Synchronized forever, never surfacing the loss.
    const SizeT block = ctx.syncManager.hasModel() ? ctx.syncManager.getModel().blockLcm : 1;
    std::vector<SizeT> drainedLost;
    for (const auto index : lost)
    {
        if (ctx.slots[index]->getQueueReader().getAvailableSamples() < block)
            drainedLost.push_back(index);
    }
    if (drainedLost.empty())
    {
        // Lost but a full block still buffered: keep reading - the loss surfaces once the
        // input can no longer fill a block
        return std::nullopt;
    }

    return outcomeWithAffected(ReaderState::DataLost, "Inputs", " missed their packet deadline", std::move(drainedLost));
}

}  // namespace

// --- The state classes ------------------------------------------------------------------------

namespace
{

/**
 * @brief Error. Terminal and latched: nothing clears the invalid flag, so this class has no exit and
 * outranks everything including inactivity.
 */
class ErrorState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Error;
    }

    Transition checkTransitionCriteria(StateContext& ctx) const override
    {
        return Transition::settled(ReaderState::Error, ctx.currentMessage.empty() ? "Reader is invalid" : ctx.currentMessage);
    }
};

/**
 * @brief Deactivated via setActive(false). Data flow is suspended, but descriptor and gap events are
 * enqueued regardless of the active flag and must still surface through reads and through the
 * dataAvailable callback - which is the whole of this class's vocabulary: {Inactive, EventPending}.
 *
 * It has no validity rung, so a descriptor that makes an input unreadable is applied silently here
 * and only becomes Incompatible on reactivation. Nor does it discard leftover segments, so a buried
 * event stays buried while inactive. Both are the pre-existing behaviour of the inactive arm, pinned
 * by MultiReaderStateTest.InactiveArmHasItsOwnReducedVocabulary.
 */
class InactiveState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Inactive;
    }

    Transition checkTransitionCriteria(StateContext& ctx) const override
    {
        // The same first two steps as the guard: Error outranks inactivity, and unused inputs are
        // drained whatever the active flag says
        if (ctx.invalid)
            return Transition::handOff(StateId::Error);

        drainUnusedSlotsOnce(ctx);

        if (ctx.isActive)
            return Transition::handOff(StateId::WaitingForValidInputs);

        // Inactive readers are not monitored for data loss
        for (SizeT i = 0; i < ctx.slots.size(); ++i)
            ctx.dataLossMonitor.setMonitored(i, false);

        // Terminology: ACTIVE/INACTIVE is the reader/port level switch (setActive) -
        // "pause the whole reader". USED/UNUSED is per-input participation (setInputUsed) -
        // "exclude this input from reading". The unused mechanism reuses port deactivation
        // internally because that is what stops data while preserving events.
        std::vector<SizeT> inactiveEventInputs;
        std::vector<SizeT> inactiveSlotIndices;
        const auto inactiveReaders = ctx.collectUsedReaders(inactiveSlotIndices);
        for (SizeT position = 0; position < inactiveReaders.size(); ++position)
        {
            const auto slotIndex = inactiveSlotIndices[position];
            // Clear-then-drain (see the guard's connection rung): clear before adopting so a
            // concurrent lock-free arrival re-arms the flag instead of being stranded.
            ctx.slots[slotIndex]->clearPacketPending();
            ctx.slots[slotIndex]->adoptQueuedPackets();
            if (!ctx.slots[slotIndex]->isConnected())
            {
                setSlotEvent(*ctx.slots[slotIndex], false);
                continue;
            }

            const bool hasEvents = inactiveReaders[position]->hasPendingEvents();
            setSlotEvent(*ctx.slots[slotIndex], hasEvents);
            if (hasEvents)
                inactiveEventInputs.push_back(slotIndex);
        }

        if (!inactiveEventInputs.empty())
        {
            return Transition::settled(
                outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(inactiveEventInputs)));
        }
        return Transition::settled(ReaderState::Inactive);
    }
};

/**
 * @brief No valid cross-input model yet, and the blocker is per input: a connection, a descriptor, a
 * compatible descriptor, or an unconsumed event. This class owns the shared input guard - every
 * class downstream of it asks the same question before trusting anything of its own.
 */
class WaitingForValidInputsState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::WaitingForValidInputs;
    }

    Transition checkTransitionCriteria(StateContext& ctx) const override
    {
        const auto outcome = runInputGuard(ctx);
        if (outcome != GuardOutcome::Validated)
            return handOffFromGuard(ctx, outcome);

        // The inputs are readable; whether they can be aligned is not this class's question
        return Transition::handOff(StateId::Synchronizing);
    }
};

/**
 * @brief The model is valid but there is no common start yet: alignment is being attempted, waiting
 * for data, or has failed recoverably.
 */
class SynchronizingState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Synchronizing;
    }

    Transition checkTransitionCriteria(StateContext& ctx) const override
    {
        const auto guardOutcome = runInputGuard(ctx);
        if (guardOutcome != GuardOutcome::Validated)
            return handOffFromGuard(ctx, guardOutcome);

        if (auto lost = checkDataLoss(ctx); lost.has_value())
            return Transition::settled(std::move(*lost));

        // Already synchronized: nothing further to establish
        if (ctx.syncManager.getCommonStart() != nullptr)
            return Transition::settled(ReaderState::Synchronized);

        const auto& usedReaders = ctx.guard.usedReaders;
        const auto& slotIndices = ctx.guard.slotIndices;

        // 8. Cross-input compatibility and the common model
        auto setup = ctx.syncManager.buildCommonModel(usedReaders, slotIndices, ctx.guard.mainPosition);
        if (!setup.ok())
        {
            ctx.readCoordinator.invalidate();
            if (ctx.exposeBuriedEvents(setup.affectedInputs))
            {
                for (const auto index : setup.affectedInputs)
                    setSlotEvent(*ctx.slots[index], ctx.slots[index]->getQueueReader().hasPendingEvents());
                return Transition::settled(
                    outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(setup.affectedInputs)));
            }
            return Transition::settled(ReaderState::Incompatible, std::move(setup.message), std::move(setup.affectedInputs));
        }

        // 10. Data on every input
        {
            std::vector<SizeT> empty;
            for (SizeT position = 0; position < usedReaders.size(); ++position)
            {
                if (usedReaders[position]->getAvailableSamples() == 0)
                    empty.push_back(slotIndices[position]);
            }
            if (!empty.empty())
                return Transition::settled(outcomeWithAffected(ReaderState::WaitingForData, "Waiting for data on inputs", "", std::move(empty)));
        }

        // 11. Alignment
        auto result = ctx.syncManager.synchronize(usedReaders, slotIndices);
        switch (result.outcome)
        {
            case SyncOutcome::Synchronized:
                // 12. Configure the read pipelines
                ctx.readCoordinator.configure(usedReaders, ctx.syncManager.getModel());
                ctx.nextReadTick = ctx.readOffset();
                return Transition::settled(ReaderState::Synchronized);
            case SyncOutcome::NeedMoreData:
                return Transition::settled(ReaderState::Synchronizing, std::move(result.message), std::move(result.affectedInputs));
            case SyncOutcome::EventPending:
            {
                for (const auto index : result.affectedInputs)
                    setSlotEvent(*ctx.slots[index], true);
                return Transition::settled(ReaderState::EventPending, std::move(result.message), std::move(result.affectedInputs));
            }
            case SyncOutcome::Failed:
                // Synchronization failure no longer deactivates the reader.
                // Unlike the Incompatible paths, we do NOT drop buffered data to surface a
                // buried event here: on a sync failure each input's data is individually valid
                // and readable (only the cross-input alignment failed), so the consumer's
                // remedy is to exclude an input or pick a main input - not to lose that input's
                // samples. A queued corrective descriptor surfaces the normal way once the
                // offending input is excluded and re-enabled (dropForInactive on re-enable).
                return Transition::settled(ReaderState::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs));
        }

        // Every SyncOutcome returns above; this is only here because the switch cannot prove it
        return Transition::settled(ReaderState::SynchronizationFailed, "Unhandled synchronization outcome");
    }
};

/**
 * @brief A common start is assigned and the model is valid: aligned blocks are readable.
 *
 * The class invariant (commonStart != nullptr) is what makes the fast path sound - the model is not
 * rebuilt here - and it is also the producer gate's `steady` predicate. Everything that can break the
 * invariant is either a guard condition or a data-loss deadline, both checked above the fast path.
 */
class ReadyState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Ready;
    }

    Transition checkTransitionCriteria(StateContext& ctx) const override
    {
        const auto guardOutcome = runInputGuard(ctx);
        if (guardOutcome != GuardOutcome::Validated)
            return handOffFromGuard(ctx, guardOutcome);

        if (auto lost = checkDataLoss(ctx); lost.has_value())
            return Transition::settled(std::move(*lost));

        if (ctx.syncManager.getCommonStart() != nullptr)
            return Transition::settled(ReaderState::Synchronized);

        // The start is gone (an event was returned, a descriptor changed, the model was rebuilt):
        // re-establishing it is SynchronizingState's job, not a condition to guess at here
        return Transition::handOff(StateId::Synchronizing);
    }
};

const ErrorState errorState;
const InactiveState inactiveState;
const WaitingForValidInputsState waitingForValidInputsState;
const SynchronizingState synchronizingState;
const ReadyState readyState;

}  // namespace

const MultiReaderState& stateFor(StateId id)
{
    switch (id)
    {
        case StateId::Error:
            return errorState;
        case StateId::Inactive:
            return inactiveState;
        case StateId::WaitingForValidInputs:
            return waitingForValidInputsState;
        case StateId::Synchronizing:
            return synchronizingState;
        case StateId::Ready:
            return readyState;
    }
    return waitingForValidInputsState;
}

void runStateEvaluation(StateContext& ctx)
{
    // Where the machine starts is derived, not remembered: the class a substate belongs to is a
    // property of the substate (stateClassOf)
    const MultiReaderState* current = &stateFor(stateClassOf(ctx.currentState, ctx.isActive));

    // A hand-off only ever runs downstream, so the longest chain is Inactive -> WaitingForValidInputs
    // -> Synchronizing. The bound mirrors synchronize()'s retry bound and exists to turn an invariant
    // break into a diagnosable state rather than a hang.
    constexpr int maxTransitions = 8;
    for (int i = 0; i < maxTransitions; ++i)
    {
        auto transition = current->checkTransitionCriteria(ctx);
        if (!transition.isHandOff())
        {
            applyStateInvariants(ctx, transition.outcome.state);
            ctx.outcome = std::move(transition.outcome);
            return;
        }
        current = &stateFor(*transition.target);
    }

    ctx.outcome = {ReaderState::Error, fmt::format("State evaluation did not settle in {}", stateClassName(current->id())), {}};
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
