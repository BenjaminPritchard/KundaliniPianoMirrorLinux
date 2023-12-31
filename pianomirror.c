//
// PianoMirror.c
// https://www.kundalinisoftware.com/piano-mirror/
//
// Benjamin Pritchard / Kundalini Software
//
// This is the ANSI-C style version of this program (that I run on my Raspberri PI.) However, I think it can be
// compiled to work on any UNIX-like system.
//
// The point of this is to perform MIDI remapping to create a left handed piano using the portmidi libraries.
// (see webpage above for more information.)
//
// Version History in version.txt
//

const char *VersionString = "2.0";

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>

#include "portmidi/portmidi.h"
#include "portmidi/pmutil.h"
#include "portmidi/porttime.h"
#include "metronome.h"
#include "logo.h"

#include "lua/include/lua.h"
#include "lua/include/lualib.h"
#include "lua/include/lauxlib.h"

#ifdef USE_NATS
#include "nats/nats.h"
#define DEFAULT_NATS_URL "nats://localhost:4222"
#endif

// message queues for the main thread to communicate with the call back
PmQueue *callback_to_main;
PmQueue *main_to_callback;

#define STRING_MAX 80

PmStream *midi_in;	// incoming midi data from piano
PmStream *midi_out; // (transposed) outgoing midi

#define IN_QUEUE_SIZE 1024
#define OUT_QUEUE_SIZE 1024

int MIDIchannel = 0;
int MIDIInputDevice = -1;  // -1 means to use the default; this can be overridden on the commmand line
int MIDIOutputDevice = -1; // -1 means to use the default; this can be overridden on the commmand line

bool ShowMIDIData;
int NoteOffset = 0;

extern int bpm;
extern bool metronome_enabled;

char SCRIPT_LOCATION[] = "scripts/";

lua_State *Lua_State;
bool script_is_loaded = FALSE;
char script_file[255];

// NOTE: it is possible to compile this code without using the NATS library at all
// additionally, if we ARE compiling with NATS, then
// NATS can OPTIONALLY be enabled on the command line when invoking this program
#ifdef USE_NATS
int natsreceive = 0;
int natsbroadcast = 0;
#endif

// simple structure to pass messages back and forth between the main thread and our callback
// these messages are inserted into
typedef struct
{

	int cmdCode;
	int Param1;
	int Param2;

} CommandMessage;

// messages from the main thread
#define CMD_QUIT_MSG 1
#define CMD_SET_SPLIT_POINT 2
#define CMD_SET_MODE 3

// ackknowledgement of received message
#define CMD_MSG_ACK 1000

// flag indicating
int callback_exit_flag;

// transposition modes we support
enum transpositionModes
{
	NO_TRANSPOSITION,
	LEFT_ASCENDING,
	RIGHT_DESCENDING,
	MIRROR_IMAGE
};

enum transpositionModes transpositionMode = NO_TRANSPOSITION;
int splitPoint = 62; // default to middle d

// 0 means no threshold; just let through all notes...
// otherwise, this number represents the highest velocity number that we will let through
int velocityThreshhold = 0;
int midiEchoDisabled = 0;

int callback_active = FALSE;

#if defined(USE_NATS)
char *nats_url = DEFAULT_NATS_URL;
natsConnection *conn = NULL;
natsSubscription *sub = NULL;
natsOptions *opts = NULL;
natsStatus NATSstatus;
volatile bool done = false;

// called whenever we get a chord change
static void
onChord(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
	printf("current Chord: %s\n",
		   natsMsg_GetData(msg));

	// Need to destroy the message!
	natsMsg_Destroy(msg);

	// Notify the main thread that we are done.
	*(bool *)(closure) = true;
}

// called whenever we get a MIDI in event
static void
onMIDIin(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure)
{
	printf("MIDI in: %s\n",
		   natsMsg_GetData(msg));

	// Need to destroy the message!
	natsMsg_Destroy(msg);

	// Notify the main thread that we are done.
	*(bool *)(closure) = true;
}

#endif

bool isFirstTime = TRUE;

bool ShouldReloadFile(char *filename);

