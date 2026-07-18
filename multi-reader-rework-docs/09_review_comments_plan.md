# Multi Reader Rework — Review Comments: Evaluation and Action Plan

**Status: DRAFT — covers the initial comment set (commit `343fe1ae`) plus the accompanying notes.
More comments are expected; no code changes are made until the full set is in and this plan is
agreed.** Architecture background and the verified facts referenced here live in
`08_internal_architecture.md` (§6 "Known hot spots" in particular).

Numbering: C1–C12 are the inline `// COMMENT:` markers in commit order; N1–N7 are the notes from
the accompanying message; S1 is the slot-reconstruction note.

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
Open question (Q3): keep read-only getters on the reader for introspection, or none at all? Plan
assumes none (comment says "only in the builder").

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

**Agree.** The consumer-action space is: do nothing / read again / `setInputUsed(id,false)` /
`setActive` / recreate. Eleven states force every consumer into a switch over distinctions it
cannot act on (the sum FB's `handleStateLocked` is the live proof). Humans get detail from
`getStateMessage`; FBs should never need to parse it.

Proposed compaction (Q1 for sign-off):

| Public state | Replaces | Consumer action |
|---|---|---|
| `Ok` | WaitingForConnections, WaitingForDescriptors, WaitingForData, Synchronizing, Synchronized | none — reading works or will work by itself; progress detail goes to the message + per-input states |
| `Inactive` | Inactive | `setActive(true)` when reading should resume |
| `InputsFailed` | Incompatible, SynchronizationFailed, DataLost | consult per-input states (C6); `setInputUsed(failing,false)` or fix/reconnect upstream |
| `Error` | Error | recreate the reader |

`EventPending` disappears as a *state*: `ReadStatus::Event` on the read status already carries
exactly that information at exactly the moment it is actionable ("read again"), and zero-count
reads keep returning events. The fine-grained distinctions do not vanish — they move down one
level, onto the inputs (C6), where they are actionable, and into the message, where humans read
them. Internal bookkeeping may keep richer granularity, but per N5 the internal machine is being
simplified too — the internal set only stays larger than the public one where a real transition
needs it.

### C6 — drop the ordered event list; add per-input statuses; FB "ignore faulty inputs" property

**Agree on all three.**

- `getEventCount`/`getEvent` duplicate `getEventPackets` (verified consumers: 11 uses in
  `test_multi_reader.cpp`, no FB uses). Remove both; the dict remains the single event surface.
- `getAffectedInputCount`/`getAffectedInputIndex` are replaced by **per-input statuses** — strictly
  more information (every input, not just the affected set) in a directly actionable shape, and
  keyed by port global id instead of by slot index. Indices were a design mistake: they are exactly
  what `removeInput` shifts (see S1), so any retained index is stale after a topology change.

  Proposed shape (Q2): on `IMultiReaderStatus`,
  `getInputStates(IDict** statesByPortId)` → `port globalId → InputState`, with

  ```
  enum class InputState { Ok, Pending, Incompatible, SynchronizationFailed,
                          DataLost, Unused, UnusedWithEvents }
  ```

  `Pending` = connected but not yet contributing (awaiting connect/descriptors/data/alignment) —
  no action possible; `Unused`/`UnusedWithEvents` make unused-input events visible (C12/N6).
  Reader-level `InputsFailed` ⇔ at least one input in `{Incompatible, SynchronizationFailed,
  DataLost}`. Per-input human-readable detail stays in the single `getStateMessage`.
- Sum FB: add a bool property (working name `IgnoreFaultyInputs`, default true = today's parking
  behavior) — when false, a failing input keeps the whole FB in Warning without being excluded.
  The FB already reports the failing set through its ComponentStatus message; it switches from
  affected-indices to the per-input dict, which also removes its slot-order/`readerPorts` mirror
  bookkeeping.

### C7 — status factory too large → builder object; reconsider fields

**Agree, with the expectation that compaction mostly solves it.** After C5/C6 the Ex factory's
fields become: `eventPackets`, `mainDescriptor`, `offset`, `state`, `stateMessage`, `inputStates` —
six pairs, within the 8-pair RTGen macro limit but at the edge of readability. Action: introduce
`MultiReaderStatusBuilder` for construction (internal creation goes through it; the old 4-field
compat factory stays for the deprecated path until Phase 6). Also reconsider `mainDescriptor`
(Q4): moving the common-output-domain descriptor to a reader accessor (e.g.
`getCommonDomainDescriptor`) would slim the status further, at the cost of an API addition — the
plan keeps it on the status unless decided otherwise, since the FB consumes it per event batch.

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
  in the connection queue until re-enable). Addressed structurally via `UnusedWithEvents` in the
  per-input states (C6) — see §2.3.
