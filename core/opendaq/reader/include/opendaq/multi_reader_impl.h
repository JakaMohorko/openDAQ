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

#include <opendaq/input_slot.h>
#include <opendaq/multi_reader_builder_ptr.h>
#include <opendaq/notification_coordinator.h>
#include <opendaq/read_coordinator.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_factory.h>
#include <opendaq/synchronization_manager.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Public facade of the multi reader: configuration, input order, the runtime state
 * machine and status creation (spec section 3). All cross-input math lives in the
 * SynchronizationManager, all queue work in the per-slot QueueReaders, read planning in
 * the ReadCoordinator and callback coalescing in the NotificationCoordinator.
 *
 * Locking (spec section 9): one state mutex; producer threads never take it
 * (InputSlot::packetReceived only touches atomics and schedules the coalesced
 * evaluation); user callbacks are invoked with no lock held.
 */
class MultiReaderImpl : public ImplementationOfWeak<IMultiReader, IReaderConfig, IInputPortNotifications>, private IInputSlotListener
{
public:
    MultiReaderImpl(const ListPtr<IComponent>& list,
                    SampleType valueReadType,
                    SampleType domainReadType,
                    ReadMode mode,
                    ReadTimeoutType timeoutType,
                    Int requiredCommonSampleRate = -1,
                    Bool startOnFullUnitOfDomain = false,
                    SizeT minReadCount = 1);

    // COMMENT: These can likely be removed in future phases.
    MultiReaderImpl(MultiReaderImpl* old, SampleType valueReadType, SampleType domainReadType);

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

    // IInputPortNotifications (compat surface; the per-port listeners are the InputSlots)
    ErrCode INTERFACE_FUNC acceptsSignal(IInputPort* port, ISignal* signal, Bool* accept) override;
    ErrCode INTERFACE_FUNC connected(IInputPort* port) override;
    ErrCode INTERFACE_FUNC disconnected(IInputPort* port) override;
    ErrCode INTERFACE_FUNC packetReceived(IInputPort* inputPort) override;

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

