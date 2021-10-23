/*
** - Amiga 1200 Paula emulator w/ Amiga high-pass filter -
** Doesn't include the 34kHz low-pass filter, nor the optional "LED" filter.
**
** This code has been crafted for use with ahx2play.
** Usage outside of ahx2play may be unstable or have unwanted results.
*/

// for finding memory leaks in debug mode with Visual Studio 
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "paula.h" // AMIGA_VOICES
#include <math.h> // ceil()
#include "replayer.h" // SIDInterrupt(), AHX_LOWEST_CIA_PERIOD, AHX_DEFAULT_CIA_PERIOD

#define MAX_SAMPLE_LENGTH (0x280/2) /* in words. AHX buffer size */
#define NORM_FACTOR 1.5 /* can clip from high-pass filter overshoot */
#define STEREO_NORM_FACTOR 0.5 /* cumulative mid/side normalization factor (1/sqrt(2))*(1/sqrt(2)) */
#define INITIAL_DITHER_SEED 0x12345000

static int8_t emptySample[MAX_SAMPLE_LENGTH*2];
static int32_t randSeed = INITIAL_DITHER_SEED;
static double *dMixBufferL, *dMixBufferR, dPrngStateL, dPrngStateR, dSideFactor, dPeriodToDeltaDiv, dMixNormalize;

// globalized
audio_t audio;
paulaVoice_t paula[AMIGA_VOICES];

/*
** Math replacement
*/
#define MY_PI 3.14159265358979323846264338327950288
#define MY_TWO_PI 6.28318530717958647692528676655900576

static double my_sqrt(double x)
{
	double number = x;
	double s = number / 2.5;

	double old = 0.0;
	while (s != old)
	{
		old = s;
		s = (number / old + old) / 2.0;
	}
 
	return s;
}

static double cosTaylorSeries(double x)
{
#define ITERATIONS 32 /* good enough... */

	x = fmod(x, MY_TWO_PI);
	if (x < 0.0)
		x = -x;

	double tmp = 1.0;
	double sum = 1.0;

	for (double i = 2.0; i <= ITERATIONS*2.0; i += 2.0)
	{
		tmp *= -(x*x) / (i * (i-1.0));
		sum += tmp;
	}

	return sum;
}

static double my_cos(double x)
{
	return cosTaylorSeries(x);
}

// -----------------------------------------------
// -----------------------------------------------

/* 1-pole 6dB/oct high-pass RC filter, from:
** https://www.musicdsp.org/en/latest/Filters/116-one-pole-lp-and-hp.html
*/

// adding this prevents denormalized numbers, which is slow
#define DENORMAL_OFFSET 1e-20

typedef struct rcFilter_t
{
	double tmp[2], c1, c2;
} rcFilter_t;

static rcFilter_t filterHiA1200;

static void calcRCFilterCoeffs(double sr, double hz, rcFilter_t *f)
{
	const double a = (hz < sr/2.0) ? my_cos((MY_TWO_PI * hz) / sr) : 1.0;
	const double b = 2.0 - a;
	const double c = b - my_sqrt((b*b)-1.0);

	f->c1 = 1.0 - c;
	f->c2 = c;
}

static void clearRCFilterState(rcFilter_t *f)
{
	f->tmp[0] = f->tmp[1] = 0.0;
}

static void RCHighPassFilterStereo(rcFilter_t *f, const double *in, double *out)
{
	// left channel
	f->tmp[0] = (f->c1*in[0] + f->c2*f->tmp[0]) + DENORMAL_OFFSET;
	out[0] = in[0]-f->tmp[0];

	// right channel
	f->tmp[1] = (f->c1*in[1] + f->c2*f->tmp[1]) + DENORMAL_OFFSET;
	out[1] = in[1]-f->tmp[1];
}

// -----------------------------------------------
// -----------------------------------------------

/*
** BLEP synthesis (coded by aciddose)
*/

