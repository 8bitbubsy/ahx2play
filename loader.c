/*
** 8bb:
** AHX loader, from AHX 2.3d-sp3.
** NOTE: The loader used in AHX is actually different
** than the loader in the supplied replayer binaries.
** It does some extra stuff like fixing rev-0 modules.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "replayer.h"
#include "paula.h"

#define SWAP16(x) \
( \
	(((uint16_t)((x) & 0x00FF)) << 8) | \
	(((uint16_t)((x) & 0xFF00)) >> 8)   \
)

#define SWAP32(x) \
( \
	(((uint32_t)((x) & 0x000000FF)) << 24) | \
	(((uint32_t)((x) & 0x0000FF00)) <<  8) | \
	(((uint32_t)((x) & 0x00FF0000)) >>  8) | \
	(((uint32_t)((x) & 0xFF000000)) >> 24)   \
)

#define READ_BYTE(x, p)  x = *(uint8_t  *)p; p += sizeof (uint8_t);
#define READ_WORD(x, p)  x = *(uint16_t *)p; p += sizeof (uint16_t); x = SWAP16(x)
#define READ_DWORD(x, p) x = *(uint32_t *)p; p += sizeof (uint32_t); x = SWAP32(x)

extern uint8_t ahxErrCode; // 8bb: replayer.c

// 8bb: AHX-header tempo value (0..3) -> Amiga PAL CIA period
static const uint16_t tabler[4] = { 14209, 7104, 4736, 3552 };

// 8bb: added +1 to all values in this table (was meant for 68k DBRA loop)
static const uint16_t lengthTable[6+6+32+1] =
{
	0x04,0x08,0x10,0x20,0x40,0x80,
	0x04,0x08,0x10,0x20,0x40,0x80,

	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,

	NOIZE_SIZE
};

static void triangleGenerate(int8_t *dst8, int16_t delta, int32_t offset, int32_t length)
{
	int16_t data = 0;
	for (int32_t i = 0; i < length+1; i++)
	{
		*dst8++ = (uint8_t)data;
		data += delta;
	}
	*dst8++ = 127;

	data = 128;
	for (int32_t i = 0; i < length; i++)
	{
		data -= delta;
		*dst8++ = (uint8_t)data;
	}

	int8_t *src8 = &dst8[offset];
	for (int32_t i = 0; i < (length+1)*2; i++)
	{
		int8_t sample = *src8++;
		if (sample == 127)
			sample = -128;
		else
			sample = 0 - sample;

		*dst8++ = sample;
	}
}

static void sawToothGenerate(int8_t *dst8, int32_t length)
{
	const int8_t delta = (int8_t)(256 / (length-1));

	int8_t data = -128;
	for (int32_t i = 0; i < length; i++)
	{
		*dst8++ = data;
		data += delta;
	}
}

static void squareGenerate(int8_t *dst8)
{
	uint16_t *dst16 = (uint16_t *)dst8;
	for (int32_t i = 1; i <= 32; i++)
	{
		for (int32_t j = 0; j < 64-i; j++)
			*dst16++ = 0x8080;

		for (int32_t j = 0; j < i; j++)
			*dst16++ = 0x7F7F;
	}
}

static void whiteNoiseGenerate(int8_t *dst8, int32_t length)
{
	uint32_t seed = 0x41595321; // 8bb: "AYS!"

	for (int32_t i = 0; i < length; i++)
	{
		if (!(seed & 256))
			*dst8++ = (uint8_t)seed;
		else if (seed & 0x8000)
			*dst8++ = -128;
		else
			*dst8++ = 127;

		ROR32(seed, 5);
		seed ^= 0b10011010;
		uint16_t tmp16 = (uint16_t)seed;
		ROL32(seed, 2);
		tmp16 += (uint16_t)seed;
		seed ^= tmp16;
		ROR32(seed, 3);
	}
}

static inline int32_t fp16Clip(int32_t x)
{
	int16_t fp16Int = x >> 16;

	if (fp16Int > 127)
	{
		fp16Int = 127;
		return fp16Int << 16;
	}

	if (fp16Int < -128)
	{
		fp16Int = -128;
		return fp16Int << 16;
	}

	return x;
}

static void setUpFilterWaveForms(void)
{
	int8_t *dst8Hi = waves->highPasses;
	int8_t *dst8Lo = waves->lowPasses;
	
	int32_t d5 = ((((8<<16)*125)/100)/100)>>8;
	for (int32_t i = 0; i < 31; i++)
	{
		int8_t *src8 =  waves->triangle04; // 8bb: beginning of waveforms
		for (int32_t j = 0; j < 6+6+32+1; j++)
		{
			const int32_t waveLength = lengthTable[j];
			
			int32_t d1;
			int32_t d2 = 0;
			int32_t d3 = 0;

			// 8bb: 1st pass
			for (int32_t k = 0; k < waveLength; k++)
			{
				const int32_t d0 = (int16_t)src8[k] << 16;

				d1 = fp16Clip(d0 - d2 - d3);
				d2 = fp16Clip(d2 + ((d1 >> 8) * d5));
				d3 = fp16Clip(d3 + ((d2 >> 8) * d5));
			}

			// 8bb: 2nd pass
			for (int32_t k = 0; k < waveLength; k++)
			{
				const int32_t d0 = (int16_t)src8[k] << 16;

				d1 = fp16Clip(d0 - d2 - d3);
				d2 = fp16Clip(d2 + ((d1 >> 8) * d5));
				d3 = fp16Clip(d3 + ((d2 >> 8) * d5));
			}

			// 8bb: 3rd pass
			for (int32_t k = 0; k < waveLength; k++)
			{
				const int32_t d0 = (int16_t)src8[k] << 16;

				d1 = fp16Clip(d0 - d2 - d3);
				d2 = fp16Clip(d2 + ((d1 >> 8) * d5));
				d3 = fp16Clip(d3 + ((d2 >> 8) * d5));
			}
			
			/* 8bb:
			** Truncate lower 8 bits so that it's bit-accurate
			** to how AHX does it (it uses a bit-reduced LUT).
			*/
			d2 &= ~0xFF;
			d3 &= ~0xFF;

			// 8bb: 4th pass (also writes to output)
			for (int32_t k = 0; k < waveLength; k++)
			{
				const int32_t d0 = (int16_t)src8[k] << 16;

				d1 = fp16Clip(d0 - d2 - d3);
				d2 = fp16Clip(d2 + ((d1 >> 8) * d5));
				d3 = fp16Clip(d3 + ((d2 >> 8) * d5));

				*dst8Hi++ = (uint8_t)(d1 >> 16);
				*dst8Lo++ = (uint8_t)(d3 >> 16);
			}

			src8 += waveLength; // 8bb: go to next waveform
		}

		d5 += ((((3<<16)*125)/100)/100)>>8;
	}
}

