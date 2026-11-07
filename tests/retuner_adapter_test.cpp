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
};

Stub stub;

int failures = 0;

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
	int16_t values[4];

	Fixture()
	{
		_NT_algorithmRequirements req;
		factory()->calculateRequirements(req, nullptr);
		static char storage[8192];
		_NT_algorithmMemoryPtrs ptrs = { reinterpret_cast<uint8_t*>(storage), nullptr, nullptr, nullptr };
		algorithm = factory()->construct(ptrs, req, nullptr);
		std::memset(values, 0, sizeof values);
		values[0] = 1;   /* channel 1 */
		values[1] = 0;   /* 12-TET */
		values[2] = 200; /* 2.00 semitones */
		values[3] = 0;   /* disarmed */
		algorithm->v = values;
		stub.algorithm = algorithm;
	}

	void changed(int p) { factory()->parameterChanged(algorithm, p); }
	static void resetStub()
	{
		stub.setCalls = 0;
		stub.lastDestination = 0;
		stub.messages.clear();
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

/* Finding 1: the host parameter index must include NT_parameterOffset(). */
void testArmSyncUsesParameterOffset()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.resetStub();

	/* Valid table: arming does not need a sync write. */
	f.values[3] = 1;
	f.changed(3);
	check(stub.setCalls == 0, "valid arm sends no sync write");

	/* MIDI panic disarms and must push Arm back to Off. */
	factory()->midiMessage(f.algorithm, 0xB0, 120, 0);
	check(stub.setCalls == 1, "MIDI panic syncs Arm");
	check(stub.setAlgorithm == 0, "sync uses algorithm index");
	check(stub.setParameter == (uint32_t)(3 + 7), "sync uses parameter offset");
	check(stub.setValue == 0, "sync writes Off");
}

/* Finding 2: realtime Stop/Reset also sync Arm. */
void testRealtimePanicSyncsArm()
{
	Fixture f;
	f.changed(0);
	f.changed(1);
	f.changed(2);
	f.values[3] = 1;
	f.changed(3);
	f.resetStub();

	factory()->midiRealtime(f.algorithm, 0xFC);
	check(stub.setCalls == 1, "Stop syncs Arm");
	check(stub.setParameter == (uint32_t)(3 + 7), "Stop sync uses offset");
	check(stub.setValue == 0, "Stop sync writes Off");
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
		if (p >= 0 && p < 4)
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

} // extern "C"

int main()
{
	testVersionAndFactory();
	testArmSyncUsesParameterOffset();
	testRealtimePanicSyncsArm();
	testArmRefusedOnInvalidTable();
	testConfigChangeSyncsArm();
	testMidiForwarding();

	if (failures)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("all retuner adapter tests passed\n");
	return 0;
}
