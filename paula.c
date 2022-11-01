/*
** - Amiga 1200 Paula emulator w/ filters -
** Doesn't include the optional "LED" filter. This was never used in AHX anyway.
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
#include <math.h> // ceil()
#include "replayer.h" // SIDInterrupt(), AHX_LOWEST_CIA_PERIOD, AHX_DEFAULT_CIA_PERIOD

#define MAX_SAMPLE_LENGTH (0x280/2) /* in words. AHX buffer size */
#define NORM_FACTOR 1.5 /* this is a good value between loudness and clipping */
#define STEREO_NORM_FACTOR 0.5 /* cumulative mid/side normalization factor (1/sqrt(2))*(1/sqrt(2)) */
#define INITIAL_DITHER_SEED 0x12345000

static bool useA1200LowPassFilter;
static int8_t emptySample[MAX_SAMPLE_LENGTH*2];
static int32_t randSeed = INITIAL_DITHER_SEED;
static double *dMixBufferL, *dMixBufferR, dPrngStateL, dPrngStateR, dSideFactor, dPeriodToDeltaDiv, dMixNormalize;

// globalized
audio_t audio;
paulaVoice_t paula[PAULA_VOICES];

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

typedef struct rcFilter_t
{
	double tmp[2], c1, c2;
} rcFilter_t;

static rcFilter_t filterLoA1200, filterHiA1200;

static void calcRCFilterCoeffs(double sr, double hz, rcFilter_t *f)
{
	const double a = (hz < sr / 2.0) ? my_cos((MY_TWO_PI * hz) / sr) : 1.0;
	const double b = 2.0 - a;
	const double c = b - my_sqrt((b*b)-1.0);

	f->c1 = 1.0 - c;
	f->c2 = c;
}

static void clearRCFilterState(rcFilter_t *f)
{
	f->tmp[0] = f->tmp[1] = 0.0;
}

static void RCLowPassFilterStereo(rcFilter_t *f, const double *in, double *out)
{
	// left channel
	f->tmp[0] = (f->c1 * in[0]) + (f->c2 * f->tmp[0]);
	out[0] = f->tmp[0];

	// right channel
	f->tmp[1] = (f->c1 * in[1]) + (f->c2 * f->tmp[1]);
	out[1] = f->tmp[1];
}

