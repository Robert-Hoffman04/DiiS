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

#include "GX2DBG.h"
#include "GXDirty.h"
#include "GPU.h"
#include "MMU.h"

#include <malloc.h>
#include <string.h>

//------------------------------------------------------------------------------
// Per-engine (0 = MAIN, 1 = SUB), per-layer (0..3) baked "resolved plane".
//------------------------------------------------------------------------------
struct Layer {
	u16     *plane;        // swizzled RGB5A3, w*h*2 bytes, 32-aligned
	u32      bytes;        // current allocation
	u16      w, h;         // texture size (= content + 2*pad)
	u16      cw, ch;       // content size (the BG's own dimensions)
	u8       pad;          // transparent apron per side (0, or GX2DBG_AFF_MARGIN)
	GXTexObj obj;
	u32      builtPalGen;
	u32      builtFullGen;
	bool     ready;        // baked and valid for this frame
	bool     affine;       // BGType_Affine (else text)
};

static Layer s_layer[2][4];
static bool  s_enabled = false;
static u32   s_totalBytes = 0;   // across both engines

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// RGB5A3 (16bpp) GX texture is 4x4-texel tiles, tiles row-major.
static inline u32 tex16_ofs(u32 x, u32 y, u32 W)
{
	return ((y >> 2) * (W >> 2) + (x >> 2)) * 16 + ((y & 3) << 2) + (x & 3);
}

static void freeLayer(int e, int n)
{
	if (s_layer[e][n].plane) {
		free(s_layer[e][n].plane);
		s_totalBytes -= s_layer[e][n].bytes;
	}
	memset(&s_layer[e][n], 0, sizeof(Layer));
}

// (re)allocate layer's plane buffer for a w*h texture; false if that would blow
// the MEM1 budget (the layer then stays on the CPU path).
static bool ensureLayer(int e, int n, u16 cw, u16 ch, u8 margin)
{
	Layer *L = &s_layer[e][n];
	const u16 w = (u16)(cw + 2 * margin), h = (u16)(ch + 2 * margin);
	u32 need = (u32)w * h * 2;
	if (L->plane && L->bytes == need && L->cw == cw && L->ch == ch && L->pad == margin)
		return true;

	if (s_totalBytes - L->bytes + need > GX2DBG_BUDGET)
		return false;

	if (L->plane) { free(L->plane); s_totalBytes -= L->bytes; }

	L->plane = (u16 *)memalign(32, need);
	if (!L->plane) { L->bytes = 0; return false; }
	L->bytes = need;
	s_totalBytes += need;
	L->w = w;   L->h = h;
	L->cw = cw; L->ch = ch;
	L->pad = margin;
	L->builtFullGen = g_gxDirty.fullGen - 1;   // force a bake
	L->builtPalGen  = g_gxDirty.palGen - 1;
	return true;
}

// Any 16 KB LCDC page backing [base, base+len) marked dirty?
static bool rangeDirty(u32 base, u32 len)
{
	u32 last = base + len - 1;
	for (u32 a = base; ; a += 0x4000) {
		u32 pg = vram_arm9_map[(a >> 14) & (VRAM_ARM9_PAGES - 1)];
		if (g_gxDirty.lcdPageDirty[pg & (GXD_LCD_PAGES - 1)])
			return true;
		if (a >= (last & ~0x3FFFu)) break;
	}
	return false;
}

