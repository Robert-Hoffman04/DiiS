/*
    harness.h - master include for the unified test harness (plan §3.0).

    DESMUME_HARNESS is the one flag a release build must never define. Every
    other harness sub-flag is meaningless without it and is folded into it
    below, so a clean build only ever has to reason about this one symbol.

    Every harness header follows the perf_zones.h pattern: real declarations
    inside #ifdef DESMUME_HARNESS / matching sub-flag, `static inline` no-op
    stubs in the #else, so call sites never need their own #ifdef. Judge every
    new header against `nm build/*.o | grep -i harness_` coming back empty on a
    flag-off build.
*/
#ifndef DESMUME_HARNESS_H
#define DESMUME_HARNESS_H

#ifdef DESMUME_HARNESS

  #include "harness_config.h"

  #ifndef HARNESS_NO_PROFILE
    #define HARNESS_PROFILE 1
    // -DHARNESS_PROFILE_VERBOSE additionally streams the raw per-registerBlock
    // "jit cache_event" line (§3.3). Off by default - the periodic per-core
    // stat block (jit_cache.cpp profEmitReport) is the always-on aggregate.
  #endif
  #ifndef HARNESS_NO_BOOT
    #define HARNESS_BOOT 1
  #endif
  #ifndef HARNESS_NO_FRAME
    #define HARNESS_FRAME 1
  #endif
  #ifndef HARNESS_NO_INPUT
    #define HARNESS_INPUT 1
  #endif
  #ifndef HARNESS_NO_CRASH
    #define HARNESS_CRASH 1     // exception handler + assertions (§3.6),
                               // and the JIT heap canary/minefield poll (§3.0)
  #endif
  #ifndef HARNESS_NO_WATCHDOG
    #define HARNESS_WATCHDOG 1  // hang detection (§3.4)
  #endif

  // Transport probe order per §0: NET first (works identically in Dolphin with
  // network passthrough and on real hardware), GECKO as an input-only
  // bootstrap before network is confirmed working, SD as legacy/CI fallback.
  #if !defined(HARNESS_TRANSPORT_NET) && !defined(HARNESS_TRANSPORT_SD) \
      && !defined(HARNESS_TRANSPORT_GECKO)
    #define HARNESS_TRANSPORT_AUTO 1
  #endif

#endif // DESMUME_HARNESS

// The channel every harness subsystem sends/receives through (§3.1). Routes
// harness_send()/harness_recv() through the backend the probe order above
// selects. Safe to include unconditionally - stubs itself out without
// DESMUME_HARNESS.
#include "harness_transport.h"

// CPU profiling sink (§3.2): perf_zones CSV rows, JIT_LOG_* strings, and the
// debug.h Logger channels all funnel through here. Self-stubs without
// DESMUME_HARNESS / with -DHARNESS_NO_PROFILE.
#include "harness_profile.h"

// Host crash reporting + pass/fail assertions (§3.6): PPC exception panic hook
// -> PKT_CRASH, and HARNESS_EXPECT() -> PKT_ASSERT. Self-stubs without
// DESMUME_HARNESS / with -DHARNESS_NO_CRASH.
#include "harness_crash.h"

// Boot manifest + multi-ROM playlist + watchdog tagging (§3.4): sd:/harness.cfg
// replaces -DDESMUME_FORCE_ROM/CORE/USB; drives the per-ROM boot path in a loop.
// Self-stubs without DESMUME_HARNESS / with -DHARNESS_NO_BOOT.
#include "harness_boot.h"

// On-demand video buffer readout (§3.4): PKT_FRAME stream, three triggers, off
// by default. Self-stubs without DESMUME_HARNESS / with -DHARNESS_NO_FRAME.
#include "harness_frame.h"

// Transport-agnostic remote input core + deterministic keypad record/replay
// (§3.5): PKT_INPUT payloads drive the same tap/edge logic geckoinput uses.
// Self-stubs without DESMUME_HARNESS / with -DHARNESS_NO_INPUT.
#include "harness_input.h"

#endif // DESMUME_HARNESS_H
