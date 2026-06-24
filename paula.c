/*
** - Simple Amiga 1200 Paula emulator w/ BLEP synthesis -
**
** This code has been crafted for use with ahx2play.
** Usage outside of ahx2play may be unstable or give unwanted results.
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
#include "paula.h" // PAULA_VOICES
#include <math.h>
#include "replayer.h" // tickReplayer(), AHX_DEFAULT_CIA_PERIOD

#define MAX_SAMPLE_LENGTH (0x280/2) /* in words. AHX buffer size */
#define AUDIO_GAIN 1.75f /* this is a good value between loudness and clipping */
#define STEREO_NORM_FACTOR 0.5f /* cumulative mid/side normalization factor (1/sqrt(2))*(1/sqrt(2)) */
#define INITIAL_DITHER_SEED 0x12345000

static int8_t nullSample[MAX_SAMPLE_LENGTH*2];
static uint32_t randSeed = INITIAL_DITHER_SEED;
static float *fMixBufferL, *fMixBufferR, fPrngStateL, fPrngStateR, fSideFactor, fPeriodToDeltaDiv, fMixNormalize;

// globalized
audio_t audio;
paulaVoice_t paula[PAULA_VOICES];

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
	float fBuffer[BLEP_RNS+1], fLastValue;
} blep_t;

static blep_t blep[PAULA_VOICES];

