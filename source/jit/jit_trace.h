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

// Block length cut. A block used to end once its *emitted* PPC size crossed
// JIT_MAX_WORDS minus the reserves below, which made guest timing (where the
// chain's quota-yield checks fall) depend on how big the generated code is.
// The cut is now decided from the guest code alone, before anything is
// emitted: jitCutWeight() (jit_trace.cpp) gives every guest instruction a
// weight that models the PPC words the pre-thunk emitters produced for it,
// and the block ends before the first instruction at which the running sum
// (plus the 2-word entry guard) exceeds these budgets -- the same thresholds
// the emitted-size check used, so blocks are cut where they always were
// (checked on every block the gate scenarios compile) while code size is
// free to change. registerBailout() has no callers, so its per-stub budget
// term was always 0 and is gone.
#define JIT_CUT_BUDGET_ARM         (JIT_MAX_WORDS - JIT_EPILOGUE_RESERVE_WORDS - JIT_MAX_INSTR_RESERVE_WORDS_ARM)
#define JIT_CUT_BUDGET_THUMB       (JIT_MAX_WORDS - JIT_EPILOGUE_RESERVE_WORDS - JIT_MAX_INSTR_RESERVE_WORDS)
// Arena reserved per compile (the unused tail is rewound): with the cut no
// longer watching the emitted size, room for the worst case of every
// instruction, the epilogue and the maximum deferred-bailout stubs.
#define JIT_BLOCK_RESERVE_WORDS    (JIT_TRACE_MAX_INSTRUCTIONS * JIT_MAX_INSTR_RESERVE_WORDS_ARM \
                                    + JIT_EPILOGUE_RESERVE_WORDS + JIT_MAX_BAILOUTS * JIT_BAILOUT_STUB_WORDS)

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

// Spin-loop fast-forward, THUMB and ARM (jit_trace.cpp, jit*SpinLoop()). 0 = off.
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

// Out-of-line ("cold") stubs, emitted after a block's last exit by
// JitTraceCtx::emitColdStubs(). An access site keeps only its predicted case
// inline and branches here for everything else (and to the SMC bail): the
// stub calls a shared memory thunk (JitMemThunk, jit_cache.h) or does the
// rare full sequence, then branches back. Branch slots are placeholders
// holding their condition with a zero displacement.
enum JitColdType {
	JCOLD_LOAD,        // bl LD thunk ; mr reg, r10 ; b back
	JCOLD_STORE,       // mr r11, reg ; bl ST thunk ; beq back ; SMC bail
	JCOLD_BLOCKLOAD,   // per-word LD thunk loop, r12 = low EA again ; b back
	JCOLD_BLOCKSTORE,  // the whole-run region decision + its SMC bails
	JCOLD_SMCBAIL      // SMC bail only (emitSmcCheckAndBail)
};
#define JIT_MAX_COLD 128   // <= 3 per guest instruction
struct JitColdStub {
	u8   type, thunk, reg, n;
	u8   nMiss, nSmc, nSmcHigh;
	u8   regs[16];         // block transfers: host registers, ascending address
	u32  span;             // block stores: 4 * (n - 1)
	u32* miss[6];          // -> stub entry
	u32* smc[3];           // -> SMC bail, r12 = the refused EA (block store: hot 1 + cold 2)
	u32* smcHigh[3];       // -> SMC bail reporting r12 + span (block store: hot 1 + cold 2)
	u32* back;             // resume point in the hot path
	u32  cycles, icount, pc;   // the bail's exit metadata
};

// Region an ARM9 access site inlines (JitTraceCtx::memPredict).
enum { JIT_MEMP_DTCM, JIT_MEMP_MAIN, JIT_MEMP_ITCM };

