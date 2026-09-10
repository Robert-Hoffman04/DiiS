/****************************************************************************
 * Visual Boy Advance GX
 *
 * Daryl Borth 2026
 *
 * JITCache.cpp
 *
 * Implements the JITCache class: the 8MB bump-allocated code arena, the
 * 65536-bucket direct-mapped (no-chaining, colliding-block-evicts) block
 * hash table, and the self-modifying inline-cache "linker stub" that makes
 * block chaining possible.
 *
 *   - allocateJITMemory()/rewindJITMemory(): the raw 32-byte-aligned bump
 *     allocator backing the arena; rewind reclaims a block's unused
 *     worst-case reservation after compilation finishes.
 *   - registerBlock(): installs a freshly compiled (or intentionally
 *     null/"don't JIT this") block into its hash bucket, evicting whatever
 *     was there before and carefully unlinking the evicted block from the
 *     SMC registry first so no dangling pointer is left behind (this was
 *     the root cause of one strand of the original cold-boot bug).
 *   - flushCache(): resets the arena and block table, and re-emits the
 *     shared self-modifying linker stub fresh into the arena's start �
 *     every JIT exit branches into this one stub, which re-derives the
 *     hash bucket for the target PC and either patches the caller's branch
 *     directly to the target block (cache hit) or falls through to
 *     ExecuteJITTrace_Return (miss or fallback stub).
 *   - invalidateSMCTarget(): the self-modifying-code guard. Given a write's
 *     target address, walks the intrusive per-page SMC registry
 *     (smcRegistry[]/smcPageFlags[]) for every 1KB page the write could
 *     have touched, and for any block whose instruction range overlaps the
 *     write, surgically patches that block's first native instruction into
 *     an unconditional branch back to the C++ handler and marks it dead �
 *     ensuring the *next* execution attempt (not the current one, which
 *     may already be mid-flight) safely bails instead of running stale
 *     compiled code. Every write path in the emulator (interpreter, DMA,
 *     BIOS-HLE, and the JIT's own inline SMC guards) must route through
 *     this to stay correct.
 ***************************************************************************/

// DeSmuMEWii port: vendored from VBA-GX (jit/upstream/PROVENANCE.md).
// P0: retargeted includes + build guard. Body unchanged.
#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include <ogc/cache.h>
#if defined(DESMUME_JIT_TRACE_FIRST) || defined(DESMUME_ARM_TIME_SPLIT)
#include <stdio.h>
#endif
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
#include <stdlib.h>   // §3.3: installFrame[] shadow array (calloc/free)
#endif

JITCache jitCacheArm7;
JITCache jitCacheArm9;

void jitInvalidateSMC(u32 addr)
{
	jitCacheArm7.invalidateSMCTarget(addr);
	jitCacheArm9.invalidateSMCTarget(addr);
}

void jitFlushAllCaches()
{
	jitCacheArm7.flushCache();
	jitCacheArm9.flushCache();
}

// P5 diagnostic: how often invalidateSMCTarget() is actually called (every
// hooked write to bank 2/3, from any of the P4 call sites -- ARM7/ARM9/DMA)
// vs. how often it actually kills a still-live registered block. Decoupled
// from jit_exec.cpp's own dispatch-count-gated report because most of these
// calls come from ARM9/interpreter write paths that never touch jitRunArm7()
// at all, so g_jitAttempts can stay near zero while this is churning.
u64 g_jitSmcChecks = 0;
u64 g_jitSmcKills  = 0;

JITCache::JITCache() {
	jitArena = nullptr;
	blockTable = nullptr;
	smcRegistry = nullptr;
	smcPageFlags = nullptr;
	linkerStubAddress = nullptr;
	linkerReturnAddress = nullptr;
	linkerStubDynamicThumbAddress = nullptr;
	linkerStubDynamicArmAddress = nullptr;
	arenaOffset = 0;
	arenaSize = 0;
	isInitialized = false;
	smcBankMask = 0;
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
	installFrame = nullptr;
	installSeq = 0;
	arenaPeak = 0;
	arenaPeakEver = 0;
	lastHeuristic = 0;
	memset(&profStats, 0, sizeof profStats);
#endif
}

JITCache::~JITCache() {
    destroy();
}

