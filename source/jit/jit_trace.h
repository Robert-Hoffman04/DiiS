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

#if defined(DESMUME_JIT)

// Lazily allocates the ARM9 JIT slot (arena/block table/SMC tables) on first
// real ARM9 JIT dispatch instead of unconditionally in jitInit() -- see
// jit_trace.cpp's jitInit()/jitEnsureArm9() comments. Called from
// jitRunArm9() (jit_exec.cpp) when jitProfile[JIT_ARM9] is still null.
// Returns true once the slot is ready (idempotent), false if jitInit()
// hasn't run yet or the allocation itself fails.
bool jitEnsureArm9();

// ARM7 counterpart (F1): the ~3.5 MB ARM7 slot is lazy too, allocated from
// jitRunArm7() on the first dispatch with jitArm7Enabled set, so Interpreter
// mode allocates no JIT memory. Publishes the DS or GBA ARM7 profile per the
// last jitSetArm7GBAMode(). Same return contract as jitEnsureArm9(); a failed
// allocation is latched until jitShutdown().
bool jitEnsureArm7();

// --- arena / block budget ------------------------------------------------
#define JIT_MAX_WORDS              3072
// JIT_YIELD_NUMBER is the constant every block's entry guard compares r3
// against (ensureArena(): `cmpwi r3, JIT_YIELD_NUMBER; bge yield`). Since the
// dynamic quota, it is only the guard's fixed *zero point*,
// not the chain length: the trampoline starts r3 at (JIT_YIELD_NUMBER -
// quota) instead of 0 (ExecuteJITTrace's 4th arg, see jitQuotaStart()), so a
// chain yields after ~quota guest cycles with the emitted code unchanged, and
// the caller subtracts the start value back out of JITResult.cycles. Keeping
// it a compile-time constant keeps the per-block check a single cmpwi.
#ifndef JIT_YIELD_NUMBER
#define JIT_YIELD_NUMBER           64
#endif

// Runtime chain quota caps, in each core's *own* cycles. armInnerLoop hands
// jitRunArm9()/jitRunArm7() the cycles left until the next scheduled hardware
// event (s32next) and the quota is min(that, cap) -- so a chain never runs
// past an event/IRQ boundary by more than the one block it overshoots with
// anyway, but also isn't cut at a fixed 64 cycles when the scheduler has
// thousands to spare (the old fixed quota yielded ~8K times/frame on ARM9
// back to C for nothing).
// The cap still bounds how far one core can get ahead of the other in the
// lockstep interleave (whichever core is behind runs next), which is what
// IPC/shared-RAM handshakes see. ARM7 cycles are doubled onto the shared
// timeline, so an ARM7 cap of N is 2N ticks of ARM9-clock time. ARM9's 512
// was picked by measurement (128/256/512/1024 swept, 512
// keeps nearly all of 1024's win at half the inter-core drift); ARM7's 256 is
// the same 512 ticks of timeline, not yet tuned on an ARM7-heavy scene. Must
// stay < 32768 - 64 (the seed is a signed value compared by cmpwi).
#ifndef JIT_ARM9_QUOTA_CAP
#define JIT_ARM9_QUOTA_CAP         512
#endif
#ifndef JIT_ARM7_QUOTA_CAP
#define JIT_ARM7_QUOTA_CAP         256
#endif

// Clamp a scheduler budget into [1, cap] and return the r3 start value that
// makes the entry guard trip after that many cycles. The lower bound of 1 is
// load-bearing: a start value >= JIT_YIELD_NUMBER would make the *first*
// block yield before executing anything, which jitRunArm*() reads as a
// zero-progress bail and demotes the block to a "don't JIT" marker.
static inline s32 jitQuotaClamp(s32 budget, s32 cap)
{
	return budget < 1 ? 1 : (budget > cap ? cap : budget);
}
static inline u32 jitQuotaStart(s32 quota)
{
	return (u32)(JIT_YIELD_NUMBER - quota);
}
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
// (The fixed ~64-cycle quota has since been replaced by a runtime one
// seeded per dispatch -- see JIT_ARM9_QUOTA_CAP above.)
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