// -DJIT_CODE_STATS (JITDEFS; compiled out by default): generated-code footprint
// accounting. Emitters bracket their sequences with JIT_STAT_SCOPE(ctx, cat,
// cold); words not inside any scope count as JCS_OTHER. A cold scope marks a
// sequence that is executed rarely (slow C-call paths, bail stubs); nested
// scopes charge their words to the innermost category. Every block also gets
// a 4-word execution counter at its entry (JCS_STATCTR, excluded from every
// reported size), which jitCodeStatsTick() (main.cpp bench_tick) turns into a
// per-frame executed-code working set. Report: sd:/jitstats.log.
#ifdef JIT_CODE_STATS
enum JitCodeStatCat {
	JCS_ENTRY,      // quota guard at the block entry
	JCS_MEMGUARD,   // inline memory region / page-window checks
	JCS_MEMHIT,     // inline access + branch to the join
	JCS_MEMSLOW,    // slowRead/slowWrite C-call sequences (+ stash / rotate fix-up)
	JCS_SMCGUARD,   // inline SMC page-flag test on stores
	JCS_SMCBAIL,    // SMC bail sequence
	JCS_FLAGS,      // N/Z/C/V materialisation
	JCS_PRED,       // ARM predication / Bcc condition tests
	JCS_EXIT,       // block exits: cycles, icount, chain tail / dynamic dispatch
	JCS_YIELD,      // quota-yield stub
	JCS_FALLBACK,   // interpreter fallback calls
	JCS_SPIN,       // spin-loop fast-forward
	JCS_OTHER,      // everything else (ALU, address arithmetic, ...)
	JCS_PAD,        // 32-byte block alignment padding
	JCS_STATCTR,    // the stats build's own entry counter (not reported)
	JCS_N
};
#endif

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
	// Bcc back to startPC -- whose single load reads [R(spinBase) + spinOff]. The
	// Bcc's taken exit then adds the cycles of every further iteration the chain
	// would have run before its quota guard trips (see jit_thumb.cpp).
	u8   spinLoopLen;
	u8   spinBase;
	s32  spinOff;
	// At a taken back-edge to startPC: emit the fast-forward when this block is
	// a detected spin loop and this is its closing branch (no-op otherwise).
	void emitSpinSkip(u32 targetPC);

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

	JitColdStub cold[JIT_MAX_COLD];
	u32         coldCount;
	bool        coldOverflow;                     // the block cannot be used (see emitColdStubs())
	JitColdStub& addCold(u8 type);                // records cycles / icount / pc for its bail
	void emitColdStubs();                         // jitCompileTrace(), after the last exit

#ifdef JIT_CODE_STATS
	u32  statWords[JCS_N];     // words charged per category (JCS_OTHER filled at finalize)
	u32  statCold;             // words inside cold scopes
	u32  statColdCat[JCS_N];   // of which per category (hot bytes by category = statWords - statColdCat)
	u32  statChild;            // words of nested scopes, for the active scope
	u8   statColdDepth;
	u32* statCounter;          // this block's execution counter (jit_trace.cpp)
