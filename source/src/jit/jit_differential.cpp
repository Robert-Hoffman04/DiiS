/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_differential.cpp  -- see jit_differential.h
 *
 * Derived from VBA-GX's JITDifferential.cpp (c) Daryl Borth, GPL v2+.
 ***************************************************************************/

#include "jit_differential.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

#include "jit_trace.h"
#include "../armcpu.h"
#include <string.h>
#include <stdio.h>

// s_mismatches (capped) gates the expensive per-mismatch sd:/jit.log write --
// SD writes are slow and a real divergence can repeat millions of times.
// s_mismatchesTotal is the true, uncapped count used in the periodic report;
// it used to not exist, so the report silently froze at JIT_DIFF_MAX_LOGS
// once the cap was hit, making it look like divergence had stopped when it
// may not have.
static int   s_mismatches      = 0;
static u64   s_mismatchesTotal = 0;
static const int JIT_DIFF_MAX_LOGS = 200;

extern u64 g_jitBlocksRun, g_jitInsnsRun;

// The interpreter "reference" run below executes through the REAL
// armcpu_exec<ARMCPU_ARM7>(), against the actual shared guest memory -- it is
// not sandboxed (armcpu_t holds only CPU registers, not RAM, so save/restore
// around it only rewinds registers, never memory). If the block contains a
// store, that reference run performs a real, permanent memory write; the JIT
// then runs "for real" afterward and reads memory the reference pass already
// mutated once. For any code that reads back what it just wrote (a counter, a
// PRNG seed, anything self-referential -- extremely common), this
// double-applies the update: once from the disposable reference pass, once
// for real. That showed up as a deterministic, reproducible register
// mismatch (confirmed via an opcode dump: the only register that ever
// diverged was fed by a plain memory load, and the ADC/SBC/flag-carry
// emitters it depends on already have dedicated, passing selftest coverage)
// that looked like a JIT correctness bug but wasn't one -- it was this
// harness corrupting shared state before the JIT even ran. It cannot happen
// in a normal (non-JIT_DIFFERENTIAL_TESTING) build: jitRunArm7() there runs
// the JIT once, with no shadow interpreter pass at all.
//
// The correct fix is to stop the reference pass from touching real memory,
// which needs guest-memory snapshotting this harness doesn't have. Sandboxing
// that is out of scope here, and routing store-containing blocks around the
// comparison entirely (tried first) changes how often their memory side
// effects apply per real dispatch and, for at least one long busy-wait loop
// in the boot path, made real-time progress collapse by ~1000x without
// actually hanging -- a correctness-neutral but unresolved performance trap
// not worth shipping. So instead: keep the existing (proven-stable, ~200k
// dispatches/sec) execution path completely unchanged, and only suppress
// trusting/logging a comparison as a real mismatch when the block contains a
// store -- those are exactly the ones subject to the double-apply artifact.
// Blocks without stores are unaffected by any of this and their mismatches
// (if any) are still fully reported.
static bool blockHasMemoryStoreScan(u32 pc, u32 length)
{
	for (u32 i = 0; i < length; i++) {
		u16 op = (u16)jitProfile[JIT_ARM7]->fetch16(pc + i * 2);
		if ((op & 0xFE00) == 0xB400) return true;                        // PUSH
		if ((op & 0xF800) == 0xC000) return true;                        // STMIA
		if ((op & 0xF000) == 0x5000 && (op & 0x0E00) <= 0x0400) return true; // STR/STRB/STRH, reg offset
		if ((op & 0xF800) == 0x6000) return true;                        // STR, imm offset
		if ((op & 0xF800) == 0x7000) return true;                        // STRB, imm offset
		if ((op & 0xF800) == 0x8000) return true;                        // STRH, imm offset
		if ((op & 0xF800) == 0x9000) return true;                        // STR, SP-relative
	}
	return false;
}

// Direct-mapped, tagged cache so the classification (up to 32 fetch16() calls)
// is amortized to once per distinct PC instead of once per dispatch of a hot
// block -- a collision just re-scans (never a correctness issue).
struct StoreCacheEntry { u32 pc; bool valid; bool hasStore; };
static StoreCacheEntry s_storeCache[16384];

static bool blockHasMemoryStore(u32 pc, u32 length)
{
	StoreCacheEntry& e = s_storeCache[(pc >> 1) & 16383];
	if (e.valid && e.pc == pc) return e.hasStore;
	bool has = blockHasMemoryStoreScan(pc, length);
	e.pc = pc; e.valid = true; e.hasStore = has;
	return has;
}

