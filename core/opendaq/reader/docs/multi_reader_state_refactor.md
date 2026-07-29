# Multi Reader state machine refactor — specification and implementation plan

**Status:** phases 0-5 are implemented; only phase 6 (documentation and cleanup) remains, and
§3.2/§3.3's sketch of the API is superseded by what the code does (see the phase notes in §5). Also
implemented: the notification-completeness work of 2.1-2.2 (`Input::listen` /
`Input::replayMissedPortCallbacks` / `Input::adoptQueuedPackets`), for which this document is the
rationale.

Read §5 top to bottom for what each phase actually found - four of them contradicted the plan in this
document, and those notes are the useful part.
**Scope:** `MultiReaderImpl`'s state handling only. No change to `SynchronizationManager`,
`QueueReader`, `ReadCoordinator`, `CallbackGate`, or any public interface.
**Companion:** [`multi_reader.md`](multi_reader.md) — §4 (states), §8 (evaluation), §9 (data plane).

---

## 1. Why

The state evaluation (`evaluateStateLadderLocked` when this was written; `evaluateStateLadder` since
phase 1) is one ~330-line function that computes the reader state by testing conditions top to bottom
and returning at the first match. Three things have frayed:

1. **It is not one ladder.** `if (!isActive)` (`multi_reader_impl.cpp:834`) forks into a second,
   much shorter ladder with its own event scan, its own `clearPacketPending`/`adoptQueuedPackets`, and a
   two-value vocabulary. `EventPending` is a terminal reachable from both arms via different code
   with different side effects. `multi_reader.md:111-112` still describes the whole thing as "a
   top-to-bottom ladder that picks the first applicable state" — true of one arm only.
2. **The step numbering is already inconsistent** — the comments run 1, 2, 3, 4/5, 6, 7, **9**,
   *(fast path)*, **8**, 10, ... 13, with 11 and 12 absent. The linear reading no longer matches the
   code.
3. **Side effects that belong to a transition are scattered.** `invalidateSynchronizationLocked`,
   `invalidateModelLocked`, `readCoordinator->invalidate()`, the all-slot event-bit suppression, the
   data-loss monitor arm/disarm and the `stateChangeNotify` latch each sit inline next to individual
   `setStateLocked` calls (17 call sites). Whether a given state clears the model is knowledge
   distributed across the function rather than a property of the state.

The goal is to make the fork a class boundary, make the side effects entry/exit invariants, and make
each state independently unit-testable — **without changing what state the reader computes for a
given set of observable inputs.**

---

## 2. The central constraint: this machine is level-triggered, and must stay that way

This is the single most important thing for a reviewer to agree with before the plan is worth
reading.

The current machine is a **pure function of observable state**, recomputed from ground truth on
every evaluation. It is not an edge-triggered FSM that advances on notifications. That is
deliberate, for three reasons visible in the code:

- **Connectivity is notification-driven, but only because the gap was closed deliberately.** The
  initial connection used to be established with no notification at all, which is why the
  evaluation polled the port. `Input::replayMissedPortCallbacks` now replays the missing callbacks
  (§2.2), so `isConnected()` is authoritative and step 3 only adopts queued packets. A state class
  may therefore trust `slotConnected` for *connectivity* — but see the two points below before
  trusting edges for anything else.
- **Producers are lock-free.** Between two evaluations, arbitrary amounts of data and any number of
  event packets can be enqueued with no ordered notification the owner observes.
- **The input set is mutable at runtime.** `addInput`, `removeInput`, `setInputUsed`, `setMainInput`,
  `setActive` each invalidate prior conclusions.

### Consequence for the proposed API

`checkTransitionCriteria` is not questionable — **it is the load-bearing method.** It *is* the
level-triggered re-derivation: the existing ladder, partitioned per state. The three `slot*` edge
methods are entry points and optimizations that decide *whether* re-derivation is needed and do
cheap bookkeeping first; they are never the authority on what state the reader is in.

Read that way, the refactor is a **partition of an existing total function**, not a change of
triggering discipline. Framing it as "add edges, make the criteria check optional" would be a
behavioural rewrite disguised as a refactor.

**Hard invariant for the whole effort:** for identical observable inputs, the substate computed
after the refactor must equal the substate computed today, on every path.

### 2.1 Threading precondition: `PacketReadyNotification::SameThread`

**The refactor assumes every slot port uses `PacketReadyNotification::SameThread`.** With that
setting `notifyPacketEnqueued` calls `notifyPacketEnqueuedSameThread`
(`input_port_impl.h:362-380`), which invokes `listener.packetReceived(port)` synchronously inside the
producer's `enqueue` call stack.

**What the assumption buys**

1. **Notification issuance order equals observation order, per thread.** With
   `PacketReadyNotification::Scheduler`, `packetReceived` is dispatched as `Work` to a pool thread
   (`notifyPacketEnqueuedScheduler`, `:382-387`) while `connected()` is invoked synchronously on the
   connecting thread. The port issues them in order, but the reader observes them from two threads
   racing for its state mutex, so `slotPacketReceived` can enter the reader before `slotConnected`.
   `SameThread` removes that hop and therefore that race.
2. **Reproducible characterization traces.** Phase 0's golden traces are only stable if the
   trigger order is deterministic for a single-threaded producer. This is the strongest practical
   argument for pinning the setting.
3. **Truthful arrival timestamps.** `Input::packetReceived` stamps `lastPacketArrival`, which feeds
   the data-loss deadline. Under `Scheduler` that stamp includes pool queueing delay.
4. **One scheduler hop instead of two.** `NotificationCoordinator` is constructed with
   `context.getScheduler()` unconditionally (`multi_reader_impl.cpp:137`, `:205`), so this reader
   already defers all real work to its own coalesced task. Port-level `Scheduler` notification adds
   a *second* hop in front of it: producer -> pool thread -> gate check -> schedule another task ->
   evaluation. For this reader the port's scheduler mode is redundant, not merely different.
   (The benchmark has always used `SameThread` — `bench_multi_reader.cpp:104` — so every published
   number already reflects this assumption.)

