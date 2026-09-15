/*  Copyright (C) 2012 DeSmuMEWii team

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

#ifndef GBA_APU_H
#define GBA_APU_H

#include "types.h"

class EMUFILE;

// docs/PLAN.md §4.3: GBA APU. DirectSound (FIFO A/B, SOUNDCNT_H's DMA Sound
// A/B) is fully wired -- the two DMA-fed 8-bit PCM channels almost every
// commercial GBA game actually uses for music/SFX (a tracker-style engine
// bitstreams pre-mixed audio through these; the four legacy PSG channels
// below are typically only used for a handful of simple effects, if at
// all). The four legacy tone/wave/noise channels (SOUND1..4CNT_*) are
// register-storage only -- writes are accepted and read back correctly so
// game code that pokes them doesn't misbehave or hang waiting on a status
// bit, but they produce no sound. This is a documented gap, not a silent
// wrong-behavior approximation (same spirit as gba_dma.h's Special-timing
// note).
//
// Output timing: each DirectSound channel's current sample only changes
// when its selected timer overflows (gbaApuOnTimerOverflow, called from
// gba_timers.cpp). SPU.cpp's isGBA branch in SPU_Emulate_user() pulls
// whatever numSamples the host audio backend asks for at that moment and
// fills them all with the *current* latched value (zero-order hold) --
// correct sample values and panning/volume, not sample-accurate
// resampling to the host callback's own rate. Good enough for audible,
// correctly-pitched-on-average output; not a cycle-accurate DAC model.

void gbaApuReset();

void gbaApuIoWrite8(u32 offset, u8 val);
void gbaApuIoWrite16(u32 offset, u16 val);
void gbaApuIoWrite32(u32 offset, u32 val);

// Called from gba_timers.cpp's overflow(timer): pops one byte from
// whichever FIFO(s) this timer drives per SOUNDCNT_H's timer-select bits
// and latches it as that channel's current output sample; once a FIFO
// drops to half empty (<=16 of its 32 bytes), requests a DMA refill via
// gbaDmaOnFifoTrigger (gba_dma.h).
void gbaApuOnTimerOverflow(int timer);

// Called from gba_dma.cpp's Special-timing DMA fire path (gbaDmaOnFifoTrigger)
// to append exactly one word (4 bytes, one per fifoPush call from that loop)
// to FIFO A (which=0) or FIFO B (which=1).
void gbaApuFifoPushByte(int which, u8 val);

// Called from SPU.cpp's SPU_Emulate_user() instead of the DS SPU_MixAudio
// path when gameInfo.isGBA. Fills numSamples interleaved stereo s16 frames.
void gbaApuMixAudio(s16* buf, u32 numSamples);

// PLAN.md §4.3 item 7 (GBA savestate support): the DirectSound FIFO A/B
// circular-buffer contents/head/count and the last-latched output sample
// -- all private module state with no register backing (SOUNDCNT_H etc.
// are plain register storage already covered by the GBA_IOREG memory
// chunk).
void gbaApuSaveState(EMUFILE* os);
bool gbaApuLoadState(EMUFILE* is, int size);

#endif
