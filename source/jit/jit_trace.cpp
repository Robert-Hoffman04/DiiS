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
#include "../perf_zones.h"
#include "../harness/harness.h"

#if defined(DESMUME_JIT)

#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <ogc/cache.h>

// The JIT heap canary + "minefield" poison-block checks are heap-corruption
// detection, so per plan §3.0 they belong with crash reporting: gated on the
// master harness flag (+ HARNESS_CRASH) and sunk through harness_send(
// PKT_CRASH, ...) rather than a hardcoded sd:/jit.log open. Without the flag
// the whole apparatus compiles to nothing and jitCheckCanaries() is a no-op,
// so a release JIT build is untouched.
#if defined(DESMUME_HARNESS) && defined(HARNESS_CRASH)
  #define JIT_CANARY_WATCH 1
#endif

#ifdef JIT_CANARY_WATCH
#include <stdio.h>
static const size_t s_jitCanaryPad = 32;
#else
static const size_t s_jitCanaryPad = 0;
#endif

// -DJIT_MEM_ACCOUNT: one-off tuning instrumentation. Prints
// which libogc arena the JIT backing store (arena + block table + SMC tables)
// actually comes out of, and how much of that pool is left afterwards, so a
// table-growth experiment can be sized against real MEM1 headroom rather than
// the assumption that these allocations land in the near-idle MEM2. Not
// defined by any build config; compiles out entirely otherwise.
#ifdef JIT_MEM_ACCOUNT
#include <stdio.h>
#include <ogc/system.h>
#include <ogc/machine/processor.h>
static void jitMemAccountReport(const char* when)
{
	// arena1/arena2 = the still-unclaimed tail of each physical pool (the
	// default malloc heap is carved from arena1 at startup, so this moves
	// only if something calls SYS_SetArenaX). mallinfo = the malloc heap
	// itself: uordblks is live bytes, fordblks free bytes.
	struct mallinfo mi = mallinfo();
	FILE* f = fopen("sd:/jitmem.log", "a");
	if (!f) return;
	fprintf(f, "[jitmem] %-6s  SYS_arena1=%d KiB  SYS_arena2=%d KiB  |  "
	           "malloc: arena=%d KiB  used=%d KiB  free=%d KiB\n",
	        when, SYS_GetArena1Size() >> 10, SYS_GetArena2Size() >> 10,
	        mi.arena >> 10, mi.uordblks >> 10, mi.fordblks >> 10);
	fclose(f);
}
// Steady-state samples, driven from bench_tick() (main.cpp): after the game
// has allocated its textures / audio / etc, so the remaining MEM1 headroom
// is real rather than the value right after boot.
void jitMemAccountTick(u32 frame)
{
	if (frame == 300)  jitMemAccountReport("f300");
	if (frame == 1200) jitMemAccountReport("f1200");
}
#endif

// =========================================================================
// Lifecycle
// =========================================================================
JitCpuProfile* jitProfile[2] = { nullptr, nullptr };

extern JitCpuProfile* jitBuildArm7Profile();      // jit_arm7_profile.cpp
extern JitCpuProfile* jitBuildArm9Profile();      // jit_arm9_profile.cpp
extern JitCpuProfile* jitBuildArm7GBAProfile();   // jit_arm7gba_profile.cpp

// roadmap #20 (GBA compat), §12.3 step 5: the DS and GBA ARM7 profiles both
// live on the one JIT_ARM7 slot (same jitCacheArm7 arena/block table/SMC
// registry -- only one boot mode is ever live at a time). jitInit() builds
// both once; jitSetArm7GBAMode() below just swaps which one jitRunArm7()
// sees via jitProfile[JIT_ARM7], which it already re-reads fresh on every
// call. jitInit() itself cannot gate on gameInfo.isGBA -- it runs exactly
// once, from NDS_Init(), before any ROM is loaded and before that flag can
// ever be meaningfully true.
static JitCpuProfile* s_arm7DsProfile  = nullptr;
static JitCpuProfile* s_arm7GbaProfile = nullptr;
// Which of the two jitRunArm7() should see -- remembered separately from
// jitProfile[JIT_ARM7], which doubles as the "slot allocated" flag and stays
// null until jitEnsureArm7() runs (F1: the ARM7 slot is lazy now too).
static bool           s_arm7GbaMode    = false;
static bool           s_arm7AllocFailed = false;   // jitEnsureArm7() OOM latch

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
#ifdef JIT_CANARY_WATCH

#define JIT_CANARY_BYTES 32
static u8 s_canaryPattern[JIT_CANARY_BYTES];
struct CanarySlot { void* base; size_t realSize; const char* name; };
static CanarySlot s_canaries[8];
static int s_canaryCount = 0;
static bool s_canaryTripped = false;

// §3.0: heap-corruption trips go out as PKT_CRASH on the harness transport
// (wii_control.py symbolicates and writes crash_N.txt), replacing the old
// hardcoded sd:/jit.log open.
static void jitCanaryEmit(const char* line)
{
	harness_send(HARNESS_PKT_CRASH, line, (u32)strlen(line));
}

static void jitCanaryArm(void* buf, size_t realSize, const char* name)
{
	if (!buf || s_canaryCount >= 8) return;
	memcpy((u8*)buf + realSize, s_canaryPattern, JIT_CANARY_BYTES);
	s_canaries[s_canaryCount++] = { buf, realSize, name };
}

#ifdef JIT_HEAP_WATCH
// §16 heap minefield: scatter poisoned blocks through the general heap at init
// so a wild host store from JIT codegen (the class of bug behind the predicated
// -Bcc corruption, which smashed newlib _malloc_r state and which the 4 buffer
// canaries above never detected) has a decent chance of landing on a mine we
// can then name. Each mine is filled with a byte derived from its own address.
enum { JIT_MINE_COUNT = 96, JIT_MINE_BYTES = 4096 };
static u8*  s_mines[JIT_MINE_COUNT];
static int  s_mineCount = 0;
static bool s_mineTripped = false;

static u8 jitMineByte(const u8* p, size_t off)
{
	uintptr_t a = (uintptr_t)p + off;
	return (u8)(0xA5 ^ (a >> 4) ^ (a >> 12));
}

static void jitArmMinefield()
{
	for (int i = 0; i < JIT_MINE_COUNT; i++) {
		u8* m = (u8*)malloc(JIT_MINE_BYTES);
		if (!m) break;
		for (size_t k = 0; k < JIT_MINE_BYTES; k++) m[k] = jitMineByte(m, k);
		s_mines[s_mineCount++] = m;
	}
}

