/*
 * disting NT adapter for the static MIDI retuner core.
 *
 * The plugin is a MIDI-only utility: it forwards no audio, only retuned
 * note/bend/controller traffic to the MIDI breakout. See docs/retuner.md
 * for the player-facing contract and remaining hardware prerequisites.
 */
#include <new>
#include <stdint.h>
#include <distingnt/api.h>

#include "retuner_core.h"

namespace {

enum
{
	kParamChannel,
	kParamTuning,
	kParamBendRange,
	kParamArm,
};

static char const * const enumTuning[] = { "12-TET", "JI C" };
static char const * const enumArm[] = { "Off", "On" };

/* Bend range is configured in hundredths of a semitone; 0 means unset. */
static const _NT_parameter parameters[] = {
	{ .name = "Channel", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Tuning", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumTuning },
	{ .name = "Bend Range", .min = 0, .max = 4800, .def = 0, .unit = kNT_unitSemitones, .scaling = kNT_scaling100, .enumStrings = NULL },
	{ .name = "Arm", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumArm },
};

static const uint8_t page1[] = { kParamChannel, kParamTuning, kParamBendRange, kParamArm };

static const _NT_parameterPage pages[] = {
	{ .name = "Retuner", .numParams = ARRAY_SIZE(page1), .params = page1 },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

struct _retunerAlgorithm : public _NT_algorithm
{
	_retunerAlgorithm() : engine(), syncing(false) {}

	retuner::Engine engine;
	bool syncing;
};

/* Send one 3-byte channel message to the MIDI breakout only. */
void sendMidi(void*, uint8_t b0, uint8_t b1, uint8_t b2)
{
	NT_sendMidi3ByteMessage(kNT_destinationBreakout, b0, b1, b2);
}

/* Re-sync the Arm parameter without recursing through parameterChanged. */
void syncArmOff(_retunerAlgorithm* algorithm)
{
	const int32_t index = NT_algorithmIndex(algorithm);
	if (index < 0)
		return;
	algorithm->syncing = true;
	NT_setParameterFromUi((uint32_t)index, kParamArm + NT_parameterOffset(), 0);
	algorithm->syncing = false;
}

void syncArmIfDisarmed(_retunerAlgorithm* algorithm)
{
	if (!algorithm->engine.isArmed() && algorithm->v[kParamArm])
		syncArmOff(algorithm);
}

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* specifications)
{
	(void)specifications;
	req.numParameters = ARRAY_SIZE(parameters);
	req.sram = sizeof(_retunerAlgorithm);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications)
{
	(void)req;
	(void)specifications;
	_retunerAlgorithm* algorithm = new (ptrs.sram) _retunerAlgorithm();
	algorithm->parameters = parameters;
	algorithm->parameterPages = &parameterPages;
	algorithm->engine.setOutput(retuner::MidiOut{ nullptr, sendMidi });
	return algorithm;
}

void parameterChanged(_NT_algorithm* self, int p)
{
	_retunerAlgorithm* algorithm = static_cast<_retunerAlgorithm*>(self);
	if (algorithm->syncing)
		return;

	switch (p)
	{
	case kParamArm:
		if (algorithm->v[kParamArm])
		{
			if (!algorithm->engine.arm())
				syncArmOff(algorithm);
		}
		else
		{
			algorithm->engine.disarm();
		}
		break;
	case kParamChannel:
		algorithm->engine.setChannel(algorithm->v[kParamChannel] - 1);
		syncArmIfDisarmed(algorithm);
		break;
	case kParamTuning:
		algorithm->engine.configureTuning(algorithm->v[kParamTuning], algorithm->engine.bendRangeSemitones());
		syncArmIfDisarmed(algorithm);
		break;
	case kParamBendRange:
		algorithm->engine.configureTuning(algorithm->engine.tuningMode(), algorithm->v[kParamBendRange] / 100.0);
		syncArmIfDisarmed(algorithm);
		break;
	}
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2)
{
	_retunerAlgorithm* algorithm = static_cast<_retunerAlgorithm*>(self);
	algorithm->engine.processChannelMessage(byte0, byte1, byte2);
	syncArmIfDisarmed(algorithm);
}

void midiRealtime(_NT_algorithm* self, uint8_t byte)
{
	_retunerAlgorithm* algorithm = static_cast<_retunerAlgorithm*>(self);
	algorithm->engine.processRealtime(byte);
	syncArmIfDisarmed(algorithm);
}

/* No audio path: the plugin only emits MIDI. */
void step(_NT_algorithm* self, float* busFrames, int numFramesBy4)
{
	(void)self;
	(void)busFrames;
	(void)numFramesBy4;
}

bool draw(_NT_algorithm* self)
{
	const _retunerAlgorithm* algorithm = static_cast<const _retunerAlgorithm*>(self);
	const retuner::Engine& engine = algorithm->engine;
	char buff[32];

	NT_drawText(0, 20, "Arm:");
	NT_drawText(40, 20, engine.isArmed() ? "On" : "Off");

	NT_drawText(0, 30, "Voices:");
	NT_intToString(buff, engine.voicesInUse());
	NT_drawText(64, 30, buff);

	NT_drawText(0, 40, "Table:");
	NT_drawText(56, 40, engine.isTableValid() ? "Valid" : "Invalid");

	if (engine.faultState() != retuner::kFaultNone)
		NT_drawText(0, 50, "Fault: overflow");

	return false;
}

static const _NT_factory factory = {
	.guid = retuner::kGuid,
	.name = "Retuner",
	.description = "Static MIDI retuner: one input channel to eight retuned voices",
	.numSpecifications = 0,
	.specifications = NULL,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = parameterChanged,
	.step = step,
	.draw = draw,
	.midiRealtime = midiRealtime,
	.midiMessage = midiMessage,
	.tags = kNT_tagUtility,
};

} // namespace

#if defined(__arm__)
_NT_DRAM_SECTION
#endif
uintptr_t pluginEntry(_NT_selector selector, uint32_t data)
{
	switch (selector)
	{
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 1;
	case kNT_selector_factoryInfo:
		return (uintptr_t)((data == 0) ? &factory : NULL);
	}
	return 0;
}