**What the assumption does NOT buy**

1. **`connected()` is still never delivered for the initial connection.** `createOrAdoptPorts:289`
   calls `port.connect(signal)` on a port created at `:286`, before any listener exists, so
   `connectInternal` finds `inputPortListener` null and skips `connected()` entirely; the descriptor
   enqueue that follows likewise finds `listenerRef` unassigned and fires no `packetReceived`.
   `createSlots:356` installs the listener afterwards. This is a listener-installation ordering
   issue and is completely independent of the notification method.
2. **`setListener` front-loads a descriptor silently.** `input_port_impl.h:469-476` calls
   `connection->enqueueLastDescriptor()` before assigning `listenerRef`;
   `ConnectionImpl::enqueueLastDescriptor` does `packets.emplace_front(...)` under the connection
   lock with **no** `notifyPacketEnqueued`. So the queue is already non-empty the instant the slot
   becomes the listener, with nothing notified.
3. **Multiple producer threads still race with each other.** `SameThread` means "no scheduler hop",
   not "serialized". Two signals sent from two threads still deliver `packetReceived` concurrently;
   that is a data-race question, answered by the gate's atomics and the pass epoch, not an ordering
   question.

Points 1 and 2 are exactly what `Input::replayMissedPortCallbacks` closes, and closing them is what
allowed the connectivity polling to be removed from every evaluation point. `SameThread` on its own
narrows the threading model; it does not make notifications complete.

### 2.2 Notification completeness (implemented)

Four comments used to justify the port re-sync with a claim that is **false**: that `packetReceived`
fires before `connected()` on the connect path. Verified against `connectInternal`
(`input_port_impl.h:656+`), the order is: `createConnection` (the `ConnectionImpl` constructor
enqueues nothing) -> `connectionRef = connection` -> **`inputPortListener->connected(port)`** ->
`events->listenerConnected(connection)` -> `createDataDescriptorChangedEventPacket()` ->
`enqueueOnThisThread` -> `notifyPacketEnqueuedOnThisThread` -> **`packetReceived`**. Single thread,
one call stack, `connected()` strictly first.

The real mechanism is the two facts under §2.1: no `connected()` is delivered for a port that was
already connected when its listener was installed, and `setListener` front-loads a descriptor
without notifying. Both are now closed rather than worked around:

| Step | What it does |
|---|---|
| `Input::listen(self)` | Installs the slot as the port's listener. Must run before any drain: `setListener` front-loads the connection's cached descriptor, which has to land AHEAD of data already queued behind it |
| `Input::replayMissedPortCallbacks()` | Replays `connected()` and `packetReceived()` for an already-connected port. Unlocked, after the reader is built (`MultiReaderImpl::replaySlotCallbacks`) |
| `Input::adoptQueuedPackets()` | Replaces `syncConnection()`: drains the connection but no longer re-reads it. Connectivity comes from callbacks; only queue contents are polled, because the lock-free producer path cannot hand them over |

`QueueReader::refreshConnection` existed only for that polling and has been deleted. The three code
comments that stated the false mechanism have been rewritten.

Incidental finding worth recording: the initial descriptor event is enqueued **twice** — once by
`listenerConnected`, which makes `ConnectionImpl::onPacketEnqueued` cache the descriptors
(`connection_impl.cpp:710-737`), and again by `setListener`'s `enqueueLastDescriptor`, which sees
those cached descriptors and front-loads a duplicate. Harmless (identical descriptors, consuming
both is idempotent) but it explains why the reader always observes a descriptor event even on ports
it connected itself, and it matters to anything that counts events.

---

## 3. Target model

### 3.1 State classes and their substates

The existing `ReaderState` enum (`multi_reader_impl.h:45-58`) is kept as the substate — it is what
`toReadStatus` (`:1385-1410`) and per-input `InputState` derivation (`:1420-1435`) map from, and
changing it would ripple into status code for no benefit.

| Class | Substates | Class invariant | Public `ReadStatus` |
|---|---|---|---|
| `ErrorState` | `Error` | `invalid == true`; terminal, latched, no exit | `Fail` |
| `InactiveState` | `Inactive`, `EventPending` | `isActive == false`; no data flow; data-loss monitoring disarmed for all slots | `Inactive`, `Event` |
| `WaitingForValidInputsState` | `WaitingForConnections`, `WaitingForDescriptors`, `Incompatible`, `EventPending` | nothing is readable yet and the blocker is per-input (a connection, a descriptor, a compatible descriptor, or an unconsumed event). **Not** "no valid cross-input model": the event rung builds the model opportunistically, so `getCommonSampleRate` works while events are still pending | `Preparing`, `InputsFailed`, `Event` |
| `SynchronizingState` | `WaitingForData`, `Synchronizing`, `SynchronizationFailed`, `DataLost` | model valid, `commonStart == nullptr` — alignment is being attempted or has failed recoverably | `Preparing`, `InputsFailed` |
| `ReadyState` | `Synchronized` | `commonStart != nullptr` **and** `syncManager->hasModel()` — aligned blocks are readable | `Ok` |

One substate in that table needs an input today's ladder does not consult: `Synchronizing` is not
derivable from the queues alone, because an alignment attempt that needs more data leaves the lagging
input *empty*, which reads as `WaitingForData` (§5.1, finding 1). It becomes derivable once the latched
synchronization target is part of what the criteria check sees: inside `SynchronizingState`, "an input
has no data **and** a target is already latched" means "waiting for that signal to catch up", which is
exactly `Synchronizing`. `SynchronizingState` must therefore read
`SynchronizationManager::pendingCandidate`, not only the queue state.

Two placements need explicit sign-off:

**`DataLost` → `SynchronizingState`, not `ReadyState`.** Entering `DataLost` calls
`invalidateSynchronizationLocked()` first (`:1073`), so `commonStart` is null afterwards. Putting it
in `ReadyState` would break the invariant `ReadyState => commonStart != nullptr`, and that invariant
is what makes two existing optimizations sound: the `Synchronized` fast path (`:1082-1086`) and the
producer gate's `steady` predicate (`:764`), which becomes exactly `currentState->isReady()`.
*Counter-argument for the reviewer:* `DataLost` is only reachable from a reader that was delivering,
and the in-band rule keeps it `Synchronized` while pre-loss data is still readable — so one could
argue it is "Ready, degraded". I recommend the invariant-preserving placement; flagging it because
it is a judgment call, not a derivation.

**`EventPending` appears in two classes.** That is not duplication introduced by the refactor — it
is the existing fork made visible. The two arrivals are genuinely different code today:

| Aspect | inactive arm (`:849-874`) | active step 4/5 (`:954-1006`) |
|---|---|---|
| `clearPacketPending` + `adoptQueuedPackets` | done here | done earlier at step 3; deliberately not repeated |
| handshake-in-flight suppression | absent | present (`:976-985`) |
| opportunistic `buildCommonModel` | absent | present (`:991-1001`) |
| `discardLeftoverSegments` beforehand | not run | run (`:948-951`) |
| data-loss monitoring | disarmed for all slots (`:837-838`) | armed for used+connected (`:942-946`) |

Consequences the flat enum hides, which become explicit class properties:
`InactiveState`'s vocabulary is exactly `{Inactive, EventPending}` — it can never report
`WaitingForDescriptors` or `Incompatible`; and a buried event behind a sub-block residual will not
produce `EventPending` while inactive, because promotion depends on `discardLeftoverSegments`, which
only the active arm runs.

**Recommendation:** preserve both behaviours exactly and document the divergence as
`InactiveState`'s reduced vocabulary. Converging them is a behaviour change and belongs in a
separate commit, if at all.

### 3.2 Abstract base class

```cpp
/// Owner-thread only. EVERY method here runs with MultiReaderImpl::mutex held, inside a
/// CallbackGate::PassGuard where the caller may move samples. States are stateless flyweights:
/// one instance per class, owned by MultiReaderImpl, all mutable data reached through the context.
class MultiReaderState
{
public:
    virtual ~MultiReaderState() = default;

    virtual StateId id() const = 0;

    // --- The authority: level-triggered re-derivation from ground truth. ---
    // Returns Transition::stay() when this class still applies (possibly with a different
    // substate), or a target class when it does not.
    virtual Transition checkTransitionCriteria(StateContext& ctx) = 0;

    // --- Edges. Owner-side reactions to a slot notification. Each returns either a
    // Transition, or Transition::reevaluate() to defer to the criteria check. ---
    virtual Transition slotConnected(StateContext& ctx, SizeT slotIndex) = 0;
    virtual Transition slotDisconnected(StateContext& ctx, SizeT slotIndex) = 0;

    // Owner-side arrival handling, NOT the producer's lock-free notification - see below.
    virtual Transition slotPacketArrived(StateContext& ctx, SizeT slotIndex) = 0;

    // --- Transition side effects, currently inline at the 17 setStateLocked call sites. ---
    virtual void onEnter(StateContext& ctx, const Transition& t) {}
    virtual void onExit(StateContext& ctx, StateId next) {}
};
```

**The three requested edges are not symmetric, and one of them cannot be a state method.**
`slotConnected` and `slotDisconnected` are already owner-side: both take the state mutex and a
`PassGuard` (`multi_reader_impl.cpp:1272`, `:1298`), so they map onto virtuals directly.
`slotPacketReceived` does **not** — it deliberately takes no mutex at all (`:1319-1342`: set
`dataPlaneDirty`, stamp the data-loss monitor, check the gate, maybe `requestEvaluation`, notify).
Under `SameThread` that body runs on the producer's thread inside the connection's enqueue stack.

Making it a virtual on `currentState` would mean either reading `currentState` without the lock
(a data race against `transitionToLocked`) or taking the lock on the producer path — which destroys
the lock-free callback gate and every benchmark number that depends on it. Neither is acceptable.

Resolution, reflected above: **the producer path is left exactly as it is** and is not part of the
state machine. The state machine gets `slotPacketArrived`, invoked owner-side during an evaluation
when the reader processes the arrival the producer already recorded (today: the `clearPacketPending`
checks at `:1191` and inside step 3). The renaming is deliberate — `slotPacketReceived` should keep
meaning "the lock-free producer notification" and nothing else.

**`StateContext`** — the collaborator bundle passed by reference, not a back-pointer to the reader:
`slots`, `syncManager`, `readCoordinator`, `dataLossMonitor`, `notificationCoordinator`, the
read-config values (`minReadCount`, `dataLossTimeout`, `mainInputId`, `isActive`, `invalid`), and a
sink for the outgoing `{substate, message, affectedInputs}`. This is what makes states unit-testable
without constructing a reader, and it is the main structural payoff of the refactor.

**`Transition`** — a value type, no allocation:

```cpp
struct Transition
{
    StateId       target;    // Stay | Reevaluate | <a class>
    ReaderState   substate;  // the existing 10-value enum
    std::string   message;
    std::vector<SizeT> affected;
};
```

**Flyweight, not allocated per transition.** Every `read()` and every `getAvailableCount()` runs the
evaluation, so a heap allocation per transition would land on the hot path we just spent a
benchmark cycle protecting. One instance per class, `currentState` is a raw pointer, states hold no
mutable data.

### 3.3 The evaluation loop

`evaluateStateLocked` becomes:

```cpp
void MultiReaderImpl::evaluateStateLocked()
{
    constexpr int maxTransitions = 8;               // mirrors synchronize()'s retry bound
    for (int i = 0; i < maxTransitions; ++i)
    {
        const auto t = currentState->checkTransitionCriteria(ctx);
        if (t.target == StateId::Stay) { applySubstateLocked(t); break; }
        transitionToLocked(t);                       // onExit -> swap -> onEnter
    }
    publishProducerGateLocked();                     // unchanged single funnel
}
```