static const float fMinBlepData[256+1] = // zero-crossings = 16, oversampling = 16
{
	 1.0000477302613517416f, 1.0000703265259194286f, 1.0000262954869634235f, 0.9999104247733368034f,
	 0.9997157443790558595f, 0.9994330149197339086f, 0.9990500857713285887f, 0.9985511219195251087f,
	 0.9979157062335919370f, 0.9971178326926340985f, 0.9961248154952055955f, 0.9948961485703640140f,
	 0.9933823593234317739f, 0.9915239090030570912f, 0.9892501993644792213f, 0.9864787508331824828f,
	 0.9831146206825892575f, 0.9790501304255075921f, 0.9741649693586746928f, 0.9683267357717054713f,
	 0.9613919686347883742f, 0.9532077106463556770f, 0.9436136285285890990f, 0.9324446987272798637f,
	 0.9195344466691151020f, 0.9047187060538732783f, 0.8878398420296869098f, 0.8687513593312519156f,
	 0.8473227944375107956f, 0.8234447704476933749f, 0.7970340756049164588f, 0.7680386121007229949f,
	 0.7364420517831927748f, 0.7022680303646210431f, 0.6655837122341694556f, 0.6265025644004150740f,
	 0.5851861905894381044f, 0.5418450950558918455f, 0.4967382699459244044f, 0.4501715295677637396f,
	 0.4024945489393364495f, 0.3540966015460170202f, 0.3054010312278475636f, 0.2568585342499346558f,
	 0.2089393685130542522f, 0.1621246460974391512f, 0.1168969014596899919f, 0.0737301592279361734f,
	 0.0330797513739862215f,-0.0046278475512338936f,-0.0390048873823494666f,-0.0697116292604941790f,
	-0.0964647767093624875f,-0.1190447905608251339f,-0.1373018517595622767f,-0.1511602687179081639f,
	-0.1606211659994899177f,-0.1657633375556412103f,-0.1667421991415036220f,-0.1637868293095470773f,
	-0.1571951447710946692f,-0.1473273120888395071f,-0.1345975517409976063f,-0.1194645407415074190f,
	-0.1024206644738059890f,-0.0839804056280038791f,-0.0646681867786920578f,-0.0450060021366877130f,
	-0.0255011826063778063f,-0.0066346360852734603f, 0.0111501080726254947f, 0.0274567449958385452f,
	 0.0419446584514933316f, 0.0543357729883130469f, 0.0644196131370170927f, 0.0720564437641972172f,
	 0.0771784246024087850f, 0.0797887728449645922f, 0.0799589884682101459f, 0.0778242556005649261f,
	 0.0735771878467293278f, 0.0674601341940760518f, 0.0597563033841086166f, 0.0507799970999210509f,
	 0.0408662649895026181f, 0.0303603067514581562f, 0.0196069479612341578f, 0.0089405070970495753f,
	-0.0013246482081264665f,-0.0109025865007772501f,-0.0195431073564850052f,-0.0270376576677117432f,
	-0.0332237305394043891f,-0.0379876606806542061f,-0.0412657844962587075f,-0.0430439870972044586f,
	-0.0433557103124065854f,-0.0422785437568130862f,-0.0399295635284086167f,-0.0364596188316990630f,
	-0.0320467946929081995f,-0.0268892981827763312f,-0.0211980257636115339f,-0.0151890704304555764f,
	-0.0090764194553641132f,-0.0030650773168921556f, 0.0026551753615487940f, 0.0079152062478091562f,
	 0.0125709938660189587f, 0.0165070221469508817f, 0.0196385421289703745f, 0.0219126779344609758f,
	 0.0233083952284523256f, 0.0238353891250968619f, 0.0235319836335969659f, 0.0224621650980049159f,
	 0.0207118967713227006f, 0.0183848800034730822f, 0.0155979391055239645f, 0.0124762116364726186f,
	 0.0091483237641666864f, 0.0057417218473517556f, 0.0023783170654907890f,-0.0008294194250053805f,
	-0.0037817967606831484f,-0.0063945924755681527f,-0.0086010292023157020f,-0.0103530098334944001f,
	-0.0116216097291982345f,-0.0123968518397700699f,-0.0126868154975107086f,-0.0125161512979873567f,
	-0.0119240922816280862f,-0.0109620650623401504f,-0.0096910133459421205f,-0.0081785503450736048f,
	-0.0064960560250743584f,-0.0047158301788018369f,-0.0029084034555543611f,-0.0011400962200064214f,
	 0.0005290998458667121f, 0.0020472594272570623f, 0.0033718407258998130f, 0.0044705608305281395f,
	 0.0053218263185221635f, 0.0059147333317129914f, 0.0062486669637696907f, 0.0063325432361187482f,
	 0.0061837478235316776f, 0.0058268337450093155f, 0.0052920453161976388f, 0.0046137377508039690f,
	 0.0038287610068399431f, 0.0029748730160543677f, 0.0020892416238459640f, 0.0012070867911180888f,
	 0.0003605053137761188f,-0.0004224900212018401f,-0.0011186958109406235f,-0.0017101978657877311f,
	-0.0021846059849888523f,-0.0025350539493808797f,-0.0027599836404749540f,-0.0028627394193863481f,
	-0.0028510045105526855f,-0.0027361150176634062f,-0.0025322893172763906f,-0.0022558109708829808f,
	-0.0019242020804577712f,-0.0015554213581010150f,-0.0011671173084782766f,-0.0007759620906248776f,
	-0.0003970861128210880f,-0.0000436275087427707f, 0.0002735953716143057f, 0.0005462861799454866f,
	 0.0007687506805363838f, 0.0009378627976334288f, 0.0010529287404153116f, 0.0011154645668973284f,
	 0.0011289048235580167f, 0.0010982612080436753f, 0.0010297505723515206f, 0.0009304110777853725f,
	 0.0008077240290279897f, 0.0006692569780186192f, 0.0005223412430787977f, 0.0003737941898757299f,
	 0.0002296936262087236f, 0.0000952086254601105f,-0.0000255118442794698f,-0.0001293938220771447f,
	-0.0002144405097769575f,-0.0002796873836864508f,-0.0003251174847873964f,-0.0003515451441525616f,
	-0.0003604769473845936f,-0.0003539588239428851f,-0.0003344178062763300f,-0.0003045062980902643f,
	-0.0002669556911830751f,-0.0002244449539125465f,-0.0001794884622946999f,-0.0001343459365493336f,
	-0.0000909559560488899f,-0.0000508932202670833f,-0.0000153485577887860f, 0.0000148702975205883f,
	 0.0000393199152742675f, 0.0000578876582698600f, 0.0000707470635261192f, 0.0000783058195432098f,
	 0.0000811499393448619f, 0.0000799874519480083f, 0.0000755945206110041f, 0.0000687663822549972f,
	 0.0000602749277155524f, 0.0000508341447950342f, 0.0000410740603062377f, 0.0000315232737098524f,
	 0.0000225996980950096f, 0.0000146087321789519f, 0.0000077477909467512f, 0.0000021159270461592f,
	-0.0000022728216146247f,-0.0000054737276186147f,-0.0000075946076493092f,-0.0000087791482829482f,
	-0.0000091912899144718f,-0.0000090014159571477f,-0.0000083748626165846f,-0.0000074630357144956f,
	-0.0000063972090792165f,-0.0000052848949776790f,-0.0000042085288174683f,-0.0000032261024680931f,
	-0.0000023733143901201f,-0.0000016667787374929f,-0.0000011078456395590f,-0.0000006866249747596f,
	-0.0000003858688186494f,-0.0000001844454404328f,-0.0000000602222727236f, 0.0000000077407240470f,
	 0.0000000377088328857f, 0.0000000444579428691f, 0.0000000390342963101f, 0.0000000289629328710f,
	 0.0000000187639946982f, 0.0000000106369370086f, 0.0000000051870995047f, 0.0000000020936704675f,
	 0.0000000006489518121f, 0.0000000001320180639f, 0.0000000000115913357f, 0.0000000000000000000f,

	 0.0000000000000000000f // copy of last point required for interpolation
};

