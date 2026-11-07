/*
 * disting NT adapter for the static MIDI retuner core.
 *
 * The plugin is a MIDI-only utility: it forwards no audio, only retuned
 * note/bend/controller traffic to the MIDI breakout. Tuning comes from the
 * disting NT Scala (.scl) library; see docs/retuner.md for the user-facing
 * behaviour and remaining hardware prerequisites.
 */
#include <math.h>
#include <new>
#include <stdint.h>
#include <distingnt/api.h>
#include <distingnt/microtuning.h>
#include <distingnt/wav.h>

#include "retuner_core.h"

namespace {

enum
{
	kParamChannel,
	kParamScale,
	kParamBendRange,
	kParamArm,
	kParamOutput,

	kNumParams,
};

static char const * const enumArm[] = { "Off", "On" };
static char const * const enumOutput[] = { "Monumatic", "Per Ch Bend", "MPE" };

/* Bend range is configured in hundredths of a semitone; 0 means unset. */
static const _NT_parameter parameters[] = {
	{ .name = "Channel", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Scale File", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Bend Range", .min = 0, .max = 4800, .def = 0, .unit = kNT_unitSemitones, .scaling = kNT_scaling100, .enumStrings = NULL },
	{ .name = "Arm", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumArm },
	{ .name = "Output", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumOutput },
};

static const uint8_t page1[] = { kParamChannel, kParamScale, kParamBendRange, kParamOutput, kParamArm };

static const _NT_parameterPage pages[] = {
	{ .name = "Retuner", .numParams = ARRAY_SIZE(page1), .params = page1 },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

struct _retunerAlgorithm : public _NT_algorithm
{
	_retunerAlgorithm()
		: engine(), params(), syncing(false), request(), notes(), scaleName(),
		  pendingScale(0), cardMounted(false), awaitingCallback(false),
		  dirty(false), scaleError(false), armOffPending(false) {}

	retuner::Engine engine;
	_NT_parameter params[kNumParams];
	bool syncing;

	_NT_sclRequest request;
	_NT_sclNote notes[retuner::kMaxScaleNotes];
	char scaleName[24];
	int pendingScale;
	bool cardMounted;
	bool awaitingCallback;
	bool dirty;
	bool scaleError;
	bool armOffPending;
};

/* Send one 3-byte channel message to the MIDI breakout only. */
void sendMidi(void*, uint8_t b0, uint8_t b1, uint8_t b2)
{
	NT_sendMidi3ByteMessage(kNT_destinationBreakout, b0, b1, b2);
}

/* Write the Arm parameter back to Off from step(), not re-entrantly. */
void writeArmOff(_retunerAlgorithm* algorithm)
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
		algorithm->armOffPending = true;
}

void syncScale(_retunerAlgorithm* algorithm, int value)
{
	const int32_t index = NT_algorithmIndex(algorithm);
	if (index < 0)
		return;
	algorithm->syncing = true;
	NT_setParameterFromUi((uint32_t)index, kParamScale + NT_parameterOffset(), (int16_t)value);
	algorithm->syncing = false;
}

/*
 * log2 for positive values without importing a firmware math symbol.
 * The atanh series converges quickly after normalising into [1, 2).
 */
double log2Positive(double value)
{
	int exponent = 0;
	while (value >= 2.0)
	{
		value *= 0.5;
		++exponent;
	}
	while (value < 1.0)
	{
		value *= 2.0;
		--exponent;
	}

	const double z = (value - 1.0) / (value + 1.0);
	const double z2 = z * z;
	double term = z;
	double sum = z;
	for (int k = 3; k <= 25; k += 2)
	{
		term *= z2;
		sum += term / (double)k;
	}
	return (double)exponent + 2.0 * sum * 1.4426950408889634;
}

/*
 * Convert disting NT's parsed .scl notes into core octave intervals.
 * Degree 0 (1/1) is implicit; the last note is the formal octave.
 */
bool buildScale(const _NT_sclNote* notes, uint32_t numNotes, retuner::Scale& scale)
{
	if (numNotes == 0 || numNotes > retuner::kMaxScaleNotes)
		return false;

	scale.count = (uint8_t)numNotes;
	for (uint32_t i = 0; i < numNotes; ++i)
	{
		double octaves;
		if (notes[i].isRatio())
		{
			/* The parser stores a ratio denominator as a negative int32. */
			if (notes[i].numeratorValue <= 0 ||
			    notes[i].denominatorValue >= 0 ||
			    notes[i].denominatorValue == INT32_MIN)
				return false;
			const int32_t numerator = notes[i].numeratorValue;
			const int32_t denominator = -notes[i].denominatorValue;
			octaves = log2Positive((double)numerator / (double)denominator);
		}
		else
		{
			octaves = notes[i].octaves;
		}

		if (!isfinite(octaves))
			return false;
		scale.interval[i] = octaves;
	}

	/* The formal octave must move upward or the pattern does not repeat. */
	return isfinite(scale.interval[scale.count - 1]) &&
	       scale.interval[scale.count - 1] > 0.0;
}

void applyLoadedScale(_retunerAlgorithm* algorithm)
{
	retuner::Scale scale;
	if (algorithm->request.error ||
	    !buildScale(algorithm->request.notes, algorithm->request.numNotes, scale))
	{
		algorithm->scaleError = true;
		if (algorithm->engine.isArmed())
			algorithm->engine.panic(retuner::kPanicScaleFailure);
		algorithm->engine.invalidateTable();
		return;
	}

	algorithm->scaleError = false;
	algorithm->engine.configureScale(scale, algorithm->engine.bendRangeSemitones());
}

void startScaleRequest(_retunerAlgorithm* algorithm, int index)
{
	if (index < 0)
		index = 0;
	if (index > 32767)
		index = 32767;

	algorithm->request.index = (uint32_t)index;
	algorithm->request.error = false;
	algorithm->request.numNotes = 0;
	algorithm->dirty = false;
	algorithm->scaleError = false;
	algorithm->engine.invalidateTable();

	algorithm->awaitingCallback = true;
	if (!NT_readScl(algorithm->request))
	{
		algorithm->awaitingCallback = false;
		algorithm->scaleError = true;
	}
}

/*
 * Apply a completed read and, if the user moved on while it was in flight,
 * start the newest request. NT_readScl may complete synchronously.
 */
void pollScale(_retunerAlgorithm* algorithm)
{
	for (int guard = 0; guard < 3; ++guard)
	{
		if (algorithm->dirty)
		{
			algorithm->dirty = false;
			applyLoadedScale(algorithm);
			continue;
		}
		if (!algorithm->awaitingCallback &&
		    (int)algorithm->request.index != algorithm->pendingScale)
		{
			startScaleRequest(algorithm, algorithm->pendingScale);
			continue;
		}
		break;
	}
}

void scaleCallback(void* callbackData)
{
	_retunerAlgorithm* algorithm = static_cast<_retunerAlgorithm*>(callbackData);
	algorithm->awaitingCallback = false;
	algorithm->dirty = true;
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

	for (int i = 0; i < (int)ARRAY_SIZE(parameters); ++i)
		algorithm->params[i] = parameters[i];
	algorithm->parameters = algorithm->params;
	algorithm->parameterPages = &parameterPages;
	algorithm->engine.setOutput(retuner::MidiOut{ nullptr, sendMidi });

	algorithm->request.notes = algorithm->notes;
	algorithm->request.maxNotes = ARRAY_SIZE(algorithm->notes);
	algorithm->request.nameBuffer = algorithm->scaleName;
	algorithm->request.nameBufferSize = sizeof algorithm->scaleName;
	algorithm->request.descriptionBuffer = nullptr;
	algorithm->request.descriptionBufferSize = 0;
	algorithm->request.callback = scaleCallback;
	algorithm->request.callbackData = algorithm;

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
			if (algorithm->engine.arm())
				algorithm->armOffPending = false;
			else
				algorithm->armOffPending = true;
		}
		else
		{
			algorithm->engine.disarm();
			algorithm->armOffPending = false;
		}
		break;
	case kParamChannel:
		algorithm->engine.setChannel(algorithm->v[kParamChannel] - 1);
		syncArmIfDisarmed(algorithm);
		break;
	case kParamScale:
	{
		algorithm->engine.disarm();
		int index = algorithm->v[kParamScale];
		if (index < 0)
			index = 0;
		if (index > 32767)
			index = 32767;
		if (index != algorithm->v[kParamScale])
			syncScale(algorithm, index);
		algorithm->pendingScale = index;
		algorithm->engine.invalidateTable();
		if (!algorithm->awaitingCallback)
			startScaleRequest(algorithm, algorithm->pendingScale);
		pollScale(algorithm);
		syncArmIfDisarmed(algorithm);
	}
		break;
	case kParamOutput:
		algorithm->engine.setOutputMode((retuner::OutputMode)algorithm->v[kParamOutput]);
		syncArmIfDisarmed(algorithm);
		break;
	case kParamBendRange:
		algorithm->engine.configureBendRange(algorithm->v[kParamBendRange] / 100.0);
		if (algorithm->awaitingCallback || algorithm->dirty || algorithm->scaleError)
			algorithm->engine.invalidateTable();
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
	(void)busFrames;
	(void)numFramesBy4;