A bounded loop is required because a class change can expose a condition the new class rejects
(e.g. `WaitingForValidInputs` → `Synchronizing` → immediately `WaitingForData`). The bound must be
asserted, not silently truncated — exhausting it is an invariant break, i.e. `ErrorState`.

---

## 4. Regression contract

These are the properties the current code establishes with explicit comments, i.e. the ones a
refactor is most likely to destroy silently. Each needs a test before the corresponding phase.

1. **`invalid <=> Error`**, checked before everything including inactivity (`:825-830`), terminal, and
   hardened at status construction: `invalid && state != Error` is coerced to `Error` (`:1474`).
2. **Rung order is preserved exactly:** used set/main → connections → *(monitoring, discard)* →
   events → descriptors → local validity → data loss → *(Synchronized fast path)* → model → data →
   synchronize. Any reordering is a behaviour change, not a refactor.
3. **Clear-then-drain** for `packetPending`: clear *before* `adoptQueuedPackets` drains, or
   a packet enqueued in the drain→clear window is stranded on the connection (the
   availability-undercount race). Must not end up duplicated in both an edge handler and the
   criteria check.
4. **All-slot event-bit suppression** when a used input has no signal (`:931-932`) or a connect
   handshake is in flight (`:981-985`). Both are cross-input rules; neither can be delegated to a
   single `Input`.
5. **The `Synchronized` fast path** (`:1082-1086`): with `commonStart != nullptr` the model is not
   rebuilt. This is `ReadyState::checkTransitionCriteria`'s whole reason to be cheap.
6. **The three-depth evaluation split** must survive: NOTIFY does not run the ladder for events,
   QUERY escalates, READ escalates and owns event surfacing
   (`refreshDataPlaneLocked(escalateOnEvent)`, `multi_reader.md:183-208`). This is a measured
   performance contract, not a style choice.
7. **`stateChangeNotify` is edge-triggered** on the `InputsFailed` family and must not re-latch on
   an unchanged failure (`:406-409`) — otherwise healthy-input packets re-fire the callback while a
   failure persists. Natural home: `onEnter`.
8. **`setStateLocked` must not schedule.** It runs under the mutex, and the inline (no-scheduler)
   executor would re-enter `onCoalescedEvaluation` and recursive-lock — the `ExpectSR` deadlock.
   `onEnter`/`onExit` inherit this prohibition.
9. **The callback gate is independent of `ReaderState`** (`multi_reader.md:213`). The FSM must not
   become a precondition for the gate; `onDataAvailable` can legitimately fire while the state is
   stale.
10. **Per-input `InputState`**, including failure-state naming from `stateAffectedInputs`
    (`:1420-1435`), is unchanged and still derived per slot.
11. **`PassGuard` bracketing** of every section that moves or consumes samples.
12. **The `DataLost` in-band rule** is `< blockLcm`, not `== 0` (`:1059-1063`): gating on empty
    stalls the reader in `Synchronized` forever when an input's divider `!= blockLcm`.
13. **`publishProducerGateLocked` remains the single funnel** every evaluation exit passes through.

---

## 5. Implementation plan

Each phase compiles, passes the suite, and is independently revertible. No phase changes behaviour
except where it says so.

### Phase 0 — characterization tests (**implemented**)

`test_multi_reader_state_transitions.cpp` (16 tests) drives a reader through one scenario per test
and records a golden trace of `trigger -> substate[affectedInputs] detail`. **Without this the
refactor is unverifiable**; it also has standalone value, since several §4 invariants were asserted
only by comments.

Two things make the traces mean something:

- **The substate is read directly**, through a new inline test hook `MultiReaderImpl::getStateForTest`
  (next to the existing `setDataLossClockForTest`, and for the same reason: the implementation is not
  exported from the library). This is the one production-file change in the phase. `ReadStatus` cannot
  substitute — `WaitingForConnections`, `WaitingForDescriptors`, `WaitingForData` and `Synchronizing`
  all project onto `Preparing`, so a projection-based trace cannot show that a partition of the ladder
  preserved the partition.
- **Observation is either a probe or a peek, and the distinction is load-bearing.** A *probe*
  (`getAvailableCount`) forces a level-triggered re-derivation from ground truth — it is what
  `checkTransitionCriteria` will be. A *peek* records the state a trigger left behind without
  re-deriving. Only the peek can see a transient (see the first finding below).

| Scenario from the original list | Covered by |
|---|---|
| cold start with staggered connects | `ColdStartStaggeredConnects` |
| handshake-in-flight | `NoCallbackUntilEveryUsedInputHasSignal` (through its consequence — see below) |
| leading vs buried events | `LeadingEventPreemptsEveryRungBelowIt`, `BuriedEventSurfacesOnReadNotOnQuery` |
| descriptor change mid-stream | `BuriedEventSurfacesOnReadNotOnQuery` (change + resynchronization) |
| incompatible descriptor and recovery | `IncompatibleDescriptorAndRecovery`, `RequiredRateNotDivisibleIsIncompatible` |
| data loss with and without a buffered block | `DataLossOnlyOnceTheInputCannotContribute`, `DataLossWithSubBlockResidual` (contract item 12, both halves) |
| `setActive` both directions, inactive-`EventPending` | `InactiveArmHasItsOwnReducedVocabulary` |
| `setInputUsed` recovery | `UnusedInputStaysObservableAndRecovers`, `UnusedInputEventBuriedBehindItsOwnLeftoverData` |
| commit failure → `Error` | **not covered** — see below |

Beyond the list: `AlignmentIsIterativeAndSynchronizingIsTransient` and
`SynchronizationDistanceExceededAndRemedy` (rung 11's two non-`Synchronized` exits, neither of which
the original list reached), `DisconnectDropsToWaitingForConnections`,
`ReconnectWithBufferedDataResynchronizesWithoutAnEvent`, `ErrorIsTerminalAndOutranksEverything`.

Two deliberate gaps:

- **Commit failure → `Error`** needs fault injection: `readCoordinator->commit` only fails on an
  internal inconsistency, which no sequence of public calls produces. The `Error` rung is
  characterized through `markAsInvalid` instead (same latch, same terminality, same coercion at status
  construction), so what is untested is the *trigger*, not the state. **Deferred:** a test-only hook
  that forces one commit to fail (a `ReadCoordinator` seam, like the data-loss clock injection) would
  close it. Not built - noted here so it is a decision rather than an omission.
- **The handshake-in-flight window is not directly observable.** It is not a rare race — every
  `connect` passes through it, because `connectInternal` calls `connected()` before enqueueing the
  descriptor, and that callback runs a full evaluation. But it is only ever observed from inside the
  connect call stack, and the whole point of the suppression is that nothing fires. What the test pins
  is the consequence: no `onDataAvailable` while a used input has no signal, and exactly one wake once
  the last input connects, carrying every input's initial event at once.

### 5.1 What Phase 0 found

Four properties the traces made explicit. None is a bug; all four are things a refactor could
silently change, and two of them bear on the target model in §3.

1. **`Synchronizing` cannot survive a re-derivation — with today's ladder.** A `NeedMoreData` attempt
   discards every packet of the lagging input that lies below the target, so that input is *empty*
   afterwards and the next evaluation stops one rung earlier, at `WaitingForData`. `NeedMoreData`
   therefore always implies "the affected input has no data", and today the two substates alternate:
   attempt → `Synchronizing`, re-derive → `WaitingForData`, new packet → attempt again.

   **Resolved for the target model (reviewed):** the missing input to the re-derivation is the
   *latched target*. Inside `SynchronizingState`, "no data on an input **and** a synchronization target
   already latched" means exactly "waiting for that signal to catch up", which is `Synchronizing`, not
   `WaitingForData`. So the condition becomes derivable once `SynchronizationManager::pendingCandidate`
   is part of what `checkTransitionCriteria` reads.

   **This is a deliberate trace change, not a regression.** `AlignmentIsIterativeAndSynchronizingIsTransient`
   currently pins `re-derived after the attempt -> WaitingForData[0]`; in the phase that introduces
   `SynchronizingState` that line becomes `Synchronizing[0]`, and the test's peek/probe split can then
   collapse. Update the golden trace in the same commit that makes the change, and say so in the
   message - it is the one place the "identical substate for identical inputs" invariant is
   consciously broken.
2. **`Incompatible` is reachable without passing through `WaitingForData`.** The model rung (8) sits
   above the data rung (10), so a reader whose events have just been consumed can go straight to
   `Incompatible` from the read that consumed them. **Consequence for Phase 2:** the slice boundary
   between `WaitingForValidInputsState` and `SynchronizingState` is not "has data yet"; it is
   "has a valid cross-input model", exactly as §3.1 states — the ordering must be preserved when the
   rungs are distributed across the two classes.

   The classification behind that boundary holds exactly, which is worth stating because it is what
   makes the two failure substates unambiguous: **`Incompatible` is always descriptor or configuration
   driven** — every `SyncSetupIssue` (`MissingDomainDescriptor`, `InvalidSampleRate`, `RatesNotEqual`,
   `RequiredRateNotDivisible`, `ArithmeticOverflow`) and every per-input `QueueReaderIssue` is computed
   from descriptors plus builder configuration, never from sample values. **`SynchronizationFailed` is
   always data-timing driven** — all three `SyncFailureReason`s (`SyncDistanceExceeded`,
   `TargetNotRepresentable`, `NoCommonTick`) are computed from the actual first samples. Nothing
   crosses over.
3. **A disconnect used to keep what the slot had adopted — now it discards it (changed).** Nothing in
   the disconnect path dropped the queue, so a reconnect while data was still buffered went straight
   back to `Synchronized` on the old signal's samples, with the new connection's descriptor event
   landing *behind* them.

   **Changed, reviewed:** a port may be reconnected to a *different* signal, so nothing of the old one
   may survive. `QueueReader::updateConnection` now discards the adopted packets, the pending events
   and the cached descriptors whenever the **connection identity** changes. Identity rather than
   "became null", because `InputPortImpl::connectInternal` replaces the connection of an already
   connected port with `notifyListener = false` — a signal swap reaches the slot as `connected()`
   only, with no disconnect at all. One check therefore covers connect, disconnect and replace, and
   leaves the first rebind of an already-connected port (construction, port adoption,
   `setInputUsed(true)`) alone, which is what keeps the descriptor `setListener` front-loads.
   After a reconnect the reader is back to establishing the input from its descriptors, so the
   handshake window of §2.1 is now genuine on a reconnect too (it was not before: the descriptors
   survived).

   **Consequence for Phase 4:** a specialized `slotConnected` edge may now assume the slot starts
   empty and without descriptors — but must still not assume a connect implies a pending event, since
   the descriptor packet arrives after the callback.
4. **An unused input's queued event was invisible while it sat behind that input's own leftover
   data — the leftover data is now dropped at exclusion time (changed).** `InputState::Event` derives
   from `hasPendingEvents()`, which is leading-only, and `setInputUsed(id, false)` did not drop what
   the slot had already adopted; only the re-enable did (`dropForInactive`).

   **Changed, reviewed:** `setInputUsed(id, false)` now calls `dropForInactive` itself. The data is
   unreadable either way — re-enabling restarts from the live stream — so dropping it at exclusion
   time costs nothing and is what lets the events lead. That matters because forwarding events is the
   only job an unused input has left: the per-input `Event` state is the recovery signal a consumer
   answers with `setInputUsed(id, true)`.

   Still open (deliberately): the producer path stamps the data-loss monitor for unused inputs too
   (`slotPacketReceived` → `onPacket`). Harmless today — `lostSlots()` requires `monitored && armed`
   and an unused slot is not monitored — but it is arming state maintained for nobody. To be
   revisited when the edges are specialized.

### Phase 1 — extract `StateContext` (**implemented**)

