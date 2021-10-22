/* Example program for interfacing with ahx2play.
**
** Please excuse my disgusting platform-independant code here...
*/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../replayer.h"
#include "posix.h"

// defaults when not overriden by argument switches
#define DEFAULT_AUDIO_FREQ 48000
#define DEFAULT_AUDIO_BUFSIZE 1024
#define DEFAULT_MASTER_VOL 256
#define DEFAULT_STEREO_SEPARATION 10
#define DEFAULT_WAVRENDER_LOOPS 0

// set to true if you want ahx2play to always render to WAV
#define DEFAULT_WAVRENDER_MODE_FLAG false

// default settings
static bool renderToWavFlag = DEFAULT_WAVRENDER_MODE_FLAG;
static int32_t stereoSeparation = DEFAULT_STEREO_SEPARATION;
static int32_t masterVolume = DEFAULT_MASTER_VOL;
static int32_t audioFrequency = DEFAULT_AUDIO_FREQ;
static int32_t audioBufferSize = DEFAULT_AUDIO_BUFSIZE;
static int32_t WAVSongLoopTimes = DEFAULT_WAVRENDER_LOOPS;
// ----------------------------------------------------------

static volatile bool programRunning;
static char *filename, *WAVRenderFilename;
static int32_t oldStereoSeparation;

static void showUsage(void);
static void handleArguments(int argc, char *argv[]);
static void readKeyboard(void);
static int32_t renderToWav(void);

// yuck!
#ifdef _WIN32
static DWORD WINAPI wavRecordingThread(LPVOID arg)
#else
static void *wavRecordingThread(void *arg)
#endif
{
	// 8bb: put this in a thread so that it can be cancelled at any time by pressing a key (it can get stuck in a loop)
	ahxRecordWAV(filename, WAVRenderFilename, 0, WAVSongLoopTimes, audioFrequency, masterVolume, stereoSeparation);

#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif

	(void)arg;
}

#ifndef _WIN32
static void sigtermFunc(int32_t signum)
{
	programRunning = false; // unstuck main loop
	(void)signum;
}
#endif

