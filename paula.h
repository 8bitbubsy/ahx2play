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

#define BPM_FRAC_BITS 52
#define BPM_FRAC_SCALE (1ULL << BPM_FRAC_BITS)
#define BPM_FRAC_MASK (BPM_FRAC_SCALE-1)

// main crystal oscillator for PAL Amiga systems
#define AMIGA_PAL_XTAL_HZ 28375160

#define AMIGA_PAL_CCK_HZ (AMIGA_PAL_XTAL_HZ / 8.0)
#define PAULA_PAL_CLK AMIGA_PAL_CCK_HZ
#define CIA_PAL_CLK (AMIGA_PAL_CCK_HZ / 5.0)

#define PAULA_VOICES 4

typedef struct audio_t
{
	volatile bool playing, pause;
	int32_t outputFreq, masterVol, stereoSeparation;
	int32_t tickSampleCounter;
	uint32_t samplesPerTickInt;
	uint64_t tickSampleCounterFrac, samplesPerTickFrac;
} audio_t;

typedef struct voice_t
{
	volatile bool active;

	// internal registers
	bool sampleJustStarted, nextSampleStage;
	int8_t AUD_DAT[2]; // DMA data buffer
	const int8_t *location; // current location
	uint16_t lengthCounter; // current length
	int32_t sampleCounter; // how many bytes left in AUD_DAT
	float fSample; // currently held sample point (multiplied by volume)
	float fDelta, fPhase;
	float fBlepDelta, fBlepPhase;

	// registers modified by Paula functions
	const int8_t *storedLocation; // data pointer
	uint16_t storedLength;
	float fStoredVol, fStoredDelta;
} paulaVoice_t;

void resetAudioDithering(void);

double amigaCIAPeriod2Hz(uint16_t period);
bool amigaSetCIAPeriod(uint16_t period); // replayer ticker speed

bool paulaInit(int32_t audioFrequency);
void paulaClose(void);

void paulaSetMasterVolume(int32_t vol);
void paulaSetStereoSeparation(int32_t percentage); // 0..100 (percentage)

void paulaTogglePause(void);
void paulaOutputSamples(int16_t *stream, int32_t numSamples);
void paulaSetDMACON(uint16_t bits);
void paulaSetPeriod(int32_t ch, uint16_t period);
void paulaSetVolume(int32_t ch, uint16_t vol);
void paulaSetLength(int32_t ch, uint16_t len);
void paulaSetData(int32_t ch, const int8_t *src);
void paulaMixSamples(int16_t *target, uint32_t numSamples);

extern audio_t audio; // paula.c
extern paulaVoice_t paula[PAULA_VOICES]; // paula.c
