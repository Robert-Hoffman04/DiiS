/****************************************************************************
 * Visual Boy Advance GX
 *
 * Daryl Borth 2026
 *
 * JITCache.h
 *
 * Declares the JITCache class and its supporting data structures:
 *   - BasicBlock: the 16-byte-aligned {startPC, length, execute, nextSMC}
 *     record stored per hash bucket. `execute == nullptr` with `length == 0`
 *     represents an as-yet-uncompiled slot; `execute == nullptr` with a
 *     nonzero length represents a deliberately cached "don't JIT this"
 *     fallback stub (as opposed to a genuine cache miss); `nextSMC` is an
 *     intrusive linked-list pointer used only by the SMC registry.
 *   - smcPageFlags[] / smcRegistry[]: the global, 1KB-page-granularity
 *     self-modifying-code tracking tables. A set flag means "at least one
 *     compiled block currently has code living on this page"; the registry
 *     holds the actual intrusive per-page block lists that invalidateSMCTarget()
 *     walks on every guest write to EWRAM/IWRAM.
 *   - JITCache: owns the arena pointer/offset, the block hash table, and
 *     the linker stub addresses (linkerStubAddress/linkerReturnAddress)
 *     that JIT-emitted code branches to directly. getBlock() is the hot-path
 *     inline lookup the interpreter's dispatch loop calls on every THUMB
 *     fetch; the heavier registerBlock()/flushCache()/invalidateSMCTarget()
 *     logic lives in JITCache.cpp.
 *
 * JIT_ARENA_SIZE (2 MB ARM7), JIT_ARENA_SIZE_ARM9 (12 MB), HASH_TABLE_SIZE
 * (65536, indexed by the multiplicative jitHashPC()), and SMC_MAP_SIZE are the
 * tuning constants -- see jit/NOTES.md for the measurements behind them.
 ***************************************************************************/

#ifndef JIT_CACHE_H
#define JIT_CACHE_H

#include <stddef.h>
#include <string.h>
#include "../types.h"
#include "jit_ppc_emitter.h"
#include "jit_debug.h"

// DeSmuMEWii port: vendored from VBA-GX (jit/upstream/PROVENANCE.md).
// P1: the hard-coded "code can live in bank 2 / bank 3" SMC test is now driven
// by smcBankMask, set from JitCpuProfile at initialize() time. For the DS ARM7
// the tracked banks happen to be the same nibbles -- 0x02xxxxxx main RAM and
// 0x03xxxxxx WRAM / shared WRAM -- but the ARM9 (and DTCM) will differ.

// Arena sized down from VBA's 8 MB: the Broadway has only 32 KB L1-I and
// Dolphin doesn't model it, so a smaller, denser arena is the safer default
// (see the plan, risk "Broadway I-cache vs arena"). Revisit with hardware.
#ifndef JIT_ARENA_SIZE
#define JIT_ARENA_SIZE					(1024 * 1024 * 2) // 2 MB (ARM7)
#endif
// ARM9 is the hot core. History: 3 MB thrashed (~90 flushes/3000 frames);
// 6 MB still thrashed - a harness capture (SM64DS savestate) showed arenapeak
// pinned at 99% with a full-cache flush every ~130 frames, ~15 over a
// 1400-frame run, driving the JIT-build cost and the p99 frametime tail.
// 12 MB (measured, same SM64DS capture to frame ~1200, vs the 6 MB numbers):
//   p99 frame time   ~41-48 ms -> ~28-34 ms
//   worst frame      ~49 ms    -> ~44 ms
//   ARM9 JIT-build   0.53 ms/f -> 0.38 ms/f   (-28%)
//   flush interval   ~1/130 fr -> ~1/175 fr
// It does NOT fix the thrash: arena peak stays 99% and collision-misses stay
// ~38k (bound by the 64K direct-mapped block table, independent of arena
// size), so SM64DS's ARM9 working set still outgrows 12 MB - this buys the
// latency tail, not saturation headroom. Which pool the arena lands in
// (MEM1 vs MEM2) is tracked in jit/NOTES.md.
#ifndef JIT_ARENA_SIZE_ARM9
#define JIT_ARENA_SIZE_ARM9				(1024 * 1024 * 12) // 12 MB (ARM9 is the hot core)
#endif
#define HASH_TABLE_SIZE					65536
#define SMC_MAP_SIZE                    65536 // 64K pages (1KB page granularity across 64MB)

