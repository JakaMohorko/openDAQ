# Multi reader: a 4-state machine with an explicit settle loop — plan

**Status:** in progress. The types and the settle skeleton are drafted in `reader_state.h` and
`multi_reader_impl.{h,cpp}`; the evaluations are stubs. Supersedes
[`multi_reader_state_refactor.md`](multi_reader_state_refactor.md), which this change folds into once
done.

## Context

The multi reader's state is an 11-entry `ReaderState` enum that mixes three different things: *what the
reader does* (nothing readable / serve aligned blocks / everything fails), *which rung of the derivation
stopped* (`WaitingForConnections`, `WaitingForDescriptors`, `WaitingForData`), and *what is wrong*
(`Incompatible`, `SynchronizationFailed`, `DataLost`, `EventPending`). Conflating the three means nothing
can be reasoned about locally: `stateIdFor` exists only to map 11 substates onto 5 behaviour classes,
`EventPending` has to be special-cased out of that table, and `Synchronizing` is not derivable from
ground truth without reading a latch inside `SynchronizationManager`.

The replacement separates them:

- **State** — the four things the reader *is*. `Establishing` → `Synchronizing` → `Ready` are increasing
  **ranks**; `Error` sits off the ladder.
- **Fault** — on a rank, why the next rank is not reachable; in `Error`, what the consumer has to fix.
  Refreshed every pass, never a state of its own.
- **Stages** — the checks inside a state's evaluation. Unnamed: a stage is a step, not a status.
- **Trigger** — why a transition happened, and also the *instruction* the machine acts on (a trigger
  implying a topology change invalidates the model, in one place).

One `settleStateLocked` call runs the stages and lands in the right state, so a caller no longer needs to
know where it will end up — it says "re-establish, because the connections changed".

**Recovery becomes explicit.** The `invalid` kill-switch goes (§1.4): being able to permanently kill a
reader serves no purpose, and while it existed it made "unrecoverable" a property of a hidden flag rather
than of the state. `Error` is a state the reader waits in for the consumer to act, and the only way out
is a consumer-driven trigger — a packet can never silently un-fail the reader.

`state_machine.*` and `state_context.*` are deleted; the state lives in `MultiReaderImpl`.

---

## 1. The types

`include/opendaq/multi_reader/reader_state.h`. `ReaderBehavior` is renamed to `ReaderState` in the same
commit that deletes the 11-entry enum, so nothing ever reads
`ReaderStateTransition { ReaderBehavior target; }`.

```cpp
enum class ReaderState : EnumType   // ReaderBehavior today
{
    Establishing = 0,  ///< rank 1: connections, events, descriptors, validity, the model
    Synchronizing,     ///< rank 2: aligning onto the common start
    Ready,             ///< rank 3: aligned blocks readable; the only state with a fast path
    Error              ///< off-ladder: waiting for the consumer to fix what the fault names
};
```

### 1.1 Faults

`FaultType`: `Unknown`, `Unconnected`, `MissingDescriptors`, `EventPending`, `Incompatible`,
`NotAligned`, `SynchronizationFailed`, `DataLost`, with `Fault { type, detail, culprits }`. `detail` is
the diagnostics verbatim from whichever component detected the fault — `SynchronizationManager`'s
messages carry values (rates, distances) that type + culprits cannot reconstruct, and `getStateMessage`
is the surface for exactly that.

On a rank, the fault is a property of the rank the reader is standing on — "why I cannot reach the next
one" — re-derived by that rank's own stages every pass, never a memory of how it got there. That is what
makes it safe to overwrite unconditionally.

`EventPending` is a fault on **every** rank rather than a state: unconsumed leading events name their
inputs and block advancement, which is what `Fault` records, and the behavioural consequence is small
enough for each state to answer from within ("a read returns events first").

### 1.2 What sends a fault to `Error`

The line is **how the fault is resolved**, not who is inconvenienced by it:

| Resolved by the normal read / produce flow → stays on a rank | Resolved by reconfiguration → `Error` |
|---|---|
| `Unconnected` — a signal connects | `Incompatible` — descriptors or builder configuration |
| `MissingDescriptors` — a descriptor arrives | `SynchronizationFailed` — exclude an input, pick a main input |
| `NotAligned` — data arrives | `DataLost` — park the input (`setInputUsed(false)`), re-enable when it recovers |
| `EventPending` — the consumer reads | `Unknown` — an internal inconsistency (see §1.4) |

`EventPending` is the case that shows why "needs consumer interaction" is the wrong discriminator: a read
is consumer action, but it is the ordinary protocol, not a reconfiguration. So the predicate that decides
`Error` is `requiresReconfiguration(FaultType)` — the drafted `needConsumerInteraction` renamed to what it
actually gates. `Error` ⇔ the settled fault requires reconfiguration, which makes the state a function of
the fault rather than a separate judgement.

`DataLost` is in the reconfiguration group: an input going silent is a serious non-standard operating
condition, and the way a consumer handles it is to park the input (`setInputUsed(false)`) and re-enable it
if it comes back — which is what the reference `SumReaderFb` already does with the whole `InputsFailed`
family. A resuming producer therefore does not recover the reader on its own; the consumer's
`setInputUsed` is the `UsedChanged` trigger that climbs back onto the ladder. The `stateChangeNotify`
latch (§2.1) is what makes that workable, since a crossed deadline carries no packet to wake anyone.

`Unknown = 0` is `Fault::type`'s default, so a `Fault` built without setting the type gets it. Classify it
with the reconfiguration group: a construction slip that reads as "still preparing" looks like a healthy
reader, whereas one that parks in `Error` fails loudly and names the site.

### 1.3 Triggers

`TransitionTrigger`: `Settled`, `Created`, `ConnectionChanged`, `UsedChanged`, `InputsReady`,
`SynchronizationSucceeded`, `EventsConsumed`, `ActiveChanged`, `Invalidated`, `TransitionLimitReached`,
`Faulted`.

`Settled` marks the terminal transition of a settle — no further transitions needed — and is what
`lastTrigger` holds once the reader comes to rest. The trigger describes *movement*, so `Settled` and
`Faulted` do not overlap: a transition with `target == currentState` is `Settled` whatever fault rides on
it, and `Faulted` is a move a fault caused. The fault always says why.

**The consumer-driven triggers are the exit from `Error`:** `ConnectionChanged`, `UsedChanged`,
`ActiveChanged` and `EventsConsumed`. `evaluateErrorLocked` always settles, so a level-triggered
re-evaluation — a packet, a deadline, a coalesced task — cannot leave `Error`. That asymmetry *is* the
explicit-recovery property: climbing back onto the ladder requires something the consumer did.

`Invalidated` no longer means "killed"; it is the trigger for an internal inconsistency reaching `Error`
(§1.4). Rename to `InternalError` if that reads better.

### 1.4 Removing the `invalid` mechanism

`invalid` disappears as reader state. Consequences to work through:

