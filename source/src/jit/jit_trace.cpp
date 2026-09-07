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
#include <stdio.h>   // GO-FIX-PH canary diagnostic wants this unconditionally; see below

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

// GO-FIX-PH diagnostic: a heap corruption (invalid write inside newlib's
// _malloc_r) surfaces after enough sustained real (non-differential) ARM9
// JIT execution, reproduces with all chaining disabled, and does NOT
// reproduce with the JIT off entirely -- so it's some JIT-owned buffer being
// written past its bounds. None of these four memalign'd allocations are
// ever resized after jitInit(), so if one of them is the culprit the overrun
// has to come from a runtime write, not (only) code generation exceeding its
// reservation (that path already has its own ARENA OVERRUN diagnostic and
// it never fires for this repro). 32-byte canary directly after each real
// allocation, checked periodically; the first mismatch pinpoints which
// buffer and by how much. Trivially removable once the bug is found.
#define JIT_CANARY_BYTES 32
static u8 s_canaryPattern[JIT_CANARY_BYTES];
struct CanarySlot { void* base; size_t realSize; const char* name; };
static CanarySlot s_canaries[8];
static int s_canaryCount = 0;
static bool s_canaryTripped = false;

static void jitCanaryArm(void* buf, size_t realSize, const char* name)
{
	if (!buf || s_canaryCount >= 8) return;
	memcpy((u8*)buf + realSize, s_canaryPattern, JIT_CANARY_BYTES);
	s_canaries[s_canaryCount++] = { buf, realSize, name };
}

void jitCheckCanaries()
{
	if (s_canaryTripped) return;
	for (int i = 0; i < s_canaryCount; i++) {
		u8* tail = (u8*)s_canaries[i].base + s_canaries[i].realSize;
		if (memcmp(tail, s_canaryPattern, JIT_CANARY_BYTES) != 0) {
			s_canaryTripped = true;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) {
				fprintf(f, "[jit] !!! CANARY TRIPPED buf=%s base=%p realSize=%u tail=%p bytes:",
				        s_canaries[i].name, s_canaries[i].base,
				        (unsigned)s_canaries[i].realSize, (void*)tail);
				for (int b = 0; b < JIT_CANARY_BYTES; b++) fprintf(f, " %02x", tail[b]);
				fprintf(f, "\n");
				fclose(f);
			}
			return;
		}
	}
}

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
	// GO-FIX-PH: over-allocate by JIT_CANARY_BYTES on every buffer so a
	// canary can be armed right after each one's *logical* end -- the size
	// passed to cache.initialize() below is unchanged, so the JIT's own
	// bounds checks (allocateJITMemory() vs arenaSize, etc.) see exactly the
	// same capacity as before this diagnostic was added.
	size_t blockTableBytes  = HASH_TABLE_SIZE * sizeof(BasicBlock);
	size_t smcRegistryBytes = SMC_MAP_SIZE * sizeof(BasicBlock*);
	size_t smcFlagsBytes    = SMC_MAP_SIZE;

	s_arena[i]        = (u32*)        memalign(32, arenaBytes        + JIT_CANARY_BYTES);
	s_blockTable[i]   = (BasicBlock*) memalign(16, blockTableBytes   + JIT_CANARY_BYTES);
	s_smcRegistry[i]  = (BasicBlock**)memalign(32, smcRegistryBytes  + JIT_CANARY_BYTES);
	s_smcPageFlags[i] = (u8*)         memalign(32, smcFlagsBytes     + JIT_CANARY_BYTES);

	if (!s_arena[i] || !s_blockTable[i] || !s_smcRegistry[i] || !s_smcPageFlags[i])
		return false;

	jitCanaryArm(s_arena[i],        arenaBytes,        i == JIT_ARM9 ? "arm9.arena"   : "arm7.arena");
	jitCanaryArm(s_blockTable[i],   blockTableBytes,    i == JIT_ARM9 ? "arm9.blockTable"  : "arm7.blockTable");
	jitCanaryArm(s_smcRegistry[i],  smcRegistryBytes,   i == JIT_ARM9 ? "arm9.smcRegistry" : "arm7.smcRegistry");
	jitCanaryArm(s_smcPageFlags[i], smcFlagsBytes,      i == JIT_ARM9 ? "arm9.smcPageFlags": "arm7.smcPageFlags");

	cache.initialize(s_arena[i], arenaBytes, s_blockTable[i], s_smcRegistry[i],
	                 s_smcPageFlags[i], profile->smcBankMask);
	jitProfile[i] = profile;
	return true;
}

