/*
** 8bb:
** Port of AHX 2.3d-sp3's replayer.
**
** This is a port of the replayer found in AHX 2.3d-sp3's tracker code,
** not the small external replayer binary. There are some minor
** differences between them.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> // ceil()
#include "replayer.h"

static const uint8_t waveOffsets[6] =
{
	0x00,0x04,0x04+0x08,0x04+0x08+0x10,0x04+0x08+0x10+0x20,0x04+0x08+0x10+0x20+0x40
};

/* 8bb:
** Added this. 129 words from before the periodTable in AHX 2.3d-sp3 (68020 version).
** This is needed because the final note can accidentally be -1 .. -129, which is not
** safe when accesing the periodTable.
*/
static const int16_t beforePeriodTable_68020[129] =
{
	0xF6F2,0xEEEA,0xE6E3,0x201B,0x1612,0x0E0A,0x0603,0x00FD,0xFAF8,0xF6F4,
	0xF2F1,0x100D,0x0A08,0x0604,0x0201,0x00FF,0xFEFE,0xFEFE,0xFEFF,0x4A30,
	0x0170,0x0000,0x0027,0x66FF,0x0000,0x00B2,0x4A30,0x0170,0x0000,0x0026,
	0x6712,0x3770,0x0170,0x0000,0x0064,0x0006,0x51F0,0x0170,0x0000,0x0026,
	0x4A30,0x0170,0x0000,0x0022,0x67FF,0x0000,0x007C,0x48E7,0x3F68,0x2470,
	0x0170,0x0000,0x005C,0x0C30,0x0003,0x0170,0x0000,0x0014,0x67FF,0x0000,
	0x0042,0x7C01,0x7405,0x9430,0x0170,0x0000,0x0015,0xE56E,0xCCFC,0x0005,
	0x5346,0x2270,0x0170,0x0000,0x0060,0x7E01,0x7400,0x1430,0x0170,0x0000,
	0x0015,0xE52F,0x5347,0x2619,0x24C3,0x51CF,0xFFFA,0x51CE,0xFFDE,0x60FF,
	0x0000,0x0016,0x2270,0x0170,0x0000,0x0060,0x7E4F,0x24D9,0x24D9,0x51CF,
	0xFFFA,0x4CDF,0x16FC,0x51F0,0x0170,0x0000,0x0022,0x3770,0x0170,0x0000,
	0x0066,0x0008,0x4E75,0x377C,0x0000,0x0008,0x4E75,0x0004,0x0000,0x0001,
	0x0000,0x0015,0x4C70,0x0015,0x4D6C,0x000E,0xA9C4,0x0015,0x5E68
};

static const int16_t periodTable[1+60] =
{
	0,
	3424,3232,3048,2880,2712,2560,2416,2280,2152,2032,1920,1812,
	1712,1616,1524,1440,1356,1280,1208,1140,1076,1016, 960, 906,
	 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
	 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
	 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113
};

static const int16_t vibTable[64] =
{
	   0,  24,  49,  74,  97, 120, 141, 161,
	 180, 197, 212, 224, 235, 244, 250, 253,
	 255, 253, 250, 244, 235, 224, 212, 197,
	 180, 161, 141, 120,  97,  74,  49,  24,
	   0, -24, -49, -74, -97,-120,-141,-161,
	-180,-197,-212,-224,-235,-244,-250,-253,
	-255,-253,-250,-244,-235,-224,-212,-197,
	-180,-161,-141,-120, -97, -74, -49, -24
};

// 8bb: globalized
volatile bool isRecordingToWAV;
song_t song;
waveforms_t *waves; // 8bb: dword-aligned from malloc()
uint8_t ahxErrCode;
// ------------

// 8bb: loader.c
bool ahxInitWaves(void);
void ahxFreeWaves(void);
// -----------

static void SetUpAudioChannels(void) // 8bb: only call this while mixer is locked!
{
	plyVoiceTemp_t *ch;

	paulaStopAllDMAs();

	ch = song.pvt;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
	{
		ch->audioPointer = waves->currentVoice[i];

		paulaSetPeriod(i, 0x88);
		paulaSetData(i, ch->audioPointer);
		paulaSetVolume(i, 0);
		paulaSetLength(i, 0x280 / 2);
	}

	paulaStartAllDMAs();
}

static void InitVoiceXTemp(plyVoiceTemp_t *ch) // 8bb: only call this while mixer is locked!
{
	int8_t *oldAudioPointer = ch->audioPointer;

	memset(ch, 0, sizeof (plyVoiceTemp_t));

	ch->TrackMasterVolume = 64;
	ch->squareSignum = 1;
	ch->squareLowerLimit = 1;
	ch->squareUpperLimit = 63;

	ch->audioPointer = oldAudioPointer;
}

static void ahxQuietAudios(void)
{
	for (int32_t i = 0; i < AMIGA_VOICES; i++)
		paulaSetVolume(i, 0);
}

static void CopyWaveformToPaulaBuffer(plyVoiceTemp_t *ch) // 8bb: I put this code in an own function
{
	// 8bb: audioPointer and audioSource buffers are dword-aligned, 32-bit access is safe
	uint32_t *dst32 = (uint32_t *)ch->audioPointer;

	if (ch->Waveform == 4-1) // 8bb: noise, copy in one go
	{
		const uint32_t *src32 = (const uint32_t *)ch->audioSource;
		for (int32_t i = 0; i < 0x280/8; i++)
		{
			*dst32++ = *src32++;
			*dst32++ = *src32++;
		}
	}
	else
	{
		const int32_t length = (1 << (5 - ch->Wavelength)) * 5;
		const int32_t copyLength = 1 << ch->Wavelength;

		for (int32_t i = 0; i < length; i++)
		{
			const uint32_t *src32 = (const uint32_t *)ch->audioSource;
			for (int32_t j = 0; j < copyLength; j++)
				*dst32++ = *src32++;
		}
	}
}