static void jitCheckMinefield()
{
	if (s_mineTripped || s_mineCount == 0) return;
	// One full mine per call, round-robin -- full sweep every s_mineCount calls,
	// cheap enough to run on the 1K-dispatch poll without throttling the soak.
	static u32 rot = 0;
	int i = (int)(rot++ % (u32)s_mineCount);
	const u8* m = s_mines[i];
	for (size_t k = 0; k < JIT_MINE_BYTES; k++) {
		if (m[k] != jitMineByte(m, k)) {
			s_mineTripped = true;
			char buf[256];
			int n = snprintf(buf, sizeof buf,
			        "[jit] !!! HEAP MINE HIT mine=%d base=%p off=%u got=%02x want=%02x ctx:",
			        i, (void*)m, (unsigned)k, m[k], jitMineByte(m, k));
			size_t s = k > 8 ? k - 8 : 0;
			for (size_t j = s; j < s + 24 && j < JIT_MINE_BYTES && n > 0 && n < (int)sizeof buf - 4; j++)
				n += snprintf(buf + n, sizeof buf - n, " %02x", m[j]);
			jitCanaryEmit(buf);
			return;
		}
	}
}
#endif

void jitCheckCanaries()
{
#ifdef JIT_HEAP_WATCH
	jitCheckMinefield();
#endif
	if (s_canaryTripped) return;
	for (int i = 0; i < s_canaryCount; i++) {
		u8* tail = (u8*)s_canaries[i].base + s_canaries[i].realSize;
		if (memcmp(tail, s_canaryPattern, JIT_CANARY_BYTES) != 0) {
			s_canaryTripped = true;
			char buf[256];
			int n = snprintf(buf, sizeof buf,
			        "[jit] !!! CANARY TRIPPED buf=%s base=%p realSize=%u tail=%p bytes:",
			        s_canaries[i].name, s_canaries[i].base,
			        (unsigned)s_canaries[i].realSize, (void*)tail);
			for (int b = 0; b < JIT_CANARY_BYTES && n > 0 && n < (int)sizeof buf - 4; b++)
				n += snprintf(buf + n, sizeof buf - n, " %02x", tail[b]);
			jitCanaryEmit(buf);
			return;
		}
	}
}

#else // !JIT_CANARY_WATCH -- release JIT build: no-op stubs, nothing emitted

static inline void jitCanaryArm(void*, size_t, const char*) {}
void jitCheckCanaries() {}

#endif // JIT_CANARY_WATCH

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
	s_arm7DsProfile = s_arm7GbaProfile = nullptr;
	s_arm7AllocFailed = false;
	s_initDone = false;
}

static bool jitInitSlot(int i, size_t arenaBytes, JITCache& cache, JitCpuProfile* profile)
{
	// GO-FIX-PH: over-allocate by the canary pad on every buffer so a canary
	// can be armed right after each one's *logical* end -- the size passed to
	// cache.initialize() below is unchanged, so the JIT's own bounds checks
	// (allocateJITMemory() vs arenaSize, etc.) see exactly the same capacity
	// as before. s_jitCanaryPad is 0 unless JIT_CANARY_WATCH (§3.0), so a
	// release build allocates exactly what it always did.
	size_t blockTableBytes  = BLOCK_TABLE_SLOTS * sizeof(BasicBlock);   // 2-way: 2 slots/set
	size_t smcRegistryBytes = SMC_MAP_SIZE * sizeof(BasicBlock*);
	size_t smcFlagsBytes    = SMC_MAP_SIZE;

	s_arena[i]        = (u32*)        memalign(32, arenaBytes        + s_jitCanaryPad);
	s_blockTable[i]   = (BasicBlock*) memalign(16, blockTableBytes   + s_jitCanaryPad);
	s_smcRegistry[i]  = (BasicBlock**)memalign(32, smcRegistryBytes  + s_jitCanaryPad);
	s_smcPageFlags[i] = (u8*)         memalign(32, smcFlagsBytes     + s_jitCanaryPad);

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

#ifdef JIT_CANARY_WATCH
	memset(s_canaryPattern, 0xC5, JIT_CANARY_BYTES);   // GO-FIX-PH: before either slot inits
#endif

#ifdef JIT_MEM_ACCOUNT
	{
		size_t blockTableBytes  = BLOCK_TABLE_SLOTS * sizeof(BasicBlock);   // 2-way: 2 slots/set
		size_t smcTablesBytes   = SMC_MAP_SIZE * (sizeof(BasicBlock*) + 1);
		FILE* f = fopen("sd:/jitmem.log", "a");
		if (f) {
			fprintf(f, "[jitmem] want: arm7 arena=%u KiB  arm9 arena=%u KiB  "
			           "per-core blockTable=%u KiB  smcTables=%u KiB  (HASH_TABLE_SIZE=%u)\n",
			        (unsigned)(JIT_ARENA_SIZE >> 10), (unsigned)(JIT_ARENA_SIZE_ARM9 >> 10),
			        (unsigned)(blockTableBytes >> 10), (unsigned)(smcTablesBytes >> 10),
			        (unsigned)HASH_TABLE_SIZE);
			fclose(f);
		}
	}
	jitMemAccountReport("before");
#endif

	// Neither core's slot is allocated here any more (F1). The ARM9 slot
	// (~17 MB: 12 MB arena + ~4 MB block table + ~1 MB SMC tables) never was
	// -- see jitEnsureArm9() below. A GBA session never calls jitRunArm9() at
	// all (gbaExecFrame() has no ARM9 side, JIT or interpreted -- GBA has no
	// ARM9), so lazy allocation there means this slot simply never gets
	// allocated for a GBA session, leaving that memory free for a GBA cart's
	// own 16+ MB full-ROM buffer (NDSSystem.cpp's GBA branch). The ARM7 slot
	// (~3.5 MB: 2 MB arena + tables) now follows the same rule via
	// jitEnsureArm7(), so a session in Interpreter mode (jit.h's runtime CPU
	// mode) allocates no JIT memory at all.
	//
	// §12.3 step 5: build both ARM7 profiles up front (struct fill only, no
	// cache/arena work) so jitSetArm7GBAMode() has both ready to swap between
	// whether or not the slot exists yet.
	s_arm7DsProfile  = jitBuildArm7Profile();
	s_arm7GbaProfile = jitBuildArm7GBAProfile();

#if defined(JIT_CANARY_WATCH) && defined(JIT_HEAP_WATCH)
	jitArmMinefield();
#endif
	s_initDone = true;
}

// Lazily allocates the ARM9 slot on first real use (jitRunArm9(), jit_exec.cpp)
// instead of unconditionally in jitInit() -- see that function's comment.
// Idempotent: jitProfile[JIT_ARM9] being non-null already is the "done" state,
// same check jitRunArm9() itself uses. Requires jitInit() to have already run
// (it builds s_arm7DsProfile/s_arm7GbaProfile and sets s_initDone); returns
// false harmlessly if it hasn't, or if this allocation itself fails (OOM --
// jitRunArm9() falls back to the interpreter either way, same as any other
// jitCompileTrace() failure).
bool jitEnsureArm9()
{
	if (jitProfile[JIT_ARM9]) return true;
	if (!s_initDone) return false;
	return jitInitSlot(JIT_ARM9, JIT_ARENA_SIZE_ARM9, jitCacheArm9, jitBuildArm9Profile());
}

