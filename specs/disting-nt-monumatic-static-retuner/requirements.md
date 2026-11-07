# disting NT → Knobula Monumatic: static MIDI retuner MVP

## 1. Outcome, status and authority
An OXI sequencer sends a polyphonic chord on one MIDI channel. A disting NT utility maps each admitted input pitch to a fixed target frequency, assigns an independent receiver voice channel, and emits pitch bend BEFORE note-on. No DAW/computer is needed during performance. This is static microtuning, not nearest-scale quantization, chord recognition or adaptive just intonation.

This is a RESEARCH-BACKED DRAFT, not an accepted implementation contract or tested device. Neal authorized research/specification only. No code, builds, automated plugin tests, hardware, firmware/preset changes, publication or deployment were performed. Screenshot participants/devices are not assumed to belong to Neal. Repository remains unassigned: existing OXI Companion supplied read-only SDK evidence; its completed Specs and code are not repurposed. Implementation, repository binding and hardware operations require later authority. Readiness/acceptance remain false/unset.

## 2. Evidence and feasibility
Authoritative research report:96e9edfb-ab6a-4e14-8e94-a8d4a239d052; detailed citations/calculations/source catalogue:3ce0529a-3afc-4747-afb7-2f46c058107c. Both are linked evidence. Labels below distinguish documented facts from proposed requirements.

**Conditional feasibility:** NT exposes the required native receive/send primitives. Exact Monumatic documentation establishes per-channel pitch bending, but does NOT establish the bend range needed to generate correct frequencies. No physical compatibility claim is made.

Critical receiver fact [M1]: Monumatic Manual v1.1.4 printed p4/PDF p5 specifies Omni MIDI reception plus **Mono mode**, one voice per channel **1–8**, with independent bend/CC74/aftertouch. Therefore this MVP is a **Monumatic-specific per-note channel/bend profile**, not a claim of standard MPE lower/upper-zone compliance. Channel1 is a voice, NOT a master; reserve channels1–8 and send no zone-master or Configuration/Sensitivity RPN traffic. Normal Poly mode or channels2–16 are not substituted. Eight documented voices, not the MIDI channel ceiling, bounds allocation.

CC22 is named Pitch Bend Range, but its values, units, default, scope/persistence and RPN support are undocumented. The official public folder lists firmware1.1.5 while the correspondingly named changelog repeats1.1.4 headings; installed firmware is unknown. These are explicit setup gaps, not permission to assume ±2/±48, borrow Poly Cinematic settings, factory-reset, calibrate or upgrade the receiver.

Initial software target [S1,N1]: NT firmware1.19.0 with C++ API14. Research inspected SDK79c0cba15ce7cb2b82ff91bcd288b5d7c02e8c6a; the versioned1.19 SDK6975a630cb5f86de6f8c709f5467f4666bbd4579 already has the needed short-MIDI functions. Native MIDI conversion features date to firmware1.9.0; that is a feature floor, NOT compatibility of a new API14 binary with1.9. Optional Scala API starts1.14. Large-SysEx support is unnecessary.

## 3. Smallest design and exclusions
Choose a small independent native C++ note retuner with fixed-size state and immutable128-note target tables. Native receive/realtime callbacks support a defined Stop/Reset policy [S1]. Lua is a smaller notes-only alternative, but the examined Lua1.19 API does not document realtime Stop/Reset hooks or a native Scala/KBM bridge [L1]. Do not add a generic platform/framework.

MVP tables: (a)12-TET identity, default; (b) clearly named **Fixed C-major JI, white keys only**. Each128-entry slot is mapped to an absolute target pitch or explicitly unmapped. No tuning editor, arbitrary file import, adaptive tuning, modulation passthrough, pedals, voice stealing, seamless tails, generic MPE output or USB-host support. Later custom maps are a separate feature.

Native NT_readScl reuses firmware parsing, but exposes scale entries rather than a complete MIDI-key→Hz mapper; no examined public .kbm/global-map access establishes wholesale reuse. Therefore no new Scala/KBM parser or import ecosystem in MVP [S1,N2].

