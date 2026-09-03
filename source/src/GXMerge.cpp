/*
    Copyright (C) 2026 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "GXMerge.h"

#include <stdio.h>
#include <string.h>
#include <malloc.h>

//------------------------------------------------------------------------------
// State
//------------------------------------------------------------------------------

static bool s_flagEnabled = false;   // requested via toggle / TESTDEFS
static bool s_active      = false;   // == s_flagEnabled && GX core, set at Init
static bool s_inited      = false;

// DS main screen is 256x192.
#define DS_W 256
#define DS_H 192

// --- 3D EFB copy ping-pong (tiled RGBA8, exactly what GX_CopyTex writes) -----
// GPU_RenderLine (which composites the 2D scene) runs before GXRender within a
// frame, so this frame's 2D was composited against the *previous* rendered 3D
// frame.  We therefore present s_slotPrevRendered on a normal frame, and
// s_slotLastRendered when GXRender was skipped this frame (the software path then
// also reuses the last rendered 3D).
static u8      *s_tiled[2]         = { NULL, NULL };
static GXTexObj s_resident[2];
static int      s_slotCur          = 0;   // slot GXRender copies into next
static int      s_slotLastRendered = -1;
static int      s_slotPrevRendered = -1;
static int      s_slotPresent      = 0;    // latched at Present
static bool     s_haveAnyCopy      = false;
static bool     s_gxRanThisFrame   = false;

// --- front-bucket 2D output (RGB555, 0x0000 == transparent) -----------------
static u16 *s_frontScreen = NULL;    // DS_W*DS_H

// --- GX textures consumed by the sandwich ----------------------------------
static u16     *s_frontTexData = NULL;   // tiled RGB5A3
static GXTexObj s_frontTex;

// --- per-frame descriptor -------------------------------------------------
static GXMergeFrame s_working;
static GXMergeFrame s_present;
static bool s_havePresent = false;

static u8  s_line3d[DS_H];           // per-scanline: 3D active & merge-handled
static u16 s_lineHofs[DS_H];         // per-scanline: BG0 X-scroll when 3D active
static u8  s_lineAlphaOver[DS_H];    // per-scanline: real-alpha blend vs opaque-key
static u8  s_lineFrontAlphaOver[DS_H]; // per-scanline: front bucket blends vs beneath
static u8  s_lineFrontEva[DS_H];     // per-scanline: BLDALPHA EVA for that blend (0..16)
static u8  s_lineBrightMode[DS_H];   // per-scanline: MASTER_BRIGHT mode (0/1/2)
static u8  s_lineBrightFactor[DS_H]; // per-scanline: MASTER_BRIGHT factor (0..16)
static bool s_frameArmed = false;
static bool s_frameMainIsTop = true;
static bool s_frameBehindContent = false;  // behind bucket has real 2D, not just backdrop
static bool s_presentBehindContent = false;

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

#define RGB15_REVERSE(col) ( 0x8000 | (((col) & 0x001F) << 10) | ((col) & 0x03E0) | (((col) & 0x7C00) >> 10) )

// Swizzle a linear 256x192 RGB555 surface into GX 4x4-tile RGB5A3, matching the
// layout main.cpp's Draw() produces for TopScreen/BottomScreen.  When keyClear is
// true, a source value of 0x0000 becomes a transparent texel (RGB5A3 alpha 0);
// every value the 2D compositor actually writes has bit 0x8000 set.
static void swizzleRGB5A3(const u16 *src, u16 *dst, bool keyClear)
{
	for (int y = 0; y < 48; y++) {
		for (int h = 0; h < 4; h++) {
			for (int x = 0; x < 64; x++) {
				for (int w = 0; w < 4; w++) {
					u16 s = src[w];
					*dst++ = (keyClear && s == 0) ? 0 : RGB15_REVERSE(s);
				}
				dst += 12;      // next tile
				src += 4;
			}
			dst -= 1020;        // next line within the 4-row tile band
		}
		dst += 1008;            // next tile row
	}
}

//------------------------------------------------------------------------------
// Toggle / lifecycle
//------------------------------------------------------------------------------

// ~480 KB of MEM1 - only claimed the first time the path is actually switched on.
static bool ensureAllocated(void)
{
	if (s_inited)
		return true;

	for (int i = 0; i < 2; i++) {
		s_tiled[i] = (u8 *)memalign(32, DS_W * DS_H * 4);
		if (!s_tiled[i])
			return false;
		memset(s_tiled[i], 0, DS_W * DS_H * 4);
		DCFlushRange(s_tiled[i], DS_W * DS_H * 4);
		GX_InitTexObj(&s_resident[i], s_tiled[i], DS_W, DS_H,
		              GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	}

	s_frontScreen  = (u16 *)memalign(32, DS_W * DS_H * 2);
	s_frontTexData = (u16 *)memalign(32, DS_W * DS_H * 2);
	if (!s_frontScreen || !s_frontTexData)
		return false;
	memset(s_frontScreen, 0, DS_W * DS_H * 2);
	memset(s_frontTexData, 0, DS_W * DS_H * 2);
	DCFlushRange(s_frontTexData, DS_W * DS_H * 2);
	GX_InitTexObj(&s_frontTex, s_frontTexData, DS_W, DS_H,
	              GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);

	s_slotCur = 0;
	s_slotLastRendered = -1;
	s_slotPrevRendered = -1;
	s_slotPresent = 0;
	s_haveAnyCopy = false;
	s_gxRanThisFrame = false;
	s_havePresent = false;
	memset(&s_working, 0, sizeof(s_working));
	memset(&s_present, 0, sizeof(s_present));

	s_inited = true;
	return true;
}

void GXMerge_SetEnabled(bool en)
{
	s_flagEnabled = en;
	if (en && !ensureAllocated()) {
		printf("\n -- GXMerge: allocation failed, staying disabled --\n");
		s_flagEnabled = false;
	}
	s_active = s_inited && s_flagEnabled;
	if (!s_active) {
		s_havePresent = false;   // stop draw_thread drawing a stale merge frame
		s_frameArmed = false;
	}
}

bool GXMerge_Enabled(void)
{
	return s_active;
}

void GXMerge_Init(void)
{
	// GX is up; real allocation is deferred to the first GXMerge_SetEnabled(true).
}

void GXMerge_Deinit(void)
{
	if (!s_inited)
		return;
	for (int i = 0; i < 2; i++) { free(s_tiled[i]); s_tiled[i] = NULL; }
	free(s_frontScreen);  s_frontScreen = NULL;
	free(s_frontTexData); s_frontTexData = NULL;
	s_inited = false;
	s_active = false;
}

//------------------------------------------------------------------------------
// 3D texture ping-pong (core thread, GXRender.cpp, under vidmutex)
//------------------------------------------------------------------------------

void *GXMerge_CopyDst(void)
{
	return s_tiled[s_slotCur];
}

void GXMerge_NoteGXRenderCopied(void)
{
	DCFlushRange(s_tiled[s_slotCur], DS_W * DS_H * 4);
	s_slotPrevRendered = s_slotLastRendered;
	s_slotLastRendered = s_slotCur;
	s_slotCur ^= 1;                 // next frame renders into the other slot
	s_haveAnyCopy = true;
	s_gxRanThisFrame = true;
}

void GXMerge_NoteGXRenderSkipped(void)
{
	// No flip: the previously rendered slot stays current, matching the software
	// path which also reuses stale gfx3d_convertedScreen on a skipped 3D frame.
}

//------------------------------------------------------------------------------
// Per-frame compositor hooks (core thread, GPU_RenderLine, MAIN engine)
//------------------------------------------------------------------------------

void GXMerge_BeginFrame(bool mainIsTop)
{
	memset(s_line3d, 0, sizeof(s_line3d));
	memset(s_lineHofs, 0, sizeof(s_lineHofs));
	memset(s_lineAlphaOver, 0, sizeof(s_lineAlphaOver));
	memset(s_lineFrontAlphaOver, 0, sizeof(s_lineFrontAlphaOver));
	memset(s_lineFrontEva, 0, sizeof(s_lineFrontEva));
	memset(s_lineBrightMode, 0, sizeof(s_lineBrightMode));
	memset(s_lineBrightFactor, 0, sizeof(s_lineBrightFactor));
	memset(&s_working, 0, sizeof(s_working));
	s_frameArmed = s_active && s_haveAnyCopy;
	s_frameMainIsTop = mainIsTop;
	s_gxRanThisFrame = false;
	s_frameBehindContent = false;

	// Phase 2: bands are now decided per scanline (GXMerge_LineMergeable in
	// GPU.cpp), so within one armed frame some lines can be merge-bands while
	// others fall back to the legacy path and never call GXMerge_FrontLine().
	// Clear the whole front-bucket surface up front so an untouched (fallback)
	// row reads back fully transparent instead of carrying over a stale row
	// from a previous frame where that same scanline *was* a merge-band.
	if (s_frontScreen)
		memset(s_frontScreen, 0, DS_W * DS_H * 2);
}

void GXMerge_RecordLine(int l, bool behindContent, u16 hofs, bool alphaOver,
                        bool frontAlphaOver, u8 frontEva,
                        u8 brightMode, u8 brightFactor)
{
	if (l >= 0 && l < DS_H) {
		s_line3d[l] = 1;
		s_lineHofs[l] = hofs & 0x1FF;
		s_lineAlphaOver[l] = alphaOver ? 1 : 0;
		s_lineFrontAlphaOver[l] = frontAlphaOver ? 1 : 0;
		s_lineFrontEva[l] = frontEva > 16 ? 16 : frontEva;
		// Only modes 1 (bright up) and 2 (bright down) do anything; a 0 factor
		// is likewise a no-op, so normalise both to "no brightness pass".
		if ((brightMode == 1 || brightMode == 2) && brightFactor != 0) {
			s_lineBrightMode[l]   = brightMode;
			s_lineBrightFactor[l] = brightFactor > 16 ? 16 : brightFactor;
		} else {
			s_lineBrightMode[l]   = 0;
			s_lineBrightFactor[l] = 0;
		}
	}
	if (behindContent)
		s_frameBehindContent = true;
}

bool GXMerge_LineWasMerged(int l)
{
	return l >= 0 && l < DS_H && s_line3d[l] != 0;
}

void GXMerge_Disarm(void)
{
	s_frameArmed = false;
}

bool GXMerge_FrameArmed(void)
{
	return s_active && s_frameArmed;
}

void GXMerge_EndFrame(void)
{
	s_working.nBands = 0;
	s_working.mainIsTop = s_frameMainIsTop;
	s_working.valid = s_frameArmed;

	if (!s_frameArmed)
		return;

	// Coalesce marked scanlines into contiguous bands, additionally splitting a
	// run wherever BG0 HOFS or the alpha-over decision changes - a mid-frame
	// HBlank-IRQ rewrite of the 3D layer's scroll or of BLDCNT needs its own
	// band even though the line stays 3D-active, since the whole band is drawn
	// with one texture sub-rect (GXMerge_HofsSegment) and one blend mode.
	int y = 0;
	while (y < DS_H) {
		if (!s_line3d[y]) { y++; continue; }
		int start = y;
		u16  hofs           = s_lineHofs[y];
		bool alphaOver      = s_lineAlphaOver[y] != 0;
		bool frontAlphaOver = s_lineFrontAlphaOver[y] != 0;
		u8   frontEva       = s_lineFrontEva[y];
		u8   brightMode     = s_lineBrightMode[y];
		u8   brightFactor   = s_lineBrightFactor[y];
		y++;
		while (y < DS_H && s_line3d[y] && s_lineHofs[y] == hofs &&
		       (s_lineAlphaOver[y] != 0) == alphaOver &&
		       (s_lineFrontAlphaOver[y] != 0) == frontAlphaOver &&
		       s_lineFrontEva[y] == frontEva &&
		       s_lineBrightMode[y] == brightMode &&
		       s_lineBrightFactor[y] == brightFactor) y++;
		if (s_working.nBands >= GXMERGE_MAX_BANDS) {
			s_working.valid = false;
			return;
		}
		s_working.bands[s_working.nBands].yStart         = (u8)start;
		s_working.bands[s_working.nBands].yEnd           = (u8)(y - 1);
		s_working.bands[s_working.nBands].hofs           = hofs;
		s_working.bands[s_working.nBands].alphaOver      = alphaOver;
		s_working.bands[s_working.nBands].frontAlphaOver = frontAlphaOver;
		s_working.bands[s_working.nBands].frontEva       = frontEva;
		s_working.bands[s_working.nBands].brightMode     = brightMode;
		s_working.bands[s_working.nBands].brightFactor   = brightFactor;
		s_working.nBands++;
	}
}

//------------------------------------------------------------------------------
// Front-bucket line buffer (core thread, GPU_RenderLine_layer)
//------------------------------------------------------------------------------

u8 *GXMerge_FrontLine(int l)
{
	u16 *line = s_frontScreen + l * DS_W;
	memset(line, 0, DS_W * 2);
	return (u8 *)line;
}

//------------------------------------------------------------------------------
// Present snapshot (core thread, Draw(), under vidmutex)
//------------------------------------------------------------------------------

#ifdef GXMERGE_DEBUG
static unsigned s_dbgFrame = 0;
#endif

void GXMerge_Present(void)
{

	GXMerge_EndFrame();   // coalesce recorded scanlines into bands

	int slot = s_gxRanThisFrame ? s_slotPrevRendered : s_slotLastRendered;

#ifdef GXMERGE_DEBUG
	if ((s_dbgFrame++ % 120) == 0) {
		int cov = 0;
		for (int b = 0; b < s_working.nBands; b++)
			cov += s_working.bands[b].yEnd - s_working.bands[b].yStart + 1;
		printf("[GXMerge] f%u armed=%d valid=%d bands=%d cov=%d slot=%d\n",
		       s_dbgFrame, (int)s_frameArmed, (int)s_working.valid,
		       s_working.nBands, cov, slot);
	}
#endif

	if (!s_working.valid || slot < 0) {
		s_havePresent = false;
		return;
	}

	swizzleRGB5A3(s_frontScreen, s_frontTexData, true);
	DCFlushRange(s_frontTexData, DS_W * DS_H * 2);

	s_present = s_working;
	s_slotPresent = slot;
	s_presentBehindContent = s_frameBehindContent;
	s_havePresent = true;
}

bool GXMerge_HasPresentFrame(void)
{
	return s_havePresent;
}

//------------------------------------------------------------------------------
// The sandwich (draw_thread, under vidmutex)
//------------------------------------------------------------------------------

// Reduce a BG0 HOFS value to the single on-screen x-run it actually reveals of the
// 3D texture.  Matches GPU.cpp's legacy per-pixel loop exactly:
//   q = (k + hofs) & 0x1FF;  if (q > 255) skip;   for screen column k in 0..255.
// The 3D texture only ever holds valid data in its native 256 columns (x=0..255);
// HOFS conceptually slides a 256-wide *window* across a 512-wide space where the
// upper half is permanently empty, so at most one contiguous run of screen columns
// ever lands back inside the textured half - never a wraparound pair, since the
// window and the data are both exactly half of that 512-wide space (two equal-
// length arcs on a circle overlap in one contiguous piece, or not at all).
// Returns false (no segment) when the run is empty (hofs == 256: window and data
// are exact complements) - the caller then skips the 3D draw for that band and the
// "behind" draw already underneath is what shows, which is correct.
static bool GXMerge_HofsSegment(u16 hofs, int *screenX0, int *count, int *texX0)
{
	hofs &= 0x1FF;
	if (hofs <= 255) {
		// k in [0, 255-hofs] => q = k+hofs in [hofs, 255]
		*screenX0 = 0;
		*texX0    = hofs;
		*count    = 256 - hofs;
	} else {
		// k in [512-hofs, 255] => q = k+hofs-512 in [0, hofs-256)
		*screenX0 = 512 - hofs;
		*texX0    = 0;
		*count    = hofs - 256;
	}
	return *count > 0;
}

// Emit one textured quad in the current model space.
static void quad(f32 x0, f32 y0, f32 x1, f32 y1,
                 f32 u0, f32 v0, f32 u1, f32 v1)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0); GX_TexCoord2f32(u0, v0);
		GX_Position2f32(x0, y1); GX_TexCoord2f32(u0, v1);
		GX_Position2f32(x1, y1); GX_TexCoord2f32(u1, v1);
		GX_Position2f32(x1, y0); GX_TexCoord2f32(u1, v0);
	GX_End();
}

void GXMerge_DrawMainScreen(f32 x0, f32 y0, f32 w, f32 h)
{
	if (!s_havePresent)
		return;

	GX_InvalidateTexAll();

	// painter's order; no depth interaction between the three sandwich layers
	GX_SetZMode(GX_ENABLE, GX_ALWAYS, GX_FALSE);

	// --- draw 2: 3D layer ---------------------------------------------------
	// Two per-band draw modes (GXMergeBand::alphaOver, decided per line by
	// GPU.cpp's GXMerge_LineMergeable and coalesced same as HOFS):
	//  - MB_OPAQUE_MASKED (default): draw the 3D texel fully opaque wherever
	//    it drew at all, alpha-keyed discard elsewhere. Matches real
	//    hardware's _master_setFinal3dColor "not blend2-eligible" branch,
	//    which always draws 3D fully opaque regardless of its own alpha.
	//    Skip the discard entirely when there is no real 2D behind the 3D
	//    layer to reveal (matches legacy and avoids eating 3D pixels the GX
	//    texture path mis-flags as transparent).
	//  - MB_ALPHA_OVER: every layer that could be "under" 3D on this band
	//    shares one blend2 eligibility bit, and it's set - so real hardware
	//    always blends 3D against it using the 3D polygon's OWN per-pixel
	//    alpha, regardless of BLDCNT's blend mode. GX_REPLACE (the ambient
	//    TEV state here) already passes the resident texture's real alpha
	//    straight through as the fragment alpha (Set3DVideoSettings no
	//    longer forces it to a binary coverage mask via GX_SetDstAlpha), so
	//    a plain GX_BL_SRCALPHA/INVSRCALPHA blend reproduces this exactly:
	//    alpha 0 (nothing drew) leaves the behind pixel untouched, alpha 255
	//    (opaque poly) fully replaces it, and anything between blends
	//    proportionally - no alpha-compare discard needed at all.
	GX_LoadTexObj(&s_resident[s_slotPresent], GX_TEXMAP0);

	for (int b = 0; b < s_present.nBands; b++) {
		int sx0, count, tx0;
		if (!GXMerge_HofsSegment(s_present.bands[b].hofs, &sx0, &count, &tx0))
			continue;   // BG0 scrolled fully out of view this band - nothing to draw

		if (s_present.bands[b].alphaOver) {
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
		} else {
			GX_SetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
			if (s_presentBehindContent)
				GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_OR, GX_NEVER, 0);
			else
				GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
		}

		f32 ys = s_present.bands[b].yStart;
		f32 ye = s_present.bands[b].yEnd + 1;
		f32 qy0 = y0 + h * (ys / (f32)DS_H);
		f32 qy1 = y0 + h * (ye / (f32)DS_H);
		f32 tv0 = ys / (f32)DS_H;
		f32 tv1 = ye / (f32)DS_H;
		f32 qx0 = x0 + w * (sx0 / (f32)DS_W);
		f32 qx1 = x0 + w * ((sx0 + count) / (f32)DS_W);
		f32 tu0 = tx0 / (f32)DS_W;
		f32 tu1 = (tx0 + count) / (f32)DS_W;
		quad(qx0, qy0, qx1, qy1, tu0, tv0, tu1, tv1);
	}

	// --- draw 3: front-bucket 2D, per band -------------------------------
	// All front content lives on 3D-active (banded) scanlines - a non-banded
	// line's full 2D composite is already in the behind texture and its front
	// line buffer is cleared to 0x0000 - so a per-band loop covers everything.
	//
	//  - default band: opaque, alpha-keyed (front texel 0x0000 == no pixel).
	//  - frontAlphaOver band: the front texel is a single translucent 2D layer
	//    (GXMerge_LineMergeable vetted the shape) that real hardware blends
	//    against what is beneath it by the global BLDALPHA EVA constant. The
	//    behind + 3D draws already put "beneath" in the EFB, so blend the front
	//    texel over it with a constant fraction: TEV forces fragment alpha to
	//    TEXA * (EVA/16) via a KONST modulate, then GX_BL_SRCALPHA/INVSRCALPHA
	//    gives  EFB = texC*(EVA/16) + EFB*(1 - EVA/16).  (EVB is thereby forced
	//    to 16-EVA; exact EVA/EVB is a Phase 3 refinement.)  A 0x0000 texel has
	//    TEXA 0 => fragment alpha 0 => the EFB pixel is left untouched, so no
	//    alpha-compare discard is needed.
	GX_LoadTexObj(&s_frontTex, GX_TEXMAP0);
	bool frontTevCustom = false;

	for (int b = 0; b < s_present.nBands; b++) {
		const GXMergeBand *bd = &s_present.bands[b];
		f32 ys = bd->yStart;
		f32 ye = bd->yEnd + 1;
		f32 qy0 = y0 + h * (ys / (f32)DS_H);
		f32 qy1 = y0 + h * (ye / (f32)DS_H);
		f32 tv0 = ys / (f32)DS_H;
		f32 tv1 = ye / (f32)DS_H;

		if (bd->frontAlphaOver) {
			u8 eva8 = (u8)((bd->frontEva * 255 + 8) / 16);
			GXColor k = { 0, 0, 0, eva8 };
			GX_SetTevKColor(GX_KCOLOR0, k);
			GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
			// out.rgb = TEXC ;  out.a = (1-KONST)*0 + KONST*TEXA = TEXA * (EVA/16)
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
			GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
			GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
			frontTevCustom = true;
		} else {
			if (frontTevCustom) {
				GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
				GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
				frontTevCustom = false;
			}
			GX_SetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
			GX_SetAlphaCompare(GX_GEQUAL, 128, GX_AOP_OR, GX_NEVER, 0);
		}
		quad(x0, qy0, x0 + w, qy1, 0.0f, tv0, 1.0f, tv1);
	}

	if (frontTevCustom) {
		GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	}

	// --- draw 4: per-band MASTER_BRIGHT (GX pass over the merged EFB) --------
	// The CPU GPU_RenderLine_MasterBrightness pass is suppressed for merged
	// scanlines (GXMerge_LineWasMerged), so the whole merged composite - behind
	// bucket, 3D, front - is brightened here instead, per band, matching the DS
	// tables (GPU.cpp): mode 1 is  c' = c + (31-c)*f/16  == blend toward white
	// with fraction f/16; mode 2 is  c' = c - c*f/16  == blend toward black with
	// fraction f/16.  Both are one constant-colour full-width quad over the band
	// with GX_BL_SRCALPHA/INVSRCALPHA and vertex alpha = f/16.
	bool anyBright = false;
	for (int b = 0; b < s_present.nBands; b++)
		if (s_present.bands[b].brightMode) { anyBright = true; break; }

	if (anyBright) {
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
		GX_SetZMode(GX_ENABLE, GX_ALWAYS, GX_FALSE);

		for (int b = 0; b < s_present.nBands; b++) {
			const GXMergeBand *bd = &s_present.bands[b];
			if (!bd->brightMode)
				continue;
			u8 f = bd->brightFactor > 16 ? 16 : bd->brightFactor;
			u8 a = (u8)((f * 255 + 8) / 16);
			u8 c = (bd->brightMode == 1) ? 255 : 0;   // 1: to white, 2: to black
			f32 ys = bd->yStart;
			f32 ye = bd->yEnd + 1;
			f32 qy0 = y0 + h * (ys / (f32)DS_H);
			f32 qy1 = y0 + h * (ye / (f32)DS_H);
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(x0,     qy0); GX_Color4u8(c, c, c, a);
				GX_Position2f32(x0,     qy1); GX_Color4u8(c, c, c, a);
				GX_Position2f32(x0 + w, qy1); GX_Color4u8(c, c, c, a);
				GX_Position2f32(x0 + w, qy0); GX_Color4u8(c, c, c, a);
			GX_End();
		}

		// back to the textured-quad vertex format + TEV draw_thread expects
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	}

	// restore state the rest of draw_thread expects
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
}

// Debug: solid status square in the MAIN screen's top-left corner.
// green  = the hardware sandwich drew this frame
// red    = merge enabled but this frame fell back to the legacy path
void GXMerge_DrawStatusMarker(f32 x0, f32 y0, f32 w, f32 h)
{
#ifdef GXMERGE_DEBUG
	if (!s_active)
		return;
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GX_SetZMode(GX_ENABLE, GX_ALWAYS, GX_FALSE);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	u8 r = s_havePresent ? 0 : 255;
	u8 g = s_havePresent ? 255 : 0;
	f32 mx = x0 + w * 0.06f, my = y0 + h * 0.06f;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0);      GX_Color4u8(r, g, 0, 255);
		GX_Position2f32(x0, my);      GX_Color4u8(r, g, 0, 255);
		GX_Position2f32(mx, my);      GX_Color4u8(r, g, 0, 255);
		GX_Position2f32(mx, y0);      GX_Color4u8(r, g, 0, 255);
	GX_End();

	// Fallback-reason bar just right of the marker: one coloured cell per reason
	// code (see g_gxmergeFailReason in GPU.cpp).  Only meaningful when red above.
	extern int g_gxmergeFailReason;
	static const u8 palR[8] = {0,255,255,255,255,  0,  0,255};
	static const u8 palG[8] = {0,  0,128,255,  0,255,255,255};
	static const u8 palB[8] = {0,  0,  0,  0,255,255,  0,255};
	int rc = g_gxmergeFailReason & 7;
	f32 bx0 = mx + w * 0.02f, bx1 = bx0 + w * 0.05f;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(bx0, y0); GX_Color4u8(palR[rc], palG[rc], palB[rc], 255);
		GX_Position2f32(bx0, my); GX_Color4u8(palR[rc], palG[rc], palB[rc], 255);
		GX_Position2f32(bx1, my); GX_Color4u8(palR[rc], palG[rc], palB[rc], 255);
		GX_Position2f32(bx1, y0); GX_Color4u8(palR[rc], palG[rc], palB[rc], 255);
	GX_End();
	// restore the textured-quad vertex format draw_thread uses
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	(void)r; (void)g;
#else
	(void)x0; (void)y0; (void)w; (void)h;
#endif
}

//------------------------------------------------------------------------------
// Phase 4 stub
//------------------------------------------------------------------------------

void GXMerge_MaterializeConverted(void)
{
	// Filled in Phase 4.  Until then GXRender.cpp keeps its own de-swizzle loop.
}
