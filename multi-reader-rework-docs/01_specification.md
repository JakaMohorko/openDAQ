# Multi Reader Rework — Specification

**Status:** Final
**Scope:** Synchronous signals with linear time domains; equal or integer-divisible rates via the direct path; arbitrary rates via resampling
**Basis:** Consolidates the prior architecture drafts, the overall-requirements draft, and the current `refactor/new-signal-reader` branch.

---

## 1. Purpose and scope

The Multi Reader reads a set of time-domain signals as one time-aligned output stream. Inputs may differ in origin (epoch), tick resolution, and sample rate. The reworked reader has exactly one owner for every responsibility, a deterministic state machine, clearly communicated events, and a defined recovery path for every failure.

**In scope:** linear time domains (`quantity="time"`, unit `"s"`, implicit linear rule, integer delta, integer sample rate per signal); partial packet reads; descriptor changes; gaps; connect/disconnect recovery; unused inputs; inactive reader; scaled/unscaled/raw reading; different rates via sample-rate dividers (direct path); arbitrary rates via resampling; data-loss detection; main-input and target-rate selection.

**Out of scope (future work):** asynchronous (explicit-rule) signals, non-linear domain rules, non-time domains.

### Terminology

- **Signal-domain timestamp** — a tick in one signal's own domain `{epoch, tick resolution}`. Do not use "native" in code or docs.
- **Common-domain timestamp** — a tick in the derived common domain to which all inputs are aligned.
- **Direct path** — reading without resampling; inputs deliver samples on their own grid; alignment via sample-rate dividers.
- **Resampled path** — inputs are interpolated onto the common output grid.

---

## 2. Architecture and ownership

```
MultiReaderImpl                     (public facade, config, state machine, locks, status)
 ├── InputSlot [0..N-1]             (implements IInputPortNotifications for its port)
 │     ├── InputPortConfigPtr       (owned internal port, or adopted external port)
 │     ├── QueueReader              (exists on branch: packet queue, cursor, descriptors, events)
 │     └── ResamplerPtr             (null on the direct path)
 ├── SynchronizationManager         (common domain/rate model, output timeline, alignment)
 ├── ReadCoordinator                (plan → validate → execute direct/resample → commit; one merged coordinator)
 ├── NotificationCoordinator        (used/ready/event bitsets, coalesced dataAvailable)
 ├── DataLossMonitor                (per-input packet-arrival deadlines)
 └── ResamplerBuilderPtr            (one reusable IResamplerBuilder; default LinearResamplerBuilder)

Stateless helpers: DomainInfo / DomainValue (domain_value.h), TypedReadingUtils / ReadLayout
                   (typed_reading_utils.h), EnumFlags<QueueReaderIssue> (enum_flags.h)
```

### Ownership rules (one owner per responsibility)

| Responsibility | Owner |
|---|---|
| Public API, configuration, input order, runtime state, status | `MultiReaderImpl` |
| One port's lifecycle, used/connected flags, last packet arrival | `InputSlot` |
| One input's packet queue, read cursor, active descriptors, local issues, pending events | `QueueReader` |
| Cross-input compatibility, common domain/rate/dividers/block size, alignment | `SynchronizationManager` |
| Atomic multi-input availability/read/skip planning and commit; per-input direct/resample path | `ReadCoordinator` |
| Readiness tracking, callback coalescing and dispatch outside locks | `NotificationCoordinator` |
| Packet-liveness deadlines and lost-input set | `DataLossMonitor` |
| Typed conversion and domain arithmetic | Stateless helpers |

Hard rules:

- `SignalReader` and `QueueReader` must not coexist. `QueueReader` is the only per-input engine; `SignalReader`, `ReaderDomainInfo`, `Comparable`/`ComparableValue`, stateful `TypedReader` objects, and `ReadInfo` are removed from the multi-reader path.
- Only `QueueReader` dequeues packets or moves a per-input cursor.
- `MultiReaderImpl` never parses packet buffers, searches timestamps, or converts samples.
- Input indices are stable and follow construction order; buffer order equals slot order. `removeInput` removes a slot; remaining indices do not silently reorder (buffers follow the remaining slot order, documented).

---

## 3. Component contracts

### 3.1 `QueueReader` (exists; contract completions marked NEW)

Owns the local packet deque, `readingPosition` in the front packet, active value/domain descriptors (`TypedReadingContext`), recoverable `EnumFlags<QueueReaderIssue>` issues, and the pending `SignalEvent` queue. Works exclusively in the signal's own domain; the only cross-domain knowledge is the assigned `sampleRateDivider` (direct path only).