/* aciddose:
** information on blep variables
**
** ZC = zero crossings, the number of ripples in the impulse
** OS = oversampling, how many samples per zero crossing are taken
** SP = step size per output sample, used to lower the cutoff (play the impulse slower)
** NS = number of samples of impulse to insert
** RNS = the lowest power of two greater than NS, minus one (used to wrap output buffer)
**
** ZC and OS are here only for reference, they depend upon the data in the table and can't be changed.
** SP, the step size can be any number lower or equal to OS, as long as the result NS remains an integer.
** for example, if ZC=8,OS=5, you can set SP=1, the result is NS=40, and RNS must then be 63.
** the result of that is the filter cutoff is set at nyquist * (SP/OS), in this case nyquist/5.
*/
#define BLEP_ZC 16
#define BLEP_OS 16
#define BLEP_SP 16
#define BLEP_NS (BLEP_ZC * BLEP_OS / BLEP_SP)
#define BLEP_RNS 31 // RNS = (2^ > NS) - 1

typedef struct blep_t
{
	int32_t index, samplesLeft;
	double dBuffer[BLEP_RNS+1], dLastValue;
} blep_t;

static blep_t blep[AMIGA_VOICES];

/* Why this table is not represented as readable floating-point numbers:
** Accurate double representation in string format requires at least 14 digits and normalized
** (scientific) notation, notwithstanding compiler issues with precision or rounding error.
** Also, don't touch this table ever, just keep it exactly identical!
*/
static const uint64_t minblepdata[] =
{
	0x3FF000320C7E95A6,0x3FF00049BE220FD5,0x3FF0001B92A41ACA,0x3FEFFF4425AA9724,
	0x3FEFFDABDF6CF05C,0x3FEFFB5AF233EF1A,0x3FEFF837E2AE85F3,0x3FEFF4217B80E938,
	0x3FEFEEECEB4E0444,0x3FEFE863A8358B5F,0x3FEFE04126292670,0x3FEFD63072A0D592,
	0x3FEFC9C9CD36F56F,0x3FEFBA90594BD8C3,0x3FEFA7F008BA9F13,0x3FEF913BE2A0E0E2,
	0x3FEF75ACCB01A327,0x3FEF5460F06A4E8F,0x3FEF2C5C0389BD3C,0x3FEEFC8859BF6BCB,
	0x3FEEC3B916FD8D19,0x3FEE80AD74F0AD16,0x3FEE32153552E2C7,0x3FEDD69643CB9778,
	0x3FED6CD380FFA864,0x3FECF374A4D2961A,0x3FEC692F19B34E54,0x3FEBCCCFA695DD5C,
	0x3FEB1D44B168764A,0x3FEA59A8D8E4527F,0x3FE9814D9B10A9A3,0x3FE893C5B62135F2,
	0x3FE790EEEBF9DABD,0x3FE678FACDEE27FF,0x3FE54C763699791A,0x3FE40C4F1B1EB7A3,
	0x3FE2B9D863D4E0F3,0x3FE156CB86586B0B,0x3FDFCA8F5005B828,0x3FDCCF9C3F455DAC,
	0x3FD9C2787F20D06E,0x3FD6A984CAD0F3E5,0x3FD38BB0C452732E,0x3FD0705EC7135366,
	0x3FCABE86754E238F,0x3FC4C0801A6E9A04,0x3FBDECF490C5EA17,0x3FB2DFFACE9CE44B,
	0x3FA0EFD4449F4620,0xBF72F4A65E22806D,0xBFA3F872D761F927,0xBFB1D89F0FD31F7C,
	0xBFB8B1EA652EC270,0xBFBE79B82A37C92D,0xBFC1931B697E685E,0xBFC359383D4C8ADA,
	0xBFC48F3BFF81B06B,0xBFC537BBA8D6B15C,0xBFC557CEF2168326,0xBFC4F6F781B3347A,
	0xBFC41EF872F0E009,0xBFC2DB9F119D54D3,0xBFC13A7E196CB44F,0xBFBE953A67843504,
	0xBFBA383D9C597E74,0xBFB57FBD67AD55D6,0xBFB08E18234E5CB3,0xBFA70B06D699FFD1,
	0xBF9A1CFB65370184,0xBF7B2CEB901D2067,0x3F86D5DE2C267C78,0x3F9C1D9EF73F384D,
	0x3FA579C530950503,0x3FABD1E5FFF9B1D0,0x3FB07DCDC3A4FB5B,0x3FB2724A856EEC1B,
	0x3FB3C1F7199FC822,0x3FB46D0979F5043B,0x3FB47831387E0110,0x3FB3EC4A58A3D527,
	0x3FB2D5F45F8889B3,0x3FB145113E25B749,0x3FAE9860D18779BC,0x3FA9FFD5F5AB96EA,
	0x3FA4EC6C4F47777E,0x3F9F16C5B2604C3A,0x3F9413D801124DB7,0x3F824F668CBB5BDF,
	0xBF55B3FA2EE30D66,0xBF86541863B38183,0xBF94031BBBD551DE,0xBF9BAFC27DC5E769,
	0xBFA102B3683C57EC,0xBFA3731E608CC6E4,0xBFA520C9F5B5DEBD,0xBFA609DC89BE6ECE,
	0xBFA632B83BC5F52F,0xBFA5A58885841AD4,0xBFA471A5D2FF02F3,0xBFA2AAD5CD0377C7,
	0xBFA0686FFE4B9B05,0xBF9B88DE413ACB69,0xBF95B4EF6D93F1C5,0xBF8F1B72860B27FA,
	0xBF8296A865CDF612,0xBF691BEEDABE928B,0x3F65C04E6AF9D4F1,0x3F8035D8FFCDB0F8,
	0x3F89BED23C431BE3,0x3F90E737811A1D21,0x3F941C2040BD7CB1,0x3F967046EC629A09,
	0x3F97DE27ECE9ED89,0x3F98684DE31E7040,0x3F9818C4B07718FA,0x3F97005261F91F60,
	0x3F95357FDD157646,0x3F92D37C696C572A,0x3F8FF1CFF2BEECB5,0x3F898D20C7A72AC4,
	0x3F82BC5B3B0AE2DF,0x3F7784A1B8E9E667,0x3F637BB14081726B,0xBF4B2DACA70C60A9,
	0xBF6EFB00AD083727,0xBF7A313758DC6AE9,0xBF819D6A99164BE0,0xBF8533F57533403B,
	0xBF87CD120DB5D340,0xBF89638549CD25DE,0xBF89FB8B8D37B1BB,0xBF89A21163F9204E,
	0xBF886BA8931297D4,0xBF8673477783D71E,0xBF83D8E1CB165DB8,0xBF80BFEA7216142A,
	0xBF7A9B9BC2E40EBF,0xBF7350E806435A7E,0xBF67D35D3734AB5E,0xBF52ADE8FEAB8DB9,
	0x3F415669446478E4,0x3F60C56A092AFB48,0x3F6B9F4334A4561F,0x3F724FB908FD87AA,
	0x3F75CC56DFE382EA,0x3F783A0C23969A7B,0x3F799833C40C3B82,0x3F79F02721981BF3,
	0x3F7954212AB35261,0x3F77DDE0C5FC15C9,0x3F75AD1C98FE0777,0x3F72E5DACC0849F2,
	0x3F6F5D7E69DFDE1B,0x3F685EC2CA09E1FD,0x3F611D750E54DF3A,0x3F53C6E392A46D17,
	0x3F37A046885F3365,0xBF3BB034D2EE45C2,0xBF5254267B04B482,0xBF5C0516F9CECDC6,
	0xBF61E5736853564D,0xBF64C464B9CC47AB,0xBF669C1AEF258F56,0xBF67739985DD0E60,
	0xBF675AFD6446395B,0xBF666A0C909B4F78,0xBF64BE9879A7A07B,0xBF627AC74B119DBD,
	0xBF5F86B04069DC9B,0xBF597BE8F754AF5E,0xBF531F3EAAE9A1B1,0xBF496D3DE6AD7EA3,
	0xBF3A05FFDE4670CF,0xBF06DF95C93A85CA,0x3F31EE2B2C6547AC,0x3F41E694A378C129,
	0x3F4930BF840E23C9,0x3F4EBB5D05A0D47D,0x3F51404DA0539855,0x3F524698F56B3F33,
	0x3F527EF85309E28F,0x3F51FE70FE2513DE,0x3F50DF1642009B74,0x3F4E7CDA93517CAE,
	0x3F4A77AE24F9A533,0x3F45EE226AA69E10,0x3F411DB747374F52,0x3F387F39D229D97F,
	0x3F2E1B3D39AF5F8B,0x3F18F557BB082715,0xBEFAC04896E68DDB,0xBF20F5BC77DF558A,
	0xBF2C1B6DF3EE94A4,0xBF3254602A816876,0xBF354E90F6EAC26B,0xBF3709F2E5AF1624,
	0xBF379FCCB331CE8E,0xBF37327192ADDAD3,0xBF35EA998A894237,0xBF33F4C4977B3489,
	0xBF317EC5F68E887B,0xBF2D6B1F793EB773,0xBF2786A226B076D9,0xBF219BE6CEC2CA36,
	0xBF17D7F36D2A3A18,0xBF0AAEC5BBAB42AB,0xBEF01818DC224040,0x3EEF2F6E21093846,
	0x3F049D6E0060B71F,0x3F0E598CCAFABEFD,0x3F128BC14BE97261,0x3F148703BC70EF6A,
	0x3F1545E1579CAA25,0x3F14F7DDF5F8D766,0x3F13D10FF9A1BE0C,0x3F1206D5738ECE3A,
	0x3F0F99F6BF17C5D4,0x3F0AA6D7EA524E96,0x3F0588DDF740E1F4,0x3F0086FB6FEA9839,
	0x3EF7B28F6D6F5EED,0x3EEEA300DCBAF74A,0x3EE03F904789777C,0x3EC1BFEB320501ED,
	0xBEC310D8E585A031,0xBED6F55ECA7E151F,0xBEDFDAA5DACDD0B7,0xBEE26944F3CF6E90,
	0xBEE346894453BD1F,0xBEE2E099305CD5A8,0xBEE190385A7EA8B2,0xBEDF4D5FA2FB6BA2,
	0xBEDAD4F371257BA0,0xBED62A9CDEB0AB32,0xBED1A6DF97B88316,0xBECB100096894E58,
	0xBEC3E8A76257D275,0xBEBBF6C29A5150C9,0xBEB296292998088E,0xBEA70A10498F0E5E,
	0xBE99E52D02F887A1,0xBE88C17F4066D432,0xBE702A716CFF56CA,0x3E409F820F781F78,
	0x3E643EA99B770FE7,0x3E67DE40CDE0A550,0x3E64F4D534A2335C,0x3E5F194536BDDF7A,
	0x3E5425CEBE1FA40A,0x3E46D7B7CC631E73,0x3E364746B6582E54,0x3E21FC07B13031DE,
	0x3E064C3D91CF7665,0x3DE224F901A0AFC7,0x3DA97D57859C74A4,0x0000000000000000,

	// extra padding needed for interpolation
	0x0000000000000000
};

