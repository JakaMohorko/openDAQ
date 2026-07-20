# Multi Reader Rework — Public API Error Contract

**Companion to:** `01_specification.md` (behavioral spec; section references below), `02_implementation_plan.md` (where the contract is implemented).
**Scope:** every public interface the multi reader touches: `IMultiReader`, `ISampleReader`, `IReader`, `IReaderConfig`, `IMultiReaderBuilder`, `IMultiReaderStatus`, `IReaderStatus`, and the new `IResampler` / `IResamplerBuilder`.

---

## 1. Design rules

1. **Error codes are for call failures, not stream conditions.** A read call that is well-formed always returns a success-class code; the *outcome* (not synchronized, incompatible descriptors, event pending, sync failed, data lost, reader in `Error` state) is reported through the returned `IMultiReaderStatus` (state, message, affected inputs — spec §6.1). Callers must never need to branch on error codes to handle normal data-stream life-cycle situations.
2. **Success-class codes are not errors.** `OPENDAQ_SUCCESS`, `OPENDAQ_IGNORED`, `OPENDAQ_NOTFOUND`, `OPENDAQ_NO_MORE_ITEMS` all pass `OPENDAQ_SUCCEEDED`. Where the current implementation returns one of these (e.g. `getOffset` → `OPENDAQ_IGNORED` when unsynchronized), that behavior is kept and documented — it is part of the compatibility surface.
3. **Every failure return carries error info.** Non-success returns are produced via `DAQ_MAKE_ERROR_INFO` with a message naming the parameter or input involved; implementation-internal exceptions are mapped to error codes at the ABI boundary and never escape.
4. **Errors never mutate reader state.** A failed call consumes no samples, moves no cursor, changes no configuration (ties to the plan-before-commit rule, spec §3.4). The single exception is `OPENDAQ_ERR_VALIDATE_FAILED` from `IMultiReaderBuilder::build`, which naturally leaves no reader behind.
5. **Out-parameter null checks come first.** Every method with out-parameters returns `OPENDAQ_ERR_ARGUMENT_NULL` when a required out-pointer is null; this is listed once here and not repeated in every table row below unless the rule differs.

## 2. Error codes used

### 2.1 Existing general codes (from `coretypes/errors.h`)

| Code | Used for |
|---|---|
| `OPENDAQ_ERR_ARGUMENT_NULL` | Required in/out pointer is null |
| `OPENDAQ_ERR_INVALIDPARAMETER` | Malformed argument value or invalid argument combination |
| `OPENDAQ_ERR_NOTFOUND` | Referenced input (global id) is not part of the reader |
| `OPENDAQ_ERR_INVALIDSTATE` | Operation is impossible in the object's current life-cycle state (e.g. disposed reader, unconfigured resampler) |
| `OPENDAQ_ERR_NOT_SUPPORTED` | Requested configuration is permanently unsupported by the implementation |
| `OPENDAQ_ERR_OUTOFRANGE` | Index argument beyond the reported count |
| `OPENDAQ_ERR_DUPLICATEITEM` | Component already present in the reader |
| `OPENDAQ_ERR_VALIDATE_FAILED` | Builder configuration fails cross-validation at `build()` |
| `OPENDAQ_ERR_INVALID_DATA` | (existing reader-family code, `reader_errors.h`) malformed source data handed to a resampler |

### 2.2 Existing success-class codes retained for compatibility