void JITCache::initialize(u32* arenaPtr, size_t arenaBytes, BasicBlock* blockPtr,
                          BasicBlock** smcRegPtr, u8* smcFlagsPtr, u32 trackedBankMask) {
	if (isInitialized) return;

	jitArena = arenaPtr;
	arenaSize = arenaBytes;
	blockTable = blockPtr;
	smcRegistry = smcRegPtr;
	smcPageFlags = smcFlagsPtr;
	smcBankMask = trackedBankMask;
	arenaOffset = 0;
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
	if (!installFrame)
		installFrame = (u32*)calloc(HASH_TABLE_SIZE, sizeof(u32));
	installSeq = 0;
	arenaPeak = 0;
	arenaPeakEver = 0;
	lastHeuristic = 0;
	memset(&profStats, 0, sizeof profStats);
#endif
	flushCache();
	isInitialized = true;
}

void JITCache::destroy() {
	jitArena = nullptr;
	blockTable = nullptr;
	smcRegistry = nullptr;
	smcPageFlags = nullptr;
	arenaOffset = 0;
	arenaSize = 0;
	isInitialized = false;
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
	free(installFrame);
	installFrame = nullptr;
#endif
}

u32* JITCache::allocateJITMemory(size_t numBytes) {
	numBytes = (numBytes + 31) & ~31;

	// If this allocation exceeds this cache's arena, flush it
	if (arenaOffset + numBytes > arenaSize) {
		flushCache();
	}

	// Grab the current pointer and bump the offset
	u32* allocatedPtr = (u32*)((u8*)jitArena + arenaOffset);
	arenaOffset += numBytes;
	return allocatedPtr;
}

void JITCache::rewindJITMemory(size_t numBytes) {
	// The caller strictly calculates a 32-byte aligned rewind size
	// Wind back the offset to recover unused memory (e.g., from bailing out)
	if (arenaOffset >= numBytes) {
		arenaOffset -= numBytes;
	} else {
		arenaOffset = 0; // Fallback underflow protection
	}
}

BasicBlock* JITCache::registerBlock(u32 pc, u32 length, JITBlockFunc execute, bool thumb) {
	u32 index = ((pc >> 1) ^ (pc >> 13)) & (HASH_TABLE_SIZE - 1);
	length = (length & 0x7FFFFFFFu) | (thumb ? 0x80000000u : 0u);

	u32 evictedPC = blockTable[index].startPC;
	PROFILER_CACHE_EVICT(evictedPC, pc);

	BasicBlock* block = &blockTable[index];

	// Unlink evicted block from the SMC bucket to prevent dangling pointers
	if (block->execute != nullptr && block->length > 0) {
		u8 oldBank = (evictedPC >> 24) & 0xFF;
		if (smcTrackedBank(oldBank)) {
			u32 oldPage = (evictedPC >> 10) & 0xFFFF;
			BasicBlock* curr = smcRegistry[oldPage];
			BasicBlock* prev = nullptr;

			while (curr) {
				if (curr == block) {
					if (prev) {
						prev->nextSMC = curr->nextSMC;
					} else {
						smcRegistry[oldPage] = curr->nextSMC;
					}
					break;
				}
				prev = curr;
				curr = curr->nextSMC;
			}

			// If the bucket is now empty, clear the global page flag
			if (smcRegistry[oldPage] == nullptr) {
				smcPageFlags[oldPage] = 0;
			}
		}
	}

	block->startPC = pc;
	block->length = length;
	block->execute = execute;
	block->nextSMC = nullptr;

	// Register with SMC tracker
	if (execute != nullptr && length > 0) {
		u8 bank = (pc >> 24) & 0xFF;
		if (smcTrackedBank(bank)) {
			u32 startPage = (pc >> 10) & 0xFFFF;
			smcPageFlags[startPage] = 1;
			// Intrusively push block to registry head
			block->nextSMC = smcRegistry[startPage];
			smcRegistry[startPage] = block;
		}
	}

	JIT_LOG_CACHE_EVENT(index, pc, evictedPC, arenaOffset, arenaOffset);
	return block;
}

