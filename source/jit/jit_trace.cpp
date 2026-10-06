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
#include <stdlib.h>
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
#ifdef JIT_ARENA_HEAP
	free(s_arena[i]);
#endif
	s_arena[i]        = nullptr;
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

#ifdef JIT_ARENA_HEAP
	s_arena[i]        = (u32*)        memalign(32, arenaBytes        + s_jitCanaryPad);
#else
	// Code arenas are static .bss arrays, which the dol loader places in MEM1
	// (the heap would put them in MEM2). Hardware A/B (SM64DS GxFast, same
	// 4 MB/1 MB sizes): frame median 27.1 -> 23.0 ms, j9_exec 5.19 -> 2.80 ms,
	// j7_exec 3.07 -> 1.77 ms. The cost is 5 MB of MEM1 held for the whole
	// session, JIT or not (Interpreter mode, GBA). -DJIT_ARENA_HEAP restores
	// the memalign path.
	static u32 s_bssArena9[(JIT_ARENA_SIZE_ARM9 + s_jitCanaryPad) / 4] __attribute__((aligned(32)));
	static u32 s_bssArena7[(JIT_ARENA_SIZE      + s_jitCanaryPad) / 4] __attribute__((aligned(32)));
	s_arena[i] = (i == JIT_ARM9) ? s_bssArena9 : s_bssArena7;
#endif
	s_blockTable[i]   = (BasicBlock*) memalign(16, blockTableBytes   + s_jitCanaryPad);
	s_smcRegistry[i]  = (BasicBlock**)memalign(32, smcRegistryBytes  + s_jitCanaryPad);
	s_smcPageFlags[i] = (u8*)         memalign(32, smcFlagsBytes     + s_jitCanaryPad);

	if (!s_arena[i] || !s_blockTable[i] || !s_smcRegistry[i] || !s_smcPageFlags[i])
		return false;

	jitCanaryArm(s_arena[i],        arenaBytes,        i == JIT_ARM9 ? "arm9.arena"   : "arm7.arena");
	jitCanaryArm(s_blockTable[i],   blockTableBytes,    i == JIT_ARM9 ? "arm9.blockTable"  : "arm7.blockTable");
	jitCanaryArm(s_smcRegistry[i],  smcRegistryBytes,   i == JIT_ARM9 ? "arm9.smcRegistry" : "arm7.smcRegistry");
	jitCanaryArm(s_smcPageFlags[i], smcFlagsBytes,      i == JIT_ARM9 ? "arm9.smcPageFlags": "arm7.smcPageFlags");

	// Both caches get their profile's shared memory thunks (ARM9: full region
	// resolution; ARM7: SMC guard + slowWrite); initialize()'s flushCache()
	// emits them. The ARM7 cache is built against the DS profile.
	cache.thunkProfile = profile;
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
	// (~2.3 MB heap: 2 MB block table + ~320 KB SMC tables; the 4 MB code arena
	// is a static MEM1 array unless JIT_ARENA_HEAP, see jitInitSlot()) never was
	// -- see jitEnsureArm9() below. A GBA session never calls jitRunArm9() at
	// all (gbaExecFrame() has no ARM9 side, JIT or interpreted -- GBA has no
	// ARM9), so lazy allocation there means this slot simply never gets
	// allocated for a GBA session, leaving that memory free for a GBA cart's
	// own 16+ MB full-ROM buffer (NDSSystem.cpp's GBA branch). The ARM7 slot
	// (the same ~2.3 MB of tables; 1 MB arena) now follows the same rule via
	// jitEnsureArm7(), so a session in Interpreter mode (jit.h's runtime CPU
	// mode) allocates no JIT heap memory at all (the static arenas stay
	// reserved in .bss either way).
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

#ifdef JIT_CODE_STATS
static u32* jitCodeStatsNewRecord(const JITCache& cache);
static void jitCodeStatsDropRecord(const JITCache& cache);
static void jitCodeStatsCommit(const JitTraceCtx& ctx, u32 emittedWords, u32 committedBytes);
static void jitCodeStatsBudgetEnd(const JITCache& cache);
#endif

// =========================================================================
// JitTraceCtx helpers
// =========================================================================
void JitTraceCtx::ensureArena()
{
	if (arenaAllocated) return;
	arenaAllocated = true;

	arenaOffsetStart = cache.getArenaOffset();
	emitPtr = cache.allocateJITMemory(JIT_BLOCK_RESERVE_WORDS * sizeof(u32));
	blockStart = emitPtr;

#ifdef JIT_CODE_STATS
	statCounter = jitCodeStatsNewRecord(cache);
	if (statCounter) {                                   // ++*statCounter (r11/r12 are free at entry)
		const u32 a = (u32)statCounter, ha = (a + 0x8000) >> 16;
		*emitPtr++ = PPC_LIS(PPC_R12, ha);
		*emitPtr++ = PPC_LWZ(PPC_R11, PPC_R12, (s32)(s16)(a & 0xFFFF));
		*emitPtr++ = PPC_ADDI(PPC_R11, PPC_R11, 1);
		*emitPtr++ = PPC_STW(PPC_R11, PPC_R12, (s32)(s16)(a & 0xFFFF));
		statWords[JCS_STATCTR] += 4;
	}
#endif

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
	if (flagDead(targetBit)) return;
	JIT_STAT_SCOPE(*this, JCS_FLAGS, false);
	ensureFlagsLoaded();
	*emitPtr++ = PPC_MERGE_FLAG_BIT(targetBit, srcReg, sh);
	flagsDirty = true;
}

void JitTraceCtx::emitFlagConst(u8 targetBit, bool value)
{
	if (flagDead(targetBit)) return;
	JIT_STAT_SCOPE(*this, JCS_FLAGS, false);
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
	JIT_STAT_SCOPE(*this, JCS_FLAGS, false);
	emitFlagBit(JITF_N, srcReg, 1);
	if (flagDead(JITF_Z)) return;
	*emitPtr++ = PPC_CNTLZW(PPC_R8, srcReg);
	emitFlagBit(JITF_Z, PPC_R8, 27);
}

void JitTraceCtx::emitCVfromXER(u32 scratchReg)
{
	if (cvDead()) return;                        // skips the mfxer too
	JIT_STAT_SCOPE(*this, JCS_FLAGS, false);
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
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
	*emitPtr++ = PPC_LWZ(PPC_R10, 1, 80);        // gpr base
	*emitPtr++ = PPC_STW(PPC_R29, PPC_R10, 15 * 4);   // guest PC -> gpr[15]
	*emitPtr++ = PPC_STW(PPC_R3, 1, 92);         // save cycle accumulator
}

void JitTraceCtx::emitMemEpilogue()
{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
	*emitPtr++ = PPC_LWZ(PPC_R3, 1, 92);
}

void JitTraceCtx::emitSlowLoad(u8 destReg, u8 eaReg, u32 size, bool signExtend)
{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
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
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
	u32 fn = (u32)cpu.slowWrite;
	*emitPtr++ = PPC_OR(PPC_R3, eaReg, eaReg);            // arg1 = addr
	*emitPtr++ = PPC_OR(PPC_R4, valReg, valReg);          // arg2 = value
	*emitPtr++ = PPC_LI(PPC_R5, (s32)size);               // arg3 = size
	*emitPtr++ = PPC_LIS(PPC_R12, fn >> 16);
	*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*emitPtr++ = PPC_MTCTR(PPC_R12);
	*emitPtr++ = PPC_BCTRL();
}

// ---- small shared emit helpers ----------------------------------------------
static inline void jitImm32(u32*& p, u8 r, u32 v)
{
	*p++ = PPC_LIS(r, v >> 16);
	if (v & 0xFFFF) *p++ = PPC_ORI(r, r, v & 0xFFFF);
}
static inline void jitCallC(u32*& p, u32 fn)                   // clobbers r12, CTR, LR
{
	*p++ = PPC_LIS(PPC_R12, fn >> 16);
	*p++ = PPC_ORI(PPC_R12, PPC_R12, fn & 0xFFFF);
	*p++ = PPC_MTCTR(PPC_R12);
	*p++ = PPC_BCTRL();
}
// cr0 <- smcPageFlags[(src >> 10) & 0xFFFF] vs 0 (NE: compiled code lives on
// the page). rIdx / rVal are scratch; rIdx may be src, never r0.
static inline void emitSmcPageTest(u32*& p, u8 src, u8 rIdx, u8 rVal, u32 flags)
{
	*p++ = PPC_RLWINM(rIdx, src, 22, 16, 31);
	*p++ = PPC_ADDIS(rIdx, rIdx, (flags + 0x8000) >> 16);
	*p++ = PPC_LBZ(rVal, rIdx, (s32)(s16)(flags & 0xFFFF));
	*p++ = PPC_CMPWI(0, rVal, 0);
}
static inline u8 jitAlignBit(u32 size) { return size == 4 ? 29 : size == 2 ? 30 : 31; }

// Store paths: if a compiled block currently lives on the written 1KB page,
// bail (resume PC = this instruction) with smcHit set so the caller can call
// JitCpuProfile::smcInvalidate. eaReg must still be live (it is reported as
// smcAddress). The page test is inline; the bail sequence is a cold stub at the
// block's tail (emitColdStubs()). Clobbers r10, r11. Does not end the block --
// the not-taken path continues compiling.
void JitTraceCtx::emitSmcCheckAndBail(u8 eaReg)
{
#ifdef JIT_GOFIXPH_NO_SMC
	(void)eaReg; return;   // GO-FIX-PH diagnostic: skip the inline SMC guard
#endif
	JIT_STAT_SCOPE(*this, JCS_SMCGUARD, false);
	JitColdStub& s = addCold(JCOLD_SMCBAIL);
	s.reg = eaReg;
	emitSmcPageTest(emitPtr, eaReg, PPC_R11, PPC_R10, (u32)cache.smcPageFlags);
	s.smc[s.nSmc++] = emitPtr; *emitPtr++ = PPC_BNE(0);    // compiled code on the page -> bail
}

// ARM7 store through the cache's ST thunk. See jit_trace.h. The bail is the
// same cold JCOLD_SMCBAIL stub emitSmcCheckAndBail registers (r12 = the EA);
// the thunk has done nothing on that path but the page test.
void JitTraceCtx::emitArm7ThunkStore(u8 valReg, u32 size)
{
	{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
	if (valReg != PPC_R11) *emitPtr++ = PPC_OR(PPC_R11, valReg, valReg);
	const int t = size == 4 ? JTH_ST_U32 : size == 2 ? JTH_ST_U16 : JTH_ST_U8;
	*emitPtr = PPC_BL((s32)((u8*)cache.memThunk[t] - (u8*)emitPtr)); emitPtr++;
	}
	JIT_STAT_SCOPE(*this, JCS_SMCGUARD, false);
	JitColdStub& s = addCold(JCOLD_SMCBAIL);
	s.reg = PPC_R12;
	s.smc[s.nSmc++] = emitPtr; *emitPtr++ = PPC_BNE(0);    // SMC refusal -> bail
}

void JitTraceCtx::emitArm7ThunkStoreWord(u8 valReg)
{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, false);
	if (valReg != PPC_R11) *emitPtr++ = PPC_OR(PPC_R11, valReg, valReg);
	*emitPtr = PPC_BL((s32)((u8*)cache.memThunk[JTH_STSLOW_U32] - (u8*)emitPtr)); emitPtr++;
}

