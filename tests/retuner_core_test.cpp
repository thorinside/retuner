/*
 * Native behavioural tests for the retuner core (AC1-AC4 of the spec).
 * These run without the disting NT API; retuner.cpp is the adapter.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "retuner_core.h"

namespace {

struct Message
{
	uint8_t b0, b1, b2;
};

struct Recorder
{
	std::vector<Message> messages;

	static void send(void* ctx, uint8_t b0, uint8_t b1, uint8_t b2)
	{
		static_cast<Recorder*>(ctx)->messages.push_back(Message{ b0, b1, b2 });
	}

	retuner::MidiOut out()
	{
		return retuner::MidiOut{ this, send };
	}

	void clear() { messages.clear(); }

	bool only(uint8_t b0, uint8_t b1, uint8_t b2) const
	{
		return messages.size() == 1 && messages[0].b0 == b0 && messages[0].b1 == b1 && messages[0].b2 == b2;
	}

	bool empty() const { return messages.empty(); }
};

int failures = 0;

void check(bool ok, const char* what)
{
	if (!ok)
	{
		++failures;
		std::printf("FAIL: %s\n", what);
	}
}

void checkMessage(const Message& m, uint8_t b0, uint8_t b1, uint8_t b2, const char* what)
{
	check(m.b0 == b0 && m.b1 == b1 && m.b2 == b2, what);
}

/* Build a repeating scale from SCL-style ratios; the last ratio is the period. */
retuner::Scale scaleFromRatios(const int ratios[][2], int count)
{
	retuner::Scale scale;
	scale.count = (uint8_t)count;
	for (int i = 0; i < count; ++i)
		scale.interval[i] = std::log((double)ratios[i][0] / ratios[i][1]) / std::log(2.0);
	return scale;
}

retuner::Scale justC7Scale()
{
	static const int ratios[7][2] = {
		{ 9, 8 }, { 5, 4 }, { 4, 3 }, { 3, 2 }, { 5, 3 }, { 15, 8 }, { 2, 1 }
	};
	return scaleFromRatios(ratios, 7);
}

retuner::Scale justC12Scale()
{
	static const int ratios[12][2] = {
		{ 16, 15 }, { 9, 8 }, { 6, 5 }, { 5, 4 }, { 4, 3 }, { 45, 32 },
		{ 3, 2 }, { 8, 5 }, { 5, 3 }, { 9, 5 }, { 15, 8 }, { 2, 1 }
	};
	return scaleFromRatios(ratios, 12);
}

/* AC1: pitch -> base note and 14-bit bend. */
void testMakeTarget()
{
	retuner::Target t;

	check(retuner::makeTarget(60.0, 2.0, t) && t.note == 60 && t.bend == 8192, "identity C4");
	check(retuner::makeTarget(63.863137138648, 2.0, t) && t.note == 64 && t.bend == 7631, "JI E4");
	check(retuner::makeTarget(67.019550008653, 2.0, t) && t.note == 67 && t.bend == 8272, "JI G4");
	check(retuner::makeTarget(-2.0, 2.0, t) && t.note == 0 && t.bend == 0, "low clamp");
	check(retuner::makeTarget(129.0, 2.0, t) && t.note == 127 && t.bend == 16383, "high clamp");
	check(!retuner::makeTarget(129.1, 2.0, t), "beyond bend range");
	check(!retuner::makeTarget(60.0, 0.0, t), "unset bend range");
	check(!retuner::makeTarget(60.0, -2.0, t), "negative bend range");
	check(!retuner::makeTarget(std::nan(""), 2.0, t), "NaN pitch");
	check(!retuner::makeTarget(60.0, std::nan(""), t), "NaN bend range");
}

