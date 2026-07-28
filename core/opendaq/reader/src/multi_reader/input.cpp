#include <opendaq/multi_reader/input.h>

#include <opendaq/connection_ptr.h>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

Input::Input(SizeT index,
                     const InputPortConfigPtr& port,
                     SampleType valueReadType,
                     SampleType domainReadType,
                     ReadMode mode,
                     const LoggerComponentPtr& logger,
                     IInputListener* listener,
                     bool globalIdFromSignal,
                     std::shared_ptr<CallbackGate> gate)
    : index(index)
    , globalIdFromSignal(globalIdFromSignal)
    , port(port)
    , queueReader(port, valueReadType, domainReadType, mode, logger, globalIdFromSignal)
    , listener(listener)
    , callbackGate(std::move(gate))
    , flags(callbackGate)
    , loggerComponent(logger)
{
    connectedState = port.getConnection().assigned();
}

ErrCode Input::acceptsSignal(IInputPort* inputPort, ISignal* signal, Bool* accept)
{
    OPENDAQ_PARAM_NOT_NULL(accept);

    return daqTry([&]
    {
        if (auto* const target = getListener())
            *accept = target->slotAcceptsSignal(index, SignalPtr::Borrow(signal)) ? True : False;
        else
            *accept = True;
        return OPENDAQ_SUCCESS;
    });
}

ErrCode Input::connected(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        connectedState = true;
        cachedInputId = nullptr;  // the connected signal (hence the input id) changed
        if (auto* const target = getListener())
            target->slotConnected(index);
        return OPENDAQ_SUCCESS;
    });
}

ErrCode Input::disconnected(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        connectedState = false;
        cachedInputId = nullptr;
        if (auto* const target = getListener())
            target->slotDisconnected(index);
        return OPENDAQ_SUCCESS;
    });
}

void Input::listen(const ObjectPtr<IInputPortNotifications>& self)
{
    // The owner passes its own strong reference rather than the slot deriving one from `this`:
    // the port stores only a weak listener reference, so the owner's ref is what keeps the slot
    // alive, and it is the owner that must be able to end the slot's lifetime (removeInput).
    port.setListener(self);
}

void Input::replayMissedPortCallbacks()
{
    // Nothing to replay if no signal is connected yet: a later connect() delivers the real thing.
    if (!port.getConnection().assigned())
        return;

    checkErrorInfo(connected(port));

    // The connection can already hold packets - at minimum the descriptor event that listen()'s
    // setListener front-loaded without notifying.
    checkErrorInfo(packetReceived(port));
}

ErrCode Input::packetReceived(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        lastPacketArrival.store(SteadyClock::now());
        packetPending = true;

        // Steady Synchronized state: raise the gate flags from a minimal introspection and let
        // the listener schedule only when the gate is open. Any other state (or an untrusted
        // snapshot) forces the evaluation - the classic packet-per-evaluation behavior.
        bool force = wakeOnAnyPacket.load();
        if (!force)
            force = !tryRaiseGateFlags();

        if (auto* const target = getListener())
            target->slotPacketReceived(index, force);
        return OPENDAQ_SUCCESS;
    });
}

bool Input::tryRaiseGateFlags()
{
    // Epoch guard (see CallbackGate): an owner pass can move samples from the connection into
    // the adopted queue between our reads, making basis + connection undercount. A raise can
    // never be wrong for long (the evaluation reconciles), but a SKIPPED raise could silence
    // the gate forever - so anything inconsistent returns false and the caller forces an
    // evaluation instead.
    const auto epochBefore = callbackGate->passEpoch();
    if (!CallbackGate::epochQuiet(epochBefore))
        return false;

    // A set event flag already holds the gate open; the caller schedules via gateSatisfied.
    if (flags.event())
        return true;

    // Events are rare and always transition the reader out of the steady synchronized state.
    // Producers never touch the event counter (that would race the owner and risk a stale flag
    // the owner's gate-skip logic would perpetuate); instead any event indication - adopted
    // (basis) or still on the connection - forces a full evaluation, which sets event flags
    // authoritatively under the state lock. Only readiness, the common data-packet case, is
    // self-gated here.
    if (basisHasEventPackets.load())
        return false;

    const auto connection = port.getConnection();
    if (!connection.assigned())
        return false;  // mid-(dis)connect: let the evaluation sort it out

    // Both connection queries are O(1) counter reads under the connection's own lock, which the
    // enqueue that triggered this notification has already released.
    if (connection.hasEventPacket())
        return false;

    if (!flags.ready())
    {
        const SizeT threshold = readyThresholdNative.load();
        if (threshold != NeverReady)
        {
            const SizeT available = basisAvailableNative.load() + static_cast<SizeT>(connection.getSamplesUntilNextEventPacket());
            if (available >= threshold)
            {
                if (callbackGate->passEpoch() != epochBefore)
                    return false;  // an owner pass ran under us; its end-of-pass truth wins
                flags.raiseReady();
            }
        }
    }
    return true;
}

SizeT Input::getIndex() const
{
    return index;
}

void Input::setIndex(SizeT newIndex)
{
    index = newIndex;
}

StringPtr Input::getInputId() const
{
    // Cached: getGlobalId walks the component's parent chain and builds a path string, and the
    // status path calls this per slot per read. The id is stable per connection, so it is only
    // recomputed after a connect/disconnect (which clears the cache). A connected signal being
    // renamed/reparented in place is out of contract and not reflected until reconnect.
    if (cachedInputId.assigned())
        return cachedInputId;

    if (globalIdFromSignal)
    {
        if (auto signal = port.getSignal(); signal.assigned())
        {
            cachedInputId = signal.getGlobalId();
            return cachedInputId;
        }
    }
    cachedInputId = port.getGlobalId();
    return cachedInputId;
}

const InputPortConfigPtr& Input::getPort() const
{
    return port;
}

QueueReader& Input::getQueueReader()
{
    return queueReader;
}

bool Input::isConnected() const
{
    return connectedState;
}

void Input::rebindConnection()
{
    queueReader.updateConnection();
}

void Input::adoptQueuedPackets()
{
    // The owner's evaluation points are the only places queues are refreshed.
    if (connectedState.load())
        queueReader.drain();
}

bool Input::isUsed() const
{
    return used;
}

void Input::setUsed(bool value)
{
    used = value;
}

bool Input::isPacketPending() const
{
    return packetPending;
}

bool Input::clearPacketPending()
{
    return packetPending.exchange(false);
}

Input::SteadyClock::time_point Input::getLastPacketArrival() const
{
    return lastPacketArrival.load();
}

void Input::setPortActive(bool active)
{
    port.setActive(active);
}

SlotGateFlags& Input::gateFlags()
{
    return flags;
}

void Input::publishGateBasis(SizeT availableNativeUntilEvent, bool hasEventPackets)
{
    basisAvailableNative.store(availableNativeUntilEvent);
    basisHasEventPackets.store(hasEventPackets);
}

void Input::setReadyThresholdNative(SizeT thresholdNative)
{
    readyThresholdNative.store(thresholdNative);
}

void Input::setWakeOnAnyPacket(bool wake)
{
    wakeOnAnyPacket.store(wake);
}

void Input::detachListener()
{
    listener = nullptr;
}

IInputListener* Input::getListener() const
{
    return listener.load();
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