// A4-P5: emit one guarded dynamic-dispatch stub (see jit_cache.h). Same hash
// calc as the static linker stub's steps 1-3 (must match getBlock() exactly),
// plus an extra mode-bit compare, no self-patching, no BL/mflr trick -- every
// visit re-does the lookup fresh since the call site's target genuinely
// varies. `expectThumb` picks which way the mode-bit check must go; the caller
// (emitDynamicExit) already knows this at compile time for every exit site, so
// there is no runtime mode test here, just two fixed-expectation stub bodies.
static u32* emitDynamicLinkerStub(u32*& emitPtr, BasicBlock* blockTable,
                                   u32 maskBegin, u32* linkerReturnAddress,
                                   bool expectThumb)
{
	u32* entry = emitPtr;

	*emitPtr++ = PPC_LIS(PPC_R10, (u32)blockTable >> 16);
	*emitPtr++ = PPC_ORI(PPC_R10, PPC_R10, (u32)blockTable & 0xFFFF);

	*emitPtr++ = PPC_SRWI(PPC_R11, PPC_R4, 1);
	*emitPtr++ = PPC_SRWI(PPC_R12, PPC_R4, 13);
	*emitPtr++ = PPC_XOR(PPC_R11, PPC_R11, PPC_R12);
	*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R11, 4, maskBegin, 27);
	*emitPtr++ = PPC_ADD(PPC_R11, PPC_R10, PPC_R11);

	// Guard 1: PC collision (hash-slot occupant is a different address)
	*emitPtr++ = PPC_LWZ(PPC_R12, PPC_R11, 0);
	*emitPtr++ = PPC_CMPW(0, PPC_R12, PPC_R4);
	u32* missPc = emitPtr++;

	// Guard 2: mode mismatch (right address, wrong ISA cached there)
	*emitPtr++ = PPC_LWZ(PPC_R12, PPC_R11, 4);              // length (thumb bit + count)
	*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 1, 31, 31);   // isolate bit31 -> 0/1
	*emitPtr++ = PPC_CMPWI(0, PPC_R12, expectThumb ? 1 : 0);
	u32* missMode = emitPtr++;

	// Guard 3: uncompiled / "don't JIT" fallback marker (execute == nullptr)
	*emitPtr++ = PPC_LWZ(PPC_R12, PPC_R11, 8);
	*emitPtr++ = PPC_CMPWI(0, PPC_R12, 0);
	u32* missExec = emitPtr++;

	// Hit: jump straight into the target block's arena code. r29/r4/flags/regs
	// were already set up by emitDynamicExit before branching here -- no
	// self-patch needed, this call site's target is data-dependent.
	*emitPtr++ = PPC_MTCTR(PPC_R12);
	*emitPtr++ = PPC_BCTR();

	u32* fallback = emitPtr;
	*missPc   = PPC_BNE((u32)((fallback - missPc) * 4));
	*missMode = PPC_BNE((u32)((fallback - missMode) * 4));
	*missExec = PPC_BEQ((u32)((fallback - missExec) * 4));
	{ s32 o = (s32)((u8*)linkerReturnAddress - (u8*)emitPtr); *emitPtr++ = PPC_B(o); }

	return entry;
}