/* AC1: 1/1 is MIDI 60 and each key advances one scale degree. */
void testPitchFor()
{
	double p;
	const retuner::Scale et = retuner::equalTemperamentScale();
	check(retuner::pitchFor(et, 0, p) && p == 0.0, "12-TET bottom");
	check(retuner::pitchFor(et, 61, p) && p == 61.0, "12-TET identity");
	check(retuner::pitchFor(et, 127, p) && p == 127.0, "12-TET top");

	const retuner::Scale ji = justC7Scale();
	check(retuner::pitchFor(ji, 60, p) && std::fabs(p - 60.0) < 1e-9, "JI 1/1 on C4");
	check(retuner::pitchFor(ji, 61, p) && std::fabs(p - (60.0 + 12.0 * std::log2(9.0 / 8.0))) < 1e-9, "JI key 61 is 9/8");
	check(retuner::pitchFor(ji, 62, p) && std::fabs(p - (60.0 + 12.0 * std::log2(5.0 / 4.0))) < 1e-9, "JI key 62 is 5/4");
	check(retuner::pitchFor(ji, 66, p) && std::fabs(p - (60.0 + 12.0 * std::log2(15.0 / 8.0))) < 1e-9, "JI key 66 is 15/8");
	check(retuner::pitchFor(ji, 67, p) && std::fabs(p - 72.0) < 1e-9, "JI period repeats at key 67");
	check(retuner::pitchFor(ji, 59, p) && std::fabs(p - (48.0 + 12.0 * std::log2(15.0 / 8.0))) < 1e-9, "JI period repeats below C4");
}

/* AC1/AC5: table validation and arming. */
void testArmAndTable()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());

	check(engine.configureScale(retuner::equalTemperamentScale(), 0.0), "configure unset");
	check(!engine.isTableValid(), "unset table invalid");
	check(engine.tableError() == retuner::kTableBendUnset, "unset error");
	check(!engine.arm(), "cannot arm unset table");

	check(engine.configureScale(retuner::equalTemperamentScale(), 2.0), "configure 12-TET");
	check(engine.isTableValid(), "12-TET table valid");
	check(engine.target(60).mapped && engine.target(60).bend == 8192, "12-TET C4");
	for (int key = 0; key < 128; ++key)
	{
		check(engine.target(key).mapped, "12-TET all keys mapped");
		check(engine.target(key).note == key, "12-TET base note");
		check(engine.target(key).bend == 8192, "12-TET centred");
	}
	check(engine.arm(), "arm 12-TET");
	check(engine.isArmed(), "armed");
	engine.disarm();
	check(!engine.isArmed(), "disarmed");

	check(engine.configureScale(justC12Scale(), 2.0), "configure JI");
	check(engine.isTableValid(), "JI table valid at R=2");
	check(engine.target(61).mapped, "12-note JI maps every key");
	check(engine.target(64).bend == 7631, "JI E4 bend");

	retuner::Scale empty;
	check(engine.configureScale(empty, 2.0), "configure empty scale");
	check(!engine.isTableValid(), "empty scale invalid");
	check(engine.tableError() == retuner::kTableScaleInvalid, "empty scale error");

	check(engine.configureScale(justC12Scale(), 0.01), "configure JI tight");
	check(!engine.isTableValid(), "JI table invalid at R=0.01");
	check(engine.tableError() == retuner::kTableBendTooSmall, "tight error");

	/* Non-finite R must be rejected, not silently treated as a huge range. */
	check(engine.configureScale(justC12Scale(), HUGE_VAL), "configure infinite R");
	check(!engine.isTableValid(), "infinite table invalid");
	check(engine.tableError() == retuner::kTableBendInvalid, "infinite error");
	check(!engine.arm(), "cannot arm infinite range");
}

/* AC2: independent voices and bend-before-on. */
void testVoiceAllocation()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	engine.processChannelMessage(0x90, 64, 100);
	engine.processChannelMessage(0x90, 67, 100);

	check(rec.messages.size() == 6, "three bend+on pairs");
	checkMessage(rec.messages[0], 0xE0, 0x00, 0x40, "voice 0 bend");
	checkMessage(rec.messages[1], 0x90, 0x3C, 0x64, "voice 0 on");
	checkMessage(rec.messages[2], 0xE1, 0x00, 0x40, "voice 1 bend");
	checkMessage(rec.messages[3], 0x91, 0x40, 0x64, "voice 1 on");
	checkMessage(rec.messages[4], 0xE2, 0x00, 0x40, "voice 2 bend");
	checkMessage(rec.messages[5], 0x92, 0x43, 0x64, "voice 2 on");

	check(engine.voicesInUse() == 3, "three voices in use");

	/* Releasing E4 must not retune or terminate C4/G4. */
	rec.clear();
	engine.processChannelMessage(0x80, 64, 0);
	check(rec.only(0x81, 0x40, 0x00), "only E4 off");
	check(engine.voicesInUse() == 2, "two voices in use");

	/* Reuse the freed voice: a fresh bend precedes the new note-on. */
	rec.clear();
	engine.processChannelMessage(0x90, 72, 100);
	check(rec.messages.size() == 2, "reuse sends bend then on");
	checkMessage(rec.messages[0], 0xE1, 0x00, 0x40, "reused bend");
	checkMessage(rec.messages[1], 0x91, 0x48, 0x64, "reused on");
}