const double *get_minblep_table(void) { return (const double *)minblepdata; }

#define LERP(x, y, z) ((x) + ((y) - (x)) * (z))

static void blepAdd(blep_t *b, double dOffset, double dAmplitude)
{
	double f = dOffset * BLEP_SP;

	int32_t i = (int32_t)f; // get integer part of f
	const double *dBlepSrc = get_minblep_table() + i;
	f -= i; // remove integer part from f

	i = b->index;
	for (int32_t n = 0; n < BLEP_NS; n++)
	{
		b->dBuffer[i] += dAmplitude * LERP(dBlepSrc[0], dBlepSrc[1], f);
		dBlepSrc += BLEP_SP;

		i = (i + 1) & BLEP_RNS;
	}

	b->samplesLeft = BLEP_NS;
}

static double blepRun(blep_t *b, double dInput)
{
	double dBlepOutput = dInput + b->dBuffer[b->index];
	b->dBuffer[b->index] = 0.0;

	b->index = (b->index + 1) & BLEP_RNS;

	b->samplesLeft--;
	return dBlepOutput;
}

// -----------------------------------------------
// -----------------------------------------------

void paulaSetMasterVolume(int32_t vol) // 0..256
{
	audio.masterVol = CLAMP(vol, 0, 256);

	// normalization w/ phase-inversion (A1200 has a phase-inverted audio signal)
	dMixNormalize = (NORM_FACTOR * (-INT16_MAX / (double)AMIGA_VOICES)) * (audio.masterVol / 256.0);
}

