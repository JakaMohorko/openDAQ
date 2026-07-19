# Multi Reader Rework — Internal Architecture and Command Flows

**Companion to:** `01_specification.md` (behavioral contract), `02_implementation_plan.md` (phases),
`05_error_contract.md` (public error semantics), `09_review_comments_plan.md` (review actions).
**Code:** everything under `core/opendaq/reader/` unless noted.

> **Addendum (2026-07-19, review batches A–C implemented — commits `aa927ddd`, `45f8bcbf`,
> `2d998ec3`, `6a060e0d`).** This document described the pre-review implementation; the flows
> below remain correct in structure, with these deltas:
>
> - **Public status surface (C5/C6/C7):** `MultiReaderState`/`getState` and the ordered/affected
>   event APIs are gone. `getReadStatus` returns the extended `ReadStatus` (`+ Preparing,
>   Inactive, InputsFailed`), per-input conditions come from `getInputStates` (input id →
>   `InputState`), statuses are built through `MultiReaderStatusBuilder`, and `getValid()` is
>   false only for `Fail`. Internally the granular states survive as the private `ReaderState`.
> - **Data plane (C11/N6):** `readInternal`, the wait predicates, `getAvailableCount` and the
>   coalesced task now run `refreshDataPlaneLocked` — while synchronized, only packet-pending
>   slots are drained and only events (leading or buried: `hasQueuedEventPackets`) and expired
>   deadlines escalate to the full `evaluateStateLocked`. The 13-step evaluation remains the
>   transition handler for mutators and establishment states.
> - **Data loss is in-band (§2.6):** the deadline no longer invalidates while the lost input has
>   buffered data; `DataLost` surfaces at the first evaluation after its queue drains.
> - **Unused inputs (C12/Q5):** their events are drained (`drainUnusedSlotsLocked`), reported as
>   `InputState::Event`, and fire the callback — the gate is `anyEvent() || allUsedReady()`.
> - **Recovery of failing producers:** `exposeBuriedEventsLocked` drops a failed input's stale
>   pre-fix data so a buried corrective descriptor change can surface.
> - **Stable slots (S1):** `removeInput` erases one slot's coordinator/monitor state
>   (`erase(index)`); the remaining inputs keep readiness bits and armed deadlines.
> - **Naming (C13):** the internals live in `daq::multi_reader` under
>   `include/opendaq/multi_reader/`; `InputSlot` is `multi_reader::Input`.
> - **Config (C1/C2):** sync distance and data-loss timeout are builder-only.

---

## 1. Component map and ownership

```
                              IMultiReader / IReaderConfig (public API)
                                            │
                       ┌────────────────────┴─────────────────────┐
                       │            MultiReaderImpl               │  multi_reader_impl.h/.cpp
                       │  facade; owns everything below; owns     │
                       │  the state machine and the single mutex  │
                       └─┬──────┬──────────┬───────────┬────────┬─┘
                         │      │          │           │        │
        ┌────────────────┘      │          │           │        └───────────────┐
        ▼                       ▼          ▼           ▼                        ▼
 InputSlot (×N)     Synchronization   ReadCoordinator  Notification      DataLossMonitor
 input_slot.*       Manager           read_coordinator NotificationCo-   data_loss_monitor.*
 per-input port +   synchronization_  .*               ordinator         per-slot deadline
 queue pairing      manager.*         availability/    notification_     tracking + waiter
        │           common model +    plan/commit      coordinator.*     thread
        ▼           alignment                          coalesced task +
 QueueReader                                           callback gate
 queue_reader.*
 per-input engine:
 connection drain,
 descriptor parsing,
 sample conversion,
 cursor/advance
```

### Ownership and responsibility rules (one owner per responsibility)