- Terminology: *active/inactive* = reader/port level (`setActive`), *used/unused* = input
  participation (`setInputUsed`). The implementation reuses port deactivation as the unused
  mechanism (that is what stops data while preserving events — worth keeping), but comments and
  messages must stop mixing the words. Doc/comment sweep plus a naming pass over the affected
  helpers.

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

### 2.1 Event-driven state maintenance (C11, N5)

Replace derive-on-read with maintain-on-change:

- A single facade flag `stateDirty` (plus the existing per-slot `packetPending` bits). Every path
  that can change the answer sets it: `slotConnected`/`slotDisconnected`, event packet drained,
  data-loss deadline callback, `setActive`, `setInputUsed`, `addInput`/`removeInput`,
  `setMainInput`, event consumption by a read. These are exactly today's invalidation points — the
  knowledge already exists, it is just currently discarded in favor of re-derivation.
- The ladder (steps 1–13, unchanged in content) becomes `revalidateLocked()`, invoked only when
  `stateDirty`. Data-plane calls (`getAvailableCount`, `readInternal`, wait predicates) run:

  ```
  if (stateDirty) revalidateLocked();
  ```

- Packet arrival on a used input while `Synchronized` must not set `stateDirty` (that would
  re-introduce per-packet ladders). Data packets only ever *add* availability; only **events** and
  **deadlines** can invalidate. Whether a drained packet contained an event is known at drain time
  (`QueueReader` reports it), so draining stays cheap and precise.
