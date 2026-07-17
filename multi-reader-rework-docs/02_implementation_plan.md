# Multi Reader Rework — Implementation Plan

**Companion to:** `01_specification.md` (spec), `03_test_plan.md` (tests), `04_test_scaffolding.md` (scaffolding), `05_error_contract.md` (public API error codes), `06_usage_examples.md` (sum FB usage example, Phase 7)
**Branch:** `refactor/new-signal-reader` (continues on this branch)
**Repo paths:** all relative to `core/opendaq/reader/` unless noted.

Every phase ends with: full build green, `test_reader` suite green (with the documented triage of `03_test_plan.md`), and the phase's new tests passing. Phases are sequential; 3 and 4 may run in parallel after 2; Phase 7 (usage example) runs last, except for step 7.3's `MultiReaderFromExisting` removal, which must land no later than Phase 6.1.

---

## Starting point (already on the branch — do not redo)

| Done | Where |
|---|---|
| `DomainInfo` / `DomainValue` with nearest-tick `to/fromCommonDomain`, `roundUpOnDomainInterval` | `include/opendaq/domain_value.h` |
| `TypedReadingUtils` (`createReadLayout`, `isSampleTypeConvertible`, `readDomainValue`, `findDomainValue`, `readData`) | `include/opendaq/typed_reading_utils.h`, `src/typed_reading_utils.cpp` |
| `QueueReader` + `SignalEvent`: adopt/drain, leading-event consumption + merging, descriptor parsing into `TypedReadingContext` + `QueueReaderIssue` flags, `advanceToDomainValue`, common/native `read`/`skip`, `dropOutdatedPacketSegments`, `dropLeftoverSegment` (with synthetic gap — to be changed), divider handling | `include/opendaq/queue_reader.h`, `src/queue_reader.cpp` |
| Unit tests: 15 QueueReader, 12 DomainValue, 5 TypedReading | `tests/test_queue_reader.cpp`, `tests/test_domain_value.cpp`, `tests/test_typed_reading.cpp` |

`MultiReaderImpl` still runs the legacy `SignalReader` architecture — that is the core of this plan.

---

## Phase 0 — Branch reconciliation with the final spec

Small, mechanical; aligns existing code with settled decisions before building on it.

| Step | Change | Files |
|---|---|---|
| 0.1 | Rename `dropLeftoverSegment` → `discardLeftoverSegment`; remove the `SignalEvent::syncGapEvent(...)` call inside it (silent discard, spec §3.1). Delete `SignalEvent::syncGapEvent`, the private `SignalEvent(Int gapDiff)` constructor, and the now-unused `gapDiff` write path. | `queue_reader.h/.cpp` |
| 0.2 | Fix `SignalEvent::merge`: return `false` whenever either side is `Gap` (gaps never merge — currently gap+gap merges and loses the second `gapDiff`; spec §3.1). | `queue_reader.cpp` |
| 0.3 | Update the branch tests that assert the synthetic sync-GAP event (`Tests for dropping leftover segments` commit) to assert silent discard: samples gone, no new event queued, original event still delivered. Add a gap+gap ordering test. | `tests/test_queue_reader.cpp` |
| 0.4 | Remove the debug `std::cout` statements from `domain_value.h` (`DomainValueImpl` constructors) — they fire in NDEBUG-off builds on every domain value creation. | `domain_value.h` |

**Done when:** reader tests green; no reference to `syncGapEvent` remains.

## Phase 1 — QueueReader contract completion (spec §3.1)

| Step | Change | Files |
|---|---|---|
| 1.1 | `advanceToDomainValue` returns `AdvanceOutcome { AdvanceResult result; std::unique_ptr<DomainValue> reachedValue; }`. Populate `reachedValue` on `Success` (first-sample domain value after the advance). Resolve the in-code TODO "first timestamp mechanism for sync tolerance checking" by also exposing `getFirstSampleAbsoluteTime()` (system-clock time of first unread sample) for `maxSynchronizationDistance` diagnostics. | `queue_reader.h/.cpp` |
| 1.2 | Add `getAvailableSamplesUntilEvent()` (common-rate equivalent up to next event packet or queue end; the existing `getAvailableSamplesNative` already stops at non-data packets — expose and document). | `queue_reader.h/.cpp` |
| 1.3 | Dimensions: `parseValueDescriptor`/`parseDomainDescriptor` compute `valuesPerSample` as the **product** of all dimension sizes — value samples of any rank (and struct type) are readable as fixed-size samples; `QueueReaderIssue::UnsupportedDimensions` only for domain descriptors with dimensions (spec §3.1). Verify `TypedReadingUtils::readData` advances output pointers by `count * valuesPerSample`. | `queue_reader.cpp`, `typed_reading_utils.cpp`, `enum_flags` users |
| 1.4 | Sample-rate parsing hardening: reject non-integer delta / non-integer rate via `UnsupportedDomainRule` (exists) and add checked-arithmetic guards (spec §4.1). | `queue_reader.cpp` |
| 1.5 | Document + enforce: pending events block `read`/`skip`/`advanceToDomainValue` (already returns `Error` in `readNative`; make `advanceToDomainValue` consistent — it must not advance past a pending reportable event). | `queue_reader.cpp` |
| 1.6 | New unit tests per `03_test_plan.md` §QueueReader additions (reached-value reporting, until-event counts, vector signals, rank-2 rejection, blocked ops under pending events). | `tests/test_queue_reader.cpp` |

