# Multi Reader — architecture and specification

This document is the onboarding reference for the openDAQ **multi reader**: what it does, the
components it is built from, the states it moves through, and the rules of its data plane. It is
meant to get a developer productive on the reader and the recent rework without reading every line
of source first.

> Scope: the multi reader lives in `core/opendaq/reader`. The public facade is
> `MultiReaderImpl` (`src/multi_reader_impl.cpp`); the internals live in the `daq::multi_reader`
> namespace under `src/multi_reader/` and `include/opendaq/multi_reader/`.

---

## 1. What the multi reader is

The multi reader reads **time-synchronized** samples from several signals at once. Each signal is
connected through an input port; the reader aligns all inputs onto one common domain grid and hands
the caller a block of samples that starts at the same domain instant on every input.

A consumer:

1. builds a reader over N signals/ports (`MultiReaderBuilder`),
2. optionally sets an `onDataAvailable` callback,
3. calls `read` / `readWithDomain` (or `skipSamples`) to pull aligned blocks, inspecting the
   returned `IMultiReaderStatus` after each call,
4. reacts to non-`Ok` statuses (events, failures) and reads again.

Reads are **block-aligned**: the reader only ever returns whole multiples of a block (see
`blockLcm`, §7), and every input advances by the same common-domain span.

---

## 2. Public API surface

`MultiReaderImpl` implements `IMultiReader`, `ISampleReader`/`IReader`, and `IReaderConfig`.

**Reading**
- `read(samples, count, timeoutMs, status)` — read `*count` common-rate samples into per-input
  buffers; on return `*count` is what was delivered.
- `readWithDomain(samples, domain, count, timeoutMs, status)` — as above, also filling domain
  buffers.
- `skipSamples(count, status)` — advance without copying values.
- `getAvailableCount(count)` — how many common-rate samples can be read right now.
- `getEmpty(empty)` — whether any used input currently has no data.

**Callback**
- `setOnDataAvailable(callback)` — invoked (off the caller thread) when data or an event becomes
  available to read. See the callback gate in §8.
- `setExternalListener(listener)` — forwards port notifications to an external listener.

**Model / domain queries**
- `getIsSynchronized`, `getCommonSampleRate`, `getTickResolution`, `getOrigin`, `getOffset`.

**Topology / lifecycle**
- `setActive` / `getActive` — pause/resume the whole reader.
- `addInput` / `removeInput` — add or drop an input.
- `setInputUsed` / `getInputUsed` — include or exclude one input from reading (it stays observable).
- `setMainInput` / `getMainInput` — pick the input whose domain defines the common grid.

**Read parameters** (set at build time): value/domain read types, `ReadMode`, `ReadTimeoutType`
(`Any` / `All`), `requiredCommonSampleRate`, `startOnFullUnitOfDomain`, `minReadCount`.

---

## 3. The read contract

Every `read` returns an `IMultiReaderStatus`. The consumer must branch on it:

- `getReadStatus()` → a `ReadStatus` (see §4).
- `getInputStates()` → a dictionary keyed by input id → `InputState` (see §5). This is how a
  consumer discovers *which* input is at fault and *why*.
- `getEventPackets()` → on an `Event` status, the event packets keyed by input id.
- `getOffset()` → the common-domain tick of the first returned sample.

**Timeouts.** `timeoutMs == 0` returns immediately with whatever is available. With a timeout and
`ReadTimeoutType::All`, the call blocks until the whole request is servable or an event arrives.
A zero-`count` call is a handshake: it reports events / current state without consuming data
(optionally waiting for an event with a timeout).

**Alignment / minimum.** A request is rounded **down** to whole blocks; a read below the effective
minimum (`max(minReadCount, blockLcm)` rounded up to whole blocks) returns nothing. Availability
always stops before the earliest pending event on any input.

---

## 4. Reader states

`ReaderState` (`multi_reader_impl.h`) is the **internal** state machine. It is not exposed
directly; it is summarized into the public `ReadStatus` returned from a read.

| `ReaderState` | Meaning | Public `ReadStatus` |
|---|---|---|
| `Inactive` | Disabled via `setActive(false)` | `Inactive` |
| `WaitingForConnections` | A used input has no signal connected | `Preparing` |
| `WaitingForDescriptors` | A used input has not received its descriptors yet | `Preparing` |
| `WaitingForData` | Valid, but some input has no samples yet | `Preparing` |
| `Synchronizing` | Alignment in progress, waiting for data to reach the aligned start | `Preparing` |
| `Synchronized` | Aligned blocks are readable | `Ok` |
| `EventPending` | Event(s) must be returned before any more data | `Event` |
| `Incompatible` | Local or cross-input validation failed (recoverable on new descriptors) | `InputsFailed` |
| `SynchronizationFailed` | Span / representability / common-tick alignment failure | `InputsFailed` |
| `DataLost` | A used input missed its packet deadline | `InputsFailed` |
| `Error` | Internal invariant violated or reader disposed — **not** recoverable | `Fail` |

