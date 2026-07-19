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
#include <opendaq/multi_reader/queue_reader.h>
#include <opendaq/signal_ptr.h>

#include <atomic>
#include <chrono>

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
     * @brief Every packet arrival (bounded producer path). Coalescing is the owner's
     * job (NotificationCoordinator); the slot's packetPending bit stays set until
     * clearPacketPending() for cheap "anything new since last evaluation" queries.
     */
    virtual void slotPacketReceived(SizeT slotIndex) = 0;
};

/**
 * @brief One input of the multi reader: owns the port reference and the per-input QueueReader,
 * implements IInputPortNotifications for that port, and holds the used/connected/pending flags.
 *
 * Threading contract (spec section 3.2/9):
 * - The IInputPortNotifications entry points are bounded: they update atomics and forward one
 *   semantic notification; no dequeue, no descriptor parsing, no locks, no user callbacks.
 * - Everything under "owner-side API" must be called with the owner's state lock held; the
 *   QueueReader has no lock of its own.
 * - The port holds only a weak reference to this object (its listener), so the owner's strong
 *   reference controls the lifetime; once it is dropped, port notifications stop.
 */
class Input final : public ImplementationOfWeak<IInputPortNotifications>
{
public:
    using SteadyClock = std::chrono::steady_clock;

    explicit Input(SizeT index,
                       const InputPortConfigPtr& port,
                       SampleType valueReadType,
                       SampleType domainReadType,
                       ReadMode mode,
                       const LoggerComponentPtr& logger,
                       IInputListener* listener,
                       bool globalIdFromSignal);

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
     * Initial event packets are enqueued while the connection is still being constructed, before
     * the connected() notification fires - the port is the source of truth, not the notifications.
     * @return true if the QueueReader was rebound to a different connection.
     */
    bool syncConnection();

    /**
     * @brief Used flag only - excluding the slot from masks, compatibility, synchronization and
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

    /// Owner teardown: no listener notifications are forwarded after this returns.
    void detachListener();

private:
    IInputListener* getListener() const;

    std::atomic<SizeT> index;
    const bool globalIdFromSignal;

    InputPortConfigPtr port;
    QueueReader queueReader;
    // Phase 5 (resampling) adds: ResamplerPtr resampler; // null on the direct path

    /// Cached getInputId() result; cleared on connect/disconnect (see getInputId).
    mutable StringPtr cachedInputId;

    std::atomic<IInputListener*> listener;
    std::atomic_bool used{true};
    std::atomic_bool connectedState{false};
    std::atomic_bool packetPending{false};
    std::atomic<SteadyClock::time_point> lastPacketArrival{SteadyClock::time_point{}};

    LoggerComponentPtr loggerComponent;
};

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
