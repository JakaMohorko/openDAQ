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

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Internal runtime states of the multi reader. The public surface is the extended
 * ReadStatus plus the per-input InputState dictionary; this enum only drives the internal
 * state machine and the diagnostic message.
 *
 * Declared here rather than in multi_reader_impl.h so the state evaluation
 * (multi_reader/state_context.h) can name it without depending on the facade.
 */
enum class ReaderState
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

END_NAMESPACE_OPENDAQ
