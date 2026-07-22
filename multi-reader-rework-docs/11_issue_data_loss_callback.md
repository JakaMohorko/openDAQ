# Issue: `DataLost` does not wake the consumer — the reader computes the loss but never notifies

Status: **Open** — proposed for the multi-reader rework.
Source: design review of the Sum Reader FB rework (`refactor/AI-refactor`), 2026-07-22.

## Guiding principle

The whole point of the multi reader is that **the consumer implements as little as possible**. The
reader owns synchronization, event ordering, data-loss detection, and recovery signalling; the
consumer should only have to (a) install one `onDataAvailable` callback and (b) `read()` when told
to, inspecting the returned status. Every timer, deadline, or liveness heuristic the consumer is
forced to run itself is a leak of reader responsibility into user code, and every such leak is a
pattern each future consumer has to re-implement (and get wrong).

Measured against that principle, **data loss is currently a leak**: the reader detects it but does
not tell the consumer, so the consumer has to run its own clock to notice.

## Problem

A `DataLost` transition does **not** raise the `onDataAvailable` callback gate. The reader detects
the loss, updates its internal `ReaderState`, and stops there — the consumer is never woken and only
discovers the loss if it happens to `read()` for some other reason.

### Evidence

The callback gate never consults `ReaderState` (`core/opendaq/reader/docs/multi_reader.md:264`,
`core/opendaq/reader/src/multi_reader/notification_coordinator.cpp:158`):

```
shouldInvokeCallback = anyEvent() || allUsedReady()
                     = event.any() || (used.any() && (ready & used) == used)
```

Trace of a data-loss deadline crossing:

1. `DataLossMonitor`'s waiter thread wakes at the deadline and fires its callback, which calls
   `notificationCoordinator->requestEvaluation()` (`multi_reader_impl.cpp:140-146`).
2. The coalesced evaluation runs the full ladder and reaches the data-loss branch
   (`multi_reader_impl.cpp:913-943`), which calls `invalidateSynchronizationLocked()` and
   `setStateWithAffectedLocked(ReaderState::DataLost, …)`.
3. `setStateLocked` (`multi_reader_impl.cpp:392-397`) writes only `state`, `stateMessage`, and
   `stateAffectedInputs`. It sets **no** event bit and **no** ready bit.
4. The dead input is still `used` but not `ready`, and no event bit is set anywhere, so
   `shouldInvokeCallback()` evaluates to `false`. **No callback fires.**

The reader has correctly computed `DataLost` (and a `read()` would return it), but nobody is told to
go read it.

### Impact on consumers

Because the reader stays silent, a consumer that must react to a stalled input has to run its own
liveness clock. The Sum Reader FB does exactly this, and it is pure duplication of logic the reader
already performs:

- `SumReaderFbImpl::onPacketReceived` overrides the port notification purely to run a
  data-loss/staleness timer off the healthy inputs' packet stream
  (`sum_reader_fb_impl.cpp:333-340`), scheduling a deferred `read()` so the loss surfaces.
- `SumReaderFbImpl::maybeProbeLocked` additionally has to abandon a "stuck probe" — an input marked
  used again whose producer is permanently silent — after one interval, because nothing ever tells
  the FB that input is not delivering (`sum_reader_fb_impl.cpp:889-916`).

Every future consumer of the multi reader would have to reinvent both mechanisms. That is precisely
the pattern the rework is meant to eliminate.

## Proposed fix

### Part 1 — a `DataLost` transition triggers the callback

The timer is up; the consumer should be told. When the reader transitions a monitored input into
`DataLost`, it must raise the callback gate so `onDataAvailable` fires and the consumer's subsequent
`read()` returns the `DataLost` status (naming the affected inputs, exactly as a normal read would).

This is nearly wired already — the `DataLossMonitor` waiter thread already drives a coalesced
evaluation on the deadline (`multi_reader_impl.cpp:140-146`). The only missing link is that the
evaluation, upon entering `DataLost`, does not raise the gate. The fix is to make the gate reflect a
state change that the consumer must observe, so the same waiter-thread-driven evaluation ends in a
fired callback rather than a silent state write.

