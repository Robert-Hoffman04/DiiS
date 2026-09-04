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
#if defined(DESMUME_JIT_TRACE_FIRST) || defined(DESMUME_ARM_TIME_SPLIT)
#include <stdio.h>
#endif

// =========================================================================
// Lifecycle
// =========================================================================
JitCpuProfile* jitProfile[2] = { nullptr, nullptr };

extern JitCpuProfile* jitBuildArm7Profile();   // jit_arm7_profile.cpp
extern JitCpuProfile* jitBuildArm9Profile();   // jit_arm9_profile.cpp

// One backing-store set per core, indexed [JIT_ARM9]=0 / [JIT_ARM7]=1.
static u32*         s_arena[2]        = { nullptr, nullptr };
static BasicBlock*  s_blockTable[2]   = { nullptr, nullptr };
static BasicBlock** s_smcRegistry[2]  = { nullptr, nullptr };
static u8*          s_smcPageFlags[2] = { nullptr, nullptr };
static bool         s_initDone        = false;

static void jitFreeSlot(int i)
{
	free(s_arena[i]);        s_arena[i]        = nullptr;
	free(s_blockTable[i]);   s_blockTable[i]   = nullptr;
	free(s_smcRegistry[i]);  s_smcRegistry[i]  = nullptr;
	free(s_smcPageFlags[i]); s_smcPageFlags[i] = nullptr;
}

void jitShutdown()
{
	jitCacheArm7.destroy();
	jitCacheArm9.destroy();
	jitFreeSlot(JIT_ARM7);
	jitFreeSlot(JIT_ARM9);
	jitProfile[JIT_ARM7] = jitProfile[JIT_ARM9] = nullptr;
	s_initDone = false;
}

static bool jitInitSlot(int i, size_t arenaBytes, JITCache& cache, JitCpuProfile* profile)
{
	s_arena[i]        = (u32*)        memalign(32, arenaBytes);
	s_blockTable[i]   = (BasicBlock*) memalign(16, HASH_TABLE_SIZE * sizeof(BasicBlock));
	s_smcRegistry[i]  = (BasicBlock**)memalign(32, SMC_MAP_SIZE * sizeof(BasicBlock*));
	s_smcPageFlags[i] = (u8*)         memalign(32, SMC_MAP_SIZE);

	if (!s_arena[i] || !s_blockTable[i] || !s_smcRegistry[i] || !s_smcPageFlags[i])
		return false;

	cache.initialize(s_arena[i], arenaBytes, s_blockTable[i], s_smcRegistry[i],
	                 s_smcPageFlags[i], profile->smcBankMask);
	jitProfile[i] = profile;
	return true;
}