static void RCHighPassFilterStereo(rcFilter_t *f, const double *in, double *out)
{
	// left channel
	f->tmp[0] = (f->c1 * in[0]) + (f->c2 * f->tmp[0]);
	out[0] = in[0] - f->tmp[0];

	// right channel
	f->tmp[1] = (f->c1 * in[1]) + (f->c2 * f->tmp[1]);
	out[1] = in[1] - f->tmp[1];
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

static blep_t blep[PAULA_VOICES];

static const double dMinblepData[256 + 1] =
{
	 1.000047730261351741631870027, 1.000070326525919428561905988, 1.000026295486963423542192686, 0.999910424773336803383472216,
	 0.999715744379055859525351480, 0.999433014919733908598686867, 0.999050085771328588712947294, 0.998551121919525108694415394,
	 0.997915706233591937035498631, 0.997117832692634098457062919, 0.996124815495205595539118804, 0.994896148570364013963285288,
	 0.993382359323431773923118726, 0.991523909003057091204880180, 0.989250199364479221308954493, 0.986478750833182482793404233,
	 0.983114620682589257505412661, 0.979050130425507592057954298, 0.974164969358674692756494551, 0.968326735771705471300663248,
	 0.961391968634788374181709969, 0.953207710646355677042151910, 0.943613628528589098998224927, 0.932444698727279863703643059,
	 0.919534446669115101968827730, 0.904718706053873278349897191, 0.887839842029686909796737382, 0.868751359331251915563143484,
	 0.847322794437510795617640724, 0.823444770447693374926245724, 0.797034075604916458779314326, 0.768038612100722994924240083,
	 0.736442051783192774827568883, 0.702268030364621043126760469, 0.665583712234169455612686761, 0.626502564400415073997407944,
	 0.585186190589438104403541274, 0.541845095055891845525763983, 0.496738269945924404424886234, 0.450171529567763739621000241,
	 0.402494548939336449500103754, 0.354096601546017020201162495, 0.305401031227847563620514393, 0.256858534249934655768754510,
	 0.208939368513054252174399039, 0.162124646097439151226637932, 0.116896901459689991908952322, 0.073730159227936173382822460,
	 0.033079751373986221452128120,-0.004627847551233893637345762,-0.039004887382349466562470042,-0.069711629260494178961238276,
	-0.096464776709362487494558991,-0.119044790560825133884925719,-0.137301851759562276722448360,-0.151160268717908163882412964,
	-0.160621165999489917686204876,-0.165763337555641210308010614,-0.166742199141503621984128358,-0.163786829309547077304642926,
	-0.157195144771094669211564110,-0.147327312088839507131510231,-0.134597551740997606328775760,-0.119464540741507418974975963,
	-0.102420664473805989036492292,-0.083980405628003879092702277,-0.064668186778692057781192659,-0.045006002136687713044427284,
	-0.025501182606377806316722001,-0.006634636085273460347211394, 0.011150108072625494748386643, 0.027456744995838545247979212,
	 0.041944658451493331552395460, 0.054335772988313046916175608, 0.064419613137017092685532305, 0.072056443764197217194400480,
	 0.077178424602408784993556878, 0.079788772844964592212413379, 0.079958988468210145938996902, 0.077824255600564926083073658,
	 0.073577187846729327769246254, 0.067460134194076051827870799, 0.059756303384108616638670242, 0.050779997099921050929260957,
	 0.040866264989502618099059816, 0.030360306751458156215850437, 0.019606947961234157812304701, 0.008940507097049575288560952,
	-0.001324648208126466466388882,-0.010902586500777250097526938,-0.019543107356485005243751374,-0.027037657667711743197935803,
	-0.033223730539404389139335194,-0.037987660680654206091233505,-0.041265784496258707536586741,-0.043043987097204458591725995,
	-0.043355710312406585404954029,-0.042278543756813086185175621,-0.039929563528408616723819335,-0.036459618831699062979634363,
	-0.032046794692908199542191738,-0.026889298182776331241905510,-0.021198025763611533928143515,-0.015189070430455576393713457,
	-0.009076419455364113236806034,-0.003065077316892155564337363, 0.002655175361548794011473662, 0.007915206247809156159256361,
	 0.012570993866018958726171739, 0.016507022146950881685834034, 0.019638542128970374461838233, 0.021912677934460975809338734,
	 0.023308395228452325614876273, 0.023835389125096861917540991, 0.023531983633596965932444078, 0.022462165098004915897433875,
	 0.020711896771322700627759872, 0.018384880003473082210607714, 0.015597939105523964467558962, 0.012476211636472618604631890,
	 0.009148323764166686397625305, 0.005741721847351755579624832, 0.002378317065490789024989615,-0.000829419425005380466127403,
	-0.003781796760683148444365242,-0.006394592475568152724341164,-0.008601029202315702004710829,-0.010353009833494400057651852,
	-0.011621609729198234539637724,-0.012396851839770069853008394,-0.012686815497510708569683935,-0.012516151297987356677543502,
	-0.011924092281628086154032786,-0.010962065062340150406461348,-0.009691013345942120493781147,-0.008178550345073604815882007,
	-0.006496056025074358440674072,-0.004715830178801836899959987,-0.002908403455554361104196115,-0.001140096220006421448914247,
	 0.000529099845866712065883819, 0.002047259427257062253113773, 0.003371840725899812995364213, 0.004470560830528139475981142,
	 0.005321826318522163493107691, 0.005914733331712991419581993, 0.006248666963769690732566353, 0.006332543236118748190832672,
	 0.006183747823531677602348910, 0.005826833745009315536356187, 0.005292045316197638814281756, 0.004613737750803969042689978,
	 0.003828761006839943078355892, 0.002974873016054367744903653, 0.002089241623845963964634098, 0.001207086791118088800120467,
	 0.000360505313776118753877481,-0.000422490021201840063209965,-0.001118695810940623525803206,-0.001710197865787731110603920,
	-0.002184605984988852254297109,-0.002535053949380879686342771,-0.002759983640474954029453425,-0.002862739419386348127538611,
	-0.002851004510552685496799219,-0.002736115017663406229209144,-0.002532289317276390557681642,-0.002255810970882980801693884,
	-0.001924202080457771161722813,-0.001555421358101014960712005,-0.001167117308478276627506376,-0.000775962090624877551779670,
	-0.000397086112821087974713435,-0.000043627508742770710237387, 0.000273595371614305691784774, 0.000546286179945486565119606,
	 0.000768750680536383766867925, 0.000937862797633428843004089, 0.001052928740415311594305625, 0.001115464566897328381120391,
	 0.001128904823558016723081265, 0.001098261208043675284801166, 0.001029750572351520628011645, 0.000930411077785372455858925,
	 0.000807724029027989654482000, 0.000669256978018619233528064, 0.000522341243078797709889494, 0.000373794189875729877727689,
	 0.000229693626208723626408101, 0.000095208625460110475721871,-0.000025511844279469758173208,-0.000129393822077144692462430,
	-0.000214440509776957504047001,-0.000279687383686450809737456,-0.000325117484787396354272565,-0.000351545144152561614761532,
	-0.000360476947384593608518510,-0.000353958823942885139873099,-0.000334417806276329982514278,-0.000304506298090264292902779,
	-0.000266955691183075056582136,-0.000224444953912546549387383,-0.000179488462294699944012469,-0.000134345936549333598141603,
	-0.000090955956048889888797271,-0.000050893220267083281950406,-0.000015348557788785960678823, 0.000014870297520588306796089,
	 0.000039319915274267510328903, 0.000057887658269859997505705, 0.000070747063526119185008535, 0.000078305819543209774719408,
	 0.000081149939344861922907622, 0.000079987451948008292520673, 0.000075594520611004130655058, 0.000068766382254997158183021,
	 0.000060274927715552435942073, 0.000050834144795034220151546, 0.000041074060306237671633470, 0.000031523273709852359548023,
	 0.000022599698095009569128290, 0.000014608732178951863478521, 0.000007747790946751235789929, 0.000002115927046159247276264,
	-0.000002272821614624657917699,-0.000005473727618614671290435,-0.000007594607649309153682893,-0.000008779148282948222000235,
	-0.000009191289914471795693680,-0.000009001415957147717553273,-0.000008374862616584595860708,-0.000007463035714495605026032,
	-0.000006397209079216532043657,-0.000005284894977678962862369,-0.000004208528817468251939280,-0.000003226102468093129124012,
	-0.000002373314390120065336351,-0.000001666778737492929471595,-0.000001107845639559032712541,-0.000000686624974759576246945,
	-0.000000385868818649388185867,-0.000000184445440432801241354,-0.000000060222272723601931954, 0.000000007740724047001495273,
	 0.000000037708832885684096027, 0.000000044457942869060847864, 0.000000039034296310091607592, 0.000000028962932871006776366,
	 0.000000018763994698223029203, 0.000000010636937008622986639, 0.000000005187099504706206719, 0.000000002093670467469700098,
	 0.000000000648951812097509606, 0.000000000132018063854003986, 0.000000000011591335682393882, 0.000000000000000000000000000,

	 0.000000000000000000000000000 // 8bitbubsy: one extra zero is required for interpolation look-up
};

#define LERP(x, y, z) ((x) + ((y) - (x)) * (z))

static inline void blepAdd(blep_t *b, double dOffset, double dAmplitude)
{
	double f = dOffset * BLEP_SP;

	int32_t i = (int32_t)f; // 8bitbubsy: get integer part of f
	const double *dBlepSrc = dMinblepData + i;
	f -= i; // 8bitbubsy: remove integer part from f

	i = b->index;
	for (int32_t n = 0; n < BLEP_NS; n++)
	{
		b->dBuffer[i] += dAmplitude * LERP(dBlepSrc[0], dBlepSrc[1], f);
		dBlepSrc += BLEP_SP;

		i = (i + 1) & BLEP_RNS;
	}

	b->samplesLeft = BLEP_NS;
}

static inline double blepRun(blep_t *b, double dInput)
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
	dMixNormalize = (NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES)) * (audio.masterVol / 256.0);
}