void DoTick(int accent)
{

	PmEvent buffer;
	int i;
	PmError err;

	static int count = 0;

	if (accent)
	{
		buffer.message = Pm_Message(144, 107, 60);
		err = Pm_Write(midi_out, &buffer, 1);

		buffer.message = Pm_Message(128, 107, 0);
		err = Pm_Write(midi_out, &buffer, 1);
	}
	else
	{
		buffer.message = Pm_Message(144, 50, 60);
		err = Pm_Write(midi_out, &buffer, 1);

		buffer.message = Pm_Message(128, 50, 0);
		err = Pm_Write(midi_out, &buffer, 1);
	}

	if (count < 12)
		count++;
	else
		count = 0;
}

// takes an input node, and maps it according to current transposition mode
PmMessage TransformNote(PmMessage Note)
{

	PmMessage retval = Note;
	int offset;

	switch (transpositionMode)
	{

	case NO_TRANSPOSITION:
		// do nothing, just return original note!
		retval = Note;
		break;

	// make left hand ascend
	case LEFT_ASCENDING:
		if (Note < 62)
		{
			offset = (62 - Note);
			retval = 62 + offset;
		} // else do nothing;
		break;

	// make right hand descend
	case RIGHT_DESCENDING:
		if (Note > 62)
		{
			offset = (Note - 62);
			retval = 62 - offset;
		} // else do nothing;
		break;

	// completely reverse the keyboard
	case MIRROR_IMAGE:
		if (Note == 62)
		{
			// do nothing
		}
		else if (Note < 62)
		{
			offset = (62 - Note);
			retval = 62 + offset;
		}
		else if (Note > 62)
		{
			offset = (Note - 62);
			retval = 62 - offset;
		}
		break;
	}

	return retval;
}

// cycles through the transposition modes in turn
// this routine is called when we detect a LOW A on the piano [which isn't used much, so we can just use it for input like this]
void DoNextTranspositionMode()
{

	switch (transpositionMode)
	{

	case NO_TRANSPOSITION:
		transpositionMode = LEFT_ASCENDING;
		printf("Left hand ascending mode active\n");
		break;

	case LEFT_ASCENDING:
		transpositionMode = RIGHT_DESCENDING;
		printf("Right Hand Descending mode active\n");
		break;

	case RIGHT_DESCENDING:
		transpositionMode = MIRROR_IMAGE;
		printf("Keyboard mirring mode active\n");
		break;

	case MIRROR_IMAGE:
		transpositionMode = NO_TRANSPOSITION;
		printf("no tranposition active\n");
		break;
	}
}

void exit_with_message(char *msg)
{
	char line[STRING_MAX];
	printf("%s\nType ENTER...", msg);
	fgets(line, STRING_MAX, stdin);
	exit(1);
}

// setup to work with digital_piano_1
void process_midi_1(PtTimestamp timestamp, void *userData)
{
	PmError result;
	PmEvent buffer;

	CommandMessage cmd;		 // incoming message from main()
	CommandMessage response; // our responses back to main()

	// if we're not intialized, do nothing
	if (!callback_active)
	{
		return;
	}

	DoMetronome();

	// process messages from the main thread
	do
	{
		result = Pm_Dequeue(main_to_callback, &cmd);
		if (result)
		{
			switch (cmd.cmdCode)
			{
			case CMD_QUIT_MSG:
				response.cmdCode = CMD_MSG_ACK;
				Pm_Enqueue(callback_to_main, &response);
				callback_active = FALSE;
				return;
				// no break needed; above statement just exits function
			case CMD_SET_SPLIT_POINT:
				break;
			case CMD_SET_MODE:
				transpositionMode = (cmd.Param1);
				response.cmdCode = CMD_MSG_ACK;
				Pm_Enqueue(callback_to_main, &response);
				break;
			}
		}
	} while (result);

	// process incoming midi data, performing transposion as necessary
	do
	{
		result = Pm_Poll(midi_in);
		if (result)
		{
			int status, data1, data2;
			if (Pm_Read(midi_in, &buffer, 1) == pmBufferOverflow)
				continue;

			// we have some MIDI data to look at

			status = Pm_MessageStatus(buffer.message);
			data1 = Pm_MessageData1(buffer.message);
			data2 = Pm_MessageData2(buffer.message);

			if (ShowMIDIData)
				printf("input:  %d, %d, %d\n", Pm_MessageStatus(buffer.message), Pm_MessageData1(buffer.message), Pm_MessageData2(buffer.message));

			// do transposition logic
			PmMessage NewNote = TransformNote(data1);

			if (status != 128)
			{
				status = status | MIDIchannel;
			}

			// do logic associated with quite mode
			int shouldEcho = (data2 < velocityThreshhold) || (velocityThreshhold == 0);

			// actually write the midi message [after all our processing] unless
			// local MIDI echo is disabled
			if (!midiEchoDisabled)
			{

				buffer.message =
					Pm_Message(status, NewNote, Pm_MessageData2(buffer.message));

				if (shouldEcho)
					Pm_Write(midi_out, &buffer, 1);
			}

#if defined(USE_NATS)
			// if we are using NATs, and we are configured to echo MIDI over nats,
			// then publish the midi event as a NATs message
			if (natsbroadcast)
			{
				const int[2] * payload;
				payload[0] = status;
				payload[1] = data1;
				payload[2] = data2;

				const char *subj = "midiOUT";

				return natsConnection_Publish(nc, subj, (const void *)payload, 3);
			}
#endif

			if (data1 == 21 && data2 == 0)
			{
				DoNextTranspositionMode();
			}
		}
	} while (result);
}