// In-block interpreter fallback. When the front end refuses
// an instruction (sets endBlock without emitting a terminator), the scanner
// used to end the block there -- and a PC whose *first* instruction is refused
// got a length-1 "don't JIT" marker, so every visit paid a full dispatch round
// trip (jitRunArm9() returns 0 -> armInnerLoop -> armcpu_exec for exactly one
// instruction -> back into jitRunArm9()). ~4,480 of ~9,700 ARM9 dispatches per
// frame on the benchmark scene were those marker hits, and nearly every clean
// chain end landed on one. With this on, the scanner instead emits a call to
// the interpreter's own opcode handler for that one instruction (via
// jitInterpFallback*() in jit_exec.cpp -- see emitInterpFallback()) and keeps
// compiling after it; the handler's result decides at run time whether the
// block continues, dynamically chains to a redirected PC, or returns to C.
// Every refused instruction on both cores and both ISAs is covered -- the
// safety net is the run-time exit test in jitInterpFallback(), not an opcode
// allow-list. 0 restores the old behaviour exactly (A/B switch).
#ifndef JIT_INTERP_FALLBACK
#define JIT_INTERP_FALLBACK 1
#endif

// Dead-flag elimination (jit_trace.cpp, jitFlagLiveness()). 0 turns
// it off (A/B switch).
#ifndef JIT_FLAG_ELIM
#define JIT_FLAG_ELIM 1
#endif

// THUMB spin-loop fast-forward (jit_trace.cpp, jitThumbSpinLoop()). 0 = off.
#ifndef JIT_SPIN_SKIP
#define JIT_SPIN_SKIP 1
#endif

// Worst-case words emitInterpFallback() emits -- comfortably inside either
// per-instruction reserve below, so the scanner's budget check needs no change.
#define JIT_INTERP_FALLBACK_WORDS  36

