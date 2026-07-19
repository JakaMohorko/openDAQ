# Multi Reader Rework — Review Comments: Evaluation and Action Plan

**Status: DRAFT — covers the comment sets (commits `343fe1ae` and `a6f14914`), the accompanying
notes, and the design discussion of 2026-07-19 (Q1–Q5 resolved — §4; the `stateDirty` draft
superseded by per-slot `HasData`/`HasEvent` — §2.1; data loss made in-band — §2.6). More comments
are expected; no code changes are made until the full set is in.** Architecture background and the
verified facts referenced here live in `08_internal_architecture.md` (§6 "Known hot spots" in
particular).

Numbering: C1–C12 are the inline `// COMMENT:` markers from `343fe1ae`; C13–C15 are the markers
from `a6f14914`; N1–N7 are the notes from the accompanying message; S1 is the slot-reconstruction
note.

---

## 1. Comment-by-comment evaluation

### C1 + C2 — `setMaxSynchronizationDistance` / `setDataLossTimeout` should be builder-only

**Agree.** They are synchronization-defining configuration, like `allowDifferentSamplingRates`;
runtime mutation buys little and costs a full resync anyway.

Action: remove the four methods (setters + getters) from `IMultiReader` and `MultiReaderImpl`;
the builder keeps `setMaxSynchronizationDistance`/`setDataLossTimeout` (+ getters). Runtime change
becomes "rebuild the reader", which consumers already do for mode changes.
Impact, verified: 13 call sites in `test_multi_reader.cpp`/`test_synchronization_manager.cpp` move
to builder configuration; the sum FB's `applyReaderConfigLocked` forwarding is replaced by a reader
rebuild on property write; spec §8.2 and `05_error_contract.md` §3.3 updated; checked-in bindings
lose the methods at the next regeneration.
Resolved (Q3): no getters on the reader — the builder is the only configuration surface.

### C3 — `IInputPortNotifications` implemented on both the reader and the slots

**Agree — it is vestigial, and removable.** Verified: `createSlots` always binds the *slot* as the
port listener (`port.setListener(slotObject)`); no port ever has the facade as listener; no code in
the repo invokes the facade's `acceptsSignal`/`connected`/`disconnected`/`packetReceived`; the
methods only forward to the external listener, duplicating forwarding that
`slotConnected`/`slotDisconnected`/`slotPacketReceived` already do. It survived the rework as a
compatibility leftover from the pre-rework design where the reader itself was the port listener.

Action: drop `IInputPortNotifications` from `MultiReaderImpl`'s interface list and delete the four
forwarding methods. `setExternalListener(IInputPortNotifications*)` is unaffected (that type
belongs to the *consumer* side, e.g. the FB). Risk: anything `queryInterface`-ing a reader for
`IInputPortNotifications` — nothing in-repo does; the removal PR runs the full sweep to confirm.

### C4 — is `IInputSlotListener` necessary?

**Yes — keep, and document why.** Unlike C3 this surface is load-bearing: it is how a slot delivers
per-port callbacks *with its slot index* to the facade on the bounded producer path
(`slotPacketReceived`: monitor arming, coalesced evaluation request, condition wake), and it is the
point where external-listener forwarding is serialized to run outside the facade mutex. Ports can
have only one listener — that listener is the slot; the slot needs a channel upward; this is that
channel. Action: a short rationale comment block at the interface declaration (text from
`08_internal_architecture.md` §1/§5.6), no functional change.

### C5 — too many public states; compact to app/FB-actionable ones