// setup to work with digital_piano_2
void process_midi_2(PtTimestamp timestamp, void *userData)
{
	PmError result;
	PmEvent buffer;

	CommandMessage cmd;		 // incoming message from main()
	CommandMessage response; // our responses back to main()

	// if we're not intialized, do nothing
	if (!callback_active)
	{
		return;
	}

	DoMetronome();

	// process messages from the main thread
	do
	{
		result = Pm_Dequeue(main_to_callback, &cmd);
		if (result)
		{
			switch (cmd.cmdCode)
			{
			case CMD_QUIT_MSG:
				response.cmdCode = CMD_MSG_ACK;
				Pm_Enqueue(callback_to_main, &response);
				callback_active = FALSE;
				return;
				// no break needed; above statement just exits function
			case CMD_SET_SPLIT_POINT:
				break;
			case CMD_SET_MODE:
				transpositionMode = (cmd.Param1);
				response.cmdCode = CMD_MSG_ACK;
				Pm_Enqueue(callback_to_main, &response);
				break;
			}
		}
	} while (result);

	// process incoming midi data, performing transposion as necessary
	do
	{
		result = Pm_Poll(midi_in);
		if (result)
		{
			int status, data1, data2;
			if (Pm_Read(midi_in, &buffer, 1) == pmBufferOverflow)
				continue;

			// we have some MIDI data to look at

			status = Pm_MessageStatus(buffer.message);
			data1 = Pm_MessageData1(buffer.message);
			data2 = Pm_MessageData2(buffer.message);

			if (ShowMIDIData)
				printf("input:  %d, %d, %d\n", Pm_MessageStatus(buffer.message), Pm_MessageData1(buffer.message), Pm_MessageData2(buffer.message));

			// do transposition logic
			data1 = TransformNote(data1);

			// if (status != 128)
			//{
			status = status + NoteOffset;
			//}

			// if (ShowMIDIData)
			// printf("output:  %d, %d, %d\n", status, data1, data2);

			// do logic associated with quite mode
			int shouldEcho = (data2 < velocityThreshhold) || (velocityThreshhold == 0);

			///////////////////////////////////////////////
			// this code needs debugged!!
			///////////////////////////////////////////////

			if (Lua_State && script_is_loaded)
			{

				// Push the fib function on the top of the lua stack
				lua_getglobal(Lua_State, "process_midi");

				// make sure the .Lua function process_midi is defined
				if (lua_isfunction(Lua_State, -1))
				{

					lua_pushnumber(Lua_State, status);
					lua_pushnumber(Lua_State, data1);
					lua_pushnumber(Lua_State, data2);

					if (lua_pcall(Lua_State, 3, 3, 0) == 0)
					{

						// Get the result from the lua stack
						if ((lua_gettop(Lua_State) == 3 && lua_isnumber(Lua_State, -3) && lua_isnumber(Lua_State, -2) && lua_isnumber(Lua_State, -1)))
						{
							status = (int)lua_tointeger(Lua_State, -3);
							data1 = (int)lua_tointeger(Lua_State, -2);
							data2 = (int)lua_tointeger(Lua_State, -1);
						}
						else
							printf("function 'process_midi' must return 3 numbers\n");

						// Clean up.  If we don't do this last step, we'll leak stack memory.
						lua_settop(Lua_State, 0); // discard anything returned, since we don't really know how many items were returned for sure
												  // lua_pop(Lua_State, 3);
					}
					else
					{
						printf("error running function `process_midi': %s\n", lua_tostring(Lua_State, -1));
					}
				}
				else
					printf("no process_midi function defined in loaded .Lua script\n");
			}

			// actually write the midi message [after all our processing] unless
			// local MIDI echo is disabled
			if (!midiEchoDisabled)
			{

				buffer.message =
					Pm_Message(status, data1, data2);

				if (shouldEcho)
					Pm_Write(midi_out, &buffer, 1);
			}

#if defined(USE_NATS)
			// if we are using NATs, and we are configured to echo MIDI over nats,
			// then publish the midi event as a NATs message
			if (natsbroadcast)
			{
				const int[2] * payload;
				payload[0] = status;
				payload[1] = data1;
				payload[2] = data2;

				const char *subj = "midiOUT";

				return natsConnection_Publish(nc, subj, (const void *)payload, 3);
			}
#endif

			if (data1 == 21 && data2 == 0)
			{
				DoNextTranspositionMode();
			}
		}
	} while (result);
}

