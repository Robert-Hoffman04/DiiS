/*
    harness_frame.h - on-demand video buffer readout (plan §3.4).

    Generalises the fixed-frame -DDESMUME_FBDUMP block (main.cpp, hardcoded
    frame, SD-only) into a PKT_FRAME stream with three triggers:
      1. every N frames        - manifest "frame_every=N" / harness_frame_set_every()
      2. an in-code call site  - DEBUG_FRAME("label")
      3. a host command        - PKT_CTRL "capture_frame [label]"

    Capture is opt-in and off by default even in a harness build (a runtime
    toggle): nothing is sent until frame_every is set or a capture is
    explicitly requested.

    Follows the perf_zones.h pattern: real bodies under DESMUME_HARNESS &&
    HARNESS_FRAME, static-inline no-ops otherwise.
*/
#ifndef HARNESS_FRAME_H
#define HARNESS_FRAME_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_FRAME)

void harness_frame_set_every(u32 n);          // 0 = off
void harness_frame_request(const char *label); // one-shot, sent on next tick

// Called once per emulated frame from DSExec(). Sends a PKT_FRAME when a
// one-shot request is pending or (every!=0 && frame % every == 0).
void harness_frame_tick(u32 frame);

// Immediate capture-and-send (the DEBUG_FRAME() call site).
void harness_frame_capture(const char *label);

#define DEBUG_FRAME(label) harness_frame_capture(label)

#else // !(_HARNESS && HARNESS_FRAME)

static inline void harness_frame_set_every(u32 n) { (void)n; }
static inline void harness_frame_request(const char *l) { (void)l; }
static inline void harness_frame_tick(u32 f) { (void)f; }
static inline void harness_frame_capture(const char *l) { (void)l; }
#define DEBUG_FRAME(label) ((void)(label))

#endif

#ifdef __cplusplus
}
#endif

#endif // HARNESS_FRAME_H
