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
//	Additionally, this code can be compiled with the conditional compilation flag "USE_NATS" which allows
// 	sending/receiving MIDI messages via the NATS messaging system.
//
// Version History in version.txt
//

const char *VersionString = "1.8";

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "portmidi/portmidi.h"
#include "portmidi/pmutil.h"
#include "portmidi/porttime.h"

#include "metronome.h"

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

int ShowMIDIData = 0;

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

void DoTick(int accent)
{

	PmEvent buffer;
	int i;
	PmError err;

	if (accent)
	{
		buffer.message = Pm_Message(144, 107, 60);
		err = Pm_Write(midi_out, &buffer, 1);
		// sleep(1);
		buffer.message = Pm_Message(144, 107, 0);
		err = Pm_Write(midi_out, &buffer, 1);
		//  printf("err = %d\n", err);
		//  printf("output: %d, %d, %d\n", Pm_MessageStatus(buffer.message), Pm_MessageData1(buffer.message), Pm_MessageData2(buffer.message));
	}
	else
	{
		printf(".");
	}
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

// callback function
void process_midi(PtTimestamp timestamp, void *userData)
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
				// echo back out the noteon command on the MIDI channel that the synth is setup to use
				// normally this is 0, but the "Williams Allegro" that I am using here uses channel 2 for some reason??
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

void initialize()
{
	const PmDeviceInfo *info;
	int id;

	/* make the message queues */
	main_to_callback = Pm_QueueCreate(IN_QUEUE_SIZE, sizeof(CommandMessage));
	assert(main_to_callback != NULL);
	callback_to_main = Pm_QueueCreate(OUT_QUEUE_SIZE, sizeof(CommandMessage));
	assert(callback_to_main != NULL);

	Pt_Start(1, &process_midi, 0);
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
					if (MIDIchannel < 0 || MIDIchannel > 9)
					{
						fprintf(stderr, "Error: value must be between 0 and 9.\n");
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

int main(int argc, char *argv[])
{

	int finished;

	int len;
	char line[STRING_MAX];

	parseCmdLine(argc, argv);

	/* determine what type of test to run */
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

	// debugIT();

	printf("no tranposition active\n");

	printf("commands:\n");
	printf(" 0 [enter] for no transposing \n 1 [enter] for left ascending mode \n 2 [enter] for right hand descending mode \n 3 [enter] for mirror image mode\n");
	printf(" 4 [enter] for quiet mode\n");
	printf(" 5 [enter] cycle to next mode\n");
	printf(" 6 [enter] to toggle debug display of incoming MIDI messages\n");
	printf(" 7  [enter] set metronome BMP\n");
	printf(" 8  [enter] set time signature\n");
	printf(" 9 [enter] enable\\disable metronome\n");
	printf(" q [enter] to quit\n");

	finished = FALSE;
	while (!finished)
	{

		fgets(line, STRING_MAX, stdin);
		len = strlen(line);
		if (len > 0)
			line[len - 1] = 0;

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

	} // while (!finished)

	shutdown();
	return 0;
}