Alternatives: stock Poly CV documents microtuned CV and one-channel MIDI chord output, not independent output-bend allocation; MIDI Filter filters/remaps rather than allocates notes. Router v0.5.2 copies messages to configured channels without a tuning allocator. No ready-made solution was verified. Monumatic's internal Hermode/JI may be enough if adaptive consonance alone is wanted; it does not provide arbitrary external static targets. These are evidence limits, not universal impossibility claims.

## 4. Setup, ports and operational boundary
Baseline: OXI MIDI OUT → NT breakout MIDI IN → this utility → NT breakout MIDI OUT → Monumatic MIDI IN. Use the correct physical breakout/adapters. Original OXI ONE manual5.6 and MkII docs describe TRS-A; use standard output, not original ONE Split encoding, MkII channel multiplexing or separately channelled bass. Actual OXI model/firmware must be recorded before a device claim. One selected input channel, default1, configurable1–16.

NT front audio/CV jacks are NOT MIDI. NT breakout wiring is configurable; do not assume TRS polarity. NT USB is device-only [N2]; two device-mode ports and a powered hub do not create a host. OXI host modes are documented but exact OXI→NT USB interoperability is not verified. USB/Select Bus/Internal output are excluded; send destination mask is breakout=1 only.

NT callbacks observe rather than consume MIDI and carry no source-port identity [S1,N2]. Disable raw breakout→breakout and all other dry routes into Monumatic; exclude competing MIDI sources, receiver-output algorithms, generated mapped-CC output and physical feedback. Plugin channel filtering cannot distinguish the same channel arriving at different inputs. Internal output must remain disabled. Host tests do not prove the physical routing is isolated.

Receiver setup prerequisite: actual Monumatic firmware recorded, documented Omni/Mono profile, ordinary ET base-note tuning, repeatable matching bend range R, internal temperament/octave stretching fully off [M1p16], no chord playback or external pitch/glide/modulation interfering with the frequency check. The utility must default DISARMED with R unset; arming requires a validated configured R>0 and a validated table. Do not automatically send CC22/RPN or other speculative setup messages.

## 5. Tuning mapping and bytes
Coordinates use MIDI69=440Hz: p=69+12log2(f/440), f=440·2^((p−69)/12). Identity p(n)=n.

Fixed C-major JI anchors note60 to C4=440·2^(−9/12)=261.625565300599Hz. C,D,E,F,G,A,B use ratios1,9/8,5/4,4/3,3/2,5/3,15/8, repeated by2/1 at octave boundaries. Accidentals are unmapped/silent, NOT snapped to white keys. Root stays C; no chord-dependent root inference. Here A4 is436.042608834Hz: A440 is the ET coordinate used to establish C, not a second JI anchor.

For a mapped target p: b=clamp(floor(p+0.5),0,127); d=p−b. Validate finite positive R and |d|≤R. q=8192+roundHalfAwayFromZero(8192·d/R). Limit q to0..16383 ONLY for representable endpoint rounding. Reject invalid/unrepresentable tables/configuration instead of silently flattening requested pitches. Reconstructed p=b+R·(q−8192)/8192. Interior rounding error≤100R/16384 cents; full-scale positive endpoint may err by100R/8192 cents. Receiver response calibration is separately unverified.

Worked examples at **illustrative R=2 only, NOT an assumed Monumatic setting**:
- C4/input60:261.625565301Hz, b60,q8192, bend bytes00 40.
- JI E4/input64:327.031956626Hz, p63.863137139,−13.686286135c from ET,b64,q7631, bytes4F 3B, quantization error−0.010002927c.
- JI G4/input67:392.438347951Hz,p67.019550009,+1.955000865c,b67,q8272, bytes50 40,error−0.001875865c.
- p=−2 at R2→base0/q0; p=129→base127/q16383 (−0.0244140625c error); p129.1 is rejected. Input61 in the JI map produces no note or bend.

MIDI status channel nibbles0–7 correspond to displayed channels1–8; bend LSB first. Expected full-status admission at velocity100:
`E0 00 40; 90 3C 64; E1 4F 3B; 91 40 64; E2 50 40; 92 43 64`.
Every admission, including reuse, sends its required bend before its note-on. Store output base/channel; an input off64 after the trace emits `81 40 vv`, not an off on original input channel1. Synthetic remap p(60)=64.25 at R2: `E0 00 48; 90 40 64`; its input off60 must emit `80 40 vv`.

