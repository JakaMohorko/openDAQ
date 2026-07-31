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

SizeT Input::availableNative() const
{
    // The adopted basis already stops at the first event in the adopted queue. If there is one, the
    // whole connection queue is behind it in stream order - leading or buried makes no difference -
    // so nothing there is available until that event has been consumed.
    const SizeT adopted = availableNativeBasis.load();
    if (basisHasEventPackets.load())
        return adopted;

    const auto connection = port.getConnection();
    if (!connection.assigned())
        return adopted;  // mid-(dis)connect

    // No guard on Connection::hasEventPacket() here: that means "an event is queued somewhere", not
    // "the queue starts with one", so it would drop the data packets in FRONT of a buried event -
    // which are readable. getSamplesUntilNextEventPacket already stops exactly at the boundary.
    //
    // It is an O(1) counter read while no event is queued and a deque walk otherwise. The producer
    // path never pays the walk: tryRaiseGateFlags checks hasEventPacket itself and forces a full
    // evaluation before it ever gets here, so the walk only happens on an owner-thread query.
    return adopted + static_cast<SizeT>(connection.getSamplesUntilNextEventPacket());
}

SizeT Input::getAvailableSamples() const
{
    const SizeT divider = queueReader.getSampleRateDivider();
    return availableNative() * (divider > 0 ? divider : 1);
}

void Input::setMinReadCount(SizeT countCommon)
{
    if (countCommon == NeverReadable)
    {
        minReadNative.store(NeverReadable);
        return;
    }

    const SizeT divider = queueReader.getSampleRateDivider();
    // Rounded UP, not truncated: one common-rate sample on an input running at half the common
    // rate is still one native sample to wait for, and a minimum of zero would make hasDataToRead
    // unconditionally true. Exact for every real minimum anyway - effectiveMinimum is a multiple
    // of blockLcm, and blockLcm is a multiple of every divider.
    const SizeT effectiveDivider = divider > 0 ? divider : 1;
    minReadNative.store((countCommon + effectiveDivider - 1) / effectiveDivider);
}

SizeT Input::getMinReadCount() const
{
    const SizeT minimum = minReadNative.load();
    if (minimum == NeverReadable)
        return NeverReadable;

    const SizeT divider = queueReader.getSampleRateDivider();
    return minimum * (divider > 0 ? divider : 1);
}

bool Input::hasDataToRead() const
{
    const SizeT minimum = minReadNative.load();
    return minimum != NeverReadable && availableNative() >= minimum;
}

bool Input::hasAdoptedDataToRead() const
{
    const SizeT minimum = minReadNative.load();
    return minimum != NeverReadable && availableNativeBasis.load() >= minimum;
}

bool Input::raiseUnderEpoch(std::uint64_t epochBefore, bool event)
{
    // The owner's end-of-pass truth wins: if a pass ran between the snapshot this raise was
    // computed from and now, the arithmetic is not trustworthy and the caller forces an evaluation.
    if (callbackGate->passEpoch() != epochBefore)
        return false;

    if (event)
        flags.raiseEvent();
    else
        flags.raiseReady();
    return true;
}

bool Input::tryRaiseGateFlags()
{
    // Epoch guard (see CallbackGate): an owner pass can move samples or events between our reads,
    // so nothing computed across one is trustworthy. A raise can never be wrong for long (the
    // evaluation reconciles), but a SKIPPED raise could silence the gate forever - so anything
    // inconsistent returns false and the caller forces an evaluation instead.
    const auto epochBefore = callbackGate->passEpoch();
    if (!CallbackGate::epochQuiet(epochBefore))
        return false;

    // Already holding the gate open for this slot; the caller schedules via gateSatisfied.
    if (flags.event())
        return true;

    // An event already adopted, leading or buried. Raised, not forced: an event is a condition the
    // slot can state, and forcing an evaluation is reserved for a snapshot it cannot trust.
    //
    // Buried-inclusive, which is exactly the owner's rule for this bit (publishProducerGate), and
    // the two must agree or the flag oscillates between them. That is deliberately weaker than
    // "the next thing to read is an event": an event behind a readable block still raises it,
    // because the block may not be servable - the other inputs also have to have one - and the
    // consumer must still be woken to discover the event.
    if (basisHasEventPackets.load())
        return raiseUnderEpoch(epochBefore, true);

    const auto connection = port.getConnection();
    if (!connection.assigned())
        return false;  // mid-(dis)connect: let the evaluation sort it out

    // Same rule on the connection side. Checking it before the availability query also keeps that
    // query O(1): getSamplesUntilNextEventPacket walks the deque only when an event is queued.
    if (connection.hasEventPacket())
        return raiseUnderEpoch(epochBefore, true);

    if (!flags.ready() && hasDataToRead())
        return raiseUnderEpoch(epochBefore, false);

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

void Input::setPortActive(bool active)
{
    port.setActive(active);
}

SlotGateFlags& Input::gateFlags()
{
    return flags;
}

void Input::publishAvailability(SizeT availableNativeUntilEvent, bool hasEventPackets)
{
    availableNativeBasis.store(availableNativeUntilEvent);
    basisHasEventPackets.store(hasEventPackets);
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

// --- Operations over a slot vector -------------------------------------------------------------

void publishSlotAvailability(Input& slot)
{
    auto& reader = slot.getQueueReader();
    const SizeT divider = reader.getSampleRateDivider() > 0 ? reader.getSampleRateDivider() : 1;
    // Buried-inclusive: a queued event behind data still bounds what the connection can add.
    const bool hasEventPackets = reader.hasPendingEvents() || reader.hasQueuedEventPackets();
    slot.publishAvailability(reader.getAvailableSamples() / divider, hasEventPackets);
}

void collectUsedReaders(const std::vector<Input*>& slots, std::vector<QueueReader*>& readers, std::vector<SizeT>& slotIndices)
{
    readers.clear();
    slotIndices.clear();
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        if (!slots[i]->isUsed())
            continue;
        readers.push_back(&slots[i]->getQueueReader());
        slotIndices.push_back(i);
    }
}

SizeT findSlotById(const std::vector<Input*>& slots, const StringPtr& id)
{
    for (SizeT i = 0; i < slots.size(); ++i)
    {
        if (slots[i]->getInputId() == id)
            return i;
    }
    return slotNotFound;
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