static void SetAudio(int32_t chNum, plyVoiceTemp_t *ch)
{
	// new PERIOD to plant ???
	if (ch->PlantPeriod)
	{
		paulaSetPeriod(chNum, ch->audioPeriod);
		ch->PlantPeriod = false;
	}

	// new FILTER or new WAVEFORM ???
	if (ch->NewWaveform)
	{
		CopyWaveformToPaulaBuffer(ch);
		ch->NewWaveform = false;
	}

	paulaSetVolume(chNum, ch->audioVolume);
}

static void ProcessStep(plyVoiceTemp_t *ch)
{
	uint8_t note, instr, cmd, param;

	ch->volumeSlideUp = 0; // means A cmd
	ch->volumeSlideDown = 0; // means A cmd

	if (ch->Track > song.highestTrack) // 8bb: added this (this is technically what happens in AHX on illegal tracks)
	{
		note = 0;
		instr = 0;
		cmd = 0;
		param = 0;
	}
	else
	{
		const uint8_t *bytes = &song.TrackTable[((ch->Track << 6) + song.NoteNr) * 3];

		note = (bytes[0] >> 2) & 0x3F;
		instr = ((bytes[0] & 3) << 4) | (bytes[1] >> 4);
		cmd = bytes[1] & 0xF;
		param = bytes[2];
	}

	// Effect  > E <  -  Enhanced Commands
	if (cmd == 0xE)
	{
		uint8_t eCmd = param >> 4;
		uint8_t eParam = param & 0xF;

		if (eCmd == 0xC) // Effect  > EC<  -  NoteCut
		{
			if (eParam < song.Tempo)
			{
				ch->NoteCutWait = eParam;
				ch->NoteCutOn = true;
				ch->HardCutRelease = false;
			}
		}

		if (eCmd == 0xD) // Effect  > ED<  -  NoteDelay
		{
			if (ch->NoteDelayOn)
			{
				ch->NoteDelayOn = false;
			}
			else if (eParam < song.Tempo)
			{
				ch->NoteDelayWait = eParam;
				if (ch->NoteDelayWait != 0)
				{
					ch->NoteDelayOn = true;
					return; // 8bb: yes, get out of here!
				}
			}
		}
	}

	if (cmd == 0x0) // Effect  > 0 <  -  PositionjumpHI
	{
		if (param != 0)
		{
			uint8_t pos = param & 0xF;
			if (pos <= 9)
				song.PosJump = (param & 0xF) << 8; // 8bb: yes, this clears the lower byte too!
		}
	}

	// 8bb: effect 8 is for external timing/syncing (probably for games/demos), let's not include it

	if (cmd == 0xD) // Effect  > D <  -  Patternbreak
	{
		song.PosJump = song.PosNr + 1; // jump to next position (8bb: yes, it clears PosJump hi-byte)

		song.PosJumpNote = ((param >> 4) * 10) + (param & 0xF);
		if (song.PosJumpNote >= song.TrackLength)
			song.PosJumpNote = 0;

		song.PatternBreak = true;
	}

	if (cmd == 0xB) // Effect  > B <  -  Positionjump
	{
		song.PosJump = (song.PosJump * 100) + ((param >> 4) * 10) + (param & 0xF);
		song.PatternBreak = true;
	}

	if (cmd == 0xF) // Effect  > F <  -  Set Tempo
	{
		song.Tempo = param;

		// 8bb: added this for the WAV renderer
		if (song.Tempo == 0)
			isRecordingToWAV = false;
	}

	// Effect  > 5 <  -  Volume Slide + Tone Portamento
	// Effect  > A <  -  Volume Slide
	if (cmd == 0x5 || cmd == 0xA)
	{
		ch->volumeSlideDown = param & 0xF;
		ch->volumeSlideUp = param >> 4;
	}

	// Instrument to initialize ?
	if (instr > 0)
	{
		int16_t delta;

		ch->perfSubVolume = 64;

		// reset portamento
		ch->periodPerfSlideSpeed = 0;
		ch->periodSlidePeriod = 0;
		ch->periodSlideLimit = 0;

		// init adsr-envelope
		instrument_t *ins = song.Instruments[instr-1];
		if (ins == NULL) // 8bb: added this (this is technically what happens in AHX on illegal instruments)
			ins = &song.EmptyInstrument;

		ch->adsr = 0; // adsr starting at vol. 0!

		ch->aFrames = ins->aFrames;
		delta = ins->aVolume << 8;
		if (ch->aFrames != 0)
			delta /= ch->aFrames;
		ch->aDelta = delta;

		ch->dFrames = ins->dFrames;
		delta = ((int8_t)ins->dVolume - (int8_t)ins->aVolume) << 8;
		if (ch->dFrames != 0)
			delta /= ch->dFrames;
		ch->dDelta = delta;

		ch->sFrames = ins->sFrames;

		ch->rFrames = ins->rFrames;
		delta = ((int8_t)ins->rVolume - (int8_t)ins->dVolume) << 8;
		if (ch->rFrames != 0)
			delta /= ch->rFrames;
		ch->rDelta = delta;

		// copy Instrument values
		ch->Wavelength = ins->filterSpeedWavelength & 0b00000111;

		if (ch->Wavelength > 5) // 8bb: safety bug-fix...
			ch->Wavelength = 5;

		ch->NoteMaxVolume = ins->Volume;

		ch->vibratoCurrent = 0;
		ch->vibratoDelay = ins->vibratoDelay;
		ch->vibratoDepth = ins->vibratoDepth & 0b00001111;
		ch->vibratoSpeed = ins->vibratoSpeed;
		ch->VibratoPeriod = 0;
		ch->HardCutRelease = !!(ins->vibratoDepth & 128);
		ch->HardCut = (ins->vibratoDepth & 0b01110000) >> 4;

		ch->IgnoreSquare = false; // don't ignore the 3xx...
		ch->squareSlidingIn = false;
		ch->squareWait = 0;
		ch->squareOn = false;

		uint8_t lowerLimit = ins->squareLowerLimit >> (5 - ch->Wavelength);
		uint8_t upperLimit = ins->squareUpperLimit >> (5 - ch->Wavelength);

		if (lowerLimit <= upperLimit)
		{
			ch->squareLowerLimit = lowerLimit;
			ch->squareUpperLimit = upperLimit;
		}
		else
		{
			ch->squareLowerLimit = upperLimit;
			ch->squareUpperLimit = lowerLimit;
		}

		ch->IgnoreFilter = 0;
		ch->filterWait = 0;
		ch->filterOn = false;
		ch->filterSlidingIn = false;

		ch->filterSpeed = ins->filterSpeedWavelength >> 3; // shift out wavelength!

		lowerLimit = ins->filterLowerLimit;
		upperLimit = ins->filterUpperLimit;
		if (lowerLimit & 128) ch->filterSpeed |= 32;
		if (upperLimit & 128) ch->filterSpeed |= 64;
		lowerLimit &= ~128;
		upperLimit &= ~128;

		if (lowerLimit <= upperLimit)
		{
			ch->filterLowerLimit = lowerLimit;
			ch->filterUpperLimit = upperLimit;
		}
		else
		{
			ch->filterLowerLimit = upperLimit;
			ch->filterUpperLimit = lowerLimit;
		}

		ch->filterPos = 32; // std: no filter!
		ch->perfWait = 0;
		ch->perfSpeed = ins->perfSpeed;
		ch->perfCurrent = 0;

		ch->Instrument = ins;
		ch->perfList = ins->perfList;
	}

	if (cmd == 0x9) // Effect  > 9 <  -  Set Squarewave-Offset
	{
		ch->squarePos = param >> (5 - ch->Wavelength);
		ch->PlantSquare = true; // now set relation...
		ch->IgnoreSquare = true; // ignore next following 3xx cmd.
	}

	if (cmd == 0x4) // Effect  > 4 <  -  Override Filter
	{
		if (param < 0x40)
			ch->IgnoreFilter = param;
		else
			ch->filterPos = param - 0x40;
	}

	ch->periodSlideOn = false;

	// Effect  > 5 <  -  TonePortamento+Volume Slide
	// Effect  > 3 <  -  TonePortamento (periodSlide Up/Down w/ Limit)
	if (cmd == 0x3 || cmd == 0x5)
	{
		if (cmd == 0x3 && param != 0)
			ch->periodSlideSpeed = param;

		bool doSlide = true;
		if (note != 0)
		{
			int16_t periodLimit = periodTable[ch->TrackPeriod] - periodTable[note]; // (ABS) SLIDE LIMIT

			const uint16_t test = periodLimit + ch->periodSlidePeriod;
			if (test == 0) // c-1 -> c-1....
				doSlide = false;
			else
				ch->periodSlideLimit = 0 - periodLimit; // neg/pos!!
		}

		if (doSlide)
		{
			ch->periodSlideOn = true;
			ch->periodSlideWithLimit = true;
			note = 0; // 8bb: don't trigger note
		}
	}

	if (note != 0)
	{
		ch->TrackPeriod = note;
		ch->PlantPeriod = true;
	}

	if (cmd == 0x1) // Effect  > 1 <  -  Portamento Up (periodSlide Down)
	{
		ch->periodSlideSpeed = 0 - param;
		ch->periodSlideOn = true;
		ch->periodSlideWithLimit = false;
	}

	if (cmd == 0x2) // Effect  > 2 <  -  Portamento Down (periodSlide Up)
	{
		ch->periodSlideSpeed = param;
		ch->periodSlideOn = true;
		ch->periodSlideWithLimit = false;
	}

	if (cmd == 0xE) // Effect  > E <  -  Enhanced Commands
	{
		uint8_t eCmd = param >> 4;
		uint8_t eParam = param & 0xF;

		if (eCmd == 0x1) // Effect  > E1<  -  FineSlide Up (periodFineSlide Down)
		{
			ch->periodSlidePeriod += 0 - eParam;
			ch->PlantPeriod = true;
		}

		if (eCmd == 0x2) // Effect  > E2<  -  FineSlide Down (periodFineSlide Up)
		{
			ch->periodSlidePeriod += eParam;
			ch->PlantPeriod = true;
		}

		if (eCmd == 0x4) // Effect  > E4<  -  Vibrato Control
			ch->vibratoDepth = eParam;

		if (eCmd == 0xA) // Effect  > EA<  -  FineVolume Up
		{
			ch->NoteMaxVolume += eParam;
			if (ch->NoteMaxVolume > 0x40)
				ch->NoteMaxVolume = 0x40;
		}

		if (eCmd == 0xB) // Effect  > EB<  -  FineVolume Down
		{
			ch->NoteMaxVolume -= eParam;
			if ((int8_t)ch->NoteMaxVolume < 0)
				ch->NoteMaxVolume = 0;
		}
	}

	if (cmd == 0xC) // Effect  > C <  -  Set Volume
	{
		int16_t p = param;
		if (p <= 0x40)
		{
			ch->NoteMaxVolume = (uint8_t)p;
		}
		else
		{
			p -= 0x50;
			if (p >= 0)
			{
				if (p <= 0x40)
				{
					// 8bb: set TrackMasterVolume for all channels
					plyVoiceTemp_t *c = song.pvt;
					for (int32_t i = 0; i < AMIGA_VOICES; i++, c++)
						c->TrackMasterVolume = (uint8_t)p;
				}
				else
				{
					p -= 0xA0-0x50;
					if (p >= 0 && p <= 0x40)
						ch->TrackMasterVolume = (uint8_t)p;
				}
			}
		}
	}
}