void jitInit()
{
	if (s_initDone) return;

	memset(s_canaryPattern, 0xC5, JIT_CANARY_BYTES);   // GO-FIX-PH: before either slot inits

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

// ---- packed flags: PPC_REG_FLAGS (r30, P12) holds the whole guest CPSR word --
// r30 is non-volatile and loaded from *cpsr by the trampoline on entry, so the
// flags are already resident on every path -- ensureFlagsLoaded() has nothing to
// emit. It stays a call so the flag helpers read naturally and a future
// lazy-load scheme could slot back in here.
void JitTraceCtx::ensureFlagsLoaded()
{
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

// P12: the packed flags live in the non-volatile r30 for the whole trace and
// the trampoline stores r30 -> *cpsr on the single shared return path that every
// exit (clean, chained, bailed, quota-yield, SMC) funnels through, so no block
// writes the CPSR word back itself. These stay as no-ops rather than being
// deleted at every call site.
void JitTraceCtx::flushDirtyFlags()  { flagsDirty = false; }
void JitTraceCtx::emitDirtyFlagFlush() {}

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
		// GO-FIX-PH hardening: every real ARM/THUMB instruction locks only a
		// handful of operands (<=4) against 14 pool registers, so this should
		// be unreachable -- but the old code indexed regCache[-1] unconditionally
		// if it ever *was* unreachable-in-theory-but-not-in-practice, emitting
		// a PPC_STW through a garbage host register at gpr_base-4, one word
		// before the guest register array: a host memory write outside the
		// guest state the differential harness compares, so a bug here can
		// corrupt unrelated heap memory without ever showing up as a DIFF.
		// If genuinely no eviction candidate exists, fall back to the oldest
		// *any* allocated slot (ignoring the lock) rather than a negative index.
		if (spillTarget < 0) {
			for (int i = 0; i < 15; i++) {
				if (regCache[i].allocated && regCache[i].age < oldestAge) {
					oldestAge = regCache[i].age;
					spillTarget = i;
				}
			}
		}
		if (spillTarget < 0) {
			// regCache has zero allocated entries yet freeMask == 0 -- would mean
			// lockedMask alone claims all 14 pool registers, i.e. >=14 locked
			// operands on one instruction. Not reachable by any real emitter;
			// bail the block rather than touch the array with a bad index.
			endBlock = true;
			return PPC_R10;
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
// r3 holds the cross-block cycle accumulator and is PPC-EABI volatile, so it's
// spilled/reloaded around the call. Guest regs (r14..r31) -- including the P12
// resident r30 packed flags and r31 instruction count -- are non-volatile and
// survive the call untouched, so the flags no longer need a spill here.
void JitTraceCtx::emitMemPrologue()
{
	flushDirtyRegisters();
	*emitPtr++ = PPC_STW(PPC_R29, 14, 15 * 4);   // guest PC -> gpr[15] (in case the C path peeks)
	*emitPtr++ = PPC_STW(PPC_R3, 1, 92);         // save cycle accumulator
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
#ifdef JIT_GOFIXPH_NO_SMC
	(void)eaReg; return;   // GO-FIX-PH diagnostic: skip the inline SMC guard
#endif
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

// emitInterpreterBail whose emitted (runtime-only) state flush must not disturb
// the compile-time dirty bookkeeping the fall-through fast path still relies on.
static void bailPreservingDirty(JitTraceCtx& ctx, u32 metaCount)
{
	const bool fd = ctx.flagsDirty;
	bool sv[15];
	for (int i = 0; i < 15; i++) sv[i] = ctx.regCache[i].dirty;
	ctx.emitInterpreterBail(metaCount);
	ctx.flagsDirty = fd;
	for (int i = 0; i < 15; i++) ctx.regCache[i].dirty = sv[i];
}

// P14/P15 shared: guard EA (PPC_R12) against the cached-descriptor page window,
// optionally also that EA + spanBytes stays in the same 1 MB page, bail to the
// interpreter (at currentPC) on a miss, then resolve the descriptor. On return
// PPC_R10 = hostBase and PPC_R11 = (EA & mask) with the low bits cleared to
// `alignMe` (29 => &~3, 30 => &~1, 31 => no clear). EA stays in PPC_R12.
// Clobbers r10, r11.
void JitTraceCtx::emitPageResolve(u32 spanBytes, u8 alignMe)
{
	u32*& p = emitPtr;
	const u32 lo   = cpu.pageDescLo;
	const u32 span = cpu.pageDescHi - cpu.pageDescLo;

	// lo <= (EA >> 20) <= hi
	*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);
	*p++ = PPC_ADDI(PPC_R10, PPC_R10, -(s32)lo);        // page - lo
	*p++ = PPC_CMPLI(0, PPC_R10, span);                 // unsigned > span => out of window
	{
		u32* inRange = p++;
		bailPreservingDirty(*this, instrCount);
		*inRange = PPC_BLE((u32)((p - inRange) * 4));
	}

	// whole run in one 1 MB page: (EA + spanBytes) >> 20 == EA >> 20
	if (spanBytes) {
		*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)spanBytes);
		*p++ = PPC_SRWI(PPC_R11, PPC_R11, 20);
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);          // reload page (r10 was page-lo)
		*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
		u32* samePage = p++;
		bailPreservingDirty(*this, instrCount);
		*samePage = PPC_BEQ((u32)((p - samePage) * 4));
		*p++ = PPC_ADDI(PPC_R10, PPC_R10, -(s32)lo);    // back to page - lo
	}

	// descriptor = pageDescBase[(page - lo) * 8] : { hostBase@+0, mask@+4 }
	*p++ = PPC_RLWINM(PPC_R10, PPC_R10, 3, 0, 28);      // (page - lo) * 8
	if ((s32)(s16)cpu.pageDescBase == (s32)cpu.pageDescBase) {
		*p++ = PPC_ADDI(PPC_R11, PPC_R10, (s32)cpu.pageDescBase);
	} else {
		*p++ = PPC_LIS(PPC_R11, cpu.pageDescBase >> 16);
		if (cpu.pageDescBase & 0xFFFF) *p++ = PPC_ORI(PPC_R11, PPC_R11, cpu.pageDescBase & 0xFFFF);
		*p++ = PPC_ADD(PPC_R11, PPC_R11, PPC_R10);
	}
	*p++ = PPC_LWZ(PPC_R10, PPC_R11, 0);                // hostBase
	*p++ = PPC_LWZ(PPC_R11, PPC_R11, 4);                // mask
	*p++ = PPC_AND(PPC_R11, PPC_R12, PPC_R11);          // EA & mask (unaligned)
	if (alignMe < 31) *p++ = PPC_RLWINM(PPC_R11, PPC_R11, 0, 0, alignMe);
}

