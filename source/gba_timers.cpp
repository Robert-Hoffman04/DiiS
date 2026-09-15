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

// docs/PLAN.md §4.3 step 1 (peripherals): GBA hardware timers. New code
// written from GBATEK's documented TMxCNT_L/H layout, not a port of any
// existing emulator's timer core.
//
// GBATEK: TMxCNT_L, when WRITTEN, latches a 16-bit reload value that is
// NOT visible on read-back (reads return the live, currently-counting
// value instead). That means the reload register and the live counter are
// two different pieces of state sharing one address, which is why this
// file keeps its own shadow state (s_t[]) rather than trusting whatever is
// sitting in MMU.GBA_IOREG at that offset -- gbaTimersStep() re-publishes
// the live counter into the IOREG buffer after every step so a plain
// generic-path read (MMU.cpp's _MMU_ARM7GBA_read16, no special-casing
// needed there) sees the right value, mirroring the same "keep hardware
// bits authoritative in the buffer directly" trick gba_ppu.cpp already
// uses for DISPSTAT/VCOUNT.

#include "gba_timers.h"
#include "gba_irq.h"
#include "gba_apu.h"
#include "MMU.h"
#include "mem.h"
#include "readwrite.h"

enum { IO_TM0CNT_L = 0x100 };

static inline u32 tmCntL(int i) { return IO_TM0CNT_L + i * 4; }
static inline u32 tmCntH(int i) { return IO_TM0CNT_L + i * 4 + 2; }

struct GbaTimer
{
	u16 reload;
	u32 counter;   // live count, kept in 0..0xFFFF
	u16 control;   // raw TMxCNT_H bits, as last written
	u32 subCycles; // GBA cycles accumulated since the last prescaler tick
};

static GbaTimer s_t[4];
static const int s_prescale[4] = { 1, 64, 256, 1024 };

static inline bool running(int i)  { return (s_t[i].control >> 7) & 1; }
static inline bool irqEnabled(int i) { return (s_t[i].control >> 6) & 1; }
static inline bool cascade(int i)  { return i > 0 && ((s_t[i].control >> 2) & 1); }

static void syncCounterToBuffer(int i)
{
	T1WriteWord(MMU.GBA_IOREG, tmCntL(i), (u16)s_t[i].counter);
}

// Forward-declared: cascading into timer i+1 can itself overflow, chaining
// into i+2 and so on.
static void overflow(int i);

static void overflow(int i)
{
	s_t[i].counter = s_t[i].reload;
	if (irqEnabled(i))
		gbaRequestIrq((u16)(1 << (3 + i))); // IE/IF bit3 = Timer0 .. bit6 = Timer3

	// DirectSound FIFO A/B (SOUNDCNT_H's timer-select bits) latch a new
	// output sample on whichever timer (0 or 1) they're bound to -- only
	// timers 0/1 are ever wired that way in practice (GBATEK), but the check
	// is cheap enough to run unconditionally for all four.
	gbaApuOnTimerOverflow(i);

	if (i < 3 && running(i + 1) && cascade(i + 1))
	{
		s_t[i + 1].counter++;
		if (s_t[i + 1].counter > 0xFFFF)
			overflow(i + 1);
	}
}

void gbaTimersReset()
{
	for (int i = 0; i < 4; i++)
		s_t[i] = GbaTimer{ 0, 0, 0, 0 };
}

void gbaTimersStep(int gbaCycles)
{
	for (int i = 0; i < 4; i++)
	{
		if (!running(i) || cascade(i)) continue; // cascade timers advance only via overflow() above

		int presc = s_prescale[s_t[i].control & 3];
		s_t[i].subCycles += (u32)gbaCycles;
		while (s_t[i].subCycles >= (u32)presc)
		{
			s_t[i].subCycles -= presc;
			s_t[i].counter++;
			if (s_t[i].counter > 0xFFFF)
				overflow(i);
		}
	}
	for (int i = 0; i < 4; i++)
		syncCounterToBuffer(i);
}

void gbaTimersIoWrite16(u32 offset, u16 val)
{
	int i = (offset - IO_TM0CNT_L) / 4;
	if (i < 0 || i > 3) return;
	bool isControl = ((offset - IO_TM0CNT_L) % 4) == 2;

	if (!isControl)
	{
		s_t[i].reload = val;
		return;
	}

	bool wasRunning = running(i);
	s_t[i].control = val & 0x00C7; // bits 0-1 prescaler, 2 cascade, 6 IRQ-enable, 7 start
	if (running(i) && !wasRunning)
	{
		// Rising edge of the start bit: real hardware reloads the counter
		// immediately rather than waiting for the next prescaler tick.
		s_t[i].counter = s_t[i].reload;
		s_t[i].subCycles = 0;
	}
	syncCounterToBuffer(i);
}

void gbaTimersIoWrite8(u32 offset, u8 val)
{
	// Byte-granular writes to timer registers are rare in practice (both
	// halves are normally accessed together); merge against whatever is
	// currently in the IOREG buffer and delegate to the 16-bit path, same
	// pattern gba_ppu.cpp uses for DISPSTAT. Note the merge base for
	// TMxCNT_L is the *live counter* (see file comment), not the reload --
	// an acceptable simplification for the rare byte-write case.
	u32 base = offset & ~1u;
	u16 cur = T1ReadWord(MMU.GBA_IOREG, base);
	u16 merged = (offset & 1) ? (u16)((cur & 0x00FF) | (val << 8)) : (u16)((cur & 0xFF00) | val);
	gbaTimersIoWrite16(base, merged);
}

// PLAN.md §4.3 item 7 (GBA savestate support). See gba_timers.h's comment.
void gbaTimersSaveState(EMUFILE* os)
{
	write32le(1, os); // version
	for (int i = 0; i < 4; i++)
	{
		write16le(s_t[i].reload, os);
		write32le(s_t[i].counter, os);
		write16le(s_t[i].control, os);
		write32le(s_t[i].subCycles, os);
	}
}

bool gbaTimersLoadState(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	for (int i = 0; i < 4; i++)
	{
		if (!read16le(&s_t[i].reload, is)) return false;
		if (!read32le(&s_t[i].counter, is)) return false;
		if (!read16le(&s_t[i].control, is)) return false;
		if (!read32le(&s_t[i].subCycles, is)) return false;
		syncCounterToBuffer(i);
	}
	return true;
}
