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
#ifndef GBA_PPU_H
#define GBA_PPU_H

// docs/PLAN.md §4.3 step 1 (peripherals): GBA PPU (video). Modeled as a
// scanline-accurate raster device driven by NDSSystem.cpp's GBA frame
// executor (gbaExecFrame(), NDSSystem.cpp), which runs the ARM7 CPU for
// each scanline's HDraw/HBlank duration in turn and calls the hooks below
// at the matching points -- mirroring how the DS side already renders one
// GPU_RenderLine() per scanline rather than one shot per frame, so
// mid-frame register rewrites (common raster effects: per-scanline
// scroll/affine warps, palette cycling) are honored, not just the register
// state at frame start.
//
// Not implemented this pass (documented simplifications, not silently
// dropped hardware behavior): alpha blending (BLDCNT/BLDALPHA/BLDY),
// windows (WIN0/WIN1/WINOBJ, WININ/WINOUT), mosaic (MOSAIC), and the
// green-swap/OBJ-semi-transparent-via-alpha modes. Every enabled layer is
// treated as fully opaque; only per-pixel transparency (palette index 0 /
// bitmap-mode "always opaque") and BG/OBJ priority ordering are honored.

#include "types.h"
#include "gx/gx_frameplan.h"

class EMUFILE;

// ---------------------------------------------------------------------
// I/O register offsets (within MMU.GBA_IOREG, i.e. relative to
// 0x04000000). Matches GBATEK's documented GBA I/O map; only the video
// registers are listed.
// ---------------------------------------------------------------------
enum
{
	IO_DISPCNT  = 0x000,
	IO_DISPSTAT = 0x004,
	IO_VCOUNT   = 0x006,
	IO_BG0CNT   = 0x008,
	IO_BG1CNT   = 0x00A,
	IO_BG2CNT   = 0x00C,
	IO_BG3CNT   = 0x00E,
	IO_BG0HOFS  = 0x010,
	IO_BG2PA    = 0x020,
	IO_BG2X     = 0x028,
	IO_BG2Y     = 0x02C,
	IO_BG3PA    = 0x030,
	IO_BG3X     = 0x038,
	IO_BG3Y     = 0x03C,
	IO_WIN0H    = 0x040,
	IO_WIN1H    = 0x042,
	IO_WIN0V    = 0x044,
	IO_WIN1V    = 0x046,
	IO_WININ    = 0x048,
	IO_WINOUT   = 0x04A,
	IO_MOSAIC   = 0x04C,
	IO_BLDCNT   = 0x050,
	IO_BLDALPHA = 0x052,
	IO_BLDY     = 0x054,
	IO_IE       = 0x200,
	IO_IF       = 0x202,
	IO_IME      = 0x208,
};

// Stage 0 (see nds-wii-render-pipeline.md / source/gx/gx_frameplan.h) draw
// plan for the GBA PPU - the first engine instance wired up, since it's the
// narrowest of the three (no 3D layer, no capture unit). gbaPpuReset()
// sizes the dirty bitmaps; gbaPpuBeginFrame() clears the accumulated trace
// for the new frame; MMU.cpp's GBA VRAM/palette/OAM write funnel and this
// file's I/O register write funnel populate it as the frame's CPU cycles
// run.
extern GxFramePlan g_gbaFramePlan;

// Composited output, one u16 per pixel, native GBA color layout
// (bit15 unused, bits10-14 B, bits5-9 G, bits0-4 R) -- this is the exact
// same X-B5-G5-R5 layout GPU_screen (GPU.h) already uses, so callers can
// blit it into GPU_screen with no per-pixel conversion (see harness_frame.
// cpp's existing RGB565 repack, which already assumes that layout).
#define GBA_SCREEN_W 240
#define GBA_SCREEN_H 160
extern u16 GBA_screen[GBA_SCREEN_W * GBA_SCREEN_H];

void gbaPpuReset();

// Frame-boundary hooks, called once per GBA video frame by NDSSystem.cpp's
// gbaExecFrame().
void gbaPpuBeginFrame();
void gbaPpuEndFrame();

// Per-scanline hooks. `line` is 0..227 (0..159 visible, 160..227 VBlank).
// Called at the HDraw/HBlank boundary of each line, after the CPU has run
// for that portion's duration -- see gbaExecFrame() for the timing this is
// paced against.
void gbaPpuHDrawEnd(int line);   // renders the line (if visible), sets HBlank flag
void gbaPpuHBlankEnd(int line);  // clears HBlank flag, advances VCOUNT (and VBlank at 160)

// I/O register writes for the 0x04000000-0x040003FF block route here
// instead of a raw buffer write (MMU.cpp) -- DISPSTAT/VCOUNT split their
// bits between CPU-writable (IRQ enables, VCount-match setting) and
// hardware-owned (VBlank/HBlank/VCounter-match flags); everything else
// just falls through to a plain write into MMU.GBA_IOREG.
void gbaPpuIoWrite8(u32 offset, u8 val);
void gbaPpuIoWrite16(u32 offset, u16 val);
void gbaPpuIoWrite32(u32 offset, u32 val);

// PLAN.md §4.3 item 7 (GBA savestate support): the private per-scanline
// counter and affine reference-point accumulators. VCOUNT/DISPSTAT
// themselves are hardware-owned bits kept authoritative directly in
// MMU.GBA_IOREG (already covered by the GBA_IOREG memory chunk) and stay
// in lockstep with the internal scanline counter this saves, but the
// affine accumulators (BGx X/Y reference point, incremented by PB/PD each
// scanline) are pure internal state with no register backing at all --
// only the frame-start latch value is ever written back to a register.
// GBA_screen (the composited pixel output) is intentionally NOT part of
// this: it's a render cache fully regenerated by the next scanline/frame
// render and never consulted as computation input, so restoring stale
// pixels would be pointless work, not a correctness gap.
void gbaPpuSaveState(EMUFILE* os);
bool gbaPpuLoadState(EMUFILE* is, int size);

#endif // GBA_PPU_H
