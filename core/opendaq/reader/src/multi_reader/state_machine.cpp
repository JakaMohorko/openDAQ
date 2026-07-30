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

#include <algorithm>
#include <limits>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

const char* stateName(StateId id)
{
    switch (id)
    {
        case StateId::Error:
            return "Error";
        case StateId::Inactive:
            return "Inactive";
        case StateId::Establishing:
            return "Establishing";
        case StateId::InputsFailed:
            return "InputsFailed";
        case StateId::Ready:
            return "Ready";
    }
    return "<unknown>";
}

StateId stateIdFor(ReaderState substate, bool isActive)
{
    switch (substate)
    {
        case ReaderState::Error:
            return StateId::Error;
        case ReaderState::Inactive:
            return StateId::Inactive;
        case ReaderState::EventPending:
            // Holding events is a condition, not a behaviour: reporting it always clears the
            // synchronization, so what is left is re-establishing - unless the reader is deactivated,
            // where surfacing events is exactly what the inactive behaviour does.
            return isActive ? StateId::Establishing : StateId::Inactive;
        case ReaderState::WaitingForConnections:
        case ReaderState::WaitingForDescriptors:
        case ReaderState::WaitingForData:
        case ReaderState::Synchronizing:
            return StateId::Establishing;
        case ReaderState::Incompatible:
        case ReaderState::SynchronizationFailed:
        case ReaderState::DataLost:
            return StateId::InputsFailed;
        case ReaderState::Synchronized:
            return StateId::Ready;
    }
    return StateId::Establishing;
}

// --- The gate policies --------------------------------------------------------------------------

namespace
{

/// Error keeps whatever message explains why the reader became invalid.
StateOutcome errorOutcome(StateContext& ctx)
{
    return {ReaderState::Error, ctx.currentMessage.empty() ? "Reader is invalid" : ctx.currentMessage, {}};
}

/// The inactive arm, defined with the other rungs below.
StateOutcome deriveWhileInactive(StateContext& ctx);

struct SlotGateBasis
{
    SizeT divider = 1;
    SizeT untilEventCommon = 0;
    bool hasEventPackets = false;
};

/**
 * @brief The part of the gate policy that is the same in every state, and the slot's published basis.
 *
 * An unconnected slot contributes nothing at all. An unused slot contributes only its event flag -
 * data is dropped at its inactive port, so a stale ready flag would let `ready >= used` open the gate
 * on data no read will ever touch.
 *
 * @return false when the slot is fully handled here and the state's own policy does not apply.
 */
bool publishSlotGateBasis(Input& slot, SlotGateBasis& basis)
{
    if (!slot.isConnected())
    {
        slot.publishGateBasis(0, false);
        slot.setReadyThresholdNative(Input::NeverReady);
        setSlotReady(slot, false);
        setSlotEvent(slot, false);
        return false;
    }

    auto& reader = slot.getQueueReader();
    basis.divider = reader.getSampleRateDivider() > 0 ? reader.getSampleRateDivider() : 1;
    basis.hasEventPackets = reader.hasPendingEvents() || reader.hasQueuedEventPackets();
    basis.untilEventCommon = reader.getAvailableSamplesUntilEvent();
    slot.publishGateBasis(basis.untilEventCommon / basis.divider, basis.hasEventPackets);

    if (!slot.isUsed())
    {
        slot.setReadyThresholdNative(Input::NeverReady);
        setSlotReady(slot, false);
        return false;
    }
    return true;
}

/// An unused input's port is inactive, so only events can arrive on it; adopting them is what makes
/// them visible in the per-input states and lets them open the gate (the recovery signal a consumer
/// answers with setInputUsed(id, true)).
void adoptUnusedSlotEvents(Input& slot)
{
    slot.adoptQueuedPackets();
    if (!slot.isConnected())
        return;

    auto& reader = slot.getQueueReader();
    reader.drain();
    publishSlotBasis(slot);
    setSlotEvent(slot, reader.hasPendingEvents());
}

/**
 * @brief The gate policy of every state but Ready: every packet forces an evaluation
 * (wakeOnAnyPacket), and the first sample marks a slot ready so the gate wakes the consumer as the
 * last input starts delivering. Event flags stay exactly as the state derivation left them.
 *
 * Forcing an evaluation per packet is what preserves the liveness of the establishment, failure and
 * recovery paths: a DataLost slot's reviving packet, or a Synchronizing slot's alignment progress,
 * must never wait on a gate whose threshold cannot be met yet.
 */
void publishGateWhileEstablishing(StateContext& ctx)
{
    for (auto* slot : ctx.slots)
    {
        slot->setWakeOnAnyPacket(true);

        SlotGateBasis basis;
        if (!publishSlotGateBasis(*slot, basis))
            continue;

        slot->setReadyThresholdNative(1);
        setSlotReady(*slot, slot->getQueueReader().getAvailableSamples() > 0);
    }
}

}  // namespace

