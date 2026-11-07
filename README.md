# retuner

Static MIDI retuner for the disting NT, targeting a Knobula Monumatic in its
documented Omni/Mono voice-per-channel mode. One input MIDI channel, eight
retuned receiver voices, pitch bend sent before each note-on. Tuning comes
from a Scala `.scl` file, with a built-in 12-TET fallback.

Player documentation: [docs/retuner.md](docs/retuner.md).

## Build

The disting NT API is a git submodule:

```bash
git submodule update --init --recursive
```

Hardware object (ARM, `-fPIC`, no exceptions/RTTI):

```bash
make hardware
make inspect TARGET=hardware
```

Native core and adapter tests:

```bash
make test
```

`make verify` runs the native tests plus the ARM build and object inspection.

The release object is `plugins/retuner.o`; copy it to the module's
`programs/plug-ins/` directory.

## Layout

- `src/retuner_core.h` — host-independent scale/note/voice engine.
- `src/retuner.cpp` — disting NT adapter (`ThRt`, "Retuner") and `.scl` loader.
- `tests/retuner_core_test.cpp` — native core behavioural tests.
- `tests/retuner_adapter_test.cpp` — adapter tests against disting NT API stubs.
- `docs/retuner.md` — player-facing contract and open hardware prerequisites.

## Status

Scala tuning, per-channel voices and host/ARM builds are covered by tests.
Monumatic bend-range semantics and physical device acceptance remain
prerequisites; see the player docs.
