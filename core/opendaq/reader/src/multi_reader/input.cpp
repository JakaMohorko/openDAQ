#include <opendaq/multi_reader/input.h>

#include <opendaq/connection_ptr.h>

#include <cassert>

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
    // The port stores only a weak listener reference; the owner's strong ref controls lifetime.
    port.setListener(self);
}

ErrCode Input::packetReceived(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        packetPending = true;

        // Raise the gate flags from a minimal introspection; an untrusted snapshot (or
        // wakeOnAnyPacket) forces the evaluation instead.
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
    // An adopted event bounds everything: the whole connection queue is behind it in stream order
    const SizeT adopted = availableNativeBasis.load();
    if (basisHasEventPackets.load())
        return adopted;

    const auto connection = port.getConnection();
    if (!connection.assigned())
        return adopted;  // mid-(dis)connect

    // getSamplesUntilNextEventPacket stops exactly at the event boundary, keeping data in
    // front of a buried event countable.
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
    // Rounded UP, not truncated: a minimum of zero would make hasDataToRead unconditionally true
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
    if (minimum == NeverReadable)
        return false;  // unused or unconnected: no basis is maintained for these

    // Catches an owner pass that moved samples without republishing the producer-visible basis
    [[maybe_unused]] const SizeT divider = queueReader.getSampleRateDivider();
    assert(availableNativeBasis.load() * (divider > 0 ? divider : 1) == queueReader.getAvailableSamples() &&
           "stale availability basis - a pass moved samples without republishing it");

    return availableNativeBasis.load() >= minimum;
}

bool Input::raiseUnderEpoch(std::uint64_t epochBefore, bool event)
{
    // If an owner pass ran since the snapshot, the arithmetic is not trustworthy
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
    // Epoch guard (see CallbackGate): a skipped raise could silence the gate forever, so
    // anything inconsistent returns false and the caller forces an evaluation instead.
    const auto epochBefore = callbackGate->passEpoch();
    if (!CallbackGate::epochQuiet(epochBefore))
        return false;

    // Either flag up means this slot's gate contribution is complete
    if (flags.ready() || flags.event())
        return true;

    // Data-first (the owner's rule too): a servable block before the next event boundary
    // raises readiness, an event behind it stays quiet until the data is consumed
    if (hasDataToRead())
        return raiseUnderEpoch(epochBefore, false);

    // No servable data before the boundary: an event is the only thing that can wake the consumer
    if (basisHasEventPackets.load())
        return raiseUnderEpoch(epochBefore, true);

    const auto connection = port.getConnection();
    if (!connection.assigned())
        return false;  // mid-(dis)connect: let the evaluation sort it out

    // Same rule for an event the owner has not adopted yet
    if (connection.hasEventPacket())
        return raiseUnderEpoch(epochBefore, true);

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
    // Cached: getGlobalId is expensive and the id is stable per connection
    // (connect/disconnect clears the cache).
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
    // Buried-inclusive: a queued event behind data still bounds what the connection can add
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