**Done when:** QueueReader suite green including new tests; API matches spec §3.1.

## Phase 2 — MultiReaderImpl rework, direct path (spec §2–§7, no resampling yet)

The big phase. `MultiReaderImpl` keeps its public class/factories; internals are replaced.

| Step | Change | Files |
|---|---|---|
| 2.1 | New internal headers/sources: `input_slot.h/.cpp` (spec §3.2, implements `IInputPortNotifications`, bounded `packetReceived`, owns one `QueueReader`), `synchronization_manager.h/.cpp` (spec §3.3, `CommonModel`, §4 math incl. rational-GCD common resolution and LCM dividers/blockLcm, §5 iterative alignment), `read_coordinator.h/.cpp` (spec §3.4, direct path only for now), `notification_coordinator.h/.cpp` (spec §3.5, bitsets + coalesced task). Add to `src/CMakeLists.txt`. | new files under `include/opendaq/` + `src/` |
| 2.2 | Rewrite `MultiReaderImpl` on top of these: replace `std::list<SignalReader> signals` with `std::vector<std::unique_ptr<InputSlot>>`; replace `invalid`/`nextPacketIsEvent`/`commonDomainStart`-derived state with `MultiReaderState state` + `evaluateState()` in the fixed order (spec §6.2); keep every public method signature. | `multi_reader_impl.h/.cpp` |
| 2.3 | Threading: `packetReceived` → slot pending bit + one coalesced scheduler task; user `readCallback` and external listener invoked outside locks; `SameThread` default for both construction types (behavior change, spec §8.5); condition-variable wake for timeout reads kept; lock order state-mutex → queue mutexes (spec §9). | `multi_reader_impl.cpp`, `input_slot.cpp`, `notification_coordinator.cpp` |
| 2.4 | Read path: availability/plan/commit via `ReadCoordinator` (blockLcm rounding for `read`/`readWithDomain`/`skipSamples` alike); events returned per spec §7.2 (one per input per call, dict + internal order retained for Phase 3); silent leftover-segment discard wired to `discardLeftoverSegment`. | `multi_reader_impl.cpp`, `read_coordinator.cpp` |
| 2.5 | Reimplement `getOffset(void*)` and status offset in common-domain ticks (spec §7.4); `getOrigin`/`getTickResolution` from `CommonModel`; `getIsSynchronized` from state; `getCommonSampleRate` from model. | `multi_reader_impl.cpp` |
| 2.6 | Recovery semantics: descriptor/gap/disconnect → recoverable states, no sticky invalid; sync failure no longer calls `setActiveInternal(false)` (spec §8.5); `markAsInvalid` → `Error`. Unused/inactive behavior per spec §3.2. | `multi_reader_impl.cpp` |
| 2.7 | Delete legacy path: remove `SignalReader` (`signal_reader.h/.cpp`), `ReaderDomainInfo` (`reader_domain_info.h`), `Comparable`/`ComparableValue` (`multi_typed_reader.h`), multi-reader usage of stateful `TypedReader`/`ReadInfo`. Check other readers (stream/block/tail) for shared usage before deleting files — if shared, only sever the multi-reader dependency. | listed files, `CMakeLists.txt` |
| 2.8 | Apply the public error contract (`05_error_contract.md` §3.1–§3.4): argument-validation paths, retained success-class returns (`getOffset`/`getTickResolution`/`getOrigin` → `OPENDAQ_IGNORED` when unestablished, `removeInput` → `OPENDAQ_NOTFOUND`, `skipSamples` → `OPENDAQ_IGNORED` in `Error`), `addInput` duplicate detection (`ERR_DUPLICATEITEM`), and `DAQ_MAKE_ERROR_INFO` messages on every failure path. | `multi_reader_impl.cpp` |
| 2.9 | Triage `tests/test_multi_reader.cpp` per `03_test_plan.md` Part C (keep / adapt / delete) and bring the suite green. | `tests/test_multi_reader.cpp` |

