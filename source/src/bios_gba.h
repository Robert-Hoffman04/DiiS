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

// roadmap #20 (GBA compat), §12.3 step 6: function-level HLE for the real
// GBA BIOS SWI table, mirroring bios.h/bios.cpp's ARM7_swi_tab pattern.
// See bios_gba.cpp for the design writeup (what's really implemented vs.
// left as a documented no-op, and why).

#ifndef BIOS_GBA_H
#define BIOS_GBA_H

#include "armcpu.h"

// Real GBA SWI numbering (GBATEK), 0x00 (SoftReset) - 0x1F (MidiKey2Freq).
// Only 32 entries: arm_instructions.cpp/thumb_instructions.cpp's swi_tab
// dispatch masks the SWI comment field with `& 0x1F` before indexing, so
// SWI numbers 0x20+ (MusicPlayer*, MultiBoot, HardReset, CustomHalt,
// SoundDriverVsyncOff/On, SoundDriverGetJumpList) can never reach a table
// entry at all -- they alias down into 0x00-0x0A instead. That's a
// pre-existing interpreter-wide limitation (equally true for DS's own
// ARM7_swi_tab/ARM9_swi_tab, also declared [32]), not something new here.
extern u32 (* ARM7GBA_swi_tab[32])();

#endif