// Differential-harness store journal. See jit_trace.h. eaReg holds the guest
// address; the call records `size` pre-write bytes so jitRunArm*Checked()'s
// interpreter reference run can be rolled back. Compiles to nothing in a
// shipping build (cpu.journalNote == 0). Clobbers the volatile registers
// r3..r12 (and CTR / LR); r3 is saved to and restored from 92(r1).
static void emitJournalCall(u32*& p, const JitCpuProfile& cpu, u8 eaReg, u32 size)
{
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

void JitTraceCtx::emitJournalNote(u8 eaReg, u32 size)
{
	if (!cpu.journalNote) return;
	emitJournalCall(emitPtr, cpu, eaReg, size);
}

// The journal note of an inline ARM9 store (differential builds only), with
// r10 / r11 / r12 (host base, offset, EA) kept across the call.
static void emitJournalKeep(JitTraceCtx& c, u32 size)
{
	if (!c.cpu.journalNote) return;
	u32*& p = c.emitPtr;
	*p++ = PPC_STW(PPC_R10, 1, 108);
	*p++ = PPC_STW(PPC_R11, 1, 120);
	*p++ = PPC_STW(PPC_R12, 1, 124);
	emitJournalCall(p, c.cpu, PPC_R12, size);
	*p++ = PPC_LWZ(PPC_R10, 1, 108);
	*p++ = PPC_LWZ(PPC_R11, 1, 120);
	*p++ = PPC_LWZ(PPC_R12, 1, 124);
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
	JIT_STAT_SCOPE(*this, JCS_MEMGUARD, false);
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
	// The table's low half folds into the two lwz displacements (ha/lo):
	// addis r11, r10, ha ; lwz lo(r11) ; lwz lo+4(r11).
	const s32 descLo = (s32)(s16)(cpu.pageDescBase & 0xFFFF);
	s32 d = 0;
	if ((s32)(s16)cpu.pageDescBase == (s32)cpu.pageDescBase) {
		*p++ = PPC_ADDI(PPC_R11, PPC_R10, (s32)cpu.pageDescBase);
	} else if (descLo <= 0x7FFF - 4) {
		*p++ = PPC_ADDIS(PPC_R11, PPC_R10, (cpu.pageDescBase + 0x8000) >> 16);
		d = descLo;
	} else {
		*p++ = PPC_LIS(PPC_R11, cpu.pageDescBase >> 16);
		if (cpu.pageDescBase & 0xFFFF) *p++ = PPC_ORI(PPC_R11, PPC_R11, cpu.pageDescBase & 0xFFFF);
		*p++ = PPC_ADD(PPC_R11, PPC_R11, PPC_R10);
	}
	*p++ = PPC_LWZ(PPC_R10, PPC_R11, d);                // hostBase
	*p++ = PPC_LWZ(PPC_R11, PPC_R11, d + 4);            // mask
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

// =========================================================================
// P16 ARM9 accesses: predicted region inline, everything else out of line
// =========================================================================
// The old inline guard tested DTCM, main RAM and (loads) ITCM at every site
// and carried the slowRead / slowWrite C call and SMC bail inline -- ~52 PPC
// words per word LDR, ~64 per STR, most of it never executed. A site now
// inlines only the region memPredict names and branches to a cold stub at the
// block's tail for any other address; the stub calls the cache's shared thunk
// (jitEmitMemThunks()), which resolves DTCM -> main RAM -> ITCM -> C call in
// the old order, reading the DTCM window live. The access each address gets
// is unchanged; only where the code sits is.

// The ARM9 region decode baked at emit time (a TCM move flushes the cache).
namespace {
struct Arm9Regions { u32 region; s32 tag; bool reach, inMain, inItcm; u32 mainMb; };
}
static Arm9Regions jitArm9Regions(const JitCpuProfile& cpu)
{
	Arm9Regions g;
	g.region = *(const volatile u32*)(uintptr_t)cpu.arm9DtcmRegionPtr;
	// _MMU_*<ARM9> compares (addr & ~0x3FFF) == MMU.DTCMRegion against the
	// full value, so a window base with any of bits 0..13 set never matches.
	g.reach  = (g.region & 0x3FFFu) == 0;
	g.tag    = (s32)(g.region >> 14);
	g.inMain = g.reach && (g.region & 0x0F000000u) == 0x02000000u;
	g.inItcm = g.reach && g.region < 0x02000000u;
	g.mainMb = (u32)__builtin_clz(cpu.arm9MainMask);   // top set bit of the mirror mask
	return g;
}

u8 JitTraceCtx::arm9RegionOf(u32 ea) const
{
	const u32 region = *(const volatile u32*)(uintptr_t)cpu.arm9DtcmRegionPtr;
	if ((ea & ~0x3FFFu) == region) return JIT_MEMP_DTCM;
	if ((ea & 0x0F000000u) == 0x02000000u) return JIT_MEMP_MAIN;
	if (ea < 0x02000000u) return JIT_MEMP_ITCM;
	return JIT_MEMP_MAIN;
}

// cr0 EQ <=> the address in src lies in the (baked, reachable) DTCM window.
// Clobbers r10 (and r11 when the tag does not fit a cmpwi immediate).
static void emitDtcmTest(u32*& p, const Arm9Regions& g, u8 src)
{
	if (g.tag <= 0x7FFF) {
		*p++ = PPC_SRWI(PPC_R10, src, 14);
		*p++ = PPC_CMPWI(0, PPC_R10, g.tag);
	} else {
		*p++ = PPC_RLWINM(PPC_R10, src, 0, 0, 17);
		jitImm32(p, PPC_R11, g.region);
		*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
	}
}

// Inline test for the one region `pred` (JIT_MEMP_*) of an access at EA (r12)
// .. EA + span: DTCM; main RAM, excluding the DTCM window when it overlays
// main RAM (DTCM has priority); ITCM (single constant-EA loads), excluding the
// window when it overlays that range. With span, both ends must be inside (and
// for main RAM, inside one 1 MB page -- the run is <= 60 bytes, so a window
// that misses both ends misses the run). alignTest also sends an unaligned EA
// out (word loads, whose rotate the thunk does); for DTCM and main RAM it is
// folded into the region compare (one rotate keeps the region field and EA
// bits 0..1 side by side). Main RAM then compares the whole top byte, not just
// its low nibble: a 0x12..0xF2 mirror word load goes to the thunk, which reads
// the same main-RAM word. Every way out is a placeholder
// branch in s.miss; on the fall-through r10 = host base, r11 = the EA's offset
// in the region cleared to alignMe. Returns the region emitted.
static int emitArm9Predicted(JitTraceCtx& c, int pred, u8 alignMe, u32 span, bool alignTest, JitColdStub& s)
{
	JIT_STAT_SCOPE(c, JCS_MEMGUARD, false);
	u32*& p = c.emitPtr;
	const Arm9Regions g = jitArm9Regions(c.cpu);
	if (pred == JIT_MEMP_DTCM && !g.reach) pred = JIT_MEMP_MAIN;
	if (pred == JIT_MEMP_ITCM && span)     pred = JIT_MEMP_MAIN;
	auto miss = [&](u32 w) { s.miss[s.nMiss++] = p; *p++ = w; };
	u32 base; u8 mb;
	if (pred == JIT_MEMP_DTCM) {
		if (alignTest && !span && g.tag <= 0x7FFF) {
			// (EA >> 14) | (EA & 3) << 18 == tag: in the window and aligned
			*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 18, 12, 31);
			*p++ = PPC_CMPWI(0, PPC_R10, g.tag);
			alignTest = false;
		} else {
			emitDtcmTest(p, g, PPC_R12);
		}
		miss(PPC_BNE(0));
		if (span) {
			*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)span);
			emitDtcmTest(p, g, PPC_R10);
			miss(PPC_BNE(0));
		}
		base = c.cpu.arm9DtcmBase; mb = 18;
	} else if (pred == JIT_MEMP_ITCM) {
		if (g.inItcm) { emitDtcmTest(p, g, PPC_R12); miss(PPC_BEQ(0)); }
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 25);                      // EA < 0x02000000
		*p++ = PPC_CMPWI(0, PPC_R10, 0);
		miss(PPC_BNE(0));
		base = c.cpu.arm9ItcmBase; mb = 17;
	} else {
		if (g.inMain) { emitDtcmTest(p, g, PPC_R12); miss(PPC_BEQ(0)); }
		if (alignTest && !span) {
			*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 22, 31);         // (EA >> 24) | (EA & 3) << 8 == 2
			alignTest = false;
		} else {
			*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 28, 31);         // (EA >> 24) & 0xF == 2
		}
		*p++ = PPC_CMPWI(0, PPC_R10, 2);
		miss(PPC_BNE(0));
		if (span) {
			*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);                  // one 1 MB page
			*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)span);
			*p++ = PPC_SRWI(PPC_R11, PPC_R11, 20);
			*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
			miss(PPC_BNE(0));
			if (g.inMain) {                                         // ...whose end is not DTCM
				*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)span);
				emitDtcmTest(p, g, PPC_R10);
				miss(PPC_BEQ(0));
			}
		}
		base = c.cpu.mainMemBase; mb = (u8)g.mainMb;
	}
	if (alignTest) { *p++ = PPC_ANDI_(0, PPC_R12, 3); miss(PPC_BNE(0)); }
	jitImm32(p, PPC_R10, base);
	*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, mb, alignMe);
	return pred;
}

// dst <- host [rA + rB], byte-swapped, with the access's sign extension.
static void emitLoadOp(u32*& p, u8 dst, u8 rA, u8 rB, u32 size, bool signExt)
{
	if (size == 4) {
		*p++ = PPC_LWBRX(dst, rA, rB);
	} else if (size == 2) {
		*p++ = PPC_LHBRX(dst, rA, rB);
		if (signExt) *p++ = PPC_EXTSH(dst, dst);
	} else {
		*p++ = PPC_LBZX(dst, rA, rB);
		if (signExt) *p++ = PPC_EXTSB(dst, dst);
	}
}
static void emitStoreOp(u32*& p, u8 val, u8 rA, u8 rB, u32 size)
{
	if (size == 4)      *p++ = PPC_STWBRX(val, rA, rB);
	else if (size == 2) *p++ = PPC_STHBRX(val, rA, rB);
	else                *p++ = PPC_STBZX (val, rA, rB);
}
static inline int jitLoadThunk(u32 size, bool signExt, bool wordRotate)
{
	return size == 1 ? (signExt ? JTH_LD_S8 : JTH_LD_U8)
	     : size == 2 ? (signExt ? JTH_LD_S16 : JTH_LD_U16)
	     : (wordRotate ? JTH_LD_U32ROT : JTH_LD_U32);
}