/* AC2: JI C/E/G overlap keeps independent, non-centred bends. */
void testJustIntonationOverlap()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(justC12Scale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	engine.processChannelMessage(0x90, 64, 100);
	engine.processChannelMessage(0x90, 67, 100);

	check(rec.messages.size() == 6, "JI C/E/G sends three pairs");
	checkMessage(rec.messages[0], 0xE0, 0x00, 0x40, "JI C4 bend");
	checkMessage(rec.messages[1], 0x90, 0x3C, 0x64, "JI C4 on");
	checkMessage(rec.messages[2], 0xE1, 0x4F, 0x3B, "JI E4 bend");
	checkMessage(rec.messages[3], 0x91, 0x40, 0x64, "JI E4 on");
	checkMessage(rec.messages[4], 0xE2, 0x50, 0x40, "JI G4 bend");
	checkMessage(rec.messages[5], 0x92, 0x43, 0x64, "JI G4 on");

	/* Releasing E4 must not retune or terminate C4/G4. */
	rec.clear();
	engine.processChannelMessage(0x80, 64, 0);
	check(rec.only(0x81, 0x40, 0x00), "only JI E4 off");
	check(engine.voicesInUse() == 2, "JI two voices remain");

	/* Reuse the freed voice with a different, non-centred bend. */
	rec.clear();
	engine.processChannelMessage(0x90, 69, 100);
	check(rec.messages.size() == 2, "JI reuse sends bend then on");
	checkMessage(rec.messages[0], 0xE1, 0x7F, 0x3A, "JI A4 reuse bend");
	checkMessage(rec.messages[1], 0x91, 0x45, 0x64, "JI A4 reuse on");
}

/* AC2: a retuned voice sends its own bend before the note-on. */
void testBendBeforeOn()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(justC12Scale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 64, 100);
	check(rec.messages.size() == 2, "JI E4 sends bend+on");
	checkMessage(rec.messages[0], 0xE0, 0x4F, 0x3B, "JI E4 bend bytes");
	checkMessage(rec.messages[1], 0x90, 0x40, 0x64, "JI E4 base note");
}

/* AC3: repeated same-key coalescing and velocity-zero offs. */
void testRepeatsAndOffs()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	engine.processChannelMessage(0x90, 60, 90);
	check(rec.messages.size() == 2, "repeat does not retrigger");

	engine.processChannelMessage(0x80, 60, 0);
	check(rec.messages.size() == 2, "first off does not release");

	rec.clear();
	engine.processChannelMessage(0x80, 60, 0);
	check(rec.only(0x80, 0x3C, 0x00), "final off releases");
	check(engine.voicesInUse() == 0, "voice freed");

	/* Velocity-zero note-on is a note-off. */
	rec.clear();
	engine.processChannelMessage(0x90, 60, 0);
	check(rec.empty(), "stray velocity-zero on ignored");
}

/* A held key released by velocity-zero note-on must emit its stored off. */
void testVelocityZeroRelease()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	rec.clear();
	engine.processChannelMessage(0x90, 60, 0);
	check(rec.only(0x80, 0x3C, 0x00), "velocity-zero on releases held key");
	check(engine.voicesInUse() == 0, "velocity-zero frees the voice");
}

/* AC3: keys outside the receiver range stay silent and cannot kill another key. */
void testUnmappedKey()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());

	/* One degree per period: keys above C4 rise an octave per key and quickly
	   leave the representable MIDI range. */
	retuner::Scale octaves;
	octaves.count = 1;
	octaves.interval[0] = 1.0;
	engine.configureScale(octaves, 2.0);
	engine.arm();
	check(!engine.target(66).mapped, "out-of-range key unmapped");

	engine.processChannelMessage(0x90, 66, 100);
	check(rec.empty(), "unmapped key sends nothing");

	engine.processChannelMessage(0x80, 66, 0);
	check(rec.empty(), "unmapped key off sends nothing");

	engine.processChannelMessage(0x90, 60, 100);
	engine.processChannelMessage(0x80, 66, 0);
	check(rec.messages.size() == 2, "unmapped off does not kill mapped key");
}