- **`MultiReaderImpl::markAsInvalid`** becomes a no-op — deprecated on the multi reader, with a logged
  warning. `IReaderConfig::markAsInvalid` stays on the interface because the single-input readers use it
  (`ReaderImpl`'s from-existing constructor calls it on the reader it supersedes), so this is a
  per-implementation deprecation, not an interface change. `MultiReaderFromExisting` is already slated for
  removal, so nothing else needs it.
- **`getIsValid`** returns `True` unconditionally — nothing can invalidate the reader.
- **The three `if (invalid)` early-outs** in `readInternal`, `getAvailableCount` and
  `onCoalescedEvaluation` become ordinary state dispatch: `Error` serves no data and reports its fault
  (§4). No hidden flag short-circuits the machine.
- **A commit failure** — the internal-invariant path in `readInternal` — transitions to `Error` with an
  internal fault. Recoverable like any other `Error`: a consumer trigger re-establishes from scratch,
  which is a better answer than dying.
- **`TransitionLimitReached`** lands in `Error` the same way.
- **Disposal keeps a flag**, because it is teardown rather than a state: after `internalDispose` the
  reader must stop touching members whose collaborators are being destroyed. A `disposed` bool read only
  by the notification and dispose paths expresses that; it is not part of the state machine and never
  reaches the status.
- **`ReadStatus::Fail` loses its multi-reader producer entirely.** Nothing about the reader is
  unrecoverable any more, so emitting a status that means "discard and construct a new reader" would
  contradict the model. Every `Error` fault projects to `InputsFailed`, and the status validity derived
  from `readStatus != Fail` is therefore permanently true. The enumerator stays: the single-input readers
  produce it and `GenericReaderStatusImpl` derives validity from it. This changes what the multi reader
  emits, not the enum.
- **An internal fault is then told apart by `culprits.empty()`** — a fault the reader raises about itself
  names no inputs, so `getReadStatus()` is `InputsFailed` while `getInputStates()` reports no failing
  input. That carries the distinction the removed `Fail` used to, without a value claiming the reader is
  dead.
- **The reference `SumReaderFb`'s `ReadStatus::Fail` branch becomes unreachable** for this reader
  (`sum_reader_fb_impl.cpp:476`, the `readerErrored` path). Follow-up, out of scope here: either keep it as
  defensive handling for other readers or re-key it on the empty-culprit test above.

### 1.5 Projections

New `src/multi_reader/reader_state.cpp` holds the enum→text and enum→public-status functions; a `.cpp` so
`multi_reader_status.h` stays out of a widely included header:

```cpp
const char* stateName(ReaderState);
const char* faultName(FaultType);
constexpr bool requiresReconfiguration(FaultType);   // the Error predicate
ReadStatus readStatusFor(FaultType);
InputState inputStateFor(FaultType);
```

`readStatusFor`: `Preparing` for the read/produce-flow group, `Event` for `EventPending`, `InputsFailed`
for the whole reconfiguration group — `Unknown` included, so it never returns `Fail`.
`inputStateFor`: `Pending`, `Event`, and the matching `Incompatible` / `SynchronizationFailed` /
`DataLost`; `Unknown` needs a total answer it can never actually give, since an internal fault names no
culprits. Both derive from `requiresReconfiguration` plus the `EventPending` case, so the grouping lives in
one place; dropping the `default:` arm lets `-Wswitch` catch a new enumerator.

`toReadStatus` in `multi_reader_impl.cpp`:

```cpp
if (hasEvents)     return ReadStatus::Event;   // this read is returning them
if (currentFault)  return readStatusFor(currentFault->type);
if (!isActive)     return ReadStatus::Inactive;
return currentState == ReaderState::Ready ? ReadStatus::Ok : ReadStatus::Preparing;
```

The state does not appear except as `Ok` vs `Preparing`: `Error` is fully described by its fault, which is
the point of making the state a function of the fault. `buildInputStateSnapshotLocked` gives a culprit
`inputStateFor(currentFault->type)`; an unused input gets `Unused` or `Event` from its own slot;
everything else is `Ok` while `Ready`, otherwise `Pending`. `getIsSynchronized` is
`currentState == ReaderState::Ready`. `StatusFingerprint` carries the state plus the fault.

`Inactive` is neither a state nor a fault: an inactive reader *is* establishing, with no input to blame,
and `isActive` is a facade field the projection reads.

## 2. The settle protocol

Facade state: `currentState`, `lastTrigger`, `currentFault` — not the whole `ReaderStateTransition`, whose
`target` would duplicate `currentState`.

```cpp
/// Callers with a cause: force a transition, then settle.
void settleStateLocked(const ReaderStateTransition& transition);
/// Level-triggered re-evaluation in place (packet escalation, deadline, coalesced task).
void settleStateLocked();
```

### 2.1 The loop

Every evaluation returns a `ReaderStateTransition`. `target == currentState` with trigger `Settled` means
"no further transitions needed", and the fault the settling stage found rides on that transition — which
is where the reader's diagnosis comes from.

**The forced transition must be applied unconditionally.** A `while (next.target != currentState)` head
skips the whole body when a caller forces the state it is already in, which is the ordinary case:
`settleStateLocked({Establishing, ConnectionChanged})` while already `Establishing` — every connect during
cold start, and construction's own `{Establishing, Created}` — evaluates nothing, records no trigger, and
leaves the previous fault standing. Apply first, then loop:

```cpp
void MultiReaderImpl::settleStateLocked(const ReaderStateTransition& transition)
{
    dataPlane.availableValid = false;        // any decision can drain, drop segments or move state
    const auto faultBefore = currentFault;

    applyTransitionLocked(transition);       // unconditional: the caller's cause is a real event

    auto next = nextStateLocked();
    for (uint8_t i = 0; next.target != currentState; ++i)
    {
        if (i == MAX_ITERATIONS)
        {
            next = {ReaderState::Error, TransitionTrigger::TransitionLimitReached, internalFault()};
            break;
        }
        applyTransitionLocked(next);
        next = nextStateLocked();
    }
    applyTransitionLocked(next);             // the Settled transition carries the diagnosis

    latchStateChangeNotifyLocked(faultBefore);
    publishProducerGateLocked();
}
```

Applying the settled transition instead of assigning `currentFault` afterwards keeps
`applyTransitionLocked` the single writer of both `lastTrigger` and `currentFault`, so `Settled` is
recorded and the fault needs no separate fixup. The last call is therefore a self-transition, which is why
the hook guard in §2.2 is mandatory.

The gate publication and the availability invalidation are why every path funnels through here, so the
limit-reached path must fall *through* to them.

**The settled fault wins outright.** Guarding the assignment on the standing fault's kind — "a
reconfiguration fault cannot have been cleared by now" — reasons backwards: reconfiguration is exactly
what the consumer does, and it is what triggered the settle. It also loses precedence: `Ready` →
`Faulted(DataLost)` → `Establishing`, whose connection stage finds an input with no signal, would report
`DataLost` while the actual blocker is `Unconnected`. Assign unconditionally; a fault whose cause persists
is re-derived next pass, since every stage re-reads ground truth.

`latchStateChangeNotifyLocked` sets `notificationCoordinator->setStateChangeNotify(true)` when the settled
fault carries neither data nor a new event — the `InputsFailed`-projecting group — and differs from
`faultBefore`. One edge per settle. `faultBefore` is captured before the loop because
`applyTransitionLocked` overwrites `currentFault` on the way through. `setStateLocked` goes away.

### 2.2 `applyTransitionLocked`

Records the trigger and fault always; fires `onExitLocked`/`onEnterLocked` **only when
`transition.target != currentState`**. Not optional — every settle ends by applying a staying transition,
so an unguarded `onExitLocked(Ready)` would invalidate the synchronization on every settle spent in
`Ready`.

The hooks stay near-empty — only what is genuinely about the edge:

- `onExitLocked(Ready)` → `invalidateSynchronizationLocked()`. Every departure from `Ready` ends the
  aligned stream, so stating it on the edge *is* the invariant, in one place.
- Model invalidation is driven by the trigger: `ConnectionChanged`, `UsedChanged`, `ActiveChanged` and
  `EventsConsumed` each call `invalidateModelLocked()`. That replaces the per-call-site invalidation the
  state classes hide today — a caller states its cause, the machine derives the consequence.

`currentFault = transition.fault` copies a string and a vector on a path reads reach; take the transition
by value and move, or keep the copy knowingly (§4 records how visible that size of cost is).

### 2.3 Call sites

`slotConnected`/`slotDisconnected` → `{Establishing, ConnectionChanged}`;
`addInput`/`removeInput`/`setInputUsed`/`setMainInput` → `{Establishing, UsedChanged}`;
`setActive` → `{Establishing, ActiveChanged}`; `readEventsLocked` → `{Establishing, EventsConsumed}`;
construction → `{Establishing, Created}`. These four triggers are also the only exits from `Error`.

One specialization worth keeping: a connect landing on an **unused** slot while `Ready` calls the no-arg
`settleStateLocked()` instead, since the model is built from the used inputs.

## 3. The four evaluations

`ReaderStateTransition MultiReaderImpl::evaluateXLocked()` — ported from `state_machine.cpp`, touching
members directly; `StateContext` and its deferred flags disappear, so `refreshMainInputDescriptorsLocked()`
and `clearModelDerivedCachesLocked()` are called where the ladder used to flag them. `nextStateLocked`
must dispatch to the *matching* evaluation (all four arms currently call `evaluateEstablishingLocked`) and
return from every path.

"Stay" below means `{currentState, Settled, Fault{...}}`; "settled" means
`{currentState, Settled, std::nullopt}`.

**`evaluateEstablishingLocked()`** — source: `state_machine.cpp:723-924, 931-955, 958-974, 1013-1043`.

| # | Stage | Result |
|---|---|---|
| 1 | `drainUnusedSlotsLocked()` | — |
| 2 | `!isActive`: disarm all monitoring, drain used slots, set/suppress event bits | stay with `EventPending` if any, else settled |
| 3 | used set empty | stay, `Unconnected` with no culprits; `invalidateSynchronizationLocked()` |
| 4 | explicit main input not among used | stay, `Unconnected` |
| 5 | clear-then-drain used slots; any unconnected | suppress **every** slot's event bit; stay, `Unconnected` |
| 6 | refresh `dataLossMonitor->setMonitored` over used ∧ connected | — |
| 7 | leading events, with the handshake-in-flight suppression; opportunistic model build | stay, `EventPending` |
| 8 | descriptors missing | stay, `MissingDescriptors` |
| 9 | `refreshMainInputDescriptorsLocked()` | — |
| 10 | per-input validity; `exposeBuriedEventsLocked` first | stay with `EventPending`, or → `Error`, `Faulted`, `Incompatible` |
| 11 | `visibleLostSlotsLocked()` non-empty | → `Error`, `Faulted`, `DataLost` |
| 12 | build the model if absent; `exposeBuriedEventsLocked` on failure | stay with `EventPending`, or → `Error`, `Faulted`, `Incompatible` |
| 13 | — | → `Synchronizing`, `InputsReady` |

Stages 5 and 7 carry the two all-slot event-bit suppressions (a used input without a signal; a connect
handshake in flight). Both are cross-input rules about the *gate*, not the fault, and both are
`Establishing`-only — elsewhere every input is connected and past its handshake.

`discardLeftoverSegments` and the `commonStart != nullptr` shortcut are not ported here: the
synchronization is already gone by the time `Establishing` runs (`onExitLocked(Ready)`). `Ready` keeps the
discard.

**`evaluateSynchronizingLocked()`** — the model is trusted: descriptors move only through an event
(step 2) and the used set only through a caller with a trigger, which forces `Establishing`.

1. `drainUnusedSlotsLocked()`; clear-then-drain used slots; any unconnected or the set empty →
   `Establishing`, `ConnectionChanged`. `!isActive` → `Establishing`, `ActiveChanged`.
2. leading events on any used input → stay, `EventPending`.
3. `visibleLostSlotsLocked()` non-empty → `Error`, `Faulted`, `DataLost`.
4. any used input with zero available samples → stay, `NotAligned`. Its own stage rather than folded into
   `synchronize()`, whose `collectFirstSamples` early-returns on the first empty input and would name one
   culprit where the reader should name all of them.
5. `syncManager->synchronize(...)`:
   - `Synchronized` → `readCoordinator->configure(...)`, `nextReadTick = readOffsetLocked()`;
     → `Ready`, `SynchronizationSucceeded`
   - `NeedMoreData` → stay, `NotAligned` with the result's culprits and message
   - `EventPending` → raise the affected slots' event bits; stay, `EventPending`
   - `Failed` → `Error`, `Faulted`, `SynchronizationFailed`

**`evaluateReadyLocked()`** — a port of `ReadyState::reassess` (`state_machine.cpp:587-632`).

1. `drainUnusedSlotsLocked()`; used set empty → `Establishing`, `UsedChanged`. `!isActive` →
   `Establishing`, `ActiveChanged`.
2. clear-then-drain used slots; any unconnected → `Establishing`, `ConnectionChanged`.
3. `getCommonStart() == nullptr || !hasModel()` → `Error` with an internal fault: `Ready` without a model
   is an invariant break, and a consumer trigger re-establishes it.
4. `discardLeftoverSegments(...)` — what turns an event buried behind a sub-minimum residual into a
   leading one, so it must precede step 5.
5. leading events on any used input → **stay `Ready`**, `EventPending`. The synchronization survives the
   *reporting* of an event; the *consumption* ends it, through `readEventsLocked`'s `EventsConsumed`
   transition. Every `Ready` operation consults the fault before serving data (§4), and availability
   already stops at an event boundary, so nothing can be served past it.
6. `visibleLostSlotsLocked()` non-empty → `Error`, `Faulted`, `DataLost`. `Ready` applies the "< one
   aligned block" rule itself (`visibleLostSlotsLocked`, today's `deriveDataLoss` body, shared with
   `Establishing` stage 11 and `Synchronizing` step 3), so it only leaves once the loss is real.
