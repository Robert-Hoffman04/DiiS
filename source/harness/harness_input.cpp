/*
    harness_input.cpp - see harness_input.h. Compiles to nothing without
    -DDESMUME_HARNESS -DHARNESS_INPUT.

    The tap-timer / edge-detection logic here is a deliberate verbatim copy of
    geckoinput.cpp's - the two feeds (network, gecko) must behave identically.
*/
#include "harness.h"
#include "harness_input.h"

#if defined(DESMUME_HARNESS) && defined(HARNESS_INPUT)

#include <gccore.h>
#include <stdio.h>
#include <string.h>

// ---- shared core (mirrors geckoinput.cpp) ----------------------------------

// How many harness_input_update() calls a received byte stays "held". Same
// ~100 ms window geckoinput uses at 60 Hz.
#define HI_TAP_FRAMES 6

enum {
	HB_A, HB_B, HB_X, HB_Y,
	HB_START,
	HB_UP, HB_DOWN, HB_LEFT, HB_RIGHT,
	HB_Z, HB_L, HB_R,
	HB_COUNT
};

static const unsigned hi_mask[HB_COUNT] = {
	PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y,
	PAD_BUTTON_START,
	PAD_BUTTON_UP, PAD_BUTTON_DOWN, PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT,
	PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R
};

static unsigned hi_frame = 0;
static unsigned hi_hold_until[HB_COUNT];
static unsigned hi_held = 0;
static unsigned hi_down = 0;

static int hi_byte_to_slot(unsigned char c)
{
	switch (c) {
		case 'a': return HB_A;
		case 'b': return HB_B;
		case 'x': return HB_X;
		case 'y': return HB_Y;
		case 's': return HB_START;
		case 'u': return HB_UP;
		case 'd': return HB_DOWN;
		case 'l': return HB_LEFT;
		case 'r': return HB_RIGHT;
		case 'z': return HB_Z;
		case 'L': return HB_L;
		case 'R': return HB_R;
		default:  return -1;
	}
}

void harness_input_feed_bytes(const void *buf, int n)
{
	const unsigned char *p = (const unsigned char *)buf;
	for (int i = 0; i < n; i++) {
		int slot = hi_byte_to_slot(p[i]);
		if (slot >= 0)
			hi_hold_until[slot] = hi_frame + HI_TAP_FRAMES;
	}
}

void harness_input_update(void)
{
	unsigned prev = hi_held;
	unsigned held = 0;

	hi_frame++;
	for (int i = 0; i < HB_COUNT; i++)
		if (hi_hold_until[i] > hi_frame)
			held |= hi_mask[i];

	hi_held = held;
	hi_down = held & ~prev;
}

unsigned harness_input_held(void) { return hi_held; }
unsigned harness_input_down(void) { return hi_down; }

// ---- deterministic per-frame keypad record / replay -----------------------
//
// File: "DMOV" | u8 ver=1 | u8 rsv[3] | { u16 BE keypad } per frame.
// Deterministic when paired with a fixed start point (manifest autoload_slot
// or a power-on ROM) and playback from frame 0.

#define HI_MOVIE_MAGIC "DMOV"

enum { HI_MOV_OFF = 0, HI_MOV_RECORD, HI_MOV_PLAY };

static int   hi_mov_mode = HI_MOV_OFF;
static FILE *hi_mov_fp   = 0;
static unsigned hi_mov_frames = 0;

static void hi_movie_close(void)
{
	if (hi_mov_fp) { fclose(hi_mov_fp); hi_mov_fp = 0; }
	hi_mov_mode = HI_MOV_OFF;
}

void harness_input_movie_cmd(const char *arg)
{
	while (*arg == ' ') arg++;

	if (!strncmp(arg, "stop", 4)) {
		if (hi_mov_mode != HI_MOV_OFF)
			harness_profile_log("harness: movie stopped");
		hi_movie_close();
		return;
	}

	int rec = !strncmp(arg, "record", 6) ? 6 : 0;
	int ply = !strncmp(arg, "play",   4) ? 4 : 0;
	if (!rec && !ply) return;

	const char *path = arg + (rec ? rec : ply);
	while (*path == ' ') path++;
	if (!*path) return;

	hi_movie_close();
	hi_mov_frames = 0;

	if (rec) {
		hi_mov_fp = fopen(path, "wb");
		if (!hi_mov_fp) { harness_profile_log("harness: movie record open failed"); return; }
		unsigned char hdr[8] = { 'D','M','O','V', 1, 0, 0, 0 };
		fwrite(hdr, 1, sizeof hdr, hi_mov_fp);
		hi_mov_mode = HI_MOV_RECORD;
		harness_profile_log("harness: movie recording");
	} else {
		hi_mov_fp = fopen(path, "rb");
		if (!hi_mov_fp) { harness_profile_log("harness: movie play open failed"); return; }
		unsigned char hdr[8];
		if (fread(hdr, 1, sizeof hdr, hi_mov_fp) != sizeof hdr ||
		    memcmp(hdr, HI_MOVIE_MAGIC, 4) != 0) {
			harness_profile_log("harness: movie bad header");
			hi_movie_close();
			return;
		}
		hi_mov_mode = HI_MOV_PLAY;
		harness_profile_log("harness: movie playing");
	}
}

void harness_input_movie_tick(u16 *keypad)
{
	if (hi_mov_mode == HI_MOV_RECORD) {
		unsigned char b[2] = { (unsigned char)(*keypad >> 8), (unsigned char)(*keypad & 0xFF) };
		fwrite(b, 1, 2, hi_mov_fp);
		hi_mov_frames++;
	} else if (hi_mov_mode == HI_MOV_PLAY) {
		unsigned char b[2];
		if (fread(b, 1, 2, hi_mov_fp) != 2) {
			char msg[64];
			snprintf(msg, sizeof msg, "harness: movie replay finished (%u frames)", hi_mov_frames);
			harness_profile_log(msg);
			hi_movie_close();
			return;
		}
		*keypad = (u16)((b[0] << 8) | b[1]);
		hi_mov_frames++;
	}
}

#endif // DESMUME_HARNESS && HARNESS_INPUT
