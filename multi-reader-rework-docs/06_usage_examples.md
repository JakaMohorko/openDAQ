# Multi Reader Rework — Usage Example: Sum Function Block

**Companion to:** `01_specification.md` (reader behavior), `02_implementation_plan.md` (Phase 7), `05_error_contract.md` (status/error semantics the FB consumes).
**Code location:** `examples/modules/ref_fb_module/modules/ref_fb_module/` — `include/ref_fb_module/sum_reader_fb_impl.h`, `src/sum_reader_fb_impl.cpp`, `tests/test_fb_sum.cpp`.

## 1. Purpose

The existing `RefFBModuleSumReader` function block (`SumReaderFbImpl`) becomes the reference consumer of the reworked multi reader: it exercises every major new capability (modes/rates, resampling with main-input selection, state-machine-driven recovery, per-input diagnostics) and doubles as living usage documentation. It also validates the rework end-to-end — a function block that can keep summing through descriptor changes, data loss, sync failures, and reconnects, using only the public API, is the acceptance test for "recovery must be very clear."

### Current state (what exists)

- Equal-rates-only reader (`setAllowDifferentSamplingRates(false)`), `Float64` value / `Int64` domain read types.
- Spare-port pattern: one always-disconnected `SumPort_N` marked `setInputUsed(false)`; on connect it becomes used and a new spare is added (`updateInputPorts`).
- `ComponentStatus` reporting via `initComponentStatus`/`setComponentStatusWithMessage` (Warning: "No signals connected!", config failures; Ok when configured).
- Event handling via `status.getEventPackets()` dict + `getMainDescriptor`; descriptor cache per port (`cachedDescriptors`); output descriptors rebuilt in `configure`.
- **Recovery via `MultiReaderFromExisting`** (`recoverReaderIfNecessary`) — this factory is removed by the rework (spec §8.4); the FB must move to in-place recovery. This is the forcing function that makes the example honest.

## 2. Modes

New `SelectionProperty("Mode", ["EqualRates", "MultiRate", "Resampled"], 0)` on the FB. `allowDifferentSamplingRates` is builder-time configuration, so a mode change rebuilds the reader (the FB already owns this pattern: `reader.dispose()` + rebuild in `createReader`, re-adding all connected ports plus the spare). After any mode change: full revalidation/resync; the parked-port set (§3) is cleared and re-derived.

### 2.1 `EqualRates` (current behavior, kept as default)

Builder: `setAllowDifferentSamplingRates(false)`. A signal with a different rate makes the reader `Incompatible` with that input in the affected set → the FB parks that port (§3) instead of failing the whole block. Sum semantics unchanged: `out[i] = Σ inputs[i]`, output descriptor cloned from the common domain, one output sample per input sample.

### 2.2 `MultiRate` (dividers, no resampling)

Builder: `setAllowDifferentSamplingRates(true)`, no resampler configuration — the reader's direct path with sample-rate dividers (spec §4.2).

- **Buffer sizing:** per-input buffer for a common-rate request `count` is `count / divider_i`, where `divider_i = commonSampleRate / rate_i`. Dividers are derived from public data: `getCommonSampleRate()` and each signal's domain descriptor (`resolution`, linear-rule delta). This derivation is part of the example's documentation value — it is the documented pattern for direct-path consumers.
- **Sum semantics:** no interpolation is available in this mode, so the sum is defined only at ticks where **every** input has a sample — every `blockLcm`-th common tick. The FB reads in aligned blocks and emits **one output sample per block**: the sum of each input's block-start sample. Output rate = `commonSampleRate / blockLcm` = GCD of the input rates; output domain descriptor = common domain with the linear delta scaled by `blockLcm`.
  - Example: 10 Hz + 15 Hz → common rate 30 Hz, dividers {3, 2}, `blockLcm` 6 → output 5 Hz, summing the samples that coincide at t = 0, 200 ms, 400 ms, …
- This mode intentionally showcases block alignment: `getAvailableCount` multiples of `blockLcm`, per-input counts differing per read, and the silent partial-block discard at events.

### 2.3 `Resampled` (main-signal selection)

Builder: `setAllowDifferentSamplingRates(true)` + default `LinearResamplerBuilder` (nothing to inject); at runtime the FB calls `setMainInput(<port global id>)` per the selection property below.