// Block-table index hash. Was ((pc>>1) ^ (pc>>13)) & (HASH_TABLE_SIZE-1): a
// -DJIT_HASH_HISTO capture (SM64DS, jit/NOTES.md) showed that shift-xor mix
// collapsing the hot ARM9 working set into ~55 buckets - one bucket alone took
// 2700+ evictions in a single run, so the "collision misses" were a hash
// weakness, not a load-factor / table-size problem. Replaced with a
// multiplicative (Fibonacci) hash: 32-bit product with the golden-ratio
// constant, keep the top HASH_TABLE_SIZE-log2 bits (which depend on every
// input bit). The hand-emitted stub in jit_cache.cpp mirrors this exactly
// (lis/ori/mullw + one rlwinm), so getBlock() and JIT-emitted lookups agree.
#define JIT_HASH_GOLDEN					0x9E3779B1u
#define JIT_HASH_BITS					(__builtin_ctz(HASH_TABLE_SIZE))
static inline u32 jitHashPC(u32 pc) {
	return (pc * JIT_HASH_GOLDEN) >> (32 - JIT_HASH_BITS);
}

// -------------------------------------------------------------------------
// ENGINE DEFINITIONS
// -------------------------------------------------------------------------
typedef void (*JITBlockFunc)();

// Force 16-byte alignment to allow fast PowerPC bit-shifting
struct __attribute__((aligned(16))) BasicBlock {
	u32 startPC;
	// bits 0..30: guest instruction count (0 == uncompiled/killed slot, 1 with
	//   execute==nullptr == deliberate "don't JIT" marker). bit 31: set if the
	//   block was compiled in THUMB mode. ARM7/ARM9 share PC-keyed tables and a
	//   guest address can (across an ITCM/overlay reload) be legitimately
	//   executed in either ISA; a mode-mismatched hit must recompile, not run
	//   the wrong decoder's code. Packed here rather than as a new field to keep
	//   the struct at 16 bytes / the table at 1 MiB.
	u32 length;
	JITBlockFunc execute;
    BasicBlock* nextSMC; // Intrusive pointer for SMC bucket chain

	inline u32  insnCount()     const { return length & 0x7FFFFFFFu; }
	inline bool thumbCompiled() const { return (length & 0x80000000u) != 0; }
	inline u32  byteLength()    const { return insnCount() * (thumbCompiled() ? 2u : 4u); }
};

class JITCache {
	private:
		u32* jitArena;
		size_t arenaOffset;
		size_t arenaSize;   // this cache's arena capacity (ARM7 and ARM9 differ)
		BasicBlock* blockTable;
		BasicBlock** smcRegistry;
		bool isInitialized;
		u32 smcBankMask;   // bit b set => guest bank b holds JIT-tracked code

		inline bool smcTrackedBank(u32 bank) const {
			return (smcBankMask >> (bank & 31)) & 1u;
		}

	public:
		JITCache();
		~JITCache();

		u32* linkerStubAddress;
		u32* linkerReturnAddress;
		// A4-P5: guarded dynamic-exit dispatch. BX/LDR-pc/POP{pc}/BLX-style exits
		// have a runtime-computed target (can't self-patch a fixed branch), but
		// each exit site's RESULTING mode is still a compile-time constant (the
		// branch that got here already decided THUMB vs ARM). These two stubs do
		// the same hash-lookup-and-jump as linkerStubAddress but never patch
		// anything, and additionally verify the found block's cached mode bit
		// matches the mode this exit expects before jumping -- a wrong-mode hit
		// (e.g. a hash slot recycled for the other ISA at the same PC) falls
		// through to linkerReturnAddress exactly like any other miss, so this is
		// pure guarded speed-up: worst case is identical to today's unconditional
		// return-to-C, never a wrong-mode jump. See emitDynamicExit().
		u32* linkerStubDynamicThumbAddress;
		u32* linkerStubDynamicArmAddress;
		u8* smcPageFlags;