// The shared thunks. See JitMemThunk (jit_cache.h) for the calling contract.
// Their C-call paths do exactly what the old inline slow paths did (guest PC
// -> gpr[15], r3 saved at 92(r1), slowRead / slowWrite, word ROR by 8*(EA&3)),
// with LR at 112(r1) and the EA at 116(r1) across the call.
void jitEmitMemThunks(JITCache& c, u32*& p)
{
	const JitCpuProfile& cpu = *c.thunkProfile;
	const u32 regionPtr = cpu.arm9DtcmRegionPtr;
	const u32 mainMb    = cpu.arm9MainMask ? (u32)__builtin_clz(cpu.arm9MainMask) : 0;
	const u32 flags     = (u32)c.smcPageFlags;

	// cr0 EQ <=> EA (r12) is in the DTCM window, read live. Clobbers r10, rTmp.
	auto liveDtcmTest = [&](u8 rTmp) {
		*p++ = PPC_LIS(PPC_R10, (regionPtr + 0x8000) >> 16);
		*p++ = PPC_LWZ(PPC_R10, PPC_R10, (s32)(s16)(regionPtr & 0xFFFF));
		*p++ = PPC_RLWINM(rTmp, PPC_R12, 0, 0, 17);
		*p++ = PPC_CMPW(0, rTmp, PPC_R10);
	};
	// The old emitMemPrologue(), plus LR.
	auto callPrologue = [&]() {
		*p++ = PPC_MFLR(0);
		*p++ = PPC_STW(0, 1, 112);
		*p++ = PPC_LWZ(PPC_R10, 1, 80);                    // gpr base
		*p++ = PPC_STW(PPC_R29, PPC_R10, 15 * 4);          // guest PC -> gpr[15]
		*p++ = PPC_STW(PPC_R3, 1, 92);                     // cycle accumulator
	};
	auto callEpilogue = [&]() {
		*p++ = PPC_LWZ(PPC_R3, 1, 92);
		*p++ = PPC_LWZ(0, 1, 112);
		*p++ = PPC_MTLR(0);
	};
	// Differential builds: journal the store at r12, keeping r11 / r12 / LR.
	auto journal = [&](u32 size) {
		if (!cpu.journalNote) return;
		*p++ = PPC_MFLR(0);
		*p++ = PPC_STW(0, 1, 112);
		*p++ = PPC_STW(PPC_R11, 1, 120);
		*p++ = PPC_STW(PPC_R12, 1, 124);
		emitJournalCall(p, cpu, PPC_R12, size);
		*p++ = PPC_LWZ(PPC_R11, 1, 120);
		*p++ = PPC_LWZ(PPC_R12, 1, 124);
		*p++ = PPC_LWZ(0, 1, 112);
		*p++ = PPC_MTLR(0);
	};

	if (!cpu.arm9DtcmBase) {
		// ARM7 (no TCM): loads are the old out-of-window slowRead path of
		// emitInlineLoad / emitInlineBlockLoad (reached from their cold stubs),
		// stores the old SMC page test + slowWrite (emitArm7ThunkStore).
		for (int k = JTH_LD_U8; k <= JTH_LD_U32ROT; k++) {
			const u32  size = k <= JTH_LD_S8 ? 1 : k <= JTH_LD_S16 ? 2 : 4;
			const bool sx   = k == JTH_LD_S8 || k == JTH_LD_S16;
			const bool rot  = k == JTH_LD_U32ROT;
			c.memThunk[k] = p;
			callPrologue();
			if (rot) *p++ = PPC_STW(PPC_R12, 1, 116);
			*p++ = PPC_OR(PPC_R3, PPC_R12, PPC_R12);
			*p++ = PPC_LI(PPC_R4, (s32)size);
			jitCallC(p, (u32)cpu.slowRead);
			*p++ = PPC_OR(PPC_R10, PPC_R3, PPC_R3);
			if (sx && size == 1) *p++ = PPC_EXTSB(PPC_R10, PPC_R10);
			if (sx && size == 2) *p++ = PPC_EXTSH(PPC_R10, PPC_R10);
			if (rot) {                                          // ROR(R10, 8 * (EA & 3))
				*p++ = PPC_LWZ(PPC_R12, 1, 116);
				*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);
				*p++ = PPC_SUBFIC(PPC_R12, PPC_R12, 4);
				*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);
				*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
			}
			callEpilogue();
			*p++ = PPC_BLR();
		}
		for (int k = JTH_ST_U8; k <= JTH_STSLOW_U32; k++) {
			const u32 size = k == JTH_ST_U8 ? 1 : k == JTH_ST_U16 ? 2 : 4;
			c.memThunk[k] = p;
			if (k != JTH_STSLOW_U32) {
				emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R10, flags);
				*p++ = PPC_BNELR();
			}
			callPrologue();
			*p++ = PPC_OR(PPC_R3, PPC_R12, PPC_R12);
			*p++ = PPC_OR(PPC_R4, PPC_R11, PPC_R11);
			*p++ = PPC_LI(PPC_R5, (s32)size);
			jitCallC(p, (u32)cpu.slowWrite);
			callEpilogue();
			*p++ = PPC_CMPW(0, PPC_R3, PPC_R3);
			*p++ = PPC_BLR();
		}
		return;
	}

	// ---- loads: value -> r10 ----
	for (int k = JTH_LD_U8; k <= JTH_LD_U32ROT; k++) {
		const u32  size = k <= JTH_LD_S8 ? 1 : k <= JTH_LD_S16 ? 2 : 4;
		const bool sx   = k == JTH_LD_S8 || k == JTH_LD_S16;
		const bool rot  = k == JTH_LD_U32ROT;
		const u8   al   = jitAlignBit(size);
		c.memThunk[k] = p;
		u32* toAcc[2];
		liveDtcmTest(PPC_R11);
		u32* notDtcm = p++;
		jitImm32(p, PPC_R10, cpu.arm9DtcmBase);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 18, al);
		toAcc[0] = p++;
		*notDtcm = PPC_BNE((u32)((p - notDtcm) * 4));
		*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 28, 31);         // main RAM
		*p++ = PPC_CMPWI(0, PPC_R10, 2);
		u32* notMain = p++;
		jitImm32(p, PPC_R10, cpu.mainMemBase);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, mainMb, al);
		toAcc[1] = p++;
		*notMain = PPC_BNE((u32)((p - notMain) * 4));
		*p++ = PPC_SRWI(PPC_R10, PPC_R12, 25);                  // ITCM: EA < 0x02000000
		*p++ = PPC_CMPWI(0, PPC_R10, 0);
		u32* toSlow = p++;
		jitImm32(p, PPC_R10, cpu.arm9ItcmBase);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 17, al);
		for (int i = 0; i < 2; i++) *toAcc[i] = PPC_B((u32)((p - toAcc[i]) * 4));
		emitLoadOp(p, PPC_R10, PPC_R10, PPC_R11, size, sx);
		if (rot) {                                              // ROR by 8x == ROL by (-8x) & 31
			*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 3, 27, 28);
			*p++ = PPC_NEG(PPC_R11, PPC_R11);
			*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R11, 0, 31);
		}
		*p++ = PPC_BLR();
		// slowRead C call
		*toSlow = PPC_BNE((u32)((p - toSlow) * 4));
		callPrologue();
		if (rot) *p++ = PPC_STW(PPC_R12, 1, 116);
		*p++ = PPC_OR(PPC_R3, PPC_R12, PPC_R12);
		*p++ = PPC_LI(PPC_R4, (s32)size);
		jitCallC(p, (u32)cpu.slowRead);
		*p++ = PPC_OR(PPC_R10, PPC_R3, PPC_R3);
		if (sx && size == 1) *p++ = PPC_EXTSB(PPC_R10, PPC_R10);
		if (sx && size == 2) *p++ = PPC_EXTSH(PPC_R10, PPC_R10);
		if (rot) {                                              // ROR(R10, 8 * (EA & 3))
			*p++ = PPC_LWZ(PPC_R12, 1, 116);
			*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);
			*p++ = PPC_SUBFIC(PPC_R12, PPC_R12, 4);
			*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);
			*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
		}
		callEpilogue();
		*p++ = PPC_BLR();
	}

	// ---- stores: value in r11; cr0 EQ done / NE SMC refusal ----
	for (int k = JTH_ST_U8; k <= JTH_ST_U32; k++) {
		const u32 size = k == JTH_ST_U8 ? 1 : k == JTH_ST_U16 ? 2 : 4;
		const u8  al   = jitAlignBit(size);
		c.memThunk[k] = p;
		liveDtcmTest(PPC_R9);                                   // DTCM: never holds JIT code
		u32* notDtcm = p++;
		journal(size);
		jitImm32(p, PPC_R10, cpu.arm9DtcmBase);
		*p++ = PPC_RLWINM(PPC_R9, PPC_R12, 0, 18, al);
		emitStoreOp(p, PPC_R11, PPC_R10, PPC_R9, size);
		*p++ = PPC_CMPW(0, PPC_R12, PPC_R12);
		*p++ = PPC_BLR();
		*notDtcm = PPC_BNE((u32)((p - notDtcm) * 4));
		*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 28, 31);         // main RAM, SMC-guarded
		*p++ = PPC_CMPWI(0, PPC_R10, 2);
		u32* toSlow = p++;
		emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R10, flags);
		*p++ = PPC_BNELR();
		journal(size);
		jitImm32(p, PPC_R10, cpu.mainMemBase);
		*p++ = PPC_RLWINM(PPC_R9, PPC_R12, 0, mainMb, al);
		emitStoreOp(p, PPC_R11, PPC_R10, PPC_R9, size);
		*p++ = PPC_CMPW(0, PPC_R12, PPC_R12);
		*p++ = PPC_BLR();
		// SMC guard + slowWrite C call (every other region, ITCM included)
		*toSlow = PPC_BNE((u32)((p - toSlow) * 4));
		emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R10, flags);
		*p++ = PPC_BNELR();
		callPrologue();
		*p++ = PPC_OR(PPC_R3, PPC_R12, PPC_R12);
		*p++ = PPC_OR(PPC_R4, PPC_R11, PPC_R11);
		*p++ = PPC_LI(PPC_R5, (s32)size);
		jitCallC(p, (u32)cpu.slowWrite);
		callEpilogue();
		*p++ = PPC_CMPW(0, PPC_R3, PPC_R3);
		*p++ = PPC_BLR();
	}

	// ---- slowWrite only (block-store slow loop; its SMC tests ran already) ----
	c.memThunk[JTH_STSLOW_U32] = p;
	callPrologue();
	*p++ = PPC_OR(PPC_R3, PPC_R12, PPC_R12);
	*p++ = PPC_OR(PPC_R4, PPC_R11, PPC_R11);
	*p++ = PPC_LI(PPC_R5, 4);
	jitCallC(p, (u32)cpu.slowWrite);
	callEpilogue();
	*p++ = PPC_BLR();
}

JitColdStub& JitTraceCtx::addCold(u8 type)
{
	static JitColdStub s_spill;                      // overflow sink (block is then dropped)
	JitColdStub& s = coldCount < JIT_MAX_COLD ? cold[coldCount] : (coldOverflow = true, s_spill);
	coldCount += coldCount < JIT_MAX_COLD;
	memset(&s, 0, sizeof s);
	s.type   = type;
	s.cycles = cyclesAccum;
	s.icount = instrCount;
	s.pc     = currentPC;
	return s;
}

// P16: full ARM9 single load. See jit_trace.h. EA in PPC_R12. Hit: the
// predicted region's test and one indexed load straight into rd's pinned
// register; anything else (or an unaligned rotating word load) goes to the
// cold stub, `bl LD thunk ; mr rd, r10 ; b back`.
void JitTraceCtx::emitArm9Load(u8 rd, u32 size, bool signExt, bool wordRotate, bool writeback, u8 rn)
{
	u32*& p = emitPtr;
	const u8 hRd = hostRegFor(rd);
	const int pred = memPredict;
	memPredict = JIT_MEMP_DTCM;
	JitColdStub& s = addCold(JCOLD_LOAD);
	s.thunk = (u8)jitLoadThunk(size, signExt, wordRotate);
	s.reg   = hRd;
	emitArm9Predicted(*this, pred, jitAlignBit(size), 0, size == 4 && wordRotate, s);
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	emitLoadOp(p, hRd, PPC_R10, PPC_R11, size, signExt);
	}
	s.back = p;
	// (the caller excludes rd == rn with writeback)
	if (writeback) *p++ = PPC_LWZ(hostRegFor(rn), 1, 104);
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

	// DS ARM7: the slow path below lives in the cache's LD thunk, reached from
	// a cold stub at the block's tail (`bl LD thunk ; mr rd, r10 ; b back`).
	JitColdStub* cs = nullptr;
	if (arm7Thunks()) {
		cs = &addCold(JCOLD_LOAD);
		cs->thunk = (u8)jitLoadThunk(size, signExt, wordRotate);
		cs->reg   = hDst;
		for (int k = 0; k < nMiss; k++) cs->miss[cs->nMiss++] = miss[k];
	}

	// ---- fast: descriptor hit, inline load ----
	u32* toEnd = nullptr;
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	if (size == 4) {
		*p++ = PPC_LWBRX(hDst, PPC_R10, PPC_R11);
		// ARM OP_LDR rotates an unaligned word: ROR(word, 8 * (EA & 3)). The
		// caller passes wordRotate to match its own slow path exactly (THUMB
		// PC/SP-relative loads have a word-aligned EA and pass false).
		if (wordRotate) {
			*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 3, 27, 28); // 8 * (EA & 3)
			*p++ = PPC_NEG(PPC_R11, PPC_R11);               // ROL by -8x & 31 == ROR by 8x
			*p++ = PPC_RLWNM(hDst, hDst, PPC_R11, 0, 31);
		}
	} else if (size == 2) {
		*p++ = PPC_LHBRX(hDst, PPC_R10, PPC_R11);
		if (signExt) *p++ = PPC_EXTSH(hDst, hDst);
	} else {
		*p++ = PPC_LBZX(hDst, PPC_R10, PPC_R11);
		if (signExt) *p++ = PPC_EXTSB(hDst, hDst);
	}
	if (cs) { cs->back = p; return true; }
	toEnd = p++;                                        // B over the slow path
	}

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
	{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, true);
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
	}

	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	return true;
}