// P14: inline RAM load. See jit_trace.h. eaReg == PPC_R12 by contract.
bool JitTraceCtx::emitInlineLoad(u8 rd, u8 eaReg, u32 size, bool signExt, bool wordRotate, u32& lockedMask)
{
	if (!cpu.pageDescBase) return false;
	(void)eaReg;                                  // contract: EA is in PPC_R12
	u32*& p = emitPtr;

	emitPageResolve(/*spanBytes=*/0, size == 4 ? 29 : size == 2 ? 30 : 31);

	// destination host reg allocated *after* the guard so a bail never dirties rd
	const u8 hDst = writeReg(rd, /*fullOverwrite=*/true, lockedMask);

	if (size == 4) {
		*p++ = PPC_LWBRX(hDst, PPC_R10, PPC_R11);
		// ARM OP_LDR rotates an unaligned word: ROR(word, 8 * (EA & 3)). THUMB's
		// load paths in DeSmuME do not (jit_thumb.cpp's slow path skips it), so
		// the caller passes wordRotate to match its own slow path exactly.
		if (wordRotate) {
			*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 30, 31); // x = EA & 3
			*p++ = PPC_SUBFIC(PPC_R11, PPC_R11, 4);         // 4 - x
			*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 3, 27, 28); // ((4 - x) & 3) << 3  in {0,24,16,8}
			*p++ = PPC_RLWNM(hDst, hDst, PPC_R11, 0, 31);   // ROL by that == ROR by 8*x
		}
	} else if (size == 2) {
		*p++ = PPC_LHBRX(hDst, PPC_R10, PPC_R11);
		if (signExt) *p++ = PPC_EXTSH(hDst, hDst);
	} else {
		*p++ = PPC_LBZX(hDst, PPC_R10, PPC_R11);
		if (signExt) *p++ = PPC_EXTSB(hDst, hDst);
	}
	return true;
}

