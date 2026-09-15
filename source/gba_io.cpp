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

#include "gba_io.h"
#include "gba_ppu.h"
#include "gba_timers.h"
#include "gba_keypad.h"
#include "gba_dma.h"
#include "gba_apu.h"
#include "MMU.h"
#include "readwrite.h"

enum {
	IO_SOUND_LO = 0x60, IO_SOUND_HI = 0xA7,
	IO_DMA_LO = 0xB0, IO_DMA_HI = 0xDF,
	IO_TM0CNT_L = 0x100, IO_TM3CNT_H = 0x10E,
	IO_KEYCNT = 0x132,
	IO_WAITCNT = 0x204
};

static inline bool inSoundRange(u32 off) { return off >= IO_SOUND_LO && off <= IO_SOUND_HI; }
static inline bool inDmaRange(u32 off)   { return off >= IO_DMA_LO && off <= IO_DMA_HI; }
static inline bool inTimerRange(u32 off) { return off >= IO_TM0CNT_L && off <= IO_TM3CNT_H; }

// docs/PLAN.md §4.3 item 4: WAITCNT-driven cartridge bus timing.
GbaCartTiming gbaCartTiming;

void gbaWaitcntRecompute(u16 waitcnt)
{
	// GBATEK WAITCNT (0x04000204):
	//   0-1   SRAM Wait Control          (0..3 = 4,3,2,8 cycles)
	//   2-3   WS0 First (N) Access       (0..3 = 4,3,2,8 cycles)
	//   4     WS0 Second (S) Access      (0..1 = 2,1 cycles)
	//   5-6   WS1 First (N) Access       (0..3 = 4,3,2,8 cycles)
	//   7     WS1 Second (S) Access      (0..1 = 4,1 cycles)
	//   8-9   WS2 First (N) Access       (0..3 = 4,3,2,8 cycles)
	//   10    WS2 Second (S) Access      (0..1 = 8,1 cycles)
	//   11-15 PHI clock / prefetch / GBA-CGB flag -- not cycle-cost-related,
	//         not modeled (prefetch buffer affects timing on real hardware,
	//         but is a documented, flagged simplification, same class as
	//         this codebase's other GBA peripheral gaps).
	static const u8 firstAccessTable[4] = { 4, 3, 2, 8 };
	static const u8 ws0SecondTable[2]   = { 2, 1 };
	static const u8 ws1SecondTable[2]   = { 4, 1 };
	static const u8 ws2SecondTable[2]   = { 8, 1 };

	gbaCartTiming.sram    = firstAccessTable[waitcnt & 0x3];
	gbaCartTiming.romN[0] = firstAccessTable[(waitcnt >> 2) & 0x3];
	gbaCartTiming.romS[0] = ws0SecondTable[(waitcnt >> 4) & 0x1];
	gbaCartTiming.romN[1] = firstAccessTable[(waitcnt >> 5) & 0x3];
	gbaCartTiming.romS[1] = ws1SecondTable[(waitcnt >> 7) & 0x1];
	gbaCartTiming.romN[2] = firstAccessTable[(waitcnt >> 8) & 0x3];
	gbaCartTiming.romS[2] = ws2SecondTable[(waitcnt >> 10) & 0x1];
}

void gbaWaitcntReset()
{
	gbaWaitcntRecompute(0x0000); // real hardware's post-reset WAITCNT value
}

u32 gbaCartRomAccessCycles(int region, int bits, bool sequential)
{
	if (bits <= 16)
		return sequential ? gbaCartTiming.romS[region] : gbaCartTiming.romN[region];
	// 32-bit cartridge access = two sequential 16-bit bus transfers
	// (GBATEK-documented): if the 32-bit access itself is sequential with
	// the previous bus access, both halves are S cycles; otherwise the
	// first half is N and the second (now following the first) is S.
	return sequential ? (u32)(2 * gbaCartTiming.romS[region])
	                  : (u32)(gbaCartTiming.romN[region] + gbaCartTiming.romS[region]);
}

u32 gbaSramAccessCycles()
{
	return gbaCartTiming.sram;
}

static void gbaWaitcntWrite16(u16 val)
{
	T1WriteWord(MMU.GBA_IOREG, IO_WAITCNT, val);
	gbaWaitcntRecompute(val);
}

void gbaIoWrite8(u32 offset, u8 val)
{
	if (inSoundRange(offset)) { gbaApuIoWrite8(offset, val); return; }
	if (inDmaRange(offset)) { gbaDmaIoWrite8(offset, val); return; }
	if (inTimerRange(offset)) { gbaTimersIoWrite8(offset, val); return; }
	if (offset == IO_KEYCNT || offset == IO_KEYCNT + 1) { gbaKeypadIoWrite8(offset, val); return; }
	if (offset == IO_WAITCNT || offset == IO_WAITCNT + 1)
	{
		u16 cur = T1ReadWord(MMU.GBA_IOREG, IO_WAITCNT);
		u16 merged = (offset == IO_WAITCNT) ? (u16)((cur & 0xFF00) | val) : (u16)((cur & 0x00FF) | (val << 8));
		gbaWaitcntWrite16(merged);
		return;
	}
	gbaPpuIoWrite8(offset, val);
}

void gbaIoWrite16(u32 offset, u16 val)
{
	if (inSoundRange(offset)) { gbaApuIoWrite16(offset, val); return; }
	if (inDmaRange(offset)) { gbaDmaIoWrite16(offset, val); return; }
	if (inTimerRange(offset)) { gbaTimersIoWrite16(offset, val); return; }
	if (offset == IO_KEYCNT) { gbaKeypadIoWrite16(val); return; }
	if (offset == IO_WAITCNT) { gbaWaitcntWrite16(val); return; }
	gbaPpuIoWrite16(offset, val);
}

void gbaIoWrite32(u32 offset, u32 val)
{
	if (inSoundRange(offset)) { gbaApuIoWrite32(offset, val); return; }
	if (inDmaRange(offset)) { gbaDmaIoWrite32(offset, val); return; }
	if (inTimerRange(offset))
	{
		gbaTimersIoWrite16(offset, (u16)val);
		gbaTimersIoWrite16(offset + 2, (u16)(val >> 16));
		return;
	}
	if (offset == IO_KEYCNT) { gbaKeypadIoWrite16((u16)val); return; }
	if (offset == IO_WAITCNT)
	{
		// High half (0x206-0x207) is unused/PHI-clock-adjacent territory
		// real hardware doesn't expose meaningfully here; only the low
		// 16 bits (the actual WAITCNT register) matter.
		gbaWaitcntWrite16((u16)val);
		return;
	}
	gbaPpuIoWrite32(offset, val);
}