// P16: ARM9 inline block load. See jit_trace.h. Low EA in PPC_R12 (and still
// there afterwards). Hit: the predicted region covering the whole run, then n
// sequential lwbrx into the pinned registers. Otherwise the cold stub loads
// each word through the LD thunk -- per word, which yields what the old
// whole-run guard's per-word slowRead fallback did (reads have no side
// effects in the inline-able regions, and slowRead resolves DTCM priority
// per word itself).
void JitTraceCtx::emitArm9BlockLoad(const u8* regs, u32 n)
{
	u32*& p = emitPtr;
	int pred = memPredict;
	memPredict = JIT_MEMP_DTCM;
	if (pred == JIT_MEMP_ITCM) pred = JIT_MEMP_MAIN;
	JitColdStub& s = addCold(JCOLD_BLOCKLOAD);
	s.thunk = JTH_LD_U32;
	s.n     = (u8)n;
	for (u32 k = 0; k < n; k++) s.regs[k] = hostRegFor(regs[k]);
	emitArm9Predicted(*this, pred, /*alignMe=*/29, 4 * (n - 1), false, s);
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);                     // r10 = host addr of the low word
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWBRX(hostRegFor(regs[k]), 0, PPC_R10);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R10, PPC_R10, 4);
	}
	}
	s.back = p;
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

	// DS ARM7: the per-word slow loop below is a cold JCOLD_BLOCKLOAD stub at
	// the block's tail instead, one LD thunk call per word (same EA reloads,
	// low EA back in r12).
	JitColdStub* cs = nullptr;
	if (arm7Thunks()) {
		cs = &addCold(JCOLD_BLOCKLOAD);
		cs->thunk = JTH_LD_U32;
		cs->n     = (u8)n;
		for (u32 k = 0; k < n; k++) cs->regs[k] = hostRegFor(regs[k]);
		for (int k = 0; k < nMiss; k++) cs->miss[cs->nMiss++] = miss[k];
	}

	// ---- fast: r10 = hostBase, r11 = aligned page offset of the low word ----
	u32* toEnd;
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	for (u32 k = 0; k < n; k++) {
		const u8 hgi = writeReg(regs[k], /*fullOverwrite=*/true, lockedMask);
		*p++ = PPC_LWBRX(hgi, PPC_R10, PPC_R11);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R11, PPC_R11, 4);
	}
	if (cs) { cs->back = p; return true; }
	toEnd = p++;                                        // B over the slow path
	}

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
	{
	JIT_STAT_SCOPE(*this, JCS_MEMSLOW, true);
	*p++ = PPC_STW(PPC_R12, 1, 96);                     // stash low EA
	emitMemPrologue();
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		if (k) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
		emitSlowLoad(hostRegFor(regs[k]), PPC_R12, 4, false);
	}
	emitMemEpilogue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);                     // restore low EA
	}

	*toEnd = PPC_B((u32)((p - toEnd) * 4));
	return true;
}

// P16: full ARM9 single store. See jit_trace.h. EA in PPC_R12; the value is in
// hVal, a pinned guest register. Hit: the predicted region's test, for main
// RAM the SMC page test (compiled code on the page -> the site's cold SMC
// bail), the journal note in differential builds, one indexed store. Any other
// region: the cold stub, `mr r11, hVal ; bl ST thunk ; beq back`, falling into
// the same bail when the thunk refuses.
void JitTraceCtx::emitArm9Store(u32 size, bool writeback, u8 rn, u8 hVal)
{
	u32*& p = emitPtr;
	int pred = memPredict;
	memPredict = JIT_MEMP_DTCM;
	if (pred == JIT_MEMP_ITCM) pred = JIT_MEMP_MAIN;              // ITCM stores keep slowWrite
	JitColdStub& s = addCold(JCOLD_STORE);
	s.thunk = (u8)(size == 4 ? JTH_ST_U32 : size == 2 ? JTH_ST_U16 : JTH_ST_U8);
	s.reg   = hVal;
	const int region = emitArm9Predicted(*this, pred, jitAlignBit(size), 0, false, s);
	if (region == JIT_MEMP_MAIN) {
		JIT_STAT_SCOPE(*this, JCS_SMCGUARD, false);
		emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R8, (u32)cache.smcPageFlags);
		s.smc[s.nSmc++] = p; *p++ = PPC_BNE(0);
	}
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	emitJournalKeep(*this, size);
	emitStoreOp(p, hVal, PPC_R10, PPC_R11, size);
	}
	s.back = p;
	// writeback straight into the pinned base register
	if (writeback) *p++ = PPC_LWZ(hostRegFor(rn), 1, 104);
}

// P16: ARM9 inline block store (STM / PUSH / STMIA, non-pc). See jit_trace.h.
// Low EA in PPC_R12 (and still there afterwards); each source register is read
// straight from its pinned host register. Hit: the predicted region covering
// the whole run (main RAM: SMC test of the low end, then of the high end, whose
// bail reports EA + span as the old one did), one journal note for the span,
// n sequential stwbrx. Everything else: the cold stub repeats the old
// whole-run decision (see emitColdStubs()).
void JitTraceCtx::emitArm9BlockStore(const u8* regs, u32 n)
{
	u32*& p = emitPtr;
	const u32 span = 4 * (n - 1);
	int pred = memPredict;
	memPredict = JIT_MEMP_DTCM;
	if (pred == JIT_MEMP_ITCM) pred = JIT_MEMP_MAIN;
	JitColdStub& s = addCold(JCOLD_BLOCKSTORE);
	s.n    = (u8)n;
	s.span = span;
	for (u32 k = 0; k < n; k++) s.regs[k] = hostRegFor(regs[k]);
	const int region = emitArm9Predicted(*this, pred, /*alignMe=*/29, span, false, s);
	if (region == JIT_MEMP_MAIN) {
		JIT_STAT_SCOPE(*this, JCS_SMCGUARD, false);
		const u32 fp = (u32)cache.smcPageFlags;
		emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R8, fp);              // low end
		s.smc[s.nSmc++] = p; *p++ = PPC_BNE(0);
		if (span) {                                                    // the run may cross a 1 KB page
			*p++ = PPC_ADDI(PPC_R9, PPC_R12, (s32)span);
			emitSmcPageTest(p, PPC_R9, PPC_R9, PPC_R8, fp);
			s.smcHigh[s.nSmcHigh++] = p; *p++ = PPC_BNE(0);
		}
	}
	{
	JIT_STAT_SCOPE(*this, JCS_MEMHIT, false);
	*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);                     // host addr of the low word
	emitJournalKeep(*this, 4 * n);
	for (u32 k = 0; k < n; k++) {
		*p++ = PPC_STWBRX(hostRegFor(regs[k]), 0, PPC_R10);
		if (k + 1 < n) *p++ = PPC_ADDI(PPC_R10, PPC_R10, 4);
	}
	}
	s.back = p;
}

// Emitted by jitCompileTrace() after the block's last exit (the yield stub):
// every cold stub registered during the scan, with its branch slots aimed at
// it. Fails (sets coldOverflow, the block is then not used) if a slot's
// conditional branch cannot reach -- never for a block within the cut budget.
void JitTraceCtx::emitColdStubs()
{
	u32*& p = emitPtr;
	auto aim = [&](u32* slot) {
		const s32 off = (s32)((p - slot) * 4);
		if (off >= 0x8000) coldOverflow = true;
		*slot |= (u32)off & 0xFFFC;
	};
	auto branchTo = [&](u32 w, const u32* target) {                  // b / beq back into the hot path
		const s32 off = (s32)((target - p) * 4);
		if ((w >> 26) == 16 && off < -0x8000) coldOverflow = true;
		*p = w | ((u32)off & ((w >> 26) == 16 ? 0xFFFCu : 0x3FFFFFCu)); p++;
	};
	auto call = [&](int t) { *p = PPC_BL((s32)((u8*)cache.memThunk[t] - (u8*)p)); p++; };
	const u32 fp = (u32)cache.smcPageFlags;

	for (u32 i = 0; i < coldCount; i++) {
		JitColdStub& s = cold[i];
		JIT_STAT_SCOPE(*this, JCS_MEMSLOW, true);
		for (u32 k = 0; k < s.nMiss; k++) aim(s.miss[k]);
		switch (s.type) {
		case JCOLD_LOAD:
			call(s.thunk);
			*p++ = PPC_OR(s.reg, PPC_R10, PPC_R10);
			branchTo(PPC_B(0), s.back);
			break;
		case JCOLD_STORE:
			*p++ = PPC_OR(PPC_R11, s.reg, s.reg);
			call(s.thunk);
			branchTo(PPC_BEQ(0), s.back);                             // NE: SMC refusal -> bail below
			break;
		case JCOLD_BLOCKLOAD:
			*p++ = PPC_STW(PPC_R12, 1, 96);                           // low EA
			for (u32 k = 0; k < s.n; k++) {
				if (k) {
					*p++ = PPC_LWZ(PPC_R12, 1, 96);
					*p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
				}
				call(JTH_LD_U32);
				*p++ = PPC_OR(s.regs[k], PPC_R10, PPC_R10);
			}
			*p++ = PPC_LWZ(PPC_R12, 1, 96);
			branchTo(PPC_B(0), s.back);
			break;
		case JCOLD_BLOCKSTORE: {
			// The old whole-run decision: the whole run in the DTCM window ->
			// inline stores; else in main RAM within one 1 MB page and not
			// ending in DTCM -> SMC test of both ends, inline stores; else SMC
			// test of both ends and a slowWrite per word.
			const Arm9Regions g = jitArm9Regions(cpu);
			u32* toSlow[4]; int nSlow = 0;
			u32* toStore = nullptr;
			*p++ = PPC_STW(PPC_R12, 1, 96);                           // low EA
			if (g.reach) {
				emitDtcmTest(p, g, PPC_R12);
				u32* notDtcm = p++;
				if (s.span) {
					*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)s.span);
					emitDtcmTest(p, g, PPC_R10);
					toSlow[nSlow++] = p; *p++ = PPC_BNE(0);
				}
				jitImm32(p, PPC_R10, cpu.arm9DtcmBase);
				*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 18, 29);
				toStore = p++;
				*notDtcm = PPC_BNE((u32)((p - notDtcm) * 4));
			}
			*p++ = PPC_RLWINM(PPC_R10, PPC_R12, 8, 28, 31);
			*p++ = PPC_CMPWI(0, PPC_R10, 2);
			toSlow[nSlow++] = p; *p++ = PPC_BNE(0);
			if (s.span) {
				*p++ = PPC_SRWI(PPC_R10, PPC_R12, 20);
				*p++ = PPC_ADDI(PPC_R11, PPC_R12, (s32)s.span);
				*p++ = PPC_SRWI(PPC_R11, PPC_R11, 20);
				*p++ = PPC_CMPW(0, PPC_R10, PPC_R11);
				toSlow[nSlow++] = p; *p++ = PPC_BNE(0);
				if (g.inMain) {
					*p++ = PPC_ADDI(PPC_R10, PPC_R12, (s32)s.span);
					emitDtcmTest(p, g, PPC_R10);
					toSlow[nSlow++] = p; *p++ = PPC_BEQ(0);
				}
			}
			emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R8, fp);
			s.smc[s.nSmc++] = p; *p++ = PPC_BNE(0);
			if (s.span) {
				*p++ = PPC_ADDI(PPC_R9, PPC_R12, (s32)s.span);
				emitSmcPageTest(p, PPC_R9, PPC_R9, PPC_R8, fp);
				s.smcHigh[s.nSmcHigh++] = p; *p++ = PPC_BNE(0);
			}
			jitImm32(p, PPC_R10, cpu.mainMemBase);
			*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, g.mainMb, 29);
			if (toStore) *toStore = PPC_B((u32)((p - toStore) * 4));
			*p++ = PPC_ADD(PPC_R10, PPC_R10, PPC_R11);
			emitJournalKeep(*this, 4 * s.n);
			for (u32 k = 0; k < s.n; k++) {
				*p++ = PPC_STWBRX(s.regs[k], 0, PPC_R10);
				if (k + 1 < s.n) *p++ = PPC_ADDI(PPC_R10, PPC_R10, 4);
			}
			u32* toDone = p++;
			for (int k = 0; k < nSlow; k++) aim(toSlow[k]);
			emitSmcPageTest(p, PPC_R12, PPC_R9, PPC_R8, fp);
			s.smc[s.nSmc++] = p; *p++ = PPC_BNE(0);
			if (s.span) {
				*p++ = PPC_ADDI(PPC_R9, PPC_R12, (s32)s.span);
				emitSmcPageTest(p, PPC_R9, PPC_R9, PPC_R8, fp);
				s.smcHigh[s.nSmcHigh++] = p; *p++ = PPC_BNE(0);
			}
			for (u32 k = 0; k < s.n; k++) {
				if (k) {
					*p++ = PPC_LWZ(PPC_R12, 1, 96);
					*p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(k * 4));
				}
				*p++ = PPC_OR(PPC_R11, s.regs[k], s.regs[k]);
				call(JTH_STSLOW_U32);
			}
			*toDone = PPC_B((u32)((p - toDone) * 4));
			*p++ = PPC_LWZ(PPC_R12, 1, 96);                           // low EA, as the hot path leaves it
			branchTo(PPC_B(0), s.back);
			break;
		}
		default:   // JCOLD_SMCBAIL: the bail only
			break;
		}
		if (s.type == JCOLD_LOAD || s.type == JCOLD_BLOCKLOAD) continue;

		// ---- SMC bail: resume at the store with the cycles / instructions
		// before it, r12 = the refused EA (reported as smcAddress) ----
		JIT_STAT_SCOPE(*this, JCS_SMCBAIL, true);
		if (s.nSmcHigh) {                                              // high end: report EA + span
			for (u32 k = 0; k < s.nSmcHigh; k++) aim(s.smcHigh[k]);
			*p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)s.span);
		}
		for (u32 k = 0; k < s.nSmc; k++) aim(s.smc[k]);
		if (s.type == JCOLD_SMCBAIL && s.reg != PPC_R12) *p++ = PPC_OR(PPC_R12, s.reg, s.reg);
		if (s.cycles) *p++ = PPC_ADDI(PPC_R3, PPC_R3, (s32)s.cycles);
		if (s.icount) *p++ = PPC_ADDI(PPC_R31, PPC_R31, (s32)s.icount);
		*p++ = PPC_LIS(PPC_R4, s.pc >> 16);
		*p++ = PPC_ORI(PPC_R4, PPC_R4, s.pc & 0xFFFF);
		*p = PPC_B((s32)((u8*)cache.smcTailAddress - (u8*)p)); p++;
	}
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
void JitTraceCtx::emitChainTail(u32 targetPC)
{
	JIT_STAT_SCOPE(*this, JCS_EXIT, false);
	u32*& p = emitPtr;
#if JIT_ENABLE_CHAINING
	{ s32 o = (s32)((u8*)cache.linkerStubAddress - (u8*)p); *p++ = PPC_BL(o); }
	*p++ = targetPC;                                   // data word, read by the stub via LR
#else
	*p++ = PPC_LIS(PPC_R4, targetPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
	{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)p); *p++ = PPC_B(o); }