void resetCachedMixerPeriod(void)
{
	paulaVoice_t *v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		v->oldPeriod = -1;
		v->dOldVoiceDelta = 0.0;
		v->dOldVoiceDeltaMul = 1.0;
	}
}

/* The following routines are only safe to call from the mixer thread,
** or from another thread if the DMAs are stopped first.
*/

void paulaSetPeriod(int32_t ch, uint16_t period)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realPeriod = period;
	if (realPeriod == 0)
		realPeriod = 1+65535; // confirmed behavior on real Amiga
	else if (realPeriod < 113)
		realPeriod = 113; // close to what happens on real Amiga (and needed for BLEP synthesis)

	// if the new period was the same as the previous period, use cached delta
	if (realPeriod != v->oldPeriod)
	{
		v->oldPeriod = realPeriod;

		// this period is not cached, calculate mixer deltas
		v->dOldVoiceDelta = dPeriodToDeltaDiv / realPeriod;

		// for BLEP synthesis (prevents division in inner mix loop)
		v->dOldVoiceDeltaMul = 1.0 / v->dOldVoiceDelta;
	}

	v->AUD_PER_delta = v->dOldVoiceDelta;

	// set BLEP stuff
	v->dDeltaMul = v->dOldVoiceDeltaMul;
	if (v->dLastDelta == 0.0)
		v->dLastDelta = v->AUD_PER_delta;
}