// P15: inline sequential block load (LDM / POP / LDMIA). See jit_trace.h.
bool JitTraceCtx::emitInlineBlockLoad(const u8* regs, u32 n, u8 eaReg, u32& lockedMask)
{
	if (!cpu.pageDescBase) return false;
	(void)eaReg;                                  // contract: low EA is in PPC_R12
	u32*& p = emitPtr;

	emitPageResolve(/*spanBytes=*/4 * (n - 1), /*alignMe=*/29);   // LDM: no unaligned rotate

	// r10 = hostBase, r11 = aligned page offset of the low word. Walk ascending;
	// each destination host reg is allocated here (after the guard) so a bail
	// never dirties them.
	for (u32 k = 0; k < n; k++) {
		const u8 hgi = writeReg(regs[k], /*fullOverwrite=*/true, lockedMask);
		*p++ = PPC_LWBRX(hgi, PPC_R10, PPC_R11);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R11, PPC_R11, 4);
	}
	return true;
}

void JitTraceCtx::emitResultMetadata(u32 count, u32 bailedOut, u32 smcHit)
{
	// P12: the guest-instruction count lives in the resident r31 accumulator for
	// the whole (possibly chained) trace -- one addi here vs. the old
	// load/add/store round-trip through out->instructions at every block
	// boundary. The trampoline zeroes r31 on entry and writes it back to
	// out->instructions once, on return.
	if (count) *emitPtr++ = PPC_ADDI(PPC_R31, PPC_R31, (s32)count);

	// bailedOut / smcHit are already 0 on a clean exit (every caller memsets the
	// JITResult and no chained block un-clears them -- any bail is terminal), so
	// only the bail paths emit these stores.
	if (bailedOut || smcHit) {
		*emitPtr++ = PPC_LWZ(PPC_R10, 1, 88);           // outResult*
		if (bailedOut) {
			*emitPtr++ = PPC_LI(PPC_R11, bailedOut);
			*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 12); // bailedOut
		}
		if (smcHit) {
			*emitPtr++ = PPC_LI(PPC_R11, smcHit);
			*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 16); // smcHit
		}
	}
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
// Block exits -- shared by jit_thumb.cpp and jit_arm.cpp
// =========================================================================
void JitTraceCtx::emitStaticExit(u32 targetPC, u32 metaCount, u32 termCycles)
{
	u32*& p = emitPtr;
	const u32 pipe = targetPC + (thumbMode ? 4u : 8u);
	emitAddCycles(cyclesAccum + termCycles);
	flushDirtyFlags();
	flushDirtyRegisters();
	emitResultMetadata(metaCount, 0);
	*p++ = PPC_LIS(PPC_R29, pipe >> 16);
	*p++ = PPC_ORI(PPC_R29, PPC_R29, pipe & 0xFFFF);
	*p++ = PPC_LIS(PPC_R4, targetPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
#if JIT_ENABLE_CHAINING
	{ s32 o = (s32)((u8*)cache.linkerStubAddress - (u8*)p); *p++ = PPC_BL(o); }
#endif
	{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)p); *p++ = PPC_B(o); }
}

