//
// PianoMirror.c
// https://www.kundalinisoftware.com/piano-mirror/
//
// Benjamin Pritchard / Kundalini Software
//
// This is the ANSI-C style version of this program (that I run on my Raspberri PI.) 
//
// Program to perform MIDI remapping to create a left handed piano using the portmidi libraries. (see webpage above for more information)
// (Adapted from example programs included with portmidi.)
//
// Version History in version.txt
//

const char *VersionString = "1.5";				

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "assert.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "portmidi/portmidi.h"
#include "portmidi/pmutil.h"
#include "portmidi/porttime.h"

// message queues for the main thread to communicate with the call back
PmQueue *callback_to_main;
PmQueue *main_to_callback;

#define STRING_MAX	80

PmStream *midi_in;		// incoming midi data from piano
PmStream *midi_out;		// (transposed) outgoing midi 

#define IN_QUEUE_SIZE	1024		
#define OUT_QUEUE_SIZE	1024

// we write to this file as midi data comes in
// it is a quick-and-dirty way for other processes 
// on the machine to get at the raw MIDI data

const char *IPC_Out = "/tmp/pianomirror.out";

int fd_out;				// file descriptor for output file 
struct flock lock;		// for locking the output file while we are writing to it

// conversely, anything we read from here, we will interpret as a command
const char *IPC_IN 	= "/tmp/pianomirror.in";

// simple structure to pass messages back and forth between the main thread and our callback
// these messages are inserted into 
typedef struct {
	
	int cmdCode;
	int Param1;
	int Param2;

} CommandMessage;

// messages from the main thread
#define CMD_QUIT_MSG		1
#define CMD_SET_SPLIT_POINT	2
#define CMD_SET_MODE		3

// ackknowledgement of received message
#define	CMD_MSG_ACK			1000

// flag indicating 
int callback_exit_flag;

// transposition modes we support
enum transpositionModes {NO_TRANSPOSITION, LEFT_ASCENDING, RIGHT_DESCENDING, MIRROR_IMAGE};

enum transpositionModes transpositionMode = NO_TRANSPOSITION;
int splitPoint = 62;	// default to middle d

// 0 means no threshold; just let through all notes... 
// otherwise, this number represents the highest velocity number that we will let through
int velocityThreshhold = 0;	

int callback_active = FALSE;

// takes an input node, and maps it according to current transposition mode
PmMessage TransformNote(PmMessage Note) {

	PmMessage	retval = Note;
	int			offset;

	switch (transpositionMode) {

	case NO_TRANSPOSITION:
		// do nothing, just return original note!
		retval = Note; 
		break;
		
	// make left hand ascend
	case LEFT_ASCENDING:
		if (Note < 62) {
			offset = (62 - Note);
			retval = 62 + offset;
		} // else do nothing;
		break;
		
	// make right hand descend
	case RIGHT_DESCENDING:
		if (Note > 62) {
			offset = (Note - 62);
			retval = 62 - offset;
		} // else do nothing;
		break;
	
	// completely reverse the keyboard
	case MIRROR_IMAGE:
		if (Note == 62) {
			// do nothing
		}
		else if (Note < 62) {
			offset = (62 - Note);
			retval = 62 + offset;
		}
		else if (Note > 62) {
			offset = (Note - 62);
			retval = 62 - offset;
		}
		break;
	}

	return retval;
}

