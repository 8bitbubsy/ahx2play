#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "paula.h"

enum
{
	ERR_SUCCESS         = 0,
	ERR_OUT_OF_MEMORY   = 1,
	ERR_AUDIO_DEVICE    = 2,
	ERR_FILE_IO         = 3,
	ERR_NOT_AN_AHX      = 4,
	ERR_NO_WAVES        = 5,
	ERR_SONG_NOT_LOADED = 6
};

#define AHX_HIGHEST_CIA_PERIOD 14209 /* ~49.92Hz */
#define AHX_DEFAULT_CIA_PERIOD AHX_HIGHEST_CIA_PERIOD

#define NOIZE_SIZE (0x280*3)
#define WAV_FILTER_LENGTH (252 + 252 + (0x80 * 32) + NOIZE_SIZE)

#define CLAMP16(i) if ((int16_t)(i) != i) i = 0x7FFF ^ (i >> 31)
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

#define SWAP16(x) \
( \
	(((uint16_t)((x) & 0x00FF)) << 8) | \
	(((uint16_t)((x) & 0xFF00)) >> 8)   \
)

// bit-rotate macros
#define ROL32(d, x) (d = (d << (x)) | (d >> (32-(x))))
#define ROR32(d, x) (d = (d >> (x)) | (d << (32-(x))))

#ifdef _MSC_VER
#pragma pack(push)
#pragma pack(1)
#endif
typedef struct
{
	uint8_t Volume;
	uint8_t filterSpeedWavelength; // NEWv1.66
	uint8_t aFrames;
	uint8_t aVolume;
	uint8_t dFrames;
	uint8_t dVolume;
	uint8_t sFrames;
	uint8_t rFrames;
	uint8_t rVolume;

	uint8_t unused1;
	uint8_t unused2;
	uint8_t unused3;

	uint8_t filterLowerLimit; // NEWv1.66!!
	uint8_t vibratoDelay;
	uint8_t vibratoDepth;
	uint8_t vibratoSpeed;

	uint8_t squareLowerLimit;
	uint8_t squareUpperLimit;
	uint8_t squareSpeed;
	uint8_t filterUpperLimit; // NEWv1.66!!

	uint8_t perfSpeed;
	uint8_t perfLength;

	uint8_t perfList[4*256]; // 8bb: room for max perfLength entries!
}
#ifdef __GNUC__
__attribute__((packed))
#endif
instrument_t;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

typedef struct // 8bb: channel structure
{
	uint8_t Track;
	int8_t Transpose;
	uint8_t NextTrack;
	int8_t NextTranspose;

	int16_t adsr; // 8 bit/8 bit floating! (8bb: 8.8fp)
	uint8_t aFrames; // somewhere bytes?!
	uint8_t dFrames;
	uint8_t sFrames; // now there's an a_vol too!
	uint8_t rFrames;
	int16_t aDelta; // 8 bit/8 bit floating! (8bb: 8.8fp)
	int16_t dDelta; // 8 bit/8 bit floating! (8bb: 8.8fp)
	int16_t rDelta; // 8 bit/8 bit floating! (8bb: 8.8fp)

	instrument_t *Instrument; // ^Current_Instrument
	uint8_t Waveform; // 1..4 (or 0..3 senseless?)
	uint8_t Wavelength; // 0..5: 4/8/10/20/40/80 ($)
	int16_t InstrPeriod; // !P!
	int16_t TrackPeriod; // !P!
	int16_t VibratoPeriod; // !P!
	uint8_t NoteMaxVolume; // instr. max/cxx override tracked
	uint8_t perfSubVolume; // cxx override perfed!
	uint8_t TrackMasterVolume; // real maximum volume!

	bool NewWaveform; // flag!
	bool PlantSquare; // flag! now baused by 9xx!
	bool SquareReverse; // flag!
	bool IgnoreSquare; // PADDY/KELLY FAMILY RULEZ! (8bb: ok...)
	bool PlantPeriod; // flag! plant volume always?
	bool FixedNote;

	uint8_t volumeSlideUp;
	uint8_t volumeSlideDown;

	uint8_t HardCut;
	bool HardCutRelease;
	uint8_t HardCutReleaseF;

	int16_t periodSlideSpeed;
	int16_t periodSlidePeriod;
	int16_t periodSlideLimit; // if 0, no limit!
	bool periodSlideOn; // on/off
	bool periodSlideWithLimit;

	int16_t periodPerfSlideSpeed;
	int16_t periodPerfSlidePeriod;
	bool periodPerfSlideOn; // on/off

	uint8_t vibratoDelay;
	uint8_t vibratoCurrent; // 0..1..2....511..0..1..
	uint8_t vibratoDepth;
	uint8_t vibratoSpeed;

	bool squareOn;
	bool squareInit; // init signum/slidein etc.?
	uint8_t squareWait; // Speed->Wait
	uint8_t squareLowerLimit;
	uint8_t squareUpperLimit;
	uint8_t squarePos; // noch die wl dazuzaehlen!!
	int8_t squareSignum; // +1/-1 , to add/neg!
	bool squareSlidingIn;

	bool filterOn;
	bool filterInit;
	uint8_t filterWait; // Speed->Wait
	uint8_t filterLowerLimit;
	uint8_t filterUpperLimit;
	uint8_t filterPos; // noch die wl dazuzaehlen!!
	int8_t filterSignum; // +2/-2 , to add/neg!
	uint8_t filterSpeed;
	bool filterSlidingIn;
	uint8_t IgnoreFilter; // 8bb: both a flag AND a value!

	uint8_t perfCurrent; // countin' down!!!!
	uint8_t perfSpeed; // 'cause speed can b chgd!
	uint8_t perfWait; // Speed->Wait
	uint8_t *perfList; // length>0, weiter gehn!

	uint8_t NoteDelayWait;
	bool NoteDelayOn;
	uint8_t NoteCutWait;
	bool NoteCutOn;

	int8_t *audioPointer; // fixed, constant 1kb buffer!
	const int8_t *audioSource; // connected to NewWaveform only!!
	uint16_t audioPeriod; // okey, if PlantPer, then -> audio
	uint16_t audioVolume;

	int8_t *SquareTempBuffer;
} plyVoiceTemp_t;

