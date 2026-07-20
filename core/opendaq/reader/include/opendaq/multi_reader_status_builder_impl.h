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
#include <opendaq/multi_reader_status_builder.h>
#include <opendaq/event_packet_ptr.h>

BEGIN_NAMESPACE_OPENDAQ

class MultiReaderStatusBuilderImpl final : public ImplementationOf<IMultiReaderStatusBuilder>
{
public:
    MultiReaderStatusBuilderImpl();

    ErrCode INTERFACE_FUNC build(IMultiReaderStatus** status) override;

    ErrCode INTERFACE_FUNC setReadStatus(ReadStatus readStatus) override;
    ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* readStatus) override;

    ErrCode INTERFACE_FUNC setMainDescriptor(IEventPacket* mainDescriptor) override;
    ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** mainDescriptor) override;

    ErrCode INTERFACE_FUNC setEventPackets(IDict* eventPackets) override;
    ErrCode INTERFACE_FUNC getEventPackets(IDict** eventPackets) override;

    ErrCode INTERFACE_FUNC setOffset(INumber* offset) override;
    ErrCode INTERFACE_FUNC getOffset(INumber** offset) override;

    ErrCode INTERFACE_FUNC setInputStates(IDict* inputStates) override;
    ErrCode INTERFACE_FUNC getInputStates(IDict** inputStates) override;

    ErrCode INTERFACE_FUNC setStateMessage(IString* message) override;
    ErrCode INTERFACE_FUNC getStateMessage(IString** message) override;

private:
    ReadStatus readStatus;
    EventPacketPtr mainDescriptor;
    DictPtr<IString, IEventPacket> eventPackets;
    NumberPtr offset;
    DictPtr<IString, IInteger> inputStates;
    StringPtr stateMessage;
};

END_NAMESPACE_OPENDAQ
