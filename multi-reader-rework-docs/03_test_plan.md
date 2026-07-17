# Multi Reader Rework — Test Plan

**Companion to:** `01_specification.md` (spec §-references below), `02_implementation_plan.md` (phase references), `04_test_scaffolding.md` (fixtures/helpers used by the matrix), `05_error_contract.md` (error codes verified in group ERR / suite B.9). Function-block-level usage-example tests (sum FB modes + error recovery, `test_fb_sum.cpp`) are specified separately in `06_usage_examples.md` §5.

Structure: **Part A** — edge-case catalog (IDs referenced everywhere else). **Part B** — full matrix of new tests. **Part C** — triage of the existing `test_multi_reader.cpp` suite (106 tests).

---

## Part A — Edge-case catalog

### DR — Domain & rate

| ID | Edge case |
|---|---|
| DR-1 | Inputs with different epochs (whole-second and fractional, e.g. `…T00:02:04.123`); earliest epoch becomes common origin |
| DR-2 | Resolutions `1/10` + `1/15` → common resolution `1/30` (rational GCD, spec §4.1); conversions stay exact |
| DR-3 | Nested resolutions (`1/1000`, `1/1000000`) → finest wins (GCD reduces to it) |
| DR-4 | Rates `{10, 15}` Hz → `commonSampleRate=30`, dividers `{3, 2}`, `blockLcm=6` (spec §4.2) |
| DR-5 | `requiredCommonSampleRate` valid multiple of all rates / invalid (not a multiple of an input **rate** — must be checked against the rate, not the divider) |
| DR-6 | Non-integer sample rate or non-integer linear delta → `UnsupportedDomainRule` issue, recoverable |
| DR-7 | Linear delta > 1 (e.g. 10 kHz clock, delta 10) |
| DR-8 | `allowDifferentRates=false` with differing rates → `Incompatible` |
| DR-9 | Overflow in LCM/GCD (huge coprime rates/resolutions) → `Incompatible`, no wraparound (spec §4.1) |
| DR-10 | Unparsable origin string → `OriginParsingFailed`, recoverable on next descriptor |
| DR-11 | Non-time domain quantity/unit rejected (existing `checkDomainUnits` rules) |

### QC — Queue & cursor (QueueReader)

| ID | Edge case |
|---|---|
| QC-1 | Read starts mid-packet (non-zero `readingPosition`) |
| QC-2 | One read spans ≥ 3 packets |
| QC-3 | Read ends exactly on a packet boundary (cursor resets to next packet) |
| QC-4 | Zero-sample data packet in the queue |
| QC-5 | Leading event packets on an otherwise empty queue |
| QC-6 | Unknown/unsupported packet type treated as a segment boundary, not crossed |
| QC-7 | Read count not a multiple of the divider → error, nothing consumed |
| QC-8 | `skip` moves the cursor identically to `read` |
| QC-9 | Reconnect (`updateConnection`) resets queue state cleanly |
| QC-10 | Vector (1-D dimension) signals: `valuesPerSample` honored in buffer math (spec §3.1) |
| QC-11 | Rank-≥2 and struct value samples readable as fixed-size blocks (`valuesPerSample` = product of dims); domain descriptors with dimensions rejected via `UnsupportedDimensions` (spec §3.1) |
| QC-12 | `dropOutdatedPacketSegments` keeps only the newest segment and all reportable events |

### EV — Events

| ID | Edge case |
|---|---|
| EV-1 | Consecutive descriptor changes merge; newest value + newest domain descriptor win |
| EV-2 | Value-only then domain-only change → single merged `DomainAndValueChanged` |
| EV-3 | Gap events never merge — gap+gap stays two events, both `gapDiff`s preserved (spec §3.1) |
| EV-4 | `Desc, Gap, Desc` order preserved as three pending events |
| EV-5 | Event behind data acts as a hard boundary; reads stop before it |
| EV-6 | Event mid-request with complete blocks before it: blocks returned first, event on next call |
| EV-7 | Fewer than one `blockLcm` block before the event → **silent discard**, no synthetic event, no dropped count; original event returned (spec §7.2) |
| EV-8 | Silent discard on one input does not trim other inputs directly (post-event resync realigns them) |
| EV-9 | Events pending on several inputs at once → one event per input per read call |
| EV-10 | More than one pending event on one input → consecutive read calls drain them in order |
| EV-11 | Pending events block `read`/`skip`/`advanceToDomainValue` on that queue |
| EV-12 | Value-descriptor change to a non-convertible type → event returned, then `Incompatible`; recoverable by a later convertible descriptor |
| EV-13 | Every returned event invalidates synchronization; no data before realignment |
| EV-14 | Gap packet (port gap detection enabled) delivered as event + forces resync |

### SY — Synchronization (spec §5)

