/*
 * audio.h - sound effects and music.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include "main.h"

extern BOOL bDisableSound; /* --nosound */
extern int audio_volume;   /* 0-100 percent, applied to everything mixed */


void Audio_Init(void);
void Audio_UnInit(void);
void Audio_EnableAudio(BOOL enable);

/* host calls */
void Call_PlaySFX(void);
void Call_PlayMusic(void);
void Call_StopMusic(void);
void Call_IsMusicPlaying(void);

#endif /* AUDIO_H */