// Full re-bake of one layer's plane from current VRAM/palette.
static void bakeLayer(GPU *gpu, int e, int n)
{
	Layer *L = &s_layer[e][n];
	const u16 W = L->w, H = L->h;         // texture (padded) size
	const u16 CW = L->cw, CH = L->ch;     // content size
	const u32 M = L->pad;
	u16 *plane = L->plane;

	const bool affine = L->affine;
	if (M) memset(plane, 0, L->bytes);   // transparent apron
	u16 cell[64];
	for (u32 cy = 0; cy < (u32)(CH >> 3); cy++) {
		for (u32 cx = 0; cx < (u32)(CW >> 3); cx++) {
			if (affine) {
				if (!GPU_ResolveAffineTile8x8(gpu, (u8)n, cx, cy, cell))
					memset(cell, 0, sizeof cell);
			} else {
				GPU_ResolveTextTile8x8(gpu, (u8)n, cx, cy, cell);
			}
			const u32 px0 = (cx << 3) + M, py0 = (cy << 3) + M;
			for (u32 r = 0; r < 8; r++)
				for (u32 c = 0; c < 8; c++)
					plane[tex16_ofs(px0 + c, py0 + r, W)] = cell[r * 8 + c];
		}
	}

	DCFlushRange(plane, L->bytes);
	if (affine) {
		// Point-sample to match the DS affine renderer - GX's default GX_LINEAR
		// bilinearly blends across the wrap seam / between minified minimap
		// texels, which reads as a translucent grid/ghost over the minimap.
		// Overflow bit (PaletteSet_Wrap, bit 13) set -> GX_REPEAT the exact
		// plane; clear -> M-texel transparent apron + GX_CLAMP so outside the
		// BG's addressable area samples transparent (DS draws nothing there).
		const u8 wm = M ? GX_CLAMP : GX_REPEAT;
		GX_InitTexObj(&L->obj, plane, W, H, GX_TF_RGB5A3, wm, wm, GX_FALSE);
		GX_InitTexObjFilterMode(&L->obj, GX_NEAR, GX_NEAR);
	} else {
		GX_InitTexObj(&L->obj, plane, W, H, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
	}
	L->builtPalGen  = g_gxDirty.palGen;
	L->builtFullGen = g_gxDirty.fullGen;
}

//------------------------------------------------------------------------------
// Public
//------------------------------------------------------------------------------

static void objFreeAll(int e);   // fwd

void GX2DBG_SetEnabled(bool on)
{
	if (on == s_enabled) return;
	s_enabled = on;
	if (!on)
		for (int e = 0; e < 2; e++) {
			for (int n = 0; n < 4; n++) freeLayer(e, n);
			objFreeAll(e);
		}
}

bool GX2DBG_Enabled(void) { return s_enabled; }

void GX2DBG_Reset(void)
{
	for (int e = 0; e < 2; e++) {
		for (int n = 0; n < 4; n++) freeLayer(e, n);
		objFreeAll(e);
	}
	s_enabled = false;
}

// Set by the recorder (GPU_RenderLine_layer) whenever a scanline passes every
// frame-invariant gate (armed, no window, no live blend/brighten, sprites GX-able)
// - i.e. the only thing that could still stop it recording is an unbaked layer.
// GX2DBG_FrameUpdate consults last frame's value to decide whether baking the BG
// planes is worth it: in real 3D gameplay (pervasive live blend) it never trips,
// so the per-frame dirty-cell bake - pure overhead there - is skipped entirely.
static bool s_wouldRec[2]     = { false, false };
static u32  s_probeCtr[2]     = { 0, 0 };
static bool s_bakeGo[2]       = { false, false };   // FrameUpdate's decision, for ObjFrameUpdate
void GX2DBG_NoteWouldRecord(int eng) { if ((unsigned)eng < 2) s_wouldRec[eng] = true; }

// Called at each engine's line 0 (core thread).  eng = gpu->core.
void GX2DBG_FrameUpdate(GPU *gpu)
{
	if (!gpu) return;
	const int e = (gpu->core == 0) ? 0 : 1;
	for (int n = 0; n < 4; n++) s_layer[e][n].ready = false;
	if (!s_enabled) return;

	// Skip the bake unless the recorder wanted a layer last frame (or a periodic
	// re-probe frame - the recorder's would-record test doesn't depend on baked
	// state, so a stale "no" self-corrects within one frame once conditions change).
	const bool probe = (s_probeCtr[e]++ & 127u) == 0;
	const bool go = s_wouldRec[e] || probe;
	s_wouldRec[e] = false;
	s_bakeGo[e] = go;
	if (!go) return;

	const bool extPal = gpu->dispCnt().ExBGxPalette_Enable;
	const bool bg0is3d = (e == 0) && gpu->dispCnt().BG0_3D;   // SUB has no 3D

	for (int n = 0; n < 4; n++) {
		const BGType bt = (BGType)gpu->BGTypes[n];
		const bool isText   = bt == BGType_Text;
		const bool isAffine = bt == BGType_Affine || bt == BGType_AffineExt_256x16 ||
		                      bt == BGType_AffineExt_256x1 || bt == BGType_AffineExt_Direct ||
		                      bt == BGType_Large8bpp;
		if (!gpu->LayersEnable[n] || (!isText && !isAffine) ||
		    (n == 0 && bg0is3d)) { freeLayer(e, n); continue; }

		const u16 w = gpu->BGSize[n][0];
		const u16 h = gpu->BGSize[n][1];
		// affine BG with the overflow bit clear gets a transparent apron so
		// out-of-bounds samples read transparent (see GX2DBG_AFF_MARGIN).
		const u8 margin = (isAffine && !gpu->bgcnt(n).PaletteSet_Wrap)
		                  ? GX2DBG_AFF_MARGIN : 0;
		if (!ensureLayer(e, n, w, h, margin))  // over budget -> CPU path
			continue;

		if (isText && extPal && gpu->bgcnt(n).Palette_256 &&
		    !MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[n]]) {
			freeLayer(e, n);
			continue;
		}

		Layer *L = &s_layer[e][n];
		L->affine = isAffine;
		const bool bitmap = bt == BGType_AffineExt_256x1 || bt == BGType_Large8bpp ||
		                    bt == BGType_AffineExt_Direct;
		// On a re-probe frame the per-page dirty flags may have been cleared by
		// GX2DBG_EndFrame during the skip window, so force a full rebake.
		bool dirty = probe ||
		             (L->builtFullGen != g_gxDirty.fullGen) ||
		             (L->builtPalGen  != g_gxDirty.palGen);
		if (!dirty) {
			if (bitmap) {
				const u32 src = (bt == BGType_Large8bpp) ? gpu->BG_bmp_large_ram[n]
				                                         : gpu->BG_bmp_ram[n];
				dirty = rangeDirty(src, (u32)w * h * (bt == BGType_AffineExt_Direct ? 2 : 1));
			} else {
				dirty = rangeDirty(gpu->BG_map_ram[n], isAffine ? 0x4000 : 0x2000) ||
				        rangeDirty(gpu->BG_tile_ram[n], 0x10000);
			}
		}
		if (dirty)
			bakeLayer(gpu, e, n);
		L->ready = true;
	}
}