void ahxFreeWaves(void)
{
	if (waves != NULL)
	{
		free(waves);
		waves = NULL;
	}
}

bool ahxInitWaves(void) // 8bb: this generates bit-accurate AHX 2.3d-sp3 waveforms
{
	ahxFreeWaves();

	// 8bb: "waves" needs dword-alignment, and that's guaranteed from malloc()
	waves = (waveforms_t *)malloc(sizeof (waveforms_t));
	if (waves == NULL)
		return false;

	// 8bb: generate waveforms

	int8_t *dst8 =  waves->triangle04;
	for (int32_t i = 0; i < 6; i++)
	{
		uint16_t fullLength = 4 << i;
		uint16_t length = fullLength >> 2;
		uint16_t delta = 128 / length;
		int32_t offset = 0 - (fullLength >> 1);

		triangleGenerate(dst8, delta, offset, length-1);
		dst8 += fullLength;
	}

	sawToothGenerate(waves->sawtooth04, 0x04);
	sawToothGenerate(waves->sawtooth08, 0x08);
	sawToothGenerate(waves->sawtooth10, 0x10);
	sawToothGenerate(waves->sawtooth20, 0x20);
	sawToothGenerate(waves->sawtooth40, 0x40);
	sawToothGenerate(waves->sawtooth80, 0x80);
	squareGenerate(waves->squares);
	whiteNoiseGenerate(waves->whiteNoiseBig, NOIZE_SIZE);

	setUpFilterWaveForms();
	return true;
}

