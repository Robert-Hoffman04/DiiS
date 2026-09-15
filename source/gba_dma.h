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

#ifndef GBA_DMA_H
#define GBA_DMA_H

#include "types.h"

// docs/PLAN.md §4.3: the four GBA DMA channels (DMA0SAD..DMA3CNT_H,
// 0x040000B0-0x040000DF). This is a "fires all at once" HLE, not a
// cycle-accurate one: an immediate-timing transfer runs to completion the
// instant its enable bit is written, and a VBlank/HBlank-timing transfer
// runs to completion the instant that scanline boundary is crossed --
// real hardware instead steals bus cycles from the CPU for the transfer's
// actual duration. Good enough to unblock code that starts a DMA and
// polls (or IRQs on) its completion, which is what this was added for
// (Minish Cap spin-waiting on a DMA enable bit with no DMA engine to ever
// clear it) -- NOT sufcient for anything timing-sensitive on the exact
// cycle a transfer finishes.

void gbaDmaReset();

// I/O write hooks -- see gba_io.cpp for the dispatcher that routes writes
// in the 0xB0-0xDF range here.
void gbaDmaIoWrite8(u32 offset, u8 val);
void gbaDmaIoWrite16(u32 offset, u16 val);
void gbaDmaIoWrite32(u32 offset, u32 val);

// Fire every channel currently armed for HBlank/VBlank timing, and (via
// vcount) DMA channel 3's Video Capture Special-timing mode. Call once per
// HBlank and once per VBlank-start (see NDSSystem.cpp's gbaExecFrame).
// vcount is the VCOUNT value of the scanline whose HBlank is firing (0..227).
//
// Per GBATEK, DMA3 with Start Timing=3 (Special) is "Video Capture", not
// Sound FIFO (that's DMA1/2 only, see gbaDmaOnFifoTrigger below): "Capture
// works similar like HBlank DMA, however, the transfer is started when
// VCOUNT=2, it is then repeated each scanline, and it gets stopped when
// VCOUNT=162." i.e. it fires at the same per-scanline point as ordinary
// HBlank-timing DMA, gated to VCOUNT in [2,161] (160 scanlines), reading a
// per-scanline word count from DMA3CNT_L each time (unlike Sound-FIFO
// Special, which hardcodes a fixed 4-word transfer regardless of CNT_L).
void gbaDmaOnHblank(int vcount);
void gbaDmaOnVblank();

// Fire DMA channel 1 or 2 if it's armed with Special timing and targeting
// fifoAddr (0x040000A0 = FIFO A, 0x040000A4 = FIFO B) -- called from
// gba_apu.cpp's gbaApuOnTimerOverflow() when that FIFO drops to half empty.
// Per GBATEK, a Sound-FIFO Special DMA always transfers exactly 4 words
// (16 bytes), ignoring DMAxCNT_L's count, with a fixed destination.
void gbaDmaOnFifoTrigger(u32 fifoAddr);

#endif