void jitInit()
{
	if (s_initDone) return;

	bool ok = jitInitSlot(JIT_ARM7, JIT_ARENA_SIZE,      jitCacheArm7, jitBuildArm7Profile())
	       && jitInitSlot(JIT_ARM9, JIT_ARENA_SIZE_ARM9, jitCacheArm9, jitBuildArm9Profile());

	if (!ok) { jitShutdown(); return; }
	s_initDone = true;
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

void JitTraceCtx::invalidateRegCache()
{
	for (int i = 0; i < 15; i++) { regCache[i].allocated = false; regCache[i].dirty = false; }
	allocatedHostRegsMask = 0;
}

// ---- guest memory via a C call to JitCpuProfile::slowRead/slowWrite --------
// r3 holds the cross-block cycle accumulator and r6 the packed flags; both are
// PPC-EABI volatile, so they're spilled/dropped around the call. Guest regs
// (r14..r31) are non-volatile and survive it.
void JitTraceCtx::emitMemPrologue()
{
	flushDirtyFlags();
	flushDirtyRegisters();
	*emitPtr++ = PPC_STW(PPC_R29, 14, 15 * 4);   // guest PC -> gpr[15]
	*emitPtr++ = PPC_STW(PPC_R3, 1, 92);         // save cycle accumulator
	flagsLoaded = false;                          // r6 clobbered by the call
}

void JitTraceCtx::emitMemEpilogue()
{
	*emitPtr++ = PPC_LWZ(PPC_R3, 1, 92);
}

void JitTraceCtx::emitSlowLoad(u8 destReg, u8 eaReg, u32 size, bool signExtend)
{
	u32 fn = (u32)cpu.slowRead;
	*emitPtr++ = PPC_OR(PPC_R3, eaReg, eaReg);            // arg1 = addr
	*emitPtr++ = PPC_LI(PPC_R4, (s32)size);               // arg2 = size
	*emitPtr++ = PPC_LIS(PPC_R12, fn >> 16);
	*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*emitPtr++ = PPC_MTCTR(PPC_R12);
	*emitPtr++ = PPC_BCTRL();
	*emitPtr++ = PPC_OR(destReg, PPC_R3, PPC_R3);
	if (signExtend && size == 1) *emitPtr++ = PPC_EXTSB(destReg, destReg);
	if (signExtend && size == 2) *emitPtr++ = PPC_EXTSH(destReg, destReg);
}

void JitTraceCtx::emitSlowStore(u8 eaReg, u8 valReg, u32 size)
{
	u32 fn = (u32)cpu.slowWrite;
	*emitPtr++ = PPC_OR(PPC_R3, eaReg, eaReg);            // arg1 = addr
	*emitPtr++ = PPC_OR(PPC_R4, valReg, valReg);          // arg2 = value
	*emitPtr++ = PPC_LI(PPC_R5, (s32)size);               // arg3 = size
	*emitPtr++ = PPC_LIS(PPC_R12, fn >> 16);
	*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*emitPtr++ = PPC_MTCTR(PPC_R12);
	*emitPtr++ = PPC_BCTRL();
}

// Store paths: if a compiled block currently lives on the written 1KB page,
// bail (resume PC = this instruction) with smcHit set so the caller can call
// JitCpuProfile::smcInvalidate. eaReg must still be live; state must already be
// flushed (emitMemPrologue). Does not end the block -- the not-taken path
// continues compiling.
void JitTraceCtx::emitSmcCheckAndBail(u8 eaReg)
{
	u32 fp = (u32)cache.smcPageFlags;
	*emitPtr++ = PPC_RLWINM(PPC_R11, eaReg, 22, 16, 31);   // r11 = (EA>>10) & 0xFFFF
	*emitPtr++ = PPC_LIS(PPC_R10, fp >> 16);
	*emitPtr++ = PPC_ORI(PPC_R10, PPC_R10, fp & 0xFFFF);
	*emitPtr++ = PPC_LBZX(PPC_R10, PPC_R10, PPC_R11);      // r10 = smcPageFlags[page]
	*emitPtr++ = PPC_CMPWI(0, PPC_R10, 0);
	u32* skip = emitPtr++;

	*emitPtr++ = PPC_LWZ(PPC_R10, 1, 88);
	*emitPtr++ = PPC_STW(eaReg, PPC_R10, 20);              // out->smcAddress = EA
	emitAddCycles(cyclesAccum);
	emitResultMetadata(instrCount, 1, 1);                  // bailedOut=1, smcHit=1
	*emitPtr++ = PPC_LIS(PPC_R4, currentPC >> 16);
	*emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, currentPC & 0xFFFF);
	s32 ret = (s32)((u8*)cache.linkerReturnAddress - (u8*)emitPtr);
	*emitPtr++ = PPC_B(ret);

	*skip = PPC_BEQ((u32)((emitPtr - skip) * 4));
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
			// Must reserve room for the epilogue/bailout stubs AND the worst-case
			// size of the instruction we're about to scan -- this check runs
			// BEFORE jitThumbEmitOne(), so "used" only reflects instructions
			// already emitted. See JIT_MAX_INSTR_RESERVE_WORDS for why.
			s32 budget = (s32)(JIT_MAX_WORDS - JIT_EPILOGUE_RESERVE_WORDS
			                   - (s32)ctx.bailoutCount * JIT_BAILOUT_STUB_WORDS
			                   - JIT_MAX_INSTR_RESERVE_WORDS);
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
#if JIT_ENABLE_CHAINING
		{ s32 o = (s32)((u8*)cache.linkerStubAddress - (u8*)ctx.emitPtr); *ctx.emitPtr++ = PPC_BL(o); }
#endif
		{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)ctx.emitPtr); *ctx.emitPtr++ = PPC_B(o); }
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

	// Hard invariant: the per-instruction budget check above must guarantee
	// this never goes negative -- a negative diff means this block's code
	// (rewind==0, so arenaOffset stays put) extends past its reserved slot
	// into memory the NEXT allocateJITMemory() call will hand out and
	// overwrite, corrupting this still-registered, still-executable block.
	// Loud and visible rather than a silent arena corruption + eventual wild
	// jump if JIT_MAX_INSTR_RESERVE_WORDS is ever undersized for a new format.
#ifdef DESMUME_JIT_TRACE_FIRST
	if (diff < 0) {
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) { fprintf(f, "[jit] !!! ARENA OVERRUN pc=%08x emittedWords=%u over=%d\n",
		                 (unsigned)startPC, (unsigned)emittedWords, (int)-diff); fclose(f); }
	}
#endif

	cache.rewindJITMemory(rewind);
	DCStoreRange(ctx.blockStart, actualBytes);
	ICInvalidateRange(ctx.blockStart, actualBytes);

	return cache.registerBlock(startPC, ctx.instrCount, (JITBlockFunc)ctx.blockStart);
}

#endif // DESMUME_JIT_ARM7