// linear interpolation macro
#define LERP(p1, p2, frac) ((p1) + (((p2) - (p1)) * (frac)))

static void inline blepAdd(blep_t *b, const float fOffset, const float fAmplitude)
{
	float f = fOffset * BLEP_SP;

	const int32_t fInt = (int32_t)f; // get integer part of f
	const float *fBlepSrc = fMinBlepData + fInt;
	f -= fInt; // remove integer part from f

	int32_t i = b->index;
	for (int32_t n = 0; n < BLEP_NS; n++)
	{
		b->fBuffer[i] += fAmplitude * LERP(fBlepSrc[0], fBlepSrc[1], f);
		fBlepSrc += BLEP_SP;

		i = (i + 1) & BLEP_RNS;
	}

	b->samplesLeft = BLEP_NS;
}

static float inline blepRun(blep_t *b, const float fInput)
{
	float fBlepOutput = fInput + b->fBuffer[b->index];
	b->fBuffer[b->index] = 0.0f;

	b->index = (b->index + 1) & BLEP_RNS;

	b->samplesLeft--;
	return fBlepOutput;
}

// -----------------------------------------------
// -----------------------------------------------

void paulaSetMasterVolume(int32_t vol) // 0..256
{
	audio.masterVol = CLAMP(vol, 0, 256);

	// normalization multiplier
	fMixNormalize = (float)(AUDIO_GAIN * ((INT16_MAX+1.0) / PAULA_VOICES)) * (audio.masterVol / 256.0f);
}

/* The following routines are only safe to call from the mixer thread,
** or from another thread if the DMAs are stopped first.
*/

void paulaSetPeriod(int32_t ch, uint16_t period)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realPeriod = period;
	if (realPeriod == 0)
		realPeriod = 65535; // On Amiga: period 0 = period 65536 (1+65535)
	else if (realPeriod < 113)
		realPeriod = 113; // close to what happens on real Amiga (and low-limit needed for BLEP synthesis)

	// to be read on next sampling step (or on DMA trigger)
	v->fStoredDelta = fPeriodToDeltaDiv / (float)realPeriod;

	// BLEP synthesis edge-case
	if (v->fBlepDelta == 0.0f)
		v->fBlepDelta = v->fDelta;
}

void paulaSetVolume(int32_t ch, uint16_t vol)
{
	int32_t realVol = vol & 127;
	if (realVol > 64)
		realVol = 64;

	// multiplying sample point by this also scales the sample from -128..127 -> -1.000 .. ~0.992
	paula[ch].fStoredVol = realVol * (1.0f / (128.0f * 64.0f));
}