void JITCache::flushCache() {
	PROFILER_CACHE_FLUSH_START();
	JIT_LOG_CACHE_FLUSH();

	arenaOffset = 0;

	// Defensive null-guards (matching the `if (jitArena)` below): flushCache()
	// is now also called from outside jit_trace.cpp's own lifecycle (P4 --
	// MMU_Reset()/savestate-load bulk memory overwrites), so it must tolerate
	// being invoked before initialize() has ever run.
	if (blockTable)   memset(blockTable, 0, HASH_TABLE_SIZE * sizeof(BasicBlock));
	if (smcRegistry)  memset(smcRegistry, 0, SMC_MAP_SIZE * sizeof(BasicBlock*));
	if (smcPageFlags) memset(smcPageFlags, 0, SMC_MAP_SIZE * sizeof(u8));

	if (jitArena) {
		u32* emitPtr = jitArena;
		linkerStubAddress = emitPtr;

		// 1. Load the Base Address of the blockTable struct array
		*emitPtr++ = PPC_LIS(PPC_R10, (u32)blockTable >> 16);
		*emitPtr++ = PPC_ORI(PPC_R10, PPC_R10, (u32)blockTable & 0xFFFF);

		// 2. Native Hash Calculation
		*emitPtr++ = PPC_SRWI(PPC_R11, PPC_R4, 1);
		*emitPtr++ = PPC_SRWI(PPC_R12, PPC_R4, 13);
		*emitPtr++ = PPC_XOR(PPC_R11, PPC_R11, PPC_R12);
		// Dynamically calculate the Native Mask boundaries based on HASH_TABLE_SIZE
		u32 hashBits = __builtin_ctz(HASH_TABLE_SIZE); // 8192 = 13, 32768 = 15, 65536 = 16
		u32 maskBegin = 27 - hashBits + 1;

		*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R11, 4, maskBegin, 27);
		*emitPtr++ = PPC_ADD(PPC_R11, PPC_R10, PPC_R11);

		// 3. Miss Guard 1: PC Collision Check
		*emitPtr++ = PPC_LWZ(PPC_R12, PPC_R11, 0);
		*emitPtr++ = PPC_CMPW(0, PPC_R12, PPC_R4);
		u32* branchCollision = emitPtr;
		*emitPtr++ = PPC_BNE(0);

		// 4. CACHE HIT: Extract Block execution address (execute is at offset 8)
		*emitPtr++ = PPC_LWZ(PPC_R12, PPC_R11, 8);

		// 5. Miss Guard 2: Hard Stop for Fallback Blocks (execute == nullptr)
		*emitPtr++ = PPC_CMPWI(0, PPC_R12, 0);
		u32* branchFailBlock = emitPtr;
		*emitPtr++ = PPC_BEQ(0);

		// 6. DIRECT PATCHING (Only runs for valid, executable JIT blocks!)
		*emitPtr++ = PPC_MFLR(PPC_R10);
		*emitPtr++ = PPC_ADDI(PPC_R10, PPC_R10, -4);
		*emitPtr++ = PPC_SUBF(PPC_R11, PPC_R10, PPC_R12);
		*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R11, 0, 6, 29);
		*emitPtr++ = PPC_ORIS(PPC_R11, PPC_R11, 0x4800);
		*emitPtr++ = PPC_STW(PPC_R11, PPC_R10, 0);

		// 7. Flush Broadway CPU Cache
		*emitPtr++ = PPC_DCBST(0, PPC_R10);
		*emitPtr++ = PPC_SYNC();
		*emitPtr++ = PPC_ICBI(0, PPC_R10);
		*emitPtr++ = PPC_SYNC();
		*emitPtr++ = PPC_ISYNC();

		// 8. Execute Target Block
		*emitPtr++ = PPC_MTCTR(PPC_R12);
		*emitPtr++ = PPC_BCTR();

		// 9. CACHE MISS / FALLBACK YIELD TARGET
		u32* missTarget = emitPtr;
		linkerReturnAddress = missTarget;

		// Correctly route code collisions and fail blocks out to the C++ handler
		*branchCollision = PPC_BNE((u32)((missTarget - branchCollision) * 4));
		*branchFailBlock = PPC_BEQ((u32)((missTarget - branchFailBlock) * 4));

		*emitPtr++ = PPC_LIS(PPC_R12, (u32)&ExecuteJITTrace_Return >> 16);
		*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, (u32)&ExecuteJITTrace_Return & 0xFFFF);
		*emitPtr++ = PPC_MTCTR(PPC_R12);
		*emitPtr++ = PPC_BCTR();

		// A4-P5: the two guarded dynamic-dispatch stubs, right after the
		// static one (same hash calc inputs still in scope: blockTable,
		// maskBegin, and linkerReturnAddress == missTarget above).
		linkerStubDynamicThumbAddress =
			emitDynamicLinkerStub(emitPtr, blockTable, maskBegin, linkerReturnAddress, /*expectThumb=*/true);
		linkerStubDynamicArmAddress =
			emitDynamicLinkerStub(emitPtr, blockTable, maskBegin, linkerReturnAddress, /*expectThumb=*/false);

		arenaOffset = ((emitPtr - jitArena) * sizeof(u32) + 31) & ~31;

		// Flush the newly generated Linker Stub from D-Cache to I-Cache
		u32 stubSize = (u8*)emitPtr - (u8*)jitArena;
		DCStoreRange((void*)jitArena, stubSize);
		ICInvalidateRange((void*)jitArena, stubSize);
	}
	PROFILER_CACHE_FLUSH_END();
}