| ID | Edge case |
|---|---|
| SY-1 | All inputs share the first tick → synchronized on first pass |
| SY-2 | Latest first-sample wins as start candidate (even when it isn't on the signal with the latest epoch) |
| SY-3 | Candidate rounded up to `Ratio(blockLcm, commonSampleRate)` interval |
| SY-4 | `startOnFullUnitOfDomain` rounds up to the full second |
| SY-5 | Target falls between an input's ticks → advance reaches a later sample → iterative re-target converges (spec §5) |
| SY-6 | Reached values verified by converting **back** to common domain — nearest-tick rounding cannot fake success (spec §4.3) |
| SY-7 | delta-2 signals on odd/even ticks (no common tick) → iteration bound → `SynchronizationFailed` / `NoCommonTick` |
| SY-8 | `NeedMoreData` keeps the candidate; sync resumes when data arrives |
| SY-9 | First sample already past the target (`OvershotError`) → candidate recomputed, no error escalation |
| SY-10 | Event encountered while advancing → `EventPending`, sync restarted after handling |
| SY-11 | `maxSynchronizationDistance` exceeded → `SynchronizationFailed` with per-input timestamps/differences; reader stays active (spec §5) |
| SY-12 | `maxSynchronizationDistance=0` disables the check |
| SY-13 | Resync forced by: returned event, reconnect, used-set change, main-input change, target-rate change, activation |
| SY-14 | Main input defines grid phase/rate; a later-starting non-main input still synchronizes (Draft 11 flaw case) |
| SY-15 | Selected main input disconnected → `WaitingForConnections`, never silently replaced |

### RS — Read semantics (spec §7)

| ID | Edge case |
|---|---|
| RS-1 | Requested count rounded down to `blockLcm`; remainder untouched |
| RS-2 | `count != 0` below `minReadCount` → `OPENDAQ_ERR_INVALIDPARAMETER` |
| RS-3 | Effective minimum raised to `ceil(minReadCount/blockLcm)*blockLcm` |
| RS-4 | Zero-count read returns events/status, consumes no data (spec §7.2) |
| RS-5 | A call returns data XOR events, never both |
| RS-6 | `skipSamples` uses identical alignment/planning as `read` (today it doesn't) |
| RS-7 | Per-signal buffer segment sizes = `count/divider_i` (direct path) |
| RS-8 | Direct path: `readWithDomain` outputs each signal's own domain values (spec §7.3) |
| RS-9 | Status offset & `getOffset(void*)` = common-domain tick; `origin + offset·resolution` equals first sample time (spec §7.4) |
| RS-10 | Timeout: wait satisfied by arriving data / by an arriving event / expires with partial data |
| RS-11 | Unused input's buffer may be null; no samples written |
| RS-12 | `getAvailableCount` returns 0 unless `Synchronized`; never mutates queues |
| RS-13 | Availability = min across inputs, stops at earliest event boundary |

### LC — Lifecycle

| ID | Edge case |
|---|---|
| LC-1 | Construction from signals vs. from ports (incl. mixed rejection) |
| LC-2 | Port-based reader with a port connected only later → `WaitingForConnections` → recovers |
| LC-3 | Disconnect while synchronized → `WaitingForConnections`; reconnect revalidates + resyncs, same instance |
| LC-4 | `setInputUsed(false)`: excluded from sync/availability/events/data-loss; port deactivated; buffers may be null |
| LC-5 | `setInputUsed(true)` again: queue reset, full revalidation + resync |
| LC-6 | `setActive(false)`: ports inactive, data dropped, descriptors retained; `setActive(true)` runs full setup |
| LC-7 | `addInput` at runtime invalidates the model; new input participates after revalidation |
| LC-8 | `removeInput` at runtime; buffer order follows remaining slots; no stale `commonStart` |
| LC-9 | Reader disposal during a blocked (timeout) read — no hang, no crash |
| LC-10 | `markAsInvalid` → `Error`, `getValid()==false`; every other failure keeps `getValid()==true` |

### TH — Threading & notifications (spec §9)

| ID | Edge case |
|---|---|
| TH-1 | User `onDataAvailable` never runs on the producer (packet) thread (spec §9) |
| TH-2 | N packets in a burst → one coalesced callback |
| TH-3 | Callback invoked outside all internal locks (calling `read` from inside the callback must not deadlock) |
| TH-4 | `getAvailableCount`/`read` correct even if the coalesced task hasn't run (synchronous pending processing) |
| TH-5 | Concurrent `read` + packet arrival stress — no torn state, counts consistent |
| TH-6 | Blocked timeout read woken by data; woken by event |
| TH-7 | Race-free coalescing loop: pending bit set during task processing triggers re-run |

### DL — Data loss (spec §3.6)

| ID | Edge case |
|---|---|
| DL-1 | Deadline fires with no packet arriving at all (scheduler-armed, not read-triggered) |
| DL-2 | Lost set lists only stale inputs; healthy inputs excluded |
| DL-3 | Next packet from a lost input clears only that input; state exits `DataLost` when the set empties |
| DL-4 | `dataLossTimeout=0` disables monitoring (default) |
| DL-5 | Inactive reader / unused input / disconnected input not monitored |
| DL-6 | Monitoring arms only after the first packet post connect/activate |

### RE — Resampling (spec §8.3)

| ID | Edge case |
|---|---|
| RE-1 | Upsampling: linear interior points exactly interpolated between bracketing source samples |
| RE-2 | Downsampling onto a coarser output grid |
| RE-3 | Direct copy auto-detected when an input's grid coincides with the output grid — builder not called for it |
| RE-4 | `targetSampleRate ≠` main rate → main input resampled too |
| RE-5 | No interpolation across an event/gap: source windows never span an event boundary |
| RE-6 | Resamplers rebuilt after every resynchronization (event, reconnect, config change) |
| RE-7 | One builder → independent resampler instances per input (no shared state) |
| RE-8 | Builder called exactly once per resampled input per configure |
| RE-9 | Resampled `readWithDomain`: identical common-grid domain arrays for all inputs (spec §7.3) |
| RE-10 | Equal output `count` for every used input on the resampled path |
| RE-11 | Builder rejects an unsupported descriptor → `Incompatible` naming the input |
| RE-12 | Bracketing sample retained across consecutive reads (window commit keeps the last source sample needed next) |
| RE-13 | Custom (mock) builder injected via `setResamplerBuilder`; its output reaches user buffers |
| RE-14 | Target-rate change at runtime invalidates sync and pipelines |

### ST — Status & API compatibility (spec §8)

| ID | Edge case |
|---|---|
| ST-1 | `getEventPackets` dict still keyed by port global id, one event per input per call |
| ST-2 | `getMainDescriptor` carries main value descriptor + common output domain descriptor (spec §8.2) |
| ST-3 | `getState` matches the §6.1 state for every reachable state |
| ST-4 | `getStateMessage` names affected inputs and details (callers infer recovery from state + inputs + message; there is no recovery enum) |
| ST-5 | `getAffectedInputIndex` uses construction-order indices |
| ST-6 | Ordered event list: multiple events, correct `(inputIndex, packet)` pairs, order preserved |
| ST-7 | Status object identity cached while visible content unchanged; new object on change |
| ST-8 | `IReaderStatus::getEventPacket` returns the first event (compat) |
| ST-9 | `getCommonSampleRate`, `getTickResolution`, `getOrigin` reflect the common model incl. after resync |

### ERR — Public API error contract (`05_error_contract.md`)

| ID | Edge case |
|---|---|
| ERR-1 | Null out-parameters → `OPENDAQ_ERR_ARGUMENT_NULL` on every getter and read method (sweep) |
| ERR-2 | `read`/`readWithDomain` with `*count != 0` and null `samples`/`domain` → `ERR_ARGUMENT_NULL`; `*count == 0` with nulls succeeds (zero-count read) |
| ERR-3 | `0 < *count < minReadCount` → `ERR_INVALIDPARAMETER`; `*count == minReadCount` succeeds (boundary) |
| ERR-4 | Stream conditions never produce error codes: unsynchronized / `Incompatible` / `EventPending` / `DataLost` reads all return success-class with the condition in the status |
| ERR-5 | `getOffset`, `getTickResolution`, `getOrigin` → `OPENDAQ_IGNORED` (success-class) before the common model / synchronization exists |
| ERR-6 | Unknown-id asymmetry retained: `removeInput` → `OPENDAQ_NOTFOUND` (success-class); `setInputUsed`/`getInputUsed` → `OPENDAQ_ERR_NOTFOUND`; `setMainInput` → `ERR_NOTFOUND` |
| ERR-7 | `addInput`: duplicate global id → `ERR_DUPLICATEITEM`; wrong component type / mixed signals+ports → `ERR_INVALIDPARAMETER` |
| ERR-8 | `setMainInput` on an unused input → `ERR_INVALIDPARAMETER`; empty/null id clears selection with success |
| ERR-9 | `setTargetSampleRate(rate ≤ 0)`, negative `setMaxSynchronizationDistance`/`setDataLossTimeout` → `ERR_INVALIDPARAMETER` |
| ERR-10 | Status index methods: `getAffectedInputIndex`/`getEvent` beyond count → `ERR_OUTOFRANGE` |
| ERR-11 | `IMultiReaderBuilder::build` with no inputs / mixed inputs / notification-list size mismatch → `ERR_VALIDATE_FAILED` with cause in error info |
| ERR-12 | Failed calls leave no trace: after any error return, cursors, configuration, and state are unchanged |
| ERR-13 | Resampler implementer contract: builder `ERR_NOT_SUPPORTED`/`ERR_RESAMPLER_BUILD_FAILED` → reader enters `Incompatible` naming the input; `resample` failure aborts before any cursor commit and never leaks the code through the read call |
| ERR-14 | `skipSamples` in `Error` state → `OPENDAQ_IGNORED`, count 0 (retained behavior) |

---

## Part B — New-test matrix

Suites: `QueueReaderTest` (unit), `SyncManagerTest` (unit, new), `ReadCoordinatorTest` (unit, new), `MultiReaderTest` (integration additions), `MultiReaderThreadingTest` (new), `DataLossTest` (new), `MultiReaderResamplingTest` (new), `MultiReaderStatusTest` (new), `MultiReaderErrorContractTest` (new). Fixtures per `04_test_scaffolding.md`.

### B.1 QueueReaderTest additions (Phase 0–1)

| Test name | Description | Edge cases |
|---|---|---|
| AdvanceReturnsReachedValue | Advance to an existing tick; outcome carries that tick as reachedValue | SY-6 |
| AdvanceReachedValueBetweenTicks | Target between samples; Success with reachedValue = next sample's tick | SY-5 |
| AdvanceBlockedByPendingEvent | Pending event → advance refuses to cross / reports without consuming data | EV-11 |
| AdvanceOvershootFirstSamplePastTarget | First unread sample already past target → OvershotError, queue untouched | SY-9 |
| AvailableUntilEventStopsAtEvent | Count stops at the event packet, not queue end | EV-5, RS-13 |
| AvailableUntilEventNoEventWholeQueue | Without events equals total available | RS-13 |
| DiscardLeftoverSilent | < 1 block before event: samples dropped, **no** synthetic event, original event still pops | EV-7 |
| DiscardLeftoverKeepsCompleteBlocks | ≥ 1 block before event → returns false, nothing dropped | EV-6 |
| DiscardLeftoverNoEventNoDrop | No event in queue → no drop even when short | EV-7 |
| DiscardLeftoverDividerScaling | divider 2, block 10 → threshold 5 native samples | DR-4, EV-7 |
| GapEventsDoNotMerge | Two gaps queued → two pending events, both gapDiffs intact | EV-3 |
| GapDescriptorOrderPreserved | Desc, Gap, Desc → three events in order | EV-4 |
| DescriptorMergeKeepsNewest | V-change then D-change then V-change → single merged event, newest descriptors | EV-1, EV-2 |
| VectorValueSignalLayout | 1-D dimension signal: read fills count·valuesPerSample values correctly | QC-10 |
| MatrixValueSignalReadable | 2-D dimension signal readable: count is in samples, buffer receives count·product(dims) values | QC-11 |
| StructValueSignalReadable | Struct-type signal readable as raw fixed-size blocks (`Undefined` read type → raw bytes); field values intact after read | QC-11 |
| DomainWithDimensionsRejected | Domain descriptor with dimensions → UnsupportedDimensions issue, recoverable | QC-11 |
| ReadPartialAcrossThreePackets | One read spanning 3 packets incl. mid-packet start | QC-1, QC-2 |
| ReadExactPacketBoundary | Read ending exactly at packet end; next read starts clean | QC-3 |
| ZeroSamplePacketHandled | Zero-sample packet doesn't break counts or cursor | QC-4 |
| NonIntegerDeltaIssue | delta=1.5 → UnsupportedDomainRule; clears on valid descriptor | DR-6 |
| NonIntegerSampleRateIssue | resolution/delta giving fractional rate → issue set | DR-6 |
| OriginParseFailureRecovers | Bad origin → OriginParsingFailed; later good origin clears it | DR-10 |
| ValueTypeNotConvertibleRecovers | Struct→Float64 conversion flagged; later Float32 descriptor clears | EV-12 |
| FirstSampleAbsoluteTime | Absolute time of first unread sample matches epoch+tick·resolution | SY-11 |
| ReadCountNotDividerMultipleErrors | count % divider ≠ 0 → Error, count 0, queue untouched | QC-7 |
| SkipMatchesReadCursor | skip(n) then read equals read(n) then read | QC-8, RS-6 |
| ConnectionReplacedResetsState | updateConnection after reconnect: fresh queue, descriptors re-parsed | QC-9, LC-3 |
| LeadingEventsOnEmptyQueueConsumed | Only events in queue → all pending, available 0 | QC-5 |
| UnknownPacketTypeBoundary | Unsupported packet type → treated as boundary, no crash, no cross | QC-6 |
| DropOutdatedKeepsEventsAndLastSegment | Multiple segments + events → only newest segment data retained, events pending | QC-12 |
| PendingEventBlocksReadAndSkip | read/skip return Error while events pending | EV-11 |

### B.2 SyncManagerTest (new suite, Phase 2/4)

| Test name | Description | Edge cases |
|---|---|---|
| CommonEpochEarliest | Mixed epochs incl. fractional → earliest chosen; getOrigin matches | DR-1 |
| CommonResolutionRationalGcd | 1/10 + 1/15 → 1/30 | DR-2 |
| CommonResolutionNested | 1/1000 + 1/1000000 → 1/1000000 | DR-3 |
| CommonRateLcmAndDividers | {10,15} Hz → rate 30, dividers {3,2}, blockLcm 6 | DR-4 |
| RequiredRateValid | required=60 with {10,15} → dividers {6,4}, blockLcm 12 | DR-5 |
| RequiredRateInvalidRejected | required=40 with 15 Hz input → Incompatible (checked against rate) | DR-5 |
| EqualRatesOnlyViolation | allowDifferentRates=false, {10,15} → Incompatible | DR-8 |
| OverflowRejected | Coprime near-2^31 rates → Incompatible, no UB | DR-9 |
| AlignFirstTry | Identical starts → Synchronized in one pass | SY-1 |
| AlignLatestStartWins | Staggered starts → candidate = latest, all advance to it | SY-2 |
| AlignRoundUpBlockInterval | Candidate rounded to Ratio(blockLcm, commonRate) | SY-3 |
| AlignRoundUpFullUnit | startOnFullUnitOfDomain → next whole second | SY-4 |
| IterativeRetargetConverges | Target between ticks of one input → re-target on its reached value → all equal | SY-5, SY-6 |
| NoCommonTickFailsAfterBound | delta-2 odd/even inputs → bound hit → SynchronizationFailed(NoCommonTick) | SY-7 |
| NeedMoreDataKeepsCandidate | One input short on data → Synchronizing; completing data finishes sync | SY-8 |
| OvershootRecomputesCandidate | Input's first sample past candidate → recompute, converge | SY-9 |
| EventDuringAlignment | Domain-change event mid-advance → EventPending, no partial sync state | SY-10 |
| SyncDistanceExceededDiagnostics | Span > threshold → failure lists offending inputs + differences | SY-11 |
| SyncDistanceZeroDisables | Span huge, threshold 0 → sync proceeds | SY-12 |
| MainInputDefinesGridPhase | Main with offset grid; others align to main's phase, not their own | SY-14 |

### B.3 ReadCoordinatorTest (new suite, Phase 2)

| Test name | Description | Edge cases |
|---|---|---|
| AvailabilityMinAcrossInputs | min(available_i · divider_i) governs | RS-13 |
| AvailabilityBlockAligned | Result floored to blockLcm multiple | RS-1 |
| AvailabilityZeroBelowEffectiveMin | Below ceil(minReadCount/blockLcm)·blockLcm → 0 | RS-3 |
| AvailabilityStopsAtEarliestEvent | Event on one input caps everyone | EV-5, RS-13 |
| AvailabilityDoesNotMutate | Repeated calls identical; cursors unmoved | RS-12 |
| PlanRoundsRequestDown | Request 17, blockLcm 6 → plan 12 | RS-1 |
| PlanPerInputCounts | count/divider_i per input | RS-7 |
| PlanRejectsNullBufferUsedInput | Null buffer for used input → error before any commit | RS-7 |
| CommitAtomicAllInputs | All cursors advance together; induced failure pre-commit leaves all untouched | spec §10.8 |
| SkipSharesPlanner | skip identical alignment/counts to read | RS-6 |
| LeftoverDiscardOnlyAffectedInput | Discard on event-input only; others untouched until resync | EV-8 |
| UnusedInputExcluded | Unused slot: no availability contribution, no buffer requirement | LC-4, RS-11 |

### B.4 MultiReaderTest — integration additions (Phase 2–4)

| Test name | Description | Edge cases |
|---|---|---|
| StateWaitingForConnections | Port-based, one unconnected → state + count 0 | LC-2, ST-3 |
| StateWaitingForDescriptors | Connected, descriptor not yet sent → state | ST-3 |
| StateIncompatibleRecoverable | Bad domain rule → Incompatible; good descriptor → Synchronized; getValid stays true | EV-12, LC-10, ST-3 |
| StateWaitingForData | Valid but an input has no samples | ST-3 |
| EventThenDataNeverMixed | Data before event returned first, event next call, count 0 with event | RS-5, EV-6 |
| EventForcesResync | After event read, next data read starts at a fresh aligned start | EV-13, SY-13 |
| PartialBlockSilentDiscardE2E | Dividers {1,2}, event after 3 of 6-block: complete blocks read, remainder silently gone, original event returned, post-resync offset shows the discontinuity | EV-7, EV-8, RS-9 |
| MultipleEventsOneReadEach | Events on 2 inputs → single read returns both (dict has 2, ordered list 2) | EV-9 |
| MultipleEventsPerInputDrained | 2 events queued on one input → two consecutive event reads | EV-10 |
| GapEventDeliveredAndResync | Gap-detection port; gap → Event status + resync (replaces DISABLED_MultiReaderGapDetection) | EV-14 |
| DisconnectReconnectRecovers | Disconnect mid-stream, reconnect, same instance reaches Synchronized | LC-3 |
| RemoveInputWhileSynchronized | Model invalidated; remaining inputs resync; buffer order follows | LC-8 |
| AddInputWhileSynchronized | New input added → revalidation; participates after connect | LC-7 |
| SetUnusedExcludesEverywhere | Unused input: no sync input, null buffer OK, its events not delivered | LC-4 |
| ReenableUsedRevalidates | Re-enable → queue reset, resync, data aligned again | LC-5 |
| InactiveKeepsDescriptorsDropsData | setActive(false)/(true) cycle per spec §3.2 | LC-6 |
| OffsetIsCommonDomainTick | Multi-epoch/resolution set: origin + offset·resolution == first sample time on every read | RS-9, DR-1 |
| GetOffsetVoidWorks | IMultiReader::getOffset returns same tick as status offset | RS-9 |
| SkipAlignedLikeRead | skipSamples rounds to blockLcm exactly like read | RS-6 |
| MinReadCountEffective | minReadCount 5, blockLcm 6 → effective 6; availability reports 0 below | RS-3 |
| ZeroCountReadReturnsEvents | count=0 read → Event status, descriptors in dict, no data consumed | RS-4 |
| DifferentResolutions10and15 | End-to-end aligned read on 1/30 grid, values verified | DR-2, DR-4 |
| Delta2PhaseMismatchFails | Odd/even tick inputs → SynchronizationFailed, reader stays active | SY-7, SY-11 |
| MaxSyncDistanceFailsWithDiagnostics | Starts 10 s apart, threshold 5 s → failure, message lists input + delta | SY-11 |
| MaxSyncDistanceZeroDisables | Same data, threshold 0 → synchronizes | SY-12 |
| TickOffsetToleranceIgnoredDeprecated | Configured tolerance has no effect; warning logged | spec §8.4 |
| MainInputSelectionGrid | setMainInput(second input) → grid phase/rate follows it | SY-14, SY-15 |
| MainInputDisconnectedWaits | Selected main disconnected → WaitingForConnections, no silent replacement | SY-15 |
| RequiredRateEndToEnd | requiredCommonSampleRate honored; getCommonSampleRate matches | DR-5, ST-9 |

### B.5 MultiReaderThreadingTest (new suite, Phase 2)

| Test name | Description | Edge cases |
|---|---|---|
| NoCallbackOnProducerThread | CallbackProbe asserts callback thread ≠ sendPacket thread | TH-1 |
| BurstCoalescedToOneCallback | 100 packets in a burst → 1 callback (≤ small bound) | TH-2 |
| ReadFromInsideCallback | Calling read() inside onDataAvailable neither deadlocks nor corrupts | TH-3 |
| AvailabilityWithoutSchedulerTick | Packets sent, scheduler task withheld → getAvailableCount still correct | TH-4 |
| ConcurrentReadPacketStress | Reader thread + producer threads, N iterations; totals and alignment verified | TH-5 |
| TimeoutReadWokenByData | Blocked read completes when enough data arrives | TH-6, RS-10 |
| TimeoutReadWokenByEvent | Blocked read returns Event when event arrives | TH-6, RS-10 |
| DisposeDuringBlockedRead | Dispose while a timeout read waits → returns, no hang/crash | LC-9 |
| CoalescingRaceRerun | Pending bit set during task processing → task re-runs (white-box or stress) | TH-7 |

### B.6 DataLossTest (new suite, Phase 4)

| Test name | Description | Edge cases |
|---|---|---|
| DeadlineFiresWithoutPackets | Virtual clock passes deadline, no packet → DataLost | DL-1 |
| LostSetOnlyStaleInputs | One stale, one healthy → only stale listed | DL-2 |
| RecoveryOnNextPacket | Packet on stale input clears it; state exits DataLost | DL-3 |
| ZeroTimeoutDisables | Default 0 → never DataLost | DL-4 |
| InactiveUnusedNotMonitored | Inactive reader / unused input never trip | DL-5 |
| ArmsAfterFirstPacketOnly | No packets since connect → not armed, no false DataLost | DL-6 |

### B.7 MultiReaderResamplingTest (new suite, Phase 5)

| Test name | Description | Edge cases |
|---|---|---|
| UpsampleLinearExact | 10 Hz ramp → 30 Hz output; interior points exact linear values | RE-1 |
| DownsampleOnGrid | 1 kHz → 100 Hz target; on-grid samples selected/interpolated | RE-2 |
| DirectCopyAutoDetected | Input matching output grid: mock builder not called for it, values byte-equal | RE-3 |
| MainInputResampledOnTargetRate | targetRate ≠ main rate → main also goes through resampler | RE-4 |
| TargetRateChangeReconfigures | setTargetSampleRate at runtime → resync + pipelines rebuilt | RE-14, SY-13 |
| NoInterpolationAcrossEvent | Mock resampler records windows; none spans the event boundary | RE-5 |
| ResamplerRebuiltAfterResync | Event → resync → builder call count increments per resampled input | RE-6, RE-8 |
| IndependentResamplersPerInput | Two inputs get distinct instances; state not shared | RE-7 |
| CommonGridDomainIdentical | readWithDomain: identical domain arrays on the output grid | RE-9 |
| EqualCountAllInputs | Every used input returns the same count | RE-10 |
| UnsupportedDescriptorIncompatible | Mock builder rejects one input → Incompatible names it | RE-11 |
| BracketingSampleRetained | Consecutive reads: last source sample reused for next interpolation, values continuous | RE-12 |
| CustomBuilderOutputReachesUser | Mock produces sentinel values → they appear in user buffers | RE-13 |

### B.8 MultiReaderStatusTest (new suite, Phase 3)

| Test name | Description | Edge cases |
|---|---|---|
| EventPacketsDictCompat | Dict keyed by port global id, populated as before | ST-1 |
| MainDescriptorCommonDomain | Domain descriptor in getMainDescriptor = common output domain (origin/resolution/rule) | ST-2 |
| StateReportedPerMachine | Drive reader through §6.1 states; getState matches each | ST-3 |
| StateMessageNamesInputs | Message contains affected input references + details sufficient to act without a recovery enum | ST-4 |
| AffectedInputIndicesStable | Construction-order indices reported | ST-5 |
| OrderedEventListPairs | Multi-input, multi-event scenario: (inputIndex, packet) order correct | ST-6 |
| StatusCachedWhenUnchanged | Two identical-state reads → same status instance | ST-7 |
| StatusNewOnChange | State change → different instance | ST-7 |
| FirstEventCompatAccessor | IReaderStatus::getEventPacket returns first of the ordered list | ST-8 |

### B.9 MultiReaderErrorContractTest (new suite, grows with Phases 2–5)

| Test name | Description | Edge cases |
|---|---|---|
| NullOutParamSweep | Every getter/read method with a null out-pointer returns ERR_ARGUMENT_NULL | ERR-1 |
| ReadNullBuffersRules | Null samples/domain with nonzero count error; zero-count read with nulls succeeds | ERR-2 |
| MinReadCountBoundary | count = minReadCount−1 errors; count = minReadCount succeeds | ERR-3 |
| StreamConditionsNeverErrorCodes | Drive unsynchronized/Incompatible/EventPending/DataLost; every read returns success-class + status | ERR-4 |
| IgnoredBeforeModelEstablished | getOffset/getTickResolution/getOrigin return OPENDAQ_IGNORED + null/default before sync/model | ERR-5 |
| UnknownIdReturnCodes | removeInput vs setInputUsed vs setMainInput return values on unknown id per contract | ERR-6 |
| AddInputDuplicateAndWrongType | Duplicate id → ERR_DUPLICATEITEM; property object → ERR_INVALIDPARAMETER; port into signal-based reader → ERR_INVALIDPARAMETER | ERR-7 |
| MainInputParameterValidation | Unused input rejected; empty id clears; unknown id ERR_NOTFOUND | ERR-8, ERR-6 |
| ConfigValueValidation | Non-positive target rate, negative distance/timeout → ERR_INVALIDPARAMETER; zero disables | ERR-9 |
| StatusIndexOutOfRange | getAffectedInputIndex/getEvent past count → ERR_OUTOFRANGE | ERR-10 |
| BuilderValidateFailed | No inputs / mixed inputs / notification-list mismatch → ERR_VALIDATE_FAILED, cause chained in error info | ERR-11 |
| ErrorLeavesStateUntouched | After each error return: getAvailableCount, cursors (via subsequent read values), and config unchanged | ERR-12 |
| ResamplerFailureContainment | MockResamplerBuilder returns NOT_SUPPORTED / RESAMPLER_BUILD_FAILED → Incompatible names input; MockResampler resample failure → no cursor commit, read returns success-class with status | ERR-13 |
| SkipSamplesIgnoredInErrorState | markAsInvalid → skipSamples returns OPENDAQ_IGNORED, count 0 | ERR-14 |

---

## Part C — Triage of the existing `MultiReaderTest` suite

Legend: **keep** — passes unchanged; **adapt** — assertion updates required by a documented behavior change (spec §8.5 reference given); **delete** — obsolete.

| Test(s) | Disposition | Notes |
|---|---|---|
| SignalStartDomainFrom0, SignalStartRelativeOffset0, WithPacketOffsetNot0, WithPacketOffsetNot0Relative, MaxTimeIsNotOnSignalWithMaxEpoch | keep | Core aligned-read/latest-start behavior unchanged |
| SignalStartDomainFrom0SkipSamples | keep | Single rate → divider 1, alignment change invisible |
| SignalStartDomainFrom0Raw, UndefinedReadWithMockSignals, UndefinedValueType, OffsetToLinear | keep | Read-mode/type paths preserved |
| IsSynchronized | keep | Now state-derived; external semantics identical |
| SignalStartDomainFrom0Timeout, SignalStartDomainFrom0TimeoutExceeded, MultiReaderTimeoutWhenDataAvailable, MultiReaderTimeoutChecking, MultiReaderBuilderFromSignalsTimeouts | keep | Timeout `All` semantics unchanged; watch for scheduler-dispatch timing (use waits, not sleeps) |
| Clock10kHzDelta10 (+Relative, WithAlignedOffset, WithAlignedOffsetRelative, WithIntersampleOffset), Clock15MHzFromEpoch | keep | delta>1 and offset alignment covered by SY-3/SY-5 semantics, unchanged externally |
| EpochChanged, EpochChangedBeforeFirstData, ResolutionChanged, SampleRateChanged | keep | Event → resync flow; verify no reliance on sticky invalid |
| Signal2Invalidated | **adapt** | Sticky invalid removed (§8.5): expect recoverable `Incompatible`/`Fail` status with `getValid()==true`, and recovery on a good descriptor |
| ReuseReader | **delete** | `MultiReaderFromExisting` removed (§8.4) |
| MultiReaderActiveCopyInactive | **delete** | Same reason |
| MultiReaderWithInputPort, MultiReaderWithNotConnectedInputPort, MultiReaderWithDifferentInputs, MultipleMultiReaderToInputPort, MultiReaderReuseInputPort, ReadWhenOnePortIsNotConnected, NotifyPortIsConnected, ReadWhilePortIsNotConnected, TestReaderWithConnectedPortConnectionEmpty, TestReaderWithConnectedPortConnectionNotEmpty | keep / adapt | Port lifecycle; adapt any assumption of `Scheduler` default notification (§8.5 SameThread default) |
| MultiReaderOnReadCallback, MultiReaderFromPortOnReadCallback, MultiReaderActiveDataAvailableCallback | **adapt** | Callback now coalesced + never on producer thread: replace direct-invocation timing expectations with waits; a packet burst may yield fewer callbacks |
| StartOnFullUnitOfDomain, SampleRateDivider, SampleRateDividerRequiredRate, ExpectSR | keep | Divider/blockLcm math identical (LCM already used) |
| MultiReaderBuilderGetSet | **adapt** | Extend for new builder properties; existing assertions stay |
| MultiReaderBuilderWithDifferentInputs, MultiReaderExceptionOnConstructor, AddRemoveInput, UsedUnusedInput, MultiReaderActive, MultiReaderActiveFromPorts, MultiReaderActiveGapPacket | keep / adapt | Verify against §6.1 states instead of implicit flags where asserted |
| DISABLED_MultiReaderGapDetection | **adapt + re-enable** | Superseded by GapEventDeliveredAndResync (B.4); enable against the specified gap flow |
| TestTickOffsetExceeded, TestTickOffsetExceededByOffset | **adapt** | `tickOffsetTolerance` deprecated (§8.4): tolerance no longer fails sync nor deactivates the reader; convert into `maxSynchronizationDistance` tests or assert the deprecation no-op |
| BuilderNotificationMethodsUnspecified, BuilderNotificationMethodDefault, BuilderNotificationMethodsOverride | **adapt** | Default for port readers becomes `SameThread` (§8.5); explicit overrides unchanged |
| ReferenceDomainIdEquality01–05, ReferenceDomainIdInequality01–06, ReferenceDomainIdEqualityReferenceTimeProtocol{Equality01–04, Inequality01–15}, ReferenceDomainIdInequalityReferenceTimeProtocolInequality01–14 (45 tests) | keep | Reference-domain compatibility rules move into `SynchronizationManager` verbatim; outcomes unchanged |
| CheckSpecificCase | keep (review) | Re-read during Phase 2.9; reclassify if it encodes removed behavior |

Existing `test_queue_reader.cpp` tests asserting the synthetic sync-GAP event (leftover-segment group) are **adapted** in Phase 0.3 to the silent-discard contract.

---

## Coverage cross-check

Every spec §10 acceptance criterion maps to ≥ 1 matrix row:

| Spec §10 criterion | Covering rows |
|---|---|
| 1 — single ownership, legacy path removed | B.3 (coordinator behavior), B.4 integration rows; enforced structurally in Phase 2.7 |
| 2 — blockLcm alignment shared by all read ops | CommonRateLcmAndDividers, AvailabilityBlockAligned, PlanRoundsRequestDown, SkipSharesPlanner, SkipAlignedLikeRead |
| 3 — sync grid/start split, convergence, exact 1/30 grid | B.2 suite; DifferentResolutions10and15, MainInputSelectionGrid |
| 4 — events before data, silent partial-block discard | EventThenDataNeverMixed, EventForcesResync, DiscardLeftoverSilent, PartialBlockSilentDiscardE2E |
| 5 — merge rules and ordering | DescriptorMergeKeepsNewest, GapEventsDoNotMerge, GapDescriptorOrderPreserved |
| 6 — every failure state recoverable, valid only false in Error | StateIncompatibleRecoverable, DisconnectReconnectRecovers, Delta2PhaseMismatchFails, DataLoss recovery rows |
| 7 — bounded producer thread, coalesced callbacks | B.5 suite |
| 8 — plan-before-commit atomicity | CommitAtomicAllInputs, PlanRejectsNullBufferUsedInput |
| 9 — resampling contract | B.7 suite |
| 10 — API compatibility, FromExisting removed | Part C keep rows, B.8 compat rows, deleted ReuseReader/ActiveCopyInactive |
| 11 — status contents and caching | B.8 suite |
| 12 — common-domain offset round-trip | OffsetIsCommonDomainTick, GetOffsetVoidWorks, assertOffsetContract usage in B.4 |

The public API error contract (`05_error_contract.md`) is covered separately by the ERR edge-case group and the B.9 suite; every §3 table row of that document maps to at least one B.9 test.
