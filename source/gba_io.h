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

#ifndef GBA_IO_H
#define GBA_IO_H

#include "types.h"

// docs/PLAN.md §4.3: single dispatch point for GBA I/O-register writes
// (0x04000000-0x040003FF, MMU.cpp's _MMU_ARM7GBA_write08/16/32), routing
// each register range to the peripheral module that owns it -- gba_ppu.cpp
// (video), gba_timers.cpp (TM0CNT-TM3CNT), gba_keypad.cpp (KEYCNT) -- and
// falling through to gba_ppu.cpp's plain backing-buffer write for every
// other register, same as before any peripheral needed register-specific
// write behavior.
void gbaIoWrite8(u32 offset, u8 val);
void gbaIoWrite16(u32 offset, u16 val);
void gbaIoWrite32(u32 offset, u32 val);

// docs/PLAN.md §4.3 item 4: WAITCNT (0x04000204)-derived cartridge bus
// timing. GBATEK's own First-Access/Second-Access terminology is kept
// verbatim in the field names below (== non-sequential/sequential cycle
// cost): WS0/WS1/WS2 are the three 32MB cartridge ROM mirror regions
// (0x08-0x09/0x0A-0x0B/0x0C-0x0D), each independently configurable even
// though all three actually address the same physical ROM -- a game
// typically picks one mirror and one WAITCNT config and sticks with it,
// but nothing stops mixing them, so all three are modeled. SRAM
// (0x0E-0x0F) has no second-access/sequential state on real hardware --
// every SRAM bus cycle is a first access.
struct GbaCartTiming
{
	u8 romN[3]; // WS0,WS1,WS2 non-sequential (first-access) cycles
	u8 romS[3]; // WS0,WS1,WS2 sequential (second-access) cycles
	u8 sram;    // SRAM/flash access cycles (always non-sequential)
};
extern GbaCartTiming gbaCartTiming;

// Recomputes gbaCartTiming from a raw WAITCNT value; call whenever
// WAITCNT (I/O offset 0x204) is written. Also resets code/data sequential-
// access tracking (a WAITCNT rewrite invalidates any in-flight notion of
// "the next access follows the last one" the same way a real bus
// reconfiguration would).
void gbaWaitcntRecompute(u16 waitcnt);

// Resets gbaCartTiming to real hardware's post-reset WAITCNT=0x0000
// state. Called from MMU_Reset alongside the rest of the GBA I/O reset.
void gbaWaitcntReset();

// Cycle cost of one cartridge-ROM bus access. `region` is 0/1/2 for
// WS0/WS1/WS2 (caller derives this from the address, not from GBADecodedAddr,
// since gbaDecodeAddr() deliberately stays a pure function -- see MMU.cpp's
// GBADecodedAddr enum comment). `bits` is 8, 16, or 32: an 8/16-bit access
// costs one N or S cycle per GBATEK; a 32-bit access is executed as two
// back-to-back 16-bit bus transfers (GBATEK-documented), non-sequential
// N+S if the access itself isn't sequential, or S+S if it is.
u32 gbaCartRomAccessCycles(int region, int bits, bool sequential);

// Cycle cost of one cartridge-SRAM bus access (always non-sequential,
// always the WAITCNT SRAM-wait-control cost regardless of access width --
// SRAM itself is only ever accessed 8 bits at a time on real hardware).
u32 gbaSramAccessCycles();

#endif