	_retunerAlgorithm* algorithm = static_cast<_retunerAlgorithm*>(self);
	const bool mounted = NT_isSdCardMounted();
	if (mounted != algorithm->cardMounted)
	{
		algorithm->cardMounted = mounted;
		if (mounted)
		{
			const uint32_t count = NT_getNumScl();
			uint32_t maximum32 = count > 0 ? count - 1 : 0;
			if (maximum32 > 32767)
				maximum32 = 32767;
			const int16_t maximum = (int16_t)maximum32;
			if (algorithm->params[kParamScale].max != maximum)
			{
				algorithm->params[kParamScale].max = maximum;
				const int32_t index = NT_algorithmIndex(algorithm);
				if (index >= 0)
					NT_updateParameterDefinition((uint32_t)index, kParamScale);
			}

			if (count > 0)
			{
				int index = algorithm->v[kParamScale];
				if (index < 0)
					index = 0;
				if (index > maximum)
				{
					index = maximum;
					syncScale(algorithm, index);
				}
				algorithm->pendingScale = index;
				if (!algorithm->engine.isArmed() &&
				    !algorithm->awaitingCallback && !algorithm->dirty)
					startScaleRequest(algorithm, index);
			}
		}
	}

	pollScale(algorithm);
	syncArmIfDisarmed(algorithm);
	if (algorithm->armOffPending)
	{
		algorithm->armOffPending = false;
		writeArmOff(algorithm);
	}
}