/* Output modes choose the receiver channel map and MPE setup. */
void testOutputModes()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);

	/* Per-channel bend uses channels 1-8 and omits the Monumatic CC64. */
	check(engine.setOutputMode(retuner::kOutputPerChannel), "select per-channel mode");
	engine.arm();
	engine.processChannelMessage(0x90, 60, 100);
	check(rec.messages.size() == 2, "per-channel bend then on");
	checkMessage(rec.messages[0], 0xE0, 0x00, 0x40, "per-channel bend channel 1");
	checkMessage(rec.messages[1], 0x90, 0x3C, 0x64, "per-channel note channel 1");

	rec.clear();
	engine.processChannelMessage(0xB0, 120, 0);
	bool sawSustain = false;
	for (const Message& m : rec.messages)
		if (m.b0 == 0xB0 && m.b1 == 64)
			sawSustain = true;
	check(!sawSustain, "per-channel panic omits CC64");
	check(engine.mode() == retuner::kOutputPerChannel, "per-channel mode retained");

	/* Standard MPE: master 1, eight members 2-9, configured on arm. */
	retuner::Engine mpe;
	mpe.setOutput(rec.out());
	mpe.configureScale(retuner::equalTemperamentScale(), 2.0);
	check(mpe.setOutputMode(retuner::kOutputMpe), "select MPE mode");
	rec.clear();
	check(mpe.arm(), "arm MPE");
	check(rec.messages.size() == 35, "MPE setup messages");
	checkMessage(rec.messages[0], 0xB0, 101, 0, "MPE RPN MSB on master");
	checkMessage(rec.messages[1], 0xB0, 100, 6, "MPE configuration RPN");
	checkMessage(rec.messages[2], 0xB0, 6, 8, "eight MPE members");
	checkMessage(rec.messages[3], 0xB1, 101, 0, "member bend RPN MSB");
	checkMessage(rec.messages[4], 0xB1, 100, 0, "member bend RPN LSB");
	checkMessage(rec.messages[5], 0xB1, 6, 2, "member bend range semitones");
	checkMessage(rec.messages[6], 0xB1, 38, 0, "member bend range cents");

	rec.clear();
	mpe.processChannelMessage(0x90, 60, 100);
	mpe.processChannelMessage(0x90, 64, 100);
	check(rec.messages.size() == 4, "two MPE notes");
	checkMessage(rec.messages[0], 0xE1, 0x00, 0x40, "MPE first bend on channel 2");
	checkMessage(rec.messages[1], 0x91, 0x3C, 0x64, "MPE first note on channel 2");
	checkMessage(rec.messages[2], 0xE2, 0x00, 0x40, "MPE second bend on channel 3");
	checkMessage(rec.messages[3], 0x92, 0x40, 0x64, "MPE second note on channel 3");

	rec.clear();
	mpe.processChannelMessage(0xB0, 120, 0);
	bool usedMaster = false;
	for (const Message& m : rec.messages)
		if ((m.b0 & 0x0F) == 0)
			usedMaster = true;
	check(!usedMaster, "MPE panic leaves master channel silent");
	checkMessage(rec.messages[0], 0x81, 0x3C, 0x00, "MPE panic off on member 2");

	/* Changing output mode while armed panics and disarms. */
	rec.clear();
	mpe.arm();
	mpe.processChannelMessage(0x90, 60, 100);
	rec.clear();
	check(mpe.setOutputMode(retuner::kOutputMonumatic), "mode change reported");
	check(!mpe.isArmed(), "mode change disarms");
	check(!rec.empty(), "mode change panics");
}