// --- The base (conservative) policies -----------------------------------------------------------

void MultiReaderState::publishProducerGate(StateContext& ctx) const
{
    publishGateWhileEstablishing(ctx);
}

bool MultiReaderState::refreshDataPlane(StateContext& /*ctx*/, bool /*escalateOnEvent*/) const
{
    // Progress toward being readable is exactly what these states are doing, and only the full
    // derivation can decide whether any has been made.
    return true;
}

bool MultiReaderState::updateCallbackState(StateContext& /*ctx*/) const
{
    return true;
}

SizeT MultiReaderState::availableCount(StateContext& /*ctx*/) const
{
    return 0;
}

ReadAction MultiReaderState::planRead(StateContext& ctx) const
{
    // Events outrank everything: they have to be consumed before data can be read again. Whether any
    // are returnable is the derivation's call, not a queue query - while a used input has no signal,
    // for instance, the events queued on the others are deliberately not returnable.
    if (ctx.substate == ReaderState::EventPending)
        return ReadAction::ReturnEvents;
    return ReadAction::ReportState;
}

bool MultiReaderState::readWaitSatisfied(StateContext& ctx, SizeT /*requested*/) const
{
    // Nothing but events can arrive for a state that cannot serve data; everything else it could wait
    // for changes the state, and the waiter re-derives on every wake.
    return planRead(ctx) == ReadAction::ReturnEvents;
}

StateOutcome MultiReaderState::reassess(StateContext& ctx) const
{
    return deriveState(ctx);
}

StateOutcome MultiReaderState::slotConnected(StateContext& ctx, SizeT /*slot*/) const
{
    // A signal appearing on a slot can move every cross-input conclusion - rates, resolutions, the
    // divider vector, the aligned block - so the model goes and the state is derived from scratch.
    ctx.invalidateModel();
    return deriveState(ctx);
}

StateOutcome MultiReaderState::slotDisconnected(StateContext& ctx, SizeT slot) const
{
    // Disarm immediately: the derivation can return before its monitoring refresh while another input
    // is unconnected, and a stale arrival must not count toward a deadline after a reconnect.
    ctx.dataLossMonitor.setMonitored(slot, false);
    ctx.invalidateModel();
    return deriveState(ctx);
}

StateOutcome MultiReaderState::activeChanged(StateContext& ctx) const
{
    // The facade has already switched the ports and, on deactivation, dropped the queued data; what
    // the reader now IS follows from ground truth.
    return deriveState(ctx);
}

StateOutcome MultiReaderState::inputSetChanged(StateContext& ctx) const
{
    // The model's divider vector is parallel to the used-input vector, so any change to that set - or
    // to which input defines the output grid - invalidates it.
    ctx.invalidateModel();
    return deriveState(ctx);
}

// --- The behaviours ----------------------------------------------------------------------------

namespace
{

class ErrorState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Error;
    }

    StateOutcome reassess(StateContext& ctx) const override
    {
        // Nothing clears the invalid flag, so there are no exit conditions to check.
        return errorOutcome(ctx);
    }
};

class InactiveState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Inactive;
    }

    StateOutcome reassess(StateContext& ctx) const override
    {
        if (ctx.invalid)
            return errorOutcome(ctx);

        // Reactivation is a full derivation: nothing below the active flag has been looked at while
        // the reader was paused
        if (ctx.isActive)
            return deriveState(ctx);

        ctx.drainUnusedSlots();
        return deriveWhileInactive(ctx);
    }
};

class EstablishingState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Establishing;
    }
};

class InputsFailedState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::InputsFailed;
    }
};

class ReadyState final : public MultiReaderState
{
public:
    StateId id() const override
    {
        return StateId::Ready;
    }

    /// The only state where producers gate their own scheduling: the reader is synchronized, so the
    /// gate's ready threshold is the smallest servable aligned request and an open gate provably means
    /// a read can return samples.
    void publishProducerGate(StateContext& ctx) const override
    {
        if (!ctx.syncManager.hasModel())
        {
            // Ready without a model is an invariant break; behave as if establishing rather than
            // publishing a threshold derived from a model that is not there
            publishGateWhileEstablishing(ctx);
            return;
        }

        const SizeT gateMinimum = ReadCoordinator::effectiveMinimum(ctx.syncManager.getModel(), ctx.minReadCount);
        for (auto* slot : ctx.slots)
        {
            slot->setWakeOnAnyPacket(false);

            SlotGateBasis basis;
            if (!publishSlotGateBasis(*slot, basis))
                continue;

            // Ground truth: ready with the smallest servable request buffered before the next event,
            // event buried-inclusive (a sub-block residual in front of a buried event must still open
            // the gate so a read can surface it).
            slot->setReadyThresholdNative(gateMinimum / basis.divider);
            setSlotReady(*slot, basis.untilEventCommon >= gateMinimum);
            setSlotEvent(*slot, basis.hasEventPackets);
        }
    }