The evaluation is now a free function over the collaborator bundle:
`multi_reader::evaluateStateLadder(StateContext&)` in `multi_reader/state_evaluation.cpp`, with
`StateContext` in `multi_reader/state_context.*`. The rungs are unchanged, in the same order, with
the same comments; only what they reach *through* changed. `MultiReaderImpl::evaluateStateLocked`
builds the context, runs the evaluation, applies its verdict and publishes the producer gate.

The collaborator set turned out to be separable with **two** exceptions, both facade-owned caches
derived from the model: the status cache (`cachedStatus` and friends) and the main-input descriptors.
The evaluation records that they need attention (`StateContext::modelInvalidated`,
`mainDescriptorsStale`) and the facade applies both the moment the evaluation returns. That is
equivalent to doing it inline, which is what keeps the phase behaviour-preserving: nothing between
the rungs that set the flags and the end of the evaluation reads either, and no status can be built
in between (statuses are built on the read and query paths, after the evaluation).

Two further findings, both recorded in the code:

- **One evaluation produces exactly one outcome.** Every rung assigns the state and returns, so the
  verdict is a value (`StateOutcome`) the facade applies once through the unchanged
  `setStateLocked` — which keeps the `stateChangeNotify` edge check (contract item 7) comparing
  against the state it is replacing.
- Three operations over the slot vector are shared with the facade's read/query/callback paths
  (`publishSlotBasis`, `collectUsedReaders`, `findSlotById`) plus the two gate writes. They belong to
  neither side and are now free functions in `multi_reader/input.h`; the facade's `...Locked` wrappers
  forward to them, so the read paths are untouched.

`ReaderState` moved to `multi_reader/reader_state.h` so a `multi_reader/` header can name it without
depending on the facade.

**Note for the rest of this document:** the `multi_reader_impl.cpp:NNN` line references in §1, §3 and
§4 predate this phase. The ladder they point into now lives in `state_evaluation.cpp`; the rung
comments (`// 1.`, `// 4./5.`, …) are the stable way to find each one.

### Phase 2 — introduce the hierarchy, `checkTransitionCriteria` only (**implemented**)

Five classes in `multi_reader/state_machine.*`, each owning the rungs its substates come from, plus
`runStateEvaluation` — the bounded loop of §3.3. Verified against the golden traces, not by reading:
all 16 unchanged.

**The class is derived, not stored.** `stateClassOf(substate, isActive)` is total — the classes are
*defined* as a grouping of the substates, so the class is a pure function of the substate, with
`EventPending` disambiguated by the active flag. There is therefore no `currentState` member: the
runner starts from `stateFor(stateClassOf(ctx.currentState, ctx.isActive))`. Storing it would be
storing a derived value, i.e. inventing a way for the class and the substate to disagree. (Adding the
member back is one line if a later phase needs per-class data — the flyweights have none.)

**Where the rungs went.** The order of §4 item 2 is preserved exactly; what changed is who owns each
rung.

| Owner | Rungs |
|---|---|
| shared input guard | invalid, the unused-slot drain, active, used set + main input (2), connections (3), the monitoring refresh and leftover-segment discard, events (4/5), descriptors (6), per-input validity (7) |
| `ErrorState` | none — always settles on `Error`, keeping the message that explains why |
| `InactiveState` | the inactive arm: disarm monitoring, clear-then-drain each used input, surface its events |
| `WaitingForValidInputsState` | the guard is its own computation; on success it hands off |
| `SynchronizingState` | data loss (9), the model (8), data (10), alignment (11/12) |
| `ReadyState` | data loss (9) and the synchronized fast path |

**The guard is shared because the machine is level-triggered.** Any of its conditions can appear at
any time, whichever state the reader was in, so every downstream class has to re-derive it — a
`ReadyState` that trusted "the inputs were fine last time" would be the classic FSM failure mode §2
exists to prevent. It is memoized per evaluation (`StateContext::guard`) because it drains queues and
clears arrival flags: correctness allows running it twice, the read path does not.

**Two transition forms, which is what keeps one condition in one place.** A *hand-off*
(`Transition::handOff`) names a class and no substate — the receiving class derives it, and the loop
iterates. A *verdict* (`Transition::settled`) carries the substate and is final even when it names
another class's substate, because the class follows from `stateClassOf`. So `Ready` reporting
`DataLost` is one verdict, not a hand-off followed by a re-derivation.

The loop is genuinely used (`Inactive` → `WaitingForValidInputs` → `Synchronizing` is the longest
chain) and cannot cycle: hand-offs only ever run downstream. The bound turns an invariant break into a
diagnosable `Error` naming the class that would not settle.

**Deviation from the phase list:** the `slot*` edge virtuals are *not* added here. They would have no
call site and no semantics until Phase 4 specializes them, and a virtual nobody calls is worse than a
missing one — the facade's `slotConnected`/`slotDisconnected` still run the full evaluation, which is
exactly what `reevaluate()` would have meant.

### Phase 3 — side effects as invariants (**implemented, and much smaller than planned**)

The premise of this phase was wrong. Almost none of the listed side effects are entry/exit
invariants of a state class; they are effects of the *rung* that decided the state, and the same
substate reached by two rungs legitimately needs two different effects. What survived is one rule,
applied in one place.

**What moved.** `applyStateInvariants` in `state_machine.cpp`, called by the runner where the machine
settles:

> `EventPending` and `DataLost` always clear the synchronization.

That replaces four separate inline calls and makes the rule true by construction on all five paths to
`EventPending` (the guard's event rung, the guard's validity rung, the inactive arm, the model rung and
the alignment rung) rather than by coincidence on four of them. Level-triggered, like everything else
here: applied whenever the machine settles on the substate, not only on the transition into it — which
is what the rungs did before, and is idempotent.

**Why the rest stayed put.** Each of these was tried and rejected for a concrete reason, not for
convenience:

| Candidate effect | Why it is not an entry/exit invariant |
|---|---|
| `invalidateModel` | Not uniform per substate. `WaitingForConnections` invalidates the model when the main input is dangling or an input has no signal, but only the synchronization when there are no used inputs at all; `Incompatible` invalidates the model from the validity rung but only the read pipelines from the model rung (where the build itself already failed). Folding these together would be a behaviour change dressed as a refactor |
| **`EventPending` must NOT invalidate the model** | The event rung *builds* the model opportunistically so `getCommonSampleRate`/`getTickResolution` work while events are still pending. A class-level "clears the model" invariant would destroy exactly that. This also **corrects §3.1**: `WaitingForValidInputsState`'s invariant is not "no valid cross-input model" |
| data-loss monitor arm/disarm | Level-triggered *and* driven from outside the machine (`slotDisconnected`, `setInputUsed` both call `setMonitored`). Entry-only arming would desynchronize the moment either of those ran while the class stayed the same |
| `readCoordinator.configure` + `nextReadTick` | Belongs to "a start was just established", not to "entering `ReadyState`". As an entry effect it would fire when `SynchronizingState` hands off to `ReadyState` on an already-assigned start and **rewind the read offset** to the common start, because `nextReadTick` advances with every read while `readOffset()` returns the start |
| `stateChangeNotify` latch | Already in exactly one place, and correct: `setStateLocked` compares the new substate *and* the new affected set against the ones it is replacing. Splitting it across `WaitingForValidInputsState` (`Incompatible`) and `SynchronizingState` (`SynchronizationFailed`, `DataLost`) would lose that comparison and gain nothing |
| all-slot event-bit suppression | Two different cross-input rules with different conditions (a used input with no signal; a connect handshake in flight), and the second one does not correspond to a state at all — it falls through to `WaitingForDescriptors` |

**No `onEnter`/`onExit` virtuals were added.** With one rule left, and that rule keyed on the substate
rather than on the class, a pair of virtuals per class would be five empty overrides and a hook with no
user. Same reasoning as the `slot*` edges in Phase 2: a virtual nobody calls is worse than a missing
one.

### Phase 4 — measure, and specialize nothing (**implemented**)

**Parity, measured properly.** Baseline = the last commit before the refactor (Phase 0); head =
Phase 3. Both binary sets kept side by side and run **alternately**, with the order flipped every
repetition, 5 repetitions over 23 throughput points in 5 scenarios:

> median **+1.0 %**, mean **+0.7 %**, range −6.3 % … +13.8 %, and **not one point whose delta exceeds
> its own run-to-run spread.**

*Methodology matters more than the numbers here.* The first attempt ran all of head, then all of
baseline, and reported a +7.7 % median "improvement" plus one −22.3 % "regression"
(`event_inputs/64`) — both artifacts. The whole-set offset had an identified external cause: the
machine was unplugged and replugged between the two halves, so the two sets ran at different CPU
frequency-scaling states. That is a one-off, not evidence that this benchmark cannot be run
sequentially.

Two things do generalize:

- **Interleave anyway.** It is nearly free and it makes the comparison immune to any whole-set offset,
  whatever the cause — power state, thermal, a background build. A sequential A/B is only as good as
  your confidence that nothing changed in between, and that confidence is not verifiable after the
  fact.
- **Some points here are inherently too noisy to read individually.** In the interleaved run, with no
  power event, `event_inputs/64` still spread 49–70 % between repetitions and the small-packet `stress`
  points 30 %. A single-digit delta on those points means nothing in either direction; only the
  population of 23 points supports a conclusion.

Scenario choice also differs from the plan. `stress` and `maxrate` are steady-state, which makes them
the *least* sensitive scenarios to this refactor — the machine is not on that path at all (below). The
runs above therefore add `events`, `event_inputs` and `resync`, which force a full evaluation per
iteration and are where a regression would actually appear.

**Nothing was specialized, because the edges that matter were never on the machine's path.**

- The producer path does not enter the state machine: `Input::packetReceived` stamps the arrival,
  raises its gate flags from an O(1) connection introspection and schedules only when the gate is
  open. Phases 1-3 did not touch it.
- The two steady-state consumer passes — `refreshDataPlaneLocked` (read and query) and
  `updateCallbackStateLocked` (the coalesced task) — bypass the machine entirely, gated on
  `state != Synchronized`. **They are the specialized `ReadyState` edges this phase was meant to
  write**, they predate the refactor, and they are hand-written for the hot path.
- So by the time a packet arrival reaches the machine, the machine is running *because* one of those
  passes escalated: an event surfaced, a deadline expired, or the reader is not Ready. Those are
  exactly the cases that require the full re-derivation, so a cheap `ReadyState::slotPacketArrived`
  would have nothing to do.
- Rewriting the escalation test as `stateClassOf(...) == StateId::Ready` would be *less* defensive for
  zero gain: `Synchronized` maps to `Ready` regardless of the active flag, so the class test cannot see
  `!isActive`, which the current three-clause guard checks explicitly.

**Guardrail audit.** No new allocation and no new dispatch on the producer path (untouched). Virtual
calls per evaluation: 2-3 (one per class in the hand-off chain) rather than the "at most one" the plan
asked for — and zero on the steady read path, since the machine is not entered there. §7's predicted
risk of "one extra indirection on the read path" did not materialize for the same reason.

### Phase 5 — split the inactive arm (**delivered by Phase 2**)

Nothing is left of this phase: the slicing had to place the inactive arm somewhere, and the only
honest place was `InactiveState::checkTransitionCriteria`. `if (!isActive)` is now a routing decision
in the shared guard (`GuardOutcome::NotActive` → hand off to `InactiveState`) rather than a second
inline ladder, and the two `EventPending` arrivals are two separate bodies — the inactive arm and the
guard's event rung — each reachable on its own.

What cannot go away is the `isActive` *test*: the machine is level-triggered, so every class has to
re-derive whether the reader is still active. "`setActive` becomes a class transition" is true in the
sense that matters (the arm is a class), not in the sense of the test disappearing.

### Phase 6 — documentation and cleanup

Correct `multi_reader.md:111-112` (the "top-to-bottom ladder" claim) and the three false
notification-ordering comments listed in §2.2, add the class/substate table, the `InactiveState`
vocabulary note and the `SameThread` precondition to §4, remove dead scaffolding, renumber the
surviving inline step comments or drop the numbering entirely.