typedef struct // 8bb: song strucure
{
	bool songLoaded; // 8bb: added this
	int32_t loopCounter, loopTimes; // 8bb: added this

	volatile bool intPlaying;
	char Name[255+1];
	uint8_t Revision;

	uint8_t highestTrack, numInstruments;
	uint8_t Subsongs;

	uint8_t Subsong; // 8bb: added this
	uint16_t SongCIAPeriod; // 8bb: added this

	plyVoiceTemp_t pvt[AMIGA_VOICES];

	uint16_t TrackLength;
	uint16_t StepWaitFrames; // 0: wait step!
	bool GetNewPosition; // flag!
	uint8_t Tempo; // some default?
	double dBPM; // 8bb: added this
	bool PatternBreak;

	uint16_t PosJump;
	uint16_t PosJumpNote;
	uint32_t WNRandom;

	uint16_t NoteNr;
	uint16_t PosNr;
	uint16_t ResNr;
	uint16_t LenNr;

	uint16_t *SubSongTable;
	uint8_t *PosTable;
	uint8_t *TrackTable;
	instrument_t *Instruments[63];

	int8_t *WaveformTab[4]; // has to be inited!!!

	instrument_t EmptyInstrument; // 8bb: added this ( initialized with def. values in ahxPlay() )
} song_t;

#ifdef _MSC_VER
#pragma pack(push)
#pragma pack(1)
#endif
typedef struct
{
	int8_t lowPasses[WAV_FILTER_LENGTH * 31];
	int8_t triangle04[0x04], triangle08[0x08], triangle10[0x10], triangle20[0x20], triangle40[0x40], triangle80[0x80];
	int8_t sawtooth04[0x04], sawtooth08[0x08], sawtooth10[0x10], sawtooth20[0x20], sawtooth40[0x40], sawtooth80[0x80];
	int8_t squares[0x80 * 32];
	int8_t whiteNoiseBig[NOIZE_SIZE];
	int8_t highPasses[WAV_FILTER_LENGTH * 31];

	// 8bb: moved these here, so that they get dword-aligned
	int8_t SquareTempBuffer[AMIGA_VOICES][0x80];
	int8_t currentVoice[AMIGA_VOICES][0x280];

	// 8bb: Added this (also put here for dword-alignment). The size is just big enough, don't change it!
	int8_t EmptyFilterSection[0x80 * 32];
}
#ifdef __GNUC__
__attribute__ ((packed))
#endif
waveforms_t;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

extern volatile bool isRecordingToWAV;
extern song_t song;
extern waveforms_t *waves; // 8bb: dword-aligned from malloc()

// loader.c
bool ahxLoadFromRAM(const uint8_t *data);
bool ahxLoad(const char *filename);
void ahxFree(void);
// --------------------------

void ahxNextPattern(void);
void ahxPrevPattern(void);

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxInit(int32_t audioFreq, int32_t audioBufferSize, int32_t masterVol, int32_t stereoSeparation);

void ahxClose(void);

bool ahxPlay(int32_t subSong);
void ahxStop(void);

// 8bb: added these WAV recorders

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxRecordWAVFromRAM(const uint8_t *data, const char *fileOut, int32_t subSong,
	int32_t songLoopTimes, int32_t audioFreq, int32_t masterVol, int32_t stereoSeparation);

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxRecordWAV(const char *fileIn, const char *fileOut, int32_t subSong,
	int32_t songLoopTimes, int32_t audioFreq, int32_t masterVol, int32_t stereoSeparation);

int32_t ahxGetErrorCode(void);

void SIDInterrupt(void); // 8bb: replayer ticker