    /**
     * @brief The synchronized fast path: no state derivation until an event is encountered or a
     * deadline expires, because data packets only ever add availability.
     */
    bool refreshDataPlane(StateContext& ctx, bool escalateOnEvent) const override
    {
        auto& plane = ctx.dataPlane;

        // Nothing to do if nothing changed since the last pass: no packet arrived and no read
        // consumed, and no deadline is pending. A buried event can only surface through an arrival or
        // a consumption, so there is nothing to re-check - this collapses the repeated refreshes of a
        // poll-then-read loop to one real pass. Any cached availability stays valid across this early
        // return: with nothing arrived and nothing consumed the counts cannot have moved.
        const bool arrived = plane.dirty.exchange(false, std::memory_order_acquire);
        if (!arrived && !plane.consumed && !ctx.dataLossMonitor.hasLostSlots())
            return false;
        plane.consumed = false;

        // This pass recomputes each used input's availability, so the read path can plan and lower
        // readiness from the cached counts instead of walking every input again in createPlan and in
        // the post-commit readiness update. The slot count is fixed after construction; size lazily.
        if (plane.slotAvailable.size() != ctx.slots.size())
            plane.slotAvailable.assign(ctx.slots.size(), 0);

        const bool haveModel = ctx.syncManager.hasModel();
        // The gate's ready threshold is the smallest servable aligned request - the same minimum the
        // availability alignment and the leftover-segment discard enforce - so an open gate always
        // means a read can actually return samples.
        const SizeT gateMinimum = haveModel ? ReadCoordinator::effectiveMinimum(ctx.syncManager.getModel(), ctx.minReadCount) : 0;

        bool escalate = false;
        bool anyEvent = false;
        SizeT availableCommon = std::numeric_limits<SizeT>::max();
        for (SizeT i = 0; i < ctx.slots.size(); ++i)
        {
            auto* slot = ctx.slots[i];
            const bool packetsArrived = slot->clearPacketPending();

            if (!slot->isUsed())
            {
                if (packetsArrived)
                    adoptUnusedSlotEvents(*slot);
                continue;
            }

            auto& reader = slot->getQueueReader();

            // The drain (the expensive step - adopts queued packets) is gated on arrival: a slot with
            // no new packet has nothing new to adopt.
            if (packetsArrived)
                reader.drain();

            // The event check runs EVERY cycle, not only on arrival: a leading event must be reported,
            // and a buried event needs the full derivation so the partial segment in front of it can
            // be discarded and the event can surface. A buried event becomes reachable through the
            // reader's own consumption (a read advancing the frontier past the data ahead of it) with
            // no new packet arriving, so gating this on packetsArrived would miss it. Both queries are
            // O(1) (empty-check / sticky adoption flag), so per-cycle is cheap.
            const bool hasEvent = reader.hasPendingEvents() || reader.hasQueuedEventPackets();
            publishSlotBasis(*slot);
            if (hasEvent)
            {
                // The read path escalates so the derivation transitions to EventPending and discards
                // residuals; the query path only records the event for the gate - buried-inclusive, so
                // a sub-block residual before a buried event still fires the callback - and re-arms
                // dirty (below) so the next read runs the derivation.
                if (escalateOnEvent)
                    escalate = true;
                else
                {
                    setSlotEvent(*slot, true);
                    anyEvent = true;
                }
                continue;
            }

            // The read path leaves event bits to the derivation; the query path owns them here, so
            // clear a stale bit once the slot's events have drained away.
            if (!escalateOnEvent)
                setSlotEvent(*slot, false);

            // Availability is O(1) here (the queue reader maintains it incrementally across drains and
            // reads), so recomputing it for every used slot each real pass is cheap - and it is exactly
            // the count createPlan needs. Readiness is derived from the same value.
            if (haveModel)
            {
                const SizeT avail = reader.getAvailableSamplesUntilEvent();
                plane.slotAvailable[i] = avail;
                availableCommon = std::min(availableCommon, avail);
                setSlotReady(*slot, avail >= gateMinimum);
            }
        }

        // Deadlines are maintained by the monitor's waiter thread; an unresolved loss must re-enter
        // the full derivation (it decides when the loss becomes visible)
        if (escalate || ctx.dataLossMonitor.hasLostSlots())
        {
            // The full derivation can drop partial segments or change state, so the availability
            // gathered above is not authoritative; it clears the cache and consumers fall back to a
            // direct walk.
            return true;
        }

        if (anyEvent)
        {
            // The query path recorded an event for the gate but did not derive the state. Re-arm dirty
            // so the next read does a full pass (escalateOnEvent) that surfaces it, and do not publish
            // the partial availability gathered above.
            plane.dirty.store(true, std::memory_order_release);
            plane.availableValid = false;
            return false;
        }

        // Publish the availability this pass computed. The sentinel survives only when no input is
        // used, which maps to nothing available.
        plane.availableCommon = availableCommon == std::numeric_limits<SizeT>::max() ? 0 : availableCommon;
        plane.availableValid = haveModel;
        return false;
    }

