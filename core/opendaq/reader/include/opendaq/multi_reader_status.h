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
#include <coretypes/listobject.h>
#include <opendaq/reader_status.h>
#include <opendaq/signal.h>

BEGIN_NAMESPACE_OPENDAQ

/*!
 * @ingroup opendaq_readers
 * @addtogroup opendaq_reader Multi reader status
 * @{
 */

/*!
 * @brief Runtime states of the multi reader. Every state except Error is recoverable in the
 * same reader instance; the sensible recovery actions follow from the state, the affected
 * inputs and the state message.
 *
 * // COMMENT: There's too many statuses here. This is user-facing API, they shouldn't need this information to react 
 * //          to read outcomes. The state here should provide information that the FB or application can react to.
 * //          We don't really want each FB or application to have a giant switch for the state. The only actions an app/fb
 * //          can take are: setActive(false) -> stop reading or setUnused(port) -> read only a subset of inputs.
 * //          Compact the states into app/fb relevant ones. Move the rest to the string message output.
 */
enum class MultiReaderState : EnumType
{
    Inactive = 0,           ///< Disabled via setActive(false)
    WaitingForConnections,  ///< A used input has no signal connected
    WaitingForDescriptors,  ///< A used input has not received its descriptors yet
    Incompatible,           ///< Local or cross-input validation failed (recoverable)
    WaitingForData,         ///< Valid, but some input has no samples
    Synchronizing,          ///< Alignment in progress, waiting for data to reach the aligned start
    Synchronized,           ///< Aligned blocks readable
    EventPending,           ///< Event(s) must be returned before data
    SynchronizationFailed,  ///< Span, representability or common-tick failure
    DataLost,               ///< A used input missed its packet deadline
    Error                   ///< Internal invariant violated or reader disposed; not recoverable
};

/*#
 * [interfaceSmartPtr(IReaderStatus, GenericReaderStatusPtr)]
 */

/*!
 * @brief IMultiReaderStatus inherits from IReaderStatus to expand information returned read function
 */
DECLARE_OPENDAQ_INTERFACE(IMultiReaderStatus, IReaderStatus)
{
    // [elementType(eventPackets, IString, IEventPacket)]
    /*!
     * @brief Retrieves the dictionary of event packets from the reading process, ordered by signals.
     * @param[out] eventPackets The dictionary with global id of input port and the corresponding event packet.
     */
    virtual ErrCode INTERFACE_FUNC getEventPackets(IDict** eventPackets) = 0;

    /*!
     * @brief Retrieves the combined descriptor-changed event packet carrying the value descriptor of
     * the main input and the common output domain descriptor (the domain in which the status offset
     * is expressed).
     * @param[out] descriptor The descriptor-changed event packet of the main input.
     */
    virtual ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** descriptor) = 0;

    /*!
     * @brief Retrieves the runtime state of the reader at the time the status was created.
     * @param[out] state The reader state.
     */
    virtual ErrCode INTERFACE_FUNC getState(MultiReaderState* state) = 0;

    /*!
     * @brief Retrieves the human-readable diagnostic message describing the state, naming the
     * affected inputs and the details needed to act on the condition.
     * @param[out] message The diagnostic message; empty when there is nothing to report.
     */
    virtual ErrCode INTERFACE_FUNC getStateMessage(IString** message) = 0;
   
    /*!
     * // COMMENT: The bottom few API methods don't make sense. All events are already obtained through `getEventPackets`.
     * //          We might rework that part of the API later, but not now. What we actually need is per-input statuses.
     * //          When the reader is in an error state, the multi reader owner should be able to know which input ports
     * //          failed to sync/have incompatible descriptors, have data loss... They should be able to set those as 
     * //          unused and continue operation. This should be an addition to the sum reader fb - it should have a property
     * //          where the user can choose that the multi reader always works and ignores faulty inputs. It should as a status
     * //          report which ones are failing.
     * 
     * @brief Retrieves the number of inputs affected by the reported condition.
     * @param[out] count The number of affected inputs.
     */
    virtual ErrCode INTERFACE_FUNC getAffectedInputCount(SizeT* count) = 0;

    /*!
     * @brief Retrieves the construction-order index of one affected input.
     * @param statusIndex Position within the affected-input list (0 to getAffectedInputCount() - 1).
     * @param[out] inputIndex The construction-order index of the affected input.
     */
    virtual ErrCode INTERFACE_FUNC getAffectedInputIndex(SizeT statusIndex, SizeT* inputIndex) = 0;

    /*!
     * @brief Retrieves the number of events in the ordered event list. The reader populates
     * the list on every event-carrying status; statuses built through the compatibility
     * factory (which cannot know input indices) carry only the event-packet dictionary and
     * report zero here.
     * @param[out] count The number of events.
     */
    virtual ErrCode INTERFACE_FUNC getEventCount(SizeT* count) = 0;

    /*!
     * @brief Retrieves one event of the ordered event list together with the construction-order
     * index of the input it originates from. Events are ordered as they are returned: per input
     * in queue order, one event per input per read call.
     * @param eventIndex Position within the event list (0 to getEventCount() - 1).
     * @param[out] inputIndex The construction-order index of the originating input.
     * @param[out] packet The event packet.
     */
    virtual ErrCode INTERFACE_FUNC getEvent(SizeT eventIndex, SizeT* inputIndex, IEventPacket** packet) = 0;
};
/*!@}*/

OPENDAQ_DECLARE_CLASS_FACTORY (
    LIBRARY_FACTORY, MultiReaderStatus,
    IEventPacket*, mainDescriptor,
    IDict*, eventPackets,
    Bool, valid,
    INumber*, offset
)

// COMMENT: If we have such a large factory, we should instead introduce a builder object. We should, however,
//          consider if we need all of these fields. 
//
// The status validity is derived from the state (Incompatible, SynchronizationFailed and
// Error are the invalid-stream conditions), so the extended factory does not take a valid flag.
// [elementType(affectedInputIndices, IInteger)]
// [elementType(eventInputIndices, IInteger)]
// [elementType(orderedEventPackets, IEventPacket)]
OPENDAQ_DECLARE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC(
    LIBRARY_FACTORY, MultiReaderStatusEx, IMultiReaderStatus, createMultiReaderStatusEx,
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