    // --- IInputSlotListener (semantic port notifications from the slots) ---
    bool slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal) override;
    void slotConnected(SizeT slotIndex) override;
    void slotDisconnected(SizeT slotIndex) override;
    void slotPacketReceived(SizeT slotIndex) override;

    // --- Construction ---
    void checkListSizeAndCacheContext(const ListPtr<IComponent>& list);
    InputType sourceComponentsType(const ListPtr<IComponent>& sources) const;
    ListPtr<IInputPortConfig> createOrAdoptPorts(const ListPtr<IComponent>& list) const;
    void createSlots(const ListPtr<IInputPortConfig>& inputPorts);
    void applyConfigToSyncManager();

    // --- State machine (spec section 6.2; state mutex held) ---
    void evaluateStateLocked();
    void invalidateSynchronizationLocked();
    void invalidateModelLocked();
    void setStateLocked(MultiReaderState newState, std::string message = {}, std::vector<SizeT> affected = {});
    /// Formats "<messagePrefix> [i, j, ...]<messageSuffix>" from the affected indices before
    /// moving them into the state - never both format and move in one argument list (the
    /// evaluation order of function arguments is unspecified).
    void setStateWithAffectedLocked(MultiReaderState newState,
                                    const char* messagePrefix,
                                    const char* messageSuffix,
                                    std::vector<SizeT> affected);

    /// Used inputs in slot order plus their slot indices; main input is the first used slot.
    std::vector<QueueReader*> collectUsedReaders(std::vector<SizeT>& slotIndices) const;

    /// Scheduler-side entry of the coalesced evaluation (never called with locks held).
    void onCoalescedEvaluation();

    // --- Read path ---
    ErrCode readInternal(void** valueBuffers, void** domainBuffers, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status, bool skip);
    MultiReaderStatusPtr readEventsLocked();
    MultiReaderStatusPtr createStatusLocked(const DictPtr<IString, IEventPacket>& eventPackets = nullptr,
                                            const NumberPtr& offset = nullptr,
                                            const ListPtr<IInteger>& eventInputIndices = nullptr,
                                            const ListPtr<IEventPacket>& orderedEventPackets = nullptr);
    /// Descriptor-changed packet for the status: main value descriptor + common output domain
    /// descriptor (the domain of the status offset, spec section 8.2)
    EventPacketPtr mainDescriptorPacketLocked();
    void updateMainDescriptorsLocked();
    std::optional<std::int64_t> currentReadOffsetLocked() const;

    SizeT findSlotByIdLocked(const StringPtr& id) const;  // returns slots.size() when not found
    void reindexSlotsLocked();
    void setPortsActiveLocked(bool active);

    static constexpr SizeT notFound = static_cast<SizeT>(-1);

    // --- State ---
    std::mutex mutex;
    std::condition_variable notifyCondition;

    bool invalid{false};  // only the Error state and disposal (spec section 6.1)
    MultiReaderState state{MultiReaderState::WaitingForConnections};
    std::string stateMessage;
    std::vector<SizeT> stateAffectedInputs;

    std::vector<ObjectPtr<IInputPortNotifications>> slotObjects;  // strong refs (ports hold weak listener refs)
    std::vector<InputSlot*> slots;                                // parallel implementation pointers

    std::unique_ptr<SynchronizationManager> syncManager;
    std::unique_ptr<ReadCoordinator> readCoordinator;
    std::unique_ptr<NotificationCoordinator> notificationCoordinator;

    /// Common-domain tick of the next unread output sample while synchronized (spec section 7.4)
    std::optional<std::int64_t> nextReadTick;

    DataDescriptorPtr mainValueDescriptor;
    DataDescriptorPtr mainDomainDescriptor;

    /// Status caching (spec section 8.2): the last event-less status is re-issued while its
    /// visible content is unchanged; any content change (or any event) creates a new object.
    /// The cache and the fingerprint are cleared whenever the cross-input model is
    /// invalidated - the cached getMainDescriptor packet embeds the common output domain,
    /// which any input's descriptor change can move.
    MultiReaderStatusPtr cachedStatus;
    struct StatusFingerprint
    {
        MultiReaderState state{};
        std::string message;
        std::vector<SizeT> affectedInputs;
        std::int64_t offset{};
        // Identity only; safe because the cache is dropped on every model invalidation,
        // which every descriptor change triggers before a new descriptor can be adopted
        IDataDescriptor* mainValue{};
        IDataDescriptor* mainDomain{};

        bool operator==(const StatusFingerprint& other) const
        {
            return state == other.state && message == other.message && affectedInputs == other.affectedInputs &&
                   offset == other.offset && mainValue == other.mainValue && mainDomain == other.mainDomain;
        }
    };
    StatusFingerprint cachedStatusFingerprint;

    /// Common-output-domain descriptor for getMainDescriptor, built lazily per model build
    DataDescriptorPtr cachedCommonDomainDescriptor;

    PropertyObjectPtr portBinder;
    ProcedurePtr readCallback;
    WeakRefPtr<IInputPortNotifications> externalListener;

    LoggerComponentPtr loggerComponent;
    ContextPtr context;

    // --- Configuration ---
    RatioPtr tickOffsetTolerance;  // deprecated; value ignored (spec section 8.4)
    std::int64_t requiredCommonSampleRate = -1;
    Bool allowDifferentRates = true;
    bool startOnFullUnitOfDomain = false;
    bool isActive{true};
    SizeT minReadCount = 1;
    PacketReadyNotification notificationMethod{PacketReadyNotification::None};
    ListPtr<PacketReadyNotification> notificationMethodsList;

    SampleType valueReadType{SampleType::Undefined};
    SampleType domainReadType{SampleType::Undefined};   // as configured
    SampleType resolvedDomainReadType{SampleType::Int64};  // integral type driving the QueueReaders
    ReadMode readMode{ReadMode::Scaled};

    InputType typeOfInputs{InputType::Unknown};
};

END_NAMESPACE_OPENDAQ
