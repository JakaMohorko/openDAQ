# Multi Reader Rework — Decision Log

Running log of architecture decisions for the multi reader rebuild. Built up incrementally
from the skeleton state (branch `refactor/multi-reader-skeleton`); newest decisions appended.

## Current state (baseline)

- Multi reader gutted to a construction skeleton: constructors, factories and slot wiring work; every interface method is a blank stub.
- Kept fully functional: `multi_reader::QueueReader`, `multi_reader::Input` (+ `CallbackGate` dependency) and their tests.
- Deleted: state machine, synchronization manager, read/notification coordinators, data-loss monitor, behavior tests, benchmark.

## Decisions

### D1 — No already-connected ports at reader creation (2026-08-25)

- Readers (and `addInput` in the future) only accept unconnected ports; connections must arrive after the slots listen.
- Function blocks must create the multi reader during construction, never later.
- Input ports are created on user interaction (property write / IP connect) and must not be connected before that call finishes; this is a contract on FB authors, enforced by lock scope.
- Consequence: `Input::replayMissedPortCallbacks` removed; signal-built readers connect their ports only after `createSlots` installs the listeners.
- TODO: throw in `createOrAdoptPorts` (adopt branch) and future `addInput` when a port is already connected.

### D2 — Remove the from-existing constructor (2026-08-25)

- `MultiReaderFromExisting` is removed; it was the only consumer of adopted-connected-port semantics.
- The legacy list (non-builder) constructor stays for now but is scheduled for deprecation; it already delegates to the builder path.
- The multi reader is created exactly once per function block; it can never become "unrecoverable" except on critical failure, in which case the function block itself is re-created.
- Consequence for the new state machine: per-input failure states instead of whole-reader invalidation; `invalid` reserved for `dispose()`/`markAsInvalid()`.
- TODO: deprecation shim for the exported `createMultiReaderFromExisting` symbol (bindings expose it); decide whether stream/block/tail from-existing variants follow.

## Open questions

- Enforcement: hard throw vs. documented contract for connected-port violations.
- `setExternalListener` forwarding must return in the reimplementation (FB's only `acceptsSignal` veto point).
- Fate of `addInput`/`removeInput` in the new architecture.
