/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_trace.cpp -- see jit_trace.h
 *
 * Owns the JIT backing memory + lifecycle, the shared JitTraceCtx helpers
 * (register allocator, packed-flag plumbing, exit metadata) and the trace
 * scanner / block epilogue. Derived from VBA-GX's JITCompiler.cpp
 * (c) Daryl Borth, GPL v2+ -- see jit/upstream/PROVENANCE.md.
 ***************************************************************************/

#include "jit_trace.h"

#if defined(DESMUME_JIT_ARM7)

#include <malloc.h>
#include <string.h>
#include <ogc/cache.h>

// =========================================================================
// Lifecycle
// =========================================================================
JitCpuProfile* jitActiveProfile = nullptr;

extern JitCpuProfile* jitBuildArm7Profile();   // jit_arm7_profile.cpp

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

// =========================================================================
// JitTraceCtx helpers
// =========================================================================
void JitTraceCtx::ensureArena()
{
	if (arenaAllocated) return;
	arenaAllocated = true;

	arenaOffsetStart = cache.getArenaOffset();
	emitPtr = cache.allocateJITMemory(JIT_MAX_WORDS * sizeof(u32));
	blockStart = emitPtr;

	// Event quota shield: on entry r3 is the accumulated cycle count across any
	// chained blocks; bail to the yield stub once it crosses the threshold.
	*emitPtr++ = PPC_CMPWI(0, PPC_R3, JIT_YIELD_NUMBER);
	quotaGuard = emitPtr;
	*emitPtr++ = PPC_BGE(0);
}

// ---- packed flags: PPC_REG_FLAGS (r6) holds the whole guest CPSR word -----
void JitTraceCtx::ensureFlagsLoaded()
{
	if (flagsLoaded) return;
	*emitPtr++ = PPC_LWZ(PPC_R9, 1, 84);              // r9 = &CPSR
	*emitPtr++ = PPC_LWZ(PPC_REG_FLAGS, PPC_R9, 0);   // r6 = CPSR
	flagsLoaded = true;
}

void JitTraceCtx::emitFlagBit(u8 targetBit, u32 srcReg, u8 sh)
{
	ensureFlagsLoaded();
	*emitPtr++ = PPC_MERGE_FLAG_BIT(targetBit, srcReg, sh);
	flagsDirty = true;
}

void JitTraceCtx::emitFlagConst(u8 targetBit, bool value)
{
	ensureFlagsLoaded();
	*emitPtr++ = PPC_LI(PPC_R8, value ? 1 : 0);
	*emitPtr++ = PPC_MERGE_FLAG_BIT(targetBit, PPC_R8, 0);
	flagsDirty = true;
}

u8 JitTraceCtx::readFlag(u8 flagIdx, u32 dstReg)
{
	ensureFlagsLoaded();
	*emitPtr++ = PPC_EXTRACT_FLAG_BIT(dstReg, flagIdx);
	return (u8)dstReg;
}

void JitTraceCtx::flushDirtyFlags()
{
	if (!flagsDirty) return;
	*emitPtr++ = PPC_LWZ(PPC_R9, 1, 84);
	*emitPtr++ = PPC_STW(PPC_REG_FLAGS, PPC_R9, 0);
	flagsDirty = false;
}

void JitTraceCtx::emitDirtyFlagFlush()
{
	if (!flagsDirty) return;
	*emitPtr++ = PPC_LWZ(PPC_R9, 1, 84);
	*emitPtr++ = PPC_STW(PPC_REG_FLAGS, PPC_R9, 0);
}

void JitTraceCtx::emitNZ(u32 srcReg)
{
	emitFlagBit(JITF_N, srcReg, 1);
	*emitPtr++ = PPC_CNTLZW(PPC_R8, srcReg);
	emitFlagBit(JITF_Z, PPC_R8, 27);
}

void JitTraceCtx::emitCVfromXER(u32 scratchReg)
{
	*emitPtr++ = PPC_MFXER(scratchReg);
	emitFlagBit(JITF_C, scratchReg, 3);
	emitFlagBit(JITF_V, scratchReg, 2);
}

