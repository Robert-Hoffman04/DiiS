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

// docs/PLAN.md §4.3: shared GBA interrupt-delivery HLE. This is not a real
// ARM-level IRQ exception -- GBA mode direct-boots with no BIOS code loaded
// (bios_gba.cpp is function-level HLE, same as the rest of GBA BIOS
// support), so there is no BIOS IRQ vector stub to jump to and no way to
// reach a game's real interrupt-service-routine pointer (0x03007FFC)
// through the normal architectural path. What *does* already exist is
// bios_gba.cpp's Halt/IntrWait HLE (gba_wait4IRQ/gba_IntrWait), which parks
// the CPU (NDS_ARM7.waitIRQ) and wakes it by polling a BIOS-convention flag
// word at 0x03007FF8 -- exactly what the real BIOS's IRQ trampoline would
// have ORed into on return from a real handler. gbaRequestIrq() stands in
// for that trampoline: it raises IF, and if IE/IME both allow the bit(s)
// through, acks them (write-1-to-clear, matching real BIOS behavior) and
// ORs the serviced bits into 0x03007FF8, un-parking the CPU. That is
// enough to unblock the extremely common IntrWait()-paced main loop used
// by essentially every commercial game, but NOT enough for a game that
// installs and relies on a real custom ISR instead of polling through
// Halt/IntrWait -- that still needs a real BIOS IRQ-trampoline HLE
// (installing a vector at 0x18/branching to [0x03007FFC]).
//
// A real ARM-level dispatch (armcpu_irqException() jumping to a hand-
// written trampoline at GBA_BIOS+0x18, calling through [0x03007FFC]) was
// first prototyped and tested live against Minish Cap in an earlier
// session: it reliably corrupted execution within the first second (PC
// drifting into garbage, floods of "Undefined instruction") even when
// gated on the ISR pointer being non-null. That attempt called
// armcpu_irqException() directly from gbaRequestIrq() -- which is itself
// reachable from a register-write handler (gba_dma.cpp's doTransfer(),
// for an immediate-timing DMA with IRQ-on-completion) while armcpu_exec()
// is still on the call stack mid-instruction. Firing a real mode-switch/
// PC-redirect exception in the middle of an in-flight instruction handler
// corrupts armcpu_exec()'s own unconditional post-execute
// armcpu_prefetch() call (it prefetches again, from the *new* IRQ-vector
// PC, immediately after armcpu_irqException() already prefetched once) --
// a real double-prefetch/PC-drift bug, not a trampoline-encoding bug. DS
// mode's equivalent (execHardware_interrupts()) never has this problem:
// it's called only from NDS_exec's own for(;;) loop, strictly between
// armInnerLoop() invocations, never nested inside one.
//
// Fixed by splitting delivery into two phases: gbaRequestIrq() (below)
// still only ever raises IF/ORs the BIOS-convention flag word/un-parks
// the CPU -- safe to call from anywhere, including mid-instruction, same
// as before. The actual armcpu_irqException() call now lives in
// gbaIrqDispatchIfPending() (bottom of this file), which gbaExecFrame()
// (NDSSystem.cpp) calls only at its own per-HDraw/HBlank chunk
// boundaries -- structurally the same position execHardware_interrupts()
// already occupies relative to armInnerLoop on the DS side.
//
// Originally lived inline in gba_ppu.cpp (VBlank-only); pulled out here
// once timers/keypad/DMA needed the exact same delivery mechanism for
// their own IE/IF bits.

#include "gba_irq.h"
#include "MMU.h"
#include "mem.h"
#include "armcpu.h"
#include <string.h>

enum { IO_IE = 0x200, IO_IF = 0x202, IO_IME = 0x208 };

static inline u16 io16(u32 off) { return T1ReadWord(MMU.GBA_IOREG, off); }
static inline void io16w(u32 off, u16 v) { T1WriteWord(MMU.GBA_IOREG, off, v); }

