/*
    harness_transport.cpp - see harness_transport.h.

    Compiles to nothing without -DDESMUME_HARNESS
    (verify: `nm build/harness_transport.o | grep harness_` empty).
*/
#include "harness_transport.h"

#ifdef DESMUME_HARNESS

#include "harness_config.h"
#include "harness_net.h"
#include "harness_wire.h"
#include "../gekko_utils/geckoinput.h"

#include <ogc/usbgecko.h>
#include <stdio.h>
#include <string.h>

// EXI channel the USB Gecko lives on (Dolphin: memory-card Slot B). Matches
// gekko_utils/geckoinput.cpp and log_console.cpp.
#define HT_GECKO_CHAN 1

enum { BK_NONE = 0, BK_NET, BK_GECKO, BK_SD };
static int s_backend = BK_NONE;

// ---- GECKO backend --------------------------------------------------------
// Input-only bootstrap: hand the raw RX-FIFO bytes up as a PKT_INPUT payload
// (they are already the geckoinput alphabet - see wire.py). usb_recvbuffer()
// is non-blocking and returns 0 when the FIFO is empty. Note: if GECKO_Update()
// is also running it will race us for these bytes; that is acceptable for a
// pre-network bootstrap and the reason GECKO is never the default once NET works.
static u32 ht_gecko_recv(u8 *out_type, void *buf, u32 bufsize)
{
	if (!buf || !bufsize) return 0;
	u32 want = bufsize > 64 ? 64 : bufsize;
	int n = usb_recvbuffer(HT_GECKO_CHAN, buf, want);
	if (n <= 0) return 0;
	if (out_type) *out_type = HARNESS_PKT_INPUT;
	return (u32)n;
}

// ---- SD backend ----------------------------------------------------------
// One-way. Route each packet type to the file the current scripts already
// expect, so `tools/benchmark/*.sh` keep working with no live listener.
static const char *ht_sd_path(u8 t)
{
	switch (t) {
		case HARNESS_PKT_PROFILE: return "sd:/perfzones.log";
		case HARNESS_PKT_FRAME:   return "sd:/fb.dump";
		case HARNESS_PKT_CRASH:
		case HARNESS_PKT_ASSERT:  return "sd:/crash.log";
		default:                  return "sd:/bench.log";
	}
}

static void ht_sd_send(u8 t, const void *payload, u32 len)
{
	bool binary = (t == HARNESS_PKT_FRAME);
	FILE *f = fopen(ht_sd_path(t), binary ? "ab" : "a");
	if (!f) return;
	if (len && payload) fwrite(payload, 1, len, f);
	if (!binary) fputc('\n', f);
	fclose(f);
}

// ---- init --------------------------------------------------------------
bool harness_transport_init(void)
{
	if (s_backend != BK_NONE) return true;

#if defined(HARNESS_TRANSPORT_SD)
	s_backend = BK_SD;
	return true;
#elif defined(HARNESS_TRANSPORT_GECKO)
	GECKO_InputInit();
	s_backend = BK_GECKO;
	return true;
#elif defined(HARNESS_TRANSPORT_NET)
	if (harness_net_init()) { s_backend = BK_NET; return true; }
	return false;
#else // HARNESS_TRANSPORT_AUTO - NET, then GECKO, then SD as the floor.
	// harness_net_init() honours HARNESS_CONNECT_RETRIES; keep that small in an
	// AUTO build so a missing listener doesn't stall boot before the fallback.
	if (harness_net_init()) { s_backend = BK_NET; return true; }
	if (usb_isgeckoalive(HT_GECKO_CHAN)) {
		GECKO_InputInit();
		s_backend = BK_GECKO;
		return true;
	}
	s_backend = BK_SD;
	return true;
#endif
}

void harness_transport_close(void)
{
	if (s_backend == BK_NET) harness_net_close();
	s_backend = BK_NONE;
}

void harness_send(u8 packet_type, const void *payload, u32 len)
{
	switch (s_backend) {
		case BK_NET:   harness_net_send(packet_type, payload, len); break;
		case BK_SD:    ht_sd_send(packet_type, payload, len);       break;
		case BK_GECKO: break; // input-only
		default:       break;
	}
}

u32 harness_recv(u8 *out_type, void *buf, u32 bufsize)
{
	switch (s_backend) {
		case BK_NET:   return harness_net_recv(out_type, buf, bufsize);
		case BK_GECKO: return ht_gecko_recv(out_type, buf, bufsize);
		default:       return 0;
	}
}

const char *harness_transport_name(void)
{
	switch (s_backend) {
		case BK_NET:   return "net";
		case BK_GECKO: return "gecko";
		case BK_SD:    return "sd";
		default:       return "none";
	}
}

#endif // DESMUME_HARNESS