#endif
}

void JitTraceCtx::emitStaticExit(u32 targetPC, u32 metaCount, u32 termCycles)
{
	JIT_STAT_SCOPE(*this, JCS_EXIT, false);
	emitAddCycles(cyclesAccum + termCycles);
	emitResultMetadata(metaCount, 0);
	emitChainTail(targetPC);
}

void JitTraceCtx::emitDynamicExit(u8 pcReg, u32 metaCount, u32 termCycles, bool targetThumb)
{
	JIT_STAT_SCOPE(*this, JCS_EXIT, false);
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
	JIT_STAT_SCOPE(*this, JCS_EXIT, false);
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
	JIT_STAT_SCOPE(*this, JCS_FALLBACK, false);
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

// Every condition is a test on the packed flags in r30 (N Z C V = PPC bits
// 0..3) that leaves its answer in cr0, then one bc taken when it FAILS:
//   EQ..VC  rlwinm. r11,r30,0,f,f                        eq <=> flag clear
//   HI/LS   rlwinm r11,r30,1,1,1 ; andc. r11,r11,r30      C -> bit 1, & ~Z: ne <=> HI
//   GE/LT   rotlwi r11,r30,3 ; xor. r11,r11,r30           V -> bit 0, ^ N: lt <=> LT
//   GT/LE   rlwinm r11,r30,3,0,0 ; xor r11,r11,r30 ;
//           rlwinm. r11,r11,0,0,1                         bits 0/1 = N^V, Z: ne <=> LE
// Mirrors the CONDITION() table in armcpu.h / arm_instructions.cpp.
u32* JitTraceCtx::emitCondSkip(u8 cond)
{
	JIT_STAT_SCOPE(*this, JCS_PRED, false);
	ensureFlagsLoaded();
	u32*& p = emitPtr;
	const u32 F = PPC_REG_FLAGS;
	u32 skip;                                                      // taken when cond fails
	switch (cond) {
	case 0x0: case 0x1: case 0x2: case 0x3:
	case 0x4: case 0x5: case 0x6: case 0x7: {
		static const u8 kFlag[4] = { JITF_Z, JITF_C, JITF_N, JITF_V };
		const u8 f = kFlag[cond >> 1];
		*p++ = PPC_RLWINM(PPC_R11, F, 0, f, f) | 1;
		skip = (cond & 1) ? PPC_BNE(0) : PPC_BEQ(0);               // EQ/CS/MI/VS need the flag set
		break;
	}
	case 0x8: case 0x9:                                            // HI  C & ~Z / LS
		*p++ = PPC_RLWINM(PPC_R11, F, 1, JITF_Z, JITF_Z);
		*p++ = PPC_ANDC(PPC_R11, PPC_R11, F) | 1;
		skip = (cond == 0x8) ? PPC_BEQ(0) : PPC_BNE(0);
		break;
	case 0xA: case 0xB:                                            // GE  ~(N ^ V) / LT
		*p++ = PPC_RLWINM(PPC_R11, F, 3, 0, 31);
		*p++ = PPC_XOR(PPC_R11, PPC_R11, F) | 1;
		skip = (cond == 0xA) ? PPC_BLT(0) : PPC_BGE(0);
		break;
	case 0xC: case 0xD:                                            // GT  ~Z & ~(N ^ V) / LE
		*p++ = PPC_RLWINM(PPC_R11, F, 3, 0, 0);
		*p++ = PPC_XOR(PPC_R11, PPC_R11, F);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 0, 0, 1) | 1;
		skip = (cond == 0xC) ? PPC_BNE(0) : PPC_BEQ(0);
		break;
	default:                                                       // AL: never skips
		*p++ = PPC_LI(PPC_R11, 1);
		*p++ = PPC_CMPWI(0, PPC_R11, 0);
		skip = PPC_BEQ(0);
		break;
	}
	u32* slot = p;
	*p++ = skip;
	return slot;
}

// =========================================================================
// Dead-flag elimination (THUMB and ARM)
// =========================================================================
// Nearly every THUMB ALU op sets flags, and most of those values are
// overwritten by the next flag-setting op before anything reads them -- yet
// each one costs a cntlzw/rlwimi pair for N/Z and an mfxer (execution-
// serialising on the 750-class core) plus two rlwimi for C/V. A backward
// liveness pass over the scan window finds, per instruction, the flags whose
// value no later instruction can observe, and the flag helpers skip them.
//
// Soundness rests on the barrier set: any instruction that can read the
// flags, leave the block (branch, bail, SMC guard on a store, interpreter
// fallback that may return to C) or that the front end might refuse (which
// turns it into a fallback) is a barrier -- every flag is live before it, and
// its own flag writes are never elided. Only straight-line ALU ops and loads
// (which never exit: out-of-window loads run slowRead in place) are
// transparent. The window is the block as jitCutLimit() bounds it, so a
// block cut short still materialises every flag its last instructions write.
//
// Classifies one THUMB opcode: returns true for a barrier, else the flags it
// reads / definitely writes (JITF bit masks). A conditional write (LSL #0's C,
// shift-by-register C for amount 0) is not a definite write and so is left
// out, which keeps the flag live through it.
static bool jitThumbFlagClass(u16 op, u8& rd, u8& wr)
{
	const u8 NZ = (1u << JITF_N) | (1u << JITF_Z), C = 1u << JITF_C, ALL = 0xF;
	rd = wr = 0;
	switch (op >> 11) {
	case 0:          wr = NZ | (((op >> 6) & 0x1F) ? C : 0); return false; // LSL imm
	case 1: case 2:  wr = NZ | C;  return false;                  // LSR / ASR imm
	case 3:          wr = ALL;     return false;                  // ADD/SUB reg/imm3
	case 4:          wr = NZ;      return false;                  // MOV imm8
	case 5: case 6: case 7: wr = ALL; return false;               // CMP/ADD/SUB imm8
	case 8:
		if (op & 0x0400) {                                        // F5 hi-reg / BX
			const u8 sub = (op >> 8) & 3;
			if (sub == 3) return true;                            // BX / BLX
			if (sub == 1) { wr = ALL; return false; }             // CMP
			const u8 rdst = (op & 7) | ((op >> 4) & 8);
			return rdst == 15;                                    // ADD/MOV pc: refused
		}
		switch ((op >> 6) & 0xF) {                                // F4 ALU
		case 5: case 6:          rd = C; wr = ALL; return false;  // ADC / SBC
		case 9: case 10: case 11:        wr = ALL; return false;  // NEG / CMP / CMN
		default:                         wr = NZ;  return false;  // logic, MUL, shifts by reg
		}
	case 9:          return false;                                // LDR Rd,[pc,#]
	case 10: case 11:                                             // F7/F8 reg offset
		return (op & 0x0200) ? ((op & 0x0C00) == 0)               //   STRH
		                     : ((op & 0x0800) == 0);              //   STR / STRB
	case 12: case 13: case 14: case 15:                           // F9 word/byte imm
	case 16: case 17: case 18: case 19:                           // F10 half, F11 sp
		return (op & 0x0800) == 0;                                //   stores
	case 20: case 21: return false;                               // ADD Rd, pc/sp
	case 22: case 23:
		if ((op & 0xFF00) == 0xB000) return false;                // ADD sp, #imm
		if ((op & 0xFF00) == 0xBC00 && (op & 0xFF)) return false; // POP {rlist} (no pc)
		return true;                                              // PUSH, POP pc, BKPT, ...
	case 24: case 25: return !(op & 0x0800) || !(op & 0xFF);      // STMIA / empty LDMIA
	default:          return true;                                // Bcc, SWI, B, BL/BLX
	}
}

// ARM counterpart of jitThumbFlagClass(). Transparent: data-processing ops
// the emitter compiles in place (no pc operand or destination -- those are
// refused or exit) and plain LDR/LDRB (no pc destination, the literal form
// only as the emitter accepts it). A predicated instruction reads every flag
// and definitely writes none. S-form logical ops write C only when the
// shifter produces a carry-out for sure (rotated immediate, non-zero
// immediate shift, RRX); a register shift may leave C, so it is no write.
static bool jitArmFlagClass(u32 op, bool /*v5*/, u8& rd, u8& wr)
{
	const u8 NZ = (1u << JITF_N) | (1u << JITF_Z), C = 1u << JITF_C, ALL = 0xF;
	rd = wr = 0;
	const u8 cond = op >> 28;
	if (cond == 0xF) return true;
	const bool al = (cond == 0xE);
	if (!al) rd = ALL;

	if ((op & 0x0C000000u) == 0) {                                // data processing
		const bool immForm = (op >> 25) & 1;
		if (!immForm && ((op >> 4) & 1) && ((op >> 7) & 1)) return true;   // mul / misc space
		const u8 aluOp = (op >> 21) & 0xF;
		const bool S = (op >> 20) & 1;
		const bool testOnly = (aluOp >= 8 && aluOp <= 11);
		if (testOnly && !S) return true;                          // MRS / MSR
		if (((op >> 12) & 0xF) == 15) return true;                // pc destination
		const bool ignoresRn = (aluOp == 13 || aluOp == 15);
		const u8 rn = (op >> 16) & 0xF, rm = op & 0xF, rs = (op >> 8) & 0xF;
		const bool regShift = !immForm && ((op >> 4) & 1);
		if (!ignoresRn && rn == 15) return true;                  // pc operand (ADR folds, but keep it simple)
		if (!immForm && (rm == 15 || (regShift && rs == 15))) return true;
		if (aluOp == 5 || aluOp == 6 || aluOp == 7) rd |= C;      // ADC / SBC / RSC
		const bool rrx = !immForm && !regShift && ((op >> 5) & 3) == 3 && ((op >> 7) & 0x1F) == 0;
		if (rrx) rd |= C;
		if (S && al) {
			const bool isLogical = (aluOp <= 1) || (aluOp == 8) || (aluOp == 9) || (aluOp >= 12);
			if (!isLogical) wr = ALL;
			else {
				bool cOut;
				if (immForm)       cOut = ((op >> 8) & 0xF) != 0;
				else if (regShift) cOut = false;
				else               cOut = rrx || ((op >> 7) & 0x1F) != 0 || ((op >> 5) & 3) != 0;
				wr = NZ | (cOut ? C : 0);
			}
		}
		return false;
	}
	if ((op & 0x0C000000u) == 0x04000000u) {                      // LDR / STR
		if (!((op >> 20) & 1)) return true;                       // store (SMC guard can bail)
		if (((op >> 12) & 0xF) == 15) return true;                // LDR pc
		const bool I = (op >> 25) & 1, P = (op >> 24) & 1, W = (op >> 21) & 1;
		if (!P && W) return true;                                 // LDRT
		if (I && ((op & 0x10) || (op & 0xF) == 15)) return true;
		if (((op >> 16) & 0xF) == 15 && (I || W || !P || !al)) return true;
		return false;
	}
	return true;
}