| Component | Owns / is the single authority for | Explicitly NOT responsible for |
|---|---|---|
| `MultiReaderImpl` | the state machine (`MultiReaderState state`), the single `mutex`, slot lifecycle, wiring all sub-components, the public error contract | per-input packet mechanics, alignment math, notification scheduling |
| `InputSlot` | pairing one `IInputPortConfig` with one `QueueReader`; per-port `IInputPortNotifications` listener; `used` flag; `packetPending` bit; last-packet-arrival timestamp | any cross-input decision; it forwards semantic notifications to the facade via `IInputSlotListener` and knows nothing about other slots |
| `QueueReader` | one connection's queue: draining (`drain()` is the only adoption point, #10 of the review-feedback batch), descriptor parsing/merging, pending events, validity issues, typed read layout, cursors (`advanceToDomainValue`, `read`) | when to drain (the owner calls `drain()` at evaluation points); state; notification |
| `SynchronizationManager` | the `CommonModel` (common domain, `commonSampleRate`, dividers, `blockLcm`, main position) and the alignment algorithm (`synchronize()`); `commonStart` | reading data; deciding when to re-run; the facade's state enum |
| `ReadCoordinator` | availability (`getAvailableCount`), read planning (`createPlan`), commit/skip across all used inputs, leftover-segment discard | state, alignment, notification |
| `NotificationCoordinator` | coalescing packet notifications into one scheduler task; per-slot `used`/`ready`/`event` masks; the callback gate (`shouldInvokeCallback` = `anyUsedEvent() || allUsedReady()`) | reading; state; it never touches queues |
| `DataLossMonitor` | per-slot arming and deadlines; the waiter thread (spawned only when callback + real clock + timeout > 0); injectable test clock | deciding what a missed deadline *means* (the facade maps it to `DataLost`) |

Declaration order in `MultiReaderImpl` matters: `notificationCoordinator` and `dataLossMonitor` are
declared **last** so a constructor throw destroys them first — no callback can fire into a
half-destroyed facade.

### Key files

| File | Contents |
|---|---|
| `include/opendaq/multi_reader_impl.h`, `src/multi_reader_impl.cpp` | facade, state machine, read path, input management, status creation |
| `include/opendaq/multi_reader/input.h`, `src/multi_reader/input.cpp` | `multi_reader::Input`, `multi_reader::IInputListener` |
| `include/opendaq/multi_reader/queue_reader.h`, `src/multi_reader/queue_reader.cpp` | per-input engine; `QueueReaderIssue` validity model |
| `include/opendaq/multi_reader/synchronization_manager.h`, `src/multi_reader/synchronization_manager.cpp` | `CommonModel`, `SyncSetupIssue`, `SyncOutcome`, alignment |
| `include/opendaq/multi_reader/read_coordinator.h`, `src/multi_reader/read_coordinator.cpp` | `ReadPlan`, `CommitResult`, block math |
| `include/opendaq/multi_reader/notification_coordinator.h`, `src/multi_reader/notification_coordinator.cpp` | task coalescing, masks, gate |
| `include/opendaq/multi_reader/data_loss_monitor.h`, `src/multi_reader/data_loss_monitor.cpp` | deadlines, waiter thread |

All six internals live in the nested `daq::multi_reader` namespace (C13): the former `InputSlot`
is now `multi_reader::Input` (`IInputSlotListener` → `multi_reader::IInputListener`), and the
headers moved under `include/opendaq/multi_reader/` so neither the class names nor the include
paths can clash with future public API.
| `include/opendaq/domain_value.h` | `DomainValue` (typed domain arithmetic: `toDomain`/`fromDomain`, `shiftTicks`, `roundUpOnDomainInterval`), `domain_conversion` helpers |
| `include/opendaq/multi_reader_status.h`, `reader_status_impl.h/.cpp` | `MultiReaderState`, `IMultiReaderStatus`, `MultiReaderStatusEx` factory |

---

## 2. Threading and locking model

- **One mutex** (`MultiReaderImpl::mutex`) guards all facade state, the model, and queue adoption.
  Functions suffixed `...Locked` **require the caller to already hold `mutex`** — the suffix is a
  precondition marker, not "this function locks". (Review comment: this convention must be
  documented at the declaration site; see `09_review_comments_plan.md` C10.)
- **Lock order:** facade `mutex` → connection/queue internals. Never the reverse. Consumer-side the
  effective order is *consumer lock → reader mutex* (the reader never calls out while holding
  `mutex` — see next point).
- **Callbacks always run outside `mutex`:** `readCallback` (dataAvailable) is copied under the lock
  and invoked after release (`onCoalescedEvaluation`); external-listener forwarding in
  `slotConnected` / `slotDisconnected` / `slotPacketReceived` happens after the locked section.
- **Bounded producer path:** `InputSlot::packetReceived` → `MultiReaderImpl::slotPacketReceived`
  takes **no facade lock and touches no queue**. It only: stamps `lastPacketArrival`, sets the slot's
  `packetPending` bit, calls `DataLossMonitor::onPacket` (own short mutex),
  `NotificationCoordinator::requestEvaluation` (atomic exchange + schedule), wakes
  `notifyCondition`, and forwards to the external listener. A producer can therefore never be
  blocked by a long read.