static void pListCommandParse(plyVoiceTemp_t *ch, uint8_t cmd, uint8_t param)
{
	if (cmd == 0x0) // 8bb: Init Filter Modulation
	{
		if (param == 0)
			return; // cmd 0-00 is STILL nuttin'

		if (ch->IgnoreFilter)
		{
			ch->filterPos = ch->IgnoreFilter;
			ch->IgnoreFilter = false;
		}
		else
		{
			ch->filterPos = param;
			ch->NewWaveform = true;
		}
	}

	else if (cmd == 0x1) // 8bb: Slide Up
	{
		ch->periodPerfSlideSpeed = param;
		ch->periodPerfSlideOn = true;
	}

	else if (cmd == 0x2) // 8bb: Slide Down
	{
		ch->periodPerfSlideSpeed = 0 - param;
		ch->periodPerfSlideOn = true;
	}

	else if (cmd == 0x3) // Init Square Modulation
	{
		if (ch->IgnoreSquare)
			ch->IgnoreSquare = false;
		else
			ch->squarePos = param >> (5 - ch->Wavelength);
	}

	else if (cmd == 0x4) // Start/Stop Modulation
	{
		if (param == 0) // 400 is downwards-compatible, means modulate square!!
		{
			ch->squareOn ^= 1;
			ch->squareInit = ch->squareOn;
			ch->squareSignum = 1;
		}
		else
		{
			if (param & 0x0F)
			{
				ch->squareOn ^= 1; // any value? FILTER MOD!!
				ch->squareInit = ch->squareOn;

				ch->squareSignum = 1;
				if ((param & 0x0F) == 0x0F) // filter +1 ???
					ch->squareSignum = 0 - ch->squareSignum;
			}

			if (param & 0xF0)
			{
				ch->filterOn ^= 1; // any value? FILTER MOD!!
				ch->filterInit = ch->filterOn;

				ch->filterSignum = 1;
				if ((param & 0xF0) == 0xF0) // filter +1 ???
					ch->filterSignum = 0 - ch->filterSignum;
			}
		}
	}

	else if (cmd == 0x5) // Jump to Step [xx]
	{
		instrument_t *ins = ch->Instrument;
		if (ins == NULL) // 8bb: safety bug-fix...
			ins = &song.EmptyInstrument;

		// 8bb: 4 bytes before perfList (this is apparently what AHX does...)
		uint8_t *perfList = ins->perfList - 4;

		/* 8bb: AHX quirk! There's no range check here.
		** You should have 4*256 perfList bytes for every instrument.
		** The bytes after 4*perfLength should be zeroed out in the loader.
		**
		** AHX does this, and it HAS to be done! Example: lead instrument on "GavinsQuest.ahx".
		*/

		ch->perfCurrent = param - 1; // 8bb: param-1 is correct (0 -> 255 = safe)
		ch->perfList = &perfList[param << 2]; // 8bb: don't do param-1 here!
	}

	else if (cmd == 0x6) // Set Volume (Command C)
	{
		int16_t p = param;
		if (p <= 0x40)
		{
			ch->NoteMaxVolume = (uint8_t)p;
		}
		else
		{
			p -= 0x50;
			if (p >= 0)
			{
				if (p <= 0x40)
				{
					ch->perfSubVolume = (uint8_t)p;
				}
				else
				{
					p -= 0xA0-0x50;
					if (p >= 0 && p <= 0x40)
						ch->TrackMasterVolume = (uint8_t)p;
				}
			}
		}
	}

	else if (cmd == 0x7) // Set Speed (Command F)
	{
		ch->perfSpeed = param;
		ch->perfWait = param;
	}
}

