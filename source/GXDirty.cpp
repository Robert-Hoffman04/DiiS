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

#include "GXDirty.h"
#include <string.h>

GXDirtyState g_gxDirty   = { { 0 }, 0, 0, 0 };
bool         g_gxDirtyArmed = false;

void GXDirty_FullInvalidate(void)
{
	memset(g_gxDirty.lcdPageDirty, 1, sizeof(g_gxDirty.lcdPageDirty));
	g_gxDirty.palGen++;
	g_gxDirty.oamGen++;
	g_gxDirty.fullGen++;
}

void GXDirty_MarkCaptureBlock(int writeBlock)
{
	// Display capture targets a 128 KB block = eight 16 KB LCDC pages, as LCDC
	// banks A..D (physical pages 0..31).  The block index is 0..3.
	u32 base = ((u32)(writeBlock & 3) * 0x20000u) >> 14;
	for (int i = 0; i < 8; i++)
		g_gxDirty.lcdPageDirty[(base + i) & (GXD_LCD_PAGES - 1)] = 1;
}

void GXDirty_SetArmed(bool on)
{
	if (on && !g_gxDirtyArmed)
		GXDirty_FullInvalidate();   // first armed frame rebuilds everything
	g_gxDirtyArmed = on;
}