- **Threads that can run reader code:**
  1. *Producer threads* (whoever calls `signal.sendPacket`) — bounded path above; with `SameThread`
     port notification also the initial connect handshake. **Consumers must assume
     `packetReceived`-forwarding can arrive on a producer stack mid-connect** (the sum FB defers all
     reader re-entry from this callback to a scheduler task for exactly this reason).
  2. *Scheduler workers* — the coalesced evaluation task, and anything the consumer schedules.
  3. *The waiter thread* — `DataLossMonitor` deadline watcher; it never touches the facade directly,
     it fires `deadlineCallback` → `requestEvaluation` + condition wake.
  4. *API callers* — any public method.
- **Coalescing invariant** (`NotificationCoordinator`): `taskState->scheduled` is cleared at task
  *start*, so updates arriving during an evaluation schedule exactly one follow-up task and none are
  lost. A destroyed coordinator leaves a harmless no-op task behind (`detach()` clears the callback;
  the task holds the shared `TaskState` alive).

---

## 3. The state machine

### States (`MultiReaderState`, public today — under review for compaction)

`Inactive`, `WaitingForConnections`, `WaitingForDescriptors`, `Incompatible`, `WaitingForData`,
`Synchronizing`, `Synchronized`, `EventPending`, `SynchronizationFailed`, `DataLost`, `Error`.
`getValid()` is derived: `Incompatible`, `SynchronizationFailed`, `DataLost`, `Error` → invalid
stream. Only `Error` is unrecoverable (invariant: `invalid == true` ⇔ `Error`).

### `evaluateStateLocked()` — the (re)derivation ladder

The state is currently **re-derived from scratch** on every evaluation, in a fixed order
(spec §6.2). Steps, as implemented:

| # | Check | Work done per evaluation | On failure |
|---|---|---|---|
| 1 | `invalid`? reader active? | — ; inactive branch: disarm monitor for all slots, sync+drain *used* slots' connections, maintain event masks so events still surface | `Error` / `Inactive` or `EventPending` |
| 2 | resolve used set, main input | `collectUsedReaders` (vector alloc), `mainSlotIndexLocked` | `WaitingForConnections` ("No used inputs" / dangling main) |
| 3 | every used input connected? | `InputSlot::syncConnection()` per used slot — **re-reads the port's connection and drains the queue** (`QueueReader::drain`) | `WaitingForConnections` |
| 4/5 | pending events? handshake-in-flight? | `hasPendingEvents` per slot; handshake rule: a connected used input with *neither* descriptors *nor* events defers `EventPending` (falls through to step 6) so blocked reads and the callback see every input's initial event at once | clear sync; `EventPending` (opportunistic model build first so `getCommonSampleRate` etc. work right after construction) |
| 6 | both descriptors on every used input? | descriptor getters per slot; then `updateMainDescriptorsLocked()` (refresh cached main value/domain descriptors) | `WaitingForDescriptors` |
| 7 | local validity (`QueueReader::isValid`) | per slot | `Incompatible` (affected inputs listed) |
| 9 | data-loss deadlines | `DataLossMonitor::lostSlots()` (own lock) — checked **before** alignment so a stalled input surfaces even while synchronized | `DataLost` |
| — | already synchronized? (`commonStart != nullptr`) | short-circuits 8/10/11/12 | stays `Synchronized` |
| 8 | cross-input model | `SynchronizationManager::buildCommonModel` (rates, dividers, blockLcm, rational-GCD resolution, overflow guards) | `Incompatible` |
| 10 | ≥1 sample on every used input | `getAvailableSamples` per slot | `WaitingForData` |
| 11 | alignment | `synchronize()` (see §5.8) | `Synchronizing` / `EventPending` / `SynchronizationFailed` |
| 12 | configure pipelines | `ReadCoordinator::configure`; capture `nextReadTick` | — |
| 13 | readiness for the callback gate | per used slot: `setReady(...)` (full aligned block when synchronized, first sample otherwise) | — |

### All call sites of `evaluateStateLocked()` (19)