- **`MainSignal` selection property**, visible only in `Resampled` mode (`EvalValue`-bound visibility on `Mode`). Its selection values are **dynamically populated** with the currently connected inputs (display: signal name; backing value: port global id). Preferred mechanism: a hidden `ListProperty("AvailableInputs")` holding the names, with the selection property's values bound via `EvalValue("%AvailableInputs")` so updating the list in `onConnected`/`onDisconnected` repopulates the dropdown; fallback if the binding proves unsupported for this shape: remove-and-re-add the `MainSignal` property with rebuilt values (both patterns exist in coreobjects — confirm at implementation, `SelectionPropertyBuilder`/`setSelectionValues`).
- Repopulation rules: on connect, append; on disconnect, remove; if the *selected* main disconnects, fall back to the first remaining input, emit a Warning naming the change, and update the property value. An explicit user selection always wins while its signal is connected (matches spec §4.4: a selected main input is never silently replaced *by the reader* — the fallback here is FB policy, done through the public API).
- **Sum semantics:** all inputs are delivered on the main input's grid with equal `count` per input → `out[i] = Σ inputs[i]` at the main rate. Output domain = the common output grid (`getMainDescriptor`'s domain descriptor, spec §8.2); `readWithDomain` is unnecessary — the FB builds output domain packets from the status offset as today.
- Changing `MainSignal` at runtime invalidates sync (spec §5); the FB just keeps reading — resync and pipeline rebuild are the reader's job.

## 3. Error handling

Design rule mirroring the spec: **the FB never gives up on a recoverable state, and never hides an unrecoverable one.** All decisions are driven by the new status surface: `getState()`, `getAffectedInputCount()/getAffectedInputIndex()`, `getStateMessage()` (spec §8.2).

### 3.1 Port parking (recoverable, per-input)

When a read/status reports one of the recoverable per-input failure states, the FB **parks** the affected ports — `reader.setInputUsed(id, false)` — so the remaining inputs keep summing:

| Reader state | Affected inputs parked | Park reason recorded |
|---|---|---|
| `Incompatible` | inputs in the affected set (bad/unconvertible descriptors, rate policy violation in EqualRates mode) | "incompatible: <details from getStateMessage>" |
| `SynchronizationFailed` | inputs that failed against the start (span exceeded, no common tick) | "cannot synchronize" |
| `DataLost` | inputs past the packet deadline | "no data" |

The FB keeps a `parkedPorts` map (port global id → reason + timestamp). Slot indices from the status map to ports via the reader's construction order (`IReaderConfig::getInputPorts`).

**Warning display:** whenever `parkedPorts` is non-empty, `setComponentStatusWithMessage(ComponentStatus::Warning, ...)` with a message listing every parked port and its reason (e.g. `"Inputs excluded from sum: SumPort_2 (cannot synchronize), SumPort_4 (no data)"`). Status returns to `Ok` when the set empties. This satisfies "unused ports should be displayed as a warning" using the existing FB status mechanism — no new API.

### 3.2 Recovery probing (un-parking)

A parked port is excluded from reader events (spec §3.2), so the FB re-tests parked ports actively:

1. **Event-driven probe:** the FB is the reader's external listener (`setExternalListener(this)`) — `packetReceived`/`connected` notifications for a parked port (descriptor change, data resumed) trigger an immediate probe.
2. **Periodic fallback probe:** every `RecoveryRetryInterval` seconds (new Int property, default 5, 0 disables), probe the oldest parked port.

A probe = `setInputUsed(id, true)`, then evaluate the next read's status: if the reader reaches `Synchronized` (or the port is no longer in any affected set), the port stays used and leaves `parkedPorts`; if the same failure recurs, it is re-parked and the probe timestamp updated. Probes run one port at a time to keep a bad port from repeatedly interrupting the healthy ones (each probe costs one resync — spec §5 invalidation on used-set change).

### 3.3 Unrecoverable errors

`getState() == Error` (or `IReaderStatus::getValid() == false`): report `ComponentStatus::Error` with `getStateMessage()` content, stop issuing reads, leave signals' last descriptors intact. No silent reader re-creation — `MultiReaderFromExisting` is gone, and `Error` means an internal invariant broke (spec §6.1); surfacing it to the user *is* the handling. `recoverReaderIfNecessary()` is deleted. (A mode change or reconnect still rebuilds the reader through the normal `createReader` path — that is explicit user action, not silent recovery.)

### 3.4 New configuration surfaced as FB properties

To make the error scenarios reachable and demonstrate the new config (spec §8.2): `DataLossTimeout` (Float seconds, default 5, 0 disables → `setDataLossTimeout`), `MaxSynchronizationDistance` (Float seconds, default 5, 0 disables → `setMaxSynchronizationDistance`), `RecoveryRetryInterval` (§3.2). Forwarded to the reader on change; changes invalidate sync per spec.

## 4. Implementation steps (Phase 7 in `02_implementation_plan.md`)

| Step | Change | Depends on |
|---|---|---|
| 7.1 | `Mode` property; per-mode builder wiring + reader rebuild on mode change; MultiRate GCD-output sum path with per-input buffer sizing | Phase 2 |
| 7.2 | `MainSignal` dynamic selection property + `setMainInput` wiring; selected-main disconnect fallback; Resampled sum path | Phase 5 |
| 7.3 | Port parking + recovery probing (`parkedPorts`, probes, external-listener hooks); **delete `recoverReaderIfNecessary`/`MultiReaderFromExisting` usage** | Phases 3–4 |
| 7.4 | Status reporting (Warning w/ parked list, Error for unrecoverable) + `DataLossTimeout`/`MaxSynchronizationDistance`/`RecoveryRetryInterval` properties | Phases 3–4 |
| 7.5 | Test suite of §5 | 7.1–7.4 |

