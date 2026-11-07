/*
 * Adapter tests for src/retuner.cpp against the disting NT API stubs.
 *
 * These cover the host-facing contract that the core tests cannot reach:
 * the global parameter offset used by NT_setParameterFromUi and the Arm
 * parameter staying in sync after MIDI-triggered panics.
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include <distingnt/api.h>
#include <distingnt/microtuning.h>

#include "retuner_core.h"

namespace {

struct Message
{
	uint8_t b0, b1, b2;
};

struct Stub
{
	int32_t algorithmIndex = 0;
	uint32_t parameterOffset = 7;
	uint32_t setAlgorithm = 0;
	uint32_t setParameter = 0;
	int16_t setValue = 0;
	int setCalls = 0;
	uint32_t lastDestination = 0;
	_NT_algorithm* algorithm = nullptr;
	std::vector<Message> messages;

	bool mounted = true;
	uint32_t numScl = 1;
	_NT_sclNote* notes = nullptr;
	uint32_t numNotes = 0;
	const char* scaleName = "12-TET";
	int readCalls = 0;
	int updateCalls = 0;
	bool deferCallback = false;
	void (*pendingCallback)(void*) = nullptr;
	void* pendingData = nullptr;

	void completeRead()
	{
		void (*callback)(void*) = pendingCallback;
		void* data = pendingData;
		pendingCallback = nullptr;
		if (callback)
			callback(data);
	}
};

Stub stub;

int failures = 0;

_NT_sclNote equalTemperamentNotes[12];
_NT_sclNote justNotes[12];
_NT_sclNote negativeCentsNotes[2];
_NT_sclNote negativeOctaveNotes[2];
_NT_sclNote ambiguousRatioNotes[2];

void setRatio(_NT_sclNote& note, int32_t numerator, int32_t denominator)
{
	note.numeratorValue = numerator;
	note.denominatorValue = -denominator;
}

void resetScaleStub()
{
	for (int i = 0; i < 12; ++i)
		equalTemperamentNotes[i].octaves = (double)(i + 1) / 12.0;

	static const int ratios[12][2] = {
		{ 16, 15 }, { 9, 8 }, { 6, 5 }, { 5, 4 }, { 4, 3 }, { 45, 32 },
		{ 3, 2 }, { 8, 5 }, { 5, 3 }, { 9, 5 }, { 15, 8 }, { 2, 1 }
	};
	for (int i = 0; i < 12; ++i)
		setRatio(justNotes[i], ratios[i][0], ratios[i][1]);

	negativeCentsNotes[0].octaves = -5.0 / 1200.0;
	setRatio(negativeCentsNotes[1], 2, 1);
	negativeOctaveNotes[0].octaves = -1.0;
	setRatio(negativeOctaveNotes[1], 2, 1);
	setRatio(ambiguousRatioNotes[0], 1075838977, 1075838976);
	setRatio(ambiguousRatioNotes[1], 2, 1);

	stub.mounted = true;
	stub.numScl = 1;
	stub.notes = equalTemperamentNotes;
	stub.numNotes = 12;
	stub.scaleName = "12-TET";
	stub.readCalls = 0;
	stub.updateCalls = 0;
	stub.deferCallback = false;
	stub.pendingCallback = nullptr;
	stub.pendingData = nullptr;
}

void check(bool ok, const char* what)
{
	if (!ok)
	{
		++failures;
		std::printf("FAIL: %s\n", what);
	}
}

const _NT_factory* factory()
{
	return reinterpret_cast<const _NT_factory*>(pluginEntry(kNT_selector_factoryInfo, 0));
}

struct Fixture
{
	_NT_algorithm* algorithm;
	int16_t values[5];

	Fixture()
	{
		resetScaleStub();
		_NT_algorithmRequirements req;
		factory()->calculateRequirements(req, nullptr);
		static char storage[8192];
		_NT_algorithmMemoryPtrs ptrs = { reinterpret_cast<uint8_t*>(storage), nullptr, nullptr, nullptr };
		algorithm = factory()->construct(ptrs, req, nullptr);
		std::memset(values, 0, sizeof values);
		values[0] = 1;   /* channel 1 */
		values[1] = 0;   /* .scl index 0 */
		values[2] = 200; /* 2.00 semitones */
		values[3] = 0;   /* disarmed */
		values[4] = 0;   /* Monumatic output */
		algorithm->v = values;
		stub.algorithm = algorithm;
	}

	void changed(int p) { factory()->parameterChanged(algorithm, p); }
	void step() { factory()->step(algorithm, nullptr, 0); }
	static void resetStub()
	{
		stub.setCalls = 0;
		stub.lastDestination = 0;
		stub.messages.clear();
		stub.readCalls = 0;
		stub.updateCalls = 0;
	}
};

