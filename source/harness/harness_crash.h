/*
    harness_crash.h - host crash reporting and pass/fail assertions (plan §3.6).

    Two things, both gated on DESMUME_HARNESS && HARNESS_CRASH:

      1. harness_crash_init() installs a PPC exception "panic" hook. On any
         unhandled trap (DSI/ISI/Program/Alignment/...) it reads PC/LR/MSR/CR
         and r0..r31 from the exception frame, walks a short back-chain
         backtrace, ships it all as one best-effort HARNESS_PKT_CRASH text
         packet, then chains to the previous panic handler so the machine
         still halts exactly as before. Symbolication is desktop-side
         (tools/harness-control/symbolicate.py against the linker .map) - the
         device only sends raw 0x8xxxxxxx addresses.

      2. HARNESS_EXPECT(cond, "message") - on a false condition, sends one
         HARNESS_PKT_ASSERT packet (pass/fail byte + text). Failures only, as
         per the plan; tools/harness-control tallies these into an exit code.

    The §3.0 JIT canary/minefield trips already emit HARNESS_PKT_CRASH
    directly (jit_trace.cpp jitCanaryEmit) - same packet type, same desktop
    path, no second mechanism.

    perf_zones.h pattern: real bodies under the flag, no-op stubs otherwise,
    so call sites never need their own #ifdef.
*/
#ifndef HARNESS_CRASH_H
#define HARNESS_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(DESMUME_HARNESS) && defined(HARNESS_CRASH)

// Install the exception panic hook. Idempotent; safe to call before the
// transport is up (the CRASH packet is simply best-effort either way).
void harness_crash_init(void);

// Backs HARNESS_EXPECT(). Sends one HARNESS_PKT_ASSERT (fail). Not for direct
// use - go through the macro so expr/file/line are captured.
void harness_expect_fail(const char *expr, const char *msg,
                         const char *file, int line);

#define HARNESS_EXPECT(cond, msg)                                        \
	do {                                                                \
		if (!(cond))                                                    \
			harness_expect_fail(#cond, (msg), __FILE__, __LINE__);      \
	} while (0)

#else // !crash

static inline void harness_crash_init(void) {}
static inline void harness_expect_fail(const char *expr, const char *msg,
                                       const char *file, int line)
	{ (void)expr; (void)msg; (void)file; (void)line; }

// Keep `cond` compiled (type-checked) but evaluate nothing at runtime.
#define HARNESS_EXPECT(cond, msg) ((void)sizeof((cond)), (void)(msg))

#endif

#ifdef __cplusplus
}
#endif

#endif // HARNESS_CRASH_H