		void initialize(u32* arenaPtr, size_t arenaBytes, BasicBlock* blockPtr,
		                BasicBlock** smcRegPtr, u8* smcFlagsPtr, u32 trackedBankMask);
		void destroy();

		u32* allocateJITMemory(size_t numBytes);
		void rewindJITMemory(size_t numBytes);
		BasicBlock* registerBlock(u32 pc, u32 length, JITBlockFunc execute, bool thumb = true);
		inline size_t getArenaOffset() const { return arenaOffset; }
		void flushCache();
		void invalidateSMCTarget(u32 targetEA);

#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
		// §3.3 JIT cache-pressure telemetry. One counter set per core; the
		// PROFILER_CACHE_* macros (jit_debug.h) drive these from getBlock() /
		// registerBlock() / flushCache(). installFrame[] shadows blockTable[]
		// with an install sequence number per slot so an eviction can price the
		// displaced block's lifetime (in registrations survived). All of this
		// compiles out of a release / non-HARNESS_PROFILE build -- BasicBlock and
		// the hand-emitted stubs are untouched (counters live on JITCache, whose
		// layout no emitted code depends on).
		struct CacheStats {
			u64 hits;
			u64 coldMisses;        // getBlock miss, slot never populated
			u64 collisionMisses;   // getBlock miss, slot held a different PC
			u64 registrations;     // registerBlock calls
			u64 evictions;         // live executable block displaced by a collision
			u64 evictLifetimeSum;  // sum of install-seq deltas over those evictions
			u64 flushes;
		};
		CacheStats profStats;
		u32* installFrame;         // HASH_TABLE_SIZE entries, or nullptr
#ifdef JIT_HASH_HISTO
		// Step 3 one-off: per-bucket real-eviction tally, to tell a structural
		// hash weakness (a few buckets hogging evictions) from a plain
		// load-factor problem (uniform spread). calloc'd in initialize().
		u16* bucketEvict;          // HASH_TABLE_SIZE entries, or nullptr
#endif
		u32  installSeq;
		u32  arenaPeak;            // §3.3b high-water arenaOffset since last flush
		u32  arenaPeakEver;        // §3.3b high-water across the whole run
		u8   lastHeuristic;        // §3.3 follow-up: edge-trigger the note/warning
		                           //   lines (0=none 1=contention 2=cold 3=arena)

		void profCacheHit();
		void profCacheMiss(u32 slotPC);
		void profCacheEvict(u32 evictedPC, u32 newPC);
		void profCacheFlushStart();
		void profEmitReport(const char* tag);
#endif

		inline BasicBlock* getBlock(u32 pc) {
			if (!isInitialized) return nullptr;
			u32 index = jitHashPC(pc);
			BasicBlock* block = &blockTable[index];
			if (block->startPC == pc) {
				PROFILER_CACHE_HIT();
				return block;
			}
			PROFILER_CACHE_MISS();
			return nullptr;
		}

#ifdef DESMUME_JIT_TRACE_FIRST
		// Raw slot peek for edge telemetry (jit_exec.cpp): unlike getBlock() this
		// returns the slot even when a different PC occupies it, so the caller can
		// tell a hash collision from a never-compiled / SMC-killed slot. Returns a
		// zeroed sentinel before initialize().
		inline const BasicBlock& debugSlot(u32 index) const {
			static const BasicBlock kZero = {};
			if (!blockTable) return kZero;
			return blockTable[index & (HASH_TABLE_SIZE - 1)];
		}
#endif
};

// One cache per emulated core -- ARM7 and ARM9 share guest address ranges
// (main RAM 0x02xxxxxx, BIOS/ITCM 0x00xxxxxx), so blocks cannot share a
// PC-keyed table. Each instance owns its own arena, block hash, SMC registry,
// page-flag map and linker-stub pair.
extern JITCache jitCacheArm7;
extern JITCache jitCacheArm9;

// SMC / coherency fan-out helpers. Shared regions (main RAM, shared WRAM) can
// hold code for either core and can be written by either core or by DMA, so a
// write there must invalidate in both caches. Safe to call before jitInit().
void jitInvalidateSMC(u32 addr);   // -> both caches
void jitFlushAllCaches();          // -> both caches (bulk memory overwrite / reset)

#endif