    bool updateCallbackState(StateContext& ctx) const override
    {
        // A crossed deadline needs the full derivation: it decides when the loss becomes visible.
        if (ctx.dataLossMonitor.hasLostSlots())
            return true;

        const bool haveModel = ctx.syncManager.hasModel();
        const SizeT gateMinimum = haveModel ? ReadCoordinator::effectiveMinimum(ctx.syncManager.getModel(), ctx.minReadCount) : 0;

        for (auto* slot : ctx.slots)
        {
            const bool used = slot->isUsed();
            auto& gateFlags = slot->gateFlags();

            // A slot that already satisfies the callback gate cannot stop satisfying it until a read
            // consumes it (the read path lowers the flag then), so this pass never needs to re-touch
            // it. Readiness only participates in the gate for used inputs; for an unused input only its
            // event participates (the recovery signal), so a stale ready flag must not skip it.
            // Skipping also leaves packetPending set, so the read path still adopts data queued behind
            // the slot.
            if (gateFlags.event() || (used && gateFlags.ready()))
                continue;

            // Nothing new here: a slot that does not already satisfy the gate and received no packet
            // cannot have risen to either.
            if (!slot->clearPacketPending())
                continue;

            if (!used)
            {
                adoptUnusedSlotEvents(*slot);
                continue;
            }

            auto& reader = slot->getQueueReader();
            reader.drain();
            publishSlotBasis(*slot);

            // Buried-inclusive: a sub-block residual before a buried event still fires the callback so
            // the consumer reads and the read path surfaces the event.
            const bool hasEvent = reader.hasPendingEvents() || reader.hasQueuedEventPackets();
            setSlotEvent(*slot, hasEvent);
            if (!hasEvent && haveModel)
                setSlotReady(*slot, reader.getAvailableSamplesUntilEvent() >= gateMinimum);
        }
        return false;
    }

    SizeT availableCount(StateContext& ctx) const override
    {
        // A leading pending event on any used input blocks a synchronized data read until it is
        // handled, and getAvailableSamplesUntilEvent cannot see it (it lives in a separate queue), so
        // report nothing available. This is Ready holding an event from within, rather than the event
        // being a state of its own. Buried events need no guard - the count stops at them.
        for (auto* slot : ctx.slots)
        {
            if (slot->isUsed() && slot->getQueueReader().hasPendingEvents())
                return 0;
        }

        // The refresh above published availability on the fast path; reuse it rather than walking every
        // input again. Fall back to a direct count only when it is not valid.
        if (ctx.dataPlane.availableValid)
            return ReadCoordinator::alignAvailable(ctx.dataPlane.availableCommon, ctx.syncManager.getModel(), ctx.minReadCount);

        collectUsedReaders(ctx.slots, ctx.dataPlane.scratchReaders, ctx.dataPlane.scratchSlotIndices);
        return ctx.readCoordinator.getAvailableCount(ctx.dataPlane.scratchReaders, ctx.syncManager.getModel(), ctx.minReadCount);
    }

    ReadAction planRead(StateContext& ctx) const override
    {
        // Ready cannot be holding a returnable event: reporting one clears the synchronization, which
        // is what takes the reader out of this state (stateIdFor). A buried event is reached through
        // refreshDataPlane, which escalates for exactly that reason.
        return ctx.substate == ReaderState::EventPending ? ReadAction::ReturnEvents : ReadAction::ServeData;
    }