Constructors ×3 (list/FromExisting/builder) · `onCoalescedEvaluation` (every coalesced packet task) ·
`slotConnected` · `slotDisconnected` · after events are returned by a read (`readEventsLocked`
caller) · `readInternal` entry · the two timeout wait-predicates (**per condition-variable wake**) ·
`readInternal` post-wait · `getAvailableCount` · `setActive` · `addInput` · `removeInput` ·
`setInputUsed` · `setMainInput` · `setMaxSynchronizationDistance` · `setDataLossTimeout`.

**Measured consequence:** one idle poll cycle (`getAvailableCount` + `read`) while `Synchronized`
executes the ladder **twice**; a blocking read executes it once per wake plus once after the wait.
Steps 3 (drain per slot), 9 (monitor query), and 13 (mask updates) run every time even when nothing
changed. This is the redundancy the review flags; the planned split into event-driven maintenance +
a clean/dirty fast path is specified in `09_review_comments_plan.md` (C11).

### State writers

`setStateLocked` / `setStateWithAffectedLocked(state, prefix, suffix, affectedInputs)` are the only
writers; the latter formats the message from affected slot indices exactly once (also avoiding an
MSVC unsequenced-argument pitfall with `fmt::format(...vec...)` + `std::move(vec)`). The
`stateMessage` and `affectedInputs` captured here are what `createStatusLocked` snapshots into
statuses.

---

## 4. Status creation and caching

`createStatusLocked(eventPackets, offset, eventInputIndices, orderedEventPackets)` builds a
`MultiReaderStatusEx` from the current `state` + `stateMessage` + `affectedInputs`. Two caches:

- **StatusFingerprint cache** — identical consecutive statuses (same state/message/affected/offset,
  no events) return the same cached object; cleared by `invalidateModelLocked`.
- **`mainDescriptorPacketLocked()`** — the `getMainDescriptor` event packet (main input's value
  descriptor + the **common output domain descriptor**, i.e. the grid `getOffset` is expressed in)
  is rebuilt lazily per model build (`cachedCommonDomainDescriptor`, also cleared on invalidation).

Validity is derived from state — the Ex factory takes no `valid` flag (RTGen factory macros cap at
8 parameter pairs on MSVC; deriving validity was also what kept the factory within the limit).

---

## 5. Command flows

Notation: **[P]** producer thread, **[S]** scheduler worker, **[A]** API caller thread,
**[W]** waiter thread. "Not involved" lists components that deliberately play no part.

### 5.1 Construction

Three constructors exist today (consolidation into the builder path is planned — C8):

1. **Builder ctor** (`MultiReaderImpl(const MultiReaderBuilderPtr&)`) — the canonical one. Copies
   config, validates deprecated `tickOffsetTolerance` (warn + ignore), resolves `mainInputId`,
   creates the four sub-components, wires `notificationCoordinator->setEvaluationCallback` and
   `dataLossMonitor->setDeadlineCallback` (→ `requestEvaluation` + condition wake), then:
   `createOrAdoptPorts(sourceComponents)` → `createSlots(ports)` → under `mutex`: validate main
   input exists → `setPortsActiveLocked(isActive)` (adopted ports may arrive deactivated from a
   previous owner parking them) → `applyDataLossTimeoutLocked()` → `evaluateStateLocked()`.
2. **List ctor** (signals or ports + read types) — same body minus builder fields;
   `checkListSizeAndCacheContext` (validates non-empty + caches `context`) and
   `sourceComponentsType` (signals vs ports; mixing throws) run first.
3. **`FromExisting` ctor** — invalidates the old reader (`Error`), moves its ports, **seeds the new
   `QueueReader`s with the old active descriptors** (`seedDescriptors`) because the original
   descriptor events were already consumed, restores per-slot `used` flags. Dies in Phase 6.1.

`createSlots(ports)` per port: external ports (no `MultiReaderInternalPort` tag) get
`setOwner(portBinder)` (a plain `PropertyObject` marking "externally owned"); the port's
notification method is set (per-port list or the single method; `Unspecified` keeps the port's
existing setting and is only legal for port-constructed readers); an `InputSlot` is created with the
facade as `IInputSlotListener`; **`port.setListener(slotObject)` — the slot, never the facade, is
the port listener.** Setting a listener on an already-connected port makes the SDK re-enqueue the
last descriptor event (`enqueueLastDescriptor`), which is how a rebuilt reader learns current
descriptors without any replay hack. Finally `notificationCoordinator->resize(n)` /
`dataLossMonitor->resize(n)` extend the per-slot arrays.

