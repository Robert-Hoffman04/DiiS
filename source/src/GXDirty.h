/*
    Copyright (C) 2026 DeSmuMEWii team

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

//------------------------------------------------------------------------------
// GXDirty - write-barrier / dirty-flag layer for the GX 2D-BG compositor
// (desmumewii-2d-compositor-plan.md Step 5.0).
//
// The GX BG path (Step 5.1, GX2DBG.*) caches each MAIN BG layer as a baked
// "resolved plane" texture and only wants to rebuild the 8x8 tiles whose
// backing bytes actually changed.  This tracks writes into:
//   - VRAM  (guest 0x06xxxxxx -> MMU.ARM9_LCD)  : per 16 KB LCDC physical page
//   - palette RAM (guest 0x05xxxxxx -> ARM9_VMEM): one whole-region generation
//   - OAM   (guest 0x07xxxxxx -> ARM9_OAM)       : one whole-region generation
//
// After MMU_LCDmap<>() a VRAM address is  0x06000000 + (lcdPage<<14) + ofs  with
// lcdPage the physical LCDC page (0..40), so (adr>>14)&0x3F is that page index.
// palette/OAM addresses are untouched by MMU_LCDmap and land at adr>>20 ==
// 0x50 / 0x70 respectively; VRAM is 0x60..0x67.
//
// THREADING: every hook and every consumer (GXMerge_Present) runs on the
// emulation/core thread.  Single writer, single reader, same thread => no
// locking.  The draw thread never touches this state.
//
// The hot-path hooks early-out on g_gxDirtyArmed (kept in lock-step with
// GXMerge's 2D-BG opt-in), so a build that never enables the GX BG path pays
// exactly one predicted-not-taken branch per guest store.  Define
// GXDIRTY_DISABLED to compile the hooks out entirely (A/B perf builds).
//------------------------------------------------------------------------------

#ifndef GXDIRTY_H
#define GXDIRTY_H

#include "types.h"

#define GXD_LCD_PAGES 64   /* covers physical LCDC pages 0..40 with room to spare */

struct GXDirtyState
{
	u8  lcdPageDirty[GXD_LCD_PAGES];  // 1 == that 16 KB LCDC page was written since last consumed
	u32 palGen;                      // ++ on any write to palette RAM (0x05xxxxxx)
	u32 oamGen;                      // ++ on any write to OAM (0x07xxxxxx)  [used by 5.2]
	u32 fullGen;                     // ++ when guest-page<->content association breaks
};

extern GXDirtyState g_gxDirty;
extern bool         g_gxDirtyArmed;

#ifdef __cplusplus
extern "C" {
#endif

// Full invalidate: bank remap, machine reset, savestate load, screen power flip.
void GXDirty_FullInvalidate(void);

// Display capture writes bypass _MMU_write*; mark the 128 KB write block.
void GXDirty_MarkCaptureBlock(int writeBlock);

// Keep the armed flag in lock-step with the GX 2D-BG opt-in.
void GXDirty_SetArmed(bool on);

#ifdef __cplusplus
}
#endif

#ifdef GXDIRTY_DISABLED

static FORCEINLINE void GXDirty_NoteARM9(u32 adr)     { (void)adr; }
static FORCEINLINE void GXDirty_NoteARM7VRAM(u32 adr) { (void)adr; }

#else

// adr is the post-MMU_LCDmap address (VRAM already collapsed to 0x0600xxxx).
static FORCEINLINE void GXDirty_NoteARM9(u32 adr)
{
	if (!g_gxDirtyArmed) return;
	u32 p = adr >> 20;
	if (p - 0x60u < 8u)  g_gxDirty.lcdPageDirty[(adr >> 14) & (GXD_LCD_PAGES - 1)] = 1;
	else if (p == 0x50u) g_gxDirty.palGen++;
	else if (p == 0x70u) g_gxDirty.oamGen++;
}

// The ARM7 write tail is shared by WRAM/RAM writes too; only VRAM banks C/D
// (post-map 0x0600xxxx) are of interest here.
static FORCEINLINE void GXDirty_NoteARM7VRAM(u32 adr)
{
	if (!g_gxDirtyArmed) return;
	if ((adr >> 20) - 0x60u < 8u)
		g_gxDirty.lcdPageDirty[(adr >> 14) & (GXD_LCD_PAGES - 1)] = 1;
}

#endif  // GXDIRTY_DISABLED

#endif  // GXDIRTY_H