void initialize()
{
	const PmDeviceInfo *info;
	int id;

	/* make the message queues */
	main_to_callback = Pm_QueueCreate(IN_QUEUE_SIZE, sizeof(CommandMessage));
	assert(main_to_callback != NULL);
	callback_to_main = Pm_QueueCreate(OUT_QUEUE_SIZE, sizeof(CommandMessage));
	assert(callback_to_main != NULL);

	if (false)
		Pt_Start(1, &process_midi_1, 0);
	else
		Pt_Start(1, &process_midi_2, 0);

	Pm_Initialize();

	// open default output device, if nothing was specified on the command line
	if (MIDIOutputDevice == -1)
		id = Pm_GetDefaultOutputDeviceID();
	else
		id = MIDIOutputDevice;

	info = Pm_GetDeviceInfo(id);
	if (info == NULL)
	{
		printf("Could not open default output device (%d).", id);
		exit_with_message("");
	}
	printf("Opening output device %d %s %s\n", id, info->interf, info->name);

	Pm_OpenOutput(&midi_out,
				  id,
				  NULL,
				  OUT_QUEUE_SIZE,
				  NULL,
				  NULL,
				  0);

	// open default midi input device, if nothing was specified on the command line
	if (MIDIInputDevice == -1)
		id = Pm_GetDefaultInputDeviceID();
	else
		id = MIDIInputDevice;

	info = Pm_GetDeviceInfo(id);
	if (info == NULL)
	{
		printf("Could not open default input device (%d).", id);
		exit_with_message("");
	}
	printf("Opening input device %d %s %s\n", id, info->interf, info->name);
	Pm_OpenInput(&midi_in,
				 id,
				 NULL,
				 0,
				 NULL,
				 NULL);

	Pm_SetFilter(midi_in, PM_FILT_ACTIVE | PM_FILT_CLOCK);

	printf("Using MIDI echo back channel %d\n", MIDIchannel);

	callback_active = TRUE;

#if defined(USE_NATS)

	if (natsbroadcast || natsreceive)
	{

		if (natsOptions_Create(&opts) != NATS_OK)
			NATSstatus = NATS_NO_MEMORY;

		if (NATSstatus == NATS_OK)
		{
			// initialize NATs
			natsOptions_SetSendAsap(opts, true);
			NATSstatus = natsConnection_ConnectTo(&conn, nats_url);
		}

		if (NATSstatus == NATS_OK)
		{
			// subscribe to chord change events...
			// for use in benevolent mode
			NATSstatus = natsConnection_Subscribe(&sub, conn, "chord", onMsg, (void *)&done);
		}

		if (NATSstatus == NATS_OK && natsreceive)
		{
			// subscribe to midiIN events
			// (these are sent from external processing scripts, who are listening for our midiOUT events,
			// after they do their processing)
			NATSstatus = natsConnection_Subscribe(&sub, conn, "midiIN", onMsg, (void *)&done);
			NATSstatus
		}
		// If there was an error, print a stack trace and exit
		if (NATSstatus != NATS_OK)
		{
			nats_PrintLastErrorStack(stderr);
			exit(2);
		}
	}

#endif
}