    bool readWaitSatisfied(StateContext& ctx, SizeT requested) const override
    {
        if (planRead(ctx) == ReadAction::ReturnEvents)
            return true;
        if (requested == 0)
            return false;

        // ReadTimeoutType::All: the whole aligned request has to be servable, so a partial block is
        // not enough to wake the read.
        const SizeT block = ctx.syncManager.getModel().blockLcm;
        const SizeT alignedRequest = block > 0 ? requested / block * block : 0;
        return alignedRequest > 0 && availableCount(ctx) >= alignedRequest;
    }

    /**
     * @brief Ready's exit conditions, in the derivation's precedence order: deactivation, a used input
     * losing its signal, the synchronization going away, a leading event, a crossed deadline. Anything
     * else that could end this state arrives as a notification that forces a full derivation of its
     * own, so it is not re-checked here.
     *
     * What this skips relative to the full derivation is what cannot change while the reader stays
     * synchronized: the used set and the main input (only a configuration call moves them, and it
     * derives), the monitoring refresh (setMonitored is a no-op when unchanged), and the descriptor and
     * validity rungs (a slot's descriptors and issues only change when an event is consumed or its
     * connection changes - both of which leave this state).
     */
    StateOutcome reassess(StateContext& ctx) const override
    {
        if (ctx.invalid)
            return errorOutcome(ctx);
        if (!ctx.isActive)
            return deriveState(ctx);

        ctx.drainUnusedSlots();

        // The used readers, without an allocation: this is the steady-state path
        auto& used = ctx.dataPlane.scratchReaders;
        collectUsedReaders(ctx.slots, used, ctx.dataPlane.scratchSlotIndices);
        if (used.empty())
            return deriveState(ctx);

        for (const auto index : ctx.dataPlane.scratchSlotIndices)
        {
            // Clear-then-drain, for the same reason the derivation does it in that order: the producer
            // path is lock-free, so a packet enqueued after the clear re-arms the flag instead of being
            // stranded on the connection.
            ctx.slots[index]->clearPacketPending();
            ctx.slots[index]->adoptQueuedPackets();
            if (!ctx.slots[index]->isConnected())
                return deriveState(ctx);
        }

        if (ctx.syncManager.getCommonStart() == nullptr || !ctx.syncManager.hasModel())
            return deriveState(ctx);

        // Partial blocks in front of an event are silently discarded so the event can surface - which
        // is what makes the leading-event check below able to see a buried one
        ctx.readCoordinator.discardLeftoverSegments(used, ctx.syncManager.getModel(), ctx.minReadCount);

        for (auto* reader : used)
        {
            if (reader->hasPendingEvents())
                return deriveState(ctx);
        }

        // Whether a crossed deadline actually ends this state is the derivation's rule (less than one
        // aligned block buffered, not empty), so hand over rather than re-implement it
        if (ctx.dataLossMonitor.hasLostSlots())
            return deriveState(ctx);

        return {ReaderState::Synchronized, {}, {}};
    }

    /**
     * @brief A connect on an UNUSED slot leaves a synchronized reader alone.
     *
     * The cross-input model is built from the used inputs, so a signal appearing on an input that is
     * excluded from reading cannot change it. The base reaction would still throw the model away and
     * make the next derivation rebuild it, re-run synchronize() and rebuild the status cache - over a
     * signal the reader is not reading.
     *
     * The outcome is the same either way, which is why no golden trace moves: an unchanged used set and
     * unchanged descriptors rebuild an identical model, and re-synchronizing lands back on the current
     * frontier because a block-aligned commit leaves every used input's first sample there. So this is
     * work avoided, not a behaviour restored. The slot's new descriptor event surfaces regardless: it
     * is adopted as an unused slot's event and reported as that input's per-input state, which is the
     * recovery signal for setInputUsed(id, true).
     */
    StateOutcome slotConnected(StateContext& ctx, SizeT slot) const override
    {
        if (slot < ctx.slots.size() && !ctx.slots[slot]->isUsed())
            return reassess(ctx);
        return MultiReaderState::slotConnected(ctx, slot);
    }
};

const ErrorState errorState;
const InactiveState inactiveState;
const EstablishingState establishingState;
const InputsFailedState inputsFailedState;
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
        case StateId::Establishing:
            return establishingState;
        case StateId::InputsFailed:
            return inputsFailedState;
        case StateId::Ready:
            return readyState;
    }
    return establishingState;
}

// --- The exhaustive derivation ------------------------------------------------------------------