void JitTraceCtx::emitDynamicExit(u8 pcReg, u32 metaCount, u32 termCycles, bool targetThumb)
{
	u32*& p = emitPtr;
	emitAddCycles(cyclesAccum + termCycles);
	flushDirtyFlags();
	flushDirtyRegisters();
	emitResultMetadata(metaCount, 0);
	// r29 (guest PC / GBA R15) needs the *pipeline* value, not the bare target
	// -- the interpreter-pipeline convention emitStaticExit also follows -- so
	// a guarded-dispatch hit lands in the next block with r29 already correct
	// and never has to reload it from memory (r29 is always-resident, never
	// spilled/reloaded like r15..r28's lazy cache).
	*p++ = PPC_ADDI(PPC_R29, pcReg, targetThumb ? 4 : 8);
	*p++ = PPC_OR(PPC_R4, pcReg, pcReg);
#if JIT_ENABLE_DYNAMIC_CHAINING
	{
		u32* stub = targetThumb ? cache.linkerStubDynamicThumbAddress : cache.linkerStubDynamicArmAddress;
		s32 o = (s32)((u8*)stub - (u8*)p);
		*p++ = PPC_B(o);
	}
#else
	s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)p);
	*p++ = PPC_B(retOff);
#endif
}

void JitTraceCtx::emitInterpreterBail(u32 metaCount)
{
	u32*& p = emitPtr;
	flushDirtyFlags();
	flushDirtyRegisters();
	emitAddCycles(cyclesAccum);
	emitResultMetadata(metaCount, 1);
	*p++ = PPC_LIS(PPC_R4, currentPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, currentPC & 0xFFFF);
	s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)p);
	*p++ = PPC_B(retOff);
}

// ARM predication: 0/1 "condition holds" -> PPC_R11. cond is 0..13. Clobbers
// r10, r11. Mirrors the CONDITION() table in armcpu.h / arm_instructions.cpp.
void JitTraceCtx::emitEvalCond(u8 cond)
{
	switch (cond) {
	case 0x0: readFlag(JITF_Z, PPC_R11); break;                                           // EQ  Z
	case 0x1: readFlag(JITF_Z, PPC_R11); *emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1); break;// NE  !Z
	case 0x2: readFlag(JITF_C, PPC_R11); break;                                           // CS  C
	case 0x3: readFlag(JITF_C, PPC_R11); *emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1); break;// CC  !C
	case 0x4: readFlag(JITF_N, PPC_R11); break;                                           // MI  N
	case 0x5: readFlag(JITF_N, PPC_R11); *emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1); break;// PL  !N
	case 0x6: readFlag(JITF_V, PPC_R11); break;                                           // VS  V
	case 0x7: readFlag(JITF_V, PPC_R11); *emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1); break;// VC  !V
	case 0x8:                                                                             // HI  C & ~Z
		readFlag(JITF_C, PPC_R10); readFlag(JITF_Z, PPC_R11);
		*emitPtr++ = PPC_ANDC(PPC_R11, PPC_R10, PPC_R11);
		break;
	case 0x9:                                                                             // LS  ~(C & ~Z)
		readFlag(JITF_C, PPC_R10); readFlag(JITF_Z, PPC_R11);
		*emitPtr++ = PPC_ANDC(PPC_R11, PPC_R10, PPC_R11);
		*emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1);
		break;
	case 0xA:                                                                             // GE  ~(N ^ V)
		readFlag(JITF_N, PPC_R10); readFlag(JITF_V, PPC_R11);
		*emitPtr++ = PPC_XOR(PPC_R11, PPC_R10, PPC_R11);
		*emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1);
		break;
	case 0xB:                                                                             // LT  N ^ V
		readFlag(JITF_N, PPC_R10); readFlag(JITF_V, PPC_R11);
		*emitPtr++ = PPC_XOR(PPC_R11, PPC_R10, PPC_R11);
		break;
	case 0xC: case 0xD:                                                                   // GT=~LE, LE=Z|(N^V)
		readFlag(JITF_N, PPC_R10); readFlag(JITF_V, PPC_R11);
		*emitPtr++ = PPC_XOR(PPC_R11, PPC_R10, PPC_R11);
		readFlag(JITF_Z, PPC_R10);
		*emitPtr++ = PPC_OR(PPC_R11, PPC_R11, PPC_R10);
		if (cond == 0xC) *emitPtr++ = PPC_XORI(PPC_R11, PPC_R11, 1);
		break;
	default:  *emitPtr++ = PPC_LI(PPC_R11, 1); break;
	}
}