Signal-constructed readers create their own ports (tagged `MultiReaderInternalPort`) and connect
them; port-constructed readers adopt.

*Not involved:* `ReadCoordinator` (configured only at sync), the waiter thread (spawned lazily by
`applyDataLossTimeoutLocked` only if callback + real clock + timeout > 0).

### 5.2 Destruction and dispose

`internalDispose`: detach monitor (stops/joins the waiter), detach coordinator (callback null — a
queued coalesced task becomes a no-op), detach every slot's listener pointer, clear
`externalListener` / `readCallback` / caches, `invalid = true`, state `Error`. **`portBinder` is
deliberately kept** — it marks the ports as externally owned so the destructor does not `remove()`
them (dispose-and-rebuild on the same ports is the documented consumer pattern; the binder dies
with the reader, releasing ownership for re-adoption).

`~MultiReaderImpl`: detach again (idempotent), then — only when `portBinder` was never assigned,
i.e. all ports are reader-created — `port.remove()` per slot.

### 5.3 Adding an input (`addInput`) **[A]**

`createOrAdoptPorts` + `createSlots` **append** a slot; existing slots, masks and monitor state are
untouched (`resize` extends). Then `invalidateModelLocked` + `evaluateStateLocked` — the used-set
change forces full revalidation and resync. Duplicate detection: `ERR_DUPLICATEITEM`.

### 5.4 Removing an input (`removeInput`) **[A]**

Current behavior (under review — "slots should not be reconstructed"):
find slot by id (`OPENDAQ_NOTFOUND` when absent) → if it was the selected main input, warn and
revert to automatic → `slot->detachListener()` → `port.remove()` only if reader-owned → erase slot →
**`reindexSlotsLocked()` shifts every subsequent slot's index** →
**`notificationCoordinator->resize(0); resize(n)` and `dataLossMonitor->resize(0); resize(n)` drop
*all* per-slot masks and arming state** (remaining inputs re-arm on their next packets) → re-derive
used masks → invalidate + evaluate.

Consequences worth knowing: any slot-index-based data a consumer holds (e.g. status
`affectedInputIndices` from an earlier read) is stale after a remove; monitor arming for healthy
inputs restarts. The planned fix is stable slot identity + per-slot erase instead of reset
(`09_review_comments_plan.md`, "stable slots").

### 5.5 Signal connect / disconnect **[P or A — whoever calls `port.connect`]**

```
port.connect(signal)
 └─ SDK: creates Connection, enqueues initial descriptor event, notifies
    ├─ InputSlot::connected(port)            (port listener = slot)
    │   └─ MultiReaderImpl::slotConnected(index)
    │       ├─ under mutex: slot->rebindConnection() (QueueReader adopts the new connection
    │       │                and drains it), invalidateModelLocked(), evaluateStateLocked()
    │       ├─ notifyCondition.notify_all()
    │       ├─ notificationCoordinator->requestEvaluation()   // guarantees one evaluation
    │       │   // after the connection is fully constructed (its first packets may have
    │       │   // raced an earlier coalesced task that found nothing returnable)
    │       └─ externalListener->connected(port)              // outside mutex
    └─ (packet notifications for the initial event follow the 5.6 path)
```

Disconnect mirrors it via `slotDisconnected`, with one addition: the slot's monitor entry is
disarmed **immediately** (`setMonitored(index,false)`) before rebinding, so a stale arrival cannot
count toward a deadline after a reconnect.

*Not involved:* `ReadCoordinator`, `SynchronizationManager` beyond invalidation.

### 5.6 Packet arrival → notification → data callback **[P → S]**

```
signal.sendPacket(...)
 └─ Connection::enqueue                      [P]
    │   (inactive port: data packets DROPPED with OPENDAQ_IGNORED; event packets still enqueue)
    ├─ port notification per configured method:
    │   SameThread → listener called inline on the producer stack
    │   Scheduler  → scheduler work item that calls the listener
    │   (the initial connect handshake uses enqueueOnThisThread → always SameThread delivery)
    └─ InputSlot::packetReceived             [P or S]
        ├─ lastPacketArrival = now; packetPending = true      (atomics, no locks)
        └─ MultiReaderImpl::slotPacketReceived(index)         BOUNDED PRODUCER PATH
            ├─ dataLossMonitor->onPacket(index)   (arms/re-arms deadline iff monitored && timeout>0)
            ├─ notificationCoordinator->requestEvaluation()   (coalesced)
            ├─ notifyCondition.notify_all()                   (wakes blocked timeout reads)
            └─ externalListener->packetReceived(port)         (unconditional — also for unused
                                                               ports; consumers must not re-enter
                                                               the reader from this callback)
Later, on a scheduler worker:
 onCoalescedEvaluation                        [S]
 ├─ under mutex: evaluateStateLocked(); callback = readCallback if shouldInvokeCallback()
 │   gate: anyUsedEvent() || allUsedReady()   (ready = full aligned block when synchronized)
 ├─ notifyCondition.notify_all()
 └─ wrapHandler(callback)                     // dataAvailable, OUTSIDE the lock
```