void gbaRequestIrq(u16 bits)
{
	u16 ifr = (u16)(io16(IO_IF) | bits);
	io16w(IO_IF, ifr);

	u16 ie = io16(IO_IE);
	u16 ime = io16(IO_IME);
	u16 serviced = ie & ifr;
	if (serviced && (ime & 1))
	{
		// Once a real handler is installed, gbaIrqDispatchIfPending() will
		// deliver a genuine ARM exception into it on the next chunk
		// boundary, and it is the GAME's own ISR that must ack IF
		// (write-1-to-clear) and OR the BIOS-convention flag word at
		// 0x03007FF8 -- exactly as it would on real hardware. Pre-acking
		// IF here would make the real ISR observe IF==0 and conclude
		// there's nothing to service, silently skipping its own
		// bookkeeping (this was found live: Minish Cap's ISR never set
		// its own "work pending" flag because this HLE had already
		// zeroed IF out from under it). Only fall back to doing the
		// ack/flags-word HLE ourselves while no real handler exists yet,
		// so Halt/IntrWait-paced code occurring before the game installs
		// its handler still gets unblocked.
		if (T1ReadLong(MMU.GBA_IWRAM, 0x7FFC) == 0)
		{
			io16w(IO_IF, (u16)(ifr & ~serviced)); // BIOS-standard ack (write-1-to-clear)
			u32 flags = T1ReadLong(MMU.GBA_IWRAM, 0x7FF8);
			T1WriteLong(MMU.GBA_IWRAM, 0x7FF8, flags | serviced);
		}
		// Real hardware: Halt/Stop unconditionally wake on any serviced,
		// enabled interrupt regardless of what the ISR does -- this part
		// is not the flags-word shortcut, keep it unconditional.
		NDS_ARM7.waitIRQ = FALSE;
	}
}

// Hand-assembled ARM trampoline (devkitARM: arm-none-eabi-as/ld -Ttext=0x18/
// objcopy -O binary -- not hand-encoded, so the machine code is known-
// correct), installed at GBA_BIOS+0x18 (armcpu->intVector is 0 for ARM7, so
// armcpu_irqException() jumps to exactly this address):
//   push {r0-r3,r12,lr}
//   ldr  r0, =0x03007FFC   ; the BIOS-convention user-IRQ-handler pointer
//   ldr  r0, [r0]
//   mov  lr, pc
//   bx   r0                ; call through it, ARM or Thumb per bit0
//   pop  {r0-r3,r12,lr}
//   subs pc, lr, #4         ; standard IRQ return (lr was set to +4 by
//                            ; armcpu_irqException, matching this)
static const u8 s_trampoline[32] = {
	0x0f,0x50,0x2d,0xe9, 0x10,0x00,0x9f,0xe5, 0x00,0x00,0x90,0xe5, 0x0f,0xe0,0xa0,0xe1,
	0x10,0xff,0x2f,0xe1, 0x0f,0x50,0xbd,0xe8, 0x04,0xf0,0x5e,0xe2, 0xfc,0x7f,0x00,0x03
};

void gbaIrqInstallTrampoline()
{
	memcpy(MMU.GBA_BIOS + 0x18, s_trampoline, sizeof(s_trampoline));
}

void gbaIrqDispatchIfPending()
{
	u16 ie = io16(IO_IE);
	u16 ifr = io16(IO_IF);
	u16 ime = io16(IO_IME);

	if (!((ie & ifr) && (ime & 1))) return;
	if (NDS_ARM7.CPSR.bits.I) return; // armcpu_irqException's own gate, checked early to avoid touching state below for nothing

	// Defensive: only take the real exception if the game has actually
	// registered a handler pointer. On real hardware an enabled-but-
	// unhandled IRQ would still fire and jump through whatever garbage is
	// at 0x03007FFC (matching hardware misbehavior, not a documented
	// simplification we'd want) -- but every commercial game that enables
	// an IRQ source sets this up during init well before that source can
	// fire, so in practice this only guards the narrow window between
	// IE being set and the handler pointer being written.
	if (T1ReadLong(MMU.GBA_IWRAM, 0x7FFC) == 0) return;

	armcpu_irqException(&NDS_ARM7);
}
