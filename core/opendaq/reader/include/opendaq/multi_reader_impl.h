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

#include <opendaq/multi_reader/data_loss_monitor.h>
#include <opendaq/multi_reader/input.h>
#include <opendaq/multi_reader_builder_ptr.h>
#include <opendaq/multi_reader/notification_coordinator.h>
#include <opendaq/multi_reader/read_coordinator.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_factory.h>
#include <opendaq/multi_reader/synchronization_manager.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Internal runtime states of the multi reader. The public surface is the extended
 * ReadStatus plus the per-input InputState dictionary (review decision C5/Q1); this enum
 * only drives the internal state machine and the diagnostic message.
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

/**
 * @brief Public facade of the multi reader: configuration, input order, the runtime state
 * machine and status creation (spec section 3). All cross-input math lives in the
 * SynchronizationManager, all queue work in the per-slot QueueReaders, read planning in
 * the ReadCoordinator and callback coalescing in the NotificationCoordinator.
 *
 * Locking (spec section 9): one state mutex; producer threads never take it
 * (Input::packetReceived only touches atomics and schedules the coalesced
 * evaluation); user callbacks are invoked with no lock held.
 *
 * Naming convention: a "...Locked" suffix means "the caller must already hold `mutex`" -
 * such a method never takes the lock itself and must only be called from code that does.
 * Methods without the suffix acquire the lock themselves (or need none).
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

    /// Deprecated MultiReaderFromExisting path; scheduled for removal (Phase 6.1) and
    /// deliberately not migrated to the builder constructor.
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
    ErrCode INTERFACE_FUNC setMainInput(IString* id) override;
    ErrCode INTERFACE_FUNC getMainInput(IString** id) override;

    /// Test hook (test scaffolding section 2.7): replaces the data-loss time source so
    /// deadline tests run on virtual time with zero real sleeps. Inline so tests can call
    /// it without the implementation being exported from the library.
    void setDataLossClockForTest(multi_reader::DataLossMonitor::Clock clock)
    {
        dataLossMonitor->setClockForTest(std::move(clock));
    }

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

    // --- IInputListener (semantic port notifications from the slots) ---
    // Why this second listener surface exists: a port can have exactly one listener, and that
    // listener is the slot (it owns the port's QueueReader pairing). This private interface is
    // the slot's channel back up to the facade - it carries the slot index, keeps the producer
    // path bounded (slotPacketReceived touches atomics and schedules the coalesced evaluation,
    // taking no facade lock), and serializes external-listener forwarding so user callbacks
    // never run under the state mutex. The facade itself is deliberately NOT an
    // IInputPortNotifications: ports never see the reader directly.
    bool slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal) override;
    void slotConnected(SizeT slotIndex) override;
    void slotDisconnected(SizeT slotIndex) override;
    void slotPacketReceived(SizeT slotIndex) override;

    // --- Construction ---
    /// Source normalization (construction and addInput): validates the list (assigned,
    /// non-empty), caches the context from the first source and narrows typeOfInputs from
    /// Unknown exactly once. Homogeneity is enforced per element by createOrAdoptPorts.
    void normalizeSources(const ListPtr<IComponent>& list);
    ListPtr<IInputPortConfig> createOrAdoptPorts(const ListPtr<IComponent>& list) const;
    void createSlots(const ListPtr<IInputPortConfig>& inputPorts);
    void applyConfigToSyncManager();

    // --- State machine (spec section 6.2; state mutex held) ---
    void evaluateStateLocked();
    void invalidateSynchronizationLocked();
    void invalidateModelLocked();
    void setStateLocked(ReaderState newState, std::string message = {}, std::vector<SizeT> affected = {});
    /// Formats "<messagePrefix> [i, j, ...]<messageSuffix>" from the affected indices before
    /// moving them into the state - never both format and move in one argument list (the
    /// evaluation order of function arguments is unspecified).
    void setStateWithAffectedLocked(ReaderState newState,
                                    const char* messagePrefix,
                                    const char* messageSuffix,
                                    std::vector<SizeT> affected);

    /// Used inputs in slot order plus their slot indices; main input is the first used slot.
    std::vector<multi_reader::QueueReader*> collectUsedReaders(std::vector<SizeT>& slotIndices) const;

    /// Scheduler-side entry of the coalesced evaluation (never called with locks held).
    void onCoalescedEvaluation();

    // --- Read path ---
    ErrCode readInternal(void** valueBuffers, void** domainBuffers, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status, bool skip);
    MultiReaderStatusPtr readEventsLocked();
    MultiReaderStatusPtr createStatusLocked(const DictPtr<IString, IEventPacket>& eventPackets = nullptr,
                                            const NumberPtr& offset = nullptr);
    /// Per-input states for the status (C6): keyed by input id, derived from the used/connected
    /// flags, pending events and the current failure state's affected set. Optionally also fills
    /// the fingerprint snapshot (same content, comparable cheaply).
    DictPtr<IString, IInteger> inputStatesLocked(std::vector<std::pair<std::string, int>>* fingerprint = nullptr) const;
    /// Descriptor-changed packet for the status: main value descriptor + common output domain
    /// descriptor (the domain of the status offset, spec section 8.2)
    EventPacketPtr mainDescriptorPacketLocked();
    void refreshMainInputDescriptorsLocked();
    std::optional<std::int64_t> currentReadOffsetLocked() const;

    SizeT findSlotByIdLocked(const StringPtr& id) const;  // returns slots.size() when not found
    void reindexSlotsLocked();
    void setPortsActiveLocked(bool active);

    /// Slot index of the explicitly selected main input; notFound when the default
    /// (first used input) applies or the selection is dangling.
    SizeT mainSlotIndexLocked() const;
    void applyDataLossTimeoutLocked();

    static constexpr SizeT notFound = static_cast<SizeT>(-1);

    // --- State ---
    std::mutex mutex;
    std::condition_variable notifyCondition;

    bool invalid{false};  // only the Error state and disposal (spec section 6.1)
    ReaderState state{ReaderState::WaitingForConnections};
    std::string stateMessage;
    std::vector<SizeT> stateAffectedInputs;

    std::vector<ObjectPtr<IInputPortNotifications>> slotObjects;  // strong refs (ports hold weak listener refs)
    std::vector<multi_reader::Input*> slots;                                // parallel implementation pointers

    std::unique_ptr<multi_reader::SynchronizationManager> syncManager;
    std::unique_ptr<multi_reader::ReadCoordinator> readCoordinator;

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
        ReaderState state{};
        std::string message;
        std::vector<SizeT> affectedInputs;
        /// Snapshot of the per-input states (input id, InputState as int) in slot order -
        /// they can move without a state change (e.g. an unused input gaining events)
        std::vector<std::pair<std::string, int>> inputStates;
        std::int64_t offset{};
        // Identity only; safe because the cache is dropped on every model invalidation,
        // which every descriptor change triggers before a new descriptor can be adopted
        IDataDescriptor* mainValue{};
        IDataDescriptor* mainDomain{};

        bool operator==(const StatusFingerprint& other) const
        {
            return state == other.state && message == other.message && affectedInputs == other.affectedInputs &&
                   inputStates == other.inputStates && offset == other.offset && mainValue == other.mainValue &&
                   mainDomain == other.mainDomain;
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
    SampleType domainReadType{SampleType::Undefined};   // as configured
    SampleType resolvedDomainReadType{SampleType::Int64};  // integral type driving the QueueReaders
    ReadMode readMode{ReadMode::Scaled};

    InputType typeOfInputs{InputType::Unknown};

    // Declared last on purpose: members destroy in reverse declaration order, so the
    // monitor (whose waiter thread can trigger an evaluation) and the notification
    // coordinator (whose queued task can do the same) are torn down before any member
    // their callbacks touch - including on the constructor-throw unwinding path where
    // ~MultiReaderImpl never runs
    std::unique_ptr<multi_reader::NotificationCoordinator> notificationCoordinator;
    std::unique_ptr<multi_reader::DataLossMonitor> dataLossMonitor;
};

END_NAMESPACE_OPENDAQ
