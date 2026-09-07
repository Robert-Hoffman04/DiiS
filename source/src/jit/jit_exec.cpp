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
 * A4-P1: JIT_ENABLE_CHAINING is on -- a call here can now run a whole chain
 * of blocks (up to ~JIT_YIELD_NUMBER guest cycles) before returning, not just
 * one. IRQ delivery, mode switches and anything a block can't compile still
 * fall back to the interpreter (a chain only runs compiled static-exit edges;
 * any dynamic exit, bailout or quota trip returns here first).
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
	                 (unsigned)jitCacheArm7.getArenaOffset()); fclose(f); }
#endif
}

#if defined(JIT_DIFFERENTIAL_TESTING)
#include "jit_differential.h"
#endif

// A2 coverage profiler (the port of VBA's Profiler): per-ARM9-step tally of
// ARM vs THUMB vs JIT-compilable-THUMB, so the THUMB-only front-end's reach can
// be reported before A3's benchmark. Dumped to sd:/jit.log with the other
// telemetry; DESMUME_JIT_TRACE_FIRST-gated, zero cost otherwise.
#ifdef DESMUME_JIT_TRACE_FIRST
// per-ARM9-step tally: arm-jit / thumb-jit == steps the respective front-end can
// take; region-out == a mode whose canEnter*() said no (uncompilable address).
u64 g_jit9Steps = 0, g_jit9ArmOk = 0, g_jit9ThumbOk = 0, g_jit9RegionOut = 0;
static void jit9ProfileReport()
{
	static u64 s_last = 0;
	if (g_jit9Steps - s_last < 2000000) return;
	s_last = g_jit9Steps;
	const u64 t = g_jit9Steps ? g_jit9Steps : 1;
	FILE* f = fopen("sd:/jit.log", "a");
	if (f) { fprintf(f, "[jit] arm9 mix: %llu steps  arm-jit=%llu%%  thumb-jit=%llu%%  region-out=%llu%%\n",
	                 (unsigned long long)g_jit9Steps,
	                 (unsigned long long)(g_jit9ArmOk * 100 / t),
	                 (unsigned long long)(g_jit9ThumbOk * 100 / t),
	                 (unsigned long long)(g_jit9RegionOut * 100 / t));
	         fclose(f); }
}
#endif

// Runtime master switch. Defaults on for a JIT build; a menu toggle can flip it.
bool jitArm7Enabled = true;

