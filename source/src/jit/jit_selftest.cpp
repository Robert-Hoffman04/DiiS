/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_selftest.cpp
 *
 * jitSelfTest(): hand-emit one minimal block into the arena and run it through
 * the real path -- ExecuteJITTrace (jit_trampoline.S) -> block body -> the
 * shared linker-stub return target -> ExecuteJITTrace_Return -> back to C --
 * then check the JITResult came back intact. This is the P1 exit criterion:
 * proof the trampoline ABI, arena, cache and linker stub round-trip on the
 * actual Broadway, before any opcode emitter exists.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include <stdio.h>
#include <string.h>
#include <ogc/cache.h>
#include "jit_ppc_emitter.h"

bool jitSelfTest()
{
	if (!jitProfile[JIT_ARM7]) {
		printf("[jit] selftest: no active profile (jitInit failed?)\n");
		return false;
	}

	const u32 kCycles = 0x1234;
	const u32 kNextPC = 0x02001abc;
	const u32 kInsns  = 3;

	const size_t before = jitCacheArm7.getArenaOffset();
	u32* code = jitCacheArm7.allocateJITMemory(64);
	if (!code) { printf("[jit] selftest: arena alloc failed\n"); return false; }

	u32* p = code;
	// r3 = cycles sentinel
	*p++ = PPC_LIS(PPC_R3, kCycles >> 16);
	*p++ = PPC_ORI(PPC_R3, PPC_R3, kCycles & 0xFFFF);
	// r4 = next-PC sentinel
	*p++ = PPC_LIS(PPC_R4, kNextPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, kNextPC & 0xFFFF);
	// P12: guest-instruction count is the resident r31 accumulator, stored back
	// to out->instructions by the trampoline on return.
	*p++ = PPC_LI(PPC_R31, kInsns);
	// out = 88(r1): bailedOut@12, smcHit@16, smcAddress@20
	*p++ = PPC_LWZ(PPC_R10, 1, 88);
	*p++ = PPC_LI(PPC_R11, 0);
	*p++ = PPC_STW(PPC_R11, PPC_R10, 12);
	*p++ = PPC_STW(PPC_R11, PPC_R10, 16);
	*p++ = PPC_STW(PPC_R11, PPC_R10, 20);
	// branch to the linker stub's shared far-return sequence (in-arena, so a
	// relative branch reaches it even though ExecuteJITTrace_Return itself may
	// be >32 MB away in .text)
	{
		s32 off = (s32)((u8*)jitCacheArm7.linkerReturnAddress - (u8*)p);
		*p++ = PPC_B(off);
	}

	const u32 bytes = (u32)((u8*)p - (u8*)code);
	DCStoreRange(code, bytes);
	ICInvalidateRange(code, bytes);

	// GPR residency: the trampoline lmw/stmw spans gpr[0..17] (R0..R15, then CPSR
	// at [16] and SPSR at [17], contiguous in armcpu_t). Mirror that layout here
	// so the bulk load/store stays in bounds.
	u32 gpr[18];
	memset(gpr, 0, sizeof(gpr));
	gpr[15] = 0x02000000;
	gpr[16] = 0x0000001F;    // CPSR: SYS mode, flags clear
	jit_cpu_state st = { gpr, &gpr[16], nullptr };

	JITResult r;
	memset(&r, 0, sizeof(r));
	ExecuteJITTrace((JITBlockFunc)code, &r, &st);

	const bool ok = (r.cycles == kCycles) && (r.nextPC == kNextPC) &&
	                (r.instructions == kInsns) && (r.bailedOut == 0);

	char line[192];
	snprintf(line, sizeof(line),
	         "[jit] selftest %s: cycles=%#x/%#x nextPC=%#x/%#x insns=%u/%u bail=%u arena=%u/%u\n",
	         ok ? "PASS" : "FAIL",
	         (unsigned)r.cycles, (unsigned)kCycles,
	         (unsigned)r.nextPC, (unsigned)kNextPC,
	         (unsigned)r.instructions, (unsigned)kInsns, (unsigned)r.bailedOut,
	         (unsigned)jitCacheArm7.getArenaOffset(), (unsigned)JIT_ARENA_SIZE);
	printf("%s", line);
	FILE* f = fopen("sd:/jit.log", "a");
	if (f) { fputs(line, f); fclose(f); }

	// this was scratch: give the arena space back
	jitCacheArm7.rewindJITMemory(jitCacheArm7.getArenaOffset() - before);
	return ok;
}

#endif // DESMUME_JIT_ARM7
