# 08 — Analysis Results: Complexity, Benchmarks, Profiling

**Date:** 2026-07-20
**Dev branch:** `refactor/AI-refactor` @ `ae028f24`
**Baseline (main):** `f26d249d`
**Build:** Release (MSVC 2022, x64), `build/x64/msvc-22/full`; benchmark target `bench_multi_reader` (standalone, public-API only, builds and runs on both branches).

This document records three analyses of the reworked multi reader: a structural complexity overview, a noise-controlled dev-vs-main benchmark, and a phase-attribution profile. It complements the handover specification in [`multi_reader.md`](multi_reader.md) (same directory).

---

## 1. Complexity Overview

### 1.1 System shape

A ~2,280-line facade (`multi_reader_impl.cpp`) owns one state mutex and an 11-state machine, delegating all cross-input work to six deliberately-thin components: QueueReader (979), SynchronizationManager (586), DataLossMonitor (239), NotificationCoordinator (167), ReadCoordinator (171), Input (180).

The structural root of the facade's complexity is that it runs **three passes over the same slots**, each with a different cost model:

| Pass | Entry | Cost | Runs when |
|---|---|---|---|
| Full state ladder | `evaluateStateLocked` (`multi_reader_impl.cpp:678`) | ~13 steps, O(slots) | topology / used / event / deadline change |
| Read/query fast pass | `refreshDataPlaneLocked` (`:540`) | O(slots), escalates to ladder | every read / `getAvailableCount` |
| Callback pass | `updateCallbackStateLocked` (`:1028`) | O(*changed* slots) | coalesced notify task |

### 1.2 Complexity ranking (most → least complex function)

| Function | Location | Nesting | Branches | Why |
|---|---|--:|--:|---|
| **`evaluateStateLocked`** | `multi_reader_impl.cpp:678` | 6 | ~60 | The 13-step transition ladder; each branch ends in a distinct `setStateLocked`+return; 4-case `SyncOutcome` switch |
| `readInternal` | `multi_reader_impl.cpp:1442` | 5 | ~38 | Whole read/skip/zero-count/commit under one lock; two `wait_for` predicate lambdas; state gate → plan+commit → cache invalidation |
| `parseDomainDescriptor` | `queue_reader.cpp:771` | 2 | ~24 | Longest function (~131 lines); 4-stage descriptor ladder feeding 6 issue flags |
| `buildCommonModelImpl` | `synchronization_manager.cpp:142` | 3 | ~26 | Sequential LCM/GCD validation gauntlet with overflow guards |
| `pickStartCandidate` | `synchronization_manager.cpp:322` | 3 | ~22 | Two-level tick-grid search (≤1024 steps × per-input), polymorphic `domainTickOf` |
| `refreshDataPlaneLocked` | `multi_reader_impl.cpp:540` | 5 | ~22 | Per-slot fast pass, two escalation modes, availability cache |
| `readNative` | `queue_reader.cpp:560` | 3 | ~21 | Per-block copy loop; readMode switch; dual function-pointer dispatch; sub-block cursor repair |

By contrast, **ReadCoordinator** (`commit`/`skip`/`createPlan`, branch counts 3–6) and the three leaf helpers (Input, NotificationCoordinator, DataLossMonitor) are intentionally near-trivial — their value is in threading contracts, not control flow.

### 1.3 The single most complex path

**The synchronized `read()`.** One `read()` call can fan through all of the above: `readInternal` → `refreshDataPlaneLocked` (fast pass, may escalate) → possibly the full `evaluateStateLocked` ladder → `createPlan`/`commit` → per-input `readNative` (per-packet, per-sample, function-pointer conversion) → possibly lazy implicit-domain materialization. It is the only path that touches every component and both the state machine and the per-sample leaf in a single lock hold. The complexity-density peak inside it is `evaluateStateLocked`.

### 1.4 Deepest call stacks (indirect calls marked)

