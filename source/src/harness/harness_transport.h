/*
    harness_transport.h - the one channel every harness subsystem sends and
    receives through (plan §3.1).

    harness_send()/harness_recv() are backend-agnostic. Which backend carries
    them is decided once, at harness_transport_init(), by the probe order in
    harness.h (NET -> GECKO -> SD) unless a build pins one with
    -DHARNESS_TRANSPORT_{NET,GECKO,SD}.

      NET   - TCP to HARNESS_HOST:HARNESS_PORT, framed packets per §2. The
              default, identical in Dolphin (network passthrough) and on real
              hardware. Two-way: harness_recv() carries INPUT/CTRL packets in.
      GECKO - input-only bootstrap over the USB Gecko RX FIFO, for use before
              network passthrough is confirmed on a given Dolphin setup.
              harness_send() is a no-op; harness_recv() yields PKT_INPUT bytes.
      SD    - legacy/CI convenience: harness_send() appends framed payloads to
              the existing "sd:/...log" files so the "boot headless, kill, pull
              the file" scripts keep working with no listener. harness_recv()
              is a no-op (one-way).

    Follows the perf_zones.h pattern: real bodies under DESMUME_HARNESS,
    static-inline no-ops otherwise, so call sites never need their own #ifdef.
*/
#ifndef HARNESS_TRANSPORT_H
#define HARNESS_TRANSPORT_H

#include <gctypes.h>
#include "harness_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DESMUME_HARNESS

// Pick a backend (see probe order above) and bring it up. Returns false only
// when a pinned backend fails; AUTO always succeeds (SD is the floor). On
// false the harness stays inert and the app runs normally. Safe to re-call.
bool harness_transport_init(void);

void harness_transport_close(void);

// Frame one packet (§2) and push it out the active backend.
void harness_send(u8 packet_type, const void *payload, u32 len);

// Non-blocking. Returns the payload length of one received packet (its type in
// *out_type, up to bufsize bytes in buf), or 0 when nothing is waiting.
u32 harness_recv(u8 *out_type, void *buf, u32 bufsize);

// "net" / "gecko" / "sd" / "none" - which backend won the probe.
const char *harness_transport_name(void);

#else // !DESMUME_HARNESS

static inline bool harness_transport_init(void) { return false; }
static inline void harness_transport_close(void) {}
static inline void harness_send(u8 packet_type, const void *payload, u32 len)
	{ (void)packet_type; (void)payload; (void)len; }
static inline u32 harness_recv(u8 *out_type, void *buf, u32 bufsize)
	{ (void)out_type; (void)buf; (void)bufsize; return 0; }
static inline const char *harness_transport_name(void) { return "off"; }

#endif // DESMUME_HARNESS

#ifdef __cplusplus
}
#endif

#endif // HARNESS_TRANSPORT_H