// dead[i] = flags dead after the i-th instruction from startPC (JITF masks);
// liveIn[i] (optional) = flags live on entry to it. Instructions at or past
// `limit` (the block's cut, jitCutLimit()) are outside the block: everything
// is live there.
static void jitFlagLiveness(const JitCpuProfile& cpu, u32 startPC, bool thumb, u32 limit, u8* dead, u8* liveIn = nullptr)
{
	u8 rd[JIT_TRACE_MAX_INSTRUCTIONS], wr[JIT_TRACE_MAX_INSTRUCTIONS];
	bool bar[JIT_TRACE_MAX_INSTRUCTIONS];
	const bool v5 = cpu.isaLevel >= 5;
	for (u32 i = 0; i < JIT_TRACE_MAX_INSTRUCTIONS; i++) {
		if (i >= limit) { bar[i] = true; continue; }
		bar[i] = thumb ? jitThumbFlagClass((u16)cpu.fetch16(startPC + 2 * i), rd[i], wr[i])
		               : jitArmFlagClass(cpu.fetch32(startPC + 4 * i), v5, rd[i], wr[i]);
	}
	u8 live = 0xF;                                   // everything live past the window
	for (int i = JIT_TRACE_MAX_INSTRUCTIONS - 1; i >= 0; i--) {
		if (bar[i]) { dead[i] = 0; live = 0xF; if (liveIn) liveIn[i] = live; continue; }
		dead[i] = (u8)(0xF & ~live);
		live = (u8)((live & ~wr[i]) | rd[i]);
		if (liveIn) liveIn[i] = live;
	}
}

// =========================================================================
// THUMB spin-loop detection
// =========================================================================
// A poll loop such as the DMA-busy wait `LDR r3,[r2] / CMP r3,#0 / BLT loop`
// cannot see anything change inside one dispatch: scheduler events (DMA
// progress, IRQs, timers, the ARM7) only run between dispatches, so every
// iteration after the first re-reads the same value and takes the same branch
// until the chain's quota guard trips. Such a loop used to run hundreds of
// times per dispatch, each with a slowRead C call. When the body is provably
// idempotent the taken back-edge instead adds, in one step, the cycles and
// instruction count of all those iterations, so emulated state and timing are
// exactly what running them would have produced.
//
// Idempotent body: 1..4 instructions from {LSL/LSR/ASR imm, ADD/SUB reg/imm3,
// MOV/CMP imm8, the F4 ALU ops except ADC/SBC, one LDR/LDRB/LDRH imm}, then a
// Bcc to the loop head. Every register an instruction reads is either never
// written by the body or already written earlier in the same iteration, and
// the load's base register is never written (so its address is invariant --
// the emitted code still refuses to skip reads of side-effecting ports, see
// jit_thumb.cpp). Returns the loop length including the Bcc, or 0.
static u8 jitThumbSpinLoop(const JitCpuProfile& cpu, u32 startPC, u8& base, s32& imm)
{
	u16 ops[5];
	u32 len = 0;
	for (; len < 5; len++) {
		ops[len] = (u16)cpu.fetch16(startPC + 2 * len);
		if ((ops[len] & 0xF000) == 0xD000) break;
	}
	if (len == 0 || len > 4) return 0;
	const u16 bcc = ops[len];
	const u8 cond = (bcc >> 8) & 0xF;
	if (cond >= 0xE) return 0;
	if (startPC + 2 * len + 4 + ((s32)(s8)(bcc & 0xFF) << 1) != startPC) return 0;

	u16 rd[4], wr[4];
	int loads = 0;
	for (u32 i = 0; i < len; i++) {
		const u16 op = ops[i];
		const u8 r0 = op & 7, r3 = (op >> 3) & 7, r6 = (op >> 6) & 7;
		rd[i] = wr[i] = 0;
		switch (op >> 11) {
		case 0: case 1: case 2: rd[i] = 1 << r3; wr[i] = 1 << r0; break;           // shift imm
		case 3: rd[i] = (1 << r3) | ((op & 0x0400) ? 0 : (1 << r6)); wr[i] = 1 << r0; break;
		case 4: wr[i] = 1 << ((op >> 8) & 7); break;                                  // MOV imm8
		case 5: rd[i] = 1 << ((op >> 8) & 7); break;                                  // CMP imm8
		case 8: {
			if (op & 0x0400) return 0;                                                // hi-reg / BX
			const u8 alu = (op >> 6) & 0xF;
			if (alu == 5 || alu == 6) return 0;                                       // ADC / SBC read C
			rd[i] = (1 << r3) | ((alu == 9 || alu == 15) ? 0 : (1 << r0));
			if (alu != 8 && alu != 10 && alu != 11) wr[i] = 1 << r0;                  // not TST/CMP/CMN
			break;
		}
		case 13: case 15: case 17:                                                    // LDR/LDRB/LDRH imm
			if (loads++) return 0;
			rd[i] = 1 << r3; wr[i] = 1 << r0;
			base = r3;
			imm  = (s32)(((op >> 6) & 0x1F) << ((op >> 11) == 13 ? 2 : (op >> 11) == 17 ? 1 : 0));
			break;
		default: return 0;
		}
	}
	if (loads != 1) return 0;
	u16 all = 0, sofar = 0;
	for (u32 i = 0; i < len; i++) all |= wr[i];
	if (all & (1 << base)) return 0;
	for (u32 i = 0; i < len; i++) {
		if (rd[i] & all & ~sofar) return 0;
		sofar |= wr[i];
	}
	return (u8)(len + 1);
}

// ARM counterpart of jitThumbSpinLoop(): 1..4 unconditional instructions
// from {data-processing without pc operands (not ADC/SBC/RSC, RRX, MRS/MSR,
// or an S-form logical op with a register shift, whose C may be left over from
// the previous iteration), one LDR/LDRB/LDRH/LDRSB/LDRSH with an immediate
// offset and no writeback}, then B<cond> (not BL, not AL) to the loop head.
// Same idempotence rule as the THUMB version. Returns the loop length
// including the branch, or 0.
static u8 jitArmSpinLoop(const JitCpuProfile& cpu, u32 startPC, u8& base, s32& off)
{
	u32 ops[5];
	u32 len = 0;
	for (; len < 5; len++) {
		ops[len] = cpu.fetch32(startPC + 4 * len);
		if ((ops[len] & 0x0E000000u) == 0x0A000000u) break;
	}
	if (len == 0 || len > 4) return 0;
	const u32 br = ops[len];
	if ((br >> 28) >= 0xE || ((br >> 24) & 1)) return 0;             // AL/NV, or BL
	const s32 disp = ((s32)(br << 8)) >> 6;
	if (startPC + 4 * len + 8 + (u32)disp != startPC) return 0;

	u16 rd[4], wr[4];
	int loads = 0;
	for (u32 i = 0; i < len; i++) {
		const u32 op = ops[i];
		if ((op >> 28) != 0xE) return 0;
		const u8 rn = (op >> 16) & 0xF, rdst = (op >> 12) & 0xF, rm = op & 0xF, rs = (op >> 8) & 0xF;
		rd[i] = wr[i] = 0;
		if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60u)) {  // LDRH / LDRSB / LDRSH
			if (!((op >> 20) & 1) || !((op >> 24) & 1) || ((op >> 21) & 1) || !((op >> 22) & 1)) return 0;
			if (rn == 15 || rdst == 15 || loads++) return 0;
			const s32 k = (s32)(((op >> 4) & 0xF0) | (op & 0xF));
			off = ((op >> 23) & 1) ? k : -k;
			base = rn; rd[i] = 1 << rn; wr[i] = 1 << rdst;
			continue;
		}
		if ((op & 0x0C000000u) == 0x04000000u) {                    // LDR / LDRB
			if (!((op >> 20) & 1) || ((op >> 25) & 1) || !((op >> 24) & 1) || ((op >> 21) & 1)) return 0;
			if (rn == 15 || rdst == 15 || loads++) return 0;
			const s32 k = (s32)(op & 0xFFF);
			off = ((op >> 23) & 1) ? k : -k;
			base = rn; rd[i] = 1 << rn; wr[i] = 1 << rdst;
			continue;
		}
		if ((op & 0x0C000000u) != 0) return 0;                     // not data-processing
		const bool immForm = (op >> 25) & 1;
		if (!immForm && ((op >> 4) & 1) && ((op >> 7) & 1)) return 0;
		const u8 aluOp = (op >> 21) & 0xF;
		const bool S = (op >> 20) & 1;
		const bool testOnly = (aluOp >= 8 && aluOp <= 11);
		const bool ignoresRn = (aluOp == 13 || aluOp == 15);
		const bool regShift = !immForm && ((op >> 4) & 1);
		const bool isLogical = (aluOp <= 1) || (aluOp == 8) || (aluOp == 9) || (aluOp >= 12);
		if (testOnly && !S) return 0;
		if (aluOp == 5 || aluOp == 6 || aluOp == 7) return 0;
		if (!immForm && !regShift && ((op >> 5) & 3) == 3 && ((op >> 7) & 0x1F) == 0) return 0;   // RRX
		if (S && isLogical && regShift) return 0;
		if (rdst == 15 || (!ignoresRn && rn == 15)) return 0;
		if (!immForm && (rm == 15 || (regShift && rs == 15))) return 0;
		if (!ignoresRn) rd[i] |= 1 << rn;
		if (!immForm) rd[i] |= 1 << rm;
		if (regShift) rd[i] |= 1 << rs;
		if (!testOnly) wr[i] = 1 << rdst;
	}
	if (loads != 1) return 0;
	u16 all = 0, sofar = 0;
	for (u32 i = 0; i < len; i++) all |= wr[i];
	if (all & (1 << base)) return 0;
	for (u32 i = 0; i < len; i++) {
		if (rd[i] & all & ~sofar) return 0;
		sofar |= wr[i];
	}
	if (off < -32768 || off > 32767) return 0;
	return (u8)(len + 1);
}

// Spin-loop fast-forward (jit*SpinLoop() above). Emitted on the taken path of
// the loop's closing branch, after its cycles and instruction count: r3 then
// holds this iteration's cycles, each further iteration adds c = the loop's
// own cost and one would run while r3 < JIT_YIELD_NUMBER, so add
// n = ceil((Y - r3) / c) iterations' worth of cycles and instructions. The
// self-chained entry guard then yields exactly as the loop would have. Skipped
// (the loop runs normally) when r3 is already at the quota or the load hits a
// port whose read has side effects (0x041xxxxx IPC FIFO / card data) or
// anything at/above 0x08000000 (slot 2).
void JitTraceCtx::emitSpinSkip(u32 targetPC)
{
	if (!spinLoopLen || instrCount + 1 != spinLoopLen || targetPC != startPC) return;
	JIT_STAT_SCOPE(*this, JCS_SPIN, false);
	u32*& p = emitPtr;
	const u32 c = cyclesAccum + 3;                                    // + the taken branch
	*p++ = PPC_ADDI(PPC_R12, hostRegFor(spinBase), spinOff);
	*p++ = PPC_SRWI(PPC_R12, PPC_R12, 20);
	*p++ = PPC_CMPLI(0, PPC_R12, 0x041);
	u32* skipA = p++;                                                 // BEQ: side-effect port
	*p++ = PPC_CMPLI(0, PPC_R12, 0x07F);
	u32* skipB = p++;                                                 // BGT: slot 2 and up
	*p++ = PPC_CMPWI(0, PPC_R3, JIT_YIELD_NUMBER);
	u32* skipC = p++;                                                 // BGE: already at quota
	*p++ = PPC_LI(PPC_R12, JIT_YIELD_NUMBER - 1);
	*p++ = PPC_SUBF(PPC_R12, PPC_R3, PPC_R12);                       // Y - 1 - r3 (>= 0)
	*p++ = PPC_LI(PPC_R11, (s32)c);
	*p++ = PPC_DIVWU(PPC_R12, PPC_R12, PPC_R11);                     // n - 1
	*p++ = PPC_ADDI(PPC_R12, PPC_R12, 1);                            // n
	*p++ = PPC_MULLW(PPC_R11, PPC_R12, PPC_R11);
	*p++ = PPC_ADD(PPC_R3, PPC_R3, PPC_R11);                         // r3 += n * c
	*p++ = PPC_MULLI(PPC_R11, PPC_R12, (s32)spinLoopLen);
	*p++ = PPC_ADD(PPC_R31, PPC_R31, PPC_R11);                       // icount += n * len
	*skipA = PPC_BEQ((u32)((p - skipA) * 4));
	*skipB = PPC_BGT((u32)((p - skipB) * 4));
	*skipC = PPC_BGE((u32)((p - skipC) * 4));
}