#endif

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

	// ARM7 (DS profile): the cache's shared thunks (JitMemThunk) carry the
	// SMC guard + slowWrite sequence instead of every store site. True only
	// when this cache's thunks were built for this profile and it has no TCM
	// regions (the GBA ARM7 profile shares the cache: its sites stay inline).
	bool arm7Thunks() const { return cache.thunkProfile == &cpu && !cpu.arm9DtcmBase; }
	// EA in PPC_R12, value in valReg: `mr r11, val ; bl ST thunk ; bne` to a
	// cold SMC bail (resume at this instruction, r12 = the EA), exactly the
	// old prologue / page test / slowWrite / epilogue. No prologue or epilogue
	// needed around it. Clobbers r0, r4..r12, CTR, cr0.
	void emitArm7ThunkStore(u8 valReg, u32 size);
	// Block-store word after the site's own SMC guard: `mr r11, val ; bl
	// STSLOW thunk` (EA in PPC_R12; a plain slowWrite, r3 preserved).
	void emitArm7ThunkStoreWord(u8 valReg);

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

	// ---- P16 ARM9 accesses: predicted region inline, the rest via thunks ----
	// The ARM9 data regions the JIT can access directly are the CP15 DTCM window
	// (base baked from *arm9DtcmRegionPtr at emit time; a TCM move flushes the
	// cache), main RAM (0x02xxxxxx) and, for loads, the 32 KB ITCM mirrored over
	// 0x00000000-0x01FFFFFF, with that priority. A site tests only the region
	// memPredict names (main RAM with a DTCM-exclusion test when the window
	// overlays it, so the priority holds) and branches to a cold stub for any
	// other address; the stub's thunk resolves DTCM -> main -> ITCM -> C call
	// exactly as the old inline guard did.

	// Full ARM9 single load: EA in PPC_R12. Predicted-region test -> inline
	// lwbrx/lhbrx/lbzx straight into rd's pinned register; anything else (and
	// an unaligned word) -> cold stub -> LD thunk. When `writeback`, the caller
	// has stashed the new base value at 104(r1) and it is committed to gpr[rn]
	// afterwards. Does not end the block. Clobbers r0, r4..r12.
	void emitArm9Load(u8 rd, u32 size, bool signExt, bool wordRotate, bool writeback, u8 rn);

	// Region the next ARM9 access inlines (JIT_MEMP_*): DTCM for a stack-based
	// access, main RAM otherwise; the region of a compile-time-constant EA.
	// Callers set it just before the access; the scanner resets it to DTCM
	// before every instruction. When the DTCM window cannot be matched with an
	// immediate compare (unreachable, or above 0x1FFFC000) DTCM falls back to
	// main RAM.
	u8   memPredict;
	// JIT_MEMP_* for a constant EA (baked DTCM window first), or MAIN when it
	// is in none of the three regions.
	u8   arm9RegionOf(u32 ea) const;

	// ARM9 inline block load (LDM / POP / LDMIA). Low guest address of the
	// contiguous word run in PPC_R12; regs the ascending destination list, n its
	// length. Predicted region covering the whole run -> n sequential inline
	// lwbrx; otherwise a cold per-word LD-thunk loop (each word resolved on its
	// own -- the same values the old whole-run guard's slowRead loop produced).
	// The caller still owns any base writeback. Clobbers r0, r4..r11; the low
	// EA is in r12 on return.
	void emitArm9BlockLoad(const u8* regs, u32 n);

	// Full ARM9 single store: EA in PPC_R12, the value in hVal (a pinned guest
	// register; when `writeback`, the new base is at 104(r1)). Predicted-region
	// test (+ for main RAM the SMC page test, whose hit goes to the site's cold
	// SMC bail) -> inline stwbrx/sthbrx/stbx (differential builds journal it
	// first); anything else -> cold stub -> ST thunk (DTCM / SMC-guarded main
	// RAM / SMC guard + slowWrite). Writeback committed to gpr[rn]. Does not end
	// the block. Clobbers r0, r4..r12.
	void emitArm9Store(u32 size, bool writeback, u8 rn, u8 hVal);

	// ARM9 inline block store (STM / PUSH / STMIA, non-pc). Low guest address of
	// the contiguous word run in PPC_R12; regs the ascending source list (0..14),
	// n its length. Predicted region covering the whole run (main RAM: SMC test
	// of both span ends) -> n sequential inline stwbrx, one journal note for the
	// span. Any other case -> a cold stub that makes the old whole-run decision
	// (whole run in DTCM / in one main-RAM page / else SMC test of both ends +
	// a per-word slowWrite), so exactly the same stores take the slowWrite path
	// (it also invalidates the ARM7 cache; the inline path does not). The caller
	// still owns any base writeback. Clobbers r0, r4..r11; the low EA is in r12
	// on return.
	void emitArm9BlockStore(const u8* regs, u32 n);

	// ---- P14 inline RAM load via cached page descriptors ------------------
	// eaReg MUST be PPC_R12 and holds the runtime EA (any alignment). Emits a
	// page-window guard, then resolves the EA through JitCpuProfile's
	// descriptor table and loads `size` bytes into rd's host register with
	// emitSlowLoad's byte-swap / sign-extend / unaligned-word-rotate semantics.
	// No memory prologue, no C call on a hit. Out of window (I/O, VRAM, ...)
	// does the same access through the slowRead C call in place and the block
	// continues (was an interpreter bail); for the DS ARM7 that call is the
	// cache's LD thunk, reached from a cold stub at the block's tail. Returns false without emitting
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
	// per-word slowRead C loop instead (DS ARM7: a cold stub calling the LD
	// thunk per word) and the block continues (was one
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

	// ARM predication / Bcc skip: emit the condition test and a placeholder branch
	// (displacement 0) that is taken when `cond` FAILS; patchCondSkip() aims
	// it at the current emit position. Every condition is a short record-form
	// test of the packed flags (1..3 words, see jit_trace.cpp) + the branch.
	// Clobbers r11 and cr0.
	//
	// Condition runs (ARM only): patchCondSkip() remembers the skip it just
	// aimed at the current position. If the very next instruction is
	// predicated, emitted nothing before its emitCondSkip() and the previous one
	// cannot have changed the flags (condRunOk, set by jitCompileTrace()), then
	//   same cond:    no test at all -- the previous skip is re-aimed past this
	//                 instruction too (returned as this instruction's slot);
	//   inverse cond: one `b` (the previous body's way over this one) and the
	//                 previous skip is aimed just past it, into this body.
	// condUndoSlot records a re-aimed skip so a refused (rewound) instruction
	// can point it back at the rewind mark.
	u32* emitCondSkip(u8 cond);
	void patchCondSkip(u32* slot)
	{
		aimSkip(slot, emitPtr);
		condRunSlot = (slot == condLastSlot) ? slot : nullptr;
		condRunEnd = emitPtr; condRunCond = condLastCond;
	}
	static void aimSkip(u32* slot, u32* target)
	{
		const u32 d = (u32)((target - slot) * 4);
		if ((*slot >> 26) == 18) *slot = (*slot & ~0x03FFFFFCu) | (d & 0x03FFFFFCu);   // b
		else                     *slot = (*slot & ~0xFFFCu) | (d & 0xFFFCu);            // bc
	}
	u32* condRunSlot;          // last patched skip, taken iff condRunCond fails
	u32* condRunEnd;           // ...and where it pointed when patched
	u8   condRunCond;
	bool condRunOk;            // the previous instruction cannot write the flags
	u32* condLastSlot;         // slot emitCondSkip() last returned...
	u8   condLastCond;         // ...and the condition it skips on
	u32* condUndoSlot;         // skip re-aimed by the current instruction (rewind fix-up)
};