7. — → settled.

**`evaluateErrorLocked()`** → always settled, keeping the fault. It deliberately re-derives nothing: the
fault names what the consumer must fix, and only a consumer-driven trigger (§2.3) re-enters the ladder.

## 4. The per-state operations

The `MultiReaderState` flyweight hierarchy is deleted. Only `ReadyState` overrode anything, so each
operation becomes one facade method with a single `currentState == ReaderState::Ready` branch. The
`StateContext` parameter threading goes with it — which also removes the per-call context construction the
previous refactor measured at +11 ns on `getAvailableCount` and +17 ns on a zero-count read.

| Facade method | `Ready` | otherwise |
|---|---|---|
| `publishProducerGateLocked()` | `wakeOnAnyPacket = false`, minimum = `effectiveMinimum(model, minReadCount)`, data-first event bit | `wakeOnAnyPacket = true`, minimum = 1, event bits untouched |
| `refreshDataPlaneLocked(bool escalateOnEvent)` | today's `ReadyState::refreshDataPlane`; escalates via `settleStateLocked()` | `settleStateLocked()` |
| `updateCallbackStateLocked()` | today's `ReadyState::updateCallbackState` | `settleStateLocked()` |
| `availableCountLocked()` | leading-event guard, then cache or direct walk | `0` |
| `planReadLocked()` | `ServeData` | `ReportState` |
| `readWaitSatisfiedLocked(SizeT requested)` | the aligned-request test | `false` |