Note the ordering constraint recorded in Phase 6.1: removing `MultiReaderFromExisting` breaks this FB's compile, so step 7.3's deletion must land no later than Phase 6.1 (pull it forward if Phase 6 runs first).

## 5. Test suite (`tests/test_fb_sum.cpp`)

### Fixture extensions (builds on the existing `SumTest`)

- Multi-rate signal factory: signals with configurable tick resolution/delta (10 Hz, 15 Hz, 1 kHz variants of the existing `timeDescriptor`), per-signal domain signals (the current fixture shares one `timeSignal` — different-rate tests need per-signal domains), ramp value fill (`value == tick`) for exact-sum assertions in every mode.
- `waitForComponentStatus(fb, status, timeoutMs)` — polls the status container; no sleeps.
- `readSumOutput(n)` helper — stream reader on the `Sum` signal collecting n samples with timeout.
- Short-timeout configuration: tests set `DataLossTimeout`/`RecoveryRetryInterval` to sub-second values to keep error-state tests fast.

### Test matrix

All tests run with `ReaderNotificationMode` = Scheduler unless noted. "Parked" = port reported unused + Warning lists it.

**Modes:**

| Test name | Description |
|---|---|
| EqualRatesSumHappyPath | Existing behavior kept: N equal-rate signals, exact sum, Ok status |
| EqualRatesDifferentRateParked | Connect a 15 Hz signal among 10 Hz ones → that port parked, others keep summing; Warning lists it |
| EqualRatesParkedRecoversOnDescriptorFix | Parked port's signal changes to a matching rate → probe re-enables it; Ok status; sum includes it again |
| MultiRateGcdOutput | 10 + 15 Hz ramps → output at 5 Hz; each output sample = sum of coinciding-tick values (exact) |
| MultiRateBufferSizing | Per-input counts = count/divider verified via read counts; no overflow/underflow with 3 rates {10, 15, 30} |
| MultiRatePartialBlockAtEvent | Descriptor change mid-block → complete blocks summed, remainder silently dropped, resync, sums continue (no duplicate/missing output tick) |
| ResampledSumExact | 10 + 15 Hz ramps, main = 15 Hz → output 15 Hz; linear-interpolated sum exact for ramps |
| ResampledMainSwitchRuntime | Switch `MainSignal` mid-run → resync; output rate/grid follows new main; no stale samples |
| MainSelectionRepopulated | Connect/disconnect signals → `MainSignal` selection values track the connected set |
| MainDisconnectFallback | Disconnect the selected main → falls back to first remaining input + Warning; property value updated |
| ModeSwitchRebuildsReader | Cycle EqualRates → MultiRate → Resampled with data flowing → each switch resyncs and output resumes with correct semantics |

**Error states (each drives the reader into the state, verifies parking/warning, then verifies recovery):**

| Test name | Description |
|---|---|
| IncompatibleDescriptorParked | `ComplexFloat32` value descriptor (existing `invalidDescriptor`) → port parked, Warning; valid descriptor → recovered, Ok |
| DataLossParkedAndRecovers | One signal stops sending; `DataLossTimeout=0.2s` → parked "no data"; data resumes → probe re-enables, Ok |
| SyncFailureParked | One signal's first samples > `MaxSynchronizationDistance` behind → parked "cannot synchronize"; aligned data arrives → recovered |
| NoCommonTickParked | delta-2 odd/even tick signals in MultiRate mode → sync failure parked (spec §5.7 `NoCommonTick`) |
| GapEventResyncContinues | Gap packet on one input → event consumed, resync, summing continues; no park (gap alone is recoverable in-band) |
| AllPortsParkedNoOutput | Every input failing → all parked, Warning "no usable inputs", zero output packets, no crash; one recovers → output resumes |
| MissingSignalSparePortInert | The spare disconnected port never blocks reads or appears in warnings (existing pattern still holds) |
| UnrecoverableErrorReported | Force `Error` (`IReaderConfig::markAsInvalid`) → `ComponentStatus::Error` with the reader's message; reads stop; no auto-recreation |
| WarningMessageContents | Two ports parked for different reasons → Warning message names both ports with both reasons |
| ProbeDoesNotFlap | Persistently bad port with `RecoveryRetryInterval=0.2s` → healthy inputs' output stream stays gap-free apart from bounded resync points; probe cadence respected |

Coverage cross-check: every FB-visible reader state from spec §6.1 appears (`Incompatible`, `SynchronizationFailed`, `DataLost`, `EventPending` via descriptor/gap tests, `Error`, `Synchronized`, `WaitingForConnections` via the no-signals Warning), and all three modes exercise their distinct read semantics with exact-value assertions.

## 6. Out of scope

Renderer/statistics/power FB migrations (they use their own readers), scaling FB, and any new public FB module — this example intentionally stays inside the existing `ref_fb_module` so the diff reads as "what changes for a multi reader consumer," nothing else.