int main(int argc, char *argv[])
{
#ifdef _DEBUG
	filename = "debug.ahx";
	(void)argc;
	(void)argv;
#else
	if (argc < 2 || (argc == 2 && (!strcmp(argv[1], "/?") || !strcmp(argv[1], "-h"))))
	{
		showUsage();
#ifdef _WIN32
		system("PAUSE");
#endif
		return 1;
	}

	handleArguments(argc, argv);
#endif

	if (renderToWavFlag)
		return renderToWav();

	// Initialize AHX system
	if (!ahxInit(audioFrequency, audioBufferSize, masterVolume, stereoSeparation))
	{
		ahxClose();

		printf("Error initializing AHX replayer: ");
		switch (ahxGetErrorCode())
		{
			default: printf("Unknown error...\n"); break;

			case ERR_OUT_OF_MEMORY:
				printf("Out of memory!\n");
			break;

			case ERR_AUDIO_DEVICE:
				printf("Error setting up audio output device!\n");
			break;
		}

		return 1;
	}

	// Load song
	if (!ahxLoad(filename))
	{
		ahxClose();

		printf("Error loading AHX module: ");
		switch (ahxGetErrorCode())
		{
			default: printf("Unknown error...\n"); break;

			case ERR_NO_WAVES:
				printf("Waveforms are not initialized! Did you call ahxInit()?\n");
			break;

			case ERR_OUT_OF_MEMORY:
				printf("Out of memory!\n");
			break;

			case ERR_FILE_IO:
				printf("Generic file I/O error! Does the file exist? Is it in use?\n");
			break;

			case ERR_NOT_AN_AHX:
				printf("This is not an AHX module!\n");
			break;
		}

		return 1;
	}

	// Play song (start at song #0)
	if (!ahxPlay(0))
	{
		ahxFree();
		ahxClose();

		printf("Error playing AHX module: ");
		switch (ahxGetErrorCode())
		{
			default: printf("Unknown error...\n"); break;

			case ERR_SONG_NOT_LOADED:
				printf("No song was loaded! Did you call ahxLoad()?\n");
			break;

			case ERR_NO_WAVES:
				printf("Waveforms are not initialized! Did you call ahxInit()?\n");
			break;

		}
	}

	// trap sigterm on Linux/macOS (since we need to properly revert the terminal)
#ifndef _WIN32
	struct sigaction action;
	memset(&action, 0, sizeof (struct sigaction));
	action.sa_handler = sigtermFunc;
	sigaction(SIGTERM, &action, NULL);
#endif

	printf("Controls:\n");
	printf("    Esc = Quit\n");
	printf("      r = Restart song\n");
	printf("  Space = Toggle pause\n");
	printf("   Plus = Next song position\n");
	printf("  Minus = Previous song position\n");
	printf("      n = Next sub-song (if any)\n");
	printf("      p = Previous sub-song (if any)\n");
	printf("      h = Toggle Amiga hard-panning\n");
	printf("\n");
	printf("Master volume: %d (%d%%)\n", audio.masterVol, (int32_t)((audio.masterVol / 256.0) * 100));
	printf("Audio output frequency: %dHz\n", audio.outputFreq);
	printf("Initial stereo separation: %d%%\n", audio.stereoSeparation);
	printf("\n");
	printf("- SONG INFO -\n");
	printf(" Name: %s\n", song.Name);
	printf(" Song revision: v%d\n", song.Revision);
	printf(" Sub-songs: %d\n", song.Subsongs);
	printf(" Song length: %d (restart pos: %d)\n", song.LenNr, song.ResNr);
	printf(" Song tick rate: %.4fHz (%.2f BPM)\n", song.dBPM / 2.5, song.dBPM);
	printf(" Track length: %d\n", song.TrackLength);
	printf(" Instruments: %d\n", song.numInstruments);
	printf("\n");
	printf("- STATUS -\n");

#ifndef _WIN32
	modifyTerminal();
#endif
	hideTextCursor();

	oldStereoSeparation = audio.stereoSeparation; // for toggling separation with 'h' key

	programRunning = true;
	while (programRunning)
	{
		readKeyboard();

		printf(" Pos: %03d/%03d - Row: %02d/%02d - Speed: %d %s               \r",
			song.PosNr, song.LenNr, song.NoteNr, song.TrackLength, song.Tempo,
			audio.pause ? "(PAUSED)" : "");

		fflush(stdout);
		Sleep(50);
	}
	printf("\n");

#ifndef _WIN32
	revertTerminal();
#endif
	showTextCursor();

	// Free loaded song
	ahxFree();

	// Close AHX system
	ahxClose();

	printf("Playback stopped.\n");
	return 0;
}

static void showUsage(void)
{
	printf("Usage:\n");
	printf("  ahx2play input_module [-f hz] [-m mixingvol] [-b buffersize]\n");
	printf("  ahx2play input_module [-s percentage] [--render-to-wav] [-wloop loops]\n");
	printf("\n");
	printf("  Options:\n");
	printf("    input_module     Specifies the module file to load (.AHX/.THX)\n");
	printf("    -f hz            Specifies the audio frequency (32000..384000)\n");
	printf("    -m mastervol     Specifies the master volume (0..256)\n");
	printf("    -b buffersize    Specifies the audio buffer size (256..8192)\n");
	printf("    -s percentage    Specifies the stereo separation (0..100).\n");
	printf("                     0 = mono, 100 = Amiga hard-panning.\n");
	printf("    --render-to-wav  Renders song to WAV instead of playing it. The output\n");
	printf("                     filename will be the input filename with .WAV added to the\n");
	printf("                     end.\n");
	printf("    --wloop loops    Specifies how many times to loop the song during WAV write.\n");
	printf("                     Parameter 0 = no loop, 1 = loop 1 time, etc.\n");
	printf("                     Any F00 command will stop the song regardless of setting.\n");
	printf("\n");
	printf("Default settings (can only be changed in the source code):\n");
	printf("  - Audio frequency:          %dHz\n", DEFAULT_AUDIO_FREQ);
	printf("  - Audio buffer size:        %d\n", DEFAULT_AUDIO_BUFSIZE);
	printf("  - Master volume:            %d\n", DEFAULT_MASTER_VOL);
	printf("  - Stereo separation:        %d%%\n", DEFAULT_STEREO_SEPARATION);
	printf("  - WAV render mode:          %s\n", DEFAULT_WAVRENDER_MODE_FLAG ? "On" : "Off");
	printf("  - WAV song loop times:      %d\n", DEFAULT_WAVRENDER_LOOPS);
	printf("\n");
}