// ---- lazy host-register allocator: guest R0..R14 -> host r15..r28 --------
u8 JitTraceCtx::allocHostReg(u8 gbaReg, bool loadFromMem, u32& lockedMask)
{
	if (gbaReg == 15) return PPC_R29;   // guest PC is hardwired to r29

	if (regCache[gbaReg].allocated) {
		regCache[gbaReg].age = ++currentAge;
		lockedMask |= (1u << regCache[gbaReg].hostReg);
		return regCache[gbaReg].hostReg;
	}

	u32 inUseMask = allocatedHostRegsMask | lockedMask;
	u32 freeMask  = (~inUseMask) & 0x1FFF8000u;   // host r15..r28
	u8  freeReg   = 0;

	if (freeMask != 0) {
		freeReg = (u8)(31 - __builtin_clz(freeMask));
	} else {
		u32 oldestAge = 0xFFFFFFFFu;
		int spillTarget = -1;
		for (int i = 0; i < 15; i++) {
			if (regCache[i].allocated &&
			    ((lockedMask & (1u << regCache[i].hostReg)) == 0) &&
			    regCache[i].age < oldestAge) {
				oldestAge = regCache[i].age;
				spillTarget = i;
			}
		}
		if (regCache[spillTarget].dirty)
			*emitPtr++ = PPC_STW(regCache[spillTarget].hostReg, 14, spillTarget * 4);
		regCache[spillTarget].allocated = false;
		regCache[spillTarget].dirty = false;
		allocatedHostRegsMask &= ~(1u << regCache[spillTarget].hostReg);
		freeReg = regCache[spillTarget].hostReg;
	}

	regCache[gbaReg].allocated = true;
	regCache[gbaReg].dirty = false;
	regCache[gbaReg].hostReg = freeReg;
	regCache[gbaReg].age = ++currentAge;

	allocatedHostRegsMask |= (1u << freeReg);
	lockedMask            |= (1u << freeReg);

	if (loadFromMem) *emitPtr++ = PPC_LWZ(freeReg, 14, gbaReg * 4);
	return freeReg;
}

u8 JitTraceCtx::writeReg(u8 gbaReg, bool fullOverwrite, u32& lockedMask)
{
	u8 h = allocHostReg(gbaReg, !fullOverwrite, lockedMask);
	if (gbaReg < 15) regCache[gbaReg].dirty = true;
	return h;
}

void JitTraceCtx::flushDirtyRegisters()
{
	for (int i = 0; i < 15; i++) {
		if (regCache[i].allocated && regCache[i].dirty) {
			*emitPtr++ = PPC_STW(regCache[i].hostReg, 14, i * 4);
			regCache[i].dirty = false;
		}
	}
}

void JitTraceCtx::emitDirtyRegisterFlush()
{
	for (int i = 0; i < 15; i++)
		if (regCache[i].allocated && regCache[i].dirty)
			*emitPtr++ = PPC_STW(regCache[i].hostReg, 14, i * 4);
}

void JitTraceCtx::emitEagerFlush()
{
	flushDirtyFlags();
	flushDirtyRegisters();
}

void JitTraceCtx::emitResultMetadata(u32 count, u32 bailedOut, u32 smcHit)
{
	*emitPtr++ = PPC_LWZ(PPC_R10, 1, 88);           // outResult*
	*emitPtr++ = PPC_LWZ(PPC_R11, PPC_R10, 8);
	*emitPtr++ = PPC_ADDI(PPC_R11, PPC_R11, count);
	*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 8);      // instructions += count
	*emitPtr++ = PPC_LI(PPC_R11, bailedOut);
	*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 12);     // bailedOut
	*emitPtr++ = PPC_LI(PPC_R11, smcHit);
	*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 16);     // smcHit
}

void JitTraceCtx::emitAddCycles(u32 n)
{
	if (n) *emitPtr++ = PPC_ADDI(PPC_R3, PPC_R3, (s32)n);
}

void JitTraceCtx::registerBailout(u32* branchPtr, JitBailoutCond cond)
{
	if (bailoutCount >= JIT_MAX_BAILOUTS) {
		*branchPtr = 0x7FE00008;   // trap
		endBlock = true;
		return;
	}
	bailouts[bailoutCount].branchPtr    = branchPtr;
	bailouts[bailoutCount].cond         = cond;
	bailouts[bailoutCount].pc           = currentPC;
	bailouts[bailoutCount].cycles       = cyclesAccum;
	bailouts[bailoutCount].instructions = instrCount;
	bailoutCount++;
}

