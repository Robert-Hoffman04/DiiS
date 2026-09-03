/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_cpu_profile.h
 *
 * The CPU-agnostic seam. The vendored VBA-GX trace JIT was welded to one
 * concrete CPU (the GBA's ARM7TDMI) and its memory map. This struct is the
 * single indirection through which the shared machinery (block cache, arena,
 * linker stub, trampoline, trace scanner, differential harness) reaches a
 * particular emulated core.
 *
 *   - ARM7  (isaLevel 4, ARMv4T) : jit_arm7_profile.cpp   -- phase P1
 *   - ARM9  (isaLevel 5, ARMv5TE): jit_arm9_profile.cpp   -- phase P8
 *
 * Only the pieces phases P1-P2 actually need are here; the struct grows as
 * later phases require (inline memory page tables land in P6, etc.).
 ***************************************************************************/

#ifndef DESMUME_JIT_CPU_PROFILE_H
#define DESMUME_JIT_CPU_PROFILE_H

#include "../types.h"

// -------------------------------------------------------------------------
// jit_cpu_state -- the block-execution context.
//
// LAYOUT IS ABI. jit_trampoline.S loads these fields by hard-coded offset,
// and compiled traces reach back through the pointers. Do not reorder or
// resize without editing jit_trampoline.S in lockstep.
// -------------------------------------------------------------------------
struct jit_cpu_state {
	u32*  gpr;        // +0  guest R[0..15]; the pipeline PC is gpr[15]
	u32*  cpsr;       // +4  guest CPSR word (packed N/Z/C/V in bits 31..28,
	                  //     which is exactly ARM's own flag nibble -- the
	                  //     packed-flag register in jit_ppc_emitter.h uses
	                  //     the same layout, so no bit shuffling is needed)
	u8**  readTable;  // +8  guest read page table; nullptr until phase P6
};

// -------------------------------------------------------------------------
// JitCpuProfile -- one instance per emulated core, built by the per-CPU
// jit_*_profile.cpp and published as jitActiveProfile by jitInit().
// -------------------------------------------------------------------------
struct JitCpuProfile {
	jit_cpu_state state;

	// Compile-time guest fetch: the trace scanner reads the guest instruction
	// stream (and folds PC-relative literals) while building a block.
	u32  (*fetch16)(u32 addr);
	u32  (*fetch32)(u32 addr);

	// Runtime guest memory -- the slow path taken for every access until the
	// inline main-RAM/WRAM fast path is emitted in P6. size is 1, 2 or 4.
	u32  (*slowRead)(u32 addr, u32 size);
	void (*slowWrite)(u32 addr, u32 val, u32 size);
	void (*smcInvalidate)(u32 addr);

	// Control -- each of these ends a trace.
	u32  (*swiHandler)(u32 comment);
	bool (*canEnterThumb)(u32 pc);
	bool (*canEnterArm)(u32 pc);

	// Timing -- must reproduce the interpreter's own per-instruction cost,
	// not VBA's GBA wait-state model (see the plan, risk "audio pitch drift").
	u8   (*cyclesForThumb)(u16 opcode);
	u8   (*cyclesForArm)(u32 opcode);

	s32  isaLevel;     // 4 = ARMv4T (ARM7), 5 = ARMv5TE (ARM9)
	u32  smcBankMask;  // bit b set => guest bank (addr>>24)==b can hold JIT code
};

extern JitCpuProfile* jitActiveProfile;   // set by jitInit(); null when JIT off

#endif // DESMUME_JIT_CPU_PROFILE_H
