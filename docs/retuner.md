# Retuner — static MIDI retuner for the disting NT

`Retuner` takes a polyphonic chord on one MIDI channel and re-sends each held
key as an independent, retuned receiver voice. Pitch bend is sent **before**
the note-on, so a receiver hears the target frequency immediately. It is a
static microtuner: it does not recognise chords, snap to a scale, or adapt to
the input.

The plugin GUID is `ThRt`; the algorithm name is **Retuner**.

## What it is not

- Not a DAW/computer pass-through. The disting NT does the work on the module.
- Not a generic MPE master/member zone. It is a Monumatic-specific profile:
  one voice per MIDI channel 1–8, no master-channel traffic.
- Not a transparent expressive bridge. Pitch bend, pressure, CC74, pedals,
  program changes, other CCs, SysEx and clock are not forwarded.
- Not a Scala/KBM importer. The two tuning tables are fixed.

## Installation

1. Build or obtain `plugins/retuner.o`.
2. Copy it to the disting NT MicroSD card under `programs/plug-ins/`.
3. Rescan plug-ins on the module and add the `Retuner` algorithm.

## Controls

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| Channel | 1 | 1–16 | The single MIDI channel the retuner listens on. |
| Tuning | 12-TET | 12-TET / JI C | Fixed 12-tone equal temperament or fixed C-major just intonation. |
| Bend Range | 0 (unset) | 0–48.00 semitones | The receiver's pitch-bend range `R`, in semitones. Must be set before arming. |
| Arm | Off | Off / On | Arm the retuner. Arming requires a valid table and `R > 0`. |

The plugin starts **disarmed** with `R` unset. Set `Bend Range` to the
receiver's actual range and set `Arm` to `On`. Changing `Channel`, `Tuning`
or `Bend Range` while armed sends a panic and disarms.

## Tuning tables

### 12-TET (default)

MIDI key `n` maps to pitch `n`; every key is mapped. The receiver's own
equal-tempered tuning must be active and its internal temperament/octave
stretching off.

### Fixed C-major JI

Middle C (MIDI 60) is anchored to C4 = 261.625565300599 Hz. The white keys
use the ratios 1, 9/8, 5/4, 4/3, 3/2, 5/3, 15/8, repeated by 2/1 at octave
boundaries. Black keys are **unmapped and silent**, not snapped to white
keys. The root stays C; there is no chord-dependent root inference. A4 is
436.042608834 Hz in this table; A440 is only the equal-tempered coordinate
used to establish C4.

If a mapped key cannot be represented within the configured `R`, the table
is rejected and the plugin will not arm. It does not silently flatten the
pitch.

## Pitch-bend bytes

For a target pitch `p`, the receiver base note is
`b = clamp(floor(p + 0.5), 0, 127)` and the bend is
`q = 8192 + roundHalfAwayFromZero(8192 * (p - b) / R)`, limited to
0..16383 only for the representable positive endpoint. Bend is sent LSB
first. Interior rounding error is at most `100R/16384` cents.

Worked examples at an illustrative `R = 2` (not an assumed Monumatic value):

| Input | Target | Base | Bend | Bytes |
| --- | --- | --- | --- | --- |
| C4 / 60 | 261.625565301 Hz | 60 | 8192 | `00 40` |
| JI E4 / 64 | 327.031956626 Hz | 64 | 7631 | `4F 3B` |
| JI G4 / 67 | 392.438347951 Hz | 67 | 8272 | `50 40` |

The C/E/G chord therefore sends, in order:
`E0 00 40; 90 3C 64; E1 4F 3B; 91 40 64; E2 50 40; 92 43 64`.

## Voice behaviour

- One selected input channel; eight reserved receiver channels 1–8. Channel 1
  is a voice, not a master.
- A distinct admitted key consumes one receiver voice. The first velocity is
  kept. Repeated note-ons for the same key increment a held count and do not
  retrigger.
- The final matching note-off releases the voice, using the stored output
  note/channel and the final release velocity. The bend is left unchanged
  during an ordinary release; the next admission sets its own bend.
- If all eight voices are busy, a new distinct key is dropped and tracked
  silently until all its note-offs arrive. It is not admitted mid-gesture when
  capacity frees.
- Unmapped keys are tracked silently. A dropped or unmapped note-off cannot
  release another key.
- At a held count of 65535, another note-on triggers a hard panic and a
  disarmed `overflow` fault rather than wrapping.

## Controllers and panic

- **Ignored:** pitch bend, channel/poly pressure, CC74, CC64/CC66 pedals,
  program changes, other CCs, SysEx, clock and active sensing. Nothing is
  forwarded.
- **CC123** on the selected channel: graceful all-notes-off. Sends stored
  note-offs, clears held counts, leaves bends alone.
- **CC120/CC121** on the selected channel: hard panic. Sends stored note-offs,
  CC64 = 0 and CC120 = 0 on channels 1–8, centres the eight bends, clears
  state and disarms. CC121 is never forwarded as a receiver reset.
- **Realtime Stop (0xFC) / System Reset (0xFF):** hard panic. Start/Continue do
  not replay notes or rearm.
- **Overflow or configuration change:** hard panic.

Panic is a proposed kill sequence whose exact receiver effect must be checked
on hardware. The plugin cannot detect a DIN unplug; it does not use an
inactivity timeout. Panic/rearm from the owner with the source stopped.

## Routing

- MIDI breakout only. USB, Select Bus and Internal destinations are not used.
- The disting NT callbacks observe MIDI and carry no source-port identity.
  Disable raw breakout → breakout and every other dry route into the receiver;
  exclude competing MIDI sources, receiver-output algorithms and generated
  mapped-CC output. Host tests cannot prove the physical route is isolated.
- The plugin sends no audio and no audio bus.

## Compatibility and open prerequisites

- Built against disting NT C++ API v14, targeting firmware 1.19.0. Native MIDI
  conversion features date to firmware 1.9.0; that is a feature floor, not a
  claim that this API v14 binary runs on 1.9.
- The receiver must be a Knobula Monumatic in its documented Omni/Mono mode
  (one voice per channel 1–8). Its actual firmware, ordinary equal-tempered
  base tuning and internal temperament/octave stretching off must be recorded
  first.
- `Bend Range` is the receiver's pitch-bend range `R`. Monumatic's CC22 value
  semantics are undocumented; measure or read the actual range. Do not assume
  ±2 or ±48 and do not borrow Poly Cinematic settings.
- The plugin does not send CC22/RPN or other speculative setup messages.
- The public API does not document callback serialisation or a guaranteed
  unload/global-bypass cleanup hook. The plugin assumes the host serialises its
  MIDI/parameter callbacks; verify this on the target firmware before a
  device-compatibility claim. Panic explicitly before unload or bypass where
  the host offers such a transition.
- Crash, power loss or a disconnected output may need an external receiver
  panic. There is no generic stuck-note-free physical guarantee.

## Evidence limits

SDK MIDI sends return `void`; the public API documents no queue capacity,
backpressure, timestamp, wire-completion or FIFO guarantee. The plugin checks
the intended SDK-call order. A future wire/device check must confirm
bend-before-on and the absence of a duplicate dry route. DIN 31250 bit/s at
10 bits/byte is 0.32 ms/byte; eight 3-byte bends plus eight 3-byte note-ons
take 48 bytes/15.36 ms, with a 13.44 ms first-to-eighth note-completion skew
before competing traffic. Chords are not sample-accurate.

Physical acceptance on the actual OXI/NT/Monumatic devices was not performed
as part of this draft. Treat the plugin as a researched, tested-on-host
implementation until that evidence exists.