`planReadLocked` and `readWaitSatisfiedLocked` test the fault first —
`currentFault && currentFault->type == FaultType::EventPending` → `ReturnEvents` / satisfied — which is
what lets `Ready` hold events without a state of its own. `availableCountLocked`'s leading-event guard is
the same rule on the count. `Error` needs no special case in this table: it is not `Ready`, so it reports
state and serves nothing, and its status names the fault.

`publishSlotGatePolicy` and `publishGateWhileEstablishing` merge into the one loop inside
`publishProducerGateLocked`, where each state only chooses the minimum. `Ready` with no model cannot occur
(§3 step 3). `DataPlane` becomes a private nested struct of `MultiReaderImpl`; `adoptUnusedSlotEvents`,
`drainUnusedSlots`, `exposeBuriedEvents`, `readOffset` and `invalidateSynchronization` become `...Locked`
facade methods; `outcomeWithAffected` becomes a `Fault`-building helper (`multi_reader_impl.cpp` already
includes `fmt/ranges.h`).

Untouched: `Input`, `QueueReader`, `SynchronizationManager`, `ReadCoordinator`, `NotificationCoordinator`,
`CallbackGate`, `DataLossMonitor`, and the slot-vector helpers in `input.h`.

## 5. Files

**Delete**, and drop from `src/CMakeLists.txt` (lines 147-148, 162-163, 199-200, 228-229) and
`tests/CMakeLists.txt` (lines 32-33):