/* The following routines are only safe to call from the mixer thread,
** or from another thread if the DMAs are stopped first.
*/

void paulaSetPeriod(int32_t ch, uint16_t period)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realPeriod = period;
	if (realPeriod == 0)
		realPeriod = 65535; // On Amiga: period 0 = one full cycle with period 65536, then period 65535 for the rest
	else if (realPeriod < 113)
		realPeriod = 113; // close to what happens on real Amiga (and needed for BLEP synthesis)

	// to be read on next sampling step (or on DMA trigger)
	v->AUD_PER_delta = dPeriodToDeltaDiv / realPeriod;
	v->AUD_PER_deltamul = 1.0 / v->AUD_PER_delta; // for BLEP synthesis (prevents division in inner mixing loop)

	// handle BLEP synthesis edge-cases

	if (v->dLastDelta == 0.0)
		v->dLastDelta = v->AUD_PER_delta;

	if (v->dLastDeltaMul == 0.0)
		v->dLastDeltaMul = v->AUD_PER_deltamul;
}

void paulaSetVolume(int32_t ch, uint16_t vol)
{
	paulaVoice_t *v = &paula[ch];

	int32_t realVol = vol & 127;
	if (realVol > 64)
		realVol = 64;

	// multiplying sample point by this also scales the sample from -128..127 -> -1.0 .. ~0.99
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

static inline void refetchPeriod(paulaVoice_t *v) // Paula stage
{
	// set BLEP stuff
	v->dLastPhase = v->dPhase;
	v->dLastDelta = v->dDelta;
	v->dLastDeltaMul = v->dDeltaMul;
	v->dBlepOffset = v->dLastPhase * v->dLastDeltaMul;

	// Paula only updates period (delta) during period refetching (this stage)
	v->dDelta = v->AUD_PER_delta;
	v->dDeltaMul = v->AUD_PER_deltamul;

	v->nextSampleStage = true;
}

static void startPaulaDMA(int32_t ch)
{
	paulaVoice_t *v = &paula[ch];

	if (v->AUD_LC == NULL)
		v->AUD_LC = emptySample;

	// immediately update AUD_LC/AUD_LEN
	v->location = v->AUD_LC;
	v->lengthCounter = v->AUD_LEN;

	// make Paula fetch new samples immediately
	v->sampleCounter = 0;
	v->DMATriggerFlag = true;

	refetchPeriod(v);
	v->dPhase = 0.0; // kludge: must be cleared *after* refetchPeriod()

	v->DMA_active = true;
}

static void stopPaulaDMA(int32_t ch)
{
	paula[ch].DMA_active = false;
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
		if (!v->DMATriggerFlag)
		{
			if (--v->lengthCounter == 0)
			{
				v->lengthCounter = v->AUD_LEN;
				v->location = v->AUD_LC;
			}
		}

		v->DMATriggerFlag = false;

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
	v->dSample = v->AUD_DAT[0] * v->AUD_VOL; // -128..127 * 0.0 .. 1.0

	// fill BLEP buffer if the new sample differs from the old one
	if (v->dSample != b->dLastValue)
	{
		if (v->dLastDelta > v->dLastPhase)
			blepAdd(b, v->dBlepOffset, b->dLastValue - v->dSample);

		b->dLastValue = v->dSample;
	}

	// progress AUD_DAT buffer
	v->AUD_DAT[0] = v->AUD_DAT[1];
	v->sampleCounter--;
}

static void paulaGenerateSamples(double *dOutL, double *dOutR, int32_t numSamples)
{
	double *dMixBufSelect[PAULA_VOICES] = { dOutL, dOutR, dOutR, dOutL };

	memset(dOutL, 0, numSamples * sizeof (double));
	memset(dOutR, 0, numSamples * sizeof (double));

	paulaVoice_t *v = paula;
	blep_t *b = blep;

	for (int32_t i = 0; i < PAULA_VOICES; i++, v++, b++)
	{
		if (!v->DMA_active || v->location == NULL || v->AUD_LC == NULL)
			continue;

		double *dMixBuffer = dMixBufSelect[i]; // what output channel to mix into (L, R, R, L)
		for (int32_t j = 0; j < numSamples; j++)
		{
			if (v->nextSampleStage)
			{
				v->nextSampleStage = false;
				nextSample(v, b); // inlined
			}

			double dSample = v->dSample; // current Paula sample, pre-multiplied by volume, scaled to -1.0 .. 0.9921875
			if (b->samplesLeft > 0)
				dSample = blepRun(b, dSample);

			dMixBuffer[j] += dSample;

			v->dPhase += v->dDelta;
			if (v->dPhase >= 1.0)
			{
				v->dPhase -= 1.0;
				refetchPeriod(v); // inlined
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

/* High-quality /2 decimator from
** https://www.musicdsp.org/en/latest/Filters/231-hiqh-quality-2-decimators.html
*/

static double R1_L, R2_L, R3_L, R4_L, R5_L, R6_L, R7_L, R8_L, R9_L;
static double R1_R, R2_R, R3_R, R4_R, R5_R, R6_R, R7_R, R8_R, R9_R;

void clearMixerDownsamplerStates(void)
{
	R1_L = R2_L = R3_L = R4_L = R5_L = R6_L = R7_L = R8_L = R9_L = 0.0;
	R1_R = R2_R = R3_R = R4_R = R5_R = R6_R = R7_R = R8_R = R9_R = 0.0;
}

static double decimate2x_L(double x0, double x1)
{
	const double h0 = 8192.0 / 16384.0;
	const double h1 = 5042.0 / 16384.0;
	const double h3 = -1277.0 / 16384.0;
	const double h5 = 429.0 / 16384.0;
	const double h7 = -116.0 / 16384.0;
	const double h9 = 18.0 / 16384.0;

	double h9x0 = h9 * x0;
	double h7x0 = h7 * x0;
	double h5x0 = h5 * x0;
	double h3x0 = h3 * x0;
	double h1x0 = h1 * x0;
	double R10 = R9_L + h9x0;

	R9_L = R8_L + h7x0;
	R8_L = R7_L + h5x0;
	R7_L = R6_L + h3x0;
	R6_L = R5_L + h1x0;
	R5_L = R4_L + h1x0 + h0 * x1;
	R4_L = R3_L + h3x0;
	R3_L = R2_L + h5x0;
	R2_L = R1_L + h7x0;
	R1_L = h9x0;

	return R10;
}

static double decimate2x_R(double x0, double x1)
{
	const double h0 = 8192.0 / 16384.0;
	const double h1 = 5042.0 / 16384.0;
	const double h3 = -1277.0 / 16384.0;
	const double h5 = 429.0 / 16384.0;
	const double h7 = -116.0 / 16384.0;
	const double h9 = 18.0 / 16384.0;

	double h9x0 = h9 * x0;
	double h7x0 = h7 * x0;
	double h5x0 = h5 * x0;
	double h3x0 = h3 * x0;
	double h1x0 = h1 * x0;
	double R10 = R9_R + h9x0;

	R9_R = R8_R + h7x0;
	R8_R = R7_R + h5x0;
	R7_R = R6_R + h3x0;
	R6_R = R5_R + h1x0;
	R5_R = R4_R + h1x0 + h0 * x1;
	R4_R = R3_R + h3x0;
	R3_R = R2_R + h5x0;
	R2_R = R1_R + h7x0;
	R1_R = h9x0;

	return R10;
}

// ------------------------------------------------------

static inline void processMixedSamplesAmigaPanning(uint32_t i, int16_t *out)
{
	int32_t smp32;
	double dPrng;

	double dL = dMixBufferL[i];
	double dR = dMixBufferR[i];

	// normalize w/ phase-inversion (A500/A1200 has a phase-inverted audio signal)
	dL *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);
	dR *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);

	// left channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dL = (dL + dPrng) - dPrngStateL;
	dPrngStateL = dPrng;
	smp32 = (int32_t)dL;
	CLAMP16(smp32);
	out[0] = (int16_t)smp32;

	// right channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dR = (dR + dPrng) - dPrngStateR;
	dPrngStateR = dPrng;
	smp32 = (int32_t)dR;
	CLAMP16(smp32);
	out[1] = (int16_t)smp32;
}

static inline void processMixedSamples(uint32_t i, int16_t *out)
{
	int32_t smp32;
	double dPrng;

	double dL = dMixBufferL[i];
	double dR = dMixBufferR[i];

	// apply stereo separation
	const double dOldL = dL;
	const double dOldR = dR;
	double dMid = (dOldL + dOldR) * STEREO_NORM_FACTOR;
	double dSide = (dOldL - dOldR) * dSideFactor;
	dL = dMid + dSide;
	dR = dMid - dSide;

	// normalize w/ phase-inversion (A500/A1200 has a phase-inverted audio signal)
	dL *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);
	dR *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);

	// left channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dL = (dL + dPrng) - dPrngStateL;
	dPrngStateL = dPrng;
	smp32 = (int32_t)dL;
	CLAMP16(smp32);
	out[0] = (int16_t)smp32;

	// right channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dR = (dR + dPrng) - dPrngStateR;
	dPrngStateR = dPrng;
	smp32 = (int32_t)dR;
	CLAMP16(smp32);
	out[1] = (int16_t)smp32;
}

static inline void processMixedSamplesAmigaPanning_2x(uint32_t i, int16_t *out) // 2x oversampling
{
	int32_t smp32;
	double dPrng, dL, dR;

	// 2x downsampling (decimation)
	const uint32_t offset1 = (i << 1) + 0;
	const uint32_t offset2 = (i << 1) + 1;
	dL = decimate2x_L(dMixBufferL[offset1], dMixBufferL[offset2]);
	dR = decimate2x_R(dMixBufferR[offset1], dMixBufferR[offset2]);

	// normalize w/ phase-inversion (A500/A1200 has a phase-inverted audio signal)
	dL *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);
	dR *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);

	// left channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dL = (dL + dPrng) - dPrngStateL;
	dPrngStateL = dPrng;
	smp32 = (int32_t)dL;
	CLAMP16(smp32);
	out[0] = (int16_t)smp32;

	// right channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dR = (dR + dPrng) - dPrngStateR;
	dPrngStateR = dPrng;
	smp32 = (int32_t)dR;
	CLAMP16(smp32);
	out[1] = (int16_t)smp32;
}