// ARM7 counterpart of jitEnsureArm9() (F1): called from jitRunArm7() on its
// first real dispatch. The cache is initialised against the DS profile (its
// smcBankMask -- identical to the GBA profile's, banks 2|3 -- is what the
// slot was always set up with, since jitInit() used to allocate it before any
// GBA switch), then whichever profile jitSetArm7GBAMode() last chose is
// published. On an allocation failure the half-allocated buffers are freed,
// the failure is latched (no memalign retry on every ARM7 step) and
// jitRunArm7() keeps returning 0, i.e. the ARM7 stays interpreted.
bool jitEnsureArm7()
{
	if (jitProfile[JIT_ARM7]) return true;
	if (!s_initDone || s_arm7AllocFailed) return false;
#ifdef JIT_MEM_ACCOUNT
	jitMemAccountReport("before-arm7");
#endif
	if (!jitInitSlot(JIT_ARM7, JIT_ARENA_SIZE, jitCacheArm7, s_arm7DsProfile)) {
		jitFreeSlot(JIT_ARM7);
		s_arm7AllocFailed = true;
		return false;
	}
#ifdef JIT_MEM_ACCOUNT
	jitMemAccountReport("after-arm7");
#endif
	jitProfile[JIT_ARM7] = s_arm7GbaMode ? s_arm7GbaProfile : s_arm7DsProfile;
	return true;
}

void jitSetArm7GBAMode(bool enable)
{
	// Guarded: a no-op if jitInit() hasn't run yet or failed (both pointers
	// stay null), so an early NDS_DebugForceGBAMode() call before NDS_Init()
	// can't dereference/publish a null profile.
	if (!s_arm7DsProfile || !s_arm7GbaProfile) return;
	s_arm7GbaMode = enable;
	// Publish now only if the slot already exists; otherwise jitEnsureArm7()
	// publishes the remembered choice when it allocates it.
	if (jitProfile[JIT_ARM7])
		jitProfile[JIT_ARM7] = enable ? s_arm7GbaProfile : s_arm7DsProfile;
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

// ---- fixed guest-register file --------------------------------------------
// Guest R0..R15 are pinned to host r14..r29 for the whole (possibly chained)
// trace; the trampoline (jit_trampoline.S) loads them from cpu.R[] on entry and
// stores them back on the one landing pad every exit funnels through. There is
// no cache, no eviction and no dirty bit -- readReg/writeReg (jit_trace.h) just
// return the pinned host register, and flushDirtyRegisters / invalidateRegCache
// are no-ops. The few emitters that still need guest state in *memory* mid-block
// (a C call that peeks cpu.R[], the interpreter bail) reload the gpr base from
// stack slot 80(r1).

// ---- guest memory via a C call to JitCpuProfile::slowRead/slowWrite --------
// r3 holds the cross-block cycle accumulator and is PPC-EABI volatile, so it's
// spilled/reloaded around the call. Every guest register (R0..R15 -> r14..r29,
// flags r30, icount r31) is non-volatile and survives the call untouched -- the
// whole point of GPR residency -- so nothing else needs saving here. The guest
// PC is still mirrored to gpr[15] defensively in case a C memory path peeks it;
// r14 is no longer the gpr base, so reload it from the stack slot the trampoline
// stashed.
void JitTraceCtx::emitMemPrologue()
{
	*emitPtr++ = PPC_LWZ(PPC_R10, 1, 80);        // gpr base
	*emitPtr++ = PPC_STW(PPC_R29, PPC_R10, 15 * 4);   // guest PC -> gpr[15]
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

// Differential-harness store journal. See jit_trace.h. eaReg holds the guest
// address; the call records `size` pre-write bytes so jitRunArm*Checked()'s
// interpreter reference run can be rolled back. Compiles to nothing in a
// shipping build (cpu.journalNote == 0). Clobbers the volatile registers
// r3..r12 (and CTR / LR); r3 is saved to and restored from 92(r1).
void JitTraceCtx::emitJournalNote(u8 eaReg, u32 size)
{
	if (!cpu.journalNote) return;
	u32*& p = emitPtr;
	const u32 fn = (u32)cpu.journalNote;
	const s32 procnum = cpu.isaLevel >= 5 ? 0 : 1;   // ARMCPU_ARM9 : ARMCPU_ARM7

	*p++ = PPC_OR(PPC_R4, eaReg, eaReg);              // arg2 = addr (before eaReg is at risk)
	*p++ = PPC_STW(PPC_R3, 1, 92);                    // save cycle accumulator
	*p++ = PPC_LI(PPC_R3, procnum);                   // arg1 = procnum
	*p++ = PPC_LI(PPC_R5, (s32)size);                 // arg3 = size
	*p++ = PPC_LIS(PPC_R12, fn >> 16);
	*p++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*p++ = PPC_MTCTR(PPC_R12);
	*p++ = PPC_BCTRL();
	*p++ = PPC_LWZ(PPC_R3, 1, 92);
}

// P14/P15 shared: guard EA (PPC_R12) against the cached-descriptor page window,
// optionally also that EA + spanBytes stays in the same 1 MB page, then resolve
// the descriptor. On the fall-through (hit) PPC_R10 = hostBase and PPC_R11 =
// (EA & mask) with the low bits cleared to `alignMe` (29 => &~3, 30 => &~1,
// 31 => no clear). EA stays in PPC_R12. Clobbers r10, r11.
//
// A miss no longer bails to the interpreter. Each miss test
// is a forward conditional branch whose target is left open: the placeholder
// word already carries the branch condition (BGT for "page outside the window",
// BNE for "run straddles a 1 MB page") with a zero displacement, and the
// caller ORs the real displacement in (patchMissSlots) once it has emitted its
// slowRead C path -- the same "miss falls to a slow C call, no round trip"
// shape as the ARM9 P16 guard. Returns the number of slots written (1 or 2)
// into missSlots[].
int JitTraceCtx::emitPageResolve(u32 spanBytes, u8 alignMe, u32** missSlots)
{
	u32*& p = emitPtr;
	int nMiss = 0;
	const u32 lo   = cpu.pageDescLo;
	const u32 span = cpu.pageDescHi - cpu.pageDescLo;

	// lo <= (EA >> 20) <= hi
	*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);
	*p++ = PPC_ADDI(PPC_R10, PPC_R10, -(s32)lo);        // page - lo
	*p++ = PPC_CMPLI(0, PPC_R10, span);                 // unsigned > span => out of window
	missSlots[nMiss++] = p;
	*p++ = PPC_BGT(0);                                  // -> caller's slow path

	// whole run in one 1 MB page: (EA + spanBytes) >> 20 == EA >> 20
	if (spanBytes) {
		*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)spanBytes);
		*p++ = PPC_SRWI(PPC_R11, PPC_R11, 20);
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);          // reload page (r10 was page-lo)
		*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
		missSlots[nMiss++] = p;
		*p++ = PPC_BNE(0);                              // -> caller's slow path
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
	return nMiss;
}

