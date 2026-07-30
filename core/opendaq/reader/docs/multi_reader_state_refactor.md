# Multi Reader state classes — plan

**Goal:** the multi reader's behaviour in a given set of circumstances lives in one class. Not a
verbose enum: each class owns what the reader *does* — how it reads, what it reports available, how it
gates producers, how it reacts to a connect or a packet — and only that class's exit conditions.

**Status:** implemented (§6 records what each step found). A first attempt (commits
`bdc44af2`..`d04d6d57`) split the *derivation* across five classes instead of the behaviour; it measured
as parity but bought little. This plan replaced it.

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

**The safety net for narrowing those checks** is the total derivation, kept as the reference:
`deriveState`. It is not dead code - it is the base `reassess`, which is the right answer for any
behaviour whose whole job is looking for progress - and a test hook forces every behaviour through it, so
each characterization scenario runs twice and the traces must match.

Note what that cross-check cannot be: the derivation has side effects (it drains queues, invalidates the
model, discards leftover segments), so running both derivations in one evaluation and comparing is
unsound - a `NeedMoreData` alignment attempt discards the lagging input's packets, and the second run
would legitimately answer `WaitingForData`. Two runs of the same scenario in separate reader instances is
the only sound comparison.

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

**The class is cached, and it follows the substate.** `MultiReaderImpl` holds `currentState`, a pointer
to a stateless flyweight, written in exactly one place (`applyStateOutcomeLocked`) from
`stateIdFor(substate, isActive)`. Reporting `EventPending` always clears the synchronization, so what
remains is re-establishment - or the inactive behaviour if the reader is deactivated - which is what keeps
that mapping total. Cached rather than recomputed because every read and query dispatches through it.

## 4. The interface

```cpp
/// Owner-thread only; every method runs with MultiReaderImpl::mutex held. Stateless flyweights: one
/// instance per behaviour, all mutable data reached through the StateContext.
class MultiReaderState
{
public:
    virtual StateId id() const = 0;

    // --- What the reader does here. The base implementations are the conservative ones (nothing
    // readable, every packet forces a full derivation, establishment gate semantics); ReadyState is
    // the only behaviour that overrides them, because it is the only one where a read can provably
    // return samples.
    virtual void publishProducerGate(StateContext&) const;
    /// @return true when the caller must run the full derivation afterwards.
    virtual bool refreshDataPlane(StateContext&, bool escalateOnEvent) const;
    virtual bool updateCallbackState(StateContext&) const;
    virtual SizeT availableCount(StateContext&) const;
    /// What kind of read this is: ReportState / ReturnEvents / ServeData. The facade executes it,
    /// since it owns the status caches and the read pipelines.
    virtual ReadAction planRead(StateContext&) const;
    virtual bool readWaitSatisfied(StateContext&, SizeT requested) const;

    // --- How it responds to the world changing. The base reactions invalidate what the change can
    // have invalidated and then derive from scratch.
    virtual StateOutcome slotConnected(StateContext&, SizeT slot) const;
    virtual StateOutcome slotDisconnected(StateContext&, SizeT slot) const;
    virtual StateOutcome activeChanged(StateContext&) const;
    /// addInput / removeInput / setInputUsed / setMainInput.
    virtual StateOutcome inputSetChanged(StateContext&) const;

    // --- Am I still the right behaviour? Only this one's exit conditions; the base is the
    // exhaustive derivation.
    virtual StateOutcome reassess(StateContext&) const;
};
```

Every decision returns a `StateOutcome` — the substate, its message and the inputs it names — and the
facade applies it through one funnel (`settleLocked`: build context, decide, apply, publish the gate).
There is no hand-off protocol: leaving a behaviour is a full derivation, because the conditions below the
one that changed have not been looked at since. That is also why the substate, not a stored class,
decides which behaviour comes next (`stateIdFor`).

There is no `slotPacketArrived`: the owner-side handling of an arrival *is* `refreshDataPlane` and
`updateCallbackState`. The producer path never enters the machine — it reads the policy those publish.

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

## 6. Steps — all implemented

Interface-first, one operation at a time, each verified against the golden traces (which run in both
cross-check modes, §2). Every step deleted a conditional rather than adding a layer.

1. **`publishProducerGate` and the `wakeOnAnyPacket` policy.** The `steady` predicate is gone. This is
   also the answer to "how can the lock-free producer path be state-aware": it cannot call a virtual, but
   the behaviour *publishes* each slot's policy, and `Input::packetReceived` reads it.