/* AC3: eight voices, ninth key dropped, then admitted after capacity frees. */
void testOverflowAndDrop()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	const uint8_t keys[9] = { 60, 62, 64, 65, 67, 69, 71, 72, 74 };
	for (int i = 0; i < 8; ++i)
		engine.processChannelMessage(0x90, keys[i], 100);
	check(engine.voicesInUse() == 8, "eight voices held");

	rec.clear();
	engine.processChannelMessage(0x90, keys[8], 100);
	check(rec.empty(), "ninth key dropped");

	engine.processChannelMessage(0x80, keys[8], 0);
	check(rec.empty(), "dropped key off silent");

	rec.clear();
	engine.processChannelMessage(0x80, keys[0], 0);
	check(rec.only(0x80, 0x3C, 0x00), "first voice released");
	check(engine.voicesInUse() == 7, "one voice free");

	rec.clear();
	engine.processChannelMessage(0x90, keys[8], 100);
	check(rec.messages.size() == 2, "dropped key admitted after free");
}

/* AC3: count overflow triggers panic and a disarmed fault. */
void testCountOverflow()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	for (int i = 0; i < 0xFFFF; ++i)
		engine.processChannelMessage(0x90, 60, 100);
	check(engine.isArmed(), "still armed at count 65535");

	rec.clear();
	engine.processChannelMessage(0x90, 60, 100);
	check(!engine.isArmed(), "overflow disarms");
	check(engine.faultState() == retuner::kFaultNoteOverflow, "overflow fault");
	check(!rec.empty(), "overflow panics");
}

/* AC4: excluded controllers emit nothing. */
void testExcludedControllers()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0xE0, 0x00, 0x40); /* pitch bend */
	engine.processChannelMessage(0xD0, 0x50, 0x00); /* channel pressure */
	engine.processChannelMessage(0xA0, 0x3C, 0x50); /* poly pressure */
	engine.processChannelMessage(0xB0, 0x4A, 0x40); /* CC74 */
	engine.processChannelMessage(0xB0, 0x40, 0x7F); /* CC64 sustain */
	engine.processChannelMessage(0xB0, 0x42, 0x7F); /* CC66 sostenuto */
	engine.processChannelMessage(0xC0, 0x05, 0x00); /* program change */
	engine.processChannelMessage(0xB1, 0x07, 0x64); /* other channel */
	check(rec.empty(), "excluded controllers silent");
}

/* CC123 is ignored; CC120 clears notes and bends but keeps Arm on. */
void testAllNotesOffAndPanic()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	engine.processChannelMessage(0x90, 64, 100);

	rec.clear();
	engine.processChannelMessage(0xB0, 123, 0);
	check(rec.empty(), "CC123 sends nothing");
	check(engine.isArmed(), "CC123 leaves armed");
	check(engine.voicesInUse() == 2, "CC123 leaves held notes alone");

	engine.processChannelMessage(0x80, 60, 0);
	engine.processChannelMessage(0x80, 64, 0);

	/* Ordinary release leaves the bend untouched. */
	engine.processChannelMessage(0x90, 67, 100);
	rec.clear();
	engine.processChannelMessage(0x80, 67, 0);
	check(rec.only(0x80, 0x43, 0x00), "ordinary release has no bend reset");

	/* Panic: offs, sustain off, all-sound-off, then centred bends. */
	engine.processChannelMessage(0x90, 72, 100);
	rec.clear();
	engine.processChannelMessage(0xB0, 120, 0);
	check(engine.isArmed(), "CC120 keeps Arm on");
	check(engine.voicesInUse() == 0, "CC120 clears voices");
	check(rec.messages.size() >= 3, "panic sends kill sequence");
	bool sawOff = false, sawSustain = false, sawSoundOff = false, sawCentre = false;
	for (const Message& m : rec.messages)
	{
		if (m.b0 == 0x80 && m.b1 == 0x48 && m.b2 == 0x00)
			sawOff = true;
		if (m.b0 == 0xB0 && m.b1 == 64 && m.b2 == 0)
			sawSustain = true;
		if (m.b0 == 0xB0 && m.b1 == 120 && m.b2 == 0)
			sawSoundOff = true;
		if (m.b0 == 0xE0 && m.b1 == 0 && m.b2 == 0x40)
			sawCentre = true;
	}
	check(sawOff && sawSustain && sawSoundOff && sawCentre, "panic sequence");
}

