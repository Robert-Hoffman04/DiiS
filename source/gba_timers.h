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

#ifndef GBA_TIMERS_H
#define GBA_TIMERS_H

#include "types.h"

class EMUFILE;

// docs/PLAN.md §4.3 step 1 (peripherals): the four GBA hardware timers
// (TM0CNT-TM3CNT, 0x04000100-0x0400010F). Reference is GBATEK's documented
// prescaler/cascade/reload/IRQ behavior.

void gbaTimersReset();

// Advances all running, non-cascade-driven timers by `gbaCycles` GBA CPU
// cycles (NOT nds_timer's doubled ARM9-clock-rate ticks -- see the call
// sites in NDSSystem.cpp's gbaExecFrame for the /2 conversion) and handles
// overflow/reload/cascade/IRQ. Call once per HDraw and once per HBlank
// segment, matching the PPU's own per-segment hooks.
void gbaTimersStep(int gbaCycles);

// I/O write hooks for the TMxCNT_L/TMxCNT_H registers -- see gba_io.cpp
// for the dispatcher that routes writes here.
void gbaTimersIoWrite8(u32 offset, u8 val);
void gbaTimersIoWrite16(u32 offset, u16 val);

// PLAN.md §4.3 item 7 (GBA savestate support). counter/subCycles aren't
// reconstructible from TMxCNT_L/H alone -- see gba_timers.cpp's file
// comment: TMxCNT_L's write-side (reload) and read-side (live counter)
// values are two different pieces of state sharing one address, and
// subCycles (the fractional prescaler tick accumulator) has no register
// backing at all.
void gbaTimersSaveState(EMUFILE* os);
bool gbaTimersLoadState(EMUFILE* is, int size);

#endif