Important gate property: a used input with **no data** (dead stream, `DataLost`, probe of a dead
input) keeps `allUsedReady()` false — the dataAvailable callback **never fires for stuck states**.
Consumers observe those either by polling reads or from packet activity on healthy inputs (the sum
FB's staleness check). This is a deliberate but subtle contract; the status/state rework revisits
how stuck states reach consumers.

### 5.7 Descriptor events, the handshake, and event reads

- Descriptor/gap packets are ordinary packets until an evaluation **drains** them
  (`QueueReader::drain` at `syncConnection`/`rebindConnection`), where `QueueReader` parses them:
  consecutive descriptor changes merge; gaps stay ordered; parse failures record
  `QueueReaderIssue`s (→ `isValid() == false` → `Incompatible`).
- Leading events make step 4/5 report `EventPending` (sync cleared). Exception — **handshake
  deferral**: while any connected used input has neither descriptors nor events (its initial event
  is still being enqueued by a mid-flight connect), event bits are suppressed and the state falls
  through to `WaitingForDescriptors`, so both blocked zero-count reads and the callback see *all*
  inputs' initial events in one batch instead of a partial set.
- A read in `EventPending` returns `ReadStatus::Event`, count 0, popping **one event per affected
  input per call** into the dict (`port globalId → event packet`) and the ordered list
  (`eventInputIndices` + `orderedEventPackets`; the ordered list is slated for removal — C6).
  Event consumption immediately `invalidateModelLocked()` + `updateMainDescriptorsLocked()` +
  re-evaluates: descriptors apply, rates may change, the model is rebuilt, resync is automatic on
  subsequent evaluations.
- **Mid-request events**: `getAvailableCount`/plan clamp availability at each input's
  `samplesUntilEvent`, so a read returns complete blocks before the event; the trailing partial
  block is silently discarded when the event becomes queue head (`discardLeftoverSegment`); the
  *next* call returns the event.
- **Gap events** are returned like descriptor events (separate dict entries) and invalidate sync;
  data never resumes without realignment. The reader itself requests no gap checking — gap packets
  exist only if the port/signal produce them.

### 5.8 Synchronization (inside `evaluateStateLocked` step 11)

Preconditions guaranteed by the ladder: active, connected, no pending events, both descriptors,
locally valid, model built, ≥1 sample everywhere.

1. First-sample `DomainValue` per used input → exact conversion to the common domain
   (`toDomain`; §4.1 of the spec makes signal→common exact by construction).
2. **Span check**: if `maxSynchronizationDistance > 0` and `latest − earliest` exceeds it →
   `SyncOutcome::Failed`; the affected set is the inputs **farther behind the latest start than the
   threshold** (i.e. the laggards).
3. **Candidate start** = latest first-sample rounded **up** on the aligned grid (full domain unit
   when `startOnFullUnitOfDomain`, else block interval `Ratio(blockLcm, commonSampleRate)`).
4. **Exact-point search**: when tick math is available, walk the **main input's grid** (the main
   input defines output-grid phase) over one aligned block, max 1024 steps, looking for a tick every
   input hits exactly (grid patterns repeat after one block, so searching further is pointless).
   Fallback acceptance: the first step where every forward phase offset satisfies
   `2·offset < blockTicks` (strictly less than half a block, else block attribution is ambiguous).
   Neither exists → `SyncFailureReason::NoCommonTick`, affected = the misaligned inputs.
5. `advanceToDomainValue(target)` per input; outcomes combine: any `DomainChanged` → `EventPending`;
   any `NeedMoreData` → `Synchronizing` (retry on next packet); `OvershotError` → recompute;
   all `Success` → verify each *reached* value equals the candidate (reached-value verification —
   rounding is never silently trusted); mismatch re-targets from max(reached), bounded by 8
   iterations → `NoCommonTick`.
