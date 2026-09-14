// Lightweight on-screen FPS counter.
//
// Rendering: a 128x8 RGB5A3 glyph atlas (16 glyphs, 8x8 each) is built once at
// startup and drawn as textured quads with the exact vertex format / TEV setup
// the DS screen quads already use (GX_VTXFMT0: POS_XY + TEX0, TEV REPLACE).
// The text sits at a fixed position in the 640x480 orthographic space, in the
// top-left corner - outside the centred DS display area in every screen layout.
//
// Measurement: FPSOverlay_Tick() is polled once per emulated frame from
// DSExec(); the rate is averaged over ~0.5s windows of the Wii timebase.

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>

#include "fps_overlay.h"

// Loaded by draw_thread(); the un-scaled, un-rotated 2D model-view matrix.
extern Mtx GXmodelView2D;

//---------------------------------------------------------------------------
// Glyph set.  Index -> character:
//   0..9  digits      10 '.'   11 ' '   12 ':'   13 'F'   14 'P'   15 'S'
// Each glyph is 8 rows, MSB = leftmost pixel.
//---------------------------------------------------------------------------
#define GLYPH_COUNT 16
#define GLYPH_W     8
#define GLYPH_H     8
#define ATLAS_W     (GLYPH_COUNT * GLYPH_W)   // 128
#define ATLAS_H     GLYPH_H                   // 8

static const unsigned char s_glyphs[GLYPH_COUNT][GLYPH_H] = {
	{ 0x3C,0x42,0x46,0x4A,0x52,0x42,0x3C,0x00 }, // 0
	{ 0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00 }, // 1
	{ 0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00 }, // 2
	{ 0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00 }, // 3
	{ 0x0C,0x14,0x24,0x44,0x7E,0x04,0x04,0x00 }, // 4
	{ 0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00 }, // 5
	{ 0x1C,0x20,0x40,0x7C,0x42,0x42,0x3C,0x00 }, // 6
	{ 0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00 }, // 7
	{ 0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00 }, // 8
	{ 0x3C,0x42,0x42,0x3E,0x02,0x04,0x38,0x00 }, // 9
	{ 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 }, // .
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, // (space)
	{ 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 }, // :
	{ 0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00 }, // F
	{ 0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00 }, // P
	{ 0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00 }, // S
};

static int glyph_index(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	switch (c) {
		case '.': return 10;
		case ':': return 12;
		case 'F': return 13;
		case 'P': return 14;
		case 'S': return 15;
		default:  return 11; // space / unknown
	}
}

//---------------------------------------------------------------------------
// Atlas texture (RGB5A3, GX 4x4-tiled).  White opaque texel = 0xFFFF,
// transparent = 0x0000; the pipeline's SRCALPHA/INVSRCALPHA blend does the
// rest, so only the lit pixels show.
//---------------------------------------------------------------------------
static u16      s_atlas[ATLAS_W * ATLAS_H] __attribute__((aligned(32)));
static GXTexObj s_tex;
static bool     s_ready = false;

void FPSOverlay_Init() {
	memset(s_atlas, 0, sizeof(s_atlas));

	const int tilesX = ATLAS_W / 4;
	for (int g = 0; g < GLYPH_COUNT; g++) {
		for (int row = 0; row < GLYPH_H; row++) {
			unsigned char bits = s_glyphs[g][row];
			for (int col = 0; col < GLYPH_W; col++) {
				if (!(bits & (0x80 >> col)))
					continue;
				int x = g * GLYPH_W + col;
				int y = row;
				int tile = (y / 4) * tilesX + (x / 4);
				int idx  = tile * 16 + (y % 4) * 4 + (x % 4);
				s_atlas[idx] = 0xFFFF;
			}
		}
	}

	DCFlushRange(s_atlas, sizeof(s_atlas));
	GX_InitTexObj(&s_tex, s_atlas, ATLAS_W, ATLAS_H, GX_TF_RGB5A3,
	              GX_CLAMP, GX_CLAMP, GX_FALSE);
	s_ready = true;
}

//---------------------------------------------------------------------------
// Rolling frame-rate measurement.
//---------------------------------------------------------------------------
static volatile float s_fps = 0.0f;

void FPSOverlay_Tick() {
	static u64 window_start = 0;
	static u32 frames = 0;

	u64 now = gettime();
	if (window_start == 0)
		window_start = now;

	frames++;

	u32 ms = ticks_to_millisecs(now - window_start);
	if (ms >= 500) {
		s_fps = (float)frames * 1000.0f / (float)ms;
		frames = 0;
		window_start = now;
	}
}

//---------------------------------------------------------------------------
// Draw.
//---------------------------------------------------------------------------
static void draw_glyph(int gi, float x, float y, float w, float h) {
	const float su = 0.5f / ATLAS_W;   // half-texel inset to stop bleed
	const float sv = 0.5f / ATLAS_H;
	float s0 = (float)(gi * GLYPH_W) / ATLAS_W + su;
	float s1 = (float)(gi * GLYPH_W + GLYPH_W) / ATLAS_W - su;
	float t0 = sv;
	float t1 = 1.0f - sv;

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x,     y);     GX_TexCoord2f32(s0, t0);
		GX_Position2f32(x,     y + h); GX_TexCoord2f32(s0, t1);
		GX_Position2f32(x + w, y + h); GX_TexCoord2f32(s1, t1);
		GX_Position2f32(x + w, y);     GX_TexCoord2f32(s1, t0);
	GX_End();
}

void FPSOverlay_Draw() {
	if (!s_ready)
		return;

	char buf[16];
	float fps = s_fps;
	if (fps < 0.0f)    fps = 0.0f;
	if (fps > 999.0f)  fps = 999.0f;
	int whole  = (int)fps;
	int tenths = (int)((fps - whole) * 10.0f + 0.5f);
	if (tenths > 9) { tenths = 0; whole++; }
	snprintf(buf, sizeof(buf), "FPS %d.%d", whole, tenths);

	// Un-scaled model-view so our coordinates are raw 640x480 ortho pixels,
	// independent of the DS screen scale/rotation the caller set up.
	GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
	GX_LoadTexObj(&s_tex, GX_TEXMAP0);

	const float scale = 3.0f;
	const float gw = GLYPH_W * scale;
	const float gh = GLYPH_H * scale;
	const float ox = 12.0f;
	const float oy = 12.0f;

	float x = ox;
	for (const char *p = buf; *p; p++, x += gw) {
		if (*p == ' ')
			continue;
		draw_glyph(glyph_index(*p), x, oy, gw, gh);
	}
}