Notes:
- `Synchronizing` and the three `Waiting*` states all read out as `Preparing` — "nothing is wrong,
  no data yet."
- `Incompatible`, `SynchronizationFailed`, `DataLost` are **recoverable**: the reader keeps running,
  buffered pre-failure data stays readable, and recovery happens per input (a new descriptor, a new
  packet, or excluding the offending input). Only `Error` is terminal.
- The full state evaluation is `evaluateStateLocked` (see §8); it is a top-to-bottom ladder that
  picks the first applicable state each time it runs.

---

## 5. Per-input states

`InputState` (`multi_reader_status.h`), returned per input id by `getInputStates()`:

| `InputState` | Meaning |
|---|---|
| `Ok` | Contributing aligned samples |
| `Pending` | Connected but not contributing yet (connect / descriptors / first data / alignment in progress) |
| `Event` | Unconsumed event(s) on this input (used or unused) |
| `Incompatible` | Descriptor or cross-input validation failed (recoverable on new descriptors) |
| `SynchronizationFailed` | Synchronization distance or common-tick failure |
| `DataLost` | Missed its packet deadline |
| `Unused` | Excluded from reading via `setInputUsed(id, false)` |

The per-input states are the actionable detail behind an `InputsFailed`/`Event` read status: a
consumer looks here to decide which input to exclude, re-enable, or wait on.

---

## 6. Components and responsibilities

The facade owns one instance of each component and is the only thing that holds the state mutex.

| Component | File | Responsibility |
|---|---|---|
| **`MultiReaderImpl`** (facade) | `multi_reader_impl.*` | Public API, input ordering, the `ReaderState` machine (`evaluateStateLocked`), the data-plane paths (§8), and status creation. Holds the single state mutex. |
| **`Input`** (slot) | `multi_reader/input.*` | One input port + its `QueueReader`. Receives port notifications on the producer thread, holds `packetPending`, and reports connect/disconnect/packet up to the facade via `IInputListener`. |
| **`QueueReader`** | `multi_reader/queue_reader.*` | Per-input queue over one connection: adopts packets (`drain`), tracks value/domain descriptors and events, computes available samples, and executes the actual value/domain copy on `read`/`skip`. |
| **`SynchronizationManager`** | `multi_reader/synchronization_manager.*` | All cross-input math: builds the `CommonModel` (common sample rate, per-input dividers, `blockLcm`, tick resolution) and aligns every input to a common start tick. |
| **`ReadCoordinator`** | `multi_reader/read_coordinator.*` | Availability, planning, and committing a read/skip under one set of alignment rules. Produces a `ReadPlan` and executes it across all inputs; a partial commit is impossible by construction. |
| **`NotificationCoordinator`** | `multi_reader/notification_coordinator.*` | Coalesces producer wake-ups into at most one scheduled evaluation, and holds the used/ready/event bit masks that decide whether `onDataAvailable` fires. |
| **`DataLossMonitor`** | `multi_reader/data_loss_monitor.*` | Per-input packet deadlines. Its timer thread requests an evaluation when a deadline is crossed; the facade decides when a crossed deadline becomes the `DataLost` state. |

The facade constructor wires these together; naming convention: a method suffixed `...Locked`
requires the caller to already hold `mutex`.

---

## 7. Synchronization model

`SynchronizationManager::buildCommonModel` produces a `CommonModel`:

- `commonSampleRate` — the common rate all inputs are expressed in (respecting a caller-set
  `requiredCommonSampleRate` when given).
- `sampleRateDividers[i]` — input `i` runs at `commonSampleRate / divider_i`.
- `blockLcm` — the LCM of all dividers: the smallest common-rate count that is a whole number of
  native samples on **every** input. All reads are multiples of `blockLcm`.
- `commonDomain` (origin, resolution) and `ticksPerCommonSample`.
- `commonStart` — the aligned start tick (assigned only once synchronized).

**Counts are common-rate equivalents.** A read of `commonCount` delivers `commonCount / divider_i`
native samples into input `i`'s buffer. Because `blockLcm` is a multiple of every divider, an
aligned `commonCount` divides evenly on every input.

`synchronize` advances each input to the aligned start; the reached value is verified against the
requested target rather than trusting tick rounding. Acceptance of a reached value is bounded to
strictly less than half an aligned block interval.