void paulaSetLength(int32_t ch, uint16_t len)
{
	// since AHX has a fixed Paula buffer size, clamp it here
	if (len == 0 || len > MAX_SAMPLE_LENGTH)
		len = MAX_SAMPLE_LENGTH;
		
	paula[ch].storedLength = len;
}

void paulaSetData(int32_t ch, const int8_t *src)
{
	if (src == NULL)
		src = nullSample;

	paula[ch].storedLocation = src;
}

static inline void refetchPeriod(paulaVoice_t *v) // Paula stage
{
	v->fBlepPhase = v->fPhase;
	v->fBlepDelta = v->fDelta;

	// Paula only updates period (delta) during period refetching (this stage)
	v->fDelta = v->fStoredDelta;

	v->nextSampleStage = true;
}

static void startPaulaDMA(int32_t ch)
{
	paulaVoice_t *v = &paula[ch];

	if (v->storedLocation == NULL)
		v->storedLocation = nullSample;

	// immediately update these
	v->location = v->storedLocation;
	v->lengthCounter = v->storedLength;

	// make Paula fetch new samples immediately
	v->sampleCounter = 0;
	v->sampleJustStarted = true;
	refetchPeriod(v);

	// kludge: must be cleared *after* refetchPeriod()
	v->fPhase = 0.0f;

	v->active = true;
}

static void stopPaulaDMA(int32_t ch)
{
	paula[ch].active = false;
}

void paulaSetDMACON(uint16_t bits) // $DFF096 register write (only controls paula DMAs)
{
	if (bits & 0x8000)
	{
		// set
		if (bits & 1) startPaulaDMA(0);
		if (bits & 2) startPaulaDMA(1);
		if (bits & 4) startPaulaDMA(2);
		if (bits & 8) startPaulaDMA(3);
	}
	else
	{
		// clear
		if (bits & 1) stopPaulaDMA(0);
		if (bits & 2) stopPaulaDMA(1);
		if (bits & 4) stopPaulaDMA(2);
		if (bits & 8) stopPaulaDMA(3);
	}
}

static inline void nextSample(paulaVoice_t *v, blep_t *b)
{
	if (v->sampleCounter == 0)
	{
		// it's time to read new samples from DMA

		// don't update AUD_LEN/AUD_LC yet on DMA trigger
		if (!v->sampleJustStarted)
		{
			if (--v->lengthCounter == 0)
			{
				v->lengthCounter = v->storedLength;
				v->location = v->storedLocation;
			}
		}

		v->sampleJustStarted = false;

		// fill DMA data buffer
		v->AUD_DAT[0] = *v->location++;
		v->AUD_DAT[1] = *v->location++;
		v->sampleCounter = 2;
	}

	/* Pre-compute current sample point.
	** Output volume is only read from AUDxVOL at this stage,
	** and we don't emulate volume PWM anyway, so we can
	** pre-multiply by volume here.
	*/
	v->fSample = v->AUD_DAT[0] * v->fStoredVol; // -128..127 * 0.0f .. 1.0f

	// fill BLEP buffer if the new sample differs from the old one
	if (v->fSample != b->fLastValue)
	{
		if (v->fBlepDelta > v->fBlepPhase) // also checks if v->fBlepDelta > 0.0f
		{
			const float fBlepOffset = v->fBlepPhase / v->fBlepDelta;
			blepAdd(b, fBlepOffset, b->fLastValue - v->fSample);
		}

		b->fLastValue = v->fSample;
	}

	// progress AUD_DAT buffer
	v->AUD_DAT[0] = v->AUD_DAT[1];
	v->sampleCounter--;
}

