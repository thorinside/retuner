# retuner

Static MIDI retuner for the disting NT, targeting a Knobula Monumatic in its
documented Omni/Mono voice-per-channel mode. One input MIDI channel, eight
retuned receiver voices, pitch bend sent before each note-on.

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

Native core tests:

```bash
make test
```

`make verify` runs the native tests plus the ARM build and object inspection.

The release object is `plugins/retuner.o`; copy it to the module's
`programs/plug-ins/` directory.

## Layout

- `src/retuner_core.h` — host-independent note/tuning/voice engine.
- `src/retuner.cpp` — disting NT adapter (`ThRt`, "Retuner").
- `tests/retuner_core_test.cpp` — native behavioural tests.
- `docs/retuner.md` — player-facing contract and open hardware prerequisites.

## Status

Research-backed draft implementation. Monumatic bend-range semantics and
physical device acceptance remain prerequisites; see the player docs.