// SMC eviction handler
void JITCache::invalidateSMCTarget(u32 targetEA) {
	// Reachable from MMU write hooks that can fire before jitInit() (and, with
	// two caches now, via jitInvalidateSMC() even if only one is up).
	if (!isInitialized) return;
	g_jitSmcChecks++;
#ifdef DESMUME_JIT_TRACE_FIRST
	{
		static u64 s_lastReport = 0;
		if (g_jitSmcChecks - s_lastReport >= 200000) {
			s_lastReport = g_jitSmcChecks;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) { fprintf(f, "[jit] smc: %llu checks, %llu kills\n",
			                 (unsigned long long)g_jitSmcChecks,
			                 (unsigned long long)g_jitSmcKills); fclose(f); }
		}
	}
#endif
	// Maximum trace span is (JIT_TRACE_MAX_INSTRUCTIONS+1)*4 bytes (ARM; THUMB is
	// half). Maximum write size is 36 bytes. Branchless minimum boundary clamp at 0.
	s32 offsetDiff = (s32)(targetEA - ((JIT_TRACE_MAX_INSTRUCTIONS + 1) * 4));
	u32 startEA = offsetDiff & ~(offsetDiff >> 31);
	u32 endEA = targetEA + 36;

	u32 startPage = (startEA >> 10) & 0xFFFF;
	u32 endPage = (endEA >> 10) & 0xFFFF;

	for (u32 page = startPage; page <= endPage; page++) {
		BasicBlock* curr = smcRegistry[page];
		BasicBlock* prev = nullptr;

		while (curr) {
			u32 blockStartPC = curr->startPC;
			u32 blockEndPC = blockStartPC + curr->byteLength();

			// Overlap Detection: Write Range vs Block Range
			if (targetEA < blockEndPC && endEA > blockStartPC) {
				if (curr->execute) {
					// Surgical Trampoline Patch: Overwrite first instruction with PPC_B to exit handler
					u32* codePtr = (u32*)curr->execute;
					s32 branchOffset = (s32)((u8*)linkerReturnAddress - (u8*)codePtr);
					*codePtr = PPC_B(branchOffset);

					// Hardware Cache Sync on 4-byte patched instruction
					DCStoreRange(codePtr, 4);
					ICInvalidateRange(codePtr, 4);
					// execute=null + length=0 reads as a genuine miss so the next
					// lookup recompiles (vs. a length-1 "don't JIT" marker).
					curr->execute = nullptr;
					curr->length = 0;
					g_jitSmcKills++;
				}

				// Remove block from the SMC bucket linked list
				BasicBlock* next = curr->nextSMC;
				if (prev) {
					prev->nextSMC = next;
				} else {
					smcRegistry[page] = next;
				}
				curr = next;
			} else {
				prev = curr;
				curr = curr->nextSMC;
			}
		}

		// Clear smcPageFlags byte if bucket list is now completely empty
		if (smcRegistry[page] == nullptr) {
			smcPageFlags[page] = 0;
		}
	}
}

// =========================================================================
// §3.3 JIT cache-pressure telemetry (HARNESS_PROFILE only)
// =========================================================================
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)

void JITCache::profCacheHit() { profStats.hits++; }

void JITCache::profCacheMiss(u32 slotPC) {
	if (slotPC == 0) profStats.coldMisses++;
	else             profStats.collisionMisses++;
}

// Called from registerBlock() *before* the slot is overwritten. Prices the
// outgoing block's lifetime (registrations survived) when a genuinely live,
// executable block is being displaced by a hash collision -- a "don't JIT"
// marker (execute==null, len>0) or an SMC-killed slot (execute==null, len==0)
// is not a thrash eviction. Then stamps the slot with the current sequence
// number for the block that registerBlock() is about to install.
void JITCache::profCacheEvict(u32 evictedPC, u32 newPC) {
	profStats.registrations++;
	if ((u32)arenaOffset > arenaPeak)     arenaPeak     = (u32)arenaOffset;  // §3.3b
	if (arenaPeak         > arenaPeakEver) arenaPeakEver = arenaPeak;
	u32 index = ((newPC >> 1) ^ (newPC >> 13)) & (HASH_TABLE_SIZE - 1);
	const BasicBlock& ev = blockTable[index];
	if (evictedPC != 0 && ev.execute != nullptr && ev.length > 0) {
		profStats.evictions++;
		if (installFrame)
			profStats.evictLifetimeSum += (installSeq - installFrame[index]);
	}
	if (installFrame) installFrame[index] = ++installSeq;
}