- Queue adoption stays at evaluation points (drain contract #10) but becomes gated by
  `packetPending`: only slots that actually received packets since the last drain are drained;
  clean slots are skipped. A drain that surfaces an event sets `stateDirty` and the ladder runs.
- Step-13 readiness/callback-gate maintenance moves fully to the coalesced task (its natural home —
  it exists for the callback gate); API-thread reads stop writing coordinator masks.
- The monitor stops being polled (`lostSlots()` per evaluation); the deadline callback already
  fires `requestEvaluation` — it now also sets `stateDirty`, and step 9 runs only inside
  `revalidateLocked`.

Net effect, synchronized steady state: `read` = dirty-flag check + drain of packet-pending slots +
availability + plan/commit. No per-slot validity re-checks, no model checks, no monitor query, no
mask writes, no `collectUsedReaders` re-allocation (used set becomes a maintained vector, rebuilt
on used-set changes only). This is N6's "no checks after sync" in implementable form.

### 2.2 Read-path shape after the change

```
readInternal:
  if invalid → status
  if stateDirty → revalidateLocked()
  [zero-count / timeout / event / not-synchronized branches as today]
  plan → commit → status        // no other checks
getAvailableCount:
  if stateDirty → revalidateLocked()
  synchronized ? coordinator availability : 0
```

The two wait predicates re-check `stateDirty` instead of re-deriving; wakes without state changes
(pure data arrivals) evaluate availability only.

### 2.3 Unused inputs that stay observable (C12, N6)

- Unused slots keep their port deactivated (data dropped at the connection — no unbounded growth)
  and events keep enqueueing, as today.
- New: the coalesced task drains **event packets** of packet-pending *unused* slots too (data
  cannot appear — the port is inactive), records them as pending on the slot, and reflects them as
  `UnusedWithEvents` in the per-input states. Consumers see it on every status; a consumer that
  cares calls `setInputUsed(id, true)` (existing re-enable semantics: stale data dropped,
  descriptor changes applied, resync) or ignores it.
- The dataAvailable callback gate is *not* widened by default — data flow stays gated on used
  inputs only (Q5 if notification-on-unused-events is wanted as an explicit callback trigger).
- Read `getEventPackets` continues to carry only used inputs' events; unused inputs' descriptor
  events are consumed internally on re-enable (they exist to keep type state coherent, not as a
  data-flow signal). If the forthcoming comments prefer unused events in the dict as well, the
  drain point above is the single place to change.

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
| `IMultiReaderStatus::getState` | 11-value enum | 4-value enum (`Ok`, `Inactive`, `InputsFailed`, `Error`) |
| `getStateMessage` | machine + human mixed | human-only diagnostic; never needed for a correct reaction |
| `getAffectedInputCount`/`getAffectedInputIndex` | slot indices | removed → `getInputStates` dict (globalId → `InputState`) |
| `getEventCount`/`getEvent` | ordered duplicate of the dict | removed |
| `MultiReaderStatusEx` factory | 8 fields | `MultiReaderStatusBuilder`, 6 fields |
| `MultiReaderImpl` interfaces | implements `IInputPortNotifications` | dropped (slots are the only port listeners) |

---

## 3. Batching and sequencing

All batches wait for the remaining comments; A and B are design-complete now, C depends on Q1/Q2.

| Batch | Contents | Risk / notes |
|---|---|---|
| **A — construction & dead surface** | C8 single-ctor delegation, C9 source normalization, C3 facade-listener removal, C10 naming/doc pass, C4 rationale comment | mechanical; test sweep only |
| **B — builder-only config** | C1/C2 removal, test migration to builder config, sum FB rebuild-on-property-write, spec + error-contract updates | small; touches 13 test sites + FB |
| **C — status & state machine** | C5 compact states, C6 per-input states + removals, C7 status builder, C11 event-driven maintenance + fast read path, C12 unused-event surfacing + terminology, S1 stable slots | the substantive batch; large test triage (state-name assertions across `test_multi_reader.cpp`), FB adaptation, `IgnoreFaultyInputs` property; bindings regeneration afterwards |

Suggested order: **A → B → C** (A and B are independent and could swap; C last because it
subsumes most call sites A touches and needs the Q1/Q2 decisions). Each batch: build green,
`test_reader` + `SumTest` green, commit, push. Bindings regeneration once after C rather than per
batch.

## 4. Open questions before implementation

1. **Q1 — public state set.** Is `{Ok, Inactive, InputsFailed, Error}` the agreed compaction? In
   particular: is folding all waiting/synchronizing conditions into `Ok` acceptable, with
   `ReadStatus::Event` covering the "read again" signal instead of a public `EventPending` state?
2. **Q2 — per-input state surface.** Dict on the status (`getInputStates`, globalId →
   `InputState`) vs a getter on the reader itself; and the exact `InputState` value set
   (`Ok, Pending, Incompatible, SynchronizationFailed, DataLost, Unused, UnusedWithEvents`).
3. **Q3 — config introspection.** After C1/C2, should the *reader* keep read-only getters for
   `maxSynchronizationDistance`/`dataLossTimeout`, or is the builder the only place (plan assumes
   builder-only)?
4. **Q4 — `getMainDescriptor`.** Keep on the status (plan) or replace with a reader accessor for
   the common output domain descriptor?
5. **Q5 — unused-input event notification.** Per-input `UnusedWithEvents` on every status (plan) —
   should unused-input events additionally trigger the dataAvailable callback and/or appear in
   `getEventPackets`?
