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
#include <coretypes/common.h>
#include <coretypes/baseobject.h>
#include <opendaq/multi_reader_status.h>

BEGIN_NAMESPACE_OPENDAQ

/*!
 * @ingroup opendaq_readers
 * @addtogroup opendaq_reader Multi reader status builder
 * @{
 */

/*!
 * @brief Builder of IMultiReaderStatus objects. The reader creates every status through this
 * builder; unset fields keep their defaults (Ok read status, empty dictionaries, empty message).
 * The status validity is derived from the read status (false only for ReadStatus::Fail), so the
 * builder takes no valid flag.
 */
DECLARE_OPENDAQ_INTERFACE(IMultiReaderStatusBuilder, IBaseObject)
{
    /*!
     * @brief Builds and returns a multi reader status using the currently set values.
     * @param[out] status The built status.
     */
    virtual ErrCode INTERFACE_FUNC build(IMultiReaderStatus** status) = 0;

    // [returnSelf]
    /*!
     * @brief Sets the read status reported by IReaderStatus::getReadStatus.
     * @param readStatus The read status.
     */
    virtual ErrCode INTERFACE_FUNC setReadStatus(ReadStatus readStatus) = 0;

    /*!
     * @brief Gets the read status.
     * @param[out] readStatus The read status.
     */
    virtual ErrCode INTERFACE_FUNC getReadStatus(ReadStatus* readStatus) = 0;

    // [returnSelf]
    /*!
     * @brief Sets the combined main-input descriptor-changed event packet.
     * @param mainDescriptor The descriptor-changed event packet.
     */
    virtual ErrCode INTERFACE_FUNC setMainDescriptor(IEventPacket* mainDescriptor) = 0;

    /*!
     * @brief Gets the combined main-input descriptor-changed event packet.
     * @param[out] mainDescriptor The descriptor-changed event packet.
     */
    virtual ErrCode INTERFACE_FUNC getMainDescriptor(IEventPacket** mainDescriptor) = 0;

    // [elementType(eventPackets, IString, IEventPacket), returnSelf]
    /*!
     * @brief Sets the dictionary of returned event packets keyed by input id.
     * @param eventPackets The event packet dictionary.
     */
    virtual ErrCode INTERFACE_FUNC setEventPackets(IDict* eventPackets) = 0;

    // [elementType(eventPackets, IString, IEventPacket)]
    /*!
     * @brief Gets the dictionary of returned event packets keyed by input id.
     * @param[out] eventPackets The event packet dictionary.
     */
    virtual ErrCode INTERFACE_FUNC getEventPackets(IDict** eventPackets) = 0;

    // [returnSelf]
    /*!
     * @brief Sets the offset of the read values.
     * @param offset The offset.
     */
    virtual ErrCode INTERFACE_FUNC setOffset(INumber* offset) = 0;

    /*!
     * @brief Gets the offset of the read values.
     * @param[out] offset The offset.
     */
    virtual ErrCode INTERFACE_FUNC getOffset(INumber** offset) = 0;

    // [elementType(inputStates, IString, IInteger), returnSelf]
    /*!
     * @brief Sets the per-input states dictionary (input id to InputState as integer).
     * @param inputStates The per-input states.
     */
    virtual ErrCode INTERFACE_FUNC setInputStates(IDict* inputStates) = 0;

    // [elementType(inputStates, IString, IInteger)]
    /*!
     * @brief Gets the per-input states dictionary.
     * @param[out] inputStates The per-input states.
     */
    virtual ErrCode INTERFACE_FUNC getInputStates(IDict** inputStates) = 0;

    // [returnSelf]
    /*!
     * @brief Sets the human-readable diagnostic message.
     * @param message The diagnostic message.
     */
    virtual ErrCode INTERFACE_FUNC setStateMessage(IString* message) = 0;

    /*!
     * @brief Gets the human-readable diagnostic message.
     * @param[out] message The diagnostic message.
     */
    virtual ErrCode INTERFACE_FUNC getStateMessage(IString** message) = 0;
};
/*!@}*/

OPENDAQ_DECLARE_CLASS_FACTORY_WITH_INTERFACE(
    LIBRARY_FACTORY, MultiReaderStatusBuilder, IMultiReaderStatusBuilder
)

END_NAMESPACE_OPENDAQ