void testVersionAndFactory()
{
	check(pluginEntry(kNT_selector_version, 0) == kNT_apiVersionCurrent, "API version");
	check(pluginEntry(kNT_selector_numFactories, 0) == 1, "one factory");
	const _NT_factory* f = factory();
	check(f != nullptr, "factory present");
	check(f->guid == retuner::kGuid, "ThRt guid");
}

/* Source CC120/CC121 are ignored; explicit Arm Off is the panic. */
void testCC120Ignored()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.resetStub();

	f.values[3] = 1;
	f.changed(3);
	check(stub.setCalls == 0, "valid arm sends no sync write");

	factory()->midiMessage(f.algorithm, 0xB0, 120, 0);
	f.step();
	check(stub.setCalls == 0, "CC120 does not write Arm off");
	check(f.values[3] == 1, "CC120 leaves Arm on");
	check(stub.messages.empty(), "CC120 emits nothing");
}

/* Realtime Stop/Reset clear notes but leave Arm on. */
void testRealtimeStopKeepsArm()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiRealtime(f.algorithm, 0xFC);
	f.step();
	check(stub.setCalls == 0, "Stop does not write Arm off");
	check(f.values[3] == 1, "Stop leaves Arm on");
}

/* Invalid table: arming is refused and the Arm parameter is pushed back. */
void testArmRefusedOnInvalidTable()
{
	Fixture f;
	f.values[2] = 0; /* unset bend range */
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.resetStub();

	f.values[3] = 1;
	f.changed(3);
	f.step();
	check(stub.setCalls == 1, "invalid arm syncs Arm");
	check(stub.setParameter == (uint32_t)(3 + 7), "invalid arm sync uses offset");
	check(stub.setValue == 0, "invalid arm sync writes Off");
}

/* A configuration change while armed panics and syncs Arm. */
void testConfigChangeSyncsArm()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	f.values[0] = 2;
	f.changed(0);
	f.step();
	check(stub.setCalls == 1, "channel change syncs Arm");
	check(stub.setParameter == (uint32_t)(3 + 7), "channel sync uses offset");
	check(stub.setValue == 0, "channel sync writes Off");
}

/* The adapter forwards MIDI in the core's order and sends no audio. */
void testMidiForwarding()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiMessage(f.algorithm, 0x90, 60, 100);
	check(stub.messages.size() == 2, "bend then note-on");
	check(stub.messages[0].b0 == 0xE0 && stub.messages[0].b1 == 0x00 && stub.messages[0].b2 == 0x40, "bend bytes");
	check(stub.messages[1].b0 == 0x90 && stub.messages[1].b1 == 0x3C && stub.messages[1].b2 == 0x64, "note-on bytes");
	check(stub.lastDestination == kNT_destinationBreakout, "breakout-only destination");

	/* Unselected channel is ignored. */
	f.resetStub();
	factory()->midiMessage(f.algorithm, 0x91, 60, 100);
	check(stub.messages.empty(), "other channel ignored");
}