// =========================================================================
// Trace scanner + epilogue
// =========================================================================
BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu)
{
	JitTraceCtx ctx{ cpu, cache };
	ctx.startPC = ctx.currentPC = startPC;

	while (!ctx.endBlock && ctx.instrCount < JIT_TRACE_MAX_INSTRUCTIONS) {
		if (ctx.arenaAllocated) {
			s32 used = (s32)(ctx.emitPtr - ctx.blockStart);
			s32 budget = (s32)(JIT_MAX_WORDS - JIT_EPILOGUE_RESERVE_WORDS
			                   - (s32)ctx.bailoutCount * JIT_BAILOUT_STUB_WORDS);
			if (used > budget) { ctx.endBlock = true; break; }
		}

		u16 opcode = (u16)cpu.fetch16(ctx.currentPC);
		jitThumbEmitOne(ctx, opcode);

		if (!ctx.endBlock) {
			ctx.instrCount++;
			ctx.currentPC   += 2;
			ctx.cyclesAccum += cpu.cyclesForThumb(opcode);
		}
	}

	if (ctx.instrCount == 0) {
		// Nothing compilable at startPC -- cache a length-1 fallback so the
		// dispatcher resolves straight to the interpreter next time.
		return cache.registerBlock(startPC, 1, nullptr);
	}

	// ---- jump over the deferred bailouts on any fall-through path ----
	u32* branchSkipBailouts = nullptr;
	if (ctx.bailoutCount > 0 && !ctx.blockTerminatedEarly)
		branchSkipBailouts = ctx.emitPtr++;

	for (u32 i = 0; i < ctx.bailoutCount; i++) {
		u32* target = ctx.emitPtr;
		u32  off = (u32)((target - ctx.bailouts[i].branchPtr) * 4);
		switch (ctx.bailouts[i].cond) {
			case JIT_COND_BEQ: *ctx.bailouts[i].branchPtr = PPC_BEQ(off); break;
			case JIT_COND_BNE: *ctx.bailouts[i].branchPtr = PPC_BNE(off); break;
			case JIT_COND_BGE: *ctx.bailouts[i].branchPtr = PPC_BGE(off); break;
			case JIT_COND_BLT: *ctx.bailouts[i].branchPtr = PPC_BLT(off); break;
			case JIT_COND_BLE: *ctx.bailouts[i].branchPtr = PPC_BLE(off); break;
		}
		ctx.emitAddCycles(ctx.bailouts[i].cycles);
		ctx.emitResultMetadata(ctx.bailouts[i].instructions, 1);
		*ctx.emitPtr++ = PPC_LIS(PPC_R4, ctx.bailouts[i].pc >> 16);
		*ctx.emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, ctx.bailouts[i].pc & 0xFFFF);
		s32 ret = (s32)((u8*)cache.linkerReturnAddress - (u8*)ctx.emitPtr);
		*ctx.emitPtr++ = PPC_B(ret);
	}

	if (branchSkipBailouts)
		*branchSkipBailouts = PPC_B((u32)((ctx.emitPtr - branchSkipBailouts) * 4));

	// ---- default epilogue: fall off the end of the block ----
	if (!ctx.blockTerminatedEarly) {
		ctx.emitAddCycles(ctx.cyclesAccum);
		ctx.flushDirtyFlags();
		ctx.flushDirtyRegisters();
		ctx.emitResultMetadata(ctx.instrCount, 0);

		*ctx.emitPtr++ = PPC_LIS(PPC_R29, (ctx.currentPC + 4) >> 16);
		*ctx.emitPtr++ = PPC_ORI(PPC_R29, PPC_R29, (ctx.currentPC + 4) & 0xFFFF);
		*ctx.emitPtr++ = PPC_LIS(PPC_R4, ctx.currentPC >> 16);
		*ctx.emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, ctx.currentPC & 0xFFFF);

		s32 stubOff = (s32)((u8*)cache.linkerStubAddress - (u8*)ctx.emitPtr);
		*ctx.emitPtr++ = PPC_BL(stubOff);
		s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)ctx.emitPtr);
		*ctx.emitPtr++ = PPC_B(retOff);
	}

	// ---- quota-shield yield stub ----
	u32* yieldTarget = ctx.emitPtr;
	ctx.emitResultMetadata(0, 1);
	*ctx.emitPtr++ = PPC_LIS(PPC_R4, startPC >> 16);
	*ctx.emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, startPC & 0xFFFF);
	s32 yieldOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)ctx.emitPtr);
	*ctx.emitPtr++ = PPC_B(yieldOff);
	*ctx.quotaGuard = PPC_BGE((u32)((yieldTarget - ctx.quotaGuard) * 4));

	// ---- finalize: rewind unused reservation, sync caches, register ----
	u32 emittedWords = (u32)(ctx.emitPtr - ctx.blockStart);
	u32 actualBytes  = emittedWords * sizeof(u32);
	u32 committed    = (actualBytes + 31) & ~31u;
	s32 diff         = (s32)(JIT_MAX_WORDS * sizeof(u32) - committed);
	u32 rewind       = diff & ~(diff >> 31);

	cache.rewindJITMemory(rewind);
	DCStoreRange(ctx.blockStart, actualBytes);
	ICInvalidateRange(ctx.blockStart, actualBytes);

	return cache.registerBlock(startPC, ctx.instrCount, (JITBlockFunc)ctx.blockStart);
}

#endif // DESMUME_JIT_ARM7
