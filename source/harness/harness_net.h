/*
    harness_net.h - HARNESS_TRANSPORT_NET backend (plan §3.1), minimal form.

    This is the Step-2 slice: enough to open the connection and push framed
    packets out, so the Dolphin network-passthrough path can be proven with a
    LOG round-trip before §3.0/§3.1 build the full transport abstraction on top.

    Follows the perf_zones.h pattern: real declarations under DESMUME_HARNESS,
    static-inline no-ops otherwise, so call sites never need their own #ifdef.
*/
#ifndef HARNESS_NET_H
#define HARNESS_NET_H

#include <gctypes.h>
#include "harness_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DESMUME_HARNESS

// Bring the Wii network stack up and connect to HARNESS_HOST:HARNESS_PORT.
// Returns false (and disables itself) on any failure - the app then runs
// normally with the harness inert. Safe to call more than once.
bool harness_net_init(void);

// Frame one packet and write it. No-op if the transport is not connected.
void harness_net_send(u8 packet_type, const void *payload, u32 len);

// Convenience: send a UTF-8 log line as HARNESS_PKT_LOG.
void harness_net_log(const char *line);

// Non-blocking framed read. Returns the payload length of one complete packet
// (type in *out_type, up to bufsize payload bytes in buf), or 0 when a whole
// packet is not yet available. Drops the transport if the peer closes.
u32 harness_net_recv(u8 *out_type, void *buf, u32 bufsize);

void harness_net_close(void);
bool harness_net_ok(void);

#else // !DESMUME_HARNESS

static inline bool harness_net_init(void) { return false; }
static inline void harness_net_send(u8 packet_type, const void *payload, u32 len)
	{ (void)packet_type; (void)payload; (void)len; }
static inline void harness_net_log(const char *line) { (void)line; }
static inline u32 harness_net_recv(u8 *out_type, void *buf, u32 bufsize)
	{ (void)out_type; (void)buf; (void)bufsize; return 0; }
static inline void harness_net_close(void) {}
static inline bool harness_net_ok(void) { return false; }

#endif // DESMUME_HARNESS

#ifdef __cplusplus
}
#endif

#endif // HARNESS_NET_H