---

## 8. The data plane: three paths

Historically one function (`refreshDataPlaneLocked`) served every caller. It is now a single
parameterized routine driven by three **purpose-built entry points**, because they need different
things:

| Path | Entry point | Job | Runs the state ladder? |
|---|---|---|---|
| **NOTIFY** | `onCoalescedEvaluation` (scheduler thread) | Decide whether `onDataAvailable` should fire | No, for events — only maintains bits |
| **QUERY** | `getAvailableCount` | Report how much is readable now | No, for events |
| **READ** | `readInternal` (`read`/`readWithDomain`/`skipSamples`) | Deliver data or surface events | Yes — owns event handling |

`refreshDataPlaneLocked(escalateOnEvent)` is the shared core. While `Synchronized`, it drains the
slots that received packets, maintains the readiness/event bits, and:

- **`escalateOnEvent == true`** (READ / QUERY / timed-read predicate): on any event it escalates to
  the full `evaluateStateLocked`, which transitions to `EventPending`, discards partial segments,
  and surfaces the event.
- **`escalateOnEvent == false`** (NOTIFY): on any event it only records the event bit for the
  callback gate — **buried-inclusive** (`hasPendingEvents() || hasQueuedEventPackets()`), so a
  sub-block residual before a buried event still fires the callback — and re-arms `dataPlaneDirty`
  so the next READ/QUERY runs the ladder and surfaces the event. It never runs the ladder for
  events.

In both modes, a **data-loss deadline** or a non-synchronized state escalates to the full ladder;
unused inputs' queued events are still drained so they surface in the per-input states (this is the
recovery signal a consumer answers with `setInputUsed(id, true)`).

**Why the split.** Running the whole state ladder on the scheduler thread for every event arrival is
wasteful and contends with reads. The heavy event processing belongs at READ, where events are
actually consumed. NOTIFY only answers "should I wake the consumer?"; QUERY only answers "how much?";
READ is the single authority that transitions state and surfaces events.

**Why READ still owns events without its own escalation code.** The producer sets `dataPlaneDirty`
on every packet, and NOTIFY re-arms it whenever it records an event it did not process. So the
read-side refresh always does a full pass (and escalates) while an event is pending. The callback
gate is independent of `ReaderState` — it reads only the bit masks — so deferring the state
transition does not affect when the callback fires.

**`getAvailableCount` event guard.** Because the query no longer transitions to `EventPending`, it
guards the one count that would otherwise be wrong: a **leading** pending event on any used input
means no synchronized read can proceed until it is handled, but `getAvailableSamplesUntilEvent`
counts the data queued *behind* that event (the event has moved to a separate queue). So
`getAvailableCount` returns 0 whenever any used input has a pending event. Buried events need no
guard — the count naturally stops at them.

---

## 9. Key mechanisms

### 9.1 Producer path and `clear-then-drain`

Packet delivery is **lock-free**: `Input::packetReceived` (producer thread) sets the input's
`packetPending` atomic and then sets `dataPlaneDirty`, and schedules the coalesced evaluation. It
never takes the state mutex.

Consumers adopt queued packets by **clearing `packetPending` before draining**, never after. A
packet that arrives after the clear re-arms the flag and is caught on the next pass (at-least-once);
clearing after a drain could strand a packet enqueued in the drain→clear window with the flag
already reset — that was the "availability undercount" race and is why the ordering is fixed.

### 9.2 Availability and planning (`ReadCoordinator`)

- `getAvailableCount(inputs, model, minReadCount)` = the min over inputs of
  `getAvailableSamplesUntilEvent`, floored to whole blocks, dropped to 0 below the effective
  minimum (`alignAvailable`).
