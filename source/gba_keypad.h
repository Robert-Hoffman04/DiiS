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

#ifndef GBA_KEYPAD_H
#define GBA_KEYPAD_H

#include "types.h"

// docs/PLAN.md §4.3 step 1 (peripherals): GBA keypad (KEYINPUT/KEYCNT,
// 0x04000130/0x04000132). KEYINPUT itself needs no dedicated module --
// NDS_applyFinalInput() (NDSSystem.cpp) already computes the exact
// active-low bit pattern GBA hardware uses (confirmed identical to the DS
// ARM7_REG/ARM9_REG KEYINPUT layout it already writes) and mirrors it
// straight into MMU.GBA_IOREG; this file only owns KEYCNT's IRQ condition.

void gbaKeypadReset();
void gbaKeypadIoWrite8(u32 offset, u8 val);
void gbaKeypadIoWrite16(u16 val);

// Evaluates KEYCNT against the current KEYINPUT and requests bit12 (Keypad)
// if the configured condition is met. Real hardware evaluates this
// continuously; checking once per frame (called from gbaExecFrame, after
// the frame's input has been latched into KEYINPUT) is a documented
// simplification -- this harness's input never changes faster than once
// per frame anyway.
void gbaKeypadCheckIrq();

#endif
