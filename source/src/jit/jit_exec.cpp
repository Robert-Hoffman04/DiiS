/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_exec.cpp
 *
 * The bridge into live emulation. armInnerLoop() (NDSSystem.cpp) calls
 * jitRunArm7() when the ARM7 is due to step: it compiles/looks up a block at
 * the current PC, runs it, re-primes the interpreter's pipeline at the resume
 * PC and returns the cycles consumed (0 => "not handled, use the interpreter
 * for one instruction").
 *
 * P3: one block per call, no chaining (JIT_ENABLE_CHAINING off) -- keeps the
 * ARM9/ARM7 interleave fine-grained. IRQ delivery, mode switches and anything
 * the block can't compile fall back to the interpreter between blocks.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include "jit_trace.h"
#include "../armcpu.h"
#include <string.h>
#include <stdio.h>

// lightweight liveness telemetry (dumped to sd:/jit.log periodically)
u64 g_jitBlocksRun = 0;   // blocks that executed >= 1 guest instruction
u64 g_jitInsnsRun  = 0;
u64 g_jitAttempts  = 0;   // getBlock/compile attempts that reached ExecuteJITTrace
u64 g_jitBail0     = 0;   // attempts that bailed with 0 instructions
extern u64 g_jitSmcKills; // jit_cache.cpp -- real (non-empty-bucket) SMC invalidations
static void jitMaybeReport()
{
#ifdef DESMUME_JIT_TRACE_FIRST
	static u64 s_lastReport = 0;
	if (g_jitAttempts - s_lastReport < 50000) return;
	s_lastReport = g_jitAttempts;
	FILE* f = fopen("sd:/jit.log", "a");
	if (f) { fprintf(f, "[jit] alive: %llu run (%llu insns), %llu attempts, %llu bail0, %llu smcKills, arena=%u\n",
	                 (unsigned long long)g_jitBlocksRun, (unsigned long long)g_jitInsnsRun,
	                 (unsigned long long)g_jitAttempts, (unsigned long long)g_jitBail0,
	                 (unsigned long long)g_jitSmcKills,
	                 (unsigned)jitCache.getArenaOffset()); fclose(f); }
#endif
}

#if defined(JIT_DIFFERENTIAL_TESTING)
#include "jit_differential.h"
#endif

// Runtime master switch. Defaults on for a JIT build; a menu toggle can flip it.
bool jitArm7Enabled = true;

u32 jitRunArm7()
{
	if (!jitArm7Enabled || !jitActiveProfile) return 0;

	armcpu_t& cpu = NDS_ARM7;
	if (cpu.CPSR.bits.T == 0) return 0;            // ARM mode -> interpreter (P7)

	const u32 pc = cpu.instruct_adr;

	BasicBlock* b = jitCache.getBlock(pc);
	if (!b || (b->execute == nullptr && b->length == 0))
		b = jitCompileTrace(pc, jitCache, *jitActiveProfile);
	if (!b || b->execute == nullptr) return 0;     // uncompilable / "don't JIT" -> interpreter

#if defined(JIT_DIFFERENTIAL_TESTING)
	return jitRunArm7Checked(&cpu, b, pc);
#endif

	cpu.R[15] = pc + 4;                            // THUMB pipeline the block expects
	jit_cpu_state st = { &cpu.R[0], &cpu.CPSR.val, nullptr };

	JITResult r;
	memset(&r, 0, sizeof r);
	ExecuteJITTrace(b->execute, &r, &st);
	g_jitAttempts++;

	if (r.smcHit)
		jitCache.invalidateSMCTarget(r.smcAddress);

#ifdef DESMUME_JIT_TRACE_FIRST
	if (g_jitAttempts <= 60) {
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) { fprintf(f, "[jit] blk pc=%08x op=%04x len=%u ins=%u bail=%u smc=%u cyc=%u npc=%08x\n",
		                 (unsigned)pc, (unsigned)jitActiveProfile->fetch16(pc & ~1u),
		                 (unsigned)b->length, (unsigned)r.instructions,
		                 (unsigned)r.bailedOut, (unsigned)r.smcHit, (unsigned)r.cycles,
		                 (unsigned)r.nextPC); fclose(f); }
	}
#endif

	// A block that made zero forward progress (bailed on its first instruction)
	// must hand that instruction to the interpreter, or the same block is
	// re-entered forever. If it is a lone terminator (length 1) that keeps
	// bailing -- a BX/POP{pc} into ARM, the common ARM7 idle-loop shape --
	// demote it to a "don't JIT" marker so future visits skip straight to
	// the interpreter instead of paying the compile+trampoline cost.
	if (r.instructions == 0) {
		if (b->length == 1) jitCache.registerBlock(pc, 1, nullptr);
		cpu.R[15] = pc + 4;
		g_jitBail0++;
		jitMaybeReport();
		return 0;
	}

	// re-prime the interpreter pipeline at the resume PC (still THUMB: any
	// mode switch bailed out before executing)
	const u32 npc = r.nextPC;
	cpu.instruct_adr     = npc;
	cpu.instruction      = jitActiveProfile->fetch16(npc & ~1u);
	cpu.next_instruction = npc + 2;
	cpu.R[15]            = npc + 4;

	g_jitBlocksRun++;
	g_jitInsnsRun += r.instructions;
	jitMaybeReport();

	return r.cycles ? r.cycles : 1;
}

#endif // DESMUME_JIT_ARM7
