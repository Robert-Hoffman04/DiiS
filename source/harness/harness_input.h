/*
    harness_input.h - transport-agnostic remote input core (plan §3.5).

    The byte protocol, edge/level detection and GECKO_TAP_FRAMES auto-release
    that geckoinput.cpp implements for the USB-Gecko channel, lifted out so the
    *network* transport (now the primary input path, per §0) drives the exact
    same logic. One alphabet, one behaviour, everywhere:

        a b x y   -> A / B / X / Y        s -> START
        u d l r   -> D-pad                z -> Z trigger (touch tap)
        L R       -> L / R triggers       (whitespace / unknown ignored)

    Feeds:
      - network : the DSExec harness_recv() drain routes PKT_INPUT payloads
                  into harness_input_feed_bytes()  (main.cpp).
      - gecko   : geckoinput.cpp keeps its own identical copy of this logic,
                  already wired through process_ctrls_event(); its output is
                  OR-ed in alongside this core's, so both stay live.

    The core's masks are libogc PAD_BUTTON_* / PAD_TRIGGER_* bit values, so
    harness_input_held()/down() can be OR-ed straight onto the gecko masks.

    It is an *additional* bit-OR input source next to WPAD/PAD - never a
    replacement - so a human can always intervene mid automated run.

    Deterministic record/replay: a self-contained per-frame keypad log
    (harness_input_movie_*), started/stopped by PKT_CTRL. Any bug hit during a
    live remote-input session can be captured and replayed exactly afterward.
    (The vendored upstream movie.{h,cpp} / _MOVIETIME_ TAS system is a heavier
    path - path.h/driver/xstring/backupDevice wiring absent from the Wii build;
    deferred.)

    perf_zones.h pattern: real bodies under DESMUME_HARNESS && HARNESS_INPUT,
    static-inline no-ops otherwise, so call sites carry no #ifdef.
*/
#ifndef HARNESS_INPUT_H
#define HARNESS_INPUT_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_INPUT)

// Stamp the tap timers from a run of protocol bytes (any length, any framing).
void harness_input_feed_bytes(const void *buf, int n);

// Advance one poll: bump the internal frame counter, recompute held/down.
// Call once per input-loop iteration (next to GECKO_Update()).
void harness_input_update(void);

unsigned harness_input_held(void);   // level mask, last update
unsigned harness_input_down(void);   // edge mask, last update

// PKT_CTRL "movie ..." dispatch: "record <path>", "play <path>", "stop".
void harness_input_movie_cmd(const char *arg);

// Called once per emulated frame just before update_keypad(): in record mode
// appends *keypad to the open movie; in play mode overwrites *keypad from it
// (and stops at end-of-file).
void harness_input_movie_tick(u16 *keypad);

#else // !(DESMUME_HARNESS && HARNESS_INPUT)

static inline void harness_input_feed_bytes(const void *b, int n) { (void)b; (void)n; }
static inline void harness_input_update(void) {}
static inline unsigned harness_input_held(void) { return 0; }
static inline unsigned harness_input_down(void) { return 0; }
static inline void harness_input_movie_cmd(const char *a) { (void)a; }
static inline void harness_input_movie_tick(u16 *k) { (void)k; }

#endif

#ifdef __cplusplus
}
#endif

#endif // HARNESS_INPUT_H
