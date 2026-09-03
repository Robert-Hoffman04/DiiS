/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_trace.cpp
 *
 * Owns the JIT's backing memory (code arena + block hash table + SMC tables),
 * the lifecycle entry points (jitInit / jitShutdown), and the front-end-
 * agnostic jitCompileTrace() dispatch.
 *
 * P1: jitCompileTrace() has nothing to dispatch to yet, so it caches a
 * "don't JIT this" fallback for every PC -- execution is byte-for-byte the
 * interpreter. The plumbing (arena alloc, cache init against the ARM7
 * JitCpuProfile, linker stub emission) is live and is what jitSelfTest()
 * exercises.
 ***************************************************************************/

#include "jit_trace.h"

#if defined(DESMUME_JIT_ARM7)

#include <malloc.h>
#include <string.h>

JitCpuProfile* jitActiveProfile = nullptr;

// Built by jit_arm7_profile.cpp.
extern JitCpuProfile* jitBuildArm7Profile();

static u32*         s_arena        = nullptr;
static BasicBlock*  s_blockTable   = nullptr;
static BasicBlock** s_smcRegistry  = nullptr;
static u8*          s_smcPageFlags = nullptr;

void jitShutdown()
{
	jitCache.destroy();
	free(s_arena);        s_arena = nullptr;
	free(s_blockTable);   s_blockTable = nullptr;
	free(s_smcRegistry);  s_smcRegistry = nullptr;
	free(s_smcPageFlags); s_smcPageFlags = nullptr;
	jitActiveProfile = nullptr;
}

void jitInit()
{
	if (jitActiveProfile) return;

	JitCpuProfile* profile = jitBuildArm7Profile();

	s_arena        = (u32*)        memalign(32, JIT_ARENA_SIZE);
	s_blockTable   = (BasicBlock*) memalign(16, HASH_TABLE_SIZE * sizeof(BasicBlock));
	s_smcRegistry  = (BasicBlock**)memalign(32, SMC_MAP_SIZE * sizeof(BasicBlock*));
	s_smcPageFlags = (u8*)         memalign(32, SMC_MAP_SIZE);

	if (!s_arena || !s_blockTable || !s_smcRegistry || !s_smcPageFlags) {
		jitShutdown();
		return;
	}

	jitCache.initialize(s_arena, s_blockTable, s_smcRegistry, s_smcPageFlags,
	                    profile->smcBankMask);
	jitActiveProfile = profile;
}

BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu)
{
	(void)cpu;
	// P1: front-end not implemented -- register a length-1 fallback marker so
	// getBlock() returns a block whose execute == nullptr (interpreter path)
	// instead of a cache miss that would re-enter the compiler every fetch.
	return cache.registerBlock(startPC, 1, nullptr);
}

#endif // DESMUME_JIT_ARM7
