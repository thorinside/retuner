/*
 * Static MIDI retuner core.
 *
 * Host-independent note/tuning engine so the behaviour can be tested natively.
 * The disting NT adapter lives in retuner.cpp.
 *
 * Contract summary:
 *  - one selected input channel, eight reserved receiver channels (1-8);
 *  - every admitted key gets its pitch bend sent before its note-on;
 *  - a key holds one receiver voice until its final note-off arrives;
 *  - panic (CC120/121, realtime Stop/Reset, overflow, setup change) releases
 *    everything, centres the eight bends and disarms.
 */
#ifndef RETUNER_CORE_H
#define RETUNER_CORE_H

#include <math.h>
#include <stdint.h>

namespace retuner {

enum
{
	kChannels = 8,
	kKeys = 128,
};

constexpr uint32_t fourCc(char a, char b, char c, char d)
{
	return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
	       ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

/* thorinside namespace, algorithm "Rt" (retuner). */
constexpr uint32_t kGuid = fourCc('T', 'h', 'R', 't');

enum Tuning
{
	kTuningEqualTemperament = 0,
	kTuningJustC = 1,
};

enum Fault
{
	kFaultNone = 0,
	kFaultNoteOverflow,
};

enum TableError
{
	kTableOk = 0,
	kTableBendUnset,
	kTableBendTooSmall,
};

struct MidiOut
{
	void* ctx;
	void (*send3)(void* ctx, uint8_t b0, uint8_t b1, uint8_t b2);
};

/* One resolved key of the active table. */
struct Target
{
	uint16_t bend;    /* 14-bit receiver bend, centre = 8192 */
	uint8_t note;     /* base note sent with the bend */
	uint8_t mapped;   /* 0 = this key is intentionally silent */
};

struct Key
{
	uint16_t count;   /* held instances, 0xFFFF is the overflow guard */
	uint8_t voice;    /* 0 = none, otherwise voice slot + 1 */
};

struct Voice
{
	uint8_t channel;
	uint8_t note;
	uint8_t key;
	uint8_t occupied;
};

/*
 * Resolve one absolute pitch to a receiver base note and 14-bit bend.
 * Returns false when the pitch cannot be represented by this bend range.
 */
inline bool makeTarget(double pitch, double bendRange, Target& out)
{
	if (!(bendRange > 0.0))
		return false;

	double base = floor(pitch + 0.5);
	if (base < 0.0)
		base = 0.0;
	if (base > 127.0)
		base = 127.0;

	const double offset = pitch - base;
	if (!(fabs(offset) <= bendRange))
		return false;

	const double scaled = 8192.0 * offset / bendRange;
	const double rounded = scaled >= 0.0 ? floor(scaled + 0.5) : ceil(scaled - 0.5);
	const double quantised = 8192.0 + rounded;

	/* |offset| <= bendRange keeps this in 0..16384; only the positive
	   full-scale endpoint needs the representable rounding step. */
	if (quantised < 0.0 || quantised > 16384.0)
		return false;

	out.bend = (uint16_t)(quantised > 16383.0 ? 16383.0 : quantised);
	out.note = (uint8_t)base;
	out.mapped = 1;
	return true;
}

/* Absolute pitch of one key, or false when the tuning leaves it unmapped. */
inline bool pitchFor(int tuning, int key, double& pitch)
{
	if (tuning == kTuningJustC)
	{
		/* Fixed C major, anchored so that middle C (MIDI 60) is exact. */
		int octave = (key - 60) / 12;
		int degree = (key - 60) % 12;
		if (degree < 0)
		{
			degree += 12;
			--octave;
		}
		double ratio = 0.0;
		switch (degree)
		{
		case 0: ratio = 1.0; break;
		case 2: ratio = 9.0 / 8.0; break;
		case 4: ratio = 5.0 / 4.0; break;
		case 5: ratio = 4.0 / 3.0; break;
		case 7: ratio = 3.0 / 2.0; break;
		case 9: ratio = 5.0 / 3.0; break;
		case 11: ratio = 15.0 / 8.0; break;
		default: return false;
		}
		pitch = 60.0 + 12.0 * octave + 12.0 * log2(ratio);
		return true;
	}
	pitch = (double)key;
	return true;
}

class Engine
{
public:
	Engine()
		: output(), selectedChannel(0), tuning(kTuningEqualTemperament),
		  bendRange(0.0), configured(false), tableValid(false), armed(false),
		  fault(kFaultNone), error(kTableBendUnset)
	{
		clearState();
		rebuild();
	}

	void setOutput(MidiOut out) { output = out; }

	int channel() const { return selectedChannel; }
	int tuningMode() const { return tuning; }
	double bendRangeSemitones() const { return bendRange; }
	bool isArmed() const { return armed; }
	bool isTableValid() const { return tableValid; }
	Fault faultState() const { return fault; }
	TableError tableError() const { return error; }
	const Target& target(int key) const { return table[key]; }

	int voicesInUse() const
	{
		int used = 0;
		for (int i = 0; i < kChannels; ++i)
			used += voices[i].occupied ? 1 : 0;
		return used;
	}

	/* Configuration changes are refused by the caller while armed; these
	   methods enforce the panic boundary themselves and report a change. */
	bool setChannel(int channelIndex)
	{
		if (channelIndex < 0)
			channelIndex = 0;
		if (channelIndex > 15)
			channelIndex = 15;
		if (channelIndex == selectedChannel)
			return false;
		if (armed)
			panic();
		selectedChannel = channelIndex;
		return true;
	}

	bool configureTuning(int mode, double semitones)
	{
		if (mode < 0)
			mode = 0;
		if (mode > 1)
			mode = 1;
		if (configured && mode == tuning && semitones == bendRange)
			return false;
		if (armed)
			panic();
		tuning = mode;
		bendRange = semitones;
		configured = true;
		rebuild();
		return true;
	}

	bool arm()
	{
		if (armed)
			return true;
		if (!tableValid)
			return false;
		clearState();
		armed = true;
		fault = kFaultNone;
		return true;
	}

	/* Explicit Disable: panic and disarm when armed. */
	void disarm()
	{
		if (armed)
			panic();
	}

	void processChannelMessage(uint8_t b0, uint8_t b1, uint8_t b2)
	{
		if (!armed)
			return;
		if ((b0 & 0x0F) != (uint8_t)selectedChannel)
			return;
		switch (b0 & 0xF0)
		{
		case 0x90:
			if (b2 > 0)
				noteOn(b1, b2);
			else
				noteOff(b1, 0);
			break;
		case 0x80:
			noteOff(b1, b2);
			break;
		case 0xB0:
			switch (b1)
			{
			case 120: /* all sound off */
			case 121: /* reset all controllers: panic, never forwarded */
				panic();
				break;
			case 123: /* all notes off */
				allNotesOff();
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}

	void processRealtime(uint8_t byte)
	{
		if (byte == 0xFC || byte == 0xFF) /* stop, system reset */
		{
			if (armed)
				panic();
		}
	}

	void noteOn(uint8_t key, uint8_t velocity)
	{
		if (!armed)
			return;

		Key& record = keys[key];
		if (record.count == 0xFFFFu)
		{
			panic();
			fault = kFaultNoteOverflow;
			return;
		}

		if (record.count == 0 && table[key].mapped)
		{
			const int slot = freeSlot();
			if (slot >= 0)
			{
				const Target& target_ = table[key];
				Voice& voice = voices[slot];
				voice.occupied = 1;
				voice.channel = (uint8_t)slot;
				voice.note = target_.note;
				voice.key = key;
				record.voice = (uint8_t)(slot + 1);

				send((uint8_t)(0xE0 | slot), (uint8_t)(target_.bend & 0x7F),
				     (uint8_t)(target_.bend >> 7));
				send((uint8_t)(0x90 | slot), target_.note, velocity);
			}
		}
		++record.count;
	}

	void noteOff(uint8_t key, uint8_t velocity)
	{
		if (!armed)
			return;

		Key& record = keys[key];
		if (record.count == 0)
			return;
		if (--record.count > 0)
			return;
		if (record.voice == 0)
			return;

		Voice& voice = voices[record.voice - 1];
		send((uint8_t)(0x80 | voice.channel), voice.note, velocity);
		voice.occupied = 0;
		record.voice = 0;
	}

	/* Graceful CC123: release everything but leave the bends alone. */
	void allNotesOff()
	{
		for (int i = 0; i < kChannels; ++i)
		{
			if (!voices[i].occupied)
				continue;
			send((uint8_t)(0x80 | voices[i].channel), voices[i].note, 0);
			voices[i].occupied = 0;
		}
		clearState();
	}

	/* Hard panic: release, silence the receiver, centre bends, disarm. */
	void panic()
	{
		for (int i = 0; i < kChannels; ++i)
		{
			if (voices[i].occupied)
			{
				send((uint8_t)(0x80 | voices[i].channel), voices[i].note, 0);
				voices[i].occupied = 0;
			}
			send((uint8_t)(0xB0 | i), 64, 0);  /* sustain off */
			send((uint8_t)(0xB0 | i), 120, 0); /* all sound off */
		}
		for (int i = 0; i < kChannels; ++i)
			send((uint8_t)(0xE0 | i), 0, 64);

		clearState();
		armed = false;
	}

private:
	void send(uint8_t status, uint8_t d1, uint8_t d2) const
	{
		if (output.send3)
			output.send3(output.ctx, status, d1, d2);
	}

	int freeSlot() const
	{
		for (int i = 0; i < kChannels; ++i)
			if (!voices[i].occupied)
				return i;
		return -1;
	}

	/* Clear note tracking and voices; the resolved table survives panic. */
	void clearState()
	{
		for (int i = 0; i < kKeys; ++i)
		{
			keys[i].count = 0;
			keys[i].voice = 0;
		}
		for (int i = 0; i < kChannels; ++i)
			voices[i] = Voice();
	}

	void rebuild()
	{
		for (int i = 0; i < kKeys; ++i)
			table[i] = Target();
		tableValid = false;

		if (!(bendRange > 0.0))
		{
			error = kTableBendUnset;
			return;
		}

		for (int key = 0; key < kKeys; ++key)
		{
			double pitch = 0.0;
			if (!pitchFor(tuning, key, pitch))
				continue;
			Target resolved;
			if (!makeTarget(pitch, bendRange, resolved))
			{
				for (int i = 0; i < kKeys; ++i)
					table[i] = Target();
				error = kTableBendTooSmall;
				return;
			}
			table[key] = resolved;
		}
		tableValid = true;
		error = kTableOk;
	}

	MidiOut output;
	Target table[kKeys];
	Key keys[kKeys];
	Voice voices[kChannels];
	int selectedChannel;
	int tuning;
	double bendRange;
	bool configured;
	bool tableValid;
	bool armed;
	Fault fault;
	TableError error;
};

} // namespace retuner

#endif