void paulaSetVolume(int32_t ch, uint16_t vol)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realVol = vol;

	realVol &= 127;
	if (realVol > 64)
		realVol = 64;

	// multiplying by this also scales the sample from -128 .. 127 -> -1.0 .. ~0.99
	v->AUD_VOL = realVol * (1.0 / (128.0 * 64.0));
}

void paulaSetLength(int32_t ch, uint16_t len)
{
	if (len == 0) // not what happens on a real Amiga, but this is fine for AHX
		len = 1;

	// since AHX has a fixed Paula buffer size, clamp it here
	if (len > MAX_SAMPLE_LENGTH)
		len = MAX_SAMPLE_LENGTH;
		
	paula[ch].AUD_LEN = len;
}

void paulaSetData(int32_t ch, const int8_t *src)
{
	if (src == NULL)
		src = emptySample;

	paula[ch].AUD_LC = src;
}

/* The following DMA functions are NOT to be
** used inside the audio thread!
** These are hard-written to be used the way AHX interfaces
** Paula (it initializes it outside of the replayer ticker).
*/

void paulaStopAllDMAs(void)
{
	lockMixer();

	paulaVoice_t *v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		v->DMA_active = false;
		v->location = v->AUD_LC = emptySample;
		v->lengthCounter = v->AUD_LEN = 1;
	}

	unlockMixer();
}

void paulaStartAllDMAs(void)
{
	paulaVoice_t *v;

	lockMixer();

	v = paula;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++)
	{
		if (v->AUD_LC == NULL)
			v->AUD_LC = emptySample;

		if (v->AUD_LEN == 0) // not what happens on a real Amiga, but this is fine for AHX
			v->AUD_LEN = 1;

		// since AHX has a fixed Paula buffer size, clamp it here
		if (v->AUD_LEN > MAX_SAMPLE_LENGTH)
			v->AUD_LEN = MAX_SAMPLE_LENGTH;

		/* This is not really accurate to what happens on Paula
		** during DMA start, but it's good enough.
		*/

		v->dDelta = v->dLastDelta = v->AUD_PER_delta;
		v->location = v->AUD_LC;
		v->lengthCounter = v->AUD_LEN;

		// pre-fill AUDxDAT buffer
		v->AUD_DAT[0] = *v->location++;
		v->AUD_DAT[1] = *v->location++;
		v->sampleCounter = 2;

		// set current sample point
		v->dSample = v->AUD_DAT[0] * v->AUD_VOL; // -128 .. 127 -> -1.0 .. ~0.99

		// progress AUD_DAT buffer
		v->AUD_DAT[0] = v->AUD_DAT[1];
		v->sampleCounter--;

		v->dPhase = v->dLastPhase = 0.0;
		v->dBlepOffset = 0.0;

		v->DMA_active = true;
	}

	unlockMixer();
}