**Done when:** all reader tests green under the triage; acceptance criteria spec §10 items 1, 2, 4–8 demonstrably hold (covered by matrix rows).

## Phase 3 — Status and state extension (spec §8.2)

| Step | Change | Files |
|---|---|---|
| 3.1 | RTGen: add `MultiReaderState`; extend `IMultiReaderStatus` with `getState`, `getStateMessage`, `getAffectedInputCount`/`getAffectedInputIndex`, `getEventCount`/`getEvent`. Existing methods untouched. Regenerate bindings (Python/C#/…, per repo RTGen workflow). | `include/opendaq/multi_reader_status.h`, generated files |
| 3.2 | Extend `MultiReaderStatusImpl`: state, message, affected-input vector, ordered `(inputIndex, eventPacket)` vector, cached-instance behavior (new object only when visible content changes). Keep the existing factory working; add an extended factory. | `reader_status_impl.h/.cpp` (or new `multi_reader_status_impl`) |
| 3.3 | `MultiReaderImpl::createReaderStatus` fills the new fields from the state machine; `getMainDescriptor` now carries main value descriptor + common output domain descriptor (spec §8.2). | `multi_reader_impl.cpp` |
| 3.4 | Status tests (`03_test_plan.md` §Status). | `tests/test_multi_reader.cpp` (StatusTest section) |

## Phase 4 — Configuration additions (spec §8.2) — can parallel Phase 3

| Step | Change | Files |
|---|---|---|
| 4.1 | Builder + reader: `setMainInput`/`getMainInput` (input global id; runtime change invalidates sync), `setMaxSynchronizationDistance`, `setDataLossTimeout` (+ getters). RTGen + bindings. | `multi_reader.h`, `multi_reader_builder.h`, `multi_reader_builder_impl.*`, `multi_reader_impl.*` |
| 4.2 | `SynchronizationManager`: enforce distance threshold with per-input diagnostics (uses Phase 1.1 absolute times); `NoCommonTick` iteration bound (spec §5.7). | `synchronization_manager.cpp` |
| 4.3 | `data_loss_monitor.h/.cpp` (spec §3.6): injectable clock for testability; scheduler-armed deadline entering `evaluateState`; `DataLost` state with lost-input set; recovery on packet. | new files, `multi_reader_impl.cpp` |
| 4.4 | Deprecate `tickOffsetTolerance` (value ignored + warning log; builder methods kept). | `multi_reader_impl.cpp` |

## Phase 5 — Resampling (spec §8.3)

| Step | Change | Files |
|---|---|---|
| 5.1 | RTGen interfaces `IResampler`, `IResamplerBuilder` + `ResamplerBuildContext`/`ResampleRequest`/`ResampleResult` shapes (finalize plain-struct vs. object-param per RTGen constraints); bindings. Add `OPENDAQ_ERR_RESAMPLER_BUILD_FAILED` (0x0002) and `OPENDAQ_ERR_RESAMPLE_FAILED` (0x0003) to `reader_errors.h` and document the implementer contract per `05_error_contract.md` §3.7–§3.8. | new `include/opendaq/resampler.h`, `resampler_builder.h`, `reader_errors.h` |
| 5.2 | `LinearResampler` + `LinearResamplerBuilder` implementation (numeric scalars, bracketing samples, `reset`, no extrapolation across window edges beyond one bracketing sample). | new `src/linear_resampler_impl.cpp` etc. |
| 5.3 | `QueueReader`: non-committing `peekSourceWindow(firstTick, lastTick)` returning packets+range for the resampler, and `commitSourceWindow` cursor advance (consumed = samples strictly before the next window's first needed tick, so bracketing samples are retained). | `queue_reader.h/.cpp` |
| 5.4 | `ReadCoordinator::configure` pipeline selection (direct iff grids coincide; builder call per resampled input; main input resampled when `targetSampleRate ≠` main rate); `getAvailableOutputCount`/plan/commit resampled branches; rebuild-on-resync wiring (sync invalidation clears pipelines). | `read_coordinator.cpp`, `multi_reader_impl.cpp` |
| 5.5 | Builder/reader `setTargetSampleRate`/`setResamplerBuilder` (+ getters); resampled-path buffer semantics (equal `count` per input) and common-grid `readWithDomain` output (spec §7.3). | `multi_reader*.h/.cpp` |
| 5.6 | Resampling tests incl. `MockResampler` (see `04_test_scaffolding.md`). | `tests/test_multi_reader_resampling.cpp` (new) |

## Phase 6 — Cleanup, deprecation, docs

| Step | Change | Files |
|---|---|---|
| 6.1 | Remove `MultiReaderFromExisting`: factory declaration + `ObjectCreator<IMultiReader>` specialization + `MultiReaderImpl(MultiReaderImpl* old, …)` constructor; delete/adapt `ReuseReader`, `MultiReaderActiveCopyInactive` tests; changelog entry + migration note ("recover in place via state machine"). **Compile dependency:** `sum_reader_fb_impl.cpp` (`recoverReaderIfNecessary`) uses this factory — replace it with the Phase 7.3 state-machine recovery (`06_usage_examples.md` §3.3) in the same PR or earlier. | `multi_reader.h`, `multi_reader_impl.h/.cpp`, `tests/test_factories.cpp`, `examples/.../sum_reader_fb_impl.cpp`, tests |
| 6.2 | Doc pass: public header doc-comments updated to final semantics (`getOffset`, `getMainDescriptor`, `getCommonSampleRate`, count semantics per path, timeout semantics) and `@retval` entries for every condition in `05_error_contract.md` §3. | `multi_reader.h`, `multi_reader_status.h`, `multi_reader_builder.h`, `resampler*.h` |
| 6.3 | Changelog: behavior-change table from spec §8.5 verbatim. | repo changelog |
| 6.4 | Full test sweep + threading stress runs (TSAN if available in CI). | — |

## Phase 7 — Usage example: sum function block (`06_usage_examples.md`)

Extends the existing `RefFBModuleSumReader` (`examples/modules/ref_fb_module/`) into the reference consumer of the reworked reader. Full design, error-handling policy, and test matrix live in `06_usage_examples.md`; steps summarized here:

| Step | Change | Depends on | Files |
|---|---|---|---|
| 7.1 | `Mode` property (`EqualRates`/`MultiRate`/`Resampled`); per-mode builder wiring + reader rebuild on mode change; MultiRate GCD-rate sum path with per-input buffer sizing (`count / divider_i` derived from public API) | Phase 2 | `sum_reader_fb_impl.h/.cpp` |
| 7.2 | `MainSignal` dynamically populated selection property + runtime `setMainInput`; selected-main disconnect fallback; Resampled sum path | Phase 5 | `sum_reader_fb_impl.h/.cpp` |
| 7.3 | Port parking + recovery probing driven by `getState`/affected inputs (park failing ports via `setInputUsed(false)`, probe them back); **delete `recoverReaderIfNecessary` / `MultiReaderFromExisting` usage** (must land ≤ Phase 6.1) | Phases 3–4 | `sum_reader_fb_impl.h/.cpp` |
| 7.4 | `ComponentStatus` reporting: Warning listing parked ports + reasons, Error for unrecoverable states; new FB properties `DataLossTimeout`, `MaxSynchronizationDistance`, `RecoveryRetryInterval` forwarded to the reader | Phases 3–4 | `sum_reader_fb_impl.h/.cpp` |
| 7.5 | Test suite per `06_usage_examples.md` §5 (mode matrix + all reachable error states, exact-sum ramp assertions, no sleeps) | 7.1–7.4 | `tests/test_fb_sum.cpp` |

**Done when:** every FB-visible reader state from spec §6.1 is demonstrated with recovery in `test_fb_sum.cpp`; the FB compiles and passes with `MultiReaderFromExisting` gone; all three modes produce exact sums on ramp inputs.

---

## Risk register

| Risk | Mitigation |
|---|---|
| LCM/GCD overflow with pathological rates or resolutions | Checked 64-bit helpers with tests (spec §4.1); `Incompatible` on overflow |
| `SameThread` default change breaks port-based consumers relying on `Scheduler` dispatch | Bounded `packetReceived` makes SameThread safe; builder override still available; changelog entry. Known in-repo consumer: `sum_reader_fb_impl.cpp` (sets its mode explicitly via `ReaderNotificationMode` config — unaffected); grep for others before merging Phase 2 |
| Existing tests encode old behavior ambiguously | Triage table in `03_test_plan.md` Part C is reviewed before Phase 2.9 starts, not during |
| RTGen binding regeneration (status, resampler interfaces) drifts from hand-written headers | Regenerate + build bindings in the same PR as the interface change |
| Shared use of `TypedReader`/`ReadInfo` by stream/block/tail readers | Phase 2.7 checks usage first; only the multi-reader dependency is severed if shared |
| Resampled availability computation cost (per-read `getRequiredSourceRange`) | Contract allows caching per configure; measure in Phase 5 before optimizing |
| `getOffset` semantic change surprises downstream tooling | Changelog + spec §7.4; value now consistent with `getOrigin`/`getTickResolution`, which was the documented contract all along |

## Suggested PR slicing

One PR per phase (Phase 2 possibly split: 2.1–2.3 internals + 2.4–2.8 integration). Each PR: spec section references in the description, triage notes for any test it adapts, green CI.
