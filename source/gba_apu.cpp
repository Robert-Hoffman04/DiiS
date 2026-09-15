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

// docs/PLAN.md §4.3: GBA APU. New code written from GBATEK's documented
// SOUND1..4CNT_*/SOUNDCNT_L/H/X/SOUNDBIAS/FIFO_A/FIFO_B layout, not a port
// of any existing emulator's APU core. See gba_apu.h for the DirectSound-
// only scope note.

#include "gba_apu.h"
#include "gba_dma.h"
#include "MMU.h"
#include "mem.h"
#include "readwrite.h"

enum
{
	IO_SOUND_LO   = 0x60,
	IO_SOUND_HI   = 0xA7,
	IO_SOUNDCNT_L = 0x80,
	IO_SOUNDCNT_H = 0x82,
	IO_SOUNDCNT_X = 0x84,
	IO_FIFO_A     = 0xA0,
	IO_FIFO_B     = 0xA4,
};

static inline bool inFifoA(u32 off) { return off >= IO_FIFO_A && off <= IO_FIFO_A + 3; }
static inline bool inFifoB(u32 off) { return off >= IO_FIFO_B && off <= IO_FIFO_B + 3; }

// 32-byte circular buffers -- the real hardware's FIFO depth. Index 0 = FIFO
// A (dst 0x040000A0, DMA channel 1), index 1 = FIFO B (dst 0x040000A4, DMA
// channel 2).
static u8  s_fifo[2][32];
static int s_fifoHead[2];
static int s_fifoCount[2];
static s8  s_fifoLatch[2]; // last byte popped -- held as the current output sample until the next pop

void gbaApuReset()
{
	for (int w = 0; w < 2; w++)
	{
		s_fifoHead[w] = 0;
		s_fifoCount[w] = 0;
		s_fifoLatch[w] = 0;
	}
}

static void fifoPush(int which, u8 val)
{
	if (which < 0 || which > 1) return;
	if (s_fifoCount[which] >= 32) return; // full: real hardware also just drops it
	int tail = (s_fifoHead[which] + s_fifoCount[which]) % 32;
	s_fifo[which][tail] = val;
	s_fifoCount[which]++;
}

static bool fifoPop(int which, s8* out)
{
	if (s_fifoCount[which] == 0) return false;
	*out = (s8)s_fifo[which][s_fifoHead[which]];
	s_fifoHead[which] = (s_fifoHead[which] + 1) % 32;
	s_fifoCount[which]--;
	return true;
}

static void fifoReset(int which)
{
	s_fifoHead[which] = 0;
	s_fifoCount[which] = 0;
	s_fifoLatch[which] = 0;
}

void gbaApuFifoPushByte(int which, u8 val)
{
	fifoPush(which, val);
}

static void handleSoundCntHWrite(u16 val)
{
	// Bits 11/15 (FIFO A/B reset) are write-only "do it now" triggers -- GBATEK
	// documents them as always reading back 0, so masking them out of what
	// gets stored is enough; no need to special-case reads separately.
	if (val & 0x0800) fifoReset(0);
	if (val & 0x8000) fifoReset(1);
	T1WriteWord(MMU.GBA_IOREG, IO_SOUNDCNT_H, val & ~0x8800);
}

void gbaApuIoWrite8(u32 offset, u8 val)
{
	if (inFifoA(offset)) { fifoPush(0, val); return; }
	if (inFifoB(offset)) { fifoPush(1, val); return; }
	if (offset == IO_SOUNDCNT_H || offset == IO_SOUNDCNT_H + 1)
	{
		u16 cur = T1ReadWord(MMU.GBA_IOREG, IO_SOUNDCNT_H);
		u16 merged = (offset == IO_SOUNDCNT_H) ? (u16)((cur & 0xFF00) | val) : (u16)((cur & 0x00FF) | (val << 8));
		handleSoundCntHWrite(merged);
		return;
	}
	T1WriteByte(MMU.GBA_IOREG, offset, val); // legacy channel regs, SOUNDCNT_L, SOUNDCNT_X, SOUNDBIAS, wave RAM: plain storage
}

void gbaApuIoWrite16(u32 offset, u16 val)
{
	if (inFifoA(offset)) { fifoPush(0, (u8)val); fifoPush(0, (u8)(val >> 8)); return; }
	if (inFifoB(offset)) { fifoPush(1, (u8)val); fifoPush(1, (u8)(val >> 8)); return; }
	if (offset == IO_SOUNDCNT_H) { handleSoundCntHWrite(val); return; }
	T1WriteWord(MMU.GBA_IOREG, offset, val);
}

