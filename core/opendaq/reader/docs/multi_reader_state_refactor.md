# Multi Reader state classes — plan

**Goal:** the multi reader's behaviour in a given set of circumstances lives in one class. Not a
verbose enum: each class owns what the reader *does* — how it reads, what it reports available, how it
gates producers, how it reacts to a connect or a packet — and only that class's exit conditions.

**Status:** plan, for review. A first attempt (commits `bdc44af2`..`d04d6d57`) split the *derivation*
across five classes instead of the behaviour; it measured as parity but bought little, and §7 records
what to keep from it. This plan replaces it.

**Scope:** `MultiReaderImpl`'s state handling and the state-dependent branches of its read, query and
notification paths. No change to `SynchronizationManager`, `QueueReader`, `ReadCoordinator`,
`CallbackGate` or any public interface.

**Companion:** [`multi_reader.md`](multi_reader.md) — §4 (states), §8 (evaluation), §9 (data plane).

---

## 1. Why

The state-dependent behaviour is not in one place. It is smeared across six functions as the same
few conditions, retested:

| Site | The conditional |
|---|---|
| `refreshDataPlaneLocked` | `invalid \|\| !isActive \|\| state != Synchronized` → full evaluation, else the fast pass |
| `updateCallbackStateLocked` | the same three-clause test again |
| `readInternal` | `invalid` → Fail; `EventPending` → return events; `!= Synchronized` → count 0; else plan + commit |
| `readInternal`'s wait predicate | wait for `EventPending`, or for aligned availability |
| `getAvailableCount` | `if (state == Synchronized)`, plus a leading-event guard inside it |
| `publishProducerGateLocked` | `steady = state == Synchronized && hasModel` → two entirely different gate policies, including each slot's `wakeOnAnyPacket` |

Every one of those is "what does the reader do in this state". Each should be a method on the state.

## 2. The one rule: level-triggered, per class

Conditions appear without notification — producers are lock-free, so arbitrary data and any number of
events can arrive between two evaluations — so a state must always be *re-derived from ground truth*,
never remembered. That is the constraint, and it is not negotiable.

It does **not** mean every state must re-derive every condition. A class re-deriving the conditions
that can take it out of *its own* state is level-triggered. The previous attempt conflated the two and
ended up with one shared guard every class had to run, which is what made the classes labels.

So: `reassess()` per class, checking that class's exit conditions and naming its successor.

**The safety net for narrowing those checks** is the existing total derivation, demoted to a test-only
oracle. It stays in the code, is never called in release, and the characterization fixture asserts
after every trigger that the class's verdict equals the oracle's. A forgotten exit condition then
fails a trace instead of shipping.

## 3. The classes

Group the substates by behaviour, not by which rung produced them:

| Class | Substates | Behaviour |
|---|---|---|
| `ErrorState` | `Error` | every operation fails; terminal, latched |
| `InactiveState` | `Inactive` | no data flow; events still adopted and surfaced; no data-loss monitoring |
| `EstablishingState` | `WaitingForConnections`, `WaitingForDescriptors`, `WaitingForData`, `Synchronizing` | nothing readable yet; every packet forces a reassessment; the gate opens on the first sample |
| `InputsFailedState` | `Incompatible`, `SynchronizationFailed`, `DataLost` | persistent condition naming its inputs, needs consumer action; one latched wake, not re-latched while unchanged |
| `ReadyState` | `Synchronized` | aligned blocks readable; the only state with a data-plane fast path and with producers self-gating |

That is `ReadStatus` (`Fail`, `Inactive`, `Preparing`, `InputsFailed`, `Ok`) — the public contract
already names the behavioural partition, which is a good sign it is the right axis.

**Pending events are a condition, not a state.** Every class answers three questions about them
instead of handing off to an event state:

- `read`: a leading event on any used input is returned before data. Consuming it invalidates the
  synchronization, so the class hands off afterwards.
- `availableCount`: zero while a leading event blocks (this is what `getAvailableCount`'s existing
  guard already does inside the synchronized branch).
