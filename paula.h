#pragma once

#include <stdint.h>
#include <stdbool.h>

// AUDIO DRIVERS
#if defined AUDIODRIVER_SDL
#include "audiodrivers/sdl/sdldriver.h"
#elif defined AUDIODRIVER_WINMM
#include "audiodrivers/winmm/winmm.h"
#else
// Read "audiodrivers/how_to_write_drivers.txt"
#endif

// main crystal oscillator for PAL Amiga systems
#define AMIGA_PAL_XTAL_HZ 28375160

#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define PAULA_PAL_CLK AMIGA_PAL_CCK_HZ
#define CIA_PAL_CLK (AMIGA_PAL_CCK_HZ / 5.0)

#define AMIGA_VOICES 4

typedef struct audio_t
{
	volatile bool playing, pause;
	int32_t outputFreq, masterVol, stereoSeparation;
	int64_t tickSampleCounter64, samplesPerTick64;
} audio_t;

typedef struct voice_t
{
	volatile bool DMA_active;

	// internal values (don't modify directly!)
	int8_t AUD_DAT[2]; // DMA data buffer
	const int8_t *location; // current location
	uint16_t lengthCounter; // current length
	int32_t sampleCounter; // how many bytes left in AUD_DAT
	double dSample; // current sample point

	// registers modified by Paula functions
	const int8_t *AUD_LC; // location
	uint16_t AUD_LEN; // length (in words)
	double AUD_PER_delta; // delta
	double AUD_VOL; // volume

	double dDelta, dPhase;

	// for BLEP synthesis
	double dLastDelta, dLastPhase, dBlepOffset, dDeltaMul;

	// period cache
	int32_t oldPeriod;
	double dOldVoiceDelta, dOldVoiceDeltaMul;
} paulaVoice_t;

void paulaClearFilterState(void);
void resetCachedMixerPeriod(void);
void resetAudioDithering(void);

double amigaCIAPeriod2Hz(uint16_t period);
bool amigaSetCIAPeriod(uint16_t period); // replayer ticker speed

void paulaMixSamples(int16_t *target, int32_t numSamples);
bool paulaInit(int32_t audioFrequency);
void paulaClose(void);

void paulaSetMasterVolume(int32_t vol);
void paulaSetStereoSeparation(int32_t percentage); // 0..100 (percentage)

void paulaTogglePause(void);
void paulaOutputSamples(int16_t *stream, int32_t numSamples);
void paulaStopAllDMAs(void);
void paulaStartAllDMAs(void);
void paulaSetPeriod(int32_t ch, uint16_t period);
void paulaSetVolume(int32_t ch, uint16_t vol);
void paulaSetLength(int32_t ch, uint16_t len);
void paulaSetData(int32_t ch, const int8_t *src);

extern audio_t audio; // paula.c
extern paulaVoice_t paula[AMIGA_VOICES]; // paula.c