namespace
{

/// The used inputs the rungs below the connection check work on.
struct InputSet
{
    std::vector<QueueReader*> readers;
    std::vector<SizeT> slotIndices;
    SizeT mainPosition = 0;
};

/**
 * @brief The effects that follow from the substate itself rather than from the rung that decided it.
 *
 * A returnable event and a missed deadline both mean the aligned stream is finished: the event has to
 * be consumed before data can be read again, and an input that went silent cannot contribute to the
 * block being assembled. Neither can leave a synchronized start behind, on any path.
 *
 * Level-triggered and idempotent, like the rest: applied whenever the derivation settles on the
 * substate, not only on the transition into it.
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

/// Inactivity suspends data flow only (declared above): descriptor and gap events are enqueued regardless of the
/// active flag and must still surface through reads and through the dataAvailable callback. This arm
/// has no validity rung and does not discard leftover segments, so its whole vocabulary is
/// {Inactive, EventPending}.
StateOutcome deriveWhileInactive(StateContext& ctx)
{
    // Inactive readers are not monitored for data loss
    for (SizeT i = 0; i < ctx.slots.size(); ++i)
        ctx.dataLossMonitor.setMonitored(i, false);

    std::vector<SizeT> eventInputs;
    std::vector<SizeT> slotIndices;
    const auto readers = ctx.collectUsedReaders(slotIndices);
    for (SizeT position = 0; position < readers.size(); ++position)
    {
        const auto slotIndex = slotIndices[position];
        // Clear-then-drain (see the connection rung): clear before adopting so a concurrent lock-free
        // arrival re-arms the flag instead of being stranded.
        ctx.slots[slotIndex]->clearPacketPending();
        ctx.slots[slotIndex]->adoptQueuedPackets();
        if (!ctx.slots[slotIndex]->isConnected())
        {
            setSlotEvent(*ctx.slots[slotIndex], false);
            continue;
        }

        const bool hasEvents = readers[position]->hasPendingEvents();
        setSlotEvent(*ctx.slots[slotIndex], hasEvents);
        if (hasEvents)
            eventInputs.push_back(slotIndex);
    }

    if (!eventInputs.empty())
        return outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(eventInputs));
    return {ReaderState::Inactive, {}, {}};
}

/**
 * @brief Everything that has to hold before any alignment question is worth asking: the used set and
 * the main input, connections, the data-loss monitoring refresh and the leftover-segment discard,
 * leading events, descriptors, and per-input validity - in that precedence order.
 *
 * @return the substate to report when something blocks reading; nothing when @p inputs is filled and
 * the alignment rungs apply.
 */
std::optional<StateOutcome> deriveInputBlocker(StateContext& ctx, InputSet& inputs)
{
    inputs.readers = ctx.collectUsedReaders(inputs.slotIndices);
    const auto& readers = inputs.readers;
    const auto& slotIndices = inputs.slotIndices;

    if (readers.empty())
    {
        ctx.invalidateSynchronization();
        return StateOutcome{ReaderState::WaitingForConnections, "No used inputs", {}};
    }

    // The explicitly selected main input is never silently replaced
    if (ctx.mainInputId.assigned())
    {
        const auto mainSlot = ctx.mainSlotIndex();
        const auto position = std::find(slotIndices.begin(), slotIndices.end(), mainSlot);
        if (mainSlot == slotNotFound || position == slotIndices.end())
        {
            ctx.invalidateModel();
            return StateOutcome{ReaderState::WaitingForConnections,
                                "The selected main input is not among the used inputs",
                                mainSlot == slotNotFound ? std::vector<SizeT>{} : std::vector<SizeT>{mainSlot}};
        }
        inputs.mainPosition = static_cast<SizeT>(position - slotIndices.begin());
    }

    // Adopt what the producers enqueued. Connectivity itself is NOT polled here: every
    // connect/disconnect/reconnect reaches the slot as a port callback, and
    // Input::replayMissedPortCallbacks replays the two callbacks the port skips for a port that was
    // already connected when the listener was installed. So isConnected() is authoritative; only the
    // queue contents need collecting, because the lock-free producer path cannot hand them over.
    {
        std::vector<SizeT> unconnected;
        for (const auto index : slotIndices)
        {
            // Clear the arrival flag BEFORE draining (clear-then-drain). The producer path is
            // lock-free, so a packet enqueued after this clear re-arms the flag and is caught by the
            // next pass; clearing AFTER the drain would instead wipe the flag of a packet enqueued in
            // the drain->clear window without ever adopting it, stranding it on the connection (the
            // availability-undercount race).
            ctx.slots[index]->clearPacketPending();
            ctx.slots[index]->adoptQueuedPackets();
            if (!ctx.slots[index]->isConnected())
                unconnected.push_back(index);
        }
        if (!unconnected.empty())
        {
            // Connections gate events: while a used input has no signal no event is returnable, so
            // the callback must not fire on the events already queued on the connected inputs
            for (const auto index : slotIndices)
                setSlotEvent(*ctx.slots[index], false);

            ctx.invalidateModel();
            return outcomeWithAffected(ReaderState::WaitingForConnections, "Inputs", " have no signal connected", std::move(unconnected));
        }
    }

    // Data-loss monitoring covers exactly the used, connected inputs of an active reader; everything
    // else is unmonitored and disarmed
    for (SizeT i = 0; i < ctx.slots.size(); ++i)
        ctx.dataLossMonitor.setMonitored(i, ctx.slots[i]->isUsed() && ctx.slots[i]->isConnected());

    // While synchronized, partial blocks in front of an event are silently discarded so the event
    // can surface
    if (ctx.syncManager.getCommonStart() != nullptr && ctx.syncManager.hasModel())
        ctx.readCoordinator.discardLeftoverSegments(readers, ctx.syncManager.getModel(), ctx.minReadCount);

    // Leading events preempt everything below them
    {
        std::vector<SizeT> eventInputs;
        bool handshakeInFlight = false;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            // packetPending was already cleared before the drain above (clear-then-drain); clearing
            // again here would re-open the drain->clear race, so it is intentionally not cleared.
            const bool hasEvents = readers[position]->hasPendingEvents();
            if (hasEvents)
                eventInputs.push_back(slotIndices[position]);
            setSlotEvent(*ctx.slots[slotIndices[position]], hasEvents);

            // A connected input with neither descriptors nor events is still completing its connect
            // handshake: the signal's initial descriptor event has not been enqueued yet (connections
            // are constructed in steps and evaluations can run in between)
            if (!hasEvents && !readers[position]->getValueDescriptor().assigned() &&
                !readers[position]->getDomainDescriptor().assigned())
            {
                handshakeInFlight = true;
            }
        }

        // While a connect handshake is in flight the reader is not yet event-ready: the in-flight
        // input's initial descriptor event arrives momentarily and re-triggers evaluation, so both the
        // dataAvailable callback and blocked reads see every input's initial event at once. The
        // derivation falls through to the descriptor rung, which truthfully reports the handshaking
        // input as WaitingForDescriptors.
        if (handshakeInFlight)
        {
            for (const auto index : slotIndices)
                setSlotEvent(*ctx.slots[index], false);
        }
        else if (!eventInputs.empty())
        {
            // Descriptors apply when leading events are consumed, so the cross-input model can be
            // built opportunistically - accessors like getCommonSampleRate and getTickResolution work
            // right after construction, like they always have
            if (!ctx.syncManager.hasModel())
            {
                bool modelBuildable = true;
                for (auto* reader : readers)
                {
                    if (!reader->getValueDescriptor().assigned() || !reader->getDomainDescriptor().assigned() || !reader->isValid())
                        modelBuildable = false;
                }
                if (modelBuildable)
                    ctx.syncManager.buildCommonModel(readers, slotIndices, inputs.mainPosition);
            }
            return outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(eventInputs));
        }
    }

    // Descriptors
    {
        std::vector<SizeT> missing;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (!readers[position]->getValueDescriptor().assigned() || !readers[position]->getDomainDescriptor().assigned())
                missing.push_back(slotIndices[position]);
        }
        if (!missing.empty())
            return outcomeWithAffected(ReaderState::WaitingForDescriptors, "Inputs", " have no descriptors yet", std::move(missing));
    }

    // The facade refreshes its main-input descriptors from here on (deferred to the end of the
    // evaluation; see StateContext::mainDescriptorsStale)
    ctx.mainDescriptorsStale = true;

    // Per-input validity
    {
        std::vector<SizeT> invalidInputs;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (!readers[position]->isValid())
                invalidInputs.push_back(slotIndices[position]);
        }
        if (!invalidInputs.empty())
        {
            ctx.invalidateModel();
            if (ctx.exposeBuriedEvents(invalidInputs))
            {
                for (const auto index : invalidInputs)
                    setSlotEvent(*ctx.slots[index], ctx.slots[index]->getQueueReader().hasPendingEvents());
                return outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(invalidInputs));
            }
            return outcomeWithAffected(
                ReaderState::Incompatible, "Inputs", " are not readable with the current descriptors", std::move(invalidInputs));
        }
    }

    return std::nullopt;
}

