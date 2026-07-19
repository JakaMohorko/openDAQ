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
 * @brief Per-input condition of one multi reader input, keyed by the input id in
 * IMultiReaderStatus::getInputStates. The reader-level ReadStatus summarizes these:
 * it reports InputsFailed exactly when at least one used input is in Incompatible,
 * SynchronizationFailed or DataLost.
 *
 * The actionable reactions are per input: a failing input can be excluded via
 * setInputUsed(id, false), fixed upstream (reconnect/descriptor change), or - for
 * Unused inputs reporting Event - re-included via setInputUsed(id, true).
 */
enum class InputState : EnumType
{
    Ok = 0,                 ///< Contributing aligned samples
    Pending,                ///< Connected but not contributing yet (connect/descriptors/first data/alignment in progress)
    Event,                  ///< Unconsumed event(s) on this input, used or unused alike
    Incompatible,           ///< Descriptor or cross-input validation failed (recoverable on new descriptors)
    SynchronizationFailed,  ///< Synchronization distance or common-tick failure
    DataLost,               ///< Missed its packet deadline
    Unused                  ///< Excluded from reading via setInputUsed(id, false)
};

/*#
 * [interfaceSmartPtr(IReaderStatus, GenericReaderStatusPtr)]
 */

/*!
 * @brief IMultiReaderStatus inherits from IReaderStatus to expand information returned read function
 *
 * The read status (getReadStatus) and the per-input states (getInputStates) are the machine
 * surface - consumers switch on them and never need to parse text. getStateMessage is the
 * human surface: a diagnostic for logs and UIs, never required for a correct reaction.
 * getValid is false only for ReadStatus::Fail - every other condition is recoverable in the
 * same reader instance.
 */
DECLARE_OPENDAQ_INTERFACE(IMultiReaderStatus, IReaderStatus)
{
    // [elementType(eventPackets, IString, IEventPacket)]
    /*!
     * @brief Retrieves the dictionary of event packets from the reading process, ordered by signals.
     * @param[out] eventPackets The dictionary with the input id and the corresponding event packet.
     */
    virtual ErrCode INTERFACE_FUNC getEventPackets(IDict** eventPackets) = 0;

    /*!
     * @brief Retrieves the combined descriptor-changed event packet carrying the value descriptor of
     * the main input and the common output domain descriptor (the domain in which the status offset
     * is expressed).
     * @param[out] descriptor The descriptor-changed event packet of the main input.
     */
    virtual ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** descriptor) = 0;

    // [elementType(inputStates, IString, IInteger)]
    /*!
     * @brief Retrieves the per-input states, keyed by input id (the same id addInput/removeInput/
     * setInputUsed use: the signal's global id when the reader was constructed from signals,
     * otherwise the port's global id). Values are InputState enumeration values.
     * @param[out] inputStates Dictionary of input id to InputState (as integer).
     */
    virtual ErrCode INTERFACE_FUNC getInputStates(IDict** inputStates) = 0;

    /*!
     * @brief Retrieves the human-readable diagnostic message describing the reader condition,
     * naming the affected inputs and the details needed to understand it. Never required for a
     * correct programmatic reaction - consumers react to getReadStatus and getInputStates.
     * @param[out] message The diagnostic message; empty when there is nothing to report.
     */
    virtual ErrCode INTERFACE_FUNC getStateMessage(IString** message) = 0;
};
/*!@}*/

OPENDAQ_DECLARE_CLASS_FACTORY (
    LIBRARY_FACTORY, MultiReaderStatus,
    IEventPacket*, mainDescriptor,
    IDict*, eventPackets,
    Bool, valid,
    INumber*, offset
)

END_NAMESPACE_OPENDAQ
