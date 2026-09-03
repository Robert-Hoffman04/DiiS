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

// -- Profiler counters ------------------------------------------------------
#define PROFILER_CACHE_HIT()                                   ((void)0)
#define PROFILER_CACHE_MISS()                                  ((void)0)
#define PROFILER_CACHE_EVICT(evicted, pc)                      ((void)0)
#define PROFILER_CACHE_FLUSH_START()                           ((void)0)
#define PROFILER_CACHE_FLUSH_END()                             ((void)0)
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
#define JIT_LOG_CACHE_EVENT(bucket, startPC, evictedPC, before, after) ((void)0)
#define JIT_LOG_CACHE_FLUSH()                                  ((void)0)
#define JIT_LOG_ARENA(startPC, allocOffset, reserved, used, rewind)    ((void)0)
#define JIT_LOG_BLOCK_COMPILED(startPC, block)                 ((void)0)
#define JIT_LOG_BLOCK_COMPILED_DETAILS(startPC)                ((void)0)
#define JIT_LOG_BLOCK_COMPILE_END(s, e, c, cy, b, r)           ((void)0)
#define JIT_LOG_BAILOUT(pc, opcode, reason)                    ((void)0)
#define JIT_LOG_BAILOUT_DETAILS(pc, opcode, reason)            ((void)0)
#define JIT_LOG_EXEC(count, blockLen, bailed)                  ((void)0)
#define JIT_LOG_FALLBACK(opcode)                               ((void)0)
#define JIT_LOG_INSN_COMPILED(pc, opcode, details, ...)        ((void)0)
#define JIT_LOG_INSN_DUMP(pc, phase, addr, word)               ((void)0)
#define JIT_LOG_TRACE_ENTRY(pc, flags)                         ((void)0)
#define JIT_LOG_TRACE_EXIT(pc, nextPC, flags, cycles)          ((void)0)
#define JIT_LOG_MISMATCH(msg)                                  ((void)0)
#define JIT_LOG_STATE_INIT()                                   ((void)0)
#define JIT_LOG_STATE_CPP(executedPC, nextPC, ticks, cycles)   ((void)0)
#define JIT_LOG_STATE_JIT(executedPC, nextPC, ticks, cycles)   ((void)0)
#define JIT_LOG_STATE_WRITE_TO_FILE()                          ((void)0)
#define JIT_DEBUG_DUMP_FIRST_JIT_BLOCK(block)                  ((void)0)
#define JIT_DIFFERENTIAL_THUMB_HOOK(pc, block)                 ((void)0)
#define JIT_RECORD_MEMORY_WRITE(addr, value, size)             ((void)0)

#endif // DESMUME_JIT_DEBUG_H