static inline void processMixedSamples_2x(uint32_t i, int16_t *out) // 2x oversampling
{
	int32_t smp32;
	double dPrng, dL, dR;

	// 2x downsampling (decimation)
	const uint32_t offset1 = (i << 1) + 0;
	const uint32_t offset2 = (i << 1) + 1;
	dL = decimate2x_L(dMixBufferL[offset1], dMixBufferL[offset2]);
	dR = decimate2x_R(dMixBufferR[offset1], dMixBufferR[offset2]);

	// apply stereo separation
	const double dOldL = dL;
	const double dOldR = dR;
	double dMid = (dOldL + dOldR) * STEREO_NORM_FACTOR;
	double dSide = (dOldL - dOldR) * dSideFactor;
	dL = dMid + dSide;
	dR = dMid - dSide;

	// normalize w/ phase-inversion (A500/A1200 has a phase-inverted audio signal)
	dL *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);
	dR *= NORM_FACTOR * (-INT16_MAX / (double)PAULA_VOICES);

	// left channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dL = (dL + dPrng) - dPrngStateL;
	dPrngStateL = dPrng;
	smp32 = (int32_t)dL;
	CLAMP16(smp32);
	out[0] = (int16_t)smp32;

	// right channel - 1-bit triangular dithering (high-pass filtered)
	dPrng = random32() * (0.5 / INT32_MAX); // -0.5..0.5
	dR = (dR + dPrng) - dPrngStateR;
	dPrngStateR = dPrng;
	smp32 = (int32_t)dR;
	CLAMP16(smp32);
	out[1] = (int16_t)smp32;
}

