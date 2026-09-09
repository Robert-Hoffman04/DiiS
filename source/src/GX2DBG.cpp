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
	u16      w, h;         // plane size the buffer was allocated / baked at
	GXTexObj obj;
	u32      builtPalGen;
	u32      builtFullGen;
	bool     ready;        // baked and valid for this frame
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
static bool ensureLayer(int e, int n, u16 w, u16 h)
{
	Layer *L = &s_layer[e][n];
	u32 need = (u32)w * h * 2;
	if (L->plane && L->bytes == need)
		return true;

	if (s_totalBytes - L->bytes + need > GX2DBG_BUDGET)
		return false;

	if (L->plane) { free(L->plane); s_totalBytes -= L->bytes; }

	L->plane = (u16 *)memalign(32, need);
	if (!L->plane) { L->bytes = 0; return false; }
	L->bytes = need;
	s_totalBytes += need;
	L->w = w;
	L->h = h;
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
	const u16 W = L->w, H = L->h;
	u16 *plane = L->plane;

	u16 cell[64];
	for (u32 cy = 0; cy < (u32)(H >> 3); cy++) {
		for (u32 cx = 0; cx < (u32)(W >> 3); cx++) {
			GPU_ResolveTextTile8x8(gpu, (u8)n, cx, cy, cell);
			const u32 px0 = cx << 3, py0 = cy << 3;
			for (u32 r = 0; r < 8; r++)
				for (u32 c = 0; c < 8; c++)
					plane[tex16_ofs(px0 + c, py0 + r, W)] = cell[r * 8 + c];
		}
	}

	DCFlushRange(plane, L->bytes);
	GX_InitTexObj(&L->obj, plane, W, H, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
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

// Called at each engine's line 0 (core thread).  eng = gpu->core.
void GX2DBG_FrameUpdate(GPU *gpu)
{
	if (!gpu) return;
	const int e = (gpu->core == 0) ? 0 : 1;
	for (int n = 0; n < 4; n++) s_layer[e][n].ready = false;
	if (!s_enabled) return;

	const bool extPal = gpu->dispCnt().ExBGxPalette_Enable;
	const bool bg0is3d = (e == 0) && gpu->dispCnt().BG0_3D;   // SUB has no 3D

	for (int n = 0; n < 4; n++) {
		if (!gpu->LayersEnable[n] || gpu->BGTypes[n] != BGType_Text ||
		    (n == 0 && bg0is3d)) { freeLayer(e, n); continue; }

		const u16 w = gpu->BGSize[n][0];
		const u16 h = gpu->BGSize[n][1];
		if (!ensureLayer(e, n, w, h))          // over budget -> CPU path
			continue;

		if (extPal && gpu->bgcnt(n).Palette_256 &&
		    !MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[n]]) {
			freeLayer(e, n);
			continue;
		}

		Layer *L = &s_layer[e][n];
		const bool dirty = (L->builtFullGen != g_gxDirty.fullGen) ||
		                   (L->builtPalGen  != g_gxDirty.palGen)  ||
		                   rangeDirty(gpu->BG_map_ram[n], 0x2000) ||
		                   rangeDirty(gpu->BG_tile_ram[n], 0x10000);
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
#define GX2OBJ_TEXCAP (64 * 64)

struct ObjS {
	u16     *tex;
	u32      bytes;
	u16      w, h;
	GXTexObj obj;
	s16      x, y;
	u8       prio, semi, hflip, vflip;
	u32      key;
};
static ObjS  s_obj[2][128];
static int   s_objN[2]       = { 0, 0 };
static bool  s_objGXable[2]  = { false, false };
static u32   s_objBuiltEp[2] = { 0, 0 };
static u32   s_objBytes      = 0;
// latched for the draw thread (Present copies s_obj -> here)
static ObjS  s_objP[2][128];
static int   s_objPN[2]      = { 0, 0 };
static bool  s_objPGXable[2] = { false, false };

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

	const u32 ep = g_gxDirty.oamGen ^ (g_gxDirty.palGen << 1) ^ (g_gxDirty.fullGen << 2);
	const bool rebakeAll = (ep != s_objBuiltEp[e]);

	static u16 tmp[GX2OBJ_TEXCAP];
	for (int i = 0; i < 128; i++) {
		int ow, oh, ox, oy; u8 op, os, hf, vf; u32 key;
		const int r = GPU_ResolveObjSprite(gpu, i, tmp, GX2OBJ_TEXCAP,
		                                   &ow, &oh, &ox, &oy, &op, &os, &hf, &vf, &key);
		if (r == 0) continue;
		if (r < 0)  { s_objGXable[e] = false; return; }        // -> CPU sprite path
		if (op != 0) { s_objGXable[e] = false; return; }       // 5.2-1: front sprites only

		ObjS *S = &s_obj[e][s_objN[e]++];
		S->x = (s16)ox; S->y = (s16)oy; S->prio = op; S->semi = os;
		S->hflip = hf;  S->vflip = vf;

		const u32 need = (u32)ow * oh * 2;
		if (rebakeAll || S->key != key || S->w != ow || S->h != oh || !S->tex) {
			if (!S->tex || S->bytes != need) {
				if (S->tex) { free(S->tex); s_objBytes -= S->bytes; }
				if (s_totalBytes + s_objBytes + need > GX2DBG_BUDGET) {
					S->tex = NULL; S->bytes = 0;
					s_objGXable[e] = false; return;
				}
				S->tex = (u16 *)memalign(32, need);
				if (!S->tex) { S->bytes = 0; s_objGXable[e] = false; return; }
				S->bytes = need; s_objBytes += need;
			}
			swizzle16(tmp, S->tex, ow, oh);
			DCFlushRange(S->tex, need);
			GX_InitTexObj(&S->obj, S->tex, ow, oh, GX_TF_RGB5A3,
			              GX_CLAMP, GX_CLAMP, GX_FALSE);
			S->key = key; S->w = (u16)ow; S->h = (u16)oh;
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
	}
}

int  GX2DBG_ObjCount(int eng)   { return (unsigned)eng < 2 ? s_objPN[eng] : 0; }

GXTexObj *GX2DBG_ObjGet(int eng, int i, s16 *x, s16 *y, u16 *w, u16 *h,
                        u8 *prio, u8 *semi, u8 *hflip, u8 *vflip)
{
	if ((unsigned)eng >= 2 || (unsigned)i >= (unsigned)s_objPN[eng]) return NULL;
	ObjS *S = &s_objP[eng][i];
	if (!S->tex) return NULL;
	if (x)     { *x = S->x; }
	if (y)     { *y = S->y; }
	if (w)     { *w = S->w; }
	if (h)     { *h = S->h; }
	if (prio)  { *prio = S->prio; }
	if (semi)  { *semi = S->semi; }
	if (hflip) { *hflip = S->hflip; }
	if (vflip) { *vflip = S->vflip; }
	return &S->obj;
}
