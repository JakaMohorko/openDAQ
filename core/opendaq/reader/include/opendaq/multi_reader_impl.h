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
#include <opendaq/multi_reader/reader_state.h>
#include <opendaq/multi_reader/state_machine.h>
#include <opendaq/reader_config_ptr.h>
#include <opendaq/reader_factory.h>
#include <opendaq/reader_status_impl.h>
#include <opendaq/multi_reader/synchronization_manager.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

BEGIN_NAMESPACE_OPENDAQ

/**
 * @brief Public facade of the multi reader: configuration, input order, the runtime state
 * machine and status creation. All cross-input math lives in the
 * SynchronizationManager, all queue work in the per-slot QueueReaders, read planning in
 * the ReadCoordinator and callback coalescing in the NotificationCoordinator.
 *
 * Locking: one state mutex; producer threads never take it - and take no other mutex either:
 * Input::packetReceived touches atomics plus two O(1) connection counters, and an evaluation
 * task is scheduled only once the callback gate is open (or the snapshot cannot be trusted).
 * User callbacks are invoked with no lock held.
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

    /// Deprecated MultiReaderFromExisting path; scheduled for removal and
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

    /// Test hook: replaces the data-loss time source so deadline tests run on virtual time
    /// with zero real sleeps. Inline so tests can call
    /// it without the implementation being exported from the library.
    void setDataLossClockForTest(multi_reader::DataLossMonitor::Clock clock)
    {
        dataLossMonitor->setClockForTest(std::move(clock));
    }

    struct StateSnapshotForTest
    {
        ReaderState state;
        std::string message;
        std::vector<SizeT> affectedInputs;
    };

    /// Test hook: the internal state-machine verdict. The public ReadStatus is a lossy projection
    /// of it (four substates collapse into Preparing alone), so the state-machine characterization
    /// tests assert on this instead - they exist to prove a refactor computes the same substate for
    /// the same observable inputs, which the projection cannot show. Inline for the same reason as
    /// setDataLossClockForTest: the implementation is not exported from the library.
    StateSnapshotForTest getStateForTest()
    {
        std::lock_guard lock(mutex);
        return {state, stateMessage, stateAffectedInputs};
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
    // path bounded (slotPacketReceived touches atomics and schedules the coalesced evaluation
    // only when the callback gate is open, taking no facade lock and no other mutex), and
    // serializes external-listener forwarding so user callbacks never run under the state
    // mutex. The facade itself is deliberately NOT an IInputPortNotifications: ports never see
    // the reader directly.
    bool slotAcceptsSignal(SizeT slotIndex, const SignalPtr& signal) override;
    void slotConnected(SizeT slotIndex) override;
    void slotDisconnected(SizeT slotIndex) override;
    void slotPacketReceived(SizeT slotIndex, bool forceEvaluation) override;

    // --- Construction ---
    /// Source normalization (construction and addInput): validates the list (assigned,
    /// non-empty), caches the context from the first source and narrows typeOfInputs from
    /// Unknown exactly once. Homogeneity is enforced per element by createOrAdoptPorts.
    void normalizeSources(const ListPtr<IComponent>& list);
    ListPtr<IInputPortConfig> createOrAdoptPorts(const ListPtr<IComponent>& list) const;
    void createSlots(const ListPtr<IInputPortConfig>& inputPorts);

    /**
     * @brief Completes slot setup by replaying the port callbacks that were never delivered for an
     * already-connected port (Input::replayMissedPortCallbacks). createSlots already installed the
     * listeners; this is the part that must happen afterwards and WITHOUT the state lock, because
     * the replayed callbacks come back in through slotConnected/slotPacketReceived, which take it.
     * @param firstSlot index of the first slot to replay; addInput replays only the new ones.
     */
    void replaySlotCallbacks(SizeT firstSlot = 0);
    void applyConfigToSyncManager();

    // --- State machine (state mutex held) ---
    /// Full state evaluation - the transition handler run by the paths that change state
    /// (connect/disconnect, used/active changes, topology, events, deadlines). Builds the
    /// StateContext, runs the state machine (multi_reader::runStateEvaluation), applies its verdict
    /// and its deferred facade effects, then publishes the producer-facing gate state
    /// (publishProducerGateLocked).
    void evaluateStateLocked();
    /// The collaborator bundle the evaluation works on; a view, rebuilt per evaluation.
    multi_reader::StateContext makeStateContextLocked();
    /// Apply what the evaluation decided: its deferred facade effects first (in the order the
    /// evaluation would have run them inline), then the substate through setStateLocked.
    void applyStateOutcomeLocked(multi_reader::StateContext& ctx);
    /**
     * @brief Publish the producer-facing callback-gate state after a full evaluation: per-slot
     * basis (adopted availability-until-event + adopted events), the ready threshold, the
     * wake-on-any-packet mode, and - while synchronized - the ground-truth ready/event flags.
     * Outside the steady Synchronized state every packet forces an evaluation, so only the
     * flags the ladder maintains matter there.
     */
    void publishProducerGateLocked();
    /// Data-plane pass for the read and query paths: while synchronized, drains the slots that
    /// received packets, publishes the availability cache, and maintains the readiness bits. With
    /// escalateOnEvent (the read path) it escalates to evaluateStateLocked when an event surfaces so
    /// the reader transitions to EventPending; without it (the query path) it records the event and
    /// re-arms dataPlaneDirty so the next read surfaces it. A deadline or a non-synchronized state
    /// escalates in either mode.
    void refreshDataPlaneLocked(bool escalateOnEvent);
    /// Callback pass for the coalesced evaluation: decides only whether onDataAvailable should fire.
    /// It maintains the event/ready bits but skips any slot that already satisfies the gate (ready
    /// or event - only a read clears that) and any slot with no pending packet, so it is O(slots
    /// that changed) rather than O(all used slots). It does not touch the availability cache or
    /// dataPlaneDirty (the read/query path owns those); a deadline or a non-synchronized state still
    /// escalates to the full evaluation.
    void updateCallbackStateLocked();
    void invalidateSynchronizationLocked();
    void invalidateModelLocked();
    /// The caches derived from the cross-input model - the status cache and the common-output-domain
    /// descriptor. Separate from invalidateModelLocked because the state evaluation invalidates the
    /// model without being able to reach these (StateContext::modelInvalidated).
    void clearModelDerivedCachesLocked();
    void setStateLocked(ReaderState newState, std::string message = {}, std::vector<SizeT> affected = {});

    /// Used inputs in slot order plus their slot indices, filling caller-owned vectors (reusing
    /// their capacity) instead of allocating - used by the read hot path.
    void collectUsedReadersInto(std::vector<multi_reader::QueueReader*>& readers, std::vector<SizeT>& slotIndices) const;

    /// Scheduler-side entry of the coalesced evaluation (never called with locks held).
    void onCoalescedEvaluation();

    // --- Read path ---
    ErrCode readInternal(void** valueBuffers, void** domainBuffers, SizeT* count, SizeT timeoutMs, IMultiReaderStatus** status, bool skip);
    MultiReaderStatusPtr readEventsLocked();
    MultiReaderStatusPtr createStatusLocked(const DictPtr<IString, IEventPacket>& eventPackets = nullptr,
                                            const NumberPtr& offset = nullptr);
    /// Per-input state snapshot for the status, in slot order: input id + InputState as int,
    /// derived from the used/connected flags, pending events and the current failure state's
    /// affected set. This is the self-contained snapshot the status boxes lazily and the cache
    /// fingerprint compares - no boxed dict is built on the read path.
    void buildInputStateSnapshotLocked(std::vector<std::pair<StringPtr, Int>>& out) const;
    /// Descriptor-changed packet for the status: main value descriptor + common output domain
    /// descriptor (the domain of the status offset).
    EventPacketPtr mainDescriptorPacketLocked();
    void refreshMainInputDescriptorsLocked();

    SizeT findSlotByIdLocked(const StringPtr& id) const;  // returns notFound when not found
    void reindexSlotsLocked();
    void setPortsActiveLocked(bool active);

    // --- Callback-gate maintenance (state mutex held) ---
    // The per-slot flags and the shared counters live in CallbackGate/SlotGateFlags; these
    // helpers are the owner-side write path (the flag word adjusts the counters itself).
    void setSlotReadyLocked(multi_reader::Input* slot, bool ready);
    void setSlotEventLocked(multi_reader::Input* slot, bool event);
    /// Used-flag change with gate accounting: adjusts the used count and drops a stale ready flag.
    void applySlotUsedLocked(multi_reader::Input* slot, bool used);
    /// Publish one slot's adopted basis (availability-until-event + adopted events) for producers.
    void publishSlotBasisLocked(multi_reader::Input* slot);
    /// Lowers every slot's ready/event flag (synchronization invalidated, reader deactivated).
    void clearGateReadinessLocked();

    /// Slot index of the explicitly selected main input; notFound when the default
    /// (first used input) applies or the selection is dangling.
    SizeT mainSlotIndexLocked() const;
    void applyDataLossTimeoutLocked();

    static constexpr SizeT notFound = multi_reader::slotNotFound;

    // --- State ---
    std::mutex mutex;
    std::condition_variable notifyCondition;

    bool invalid{false};  // only the Error state and disposal
    ReaderState state{ReaderState::WaitingForConnections};
    std::string stateMessage;
    std::vector<SizeT> stateAffectedInputs;

    std::vector<ObjectPtr<IInputPortNotifications>> slotObjects;  // strong refs (ports hold weak listener refs)
    std::vector<multi_reader::Input*> slots;                                // parallel implementation pointers

    std::unique_ptr<multi_reader::SynchronizationManager> syncManager;
    std::unique_ptr<multi_reader::ReadCoordinator> readCoordinator;

    /// Read-path scratch, reused across reads to avoid per-read heap allocation. Only ever live
    /// within a single readInternal call (which holds the mutex; no reentrancy), never aliased.
    std::vector<multi_reader::QueueReader*> readScratchUsed;
    std::vector<SizeT> readScratchSlotIndices;
    std::vector<void*> readScratchValueBuffers;
    std::vector<void*> readScratchDomainBuffers;

    /// Availability-query scratch (getAvailableCount and the read timeout predicate), reused to
    /// avoid a per-call heap allocation. Kept separate from the read scratch above so the two
    /// never alias, though both are only ever live under the mutex within one call.
    std::vector<multi_reader::QueueReader*> availScratchUsed;
    std::vector<SizeT> availScratchSlotIndices;

    /// Data-plane change tracking for the synchronized fast path. refreshDataPlaneLocked can skip
    /// its whole per-slot pass when nothing has changed since the last one: no packet has arrived
    /// (dataPlaneDirty, set on the producer path) and no read has consumed (dataPlaneConsumed, set
    /// under the mutex) - a buried event can only surface through an arrival or a consumption, so
    /// there is nothing to re-check. This collapses the repeated getAvailableCount/read refreshes
    /// of a poll-then-read loop to one real pass. Any full evaluateStateLocked re-arms dataPlaneDirty.
    std::atomic_bool dataPlaneDirty{true};
    bool dataPlaneConsumed{false};

    /// Availability captured by the last non-escalating synchronized fast pass of
    /// refreshDataPlaneLocked, so the read path can plan and lower readiness without a second
    /// walk over every input (dedup of the refresh and createPlan availability passes). The
    /// per-slot counts (common-rate equivalent, until the next event) are indexed by slot-vector
    /// index; dataPlaneAvailableCommon is their raw minimum over used slots (pre-alignment).
    /// Valid only while dataPlaneAvailableValid: the fast pass sets it, and any escalation,
    /// non-synchronized refresh, or consuming read clears it (consumers then fall back to a
    /// direct getAvailableCount walk).
    std::vector<SizeT> dataPlaneSlotAvailable;
    SizeT dataPlaneAvailableCommon{0};
    bool dataPlaneAvailableValid{false};

    /// Common-domain tick of the next unread output sample while synchronized.
    std::optional<std::int64_t> nextReadTick;

    DataDescriptorPtr mainValueDescriptor;
    DataDescriptorPtr mainDomainDescriptor;

    /// Status caching: the last event-less status is re-issued while its
    /// visible content is unchanged; any content change (or any event) creates a new object.
    /// The offset is deliberately NOT part of the fingerprint - it advances on every data read,
    /// while the rest of the status content stays constant in steady synchronized state. When the
    /// content matches, the cached status is either re-issued as-is (offset unchanged) or its
    /// offset-independent content is shared into a new status stamped with the advanced offset;
    /// only a genuine content change rebuilds it. The cache is cleared whenever the cross-input
    /// model is invalidated - the cached getMainDescriptor packet embeds the common output
    /// domain, which any input's descriptor change can move.
    MultiReaderStatusPtr cachedStatus;
    struct StatusFingerprint
    {
        ReaderState state{};
        std::string message;
        std::vector<SizeT> affectedInputs;
        // Identity only; safe because the cache is dropped on every model invalidation,
        // which every descriptor change triggers before a new descriptor can be adopted
        IDataDescriptor* mainValue{};
        IDataDescriptor* mainDomain{};

        bool operator==(const StatusFingerprint& other) const
        {
            return state == other.state && message == other.message && affectedInputs == other.affectedInputs &&
                   mainValue == other.mainValue && mainDomain == other.mainDomain;
        }
    };
    StatusFingerprint cachedStatusFingerprint;
    /// Offset-independent content shared with (and by) the cached status, so an offset-only
    /// change restamps a new status without rebuilding any of it. Compared against a freshly
    /// built snapshot each read (the snapshot can move without a fingerprint change, e.g. an
    /// unused input gaining events). Valid only while cachedStatus is assigned.
    InputStateSnapshotPtr cachedInputSnapshot;
    StringPtr cachedStatusMessage;
    ReadStatus cachedReadStatus{};
    std::int64_t cachedStatusOffset{};
    /// Reused across reads to build the current input-state snapshot for the cache comparison.
    std::vector<std::pair<StringPtr, Int>> statusSnapshotScratch;

    /// Common-output-domain descriptor for getMainDescriptor, built lazily per model build
    DataDescriptorPtr cachedCommonDomainDescriptor;

    /// The status main-descriptor event packet, built lazily and reused across reads; cleared
    /// on model/main-descriptor change (invalidateModelLocked, refreshMainInputDescriptorsLocked)
    EventPacketPtr cachedMainDescriptorPacket;

    PropertyObjectPtr portBinder;
    ProcedurePtr readCallback;
    WeakRefPtr<IInputPortNotifications> externalListener;

    LoggerComponentPtr loggerComponent;
    ContextPtr context;

    // --- Configuration ---
    RatioPtr tickOffsetTolerance;  // deprecated; value ignored
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
