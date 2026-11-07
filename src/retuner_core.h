/*
 * Static MIDI retuner core.
 *
 * Host-independent note/tuning engine so the behaviour can be tested natively.
 * The disting NT adapter lives in retuner.cpp.
 *
 * Contract summary:
 *  - one selected input channel, eight reserved receiver channels (1-8);
 *  - tuning is a repeating Scala scale anchored at MIDI 60 (C4);
 *  - every admitted key gets its pitch bend sent before its note-on;
 *  - a key holds one receiver voice until its final note-off arrives;
 *  - CC120/121, System Reset and overflow release everything and centre the
 *    bends; input panics keep Arm on, configuration changes disarm.
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
	kMaxScaleNotes = 128,
	kRootNote = 60,
};

constexpr uint32_t fourCc(char a, char b, char c, char d)
{
	return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
	       ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

/* thorinside namespace, algorithm "Rt" (retuner). */
constexpr uint32_t kGuid = fourCc('T', 'h', 'R', 't');

enum OutputMode
{
	kOutputMonumatic = 0,
	kOutputPerChannel = 1,
	kOutputMpe = 2,
};

enum Fault
{
	kFaultNone = 0,
	kFaultNoteOverflow,
};

enum PanicReason
{
	kPanicNone = 0,
	kPanicDisable,
	kPanicAllSoundOff,
	kPanicResetControllers,
	kPanicReset,
	kPanicOverflow,
	kPanicConfig,
	kPanicScaleFailure,
};

enum TableError
{
	kTableOk = 0,
	kTableBendUnset,
	kTableBendInvalid,
	kTableBendTooSmall,
	kTableScaleInvalid,
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

/*
 * One Scala scale: `count` pitch values from the .scl file. Degree 0 is the
 * implicit 1/1; interval[d - 1] is degree d. The last value is the formal
 * octave (period) and is not a playable degree in the mapping pattern.
 */
struct Scale
{
	Scale() : count(0), interval() {}

	uint8_t count;
	double interval[kMaxScaleNotes];
};

/* Built-in fallback so the plugin is usable without an SD card or .scl file. */
inline Scale equalTemperamentScale()
{
	Scale scale;
	scale.count = 12;
	for (int i = 0; i < 12; ++i)
		scale.interval[i] = (double)(i + 1) / 12.0;
	return scale;
}

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
	/* Reject unset, non-finite and non-positive ranges. */
	if (!(bendRange > 0.0) || !isfinite(bendRange))
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

/*
 * Absolute pitch of one key under a repeating scale.
 *
 * The disting NT automatic keyboard map anchors 1/1 at the root note and
 * advances one scale degree per key. Here the root is fixed to MIDI 60, so
 * key 60 is 1/1 and key 60 + count starts the next period.
 */
inline bool pitchFor(const Scale& scale, int key, double& pitch)
{
	if (scale.count == 0 || scale.count > kMaxScaleNotes)
		return false;
	if (key < 0 || key >= kKeys)
		return false;

	int offset = key - kRootNote;
	int octave = offset / scale.count;
	int step = offset % scale.count;
	if (step < 0)
	{
		step += scale.count;
		--octave;
	}

	const double within = step > 0 ? scale.interval[step - 1] : 0.0;
	const double period = scale.interval[scale.count - 1];
	pitch = (double)kRootNote + 12.0 * ((double)octave * period + within);
	return isfinite(pitch);
}

class Engine
{
public:
	Engine()
		: output(), selectedChannel(0), outputMode(kOutputMonumatic),
		  scale(equalTemperamentScale()), bendRange(0.0), configured(false),
		  tableValid(false), armed(false), fault(kFaultNone),
		  lastPanicReason(kPanicNone), error(kTableBendUnset)
	{
		clearState();
		rebuild();
	}

	void setOutput(MidiOut out) { output = out; }

	int channel() const { return selectedChannel; }
	OutputMode mode() const { return outputMode; }
	const Scale& currentScale() const { return scale; }
	double bendRangeSemitones() const { return bendRange; }
	bool isArmed() const { return armed; }
	bool isTableValid() const { return tableValid; }
	Fault faultState() const { return fault; }
	PanicReason lastPanic() const { return lastPanicReason; }
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
			panic(kPanicConfig);
		selectedChannel = channelIndex;
		return true;
	}

	bool setOutputMode(OutputMode newMode)
	{
		if (newMode < kOutputMonumatic)
			newMode = kOutputMonumatic;
		if (newMode > kOutputMpe)
			newMode = kOutputMpe;
		if (newMode == outputMode)
			return false;
		if (armed)
			panic(kPanicConfig);
		outputMode = newMode;
		return true;
	}

	bool configureScale(const Scale& newScale, double semitones)
	{
		if (configured && sameScale(newScale, scale) && semitones == bendRange && tableValid)
			return false;
		if (armed)
			panic(kPanicConfig);
		scale = newScale;
		bendRange = semitones;
		configured = true;
		rebuild();
		return true;
	}

	bool configureBendRange(double semitones)
	{
		if (configured && semitones == bendRange && tableValid)
			return false;
		if (armed)
			panic(kPanicConfig);
		bendRange = semitones;
		configured = true;
		rebuild();
		return true;
	}

