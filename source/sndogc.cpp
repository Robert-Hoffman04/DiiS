/*  Copyright 2005-2006 Theo Berkau
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <gccore.h>
#include <gctypes.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "types.h"
#include "SPU.h"
#include "sndogc.h"
#include "debug.h"
#include <unistd.h>

#define NEW_SOUND_SYSTEM


#ifdef NEW_SOUND_SYSTEM
#include <asndlib.h>
#endif



int SNDOGCInit(int buffersize);
void SNDOGCDeInit();
void SNDOGCUpdateAudio(s16 *buffer, u32 num_samples);
u32 SNDOGCGetAudioSpace();
void SNDOGCMuteAudio();
void SNDOGCUnMuteAudio();
void SNDOGCSetVolume(int volume);

SoundInterface_struct SNDOGC = {
	SNDCORE_OGC,
	"OGC Sound Interface",
	SNDOGCInit,
	SNDOGCDeInit,
	SNDOGCUpdateAudio,
	SNDOGCGetAudioSpace,
	SNDOGCMuteAudio,
	SNDOGCUnMuteAudio,
	SNDOGCSetVolume
};

static u8 *stereodata16[2] = {NULL, NULL};
static u8 *tmpbuffer = NULL;
static u8 whichab;
static u32 soundpos;
static u32 soundoffset;
static u32 soundbufsize;
static lwpq_t audioqueue = LWP_TQUEUE_NULL;
static lwp_t audiothread = LWP_THREAD_NULL;
static mutex_t audiomutex = LWP_MUTEX_NULL;
static int sndogcvolume = 100;

static void *audio_thread(void*)
{
	u8 *sdata, *soundbuf;
	while(1)
	{
		SPU_Emulate_user();
		usleep(10);
		LWP_MutexLock(audiomutex);

		whichab ^= 1;
		sdata = (u8*)stereodata16[whichab];
		soundbuf = (u8*)tmpbuffer;

		for (u32 i = 0; i < soundbufsize; i++)
		{
			if (soundpos >= soundbufsize)
				soundpos = 0;

			sdata[i] = soundbuf[soundpos];
			soundpos++;
		}

		LWP_MutexUnlock(audiomutex);

		LWP_ThreadSleep(audioqueue);
	}

	return NULL;
}

//////////////////////////////////////////////////////////////////////////////

static void MixAudio()
{

#ifdef NEW_SOUND_SYSTEM
	DCFlushRange(stereodata16[whichab], soundbufsize);
	AUDIO_InitDMA((u32) stereodata16[whichab], soundbufsize);
#else
	AUDIO_StopDMA();
	DCFlushRange(stereodata16[whichab], soundbufsize);
	AUDIO_InitDMA((u32) stereodata16[whichab], soundbufsize);
	AUDIO_StartDMA();
#endif

	LWP_ThreadSignal(audioqueue);
}

//////////////////////////////////////////////////////////////////////////////

int SNDOGCInit(int buffersize)
{
	whichab = 0;

#ifdef NEW_SOUND_SYSTEM
	ASND_Init();
#else
	AUDIO_Init(NULL);
	AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_SetStreamSampleRate(AI_SAMPLERATE_48KHZ);
	AUDIO_SetStreamVolLeft(sndogcvolume);
	AUDIO_SetStreamVolRight(sndogcvolume);
#endif
	

	soundoffset = 0;
	soundbufsize = buffersize;
	soundpos = 0;

	if ((stereodata16[0] = (u8 *)memalign(32, soundbufsize)) == NULL ||
	    (stereodata16[1] = (u8 *)memalign(32, soundbufsize)) == NULL ||
		(tmpbuffer       = (u8 *)malloc(soundbufsize)) == NULL)
		return -1;

	memset(stereodata16[0], 0, soundbufsize);
	memset(stereodata16[1], 0, soundbufsize);
	memset(tmpbuffer,       0, soundbufsize);


#ifdef NEW_SOUND_SYSTEM

	DCFlushRange(stereodata16[0], soundbufsize);
	DCFlushRange(stereodata16[1], soundbufsize);
	AUDIO_InitDMA((u32)stereodata16[whichab],soundbufsize);
	AUDIO_StartDMA();
#endif

	if (audiomutex == LWP_MUTEX_NULL)
		LWP_MutexInit(&audiomutex, false);

	if (audioqueue == LWP_TQUEUE_NULL)
		LWP_InitQueue(&audioqueue);
	
	if (audiothread == LWP_THREAD_NULL)
		LWP_CreateThread(&audiothread, audio_thread, NULL, NULL, 0, 67);

	SNDOGCUnMuteAudio();

	return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SNDOGCDeInit()
{
	SNDOGCMuteAudio();
	AUDIO_StopDMA();

	if (stereodata16[0])
		free(stereodata16[0]);
	if (stereodata16[1])
		free(stereodata16[1]);
	if (tmpbuffer)
		free(tmpbuffer);

	LWP_MutexDestroy(audiomutex);
	audiomutex = LWP_MUTEX_NULL;
}

//////////////////////////////////////////////////////////////////////////////

void SNDOGCUpdateAudio(s16 *buffer, u32 num_samples)
{
	u32 copy1size = 0, copy2size = 0;
	LWP_MutexLock(audiomutex);

	if ((soundbufsize - soundoffset) < (num_samples * sizeof(s16) * 2))
	{
		copy1size = (soundbufsize - soundoffset);
		copy2size = (num_samples * sizeof(s16) * 2) - copy1size;
	}
	else
	{
		copy1size = (num_samples * sizeof(s16) * 2);
		copy2size = 0;
	}

	memcpy((((u8 *)tmpbuffer)+soundoffset), buffer, copy1size);

	if (copy2size)
		memcpy(tmpbuffer, ((u8 *) buffer) + copy1size, copy2size);

	soundoffset += copy1size + copy2size;
	soundoffset %= soundbufsize;

	LWP_MutexUnlock(audiomutex);
}

//////////////////////////////////////////////////////////////////////////////

u32 SNDOGCGetAudioSpace()
{
	u32 freespace=0;

	if (soundoffset > soundpos)
		freespace = soundbufsize - soundoffset + soundpos;
	else
		freespace = soundpos - soundoffset;

	return (freespace / sizeof(s16) / 2);
}

//////////////////////////////////////////////////////////////////////////////

void SNDOGCMuteAudio()
{
	AUDIO_RegisterDMACallback(NULL);
}

//////////////////////////////////////////////////////////////////////////////

void SNDOGCUnMuteAudio()
{
	AUDIO_RegisterDMACallback(MixAudio);
	MixAudio();
}

//////////////////////////////////////////////////////////////////////////////

void SNDOGCSetVolume(int volume)
{
	volume < 0 ? volume = 0 : volume > 255 ? volume = 255 : 0;
	
	sndogcvolume = volume;
	
#ifdef NEW_SOUND_SYSTEM

	// TODO: The closest I found was: 
	// ASND_ChangeVolumeVoice(s32 voice, s32 volume_l, s32 volume_r)

	ASND_ChangeVolumeVoice(0, sndogcvolume, sndogcvolume);
#else

	AUDIO_SetStreamVolLeft(sndogcvolume);
	AUDIO_SetStreamVolRight(sndogcvolume);

#endif
}

//////////////////////////////////////////////////////////////////////////////