- `createPlan(...)` rounds the request down to whole blocks and clamps to availability, producing a
  `ReadPlan` (a `commonCount` plus non-owning views of the caller's per-input buffers). An overload
  takes an already-computed availability so the read path does not walk every input twice.
- `commit` / `skip` execute the plan input by input. Because availability validated the whole plan
  under the held mutex and only the owner thread touches the queues, every input read must succeed;
  anything else is an `InternalError` → `Error`.

### 9.3 Events: leading vs buried

- A **leading** event sits at the front of an input; it must be returned (`EventPending`) before any
  more data on that input.
- A **buried** event sits behind data. The data in front is readable; the event surfaces on the read
  that consumes past it. When the data in front is a partial (sub-block) residual that can never be
  read on its own, `discardLeftoverSegments` silently drops it so the event becomes leading.

Event surfacing and `discardLeftoverSegments` happen only on the READ path (or the full ladder).

### 9.4 Readiness / event bits and the callback gate

`NotificationCoordinator` holds three per-slot bit masks — `used`, `ready`, `event`. The gate is:

```
shouldInvokeCallback = event.any() || (used.any() && (ready & used) == used)
```

i.e. fire when any input has an event (used or unused — the recovery signal) or when every used
input has a readable block. Readiness while synchronized means "a full aligned block before the next
event"; while synchronizing it means "the first sample." The owner sets these bits during the
data-plane pass; the gate never reads `ReaderState`.

### 9.5 Data loss (in-band)

Loss is reported **in band**: buffered pre-loss data stays readable (the producer went silent *after*
producing it). A crossed deadline only becomes `DataLost` once the affected input can no longer
contribute a whole block. The `DataLossMonitor`'s timer requests an evaluation on a deadline; the
facade re-checks on each evaluation while a loss is outstanding.

### 9.6 Availability cache

The synchronized fast pass publishes the per-slot availability and their aggregate minimum. The read
path reuses this to plan and to lower readiness by subtraction without walking every input again, and
a poll loop reuses the cached aggregate without re-draining. The cache is valid only between a
non-escalating synchronized fast pass and the next thing that runs: `evaluateStateLocked` (the single
funnel for every full evaluation) clears it, and a consuming read invalidates it. Any consumer falls
back to a direct walk when it is not valid, so correctness never depends on the cache.

### 9.7 Status caching

An event-less status is re-issued while its visible content is unchanged; any content change (or any
event) creates a new status object. The offset is deliberately not part of the content fingerprint —
it advances every read while the rest of the status stays constant in steady state — so a cached
status is either re-issued as-is or has its offset-independent content shared into a new object
stamped with the advanced offset.

---

## 10. Lifecycle and topology

- **Construction** wires the components and runs an initial evaluation. Initial descriptor events
  arrive while the connection is still being built, so connections are re-synced from the ports
  themselves inside `evaluateStateLocked`.
- **Connect / disconnect** (`slotConnected` / `slotDisconnected`) rebind the input's queue and
  re-evaluate.
- **`setActive(false)`** stops data flow (events still enqueue) and reports `Inactive`.
- **`setInputUsed(id, false)`** excludes an input from reading but keeps it observable: its port is
  deactivated (so only events arrive), its data-loss monitoring is disarmed, and it reports `Unused`
  or `Event`. Re-enabling drops stale buffered data so a queued corrective descriptor can surface.
- **`setMainInput(id)`** selects the input whose domain defines the common grid.
- **`addInput` / `removeInput`** change the input set and re-index the slots.

---

## 11. Known limitations and future work

Captured here so they are not lost (they were previously inline `// COMMENT:` notes):

- **Resampling (Phase 5).** Only the direct-copy path exists today; inputs whose grid differs from
  the output grid are not resampled. `ReadCoordinator::configure` is where per-input pipeline
  selection (direct copy vs. a built resampler) will go.
- **`MultiReaderFromExisting` removal (Phase 6).** The "reader from existing" constructor is
  deprecated and deliberately not migrated to the builder path; it is scheduled for removal.
- **Per-input domain output.** Producing domain values for every signal is likely unnecessary since
  all inputs are time-aligned to the common domain; the domain buffer / domain read type / domain
  transform may be reducible to a single common-domain output.
- **Value/domain transform functions.** Whether the per-input transform functions are still needed
  (and where they should apply, if at all) is open.
- **`ReadMode` clarity.** The relationship between read mode and read types is under-specified
  (`Unscaled` in particular); it should be clarified or reworked.
- **`TypedReadingUtils`.** The two-type copy/convert dispatch is powerful but hard to read; it could
  be simplified, ideally by removing type combinations the multi reader never uses.
- **Bindings.** Python/C#/C bindings are generated and are OFF in the local build; the extended
  `ReadStatus`, the status object, and the builder interface changes need a bindings regeneration.
- **Power function block.** The power FB is another multi-reader consumer and needs its own migration
  to the reworked reader (out of scope here).

---

## 12. Build and test

- Build the reader tests (Debug): `cmake --build build/x64/msvc-22/full --target test_reader
  --config Debug -- -m`, then run `build/x64/msvc-22/full/bin/Debug/test_reader_debug.exe`.
- The multi-reader unit suites are `MultiReaderTest`, `QueueReaderTest`, `ReadCoordinatorTest`,
  `SynchronizationManagerTest`, `NotificationCoordinatorTest`, `DataLossMonitorTest`, and
  `MultiReaderInputTest`.
- A standalone micro-benchmark harness (`tests/bench_multi_reader.cpp`, excluded from the default
  build) measures throughput/latency/per-call costs against the public API.
