/****************************************************************************
 * DeSmuMEWii ARM9 JIT
 *
 * jit_arm9_region.h
 *
 * The ARM9 "may a block start here?" region rule, as an inline. It is the
 * body of the ARM9 profile's canEnterThumb/canEnterArm (jit_arm9_profile.cpp,
 * which just wrap it) and is also called directly by jitRunArm9()
 * (jit_exec.cpp): there is only one ARM9 profile, so the dispatcher does not
 * need the indirect call through JitCpuProfile on every dispatch (PERF_LOG
 * Step 5). Keep the rule here only, so the two can never disagree.
 ***************************************************************************/

#ifndef DESMUME_JIT_ARM9_REGION_H
#define DESMUME_JIT_ARM9_REGION_H

#include "../types.h"
#include "../MMU.h"

// True iff a block may be compiled starting at `pc`. Mirrors the ARM9
// MMU_AT_CODE fast path (MMU.h): the JIT's compile-time fetch and the
// interpreter's own prefetch both go through _MMU_read*<ARM9, MMU_AT_CODE>, so
// any address that path decodes to a real backing store is safe to scan. The
// one exclusion is the DTCM window: DeSmuME's code path does NOT patch DTCM
// over main RAM (it returns MAIN_MEM there), so a block scanned from a
// DTCM-shadowed main-RAM address would compile the wrong bytes -- conservative
// bail, matching the plan. ARM and THUMB share the rule; B1 emitters are
// ARMv4T-safe, and ARMv5 edges (PC-interworking, CLZ/QADD/...) either bail or
// land in later B-groups.
static inline bool jitArm9CanEnter(u32 pc, bool thumb)
{
#ifdef DESMUME_JIT_ARM9_THUMB_ONLY
	// A5 isolation knob: keep the ARM9 THUMB JIT but route every ARM-mode block
	// to the interpreter, to tell an ARM-front-end (B1-B7c) bug apart from a
	// pre-existing THUMB (A2) one.
	if (!thumb) return false;
#else
	(void)thumb;
#endif
	// DTCM window (relocatable via CP15; default 0x027C0000). Excluded first
	// because it overlays the main-RAM range.
	if ((pc & ~0x3FFFu) == MMU.DTCMRegion) return false;

	if (pc < 0x02000000)                    return true;   // ITCM window
	if ((pc & 0x0F000000) == 0x02000000)    return true;   // main RAM (shared)
	if ((pc >> 24) == 0x03)                 return true;   // shared WRAM
	if ((pc & 0xFFFF0000u) == 0xFFFF0000u)  return true;   // ARM9 BIOS
	return false;
}

#endif // DESMUME_JIT_ARM9_REGION_H