// Point every emitPageResolve miss branch at `target` (the caller's slow path).
// Each placeholder already holds its condition with a zero displacement.
static void patchMissSlots(u32* const* missSlots, int nMiss, const u32* target)
{
	for (int i = 0; i < nMiss; i++)
		*missSlots[i] |= (u32)((target - missSlots[i]) * 4) & 0xFFFC;
}

// P16: ARM9 two-region inline guard (plus ITCM for loads).
// See jit_trace.h. EA in PPC_R12.
int JitTraceCtx::emitArm9RegionGuard(u32 size, u8 alignMe, u32 spanBytes, u32** fastSlots, bool withItcm)
{
	(void)size;
	u32*& p = emitPtr;
	int nFast = 0;
	u32* slow[6]; int nSlow = 0; bool slowIsBeq[6] = { false };

	// DTCM window base, read live at emit time and baked (see the profile note).
	// _MMU_*<ARM9> compares (addr & ~0x3FFF) == MMU.DTCMRegion against the *full*
	// value, so a window base with any of bits 0..13 set can never match -- DTCM
	// is then unreachable and the whole branch is skipped at compile time. When
	// it can match it is 16 KB aligned, so the shifted-tag compare is exact.
	const u32 dtcmRegion   = *(const volatile u32*)(uintptr_t)cpu.arm9DtcmRegionPtr;
	const bool dtcmReach   = (dtcmRegion & 0x3FFFu) == 0;
	const s32 dtcmTag      = (s32)(dtcmRegion >> 14);
	const bool dtcmInMain  = dtcmReach && (dtcmRegion & 0x0F000000u) == 0x02000000u;
	const bool dtcmInItcm  = dtcmReach && dtcmRegion < 0x02000000u;
	const u32 mainMb       = (u32)__builtin_clz(cpu.arm9MainMask); // top set bit of the mirror mask
	const u32 dtcmBase     = cpu.arm9DtcmBase;
	const u32 mainBase     = cpu.mainMemBase;

	// ---- DTCM: (EA >> 14) == (dtcmRegion >> 14) ----
	u32* notDtcm = nullptr;
	if (dtcmReach) {
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 14);
		*p++ = PPC_CMPWI(0, PPC_R10, dtcmTag);
		notDtcm = p++;                                             // BNE -> main check
		if (spanBytes) {                                           // whole run in the 16 KB window
			*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)spanBytes);
			*p++ = PPC_SRWI(PPC_R10, PPC_R10, 14);
			*p++ = PPC_CMPWI(0, PPC_R10, dtcmTag);
			slow[nSlow++] = p++;                                   // BNE -> slow (straddles window)
		}
		*p++ = PPC_LIS(PPC_R10, dtcmBase >> 16);
		if (dtcmBase & 0xFFFF) *p++ = PPC_ORI(PPC_R10, PPC_R10, dtcmBase & 0xFFFF);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 18, alignMe);       // EA & 0x3FFF, cleared to align
		fastSlots[nFast++] = p++;                                  // B -> caller inline block

		*notDtcm = PPC_BNE((u32)((p - notDtcm) * 4));
	}

	// ---- main RAM: (EA >> 24) & 0xF == 2 ----
	*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 28, 31);
	*p++ = PPC_CMPWI(0, PPC_R10, 2);
	u32* notMain = p++;                                            // BNE -> ITCM test / slow
	if (spanBytes) {
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);
		*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)spanBytes);
		*p++ = PPC_SRWI(PPC_R11, PPC_R11, 20);
		*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
		slow[nSlow++] = p++;                                       // BNE -> slow (crosses a 1 MB page)
		if (dtcmInMain) {                                          // ...and not into the DTCM window
			*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)spanBytes);
			*p++ = PPC_SRWI(PPC_R10, PPC_R10, 14);
			*p++ = PPC_CMPWI(0, PPC_R10, dtcmTag);
			slowIsBeq[nSlow] = true;
			slow[nSlow++] = p++;                                   // BEQ -> slow
		}
	}
	*p++ = PPC_LIS(PPC_R10, mainBase >> 16);
	if (mainBase & 0xFFFF) *p++ = PPC_ORI(PPC_R10, PPC_R10, mainBase & 0xFFFF);
	*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, mainMb, alignMe);       // EA & mirrorMask, cleared to align
	fastSlots[nFast++] = p++;                                      // B -> caller inline block

	// ---- ITCM (loads only): EA < 0x02000000 ----
	// Tested after main RAM, on main RAM's miss branch, so the common main-RAM
	// hit pays nothing for it. The rule is exactly the interpreter's
	// (_MMU_ARM9_read*, reached once the DTCM and main-RAM checks in _MMU_read*
	// have missed): every address below 0x02000000 is the 32 KB ITCM, mirrored
	// by `addr & 0x7FFF`. That decode ignores the CP15 ITCM size/enable
	// register (cp15.cpp pins MMU.ITCMRegion to 0 and nothing reads the size),
	// so there is nothing CP15-dependent to bake: the only emit-time constant is
	// MMU.ARM9_ITCM, allocated with MAIN_MEM / ARM9_DTCM and never moved. DTCM
	// keeps priority because its test above runs first; the span test below
	// keeps a run inside one 32 KB mirror (the interpreter wraps each word
	// separately at the edge, the inline lwbrx run would not -- and the last
	// mirror's edge is 0x02000000 itself) and, when DTCM sits in the ITCM
	// range, out of the DTCM window -- the same shape as the main-RAM span
	// tests. Stores do not take this path: ITCM holds JIT code, and the
	// measured ITCM store rate on the benchmark scene was zero, so they stay on
	// slowWrite (whose _MMU_ARM9_write* does the SMC invalidate itself).
	if (!withItcm) {
		slow[nSlow++] = notMain;                                   // not main RAM -> slow
	} else {
		const u32 itcmBase = cpu.arm9ItcmBase;
		*notMain = PPC_BNE((u32)((p - notMain) * 4));              // not main RAM -> here
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 25);
		*p++ = PPC_CMPWI(0, PPC_R10, 0);
		slow[nSlow++] = p++;                                       // BNE -> slow (not ITCM)
		if (spanBytes) {
			*p++ = PPC_SRWI(PPC_R10, PPC_R12, 15);
			*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)spanBytes);
			*p++ = PPC_SRWI(PPC_R11, PPC_R11, 15);
			*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
			slow[nSlow++] = p++;                                   // BNE -> slow (crosses a 32 KB mirror)
			if (dtcmInItcm) {                                      // ...and not into the DTCM window
				*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)spanBytes);
				*p++ = PPC_SRWI(PPC_R10, PPC_R10, 14);
				*p++ = PPC_CMPWI(0, PPC_R10, dtcmTag);
				slowIsBeq[nSlow] = true;
				slow[nSlow++] = p++;                               // BEQ -> slow
			}
		}
		*p++ = PPC_LIS(PPC_R10, itcmBase >> 16);
		if (itcmBase & 0xFFFF) *p++ = PPC_ORI(PPC_R10, PPC_R10, itcmBase & 0xFFFF);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 17, alignMe);       // EA & 0x7FFF, cleared to align
		fastSlots[nFast++] = p++;                                  // B -> caller inline block
	}

	// slow fall-through starts here; retarget every miss branch to it
	for (int i = 0; i < nSlow; i++) {
		const u32 off = (u32)((p - slow[i]) * 4);
		*slow[i] = slowIsBeq[i] ? PPC_BEQ(off) : PPC_BNE(off);
	}
	return nFast;
}