u32 jitRunArm7()
{
	JitCpuProfile* prof = jitProfile[JIT_ARM7];
	if (!jitArm7Enabled || !prof) return 0;

	// GO-FIX-PH: cheap periodic canary poll (see jit_trace.cpp / jit.h).
	{
		static u32 s_canaryPoll7 = 0;
		if ((++s_canaryPoll7 & 0xFFFFu) == 0) jitCheckCanaries();
	}

	armcpu_t& cpu = NDS_ARM7;
	const u32 pc = cpu.instruct_adr;
	const bool thumb = (cpu.CPSR.bits.T != 0);     // P11: ARM mode is JITted too
	const bool canEnter = thumb ? prof->canEnterThumb(pc) : prof->canEnterArm(pc);
	if (!canEnter) return 0;                       // uncompilable region -> interpreter

	BasicBlock* b = jitCacheArm7.getBlock(pc);
	if (!b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb)
		b = jitCompileTrace(pc, jitCacheArm7, *prof, thumb);
	if (!b || b->execute == nullptr) return 0;     // uncompilable / "don't JIT" -> interpreter

#if defined(JIT_DIFFERENTIAL_TESTING)
	return jitRunArm7Checked(&cpu, b, pc);
#endif

	cpu.R[15] = pc + (thumb ? 4 : 8);             // pipeline offset the block expects
	jit_cpu_state st = { &cpu.R[0], &cpu.CPSR.val, nullptr };

	JITResult r;
	memset(&r, 0, sizeof r);
	ExecuteJITTrace(b->execute, &r, &st);
	g_jitAttempts++;

	if (r.smcHit)
		jitCacheArm7.invalidateSMCTarget(r.smcAddress);

#ifdef DESMUME_JIT_TRACE_FIRST
	if (g_jitAttempts <= 60) {
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) { fprintf(f, "[jit] blk %s pc=%08x op=%08x len=%u ins=%u bail=%u smc=%u cyc=%u npc=%08x\n",
		                 thumb ? "T" : "A", (unsigned)pc,
		                 (unsigned)(thumb ? prof->fetch16(pc & ~1u) : prof->fetch32(pc & ~3u)),
		                 (unsigned)b->insnCount(), (unsigned)r.instructions,
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
		if (b->insnCount() == 1) jitCacheArm7.registerBlock(pc, 1, nullptr, thumb);
		cpu.R[15] = pc + (thumb ? 4 : 8);
		g_jitBail0++;
		jitMaybeReport();
		return 0;
	}

	// re-prime the interpreter pipeline at the resume PC. Most exits stay in
	// THUMB (any mode switch via BX/hi-reg bailed out before executing, see
	// above) -- but POP{...,PC} switches mode inline (jit_thumb.cpp) and
	// clears CPSR.T itself before returning here, so check it rather than
	// assuming THUMB: landing an ARM-mode target through a 16-bit THUMB
	// fetch misdecodes the real first opcode and sends ARM7's PC off into
	// unmapped memory (see the plan memory's boot-window regression writeup).
	const u32 npc = r.nextPC;
	cpu.instruct_adr = npc;
	if (cpu.CPSR.bits.T) {
		cpu.instruction      = prof->fetch16(npc & ~1u);
		cpu.next_instruction = npc + 2;
		cpu.R[15]            = npc + 4;
	} else {
		cpu.instruction      = prof->fetch32(npc & ~3u);
		cpu.next_instruction = npc + 4;
		cpu.R[15]            = npc + 8;
	}

	g_jitBlocksRun++;
	g_jitInsnsRun += r.instructions;
	jitMaybeReport();

	return r.cycles ? r.cycles : 1;
}

// ---------------------------------------------------------------------------
// ARM9 counterpart. Structurally identical to jitRunArm7() above (same shared
// scanner, trampoline and resume-pipeline logic) but against NDS_ARM9 /
// jitCacheArm9 / the ARM9 profile.
//
// A2: the ARM9 profile's canEnterThumb() is now a real region check and the
// THUMB emitter table is reused as-is (+ BLX). In a normal build jitArm9Enabled
// still defaults false -- ARM9 blast radius = whole game -- so this returns 0
// until A4 flips it. In a JIT_DIFFERENTIAL_TESTING build the enable is bypassed
// so a full boot+gameplay capture runs every ARM9 THUMB block through
// jitRunArm9Checked() against the hardened harness.
// ---------------------------------------------------------------------------
// Default OFF -- the ARM9 JIT's blast radius is the whole game, so it stays
// opt-in until A4 signs off the "is it worth it" benchmark + soak. The
// benchmark's jit9on A/B mode and any manual test build define
// DESMUME_JIT_ARM9_ON to start it enabled without touching the production
// default (the mirror of the jitoff/jiton renderer A/B for ARM7).
#ifdef DESMUME_JIT_ARM9_ON
bool jitArm9Enabled = true;
#else
bool jitArm9Enabled = false;
#endif

u32 jitRunArm9()
{
	JitCpuProfile* prof = jitProfile[JIT_ARM9];
	if (!prof) return 0;
#if !defined(JIT_DIFFERENTIAL_TESTING)
	if (!jitArm9Enabled) return 0;
#endif

	// GO-FIX-PH: cheap periodic canary poll (see jit_trace.cpp / jit.h).
	{
		static u32 s_canaryPoll = 0;
		if ((++s_canaryPoll & 0xFFFFu) == 0) jitCheckCanaries();
	}

	armcpu_t& cpu = NDS_ARM9;
	const u32 pc = cpu.instruct_adr;
	const bool thumb = (cpu.CPSR.bits.T != 0);
	const bool canEnter = thumb ? prof->canEnterThumb(pc) : prof->canEnterArm(pc);

#ifdef DESMUME_JIT_TRACE_FIRST
	g_jit9Steps++;
	if (!canEnter)   g_jit9RegionOut++;
	else if (thumb)  g_jit9ThumbOk++;
	else             g_jit9ArmOk++;
	jit9ProfileReport();
#endif

	if (!canEnter) return 0;                       // uncompilable region / ARM off

	BasicBlock* b = jitCacheArm9.getBlock(pc);
#ifdef DESMUME_JIT_TRACE_FIRST
	const bool wasMiss = !b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb;
#endif
	if (!b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb)
		b = jitCompileTrace(pc, jitCacheArm9, *prof, thumb);
	if (!b || b->execute == nullptr) return 0;

#if defined(JIT_DIFFERENTIAL_TESTING)
	return jitRunArm9Checked(&cpu, b, pc);
#endif

	cpu.R[15] = pc + (thumb ? 4 : 8);
	jit_cpu_state st = { &cpu.R[0], &cpu.CPSR.val, nullptr };

	JITResult r;
	memset(&r, 0, sizeof r);
	ExecuteJITTrace(b->execute, &r, &st);

#ifdef DESMUME_JIT_TRACE_FIRST
	{
		static u64 s_disp = 0, s_comp = 0, s_bail0 = 0, s_arm = 0, s_armbail0 = 0, s_lastRep = 0;
		s_disp++;
		if (wasMiss) s_comp++;
		if (r.instructions == 0) s_bail0++;
		if (!thumb) { s_arm++; if (r.instructions == 0) s_armbail0++; }
		static int s_dump = 0;
		if (!thumb && s_dump < 40) {
			s_dump++;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) { fprintf(f, "[jit] a9 %s pc=%08x op=%08x len=%u ins=%u bail=%u smc=%u cyc=%u npc=%08x\n",
			                 wasMiss ? "MISS" : "hit", (unsigned)pc, (unsigned)prof->fetch32(pc),
			                 (unsigned)b->insnCount(), (unsigned)r.instructions, (unsigned)r.bailedOut,
			                 (unsigned)r.smcHit, (unsigned)r.cycles, (unsigned)r.nextPC); fclose(f); }
		}
		if (s_disp - s_lastRep >= 500000) {
			s_lastRep = s_disp;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) { fprintf(f, "[jit] a9 tally: disp=%llu compiles=%llu bail0=%llu | arm disp=%llu arm bail0=%llu\n",
			                 (unsigned long long)s_disp, (unsigned long long)s_comp, (unsigned long long)s_bail0,
			                 (unsigned long long)s_arm, (unsigned long long)s_armbail0); fclose(f); }
		}
	}