- `publishProducerGate`: the event flag opens the gate.

`ReaderState::EventPending` stays as the *reported* substate while unconsumed events are held, so
`ReadStatus::Event`, the state message, `InputState::Event` and `getIsSynchronized` keep working
unchanged.

**The class is stored, not derived.** With `EventPending` reportable from four classes, the substate no
longer determines the class. `MultiReaderImpl` holds `currentState` (a pointer to a stateless
flyweight) and the substate remains the diagnostic detail within it.

## 4. The interface

```cpp
/// Owner-thread only; every method runs with MultiReaderImpl::mutex held. Stateless flyweights: one
/// instance per class, all mutable data reached through the StateContext.
class MultiReaderState
{
public:
    virtual StateId id() const = 0;

    // --- What the reader does here ---
    virtual MultiReaderStatusPtr read(StateContext&, ReadRequest&) const = 0;
    /// The timed read's wait predicate: has this state become able to serve the request?
    virtual bool readWaitSatisfied(StateContext&, const ReadRequest&) const = 0;
    virtual SizeT availableCount(StateContext&) const = 0;
    /// The data-plane pass for the read and query paths (escalateOnEvent distinguishes them).
    virtual void refreshDataPlane(StateContext&, bool escalateOnEvent) const = 0;
    /// The coalesced task's pass: decides only whether onDataAvailable should fire.
    virtual void updateCallbackState(StateContext&) const = 0;
    /// Per-slot gate policy: thresholds, event/ready flags and wakeOnAnyPacket.
    virtual void publishProducerGate(StateContext&) const = 0;

    // --- How it responds to the world changing ---
    virtual Transition slotConnected(StateContext&, SizeT slot) const = 0;
    virtual Transition slotDisconnected(StateContext&, SizeT slot) const = 0;
    virtual Transition slotPacketArrived(StateContext&, SizeT slot) const = 0;
    virtual Transition activeChanged(StateContext&, bool active) const = 0;
    /// addInput / removeInput / setInputUsed / setMainInput.
    virtual Transition inputSetChanged(StateContext&) const = 0;

    // --- Am I still the right state? Only this class's exit conditions. ---
    virtual Transition reassess(StateContext&) const = 0;
};
```

`Transition` stays as it is: either a verdict (substate + message + affected inputs) or a hand-off to
the class that owns what happens next. A hand-off carries no substate — the receiving class derives
it — which is what keeps one condition in one place.

## 5. What a rework must not break

The properties the current code establishes deliberately, i.e. the ones most likely to be destroyed
silently. Each is covered by `test_multi_reader_state_transitions.cpp`; keep it that way.

1. **`invalid` ⇔ `Error`**, checked before everything including inactivity, terminal, and hardened at
   status construction (`invalid && state != Error` is coerced to `Error`).
2. **Clear-then-drain** for `packetPending`: clear *before* draining, or a packet enqueued in the
   drain→clear window is stranded on the connection. Must not end up duplicated across an edge handler
   and a reassessment.
3. **Two all-slot event-bit suppressions**, both cross-input rules: while a used input has no signal,
   and while a connect handshake is in flight (the window between `connected()` and the descriptor
   packet, which every connect passes through). Neither can be delegated to a single `Input`.
4. **The three-depth split** must survive: the coalesced task does not run the ladder for events, the
   query path records them, the read path surfaces them. This is a measured performance contract.
5. **`stateChangeNotify` is edge-triggered** on the `InputsFailed` family and must not re-latch on an
   unchanged failure — otherwise healthy-input packets re-fire the callback while a failure persists.
   `InputsFailedState`'s natural property; it needs the previous affected set to compare against.
6. **Nothing in the state path may schedule.** It runs under the state mutex and the inline
   (no-scheduler) executor would re-enter `onCoalescedEvaluation` and recursive-lock.
7. **The callback gate is independent of the state.** `onDataAvailable` can legitimately fire while the
   state is stale; the machine must not become a precondition for the gate.