// Clear the per-page dirty flags once both engines have consumed them
// (GXMerge_Present, core thread).
void GX2DBG_EndFrame(void)
{
	memset(g_gxDirty.lcdPageDirty, 0, sizeof(g_gxDirty.lcdPageDirty));
}

bool GX2DBG_LayerReady(int eng, int num)
{
	return (unsigned)eng < 2 && (unsigned)num < 4 && s_layer[eng][num].ready;
}

GXTexObj *GX2DBG_LayerTex(int eng, int num, u16 *w, u16 *h)
{
	if ((unsigned)eng >= 2 || (unsigned)num >= 4 || !s_layer[eng][num].ready)
		return NULL;
	if (w) *w = s_layer[eng][num].w;
	if (h) *h = s_layer[eng][num].h;
	return &s_layer[eng][num].obj;
}

//------------------------------------------------------------------------------
// Step 5.2 - non-affine tiled sprites as GX quads
//------------------------------------------------------------------------------
#define GX2OBJ_TEXCAP (72 * 72)   // 64x64 sprite + 2px margin each side, padded

struct ObjS {
	u16     *tex;
	u32      bytes;
	u16      tw, th;        // padded texture dims
	GXTexObj obj;
	s16      x, y;          // field rect origin (screen)
	u16      fx, fy;        // field rect size (screen footprint)
	float    uv[8];         // quad corner UVs: TL,BL,BR,TR
	u8       prio, semi;
	u32      key;
};
static ObjS  s_obj[2][128];
static int   s_objN[2]       = { 0, 0 };
static bool  s_objGXable[2]  = { false, false };
static u32   s_objBuiltEp[2] = { 0, 0 };
static u32   s_objBytes      = 0;
static u8    s_objEva[2]     = { 16, 16 };   // BLDALPHA EVA for semi-transparent sprites
// latched for the draw thread (Present copies s_obj -> here)
static ObjS  s_objP[2][128];
static int   s_objPN[2]      = { 0, 0 };
static bool  s_objPGXable[2] = { false, false };
static u8    s_objEvaP[2]    = { 16, 16 };