| Code | Where |
|---|---|
| `OPENDAQ_IGNORED` | `IMultiReader::getOffset` when not synchronized (documented today); `getTickResolution`/`getOrigin` when no common model exists yet; `skipSamples` in `Error` state (today's behavior) |
| `OPENDAQ_NOTFOUND` | `IMultiReader::removeInput` with an unknown id (today's behavior — note it is success-class, unlike `setInputUsed`, which returns `OPENDAQ_ERR_NOTFOUND`; the inconsistency is kept for compatibility and documented) |

### 2.3 New codes (added to `reader_errors.h`, family `OPENDAQ_ERRTYPE_READER = 0x0D`)

```c
#define OPENDAQ_ERR_RESAMPLER_BUILD_FAILED  OPENDAQ_ERROR_CODE(OPENDAQ_ERRTYPE_READER, 0x0002u)
#define OPENDAQ_ERR_RESAMPLE_FAILED         OPENDAQ_ERROR_CODE(OPENDAQ_ERRTYPE_READER, 0x0003u)
```

| Code | Meaning | Returned by |
|---|---|---|
| `OPENDAQ_ERR_RESAMPLER_BUILD_FAILED` | The build context was well-formed and of a supported kind, but the builder could not produce a resampler (resource/state failure). Distinct from `NOT_SUPPORTED` ("this strategy will never handle this descriptor") and `INVALIDPARAMETER` ("the context is malformed"). | `IResamplerBuilder::build` implementations |
| `OPENDAQ_ERR_RESAMPLE_FAILED` | The source window covered the required range but resampling failed (numerical failure, internal state corruption). | `IResampler::resample` implementations |

The multi reader itself never propagates these two codes to its own public methods: a failing build or resample surfaces as the `Incompatible` / `Error` state with the code and message embedded in `getStateMessage` (spec §8.3). They exist to give resampler implementers precise, documented semantics.

No other new codes are introduced: every reader-side failure condition maps cleanly onto an existing general code, and changing codes already returned today (e.g. `INVALIDPARAMETER` for `count < minReadCount`) would break callers that match specific codes.

---

## 3. Per-interface contract

Methods not listed under an interface return only `OPENDAQ_SUCCESS` (plus rule 5 null checks).

### 3.1 `IReader` (base)

| Method | Non-success returns | When |
|---|---|---|
| `getAvailableCount(count)` | `ERR_ARGUMENT_NULL` | `count` null. Otherwise always succeeds; returns 0 unless `Synchronized` (spec §7.1) |
| `setOnDataAvailable(callback)` | — | null unsets the callback |
| `setExternalListener(listener)` | — | null unsets the listener |
| `getEmpty(empty)` | `ERR_ARGUMENT_NULL` | `empty` null |

### 3.2 `ISampleReader`

| Method | Non-success returns | When |
|---|---|---|
| `getValueReadType` / `getDomainReadType` / `getReadMode` | `ERR_ARGUMENT_NULL` | out-pointer null |
| `setValueTransformFunction` / `setDomainTransformFunction` | — | null unsets; the function itself is validated lazily at read time (a throwing/failing transform fails that read's status, not the setter) |

### 3.3 `IMultiReader`

| Method | Non-success returns | When |
|---|---|---|
| `read(samples, count, timeoutMs, status)` | `ERR_ARGUMENT_NULL` | `count` null; or `samples` null while `*count != 0` |
| | `ERR_INVALIDPARAMETER` | `0 < *count < minReadCount` (unchanged from today, spec §7.2). Not returned for unaligned counts — those are rounded down silently |
| | *(none for stream conditions)* | `Error`/`Incompatible`/unsynchronized/etc. → `OPENDAQ_SUCCESS`, `*count = 0`, condition in `status` |
| `readWithDomain(samples, domain, count, timeoutMs, status)` | as `read` | plus `ERR_ARGUMENT_NULL` when `domain` null while `*count != 0` |
| `skipSamples(count, status)` | `ERR_ARGUMENT_NULL` | `count` null |
| | `ERR_INVALIDPARAMETER` | `0 < *count < minReadCount` |
| | `OPENDAQ_IGNORED` (success-class) | reader in `Error` state — retained current behavior; `*count = 0`, status carries `Error` |
| `getTickResolution(resolution)` | `ERR_ARGUMENT_NULL`; `OPENDAQ_IGNORED` (success-class) | out null; common model not yet established (no valid descriptors seen) — output set to null |
| `getOrigin(origin)` | `ERR_ARGUMENT_NULL`; `OPENDAQ_IGNORED` (success-class) | as above |
| `getOffset(domainStart)` | `ERR_ARGUMENT_NULL`; `OPENDAQ_IGNORED` (success-class) | out null; reader not synchronized (existing documented contract, spec §7.4) |
| `getIsSynchronized(isSynchronized)` | `ERR_ARGUMENT_NULL` | out null |
| `getCommonSampleRate(commonSampleRate)` | `ERR_ARGUMENT_NULL` | out null. Returns `OPENDAQ_SUCCESS` with `-1` while no common model exists (retained sentinel) |
| `setActive(isActive)` / `getActive` | `ERR_ARGUMENT_NULL` (getter) | — |
| `addInput(input)` | `ERR_ARGUMENT_NULL` | `input` null |
| | `ERR_INVALIDPARAMETER` | component is neither `ISignal` nor `IInputPort`; or mixes type with existing inputs (signals vs ports); or violates the per-port notification-methods list constraint |
| | `ERR_DUPLICATEITEM` | a component with the same global id is already an input (new, previously undefined behavior) |
| `removeInput(id)` | `ERR_ARGUMENT_NULL` | `id` null |
| | `OPENDAQ_NOTFOUND` (success-class) | no input with that global id — retained current behavior |
| `setInputUsed(id, isUsed)` | `ERR_ARGUMENT_NULL`; `ERR_NOTFOUND` | `id` null; unknown global id (retained current behavior) |
| `getInputUsed(id, isUsed)` | `ERR_ARGUMENT_NULL`; `ERR_NOTFOUND` | either pointer null; unknown global id |
| `setMainInput(inputId)` *(new)* | `ERR_NOTFOUND` | non-empty id not among the inputs. Null/empty id clears the selection (success). Selecting an **unused** input → `ERR_INVALIDPARAMETER` |
| `getMainInput(inputId)` *(new)* | `ERR_ARGUMENT_NULL` | out null; returns empty string when automatic (first used input) |
| `setTargetSampleRate(rate)` *(new)* | `ERR_INVALIDPARAMETER` | rate ≤ 0. Null clears (revert to main input's rate) |
| `getTargetSampleRate(rate)` *(new)* | `ERR_ARGUMENT_NULL` | out null; returns null when unset |
| `setResamplerBuilder(builder)` *(new)* | — | null restores the default `LinearResamplerBuilder` |
| `getResamplerBuilder(builder)` *(new)* | `ERR_ARGUMENT_NULL` | out null |
| `setMaxSynchronizationDistance(seconds)` / `setDataLossTimeout(seconds)` *(new, builder-only per C1/C2)* | `ERR_INVALIDPARAMETER` | negative value; 0 disables; null → `ERR_ARGUMENT_NULL`; not present on the reader |

Runtime configuration changes (`setMainInput`, `setTargetSampleRate`, `setResamplerBuilder`, `setInputUsed`) succeed immediately and invalidate synchronization (spec §5); any resulting incompatibility is reported through the state machine on the next evaluation, **not** as an error code from the setter — the setter cannot know yet (descriptors may still change).

### 3.4 `IReaderConfig`

| Method | Non-success returns | When |
|---|---|---|
| `getValueTransformFunction` / `getDomainTransformFunction` | `ERR_ARGUMENT_NULL`; `ERR_INVALIDSTATE` | out null; reader has no inputs (retained current behavior) |
| `getInputPorts(ports)` | `ERR_ARGUMENT_NULL` | out null |
| `getReadTimeoutType(timeoutType)` | `ERR_ARGUMENT_NULL` | out null |
| `markAsInvalid()` | — | always succeeds; transitions the reader to `Error` (spec §6.1) |
| `getIsValid(isValid)` | `ERR_ARGUMENT_NULL` | out null |

### 3.5 `IMultiReaderBuilder`

| Method | Non-success returns | When |
|---|---|---|
| `addSignal` / `addInputPort` | `ERR_ARGUMENT_NULL` | component null |
| `addSignals` / `addInputPorts` | `ERR_ARGUMENT_NULL`; `ERR_INVALIDPARAMETER` | list null; list element of wrong type |
| all simple setters (`setValueReadType`, `setDomainReadType`, `setReadMode`, `setReadTimeoutType`, `setStartOnFullUnitOfDomain`, `setAllowDifferentSamplingRates`, `setInputPortNotificationMethod(s)`, `setTickOffsetTolerance` *(deprecated, value ignored)*, and the new setters of §3.3) | — | value stored; cross-validation deferred to `build()` |
| `setMinReadCount(count)` | `ERR_INVALIDPARAMETER` | `count == 0` |
| `setRequiredCommonSampleRate(rate)` | — | any value stored; `rate <= 0` means "not required"; compatibility with actual input rates is a runtime condition (→ `Incompatible` state), not a build error |
| all getters | `ERR_ARGUMENT_NULL` | out null |
| `build(multiReader)` | `ERR_ARGUMENT_NULL` | out null |
| | `ERR_VALIDATE_FAILED` | invalid configuration combination detected at build time: no inputs; signals and ports mixed; notification-methods list size ≠ input count; unspecified notification method with signal inputs. (Today these surface as `ERR_INVALIDPARAMETER`/`ERR_NOTASSIGNED` from constructor exceptions; `build` wraps them uniformly as `ERR_VALIDATE_FAILED` with the original cause in the error info chain — callers using `OPENDAQ_FAILED` are unaffected) |

Signal/descriptor problems (wrong domain unit, non-linear rule, incompatible reference domains, …) are **never** build errors: the reader is constructed successfully and reports them through its state (`Incompatible` etc.), because connected signals can change descriptors at any time.

### 3.6 `IMultiReaderStatus` (incl. new methods) and `IReaderStatus`

| Method | Non-success returns | When |
|---|---|---|
| `getReadStatus` / `getValid` / `getOffset` / `getEventPacket` (`IReaderStatus`) | `ERR_ARGUMENT_NULL` | out null; unassigned values yield null/defaults with success |
| `getEventPackets` / `getMainDescriptor` / `getState` / `getStateMessage` / `getAffectedInputCount` / `getEventCount` | `ERR_ARGUMENT_NULL` | out null |
| `getAffectedInputIndex(statusIndex, inputIndex)` | `ERR_ARGUMENT_NULL`; `ERR_OUTOFRANGE` | out null; `statusIndex >= getAffectedInputCount()` |
| `getEvent(eventIndex, inputIndex, packet)` | `ERR_ARGUMENT_NULL`; `ERR_OUTOFRANGE` | any out null; `eventIndex >= getEventCount()` |

Status objects are immutable snapshots; no method on them can fail for state reasons.

### 3.7 `IResamplerBuilder` (contract binding implementers)

| Method | Non-success returns | When |
|---|---|---|
| `build(context, resampler)` | `ERR_ARGUMENT_NULL` | context or out null |
| | `ERR_INVALIDPARAMETER` | context malformed: missing descriptors, output rate ≤ 0 |
| | `ERR_NOT_SUPPORTED` | this strategy can never resample the given descriptors (e.g. `LinearResamplerBuilder` with non-numeric or non-scalar values, spec §8.3) |
| | `ERR_RESAMPLER_BUILD_FAILED` *(new)* | context valid and supported, but the build failed for another reason |

The multi reader treats **any** non-success return as "no pipeline for this input" → `Incompatible` state naming the input, with the builder's error message included in `getStateMessage`. It never re-throws the code to reader callers.

### 3.8 `IResampler` (contract binding implementers)

| Method | Non-success returns | When |
|---|---|---|
| `getRequiredSourceRange(outputStartTick, outputCount, firstNeeded, lastNeeded)` | `ERR_ARGUMENT_NULL`; `ERR_INVALIDPARAMETER` | out pointers null; `outputCount == 0` |
| `resample(request, result)` | `ERR_ARGUMENT_NULL` | request/result or their buffers null while `outputCount != 0` |
| | `ERR_INVALID_DATA` | source window does not cover the range previously reported by `getRequiredSourceRange`, or source domain values are non-monotonic |
| | `ERR_RESAMPLE_FAILED` *(new)* | window valid but resampling failed (numerical or internal failure) |
| | `ERR_INVALIDSTATE` | called before configuration / after a failure that requires `reset()` |
| `reset()` | — | always succeeds |

A non-success return from `resample` aborts the read plan **before any cursor commits** (spec §3.4): the read call itself still returns `OPENDAQ_SUCCESS` with `count = 0` and the reader transitions to `Error` (repeated internal failure) or `Incompatible` (recoverable, e.g. rebuilt pipeline), per spec §6.1.

---

## 4. Documentation and enforcement

- Every condition in §3 is documented as `@retval` in the corresponding public header (implementation plan Phase 6.2).
- The new codes land in `reader_errors.h` together with the `IResampler`/`IResamplerBuilder` headers (Phase 5.1).
- The error contract is covered by the error-contract test rows in `03_test_plan.md` (§B.9): null-argument sweeps, min-read-count boundaries, unknown-id returns (including the `removeInput` vs `setInputUsed` asymmetry), out-of-range status indices, builder validation failures, and the resampler implementer contract via the mock resampler.