static bool ahxInitModule(const uint8_t *p)
{
	bool trkNullEmpty;
	uint16_t flags;

	song.songLoaded = false;

	// 8bb: added this check
	if (waves == NULL)
	{
		ahxErrCode = ERR_NO_WAVES;
		return false;
	}

	song.Revision = p[3];

	if (memcmp("THX", p, 3) != 0 || song.Revision > 1) // 8bb: added revision check
	{
		ahxErrCode = ERR_NOT_AN_AHX;
		return false;
	}

	p += 6;

	READ_WORD(flags, p);
	trkNullEmpty = !!(flags & 32768);
	song.LenNr = flags & 0x3FF;
	READ_WORD(song.ResNr, p);
	READ_BYTE(song.TrackLength, p);
	READ_BYTE(song.highestTrack, p); // max track nr. like 0
	READ_BYTE(song.numInstruments, p); // max instr nr. 0/1-63
	READ_BYTE(song.Subsongs, p);
	uint32_t numTracks = song.highestTrack + 1;

	if (song.ResNr >= song.LenNr) // 8bb: safety bug-fix...
		song.ResNr = 0;

	// 8bb: read sub-song table
	const int32_t subSongTableBytes = song.Subsongs << 1;

	song.SubSongTable = (uint16_t *)malloc(subSongTableBytes);
	if (song.SubSongTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	const uint16_t *ptr16 = (uint16_t *)p;
	for (int32_t i = 0; i < song.Subsongs; i++)
		song.SubSongTable[i] = SWAP16(ptr16[i]);
	p += subSongTableBytes;


	// 8bb: read position table
	const int32_t posTableBytes = song.LenNr << 3;

	song.PosTable = (uint8_t *)malloc(posTableBytes);
	if (song.PosTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	for (int32_t i = 0; i < posTableBytes; i++)
		song.PosTable[i] = *p++;


	// 8bb: read track table
	song.TrackTable = (uint8_t *)calloc(numTracks, 3*64);
	if (song.TrackTable == NULL)
	{
		ahxFree();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	int32_t tracksToRead = numTracks;
	uint8_t *dst8 = song.TrackTable;

	if (trkNullEmpty)
	{
		dst8 += 3*64;
		tracksToRead--;
	}
	
	if (tracksToRead > 0)
	{
		const int32_t trackBytes = song.TrackLength * 3;
		for (int32_t i = 0; i < tracksToRead; i++)
		{
			memcpy(&dst8[i * 3 * 64], p, trackBytes);
			p += trackBytes;
		}
	}

	// 8bb: read instruments
	for (int32_t i = 0; i < song.numInstruments; i++)
	{
		instrument_t *ins = (instrument_t *)p;

		const int32_t instrBytes = 22 + (ins->perfLength << 2);

		// 8bb: calloc is needed here, to clear all non-written perfList bytes!
		song.Instruments[i] = (instrument_t *)calloc(1, sizeof (instrument_t));
		if (song.Instruments[i] == NULL)
		{
			ahxFree();
			ahxErrCode = ERR_OUT_OF_MEMORY;
			return false;
		}

		memcpy(song.Instruments[i], p, instrBytes);
		p += instrBytes;
	}

	song.Name[255] = '\0';
	for (int32_t i = 0; i < 255; i++)
	{
		song.Name[i] = (char)p[i];
		if (song.Name[i] == '\0')
			break;
	}

	// 8bb: remove filter commands on rev-0 songs, if present (AHX does this)
	if (song.Revision == 0)
	{
		uint8_t *ptr8;

		// 8bb: clear command 4 (override filter) parameter
		ptr8 = song.TrackTable;
		for (int32_t i = 0; i <= song.highestTrack; i++)
		{
			for (int32_t j = 0; j < song.TrackLength; j++)
			{
				const uint8_t fx = ptr8[1] & 0x0F;
				if (fx == 4) // FX: OVERRIDE FILTER!
				{
					ptr8[1] &= 0xF0;
					ptr8[2] = 0; // override w/ zero!!
				}
				
				ptr8 += 3;
			}
		}

		// 8bb: clear command 0/4 parameter in instrument plists
		for (int32_t i = 0; i < song.numInstruments; i++)
		{
			instrument_t *ins = song.Instruments[i];
			if (ins == NULL)
				continue;

			ptr8 = ins->perfList;
			for (int32_t j = 0; j < ins->perfLength; j++)
			{
				const uint8_t fx1 = (ptr8[0] >> 2) & 7;
				if (fx1 == 0 || fx1 == 4)
					ptr8[2] = 0; // 8bb: clear fx1 parameter

				const uint8_t fx2 = (ptr8[0] >> 5) & 7;
				if (fx2 == 0 || fx2 == 4)
					ptr8[3] = 0; // 8bb: clear fx2 parameter

				ptr8 += 4;
			}
		}
	}

	// 8bb: added this (BPM/tempo)
	song.SongCIAPeriod = tabler[(flags >> 13) & 3];

	// 8bb: set up waveform pointers (Note: song.WaveformTab[2] gets initialized in the replayer!)
	song.WaveformTab[0] = waves->triangle04;
	song.WaveformTab[1] = waves->sawtooth04;
	song.WaveformTab[3] = waves->whiteNoiseBig;

	// 8bb: Added this. Set default values for EmptyInstrument (used for non-loaded instruments in replayer)
	instrument_t *ins = &song.EmptyInstrument;
	memset(ins, 0, sizeof (instrument_t));
	ins->aFrames = 1;
	ins->dFrames = 1;
	ins->sFrames = 1;
	ins->rFrames = 1;
	ins->perfSpeed = 1;
	ins->squareLowerLimit = 0x20;
	ins->squareUpperLimit = 0x3F;
	ins->squareSpeed = 1;
	ins->filterLowerLimit = 1;
	ins->filterUpperLimit = 0x1F;
	ins->filterSpeedWavelength = 4<<3; // fs 3 wl 04 !!
	// ----------------------------------------------------

	song.songLoaded = true;
	return true;
}

bool ahxLoadFromRAM(const uint8_t *data)
{
	ahxErrCode = ERR_SUCCESS;
	if (!ahxInitModule(data))
	{
		ahxFree();
		return false;
	}

	return true;
}

bool ahxLoad(const char *filename)
{
	ahxErrCode = ERR_SUCCESS;

	FILE *f = fopen(filename, "rb");
	if (f == NULL)
	{
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	fseek(f, 0, SEEK_END);
	const uint32_t filesize = (uint32_t)ftell(f);
	rewind(f);

	uint8_t *fileBuffer = (uint8_t *)malloc(filesize);
	if (fileBuffer == NULL)
	{
		fclose(f);
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	if (fread(fileBuffer, 1, filesize, f) != filesize)
	{
		free(fileBuffer);
		fclose(f);
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	fclose(f);

	if (!ahxLoadFromRAM((const uint8_t *)fileBuffer))
	{
		free(fileBuffer);
		return false;
	}

	free(fileBuffer);
	return true;
}

void ahxFree(void)
{
	ahxStop();
	paulaStopAllDMAs(); // 8bb: song can be free'd now

	if (song.SubSongTable != NULL)
		free(song.SubSongTable);

	if (song.PosTable != NULL)
		free(song.PosTable);

	if (song.TrackTable != NULL)
		free(song.TrackTable);

	for (int32_t i = 0; i < song.numInstruments; i++)
	{
		if (song.Instruments[i] != NULL)
			free(song.Instruments[i]);
	}

	memset(&song, 0, sizeof (song));
}