/**
 * @brief Data-loss deadlines. In-band: an input's buffered pre-loss data stays readable (the producer
 * went silent AFTER producing it), so the loss only becomes the reader state once the affected input
 * can no longer contribute. Recovery is per input on its next packet.
 */
std::optional<StateOutcome> deriveDataLoss(StateContext& ctx)
{
    const auto lost = ctx.dataLossMonitor.lostSlots();
    if (lost.empty())
        return std::nullopt;

    // "Can no longer contribute" is < one aligned block, not empty: block-aligned reads floor to
    // whole blocks, so a residual sub-block (possible whenever an input's divider != blockLcm) is
    // unreadable and, with the producer dead, no event will ever end its segment to let it drain.
    // Gating on == 0 would stall the reader in Synchronized forever, never surfacing the loss.
    const SizeT block = ctx.syncManager.hasModel() ? ctx.syncManager.getModel().blockLcm : 1;
    std::vector<SizeT> drainedLost;
    for (const auto index : lost)
    {
        if (ctx.slots[index]->getQueueReader().getAvailableSamples() < block)
            drainedLost.push_back(index);
    }
    if (drainedLost.empty())
    {
        // Lost but a full block still buffered: keep reading - the loss surfaces once the input can
        // no longer fill a block
        return std::nullopt;
    }
    return outcomeWithAffected(ReaderState::DataLost, "Inputs", " missed their packet deadline", std::move(drainedLost));
}

