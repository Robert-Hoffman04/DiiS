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

#include "gba_keypad.h"
#include "gba_irq.h"
#include "MMU.h"
#include "mem.h"

enum { IO_KEYINPUT = 0x130, IO_KEYCNT = 0x132 };

static inline u16 io16(u32 off) { return T1ReadWord(MMU.GBA_IOREG, off); }
static inline void io16w(u32 off, u16 v) { T1WriteWord(MMU.GBA_IOREG, off, v); }

void gbaKeypadReset()
{
	io16w(IO_KEYCNT, 0);
}

void gbaKeypadIoWrite16(u16 val)
{
	io16w(IO_KEYCNT, val);
}

void gbaKeypadIoWrite8(u32 offset, u8 val)
{
	u16 cur = io16(IO_KEYCNT);
	u16 merged = (offset == IO_KEYCNT) ? (u16)((cur & 0xFF00) | val) : (u16)((cur & 0x00FF) | (val << 8));
	io16w(IO_KEYCNT, merged);
}

// GBATEK: KEYCNT bit14 = IRQ enable, bit15 = condition (0 = logical OR --
// fire if any selected button is pressed; 1 = logical AND -- fire only if
// every selected button is pressed), bits0-9 = button select mask, tested
// against KEYINPUT (active-low: pressed = bit clear).
void gbaKeypadCheckIrq()
{
	u16 keycnt = io16(IO_KEYCNT);
	if (!((keycnt >> 14) & 1)) return;

	u16 mask = keycnt & 0x03FF;
	if (!mask) return;

	u16 pressed = (u16)(~io16(IO_KEYINPUT)) & 0x03FF;
	bool andCond = (keycnt >> 15) & 1;
	bool fire = andCond ? ((pressed & mask) == mask) : ((pressed & mask) != 0);
	if (fire) gbaRequestIrq(1 << 12);
}