/* No card observed yet: the built-in 12-TET table remains armable. */
void testNoCardFallback()
{
	Fixture f;
	stub.mounted = false;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	check(stub.setCalls == 0, "no-card fallback arms");

	factory()->midiMessage(f.algorithm, 0x90, 60, 100);
	check(stub.messages.size() == 2, "no-card fallback sends a note");
	check(stub.messages[0].b0 == 0xE0 && stub.messages[1].b0 == 0x90, "no-card fallback channel 1");
}

/* A smaller remount library clamps the selector; an armed reload must disarm. */
void testArmedRemountClampDisarms()
{
	Fixture f;
	stub.numScl = 2;
	f.step(); /* mount and load the default scale */
	f.values[1] = 1;
	f.changed(1);
	f.values[2] = 200;
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.step();
	check(f.values[3] == 1, "armed before remount");

	f.resetStub();
	stub.mounted = false;
	f.step();
	stub.numScl = 1;
	stub.mounted = true;
	f.step();

	check(f.values[3] == 0, "remount clamp disarms");
	check(stub.setCalls >= 1 && stub.setParameter == (uint32_t)(3 + 7) && stub.setValue == 0,
	      "remount clamp writes Arm off");
}

/* Ambiguous union entries are rejected rather than turned into wrong pitches. */
void expectAmbiguousRejected(_NT_sclNote* notes, const char* what)
{
	Fixture f;
	stub.notes = notes;
	stub.numNotes = 2;
	stub.scaleName = "ambiguous.scl";
	f.step(); /* mount and attempt to load */
	f.values[2] = 200;
	f.changed(2);
	f.resetStub();
	f.values[3] = 1;
	f.changed(3);
	f.step();
	check(stub.setCalls == 1 && stub.setValue == 0, what);
}

void testAmbiguousScalesRejected()
{
	expectAmbiguousRejected(negativeCentsNotes, "negative cents scale rejected");
	expectAmbiguousRejected(negativeOctaveNotes, "negative octave scale rejected");
	expectAmbiguousRejected(ambiguousRatioNotes, "ambiguous packed ratio rejected");
}

/* A deferred read from the old card must not become the new card's scale. */
void testStaleReadAcrossRemount()
{
	Fixture f;
	stub.notes = justNotes;
	stub.deferCallback = true;
	f.step(); /* mount card A, start JI read */
	check(stub.pendingCallback != nullptr, "old card read started");

	stub.mounted = false;
	f.step();
	stub.notes = equalTemperamentNotes;
	stub.mounted = true;
	f.step();
	check(stub.pendingCallback != nullptr, "old read still owns the request");

	stub.completeRead(); /* old card result arrives after remount */
	f.step();
	check(stub.pendingCallback != nullptr, "stale result discarded and refetched");

	stub.completeRead(); /* new card result */
	f.step();
	f.values[2] = 200;
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiMessage(f.algorithm, 0x90, 64, 100);
	check(stub.messages.size() == 2, "new card scale sends bend then on");
	check(stub.messages[0].b1 == 0x00 && stub.messages[0].b2 == 0x40,
	      "remount uses 12-TET, not the old JI bend");
}

/* An empty-library remount still clamps the selector to max 0. */
void testEmptyLibraryClamp()
{
	Fixture f;
	stub.numScl = 2;
	f.step();
	f.values[1] = 1;
	f.changed(1);

	stub.mounted = false;
	f.step();
	stub.numScl = 0;
	stub.mounted = true;
	f.step();

	check(f.values[1] == 0, "empty library resets selector to zero");
	check(f.algorithm->parameters[1].max == 0, "empty library parameter max is zero");
}