**Agree — and resolved (Q1): no separate reader-state enum at all; `ReadStatus` is extended
instead.** The consumer-action space is: do nothing / wait / read again / `setInputUsed(id,false)`
/ `setActive` / recreate. Eleven states force every consumer into a switch over distinctions it
cannot act on (the sum FB's `handleStateLocked` is the live proof). And state is only ever
consumed through the universal read pattern — 1. read the data returned, 2. react to the status —
so it belongs on the status, not behind a separate reader-level getter (consistent with Q4).

`ReadStatus` (`reader_status.h`, today `{Ok = 0, Event, Fail, Unknown = 0xFFFF}`) gains three
appended values — existing numeric values are untouched, and single readers simply never return
the new ones:

| Status | Meaning | Consumer action |
|---|---|---|
| `Ok` | synchronized, data delivered | none |
| `Event` | this read hit an in-band event; data *before* it was returned in the same call | consult per-input states (C6), react, read again |
| `Preparing` *(new)* | nothing is wrong, but no data should be expected yet — waiting for connections / descriptors / first data / sync | none — wait |
| `Inactive` *(new)* | deliberately deactivated | `setActive(true)` when reading should resume |
| `InputsFailed` *(new)* | one or more used inputs failed — persistent until acted on | consult per-input states; `setInputUsed`/fix upstream |
| `Fail` | unrecoverable | recreate the reader |

- `Event` is transient (what *this read* encountered); the rest are persistent snapshots. After an
  `Event` read is handled, the next read reports the resulting persistent state: `Ok` after an
  applied descriptor change, `InputsFailed` after a data loss, `Preparing` during a triggered
  resync.
- `MultiReaderState`, `getState`, and `EventPending` leave the public API. `getValid()` (base
  `IReaderStatus`) becomes false **only** for `Fail` on the multi reader status — the sum FB work
  already proved that "invalid on recoverable states" misleads consumers into rebuilds.
- The fine-grained distinctions do not vanish — they move onto the inputs (C6), where they are
  actionable, and into the message, where humans read them.

Why `Preparing` earns its place (Q1 asked for a concrete example): the sum FB's
`handleStateLocked` cannot distinguish "sync in progress — be patient" from "sync failed — park
someone" without inspecting fine-grained states, and a wrong guess is exactly the probe-flapping
failure mode the `ProbeDoesNotFlap` test guards against. With `Preparing` the FB's do-nothing
branch is trivial: `Preparing → return` — never park, never probe, never warn while the reader is
still settling (reader rebuilds on mode switches and probe-triggered resyncs pass through this
window every time). Application-side: a recorder UI arming a measurement across several devices
shows "waiting for signals to align…" on `Preparing` instead of a spurious warning, and can attach
a startup timeout ("still `Preparing` after 10 s → report which input is `Pending`") without ever
misclassifying a real failure — `InputsFailed` stays unambiguous.

Naming: `Preparing` preferred over `Pending` here to avoid collision with the per-input `Pending`
(C6); final call at implementation time.

### C6 — drop the ordered event list; add per-input statuses; FB "ignore faulty inputs" property

**Agree on all three.**

- `getEventCount`/`getEvent` duplicate `getEventPackets` (verified consumers: 11 uses in
  `test_multi_reader.cpp`, no FB uses). Remove both; the dict remains the single event surface.
- `getAffectedInputCount`/`getAffectedInputIndex` are replaced by **per-input statuses** — strictly
  more information (every input, not just the affected set) in a directly actionable shape, and
  keyed by port global id instead of by slot index. Indices were a design mistake: they are exactly
  what `removeInput` shifts (see S1), so any retained index is stale after a topology change.

  Resolved shape (Q2): on `IMultiReaderStatus`,
  `getInputStates(IDict** statesByPortId)` → `port globalId → InputState`, with

  ```
  enum class InputState { Ok, Pending, Event, Incompatible, SynchronizationFailed,
                          DataLost, Unused }
  ```

  `Pending` = connected but not yet contributing (awaiting connect/descriptors/data/alignment) —
  no action possible. `Event` (replaces the draft's `UnusedWithEvents`, per Q2) = unconsumed
  event(s) on this input, used and unused alike: on a used input it accompanies
  `ReadStatus::Event`; on an unused input it is the recovery signal (§2.3). Deliberate trade-off:
  while an unused input has pending events its dict entry reads `Event`, not `Unused` — acceptable
  because the used/unused set is consumer-controlled knowledge (the consumer parked it; the dict
  tells it something happened there). Reader-level `InputsFailed` ⇔ at least one **used** input in
  `{Incompatible, SynchronizationFailed, DataLost}`. Per-input human-readable detail stays in the
  single `getStateMessage`.
- Sum FB: add a bool property (working name `IgnoreFaultyInputs`, default true = today's parking
  behavior) — when false, a failing input keeps the whole FB in Warning without being excluded.
  The FB already reports the failing set through its ComponentStatus message; it switches from
  affected-indices to the per-input dict, which also removes its slot-order/`readerPorts` mirror
  bookkeeping.

### C7 — status factory too large → builder object; reconsider fields

**Agree, with the expectation that compaction mostly solves it.** After C5/C6 the explicit fields
become: `eventPackets`, `mainDescriptor`, `offset`, `stateMessage`, `inputStates` (plus the
inherited `readStatus`/`valid`) — five pairs, comfortably inside the 8-pair RTGen macro limit.
Action: introduce `MultiReaderStatusBuilder` for construction (internal creation goes through it;
the old 4-field compat factory stays for the deprecated path until Phase 6). Resolved (Q4):
`mainDescriptor` stays on the status and gets **no** reader accessor — users never query in-band
reader state through the reader; the status is the single reporting surface.

### C8 — remove the list constructor; everything delegates to the builder constructor

**Agree.** The list/signals/ports constructors become thin factories that populate a
`MultiReaderBuilder` and invoke the builder ctor — one wiring path, one place for the C1/C2/C9
logic. The `FromExisting` constructor is already scheduled for deletion (Phase 6.1) and is not
worth migrating. Public factory functions and their signatures are unchanged.

### C9 — `checkListSizeAndCacheContext` does two things; `sourceComponentsType` result not cached

**Agree.** Both fold into a single source-normalization step used by the (now single) construction
path: validate non-empty + homogeneous (signals xor ports), cache `context`, and store
`typeOfInputs` once. `addInput`'s `typeOfInputs == Unknown` special case is absorbed by the same
helper.

### C10 — "…Locked" naming (`evaluateStateLocked`, `updateMainDescriptorsLocked`)

The suffix means "**caller must hold `mutex`**" (a common convention, but evidently not
self-explanatory, and `updateMainDescriptorsLocked` reads especially badly — it updates the cached
*main-input descriptors*, and "descriptors locked" parses as nonsense). Action: document the
convention once at the top of `multi_reader_impl.h`; rename the worst offender
(`updateMainDescriptorsLocked` → `refreshMainInputDescriptors...`); keep the suffix itself unless
the forthcoming comments prefer a different marker — renaming ~40 functions is mechanical and can
ride along with the C11 restructuring, which touches most call sites anyway.

### C11 — `evaluateStateLocked` is bulky, runs everywhere, re-checks everything

**Agree — this is the centerpiece of the batch, and the measurements back the comment.** Verified
(details in `08_internal_architecture.md` §3/§6): 19 call sites; an idle synchronized
`getAvailableCount` + `read` cycle runs the full 13-step ladder twice; blocking reads run it once
per condition-variable wake; steps 3 (per-slot connection sync + queue drain), 9 (monitor query)
and 13 (readiness-mask updates) plus a `collectUsedReaders` vector allocation execute on every
evaluation even when nothing changed since the last one.

Direction (see §2 below for the design): state becomes **maintained by the paths that change it**
and **trusted by the paths that read it**. Data-plane calls check one dirty flag; the ladder runs
only when something actually happened.

### C12 — inactive-branch documentation; unused-port events must reach users; terminology

**Agree.** Three sub-items:
- The inactive branch keeps draining used inputs and maintaining event bits so descriptor events
  surface while inactive — correct behavior, under-documented. Gets the rationale comment.
- Unused-input events: today they are invisible except as raw external-listener packet
  notifications (verified — the active-reader ladder never touches unused slots; their events sit
  in the connection queue until re-enable). Addressed structurally via the per-input `Event`
  state (C6) plus the widened `onDataAvailable` gate (Q5) — see §2.3.
- Terminology: *active/inactive* = reader/port level (`setActive`), *used/unused* = input
  participation (`setInputUsed`). The implementation reuses port deactivation as the unused
  mechanism (that is what stops data while preserving events — worth keeping), but comments and
  messages must stop mixing the words. Doc/comment sweep plus a naming pass over the affected
  helpers.

### C13 — `InputSlot` is too generic; rename (`a6f14914`)

**Agree.** `InputSlot` sits directly in the `daq` namespace with the generic header path
`opendaq/input_slot.h` — clash-prone on both axes. The rename is fully internal (plain C++ class,
no RTGen interface, no bindings); verified scope: ~87 occurrences in 11 files (impl, header,
tests, CMakeLists, docs).

Preferred target: **`MultiReaderInput`** (over `MultiReaderInputSlot`) — the public API already
speaks "input" (`addInput`, `setInputUsed`, `getInputStates`, `InputState`), and S1 moves consumer
identity away from positional slots, so "slot" is the wrong mental model to bake into the name.
Along with it: `IInputSlotListener` → `IMultiReaderInputListener`; files →
`multi_reader_input.h/.cpp`, `test_multi_reader_input.cpp`.

Decision point for Batch A: the same generic-name argument applies to the other five internals —
`QueueReader`, `SynchronizationManager`, `ReadCoordinator`, `NotificationCoordinator`,
`DataLossMonitor` (all in `daq`, all with generic headers like `synchronization_manager.h`).
Options: (a) prefix them all (`MultiReaderSynchronizationManager` — unambiguous but long), or
(b) move the multi reader internals into a nested `daq::multi_reader` namespace (repo precedent:
`daq::details`, `daq::config_protocol`, `daq::modules`), keeping short class names, optionally
with an `opendaq/multi_reader/` include subdirectory. Under (b) the comment's rename becomes
`multi_reader::Input`. The plan recommends (b) as the generalized fix; either way C13 is
satisfied.

### C14 — remove the reference-domain compatibility checks (`a6f14914`)

**Agree.** The check is peripheral to synchronization: it gates `buildCommonModel` but contributes
nothing to the alignment math (epoch + resolution + tick arithmetic is what sync actually uses).
It is also weak in its current form — it rejects exactly two narrow cases (two different *known*
time protocols; distinct assigned ids with no known protocol to relate them) and only debug-logs
everything else, which is false confidence rather than protection. And it sits at the wrong
altitude per N4: whether two signals' reference domains can meaningfully be combined is topology
knowledge that belongs to whoever connects the signals, not to the reader.

Action (deletion only): drop `checkReferenceDomains`, `ReferenceDomainBin`, and
`SyncSetupIssue::ReferenceDomainIncompatible`; delete the dedicated tests — verified: **43**
`ReferenceDomain*` tests in `test_multi_reader.cpp` (lines ~2570–3700) plus
`SyncManagerTest.ModelReferenceDomainIncompatible`.

Behavior change to be aware of: the reader will align signals from different reference domains
(e.g. different PTP domains) purely on epoch/resolution arithmetic. That is the stated intent
("handled at a later stage"); when the capability returns, its natural home is a per-input
`Incompatible` state fed by a dedicated validation step — or topology-level validation outside
the reader — not a gate inside `buildCommonModel`.

### C15 — `synchronize()` is hard to parse (`a6f14914`)

**Agree on readability; the algorithm itself should stay.** The function is ~215 lines with four
distinct concerns inlined: collect + convert first samples, the sync-distance guard,
start-candidate selection (including the main-grid search), and advance-and-verify with the retry
loop.

Action (no behavioral change):

- Extract the steps as private helpers, each with a plain-English doc comment:
  `collectFirstSamples`, `checkSynchronizationDistance`, `pickStartCandidate` (owns the grid
  search), `advanceAllInputs`. The body then reads as a short loop over named steps.
- Rewrite the comments in plain language ("every input produces samples at regular tick spacing;
  walk the main input's grid inside one aligned block looking for a tick every input hits
  exactly; if none is exact, take the first tick where every input's offset is unambiguously
  attributable — less than half a block; otherwise there is no common tick") and rename the terse
  locals (`step` → `candidateTick`, `firstsCommon` → `firstSamplesInCommonDomain`, …).
- Hoist the per-iteration vector allocations out of the retry loop (minor; the loop rarely
  repeats).

Optimization considered and rejected: replacing the bounded grid scan (≤ 1024 steps, ≤ one
aligned block) with a generalized-CRT congruence solve — mathematically equivalent, measurably
irrelevant at these bounds, and strictly harder to parse, which is the opposite of what the
comment asks for.

### N1–N7 (message notes) and S1

| Note | Disposition |
|---|---|
| N1 evaluate comments | this document |
| N2 reduce complexity | C8/C9 (construction), C3 (dead surface), C11 (state machine), C5/C6/C7 (status surface) together remove the bulk; §2.2 quantifies what the read path shrinks to |
| N3 redundant status checks | C11 — the double ladder per poll cycle and per-wake re-derivation are the concrete instances |
| N4 status reporting, FB vs human user | C5/C6 design principle: **enum + per-input states are the machine surface (FBs switch on them, never parse text); `getStateMessage` is the human surface (never required for a correct reaction)**. No information appears in both beyond the state name. |
| N5 state-machine complexity; "status check should be trivial if state is properly maintained by those responsible" | C11 — §2.1 |
| N6 post-sync reads check nothing until an event on *any* signal (incl. unused); users notified of unused events | §2.2 + §2.3 |
| N7 / S1 slots must not be reconstructed on add/remove | §2.4 — verified today `removeInput` erases + reindexes every following slot and `resize(0)`s the coordinator and monitor, dropping healthy inputs' arming/readiness |

---

## 2. Target design

### 2.1 Event-driven state maintenance (C11, N5) — per-slot `HasData`/`HasEvent`

*(Supersedes the earlier `stateDirty` draft — discussion of 2026-07-19. `stateDirty` was still
lazy re-derivation, only rarer; the agreed model is fully eager: every trigger updates state at
the point of change, and reads never re-derive anything. There is no facade dirty flag.)*

Two per-slot flags, maintained by the slot's own components:

- **`HasData`** — the slot has ≥ 1 readable sample *before its next in-band event*. Owned by
  `QueueReader`, which already clamps availability at the frontier event — that clamping is the
  invariant that makes `HasData == true, HasEvent == false` safe while an event sits behind
  buffered data. `HasData` is the cheap gate ("is a read worth attempting", checked across all
  used slots); the readable *amount* still comes from the coordinator's min-across-slots
  divider/blockLcm computation, run only behind that gate.
- **`HasEvent`** — the slot's readable frontier is an event, or an out-of-band condition was
  raised on it. Means: reads must return promptly (with whatever data precedes the frontier —
  §2.2), the per-input state shows the cause, and the consumer must be notified.

Two trigger classes with different timing:

- **In-band** (have a queue position; data before them stays readable; `HasEvent` becomes true
  only when they reach the readable frontier): descriptor change, gap, and **data loss** (§2.6 —
  the deadline inserts a marker *behind* everything already buffered).
- **Out-of-band** (no queue position; take effect immediately in their own call/callback under
  the mutex): `slotConnected`/`slotDisconnected`, `setInputUsed`, `setActive`,
  `addInput`/`removeInput`, `setMainInput`. Runtime config changes disappear as a trigger class
  once C1/C2 make configuration builder-only.

The 13-step ladder decomposes into per-trigger handlers owned by the components — N5's "parts of
the multi reader should handle their own state":

| Trigger | Handled by | Effect |
|---|---|---|
| data packet drained (used slot, synchronized) | `QueueReader` | `HasData` update only — reader state is never touched |
| in-band event reaches the frontier | `QueueReader` → facade handler | `HasEvent`, per-input `Event`; descriptor parse; sync invalidation where applicable |
| connect / disconnect | `InputSlot` handler | per-input state, sync invalidation |
| used/unused, active/inactive | the respective setter | used-set vector, port activation, per-input state |
| data-loss deadline | monitor callback | in-band loss marker (§2.6) |
| all-used-`HasData` while unsynchronized (descriptors parseable) | coalesced task, or the read that observes it | attempt sync — the one residual "conditions became right" check; it runs only in non-synchronized states, so N6's post-sync rule holds |

`MultiReaderImpl` itself shrinks to the N5 target: hold the mutex, forward API requests, map
component state to `ReadStatus` + the per-input dict, and let `NotificationCoordinator` fire
`onDataAvailable` on the gate `allUsed(HasData) || any(HasEvent)` — unused slots included (Q5).

The producer path stays bounded: `slotPacketReceived` still only sets `packetPending` and wakes;
whether a pending packet is data or an event is discovered at drain time (next read or coalesced
task) — the same visibility as today. Queue adoption stays gated by `packetPending`: only slots
that actually received packets since the last drain are drained; clean slots are skipped.

Net effect, synchronized steady state: `read` = drain of packet-pending slots + availability +
plan/commit. No per-slot validity re-checks, no model checks, no monitor query, no mask writes, no
`collectUsedReaders` re-allocation (the used set becomes a maintained vector, rebuilt on used-set
changes only). This is N6's "no checks after sync" in implementable form.

### 2.2 Read-path shape after the change

```
readInternal:
  drain packet-pending slots                    // HasData/HasEvent maintenance, nothing else
  switch (maintained state):
    Fail | Inactive | Preparing | InputsFailed → status, no data (timeout wait only where it
                                                 makes sense today)
    synchronized:
      plan → commit over min(available-before-frontier)
      frontier event reached → handle it, status Event   // the data before it was just returned
      otherwise → status Ok
getAvailableCount:
  drain packet-pending slots
  synchronized ? coordinator availability (clamped at frontiers) : 0
```

This encodes the universal reader pattern — 1. read the data returned, 2. react to the status —
and the read-as-far-as-possible rule: a read that runs into an in-band event returns all data up
to it **and** the `Event` status in the same call. The wait predicates wake on
`HasData`/`HasEvent` changes and re-check the flags only; wakes from pure data arrivals evaluate
availability only.

### 2.3 Unused inputs that stay observable (C12, N6, Q5)

- Unused slots keep their port deactivated (data dropped at the connection — no unbounded growth)
  and events keep enqueueing, as today.
- The coalesced task drains **event packets** of packet-pending *unused* slots too (data cannot
  appear — the port is inactive) and sets their per-input state to `Event`.
- Resolved (Q5): unused-input events **do trigger `onDataAvailable`** — the gate is
  `allUsed(HasData) || any(HasEvent)` with unused slots included. This is the recovery API: the
  consumer gets the callback, reads (possibly zero samples), sees `ReadStatus::Event` with the
  unused port's dict entry at `Event`, and either calls `setInputUsed(id, true)` (existing
  re-enable semantics: stale data dropped, descriptor changes applied, resync) or ignores it.
- `getEventPackets` continues to carry only used inputs' events; unused inputs' descriptor events
  are consumed internally on re-enable. The per-input `Event` state is the API (minor open point
  in §4 if packet exposure turns out to be wanted).
- The **sum FB is the reference example** (Q5): parked-port recovery moves onto this path, and
  the FB drops `setExternalListener` entirely — a descriptor fix on a parked port produces
  `onDataAvailable` → `deferredCheck` → dict shows `Event` on the parked port → immediate probe.
  One recovery path stays timer/traffic-paced: silent data resume after data loss (data packets
  on an inactive port are dropped at the connection and produce no event), which is what the
  periodic probe remains for.

### 2.4 Stable slots (S1)

- `removeInput` erases exactly one slot's state everywhere: `NotificationCoordinator` and
  `DataLossMonitor` get `erase(index)` (shift-preserving) instead of `resize(0)+resize(n)`;
  remaining inputs keep readiness/event masks and armed deadlines.
- Slot identity for consumers moves entirely to **port global id** (per-input states, C6), so
  internal index compaction on remove stops leaking into the API. Internally indices remain the
  storage key (reindex stays, but becomes invisible and cheap: `setIndex` on the followers plus the
  two `erase(index)` calls).
- `addInput` already appends without touching existing state — unchanged.

### 2.5 API surface after the batch (summary)

| Surface | Before | After |
|---|---|---|
| `IMultiReader` | + `setMaxSynchronizationDistance`/get, `setDataLossTimeout`/get | removed (builder-only) |
| `IMultiReaderStatus::getState` | 11-value `MultiReaderState` | removed — `getReadStatus` returns extended `ReadStatus` (`+ Preparing, Inactive, InputsFailed`) |
| `IReaderStatus::getValid` (multi) | false for every failure state | false only for `Fail` (unrecoverable) |
| `getStateMessage` | machine + human mixed | human-only diagnostic; never needed for a correct reaction |
| `getAffectedInputCount`/`getAffectedInputIndex` | slot indices | removed → `getInputStates` dict (globalId → `InputState`) |
| `getEventCount`/`getEvent` | ordered duplicate of the dict | removed |
| `MultiReaderStatusEx` factory | 8 fields | `MultiReaderStatusBuilder`, 5 explicit fields |
| `MultiReaderImpl` interfaces | implements `IInputPortNotifications` | dropped (slots are the only port listeners) |
| `onDataAvailable` gate | `anyUsedEvent() \|\| allUsedReady()` (used inputs only) | `allUsed(HasData) \|\| any(HasEvent)`, unused inputs included (Q5) |
| data loss | instant invalidation; buffered pre-loss data discarded | in-band marker; buffered data readable first, then `Event`/`DataLost` (§2.6) |

### 2.6 In-band data loss (resolved 2026-07-19)

Data loss stops being an instant invalidation and becomes an **in-band event**, obeying the same
read pattern as every other event:

- The `DataLossMonitor` deadline callback no longer flips reader state. It inserts a **loss
  marker** into the slot's queue *behind everything already buffered* — implementation choice: a
  synthesized gap-style event in `QueueReader`, so the frontier machinery handles it uniformly.
- Buffered pre-loss data stays readable: the producer went silent *after* producing it, so it is
  valid; `HasData` remains true and aligned reads keep returning it.
- The read that drains to the marker returns the remaining data **and** `ReadStatus::Event`, with
  that input's dict entry at `DataLost` — "return all the data and have the status of the read be
  the data loss".
- Subsequent reads report `InputsFailed` (persistent) until recovery. If data resumed after the
  marker, recovery is a resync from post-marker data — identical to gap handling. A
  `setInputUsed(false → true)` recovery cycle now drops only post-marker stale content, not the
  valid pre-loss buffer (today the immediate invalidation makes that buffer unreadable and
  recovery discards it — `08_internal_architecture.md` §5.10).

---

## 3. Batching and sequencing

All batches wait for the remaining comments; A, B, and C are all design-complete (Q1–Q5 resolved
2026-07-19). Note that C now also touches the shared `ReadStatus` enum (appended values only —
existing numeric values unchanged), which shows up in every reader's docs and in the bindings.

| Batch | Contents | Risk / notes |
|---|---|---|
| **A — construction & dead surface** | C8 single-ctor delegation, C9 source normalization, C3 facade-listener removal, C10 naming/doc pass, C4 rationale comment, C13 `InputSlot` rename + internals-naming decision, C14 reference-domain check removal (43 + 1 tests deleted), C15 `synchronize()` readability pass | mechanical / deletion-only / naming; test sweep only |
| **B — builder-only config** | C1/C2 removal, test migration to builder config, sum FB rebuild-on-property-write, spec + error-contract updates | small; touches 13 test sites + FB |
| **C — status & state machine** | C5 compact states, C6 per-input states + removals, C7 status builder, C11 event-driven maintenance + fast read path, C12 unused-event surfacing + terminology, S1 stable slots | the substantive batch; large test triage (state-name assertions across `test_multi_reader.cpp`), FB adaptation, `IgnoreFaultyInputs` property; bindings regeneration afterwards |

Suggested order: **A → B → C** (A and B are independent and could swap; C last because it
subsumes most call sites A touches and needs the Q1/Q2 decisions). Each batch: build green,
`test_reader` + `SumTest` green, commit, push. Bindings regeneration once after C rather than per
batch.

## 4. Design decisions (Q1–Q5 — resolved 2026-07-19)

1. **Q1 — public state set.** No new reader-state enum: **extend `ReadStatus`** with appended
   `Preparing`, `Inactive`, `InputsFailed` (C5). `MultiReaderState` and `getState` leave the
   public API; `EventPending` is covered by `ReadStatus::Event`.
2. **Q2 — per-input surface.** Dict on the status (`getInputStates`, globalId → `InputState`);
   **`Event` replaces `UnusedWithEvents`** and applies to used and unused inputs alike (C6).
3. **Q3 — config introspection.** None on the reader; the builder is the only configuration
   surface (C1/C2).
4. **Q4 — main descriptor.** Stays on the status; no reader accessor. Users never query in-band
   reader state through the reader — the status is the single reporting surface.
5. **Q5 — unused-input events.** They trigger `onDataAvailable` (the recovery API); the sum FB is
   the reference example (§2.3).

Remaining minor points (implementation-time, none blocking):

- Naming: `Preparing` vs `Pending` for the reader-level status (`Preparing` preferred here to
  avoid collision with the per-input `Pending`).
- Whether unused inputs' event packets should also appear in `getEventPackets` (plan: no — the
  per-input `Event` state is the API).
- With `getState` gone, `getStateMessage` could rename to plain `getMessage` (it is the human
  diagnostic, no longer tied to a state enum).
- `getValid()` narrowing to `Fail`-only is scoped to the multi reader status; single-reader
  statuses keep their current semantics.