static void mixChannels(int32_t numSamples)
{
	double *dMixBufSelect[AMIGA_VOICES] = { dMixBufferL, dMixBufferR, dMixBufferR, dMixBufferL };

	paulaVoice_t *v = paula;
	blep_t *bSmp = blep;

	for (int32_t i = 0; i < AMIGA_VOICES; i++, v++, bSmp++)
	{
		if (!v->DMA_active)
			continue;

		double *dMixBuf = dMixBufSelect[i]; // what output channel to mix into (L, R, R, L)
		for (int32_t j = 0; j < numSamples; j++)
		{
			double dSmp = v->dSample;
			if (dSmp != bSmp->dLastValue)
			{
				if (v->dLastDelta > v->dLastPhase)
					blepAdd(bSmp, v->dBlepOffset, bSmp->dLastValue - dSmp);

				bSmp->dLastValue = dSmp;
			}

			if (bSmp->samplesLeft > 0)
				dSmp = blepRun(bSmp, dSmp);

			dMixBuf[j] += dSmp;

			v->dPhase += v->dDelta;
			if (v->dPhase >= 1.0) // next sample point
			{
				v->dPhase -= 1.0; // we use single-step deltas (< 1.0), so this is safe

				v->dDelta = v->AUD_PER_delta; // Paula only updates period (delta) during sample fetching

				if (v->sampleCounter == 0)
				{
					// it's time to read new samples from DMA

					if (--v->lengthCounter == 0)
					{
						v->lengthCounter = v->AUD_LEN;
						v->location = v->AUD_LC;
					}

					// fill DMA data buffer
					v->AUD_DAT[0] = *v->location++;
					v->AUD_DAT[1] = *v->location++;
					v->sampleCounter = 2;
				}

				/* Pre-compute current sample point.
				** Output volume is only read from AUD_VOL at this stage,
				** and we don't emulate volume PWM anyway, so we can
				** pre-multiply by volume at this point.
				*/
				v->dSample = v->AUD_DAT[0] * v->AUD_VOL; // -128 .. 127 -> -1.0 .. ~0.99

				// progress AUD_DAT buffer
				v->AUD_DAT[0] = v->AUD_DAT[1];
				v->sampleCounter--;

				// setup BLEP stuff
				v->dBlepOffset = v->dPhase * v->dDeltaMul;
				v->dLastPhase = v->dPhase;
				v->dLastDelta = v->dDelta;
			}
		}
	}
}

void resetAudioDithering(void)
{
	randSeed = INITIAL_DITHER_SEED;
	dPrngStateL = 0.0;
	dPrngStateR = 0.0;
}

static inline int32_t random32(void)
{
	// LCG random 32-bit generator (quite good and fast)
	randSeed *= 134775813;
	randSeed++;
	return randSeed;
}

void paulaMixSamples(int16_t *target, int32_t numSamples)
{
	int32_t smp32;
	double dOut[2], dPrng;

	mixChannels(numSamples);

	// apply filter, normalize, adjust stereo separation (if needed), dither and quantize
	
	if (audio.stereoSeparation == 100) // Amiga panning (no stereo separation)
	{
		for (int32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			// clear what we read
			dMixBufferL[i] = 0.0;
			dMixBufferR[i] = 0.0;

			RCHighPassFilterStereo(&filterHiA1200, dOut, dOut);

			double dL = dOut[0] * dMixNormalize;
			double dR = dOut[1] * dMixNormalize;

			// left channel - 1-bit triangular dithering (high-pass filtered)
			dPrng = random32() * (0.5 / INT32_MAX); // -0.5 .. 0.5
			dL = (dL + dPrng) - dPrngStateL;
			dPrngStateL = dPrng;
			smp32 = (int32_t)dL;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;

			// right channel - 1-bit triangular dithering (high-pass filtered)
			dPrng = random32() * (0.5 / INT32_MAX); // -0.5 .. 0.5
			dR = (dR + dPrng) - dPrngStateR;
			dPrngStateR = dPrng;
			smp32 = (int32_t)dR;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;
		}
	}
	else
	{
		for (int32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			dMixBufferL[i] = 0.0;
			dMixBufferR[i] = 0.0;

			RCHighPassFilterStereo(&filterHiA1200, dOut, dOut);

			double dL = dOut[0] * dMixNormalize;
			double dR = dOut[1] * dMixNormalize;

			// apply stereo separation
			const double dOldL = dL;
			const double dOldR = dR;
			double dMid  = (dOldL + dOldR) * STEREO_NORM_FACTOR;
			double dSide = (dOldL - dOldR) * dSideFactor;
			dL = dMid + dSide;
			dR = dMid - dSide;
			// -----------------------

			// left channel
			dPrng = random32() * (0.5 / INT32_MAX);
			dL = (dL + dPrng) - dPrngStateL;
			dPrngStateL = dPrng;
			smp32 = (int32_t)dL;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;

			// right channel
			dPrng = random32() * (0.5 / INT32_MAX);
			dR = (dR + dPrng) - dPrngStateR;
			dPrngStateR = dPrng;
			smp32 = (int32_t)dR;
			CLAMP16(smp32);
			*target++ = (int16_t)smp32;
		}
	}
}