// =========================================================================
// Guest-side block cut (see JIT_CUT_BUDGET_ARM in jit_trace.h)
// =========================================================================
// Weight of one guest instruction: the PPC words the pre-thunk emitters
// (HEAD 439c262) produced for it, modelled per instruction class from a
// -DJIT_CUT_PROBE capture of every block the gate scenarios compile (SM64DS
// boot + gameplay, the three wrestlers). Only memory-heavy blocks ever come
// near the budget (32 instructions at <= 80 words each never can), so the
// memory classes are modelled closely -- block transfers per register --
// and everything else coarsely. Depends on the opcode and the core only;
// what the emitters actually produce no longer matters.
static u32 jitCutWeightArm(u32 op, bool arm9)
{
	const u32 cond = op >> 28;
	const u32 pred = (cond != 0xE && cond != 0xF) ? 2 : 0;
	if ((op & 0x0E000000u) == 0x0A000000u) return 6;                         // B / BL
	if ((op & 0x0E000000u) == 0x08000000u) {                                  // LDM / STM
		const u32 n  = (u32)__builtin_popcount(op & 0xFFFF);
		const u32 wb = (op >> 21) & 1 ? 3 : 0;
		if ((op >> 20) & 1)
			return (arm9 ? 46 : 24) + 11 * n + wb + ((op >> 15) & 1 ? (arm9 ? 19 : 8) : 0) + pred;
		if (arm9) return (n == 1 ? 75 : 117 + 11 * n) + wb + pred;
		return 24 + 9 * n + wb + pred;
	}
	if ((op & 0x0C000000u) == 0x04000000u) {                                  // LDR / STR
		const bool L = (op >> 20) & 1, B = (op >> 22) & 1;
		const u32 rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
		if (rn == 15) return L ? (pred ? 31 : arm9 ? 3 : 14) : 33;
		if (L && rd == 15) return arm9 ? 75 : 30;
		if (L) return (B ? (arm9 ? 39 : 28) : (arm9 ? 54 : 37)) + pred;
		return (arm9 ? 65 : 35) + pred;
	}
	if ((op & 0x0E000090u) == 0x00000090u && (op & 0x60))                    // LDRH / STRH / LDRS*
		return ((op >> 20) & 1 ? (arm9 ? 39 : 29) : (arm9 ? 65 : 35)) + pred;
	return 2 + pred;
}

static u32 jitCutWeightThumb(u16 op, bool arm9)
{
	if ((op & 0xF800) == 0x4800) return arm9 ? 39 : 29;                     // LDR Rd,[PC,#]
	const u32 fmt = op & 0xF000;
	if (fmt >= 0x5000 && fmt <= 0x9000) {                                     // F7..F11 load / store
		const bool L    = (op & 0x0800) != 0;
		const bool word = fmt == 0x5000 ? ((op & 0x0E00) == 0x0000 || (op & 0x0E00) == 0x0800)
		                                : (fmt == 0x6000 || fmt == 0x9000);
		if (!L) return arm9 ? 64 : 35;
		return word ? (arm9 ? 53 : 36) : (arm9 ? 38 : 28);
	}
	if ((op & 0xF600) == 0xB400) {                                            // PUSH / POP
		const bool R = (op >> 8) & 1;
		const u32 n = (u32)__builtin_popcount(op & 0xFF) + (R ? 1 : 0);
		if (op & 0x0800) return arm9 ? (R ? 32 + 13 * n : 31 + 11 * n) : (R ? 25 + 12 * n : 21 + 11 * n);
		return arm9 ? (n == 1 ? 79 : 119 + 11 * n) : 26 + 9 * n;
	}
	if (fmt == 0xC000) {                                                      // LDMIA / STMIA
		const u32 n = (u32)__builtin_popcount(op & 0xFF);
		if (op & 0x0800) return (arm9 ? 31 : 26) + 11 * n;
		return arm9 ? (n == 1 ? 77 : 119 + 11 * n) : 26 + 9 * n;
	}
	return 3;
}

// Number of guest instructions the block starting at startPC may hold: the
// first index at which 2 (the entry guard) plus the weights of the
// instructions before it exceeds the budget, else JIT_TRACE_MAX_INSTRUCTIONS.
// Terminators and the instruction cap still end a block earlier.
static u32 jitCutLimit(const JitCpuProfile& cpu, u32 startPC, bool thumb)
{
	const bool arm9 = cpu.arm9DtcmBase != 0;
	const u32 budget = thumb ? JIT_CUT_BUDGET_THUMB : JIT_CUT_BUDGET_ARM;
	u32 w = 2;
	for (u32 i = 0; i < JIT_TRACE_MAX_INSTRUCTIONS; i++) {
		if (w > budget) return i;
		w += thumb ? jitCutWeightThumb((u16)cpu.fetch16(startPC + 2 * i), arm9)
		           : jitCutWeightArm(cpu.fetch32(startPC + 4 * i), arm9);
	}
	return JIT_TRACE_MAX_INSTRUCTIONS;
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

#if JIT_SPIN_SKIP
	ctx.spinLoopLen = thumb ? jitThumbSpinLoop(cpu, startPC, ctx.spinBase, ctx.spinOff)
	                        : jitArmSpinLoop(cpu, startPC, ctx.spinBase, ctx.spinOff);
#endif
	// Guest-side length cut, fixed before anything is emitted.
	const u32 cutLimit = jitCutLimit(cpu, startPC, thumb);
#if JIT_FLAG_ELIM
	u8 flagDeadAt[JIT_TRACE_MAX_INSTRUCTIONS], flagLiveIn[JIT_TRACE_MAX_INSTRUCTIONS];
	jitFlagLiveness(cpu, startPC, thumb, cutLimit, flagDeadAt, flagLiveIn);
#endif

	while (!ctx.endBlock && ctx.instrCount < cutLimit) {
		ctx.memPredict = JIT_MEMP_DTCM;
#if JIT_FLAG_ELIM
		// Elision assumes the block runs on to the overwriting instruction; the
		// liveness window above ends at the cut, so it does.
		ctx.deadFlags = 0;
		{
			const u32 idx = (ctx.currentPC - startPC) >> (thumb ? 1 : 2);
			if (idx < JIT_TRACE_MAX_INSTRUCTIONS) ctx.deadFlags = flagDeadAt[idx];
			// CMP+Bcc fusion (THUMB): this is CMP #imm8 / CMP lo,lo, the next
			// instruction a Bcc on a relational condition (not MI/PL/VS/VC,
			// SWI, AL), and no flag is live after that Bcc's fall-through.
			ctx.fuseCmp = false;
			if (thumb && idx + 2 < JIT_TRACE_MAX_INSTRUCTIONS && flagLiveIn[idx + 2] == 0) {
				const u16 op0 = (u16)cpu.fetch16(ctx.currentPC), op1 = (u16)cpu.fetch16(ctx.currentPC + 2);
				const u8 bc = (op1 >> 8) & 0xF;
				ctx.fuseCmp = ((op0 & 0xF800) == 0x2800 || (op0 & 0xFFC0) == 0x4280) &&
				              (op1 & 0xF000) == 0xD000 && bc <= 0xD && (bc < 4 || bc > 7);
			}
		}
#endif

		// Snapshot for the interpreter fallback below: a refusal may come after
		// the emitter already wrote part of a sequence (or registered a deferred
		// bailout pointing into it), so a refused instruction is rewound to
		// exactly here before the fallback call replaces it.
		u32* const emitMark = ctx.arenaAllocated ? ctx.emitPtr : nullptr;
		const u32  bailMark = ctx.bailoutCount;
		const u32  coldMark = ctx.coldCount;
#ifdef JIT_CODE_STATS
		u32 statMark[JCS_N]; memcpy(statMark, ctx.statWords, sizeof statMark);
		const u32 statColdMark = ctx.statCold;
		u32 statColdCatMark[JCS_N]; memcpy(statColdCatMark, ctx.statColdCat, sizeof statColdCatMark);
#endif

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
#ifdef JIT_CODE_STATS
			{   // the rewound words are gone; keep the entry counter ensureArena() may just have charged
				const u32 ctr = ctx.statWords[JCS_STATCTR];
				memcpy(ctx.statWords, statMark, sizeof statMark);
				ctx.statWords[JCS_STATCTR] = ctr;
				ctx.statCold = statColdMark;
				memcpy(ctx.statColdCat, statColdCatMark, sizeof statColdCatMark);
			}
#endif
			ctx.bailoutCount = bailMark;
			ctx.coldCount = coldMark;
			ctx.endBlock = false;
			ctx.deadFlags = 0;
			ctx.emitInterpFallback(opcode);
			ctx.instrCount++;
			ctx.currentPC += thumb ? 2 : 4;
		}
#endif
	}

#ifdef JIT_CODE_STATS
	if (!ctx.endBlock && ctx.instrCount >= cutLimit && cutLimit < JIT_TRACE_MAX_INSTRUCTIONS)
		jitCodeStatsBudgetEnd(cache);
#endif

#if JIT_FLAG_ELIM
	// A fused CMP whose Bcc never got emitted (cannot happen by construction:
	// the Bcc always follows and is never refused, and fusion needs the Bcc's
	// successor inside the cut) would leave its flags unwritten -- materialise them.
	if (ctx.fusedCmpValid) {
		ctx.fusedCmpValid = false;
		ctx.deadFlags = 0;
		jitThumbEmitCmpFlags(ctx, ctx.fusedCmpOp);
	}
#endif

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
		// or every marker leaks JIT_BLOCK_RESERVE_WORDS of arena until the next flush.
		if (ctx.arenaAllocated)
			cache.rewindJITMemory((JIT_BLOCK_RESERVE_WORDS * sizeof(u32) + 31) & ~31u);
#ifdef JIT_CODE_STATS
		if (ctx.statCounter) jitCodeStatsDropRecord(cache);
#endif
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
		JIT_STAT_SCOPE(ctx, JCS_EXIT, false);
		ctx.emitAddCycles(ctx.cyclesAccum);
		ctx.emitResultMetadata(ctx.instrCount, 0);

		ctx.emitChainTail(ctx.currentPC);
	}

	// ---- quota-shield yield stub: resume PC, then the shared yield tail ----
	// (bailedOut = 1 and the exit live there; see JITCache::yieldTailAddress.)
	u32* yieldTarget = ctx.emitPtr;
	{
	JIT_STAT_SCOPE(ctx, JCS_YIELD, true);
	*ctx.emitPtr++ = PPC_LIS(PPC_R4, startPC >> 16);
	*ctx.emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, startPC & 0xFFFF);
	s32 yieldOff = (s32)((u8*)cache.yieldTailAddress - (u8*)ctx.emitPtr);
	*ctx.emitPtr++ = PPC_B(yieldOff);
	}
	*ctx.quotaGuard = PPC_BGE((u32)((yieldTarget - ctx.quotaGuard) * 4));

	// ---- cold stubs: memory accesses off their predicted region, SMC bails ----
	ctx.emitColdStubs();
	if (ctx.coldOverflow) {
		// Cannot happen for a block within the cut budget (<= 3 stubs per
		// instruction, conditional branches well inside +-32 KB); if it ever
		// did, the interpreter runs this PC instead of broken code.
		cache.rewindJITMemory((JIT_BLOCK_RESERVE_WORDS * sizeof(u32) + 31) & ~31u);
#ifdef JIT_CODE_STATS
		if (ctx.statCounter) jitCodeStatsDropRecord(cache);
#endif
		return cache.registerBlock(startPC, 1, nullptr, thumb);
	}

	// ---- finalize: rewind unused reservation, sync caches, register ----
	u32 emittedWords = (u32)(ctx.emitPtr - ctx.blockStart);
	u32 actualBytes  = emittedWords * sizeof(u32);
	u32 committed    = (actualBytes + 31) & ~31u;
	s32 diff         = (s32)(JIT_BLOCK_RESERVE_WORDS * sizeof(u32) - committed);
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
#ifdef JIT_CODE_STATS
	if (ctx.statCounter) jitCodeStatsCommit(ctx, emittedWords, committed);