2. **`availableCount`** — `ReadyState` computes it, the base returns zero. `getAvailableCount`'s
   `if (state == Synchronized)` is gone, and the leading-event guard moved inside `ReadyState`, which is
   the first example of a behaviour answering "I am holding an event" from within.
3. **`refreshDataPlane` + `updateCallbackState`** — both three-clause guards gone; `ReadyState` owns
   both fast passes. The facade's five data-plane members became one `DataPlane` struct threaded through
   the context, because the states maintain it and the facade only owns it.
4. **`read` + `readWaitSatisfied`** — `planRead` returns what kind of read this is
   (`ReportState` / `ReturnEvents` / `ServeData`) and the facade executes it, which keeps the status
   caches and the read pipelines where they belong. All four state tests are gone from `readInternal`.
5. **`reassess`** — `Error`, `Inactive` and `Ready` narrow it; `Establishing` and `InputsFailed`
   inherit the exhaustive derivation, which is the honest answer for a state whose job is to look for
   progress. Leaving a behaviour is always a full derivation.
6. **The notification edges** — `slotConnected`, `slotDisconnected`, `activeChanged` and
   `inputSetChanged` are on the interface, and `settleLocked` is the single funnel every one of them and
   every evaluation passes through (build context → decide → apply → publish gate).

### 6.1 What the implementation found

- **`slotPacketArrived` was not added.** The owner-side handling of an arrival already *is*
  `refreshDataPlane` (read/query) and `updateCallbackState` (the coalesced task). A third method for the
  same thing would have been a name, not a behaviour. The producer path still never enters the machine.
- **The base class carries the conservative implementations and `ReadyState` is the only overrider of
  the operations.** That is not the hierarchy failing to pay: it is the reader having exactly one
  behaviour that can serve data and four that cannot, now stated once instead of retested in six places.
- **What `ReadyState::reassess` still has to do** is drain the unused slots, clear-then-drain the used
  ones, check connectivity, discard leftover segments, check for a leading event and for a crossed
  deadline. What it skips is the used-set and main-input resolution, the monitoring refresh, and the
  descriptor and validity rungs - none of which can change while the reader stays synchronized without a
  notification that derives anyway. A real saving, not a dramatic one.
- **A context built per operation must be references only.** Threading `StateContext` into the read and
  query paths made its construction a per-call cost, and the first version snapshotted five
  configuration fields and carried a `StateOutcome` (a string and a vector) - about 6-8 ns a
  construction, twice per `getAvailableCount` and up to three times per zero-count read. The macro
  benchmark could not see it (its spreads are 12-65 %); the `micro` scenario measured it exactly:
  **+11 ns on `getAvailableCount` (+25 %) and +17 ns on a zero-count read.** Fixed by making every
  member a reference and moving the outcome out, then building one context per public call and passing
  it down - which also makes a context correct across an escalation that changes the state under it.
  Back to parity (+0.8 / +2.8 / +4.0 ns, all inside spread). **Anything added to that struct has to be
  a reference, and the `micro` scenario is the only place a regression of this size is visible.**
- **One edge specialization exists:** `ReadyState::slotConnected` does not invalidate the model when the
  connect lands on an *unused* slot, since the model is built from the used inputs. The outcome is
  identical either way (an unchanged used set rebuilds an identical model, and re-synchronizing returns
  to the current frontier), so this is work avoided rather than behaviour changed - which is why no trace
  moved and why the test that pins it passes in both cross-check modes.

## 7. What already exists to build on

- `test_multi_reader_state_transitions.cpp` — 16 golden traces of
  `trigger -> substate[affected] detail`, read through `MultiReaderImpl::getStateForTest`. They verified
  three structural rewrites without a false pass. They are the reason this can be done at all.
- `StateContext` (`multi_reader/state_context.*`) — the collaborator bundle the state code works on:
  slots, sync manager, read coordinator, data-loss monitor, the configuration it reads, the single
  `StateOutcome` it produces, and two deferred flags for the facade-owned caches it cannot reach
  (`modelInvalidated`, `mainDescriptorsStale`). Extend it rather than reaching back into the facade.
- `multi_reader/state_machine.*` — the five behaviours, the flyweight registry, `deriveState` (the
  reference derivation, in four named rung functions) and `applyStateInvariants` (item 9 of §5, in one
  place).

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
