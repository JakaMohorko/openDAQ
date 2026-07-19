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

#include <memory>
#include <utility>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/// Self-contained per-input state snapshot (input id + InputState as int, in slot order). Held
/// by shared_ptr so the multi reader's status cache and every status it hands out can share the
/// same snapshot - an offset-only restamp of a cached status is then O(1), with no copy.
using InputStateSnapshotPtr = std::shared_ptr<const std::vector<std::pair<StringPtr, Int>>>;

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

    /// Compatibility constructor: the read status is derived from the events and the valid flag.
    explicit MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor, const DictPtr<IString, IEventPacket>& eventPackets, Bool valid, const NumberPtr& offset);

    /// Full constructor (creation goes through MultiReaderStatusBuilder). The validity is
    /// derived from the read status: false only for ReadStatus::Fail - every other condition
    /// is recoverable in the same reader instance.
    explicit MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor,
                                   const DictPtr<IString, IEventPacket>& eventPackets,
                                   const NumberPtr& offset,
                                   ReadStatus readStatus,
                                   const StringPtr& stateMessage,
                                   const DictPtr<IString, IInteger>& inputStates);

    /// Lazy constructor (multi reader read path). Holds a shared self-contained snapshot of the
    /// per-input states (input id + InputState as int, in slot order) captured at read time and
    /// boxes the IDict only on the first getInputStates() call - most reads never inspect it.
    /// The snapshot is shared, not owned; the status never calls back into the reader.
    explicit MultiReaderStatusImpl(const EventPacketPtr& mainDescriptor,
                                   const DictPtr<IString, IEventPacket>& eventPackets,
                                   const NumberPtr& offset,
                                   ReadStatus readStatus,
                                   const StringPtr& stateMessage,
                                   InputStateSnapshotPtr inputStateSnapshot);

    ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* status) override;

    ErrCode INTERFACE_FUNC getEventPackets(IDict** events) override;

    ErrCode INTERFACE_FUNC getEventPacket(IEventPacket** packet) override;

    ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** descriptor) override;

    ErrCode INTERFACE_FUNC getInputStates(IDict** inputStates) override;

    ErrCode INTERFACE_FUNC getStateMessage(IString** message) override;

private:
    DictPtr<IString, IEventPacket> eventPackets;
    ReadStatus readStatus;
    StringPtr stateMessage;
    /// Boxed per-input states. Assigned eagerly by the dict constructor; left null by the lazy
    /// constructor and built on demand from inputStateSnapshot on the first getInputStates().
    DictPtr<IString, IInteger> inputStates;
    /// Lazy source for inputStates (null when a dict was supplied directly).
    InputStateSnapshotPtr inputStateSnapshot;
};

END_NAMESPACE_OPENDAQ
