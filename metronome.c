
//
// Metronome.c
//
// Benjamin Pritchard / Kundalini Software
//
// Routines for dealing with a metronome, adopted from code on the internet
// This code uses the code in WAVE.C to actually play 'tick' sound on each beat
//
// Usage:
//	InitMetronome()
//	setBMP(180);
//	setBeatsPerMeasure(4);		// optional
//	EnableMetronome();
//	While (1)
//		DoMetronome
//
//

#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "metronome.h"

void DoTick(int);

void InitMetronome()
{
	bpm = 100;
	timer_flag = 0;
	metronome_enabled = false;
	measure = 0;
	beat = 0;
	beats_per_measure = 0;
}

void KillMetronome()
{
	if (metronome_enabled)
		DisableMetronome();
}

void EnableMetronome()
{
	struct sigaction sa;

	sa.sa_handler = DoTick;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGALRM, &sa, NULL) == -1)
	{
		perror("sigaction");
		exit(1);
	}

	// Set up the initial timer interval
	struct itimerval timer;
	timer.it_interval.tv_sec = 60 / bpm;
	timer.it_interval.tv_usec = (60 % bpm) * 1000000 / bpm;
	timer.it_value = timer.it_interval;
	setitimer(ITIMER_REAL, &timer, NULL);

	metronome_enabled = true;
}

void DisableMetronome()
{
	// how to disable signal??
	metronome_enabled = false;
}

// call this in a tight loop; it increases the beat count, and incremements the current measure, plus plays the .TIC sound
void DoMetronome()
{
	if (metronome_enabled && timer_flag)
	{

		if (beat == 0 || beats_per_measure == 0)
			DoTick(0);
		else
			DoTick(1);

		if (beats_per_measure == 0)
		{
			beat++;
		}
		else if (beat++ == (beats_per_measure - 1))
		{
			beat = 0;
			measure++;
		}

		timer_flag = 0;
	}
}

void setBeatsPerMinute(const int BPM)
{
	DisableMetronome();
	bpm = BPM;
	EnableMetronome();
}

void setBeatsPerMeasure(const int BeatsPerMeasure)
{
	beat = 0;
	measure = 0;
	beats_per_measure = BeatsPerMeasure;
}

// this function is called by timeSetEvent()
// it just sets the timer_flag, which DoMetronome() looks at
void MetronomeTimerProc()
{
	timer_flag = 1;
}