| Path | Entry | Depth | Indirect hops | Character |
|---|---|--:|--:|---|
| **Data-loss escalation** | `data_loss_monitor.cpp:231` (`waiterLoop`) | 15 | 7 | Waiter thread → scheduler worker; ends at `setStateLocked(DataLost)` |
| **NOTIFY → onDataAvailable** | `input.cpp:64` (`packetReceived`) | 14 | **8 (most)** | Producer thread → scheduler worker → user callback |
| **Sync establishment** | `multi_reader_impl.cpp:678` (`evaluateStateLocked`) | 12–13 | **0** | Entirely static dispatch down the spine |
| **Synchronized READ** | `multi_reader_impl.cpp:1639` (`readWithDomain`) | 11 | 3 | Deepest leaf is signal-layer implicit-domain fill, not the reader |

Two findings worth flagging:

1. **The indirect calls live in the notification plumbing, not the data path.** The NOTIFY chain threads **8** indirect hops — `IInputListener` virtual → `std::function` WorkExecutor → `IScheduler` vtable → **taskflow thread boundary** → `IWork::execute` → task `std::function` → evaluation `std::function` → `IProcedure::dispatch` into the user handler. Data-loss is the same async spine (7 hops). N packets from N producers coalesce into **at most one** scheduled task via `scheduled.exchange(true)` (`notification_coordinator.cpp:39`).

2. **The sync-establishment spine is 100% statically dispatched** (`evaluateStateLocked → synchronize → advanceAllInputs → advanceToDomainValue → addEncounteredEvent → parseCachedDescriptors → parseDomainDescriptor → isSampleTypeConvertible → visitTwoSampleTypes → visitSampleType×2 → detail::isSampleTypeConvertible`, depth 12, **0 indirect hops**): `syncManager` is a concrete `unique_ptr`, slots/inputs are concrete pointer vectors, and the `visit*` dispatch is a compile-time monomorphized visitor. The `DomainValue` virtual family (`toDomain`/`fromDomain`/`compare`) and the `IConnection`/`IPacket` COM boundary *are* virtual, but they sit on **shallow leaf side-calls** (depth ~5), not on the deepest edge. The one genuine interface hop on the establishment branch is `dequeueUpTo` via `adoptPackets` (depth 6).

In the read leaf, per-packet cost is one pre-resolved function pointer (`valueReadFn`/`domainReadFn` → `detail::readData`, `memcpy`/`copy_n` fast path) plus per-packet `IDataPacket` virtual getters; the deepest READ frames are actually the signal layer's `DataRuleCalcTyped::calculateLinearRule` materializing implicit-domain samples (one `malloc` + O(sampleCount) fill per packet), *outside* the reader.

---

## 2. Benchmark Results — dev vs main

### 2.1 Method

- **30 interleaved rounds** (dev, main, dev, main …) on an idle machine to cancel slow thermal / background drift between the two binaries.
- **Per-key MAD outlier rejection**: reject a run where \|x − median\| > 3 · 1.4826 · MAD.
- Parsed with **InvariantCulture** so absolute values are exact (not only Δ%).
- Per-branch **coefficient of variation (CV)** reported; a key is flagged NOISY when max(CV) > 5%.
- `improve_pct` is signed so **positive always means dev is faster** regardless of whether the metric is higher-better (`Msamp_s`) or lower-better (`ns_*`, `us_per_resync`).

### 2.2 Noise verdict

Medians are **stable between the 15-round and 30-round aggregations**, so the conclusions are not artifacts of a particular set of runs. 28 of 83 keys still show >5% per-run CV, but that variance is **intrinsic** (scheduler / turbo / allocation jitter in the throughput scenarios) and does not shrink with more rounds — what more rounds bought is reliable medians and confident outlier rejection. Each finding below is classified by effect-size vs. noise.

### 2.3 Robust dev **wins** (effect ≫ noise)

