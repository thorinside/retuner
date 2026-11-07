# Retuner — static MIDI retuner for the disting NT

`Retuner` takes a polyphonic chord on one MIDI channel and re-sends each held
key as an independent, retuned receiver voice. Pitch bend is sent **before**
the note-on, so a receiver hears the target frequency immediately. It is a
static microtuner: it does not recognise chords, snap to a scale, or adapt to
the input.

Tuning comes from a Scala `.scl` file in the disting NT `scl` folder. The
plugin also has a built-in 12-TET fallback for the case where no scale has been
selected or the card is not mounted.

The plugin GUID is `ThRt`; the algorithm name is **Retuner**.

## What it is not

- Not a DAW/computer pass-through. The disting NT does the work on the module.
- Not a generic MPE master/member transmitter. It is a per-channel bend
  utility: one voice per MIDI channel 1–8, with no master-channel traffic.
- Not a transparent expressive bridge. Pitch bend, pressure, CC74, pedals,
  program changes, other CCs, SysEx and clock are not forwarded.
- Not a `.kbm` importer. It uses the disting NT automatic keyboard mapping
  described below, with 1/1 fixed at MIDI 60.

## Installation

1. Build or obtain `plugins/retuner.o`.
2. Copy it to the disting NT MicroSD card under `programs/plug-ins/`.
3. Copy the scales you want to use to the card's `scl` folder.
4. Rescan plug-ins on the module and add the `Retuner` algorithm.

The module's normal Scala file library is used, so the same `.scl` files work
with the stock microtuned algorithms.

## Controls

| Parameter | Default | Range | Meaning |
| --- | --- | --- | --- |
| Channel | 1 | 1–16 | The single MIDI channel the retuner listens on. |
| Scale | 0 (first `.scl`) | `.scl` file index | Scala scale loaded from the card. The parameter text shows the file name. Before any file loads, the built-in 12-TET table is used. |
| Bend Range | 0 (unset) | 0–48.00 semitones | The receiver's pitch-bend range `R`, in semitones. Must be set before arming. |
| Arm | Off | Off / On | Arm the retuner. Arming requires a valid table and `R > 0`. |

The plugin starts **disarmed** with `R` unset. Set `Bend Range` to the
receiver's actual range, choose a `Scale`, and set `Arm` to `On`. Changing
`Channel`, `Scale` or `Bend Range` while armed sends a panic and disarms.

Before any `.scl` file has loaded (including when the card is not mounted),
the built-in 12-TET table is used. A loaded scale stays in memory and remains
usable if the card is removed; a failed read invalidates the table and refuses
arming until a valid file loads.

## SCL mapping

The mapping follows the disting NT automatic keyboard map:

- The implicit degree 0 (1/1) is anchored at MIDI note 60.
- Each successive MIDI key advances one scale degree.
- The last pitch value in the `.scl` file is the formal octave (the period).
- The pattern repeats after that number of keys.

So a 12-note scale maps MIDI 60–71 to its first twelve degrees and puts 1/1 of
the next period on MIDI 72. A seven-note scale maps seven keys per period;
black keys are **not** automatically silent or snapped to white keys.

MIDI note 60 is C4 = 261.625565300599 Hz. This is the only absolute pitch
anchor; the `.scl` file supplies the intervals above it. The plugin does not
read a `.kbm` file, reference frequency or alternate root from the card.

If a key's target would be outside the receiver's representable MIDI range
(0–127 plus the configured bend range), that key is intentionally silent. It
is not folded to a neighbour. If a target is inside that range but needs more
bend than `R`, the whole table is rejected rather than flattened.

### Worked 12-note just-intonation example

Assume a 12-note 5-limit `.scl` file and an illustrative `R = 2` (not an
assumed Monumatic value):

| Input | Target | Base | Bend | Bytes |
| --- | --- | --- | --- | --- |
| C4 / 60 | 261.625565301 Hz | 60 | 8192 | `00 40` |
| E4 / 64 | 327.031956626 Hz | 64 | 7631 | `4F 3B` |
| G4 / 67 | 392.438347951 Hz | 67 | 8272 | `50 40` |

The C/E/G chord therefore sends, in order:
`E0 00 40; 90 3C 64; E1 4F 3B; 91 40 64; E2 50 40; 92 43 64`.

## Pitch-bend bytes

For a target pitch `p`, the receiver base note is
`b = clamp(floor(p + 0.5), 0, 127)` and the bend is
`q = 8192 + roundHalfAwayFromZero(8192 * (p - b) / R)`, limited to
0..16383 only for the representable positive endpoint. Bend is sent LSB
first. Interior rounding error is at most `100R/16384` cents.

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
- **Overflow, scale change or other configuration change:** hard panic.

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
as part of this implementation. Treat the plugin as a researched, tested-on-host
implementation until that evidence exists.
