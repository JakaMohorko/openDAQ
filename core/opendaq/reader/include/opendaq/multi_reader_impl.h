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
#include <opendaq/multi_reader.h>
#include <opendaq/multi_reader_status.h>

#include <coreobjects/property_object_ptr.h>
#include <opendaq/multi_reader/input.h>
#include <opendaq/multi_reader_builder_ptr.h>
#include <opendaq/reader_config_ptr.h>

#include <memory>
#include <mutex>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Skeleton of the multi reader: construction (source normalization, port adoption, slot
 * wiring) is functional, every interface method is a blank stub awaiting reimplementation.
 * The per-input building blocks (multi_reader::Input, multi_reader::QueueReader) are functional.
 */
class MultiReaderImpl : public ImplementationOfWeak<IMultiReader, IReaderConfig>, private multi_reader::IInputListener
{
public:
    /// Legacy list factory path; delegates to the builder constructor (the single wiring path).
    MultiReaderImpl(const ListPtr<IComponent>& list,
                    SampleType valueReadType,
                    SampleType domainReadType,
                    ReadMode mode,
                    ReadTimeoutType timeoutType,
                    Int requiredCommonSampleRate = -1,
                    Bool startOnFullUnitOfDomain = false,
                    SizeT minReadCount = 1);

    MultiReaderImpl(const MultiReaderBuilderPtr& builder);

    ~MultiReaderImpl() override;

    // IReader
    ErrCode INTERFACE_FUNC getAvailableCount(SizeT* count) override;
    ErrCode INTERFACE_FUNC setOnDataAvailable(IProcedure* callback) override;
    ErrCode INTERFACE_FUNC setExternalListener(IInputPortNotifications* listener) override;
    ErrCode INTERFACE_FUNC getEmpty(Bool* empty) override;

    // ISampleReader
    ErrCode INTERFACE_FUNC getValueReadType(SampleType* sampleType) override;
    ErrCode INTERFACE_FUNC getDomainReadType(SampleType* sampleType) override;
    ErrCode INTERFACE_FUNC setValueTransformFunction(IFunction* transform) override;
    ErrCode INTERFACE_FUNC setDomainTransformFunction(IFunction* transform) override;
    ErrCode INTERFACE_FUNC getReadMode(ReadMode* mode) override;

    // IMultiReader
    ErrCode INTERFACE_FUNC read(void* samples, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status) override;
    ErrCode INTERFACE_FUNC readWithDomain(void* samples, void* domain, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status) override;
    ErrCode INTERFACE_FUNC skipSamples(SizeT* count, IMultiReaderStatus** status) override;
    ErrCode INTERFACE_FUNC getTickResolution(IRatio** resolution) override;
    ErrCode INTERFACE_FUNC getOrigin(IString** origin) override;
    ErrCode INTERFACE_FUNC getOffset(void* domainStart) override;
    ErrCode INTERFACE_FUNC getIsSynchronized(Bool* isSynchronized) override;
    ErrCode INTERFACE_FUNC getCommonSampleRate(Int* commonSampleRate) override;
    ErrCode INTERFACE_FUNC setActive(Bool isActive) override;
    ErrCode INTERFACE_FUNC getActive(Bool* isActive) override;
    ErrCode INTERFACE_FUNC addInput(IComponent* port) override;
    ErrCode INTERFACE_FUNC removeInput(IString* id) override;
    ErrCode INTERFACE_FUNC setInputUsed(IString* id, Bool isUsed) override;
    ErrCode INTERFACE_FUNC getInputUsed(IString* id, Bool* isUsed) override;
    ErrCode INTERFACE_FUNC setMainInput(IString* id) override;
    ErrCode INTERFACE_FUNC getMainInput(IString** id) override;

    // IReaderConfig
    ErrCode INTERFACE_FUNC getValueTransformFunction(IFunction** transform) override;
    ErrCode INTERFACE_FUNC getDomainTransformFunction(IFunction** transform) override;
    ErrCode INTERFACE_FUNC getInputPorts(IList** ports) override;
    ErrCode INTERFACE_FUNC getReadTimeoutType(ReadTimeoutType* timeoutType) override;
    ErrCode INTERFACE_FUNC markAsInvalid() override;
    ErrCode INTERFACE_FUNC getIsValid(Bool* isValid) override;

    void internalDispose(bool disposing) override;

private:
    enum class InputType
    {
        Unknown,
        Signals,
        Ports,
    };

    // --- IInputListener (semantic port notifications from the slots; blank stubs) ---
    bool slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal) override;
    void slotConnected(SizeT slotIndex) override;
    void slotDisconnected(SizeT slotIndex) override;
    void slotPacketReceived(SizeT slotIndex, bool forceEvaluation) override;

    // --- Construction ---
    /// Validates the source list, caches the context and narrows typeOfInputs exactly once.
    void normalizeSources(const ListPtr<IComponent>& list);
    /// Creates ports for signals (collected into signalsToConnect, connected only after the
    /// slots listen) and adopts user-supplied ports, which must not be connected yet.
    ListPtr<IInputPortConfig> createOrAdoptPorts(const ListPtr<IComponent>& list, std::vector<SignalPtr>& signalsToConnect) const;
    void createSlots(const ListPtr<IInputPortConfig>& inputPorts);

    static constexpr SizeT notFound = multi_reader::slotNotFound;

    // --- State ---
    std::mutex mutex;

    bool invalid{false};

    std::vector<ObjectPtr<IInputPortNotifications>> slotObjects;  // strong refs (ports hold weak listener refs)
    std::vector<multi_reader::Input*> slots;                      // parallel implementation pointers

    /// Shared producer-side gate the slots raise their ready/event flags on (see CallbackGate).
    std::shared_ptr<multi_reader::CallbackGate> callbackGate;

    PropertyObjectPtr portBinder;

    LoggerComponentPtr loggerComponent;
    ContextPtr context;

    // --- Configuration ---
    StringPtr mainInputId;         // explicitly selected main input; null -> first used input
    RatioPtr maxSynchronizationDistance;  // seconds; null/zero disables
    RatioPtr dataLossTimeout;             // seconds; null/zero disables
    std::int64_t requiredCommonSampleRate = -1;
    Bool allowDifferentRates = true;
    bool startOnFullUnitOfDomain = false;
    bool isActive{true};
    SizeT minReadCount = 1;
    PacketReadyNotification notificationMethod{PacketReadyNotification::None};
    ListPtr<PacketReadyNotification> notificationMethodsList;

    SampleType valueReadType{SampleType::Undefined};
    SampleType domainReadType{SampleType::Int64};  // resolved at construction (Undefined -> Int64)
    ReadMode readMode{ReadMode::Scaled};

    InputType typeOfInputs{InputType::Unknown};
};

END_NAMESPACE_OPENDAQ