static void ProcessFrame(plyVoiceTemp_t *ch)
{
	if (ch->HardCut != 0)
	{
		uint8_t track = ch->Track;

		uint16_t noteNr = song.NoteNr + 1; // chk next note!
		if (noteNr == song.TrackLength)
		{
			noteNr = 0; // note 0 from next pos!
			track = ch->NextTrack;
		}

		const uint8_t *bytes = &song.TrackTable[((track << 6) + noteNr) * 3];

		uint8_t nextInstr = ((bytes[0] & 3) << 4) | (bytes[1] >> 4);
		if (nextInstr != 0)
		{
			int8_t range = song.Tempo - ch->HardCut; // range 1->7, tempo=6, hc=1, cut at tick 5, right
			if (range < 0)
				range = 0; // tempo=2, hc=7, cut at tick 0 (NOW!!)

			if (!ch->NoteCutOn)
			{
				ch->NoteCutOn = true;
				ch->NoteCutWait = range;
				ch->HardCutReleaseF = 0 - (ch->NoteCutWait - song.Tempo);
			}

			ch->HardCut = 0;
		}
	}

	if (ch->NoteCutOn)
	{
		if (ch->NoteCutWait == 0)
		{
			ch->NoteCutOn = false;
			if (ch->HardCutRelease)
			{
				instrument_t *ins = ch->Instrument;
				if (ins == NULL) // 8bb: safety bug-fix...
					ins = &song.EmptyInstrument;

				ch->rFrames = ch->HardCutReleaseF;
				ch->rDelta = 0 - ((ch->adsr - (ins->rVolume << 8)) / ch->HardCutReleaseF);
				ch->aFrames = 0;
				ch->dFrames = 0;
				ch->sFrames = 0;
			}
			else
			{
				ch->NoteMaxVolume = 0;
			}
		}

		ch->NoteCutWait--;
	}

	if (ch->NoteDelayOn)
	{
		if (ch->NoteDelayWait == 0)
			ProcessStep(ch);
		else
			ch->NoteDelayWait--;
	}

	instrument_t *ins = ch->Instrument;
	if (ins == NULL) // 8bb: safety bug-fix...
		ins = &song.EmptyInstrument;

	if (ch->aFrames != 0)
	{
		ch->adsr += ch->aDelta;

		ch->aFrames--;
		if (ch->aFrames == 0)
			ch->adsr = ins->aVolume << 8;
	}
	else if (ch->dFrames != 0)
	{
		ch->adsr += ch->dDelta;

		ch->dFrames--;
		if (ch->dFrames == 0)
			ch->adsr = ins->dVolume << 8;
	}
	else if (ch->sFrames != 0)
	{
		ch->sFrames--;
	}
	else if (ch->rFrames != 0)
	{
		ch->adsr += ch->rDelta;

		ch->rFrames--;
		if (ch->rFrames == 0)
			ch->adsr = ins->rVolume << 8;
	}

	// Volume Slide Treatin'
	ch->NoteMaxVolume -= ch->volumeSlideDown;
	ch->NoteMaxVolume += ch->volumeSlideUp;
	ch->NoteMaxVolume = CLAMP((int8_t)ch->NoteMaxVolume, 0, 0x40);

	// Portamento Treatin' (periodSlide)
	if (ch->periodSlideOn)
	{
		if (ch->periodSlideWithLimit)
		{
			int16_t speed = ch->periodSlideSpeed;

			int16_t period = ch->periodSlidePeriod - ch->periodSlideLimit; // source-value
			if (period != 0)
			{
				if (period > 0)
					speed = -speed;

				int16_t limitTest = (period + speed) ^ period;
				if (limitTest >= 0)
					ch->periodSlidePeriod += speed;
				else
					ch->periodSlidePeriod = ch->periodSlideLimit;

				ch->PlantPeriod = true;
			}
		}
		else
		{
			ch->periodSlidePeriod += ch->periodSlideSpeed;  // normal 1er/2er period slide!
			ch->PlantPeriod = true;
		}
	}

	// Vibrato Treatin'
	if (ch->vibratoDepth != 0)
	{
		if (ch->vibratoDelay != 0)
		{
			ch->vibratoDelay--;
		}
		else
		{
			ch->VibratoPeriod = (vibTable[ch->vibratoCurrent] * (int16_t)ch->vibratoDepth) >> 7;
			ch->PlantPeriod = true;
			ch->vibratoCurrent = (ch->vibratoCurrent + ch->vibratoSpeed) & 63;
		}
	}

	// pList Treatin'
	ins = ch->Instrument;
	if (ins != NULL)
	{
		if (ch->perfCurrent == ins->perfLength)
		{
			if (ch->perfWait != 0)
				ch->perfWait--;
			else
				ch->periodPerfSlideSpeed = 0; // only STOP sliding!!
		}
		else
		{
			/* 8bb: AHX QUIRK! Perf speed $80 results in no delay. This has to do with
			** "sub.b #1,Dn | bgt.s .Delay". The BGT instruction will not branch if
			** the register got overflown before the comparison (V flag set).
			** WinAHX/AHX.cpp is not handling this correctly, porters beware!
			**
			** "Enchanted Friday Nights" by JazzCat is a song that depends on this quirk
			** for the lead instrument to sound right.
			*/
			bool signedOverflow = (ch->perfWait == 128); // -128 as signed

			ch->perfWait--;
			if (signedOverflow || (int8_t)ch->perfWait <= 0) // 8bb: signed comparison is needed here
			{
				const uint8_t *bytes = ch->perfList;

				uint8_t cmd2 = (bytes[0] >> 5) & 7;
				uint8_t cmd1 = (bytes[0] >> 2) & 7;
				uint8_t wave = ((bytes[0] << 1) & 6) | (bytes[1] >> 7);
				bool fixed = (bytes[1] >> 6) & 1;
				uint8_t note = bytes[1] & 0x3F;
				uint8_t param1 = bytes[2];
				uint8_t param2 = bytes[3];
				
				// Check Waveform-Field from pList
				if (wave != 0)
				{
					if (wave > 4) // 8bb: safety bug-fix...
						wave = 0;

					ch->Waveform = wave-1; // 0 to 3...
					ch->NewWaveform = true; // New Waveform hit!
					ch->periodPerfSlideSpeed = 0;
					ch->periodPerfSlidePeriod = 0;
				}

				ch->periodPerfSlideOn = false;

				pListCommandParse(ch, cmd1, param1); // Check Command 1 in pList
				pListCommandParse(ch, cmd2, param2); // Check Command 2 in pList

				// Check Note(Fixed)-Field from pList
				if (note != 0)
				{
					ch->InstrPeriod = note;
					ch->PlantPeriod = true;
					ch->FixedNote = fixed;
				}

				// End of Treatin! Goto next entry for next step!
				ch->perfList += 4;
				ch->perfCurrent++;
				ch->perfWait = ch->perfSpeed;
			}
		}
	}

	// ==========================================================================
	// =========================== Treat Waveforms ==============================
	// ==========================================================================
	// =========================== And Modulations ==============================
	// ==========================================================================

	// perfPortamento Treatin' (periodPerfSlide)
	if (ch->periodPerfSlideOn)
	{
		ch->periodPerfSlidePeriod -= ch->periodPerfSlideSpeed;
		if (ch->periodPerfSlidePeriod != 0)
			ch->PlantPeriod = true;
	}

	// Square Treatin' (Modulation-Stuff)
	if (ch->Waveform == 3-1 && ch->squareOn)
	{
		ch->squareWait--;
		if ((int8_t)ch->squareWait <= 0) // 8bb: signed comparison is needed here
		{
			if (ch->squareInit)
			{
				ch->squareInit = false;

				// 8bb: signed comparison is needed here
				if ((int8_t)ch->squarePos <= (int8_t)ch->squareLowerLimit)
				{
					ch->squareSlidingIn = true;
					ch->squareSignum = 1;
				}
				else if ((int8_t)ch->squarePos >= (int8_t)ch->squareUpperLimit)
				{
					ch->squareSlidingIn = true;
					ch->squareSignum = -1;
				}
			}

			if (ch->squarePos == ch->squareLowerLimit || ch->squarePos == ch->squareUpperLimit)
			{
				if (ch->squareSlidingIn)
					ch->squareSlidingIn = false;
				else
					ch->squareSignum = 0 - ch->squareSignum;
			}

			ch->squarePos += ch->squareSignum;
			ch->PlantSquare = true; // when modulating, refresh square!!
			ch->squareWait = ins->squareSpeed;
		}
	}

	// Filter Treatin' (Modulation-Stuff)
	if (ch->filterOn)
	{
		ch->filterWait--;
		if ((int8_t)ch->filterWait <= 0) // 8bb: signed comparison is needed here
		{
			if (ch->filterInit)
			{
				ch->filterInit = false;

				// 8bb: signed comparison is needed here
				if ((int8_t)ch->filterPos <= (int8_t)ch->filterLowerLimit)
				{
					ch->filterSlidingIn = true;
					ch->filterSignum = 1;
				}
				else if ((int8_t)ch->filterPos >= (int8_t)ch->filterUpperLimit)
				{
					ch->filterSlidingIn = true;
					ch->filterSignum = -1;
				}
			}

			int32_t cycles = 1;
			if (ch->filterSpeed < 4) // 8bb: < 4 is correct, not < 3 like in WinAHX/AHX.cpp!
				cycles = 5 - ch->filterSpeed;

			for (int32_t i = 0; i < cycles; i++)
			{
				if (ch->filterPos == ch->filterLowerLimit || ch->filterPos == ch->filterUpperLimit)
				{
					if (ch->filterSlidingIn)
						ch->filterSlidingIn = false;
					else
						ch->filterSignum = 0 - ch->filterSignum;
				}

				ch->filterPos += ch->filterSignum;
			}

			ch->NewWaveform = true;

			ch->filterWait = ch->filterSpeed - 3;
			if ((int8_t)ch->filterWait < 1)
				ch->filterWait = 1;
		}
	}

	// Square Treatin' (Calculation-Stuff)
	if (ch->Waveform == 3-1 || ch->PlantSquare)
	{
		const int8_t *src8;

		// 8bb: safety bug-fix... If filter is out of range, use empty buffer (yes, this can easily happen)
		if (ch->filterPos == 0 || ch->filterPos > 63)
			src8 = waves->EmptyFilterSection;
		else
			src8 = (const int8_t *)&waves->squares[((int32_t)ch->filterPos - 32) * WAV_FILTER_LENGTH]; // squares@desired.filter

		uint8_t whichSquare = ch->squarePos << (5 - ch->Wavelength);
		if ((int8_t)whichSquare > 0x20)
		{
			whichSquare = 0x40 - whichSquare;
			ch->SquareReverse = true;
		}

		whichSquare--;
		if ((int8_t)whichSquare < 0)
			whichSquare = 0;

		src8 += whichSquare << 7; // *$80

		song.WaveformTab[2] = ch->SquareTempBuffer;

		const int32_t delta = (1 << 5) >> ch->Wavelength;
		const int32_t cycles = (1 << ch->Wavelength) << 2; // 8bb: <<2 since we do bytes not dwords, unlike AHX

		// And calc it, too!
		for (int32_t i = 0; i < cycles; i++)
		{
			ch->SquareTempBuffer[i] = *src8;
			src8 += delta;
		}

		ch->NewWaveform = true;
		ch->PlantSquare = false; // enough mod. for this frame
	}

	// Noise Treatin'
	if (ch->Waveform == 4-1)
		ch->NewWaveform = true;

	// Init the final audioPointer
	if (ch->NewWaveform)
	{
		const int8_t *audioSource = song.WaveformTab[ch->Waveform];

		// Waveform 3 (doesn't need filter add)..
		if (ch->Waveform != 3-1)
		{
			// 8bb: safety bug-fix... If filter is out of range, use empty buffer (yes, this can easily happen)
			if (ch->filterPos == 0 || ch->filterPos > 63)
				audioSource = waves->EmptyFilterSection;
			else
				audioSource += ((int32_t)ch->filterPos - 32) * WAV_FILTER_LENGTH;
		}

		// Waveform 1 or 2
		if (ch->Waveform < 3-1)
			audioSource += waveOffsets[ch->Wavelength];

		// Waveform 4
		if (ch->Waveform == 4-1)
		{
			uint32_t seed = song.WNRandom;

			audioSource += seed & ((NOIZE_SIZE-0x280) - 1);

			seed += 2239384;
			ROR32(seed, 8); // 8bb: 32-bit right-bit-rotate by 8
			seed += 782323;
			seed ^= 0b1001011;
			seed -= 6735;
			song.WNRandom = seed;
		}

		ch->audioSource = audioSource;
	}

	// Init the final audioPeriod - always cal. not always write2audio!
	int16_t note = ch->InstrPeriod;
	if (!ch->FixedNote)
	{
		// not fixed, add other note-stuff!!
		note += ch->Transpose; // 8bb: -128 .. 127
		note += ch->TrackPeriod-1; // 8bb: results in -1 if no note
	}

	if (note > 5*12) // 8bb: signed comparison. Allows negative notes, read note below.
		note = 5*12;

	int16_t period;
	if (note < 0)
	{
		/* 8bb:
		** Note can be negative (a common tradition in AHX is to not properly clamp),
		** and this results in reading up to 129 words from before the period table.
		** I added a table which has the correct byte-swapped underflow-words
		** from AHX 2.3d-sp3 (68020 version).
		*/

		if (note < -129) // 8bb: just in case my calculations were off
			note = -129;

		note += 129; // -1 .. -129 -> 0 .. 128
		period = beforePeriodTable_68020[note];
	}
	else
	{
		period = periodTable[note];
	}

	if (!ch->FixedNote)
		period += ch->periodSlidePeriod;

	// but nevertheless add PERFportamento/Vibrato!!!
	period += ch->periodPerfSlidePeriod;
	period += ch->VibratoPeriod;
	ch->audioPeriod = CLAMP(period, 113, 3424);

	// Init the final audioVolume
	uint16_t finalVol = ch->adsr >> 8;
	finalVol = (finalVol * ch->NoteMaxVolume) >> 6;
	finalVol = (finalVol * ch->perfSubVolume) >> 6;
	ch->audioVolume = (finalVol * ch->TrackMasterVolume) >> 6;
}