8. **`DataLost` is in-band**: an input is lost once it has *less than one aligned block* buffered, not
   once it is empty. Gating on empty stalls the reader in `Synchronized` forever when a divider differs
   from `blockLcm`.
9. **`EventPending` and `DataLost` always clear the synchronization**, on every path to them.
10. **`ReadyState` implies `commonStart != nullptr`**, which is what makes the fast path and the
    producer gate's steady policy sound.

Two facts worth knowing before writing `reassess`:

- **`Synchronizing` is only derivable with the latched target.** A `NeedMoreData` attempt discards the
  lagging input's below-target packets, so that input is *empty* afterwards, which reads as
  `WaitingForData`. "No data **and** a target already latched" is what means "waiting for that signal to
  catch up", so `EstablishingState::reassess` must read `SynchronizationManager::pendingCandidate`.
- **`Incompatible` is always descriptor- or configuration-driven; `SynchronizationFailed` is always
  data-timing-driven.** Every `SyncSetupIssue` and `QueueReaderIssue` is computed from descriptors plus
  builder configuration; all three `SyncFailureReason`s from actual sample values. Nothing crosses over,
  which is what makes them one class with two unambiguous substates.

## 6. Steps

Interface-first, one operation at a time, each step behaviour-preserving and verified against the
golden traces. Each deletes a conditional rather than adding a layer.

1. **`publishProducerGate` and the `wakeOnAnyPacket` policy.** Smallest and cleanest split, and it
   deletes the `steady` predicate. Also the answer to "how can the lock-free producer path be
   state-aware": it cannot call a virtual, but the class *publishes* each slot's policy, so the producer
   follows it without a call.
2. **`availableCount`** — `ReadyState` computes it, everyone else returns zero.
3. **`refreshDataPlane` + `updateCallbackState`** — the biggest win: both three-clause guards go, and
   `ReadyState` becomes the sole owner of the fast passes.
4. **`read` + `readWaitSatisfied`** — this is where the class re-grouping of §3 happens, and where
   pending events become a per-class concern.
5. **`reassess`** — narrow the per-class checks. Land the test-only oracle and its cross-check *first*.
6. **The notification edges** — route `slotConnected` / `slotDisconnected` / `slotPacketArrived` /
   `activeChanged` / `inputSetChanged` into the classes, each naming its successor. The shared guard and
   its memo disappear here.

## 7. What already exists to build on

- `test_multi_reader_state_transitions.cpp` — 16 golden traces of
  `trigger -> substate[affected] detail`, read through `MultiReaderImpl::getStateForTest`. They verified
  three structural rewrites without a false pass. They are the reason this can be done at all.
- `StateContext` (`multi_reader/state_context.*`) — the collaborator bundle the state code works on:
  slots, sync manager, read coordinator, data-loss monitor, the configuration it reads, the single
  `StateOutcome` it produces, and two deferred flags for the facade-owned caches it cannot reach
  (`modelInvalidated`, `mainDescriptorsStale`). Extend it rather than reaching back into the facade.
- `multi_reader/state_machine.*` — `StateId`, `Transition`, the flyweight registry and the current
  derivation. The derivation becomes the test oracle of §2; the rest is the skeleton to grow.
- `applyStateInvariants` — item 9 of §5, already in one place.

## 8. Open

- **Enforcing `PacketReadyNotification::SameThread`.** The state path assumes it: with `Scheduler`,
  `packetReceived` is dispatched to a pool thread while `connected()` runs on the connecting thread, so
  the reader can observe them out of order. Every test and the benchmark already use `SameThread`, and
  the port's scheduler mode is redundant in front of a reader that already coalesces onto a scheduler
  thread. Options: reject it in `createSlots` (breaking, since the builder setter is public), or force
  `SameThread` on the port and document the setter as redundant. Recommend forcing.
- **A fault-injection seam for commit failure.** `readInternal`'s `commit` failure is the one path to
  `Error` that no sequence of public calls can produce, so it is characterized through `markAsInvalid`
  instead. A `ReadCoordinator` seam like the data-loss clock injection would close it.