#ifdef JIT_CODE_STATS
struct JitStatScope {
	JitTraceCtx& c; u8 cat; bool cold; u32* start; u32 savedChild;
	JitStatScope(JitTraceCtx& c_, u8 cat_, bool cold_)
		: c(c_), cat(cat_), cold(cold_), start(c_.emitPtr), savedChild(c_.statChild)
	{ c.statChild = 0; if (cold) c.statColdDepth++; }
	~JitStatScope() {
		const u32 total = (u32)(c.emitPtr - start);
		c.statWords[cat] += total - c.statChild;
		if (c.statColdDepth) c.statColdCat[cat] += total - c.statChild;
		if (cold && --c.statColdDepth == 0) c.statCold += total;
		c.statChild = savedChild + total;
	}
};
#define JIT_STAT_CAT2(a, b) a##b
#define JIT_STAT_CAT(a, b)  JIT_STAT_CAT2(a, b)
#define JIT_STAT_SCOPE(ctx, cat, cold) JitStatScope JIT_STAT_CAT(jitStatScope_, __LINE__)((ctx), (cat), (cold))
void jitCodeStatsTick(u32 frame);                   // main.cpp bench_tick
void jitCodeStatsOnFlush(const JITCache* cache);    // jit_cache.cpp flushCache
#else
#define JIT_STAT_SCOPE(ctx, cat, cold) ((void)0)
#endif

// Emit one guest instruction at ctx.currentPC (opcode already fetched). Sets
// ctx.endBlock when the trace must stop here.
void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode);   // jit_thumb.cpp
void jitThumbEmitCmpFlags(JitTraceCtx& ctx, u16 cmpOpcode);        // jit_thumb.cpp (CMP+Bcc fusion)
void jitArmEmitOne(JitTraceCtx& ctx, u32 opcode);     // jit_arm.cpp

BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu, bool thumb);

#endif // DESMUME_JIT

#endif // DESMUME_JIT_TRACE_H