void SIDInterrupt(void)
{
	plyVoiceTemp_t *ch;

	if (!song.intPlaying)
		return;

	// set audioregisters... (8bb: yes, this is done here, NOT last like in WinAHX/AHX.cpp!)
	ch = song.pvt;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
		SetAudio(i, ch);

	if (song.StepWaitFrames == 0)
	{
		if (song.GetNewPosition)
		{
			uint16_t posNext = song.PosNr + 1;
			if (posNext == song.LenNr)
				posNext = 0;

			// get Track AND Transpose (8bb: also for next position)
			uint8_t *posTable = &song.PosTable[song.PosNr << 3];
			uint8_t *posTableNext = &song.PosTable[posNext << 3];

			ch = song.pvt;
			for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
			{
				const int32_t offset = i << 1;
				ch->Track = posTable[offset+0];
				ch->Transpose = posTable[offset+1];
				ch->NextTrack = posTableNext[offset+0];
				ch->NextTranspose = posTableNext[offset+1];
			}

			song.GetNewPosition = false; // got new pos.
		}

		// - new pos or not, now treat STEPs (means 'em notes 'emself)
		ch = song.pvt;
		for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
			ProcessStep(ch);

		song.StepWaitFrames = song.Tempo;
	}

	ch = song.pvt;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
		ProcessFrame(ch);

	song.StepWaitFrames--;
	if (song.StepWaitFrames == 0)
	{
		if (!song.PatternBreak)
		{
			song.NoteNr++;
			if (song.NoteNr == song.TrackLength)
			{
				// norm. next pos. does just position-jump!
				song.PosJump = song.PosNr + 1;
				song.PatternBreak = true;
			}
		}

		if (song.PatternBreak)
		{
			song.PatternBreak = false;

			song.NoteNr = song.PosJumpNote;
			song.PosJumpNote = 0;

			song.PosNr = song.PosJump;
			song.PosJump = 0;

			if (song.PosNr == song.LenNr)
			{
				song.PosNr = song.ResNr;

				// 8bb: added this (for WAV rendering)
				if (song.loopCounter >= song.loopTimes)
					isRecordingToWAV = false;
				else
					song.loopCounter++;
			}

			// 8bb: safety bug-fix..
			if (song.PosNr >= song.LenNr)
			{
				song.PosNr = 0;

				// 8bb: added this (for WAV rendering)
				if (song.loopCounter >= song.loopTimes)
					isRecordingToWAV = false; // 8bb: stop WAV recording
				else
					song.loopCounter++;
			}

			song.GetNewPosition = true;
		}
	}
}