/* AC4: panic covers all eight channels, not only the first. */
void testPanicAllChannels()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	const uint8_t keys[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
	for (int i = 0; i < 8; ++i)
		engine.processChannelMessage(0x90, keys[i], 100);

	rec.clear();
	engine.processChannelMessage(0xB0, 120, 0);

	check(rec.messages.size() == 32, "panic sends 8 offs, 8 CC64, 8 CC120, 8 bends");
	check(engine.isArmed(), "CC120 keeps Arm on");
	for (int i = 0; i < 8; ++i)
	{
		checkMessage(rec.messages[i * 3 + 0], (uint8_t)(0x80 | i), keys[i], 0x00, "panic off per channel");
		checkMessage(rec.messages[i * 3 + 1], (uint8_t)(0xB0 | i), 64, 0, "panic sustain per channel");
		checkMessage(rec.messages[i * 3 + 2], (uint8_t)(0xB0 | i), 120, 0, "panic sound off per channel");
		checkMessage(rec.messages[24 + i], (uint8_t)(0xE0 | i), 0, 0x40, "panic centre per channel");
	}
}

/* AC4: Stop is ignored; Reset clears notes; Start/Continue do not replay. */
void testRealtime()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	rec.clear();
	engine.processRealtime(0xFC);
	check(rec.empty(), "stop sends nothing");
	check(engine.voicesInUse() == 1, "stop leaves held notes alone");
	check(engine.isArmed(), "stop leaves armed");

	engine.processChannelMessage(0x90, 64, 100);
	rec.clear();
	engine.processRealtime(0xFF);
	check(engine.isArmed(), "reset clears notes but stays armed");
	check(engine.voicesInUse() == 0, "reset clears voices");
	check(!rec.empty(), "reset clears notes");

	rec.clear();
	engine.arm();
	engine.processRealtime(0xFA);
	engine.processRealtime(0xFB);
	check(rec.empty(), "start/continue do not replay");
	check(engine.isArmed(), "start/continue leave armed");
}

/* AC3: a dropped key held across a freed slot is not admitted mid-gesture. */
void testDroppedKeyMidGesture()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	const uint8_t keys[9] = { 60, 62, 64, 65, 67, 69, 71, 72, 74 };
	for (int i = 0; i < 8; ++i)
		engine.processChannelMessage(0x90, keys[i], 100);
	engine.processChannelMessage(0x90, keys[8], 100); /* dropped */

	engine.processChannelMessage(0x80, keys[0], 0); /* free a voice */
	rec.clear();
	engine.processChannelMessage(0x90, keys[8], 100); /* still held */
	check(rec.empty(), "dropped key not admitted mid-gesture");
}

/* AC4/AC5: reconfiguration panics and disarms. */
void testReconfiguration()
{
	Recorder rec;
	retuner::Engine engine;
	engine.setOutput(rec.out());
	engine.configureScale(retuner::equalTemperamentScale(), 2.0);
	engine.arm();

	engine.processChannelMessage(0x90, 60, 100);
	rec.clear();
	check(engine.setChannel(2), "channel change reported");
	check(!engine.isArmed(), "channel change disarms");
	check(!rec.empty(), "channel change panics");

	engine.arm();
	rec.clear();
	check(engine.configureScale(justC12Scale(), 2.0), "tuning change reported");
	check(!engine.isArmed(), "tuning change disarms");
	check(!rec.empty(), "tuning change panics");

	/* Unchanged configuration is not a panic boundary. */
	engine.arm();
	rec.clear();
	check(!engine.configureScale(justC12Scale(), 2.0), "same tuning ignored");
	check(engine.isArmed(), "same tuning leaves armed");
	check(rec.empty(), "same tuning sends nothing");
}

} // namespace

int main()
{
	testMakeTarget();
	testPitchFor();
	testArmAndTable();
	testVoiceAllocation();
	testJustIntonationOverlap();
	testBendBeforeOn();
	testRepeatsAndOffs();
	testUnmappedKey();
	testOutputModes();
	testOverflowAndDrop();
	testCountOverflow();
	testExcludedControllers();
	testAllNotesOffAndPanic();
	testPanicAllChannels();
	testVelocityZeroRelease();
	testRealtime();
	testDroppedKeyMidGesture();
	testReconfiguration();

	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all retuner core tests passed\n");
	return 0;
}
