#pragma once

int bpm;
int metronome_enabled;
int timer_flag;
int beat;
int measure;
int beats_per_measure;

void InitMetronome();
void KillMetronome();
void EnableMetronome();
void DisableMetronome();
void DoMetronome();
void setBeatsPerMinute(const int BPM);
void setBeatsPerMeasure(const int BeatsPerMeasure);
