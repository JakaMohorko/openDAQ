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
#include <coretypes/intfs.h>
#include <opendaq/input_port_config_ptr.h>
#include <opendaq/input_port_notifications.h>
#include <opendaq/logger_component_ptr.h>
#include <opendaq/multi_reader/callback_gate.h>
#include <opendaq/multi_reader/queue_reader.h>
#include <opendaq/signal_ptr.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Semantic notifications an Input raises toward its owner (the multi reader). Non-owning;
 * calls arrive on producer/connection threads and must stay bounded.
 */
struct IInputListener
{
    virtual ~IInputListener() = default;

    /// A signal is proposed to the slot's port; return true to accept.
    virtual bool slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal) = 0;
    /// A signal was connected to the slot's port.
    virtual void slotConnected(SizeT slotIndex) = 0;
    /// The signal was disconnected from the slot's port.
    virtual void slotDisconnected(SizeT slotIndex) = 0;
    /// Every packet arrival (bounded producer path); forceEvaluation means the slot could not
    /// trust its snapshot and the listener should evaluate unconditionally.
    virtual void slotPacketReceived(SizeT slotIndex, bool forceEvaluation) = 0;
};

/**
 * @brief One input of the multi reader: owns the port reference and the per-input QueueReader,
 * implements IInputPortNotifications for that port, and holds the used/connected/pending flags
 * plus this slot's producer-facing callback-gate state. Owner-side API needs the owner's lock.
 */
class Input final : public ImplementationOfWeak<IInputPortNotifications>
{
public:
    /// A minimum nothing can meet: hasDataToRead() is false whatever the queues hold.
    static constexpr SizeT NeverReadable = std::numeric_limits<SizeT>::max();

    explicit Input(SizeT index,
                       const InputPortConfigPtr& port,
                       SampleType valueReadType,
                       SampleType domainReadType,
                       ReadMode mode,
                       const LoggerComponentPtr& logger,
                       IInputListener* listener,
                       bool globalIdFromSignal,
                       std::shared_ptr<CallbackGate> gate);

    // IInputPortNotifications (producer/connection threads)
    ErrCode INTERFACE_FUNC acceptsSignal(IInputPort* inputPort, ISignal* signal, Bool* accept) override;
    ErrCode INTERFACE_FUNC connected(IInputPort* inputPort) override;
    ErrCode INTERFACE_FUNC disconnected(IInputPort* inputPort) override;
    ErrCode INTERFACE_FUNC packetReceived(IInputPort* inputPort) override;

    /// Take over the port as its listener; the owner's strong ref keeps the slot alive.
    void listen(const ObjectPtr<IInputPortNotifications>& self);

    // --- Owner-side API (owner state lock held) ---

    SizeT getIndex() const;
    /// Owner reindexes remaining slots after removeInput; buffer order follows slot order.
    void setIndex(SizeT newIndex);

    /// Input identity: the connected signal's global id when built from signals, otherwise
    /// the port's global id (fallback while no signal is connected).
    StringPtr getInputId() const;

    const InputPortConfigPtr& getPort() const;
    QueueReader& getQueueReader();

    bool isConnected() const;

    /// Point the QueueReader at the port's current connection (connect, reconnect and disconnect alike).
    void rebindConnection();

    /// Adopt whatever the producers enqueued on the connection since the last evaluation.
    void adoptQueuedPackets();

    /// Used flag only - all consequences of the flag are the owner's responsibility.
    bool isUsed() const;
    void setUsed(bool value);

    bool isPacketPending() const;
    /// @return true if a packet was pending; re-arms slotPacketPending for the next arrival.
    bool clearPacketPending();

    void setPortActive(bool active);

    // --- Availability (owner state lock held unless noted) ---

    /// Samples this input can contribute right now (adopted + connection, stopping at the first
    /// event), common-rate equivalent. Owner thread only.
    SizeT getAvailableSamples() const;