| Scenario | metric | dev vs main |
|---|---|--:|
| `micro_status0` / `micro_statusbuild` (n=1/4/16) | ns/call | **+94 / +90 / +77%** (status0), +96 / +92 / +76% (statusbuild) |
| `micro_avail` (n=1/4/16) | ns/call | +31 / +67 / **+86%** |
| `inputs` 32 / 64 / 128 | ns/sample | +9 / +27 / **+30%** |
| `stress` 32×8 / 32×32 / 64×8 / 64×32 | ns/sample | +21 / +21 / **+43** / +37% |
| `event_inputs` 64 | Msamp_s | **+52%** |
| `convert` i16 / i32 / f64 | ns/sample | +10 / +10 / +5% |
| `inputs` 1 / 2 | ns/sample | +22 / +7% |
| `event_inputs` 16 | Msamp_s | +5% |
| `rates` equal_1_1_1_1 | ns/sample | +22% (main-side CV high) |

### 2.4 Robust dev **losses** (overhead-bound)

| Scenario | metric | dev vs main |
|---|---|--:|
| `inputs` 4 / 8 / 16 | ns/sample | −2.7 / **−8.7** / −4.3% |
| `packet` 1 / 2 / 4 / 8 | ns/sample | −6.2 / −8.1 / −3.4 / −3.8% |
| `packet` 16 / 256 | ns/sample | −2.3 / −3.8% |
| `stress` 16×8 / 16×32 | ns/sample | −5.2 / −5.4% |
| `rates` gcd_1_2 / gcd_1_2_5 / wide | ns/sample | −2.4 / −3.9 / −3.9% |

### 2.5 Within noise (no real difference)

`packet` 64 (+0.9%), 1024 (−1.2%), 16384 (−3.1%, CV~7%); `events` every_1/2/8 (+0.7 / +3.2 / −0.4%); `resync` 8 / 16 (+1.4 / +3.8%). Genuinely noisy — interpret loosely: `packet 4096` (−11.9%, CV~10%), `resync 4` (−14.9%, CV~9%), `events every_32/128` (−4.9 / −5.2%, CV~10%), `inputs 64` (large win but CV~15%).

### 2.6 The crossover

The `inputs` and `stress` sweeps show one clean turning point: **dev trails at low-to-mid input counts (4–16) and wins increasingly from 32 up.**

- `inputs` (ns/sample improve): +22 (1) → +7 (2) → −2.7 (4) → **−8.7 (8)** → −4.3 (16) → +9 (32) → +27 (64) → **+30% (128)**
- `stress` (ns/sample improve): −5 (16×) → +21 (32×) → **+37…43% (64×)**

Section 3 explains the mechanism.

---

## 3. Profiling — Phase Attribution (dev vs main)

### 3.1 Method

True function-level sampling (ETW / `wpr` / `xperf`) requires administrator rights, which the analysis shell does not have, and the RelWithDebInfo build fails on an RTGen codegen copy step (so no Release PDBs) — symbols are moot without a sampler either way. Instead, an **admin-free differential timing** scenario (`bench_multi_reader profile`, added in `ae028f24`) splits the per-call cost into phases, with packet **send held outside every timed region** so the numbers reflect the reader alone. 9 interleaved rounds on both branches. Function attribution comes from the static call-stack analysis in §1.

Phases: `avail` = `getAvailableCount` (state-eval + availability); `statusbuild` = `read(nullptr,&0)` minus `avail` (status construction); `read` = a consuming `read` of a fixed chunk, backlog refilled outside the clock (plan + commit + per-sample copy); `avail_event` = `getAvailableCount` with a leading descriptor event pending (event detection).

### 3.2 Phase table (ns; lower is faster)

| Phase | standard 4×2048 | small 4×8 | many 16×1024 | many 64×1024 |
|---|--:|--:|--:|--:|
| **avail** dev / main | 30 / 83 | 30 / 82 | 45 / 263 | **79 / 1937** |
| **statusbuild** dev / main | 90 / 893 | 90 / 1071 | 361 / 1149 | **1410 / 3960** |
| **read** dev / main (ns/sample) | 6.5 / 8.4 | **62 / 92** | 21 / 30 | 93 / 148 |
| **avail w/ event** dev / main | — | — | — | 30 / 155 (4×1024) |