static void paulaGenerateSamples(float *fOutL, float *fOutR, int32_t numSamples)
{
	float *fMixBufSelect[PAULA_VOICES];

	if (numSamples <= 0)
		return;

	fMixBufSelect[0] = fOutL;
	fMixBufSelect[1] = fOutR;
	fMixBufSelect[2] = fOutR;
	fMixBufSelect[3] = fOutL;

	// clear mix buffer block
	memset(fOutL, 0, numSamples * sizeof (float));
	memset(fOutR, 0, numSamples * sizeof (float));

	// mix samples

	paulaVoice_t *v = paula;
	blep_t *b = blep;

	for (int32_t i = 0; i < PAULA_VOICES; i++, v++, b++)
	{
		if (!v->active || v->location == NULL || v->storedLocation == NULL)
			continue;

		float *fMixBuffer = fMixBufSelect[i]; // what output channel to mix into (L, R, R, L)
		for (int32_t j = 0; j < numSamples; j++)
		{
			if (v->nextSampleStage)
			{
				v->nextSampleStage = false;
				nextSample(v, b); // inlined
			}

			float fSample = v->fSample; // current sample, pre-multiplied by vol, scaled to -1.0 .. 0.992
			if (b->samplesLeft > 0)
				fSample = blepRun(b, fSample);

			fMixBuffer[j] += fSample;

			v->fPhase += v->fDelta;
			if (v->fPhase >= 1.0f)
			{
				v->fPhase -= 1.0f;
				refetchPeriod(v);
			}
		}
	}
}

void resetAudioDithering(void)
{
	randSeed = INITIAL_DITHER_SEED;
	fPrngStateL = 0.0f;
	fPrngStateR = 0.0f;
}

static inline int32_t random32(void)
{
	// LCG 32-bit random
	randSeed *= 134775813;
	randSeed++;

	return (int32_t)randSeed;
}

static inline void processMixedSamplesAmigaPanning(uint32_t i, int16_t *out)
{
	int32_t out32;
	float fOut, fPrng;

	float fL = fMixBufferL[i];
	float fR = fMixBufferR[i];

	// normalize
	fL *= fMixNormalize;
	fR *= fMixNormalize;

	// left channel - 1-bit triangular dithering
	fPrng = (float)random32() * (1.0f / ((float)UINT32_MAX+1.0f)); // -0.5f .. 0.5f
	fOut = (fL + fPrng) - fPrngStateL;
	fPrngStateL = fPrng;
	out32 = (int32_t)fOut;
	out[0] = (int16_t)(CLAMP(out32, INT16_MIN, INT16_MAX));

	// right channel - 1-bit triangular dithering
	fPrng = (float)random32() * (1.0f / ((float)UINT32_MAX+1.0f)); // -0.5f .. 0.5f
	fOut = (fR + fPrng) - fPrngStateR;
	fPrngStateR = fPrng;
	out32 = (int32_t)fOut;
	out[1] = (int16_t)(CLAMP(out32, INT16_MIN, INT16_MAX));
}

static inline void processMixedSamples(uint32_t i, int16_t *out)
{
	int32_t out32;
	float fOut, fPrng;

	float fL = fMixBufferL[i];
	float fR = fMixBufferR[i];

	// apply stereo separation
	const float fOldL = fL;
	const float fOldR = fR;
	float fMid  = (fOldL + fOldR) * STEREO_NORM_FACTOR;
	float fSide = (fOldL - fOldR) * fSideFactor;
	fL = fMid + fSide;
	fR = fMid - fSide;

	// normalize
	fL *= fMixNormalize;
	fR *= fMixNormalize;

	// left channel - 1-bit triangular dithering
	fPrng = (float)random32() * (1.0f / ((float)UINT32_MAX+1.0f)); // -0.5f .. 0.5f
	fOut = (fL + fPrng) - fPrngStateL;
	fPrngStateL = fPrng;
	out32 = (int32_t)fOut;
	out[0] = (int16_t)(CLAMP(out32, INT16_MIN, INT16_MAX));

	// right channel - 1-bit triangular dithering
	fPrng = (float)random32() * (1.0f / ((float)UINT32_MAX+1.0f)); // -0.5f .. 0.5f
	fOut = (fR + fPrng) - fPrngStateR;
	fPrngStateR = fPrng;
	out32 = (int32_t)fOut;
	out[1] = (int16_t)(CLAMP(out32, INT16_MIN, INT16_MAX));
}