6. Success: `commonStart = candidate`, `ReadCoordinator::configure` builds the direct pipelines,
   `nextReadTick` anchors `getOffset`.

**Invalidation triggers** (each clears `commonStart` and read pipelines; next evaluation re-runs
setup): any returned event · connect/disconnect · descriptor change applied · main-input change ·
used-set change (`setInputUsed`, `addInput`, `removeInput`) · activation change ·
`setMaxSynchronizationDistance`/`setDataLossTimeout` (sync-affecting config) · data-loss detection.

### 5.9 Read path **[A]**

`read` / `readWithDomain` / `skipSamples` → `readInternal(valueBuffers, domainBuffers, count,
timeoutMs, status, skip)`:

1. `invalid` → status(count 0), `OPENDAQ_IGNORED` for skip.
2. `evaluateStateLocked()` (full ladder).
3. **Zero-count read** (the descriptor handshake): with timeout, wait on `notifyCondition` until
   `EventPending` (predicate re-runs the ladder per wake); return `readEventsLocked()` if events,
   else a plain status. Never consumes data.
4. Non-zero count with timeout: wait until the *whole aligned request* is servable or an event
   arrives (`ReadTimeoutType::All` — the only honored mode); predicate per wake: ladder +
   availability.
5. `EventPending` → return events (one per input, count 0). Not `Synchronized` → status, count 0.
6. `Synchronized`: `collectUsedReaders` → map caller buffers (jagged, ordered by slot index; null
   allowed for unused slots) to used positions → `ReadCoordinator::createPlan(requested …)` —
   requested count is rounded **down** to `blockLcm`; per-input sample count = common count ÷
   divider — → `commit` (or `skip`). A commit failure is an invariant break: `invalid = true`,
   state `Error`.
7. `getOffset` bookkeeping: status offset = `nextReadTick` (common-domain tick of this read's first
   sample); advance by `commonCount · ticksPerCommonSample`.

`getAvailableCount`: ladder + `ReadCoordinator::getAvailableCount` (min over used inputs of
`samplesUntilEvent · divider`, floored to a `blockLcm` multiple, zero below `minReadCount`).
Returns 0 in any state but `Synchronized`.

*Not involved in reads:* `DataLossMonitor` beyond the ladder's step-9 query; producers (the read
never blocks them — it holds `mutex`, they don't take it).

### 5.10 Data-loss timeout **[P, W, S]**

- Arming: `onPacket(slot)` **[P]** arms/re-arms `deadline = now + timeout` iff the slot is
  `monitored` && timeout > 0. Monitored is maintained by the facade = used ∧ connected ∧ reader
  active (refreshed in the ladder; disarmed immediately on disconnect/unused/inactive).
  First packet arms; silence afterwards trips.
- The **waiter thread** exists only when a deadline callback is set, the real clock is used, and
  timeout > 0 (`ensureWaiterLocked`; the injectable test clock disables it — loop condition
  `!stopping && useRealClock`). It sleeps until the earliest deadline, re-checks after every wake
  (a `wait_until` timeout on an already-expired deadline still re-validates), and fires
  `deadlineCallback` **[W]** → `requestEvaluation()` + condition wake — deadlines enter the same
  coalesced evaluation path as packets.
- The coalesced evaluation **[S]** hits ladder step 9: `lostSlots()` non-empty →
  `invalidateSynchronizationLocked()` + state `DataLost` with the late inputs as affected.
- `setTimeout` clears all arming (both directions); `resize` drops it; recovery is per input — its
  next packet re-arms and the normal ladder resynchronizes.
- Reminder: `DataLost` does **not** fire the dataAvailable callback (gate; §5.6).

### 5.11 Unused inputs (`setInputUsed(id, false/true)`) — current semantics **[A]**

Marking unused: `slot->setUsed(false)`, coordinator mask cleared, monitor disarmed,
**`slot->setPortActive(false)`** — the *port* is deactivated, so the connection **drops data
packets at enqueue** (no growth) **but keeps event packets**; `invalidateModelLocked` + ladder
(model/rates/blockLcm rebuild over the remaining used set).