// P16: full ARM9 single load. See jit_trace.h. EA in PPC_R12.
void JitTraceCtx::emitArm9Load(u8 rd, u32 size, bool signExt, bool wordRotate, bool writeback, u8 rn)
{
	u32*& p = emitPtr;

	// Guest state is resident (r14..r31) so the SMC-bail / slowRead / inline
	// paths all see coherent registers with nothing to flush.
	*p++ = PPC_STW(PPC_R12, 1, 96);                                // stash EA for the slow path

	u32* fast[3];
	const u8 alignMe = size == 4 ? 29 : size == 2 ? 30 : 31;
	const int nFast = emitArm9RegionGuard(size, alignMe, /*spanBytes=*/0, fast, /*withItcm=*/true);

	// ---- slow: slowRead C call ----
	emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	emitSlowLoad(PPC_R10, PPC_R12, size, signExt);
	if (wordRotate) {                                              // ROR(R10, 8 * (EA & 3))
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);
		*p++ = PPC_LI(PPC_R11, 4);
		*p++ = PPC_SUBF(PPC_R12, PPC_R12, PPC_R11);
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);
		*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
	}
	emitMemEpilogue();
	u32* toEnd = p++;                                              // B over the fast block

	// ---- fast: inline lwbrx (r10 = host base, r11 = aligned in-region offset) ----
	for (int i = 0; i < nFast; i++) *fast[i] = PPC_B((u32)((p - fast[i]) * 4));
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);
	if (size == 4) {
		*p++ = PPC_LWBRX(PPC_R10, 0, PPC_R10);
		if (wordRotate) {
			*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 30, 31);        // x = EA & 3
			*p++ = PPC_SUBFIC(PPC_R11, PPC_R11, 4);                // 4 - x
			*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 3, 27, 28);        // ((4 - x) & 3) << 3
			*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R11, 0, 31);
		}
	} else if (size == 2) {
		*p++ = PPC_LHBRX(PPC_R10, 0, PPC_R10);
		if (signExt) *p++ = PPC_EXTSH(PPC_R10, PPC_R10);
	} else {
		*p++ = PPC_LBZX(PPC_R10, 0, PPC_R10);
		if (signExt) *p++ = PPC_EXTSB(PPC_R10, PPC_R10);
	}

	// ---- converge ---- (guest regs are pinned: commit straight into them)
	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	if (writeback) *p++ = PPC_LWZ(hostRegFor(rn), 1, 104);
	*p++ = PPC_OR(hostRegFor(rd), PPC_R10, PPC_R10);
}

// P14: inline RAM load. See jit_trace.h. eaReg == PPC_R12 by contract.
bool JitTraceCtx::emitInlineLoad(u8 rd, u8 eaReg, u32 size, bool signExt, bool wordRotate, u32& lockedMask)
{
	(void)eaReg;                                  // contract: EA is in PPC_R12
	if (cpu.arm9DtcmBase) {                        // P16 ARM9 two-region path
		(void)lockedMask;
		emitArm9Load(rd, size, signExt, wordRotate, /*writeback=*/false, /*rn=*/0);
		return true;
	}
	if (!cpu.pageDescBase) return false;
	u32*& p = emitPtr;

	u32* miss[2];
	const int nMiss = emitPageResolve(/*spanBytes=*/0, size == 4 ? 29 : size == 2 ? 30 : 31, miss);

	// Guest registers are pinned (writeReg just names the fixed host slot), so
	// the fast and slow paths below both commit into this one register.
	const u8 hDst = writeReg(rd, /*fullOverwrite=*/true, lockedMask);

	// ---- fast: descriptor hit, inline load ----
	if (size == 4) {
		*p++ = PPC_LWBRX(hDst, PPC_R10, PPC_R11);
		// ARM OP_LDR rotates an unaligned word: ROR(word, 8 * (EA & 3)). The
		// caller passes wordRotate to match its own slow path exactly (THUMB
		// PC/SP-relative loads have a word-aligned EA and pass false).
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
	u32* toEnd = p++;                                   // B over the slow path

	// ---- slow: EA outside the RAM page window -- I/O,
	// VRAM, BIOS, GBA slot, open bus. Do the access in place through the
	// profile's slowRead C call and keep running the block; this used to end
	// the whole chain with an interpreter bail that re-ran the instruction.
	// Same semantics as emitLoadStoreTail's slow tail: sign-extend inside
	// emitSlowLoad, then the unaligned-word ROR from the stashed EA. Any base
	// writeback is committed by the caller after we return, and rd == rn with
	// writeback never reaches this helper (the caller keeps that form on its
	// own slow tail), so the ordering is unchanged. Clobbers r10..r12, as the
	// helper's contract already allows.
	patchMissSlots(miss, nMiss, p);
	*p++ = PPC_STW(PPC_R12, 1, 96);                     // stash EA across the call
	emitMemPrologue();
	emitSlowLoad(PPC_R10, PPC_R12, size, signExt);
	if (wordRotate) {                                   // ROR(R10, 8 * (EA & 3))
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);
		*p++ = PPC_SUBFIC(PPC_R12, PPC_R12, 4);
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);
		*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
	}
	emitMemEpilogue();
	*p++ = PPC_OR(hDst, PPC_R10, PPC_R10);

	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	return true;
}