Existing API kept: `getDomainInfo`, `getFirstSampleDomainValue`, `advanceToDomainValue`, `getSampleRate`, `dropOutdatedPacketSegments`, `getAvailableSamples` (common-rate equivalent), `hasPendingEvents`, `popFrontEvent`, `isValid`, `updateConnection`, `set/getSampleRateDivider`, `read`, `skip`.

Contract completions:

- **NEW `AdvanceOutcome`** — `advanceToDomainValue` returns `{AdvanceResult result, std::unique_ptr<DomainValue> reachedValue}`. `reachedValue` is the first-sample timestamp actually reached (assigned on `Success`). Required by the iterative synchronization algorithm (§5) and the rounding policy (§4.3).
- **NEW `getAvailableSamplesUntilEvent()`** — samples (common-rate equivalent) from the cursor to the next event packet or queue end; used by availability and leftover-segment logic.
- **NEW `getFirstSampleAbsoluteTime()`** (or out-param on `getFirstSampleDomainValue`) — system-clock time of the first unread sample, for `maxSynchronizationDistance` diagnostics.
- **`discardLeftoverSegment(SizeT samplesInBlock)`** — renamed from `dropLeftoverSegment`. If an event packet exists in the queue and fewer than `samplesInBlock / sampleRateDivider` signal-domain samples remain before it, drop those samples and consume the event packets into the pending-event queue. **It creates no synthetic event and reports no dropped count** — `SignalEvent::syncGapEvent` and its `GAP_DIFF` construction are removed. The discontinuity is observable from the next read's domain output/offset.
- **Event merge rule:** consecutive descriptor-change events merge (newest value/domain descriptor wins). **Gap events never merge with anything, including other gaps** — the current `SignalEvent::merge` silently drops the second gap's `gapDiff`; this is a bug. Order is preserved: `Desc, Gap, Desc` stays three entries.
- **Pending events block data operations:** while `hasPendingEvents()` is true, `read`/`skip`/`advanceToDomainValue` do not cross the event boundary; `readNative` already returns `Error` — keep, and document that owners must pop events first.
- **Dimensions and structs:** value samples of any rank are readable — the reader treats one sample as a fixed-size block whose size follows the descriptor (`valuesPerSample` = product of all dimension sizes; byte size from `rawSampleSize`). Struct-type values are readable the same way. Only the per-sample size differs for the multi reader; counts and alignment are always in samples. Type *conversion* still applies only where `isSampleTypeConvertible` allows it — multi-dimensional and struct data is copied same-type/raw, not converted. Domain samples must be scalar (a vector timestamp has no meaning): a domain descriptor with dimensions sets `QueueReaderIssue::UnsupportedDimensions`.
- **Packet refresh model:** `QueueReader` drains its connection automatically (`drainConnection`) before any queue-dependent operation, under the reader lock. `InputSlot::packetReceived` does not touch the queue (§7).

`QueueReaderIssue` flags stay recoverable: recomputed on every descriptor parse; a later valid descriptor clears them (no sticky invalid). Add `UnsupportedDimensions`; keep `ValueTypesNotConvertible`, `DomainTypesNotConvertible`, `UnsupportedDomainRule`, `OriginParsingFailed`.

### 3.2 `InputSlot` (new)

Implements `IInputPortNotifications` and is registered as its port's listener. Translates raw notifications into semantic calls on a non-owning `IInputSlotListener*` (implemented by `MultiReaderImpl`), avoiding an ownership cycle.

```
class InputSlot final : public IInputPortNotifications
    SizeT index; StringPtr inputId (component global id)
    InputPortConfigPtr port          // internal (created for signal) or adopted external
    QueueReader queueReader
    ResamplerPtr resampler           // null on direct path
    std::atomic_bool used{true}, connectedState{false}, packetPending{false}
    std::atomic<steady_clock::time_point> lastPacketArrival
```

`packetReceived` is bounded: store `lastPacketArrival`, set `packetPending` (first transition notifies the owner), return. No dequeue, no descriptor parsing, no user callback.

`connected`/`disconnected` update the slot, reset/replace the `QueueReader` connection, then notify the owner. External listener forwarding (`setExternalListener`) happens in `MultiReaderImpl`, outside internal locks.

Unused slot: excluded from connection masks, compatibility, synchronization, events, availability, readiness, and data-loss checks; its queue is cleared up to the last event (`skipUntilLastEventPacket` equivalent) and its port deactivated. Re-enabling resets the queue and re-runs full validation and synchronization.

### 3.3 `SynchronizationManager` (new)

Owns everything derived from more than one input:

