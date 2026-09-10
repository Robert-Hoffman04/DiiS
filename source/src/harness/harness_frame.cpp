/*
    harness_frame.cpp - see harness_frame.h. Compiles to nothing without
    -DDESMUME_HARNESS -DHARNESS_FRAME.
*/
#include "harness.h"
#include "harness_frame.h"

#if defined(DESMUME_HARNESS) && defined(HARNESS_FRAME)

#include "harness_wire.h"
#include "../GPU.h"

#include <ogc/lwp_watchdog.h>   // gettime()
#include <stdlib.h>
#include <string.h>

// Both DS screens, stacked: 256 wide, 192*2 tall, one u16 per pixel.
#define FRAME_W 256
#define FRAME_H (192 * 2)
#define FRAME_PIX (FRAME_W * FRAME_H)

static u32  s_every  = 0;
static char s_label[32];
static int  s_pending = 0;

void harness_frame_set_every(u32 n) { s_every = n; }

void harness_frame_request(const char *label)
{
	s_label[0] = 0;
	if (label) { strncpy(s_label, label, sizeof s_label - 1); s_label[sizeof s_label - 1] = 0; }
	s_pending = 1;
}

void harness_frame_capture(const char *label)
{
	// PKT_FRAME sub-header (wire.py _FRAME_SUBHDR, big-endian >IQHHBB):
	//   seq u32 | tick u64 | w u16 | h u16 | fmt u8 | label_len u8 | label | image
	static u32 seq = 0;
	seq++;

	const char *lbl = (label && *label) ? label : "";
	u32 llen = (u32)strlen(lbl);
	if (llen > 255) llen = 255;

	const u32 hdr = 4 + 8 + 2 + 2 + 1 + 1;
	const u32 img = FRAME_PIX * 2;   // RGB565, 2 bytes/pixel
	u8 *buf = (u8 *)malloc(hdr + llen + img);
	if (!buf) return;

	u64 tick = gettime();
	u8 *p = buf;
	*p++ = seq >> 24; *p++ = seq >> 16; *p++ = seq >> 8; *p++ = seq;
	for (int i = 56; i >= 0; i -= 8) *p++ = (u8)(tick >> i);
	*p++ = (u8)(FRAME_W >> 8); *p++ = (u8)(FRAME_W & 0xFF);
	*p++ = (u8)(FRAME_H >> 8); *p++ = (u8)(FRAME_H & 0xFF);
	*p++ = HARNESS_FRAME_FMT_RGB565;
	*p++ = (u8)llen;
	memcpy(p, lbl, llen); p += llen;

	// GPU_screen is native-endian u16, layout X-B5-G5-R5 (the software/GX
	// output the RGB15_REVERSE() present path consumes). Repack to RGB565 and
	// emit little-endian bytes - wii_control.py reads RGB565 low-byte-first.
	const u16 *src = (const u16 *)GPU_screen;
	for (u32 i = 0; i < FRAME_PIX; i++) {
		u16 px = src[i];
		u32 r = px & 0x1F;
		u32 g = (px >> 5) & 0x1F;
		u32 b = (px >> 10) & 0x1F;
		u16 o = (u16)((r << 11) | ((g << 1 | g >> 4) << 5) | b);
		*p++ = (u8)o;
		*p++ = (u8)(o >> 8);
	}

	harness_send(HARNESS_PKT_FRAME, buf, (u32)(p - buf));
	free(buf);
}

void harness_frame_tick(u32 frame)
{
	if (s_pending) {
		s_pending = 0;
		harness_frame_capture(s_label[0] ? s_label : 0);
		return;
	}
	if (s_every && (frame % s_every) == 0)
		harness_frame_capture(0);
}

#endif // DESMUME_HARNESS && HARNESS_FRAME