void shutdown()
{
	// shutting everything down; just ignore all errors; nothing we can do anyway...

	KillMetronome();

	// close down our lua interpreter
	if (Lua_State)
	{
		lua_close(Lua_State);
	}

	Pt_Stop();
	Pm_QueueDestroy(callback_to_main);
	Pm_QueueDestroy(main_to_callback);

	Pm_Close(midi_in);
	Pm_Close(midi_out);

	Pm_Terminate();

#if defined(USE_NATS)

	// shutdown NATs
	natsSubscription_Destroy(sub);
	natsConnection_Destroy(conn);
	natsOptions_Destroy(opts);

#endif
}

// send a quit message to the callback function
// then wait around until it sends us an ACK back
void signalExitToCallBack()
{

	int gotFinalAck;

	CommandMessage msg;
	CommandMessage response;

	// send a quit message to the callback
	msg.cmdCode = CMD_QUIT_MSG;
	Pm_Enqueue(main_to_callback, &msg);

	// wait for the callback to send back acknowledgement
	gotFinalAck = FALSE;
	do
	{
		if (Pm_Dequeue(callback_to_main, &response) == 1)
		{
			if (response.cmdCode == CMD_MSG_ACK)
			{
				int i = 1;
				gotFinalAck = TRUE;
			}
		}
	} while (!gotFinalAck);
}

// send a quit message to the callback function
// then wait around until it sends us an ACK back
void set_transposition_mode(enum transpositionModes newmode)
{
	int receivedAck;

	CommandMessage msg;
	CommandMessage response;

	msg.cmdCode = CMD_SET_MODE;
	msg.Param1 = newmode;
	Pm_Enqueue(main_to_callback, &msg);

	// wait for the callback to send back acknowledgement
	receivedAck = FALSE;
	do
	{
		if (Pm_Dequeue(callback_to_main, &response) == 1)
		{
			if (response.cmdCode == CMD_MSG_ACK)
			{
				receivedAck = TRUE;
			}
		}
	} while (!receivedAck);
}

void list_midi_devices()
{
	int num_devs = Pm_CountDevices();
	const PmDeviceInfo *pmInfo;

	printf("\n");

	if (num_devs == 0)
	{
		printf("No MIDI ports were found\n");
		exit(1);
	}

	printf("MIDI input ports:\n");
	for (int i = 0; i < num_devs; i++)
	{
		pmInfo = Pm_GetDeviceInfo(i);

		if (pmInfo->input)
		{
			printf("%d, %s %s\n", i, pmInfo->name, (i == Pm_GetDefaultInputDeviceID()) ? "(default)" : "");
		}
	}

	printf("\nMIDI output ports:\n");
	for (int i = 0; i < num_devs; i++)
	{
		pmInfo = Pm_GetDeviceInfo(i);

		if (pmInfo->output)
		{
			printf("%d - %s %s\n", i, pmInfo->name, (i == Pm_GetDefaultOutputDeviceID()) ? "(default)" : "");
		}
	}
	printf("\n");
	exit(0);
}