void JITCache::profCacheFlushStart() {
	profStats.flushes++;
	if ((u32)arenaOffset > arenaPeak)     arenaPeak     = (u32)arenaOffset;  // §3.3b
	if (arenaPeak         > arenaPeakEver) arenaPeakEver = arenaPeak;
	arenaPeak = 0;   // per-flush high-water resets with the arena
	if (installFrame) memset(installFrame, 0, HASH_TABLE_SIZE * sizeof(u32));
	installSeq = 0;
}

void JITCache::profEmitReport(const char* tag) {
	const CacheStats& s = profStats;
	u64 misses  = s.coldMisses + s.collisionMisses;
	u64 lookups = s.hits + misses;
	u64 hitPct  = lookups ? s.hits * 100 / lookups : 0;
	u64 lifeAvg = s.evictions ? s.evictLifetimeSum / s.evictions : 0;

	if ((u32)arenaOffset > arenaPeak)     arenaPeak     = (u32)arenaOffset;
	if (arenaPeak         > arenaPeakEver) arenaPeakEver = arenaPeak;
	u32 cap     = arenaSize ? (u32)arenaSize : 1;
	u32 fillPct = (u32)(((u64)arenaOffset * 100) / cap);
	u32 peakPct = (u32)(((u64)arenaPeakEver * 100) / cap);

	harness_profile_emitf(
		"jit cache=%s lookups=%llu hit=%llu%% coldmiss=%llu collmiss=%llu "
		"reg=%llu evict=%llu evictlife_avg=%llu flush=%llu "
		"arena=%u/%u(%u%%) arenapeak=%u(%u%%)",
		tag, (unsigned long long)lookups, (unsigned long long)hitPct,
		(unsigned long long)s.coldMisses, (unsigned long long)s.collisionMisses,
		(unsigned long long)s.registrations, (unsigned long long)s.evictions,
		(unsigned long long)lifeAvg, (unsigned long long)s.flushes,
		(unsigned)arenaOffset, (unsigned)cap, (unsigned)fillPct,
		(unsigned)arenaPeakEver, (unsigned)peakPct);

	// §3.3 / §3.3b heuristic, three-way and edge-triggered (emit only when the
	// verdict changes, not every report):
	//   1 bucket contention  - many live blocks displacing each other, short
	//     lifetimes, arena NOT full  => table too small / hash mixing bad.
	//   3 arena-capacity thrash - repeated full-cache flushes with the arena
	//     running near full          => real recycling pressure; grow the arena.
	//   2 healthy exploration - misses mostly cold, survivors long-lived.
	u8 verdict = 0;
	if (s.flushes >= 4 && peakPct >= 85 && lookups > 2000)
		verdict = 3;
	else if (s.evictions > 64 && lifeAvg < 8 && peakPct < 75 && lookups > 2000)
		verdict = 1;
	else if (misses > 2000 && s.collisionMisses * 4 < misses && lifeAvg >= 64)
		verdict = 2;

	if (verdict && verdict != lastHeuristic) {
		if (verdict == 3)
			harness_profile_emitf(
				"jit cache=%s WARNING arena-capacity thrash: flush=%llu with "
				"arenapeak=%u%% => recycling pressure, grow JIT_ARENA_SIZE%s",
				tag, (unsigned long long)s.flushes, (unsigned)peakPct,
				tag[3] == '9' ? "_ARM9" : "");
		else if (verdict == 1)
			harness_profile_emitf(
				"jit cache=%s WARNING bucket contention: evict=%llu + short "
				"lifetime (avg=%llu) with arena only %u%% full => widen "
				"HASH_TABLE_SIZE / hash mixing",
				tag, (unsigned long long)s.evictions,
				(unsigned long long)lifeAvg, (unsigned)peakPct);
		else
			harness_profile_emitf(
				"jit cache=%s note: misses mostly cold (coll=%llu/%llu), "
				"survivors long-lived (avg=%llu) => healthy exploration",
				tag, (unsigned long long)s.collisionMisses,
				(unsigned long long)misses, (unsigned long long)lifeAvg);
	}
	lastHeuristic = verdict;
}

#endif // DESMUME_HARNESS && HARNESS_PROFILE

#endif // DESMUME_JIT_ARM7