## 6. Voice state and minimal lifecycle
Fixed128 key records contain bounded held-count and assignment/dropped marker. Fixed8 voice records store input key, emitted base/channel/bend and table identity. Process messages in delivered order; choose lowest free receiver channel.
- Distinct admitted mapped key consumes one voice. Preserve first velocity. Repeated same key increments a16-bit count, without another note/retrigger; final matching off alone releases it. Independent duplicate articulations are intentionally coalesced, not promised. Velocity-zero note-on is off; stray off has no effect.
- When eight channels are busy, drop a new distinct key and count its instances until all matching offs arrive. Never admit that key mid-gesture when capacity frees. Unmapped keys use the same silent tracking. A dropped/unmapped off cannot kill another key. At count65535, another on triggers hard panic and a disarmed fault rather than wrapping.
- Final admitted off uses stored output note/channel and final release velocity, then frees the slot. Leave the bend unchanged during ordinary release; the next admission sets its own bend. Reuse may retune/cut an old receiver tail: eight held voices does not mean extra independently bendable tails.
- Ignore input bend (no composition), channel/poly pressure, CC74, CC64/66 pedals, program changes, other CCs, SysEx, Clock and Active Sensing. Do NOT forward them. This is not a transparent expressive bridge. In particular do not pass receiver calibration/disable/mode CC23/24/106/119 [M1]. Pedal support can be added separately if wanted.
- Selected-input-channel CC123 is graceful all-notes-off: emit explicit stored offs and clear all counts/dropped state; retain bends for normal tails. Do not rely on undocumented receiver CC123 behavior.
- Hard Panic for selected CC120/121, explicit Panic/Disable, delivered realtime Stop(FC)/Reset(FF), overflow or reconfiguration: emit stored offs, CC64=0 and CC120=0 across reserved channels1–8, then center bends; clear state and DISARM. This is a proposed kill sequence whose exact receiver effect must be checked, not a guarantee from local state clearing. Never forward CC121 as a receiver reset. Start/Continue do not replay notes or automatically rearm. User rearms with the source stopped/quiescent; old note-offs after state reset cannot be unambiguously paired with a new same-key gesture.
- Table/R/channel/routing changes require DISARMED state, a panic boundary and complete validation; no held-note retuning. Do not send MIDI from draw/customUi. MIDI state must have one serialized owner; callback threading/serialization was not established by public declarations. Resolve target-firmware callback integration before implementation; do not claim RT safety from fixed arrays alone or add a speculative universal concurrency framework. No allocation, file I/O or unbounded work in MIDI/audio callbacks.
- DIN unplug cannot be detected by this API. Do not kill a legitimate long chord using an inactivity timeout. Disconnect/restart requires owner panic/rearm with the source stopped. No documented unload/global-bypass cleanup hook guarantees output; explicitly panic before such transitions. Crash/power loss/disconnected output may require external receiver panic. No generic stuck-note-free physical guarantee.

## 7. Ordering, bandwidth and resource claims
SDK sends return void; no examined public queue capacity, admission/backpressure, timestamp, wire-completion, concurrency or FIFO guarantee exists. Host acceptance checks intended SDK-call order. A future wire/device check must confirm bend-before-on and no dry route; do not claim sample-accurate chords.

DIN31250bit/s with10bits/byte is0.32ms/byte. Eight3-byte bends plus eight3-byte note-ons take48bytes/15.36ms, first-to-eighth note-completion skew13.44ms before competing traffic. Running status is not assumed. Panic is also serialized, not instantaneous. Bounded128-key/8-voice state and immutable tables, no import/background tasks, are the MVP resource design; implementation measurements remain future work, no invented hard CPU budget.