While unused: the active-reader ladder **skips the slot entirely** (`collectUsedReaders`), so its
queued events are neither drained nor surfaced in any status — however, event enqueues still fire
`slotPacketReceived`, so the **external listener does see raw `packetReceived` for unused ports**
(this is what drives the sum FB's event-driven probing). Surfacing unused-input events through the
status/callback properly is a review action item (C12).

Re-enabling: `setPortActive(isActive)` + `rebindConnection()` +
**`QueueReader::dropForInactive()`** — data and gap packets queued while unused are dropped,
descriptor changes are **kept** so the type state stays coherent ("restart from the live stream");
then invalidate + ladder → revalidation, resync.

Terminology note (review item): *active/inactive* is reader- and port-level (`setActive`),
*used/unused* is input-participation level (`setInputUsed`); the implementation reuses port
deactivation as the unused mechanism, which is why the terms blur in places.

### 5.12 Reader deactivation (`setActive(false)` / `(true)`) **[A]**

Deactivation: ports of used slots deactivated, sync invalidated, readiness cleared, and
`QueueReader::dropForInactive()` per slot — queued data/gaps dropped, descriptor changes kept.
While inactive, the ladder's inactive branch still drains used inputs' connections and maintains
event bits, so descriptor events surface through reads (`EventPending`) and the callback even when
inactive; plain inactivity reports `Inactive` (reads: `Ok`, count 0). Reactivation restores port
activity and re-evaluates (full resync).

### 5.13 Config changes **[A]**

`setMainInput` (unknown id → `ERR_NOTFOUND`; unused input → `ERR_INVALIDPARAMETER`; empty clears to
automatic = first used input), `setMaxSynchronizationDistance` / `setDataLossTimeout` (null →
`ERR_ARGUMENT_NULL`, negative → `ERR_INVALIDPARAMETER`, zero disables) — each applies the value,
invalidates what it affects, and re-evaluates. The two rate/timeout setters are slated to become
builder-only (C1/C2), turning runtime changes into a rebuild.

---

## 6. Known hot spots and structural debts (inputs to the review plan)

**All nine items below are resolved by the review batches (see the addendum at the top and
`09_review_comments_plan.md` §5); kept as the record of what the review was measured against.**

Facts, verified on the pre-review branch — the *decisions* about them live in
`09_review_comments_plan.md`:

1. **Full re-derivation per data-plane call.** 19 `evaluateStateLocked` call sites; an idle
   synchronized poll cycle runs the 13-step ladder twice (availability + read), a blocking read once
   per wake. Steps 3 (per-slot connection sync + drain), 9 (monitor query), 13 (mask updates) and
   the `collectUsedReaders` vector allocation repeat regardless of whether anything changed.
2. **State is re-derived, never maintained.** The mutating paths (connect, events, deadline, config)
   already know exactly what changed but only trigger a full re-derivation instead of updating state
   incrementally; nothing tracks "clean since last evaluation".
3. **`removeInput` resets everything per-slot.** Coordinator and monitor state for *all* inputs is
   dropped (`resize(0)`), slot indices shift (`reindexSlotsLocked`), index-based status data goes
   stale across a remove.
4. **Vestigial facade listener surface.** `MultiReaderImpl` implements `IInputPortNotifications`,
   but no port ever has the facade as listener (`createSlots` always binds the slot) and no code
   in the repo invokes the facade's `connected`/`disconnected`/`packetReceived`/`acceptsSignal`;
   the methods only forward to `externalListener`, duplicating the forwarding already done in
   `slotConnected`/`slotDisconnected`/`slotPacketReceived`.
5. **`IInputSlotListener` is load-bearing** (unlike #4): it carries the slot index into the facade
   on the bounded producer path and serializes external-listener forwarding outside locks.
6. **Status surface breadth.** 11 public states; ordered event list (`getEventCount`/`getEvent`)
   duplicating `getEventPackets` (consumers: 11 uses in `test_multi_reader.cpp`, none in FBs);
   slot-index-based `getAffectedInputCount`/`getAffectedInputIndex` (consumed by the sum FB and
   tests) — indices are exactly the thing `removeInput` invalidates.
7. **Unused-input events invisible** except as raw external-listener packet notifications (§5.11).
8. **Three constructors** with duplicated wiring; `checkListSizeAndCacheContext` doing validation
   *and* context caching; `sourceComponentsType` recomputed rather than cached at validation time.
9. **Config split** between builder and runtime setters for `maxSynchronizationDistance` /
   `dataLossTimeout` (runtime setters used by 13 test sites + the sum FB).