/// The cross-input model, data on every input, and the alignment itself.
StateOutcome deriveAlignment(StateContext& ctx, const InputSet& inputs)
{
    const auto& readers = inputs.readers;
    const auto& slotIndices = inputs.slotIndices;

    auto setup = ctx.syncManager.buildCommonModel(readers, slotIndices, inputs.mainPosition);
    if (!setup.ok())
    {
        ctx.readCoordinator.invalidate();
        if (ctx.exposeBuriedEvents(setup.affectedInputs))
        {
            for (const auto index : setup.affectedInputs)
                setSlotEvent(*ctx.slots[index], ctx.slots[index]->getQueueReader().hasPendingEvents());
            return outcomeWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(setup.affectedInputs));
        }
        return {ReaderState::Incompatible, std::move(setup.message), std::move(setup.affectedInputs)};
    }

    {
        std::vector<SizeT> empty;
        for (SizeT position = 0; position < readers.size(); ++position)
        {
            if (readers[position]->getAvailableSamples() == 0)
                empty.push_back(slotIndices[position]);
        }
        if (!empty.empty())
            return outcomeWithAffected(ReaderState::WaitingForData, "Waiting for data on inputs", "", std::move(empty));
    }

    auto result = ctx.syncManager.synchronize(readers, slotIndices);
    switch (result.outcome)
    {
        case SyncOutcome::Synchronized:
            ctx.readCoordinator.configure(readers, ctx.syncManager.getModel());
            ctx.nextReadTick = ctx.readOffset();
            return {ReaderState::Synchronized, {}, {}};
        case SyncOutcome::NeedMoreData:
            return {ReaderState::Synchronizing, std::move(result.message), std::move(result.affectedInputs)};
        case SyncOutcome::EventPending:
            for (const auto index : result.affectedInputs)
                setSlotEvent(*ctx.slots[index], true);
            return {ReaderState::EventPending, std::move(result.message), std::move(result.affectedInputs)};
        case SyncOutcome::Failed:
            // Unlike the Incompatible paths, buffered data is NOT dropped to surface a buried event
            // here: on a sync failure each input's data is individually valid and readable (only the
            // cross-input alignment failed), so the consumer's remedy is to exclude an input or pick a
            // main input - not to lose that input's samples. A queued corrective descriptor surfaces
            // the normal way once the offending input is excluded and re-enabled.
            return {ReaderState::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs)};
    }
    return {ReaderState::SynchronizationFailed, "Unhandled synchronization outcome", {}};
}

}  // namespace

StateOutcome deriveState(StateContext& ctx)
{
    StateOutcome outcome = [&]() -> StateOutcome
    {
        // Error is terminal; inactivity gates everything else
        if (ctx.invalid)
            return {ReaderState::Error, ctx.currentMessage.empty() ? "Reader is invalid" : ctx.currentMessage, {}};

        ctx.drainUnusedSlots();

        if (!ctx.isActive)
            return deriveWhileInactive(ctx);

        InputSet inputs;
        if (auto blocker = deriveInputBlocker(ctx, inputs); blocker.has_value())
            return std::move(*blocker);

        if (auto lost = deriveDataLoss(ctx); lost.has_value())
            return std::move(*lost);

        // Already synchronized: nothing further to establish, and the model is deliberately not
        // rebuilt - that is what makes this the fast answer
        if (ctx.syncManager.getCommonStart() != nullptr)
            return {ReaderState::Synchronized, {}, {}};

        return deriveAlignment(ctx, inputs);
    }();

    applyStateInvariants(ctx, outcome.state);
    return outcome;
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