## 8. Focused future acceptance plan — NOT executed
Use an SDK-call recorder/pure host core and controlled events; no broad repository test gate.
AC1: identity all MIDI keys; illustrative JI values/bytes above; endpoints/unmapped/invalid R/NaN or unrepresentable target fail as specified. Expected q/error agrees with formula; no silent target clipping.
AC2: C/E/G overlap gives independent channels1/2/3 and exact trace; changing/releasing one never retunes or terminates another held key. Reused channel gets new bend before on.
AC3: repeated same-key counts/coalescing, velocity0, stray/remapped offs,8th/9th key/drop-then-off and count-overflow fault match section6; no stuck host-held state or cross-key off.
AC4: excluded controllers emit no output; CC123 and hard Panic/Stop/Reset/configuration disarm produce defined calls/state. No ordinary-release bend reset. No automatic replay/rearm or silence timeout.
AC5: routing/config validation enforces one selected input channel/breakout-only output; API source-port blindness and missing unload/disconnect delivery are documented, not faked by host simulation.
AC6: after separate authority, the ACTUAL DEVICE OWNER records OXI model/firmware/standard output, NT1.19 setup/breakout wiring/no dry route, and Monumatic actual firmware/Omni+Mono/temperament-off/ET baseline. On Monumatic establish one repeatable R from documented/manual setting or measured center/min/max bends, without calibration/factory-reset/CC-disable probes. Verify two simultaneously held channels INCLUDING1 bend independently, then all8 voices; note-off/CC120/hold-off, retrigger/tails and stop/panic/reconnect behavior. Capture one chord's bend-before-on and absence of duplicate dry notes. Compare C4/E4/G4 identity versus JI frequencies with section5 targets; record measured baseline/tuner uncertainty and deviation. A tuning pass must agree within quantization plus measured baseline/measurement uncertainty; inconclusive measurements are not a pass. This is physical acceptance on those devices, not performed or claimed here.

## 9. Remaining prerequisites and next action
Primary enabling gap: Monumatic bend-range numeric semantics and actual receiver firmware/independent-channel behavior. Needed next evidence: its device owner supplies actual firmware and a repeatable configured/measured R, then the short two-channel/eight-note/panic check after authorization. No request to buy/flash/reset anything. Formal MPE compliance is not inferred from marketing.

Implementation integration question: establish target callback serialization and safe allowed-context control/Panic handling; verify actual output sequencing on the intended route. These remain explicit draft decisions, not a reason for speculative infrastructure. No further implementation or hardware activity is authorized by this draft. Research and drafting can finish with conditional feasibility; implementation readiness cannot.

## 10. Primary references
[S1] https://github.com/expertsleepersltd/distingNT_API/tree/79c0cba15ce7cb2b82ff91bcd288b5d7c02e8c6a — include/distingnt/api.h31–51,108–138,450–491,649–658; include/distingnt/microtuning.h32–101; examples/microtuningDemo.cpp40–90,117–136.
[N1] https://expert-sleepers.co.uk/distingNTfirmwareupdates.html —1.9,1.14,1.19/API14 history.
[N2] https://expert-sleepers.co.uk/downloads/manuals/disting_NT_user_manual_1.19.pdf —pp13–14,21–23,53–54,59,61–64,76,222–223,261–264.
[L1] https://expert-sleepers.co.uk/downloads/manuals/disting_NT_lua_scripting_1.19.pdf —pp6,15,28.
[M1] https://www.knobula.com/downloads → public Drive Monumatic/Docs; manual1.1.4 https://drive.google.com/uc?export=download&id=1UGty8EshRJmAav8yHmSoVT5cFBYA7OiL —printed pp4,16,20/PDF pp5,17,21. Binary SHA256181a0d3c44cdeee0d132767df81237d0dbbeaf2a84c01c53dbad177a6106618f. Parent clean-web fetch returned unsupported binary content; source researcher retrieved/extracted it through the official chain, with evidence/quotes preserved in annex3ce0529a. Do not substitute Poly Cinematic.
[O1/O2] https://oxiinstruments.com/support ; original manual5.6 https://drive.google.com/uc?export=download&id=18S9YaR88RzGbJc3eaflH6dNYpyFURrpj —pp15,54,120,123; MkII https://docs.oxiinstruments.com/oxi-one-mkii-manual/ —chord-mode6.7/connectivity.
[R1] https://github.com/thorinside/nt-midi-router/blob/1567d7e3cc600dc298dcad2c86a647807c76aae4/nt_midi_router.cpp —228–285; v0.5.2.
[MP] https://midi.org/midi-polyphonic-expression-mpe-specification-adopted —generic standard context only; full normative specification was not retrieved. This draft does not certify a standard MPE zone.