void paulaMixSamples(int16_t *target, uint32_t numSamples)
{
	// normalize, adjust stereo separation (if needed), dither and quantize
	
	paulaGenerateSamples(fMixBufferL, fMixBufferR, numSamples);

	int16_t out[2];
	int16_t *outStream = target;
	if (audio.stereoSeparation == 100)
	{
		for (uint32_t i = 0; i < numSamples; i++)
		{
			processMixedSamplesAmigaPanning(i, out);
			*outStream++ = out[0];
			*outStream++ = out[1];
		}
	}
	else
	{
		for (uint32_t i = 0; i < numSamples; i++)
		{
			processMixedSamples(i, out);
			*outStream++ = out[0];
			*outStream++ = out[1];
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
		memset(stream, 0, numSamples * 2 * sizeof (int16_t));
		return;
	}

	int32_t samplesLeft = numSamples;
	while (samplesLeft > 0)
	{
		if (audio.tickSampleCounter <= 0) // new replayer tick
		{
			tickReplayer();
			audio.tickSampleCounter = audio.samplesPerTickInt;

			audio.tickSampleCounterFrac += audio.samplesPerTickFrac;
			if (audio.tickSampleCounterFrac >= BPM_FRAC_SCALE)
			{
				audio.tickSampleCounterFrac &= BPM_FRAC_MASK;
				audio.tickSampleCounter++;
			}
		}

		int32_t samplesToMix = samplesLeft;
		if (audio.tickSampleCounter > 0 && samplesToMix > audio.tickSampleCounter)
			samplesToMix = audio.tickSampleCounter;

		paulaMixSamples(streamOut, samplesToMix);
		streamOut += samplesToMix * 2; // *2 for stereo

		samplesLeft -= samplesToMix;
		audio.tickSampleCounter -= samplesToMix;
	}
}

void paulaSetStereoSeparation(int32_t percentage) // 0..100 (percentage)
{
	audio.stereoSeparation = CLAMP(percentage, 0, 100);
	fSideFactor = (audio.stereoSeparation / 100.0f) * STEREO_NORM_FACTOR;
}

double amigaCIAPeriod2Hz(uint16_t period)
{
	if (period == 0)
		return 0.0;

	return (double)CIA_PAL_CLK / (period+1); // +1, CIA triggers on underflow
}

bool amigaSetCIAPeriod(uint16_t period)
{
	const double dCIAHz = amigaCIAPeriod2Hz(period);
	if (dCIAHz == 0.0)
		return false;

	const double dSamplesPerTick = audio.outputFreq / dCIAHz;

	double dSamplesPerTickInt, dSamplesPerTickFrac = modf(dSamplesPerTick, &dSamplesPerTickInt);

	audio.samplesPerTickInt = (uint32_t)dSamplesPerTickInt;
	audio.samplesPerTickFrac = (uint64_t)(dSamplesPerTickFrac * BPM_FRAC_SCALE);

	return true;
}

bool paulaInit(int32_t audioFrequency)
{
	const int32_t minFreq = (int32_t)(PAULA_PAL_CLK / 113.0)+1; // mixer requires single-step deltas
	audio.outputFreq = CLAMP(audioFrequency, minFreq, 384000);

	// set defaults
	paulaSetStereoSeparation(20);
	paulaSetMasterVolume(256);

	fPeriodToDeltaDiv = (float)((double)PAULA_PAL_CLK / audio.outputFreq);

	int32_t maxSamplesToMix = (int32_t)ceil(audio.outputFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));

	fMixBufferL = (float *)malloc(maxSamplesToMix * sizeof (float));
	fMixBufferR = (float *)malloc(maxSamplesToMix * sizeof (float));

	if (fMixBufferL == NULL || fMixBufferR == NULL)
	{
		paulaClose();
		return false;
	}

	amigaSetCIAPeriod(AHX_DEFAULT_CIA_PERIOD);

	audio.tickSampleCounter = 0; // zero tick sample counter so that it will instantly initiate a tick
	audio.samplesPerTickFrac = 0;

	resetAudioDithering();
	return true;
}

void paulaClose(void)
{
	if (fMixBufferL != NULL)
	{
		free(fMixBufferL);
		fMixBufferL = NULL;
	}

	if (fMixBufferR != NULL)
	{
		free(fMixBufferR);
		fMixBufferR = NULL;
	}
}