/* nt_helper's community Scala editor matches the parameter name pattern. */
void testScaleParameterName()
{
	Fixture f;
	const _NT_parameter& scale = f.algorithm->parameters[1];
	check(std::strcmp(scale.name, "Scale File") == 0, "scale parameter is named Scale File");
	check(scale.unit == kNT_unitConfirm, "scale parameter uses confirm unit");

	const _NT_parameter& output = f.algorithm->parameters[4];
	check(std::strcmp(output.name, "Output") == 0, "output parameter present");
	check(output.max == 2, "output parameter has three modes");
}

/* The Output parameter selects the MPE channel map. */
void testOutputModeForwarding()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[4] = 2; /* MPE */
	f.changed(4);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiMessage(f.algorithm, 0x90, 60, 100);
	check(stub.messages.size() == 2, "MPE sends bend then on");
	check(stub.messages[0].b0 == 0xE1 && stub.messages[1].b0 == 0x91, "MPE uses member channel 2");

	/* Changing output while armed must push Arm back to Off. */
	f.resetStub();
	f.values[4] = 0;
	f.changed(4);
	f.step();
	check(stub.setCalls == 1 && stub.setValue == 0, "output change syncs Arm off");
}

/* A loaded .scl file rebuilds the 128-key table. */
void testScaleLoadRetunes()
{
	Fixture f;
	stub.notes = justNotes;
	stub.scaleName = "just.scl";
	f.step(); /* observe the mounted card */
	f.resetStub();
	f.changed(0);
	f.changed(1);
	f.changed(2);
	check(stub.readCalls == 1, "scale request issued");
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiMessage(f.algorithm, 0x90, 64, 100);
	check(stub.messages.size() == 2, "JI sends bend then on");
	check(stub.messages[0].b0 == 0xE0 && stub.messages[0].b1 == 0x4F && stub.messages[0].b2 == 0x3B, "JI E4 bend");
	check(stub.messages[1].b0 == 0x90 && stub.messages[1].b1 == 0x40 && stub.messages[1].b2 == 0x64, "JI E4 on");
}

/* An asynchronous read completes on a later step, not in parameterChanged. */
void testDeferredScaleLoad()
{
	Fixture f;
	stub.notes = justNotes;
	stub.deferCallback = true;
	f.step(); /* mount starts the deferred read */
	check(stub.pendingCallback != nullptr, "read is in flight");

	/* Arming while the scale is still loading must be refused. */
	f.changed(2);
	f.resetStub();
	f.values[3] = 1;
	f.changed(3);
	f.step();
	check(stub.setCalls == 1 && stub.setValue == 0, "arm refused while loading");

	stub.completeRead();

	/* Editing R after completion but before poll must use the new range. */
	f.values[2] = 400;
	f.changed(2);
	f.step();
	f.resetStub();
	f.values[3] = 1;
	f.changed(3);
	check(stub.setCalls == 0, "arm accepted after load completes");

	factory()->midiMessage(f.algorithm, 0x90, 64, 100);
	check(stub.messages.size() == 2, "deferred JI sends bend then on");
	check(stub.messages[0].b0 == 0xE0 && stub.messages[0].b1 == 0x68 && stub.messages[0].b2 == 0x3D, "deferred JI E4 bend at R=4");
}

/* Reloading the same file must rebuild after the table was invalidated. */
void testIdenticalScaleReload()
{
	Fixture f;
	f.step(); /* observe the mounted card */
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);

	f.resetStub();
	f.changed(1); /* same index: panic, invalidate, load, rebuild */
	f.resetStub();
	f.values[3] = 1;
	f.changed(3);
	check(stub.setCalls == 0, "identical reload keeps the table armable");
}

/* The SD file count cannot narrow the int16 parameter range below zero. */
void testScaleCountClamp()
{
	Fixture f;
	stub.numScl = 32769;
	f.step();
	check(f.algorithm->parameters[1].max == 32767, "SD count clamps to parameter maximum");
}