bool draw(_NT_algorithm* self)
{
	const _retunerAlgorithm* algorithm = static_cast<const _retunerAlgorithm*>(self);
	const retuner::Engine& engine = algorithm->engine;
	char buff[32];

	NT_drawText(0, 30, "Scale:");
	NT_drawText(56, 30, algorithm->scaleName[0] ? algorithm->scaleName : "12-TET");

	NT_drawText(0, 40, "Arm:");
	NT_drawText(40, 40, engine.isArmed() ? "On" : "Off");
	NT_drawText(72, 40, "Voices:");
	NT_intToString(buff, engine.voicesInUse());
	NT_drawText(120, 40, buff);

	NT_drawText(0, 50, "Table:");
	NT_drawText(56, 50, engine.isTableValid() ? "Valid" : "Invalid");

	const char* status = nullptr;
	if (algorithm->scaleError)
		status = "Scale load failed";
	else if (engine.faultState() != retuner::kFaultNone)
		status = "Fault: overflow";
	else if (engine.lastPanic() != retuner::kPanicNone)
	{
		switch (engine.lastPanic())
		{
		case retuner::kPanicAllSoundOff:
			status = "Panic: CC120";
			break;
		case retuner::kPanicResetControllers:
			status = "Panic: CC121";
			break;
		case retuner::kPanicStop:
			status = "Panic: Stop";
			break;
		case retuner::kPanicReset:
			status = "Panic: Reset";
			break;
		case retuner::kPanicOverflow:
			status = "Panic: overflow";
			break;
		case retuner::kPanicConfig:
			status = "Panic: config";
			break;
		case retuner::kPanicScaleFailure:
			status = "Panic: scale";
			break;
		case retuner::kPanicDisable:
			status = "Panic: disable";
			break;
		default:
			break;
		}
	}
	else if (!engine.isTableValid())
	{
		switch (engine.tableError())
		{
		case retuner::kTableBendUnset:
			status = "Set Bend Range";
			break;
		case retuner::kTableBendInvalid:
			status = "Bad Bend Range";
			break;
		case retuner::kTableBendTooSmall:
			status = "Bend Range low";
			break;
		case retuner::kTableScaleInvalid:
			status = "Load Scale File";
			break;
		default:
			status = "Table invalid";
			break;
		}
	}
	if (status)
		NT_drawText(0, 60, status);

	return false;
}

int parameterString(_NT_algorithm* self, int p, int v, char* buff)
{
	if (p != kParamScale)
		return 0;

	const _retunerAlgorithm* algorithm = static_cast<const _retunerAlgorithm*>(self);
	_NT_sclInfo info;
	NT_getSclInfo((uint32_t)(v < 0 ? 0 : v), info);
	const char* name = (info.name && info.name[0]) ? info.name :
	                   (algorithm->scaleName[0] ? algorithm->scaleName : "12-TET");

	int length = 0;
	while (length < kNT_parameterStringSize - 1 && name[length])
	{
		buff[length] = name[length];
		++length;
	}
	buff[length] = 0;
	return length;
}

} // namespace

static const _NT_factory factory = {
	.guid = NT_MULTICHAR( 'T', 'h', 'R', 't' ),
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
	.parameterString = parameterString,
};

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
