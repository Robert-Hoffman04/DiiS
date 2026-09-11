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
#include "../perf_zones.h"
#include "../harness/harness.h"
#include <string.h>
#include <stdio.h>
#ifdef JIT_CORE_COST_HISTO
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_microsecs() -- Step 3 exec timing
#endif

// §3.0: the JIT heap canary/minefield poll is crash-detection and only runs
// under the master harness flag (+ HARNESS_CRASH). Without it jitCheckCanaries()
// is a no-op stub and these poll sites compile away entirely.
#if defined(DESMUME_HARNESS) && defined(HARNESS_CRASH)
  #define JIT_CANARY_WATCH 1
#endif

// lightweight liveness telemetry (dumped to sd:/jit.log periodically)
u64 g_jitBlocksRun = 0;   // blocks that executed >= 1 guest instruction
u64 g_jitInsnsRun  = 0;
u64 g_jitAttempts  = 0;   // getBlock/compile attempts that reached ExecuteJITTrace
u64 g_jitBail0     = 0;   // attempts that bailed with 0 instructions
// §16: predicated ARM B/BL taken-exit paths emitted (compile-time), split by
// core. Predicated Bcc/BLcc always compile now (the -DJIT_ARM_PRED_BRANCH gate
// was removed once both CPU-correctness gates cleared); this proves coverage of
// the predicated-branch path in a differential / soak run.
u64 g_jitPredBcc7  = 0;
u64 g_jitPredBcc9  = 0;
extern u64 g_jitSmcKills; // jit_cache.cpp -- real (non-empty-bucket) SMC invalidations

// ---------------------------------------------------------------------------
// -DJIT_CORE_COST_HISTO (off by default, zero-cost when undefined): per-core
// dispatch accounting for the "ARM7 JIT execute costs as much wall time as
// ARM9 for half the guest work" investigation (jit/NOTES.md). Step 1 asks:
// is ARM7 paying the fixed per-call overhead (cache report, canary poll,
// canEnter + getBlock lookup, occasional compile) more often, for less
// forward progress per call, than ARM9?
//
// Counters are cumulative; jitCoreCostEmit() is called once per perfzones
// block (60 frames) from perf_zones.cpp with the frame number, so a window
// (f780-1200, f1200-2400) is just the delta between two emitted lines --
// same method the zone-breakdown table already uses.
// ---------------------------------------------------------------------------
#ifdef JIT_CORE_COST_HISTO
struct JitCoreCost {
	u64 calls;      // entries into jitRunArmX() (before the canEnter check)
	u64 noEnter;    // returned 0: canEnter*() said the region is uncompilable
	u64 noBlock;    // returned 0: getBlock/compile gave null or a "don't JIT"
	                //   marker (execute==nullptr) -- includes re-hits on an
	                //   already-demoted idle-loop PC
	u64 bail0;      // ran a block but it made zero forward progress
	                //   (r.instructions == 0, the bail-and-demote path)
	u64 demote1;    // of those, blocks that were length-1 terminators and got
	                //   registered as a permanent "don't JIT" marker
	u64 ran;        // executed >= 1 guest instruction
	u64 cyc;        // sum of r.cycles over the "ran" calls (guest progress)
	u64 ins;        // sum of r.instructions over the "ran" calls
	u64 entryLen[5];// Step 2: entry-block insnCount() bucket over "ran" calls
	                //   (1, 2-4, 5-8, 9-16, 17+) -- length of the block actually
	                //   dispatched, independent of compile churn
	u64 execTicks;  // Step 3: timebase ticks spent strictly inside ExecuteJITTrace()
	                //   (the trampoline + emitted block), summed over every
	                //   dispatch that ran one. zone_time - exec_time == the true
	                //   dispatch/lookup/compile overhead; exec_time / retired
	                //   insns == the true per-instruction execute cost.
	// Step 4: idle-loop demotion coverage.
	u64 markerHits; // noBlock calls that landed on a pre-existing matching-mode
	                //   length-1 "don't JIT" marker (interpreter fallback working
	                //   as intended -- no compile, just a getBlock + return)
	u64 freshNoBlk; // noBlock calls that were NOT a clean pre-existing marker
	                //   (fresh compile -> instrCount 0, or hash-collision slot,
	                //   or mode flip) -- these paid the compile scan
	u32 demoPC[4096]; u32 demoDistinct;              // distinct PCs demoted via the exec-side len-1 path
	u32 spinPC[4096]; u32 spinDistinct; u64 spinHits;// bail0 with insnCount() != 1: a block that keeps
	                //   bailing on its first instruction but the single-terminator
	                //   demotion check never fires (2-3 insn spin-wait shape)
};
static JitCoreCost g_coreCost[2];   // [0] = ARM7, [1] = ARM9