void parseCmdLine(int argc, char **argv)
{
	int printHelp;

	for (int i = 1; i < argc; i++)
	{
		if (argv[i][0] == '-')
		{
			if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--h") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0)
			{
				printf(
					"Kundalini Piano Mirror\n"
					"Usage: pianomirror [OPTIONS]\n"
					"   -h,  --help                 Displays this information.\n"
					"   -d,  --debug                Print incoming MIDI messages.\n"
					"   -i,  --input <0-9>          Specify MIDI input device number\n"
					"   -o,  --output <0-9>         Specify MIDI output device number\n"
					"   -c,  --channel <0-9>        Specify MIDI (echo back) channel number\n"
					"   -e,  --noecho               disable local midi echo"
					"   -v,  --version              Displays version information\n"
					"   -l,  --list                 List available MIDI devices\n"
#ifdef USE_NATS
					"   -n,  --nats <url>           Specify NATS URL, default =  " DEFAULT_NATS_URL "\n"
					"   -nb, --natsbroadcast        broadcast incoming MIDI messages via NATs\n"
					"   -nr, --natsreceive          don't echo MIDI; only send MIDI on NATs receive\n"

#endif
					"\n"
					"Source code at: https://github.com/BenjaminPritchard/KundaliniPianoMirrorLinux\n"
					"\n");
				exit(0);
			}

			else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0)
			{
				if (i + 1 < argc)
				{
					MIDIInputDevice = atof(argv[i + 1]);
					if (MIDIInputDevice < 0 || MIDIInputDevice > 9)
					{
						fprintf(stderr, "Error: value must be between 0 and 9.\n");
						exit(1);
					}
				}
				else
				{
					fprintf(stderr, "Error: -i needs a value\n");
					exit(1);
				}
			}
			else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0)
			{
				if (i + 1 < argc)
				{
					MIDIOutputDevice = atof(argv[i + 1]);
					if (MIDIOutputDevice < 0 || MIDIOutputDevice > 9)
					{
						fprintf(stderr, "Error: value must be between 0 and 9.\n");
						exit(1);
					}
				}
				else
				{
					fprintf(stderr, "Error: -o needs a value\n");
					exit(1);
				}
			}
			else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0)
			{
				list_midi_devices();
				exit(1);
			}
			else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--channel") == 0)
			{
				if (i + 1 < argc)
				{
					MIDIchannel = atof(argv[i + 1]);
					if (MIDIchannel < 0 || MIDIchannel > 16)
					{
						fprintf(stderr, "Error: value must be between 0 and 16.\n");
						exit(1);
					}
				}
				else
				{
					fprintf(stderr, "Error: -c needs a value\n");
					exit(1);
				}
			}
			else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
			{
				printf("pianomirror version %s\n", VersionString);
				exit(0);
			}
			else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--noecho") == 0)
			{
				midiEchoDisabled = FALSE;
				printf("local midi echo disabled");
			}

			else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0)
			{
				ShowMIDIData = TRUE;
			}
#ifdef USE_NATS
			else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--nats_url") == 0)
			{
				if (i + 1 < argc)
				{
					nats_url = strdup(argv[i + 1]);
				}
				else
				{
					fprintf(stderr, "Error: --nats_url needs a value\n");
					exit(1);
				}
			}
			else if (strcmp(argv[i], "-nb") == 0 || strcmp(argv[i], "--natsbroadcast") == 0)
			{
				natsbroadcast = TRUE;
			}
			else if (strcmp(argv[i], "-nr") == 0 || strcmp(argv[i], "--natsreceive") == 0)
			{
				natsreceive = TRUE;
			}
#endif
			else
			{
				fprintf(stderr, "Error: unknown option %s\n", argv[i]);
				exit(1);
			}
		}
	}
}

void CheckVoices()
{
}

void ShowCommands()
{
	printf("commands:\n");
	printf(" 0 [enter] for no transposing \n");
	printf(" 1 [enter] for left ascending mode\n");
	printf(" 2 [enter] for right hand descending mode \n");
	printf(" 3 [enter] for mirror image mode\n");
	printf(" 4 [enter] for quiet mode\n");
	printf(" 5 [enter] cycle to next mode\n");
	printf(" 6 [enter] to toggle debug display of incoming MIDI messages\n");
	printf(" 7 [enter] set metronome BMP\n");
	printf(" 8 [enter] set time signature\n");
	printf(" 9 [enter] enable\\disable metronome\n");
	printf("10 [enter] set note offset\n");
	printf("11 [enter] to load lua script\n");
	printf("12 [enter] clear lua state\n");
	printf("13 [enter] reload last script\n");
	printf(" q [enter] to quit\n");
}

/*
 * Check if a file exist using fopen() function
 * return 1 if the file exist otherwise return 0
 */
int fileexists(const char *filename)
{
	/* try to open file to read */
	FILE *file;
	if (file = fopen(filename, "r"))
	{
		fclose(file);
		return 1;
	}
	return 0;
}