### Not in scope (candidate Phase 7)

Configuration edges — `setActive`, `addInput`, `removeInput`, `setInputUsed`, `setMainInput` — all
funnel into a full re-derivation today (`:2082`, `:2142`, `:2189`, `:2224`, `:2267`) and have no
state-specific behaviour to specialize. Adding virtuals for them now would be speculative. Revisit
only if a measured cost appears.

---

## 6. Open questions for review

1. **`DataLost` placement** — `SynchronizingState` (recommended, preserves
   `ReadyState => commonStart != nullptr`) or `ReadyState` (matches "was working, degraded")?
2. **`EventPending` in two classes** — accept as the honest encoding of the existing fork, or treat
   convergence as a follow-up?
3. **Flat `ReaderState` substate enum** — keep (recommended: status mapping and `getInputStates`
   depend on it) or split per class? **Answered by phase 2:** keep. The flat enum is what makes the
   class derivable (`stateClassOf`) instead of a second variable to keep in sync.
4. **Where do the read-path transitions live?** `readInternal`'s commit failure → `Error` (`:1740`)
   and event surfacing in `readEventsLocked` (`:1588`) are state transitions driven by the read, not
   by a slot notification. Options: a `readCommitFailed(ctx)` edge on the base class; or leave them
   as direct `transitionTo(ErrorState)` calls from the read path. I lean to the latter — a commit
   failure is state-independent by definition.
5. **Is `slotAcceptsSignal` an edge?** It is the fourth `IInputListener` method and is currently
   pure pass-through with no state involvement (`:1253-1265`). Recommend leaving it off the base
   class.
6. **Phase 4 scope** — specialize only `ReadyState::slotPacketArrived`, or attempt all five
   classes? Recommend the former until a benchmark says otherwise.
7. **How is the `SameThread` precondition (§2.1) enforced?** See §6.1 — this is the one open
   question that changes production code outside the state machine.
8. **Does `Synchronizing` stay a substate?** Phase 0 showed it is not re-derivable (§5.1, finding 1):
   it names "an attempt just happened and needs more data", not a condition the ladder can recompute.
   Options: keep it as the attempt's result (recommended — it is what the status message reports, and
   dropping it would make a `NeedMoreData` attempt indistinguishable from never having tried), or fold
   it into `WaitingForData` and carry the distinction in the message only.

---

### 6.1 Enforcing the `SameThread` precondition

Three options, in decreasing severity. All of them touch only `createSlots`
(`multi_reader_impl.cpp:328-356`); the rest of the plan is unchanged either way.

| Option | Behaviour | Cost |
|---|---|---|
| **a. Assert / reject** | `createSlots` throws if a resolved notification method is not `SameThread` | Breaking API change: `setInputPortNotificationMethod` / `setInputPortNotificationMethods` are public on the builder and `Scheduler` is currently legal. Needs a deprecation story |
| **b. Hardcode, documented** | `createSlots` always calls `port.setNotificationMethod(SameThread)`, ignoring the builder value; setter deprecated | Silently overrides a user setting unless clearly documented |
| **c. Normalize** | Keep accepting the setting at the API level, force `SameThread` on the port, document *why* the setting is redundant | Same as (b), framed as an optimization rather than a restriction |

**Recommendation: (b) or (c)** — they differ only in documentation posture. Both are justified by
§2.1 point 4: the port's `Scheduler` mode adds a hop in front of a reader that already coalesces
onto a scheduler thread, so it costs performance and buys nothing. (a) is the honest choice if the
project prefers loud failures to silent normalization, but it breaks callers for no functional gain.

There is precedent either way: the existing validation at `:337-342` already rejects `Unspecified`
for the signals path, so `createSlots` policing the notification method is not a new idea.

---

## 7. What this refactor does not fix

The ladder is a total function over observable state; partitioning it into five classes relocates
the conditions, it does not reduce them. Expect roughly the same number of branches afterward.

The wins are structural: the inactive fork becomes a class boundary instead of a hidden branch;
transition side effects become entry/exit invariants of a named state; each state becomes unit-
testable against a `StateContext` without a live reader; and the producer gate's `steady` predicate
collapses to `currentState->isReady()`.

The risks are one extra indirection on the read path, and the classic FSM failure mode — someone
later trusting an edge instead of re-deriving. §2 exists to be cited in that review.

### 7.1 Verdict after the fact

That prediction was right, and understated. Of the four claimed wins above, one landed as described
(the inactive fork is a class boundary), one was refuted (the side effects are not entry/exit
invariants — §5 phase 3), one is available but unused (no test yet drives a `StateContext` without a
reader), and one never applied (the gate's `steady` predicate is untouched, because the class is derived
from the substate rather than stored). The extra read-path indirection did not materialize: the machine
is not on that path at all.

**So the class hierarchy of §3 is the part of this effort that pays least.** It cost `StateId`,
`Transition`, five flyweights, a runner loop and a hand-off protocol - and the guard memo, which exists
*only* because splitting the derivation across classes made the shared guard run more than once. The
shape the code actually wanted is four named functions over the context:

```
if (invalid)      -> Error
drainUnusedSlots()
if (!isActive)    -> inactiveArm(ctx)
switch (inputGuard(ctx)) { blocked -> the blocker; validated -> alignmentTail(ctx) }
```

which keeps every gain of phases 1 and 3 (the context extraction, the named pieces, the single substate
invariant) and drops the rest, with the guard running exactly once by construction and no memo needed.
**Collapsing phase 2 that way is the recommended next change** — the golden traces verify a collapse
exactly as they verified the split. `stateClassOf` survives only if something wants the coarse grouping
for diagnostics or the status; otherwise it goes with the classes.

What was worth having, and is independent of all of the above: the characterization suite (§5, §5.1),
the two behaviour fixes it produced, the evaluation moved out of the facade, and the corrections to the
four false claims this document and the code used to make.