// P16: ARM9 inline block load. See jit_trace.h. Low EA in PPC_R12.
void JitTraceCtx::emitArm9BlockLoad(const u8* regs, u32 n)
{
	u32*& p = emitPtr;

	*p++ = PPC_STW(PPC_R12, 1, 96);                                // stash low EA

	u32* fast[3];
	const int nFast = emitArm9RegionGuard(4, /*alignMe=*/29, /*spanBytes=*/4 * (n - 1), fast, /*withItcm=*/true);

	// ---- slow: per-word slowRead C loop, straight into the pinned regs ----
	emitMemPrologue();
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		if (k) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
		emitSlowLoad(hostRegFor(regs[k]), PPC_R12, 4, false);
	}
	emitMemEpilogue();
	u32* toEnd = p++;                                              // B over the fast block

	// ---- fast: n sequential inline lwbrx into the pinned regs ----
	for (int i = 0; i < nFast; i++) *fast[i] = PPC_B((u32)((p - fast[i]) * 4));
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);                     // r10 = host addr of the low word
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWBRX(hostRegFor(regs[k]), 0, PPC_R10);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R10, PPC_R10, 4);
	}

	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	*p++ = PPC_LWZ(PPC_R12, 1, 96);               // restore low EA (THUMB LDMIA reads it back)
}

// P15: inline sequential block load (LDM / POP / LDMIA). See jit_trace.h.
bool JitTraceCtx::emitInlineBlockLoad(const u8* regs, u32 n, u8 eaReg, u32& lockedMask)
{
	(void)eaReg;                                  // contract: low EA is in PPC_R12
	if (cpu.arm9DtcmBase) {                        // P16 ARM9 two-region path
		(void)lockedMask;
		emitArm9BlockLoad(regs, n);
		return true;
	}
	if (!cpu.pageDescBase) return false;
	u32*& p = emitPtr;

	u32* miss[2];
	const int nMiss = emitPageResolve(/*spanBytes=*/4 * (n - 1), /*alignMe=*/29, miss);   // LDM: no unaligned rotate

	// ---- fast: r10 = hostBase, r11 = aligned page offset of the low word ----
	for (u32 k = 0; k < n; k++) {
		const u8 hgi = writeReg(regs[k], /*fullOverwrite=*/true, lockedMask);
		*p++ = PPC_LWBRX(hgi, PPC_R10, PPC_R11);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R11, PPC_R11, 4);
	}
	u32* toEnd = p++;                                   // B over the slow path

	// ---- slow: the run is outside the RAM page window or
	// straddles a 1 MB page. Per-word slowRead C loop over the whole run,
	// straight into the pinned registers, and the block continues -- the same
	// shape as emitArm9BlockLoad's slow path (and the unpredicated slow LDM
	// this path originally replaced), where it used to be one interpreter bail
	// for the whole instruction. Each word re-reads the low EA from the stash,
	// so a base register in the list (legal only without writeback) being
	// overwritten mid-run is harmless. The raw low EA goes back into r12 on
	// exit, where the fast path leaves it (THUMB LDMIA derives its writeback
	// from r12). Worst case, ARM LDM with 15 registers: ~9 words per register,
	// ~200 words for the whole instruction including the fast path -- inside
	// JIT_MAX_INSTR_RESERVE_WORDS_ARM; THUMB lists are <= 8 registers, inside
	// JIT_MAX_INSTR_RESERVE_WORDS. The ARM9 block load already emits the same
	// per-word loop for the same list sizes.
	patchMissSlots(miss, nMiss, p);
	*p++ = PPC_STW(PPC_R12, 1, 96);                     // stash low EA
	emitMemPrologue();
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		if (k) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
		emitSlowLoad(hostRegFor(regs[k]), PPC_R12, 4, false);
	}
	emitMemEpilogue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);                     // restore low EA

	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	return true;
}

// value in PPC_R11, host address in PPC_R10.
static inline void emitArm9Stw(u32*& p, u32 size)
{
	if (size == 4)      *p++ = PPC_STWBRX(PPC_R11, 0, PPC_R10);
	else if (size == 2) *p++ = PPC_STHBRX(PPC_R11, 0, PPC_R10);
	else                *p++ = PPC_STBZX (PPC_R11, 0, PPC_R10);
}

// P16: full ARM9 single store. See jit_trace.h. EA in PPC_R12, value at 100(r1).
void JitTraceCtx::emitArm9Store(u32 size, bool writeback, u8 rn)
{
	u32*& p = emitPtr;

	// Guest state is resident, so the SMC-bail / slowWrite / inline paths all
	// see coherent registers -- same shape as emitArm9Load.
	*p++ = PPC_STW(PPC_R12, 1, 96);                                // stash EA

	u32* fast[2];
	const u8 alignMe = size == 4 ? 29 : size == 2 ? 30 : 31;
	const int nFast = emitArm9RegionGuard(size, alignMe, /*spanBytes=*/0, fast, /*withItcm=*/false);

	// ---- slow: SMC guard + slowWrite C call (no interpreter round-trip) ----
	emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	emitSmcCheckAndBail(PPC_R12);
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	*p++ = PPC_LWZ(PPC_R10, 1, 100);
	emitSlowStore(PPC_R12, PPC_R10, size);
	emitMemEpilogue();
	u32* toEnd = p++;                                              // B over the fast block(s)

	// ---- fast: main RAM (SMC-guard the written page) ----
	// r10 = region host base, r11 = aligned in-region offset, r12 = EA.
	const bool haveDtcm = (nFast == 2);
	*fast[nFast - 1] = PPC_B((u32)((p - fast[nFast - 1]) * 4));
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);                     // host store address
	*p++ = PPC_STW(PPC_R10, 1, 108);                               // stash across SMC / journal
	emitSmcCheckAndBail(PPC_R12);                                  // EA still in r12
	u32* toShared = haveDtcm ? p++ : nullptr;                      // B over the DTCM entry

	// ---- fast: DTCM (never holds JIT code -> no SMC guard) ----
	if (haveDtcm) {
		*fast[0] = PPC_B((u32)((p - fast[0]) * 4));
		*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);
		*p++ = PPC_STW(PPC_R10, 1, 108);
	}

	// ---- shared fast tail: differential journal note + the inline store ----
	if (toShared) *toShared = PPC_B((u32)((p - toShared) * 4));
	*p++ = PPC_LWZ(PPC_R12, 1, 96);                                // EA (journal arg)
	emitJournalNote(PPC_R12, size);
	*p++ = PPC_LWZ(PPC_R10, 1, 108);                               // host store address
	*p++ = PPC_LWZ(PPC_R11, 1, 100);                               // value
	emitArm9Stw(p, size);

	// ---- converge ---- (writeback straight into the pinned base register)
	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	if (writeback) *p++ = PPC_LWZ(hostRegFor(rn), 1, 104);
}