// Run one guest instruction through the interpreter's handler table, from
// compiled code. The caller (emitted code) has already stored the pinned
// register file + CPSR to cpu.R[]/CPSR; these set the interpreter pipeline
// state the handlers read (instruction / instruct_adr / next_instruction /
// R[15]), apply the condition check (ARM), call the handler, and return its
// cycle count in bits 0..29 plus two exit flags -- see jit_exec.cpp.
#define JIT_FALLBACK_EXIT_CHAIN  0x40000000u   // PC redirected, same ISA: dynamic-chain on
#define JIT_FALLBACK_EXIT_TO_C   0x80000000u   // must return to the dispatcher
u32 jitInterpFallbackArm9Arm(u32 opcode, u32 pc);
u32 jitInterpFallbackArm9Thumb(u32 opcode, u32 pc);
u32 jitInterpFallbackArm7Arm(u32 opcode, u32 pc);
u32 jitInterpFallbackArm7Thumb(u32 opcode, u32 pc);

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

	// Dead-flag elimination: bit (1 << JITF_x) set => that flag is
	// overwritten by a later instruction of this block before anything can read
	// it, so the current instruction need not compute it. Set per instruction by
	// jitCompileTrace() from a backward liveness pass (jitFlagLiveness()). The
	// flag helpers below consult it, so emitters only
	// need to check it where they can also pick a cheaper arithmetic form.
	u8   deadFlags;

	// THUMB spin-loop fast-forward (jitThumbSpinLoop()). Non-zero when the block
	// opens with an idempotent poll loop -- spinLoopLen instructions, the last a
	// Bcc back to startPC -- whose single load reads [R(spinBase) + spinImm]. The
	// Bcc's taken exit then adds the cycles of every further iteration the chain
	// would have run before its quota guard trips (see jit_thumb.cpp).
	u8   spinLoopLen;
	u8   spinBase;
	u8   spinImm;

	// THUMB CMP+Bcc fusion. fuseCmp is set by jitCompileTrace() for a CMP (imm8
	// or lo-reg) directly followed by a fusable Bcc whose fall-through needs no
	// flag: the CMP then emits nothing and parks its opcode in fusedCmpOp, and
	// the Bcc compares natively (cmpw/cmplw + bc) and materialises N/Z/C/V
	// only on its taken (exit) path -- see jit_thumb.cpp.
	bool fuseCmp;
	bool fusedCmpValid;
	u16  fusedCmpOp;
	bool flagDead(u8 flagIdx) const { return (deadFlags >> flagIdx) & 1; }
	bool cvDead() const { return flagDead(JITF_C) && flagDead(JITF_V); }

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

	// ---- fixed guest-register file: guest R0..R15 pinned to host r14..r29 ----
	// Every guest GPR is resident in a callee-saved host register for the whole
	// (possibly chained) trace; the trampoline (jit_trampoline.S) is the only
	// place they touch cpu.R[]. readReg/writeReg just return the pinned host
	// register (no load, no dirty tracking); the flush/invalidate helpers are
	// retained as no-ops so their ~30 call sites need no edit (same pattern as
	// ensureFlagsLoaded() after P12). lockedMask is vestigial -- no eviction.
	static u8 hostRegFor(u8 gbaReg) { return (u8)(14 + gbaReg); }   // R15 -> r29
	u8   readReg(u8 gbaReg, u32& lockedMask)  { (void)lockedMask; return hostRegFor(gbaReg); }
	u8   writeReg(u8 gbaReg, bool fullOverwrite, u32& lockedMask)
	     { (void)fullOverwrite; (void)lockedMask; return hostRegFor(gbaReg); }
	void flushDirtyRegisters()   {}              // resident: nothing to flush
	void emitDirtyRegisterFlush() {}
	void invalidateRegCache()    {}              // resident: no cache to drop

	// ---- guest memory via a C call to JitCpuProfile::slowRead/slowWrite ----
	// eaReg / valReg are host scratch (r10..r12). r3 (cross-block cycle accum)
	// and the packed flags are spilled/reloaded around the call.
	void emitMemPrologue();                       // flush state, save r3, drop r6
	void emitMemEpilogue();                       // restore r3
	void emitSlowLoad(u8 destReg, u8 eaReg, u32 size, bool signExtend);
	void emitSlowStore(u8 eaReg, u8 valReg, u32 size);
	void emitSmcCheckAndBail(u8 eaReg);           // store paths: page-flag guard

	// Differential-harness store journal: emit a call to JitCpuProfile::journalNote
	// (jitDiffJournalNote) recording `size` bytes at the address in eaReg before an
	// inline store mutates them. No-op unless the profile sets journalNote (only a
	// JIT_DIFFERENTIAL_TESTING build does). Caller must have flushed dirty
	// registers; r3 is saved/restored around the call. Clobbers r3, r4, r5, r12.
	void emitJournalNote(u8 eaReg, u32 size);

	// ---- P14/P15 cached page descriptors --------------------------------
	// Guard EA (PPC_R12) against the descriptor page window (+ optionally that
	// EA + spanBytes stays in the same 1 MB page), then resolve on the
	// fall-through: PPC_R10 <- hostBase, PPC_R11 <- (EA & mask) with low bits
	// cleared to `alignMe` (29 => &~3, 30 => &~1, 31 => none). Each miss test
	// is a forward branch left for the caller to aim at its slowRead C path:
	// the slots go to missSlots[] (room for 2) and the count is returned.
	// Clobbers r10, r11; EA stays in r12.
	int  emitPageResolve(u32 spanBytes, u8 alignMe, u32** missSlots);

	// ---- P16 ARM9 two-region inline guard --------------------------------
	// EA must be in PPC_R12 (any alignment). Emits a runtime guard for the two
	// inline-able ARM9 data regions -- main RAM (0x02xxxxxx) and the CP15 DTCM
	// window (base baked from *arm9DtcmRegionPtr at emit time). On a hit the
	// emitted code sets PPC_R10 = host base, PPC_R11 = (EA & regionMask) cleared
	// to `alignMe`, and branches forward; the caller patches those branch slots
	// (returned in fastSlots[0..count-1]) to its inline-load block. A miss (or,
	// for spanBytes > 0, a run that straddles a region edge / 1 MB page) falls
	// through -- the caller emits the slowRead C path there. Clobbers r10, r11;
	// EA stays in r12. Returns the number of fast-branch slots written (1-3).
	// withItcm (loads only) adds a third region, the 32 KB
	// ITCM mirrored over 0x00000000-0x01FFFFFF, tested on main RAM's miss
	// branch so a main-RAM hit costs the same as before; its fast slot comes
	// last. Stores pass false (ITCM holds JIT code; they keep slowWrite).
	int  emitArm9RegionGuard(u32 size, u8 alignMe, u32 spanBytes, u32** fastSlots, bool withItcm,
	                         void (*onHit)(JitTraceCtx&, void*, int) = nullptr, void* hitArg = nullptr);
	// onHit (optional): instead of leaving a fast-branch slot, the guard calls
	// onHit(ctx, hitArg, region) at each hit point (region 0 DTCM, 1 main RAM,
	// 2 ITCM) with r10 = host base, r11 = aligned in-region offset, EA in r12,
	// and the callback emits the access itself (ending in its own branch to
	// the caller's join point). Returns 0 then.

	// Full ARM9 single load: EA in PPC_R12. Unconditional dirty flush, then the
	// region guard -> inline lwbrx (main RAM / DTCM / ITCM) or the slowRead C call
	// (every other region, no interpreter round-trip); result -> gpr[rd] and the
	// register cache is invalidated. When `writeback`, the caller has stashed the
	// new base value at 104(r1) and it is committed to gpr[rn] afterwards. Does
	// not end the block. Clobbers r10, r11, r12.
	void emitArm9Load(u8 rd, u32 size, bool signExt, bool wordRotate, bool writeback, u8 rn);

	// ARM9 inline block load (LDM / POP / LDMIA, non-pc). Low guest address of
	// the contiguous word run in PPC_R12; regs the ascending destination list
	// (0..14), n its length. Region guard covering the whole run -> n sequential
	// inline lwbrx, or the per-word slowRead C loop (no round-trip); results ->
	// gpr slots, register cache invalidated. The caller still owns any base
	// writeback (stash + post writeReg), exactly as the P15 path. Clobbers
	// r10, r11; the low EA is restored to r12 on return.
	void emitArm9BlockLoad(const u8* regs, u32 n);

	// Full ARM9 single store: EA in PPC_R12, the value already stashed at 100(r1)
	// (and, when `writeback`, the new base at 104(r1)). Unconditional dirty flush,
	// then the region guard -> inline stwbrx/sthbrx/stbx into main RAM or DTCM
	// (each behind its own SMC-page guard for main RAM + differential journal
	// note), or the slowWrite C call for every other region (no interpreter
	// round-trip). Register cache invalidated; writeback committed to gpr[rn].
	// Does not end the block. Clobbers r3, r4, r5, r10, r11, r12.
	void emitArm9Store(u32 size, bool writeback, u8 rn, u8 hVal);
	void emitArm9StoreJournaled(u32 size, bool writeback, u8 rn);   // differential builds

	// ARM9 inline block store (STM / PUSH / STMIA, non-pc). Low guest address of
	// the contiguous word run in PPC_R12; regs the ascending source list (0..14),
	// n its length. Region guard covering the whole run -> n sequential inline
	// stwbrx (SMC-guarded for main RAM, one journal note for the span), or the
	// per-word slowWrite C loop (no round-trip). Register cache invalidated; the
	// caller still owns any base writeback. Clobbers r3, r4, r5, r10, r11, r12;
	// the low EA is restored to r12 on return.
	void emitArm9BlockStore(const u8* regs, u32 n);

	// ---- P14 inline RAM load via cached page descriptors ------------------
	// eaReg MUST be PPC_R12 and holds the runtime EA (any alignment). Emits a
	// page-window guard, then resolves the EA through JitCpuProfile's
	// descriptor table and loads `size` bytes into rd's host register with
	// emitSlowLoad's byte-swap / sign-extend / unaligned-word-rotate semantics.
	// No memory prologue, no C call on a hit. Out of window (I/O, VRAM, ...)
	// does the same access through the slowRead C call in place and the block
	// continues (was an interpreter bail). Returns false without emitting
	// anything when the profile has no descriptor table -- the caller then emits
	// its own slow path. wordRotate applies OP_LDR's unaligned-word ROR (ARM
	// callers pass true for a word load; THUMB callers pass false to match
	// jit_thumb.cpp's non-rotating slow path). Clobbers r10, r11, r12.
	bool emitInlineLoad(u8 rd, u8 eaReg, u32 size, bool signExt, bool wordRotate, u32& lockedMask);

	// ---- P15 inline sequential block load (LDM / POP / LDMIA) ------------
	// eaReg MUST be PPC_R12 and holds the *low* guest address of the contiguous
	// word run (callers already fold IA/IB/DA/DB into this). regs is the
	// ascending list of destination guest registers (0..14, never 15), n its
	// length (>= 1). Emits a single page-window guard covering the whole run,
	// one descriptor resolve, then n sequential lwbrx into the registers' host
	// slots. Out of window, or a run that straddles a 1 MB page, takes a
	// per-word slowRead C loop instead and the block continues (was one
	// interpreter bail for the whole instruction). The low EA
	// is back in r12 on return either way. LDM/LDMIA word loads do NOT rotate
	// an unaligned base (matches OP_L_IA). Returns false without emitting when
	// disabled. Clobbers r10, r11.
	bool emitInlineBlockLoad(const u8* regs, u32 n, u8 eaReg, u32& lockedMask);

	// ---- exits (shared by jit_thumb.cpp and jit_arm.cpp) ----
	void emitAddCycles(u32 n);   // r3 += n  (compile-time-known)
	// count -> r31 (resident instruction accumulator, flushed to out->instructions
	// by the trampoline). bailedOut / smcHit stores are emitted only when nonzero
	// (a clean exit leaves the caller's memset-zeroed fields alone).
	void emitResultMetadata(u32 count, u32 bailedOut, u32 smcHit = 0);
	void registerBailout(u32* branchPtr, JitBailoutCond cond);

	// Static-target exit: chain through the linker stub (self-patches on a hit).
	void emitStaticExit(u32 targetPC, u32 metaCount, u32 termCycles);
	// The tail every static exit ends with, after its cycles/metadata:
	// `bl linkerStub ; .long targetPC`. The stub reads the target from the word
	// at LR (so r4 needs no lis/ori pair) and, on a hit, patches the bl into a
	// direct `b target`; on a miss it returns to C with r4 = target. Guest R15's
	// pinned r29 is not set: C rewrites cpu.R[15] from the returned nextPC
	// (jitPointPipeline()) after every trace, and no compiled code reads r29
	// as an operand (pc reads are compile-time constants).
	void emitChainTail(u32 targetPC);
	// Dynamic-target exit: pcReg holds the runtime PC (already aligned). pcReg
	// must be a scratch (r10..r12) that survives the register flush. targetThumb
	// is the resume ISA this exit path leads to -- always a compile-time
	// constant at the call site (the branch already decided it; see B6/B7c/B4's
	// two-path bit0 dispatch), used both for the pipeline-register (r29) offset
	// and, with JIT_ENABLE_DYNAMIC_CHAINING, to pick the matching guarded stub.
	void emitDynamicExit(u8 pcReg, u32 metaCount, u32 termCycles, bool targetThumb);
	// Bail to the interpreter at ctx.currentPC (this instruction re-run there).
	void emitInterpreterBail(u32 metaCount);
	// Execute the refused instruction at ctx.currentPC in place through the
	// interpreter's handler (JIT_INTERP_FALLBACK, see above) and fall through
	// to the next instruction when it neither redirected control nor changed
	// anything a compiled continuation can't absorb. Does not end the block
	// and adds nothing to cyclesAccum (the handler's own cycle count goes
	// straight into r3 at run time).
	void emitInterpFallback(u32 opcode);

	// ARM predication: emit a 0/1 "condition holds" value into PPC_R11 for one of
	// the 14 real ARM condition codes (0..13; not AL/NV). Clobbers r10, r11.
	void emitEvalCond(u8 cond);
};

// Emit one guest instruction at ctx.currentPC (opcode already fetched). Sets
// ctx.endBlock when the trace must stop here.
void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode);   // jit_thumb.cpp
void jitThumbEmitCmpFlags(JitTraceCtx& ctx, u16 cmpOpcode);        // jit_thumb.cpp (CMP+Bcc fusion)
void jitArmEmitOne(JitTraceCtx& ctx, u32 opcode);     // jit_arm.cpp

BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu, bool thumb);

#endif // DESMUME_JIT

#endif // DESMUME_JIT_TRACE_H
