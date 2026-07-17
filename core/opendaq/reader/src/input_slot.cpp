#include <opendaq/input_slot.h>

BEGIN_NAMESPACE_OPENDAQ

InputSlot::InputSlot(SizeT index,
                     const InputPortConfigPtr& port,
                     SampleType valueReadType,
                     SampleType domainReadType,
                     ReadMode mode,
                     const LoggerComponentPtr& logger,
                     IInputSlotListener* listener,
                     bool globalIdFromSignal)
    : index(index)
    , globalIdFromSignal(globalIdFromSignal)
    , port(port)
    , queueReader(port, valueReadType, domainReadType, mode, logger, globalIdFromSignal)
    , listener(listener)
    , loggerComponent(logger)
{
    connectedState = port.getConnection().assigned();
}

ErrCode InputSlot::acceptsSignal(IInputPort* inputPort, ISignal* signal, Bool* accept)
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

ErrCode InputSlot::connected(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        connectedState = true;
        if (auto* const target = getListener())
            target->slotConnected(index);
        return OPENDAQ_SUCCESS;
    });
}

ErrCode InputSlot::disconnected(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        connectedState = false;
        if (auto* const target = getListener())
            target->slotDisconnected(index);
        return OPENDAQ_SUCCESS;
    });
}

ErrCode InputSlot::packetReceived(IInputPort* /*inputPort*/)
{
    return daqTry([&]
    {
        lastPacketArrival.store(SteadyClock::now());
        packetPending = true;
        if (auto* const target = getListener())
            target->slotPacketReceived(index);
        return OPENDAQ_SUCCESS;
    });
}

SizeT InputSlot::getIndex() const
{
    return index;
}

void InputSlot::setIndex(SizeT newIndex)
{
    index = newIndex;
}

StringPtr InputSlot::getInputId() const
{
    if (globalIdFromSignal)
    {
        if (auto signal = port.getSignal(); signal.assigned())
            return signal.getGlobalId();
    }
    return port.getGlobalId();
}

const InputPortConfigPtr& InputSlot::getPort() const
{
    return port;
}

QueueReader& InputSlot::getQueueReader()
{
    return queueReader;
}

bool InputSlot::isConnected() const
{
    return connectedState;
}

void InputSlot::rebindConnection()
{
    queueReader.updateConnection();
}

bool InputSlot::syncConnection()
{
    connectedState = port.getConnection().assigned();
    return queueReader.refreshConnection();
}

bool InputSlot::isUsed() const
{
    return used;
}

void InputSlot::setUsed(bool value)
{
    used = value;
}

bool InputSlot::isPacketPending() const
{
    return packetPending;
}

bool InputSlot::clearPacketPending()
{
    return packetPending.exchange(false);
}

InputSlot::SteadyClock::time_point InputSlot::getLastPacketArrival() const
{
    return lastPacketArrival.load();
}

void InputSlot::setPortActive(bool active)
{
    port.setActive(active);
}

void InputSlot::detachListener()
{
    listener = nullptr;
}

IInputSlotListener* InputSlot::getListener() const
{
    return listener.load();
}

END_NAMESPACE_OPENDAQ