// tiny open-addressing u32 set for distinct-PC counting; returns true if newly added
static bool jccSetAdd(u32* set, u32 cap, u32 key, u32* distinct)
{
	if (!key) key = 0xFFFFFFFFu;
	u32 h = (key * 2654435761u) & (cap - 1);
	for (u32 i = 0; i < cap; i++) {
		u32 s = (h + i) & (cap - 1);
		if (set[s] == key) return false;
		if (set[s] == 0)   { set[s] = key; (*distinct)++; return true; }
	}
	return false;   // table full -- distinct count saturates, hits still counted
}

extern "C" void jitCoreCostEmit(u32 frame)
{
	for (int c = 0; c < 2; c++) {
		const JitCoreCost& x = g_coreCost[c];
		u64 wasted = x.noBlock + x.bail0;
		harness_profile_emitf(
			"jitcorecost frame=%u core=%s calls=%llu noenter=%llu noblock=%llu "
			"bail0=%llu demote1=%llu ran=%llu wasted=%llu cyc=%llu ins=%llu "
			"elen1=%llu elen2_4=%llu elen5_8=%llu elen9_16=%llu elen17=%llu",
			frame, c ? "arm9" : "arm7",
			(unsigned long long)x.calls, (unsigned long long)x.noEnter,
			(unsigned long long)x.noBlock, (unsigned long long)x.bail0,
			(unsigned long long)x.demote1, (unsigned long long)x.ran,
			(unsigned long long)wasted, (unsigned long long)x.cyc,
			(unsigned long long)x.ins,
			(unsigned long long)x.entryLen[0], (unsigned long long)x.entryLen[1],
			(unsigned long long)x.entryLen[2], (unsigned long long)x.entryLen[3],
			(unsigned long long)x.entryLen[4]);
		harness_profile_emitf("jitcorecost2 frame=%u core=%s exec_us=%llu "
			"marker_hits=%llu fresh_noblk=%llu demoted_pcs=%u spin_pcs=%u spin_hits=%llu",
			frame, c ? "arm9" : "arm7",
			(unsigned long long)ticks_to_microsecs(x.execTicks),
			(unsigned long long)x.markerHits, (unsigned long long)x.freshNoBlk,
			(unsigned)x.demoDistinct, (unsigned)x.spinDistinct,
			(unsigned long long)x.spinHits);
	}
	jitCacheArm7.ccBlockLenReport("arm7");   // Step 2: compiled-block-length distribution
	jitCacheArm9.ccBlockLenReport("arm9");
}
  #define JCC_CALL(core)          (g_coreCost[core].calls++)
  #define JCC_NOENTER(core)       (g_coreCost[core].noEnter++)
  #define JCC_NOBLOCK(core, pre)  do { JitCoreCost& _x = g_coreCost[core]; _x.noBlock++; \
        if (pre) _x.markerHits++; else _x.freshNoBlk++; } while (0)
  #define JCC_BAIL0(core, len1, pc, len) do { JitCoreCost& _x = g_coreCost[core]; _x.bail0++; \
        if (len1) { _x.demote1++; jccSetAdd(_x.demoPC, 4096, (pc), &_x.demoDistinct); } \
        else { _x.spinHits++; jccSetAdd(_x.spinPC, 4096, (pc), &_x.spinDistinct); } } while (0)
  #define JCC_RAN(core, _c, _i, _el) do { JitCoreCost& _x = g_coreCost[core]; _x.ran++; _x.cyc += (_c); _x.ins += (_i); \
        _x.entryLen[(_el) <= 1 ? 0 : (_el) <= 4 ? 1 : (_el) <= 8 ? 2 : (_el) <= 16 ? 3 : 4]++; } while (0)
  #define JCC_EXEC_BEGIN()        const u64 _jccE0 = gettime()
  #define JCC_EXEC_END(core)      (g_coreCost[core].execTicks += gettime() - _jccE0)
