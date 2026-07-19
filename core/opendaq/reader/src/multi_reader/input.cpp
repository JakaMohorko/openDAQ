#include <opendaq/multi_reader/input.h>

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
        if (auto* const target = getListener())
            target->slotDisconnected(index);
        return OPENDAQ_SUCCESS;
    });
}

ErrCode Input::packetReceived(IInputPort* /*inputPort*/)
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
    if (globalIdFromSignal)
    {
        if (auto signal = port.getSignal(); signal.assigned())
            return signal.getGlobalId();
    }
    return port.getGlobalId();
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

bool Input::syncConnection()
{
    connectedState = port.getConnection().assigned();
    const bool rebound = queueReader.refreshConnection();
    // The owner's evaluation points are the only places queues are refreshed (#10) -
    // adopt whatever the producers enqueued since the last evaluation
    if (!rebound && connectedState)
        queueReader.drain();
    return rebound;
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