- `include/opendaq/multi_reader/state_machine.h`, `src/multi_reader/state_machine.cpp`
- `include/opendaq/multi_reader/state_context.h`, `src/multi_reader/state_context.cpp`

No test includes either header. **Add** `src/multi_reader/reader_state.cpp` to both lists.

**Modify**: `include/opendaq/multi_reader/reader_state.h`, `include/opendaq/multi_reader_impl.h`,
`src/multi_reader_impl.cpp`.

## 6. Tests

Run the suite, read what fails, decide per failure — the traces are a baseline to move, not a
specification to preserve. Two areas will need real work rather than re-baselining:

- **`test_multi_reader.cpp`** pins the old recoverability model in several places (statuses read while
  faulted, `getValid`/`getIsValid` while faulted, per-input failure values). Those assertions describe
  where faults used to live, so they move with the taxonomy in §1.2.
- **`test_multi_reader_state_transitions.cpp`** needs the trace format rewritten around the new types and
  the `markAsInvalid`-driven scenario replaced — a reader that cannot be killed needs an `Error`-entry and
  consumer-recovery trace instead:

```cpp
struct StateSnapshotForTest
{
    ReaderState state;
    TransitionTrigger trigger;
    std::optional<Fault> fault;
};
```

with a line of `label -> State/FaultType[culprits] "detail" observation`:

```
"built -> Establishing/Unconnected[0,1,2] \"Inputs [0, 1, 2] have no signal connected\" avail=0",
"connect 2 -> Establishing/EventPending[0,1,2] \"Events pending on inputs [0, 1, 2]\" avail=0",
"read initial events -> Synchronizing/NotAligned[0,1] \"Waiting for data on inputs [0, 1]\" status=Event events=3",
"data on all -> Ready[] \"\" avail=10",
```

Fault type + culprits + detail is more information than the substate name it replaces, so no trace loses
resolution.

**The cross-check hook.** `setExhaustiveDerivationForTest` becomes `setRestartFromEstablishingForTest`: in
that mode the no-arg `settleStateLocked()` forwards to `settleStateLocked({Establishing, ...})` unless
`currentState` is `Ready` or `Error`. Both carve-outs are structural — leaving `Ready` invalidates the
synchronization, and leaving `Error` is the consumer's prerogative — so what the mode checks is
`Synchronizing`'s narrow evaluation against a full restart. Weaker than the dual derivation it replaces;
say so in the suite header.

## 7. Docs

- `multi_reader.md`: §2 (the deprecated `markAsInvalid` / always-valid `getIsValid`), §4 (the state table
  → four states, the fault taxonomy, the projections), §6 (fold the state-machine row into the facade
  row), §8 (the data-plane paths branch on `currentState == Ready`), §9.4 (the gate policy is one loop
  choosing one minimum — now literally true), §10 (recovery from `Error` is consumer-triggered).
- `multi_reader_state_refactor.md`: fold into this document once the change lands.

## 8. Verification

1. `cmake --build build/x64/msvc-22/full --target test_reader --config Debug -- -m`
2. `build/x64/msvc-22/full/bin/Debug/test_reader_debug.exe`
3. `MultiReaderStateTest` in both parameterizations, traces matching in each.
4. The collaborator suites (`QueueReaderTest`, `ReadCoordinatorTest`, `SynchronizationManagerTest`,
   `NotificationCoordinatorTest`, `DataLossMonitorTest`, `MultiReaderInputTest`) — nothing here should
   touch them, so a failure there is a signal something leaked.
5. The single-input reader suites still exercise `markAsInvalid` through `ReaderImpl`; they must stay green
   since the interface method is untouched.
6. `bench_multi_reader` `micro` scenario on `getAvailableCount` and a zero-count read — the only place a
   regression of the `StateContext` size is visible.

## 9. Suggested commit sequence

1. Finish `reader_state.h` (`DataLost` placement, `requiresReconfiguration`, the `Invalidated` rename)
   plus `reader_state.cpp` with the name and projection functions. Nothing uses them yet.
2. Remove the `invalid` mechanism (§1.4): the no-op `markAsInvalid`, the always-true `getIsValid`, the
   `disposed` flag, and the three early-outs replaced by state dispatch.
3. The projections rewritten against `currentState` + `currentFault`, fed by a temporary mapping from the
   old substate.
4. The operations (§4) folded into the facade; delete the `MultiReaderState` hierarchy.
5. The settle loop (§2) and the four evaluations; delete `deriveState`, `state_machine.*`,
   `state_context.*`, the temporary mapping and the old `ReaderState`.
6. Trace rewrite and the test hook rename.
7. Docs.