#else
  #define JCC_CALL(core)          ((void)0)
  #define JCC_NOENTER(core)       ((void)0)
  #define JCC_NOBLOCK(core, pre)  ((void)0)
  #define JCC_BAIL0(core, len1, pc, len) ((void)0)
  #define JCC_RAN(core, _c, _i, _el) ((void)0)
  #define JCC_EXEC_BEGIN()        ((void)0)
  #define JCC_EXEC_END(core)      ((void)0)
#endif
static void jitMaybeReport()
{
#ifdef DESMUME_JIT_TRACE_FIRST
	static u64 s_lastReport = 0;
	if (g_jitAttempts - s_lastReport < 50000) return;
	s_lastReport = g_jitAttempts;
	FILE* f = fopen("sd:/jit.log", "a");
	if (f) { fprintf(f, "[jit] alive: %llu run (%llu insns), %llu attempts, %llu bail0, %llu smcKills, arena=%u, predBcc a7=%llu a9=%llu\n",
	                 (unsigned long long)g_jitBlocksRun, (unsigned long long)g_jitInsnsRun,
	                 (unsigned long long)g_jitAttempts, (unsigned long long)g_jitBail0,
	                 (unsigned long long)g_jitSmcKills,
	                 (unsigned)jitCacheArm7.getArenaOffset(),
	                 (unsigned long long)g_jitPredBcc7, (unsigned long long)g_jitPredBcc9); fclose(f); }
#endif
}

#if defined(JIT_DIFFERENTIAL_TESTING)
#include "jit_differential.h"
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
// §3.3: periodic per-core JIT cache-pressure stat block over PKT_PROFILE. Both
// cores report (ARM7 always, ARM9 whenever its dispatcher runs). Throttled by a
// shared dispatch tick rather than a frame boundary (frames land in §3.3b).
static void jitHarnessCacheReport()
{
	static u32 s_tick = 0;
	if ((++s_tick & 0x1FFFu) != 0) return;   // ~every 8192 dispatches
	jitCacheArm7.profEmitReport("arm7");
	jitCacheArm9.profEmitReport("arm9");
}
#else
static inline void jitHarnessCacheReport() {}
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

	// perf_zones: everything below (dispatch, compile, trampoline, resume) is
	// ARM7 JIT time; jitCompileTrace() re-tags its own interval as ARM7_BUILD.
	PZ_SCOPE(PZ_ARM7_JIT);

	jitHarnessCacheReport();   // §3.3: throttled internally

	// GO-FIX-PH: cheap periodic canary poll (see jit_trace.cpp / jit.h).
#ifdef JIT_CANARY_WATCH
	{
		static u32 s_canaryPoll7 = 0;
#ifdef JIT_HEAP_WATCH
		if ((++s_canaryPoll7 & 0x3FFu) == 0) jitCheckCanaries();
#else
		if ((++s_canaryPoll7 & 0xFFFFu) == 0) jitCheckCanaries();
#endif
	}
#endif

	armcpu_t& cpu = NDS_ARM7;
	const u32 pc = cpu.instruct_adr;
	const bool thumb = (cpu.CPSR.bits.T != 0);     // P11: ARM mode is JITted too
	const bool canEnter = thumb ? prof->canEnterThumb(pc) : prof->canEnterArm(pc);
	JCC_CALL(0);
	if (!canEnter) { JCC_NOENTER(0); return 0; }   // uncompilable region -> interpreter

	BasicBlock* b = jitCacheArm7.getBlock(pc);
#ifdef JIT_CORE_COST_HISTO
	const bool _preMarker7 = (b && b->execute == nullptr && b->insnCount() == 1 && b->thumbCompiled() == thumb);
#endif
	if (!b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb)
		b = jitCompileTrace(pc, jitCacheArm7, *prof, thumb);
	if (!b || b->execute == nullptr) { JCC_NOBLOCK(0, _preMarker7); return 0; } // uncompilable / "don't JIT" -> interpreter

#if defined(JIT_DIFFERENTIAL_TESTING)
	return jitRunArm7Checked(&cpu, b, pc);
