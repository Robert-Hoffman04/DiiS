/*
    harness_net.cpp - see harness_net.h. Compiles to nothing without
    -DDESMUME_HARNESS (verify: `nm build/harness_net.o | grep harness_` empty).
*/
#include "harness_net.h"

#ifdef DESMUME_HARNESS

#include "harness_config.h"
#include "harness_wire.h"

#include <network.h>
#include <sys/filio.h>   // FIONBIO
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static s32  s_sock = INVALID_SOCKET;
static bool s_ok   = false;

// Framed-read accumulator: TCP is a byte stream, so a packet can arrive split
// across reads or several at once. harness_net_recv() tops this up non-blocking
// and hands back one whole packet at a time.
static u8  s_rx[4096];
static u32 s_rxlen = 0;

static void net_set_nonblocking(s32 s)
{
	u32 on = 1;
	net_ioctl(s, FIONBIO, &on);
}

// libogc's net_init() can return -EAGAIN while the IOS stack is still coming
// up; retry a few times. On Dolphin it succeeds on the first call.
static bool net_stack_up(void)
{
	for (int i = 0; i < 20; i++) {
		s32 r = net_init();
		if (r >= 0) return true;
		usleep(200 * 1000);
	}
	return false;
}

static bool resolve_host(struct sockaddr_in *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port   = htons(HARNESS_PORT);

	if (inet_aton(HARNESS_HOST, &sa->sin_addr) != 0)
		return true;

	struct hostent *he = net_gethostbyname(HARNESS_HOST);
	if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
		printf("harness: cannot resolve '%s'\n", HARNESS_HOST);
		return false;
	}
	memcpy(&sa->sin_addr, he->h_addr_list[0], sizeof(sa->sin_addr));
	return true;
}

bool harness_net_init(void)
{
	if (s_ok) return true;

	if (!net_stack_up()) {
		printf("harness: net_init() failed; harness disabled\n");
		return false;
	}

	struct sockaddr_in sa;
	if (!resolve_host(&sa))
		return false;

	for (int attempt = 0; attempt < HARNESS_CONNECT_RETRIES; attempt++) {
		s32 sock = net_socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) { usleep(250 * 1000); continue; }

		if (net_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) >= 0) {
			net_set_nonblocking(sock);
			s_sock = sock;
			s_rxlen = 0;
			s_ok   = true;
			printf("harness: connected %s:%d\n", HARNESS_HOST, HARNESS_PORT);
			harness_net_log("harness: device up");
			return true;
		}
		net_close(sock);
		usleep(250 * 1000);
	}

	printf("harness: connect to %s:%d failed; harness disabled\n",
	       HARNESS_HOST, HARNESS_PORT);
	return false;
}

static bool write_all(const void *data, s32 len)
{
	const u8 *p = (const u8 *)data;
	while (len > 0) {
		s32 n = net_write(s_sock, p, len);
		if (n <= 0) {
			// peer gone / fatal - drop the transport, keep the app alive
			net_close(s_sock);
			s_sock = INVALID_SOCKET;
			s_ok   = false;
			return false;
		}
		p   += n;
		len -= n;
	}
	return true;
}

void harness_net_send(u8 packet_type, const void *payload, u32 len)
{
	if (!s_ok) return;

	u8 hdr[HARNESS_WIRE_HEADER_LEN];
	hdr[0] = HARNESS_WIRE_MAGIC0;
	hdr[1] = HARNESS_WIRE_MAGIC1;
	hdr[2] = HARNESS_WIRE_MAGIC2;
	hdr[3] = HARNESS_WIRE_MAGIC3;
	hdr[4] = HARNESS_WIRE_VERSION;
	hdr[5] = packet_type;
	hdr[6] = (u8)(len >> 24);
	hdr[7] = (u8)(len >> 16);
	hdr[8] = (u8)(len >> 8);
	hdr[9] = (u8)(len);

	if (!write_all(hdr, sizeof(hdr))) return;
	if (len && payload) write_all(payload, (s32)len);
}

void harness_net_log(const char *line)
{
	if (!line) return;
	harness_net_send(HARNESS_PKT_LOG, line, (u32)strlen(line));
}

u32 harness_net_recv(u8 *out_type, void *buf, u32 bufsize)
{
	if (!s_ok) return 0;

	// Top up the accumulator (non-blocking: net_recv returns <0 on would-block).
	if (s_rxlen < sizeof(s_rx)) {
		s32 n = net_recv(s_sock, s_rx + s_rxlen, sizeof(s_rx) - s_rxlen, 0);
		if (n == 0) { harness_net_close(); return 0; }  // peer closed
		if (n > 0)  s_rxlen += (u32)n;
	}

	if (s_rxlen < HARNESS_WIRE_HEADER_LEN)
		return 0;

	// Resync on a bad magic by dropping one byte at a time.
	if (s_rx[0] != HARNESS_WIRE_MAGIC0 || s_rx[1] != HARNESS_WIRE_MAGIC1 ||
	    s_rx[2] != HARNESS_WIRE_MAGIC2 || s_rx[3] != HARNESS_WIRE_MAGIC3) {
		memmove(s_rx, s_rx + 1, --s_rxlen);
		return 0;
	}

	u32 plen = ((u32)s_rx[6] << 24) | ((u32)s_rx[7] << 16) |
	           ((u32)s_rx[8] << 8)  |  (u32)s_rx[9];
	if (plen > sizeof(s_rx) - HARNESS_WIRE_HEADER_LEN) {
		// Larger than we can ever buffer - the stream is corrupt; reset.
		s_rxlen = 0;
		return 0;
	}

	u32 total = HARNESS_WIRE_HEADER_LEN + plen;
	if (s_rxlen < total)
		return 0;  // rest of the payload not here yet

	if (out_type) *out_type = s_rx[5];
	u32 copy = plen < bufsize ? plen : bufsize;
	if (copy && buf) memcpy(buf, s_rx + HARNESS_WIRE_HEADER_LEN, copy);

	s_rxlen -= total;
	memmove(s_rx, s_rx + total, s_rxlen);
	return plen;
}

void harness_net_close(void)
{
	if (s_sock != INVALID_SOCKET) net_close(s_sock);
	s_sock = INVALID_SOCKET;
	s_ok   = false;
}

bool harness_net_ok(void) { return s_ok; }

#endif // DESMUME_HARNESS