// cycles through the transposition modes in turn
// this routine is called when we detect a LOW A on the piano [which isn't used much, so we can just use it for input like this]
void DoNextTranspositionMode() {

	switch (transpositionMode) {

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


void write_to_IPC() {
	write(fd_out, "TEST\n", 5); 
}

// callback function 
void process_midi(PtTimestamp timestamp, void *userData)
{
	PmError result;
	PmEvent buffer;

	CommandMessage cmd;					// incoming message from main()
	CommandMessage response;			// our responses back to main()

	// if we're not intialized, do nothing
	if (!callback_active) {
		return;
	}

	// process messages from the main thread
	do {
		result = Pm_Dequeue(main_to_callback, &cmd);
		if (result) {
			switch (cmd.cmdCode) {
				case CMD_QUIT_MSG:
					response.cmdCode = CMD_MSG_ACK;
					Pm_Enqueue(callback_to_main, &response);
					callback_active = FALSE;
					return;
					//no break needed; above statement just exits function
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
	do {
		result = Pm_Poll(midi_in);
		if (result) {
			int status, data1, data2;
			if (Pm_Read(midi_in, &buffer, 1) == pmBufferOverflow)
				continue;
			
			// we have some MIDI data to look at
			
			// for now, just write something to the IPC file whenever incoming data comes in...
			write_to_IPC();

			status = Pm_MessageStatus(buffer.message);
			data1 = Pm_MessageData1(buffer.message);
			data2 = Pm_MessageData2(buffer.message);

			if (FALSE) {
				printf("status = %d, data1 = %d, data2 = %d \n", status, data1, data2);
			}

			// do transposition logic
			PmMessage NewNote = TransformNote(data1);

			// do logic associated with channel number
			int channel = 1;
			//if (data1 < 62) channel = 1; else channel = 7;
			status = status | channel;

			// do logic associated with quite mode
			int shouldEcho = (data2 < velocityThreshhold) || (velocityThreshhold == 0);

			buffer.message =
				Pm_Message(status, NewNote, Pm_MessageData2(buffer.message));

			if (shouldEcho)
				Pm_Write(midi_out, &buffer, 1);

			if (data1 == 21 && data2 == 0) {
				DoNextTranspositionMode();
			}

		}
	} while (result);

}

// just open up a file called /tmp/pianomirror.out which we will
// write to as MIDI bytes come in
void InitIPC()
{	
	if ((fd_out = open(IPC_Out, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)  /* -1 signals an error */
		exit_with_message("open failed...");
}

// not much to do for now
// just close down /tmp/pianomirror.out
void ShutdownIPC() {
	close(fd_out); 
}

void initialize()
{
	const PmDeviceInfo *info;
	int id;
	
	InitIPC();

	/* make the message queues */
	main_to_callback = Pm_QueueCreate(IN_QUEUE_SIZE, sizeof(CommandMessage));
	assert(main_to_callback != NULL);
	callback_to_main = Pm_QueueCreate(OUT_QUEUE_SIZE, sizeof(CommandMessage));
	assert(callback_to_main != NULL);

	Pt_Start(1, &process_midi, 0);
	Pm_Initialize();

	// open default output device
	id = 2; //Pm_GetDefaultOutputDeviceID();
	info = Pm_GetDeviceInfo(id);
	if (info == NULL) {
		printf("Could not open default output device (%d).", id);
		exit_with_message("");
	}
	printf("Opening output device %s %s\n", info->interf, info->name);

	Pm_OpenOutput(&midi_out,
		id,
		NULL,
		OUT_QUEUE_SIZE,
		NULL,
		NULL,
		0);

	// open default midi input device
	id = 3; //Pm_GetDefaultInputDeviceID();
	info = Pm_GetDeviceInfo(id);
	if (info == NULL) {
		printf("Could not open default input device (%d).", id);
		exit_with_message("");
	}
	printf("Opening input device %s %s\n", info->interf, info->name);
	Pm_OpenInput(&midi_in,
		id,
		NULL,
		0,
		NULL,
		NULL);

	Pm_SetFilter(midi_in, PM_FILT_ACTIVE | PM_FILT_CLOCK);
	
	callback_active = TRUE;
}

void shutdown()
{
	ShutdownIPC();
	
	// shutting everything down; just ignore all errors; nothing we can do anyway...

	Pt_Stop();
	Pm_QueueDestroy(callback_to_main);
	Pm_QueueDestroy(main_to_callback);

	Pm_Close(midi_in);
	Pm_Close(midi_out);

	Pm_Terminate();

}

// send a quit message to the callback function
// then wait around until it sends us an ACK back
void signalExitToCallBack() {

	int gotFinalAck;

	CommandMessage msg;
CommandMessage response;

// send a quit message to the callback
msg.cmdCode = CMD_QUIT_MSG;
Pm_Enqueue(main_to_callback, &msg);

// wait for the callback to send back acknowledgement
gotFinalAck = FALSE;
do {
	if (Pm_Dequeue(callback_to_main, &response) == 1) {
		if (response.cmdCode == CMD_MSG_ACK) {
			int i = 1;
			gotFinalAck = TRUE;
		}
	}
} while (!gotFinalAck);

}

// send a quit message to the callback function
// then wait around until it sends us an ACK back
void set_transposition_mode(enum transpositionModes newmode) {

	int receivedAck;

	CommandMessage msg;
	CommandMessage response;

	msg.cmdCode = CMD_SET_MODE;
	msg.Param1 = newmode;
	Pm_Enqueue(main_to_callback, &msg);

	// wait for the callback to send back acknowledgement
	receivedAck = FALSE;
	do {
		if (Pm_Dequeue(callback_to_main, &response) == 1) {
			if (response.cmdCode == CMD_MSG_ACK) {
				receivedAck = TRUE;
			}
		}
	} while (!receivedAck);

}

int main(int argc, char *argv[])
{

	int finished;

	int len;
	char line[STRING_MAX];

	/* determine what type of test to run */
	printf("Kundalini Piano Mirror version %s, written by Benjamin Pritchard\n", VersionString);
	printf("NOTE: Make sure to turn off local echo mode on your digital piano.\n");

	initialize();
	

	printf("no tranposition active\n");

	printf("commands:\n");
	printf(" 0 [enter] for no transposing \n 1 [enter] for left ascending mode \n 2 [enter] for right hand descending mode \n 3 [enter] for mirror image mode\n");
	printf(" 4 [enter] for quiet mode\n");
	printf(" 5 [enter] cycle to next mode");
	printf(" q [enter] to quit\n");

	finished = FALSE;
	while (!finished) {

		fgets(line, STRING_MAX, stdin);
		len = strlen(line);
		if (len > 0) line[len - 1] = 0;

		if (strcmp(line, "q") == 0) {
			signalExitToCallBack();
			finished = TRUE;
		}

		if (strcmp(line, "0") == 0) {
			set_transposition_mode(NO_TRANSPOSITION);
			printf("no tranposition active\n");
		}

		if (strcmp(line, "1") == 0) {
			set_transposition_mode(LEFT_ASCENDING);
			printf("Left hand ascending mode active\n");
		}

		if (strcmp(line, "2") == 0) {
			set_transposition_mode(RIGHT_DESCENDING);
			printf("Right Hand Descending mode active\n");
		}

		if (strcmp(line, "3") == 0) {
			set_transposition_mode(MIRROR_IMAGE);
			printf("Keyboard mirring mode active\n");
		}

		if (strcmp(line, "4") == 0) {
			printf("Enter volocity threshold, or 0 to disable quiet mode: ");
			int n;
			if (scanf("%d", &n) == 1) {
				velocityThreshhold = n;
				if (n == 0) printf("quiet mode turned off\n");
				else printf("threshold set to %d\n", n);
			}
		}
		
		if (strcmp(line, "5") == 0) {
			DoNextTranspositionMode();
		}

	} // while (!finished)

	shutdown();
	return 0;
}