void gbaApuIoWrite32(u32 offset, u32 val)
{
	if (inFifoA(offset))
	{
		fifoPush(0, (u8)val); fifoPush(0, (u8)(val >> 8));
		fifoPush(0, (u8)(val >> 16)); fifoPush(0, (u8)(val >> 24));
		return;
	}
	if (inFifoB(offset))
	{
		fifoPush(1, (u8)val); fifoPush(1, (u8)(val >> 8));
		fifoPush(1, (u8)(val >> 16)); fifoPush(1, (u8)(val >> 24));
		return;
	}
	if (offset == IO_SOUNDCNT_L) // combined SOUNDCNT_L (low 16) + SOUNDCNT_H (high 16)
	{
		T1WriteWord(MMU.GBA_IOREG, IO_SOUNDCNT_L, (u16)val);
		handleSoundCntHWrite((u16)(val >> 16));
		return;
	}
	T1WriteLong(MMU.GBA_IOREG, offset, val);
}

void gbaApuOnTimerOverflow(int timer)
{
	u16 cntx = T1ReadWord(MMU.GBA_IOREG, IO_SOUNDCNT_X);
	if (!((cntx >> 7) & 1)) return; // master sound disabled

	u16 cnth = T1ReadWord(MMU.GBA_IOREG, IO_SOUNDCNT_H);
	static const u32 fifoDst[2] = { 0x040000A0u, 0x040000A4u };
	for (int which = 0; which < 2; which++)
	{
		int timerSel = which == 0 ? ((cnth >> 10) & 1) : ((cnth >> 14) & 1);
		if (timerSel != timer) continue;

		s8 sample;
		if (fifoPop(which, &sample))
			s_fifoLatch[which] = sample;
		// else: FIFO underrun -- hold the last latched sample (matches real hardware)

		if (s_fifoCount[which] <= 16) // half empty: request a 4-word (16-byte) DMA refill
			gbaDmaOnFifoTrigger(fifoDst[which]);
	}
}

void gbaApuMixAudio(s16* buf, u32 numSamples)
{
	u16 cntx = T1ReadWord(MMU.GBA_IOREG, IO_SOUNDCNT_X);
	s32 left = 0, right = 0;

	if ((cntx >> 7) & 1)
	{
		u16 cnth = T1ReadWord(MMU.GBA_IOREG, IO_SOUNDCNT_H);
		for (int which = 0; which < 2; which++)
		{
			s32 v = (s32)s_fifoLatch[which] * 256; // s8 [-128,127] -> s16-ish range
			bool volFull = which == 0 ? ((cnth >> 2) & 1) : ((cnth >> 3) & 1);
			if (!volFull) v >>= 1; // 50% volume
			bool enR = which == 0 ? ((cnth >> 8) & 1)  : ((cnth >> 12) & 1);
			bool enL = which == 0 ? ((cnth >> 9) & 1)  : ((cnth >> 13) & 1);
			if (enL) left  += v;
			if (enR) right += v;
		}
	}

	if (left  > 32767) left  = 32767; else if (left  < -32768) left  = -32768;
	if (right > 32767) right = 32767; else if (right < -32768) right = -32768;

	s16 l = (s16)left, r = (s16)right;
	for (u32 i = 0; i < numSamples; i++)
	{
		buf[i * 2]     = l;
		buf[i * 2 + 1] = r;
	}
}

// PLAN.md §4.3 item 7 (GBA savestate support). See gba_apu.h's comment.
void gbaApuSaveState(EMUFILE* os)
{
	write32le(1, os); // version
	for (int w = 0; w < 2; w++)
	{
		os->fwrite((char*)s_fifo[w], sizeof(s_fifo[w]));
		write32le((u32)s_fifoHead[w], os);
		write32le((u32)s_fifoCount[w], os);
		write8le((u8)s_fifoLatch[w], os);
	}
}

bool gbaApuLoadState(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	for (int w = 0; w < 2; w++)
	{
		if ((u32)is->fread((char*)s_fifo[w], sizeof(s_fifo[w])) != sizeof(s_fifo[w])) return false;
		u32 v; u8 b;
		if (!read32le(&v, is)) return false; s_fifoHead[w] = (int)v;
		if (!read32le(&v, is)) return false; s_fifoCount[w] = (int)v;
		if (!read8le(&b, is)) return false; s_fifoLatch[w] = (s8)b;
	}
	return true;
}