#endif

	cpu.R[15] = pc + (thumb ? 4 : 8);             // pipeline offset the block expects
	jit_cpu_state st = { &cpu.R[0], &cpu.CPSR.val, nullptr };

	JITResult r;
	memset(&r, 0, sizeof r);
	JCC_EXEC_BEGIN();
	ExecuteJITTrace(b->execute, &r, &st);
	JCC_EXEC_END(0);
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
		const bool len1 = (b->insnCount() == 1);
		if (len1) jitCacheArm7.registerBlock(pc, 1, nullptr, thumb);
		JCC_BAIL0(0, len1, pc, b->insnCount());
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
	JCC_RAN(0, r.cycles, r.instructions, b->insnCount());
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

	// perf_zones: dispatch + compile + trampoline + resume-pipeline is ARM9 JIT
	// time; jitCompileTrace() re-tags its own interval as ARM9_BUILD.
	PZ_SCOPE(PZ_ARM9_JIT);

	jitHarnessCacheReport();   // §3.3: throttled internally

	// GO-FIX-PH: cheap periodic canary poll (see jit_trace.cpp / jit.h).
	// JIT_HEAP_WATCH tightens the interval from 64K to 1K dispatches for §16
	// host-memory-safety soaks (predicated-branch corruption hunt).
#ifdef JIT_CANARY_WATCH
	{
		static u32 s_canaryPoll = 0;
#ifdef JIT_HEAP_WATCH
		if ((++s_canaryPoll & 0x3FFu) == 0) jitCheckCanaries();
#else
		if ((++s_canaryPoll & 0xFFFFu) == 0) jitCheckCanaries();
#endif
	}
#endif

	armcpu_t& cpu = NDS_ARM9;
	const u32 pc = cpu.instruct_adr;
	const bool thumb = (cpu.CPSR.bits.T != 0);
	const bool canEnter = thumb ? prof->canEnterThumb(pc) : prof->canEnterArm(pc);
	JCC_CALL(1);

#ifdef DESMUME_JIT_TRACE_FIRST
	g_jit9Steps++;
	if (!canEnter)   g_jit9RegionOut++;
	else if (thumb)  g_jit9ThumbOk++;
	else             g_jit9ArmOk++;
	jit9ProfileReport();
#endif

	if (!canEnter) { JCC_NOENTER(1); return 0; }   // uncompilable region / ARM off

	BasicBlock* b = jitCacheArm9.getBlock(pc);
#ifdef DESMUME_JIT_TRACE_FIRST
	const bool wasMiss = !b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb;
#endif
#ifdef JIT_CORE_COST_HISTO
	const bool _preMarker9 = (b && b->execute == nullptr && b->insnCount() == 1 && b->thumbCompiled() == thumb);
#endif
	if (!b || (b->execute == nullptr && b->insnCount() == 0) || b->thumbCompiled() != thumb)
		b = jitCompileTrace(pc, jitCacheArm9, *prof, thumb);
	if (!b || b->execute == nullptr) { JCC_NOBLOCK(1, _preMarker9); return 0; }

#if defined(JIT_DIFFERENTIAL_TESTING)
	return jitRunArm9Checked(&cpu, b, pc);
#endif

	cpu.R[15] = pc + (thumb ? 4 : 8);
	jit_cpu_state st = { &cpu.R[0], &cpu.CPSR.val, nullptr };

	JITResult r;
	memset(&r, 0, sizeof r);
	JCC_EXEC_BEGIN();
	ExecuteJITTrace(b->execute, &r, &st);
	JCC_EXEC_END(1);