// =========================================================================
// Trace scanner + epilogue
// =========================================================================
BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu, bool thumb)
{
	JitTraceCtx ctx{ cpu, cache };
	ctx.startPC = ctx.currentPC = startPC;
	ctx.thumbMode = thumb;

	while (!ctx.endBlock && ctx.instrCount < JIT_TRACE_MAX_INSTRUCTIONS) {
		if (ctx.arenaAllocated) {
			s32 used = (s32)(ctx.emitPtr - ctx.blockStart);
			// Must reserve room for the epilogue/bailout stubs AND the worst-case
			// size of the instruction we're about to scan -- this check runs
			// BEFORE the emitter, so "used" only reflects instructions already
			// emitted. See JIT_MAX_INSTR_RESERVE_WORDS[_ARM] for why.
			s32 budget = (s32)(JIT_MAX_WORDS - JIT_EPILOGUE_RESERVE_WORDS
			                   - (s32)ctx.bailoutCount * JIT_BAILOUT_STUB_WORDS
			                   - (thumb ? JIT_MAX_INSTR_RESERVE_WORDS
			                            : JIT_MAX_INSTR_RESERVE_WORDS_ARM));
			if (used > budget) { ctx.endBlock = true; break; }
		}

		if (thumb) {
			u16 opcode = (u16)cpu.fetch16(ctx.currentPC);
			jitThumbEmitOne(ctx, opcode);
			if (!ctx.endBlock) {
				ctx.instrCount++;
				ctx.currentPC   += 2;
				ctx.cyclesAccum += cpu.cyclesForThumb(opcode);
			}
		} else {
			u32 opcode = cpu.fetch32(ctx.currentPC);
			jitArmEmitOne(ctx, opcode);
			if (!ctx.endBlock) {
				ctx.instrCount++;
				ctx.currentPC   += 4;
				ctx.cyclesAccum += cpu.cyclesForArm(opcode);
			}
		}
	}

	if (ctx.instrCount == 0) {
		// Nothing compilable at startPC -- cache a length-1 fallback so the
		// dispatcher resolves straight to the interpreter next time.
		return cache.registerBlock(startPC, 1, nullptr, thumb);
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

		const u32 pipe = ctx.currentPC + (thumb ? 4u : 8u);
		*ctx.emitPtr++ = PPC_LIS(PPC_R29, pipe >> 16);
		*ctx.emitPtr++ = PPC_ORI(PPC_R29, PPC_R29, pipe & 0xFFFF);
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

	return cache.registerBlock(startPC, ctx.instrCount, (JITBlockFunc)ctx.blockStart, thumb);
}

#endif // DESMUME_JIT_ARM7
