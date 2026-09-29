/****************************************************************************
 * DeSmuMEWii ARM JIT
 *
 * jit.h
 *
 * Top-level interface for the ARM-to-PowerPC trace JIT (ARM9 and ARM7, ARM and
 * THUMB). Derived from Visual Boy Advance GX's source/vba/gba/JIT.h (c) Daryl
 * Borth, GPL v2+ -- see jit/upstream/PROVENANCE.md.
 *
 * Built in by default (make NOJIT=1 drops it); whether it runs is a runtime
 * choice, Interpreter or JIT for both cores (jitSetEnabled/jitRequestMode).
 * Opcodes without a front-end are run through the interpreter from inside the
 * trace (jitInterpFallback) instead of ending the block.
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

#if defined(DESMUME_JIT)

// Hand-written PowerPC ABI bridge (jit_trampoline.S). startCycles seeds the
// r3 cycle accumulator (0 == the old fixed JIT_YIELD_NUMBER quota); the
// returned out->cycles includes it -- see jitQuotaStart() in jit_trace.h.
extern "C" void ExecuteJITTrace(JITBlockFunc execute, JITResult* out, jit_cpu_state* st, u32 startCycles);
extern "C" void ExecuteJITTrace_Return();

// Lifecycle -- called from NDS_Init() / NDS_DeInit().
void jitInit();
void jitShutdown();

// roadmap #20 (GBA compat), §12.3 step 5: swaps jitProfile[JIT_ARM7] between
// the DS ARM7 profile and the GBA ARM7 profile (jit_arm7gba_profile.cpp) --
// both are built once by jitInit() and share jitCacheArm7, since only one
// boot mode is ever live at a time. Called from NDS_DebugForceGBAMode(),
// alongside that function's other gameInfo.isGBA/MMU.isGBA mirror updates.
// A no-op before jitInit() has (successfully) run. Safe before the (lazy) ARM7
// slot exists: the choice is remembered and published by jitEnsureArm7().
void jitSetArm7GBAMode(bool enable);

// GO-FIX-PH temporary diagnostic (jit_trace.cpp): verifies the canary bytes
// planted right after each JIT-owned memalign'd buffer are still intact;
// logs once to sd:/jit.log and latches on the first mismatch. Cheap enough
// to poll periodically from the dispatch path. Remove once the heap
// corruption bug is found.
void jitCheckCanaries();

// F1 -- the runtime CPU mode. One user-facing setting with two values:
// Interpreter (both cores interpreted) or JIT (both cores JITted, the
// default). g_jitOn is the *applied* mode. NDS_exec() reads it once per frame
// to pick the armInnerLoop<..., jit> instantiation (NDSSystem.cpp), so
// interpreter mode never calls jitRunArm*() at all, and the MMU SMC hooks
// (MMU.h / MMU.cpp) test it inline, so interpreter mode pays one load+branch
// per hooked write instead of a call into jit_cache.cpp. Skipping those
// invalidations while the JIT is off is safe only because every switch
// flushes both block caches (jitSetEnabled() below).
//
// jitRequestMode() is the mid-game entry point (harness PKT_CTRL "cpumode",
// main.cpp): it only records the request, and NDS_exec() applies it at the
// top of the next frame via jitApplyPendingMode() -- a frame boundary, where
// neither core is inside a JIT dispatch. jitSetEnabled() applies at once and
// is for callers already at such a point (the startup picker, before
// NDS_Init()). Applying a mode syncs both cores' lazily-fetched interpreter
// pipelines (jitSyncPipeline()) and flushes both JIT caches, then drives
// jitArm7Enabled/jitArm9Enabled from it. Those two per-core flags remain the
// flags jitRunArm7()/jitRunArm9() actually check, so a caller that wants one
// core interpreted while the JIT mode is on (the armwrestler/arm7wrestler
// probes in main.cpp) can still clear one after a mode is applied.
extern bool g_jitOn;
extern s8   g_jitModePending;   // -1 = none, else the requested g_jitOn
void jitSetEnabled(bool jit);
void jitRequestMode(bool jit);
static inline void jitApplyPendingMode()
{
	if (g_jitModePending >= 0) {
		const bool jit = (g_jitModePending != 0);
		g_jitModePending = -1;
		jitSetEnabled(jit);
	}
}

// Live execution. Called from armInnerLoop() when the ARM7 is due to step.
// Runs one JIT block from NDS_ARM7.instruct_adr and points the interpreter
// pipeline at the resume PC (the opcode fetch itself is deferred -- see
// jitSyncPipeline() below); returns cycles consumed, or 0 if the interpreter
// should handle this instruction (uncompilable region / "don't JIT" /
// disabled). Both THUMB and ARM mode are compiled (P11). `budget` is how many ARM7 cycles the
// scheduler can spare before its next event: a chain of
// linked blocks runs up to min(budget, JIT_ARM7_QUOTA_CAP) cycles (plus the
// usual one-block overshoot) before returning. The ARM7 slot (~3.5 MB arena +
// tables) is allocated on the first call that gets past jitArm7Enabled -- see
// jitEnsureArm7() (jit_trace.h).
extern bool jitArm7Enabled;
u32 jitRunArm7(s32 budget);

// ARM9 counterpart. Spliced into armInnerLoop()'s ARM9 arm. jitArm9Enabled
// follows the runtime mode like jitArm7Enabled (a JIT_DIFFERENTIAL_TESTING
// build ignores it and always runs the checked path). `budget` is in ARM9
// cycles, capped by JIT_ARM9_QUOTA_CAP -- see jitRunArm7().
extern bool jitArm9Enabled;
u32 jitRunArm9(s32 budget);

// Lazy interpreter-pipeline re-prime. A compiled run
// leaves instruct_adr / next_instruction / R[15] pointing at the resume PC
// but no longer fetches the opcode there into cpu.instruction: the next
// thing to run is almost always the JIT again, which only reads
// instruct_adr, so that fetch (an indirect call into _MMU_read*<MMU_AT_CODE>
// per dispatch) was wasted. Instead the core is flagged stale, and every
// reader of cpu.instruction outside the interpreter/fallback handlers --
// armInnerLoop's armcpu_exec() fallback, savestate_save(), the LOG_ARM*
// tracers -- calls jitSyncPipeline() first. Indexed JIT_ARM9 / JIT_ARM7
// (== ARMCPU_ARM9 / ARMCPU_ARM7). A stale flag that is set when the pipeline
// is actually fresh (e.g. armcpu_irqException() prefetched the vector since)
// only costs a redundant re-fetch of the same opcode, never a wrong one: the
// sync derives everything from instruct_adr + CPSR.T, exactly like
// armcpu_prefetch() does from next_instruction.
extern u8 g_jitPipeStale[2];
void jitSyncPipelineSlow(int core);
static inline void jitSyncPipeline(int core)
{
	if (g_jitPipeStale[core]) jitSyncPipelineSlow(core);
}

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

#endif // DESMUME_JIT

#endif // DESMUME_JIT_H
