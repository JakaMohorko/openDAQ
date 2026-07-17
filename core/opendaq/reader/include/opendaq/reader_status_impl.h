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
#include <opendaq/reader_status.h>
#include <opendaq/block_reader_status.h>
#include <opendaq/tail_reader_status.h>
#include <opendaq/multi_reader_status.h>
#include <opendaq/event_packet_ptr.h>

BEGIN_NAMESPACE_OPENDAQ

template <class MainInterface, class ... Interfaces>
class GenericReaderStatusImpl : public ImplementationOf<MainInterface, Interfaces...>
{
public:
    explicit GenericReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset);

    virtual ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* status) override;

    ErrCode INTERFACE_FUNC getEventPacket(IEventPacket** packet) override;

    ErrCode INTERFACE_FUNC getValid(Bool* valid) override;

    ErrCode INTERFACE_FUNC getOffset(INumber** offset) override;

private:
    EventPacketPtr eventPacket;
    Bool valid;
    NumberPtr offset;
};

using ReaderStatusImpl = GenericReaderStatusImpl<IReaderStatus>;

class BlockReaderStatusImpl final : public GenericReaderStatusImpl<IBlockReaderStatus>
{
public:
    using Super = GenericReaderStatusImpl<IBlockReaderStatus>;
    explicit BlockReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset, SizeT readSamples);

    ErrCode INTERFACE_FUNC getReadSamples(SizeT* readSamples) override;

private:
    SizeT readSamples;
};

class TailReaderStatusImpl final : public GenericReaderStatusImpl<ITailReaderStatus>
{
public:
    using Super = GenericReaderStatusImpl<ITailReaderStatus>;
    explicit TailReaderStatusImpl(const EventPacketPtr& eventPacket, Bool valid, const NumberPtr& offset, Bool sufficientHistory);

    ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* status) override;

    ErrCode INTERFACE_FUNC getSufficientHistory(Bool* status) override;
private:
    Bool sufficientHistory;
};

class MultiReaderStatusImpl final : public GenericReaderStatusImpl<IMultiReaderStatus>
{
public:
    using Super = GenericReaderStatusImpl<IMultiReaderStatus>;

    /// Compatibility constructor: the state is derived from the events and the valid flag.
    explicit MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor, const DictPtr<IString, IEventPacket>& eventPackets, Bool valid, const NumberPtr& offset);

    /// Full constructor carrying the reader state, diagnostics and the ordered event list
    /// (eventInputIndices and orderedEventPackets are parallel). The validity is derived
    /// from the state: Incompatible, SynchronizationFailed and Error report invalid.
    explicit MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor,
                                   const DictPtr<IString, IEventPacket>& eventPackets,
                                   const NumberPtr& offset,
                                   MultiReaderState state,
                                   const StringPtr& stateMessage,
                                   const ListPtr<IInteger>& affectedInputIndices,
                                   const ListPtr<IInteger>& eventInputIndices,
                                   const ListPtr<IEventPacket>& orderedEventPackets);

    ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* status) override;

    ErrCode INTERFACE_FUNC getEventPackets(IDict** events) override;

    ErrCode INTERFACE_FUNC getEventPacket(IEventPacket** packet) override;

    ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** descriptor) override;

    ErrCode INTERFACE_FUNC getState(MultiReaderState* state) override;

    ErrCode INTERFACE_FUNC getStateMessage(IString** message) override;

    ErrCode INTERFACE_FUNC getAffectedInputCount(SizeT* count) override;

    ErrCode INTERFACE_FUNC getAffectedInputIndex(SizeT statusIndex, SizeT* inputIndex) override;

    ErrCode INTERFACE_FUNC getEventCount(SizeT* count) override;

    ErrCode INTERFACE_FUNC getEvent(SizeT eventIndex, SizeT* inputIndex, IEventPacket** packet) override;

private:
    DictPtr<IString, IEventPacket> eventPackets;
    MultiReaderState state;
    StringPtr stateMessage;
    ListPtr<IInteger> affectedInputIndices;
    ListPtr<IInteger> eventInputIndices;
    ListPtr<IEventPacket> orderedEventPackets;
};

END_NAMESPACE_OPENDAQ