```
CommonModel {
    DomainInfo  commonDomain        // epoch + resolution, §4
    Int         commonSampleRate
    SizeT       sampleRateDivider[slot]
    SizeT       blockLcm            // LCM of all dividers
    std::unique_ptr<DomainValue> commonStart   // assigned only while Synchronized
    SizeT       mainSlot            // grid/phase reference, §5
}
```

Responsibilities: cross-input checks (reference-domain compatibility as implemented in `checkReferenceDomainInfo`; rate policy; required-rate validity; overflow guards), common-model construction, and the alignment algorithm of §5. It never owns queues, never consumes reportable events, never reads user buffers.

`isSynchronized()` derives from the state machine, never from "is `commonStart` assigned" (today's bug).

### 3.4 `ReadCoordinator` (new; single merged coordinator)

One coordinator handles both paths; resampling is a per-input execution branch, not a second coordinator.

- `configure(usedInputs, model, timeline, resamplerBuilder)` — after every successful synchronization: per input, select direct copy (source grid exactly matches output grid) or build a fresh `IResampler` via the injected builder. Builders are called once per input per configure; resamplers are never created during reads.
- `getAvailableOutputCount(...)` — complete `blockLcm`-aligned blocks (direct) or complete output-grid samples (resampled) available on **every** used input before the earliest event boundary; returns 0 below the effective minimum (`ceil(minReadCount / blockLcm) * blockLcm`). Never mutates queues.
- `createPlan(requestedCount)` — round down to `blockLcm` (direct) / clamp to available (resampled); compute per-input counts (`count / divider_i` direct, `count` resampled); validate buffers and pipelines; stop before any event boundary.
- `commit(plan)` — execute all input reads/resamples, then advance all cursors in stable slot order. A queue failing to commit a validated plan is an internal error (`Error` state).
- `discardLeftoverSegments()` — when no complete block remains before an event on some input, call that input's `QueueReader::discardLeftoverSegment`; silent (§3.1). Other inputs are realigned by the mandatory post-event resynchronization, not trimmed here.
- `skip` shares the planner and all alignment rules with `read`.

### 3.5 `NotificationCoordinator` (new)

Maintains `usedMask`, `readyMask`, `eventMask` bitsets. After any slot update:

```
schedule one coalesced task  iff  (eventMask & usedMask).any()
                                   || (readyMask & usedMask) == usedMask
                                   || stateChangeNotify
```

The scheduled task re-runs state evaluation and invokes the public `onDataAvailable` callback when an event is returnable, one full block is readable, **or** the reader has just recognized a condition the consumer must act on that carries neither returnable data nor a returnable event. This last case is a transition into an `InputsFailed` state (`Incompatible` / `SynchronizationFailed` / `DataLost`): once the descriptors that caused the failure are cached, no new event fires, and no data is ready, so nothing else would wake the consumer.

Failure recognition **must** wake the consumer. The reader detects it on its own — a data-loss deadline elapses (scheduler-armed; see §3.6), or a re-included input's cached descriptors are re-evaluated as incompatible / unsynchronizable — and the recognition is itself the notification: the consumer must not have to poll or run its own liveness timer to discover a stalled or failing input. On the transition into `InputsFailed` the reader raises `onDataAvailable`, and the consumer reads the naming status (with per-input states) on its next `read()`. This keeps the consumer implementation lean — a single `onDataAvailable` handler plus a `read()` is sufficient to observe data, events, and failing inputs alike, including re-probing a parked input to see whether it recovered.

`stateChangeNotify` is a latch set on entry to such a state (or when the set of affected inputs changes) and cleared once the consumer has been notified, so a single failure wakes the consumer exactly once and does not busy-loop while the failure is outstanding. Callback is never invoked from `packetReceived` and never while any internal lock is held. The "ready" meaning is phase-dependent: first sample while synchronizing, one full block while synchronized. Blocked reads with a timeout are woken through the same path plus a condition variable.

`getAvailableCount` and the read methods process pending inputs synchronously — correctness never depends on the scheduler having run the coalesced task.

### 3.6 `DataLossMonitor` (new)

`setDataLossTimeout(t)`; `0` disables (default). A slot is **armed at the moment it becomes monitored** (used + connected + active), with its deadline set one full timeout ahead — arming is **not** deferred until the first packet. A first packet that never arrives is just as harmful as a producer that stops after delivering some, and both trip the same deadline. The timeout therefore also bounds a used input that is established (descriptors present) but delivers no data: it is reported `DataLost` at ladder step 9, rather than lingering in `WaitingForData` indefinitely. This is what lets a consumer's re-enabled-but-silent input — a probe of a producer that has gone quiet, whose queued data was dropped on re-enable — surface as a loss with no consumer-side timer. (An input that has not yet delivered even its initial descriptors precedes step 9 in the ladder and remains `WaitingForDescriptors`; a never-established input is not considered "lost". The initial descriptor event, once it arrives, refreshes the deadline, so the data timeout is measured from establishment.) Each arriving packet refreshes the slot's deadline to one timeout ahead. On expiry (scheduler-armed deadline; fires even with no further packets), the affected inputs form the lost set → `DataLost` state listing every lost input, and the reader raises the `onDataAvailable` callback (§3.5) so the consumer is notified of the loss without polling. The next packet from an input clears only that input; the reader leaves `DataLost` when the set is empty. Inactive, unused, and disconnected slots are not monitored; a slot re-arms from scratch each time it (re)enters the monitored set (e.g. after reconnect, reactivation, or `setInputUsed(true)`), so a stale pre-off arrival never counts toward a new deadline.

---

## 4. Domain and rate model

### 4.1 Common domain

For used input *i*: `DomainInfo_i = {epoch_i, resolution_i}` (from `DomainInfo::fromDescriptor`).

- `commonEpoch = min(epoch_i)` (earliest).
- `commonResolution = rational GCD of resolution_i = gcd(numerators) / lcm(denominators)`. This is *not* merely the finest input resolution: `1/10 s` and `1/15 s` require `1/30 s`. When all resolutions are integer multiples of the finest one, this reduces to the "finest resolution" rule. Every input tick then converts to the common domain by an integer multiplier — conversions are exact.
- The rational GCD additionally folds in `1/commonSampleRate`, so one output sample period is always a whole number of common ticks. With the LCM-derived rate this is a no-op; a *required* rate above every input rate refines the resolution (rates `{1000, 500}` with resolutions `1/1000` and required rate `2000` yield `commonResolution = 1/2000`). Input conversions stay exact.
- All computations use checked 64-bit arithmetic; overflow ⇒ `Incompatible` with a diagnostic, never wraparound.

`getOrigin()` returns `commonEpoch` (ISO 8601); `getTickResolution()` returns `commonResolution`.

### 4.2 Rates, dividers, block size

```
sampleRate_i        = resolution_i.den / (resolution_i.num * linearDelta_i)      (must be a positive integer)
commonSampleRate    = requiredCommonSampleRate  (if configured; every sampleRate_i must divide it)
                    = lcm(sampleRate_i)          (otherwise)
sampleRateDivider_i = commonSampleRate / sampleRate_i
blockLcm            = lcm(sampleRateDivider_i)
```

`blockLcm` is the minimum aligned read quantum: dividers `{2, 3}` require blocks of 6. Matches the existing `sampleRateDividerLcm`. The required-rate check validates `requiredCommonSampleRate % sampleRate_i == 0` (against the *rate*, not the divider — current code must be verified on this point). If `allowDifferentRates` is false, all rates must be equal.

### 4.3 `DomainValue` conversion and rounding policy

`toCommonDomain`/`fromCommonDomain` round to the nearest tick (branch behavior, kept). Policy:

1. Nearest-tick rounding is a *representability* device only. With the §4.1 common domain, signal→common is always exact; common→signal may round when the target does not fall on the signal's grid.
2. Synchronization correctness never relies on the rounded target: after each `advanceToDomainValue`, the **reached** value is converted back to the common domain and verified (§5). Mismatch triggers re-targeting, not silent acceptance.
3. A target whose signal-domain representation, converted back, differs from the intended common tick by more than one signal-domain tick period is reported as sync failure `TargetNotRepresentable`.

### 4.4 Output timeline

```
OutputTimeline {
    DataDescriptorPtr commonDomainDescriptor   // origin=commonEpoch, resolution=commonResolution,
                                               // linear rule with delta = commonSampleRate/outputRate ticks
    RatioPtr outputSampleRate                  // targetSampleRate if set, else main input's rate
    DomainValue nextOutputStart
}
```

The **main input** supplies the output grid identity (domain phase and default rate). Default: first used input in slot order; selectable via `setMainInput`. A disconnected selected main input yields `WaitingForConnections`; it is never silently replaced.

---

## 5. Synchronization algorithm

Aligning all inputs to the main input's first timestamp cannot work when a non-main input starts later than the main input. The algorithm therefore separates the *grid* (from the main input) from the *start* (from all inputs):

1. **Preconditions** (from the state evaluation, §6): reader active; all used inputs connected, locally valid, cross-compatible; no pending events; every used input has ≥ 1 unread sample.
2. Get each used input's first-sample `DomainValue`; convert all to the common domain (exact).
3. `span = latest − earliest`, in seconds. If `maxSynchronizationDistance > 0` and `span` exceeds it → `SynchronizationFailed`; status lists every input farther from the latest start than the threshold, with per-input timestamps and differences. Synchronization failure **does not** deactivate the reader (removes today's `setActive(false)` side effect).
4. **Candidate start** = latest first-sample, rounded **up** on the common grid: to a full domain unit when `startOnFullUnitOfDomain`, else to interval `Ratio(blockLcm, commonSampleRate)` (direct path) or the output sample period (resampled path), using `DomainValue::roundUpOnDomainInterval`.
5. For every used input: `target_i = candidate.fromCommonDomain(domain_i)`; call `advanceToDomainValue(target_i)` → `AdvanceOutcome`.
6. Combine outcomes:
   - any `DomainChanged` → events pending → `EventPending` (candidate discarded);
   - any `NeedMoreData` → keep candidate, state `Synchronizing`; retry on next packet;
   - any `OvershotError` (first sample already past target) → recompute from step 2;
   - all `Success` → convert each `reachedValue` to the common domain; if all equal the candidate → **`Synchronized`**, `commonStart = candidate`; otherwise set candidate = max(reached values) rounded up per step 4 and repeat from step 5.
7. Iteration bound (default 8). Exceeding it means the signals share no common tick (e.g. delta-2 signals on odd/even ticks) → `SynchronizationFailed` with reason `NoCommonTick`. (The legacy `tickOffsetTolerance` "shift signals onto the same tick" behavior is not reimplemented; deprecated, §9.)

Invalidation: any returned event, disconnect, descriptor change, main-input change, target-rate change, used-set change, or activation change clears `commonStart` and all configured read pipelines; the next evaluation re-runs setup (§6) and, after success, `ReadCoordinator::configure` rebuilds direct/resample pipelines. Resamplers are therefore rebuilt after every resynchronization — no separate resampling recovery flow exists.

---

## 6. Runtime state machine

### 6.1 States

There is deliberately **no recovery-options enum** in the API: the sensible actions follow from the state, the affected inputs, and the message. Function blocks know what to do when inputs fail to synchronize or lack data (mark them unused via `setInputUsed(false)`, or deactivate the reader); application software can additionally present `getStateMessage` to the user. The "expected handling" column below is documentation, not API surface.

| State | Meaning | Read result | Expected handling |
|---|---|---|---|
| `Inactive` | Disabled via `setActive(false)` | `Ok`, count 0 | activate when reading should resume |
| `WaitingForConnections` | A used input has no signal | `Ok`, count 0 | connect a signal / `setInputUsed(false)` / deactivate |
| `WaitingForDescriptors` | A used input lacks value or domain descriptor | `Ok`, count 0 | wait / reconnect |
| `Incompatible` | Local or cross-input validation failed (recoverable) | `Fail`, count 0 | fix descriptors / `setInputUsed(false)` / deactivate |
| `WaitingForData` | Valid, but some input has no samples | `Ok`, count 0 | wait |
| `Synchronizing` | Alignment in progress (`NeedMoreData`) | `Ok`, count 0 | wait |
| `Synchronized` | Aligned blocks readable | `Ok`, data | — |
| `EventPending` | Event(s) must be returned before data | `Event`, count 0 | read again (resync is automatic) |
| `SynchronizationFailed` | Span/representability/common-tick failure | `Fail`, count 0 | reconnect / `setInputUsed(false)` / deactivate |
| `DataLost` | A used input missed its packet deadline | `Fail`, count 0 | restore the stream / `setInputUsed(false)` |
| `Error` | Internal invariant violated / disposed | `Fail`, `getValid()==false` | recreate the reader |

`getValid()` is `false` only in `Error` (and after dispose). `getIsSynchronized()` ⇔ state `Synchronized`. All other conditions are recoverable in the same reader instance — no sticky `invalid` for descriptor/domain problems (today both `MultiReaderImpl::invalid` and `SignalReader::invalid` are sticky).

### 6.2 Fixed evaluation order

`evaluateState()` runs on: construction, connect/disconnect, coalesced packet task, data-loss deadline, config change (`setActive`, `setInputUsed`, `setMainInput`, `setTargetSampleRate`, add/remove input), and every public availability/read call.

| # | Check | On failure |
|---|---|---|
| 1 | Reader active? | `Inactive` |
| 2 | Resolve used set and main input | continue |
| 3 | Every used input connected? | `WaitingForConnections` |
| 4 | Refresh pending queues; leading events consumed | continue |
| 5 | Any pending event on a used input? | clear sync; `EventPending` |
| 6 | Every used input has both descriptors? | `WaitingForDescriptors` |
| 7 | Every used input locally valid (`QueueReader::isValid`)? | `Incompatible` (issues listed) |
| 8 | Cross-input compatibility; (re)build `CommonModel` + `OutputTimeline` | `Incompatible` |
| 9 | Any used input past its data-loss deadline? | `DataLost` |
| 10 | Every used input has ≥ 1 sample? | `WaitingForData` |
| 11 | Run §5 alignment | `Synchronizing` / `SynchronizationFailed` / `EventPending` |
| 12 | Configure read pipelines (direct/resamplers) | `Incompatible` |
| 13 | ≥ 1 complete block available? | `Synchronized` (else remain, waiting for data) |

Step 13's block check gates only the `dataAvailable` callback and `getAvailableCount`, not the `Synchronized` state itself once alignment succeeded.

---

## 7. Read and event contract

### 7.1 Availability

`getAvailableCount` returns 0 unless `Synchronized`. Direct path:

```
availableCommon = min_i( samplesUntilEvent_i * divider_i )
count           = floor(availableCommon / blockLcm) * blockLcm
count           = 0                        if count < max(minReadCount, blockLcm)
```

Resampled path: minimum over inputs of complete output samples producible before each input's event boundary (via `IResampler::getRequiredSourceRange`). Availability never mutates queues.

### 7.2 Read / readWithDomain / skipSamples

A call returns **either data or events, never both**.

1. `count == 0` (zero-count read): returns immediately with the current status; if events are pending they are returned (`ReadStatus::Event`). This is the descriptor handshake used after construction and after `dataAvailable`; it is kept.
2. `count != 0` and `count < minReadCount` → `OPENDAQ_ERR_INVALIDPARAMETER` (unchanged).
3. If `EventPending`: pop **one** event per affected input; fill the compat dict (`port globalId → event packet`) and the ordered event list (§8.2); return `Event`, count 0. Synchronization is already invalidated; the next call revalidates and resynchronizes.
4. If `Synchronized`: requested count is rounded down to `blockLcm` (direct) — `read`, `readWithDomain`, and `skipSamples` all use the same rounding (skip today bypasses it — fixed); plan; commit; return `Ok`, actual count, and the status offset.
5. Timeout (`timeoutMs`): wait on the notification path until the request is satisfiable, an event arrives, or the deadline passes; then perform steps 3–4 with whatever is available. `ReadTimeoutType` semantics: `All` (the only currently honored mode) — documented as such; the builder value is validated and stored.
6. If an event lies mid-request: return all complete blocks before it (`Ok`); the trailing partial block is discarded silently at the moment the event becomes the head (§3.1); the *next* call returns the original event.

Buffer contract (unchanged shape): jagged arrays ordered by slot. Direct path: segment *i* holds `count / divider_i` samples. Resampled path: every segment holds `count` samples. Unused inputs: buffer pointer may be null; they contribute nothing.

### 7.3 Domain output

- **Direct path:** `readWithDomain` writes each signal's **own** domain values (current observable behavior, preserved for compatibility). Absolute time per signal = its origin + tick × its resolution.
- **Resampled path:** all inputs share the output grid; domain buffers receive **common-output-grid** ticks (`commonDomainDescriptor` interpretation) and are identical across resampled inputs.

### 7.4 Status offset

`IReaderStatus::getOffset` (on the returned status) and `IMultiReader::getOffset(void*)` are the **common-domain tick of the first sample of the last successful read** (or of the synchronized start before the first read). Interpretation: `time = getOrigin() + offset * getTickResolution()`. The current implementation returns the first signal's signal-domain packet offset, which is wrong whenever that signal's resolution or epoch differs from the common one; `getOffset(void*)` is currently a commented-out no-op on the branch — both are reimplemented against the common domain.

### 7.5 Event ordering and payload

- Per input, events are returned in queue order; consecutive descriptor changes arrive pre-merged; gaps are separate entries in order.
- `IReaderStatus::getEventPacket` returns the first event (compat). `IMultiReaderStatus::getEventPackets` returns the dict keyed by port global id (compat; one event per input per call). The new ordered list (§8.2) exposes everything, including multiple queued events per input across consecutive calls.
- Every returned event invalidates synchronization; data never resumes without realignment.

---

## 8. Public API

The complete per-method error contract — which error codes each public method can return and when, including the two new reader-family codes for resampler implementations — is specified in `05_error_contract.md`. Its governing rule: error codes signal call failures (null arguments, invalid parameter values, unknown ids); data-stream conditions (unsynchronized, incompatible, events, data loss) are always reported through the status and state machine with a success-class return.

### 8.1 Unchanged (must keep working)

`IMultiReader`: `read`, `readWithDomain`, `skipSamples`, `getTickResolution`, `getOrigin`, `getOffset`, `getIsSynchronized`, `getCommonSampleRate`, `setActive`/`getActive`, `addInput`/`removeInput`, `setInputUsed`/`getInputUsed`; `ISampleReader`/`IReader` methods; `IReaderConfig` (`markAsInvalid` maps to `Error`); `IMultiReaderBuilder` (all existing setters/getters); factories `MultiReader`, `MultiReaderEx`, `MultiReaderFromBuilder`.

`getCommonSampleRate` returns the resolved output rate. On the resampled path this equals `targetSampleRate` (as an integer if representable; see `getTargetSampleRate` for the exact ratio).

### 8.2 Additions (additive only)

`IMultiReaderBuilder` + `IMultiReader` (runtime-changeable where noted):

```
setMainInput(IString* inputId) / getMainInput(IString** )          // runtime too; empty → first used input
setTargetSampleRate(IRatio* rate) / getTargetSampleRate(...)       // runtime too; unset → main input rate
setResamplerBuilder(IResamplerBuilder*) / getResamplerBuilder(...) // default: LinearResamplerBuilder
setMaxSynchronizationDistance(IRatio* seconds) / get...            // BUILDER-ONLY (C1); 0 disables (default 0)
setDataLossTimeout(IRatio* seconds) / get...                       // BUILDER-ONLY (C2); 0 disables (default 0)
```

Per review comments C1/C2 the synchronization distance and data-loss timeout exist only on
`IMultiReaderBuilder` — the reader has neither setters nor getters. Changing them at runtime
means rebuilding the reader (the documented dispose-and-rebuild pattern).

`IMultiReaderStatus` additions (existing `getEventPackets`, `getMainDescriptor` retained):

```
enum class MultiReaderState : EnumType { Inactive, WaitingForConnections, WaitingForDescriptors,
    Incompatible, WaitingForData, Synchronizing, Synchronized, EventPending,
    SynchronizationFailed, DataLost, Error };

getState(MultiReaderState*)
getStateMessage(IString**)                              // human-readable diagnostics
getAffectedInputCount(SizeT*) / getAffectedInputIndex(SizeT statusIndex, SizeT* inputIndex)
getEventCount(SizeT*) / getEvent(SizeT eventIndex, SizeT* inputIndex, IEventPacket** packet)
```

Named `getStateMessage` to avoid colliding with any base `getMessage`. Structured status stays minimal — state plus affected input indices; detailed diagnostics go into the message. Recovery actions are inferred by the caller: function blocks act on the state and affected inputs (`setInputUsed(false)`, `setActive(false)`); applications can additionally parse or display the message. Status objects are cached and re-issued while visible content is unchanged; normal data reads return the cached instance.

`getMainDescriptor` semantic clarification: the returned descriptor-changed event packet carries the value descriptor of the main input and the **common output domain descriptor** (origin/resolution/rule of §4.4) — the domain in which `getOffset` and resampled `readWithDomain` values are expressed. It is no longer documented as "descriptor of the first signal".

### 8.3 Resampling interfaces (new, RTGen)

```
struct ResamplerBuildContext {           // rt struct or params object
    DataDescriptorPtr inputValueDescriptor, inputDomainDescriptor;
    DataDescriptorPtr commonDomainDescriptor;
    SampleType outputValueType;
    RatioPtr  outputSampleRate;
};

DECLARE_OPENDAQ_INTERFACE(IResampler, IBaseObject)
{
    // Source interval [first, last] of signal-domain ticks needed to produce
    // outputCount samples starting at outputStartTick on the output grid.
    ErrCode getRequiredSourceRange(Int outputStartTick, SizeT outputCount, Int* firstNeeded, Int* lastNeeded);

    // Consume sourceWindow samples (values+domain), write outputCount samples on the grid.
    ErrCode resample(const ResampleRequest*, ResampleResult*);

    ErrCode reset();                     // clear interpolation history
};

DECLARE_OPENDAQ_INTERFACE(IResamplerBuilder, IBaseObject)
{
    // Repeatable: each call returns a new independent resampler; must not consume the builder.
    ErrCode build(const ResamplerBuildContext*, IResampler** resampler);
};
```

(Exact RTGen-compatible parameter shapes — plain structs vs. property objects — are decided in implementation Phase 5; the contract above is normative.)

Rules:

- One reusable builder on the reader; called once per input needing resampling, at every `configure`.
- Direct copy iff the input's grid (epoch offset, resolution, rate, phase) coincides exactly with the output grid; then `resampler == null`.
- If `targetSampleRate ≠ main input rate`, the main input is resampled too.
- **No interpolation across an event or gap** — a resampler only ever sees a `SourceWindow` that ends before the next event boundary, and it is `reset`/rebuilt after every resynchronization.
- Default `LinearResamplerBuilder`: linear interpolation, requires numeric scalar values and bracketing source samples; rejects unsupported descriptors at `build` (→ `Incompatible`).
- A rejected/failed build → `Incompatible` naming the input; user may inject a different builder or mark the input unused.

### 8.4 Deprecations and removals

| Item | Action |
|---|---|
| `MultiReaderFromExisting` factory + `MultiReaderImpl(MultiReaderImpl* old, …)` | **Removed** (sanctioned exception). Recovery happens in the same instance; the state machine makes the invalidate-and-transfer pattern obsolete. |
| `setTickOffsetTolerance` / `getTickOffsetTolerance` | **Deprecated**, kept on the builder for ABI. Value is ignored (logged warning). Superseded by `setMaxSynchronizationDistance` + `NoCommonTick` failure reason. |
| `readTimeoutType` | Kept; only `All` semantics documented and honored. |

### 8.5 Behavior changes (compatibility table)

| Change | Old | New |
|---|---|---|
| Notification default for port-constructed readers | `Scheduler` | `SameThread` (bounded `packetReceived`); explicit builder settings still override |
| User callback thread | could run on producer thread inside `packetReceived` | always via coalesced scheduler task, outside locks |
| Sync failure side effect | reader set inactive | reader stays active in `SynchronizationFailed` |
| Descriptor problem | sticky `invalid`, `getValid()==false` forever | recoverable `Incompatible`; `getValid()==false` only in `Error` |
| Status offset | first signal's signal-domain packet offset | common-domain tick |
| Partial block at event | (old impl: undefined; branch: synthetic sync-GAP event) | silent discard, original event only |
| `skipSamples` alignment | not blockLcm-aligned | same rounding as `read` |
| Status object identity | new object per read | cached while unchanged |

---

## 9. Locking and threading

1. Lock order: `MultiReaderImpl` state mutex → `QueueReader` mutexes in ascending slot order. Never reversed.
2. No user callback (`onDataAvailable`, external `IInputPortNotifications`) while holding any internal lock.
3. Producer-thread work is bounded to `InputSlot::packetReceived` (§3.2).
4. Coalescing: at most one evaluation task scheduled; the task drains pending bits, re-checks after processing, and re-schedules itself if bits were set concurrently.
5. `DataLossMonitor` deadlines fire on the scheduler; they enter the same evaluation path as packets.

---

## 10. Acceptance criteria

1. Ownership graph of §2 matches the implementation; only `QueueReader` moves a cursor; `SignalReader`/`ReaderDomainInfo`/`Comparable`/`TypedReader`/`ReadInfo` are gone from the multi path.
2. `blockLcm = LCM(dividers)`; dividers `{2,3}` read in blocks of 6; availability, `read`, `readWithDomain`, `skipSamples` share identical alignment.
3. Synchronization: grid from main input, start from latest input; iterative re-target converges or reports `NoCommonTick`; reached values verified in the common domain; distinct epochs and resolutions `1/10 + 1/15` align exactly on a `1/30` grid.
4. Every event from a used input is returned before any post-event data, always invalidates synchronization, and never crosses a read; trailing partial blocks are silently discarded with no synthetic event and no dropped count.
5. Gap events never merge; consecutive descriptor changes merge keeping the newest descriptors; ordering is preserved.
6. Every failure state in §6.1 is recoverable per its expected handling in the same reader instance; `getValid()==false` only in `Error`.
7. `packetReceived` performs no dequeue, parsing, synchronization, or user callback; callbacks are coalesced and lock-free; reads don't depend on scheduler timing.
8. Read planning validates every used input before any cursor commits; a partial commit is impossible.
9. Resampling: builder called only at configure; per-input independent resamplers; no interpolation across events; rebuilt after every resync; direct copy auto-detected; same output count for every used input on the resampled path.
10. Public API: all §8.1 methods keep signatures and documented semantics (modulo §8.5); additions are purely additive; `MultiReaderFromExisting` is removed; existing status accessors still populate.
11. Status: state, message, affected inputs, and ordered events all populated per §6.1/§8.2; status instances cached while unchanged.
12. `getOffset` (both forms) returns the common-domain tick and round-trips to absolute time via `getOrigin`/`getTickResolution`.