u32 jitRunArm7Checked(armcpu_t* cpu, BasicBlock* block, u32 pc)
{
	const armcpu_t save = *cpu;

	// ---- interpreter reference: up to block->length steps ----
	cpu->R[15] = pc + 4;
	cpu->instruct_adr     = pc;
	cpu->instruction      = jitProfile[JIT_ARM7]->fetch16(pc & ~1u);
	cpu->next_instruction = pc + 2;

	u32 istep = 0;
	while (istep < block->length && cpu->CPSR.bits.T && !cpu->waitIRQ) {
		u32 curPC = cpu->instruct_adr;
		armcpu_exec<ARMCPU_ARM7>();
		istep++;
		// a taken branch / mode switch ends the comparable run
		if (cpu->instruct_adr != curPC + 2) break;
	}
	u32 iR[16];
	memcpy(iR, cpu->R, sizeof iR);
	const u32 iCPSR = cpu->CPSR.val;
	const u32 iPC   = cpu->instruct_adr;

	// ---- restore, then run the JIT block for real ----
	*cpu = save;
	cpu->R[15] = pc + 4;
	jit_cpu_state st = { &cpu->R[0], &cpu->CPSR.val, nullptr };
	JITResult r;
	memset(&r, 0, sizeof r);
	ExecuteJITTrace(block->execute, &r, &st);
	if (r.smcHit) jitCacheArm7.invalidateSMCTarget(r.smcAddress);

	if (r.instructions == 0) {
		// JIT made no progress -- restore and let the interpreter step once
		*cpu = save;
		cpu->R[15] = pc + 4;
		u32 c = armcpu_exec<ARMCPU_ARM7>();
		return c ? c : 1;
	}

	// See jit_exec.cpp's identical fix: POP{...,PC} can switch to ARM mode
	// and clears CPSR.T itself before returning here -- check it rather than
	// assuming THUMB.
	const u32 npc = r.nextPC;
	cpu->instruct_adr = npc;
	if (cpu->CPSR.bits.T) {
		cpu->instruction      = jitProfile[JIT_ARM7]->fetch16(npc & ~1u);
		cpu->next_instruction = npc + 2;
		cpu->R[15]            = npc + 4;
	} else {
		cpu->instruction      = jitProfile[JIT_ARM7]->fetch32(npc & ~3u);
		cpu->next_instruction = npc + 4;
		cpu->R[15]            = npc + 8;
	}

	// ---- compare (only when both ran the same number of instructions, and
	// the block has no store -- see the double-apply note above) ----
	// Always compute the diff (cheap: <=15 word compares) so s_mismatchesTotal
	// stays accurate; only the detailed sd:/jit.log write is capped.
	if (r.instructions == istep && !r.bailedOut && !blockHasMemoryStore(pc, block->length)) {
		char d[256]; size_t k = 0; d[0] = 0;
		for (int i = 0; i < 15; i++)
			if (cpu->R[i] != iR[i] && k + 32 < sizeof d)
				k += snprintf(d + k, sizeof d - k, " R%d j=%08x i=%08x", i, cpu->R[i], iR[i]);
		if ((cpu->CPSR.val & 0xF0000000u) != (iCPSR & 0xF0000000u) && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " NZCV j=%x i=%x", cpu->CPSR.val >> 28, iCPSR >> 28);
		if (npc != iPC && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " PC j=%08x i=%08x", npc, iPC);
		if (d[0]) {
			s_mismatchesTotal++;
			if (s_mismatches < JIT_DIFF_MAX_LOGS) {
				s_mismatches++;
				FILE* f = fopen("sd:/jit.log", "a");
				if (f) {
					fprintf(f, "[jit] DIFF @%08x len=%u ins=%u:%s\n",
					        pc, (unsigned)block->length, (unsigned)r.instructions, d);
					// One-time opcode dump per distinct offending PC -- lets a
					// mismatch be root-caused from the log alone instead of
					// needing a live debugger session.
					static u32 s_lastDumpPC = 0xFFFFFFFFu;
					if (pc != s_lastDumpPC) {
						s_lastDumpPC = pc;
						fprintf(f, "[jit]   opcodes:");
						for (u32 i = 0; i < block->length; i++)
							fprintf(f, " %04x", (unsigned)jitProfile[JIT_ARM7]->fetch16(pc + i * 2));
						fprintf(f, "\n");
					}
					fclose(f);
				}
			}
		}
	}

	g_jitBlocksRun++;
	g_jitInsnsRun += r.instructions;
	static u64 lastReport = 0;
	if (g_jitBlocksRun - lastReport >= 100000) {
		lastReport = g_jitBlocksRun;
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) { fprintf(f, "[jit] diff alive: %llu blocks, %llu insns, %llu mismatches (%d logged)\n",
		                 (unsigned long long)g_jitBlocksRun, (unsigned long long)g_jitInsnsRun,
		                 (unsigned long long)s_mismatchesTotal, s_mismatches); fclose(f); }
	}

	return r.cycles ? r.cycles : 1;
}

#endif // DESMUME_JIT_ARM7 && JIT_DIFFERENTIAL_TESTING
