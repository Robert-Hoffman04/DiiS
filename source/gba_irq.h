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

#ifndef GBA_IRQ_H
#define GBA_IRQ_H

#include "types.h"

// docs/PLAN.md §4.3: shared IRQ-delivery HLE, factored out of gba_ppu.cpp's
// original VBlank-only version so every peripheral interrupt source
// (timers, keypad, and PPU's own VBlank/HBlank/VCount) goes through one
// place. See gba_irq.cpp for why this is a BIOS-IRQ-trampoline stand-in,
// not a real ARM-level IRQ exception, and what that does and doesn't cover.
//
// `bits` is one or more of GBATEK's IE/IF bit positions (bit0 = VBlank,
// bit3-6 = Timer0-3, bit12 = Keypad, etc.) ORed together.
void gbaRequestIrq(u16 bits);

// Installs the hand-assembled BIOS IRQ trampoline at GBA_BIOS+0x18 (see
// gba_irq.cpp for why this exists and what it does). Call once, from
// NDS_Reset's GBA branch, after GBA_BIOS itself has been reset/reloaded.
void gbaIrqInstallTrampoline();

// Performs the real ARM-level IRQ exception dispatch (armcpu_irqException)
// if the architectural condition (IE&IF && IME) currently holds. Must be
// called ONLY from a point between armcpu_exec() invocations -- never from
// inside a register-write handler that might itself run while an
// instruction is mid-execution -- see gba_irq.cpp's file comment for why.
// gbaExecFrame() calls this at its own per-HDraw/HBlank chunk boundaries,
// the same structural position DS mode's execHardware_interrupts() already
// occupies relative to its own armInnerLoop calls.
void gbaIrqDispatchIfPending();

#endif