static void handleArguments(int argc, char *argv[])
{
	filename = argv[1];
	if (argc > 2) // parse arguments
	{
		for (int32_t i = 1; i < argc; i++)
		{
			if (!_stricmp(argv[i], "-f") && i+1 < argc)
			{
				const int32_t num = atoi(argv[i+1]);
				audioFrequency = CLAMP(num, 32000, 384000);
			}
			else if (!_stricmp(argv[i], "-m") && i+1 < argc)
			{
				const int32_t num = atoi(argv[i+1]);
				masterVolume = CLAMP(num, 0, 256);
			}
			else if (!_stricmp(argv[i], "-b") && i+1 < argc)
			{
				const int32_t num = atoi(argv[i+1]);
				audioBufferSize = CLAMP(num, 256, 8192);
			}
			else if (!_stricmp(argv[i], "-s") && i+1 < argc)
			{
				const int32_t num = atoi(argv[i+1]);
				stereoSeparation = CLAMP(num, 0, 100);
			}
			else if (!_stricmp(argv[i], "--render-to-wav"))
			{
				renderToWavFlag = true;
			}
			else if (!_stricmp(argv[i], "-wloops") && i + 1 < argc)
			{
				const int32_t num = atoi(argv[i + 1]);
				WAVSongLoopTimes = CLAMP(num, 0, 100);
			}
		}
	}
}

static void readKeyboard(void)
{
	if (_kbhit())
	{
		const int32_t key = _getch();
		switch (key)
		{
			case 0x1B: // esc
				programRunning = false;
			break;

			case 'r': // restart
				ahxPlay(0);
			break;

			case 'n': // next sub-song
			{
				if (song.Subsongs > 0)
				{
					if (song.Subsong < song.Subsongs)
						ahxPlay(song.Subsong + 1);
				}
			}
			break;

			case 'p': // previous sub-song
			{
				if (song.Subsongs > 0)
				{
					if (song.Subsong > 0)
						ahxPlay(song.Subsong - 1);
				}
			}
			break;

			case 'h': // toggle Amiga hard-pan
			{
				if (audio.stereoSeparation == 100)
					paulaSetStereoSeparation(oldStereoSeparation);
				else
					paulaSetStereoSeparation(100);
			}
			break;

			case 0x20: // space (toggle pause)
				paulaTogglePause();
			break;

			case 0x2B: // numpad + (next song position)
				ahxNextPattern();
			break;

			case 0x2D: // numpad - (previous song position)
				ahxPrevPattern();
			break;
			
			default: break;
		}
	}
}

static int32_t renderToWav(void)
{
	const size_t filenameLen = strlen(filename);
	WAVRenderFilename = (char *)malloc(filenameLen+1+3+1);

	if (WAVRenderFilename == NULL)
	{
		printf("Error: Out of memory!\n");
		return 1;
	}

	strcpy(WAVRenderFilename, filename);
	strcat(WAVRenderFilename, ".wav");

	isRecordingToWAV = true; // this is also set in wavRecordingThread(), but do it here to be sure...
	if (!createSingleThread(wavRecordingThread))
	{
		printf("Error: Couldn't create WAV rendering thread!\n");
		free(WAVRenderFilename);
		return 1;
	}

	printf("Rendering to WAV. If stuck forever, press any key to stop rendering...\n");

#ifndef _WIN32
	modifyTerminal();
#endif
	while (isRecordingToWAV)
	{
		if ( _kbhit())
			isRecordingToWAV = false;

		Sleep(50);
	}
	Sleep(250); // needed for safety (making sure WAV thread closed properly before we got here)

#ifndef _WIN32
	revertTerminal();
#endif

	closeSingleThread();

	free(WAVRenderFilename);
	return 0;
}