#ifdef DESMUME_JIT_TRACE_FIRST
	{
		static u64 s_disp = 0, s_comp = 0, s_bail0 = 0, s_arm = 0, s_armbail0 = 0, s_lastRep = 0;
		static u64 s_insns = 0, s_entries = 0, s_blk0 = 0;
		static u64 s_edge = 0, s_slotResident = 0, s_slotOther = 0, s_slotEmpty = 0,
		          s_slotDontJit = 0, s_slotSmc = 0, s_yield = 0, s_edgeShort = 0;
		s_disp++;
		if (wasMiss) s_comp++;
		if (r.instructions == 0) s_bail0++;
		if (!thumb) { s_arm++; if (r.instructions == 0) s_armbail0++; }
		// chain-length picture: r.instructions is the whole chained-dispatch's
		// guest-instruction count (the trampoline accumulates it in r31 across
		// every statically-chained block); b->insnCount() is just the entry
		// block. s_insns/s_entries == guest instructions per trampoline
		// round-trip; s_insns/s_blk0 == blocks per round-trip (approx chain len).
		if (r.instructions != 0) { s_insns += r.instructions; s_entries++; s_blk0 += b->insnCount(); }
		// Why did this non-bail chain exit pay a full trampoline round-trip
		// instead of chaining on? Classify by what sits in the resume PC's
		// direct-mapped block-table slot:
		//   resident -> a valid same-PC block is right there; the dispatcher
		//               should have chained in (guard too strict / self-patch
		//               not sticking) -- cheap to fix.
		//   other    -> a DIFFERENT PC occupies the slot: hash collision evicted
		//               our target -> set-associative block table.
		//   empty    -> slot never populated / flushed: genuine first-visit /
		//               arena churn.
		//   dontJIT  -> same PC, execute==null, len==1: the scanner found
		//               nothing compilable there (predicated LDR/STR/POP/BX/MUL,
		//               CP15, ...) so it is interpreted one instr at a time --
		//               the real ARM9 ceiling. Fix = widen emitter coverage.
		//   smc      -> same PC, execute==null, len==0: SMC-killed, recompile due.
		if (r.instructions != 0 && !r.bailedOut && !r.smcHit) {
			s_edge++;
			if (r.cycles >= JIT_YIELD_NUMBER) s_yield++;
			if (r.instructions < 4)           s_edgeShort++;
			{
				u32 tpc = r.nextPC & ~1u;
				u32 idx = jitHashPC(tpc);
				const BasicBlock& sl = jitCacheArm9.debugSlot(idx);
				if      (sl.startPC == 0)      s_slotEmpty++;
				else if (sl.startPC != tpc)    s_slotOther++;
				else if (sl.execute != 0)      s_slotResident++;
				else if (sl.insnCount() == 1)  s_slotDontJit++;
				else                           s_slotSmc++;
			}
		}
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
			if (f) { fprintf(f, "[jit] a9 tally: disp=%llu compiles=%llu bail0=%llu | arm disp=%llu arm bail0=%llu"
			                 " | ins/entry=%llu.%02llu blk0/entry=%llu.%02llu (entries=%llu)\n",
			                 (unsigned long long)s_disp, (unsigned long long)s_comp, (unsigned long long)s_bail0,
			                 (unsigned long long)s_arm, (unsigned long long)s_armbail0,
			                 (unsigned long long)(s_entries ? s_insns / s_entries : 0),
			                 (unsigned long long)(s_entries ? (s_insns * 100 / s_entries) % 100 : 0),
			                 (unsigned long long)(s_entries ? s_insns / (s_blk0 ? s_blk0 : 1) : 0),
			                 (unsigned long long)(s_entries ? (s_insns * 100 / (s_blk0 ? s_blk0 : 1)) % 100 : 0),
			                 (unsigned long long)s_entries); fclose(f); }
			f = fopen("sd:/jit.log", "a");
			if (f) { fprintf(f, "[jit] a9 edge: edge=%llu (%llu%% of disp) | slot: resident=%llu other=%llu"
			                 " empty=%llu dontJIT=%llu smc=%llu | short(<4)=%llu quota=%llu\n",
			                 (unsigned long long)s_edge,
			                 (unsigned long long)(s_disp ? s_edge * 100 / s_disp : 0),
			                 (unsigned long long)s_slotResident, (unsigned long long)s_slotOther,
			                 (unsigned long long)s_slotEmpty, (unsigned long long)s_slotDontJit,
			                 (unsigned long long)s_slotSmc,
			                 (unsigned long long)s_edgeShort, (unsigned long long)s_yield);
			         fclose(f); }
		}
	}
#endif

	if (r.smcHit)
		jitCacheArm9.invalidateSMCTarget(r.smcAddress);

	if (r.instructions == 0) {
		const bool len1 = (b->insnCount() == 1);
		if (len1) jitCacheArm9.registerBlock(pc, 1, nullptr, thumb);
		JCC_BAIL0(1, len1, pc, b->insnCount());
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

	JCC_RAN(1, r.cycles, r.instructions, b->insnCount());
	return r.cycles ? r.cycles : 1;
}

#endif // DESMUME_JIT_ARM7