static void swizzle16(const u16 *src, u16 *dst, int w, int h)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			dst[tex16_ofs(x, y, w)] = src[y * w + x];
}

static void objFreeAll(int e)
{
	for (int i = 0; i < 128; i++) {
		if (s_obj[e][i].tex) { free(s_obj[e][i].tex); s_objBytes -= s_obj[e][i].bytes; }
		memset(&s_obj[e][i], 0, sizeof(ObjS));
	}
	s_objN[e] = 0;
}

void GX2DBG_ObjFrameUpdate(GPU *gpu)
{
	if (!gpu) return;
	const int e = (gpu->core == 0) ? 0 : 1;
	s_objN[e] = 0;
	s_objGXable[e] = true;
	if (!s_enabled) return;
	if (!gpu->LayersEnable[4]) return;   // no OBJ layer -> trivially GX-able

	// Same bake-worth-it gate as GX2DBG_FrameUpdate: if the recorder took nothing
	// last frame, skip the 128-entry OAM scan + sprite bake (pure overhead in a
	// scene the recorder can't use).  s_objGXable stays false so the recorder
	// bails, exactly as it would once it reached the OBJ check.
	if (!s_bakeGo[e]) { s_objGXable[e] = false; return; }

	// Frame-level early-out.  A BLDCNT colour effect no longer disqualifies the
	// whole engine (§5.1a blend): the sprite pass draws opaque quads, which is
	// still correct as long as OBJ itself isn't a blend target - if OBJ is a 1st
	// or 2nd target the per-pixel blend against the layer beneath is genuinely
	// per-pixel, so fall back.  Windows still disqualify.  Semi-transparent
	// sprites are caught per-entry in the scan loop below.
	const u16 bld = gpu->BLDCNT;
	const bool objInBlend = ((bld >> 6) & 3) != 0 && (bld & 0x1010) != 0;
	if (objInBlend ||
	    gpu->WIN0_ENABLED || gpu->WIN1_ENABLED || gpu->WINOBJ_ENABLED) {
		s_objGXable[e] = false;
#ifdef DESMUME_BENCH
		{ extern u32 g_gx2objDis[2][4];
		  g_gx2objDis[e][objInBlend ? 0 : 1]++;
		  if (gpu->WINOBJ_ENABLED) g_gx2objDis[e][2]++; }
#endif
		return;
	}

	s_objEva[e] = gpu->BLDALPHA_EVA > 16 ? 16 : gpu->BLDALPHA_EVA;

	const u32 ep = g_gxDirty.oamGen ^ (g_gxDirty.palGen << 1) ^ (g_gxDirty.fullGen << 2);
	const bool rebakeAll = (ep != s_objBuiltEp[e]);

	static u16 tmp[GX2OBJ_TEXCAP];
	for (int i = 0; i < 128; i++) {
		int fx, fy, ox, oy, tw, th; u8 op, os; u32 key; float uv[8];
		const int r = GPU_ResolveObjSprite(gpu, i, tmp, GX2OBJ_TEXCAP,
		                                   &fx, &fy, &ox, &oy, &op, &os, &key, uv, &tw, &th);
		if (r == 0) continue;
		if (r < 0)  { s_objGXable[e] = false; return; }        // -> CPU sprite path
		// Semi-transparent sprite (OBJ mode 1): drawn in a constant-EVA/16 blend
		// against the EFB by the replay (approximates the per-pixel blend2[under]
		// gate as "always blend" - fine for HUD sprites over 2nd-target content).
		// NOTE: 5.2 draws sprites last (over all BG bands).  This is only correct
		// when no opaque BG sits in front of a sprite; a per-priority interleave
		// is 5.2-2.  Both bench scenes' sprites are HUD (front), so allow all.

		ObjS *S = &s_obj[e][s_objN[e]++];
		S->x = (s16)ox; S->y = (s16)oy;
		S->fx = (u16)fx; S->fy = (u16)fy;
		S->prio = op; S->semi = os;
		memcpy(S->uv, uv, sizeof S->uv);

		const u32 need = (u32)tw * th * 2;
		if (rebakeAll || S->key != key || S->tw != tw || S->th != th || !S->tex) {
			if (!S->tex || S->bytes != need) {
				if (S->tex) { free(S->tex); s_objBytes -= S->bytes; }
				if (s_totalBytes + s_objBytes + need > GX2DBG_BUDGET) {
					S->tex = NULL; S->bytes = 0;
					s_objGXable[e] = false;
#ifdef DESMUME_BENCH
					{ extern u32 g_gx2objDis[2][4]; g_gx2objDis[e][3]++; }
#endif
					return;
				}
				S->tex = (u16 *)memalign(32, need);
				if (!S->tex) { S->bytes = 0; s_objGXable[e] = false;
#ifdef DESMUME_BENCH
					{ extern u32 g_gx2objDis[2][4]; g_gx2objDis[e][3]++; }
#endif
					return; }
				S->bytes = need; s_objBytes += need;
			}
			swizzle16(tmp, S->tex, tw, th);
			DCFlushRange(S->tex, need);
			GX_InitTexObj(&S->obj, S->tex, tw, th, GX_TF_RGB5A3,
			              GX_CLAMP, GX_CLAMP, GX_FALSE);
			S->key = key; S->tw = (u16)tw; S->th = (u16)th;
		}
	}
	s_objBuiltEp[e] = ep;
}