static void processFilters(uint32_t numSamples)
{
	double dOut[2];

	if (useA1200LowPassFilter)
	{
		for (uint32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			// low-pass RC filter
			RCLowPassFilterStereo(&filterLoA1200, dOut, dOut);

			// high-pass RC filter
			RCHighPassFilterStereo(&filterHiA1200, dOut, dOut);

			dMixBufferL[i] = dOut[0];
			dMixBufferR[i] = dOut[1];
		}
	}
	else
	{
		for (uint32_t i = 0; i < numSamples; i++)
		{
			dOut[0] = dMixBufferL[i];
			dOut[1] = dMixBufferR[i];

			// high-pass RC filter
			RCHighPassFilterStereo(&filterHiA1200, dOut, dOut);

			dMixBufferL[i] = dOut[0];
			dMixBufferR[i] = dOut[1];
		}
	}
}

void paulaMixSamples(int16_t *target, uint32_t numSamples)
{
	// apply filter, normalize, adjust stereo separation (if needed), dither and quantize
	
	if (audio.oversamplingFlag) // 2x oversampling
	{
		// mix and filter channels (at 2x rate)
		paulaGenerateSamples(dMixBufferL, dMixBufferR, numSamples*2);
		processFilters(numSamples*2);

		// downsample, normalize and dither
		int16_t out[2];
		int16_t *outStream = target;
		if (audio.stereoSeparation == 100)
		{
			for (uint32_t i = 0; i < numSamples; i++)
			{
				processMixedSamplesAmigaPanning_2x(i, out);
				*outStream++ = out[0];
				*outStream++ = out[1];
			}
		}
		else
		{
			for (uint32_t i = 0; i < numSamples; i++)
			{
				processMixedSamples_2x(i, out);
				*outStream++ = out[0];
				*outStream++ = out[1];
			}
		}
	}
	else
	{
		paulaGenerateSamples(dMixBufferL, dMixBufferR, numSamples);
		processFilters(numSamples);

		// normalize and dither
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
	clearRCFilterState(&filterLoA1200);
	clearRCFilterState(&filterHiA1200);
}

static void calculateFilterCoeffs(void)
{
	/* Amiga 1200 filter emulation
	**
	** NOTE: Doesn't include the Sallen-Key "LED" filter because AHX never uses it.
	**
	** Keep in mind that many of the Amiga schematics that are floating around on
	** the internet have wrong RC values! They were most likely very early schematics
	** that didn't change before production (or changes that never reached production).
	** This has been confirmed by measuring the components on several Amiga motherboards.
	**
	** Correct values for A1200, all revs (A1200_R2.pdf):
	** - 1-pole RC 6dB/oct low-pass: R=680 ohm, C=6800pF
	** - Sallen-key low-pass ("LED"): R1/R2=10k ohm, C1=6800pF, C2=3900pF (same as A500)
	** - 1-pole RC 6dB/oct high-pass: R=1390 ohm (1000+390), C=22uF
	*/
	double dAudioFreq = audio.outputFreq;
	double R, C, fc;

	if (audio.oversamplingFlag)
		dAudioFreq *= 2.0; // 2x oversampling

	// A1200 1-pole (6db/oct) static RC low-pass filter:
	R = 680.0;  // R321 (680 ohm)
	C = 6.8e-9; // C321 (6800pF)
	fc = 1.0 / (MY_TWO_PI * R * C); // cutoff = ~34419.32Hz

	useA1200LowPassFilter = false;
	if (dAudioFreq/2.0 > fc)
	{
		calcRCFilterCoeffs(dAudioFreq, fc, &filterLoA1200);
		useA1200LowPassFilter = true;
	}

	// A1200 1-pole (6dB/oct) static RC high-pass filter:
	R = 1390.0; // R324 (1K ohm resistor) + R325 (390 ohm resistor)
	C = 2.2e-5; // C334 (22uF capacitor)
	fc = 1.0 / (MY_TWO_PI * R * C); // cutoff = ~5.20Hz
	calcRCFilterCoeffs(dAudioFreq, fc, &filterHiA1200);

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

	// we do 2x oversampling if the audio output rate is below 96kHz
	audio.oversamplingFlag = (audio.outputFreq < 96000);

	dPeriodToDeltaDiv = (double)PAULA_PAL_CLK / audio.outputFreq;
	if (audio.oversamplingFlag)
		dPeriodToDeltaDiv *= 0.5;

	int32_t maxSamplesToMix = (int32_t)ceil(audio.outputFreq / amigaCIAPeriod2Hz(AHX_HIGHEST_CIA_PERIOD));
	if (audio.oversamplingFlag)
		maxSamplesToMix *= 2;

	dMixBufferL = (double *)malloc(maxSamplesToMix * sizeof (double));
	dMixBufferR = (double *)malloc(maxSamplesToMix * sizeof (double));

	if (dMixBufferL == NULL || dMixBufferR == NULL)
	{
		paulaClose();
		return false;
	}

	calculateFilterCoeffs();

	amigaSetCIAPeriod(AHX_DEFAULT_CIA_PERIOD);
	audio.tickSampleCounter64 = 0; // clear tick sample counter so that it will instantly initiate a tick

	resetAudioDithering();
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