#endif
	DCStoreRange(ctx.blockStart, actualBytes);
	ICInvalidateRange(ctx.blockStart, actualBytes);

#ifdef JIT_HEAP_WATCH
	// §16: check the buffer canaries on every compile, not just the 1K-dispatch
	// poll -- a corrupting block is most likely to trip one right after it emits.
	jitCheckCanaries();
#endif

	return cache.registerBlock(startPC, ctx.instrCount, (JITBlockFunc)ctx.blockStart, thumb);
}

// =========================================================================
// -DJIT_CODE_STATS: generated-code footprint report (see jit_trace.h)
// =========================================================================
#ifdef JIT_CODE_STATS
namespace {
// One record per compiled block, in compile order since the core's last
// cache flush (a flush discards all code, so it resets the list). The block's
// entry counter is `execs`; the frame scan diffs it against `seen`.
struct JitStatRec {
	u32 execs;          // incremented by the block's own entry code
	u32 seen;           // execs at the previous frame scan
	u32 insns;          // guest instructions compiled
	u16 words;          // emitted words, entry counter excluded
	u16 cold;           // of which in cold scopes
	u16 cat[JCS_N];     // words per category
	u16 hotCat[JCS_N];  // of which outside cold scopes
	u8  inWindow;       // executed in the current report window
	u8  pad[3];
};
enum { JCS_REC_ARM9 = 49152, JCS_REC_ARM7 = 16384, JCS_WINDOW = 60 };
struct JitStatCore {
	JitStatRec* rec; u32 cap, n;
	// cumulative over the run (all compiles, flushed or not)
	u64 blocks, insns, words, pad, cold, cat[JCS_N], flushes, budgetEnds;
	// current window
	u64 fBlocks, fWords, fCold, fEntries, fInsnsRun, frames;
	u64 fHotCat[JCS_N];   // hot words per category of the blocks executed in a frame (summed over frames)
	u64 fExecCat[JCS_N];  // hot words per category x entries (upper bound on fetched words)
	u64 wBlocks, wWords, wLines, wCold, wInsns, wCat[JCS_N];
};
JitStatCore s_jcs[2];
int jcsCore(const JITCache& c) { return &c == &jitCacheArm9 ? JIT_ARM9 : JIT_ARM7; }
}

static u32* jitCodeStatsNewRecord(const JITCache& cache)
{
	JitStatCore& S = s_jcs[jcsCore(cache)];
	if (!S.rec) {
		S.cap = jcsCore(cache) == JIT_ARM9 ? JCS_REC_ARM9 : JCS_REC_ARM7;
		S.rec = (JitStatRec*)calloc(S.cap, sizeof(JitStatRec));
		if (!S.rec) { S.cap = 0; return nullptr; }
	}
	if (S.n >= S.cap) return nullptr;
	JitStatRec& r = S.rec[S.n++];
	memset(&r, 0, sizeof r);
	return &r.execs;
}

static void jitCodeStatsDropRecord(const JITCache& cache)
{
	JitStatCore& S = s_jcs[jcsCore(cache)];
	if (S.n) S.n--;
}

static void jitCodeStatsCommit(const JitTraceCtx& ctx, u32 emittedWords, u32 committedBytes)
{
	JitStatCore& S = s_jcs[jcsCore(ctx.cache)];
	JitStatRec& r = *(JitStatRec*)ctx.statCounter;
	u32 cat[JCS_N]; memcpy(cat, ctx.statWords, sizeof cat);
	cat[JCS_ENTRY] += 2;                                       // ensureArena()'s quota guard
	const u32 words = emittedWords - cat[JCS_STATCTR];
	u32 named = 0;
	for (int i = 0; i < JCS_N; i++) if (i != JCS_OTHER && i != JCS_PAD && i != JCS_STATCTR) named += cat[i];
	cat[JCS_OTHER] = words - named;
	cat[JCS_PAD]   = committedBytes / 4 - emittedWords;
	r.insns = ctx.instrCount; r.words = (u16)words; r.cold = (u16)ctx.statCold;
	for (int i = 0; i < JCS_N; i++) r.cat[i] = (u16)cat[i];
	for (int i = 0; i < JCS_N; i++) r.hotCat[i] = (u16)(cat[i] - ctx.statColdCat[i]);
	r.hotCat[JCS_PAD] = 0;
	S.blocks++; S.insns += ctx.instrCount; S.words += words; S.pad += cat[JCS_PAD]; S.cold += ctx.statCold;
	for (int i = 0; i < JCS_N; i++) S.cat[i] += cat[i];
}

static void jitCodeStatsBudgetEnd(const JITCache& cache)
{
	s_jcs[jcsCore(cache)].budgetEnds++;      // block cut by jitCutLimit(), not the insn cap / a terminator
}

void jitCodeStatsOnFlush(const JITCache* cache)
{
	if (cache != &jitCacheArm9 && cache != &jitCacheArm7) return;
	JitStatCore& S = s_jcs[jcsCore(*cache)];
	S.flushes++;
	S.n = 0;
}

static const char* const k_jcsName[JCS_N] = {
	"entry", "memguard", "memhit", "memslow", "smcguard", "smcbail", "flags", "pred",
	"exit", "yield", "fallback", "spin", "other", "pad", "statctr" };

void jitCodeStatsTick(u32 frame)
{
	for (int c = 0; c < 2; c++) {
		JitStatCore& S = s_jcs[c];
		S.frames++;
		for (u32 i = 0; i < S.n; i++) {
			JitStatRec& r = S.rec[i];
			const u32 d = r.execs - r.seen;
			if (!d) continue;
			r.seen = r.execs;
			S.fBlocks++; S.fWords += r.words; S.fCold += r.cold; S.fEntries += d; S.fInsnsRun += (u64)d * r.insns;
			for (int k = 0; k < JCS_N; k++) { S.fHotCat[k] += r.hotCat[k]; S.fExecCat[k] += (u64)d * r.hotCat[k]; }
			if (!r.inWindow) {
				r.inWindow = 1;
				S.wBlocks++; S.wWords += r.words; S.wLines += (r.words * 4 + 31) / 32; S.wCold += r.cold; S.wInsns += r.insns;
				for (int k = 0; k < JCS_N; k++) S.wCat[k] += r.cat[k];
			}
		}
	}
	if (frame % JCS_WINDOW) return;
	static bool s_init = false;
	FILE* f = fopen("sd:/jitstats.log", s_init ? "a" : "w");
	if (f && !s_init) {
		s_init = true;
		fprintf(f, "# jitstats: arena9=%p (%u KiB) arena7=%p (%u KiB) table9=%p smcflags9=%p dtcmregion=%08x"
		           " sharedstubs9=%u sharedstubs7=%u (bytes: SMC tail + memory thunks) main=%08x dtcm=%08x itcm=%08x"
		           " pagedesc7=%08x\n",
		        (void*)s_arena[JIT_ARM9], (unsigned)(JIT_ARENA_SIZE_ARM9 >> 10),
		        (void*)s_arena[JIT_ARM7], (unsigned)(JIT_ARENA_SIZE >> 10), (void*)s_blockTable[JIT_ARM9],
		        (void*)s_smcPageFlags[JIT_ARM9],
		        jitProfile[JIT_ARM9] ? (unsigned)*(const volatile u32*)(uintptr_t)jitProfile[JIT_ARM9]->arm9DtcmRegionPtr : 0u,
		        (unsigned)jitCacheArm9.thunkWords * 4, (unsigned)jitCacheArm7.thunkWords * 4,
		        jitProfile[JIT_ARM9] ? (unsigned)jitProfile[JIT_ARM9]->mainMemBase : 0u,
		        jitProfile[JIT_ARM9] ? (unsigned)jitProfile[JIT_ARM9]->arm9DtcmBase : 0u,
		        jitProfile[JIT_ARM9] ? (unsigned)jitProfile[JIT_ARM9]->arm9ItcmBase : 0u,
		        jitProfile[JIT_ARM7] ? (unsigned)jitProfile[JIT_ARM7]->pageDescBase : 0u);
	}
	for (int c = 0; c < 2; c++) {
		JitStatCore& S = s_jcs[c];
		const u64 fr = S.frames ? S.frames : 1;
		if (f) {
			fprintf(f, "frame=%u core=%d cum: blocks=%llu insns=%llu bytes=%llu pad=%llu cold=%llu flushes=%llu budgetends=%llu live=%u |",
			        frame, c ? 7 : 9, (unsigned long long)S.blocks, (unsigned long long)S.insns,
			        (unsigned long long)S.words * 4, (unsigned long long)S.pad * 4, (unsigned long long)S.cold * 4,
			        (unsigned long long)S.flushes, (unsigned long long)S.budgetEnds, (unsigned)S.n);
			for (int k = 0; k < JCS_N; k++) if (k != JCS_STATCTR) fprintf(f, " %s=%llu", k_jcsName[k], (unsigned long long)S.cat[k] * 4);
			fprintf(f, "\nframe=%u core=%d perframe: blocks=%llu bytes=%llu hotbytes=%llu entries=%llu insnsrun=%llu"
			           " | window%u: blocks=%llu insns=%llu bytes=%llu lines32=%llu hotbytes=%llu |",
			        frame, c ? 7 : 9, (unsigned long long)(S.fBlocks / fr), (unsigned long long)(S.fWords * 4 / fr),
			        (unsigned long long)((S.fWords - S.fCold) * 4 / fr), (unsigned long long)(S.fEntries / fr),
			        (unsigned long long)(S.fInsnsRun / fr), (unsigned)JCS_WINDOW,
			        (unsigned long long)S.wBlocks, (unsigned long long)S.wInsns, (unsigned long long)S.wWords * 4,
			        (unsigned long long)S.wLines * 32, (unsigned long long)(S.wWords - S.wCold) * 4);
			for (int k = 0; k < JCS_N; k++) if (k != JCS_STATCTR) fprintf(f, " %s=%llu", k_jcsName[k], (unsigned long long)S.wCat[k] * 4);
			fprintf(f, "\nframe=%u core=%d hotcat: per-frame hot bytes |", frame, c ? 7 : 9);
			for (int k = 0; k < JCS_N; k++) if (k != JCS_STATCTR && k != JCS_PAD) fprintf(f, " %s=%llu", k_jcsName[k], (unsigned long long)(S.fHotCat[k] * 4 / fr));
			fprintf(f, "\nframe=%u core=%d execcat: per-frame hot bytes x entries |", frame, c ? 7 : 9);
			for (int k = 0; k < JCS_N; k++) if (k != JCS_STATCTR && k != JCS_PAD) fprintf(f, " %s=%llu", k_jcsName[k], (unsigned long long)(S.fExecCat[k] * 4 / fr));
			fprintf(f, "\n");
		}
		S.fBlocks = S.fWords = S.fCold = S.fEntries = S.fInsnsRun = S.frames = 0;
		memset(S.fHotCat, 0, sizeof S.fHotCat); memset(S.fExecCat, 0, sizeof S.fExecCat);
		S.wBlocks = S.wWords = S.wLines = S.wCold = S.wInsns = 0;
		memset(S.wCat, 0, sizeof S.wCat);
		for (u32 i = 0; i < S.n; i++) S.rec[i].inWindow = 0;
	}
	if (f) fclose(f);
}
#endif // JIT_CODE_STATS

#endif // DESMUME_JIT