// P16: ARM9 inline block store (STM / PUSH / STMIA, non-pc). See jit_trace.h.
// Low EA in PPC_R12; each source register is read straight from its pinned host
// register (r14..r29).
void JitTraceCtx::emitArm9BlockStore(const u8* regs, u32 n)
{
	u32*& p = emitPtr;
	const u32 span = 4 * (n - 1);

	*p++ = PPC_STW(PPC_R12, 1, 96);                                // stash low EA

	u32* fast[2];
	const int nFast = emitArm9RegionGuard(4, /*alignMe=*/29, /*spanBytes=*/span, fast, /*withItcm=*/false);

	// ---- slow: SMC guard (whole span) + per-word slowWrite C loop ----
	emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	emitSmcCheckAndBail(PPC_R12);
	if (span) { *p++ = PPC_LWZ(PPC_R12, 1, 96); *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)span);
	            emitSmcCheckAndBail(PPC_R12); }                     // the run may cross a 1 KB page
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		if (k) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
		emitSlowStore(PPC_R12, hostRegFor(regs[k]), 4);
	}
	emitMemEpilogue();
	u32* toEnd = p++;                                              // B over the fast block(s)

	// ---- fast: main RAM (SMC-guard both ends of the span) ----
	const bool haveDtcm = (nFast == 2);
	*fast[nFast - 1] = PPC_B((u32)((p - fast[nFast - 1]) * 4));
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);                     // host addr of the low word
	*p++ = PPC_STW(PPC_R10, 1, 108);
	emitSmcCheckAndBail(PPC_R12);                                  // low end (EA in r12)
	if (span) { *p++ = PPC_LWZ(PPC_R12, 1, 96); *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)span);
	            emitSmcCheckAndBail(PPC_R12); }                     // the run may cross a 1 KB page
	u32* toShared = haveDtcm ? p++ : nullptr;                      // B over the DTCM entry

	// ---- fast: DTCM (no SMC guard) ----
	if (haveDtcm) {
		*fast[0] = PPC_B((u32)((p - fast[0]) * 4));
		*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);
		*p++ = PPC_STW(PPC_R10, 1, 108);
	}

	// ---- shared fast tail: one journal note for the span + n inline stwbrx ----
	if (toShared) *toShared = PPC_B((u32)((p - toShared) * 4));
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	emitJournalNote(PPC_R12, 4 * n);
	*p++ = PPC_LWZ(PPC_R10, 1, 108);
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_STWBRX(hostRegFor(regs[k]), 0, PPC_R10);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R10, PPC_R10, 4);
	}

	// ---- converge ----
	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	*p++ = PPC_LWZ(PPC_R12, 1, 96);                                // restore low EA for the caller
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
	emitResultMetadata(metaCount, 0);
	// r29 (guest R15 / pipeline PC) needs the *pipeline* value, not the bare
	// target -- the interpreter-pipeline convention emitStaticExit also follows
	// -- so a guarded-dispatch hit lands in the next block with r29 already
	// correct. It is resident (the i=15 slot of the pinned register file), so
	// no block ever reloads it from memory.
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
	// No state flush here: guest R0..R15 + flags are resident and the single
	// trampoline landing pad this branch reaches writes them back to cpu.R[]
	// before the interpreter resumes at currentPC.
	emitAddCycles(cyclesAccum);
	emitResultMetadata(metaCount, 1);
	*p++ = PPC_LIS(PPC_R4, currentPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, currentPC & 0xFFFF);
	s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)p);
	*p++ = PPC_B(retOff);
}