/* A failed .scl read must not fall back to a stale table when R changes. */
void testScaleLoadFailureRefusesArm()
{
	Fixture f;
	stub.numNotes = 0;
	f.step(); /* mount triggers the failed read */
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.resetStub();

	f.values[3] = 1;
	f.changed(3);
	f.step();
	check(stub.setCalls == 1, "failed scale arms refused");
	check(stub.setValue == 0, "failed scale writes Arm off");
}

} // namespace

extern "C" {

const _NT_globals NT_globals = { 0, 0, nullptr, 0, 0, 0 };
uint8_t NT_screen[128 * 64];

int32_t NT_algorithmIndex(const _NT_algorithm*)
{
	return stub.algorithmIndex;
}

uint32_t NT_parameterOffset(void)
{
	return stub.parameterOffset;
}

void NT_setParameterFromUi(uint32_t algorithmIndex, uint32_t parameter, int16_t value)
{
	stub.setAlgorithm = algorithmIndex;
	stub.setParameter = parameter;
	stub.setValue = value;
	++stub.setCalls;

	/* Simulate the host: update v and re-enter parameterChanged. */
	if (stub.algorithm && parameter >= stub.parameterOffset)
	{
		const int p = (int)(parameter - stub.parameterOffset);
		if (p >= 0 && p < 5)
		{
			const_cast<int16_t*>(stub.algorithm->v)[p] = value;
			factory()->parameterChanged(stub.algorithm, p);
		}
	}
}

void NT_sendMidi3ByteMessage(uint32_t destination, uint8_t b0, uint8_t b1, uint8_t b2)
{
	stub.lastDestination = destination;
	stub.messages.push_back(Message{ b0, b1, b2 });
}

void NT_drawText(int, int, const char*, int, _NT_textAlignment, _NT_textSize)
{
}

int NT_intToString(char* buffer, int32_t value)
{
	return std::snprintf(buffer, 16, "%d", value);
}

void NT_updateParameterDefinition(uint32_t, uint32_t)
{
	++stub.updateCalls;
}

bool NT_isSdCardMounted(void)
{
	return stub.mounted;
}

uint32_t NT_getNumScl(void)
{
	return stub.numScl;
}

void NT_getSclInfo(uint32_t index, _NT_sclInfo& info)
{
	info.name = index < stub.numScl ? stub.scaleName : nullptr;
}

bool NT_readScl(_NT_sclRequest& request)
{
	++stub.readCalls;
	if (!stub.mounted || stub.numNotes == 0 || !stub.notes)
	{
		request.error = true;
		return false;
	}

	const uint32_t count = stub.numNotes < request.maxNotes ? stub.numNotes : request.maxNotes;
	for (uint32_t i = 0; i < count; ++i)
		request.notes[i] = stub.notes[i];
	request.numNotes = count;
	request.error = false;

	if (request.nameBuffer && request.nameBufferSize > 0)
	{
		std::strncpy(request.nameBuffer, stub.scaleName, request.nameBufferSize - 1);
		request.nameBuffer[request.nameBufferSize - 1] = 0;
	}

	if (stub.deferCallback)
	{
		stub.pendingCallback = request.callback;
		stub.pendingData = request.callbackData;
	}
	else if (request.callback)
	{
		request.callback(request.callbackData);
	}
	return true;
}

} // extern "C"

int main()
{
	testVersionAndFactory();
	testCC120Ignored();
	testRealtimeStopKeepsArm();
	testArmRefusedOnInvalidTable();
	testConfigChangeSyncsArm();
	testMidiForwarding();
	testNoCardFallback();
	testArmedRemountClampDisarms();
	testAmbiguousScalesRejected();
	testStaleReadAcrossRemount();
	testEmptyLibraryClamp();
	testScaleParameterName();
	testOutputModeForwarding();
	testScaleLoadRetunes();
	testDeferredScaleLoad();
	testIdenticalScaleReload();
	testScaleCountClamp();
	testScaleLoadFailureRefusesArm();

	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all retuner adapter tests passed\n");
	return 0;
}
