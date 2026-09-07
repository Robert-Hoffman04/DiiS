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

// ARM (32-bit) worst case is heavier than THUMB: LDM/STM with a full {r0-r15}
// list is 16 registers each ~10 words (address calc + emitSlow* C-call + gpr
// store/load) plus the mem prologue/epilogue, SMC guard and predication
// wrapper. ~1.4x the THUMB reserve. Used by jitCompileTrace() when the block is
// ARM mode; see JIT_MAX_INSTR_RESERVE_WORDS.
#define JIT_MAX_INSTR_RESERVE_WORDS_ARM 440

// Block-to-block chaining (self-patching linker stub). Was off P3..A3: one
// block per armInnerLoop turn paid a full ExecuteJITTrace trampoline round
// trip (18-register stmw/lmw + state reload/flush) per block, which the A3
// benchmark showed dominates when blocks are tiny (often 1 guest instruction
// in a tight loop) -- effectively hanging ARM-mode-heavy code (PH boot,
// ~70-90x slower than the interpreter). On for A4-P1: a static-target exit
// (B/BL, non-S data-proc->pc, the trace-scanner's own fall-through) now BLs
// to cache.linkerStubAddress instead of always returning to C; the stub
// hash-looks-up the target and, on a hit, self-patches *that call site* to a
// direct branch straight into the target block's arena code, so repeat
// visits to the same static edge skip the hash lookup too. This call site is
// only ever reached with the one compile-time-constant target baked into it
// -- the caching is sound. emitDynamicExit (BX, LDR pc, POP{pc} and friends)
// deliberately does NOT chain: the same call site can resolve to a different
// PC on every visit (a real indirect branch), so self-patching it would
// wire the branch to whatever target happened to resolve first. Those exits
// keep returning to C every time.
//
// This also delivers the "run many blocks per armInnerLoop turn" scheduler
// quota (A4-P2) for free: JIT_YIELD_NUMBER below is a *cross-block* cycle
// quota already threaded through r3 by ensureArena()'s guard at the top of
// every block and emitAddCycles() at every exit -- with chaining on, a chain
// of blocks now runs to ~JIT_YIELD_NUMBER guest cycles inside compiled code,
// with zero trampoline round trips, before yielding back to jitRunArm9()/
// jitRunArm7() and the armInnerLoop interleave. No new scheduler code needed;
// armcpu_exec_block(quota) was already this mechanism, just gated off.
#ifndef JIT_ENABLE_CHAINING
#define JIT_ENABLE_CHAINING 1
#endif

// A4-P5: guarded dynamic-exit dispatch. A4-P1's chaining only ever covered
// static-target exits (B/BL, non-S data-proc->pc, fall-through) -- BX, LDR pc,
// POP{...,pc} and BLX(imm/reg) all deliberately keep returning to C every
// single time, because a dynamic exit's *target* is data-dependent. But its
// *mode* (THUMB vs ARM) is still a compile-time constant at every call site
// (see emitDynamicExit's callers), so a guarded inline cache is sound: redo
// the hash lookup fresh on every visit (no self-patch, since there is nothing
// fixed to patch), verify address AND cached mode both match, and only then
// jump straight into the target block -- any mismatch falls through to
// linkerReturnAddress exactly like today. This matters more than it sounds:
// profiling Phantom Hourglass's boot spin-loop (a compile-time-fixed
// 347-block footprint that A4-P1's chaining left untouched) found its hot
// path is almost entirely BX-terminated leaf calls -- every single iteration
// paid the full ExecuteJITTrace C round trip regardless of chaining, which is
// exactly the "0 bench frames" A4-P1 couldn't explain. See
// JITCache::linkerStubDynamicThumbAddress / ...ArmAddress (jit_cache.cpp).
#ifndef JIT_ENABLE_DYNAMIC_CHAINING
#define JIT_ENABLE_DYNAMIC_CHAINING 1
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
	bool thumbMode;            // true: THUMB front-end (jit_thumb.cpp, 16-bit,
	                           //   PC+=2). false: ARM front-end (jit_arm.cpp,
	                           //   32-bit, PC+=4). Set by jitCompileTrace().
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

	// ---- exits (shared by jit_thumb.cpp and jit_arm.cpp) ----
	void emitAddCycles(u32 n);   // r3 += n  (compile-time-known)
	// count -> r31 (resident instruction accumulator, flushed to out->instructions
	// by the trampoline). bailedOut / smcHit stores are emitted only when nonzero
	// (a clean exit leaves the caller's memset-zeroed fields alone).
	void emitResultMetadata(u32 count, u32 bailedOut, u32 smcHit = 0);
	void registerBailout(u32* branchPtr, JitBailoutCond cond);

	// Static-target exit: chain through the linker stub (self-patches on a hit).
	void emitStaticExit(u32 targetPC, u32 metaCount, u32 termCycles);
	// Dynamic-target exit: pcReg holds the runtime PC (already aligned). pcReg
	// must be a scratch (r10..r12) that survives the register flush. targetThumb
	// is the resume ISA this exit path leads to -- always a compile-time
	// constant at the call site (the branch already decided it; see B6/B7c/B4's
	// two-path bit0 dispatch), used both for the pipeline-register (r29) offset
	// and, with JIT_ENABLE_DYNAMIC_CHAINING, to pick the matching guarded stub.
	void emitDynamicExit(u8 pcReg, u32 metaCount, u32 termCycles, bool targetThumb);
	// Bail to the interpreter at ctx.currentPC (this instruction re-run there).
	void emitInterpreterBail(u32 metaCount);

	// ARM predication: emit a 0/1 "condition holds" value into PPC_R11 for one of
	// the 14 real ARM condition codes (0..13; not AL/NV). Clobbers r10, r11.
	void emitEvalCond(u8 cond);
};

// Emit one guest instruction at ctx.currentPC (opcode already fetched). Sets
// ctx.endBlock when the trace must stop here.
void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode);   // jit_thumb.cpp
void jitArmEmitOne(JitTraceCtx& ctx, u32 opcode);     // jit_arm.cpp

BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu, bool thumb);

#endif // DESMUME_JIT_ARM7

#endif // DESMUME_JIT_TRACE_H
