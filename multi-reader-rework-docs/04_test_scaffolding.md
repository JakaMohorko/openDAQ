# Multi Reader Rework — Test Scaffolding Plan

**Companion to:** `03_test_plan.md` (every suite there names its fixture/helpers from this document).
**Location:** `core/opendaq/reader/tests/` unless stated. New shared helpers go into `tests/multi_reader_test_utils.h` (+ `.cpp` where non-header-only), so both `test_multi_reader*.cpp` and the unit suites can include them.

---

## 1. Existing scaffolding — reused as-is

| Item | File | Provides | Used by |
|---|---|---|---|
| `ReaderTest<>` | `reader_common.h` | Context + 1-worker scheduler + logger with `LastMessageLoggerSink`, `signal`, `sendPacket(wait)`, `createDomainDescriptor(epoch, resolution, rule, refDomainInfo)`, `setupDescriptor`/`setupConfigurableDescriptor` | every suite |
| `MultiReaderTest` fixture | `test_multi_reader.cpp` | `addSignal(offset, packetSize, domainSignal, valueType)`, `createDomainSignal(...)`, `signalsToList()`, `portsList(gapDetection)`, `sendPackets(index)`, `printData`/`roundData` | integration suites |
| `ReadSignal` | `test_multi_reader.cpp` | Per-signal packet generator: linear domain packets from `packetOffset`/`packetSize`/delta, typed value fill, descriptor swap (`setValueDescriptor`), `toSysTime` | integration suites |
| `QueueReaderTest` fixture | `test_queue_reader.cpp` | Value+domain signal pair, `setDomainDescriptor`/`setValueDescriptor`, `setOffsetDelta`, `setPacketSize`, `sendNextPacket()` (auto-incrementing offset/values), `assertReaderAtDomainValue` | B.1 unit tests |
| Descriptor/packet factories | core (`DataDescriptorBuilder`, `DataPacket`, `DataPacketWithDomain`, `ImplicitDomainGapDetectedEventPacket`, `DataDescriptorChangedEventPacket`) | raw building blocks | everything |

Refactor note (small, Phase 2.9): move `ReadSignal` and the `MultiReaderTest` fixture out of `test_multi_reader.cpp` into `multi_reader_test_utils.h` so the new suites (`test_multi_reader_resampling.cpp`, `test_multi_reader_threading.cpp`, `test_multi_reader_status.cpp`, `test_data_loss.cpp`) can share them without duplicating ~250 lines.

---

## 2. New scaffolding

### 2.1 `MultiRateSignalBank` (header-only, `multi_reader_test_utils.h`)

Declarative multi-signal source with analytic ground truth — the workhorse for alignment assertions.

```cpp
struct BankSignalSpec {
    std::string epoch;          // ISO 8601
    RatioPtr    resolution;     // e.g. Ratio(1, 1000)
    Int         delta = 1;      // linear rule delta
    Int         firstTick;      // domain tick of sample 0
    SizeT       packetSize;
    SampleType  valueType = SampleType::Float64;
};

class MultiRateSignalBank {           // built on ReadSignal + createDomainSignal
public:
    void addSignal(const BankSignalSpec&);
    void sendAll(SizeT packetsEach);              // interleaved sends
    void send(SizeT sigIdx, SizeT packetCount);
    ListPtr<ISignal> signals() const;
    ListPtr<IInputPortConfig> ports(bool gapDetection = false) const;

    // Analytic ground truth (no reading involved):
    // value at absolute time t for signal i is defined as valueAt(i, tick) = tick (ramp),
    // so aligned reads and linear resampling results are predictable in closed form.
    double expectedValueAtCommonTick(SizeT sigIdx, Int commonTick) const;
    Int    expectedAlignedStartCommonTick(SizeT blockLcm, Int commonSampleRate,
                                          bool fullUnit) const;
};
```

Ramp values (`value == domain tick`) make both divider-aligned reads and linear interpolation exactly predictable (`expected = interpolated tick value`), so tests assert equality, not tolerances (double-precision exactness holds for the tick magnitudes used in tests). Used by: B.2 (SyncManagerTest), B.4 (DifferentResolutions10and15, OffsetIsCommonDomainTick…), B.7 (all resampling value checks).

### 2.2 `ScenarioScript` (header-only)

Declarative per-signal action sequence — makes event/edge tests readable and reviewable against the spec.

```cpp
struct ScenarioStep {  // one of:
    // data(sigIdx, sampleCount)              – send one data packet
    // descChange(sigIdx, ValuePart|DomainPart|Both, descriptor)
    // gap(sigIdx, diff)                      – enqueue gap event packet
    // disconnect(sigIdx) / reconnect(sigIdx)
    // barrier()                              – scheduler.waitAll()
};
class ScenarioScript {
    ScenarioScript& data(SizeT sig, SizeT n);
    ScenarioScript& descChange(SizeT sig, ...);
    ScenarioScript& gap(SizeT sig, Int diff);
    ScenarioScript& disconnect(SizeT sig);
    ScenarioScript& reconnect(SizeT sig);
    ScenarioScript& barrier();
    void run(MultiRateSignalBank&);
};
```

Used by: B.1 event-ordering tests, B.4 (PartialBlockSilentDiscardE2E, MultipleEventsOneReadEach, DisconnectReconnectRecovers, GapEventDeliveredAndResync), B.6.

### 2.3 `SyncExpectation` helpers

Assertions over read results, both domain-output modes (spec §7.3):

