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
#include <opendaq/multi_reader/state_context.h>

#include <algorithm>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

void evaluateStateLadder(StateContext& ctx)
{
    // 1. Error is terminal; inactivity gates everything else
    if (ctx.invalid)
    {
        ctx.setState(ReaderState::Error, ctx.currentMessage.empty() ? "Reader is invalid" : ctx.currentMessage);
        return;
    }

    ctx.drainUnusedSlots();

    if (!ctx.isActive)
    {
        // Inactive readers are not monitored for data loss
        for (SizeT i = 0; i < ctx.slots.size(); ++i)
            ctx.dataLossMonitor.setMonitored(i, false);

        // Terminology: ACTIVE/INACTIVE is the reader/port level switch (setActive) -
        // "pause the whole reader". USED/UNUSED is per-input participation (setInputUsed) -
        // "exclude this input from reading". The unused mechanism reuses port deactivation
        // internally because that is what stops data while preserving events.
        //
        // Inactivity suspends data flow only: descriptor/gap events are enqueued regardless
        // of the active flag and must still surface through reads (and through the
        // dataAvailable callback, which is why the event bits are maintained here too).
        // Unused inputs' events were already drained above (drainUnusedSlots).
        std::vector<SizeT> inactiveEventInputs;
        std::vector<SizeT> inactiveSlotIndices;
        const auto inactiveReaders = ctx.collectUsedReaders(inactiveSlotIndices);
        for (SizeT position = 0; position < inactiveReaders.size(); ++position)
        {
            const auto slotIndex = inactiveSlotIndices[position];
            // Clear-then-drain (see step 3): clear before adopting so a concurrent
            // lock-free arrival re-arms the flag instead of being stranded.
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
            ctx.invalidateSynchronization();
            ctx.setStateWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(inactiveEventInputs));
        }
        else
        {
            ctx.setState(ReaderState::Inactive);
        }
        return;
    }

    // 2. Resolve the used set and the main input (the explicitly selected main input is
    // never silently replaced)
    std::vector<SizeT> slotIndices;
    const auto usedReaders = ctx.collectUsedReaders(slotIndices);
    if (usedReaders.empty())
    {
        ctx.invalidateSynchronization();
        ctx.setState(ReaderState::WaitingForConnections, "No used inputs");
        return;
    }

    SizeT mainPosition = 0;
    if (ctx.mainInputId.assigned())
    {
        const auto mainSlot = ctx.mainSlotIndex();
        const auto position = std::find(slotIndices.begin(), slotIndices.end(), mainSlot);
        if (mainSlot == slotNotFound || position == slotIndices.end())
        {
            ctx.invalidateModel();
            ctx.setState(ReaderState::WaitingForConnections,
                         "The selected main input is not among the used inputs",
                         mainSlot == slotNotFound ? std::vector<SizeT>{} : std::vector<SizeT>{mainSlot});
            return;
        }
        mainPosition = static_cast<SizeT>(position - slotIndices.begin());
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
            ctx.setStateWithAffected(ReaderState::WaitingForConnections, "Inputs", " have no signal connected", std::move(unconnected));
            return;
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
                    ctx.syncManager.buildCommonModel(usedReaders, slotIndices, mainPosition);
            }

            ctx.invalidateSynchronization();
            ctx.setStateWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(eventInputs));
            return;
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
            ctx.setStateWithAffected(ReaderState::WaitingForDescriptors, "Inputs", " have no descriptors yet", std::move(missing));
            return;
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
                ctx.setStateWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(invalidInputs));
                return;
            }
            ctx.setStateWithAffected(
                ReaderState::Incompatible, "Inputs", " are not readable with the current descriptors", std::move(invalidInputs));
            return;
        }
    }

    // 9. Data-loss deadlines. In-band: an input's buffered pre-loss data stays readable
    // (the producer went silent AFTER producing it),
    // so the loss only becomes the reader state once the affected input can no longer
    // contribute. Recovery is per input on its next packet, after which synchronization is
    // re-established.
    {
        const auto lost = ctx.dataLossMonitor.lostSlots();
        if (!lost.empty())
        {
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
            if (!drainedLost.empty())
            {
                ctx.invalidateSynchronization();
                ctx.setStateWithAffected(ReaderState::DataLost, "Inputs", " missed their packet deadline", std::move(drainedLost));
                return;
            }
            // Lost but a full block still buffered: keep reading - the loss surfaces once the
            // input can no longer fill a block
        }
    }

    // Already synchronized: nothing further to establish
    if (ctx.syncManager.getCommonStart() != nullptr)
    {
        ctx.setState(ReaderState::Synchronized);
    }
    else
    {
        // 8. Cross-input compatibility and the common model
        auto setup = ctx.syncManager.buildCommonModel(usedReaders, slotIndices, mainPosition);
        if (!setup.ok())
        {
            ctx.readCoordinator.invalidate();
            if (ctx.exposeBuriedEvents(setup.affectedInputs))
            {
                for (const auto index : setup.affectedInputs)
                    setSlotEvent(*ctx.slots[index], ctx.slots[index]->getQueueReader().hasPendingEvents());
                ctx.setStateWithAffected(ReaderState::EventPending, "Events pending on inputs", "", std::move(setup.affectedInputs));
                return;
            }
            ctx.setState(ReaderState::Incompatible, std::move(setup.message), std::move(setup.affectedInputs));
            return;
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
            {
                ctx.setStateWithAffected(ReaderState::WaitingForData, "Waiting for data on inputs", "", std::move(empty));
                return;
            }
        }

        // 11. Alignment
        auto result = ctx.syncManager.synchronize(usedReaders, slotIndices);
        switch (result.outcome)
        {
            case SyncOutcome::Synchronized:
                // 12. Configure the read pipelines
                ctx.readCoordinator.configure(usedReaders, ctx.syncManager.getModel());
                ctx.nextReadTick = ctx.readOffset();
                ctx.setState(ReaderState::Synchronized);
                break;
            case SyncOutcome::NeedMoreData:
                ctx.setState(ReaderState::Synchronizing, std::move(result.message), std::move(result.affectedInputs));
                break;
            case SyncOutcome::EventPending:
            {
                // setState moved affectedInputs into the outcome; read them back from there
                ctx.invalidateSynchronization();
                auto affected = result.affectedInputs;
                ctx.setState(ReaderState::EventPending, std::move(result.message), std::move(result.affectedInputs));
                for (const auto index : affected)
                    setSlotEvent(*ctx.slots[index], true);
                break;
            }
            case SyncOutcome::Failed:
                // Synchronization failure no longer deactivates the reader.
                // Unlike the Incompatible paths, we do NOT drop buffered data to surface a
                // buried event here: on a sync failure each input's data is individually valid
                // and readable (only the cross-input alignment failed), so the consumer's
                // remedy is to exclude an input or pick a main input - not to lose that input's
                // samples. A queued corrective descriptor surfaces the normal way once the
                // offending input is excluded and re-enabled (dropForInactive on re-enable).
                ctx.setState(ReaderState::SynchronizationFailed, std::move(result.message), std::move(result.affectedInputs));
                break;
        }
    }

    // 13. Readiness for the callback gate - the smallest servable request while synchronized,
    // the first sample while still establishing - is published by publishProducerGateLocked,
    // which every evaluateStateLocked exit path funnels through.
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