void LoadLuaScript()
{

	char tmp[255];
	char ext[] = ".lua";

	if (Lua_State)
	{
		lua_close(Lua_State);
	}

	// each time we call this function, we create a new environment
	// this is so that we can have a script loaded... then change it, and reload our changes
	Lua_State = luaL_newstate();
	luaL_openlibs(Lua_State);

	printf("Enter lua script: ");

	if (scanf("%s", tmp) == 1)
	{

		strcpy(script_file, SCRIPT_LOCATION);
		strcat(script_file, tmp);

		// tack on the extension if none is present
		if (!strchr(script_file, '.'))
			strcat(script_file, ext);

		if (fileexists(script_file))
		{
			script_is_loaded = (luaL_dofile(Lua_State, script_file) == 0);
			if (!script_is_loaded)
			{
				printf("%s\n", lua_tostring(Lua_State, -1));
			}
		}
		else
			printf("lua script not found: %s\n", script_file);
	}
}

// resets the LUA state, and reloads [restarts] the last script we had loaded
void ReLoadLuaScript()
{

	if (Lua_State)
	{
		lua_close(Lua_State);
	}

	// each time we call this function, we create a new environment
	// this is so that we can have a script loaded... then change it, and reload our changes
	Lua_State = luaL_newstate();
	luaL_openlibs(Lua_State);

	if (fileexists(script_file))
	{
		script_is_loaded = (luaL_dofile(Lua_State, script_file) == 0);
		if (!script_is_loaded)
		{
			printf("error in .Lua script: %s\n", lua_tostring(Lua_State, -1));
		}
	}
	else
		printf("error loading lau script %s\n", script_file);
}

void *CheckOnFile(void *arg)
{
	while (1)
	{
		if (script_is_loaded)
			if (ShouldReloadFile(script_file))
			{
				printf("script modified...\n");
				ReLoadLuaScript();
			}

		sleep(5);
	}
}

void *MainThread(void *arg)
{
	int len;
	char line[STRING_MAX];
	bool finished;

	ShowCommands();

	finished = FALSE;
	while (!finished)
	{

		line[0] = 0;
		int x = scanf("%s", line);
		printf("%d\n", x);

		if (strcmp(line, "q") == 0)
		{
			signalExitToCallBack();
			finished = TRUE;
		}

		if (strcmp(line, "0") == 0)
		{
			set_transposition_mode(NO_TRANSPOSITION);
			printf("no tranposition active\n");
		}

		if (strcmp(line, "1") == 0)
		{
			set_transposition_mode(LEFT_ASCENDING);
			printf("Left hand ascending mode active\n");
		}

		if (strcmp(line, "2") == 0)
		{
			set_transposition_mode(RIGHT_DESCENDING);
			printf("Right Hand Descending mode active\n");
		}

		if (strcmp(line, "3") == 0)
		{
			set_transposition_mode(MIRROR_IMAGE);
			printf("Keyboard mirring mode active\n");
		}

		if (strcmp(line, "4") == 0)
		{
			printf("Enter volocity threshold, or 0 to disable quiet mode: ");
			int n;
			if (scanf("%d", &n) == 1)
			{
				velocityThreshhold = n;
				if (n == 0)
					printf("quiet mode turned off\n");
				else
					printf("threshold set to %d\n", n);
			}
		}

		if (strcmp(line, "5") == 0)
		{
			DoNextTranspositionMode();
		}

		if (strcmp(line, "6") == 0)
		{
			ShowMIDIData = !ShowMIDIData;
		}

		if (strcmp(line, "7") == 0)
		{
			printf("Enter bpm: ");
			int n;
			if (scanf("%d", &n) == 1)
			{
				setBeatsPerMinute(n); // this will reinitialize the timer for the metronome to the right frequency for us...
				printf("bmp set to %d\n", n);
			}
		}

		if (strcmp(line, "8") == 0)
		{

			printf("\nTime Signatures:\n"
				   " 0 [enter] none (just click on each beat) \n"
				   " 1 [enter] 2/4 \n"
				   " 2 [enter] 3/4 \n"
				   " 3 [enter] 4/4 \n"
				   " 4 [enter] 5/4 \n"
				   " 5 [enter] 6/8 \n");

			int timeSig;
			if (scanf("%d", &timeSig) == 1)
			{

				switch (timeSig)
				{
				case 0:
					printf("none\n");
					setBeatsPerMeasure(0);
					break;
				case 1:
					printf("2/4\n");
					setBeatsPerMeasure(2);
					break;
				case 2:
					printf("3/4\n");
					setBeatsPerMeasure(3);
					break;
				case 3:
					printf("4/4\n");
					setBeatsPerMeasure(4);
					break;
				case 4:
					printf("5/4\n");
					setBeatsPerMeasure(5);
					break;
				case 5:
					printf("6/8\n");
					setBeatsPerMeasure(6);
					break;
				}
			}
		}

		if (strcmp(line, "9") == 0)
		{
			if (metronome_enabled)
			{
				DisableMetronome();
				printf("metronome disabled\n");
			}
			else
			{
				EnableMetronome();
				printf("metronome enabled\n");
			}
		}

		if (strcmp(line, "10") == 0)
		{
			printf("Enter offset: ");
			int n;
			if (scanf("%d", &n) == 1)
			{
				NoteOffset = n;
				printf("noteoffset set to %d\n", n);
			}
		}

		if (strcmp(line, "11") == 0)
		{
			LoadLuaScript();
		}

		if (strcmp(line, "12") == 0)
		{
			if (Lua_State)
			{
				lua_close(Lua_State);
				Lua_State = 0;
			}
		}

		if (strcmp(line, "13") == 0)
		{
			ReLoadLuaScript();
			isFirstTime = true;
		}

		ShowCommands();
	} // while (!finished)
}

