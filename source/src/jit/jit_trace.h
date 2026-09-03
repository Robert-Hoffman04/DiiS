/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_trace.h
 *
 * The front-end-agnostic half of the compiler: the trace scanner, the lazy
 * host-register allocator, the packed-flag helpers, deferred-bailout
 * bookkeeping and the block epilogue. jit_thumb.cpp (P2) and jit_arm.cpp (P7)
 * are just emitter tables that run on top of this.
 *
 * Derived from VBA-GX's JITCompiler.cpp (c) Daryl Borth, GPL v2+ -- see
 * jit/upstream/PROVENANCE.md. The GBA prefetch-buffer / wait-state timing
 * machinery is dropped (DeSmuME leaves ARM7 access timing disabled); cycle
 * cost is a compile-time sum of JitCpuProfile::cyclesForThumb.
 ***************************************************************************/

#ifndef DESMUME_JIT_TRACE_H
#define DESMUME_JIT_TRACE_H

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

// --- arena / block budget ------------------------------------------------
#define JIT_MAX_WORDS              3072
#define JIT_YIELD_NUMBER           64
#define JIT_MAX_BAILOUTS           256
#define JIT_EPILOGUE_RESERVE_WORDS 64
#define JIT_BAILOUT_STUB_WORDS     20

// Worst-case PPC words a single THUMB instruction's emitter can produce before
// the scanner's next per-iteration budget check runs again. The heaviest
// formats are PUSH/POP and LDMIA/STMIA with a full 8-9 register list, each
// register costing ~10 words (address calc + emitSlowLoad/Store's C-call
// sequence + store/load back into the guest gpr array) on top of the
// mem prologue/epilogue and SMC check -- hand-measured worst case ~150 words
// for POP{r0-r7,pc}. This is the headroom the pre-emission budget check in
// jitCompileTrace() must reserve on top of JIT_EPILOGUE_RESERVE_WORDS; without
// it, the check only guarantees room for the *previous* instructions plus the
// epilogue, and a single heavy instruction can emit past JIT_MAX_WORDS. That
// overrun isn't caught (rewindJITMemory() only ever shrinks, so a negative
// "unused space" silently becomes a rewind of 0) -- the arena's bump allocator
// then hands the very next compiled block a region starting inside this
// block's already-emitted tail, and that block's compiler overwrites it with
// unrelated code. The corrupted block later runs off into garbage/zeroed
// memory (observed as a Broadway ISI exception, PC=0, r14=0). 2x margin over
// the hand-measured worst case.
#define JIT_MAX_INSTR_RESERVE_WORDS 300

// Block-to-block chaining (self-patching linker stub). Off for P3: one block
// per armInnerLoop turn keeps the ARM9/ARM7 interleave fine-grained. Revisit
// in P6 ("block chaining tuning").
#ifndef JIT_ENABLE_CHAINING
#define JIT_ENABLE_CHAINING 0
#endif

// Packed-flag bit indices. These are IBM/rlwinm bit numbers 0..3 (the top
// nibble, conventional bits 31..28) -- which is exactly ARM CPSR's N/Z/C/V
// layout, so PPC_REG_FLAGS can just hold the whole CPSR word.
#define JITF_N 0
#define JITF_Z 1
#define JITF_C 2
#define JITF_V 3

enum JitBailoutCond { JIT_COND_BEQ, JIT_COND_BNE, JIT_COND_BGE, JIT_COND_BLT, JIT_COND_BLE };

struct JitDeferredBailout {
	u32* branchPtr;
	JitBailoutCond cond;
	u32 pc;
	u32 cycles;
	u32 instructions;
};

struct JitRegSlot {
	bool allocated;
	bool dirty;
	u8   hostReg;
	u32  age;
};

// Per-compile state, threaded through the scanner and the emitter table.
struct JitTraceCtx {
	const JitCpuProfile& cpu;
	JITCache&            cache;

	u32* emitPtr;
	u32* blockStart;
	u32* quotaGuard;
	bool arenaAllocated;
	u32  arenaOffsetStart;

	u32  startPC;
	u32  currentPC;
	u32  instrCount;
	u32  cyclesAccum;          // running compile-time cycle sum for this block
	bool endBlock;
	bool blockTerminatedEarly; // a hard exit was emitted; skip the default epilogue

	bool flagsLoaded;
	bool flagsDirty;

	JitRegSlot regCache[15];
	u32        allocatedHostRegsMask;
	u32        currentAge;

	JitDeferredBailout bailouts[JIT_MAX_BAILOUTS];
	u32                bailoutCount;

	// ---- lifecycle ----
	void ensureArena();

	// ---- packed flags (PPC_REG_FLAGS == r6 holds the guest CPSR word) ----
	void ensureFlagsLoaded();
	void emitFlagBit(u8 targetBit, u32 srcReg, u8 sh);
	void emitFlagConst(u8 targetBit, bool value);
	u8   readFlag(u8 flagIdx, u32 dstReg);        // -> dstReg, holding 0/1
	void flushDirtyFlags();                       // stores + clears dirty
	void emitDirtyFlagFlush();                    // stores, leaves dirty set
	void emitNZ(u32 srcReg);
	void emitCVfromXER(u32 scratchReg);

	// ---- lazy host-register allocator (guest R0..R14 -> host r15..r28) ----
	u8   allocHostReg(u8 gbaReg, bool loadFromMem, u32& lockedMask);
	u8   readReg(u8 gbaReg, u32& lockedMask)  { return allocHostReg(gbaReg, true,  lockedMask); }
	u8   writeReg(u8 gbaReg, bool fullOverwrite, u32& lockedMask);
	void flushDirtyRegisters();                   // stores + clears dirty
	void emitDirtyRegisterFlush();                // stores, leaves dirty set
	void emitEagerFlush();
	void invalidateRegCache();                    // drop all host-reg allocations

	// ---- guest memory via a C call to JitCpuProfile::slowRead/slowWrite ----
	// eaReg / valReg are host scratch (r10..r12). r3 (cross-block cycle accum)
	// and the packed flags are spilled/reloaded around the call.
	void emitMemPrologue();                       // flush state, save r3, drop r6
	void emitMemEpilogue();                       // restore r3
	void emitSlowLoad(u8 destReg, u8 eaReg, u32 size, bool signExtend);
	void emitSlowStore(u8 eaReg, u8 valReg, u32 size);
	void emitSmcCheckAndBail(u8 eaReg);           // store paths: page-flag guard

	// ---- exits ----
	void emitAddCycles(u32 n);   // r3 += n  (compile-time-known)
	void emitResultMetadata(u32 count, u32 bailedOut, u32 smcHit = 0);
	void registerBailout(u32* branchPtr, JitBailoutCond cond);
};

// Emit one THUMB instruction at ctx.currentPC (opcode already fetched). Sets
// ctx.endBlock when the trace must stop here. Implemented in jit_thumb.cpp.
void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode);

BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu);

#endif // DESMUME_JIT_ARM7

#endif // DESMUME_JIT_TRACE_H
