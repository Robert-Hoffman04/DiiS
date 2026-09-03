/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit.h
 *
 * Top-level interface for the ARM7 trace JIT. Derived from Visual Boy Advance
 * GX's source/vba/gba/JIT.h (c) Daryl Borth, GPL v2+ -- see
 * jit/upstream/PROVENANCE.md and desmumewii-arm7-jit-plan.md.
 *
 * Phase P1: the CPU-agnostic infrastructure is present and wired to an ARM7
 * JitCpuProfile, but no opcode front-end exists yet -- jitCompileTrace()
 * always produces a "don't JIT this" fallback block, so execution is
 * unchanged. jitSelfTest() round-trips a hand-emitted block through the
 * trampoline + linker stub to prove the ABI on real hardware.
 ***************************************************************************/

#ifndef DESMUME_JIT_H
#define DESMUME_JIT_H

#include "../types.h"
#include "jit_debug.h"
#include "jit_cpu_profile.h"
#include "jit_cache.h"

// Maximum guest instructions the trace scanner will pull into one block.
// Kept modest so one block ~= one armInnerLoop interleave slice (P3).
#define JIT_TRACE_MAX_INSTRUCTIONS 32

// Fixed-layout handshake struct that compiled traces write their outcome into.
// Must stay 32-byte aligned and layout-stable: jit_trampoline.S and the emitted
// epilogues reach into it by hard-coded offset (cycles@0, nextPC@4,
// instructions@8, bailedOut@12, smcHit@16, smcAddress@20).
struct JITResult {
	u32 cycles;
	u32 nextPC;
	u32 instructions;
	u32 bailedOut;   // 1 if a guard failed, 0 on a clean exit / quota yield
	u32 smcHit;      // 1 if an SMC guard tripped
	u32 smcAddress;  // written EA when smcHit
} __attribute__((aligned(32)));

#if defined(DESMUME_JIT_ARM7)

// Hand-written PowerPC ABI bridge (jit_trampoline.S).
extern "C" void ExecuteJITTrace(JITBlockFunc execute, JITResult* out, jit_cpu_state* st);
extern "C" void ExecuteJITTrace_Return();

// Lifecycle -- called from NDS_Init() / NDS_DeInit().
void jitInit();
void jitShutdown();

// Live execution. Called from armInnerLoop() when the ARM7 is due to step.
// Runs one JIT block from NDS_ARM7.instruct_adr and re-primes the interpreter
// pipeline; returns cycles consumed, or 0 if the interpreter should handle
// this instruction (ARM mode / uncompilable / disabled).
extern bool jitArm7Enabled;
u32 jitRunArm7();

// One-shot ABI round-trip check (hand-emitted block -> trampoline -> linker
// stub miss path -> return). Returns true on success; logs either way.
bool jitSelfTest();

#if defined(DESMUME_JIT_SELFTEST)
// Synthetic interpreter-vs-JIT differential over a table of THUMB vectors
// (jit_thumb_test.cpp). Returns the number of failing vectors.
int jitThumbSelfTest();
#endif

#endif // DESMUME_JIT_ARM7

#endif // DESMUME_JIT_H