void paulaTogglePause(void)
{
	audio.pause ^= 1;
}

void paulaOutputSamples(int16_t *stream, int32_t numSamples)
{
	int16_t *streamOut = (int16_t *)stream;

	if (audio.pause)
	{
		memset(stream, 0, numSamples * 2 * sizeof (short));
		return;
	}

	int32_t samplesLeft = numSamples;
	while (samplesLeft > 0)
	{
		if (audio.tickSampleCounter64 <= 0) // new replayer tick
		{
			SIDInterrupt(); // replayer.c
			audio.tickSampleCounter64 += audio.samplesPerTick64;
		}

		const int32_t remainingTick = (audio.tickSampleCounter64 + UINT32_MAX) >> 32; // ceil rounding (upwards)

		int32_t samplesToMix = samplesLeft;
		if (samplesToMix > remainingTick)
			samplesToMix = remainingTick;

		paulaMixSamples(streamOut, samplesToMix);
		streamOut += samplesToMix * 2;

		samplesLeft -= samplesToMix;
		audio.tickSampleCounter64 -= (int64_t)samplesToMix << 32;
	}
}

void paulaClearFilterState(void)
{
	clearRCFilterState(&filterHiA1200);
}

static void calculateFilterCoeffs(void)
{
	// Amiga 1200 1-pole (6dB/oct) static RC high-pass filter
	double R = 1390.0; // R324 (1K ohm resistor) + R325 (390 ohm resistor)
	double C = 2.2e-5; // C334 (22uF capacitor)
	double fc = 1.0 / (MY_TWO_PI * R * C); // cutoff = ~5.20Hz
	calcRCFilterCoeffs(audio.outputFreq, fc, &filterHiA1200);

	paulaClearFilterState();
}

void paulaSetStereoSeparation(int32_t percentage) // 0..100 (percentage)
{
	audio.stereoSeparation = CLAMP(percentage, 0, 100);
	dSideFactor = (percentage / 100.0) * STEREO_NORM_FACTOR;
}

double amigaCIAPeriod2Hz(uint16_t period)
{
	if (period == 0)
		return 0.0;

	return (double)CIA_PAL_CLK / (period+1); // +1, CIA triggers on underflow
}

bool amigaSetCIAPeriod(uint16_t period) // replayer ticker
{
	const double dCIAHz = amigaCIAPeriod2Hz(period);
	if (dCIAHz == 0.0)
		return false;

	const double dSamplesPerTick = audio.outputFreq / dCIAHz;
	audio.samplesPerTick64 = (int64_t)(dSamplesPerTick * (UINT32_MAX+1.0)); // 32.32fp

	return true;
}

bool paulaInit(int32_t audioFrequency)
{
	const int32_t minFreq = (int32_t)(PAULA_PAL_CLK / 113.0)+1; // mixer requires single-step deltas
	audio.outputFreq = CLAMP(audioFrequency, minFreq, 384000);

	// set defaults
	paulaSetStereoSeparation(20);
	paulaSetMasterVolume(256);

	dPeriodToDeltaDiv = (double)PAULA_PAL_CLK / audio.outputFreq;

	int32_t maxSamplesToMix = (int32_t)ceil(audio.outputFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));

	const int32_t bufferBytes = maxSamplesToMix * sizeof (double);

	dMixBufferL = (double *)calloc(1, bufferBytes);
	dMixBufferR = (double *)calloc(1, bufferBytes);

	if (dMixBufferL == NULL || dMixBufferR == NULL)
	{
		paulaClose();
		return false;
	}

	calculateFilterCoeffs();

	amigaSetCIAPeriod(AHX_DEFAULT_CIA_PERIOD);
	audio.tickSampleCounter64 = 0; // clear tick sample counter so that it will instantly initiate a tick

	resetAudioDithering();
	resetCachedMixerPeriod();
	return true;
}

void paulaClose(void)
{
	if (dMixBufferL != NULL)
	{
		free(dMixBufferL);
		dMixBufferL = NULL;
	}

	if (dMixBufferR != NULL)
	{
		free(dMixBufferR);
		dMixBufferR = NULL;
	}
}
