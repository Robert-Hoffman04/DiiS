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

static int   s_mismatches = 0;
static const int JIT_DIFF_MAX_LOGS = 200;

extern u64 g_jitBlocksRun, g_jitInsnsRun;

u32 jitRunArm7Checked(armcpu_t* cpu, BasicBlock* block, u32 pc)
{
	const armcpu_t save = *cpu;

	// ---- interpreter reference: up to block->length steps ----
	cpu->R[15] = pc + 4;
	cpu->instruct_adr     = pc;
	cpu->instruction      = jitActiveProfile->fetch16(pc & ~1u);
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
	if (r.smcHit) jitCache.invalidateSMCTarget(r.smcAddress);

	if (r.smcHit && r.instructions == 0) { /* invalidate handled above */ }
	if (r.instructions == 0) {
		// JIT made no progress -- restore and let the interpreter step once
		*cpu = save;
		cpu->R[15] = pc + 4;
		u32 c = armcpu_exec<ARMCPU_ARM7>();
		return c ? c : 1;
	}

	const u32 npc = r.nextPC;
	cpu->instruct_adr     = npc;
	cpu->instruction      = jitActiveProfile->fetch16(npc & ~1u);
	cpu->next_instruction = npc + 2;
	cpu->R[15]            = npc + 4;

	// ---- compare (only when both ran the same number of instructions) ----
	if (r.instructions == istep && !r.bailedOut && s_mismatches < JIT_DIFF_MAX_LOGS) {
		char d[256]; size_t k = 0; d[0] = 0;
		for (int i = 0; i < 15; i++)
			if (cpu->R[i] != iR[i] && k + 32 < sizeof d)
				k += snprintf(d + k, sizeof d - k, " R%d j=%08x i=%08x", i, cpu->R[i], iR[i]);
		if ((cpu->CPSR.val & 0xF0000000u) != (iCPSR & 0xF0000000u) && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " NZCV j=%x i=%x", cpu->CPSR.val >> 28, iCPSR >> 28);
		if (npc != iPC && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " PC j=%08x i=%08x", npc, iPC);
		if (d[0]) {
			s_mismatches++;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) {
				fprintf(f, "[jit] DIFF @%08x len=%u ins=%u:%s\n",
				        pc, (unsigned)block->length, (unsigned)r.instructions, d);
				fclose(f);
			}
		}
	}

	g_jitBlocksRun++;
	g_jitInsnsRun += r.instructions;
	static u64 lastReport = 0;
	if (g_jitBlocksRun - lastReport >= 100000) {
		lastReport = g_jitBlocksRun;
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) { fprintf(f, "[jit] diff alive: %llu blocks, %llu insns, %d mismatches\n",
		                 (unsigned long long)g_jitBlocksRun, (unsigned long long)g_jitInsnsRun,
		                 s_mismatches); fclose(f); }
	}

	return r.cycles ? r.cycles : 1;
}

#endif // DESMUME_JIT_ARM7 && JIT_DIFFERENTIAL_TESTING