Suggested mechanism (implementer's choice of the two):

- **Preferred — an explicit "attention" latch in `NotificationCoordinator`.** Add a latched signal
  that `shouldInvokeCallback()` ORs in, set whenever the reader enters a state the consumer must be
  told about that carries no event packet and no readiness (`DataLost`, and by the same argument
  `SynchronizationFailed` / `Incompatible` / `Fail`), and cleared when the consumer reads that
  status (or when the reader leaves the state). This keeps the existing `event` bit meaning strictly
  "a returnable event packet exists" and cleanly separates "a state change you must see."
- **Minimal alternative — set the event bit on the affected slots** at the `DataLost` transition.
  Smaller diff, but overloads the `event` bit (whose current contract is "returnable event packet"),
  so the read path must tolerate an event-gated wake that yields a state rather than a packet.

Either way the consumer contract becomes: *you are always woken when there is something to read —
data, an event, or a state change that demands action.* No consumer-side timer required.

### Part 2 — arm the monitor at the start of monitoring, not on the first packet

Today the monitor **arms on the first packet after a slot becomes monitored**: `onPacket` sets
`armed = true` (`data_loss_monitor.cpp:116-135`), while `setMonitored(slot, true)` deliberately does
not arm (`data_loss_monitor.cpp:137-153`), and `hasLostSlots()` requires `armed`
(`data_loss_monitor.cpp:101-114`). Consequently the monitor can only see *"was producing, then went
silent"* — it is blind to *"became used/connected/active but never produced a first packet."*

That blind spot is the entire reason the FB needs its separate stuck-probe abandonment: a probed
input whose producer is permanently silent never arms, so no deadline ever trips.

**Fix: arm at the start of monitoring.** In `setMonitored(slot, true)`, set `armed = true` and
`lastArrival = clock()`, so the slot has a deadline exactly one timeout into the future from the
moment it starts being monitored. A first packet that never comes is just as harmful as a producer
that stops — both should trip the same deadline. Packets still refresh `lastArrival` and clear the
loss on recovery, exactly as now.

With Part 2 in place, the "input never produced" / "stuck probe" case is covered by the very same
`DataLost` path as "input died," and Part 1 makes both wake the consumer. The two parts together let
a consumer delete all of its own liveness timing.

## Consequences / considerations for the implementer

- **The data-loss timeout also becomes a first-data/establishment deadline.** Arming at start means
  an input that connects but is slow to deliver its first packet (or its first descriptor) beyond the
  timeout will be reported `DataLost`. This is the intended broadening (no data at the start is as
  harmful as data stopping), but it couples "slow initial connection" and "steady-state loss" under
  one timeout. If a different establishment budget is ever wanted, it can be a separate value that
  defaults to the data-loss timeout; not required for this fix.
- **Monitoring is still gated on `used && connected && active`** (`multi_reader_impl.cpp:807-808`),
  so parked/unused inputs and an inactive reader still do not arm — arming at start only affects
  slots that are actually being monitored. A slot turned off clears its arming as it does today.
- **Fire-once semantics stay.** The monitor's `reported` flag already ensures one callback per
  crossing (`data_loss_monitor.cpp:198-235`); the attention latch (Part 1) must likewise not
  re-fire until cleared, to avoid a busy-wake loop while a loss is outstanding.
- **Consumer simplification is the acceptance signal.** After this change, `SumReaderFbImpl` should
  be able to drop the staleness branch of `onPacketReceived` and the stuck-probe abandonment in
  `maybeProbeLocked`, relying solely on `onDataAvailable` + the returned status. If it still needs a
  timer, the fix is incomplete.

## Affected files

- `core/opendaq/reader/src/multi_reader/notification_coordinator.{h,cpp}` — attention latch + gate.
- `core/opendaq/reader/src/multi_reader_impl.cpp` — raise the gate on the `DataLost` transition
  (and, if adopted, the other attention states); clear on read/state-exit.
- `core/opendaq/reader/src/multi_reader/data_loss_monitor.cpp` — arm in `setMonitored(_, true)`.
- `core/opendaq/reader/docs/multi_reader.md` — document that a state change raises the gate, and the
  arm-at-start semantics (§9.5 Data loss, §the gate formula).
- Consumer cleanup (validation): `examples/modules/ref_fb_module/.../src/sum_reader_fb_impl.cpp`.

## Acceptance criteria

1. A monitored input that stops producing for longer than the timeout fires `onDataAvailable`; the
   next `read()` returns a `DataLost` status naming that input, with no `read()`/timer on the
   consumer side in between.
2. A monitored input that never produces a first packet within the timeout is reported identically.
3. Recovery is unchanged: a packet on the affected input clears the loss and resynchronizes.
4. No repeated wakes while a single loss is outstanding.
5. `SumReaderFbImpl` compiles and behaves correctly with its consumer-side liveness timing removed.