// recorder gate (core thread, this frame's freshly-built list)
bool GX2DBG_ObjGXable(int eng)  { return (unsigned)eng < 2 && s_objGXable[eng]; }

// core thread, GXMerge_Present: latch the sprite list for the draw thread.
void GX2DBG_ObjLatch(void)
{
	for (int e = 0; e < 2; e++) {
		memcpy(s_objP[e], s_obj[e], sizeof(s_objP[e]));
		s_objPN[e]      = s_objN[e];
		s_objPGXable[e] = s_objGXable[e];
		s_objEvaP[e]    = s_objEva[e];
	}
}

int  GX2DBG_ObjCount(int eng)   { return (unsigned)eng < 2 ? s_objPN[eng] : 0; }
u8   GX2DBG_ObjEva(int eng)     { return (unsigned)eng < 2 ? s_objEvaP[eng] : 16; }

GXTexObj *GX2DBG_ObjGet(int eng, int i, s16 *x, s16 *y, u16 *fx, u16 *fy,
                        u8 *prio, u8 *semi, const float **uv)
{
	if ((unsigned)eng >= 2 || (unsigned)i >= (unsigned)s_objPN[eng]) return NULL;
	ObjS *S = &s_objP[eng][i];
	if (!S->tex) return NULL;
	if (x)    { *x = S->x; }
	if (y)    { *y = S->y; }
	if (fx)   { *fx = S->fx; }
	if (fy)   { *fy = S->fy; }
	if (prio) { *prio = S->prio; }
	if (semi) { *semi = S->semi; }
	if (uv)   { *uv = S->uv; }
	return &S->obj;
}