```cpp
// Direct path: convert each signal's domain output to absolute time via ITS OWN
// descriptor and assert cross-signal equality of the first sample (and spacing).
void assertAlignedStart(const MultiRateSignalBank&, void** domainBuffers, SizeT count);

// Resampled path: assert all domain arrays are identical common-grid ticks and
// match OutputTimeline (origin/resolution/rate).
void assertCommonGridDomain(const MultiReaderPtr&, void** domainBuffers, SizeT count);

// Offset contract (spec §7.4): origin + offset*resolution == expected first-sample time.
void assertOffsetContract(const MultiReaderPtr&, const IMultiReaderStatusPtr&,
                          std::chrono::system_clock::time_point expectedFirstSample);
```

Used by: nearly every B.4/B.7 row; replaces today's manual `printData`/`ElementsAreArray` idiom.

### 2.4 `MockResampler` / `MockResamplerBuilder` (`tests/mock_resampler.h`)

Implements the new `IResampler`/`IResamplerBuilder` interfaces (Phase 5):

- `MockResamplerBuilder` records every `build` call (`ResamplerBuildContext` copies, call count, per-input association) and returns scriptable `MockResampler`s; can be configured to reject specific descriptors (RE-11).
- `MockResampler` records every `getRequiredSourceRange`/`resample` request (windows, output ranges) and writes sentinel or scripted values into output buffers; asserts-friendly getters: `windowsSpanningTick(t)`, `buildGeneration()`.
- Verifies: RE-3 (no build for direct-copy input), RE-5 (no window spans an event tick), RE-6/RE-8 (rebuild counts), RE-7 (instance identity), RE-13 (sentinel values reach user buffers).

The real `LinearResampler` is tested against `MultiRateSignalBank` ground truth (RE-1/RE-2/RE-12), not mocks.

### 2.5 `CallbackProbe` (`multi_reader_test_utils.h`)

```cpp
class CallbackProbe {                     // wraps setOnDataAvailable
    SizeT invocationCount() const;
    std::thread::id lastThread() const;
    bool waitForInvocation(std::chrono::milliseconds timeout);   // condition-variable based
    void setBody(std::function<void()>);  // e.g. call read() inside the callback (TH-3)
};
```

Producer-thread detection: the test records the thread id inside `signal.sendPacket` scope (SameThread notification ⇒ `packetReceived` runs there) and asserts `lastThread()` differs (TH-1). Coalescing bound assertions for TH-2. No sleeps — waits on condition variables with generous timeouts.

### 2.6 `StatusMatchers` (gtest matchers, `multi_reader_test_utils.h`)

```cpp
MATCHER: HasState(MultiReaderState)
MATCHER: AffectedInputsAre(std::vector<SizeT>)
MATCHER: StateMessageContains(std::vector<std::string> fragments)   // input names/ids in diagnostics
MATCHER: EventListIs(std::vector<std::pair<SizeT /*inputIdx*/, PacketType /*or event id*/>>)
MATCHER: SameStatusInstanceAs(const IMultiReaderStatusPtr&)   // ST-7 identity check
```

Used by B.8 and every B.4 row asserting states.

### 2.7 Deterministic time for `DataLossMonitor`

`DataLossMonitor` (Phase 4.3) takes an injectable clock:

```cpp
struct TestClock {                        // injected via DataLossMonitor ctor / setClockForTest
    steady_clock::time_point now() const;
    void advance(Duration d);             // fires due deadlines synchronously through
};                                        // the same evaluateState path the scheduler uses
```

All B.6 tests drive `TestClock::advance` — zero real sleeps, deterministic on CI. The production path (scheduler-armed deadline) gets one smoke test with a short real timeout, marked slow.

### 2.8 Unit-suite harnesses for new internals (Phase 2)

`SyncManagerTest` and `ReadCoordinatorTest` operate below `MultiReaderImpl`:

```cpp
class InputHarness {   // per input: real InputPort + real QueueReader fed by a ReadSignal
    InputSlot&   slot();
    QueueReader& queue();
    ReadSignal&  source();
};
class MultiInputHarness {          // N InputHarnesses + SynchronizationManager + ReadCoordinator
    SynchronizationManager& sync();
    ReadCoordinator&        coordinator();
    // convenience: buildModel(), synchronize(), makeBuffers(count) -> jagged arrays
};
```

Real ports/connections/packets (no connection mocks — the openDAQ `Connection` is cheap and behaviorally relevant); determinism comes from `SameThread` notification plus explicit `barrier()`.

---

## 3. Conventions

- **No `std::this_thread::sleep_for` in assertions** — condition-variable waits (`CallbackProbe::waitForInvocation`, `scheduler.waitAll()`, `TestClock`) only. The existing `TearDown` sleep in `ReaderTest` stays as-is.
- **Exact-value assertions** everywhere ground truth is analytic (ramp signals); tolerances only where double conversion is inherent, and then `1 ulp`-scale, not magic epsilons.
- Each test names the spec rule it verifies in a leading comment: `// spec §7.2.6 / EV-7`.
- New files: `tests/multi_reader_test_utils.h/.cpp`, `tests/mock_resampler.h`, `tests/test_multi_reader_threading.cpp`, `tests/test_multi_reader_status.cpp`, `tests/test_data_loss.cpp`, `tests/test_multi_reader_resampling.cpp`, `tests/test_sync_manager.cpp`, `tests/test_read_coordinator.cpp`, `tests/test_multi_reader_error_contract.cpp` — added to `tests/CMakeLists.txt` in their respective phases.

## 4. Build order of scaffolding vs. phases

| Phase (02_implementation_plan) | Scaffolding needed first |
|---|---|
| 0–1 | none new (QueueReaderTest fixture suffices) |
| 2 | §2.8 harnesses, §2.1 bank, §2.2 script, §2.3 expectations, §2.5 probe; `ReadSignal`/fixture extraction (§1 note) |
| 3 | §2.6 matchers |
| 4 | §2.7 test clock |
| 5 | §2.4 mock resampler |