#endif

	if (r.smcHit)
		jitCacheArm9.invalidateSMCTarget(r.smcAddress);

	if (r.instructions == 0) {
		if (b->insnCount() == 1) jitCacheArm9.registerBlock(pc, 1, nullptr, thumb);
		cpu.R[15] = pc + (thumb ? 4 : 8);
		return 0;
	}

	const u32 npc = r.nextPC;

#ifdef DESMUME_JIT_TRACE_FIRST
	// One-shot diagnostic: an ARM9 JIT block whose resume PC lands outside every
	// executable region -- the runaway's first observable symptom. Dump the
	// block's opcodes so the offending emitter is identifiable.
	{
		const bool sane = (npc < 0x02000000) ||
		                  ((npc & 0x0F000000u) == 0x02000000u) ||
		                  ((npc >> 24) == 0x03) ||
		                  ((npc & 0xFF000000u) == 0xFF000000u);
		static int s_badLogged = 0;
		if (!sane && s_badLogged < 12) {
			s_badLogged++;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) {
				fprintf(f, "[jit] !!! ARM9 bad resume pc=%08x npc=%08x thumb=%d ins=%u cyc=%u len=%u ops:",
				        (unsigned)pc, (unsigned)npc, (int)thumb,
				        (unsigned)r.instructions, (unsigned)r.cycles, (unsigned)b->insnCount());
				for (u32 i = 0; i < b->insnCount() && i < 34; i++)
					fprintf(f, " %08x", (unsigned)(thumb ? prof->fetch16(pc + i * 2)
					                                      : prof->fetch32(pc + i * 4)));
				fprintf(f, "\n");
				fclose(f);
			}
		}
	}
#endif

	cpu.instruct_adr = npc;
	if (cpu.CPSR.bits.T) {
		cpu.instruction      = prof->fetch16(npc & ~1u);
		cpu.next_instruction = npc + 2;
		cpu.R[15]            = npc + 4;
	} else {
		cpu.instruction      = prof->fetch32(npc & ~3u);
		cpu.next_instruction = npc + 4;
		cpu.R[15]            = npc + 8;
	}

	return r.cycles ? r.cycles : 1;
}

#endif // DESMUME_JIT_ARM7
