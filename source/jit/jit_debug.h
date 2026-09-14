/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_debug.h
 *
 * No-op debug/profiler macro shims for the vendored VBA-GX JIT infrastructure.
 * Upstream (VBA-GX) provides these from Debug.h behind VBAGX_DEBUG; the
 * DeSmuMEWii port keeps the call sites intact and stubs them here until the
 * Profiler / JITDebugStateLog subsystems are ported (plan phase P5).
 *
 * Derived from Visual Boy Advance GX source/vba/gba/Debug.h
 *   (c) Daryl Borth, GPL v2+  --  see jit/upstream/PROVENANCE.md
 ***************************************************************************/

#ifndef DESMUME_JIT_DEBUG_H
#define DESMUME_JIT_DEBUG_H

#include "../harness/harness.h"   // §3.2 PKT_PROFILE sink; self-stubs otherwise

// -- Profiler counters ------------------------------------------------------
// §3.3: PROFILER_CACHE_* get real per-cache bodies under HARNESS_PROFILE. Every
// use site is a JITCache method (getBlock / registerBlock / flushCache), so
// `this` is valid and the counters live on the cache object itself (jit_cache.h).
// In getBlock() the local `block` points at the hash slot -> its startPC tells a
// cold miss (slot == 0) from a collision miss. The rest of PROFILER_* stay
// no-ops (timer / bin / bailout instrumentation not ported).
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
#define PROFILER_CACHE_HIT()              this->profCacheHit()
#define PROFILER_CACHE_MISS()             this->profCacheMiss(block ? block->startPC : 0u)
#define PROFILER_CACHE_EVICT(evicted, pc, idx) this->profCacheEvict((evicted), (pc), (idx))
#define PROFILER_CACHE_FLUSH_START()      this->profCacheFlushStart()
#define PROFILER_CACHE_FLUSH_END()        ((void)0)
#else
#define PROFILER_CACHE_HIT()                                   ((void)0)
#define PROFILER_CACHE_MISS()                                  ((void)0)
#define PROFILER_CACHE_EVICT(evicted, pc, idx)                 ((void)0)
#define PROFILER_CACHE_FLUSH_START()                           ((void)0)
#define PROFILER_CACHE_FLUSH_END()                             ((void)0)
#endif
#define PROFILER_START_TIMER(name)                             ((void)0)
#define PROFILER_ADD_TIME(stat, name)                          ((void)0)
#define PROFILER_INC(stat)                                     ((void)0)
#define PROFILER_ADD(stat, val)                                ((void)0)
#define PROFILER_BIN_BLOCK(len)                                ((void)0)
#define PROFILER_DECLARE_BAILOUT_FLAG()                        ((void)0)
#define PROFILER_CHECK_BAILOUT_TRANSITION()                    ((void)0)
#define PROFILER_SET_BAILOUT_FLAG()                            ((void)0)
#define PROFILER_CLEAR_BAILOUT_FLAG()                          ((void)0)
#define PROFILER_MARK_FRAME()                                  ((void)0)
#define PROFILER_FRAG_STATS(count, blockLen, bailed)           ((void)0)

// -- JIT trace / cache logging -------------------------------------------------
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
// §3.3 follow-up: cache_event fires once per registerBlock() -- ~750/frame on
// ARM9 in steady state, which drowns the stream. The §3.3 periodic per-core
// stat block (jit_cache.cpp profEmitReport) is the always-on aggregate; the
// raw per-block line is opt-in behind -DHARNESS_PROFILE_VERBOSE.
#ifdef HARNESS_PROFILE_VERBOSE
#define JIT_LOG_CACHE_EVENT(bucket, startPC, evictedPC, before, after) \
	harness_profile_emitf("jit cache_event bucket=%u pc=%08x evicted=%08x arena=%u->%u", \
		(unsigned)(bucket), (unsigned)(startPC), (unsigned)(evictedPC), \
		(unsigned)(before), (unsigned)(after))
#else
#define JIT_LOG_CACHE_EVENT(bucket, startPC, evictedPC, before, after) ((void)0)
#endif
#define JIT_LOG_CACHE_FLUSH() harness_profile_emitf("jit cache_flush")
#define JIT_LOG_BAILOUT(pc, opcode, reason) \
	harness_profile_emitf("jit bailout pc=%08x op=%08x reason=%d", \
		(unsigned)(pc), (unsigned)(opcode), (int)(reason))
#define JIT_LOG_FALLBACK(opcode) \
	harness_profile_emitf("jit fallback op=%08x", (unsigned)(opcode))
#define JIT_LOG_MISMATCH(msg) harness_profile_emit(msg)
#else
#define JIT_LOG_CACHE_EVENT(bucket, startPC, evictedPC, before, after) ((void)0)
#define JIT_LOG_CACHE_FLUSH()                                  ((void)0)
#define JIT_LOG_BAILOUT(pc, opcode, reason)                    ((void)0)
#define JIT_LOG_FALLBACK(opcode)                               ((void)0)
#define JIT_LOG_MISMATCH(msg)                                  ((void)0)
#endif
#define JIT_LOG_ARENA(startPC, allocOffset, reserved, used, rewind)    ((void)0)
#define JIT_LOG_BLOCK_COMPILED(startPC, block)                 ((void)0)
#define JIT_LOG_BLOCK_COMPILED_DETAILS(startPC)                ((void)0)
#define JIT_LOG_BLOCK_COMPILE_END(s, e, c, cy, b, r)           ((void)0)
#define JIT_LOG_BAILOUT_DETAILS(pc, opcode, reason)            ((void)0)
#define JIT_LOG_EXEC(count, blockLen, bailed)                  ((void)0)
#define JIT_LOG_INSN_COMPILED(pc, opcode, details, ...)        ((void)0)
#define JIT_LOG_INSN_DUMP(pc, phase, addr, word)               ((void)0)
#define JIT_LOG_TRACE_ENTRY(pc, flags)                         ((void)0)
#define JIT_LOG_TRACE_EXIT(pc, nextPC, flags, cycles)          ((void)0)
#define JIT_LOG_STATE_INIT()                                   ((void)0)
#define JIT_LOG_STATE_CPP(executedPC, nextPC, ticks, cycles)   ((void)0)
#define JIT_LOG_STATE_JIT(executedPC, nextPC, ticks, cycles)   ((void)0)
#define JIT_LOG_STATE_WRITE_TO_FILE()                          ((void)0)
#define JIT_DEBUG_DUMP_FIRST_JIT_BLOCK(block)                  ((void)0)
#define JIT_DIFFERENTIAL_THUMB_HOOK(pc, block)                 ((void)0)
#define JIT_RECORD_MEMORY_WRITE(addr, value, size)             ((void)0)

#endif // DESMUME_JIT_DEBUG_H
