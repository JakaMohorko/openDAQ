#include <coretypes/validation.h>
#include <opendaq/multi_reader_status_builder_impl.h>
#include <opendaq/reader_status_impl.h>

BEGIN_NAMESPACE_OPENDAQ

MultiReaderStatusBuilderImpl::MultiReaderStatusBuilderImpl()
    : readStatus(ReadStatus::Ok)
{
}

ErrCode MultiReaderStatusBuilderImpl::build(IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(status);

    return daqTry(
        [&]
        {
            *status = createWithImplementation<IMultiReaderStatus, MultiReaderStatusImpl>(
                          mainDescriptor, eventPackets, offset, readStatus, stateMessage, inputStates)
                          .detach();
            return OPENDAQ_SUCCESS;
        });
}

ErrCode MultiReaderStatusBuilderImpl::setReadStatus(ReadStatus readStatus)
{
    this->readStatus = readStatus;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getReadStatus(ReadStatus* readStatus)
{
    OPENDAQ_PARAM_NOT_NULL(readStatus);
    *readStatus = this->readStatus;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::setMainDescriptor(IEventPacket* mainDescriptor)
{
    this->mainDescriptor = mainDescriptor;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getMainDescriptor(IEventPacket** mainDescriptor)
{
    OPENDAQ_PARAM_NOT_NULL(mainDescriptor);
    *mainDescriptor = this->mainDescriptor.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::setEventPackets(IDict* eventPackets)
{
    this->eventPackets = eventPackets;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getEventPackets(IDict** eventPackets)
{
    OPENDAQ_PARAM_NOT_NULL(eventPackets);
    *eventPackets = this->eventPackets.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::setOffset(INumber* offset)
{
    this->offset = offset;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getOffset(INumber** offset)
{
    OPENDAQ_PARAM_NOT_NULL(offset);
    *offset = this->offset.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::setInputStates(IDict* inputStates)
{
    this->inputStates = inputStates;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getInputStates(IDict** inputStates)
{
    OPENDAQ_PARAM_NOT_NULL(inputStates);
    *inputStates = this->inputStates.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::setStateMessage(IString* message)
{
    this->stateMessage = message;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderStatusBuilderImpl::getStateMessage(IString** message)
{
    OPENDAQ_PARAM_NOT_NULL(message);
    *message = this->stateMessage.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE(
    LIBRARY_FACTORY, MultiReaderStatusBuilder, IMultiReaderStatusBuilder
)

END_NAMESPACE_OPENDAQ