void SetUpInitialVoices()
{
	PmEvent buffer;
	int i = 0;
	PmError err;

	uint8_t program_change[2] = {0xC0, 10};

	buffer.message = Pm_Message(194, 6, 0);
	err = Pm_Write(midi_out, &buffer, 1);

	return;

	while (true)
	{
		printf("%d\n", i);

		buffer.message = Pm_Message(192, 19, 0);
		err = Pm_Write(midi_out, &buffer, 1);

		buffer.message = Pm_Message(144, 90, 50);
		err = Pm_Write(midi_out, &buffer, 1);

		buffer.message = Pm_Message(129, 90, 50);
		err = Pm_Write(midi_out, &buffer, 1);

		sleep(1);
		i++;
	}

	buffer.message = Pm_Message(193, 12, 0);
	err = Pm_Write(midi_out, &buffer, 1);

	buffer.message = Pm_Message(194, 13, 0);
	err = Pm_Write(midi_out, &buffer, 1);

	buffer.message = Pm_Message(195, 14, 0);
	err = Pm_Write(midi_out, &buffer, 1);
}

// returns TRUE if the loaded script has been modified, so that we can reload it
bool ShouldReloadFile(char *filename)
{

	static time_t old_mtime;
	struct stat file_stat;

	// Retrieve the file times for the file.
	if (stat(filename, &file_stat) < 0)
	{
		return FALSE;
	}

	// if this is the first time we are checking, then we definitely shouldn't reload the file
	if (isFirstTime)
	{
		isFirstTime = FALSE;
		old_mtime = file_stat.st_mtime;
		return FALSE;
	}

	int retval = (old_mtime != file_stat.st_mtime);

	old_mtime = file_stat.st_mtime;

	return retval;
}

int main(int argc, char *argv[])
{

	parseCmdLine(argc, argv);

	printf("%s\n", logo_txt);

	printf("Kundalini Piano Mirror version %s, written by Benjamin Pritchard\n", VersionString);
	printf("NOTE: Make sure to turn off local echo mode on your digital piano!!\n");

#ifdef USE_NATS
	if (natsbroadcast || natsreceive)
	{
		printf("using NATs url: %s\n", nats_url);
	}
	if (natsbroadcast)
	{
		printf("NATs broadcast enabled\n");
	}
	if (natsreceive)
	{
		printf("NATs receive enabled\n");
	}
#endif

	initialize();

	printf("no tranposition active\n");

	bpm = 60;
	setBeatsPerMeasure(4);

	SetUpInitialVoices();

	pthread_t thread_id1;
	pthread_t thread_id2;
	int err1 = pthread_create(&thread_id1, NULL, MainThread, NULL);
	int err2 = pthread_create(&thread_id2, NULL, CheckOnFile, NULL);
	pthread_join(thread_id1, NULL); // wait for the main thread to exit

	shutdown();
	return 0;
}
