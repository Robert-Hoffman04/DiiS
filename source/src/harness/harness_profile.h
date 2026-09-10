/*
    harness_profile.h - CPU profiling sink for the unified test harness (§3.2).

    Consolidates the three pre-existing profiling paths onto one transport:
      1. perf_zones.h's zone accountant - pzFrameTick() formats its per-block
         CSV row and hands it to harness_profile_emit() instead of writing
         sd:/perfzones.log directly. The pzSet/pzGet/PZ_SCOPE mechanism is
         untouched.
      2. jit_debug.h's JIT_LOG_* string macros - previously ((void)0), now
         routed to harness_profile_emitf() under HARNESS_PROFILE.
      3. debug.h's Logger/LOGC channels - Logger::vprintf() also fans every
         line into PKT_LOG via harness_profile_log() when DESMUME_HARNESS is
         defined. No call-site changes.

    Follows the perf_zones.h pattern: real bodies under the compile flag,
    `static inline` no-op stubs otherwise, so callers never need an #ifdef.
*/
#ifndef HARNESS_PROFILE_H
#define HARNESS_PROFILE_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DESMUME_HARNESS)

// Fan-in for debug.h's Logger channels: one already-formatted log line out as
// a PKT_LOG frame. Gated only on DESMUME_HARNESS (not HARNESS_PROFILE) so the
// generic log channel survives -DHARNESS_NO_PROFILE.
void harness_profile_log(const char *line);

#else
static inline void harness_profile_log(const char *line) { (void)line; }
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)

// One already-formatted profile record (CSV row, stat block, ...) as PKT_PROFILE.
void harness_profile_emit(const char *text);

// printf-style, emitted as one PKT_PROFILE frame (truncated at 511 bytes).
void harness_profile_emitf(const char *fmt, ...);

#else
static inline void harness_profile_emit(const char *text) { (void)text; }
static inline void harness_profile_emitf(const char *fmt, ...) { (void)fmt; }
#endif

#ifdef __cplusplus
}
#endif

#endif // HARNESS_PROFILE_H
