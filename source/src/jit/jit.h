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

// GO-FIX-PH temporary diagnostic (jit_trace.cpp): verifies the canary bytes
// planted right after each JIT-owned memalign'd buffer are still intact;
// logs once to sd:/jit.log and latches on the first mismatch. Cheap enough
// to poll periodically from the dispatch path. Remove once the heap
// corruption bug is found.
void jitCheckCanaries();

// Live execution. Called from armInnerLoop() when the ARM7 is due to step.
// Runs one JIT block from NDS_ARM7.instruct_adr and re-primes the interpreter
// pipeline; returns cycles consumed, or 0 if the interpreter should handle
// this instruction (uncompilable region / "don't JIT" / disabled). Both THUMB
// and ARM mode are compiled (P11).
extern bool jitArm7Enabled;
u32 jitRunArm7();

// ARM9 counterpart. Spliced into armInnerLoop()'s ARM9 arm. Inert until the
// ARM9 THUMB front-end lands (A2): jitArm9Enabled defaults false and the ARM9
// profile's canEnter* return false, so this compiles nothing and always
// returns 0. See desmumewii-arm9-jit-plan.md.
extern bool jitArm9Enabled;
u32 jitRunArm9();

#if defined(JIT_DIFFERENTIAL_TESTING)
// Differential-harness guest-memory journal (jit_differential.cpp). The three
// MMU write choke points (_MMU_write08/16/32 in MMU.h) call this *before*
// mutating memory, so the interpreter reference run inside jitRunArm*Checked()
// can be rolled back byte-for-byte and store-containing blocks are actually
// compared. A no-op (one predictable-branch load) unless the harness has armed
// the journal around a reference run; never compiled into a shipping build.
void jitDiffJournalNote(int procnum, u32 addr, u32 size);

// Read side of the same harness gap: the interpreter reference run and the JIT
// run each execute the block's loads once, so a read with a side effect (IPC
// FIFO pop at 0x04100000, some I/O) is applied twice and the JIT sees advanced
// state -- a false mismatch, the read-analogue of the store-double-apply bug A1
// fixed for writes. Called from the _MMU_read* choke points; when the journal
// is armed, a data read from the I/O bank marks the block untrusted so the
// comparison is skipped (idempotent VRAM/palette/OAM reads stay trusted). `at`
// is MMU_ACCESS_TYPE as int (MMU_AT_CODE reads are ignored).
void jitDiffJournalNoteRead(int procnum, int at, u32 addr);

// Boot-time proof that the journal records + rolls back correctly (the PH boot
// path runs almost no ARM7 THUMB blocks, so the live harness alone can't
// exercise it). Logs [jit] journal selftest ... to sd:/jit.log. Call after
// MMU_Init().
bool jitDiffJournalSelfTest();
#endif

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
