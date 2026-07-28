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
#include <chrono>
#include <limits>
#include <memory>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

/**
 * @brief Semantic notifications an Input raises toward its owner (the multi reader).
 *
 * Non-owning: the owner holds the strong reference to every slot and detaches itself
 * (detachListener) before it goes away. Calls arrive on producer/connection threads and
 * must stay bounded: implementations record the fact, take the owner state lock outside
 * this callback where needed, and never call back into the slot from inside the callback
 * except through the owner-locked slot API.
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
    /**
     * @brief Every packet arrival (bounded producer path). The slot has already updated its
     * gate flags from a minimal connection introspection; the listener decides whether the
     * shared gate warrants scheduling an evaluation. forceEvaluation is the conservative
     * escape hatch: the slot could not trust its snapshot (owner pass in flight, connection
     * mid-rebind) or the reader is in a state where every packet must re-enter the state
     * machine (anything but steady Synchronized) - the listener then schedules
     * unconditionally, restoring the classic packet-per-evaluation behavior.
     */
    virtual void slotPacketReceived(SizeT slotIndex, bool forceEvaluation) = 0;
};

/**
 * @brief One input of the multi reader: owns the port reference and the per-input QueueReader,
 * implements IInputPortNotifications for that port, and holds the used/connected/pending flags
 * plus this slot's producer-facing callback-gate state.
 *
 * Gate state (all atomics, producer-readable):
 * - gate flags (SlotGateFlags): this slot's ready/event contribution to the shared CallbackGate.
 * - basis: the adopted queue's availability-until-event (native samples) and whether any event
 *   packet is adopted - published by the owner after every pass that moves or consumes samples.
 *   The producer adds the connection's own O(1) counters on top to get the current truth.
 * - readyThresholdNative: the effective minimum (native samples) at which this slot becomes
 *   ready; NeverReady disables producer ready-raises (no model, unused, unconnected).
 * - wakeOnAnyPacket: every packet forces an evaluation (any state but steady Synchronized).
 *
 * Threading contract:
 * - The IInputPortNotifications entry points are bounded: they update atomics, read two O(1)
 *   connection counters and forward one semantic notification; no dequeue, no descriptor
 *   parsing, no reader-state locks, no user callbacks.
 * - Everything under "owner-side API" must be called with the owner's state lock held; the
 *   QueueReader has no lock of its own.
 * - The port holds only a weak reference to this object (its listener), so the owner's strong
 *   reference controls the lifetime; once it is dropped, port notifications stop.
 */
class Input final : public ImplementationOfWeak<IInputPortNotifications>
{
public:
    using SteadyClock = std::chrono::steady_clock;

    /// Sentinel threshold: the producer never raises the ready flag.
    static constexpr SizeT NeverReady = std::numeric_limits<SizeT>::max();

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

    // --- Owner-side API (owner state lock held) ---

    SizeT getIndex() const;
    /// Owner reindexes remaining slots after removeInput; buffer order follows slot order.
    void setIndex(SizeT newIndex);

    /**
     * @brief Identity used by removeInput/setInputUsed and the status event dictionary:
     * the connected signal's global id when the reader was constructed from signals,
     * otherwise the port's global id. Falls back to the port id while no signal is connected.
     */
    StringPtr getInputId() const;

    const InputPortConfigPtr& getPort() const;
    QueueReader& getQueueReader();

    bool isConnected() const;

    /// Point the QueueReader at the port's current connection (connect, reconnect and disconnect alike).
    void rebindConnection();

    /**
     * @brief Resync connected state and the QueueReader's connection from the port itself.
     * A port connected before its listener was installed produces no connected() callback, and
     * InputPort::setListener front-loads a descriptor event (Connection::enqueueLastDescriptor)
     * without notifying - so a slot can start out connected with queued events and no callback
     * ever delivered. The port is the source of truth, not the notifications.
     * @return true if the QueueReader was rebound to a different connection.
     */
    bool syncConnection();

    /**
     * @brief Used flag only - excluding the slot from the gate, compatibility, synchronization and
     * availability is the owner's responsibility, as is deactivating the port (setPortActive)
     * and resetting/revalidating on re-enable.
     */
    bool isUsed() const;
    void setUsed(bool value);

    bool isPacketPending() const;
    /// @return true if a packet was pending; re-arms slotPacketPending for the next arrival.
    bool clearPacketPending();
    SteadyClock::time_point getLastPacketArrival() const;

    void setPortActive(bool active);

    // --- Gate maintenance (owner state lock held unless noted) ---

    /// This slot's ready/event contribution to the shared gate. Producer-safe for raises;
    /// owner-only for lowering.
    SlotGateFlags& gateFlags();

    /**
     * @brief Publish the adopted queue's producer-visible basis: availability until the next
     * event (native samples) and whether any event packet (leading or buried) is adopted.
     * Owner-called after every pass that adopts or consumes samples on this slot.
     */
    void publishGateBasis(SizeT availableNativeUntilEvent, bool hasEventPackets);

    /// Native-sample threshold at which the producer raises the ready flag; NeverReady disables.
    void setReadyThresholdNative(SizeT thresholdNative);

    /// True in every state but steady Synchronized: each packet forces an evaluation.
    void setWakeOnAnyPacket(bool wake);

    /// Owner teardown: no listener notifications are forwarded after this returns.
    void detachListener();

private:
    IInputListener* getListener() const;
    /**
     * @brief Producer-side gate maintenance: raise this slot's ready/event flags from the
     * published basis plus the connection's O(1) counters, guarded by the owner-pass epoch.
     * @return false when the snapshot cannot be trusted (owner pass in flight, epoch moved,
     * connection unassigned) - the caller then forces an evaluation instead.
     */
    bool tryRaiseGateFlags();

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
    std::atomic<SteadyClock::time_point> lastPacketArrival{SteadyClock::time_point{}};

    std::shared_ptr<CallbackGate> callbackGate;
    SlotGateFlags flags;
    std::atomic<SizeT> basisAvailableNative{0};
    std::atomic_bool basisHasEventPackets{false};
    std::atomic<SizeT> readyThresholdNative{NeverReady};
    /// Defaults to true: until the first full evaluation publishes a steady Synchronized
    /// state, every packet re-enters the state machine (classic behavior).
    std::atomic_bool wakeOnAnyPacket{true};

    LoggerComponentPtr loggerComponent;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