Improvements: `avail` +64…96%, `statusbuild` +64…92%, `read` +23…37%, `avail_event` +81%.

### 3.3 Headline and reconciliation

**Reader-only, dev is faster on every phase in every scenario.** This flips the naive reading of §2: the throughput losses at low input counts and small packets are **not in the read path**.

The throughput benchmark also pays the rework's **per-packet producer-side notification** — every `sendPacket` fires `Input::packetReceived` (`input.cpp:64`) → sets `dataPlaneDirty` → `requestEvaluation` (`notification_coordinator.cpp:39`) → a coalesced `updateCallbackStateLocked` (`multi_reader_impl.cpp:1028`) on the scheduler thread — which the profile excludes (send outside the clock). That O(packets-per-batch) producer cost is what dev pays at low input counts; it is outrun only once main's **uncached O(inputs)** read-side cost explodes (main `avail` 1937 ns and `statusbuild` 3960 ns at 64 inputs, vs dev's cached 79 / 1410 ns). Hence the crossover at ~16–32 inputs.

> **Attribution caveat:** the producer-side conclusion is *inferred* from the profile-vs-throughput differential (reader-only dev wins everywhere + throughput dev loses at low N ⇒ the deficit is outside the timed read region, i.e. the notification path; packet creation/enqueue is branch-identical). A direct producer-path timer, or an elevated ETW sampled profile, would confirm it.

### 3.4 Hot paths per scenario

- **Small packets** — the read leaf dominates: **62 ns/sample vs 6.5 at standard** (≈10×), because a fixed read span crosses many packets and re-runs the per-packet loop in `readNative` (`queue_reader.cpp:560`). Fixed per-call overhead (`avail`+`statusbuild` ≈ 120 ns) is amortized over few samples. Dev's reader still beats main (+32%); the throughput loss is the producer-notify path.
- **Many inputs** — main's **uncached O(inputs)** `avail`+`statusbuild` is the hot path (2–4 µs at 64 inputs). Dev's availability/status caching caps this, so dev's read+poll wins scale up with input count.
- **Many events** — event *detection* is nearly free (`avail_event` ≈ `avail`; the leading-event zero-guard). The event *cost* lives in the resync path — `evaluateStateLocked` → `synchronize` → per-input `parseDomainDescriptor` (the depth-12 static spine from §1.4).
- **Standard** — per-sample copy dominates (6.5 ns/sample); fixed overhead negligible. Dev's read is ~23% faster per call; throughput is near-parity once the small producer-notify cost is added.

---

## 4. Reproduction

```powershell
# Build the benchmark on both branches (Release)
cmake --build build/x64/msvc-22/full --target bench_multi_reader --config Release -- -m

# Run one scenario (or omit the arg for the full suite; 'profile' is opt-in only)
bench_multi_reader.exe                 # full comparison suite
bench_multi_reader.exe profile         # phase-attribution scenario
bench_multi_reader.exe stress          # a single scenario
```

Harness (in the analysis scratchpad, reusable):
- `run_bench.ps1 -DevExe <path> -MainExe <path> -Rounds N [-StartRound k] [-Only <scenario>] -OutDir <dir>` — interleaved runner, one CSV per run.
- `aggregate_bench.ps1 -OutDir <dir> [-MadK 3.0] [-CvWarn 5.0]` — MAD outlier rejection, InvariantCulture parsing, per-branch CV, signed `improve_pct`, writes `summary.csv`.

Bench scenarios: `inputs` (1–128), `packet` (1–16384), `rates`, `events`, `event_inputs`, `stress`, `resync`, `convert`, `micro`, `profile`. All use only the public API common to both branches; the pre-existing comparison rows are byte-identical to before the extreme-case additions, so historical results remain valid.
