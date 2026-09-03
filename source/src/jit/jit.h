/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit.h
 *
 * Top-level interface for the ARM7 trace JIT. Derived from Visual Boy Advance
 * GX's source/vba/gba/JIT.h (c) Daryl Borth, GPL v2+ -- see
 * jit/upstream/PROVENANCE.md and desmumewii-arm7-jit-plan.md.
 *
 * Phase P0: only the CPU-agnostic infrastructure (block cache + arena + linker
 * stub in jit_cache.*, PowerPC emitter macros in jit_ppc_emitter.h, ABI
 * trampoline in jit_trampoline.S) is present and compiled behind
 * DESMUME_JIT_ARM7. The THUMB / ARM front-ends and the interpreter wiring
 * arrive in later phases.
 ***************************************************************************/

#ifndef DESMUME_JIT_H
#define DESMUME_JIT_H

#include "../types.h"
#include "jit_debug.h"
#include "jit_cache.h"

// Maximum guest instructions the trace scanner will pull into one block.
#define JIT_TRACE_MAX_INSTRUCTIONS 42

// Fixed-layout handshake struct that compiled traces write their outcome into.
// Must stay 32-byte aligned and layout-stable: jit_trampoline.S and the emitted
// epilogues reach into it by hard-coded offset.
struct JITResult {
	u32 cycles;
	u32 nextPC;
	u32 instructions;
	u32 bailedOut;   // 1 if a guard failed, 0 on a clean exit / quota yield
	u32 smcHit;      // 1 if an SMC guard tripped
	u32 smcAddress;  // written EA when smcHit
} __attribute__((aligned(32)));

#if defined(DESMUME_JIT_ARM7)

// Hand-written PowerPC ABI bridge (jit_trampoline.S). The trailing two
// arguments are the guest flag store and the guest read-page table; their
// concrete types are pinned down when the JitCpuProfile seam lands in P1.
extern "C" void ExecuteJITTrace(JITBlockFunc execute, JITResult* outResult,
                                u32* busPrefetchCount, u32* guestRegs,
                                void* guestFlags, void* readTable);
extern "C" void ExecuteJITTrace_Return();

#endif // DESMUME_JIT_ARM7

#endif // DESMUME_JIT_H