// In-block interpreter fallback (JIT_INTERP_FALLBACK, see
// jit_trace.h). The guest register file is only ever in cpu.R[] at trampoline
// boundaries, so the call is bracketed by the same sync the trampoline does:
// one stmw of r14..r31 -> R[0..15], CPSR (r30) and SPSR -- the last is r31's
// instruction count, so SPSR is saved around it exactly like
// ExecuteJITTrace_Return does -- and one lmw back afterwards. Reloading
// *everything* is what makes mode switches work: armcpu_switchMode() swaps the
// banked R8-R14 in cpu.R[] and the lmw picks up whichever bank is now live,
// with the new CPSR in r30. r31 (icount) rides through the lmw in r12.
//
// jitInterpFallback*() (jit_exec.cpp) sets up the interpreter pipeline state,
// runs the handler and returns its cycles | exit flags:
//   0                         -> fall through into the next compiled insn
//   JIT_FALLBACK_EXIT_CHAIN   -> PC redirected, ISA unchanged: exit to
//                                cpu.next_instruction through the guarded
//                                dynamic-chaining stub (same as BX/POP{pc})
//   JIT_FALLBACK_EXIT_TO_C    -> anything the dispatcher must see (T flip,
//                                IRQ unmask, halt, SMC kill / cache flush,
//                                a fresh reschedule request): plain return
// Handler cycles go straight into r3; the instruction counts in r31 on both
// exits (it has executed). ~31 words.
void JitTraceCtx::emitInterpFallback(u32 opcode)
{
	ensureArena();
	u32*& p = emitPtr;
	const bool arm9 = (cpu.isaLevel >= 5);
	const u32 fn = arm9 ? (thumbMode ? (u32)&jitInterpFallbackArm9Thumb : (u32)&jitInterpFallbackArm9Arm)
	                    : (thumbMode ? (u32)&jitInterpFallbackArm7Thumb : (u32)&jitInterpFallbackArm7Arm);
	const u32 pc = currentPC;

	// ---- pinned R0..R15 + CPSR -> cpu.R[] / CPSR ----
	*p++ = PPC_LWZ(PPC_R10, 1, 80);                   // &cpu.R[0]
	*p++ = PPC_LWZ(0, PPC_R10, 17 * 4);               // SPSR (the stmw clobbers it with r31)
	*p++ = PPC_STMW(14, PPC_R10, 0);
	*p++ = PPC_STW(0, PPC_R10, 17 * 4);
	*p++ = PPC_STW(PPC_R3, 1, 92);                    // cycle accumulator
	*p++ = PPC_LIS(PPC_R3, opcode >> 16);             // arg1 = opcode
	*p++ = PPC_ORI(PPC_R3, PPC_R3, opcode & 0xFFFF);
	*p++ = PPC_LIS(PPC_R4, pc >> 16);                 // arg2 = its address
	*p++ = PPC_ORI(PPC_R4, PPC_R4, pc & 0xFFFF);
	*p++ = PPC_LIS(PPC_R12, fn >> 16);
	*p++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*p++ = PPC_MTCTR(PPC_R12);
	*p++ = PPC_BCTRL();

	// ---- cpu.R[] / CPSR -> pinned registers (possibly a different bank now) ----
	*p++ = PPC_LWZ(PPC_R10, 1, 80);
	*p++ = PPC_OR(PPC_R12, PPC_R31, PPC_R31);         // icount survives the lmw in r12
	*p++ = PPC_LMW(14, PPC_R10, 0);                   // r14..r29 = R0..R15, r30 = CPSR, r31 = SPSR
	*p++ = PPC_OR(PPC_R31, PPC_R12, PPC_R12);
	*p++ = PPC_OR(PPC_R11, PPC_R3, PPC_R3);           // r11 = cycles | exit flags
	*p++ = PPC_LWZ(PPC_R3, 1, 92);
	*p++ = PPC_RLWINM(PPC_R12, PPC_R11, 0, 2, 31);    // handler cycles
	*p++ = PPC_ADD(PPC_R3, PPC_R3, PPC_R12);
	*p++ = PPC_RLWINM(PPC_R12, PPC_R11, 2, 30, 31) | 1;   // rlwinm.: TO_C -> 2, CHAIN -> 1
	u32* cont = p++;

	// ---- exit: the handler moved PC, or the dispatcher has to look ----
	*p++ = PPC_LWZ(PPC_R4, PPC_R10, -4);              // cpu.next_instruction (R[-1])
	emitAddCycles(cyclesAccum);
	emitResultMetadata(instrCount + 1, 0);
	*p++ = PPC_CMPWI(0, PPC_R12, 1);
	u32* toC = p++;
#if JIT_ENABLE_DYNAMIC_CHAINING
	*p++ = PPC_ADDI(PPC_R29, PPC_R4, thumbMode ? 4 : 8);
	{
		u32* stub = thumbMode ? cache.linkerStubDynamicThumbAddress : cache.linkerStubDynamicArmAddress;
		s32 o = (s32)((u8*)stub - (u8*)p);
		*p++ = PPC_B(o);
	}
#endif
	*toC = PPC_BNE((u32)((p - toC) * 4));
	{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)p); *p++ = PPC_B(o); }

	*cont = PPC_BEQ((u32)((p - cont) * 4));
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
	// perf_zones: this whole call is "JIT build" time, carved out of the
	// enclosing ARM9_JIT / ARM7_JIT dispatch interval.
	PZ_SCOPE(cpu.isaLevel >= 5 ? PZ_ARM9_BUILD : PZ_ARM7_BUILD);

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

		// Snapshot for the interpreter fallback below: a refusal may come after
		// the emitter already wrote part of a sequence (or registered a deferred
		// bailout pointing into it), so a refused instruction is rewound to
		// exactly here before the fallback call replaces it.
		u32* const emitMark = ctx.arenaAllocated ? ctx.emitPtr : nullptr;
		const u32  bailMark = ctx.bailoutCount;

		u32 opcode;
		if (thumb) {
			opcode = (u16)cpu.fetch16(ctx.currentPC);
			jitThumbEmitOne(ctx, (u16)opcode);
			if (!ctx.endBlock) {
				ctx.instrCount++;
				ctx.currentPC   += 2;
				ctx.cyclesAccum += cpu.cyclesForThumb((u16)opcode);
			}
		} else {
			opcode = cpu.fetch32(ctx.currentPC);
			jitArmEmitOne(ctx, opcode);
			if (!ctx.endBlock) {
				ctx.instrCount++;
				ctx.currentPC   += 4;
				ctx.cyclesAccum += cpu.cyclesForArm(opcode);
			}
		}

#if JIT_INTERP_FALLBACK
		// endBlock without blockTerminatedEarly == the front end refused this
		// instruction (every terminator sets both and has already counted
		// itself). Instead of ending the block here -- and, at a block's first
		// instruction, caching a "don't JIT" marker -- run it through the
		// interpreter's handler in place and keep scanning. No cyclesAccum
		// term: the handler's own count is added to r3 at run time.
		if (ctx.endBlock && !ctx.blockTerminatedEarly) {
			if (emitMark)                ctx.emitPtr = emitMark;
			else if (ctx.arenaAllocated) ctx.emitPtr = ctx.quotaGuard + 1;   // just past ensureArena()'s guard
			ctx.bailoutCount = bailMark;
			ctx.endBlock = false;
			ctx.emitInterpFallback(opcode);
			ctx.instrCount++;
			ctx.currentPC += thumb ? 2 : 4;
		}
#endif
	}

	if (ctx.instrCount == 0) {
		// Nothing compilable at startPC -- cache a length-1 fallback so the
		// dispatcher resolves straight to the interpreter next time.
#ifdef DESMUME_JIT_TRACE_FIRST
		{
			static u32 s_seen[256]; static u32 s_cnt[256]; static int s_n = 0;
			static u64 s_tot = 0; s_tot++;
			u32 opc = thumb ? (u32)cpu.fetch16(startPC) : cpu.fetch32(startPC);
			int i = 0; for (; i < s_n; i++) if (s_seen[i] == opc) break;
			if (i == s_n && s_n < 256) { s_seen[s_n] = opc; s_cnt[s_n] = 0; s_n++; }
			if (i < 256) s_cnt[i]++;
			if ((s_tot & 0xFF) == 0) {   // TODO item 6: lowered from 0xFFF so a
			                             // short soak actually reaches a dump
				// bubble the top few to the front, then dump
				for (int a = 0; a < s_n; a++)
					for (int b2 = a + 1; b2 < s_n; b2++)
						if (s_cnt[b2] > s_cnt[a]) {
							u32 t = s_cnt[a]; s_cnt[a] = s_cnt[b2]; s_cnt[b2] = t;
							t = s_seen[a]; s_seen[a] = s_seen[b2]; s_seen[b2] = t;
						}
				FILE* f = fopen("sd:/jit.log", "a");
				if (f) {
					fprintf(f, "[jit] dontJIT tot=%llu uniq=%d %s top:",
					        (unsigned long long)s_tot, s_n, thumb ? "T" : "A");
					for (int a = 0; a < s_n && a < 12; a++)
						fprintf(f, " %08x=%u", (unsigned)s_seen[a], (unsigned)s_cnt[a]);
					fprintf(f, "\n");
					fclose(f);
				}
			}
		}
#endif
		// Give back the slot ensureArena() reserved, if the scan got that far,
		// or every marker leaks JIT_MAX_WORDS of arena until the next flush.
		if (ctx.arenaAllocated)
			cache.rewindJITMemory((JIT_MAX_WORDS * sizeof(u32) + 31) & ~31u);
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

#ifdef JIT_HEAP_WATCH
	// §16: check the buffer canaries on every compile, not just the 1K-dispatch
	// poll -- a corrupting block is most likely to trip one right after it emits.
	jitCheckCanaries();
#endif

	return cache.registerBlock(startPC, ctx.instrCount, (JITBlockFunc)ctx.blockStart, thumb);
}

#endif // DESMUME_JIT