    /// Minimum samples (common-rate) before the slot counts as readable; NeverReadable while
    /// unused/unconnected. Owner thread only; re-set after anything changes the divider.
    void setMinReadCount(SizeT countCommon);
    SizeT getMinReadCount() const;

    /// getAvailableSamples() >= getMinReadCount(); the PRODUCER-path question, safe on any thread.
    /// Advisory off the owner thread; on owner paths use hasAdoptedDataToRead() (much cheaper).
    bool hasDataToRead() const;

    /// The same minimum against the adopted half alone; the OWNER-path question (two atomic loads).
    bool hasAdoptedDataToRead() const;

    // --- Gate maintenance (owner state lock held unless noted) ---

    /// This slot's ready/event contribution to the shared gate. Producer-safe for raises;
    /// owner-only for lowering.
    SlotGateFlags& gateFlags();

    /// Publish the adopted queue's producer-visible basis; owner-called after every pass that
    /// adopts or consumes samples on this slot.
    void publishAvailability(SizeT availableNativeUntilEvent, bool hasEventPackets);

    /// True in every state but steady Synchronized: each packet forces an evaluation.
    void setWakeOnAnyPacket(bool wake);

    /// Owner teardown: no listener notifications are forwarded after this returns.
    void detachListener();

private:
    IInputListener* getListener() const;
    /// Producer-side gate maintenance (epoch-guarded): raise ready or event as appropriate.
    /// @return false when the snapshot cannot be trusted - the caller forces an evaluation.
    bool tryRaiseGateFlags();

    /// Re-check the epoch, then raise the ready (event == false) or event flag.
    /// @return false when an owner pass ran under the caller's snapshot.
    bool raiseUnderEpoch(std::uint64_t epochBefore, bool event);

    /// Native samples until the first event (adopted basis + connection); producer-safe.
    SizeT availableNative() const;

    std::atomic<SizeT> index;
    const bool globalIdFromSignal;

    InputPortConfigPtr port;
    QueueReader queueReader;

    /// Cached getInputId() result; cleared on connect/disconnect (see getInputId).
    mutable StringPtr cachedInputId;

    std::atomic<IInputListener*> listener;
    std::atomic_bool used{true};
    std::atomic_bool connectedState{false};
    std::atomic_bool packetPending{false};

    std::shared_ptr<CallbackGate> callbackGate;
    SlotGateFlags flags;
    std::atomic<SizeT> availableNativeBasis{0};
    std::atomic_bool basisHasEventPackets{false};
    /// Native equivalent of the common-rate minimum handed to setMinReadCount; defaults to 1.
    std::atomic<SizeT> minReadNative{1};
    /// Defaults to true: until an owner publishes otherwise, every packet forces an evaluation.
    std::atomic_bool wakeOnAnyPacket{true};

    LoggerComponentPtr loggerComponent;
};

// --- Operations over a slot vector -------------------------------------------------------------
// Shared helpers over the facade's slot vector; they belong to no single slot and live here.

constexpr SizeT slotNotFound = static_cast<SizeT>(-1);

/// Owner-side gate writes; the owner is the authority on both flags.
inline void setSlotReady(Input& slot, bool ready)
{
    slot.gateFlags().setReady(ready);
}

inline void setSlotEvent(Input& slot, bool event)
{
    slot.gateFlags().setEvent(event);
}

/// Publish one slot's producer-visible availability from its adopted queue; every owner pass
/// that moves or consumes samples on a slot must end in one of these.
void publishSlotAvailability(Input& slot);

/// Used inputs in slot order plus their slot indices; reuses the vectors' capacity.
void collectUsedReaders(const std::vector<Input*>& slots, std::vector<QueueReader*>& readers, std::vector<SizeT>& slotIndices);

/// Slot index of the input with this id, or slotNotFound.
SizeT findSlotById(const std::vector<Input*>& slots, const StringPtr& id);

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
