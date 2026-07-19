#include <opendaq/reader_status_impl.h>
#include <coretypes/validation.h>
#include <coretypes/common.h>

#include <algorithm>

BEGIN_NAMESPACE_OPENDAQ

template <class MainInterface, class ... Interfaces>
GenericReaderStatusImpl<MainInterface, Interfaces...>::GenericReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset)
    : eventPacket(eventPacket)
    , valid(valid)
    , offset(offset.assigned() ? offset : NumberPtr(0))
{
}

template <class MainInterface, class ... Interfaces>
ErrCode GenericReaderStatusImpl<MainInterface, Interfaces...>::getReadStatus(ReadStatus* status)
{
    OPENDAQ_PARAM_NOT_NULL(status);
    if (valid && !eventPacket.assigned())
        *status = ReadStatus::Ok;
    else if (eventPacket.assigned())
        *status = ReadStatus::Event;
    else
        *status = ReadStatus::Fail;

    return OPENDAQ_SUCCESS;
}

template <class MainInterface, class ... Interfaces>
ErrCode GenericReaderStatusImpl<MainInterface, Interfaces...>::getEventPacket(IEventPacket** packet)
{
    OPENDAQ_PARAM_NOT_NULL(packet);
    *packet = eventPacket.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <class MainInterface, class ... Interfaces>
ErrCode GenericReaderStatusImpl<MainInterface, Interfaces...>::getValid(Bool* valid)
{
    OPENDAQ_PARAM_NOT_NULL(valid);
    *valid = this->valid;
    return OPENDAQ_SUCCESS;
}

template <class MainInterface, class ... Interfaces>
ErrCode GenericReaderStatusImpl<MainInterface, Interfaces...>::getOffset(INumber** offset)
{
    OPENDAQ_PARAM_NOT_NULL(offset);
    *offset = this->offset.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

BlockReaderStatusImpl::BlockReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset, SizeT readSamples)
    : Super(eventPacket, valid, offset)
    , readSamples(readSamples)
{
}

ErrCode BlockReaderStatusImpl::getReadSamples(SizeT* readSamples)
{
    OPENDAQ_PARAM_NOT_NULL(readSamples);
    *readSamples = this->readSamples;
    return OPENDAQ_SUCCESS;
}

TailReaderStatusImpl::TailReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset, Bool sufficientHistory)
    : Super(eventPacket, valid, offset)
    , sufficientHistory(sufficientHistory)
{
}

ErrCode TailReaderStatusImpl::getReadStatus(ReadStatus* status)
{
    OPENDAQ_PARAM_NOT_NULL(status);
    if (!sufficientHistory)
    {
        *status = ReadStatus::Fail;
        return OPENDAQ_SUCCESS;
    }
    return Super::getReadStatus(status);
}

ErrCode TailReaderStatusImpl::getSufficientHistory(Bool* status)
{
    OPENDAQ_PARAM_NOT_NULL(status);
    *status = sufficientHistory;
    return OPENDAQ_SUCCESS;
}

namespace
{

// The compatibility factory carries no explicit read status - derive the closest one from
// what it does carry, so getValid() keeps reporting exactly what the factory was given.
ReadStatus deriveCompatReadStatus(const DictPtr<IString, IEventPacket>& eventPackets, Bool valid)
{
    if (!valid)
        return ReadStatus::Fail;
    if (eventPackets.assigned() && eventPackets.getCount() > 0)
        return ReadStatus::Event;
    return ReadStatus::Ok;
}

}  // namespace

MultiReaderStatusImpl::MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor, const DictPtr<IString, IEventPacket>& eventPackets, Bool valid, const NumberPtr& offset)
    : MultiReaderStatusImpl(mainDescriptor,
                            eventPackets,
                            offset,
                            deriveCompatReadStatus(eventPackets, valid),
                            String(""),
                            nullptr)
{
}

MultiReaderStatusImpl::MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor,
                                             const DictPtr<IString, IEventPacket>& eventPackets,
                                             const NumberPtr& offset,
                                             ReadStatus readStatus,
                                             const StringPtr& stateMessage,
                                             const DictPtr<IString, IInteger>& inputStates)
    // Only Fail is unrecoverable, so only Fail reads as invalid (review decision C5/Q1)
    : Super(mainDescriptor, readStatus != ReadStatus::Fail, offset)
    , eventPackets(eventPackets.assigned() ? eventPackets : Dict<IString, IEventPacket>())
    , readStatus(readStatus)
    , stateMessage(stateMessage.assigned() ? stateMessage : String(""))
    , inputStates(inputStates.assigned() ? inputStates : Dict<IString, IInteger>())
{
}

ErrCode MultiReaderStatusImpl::getReadStatus(ReadStatus* status)
{
    OPENDAQ_PARAM_NOT_NULL(status);
    *status = readStatus;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getEventPacket(IEventPacket** packet)
{
    OPENDAQ_PARAM_NOT_NULL(packet);

    // Compatibility accessor: the first entry of the event dictionary, so the accessor
    // still reports an event whenever getReadStatus() does
    if (eventPackets.getCount() > 0)
        *packet = eventPackets.getValueList()[0].asPtr<IEventPacket>().addRefAndReturn();
    else
        *packet = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getMainDescriptor(IEventPacket** descriptor)
{
    return Super::getEventPacket(descriptor);
}

ErrCode MultiReaderStatusImpl::getEventPackets(IDict** events)
{
    OPENDAQ_PARAM_NOT_NULL(events);
    *events = eventPackets.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getInputStates(IDict** inputStates)
{
    OPENDAQ_PARAM_NOT_NULL(inputStates);
    *inputStates = this->inputStates.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getStateMessage(IString** message)
{
    OPENDAQ_PARAM_NOT_NULL(message);
    *message = stateMessage.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

OPENDAQ_DEFINE_CLASS_FACTORY (
    LIBRARY_FACTORY, ReaderStatus,
    IEventPacket*, eventPacket,
    Bool, valid,
    INumber*, offset
)

OPENDAQ_DEFINE_CLASS_FACTORY (
    LIBRARY_FACTORY, BlockReaderStatus,
    IEventPacket*, eventPacket,
    Bool, valid,
    INumber*, offset,
    SizeT, readSamples
)

OPENDAQ_DEFINE_CLASS_FACTORY (
    LIBRARY_FACTORY, TailReaderStatus,
    IEventPacket*, eventPacket,
    Bool, valid,
    INumber*, offset,
    Bool, sufficientHistory
)

OPENDAQ_DEFINE_CLASS_FACTORY (
    LIBRARY_FACTORY, MultiReaderStatus,
    IEventPacket*, mainDescriptor,
    IDict*, eventPackets,
    Bool, valid,
    INumber*, offset
)

END_NAMESPACE_OPENDAQ