/***************************************************************************
 *        PLAYER INTERFACING ROUTINES                                      *
 ***************************************************************************/

void ahxNextPattern(void)
{
	lockMixer();

	if (song.PosNr+1 < song.LenNr)
	{
		song.PosJump = song.PosNr + 1;
		song.PatternBreak = true;
		audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick
	}

	unlockMixer();
}

void ahxPrevPattern(void)
{
	lockMixer();

	if (song.PosNr > 0)
	{
		song.PosJump = song.PosNr - 1;
		song.PatternBreak = true;
		audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick
	}

	unlockMixer();
}

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxInit(int32_t audioFreq, int32_t audioBufferSize, int32_t masterVol, int32_t stereoSeparation)
{
	ahxErrCode = ERR_SUCCESS;

	if (!ahxInitWaves())
	{
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	if (!paulaInit(audioFreq))
	{
		paulaClose();
		ahxFreeWaves();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	paulaSetStereoSeparation(stereoSeparation);
	paulaSetMasterVolume(masterVol);

	if (!openMixer(audioFreq, audioBufferSize))
	{
		closeMixer();
		paulaClose();
		ahxFreeWaves();
		ahxErrCode = ERR_AUDIO_DEVICE;
		return false;
	}

	return true;
}

void ahxClose(void)
{
	closeMixer();
	paulaClose();
	ahxFreeWaves();
}

bool ahxPlay(int32_t subSong)
{
	ahxErrCode = ERR_SUCCESS;

	if (!song.songLoaded)
	{
		ahxErrCode = ERR_SONG_NOT_LOADED;
		return false;
	}

	if (waves == NULL)
	{
		ahxErrCode = ERR_NO_WAVES;
		return false; // 8bb: waves not set up!
	}

	lockMixer();

	song.Subsong = 0;
	song.PosNr = 0;
	if (subSong > 0 && song.Subsongs > 0)
	{
		subSong--;
		if (subSong >= song.Subsongs)
			subSong = song.Subsongs-1;

		song.Subsong = (uint8_t)(subSong + 1);
		song.PosNr = song.SubSongTable[subSong];
	}

	song.StepWaitFrames = 0;
	song.GetNewPosition = true;
	song.NoteNr = 0;

	ahxQuietAudios();

	for (int32_t i = 0; i < AMIGA_VOICES; i++)
		InitVoiceXTemp(&song.pvt[i]);

	SetUpAudioChannels();
	amigaSetCIAPeriod(song.SongCIAPeriod);

	// 8bb: Added this. Clear custom data (these are put in the waves struct for dword-alignment)
	memset(waves->SquareTempBuffer,   0, sizeof (waves->SquareTempBuffer));
	memset(waves->currentVoice,       0, sizeof (waves->currentVoice));
	memset(waves->EmptyFilterSection, 0, sizeof (waves->EmptyFilterSection));

	plyVoiceTemp_t *ch = song.pvt;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, ch++)
		ch->SquareTempBuffer = waves->SquareTempBuffer[i];

	song.PosJump = false;
	song.Tempo = 6;
	song.intPlaying = true;

	song.loopCounter = 0;
	song.loopTimes = 0; // 8bb: updated later in WAV writing mode

	audio.tickSampleCounter64 = 0; // 8bb: clear tick sample counter so that it will instantly initiate a tick

	paulaClearFilterState();
	resetCachedMixerPeriod();
	resetAudioDithering();

	song.dBPM = amigaCIAPeriod2Hz(song.SongCIAPeriod) * 2.5;

	song.WNRandom = 0; // 8bb: Clear RNG seed (AHX doesn't do this)

	unlockMixer();

	return true;
}

void ahxStop(void)
{
	lockMixer();

	song.intPlaying = false;
	ahxQuietAudios();

	for (int32_t i = 0; i < AMIGA_VOICES; i++)
		InitVoiceXTemp(&song.pvt[i]);

	unlockMixer();
}

/***************************************************************************
 *        WAV DUMPING ROUTINES                                             *
 ***************************************************************************/

static void writeWAVHeader(FILE *f, int32_t audioFrequency)
{
	uint16_t w;
	uint32_t l;

	// 12 bytes

	const uint32_t RIFF = 0x46464952; // "RIFF"
	fwrite(&RIFF, 4, 1, f);
	fseek(f, 4, SEEK_CUR);
	const uint32_t WAVE = 0x45564157; // "WAVE"
	fwrite(&WAVE, 4, 1, f);

	// 24 bytes

	const uint32_t fmt = 0x20746D66; // " fmt"
	fwrite(&fmt, 4, 1, f);
	l = 16; fwrite(&l, 4, 1, f);
	w = 1; fwrite(&w, 2, 1, f);
	w = 2; fwrite(&w, 2, 1, f);
	l = audioFrequency; fwrite(&l, 4, 1, f);
	l = audioFrequency*2*2; fwrite(&l, 4, 1, f);
	w = 2*2; fwrite(&w, 2, 1, f);
	w = 8*2; fwrite(&w, 2, 1, f);

	// 8 bytes

	const uint32_t DATA = 0x61746164; // "DATA"
	fwrite(&DATA, 4, 1, f);
	fseek(f, 4, SEEK_CUR);
}

static void finishWAVHeader(FILE *f, uint32_t numDataBytes)
{
	fseek(f, 4, SEEK_SET);
	uint32_t l = numDataBytes+4+24+8;
	fwrite(&l, 4, 1, f);
	fseek(f, 12+24+4, SEEK_SET);
	fwrite(&numDataBytes, 4, 1, f);
}

static int32_t ahxGetFrame(int16_t *streamOut) // 8bb: returns bytes mixed
{
	if (audio.tickSampleCounter64 <= 0) // 8bb: new replayer tick
	{
		SIDInterrupt();
		audio.tickSampleCounter64 += audio.samplesPerTick64;
	}

	const int32_t samplesToMix = (audio.tickSampleCounter64 + UINT32_MAX) >> 32; // 8bb: ceil (rounded upwards)

	paulaMixSamples(streamOut, samplesToMix);
	streamOut += samplesToMix * 2;

	audio.tickSampleCounter64 -= (int64_t)samplesToMix << 32;

	return samplesToMix * 2 * sizeof (short);
}

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxRecordWAVFromRAM(const uint8_t *data, const char *fileOut, int32_t subSong,
	int32_t songLoopTimes, int32_t audioFreq, int32_t masterVol, int32_t stereoSeparation)
{
	ahxErrCode = ERR_SUCCESS;

	if (!ahxInitWaves())
	{
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	if (!paulaInit(audioFreq))
	{
		ahxFreeWaves();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	paulaSetStereoSeparation(stereoSeparation);
	paulaSetMasterVolume(masterVol);

	if (!ahxLoadFromRAM(data)) // 8bb: modifies error code
	{
		paulaClose();
		ahxFreeWaves();
		return false;
	}

	const int32_t maxSamplesPerTick = (int32_t)ceil(audioFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));

	int16_t *outputBuffer = (int16_t *)malloc(maxSamplesPerTick * (2 * sizeof (int16_t)));
	if (outputBuffer == NULL)
	{
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	FILE *f = fopen(fileOut, "wb");
	if (f == NULL)
	{
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		free(outputBuffer);
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	writeWAVHeader(f, audioFreq);

	isRecordingToWAV = true;
	if (!ahxPlay(subSong)) // 8bb: modifies error code (also resets audio.tickSampleCounter64)
	{
		isRecordingToWAV = false;
		fclose(f);
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		free(outputBuffer);
		return false;
	}

	song.loopTimes = songLoopTimes;

	uint32_t totalBytes = 0;
	while (isRecordingToWAV)
	{
		const int32_t bytesMixed = ahxGetFrame(outputBuffer);
		fwrite(outputBuffer, 1, bytesMixed, f);
		totalBytes += bytesMixed;
	}

	finishWAVHeader(f, totalBytes);
	isRecordingToWAV = false;

	fclose(f);
	ahxFree();
	paulaClose();
	ahxFreeWaves();
	free(outputBuffer);

	return true;
}

// 8bb: masterVol = 0..256 (default = 256), stereoSeparation = 0..100 (percentage, default = 20)
bool ahxRecordWAV(const char *fileIn, const char *fileOut, int32_t subSong,
	int32_t songLoopTimes, int32_t audioFreq, int32_t masterVol, int32_t stereoSeparation)
{
	ahxErrCode = ERR_SUCCESS;

	if (!ahxInitWaves())
	{
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	if (!paulaInit(audioFreq))
	{
		ahxFreeWaves();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	paulaSetStereoSeparation(stereoSeparation);
	paulaSetMasterVolume(masterVol);

	if (!ahxLoad(fileIn)) // 8bb: modifies error code
	{
		paulaClose();
		ahxFreeWaves();
		return false;
	}

	const int32_t maxSamplesPerTick = (int32_t)ceil(audioFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));

	int16_t *outputBuffer = (int16_t *)malloc(maxSamplesPerTick * (2 * sizeof (int16_t)));
	if (outputBuffer == NULL)
	{
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		ahxErrCode = ERR_OUT_OF_MEMORY;
		return false;
	}

	FILE *f = fopen(fileOut, "wb");
	if (f == NULL)
	{
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		free(outputBuffer);
		ahxErrCode = ERR_FILE_IO;
		return false;
	}

	writeWAVHeader(f, audioFreq);

	isRecordingToWAV = true;
	if (!ahxPlay(subSong)) // 8bb: modifies error code (also resets audio.tickSampleCounter64)
	{
		isRecordingToWAV = false;
		fclose(f);
		ahxFree();
		paulaClose();
		ahxFreeWaves();
		free(outputBuffer);
		return false;
	}

	song.loopTimes = songLoopTimes;

	uint32_t totalBytes = 0;
	while (isRecordingToWAV)
	{
		const uint32_t size = ahxGetFrame(outputBuffer);
		fwrite(outputBuffer, 1, size, f);
		totalBytes += size;
	}

	finishWAVHeader(f, totalBytes);
	isRecordingToWAV = false;

	fclose(f);
	ahxFree();
	paulaClose();
	ahxFreeWaves();
	free(outputBuffer);

	return true;
}

int32_t ahxGetErrorCode(void)
{
	return ahxErrCode;
}