	/* Force the table invalid while a new .scl file is being loaded. */
	void invalidateTable()
	{
		for (int i = 0; i < kKeys; ++i)
			table[i] = Target();
		tableValid = false;
		error = kTableScaleInvalid;
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
		lastPanicReason = kPanicNone;
		if (outputMode == kOutputMpe)
			sendMpeSetup();
		return true;
	}

	/* Explicit Disable: panic and disarm when armed. */
	void disarm()
	{
		if (armed)
			panic(kPanicDisable);
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
				panic(kPanicAllSoundOff, false);
				break;
			case 121: /* reset all controllers: panic, never forwarded */
				panic(kPanicResetControllers, false);
				break;
			case 123: /* all notes off: ignored, note-offs own the lifecycle */
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
		/* Stop (0xFC) is ignored: some sources emit it during ordinary
		   navigation. System Reset (0xFF) still clears held notes. */
		if (byte == 0xFF && armed)
			panic(kPanicReset, false);
	}

	void noteOn(uint8_t key, uint8_t velocity)
	{
		if (!armed)
			return;

		Key& record = keys[key];
		if (record.count == 0xFFFFu)
		{
			panic(kPanicOverflow);
			fault = kFaultNoteOverflow;
			return;
		}

		if (record.count == 0 && table[key].mapped)
		{
			const int slot = freeSlot();
			if (slot >= 0)
			{
				const Target& target_ = table[key];
				const int channel = outputChannel(slot);
				Voice& voice = voices[slot];
				voice.occupied = 1;
				voice.channel = (uint8_t)channel;
				voice.note = target_.note;
				voice.key = key;
				record.voice = (uint8_t)(slot + 1);

				send((uint8_t)(0xE0 | channel), (uint8_t)(target_.bend & 0x7F),
				     (uint8_t)(target_.bend >> 7));
				send((uint8_t)(0x90 | channel), target_.note, velocity);
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

	/* Release, silence the receiver and centre bends; optionally disarm. */
	void panic(PanicReason reason = kPanicNone, bool disarmOutput = true)
	{
		lastPanicReason = reason;
		const int base = outputBase();
		for (int i = 0; i < kChannels; ++i)
		{
			if (voices[i].occupied)
			{
				send((uint8_t)(0x80 | voices[i].channel), voices[i].note, 0);
				voices[i].occupied = 0;
			}
			if (outputMode != kOutputPerChannel)
				send((uint8_t)(0xB0 | (base + i)), 64, 0);  /* sustain off */
			send((uint8_t)(0xB0 | (base + i)), 120, 0); /* all sound off */
		}
		for (int i = 0; i < kChannels; ++i)
			send((uint8_t)(0xE0 | (base + i)), 0, 64);

		clearState();
		if (disarmOutput)
			armed = false;
	}

private:
	int outputBase() const
	{
		return outputMode == kOutputMpe ? 1 : 0;
	}

	int outputChannel(int slot) const
	{
		return outputBase() + slot;
	}

	void sendMpeSetup()
	{
		/* Lower zone, master channel 1: eight member channels 2-9. */
		send(0xB0, 101, 0);
		send(0xB0, 100, 6);
		send(0xB0, 6, (uint8_t)kChannels);

		int semitones = (int)bendRange;
		int cents = (int)((bendRange - (double)semitones) * 100.0 + 0.5);
		if (cents >= 100)
		{
			++semitones;
			cents = 0;
		}
		if (semitones > 127)
		{
			semitones = 127;
			cents = 0;
		}

		for (int i = 0; i < kChannels; ++i)
		{
			const uint8_t channel = (uint8_t)(1 + i);
			send((uint8_t)(0xB0 | channel), 101, 0);
			send((uint8_t)(0xB0 | channel), 100, 0);
			send((uint8_t)(0xB0 | channel), 6, (uint8_t)semitones);
			send((uint8_t)(0xB0 | channel), 38, (uint8_t)cents);
		}
	}

	static bool sameScale(const Scale& a, const Scale& b)
	{
		if (a.count != b.count)
			return false;
		for (int i = 0; i < a.count; ++i)
			if (a.interval[i] != b.interval[i])
				return false;
		return true;
	}

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
		if (!isfinite(bendRange))
		{
			error = kTableBendInvalid;
			return;
		}
		if (scale.count == 0 || scale.count > kMaxScaleNotes)
		{
			error = kTableScaleInvalid;
			return;
		}

		const double period = scale.interval[scale.count - 1];
		if (!isfinite(period) || !(period > 0.0))
		{
			error = kTableScaleInvalid;
			return;
		}
		for (int i = 0; i < scale.count; ++i)
		{
			if (!isfinite(scale.interval[i]))
			{
				error = kTableScaleInvalid;
				return;
			}
		}

		for (int key = 0; key < kKeys; ++key)
		{
			double pitch = 0.0;
			if (!pitchFor(scale, key, pitch))
				continue;

			/* Keys outside the receiver's representable note range are
			   intentionally silent rather than folded to a neighbour. */
			if (pitch < -bendRange || pitch > 127.0 + bendRange)
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
	OutputMode outputMode;
	Scale scale;
	double bendRange;
	bool configured;
	bool tableValid;
	bool armed;
	Fault fault;
	PanicReason lastPanicReason;
	TableError error;
};

} // namespace retuner

#endif
