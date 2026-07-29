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

#include <fmt/format.h>
#include <fmt/ranges.h>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

void invalidateSynchronization(SynchronizationManager& syncManager,
                               ReadCoordinator& readCoordinator,
                               std::optional<std::int64_t>& nextReadTick)
{
    syncManager.clearSynchronization();
    readCoordinator.invalidate();
    nextReadTick.reset();
}

StateOutcome outcomeWithAffected(ReaderState state,
                                 const char* messagePrefix,
                                 const char* messageSuffix,
                                 std::vector<SizeT> affected)
{
    auto message = fmt::format("{} [{}]{}", messagePrefix, fmt::join(affected, ", "), messageSuffix);
    return {state, std::move(message), std::move(affected)};
}

std::vector<QueueReader*> StateContext::collectUsedReaders(std::vector<SizeT>& slotIndices) const
{
    std::vector<QueueReader*> readers;
    multi_reader::collectUsedReaders(slots, readers, slotIndices);
    return readers;
}

SizeT StateContext::mainSlotIndex() const
{
    if (!mainInputId.assigned())
        return slotNotFound;
    return findSlotById(slots, mainInputId);
}

void StateContext::invalidateSynchronization()
{
    multi_reader::invalidateSynchronization(syncManager, readCoordinator, nextReadTick);
}

void StateContext::invalidateModel()
{
    invalidateSynchronization();
    syncManager.invalidateModel();
    modelInvalidated = true;
}

void StateContext::drainUnusedSlots()
{
    // Unused inputs stay observable. Their ports are inactive, so data is dropped at
    // the connection and only events can arrive; draining them makes pending events visible
    // in the per-input states and lets them fire the dataAvailable callback (the recovery
    // signal a consumer answers with setInputUsed(id, true)).
    for (auto* slot : slots)
    {
        if (slot->isUsed())
            continue;

        slot->adoptQueuedPackets();
        if (!slot->isConnected())
        {
            slot->publishGateBasis(0, false);
            setSlotEvent(*slot, false);
            continue;
        }

        slot->clearPacketPending();
        auto& reader = slot->getQueueReader();
        reader.drain();
        publishSlotBasis(*slot);
        setSlotEvent(*slot, reader.hasPendingEvents());
    }
}

bool StateContext::exposeBuriedEvents(const std::vector<SizeT>& affected)
{
    bool exposed = false;
    for (const auto index : affected)
    {
        if (index >= slots.size())
            continue;

        auto& reader = slots[index]->getQueueReader();
        if (!reader.hasPendingEvents() && reader.hasQueuedEventPackets())
        {
            // The failing input has a corrective descriptor change queued behind data that
            // was produced under the old, failing descriptor. That data can never be read
            // while the input keeps failing, so drop it and let the event surface - the same
            // semantics the setInputUsed(false -> true) recovery applies (dropForInactive).
            // Without this, an actively producing input could never recover: the fix would
            // stay buried behind unreadable data forever.
            reader.dropForInactive();
            exposed |= reader.hasPendingEvents();
        }
    }
    return exposed;
}

std::optional<std::int64_t> StateContext::readOffset() const
{
    const auto* start = syncManager.getCommonStart();
    if (start == nullptr)
        return std::nullopt;

    switch (resolvedDomainReadType)
    {
        case SampleType::Int64:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int64_t>*>(start))
                return typed->getValue();
            break;
        case SampleType::UInt64:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint64_t>*>(start))
                return static_cast<std::int64_t>(typed->getValue());
            break;
        case SampleType::Int32:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int32_t>*>(start))
                return typed->getValue();
            break;
        case SampleType::UInt32:
            if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint32_t>*>(start))
                return typed->getValue();
            break;
        default:
            break;
    }
    return std::nullopt;
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
