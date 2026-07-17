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

// The compatibility factory carries no explicit state - derive the closest one from what it
// does carry. The caller's valid flag takes precedence so getValid() keeps reporting exactly
// what the compat factory was given (events with valid=false stay invalid).
MultiReaderState deriveCompatState(const DictPtr<IString, IEventPacket>& eventPackets, Bool valid)
{
    if (!valid)
        return MultiReaderState::Error;
    if (eventPackets.assigned() && eventPackets.getCount() > 0)
        return MultiReaderState::EventPending;
    return MultiReaderState::Synchronized;
}

// The stream is invalid exactly in the failure states (error contract section 3.3)
Bool deriveValidity(MultiReaderState state)
{
    return state != MultiReaderState::Incompatible && state != MultiReaderState::SynchronizationFailed &&
           state != MultiReaderState::Error;
}

}  // namespace

MultiReaderStatusImpl::MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor, const DictPtr<IString, IEventPacket>& eventPackets, Bool valid, const NumberPtr& offset)
    : MultiReaderStatusImpl(mainDescriptor,
                            eventPackets,
                            offset,
                            deriveCompatState(eventPackets, valid),
                            String(""),
                            nullptr,
                            nullptr,
                            nullptr)
{
}

MultiReaderStatusImpl::MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor,
                                             const DictPtr<IString, IEventPacket>& eventPackets,
                                             const NumberPtr& offset,
                                             MultiReaderState state,
                                             const StringPtr& stateMessage,
                                             const ListPtr<IInteger>& affectedInputIndices,
                                             const ListPtr<IInteger>& eventInputIndices,
                                             const ListPtr<IEventPacket>& orderedEventPackets)
    : Super(mainDescriptor, deriveValidity(state), offset)
    , eventPackets(eventPackets.assigned() ? eventPackets : Dict<IString, IEventPacket>())
    , state(state)
    , stateMessage(stateMessage.assigned() ? stateMessage : String(""))
    , affectedInputIndices(affectedInputIndices.assigned() ? affectedInputIndices : List<IInteger>())
    , eventInputIndices(eventInputIndices.assigned() ? eventInputIndices : List<IInteger>())
    , orderedEventPackets(orderedEventPackets.assigned() ? orderedEventPackets : List<IEventPacket>())
{
}

ErrCode MultiReaderStatusImpl::getReadStatus(ReadStatus* status)
{
    OPENDAQ_PARAM_NOT_NULL(status);
    Bool valid;
    Super::getValid(&valid);

    if (valid && (eventPackets.getCount() == 0))
        *status = ReadStatus::Ok;
    else if (eventPackets.getCount())
        *status = ReadStatus::Event;
    else
        *status = ReadStatus::Fail;

    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getEventPacket(IEventPacket** packet)
{
    OPENDAQ_PARAM_NOT_NULL(packet);

    // Compatibility accessor: the first event of the ordered list. Statuses built through
    // the compatibility factory carry no ordered list - fall back to the first dict entry
    // so the accessor still reports an event whenever getReadStatus() does.
    if (orderedEventPackets.getCount() > 0)
        *packet = orderedEventPackets[0].addRefAndReturn();
    else if (eventPackets.getCount() > 0)
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

ErrCode MultiReaderStatusImpl::getState(MultiReaderState* state)
{
    OPENDAQ_PARAM_NOT_NULL(state);
    *state = this->state;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getStateMessage(IString** message)
{
    OPENDAQ_PARAM_NOT_NULL(message);
    *message = stateMessage.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getAffectedInputCount(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);
    *count = affectedInputIndices.getCount();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getAffectedInputIndex(SizeT statusIndex, SizeT* inputIndex)
{
    OPENDAQ_PARAM_NOT_NULL(inputIndex);
    if (statusIndex >= affectedInputIndices.getCount())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_OUTOFRANGE, "Affected-input index out of range");

    *inputIndex = static_cast<SizeT>(static_cast<Int>(affectedInputIndices[statusIndex]));
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getEventCount(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);
    // Parallel-list contract: only pairs with both a packet and an input index count
    *count = std::min(orderedEventPackets.getCount(), eventInputIndices.getCount());
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusImpl::getEvent(SizeT eventIndex, SizeT* inputIndex, IEventPacket** packet)
{
    OPENDAQ_PARAM_NOT_NULL(inputIndex);
    OPENDAQ_PARAM_NOT_NULL(packet);
    // The two lists are parallel by contract, but the public factory cannot enforce equal
    // lengths - bound the index by both so a mismatch reports an error instead of throwing
    if (eventIndex >= orderedEventPackets.getCount() || eventIndex >= eventInputIndices.getCount())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_OUTOFRANGE, "Event index out of range");

    *inputIndex = static_cast<SizeT>(static_cast<Int>(eventInputIndices[eventIndex]));
    *packet = orderedEventPackets[eventIndex].addRefAndReturn();
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

OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ (
    LIBRARY_FACTORY, MultiReaderStatusImpl, IMultiReaderStatus, createMultiReaderStatusEx,
    IEventPacket*, mainDescriptor,
    IDict*, eventPackets,
    INumber*, offset,
    MultiReaderState, state,
    IString*, stateMessage,
    IList*, affectedInputIndices,
    IList*, eventInputIndices,
    IList*, orderedEventPackets
)

END_NAMESPACE_OPENDAQ
