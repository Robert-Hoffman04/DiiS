#include "gx_ds_3d_render.h"
#include "gx_rendermode.h"
#include "gx_swizzle.h"
#include "../gfx3d.h"
#include "../MMU.h"
#include "../texcache.h"
#include <stdio.h>
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#if defined(DSA_GXGEOM_DEBUGWHY) || defined(DSA_GXGEOM_TEXSTATS)
#include "../harness/harness.h"
#include "../harness/harness_profile.h"
#endif

extern MMU_struct MMU;
extern GXRModeObj *rmode;   // main.cpp (EFB pixel format choice: the polygon-ID tag pass needs RGB8)

// DS screen dimensions -- duplicated from gx_ds_engine_impl.inc's kDsBScreenW/H (not
// visible from here; both are just the fixed 256x192 DS screen size, not expected to
// ever differ) rather than pulling in that whole translation unit's static state.
static const int kScreenW = 256;
static const int kScreenH = 192;
// GX samples pixel centres, the DS rasterizer pixel corners (see gxDs3dRenderAccurate).
static const float kGxDs3dSampleOffset = 0.5f;

static inline u8 gxDs3d6To8Tex(u8 v6) { return (u8)((v6 << 2) | (v6 >> 4)); }

// POLYGON_ATTR shading mode, bits 4-5: 0 modulate, 1 decal, 2 toon/highlight, 3 shadow.
static inline int gxDs3dPolyMode(const POLY &p) { return (int)((p.polyAttr >> 4) & 3); }
// TEXIMAGE_PARAM format, bits 26-28 (see POLY::isTranslucent's own use of this same shift).
static inline int gxDs3dTexFormat(const POLY &p) { return (int)((p.texParam >> 26) & 7); }

// GxFast transform for one polygon. GX_LoadProjectionMtx keeps only 6 entries and
// hard-wires w = -z_in (perspective) or w = 1 (orthographic), and GX's clip-space Z
// range is [-w, 0] where the DS's is [-w, w]. So a general DS projection P can't be
// loaded as-is. Instead P's x, y and w rows are folded into the (affine) position
// matrix together with the modelview, and GX gets a canonical projection that only
// remaps depth: z_gx/w = (z_ds/w - 1)/2, which makes GX's depth equal the DS's
// (nz+1)/2, i.e. the same value gxDs3dRenderAccurate() feeds. Works for any
// P = perspective * affine; anything else is not representable and bails.
//
// W-buffer frames (gfx3d.wbuffer, GxFast only) don't use P's depth row at all: depth is
// gxDs3dWDepthInv(1/w) (see s_wK below), which GX gets from a constant
// z_clip = -K (perspective, depth = z_clip/w + 1 = 1 - K/w) or -K/w (orthographic, w
// constant per polygon). So in W mode any P whose w row is affine is representable.
struct GxDs3dFastXform {
	Mtx pos;
	Mtx44 proj;
	u8 projType;
};

// DS matrices are column-major: element (row r, col c) = m[4*c + r].
static inline float gxDs3dM(const float *m, int r, int c) { return m[4 * c + r]; }

static bool gxDs3dFastXformBuild(const POLY &p, GxDs3dFastXform &x, bool wbuf, float wK)
{
	const float *P = p.projMatrix, *MV = p.mvMatrix;
	const float eps = 1e-4f;
	if (fabsf(gxDs3dM(MV, 3, 0)) > eps || fabsf(gxDs3dM(MV, 3, 1)) > eps ||
	    fabsf(gxDs3dM(MV, 3, 2)) > eps || fabsf(gxDs3dM(MV, 3, 3) - 1.0f) > eps)
		return false;

	float M[3][4];
	memset(x.proj, 0, sizeof(x.proj));
	const bool ortho = fabsf(gxDs3dM(P, 3, 0)) <= eps && fabsf(gxDs3dM(P, 3, 1)) <= eps &&
	                   fabsf(gxDs3dM(P, 3, 2)) <= eps;
	if (ortho) {
		const float w = gxDs3dM(P, 3, 3);
		if (fabsf(w) <= eps) return false;
		for (int c = 0; c < 4; ++c) {
			M[0][c] = gxDs3dM(P, 0, c) / w;
			M[1][c] = gxDs3dM(P, 1, c) / w;
			M[2][c] = gxDs3dM(P, 2, c) / w;
		}
		x.proj[0][0] = 1; x.proj[1][1] = 1;
		x.proj[2][2] = 0.5f; x.proj[2][3] = -0.5f;
		if (wbuf) { x.proj[2][2] = 0; x.proj[2][3] = -wK / w; }
		x.proj[3][3] = 1;
		x.projType = GX_ORTHOGRAPHIC;
	} else if (wbuf) {
		for (int c = 0; c < 4; ++c) {
			M[0][c] = gxDs3dM(P, 0, c);
			M[1][c] = gxDs3dM(P, 1, c);
			M[2][c] = -gxDs3dM(P, 3, c);
		}
		x.proj[0][0] = 1; x.proj[1][1] = 1;
		x.proj[2][3] = -wK;
#ifdef DSA_GXGEOM_MUTATE_WREV
		x.proj[2][2] = 1; x.proj[2][3] = wK;   // mutation: depth K/w, far wins
#endif
		x.proj[3][2] = -1;
		x.projType = GX_PERSPECTIVE;
	} else {
		// P2 must equal alpha*P3 + beta*e_w (depth affine in w).
		int k = 0;
		for (int c = 1; c < 3; ++c)
			if (fabsf(gxDs3dM(P, 3, c)) > fabsf(gxDs3dM(P, 3, k))) k = c;
		const float alpha = gxDs3dM(P, 2, k) / gxDs3dM(P, 3, k);
		float scale = 0;
		for (int c = 0; c < 3; ++c) scale = fmaxf(scale, fabsf(gxDs3dM(P, 2, c)));
		for (int c = 0; c < 3; ++c)
			if (fabsf(gxDs3dM(P, 2, c) - alpha * gxDs3dM(P, 3, c)) > eps * fmaxf(1.0f, scale))
				return false;
		const float beta = gxDs3dM(P, 2, 3) - alpha * gxDs3dM(P, 3, 3);
		for (int c = 0; c < 4; ++c) {
			M[0][c] = gxDs3dM(P, 0, c);
			M[1][c] = gxDs3dM(P, 1, c);
			M[2][c] = -gxDs3dM(P, 3, c);   // z_in = -w_ds, so GX's w = -z_in = w_ds
		}
		x.proj[0][0] = 1; x.proj[1][1] = 1;
		x.proj[2][2] = (1.0f - alpha) * 0.5f;
		x.proj[2][3] = beta * 0.5f;
		x.proj[3][2] = -1;
		x.projType = GX_PERSPECTIVE;
	}
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) {
			float s = (c == 3) ? M[r][3] : 0.0f;
			for (int k = 0; k < 3; ++k)
				s += M[r][k] * gxDs3dM(MV, k, c);
			x.pos[r][c] = s;
		}
	return true;
}

// ---------------------------------------------------------------------------------
// Textured polygons (gx-remaining-work.md section 1, "Textured polygons").
//
// Texels come from the CPU rasterizer's own decoder (texcache.cpp, TexFormat_15bpp),
// so every NDS format (I2/I4/I8 paletted, 4x4 compressed, direct 16bpp) yields the
// exact texel values rasterize.cpp samples, and texcache's VRAM-mapping validation is
// shared. Each decoded 6665 texture is re-packed once into a GX RGB5A3 texture
// (opaque texels 1:5:5:5 lossless, transparent ones alpha 0) owned by the small cache
// below. Deviation from nds-wii-texture-format-mapping.md (native CI4/CI8 + TLUT for
// the paletted formats, TLUT slots 0-5): not needed for exactness and not done in this
// slice; RGB5A3 costs 2 bytes/texel instead of 0.5-1 plus a TLUT.
//
// Snapshot timing: gxDs3dPrepareTextures() runs at VBlank end (gfx3d_VBlankEndSignal),
// the moment the CPU raster samples texture VRAM, and the draw at line 191 uses that
// snapshot. The only way texture/palette content can change in between is a VRAMCNT
// remap (texture-mapped VRAM isn't CPU-writable), which also resolves a deferred raster
// first (gfx3d_vramRemapBarrier), so GX and the CPU raster see the same texels.
// ---------------------------------------------------------------------------------
struct GxDs3dTex {
	u32 key, pal;          // texParam with the wrap/flip/texgen bits masked, PLTT_BASE
	u32 amode;             // translucent formats: baked alpha mode (gxDs3dTexAMode), else 0
	TexCacheItem *src;     // texcache item this was converted from; NULL = stale
	void *data;
	u32 bytes;
	u16 w, h;
	u32 seq;               // g_gfx3dRenderSeq of the last prepare that used it
	GXTexObj obj;          // wrap modes set per polygon, see gxDs3dBindTex
};
static const int kTexSlots = 256;
static const u32 kTexBudget = 1536 * 1024;   // heap is tight (Task ssload-hang)
static GxDs3dTex s_tex[kTexSlots];
static int s_texCount = 0;
static u32 s_texBytes = 0;
static u32 s_texPrepSeq = 0xFFFFFFFF;
static bool s_texDirty = false;              // a texture was (re)written since the last draw
static int s_texLastHit = 0;

// texParam bits 0-15 address, 20-28 size/format, 29 colour-0 transparent.
static inline u32 gxDs3dTexKey(u32 texParam) { return texParam & 0x3FF0FFFF; }

// ---------------------------------------------------------------------------------
// Translucent polygons (gx-remaining-work.md section 1), GxFast only.
//
// rasterize.cpp draws them after the opaque ones in gfx3d.indexlist order (gfx3d_doFlush:
// opaque y-sorted, then translucent in list order or y-sorted per sortmode), and per
// fragment of alpha a (5 bits): a == 0 nothing; a < alphaTestRef (alpha test on) nothing;
// a == 31 an opaque write (colour, depth); otherwise, unless the pixel's last translucent
// polygon ID equals this one's, dst = ((a+1)*src + (31-a)*dst) >> 5 in 6-bit channels
// (plain write if dst alpha is 0), alpha max(src, dst), depth only with POLYGON_ATTR bit 11.
// GX reproduces it with a GX_BM_BLEND of source alpha A = gxDs3dGxAlpha(a) = 8*(a+1)
// (255 for 31, so opaque texels replace exactly) over the EFB, which holds exactly the 3D
// layer under the overlay: the clear colour (BG0's clear-only texture, see
// gxDsA3dScan) or an opaque polygon drawn earlier this pass. What is NOT exact, and why
// this is GxFast only: GX blends 8-bit expansions and the output is cut to 5 bits; modelled
// exhaustively over all a/src/dst, 77-82% of results match and the rest are +-1 LSB (5-bit)
// per blend (chained blends can stack that). Scope limits (gate reasons):
//  - clear alpha must be 31 (`transclear`): then every 3D pixel ends at alpha 31, the
//    dst-alpha-0 "plain write" case can't happen, and the 2D compositor shows the result
//    opaque, like the overlay (a 3D pixel with alpha < 31 would blend with a 2nd target).
//  - alpha blending on (DISP3DCNT bit 3; `transblend`), for the same reason.
//  - the polygon-ID rule has no GX dst compare: runs of same-ID polygons that overlap go
//    through a tag pass (gxDs3dTransIdPlan, gxDs3dTagRunPrepass); overlap with a same-ID
//    polygon of an earlier run, or an alpha-31 polygon in a tagged run, bails (`transid`).
// Fragment alpha: untextured and opaque-format textures -> the polygon alpha as vertex
// alpha (texel alpha is 255 or 0, the TEV multiply by 255 is exact). A3I5/A5I3 -> the
// per-texel modulated alpha (modulate_table[5to6(tA)][5to6(pA)] >> 1), alpha test
// included, baked into a GX RGBA8 texture keyed on the polygon alpha and test ref too
// (RGB5A3 has 3 alpha bits, A5I3 needs 5), drawn with vertex alpha 255. Depth: a polygon
// without bit 11 still writes depth for its a == 31 fragments (only possible for an
// A3I5/A5I3 polygon of alpha 31), so it is drawn twice, alpha == 255 with Z write, then
// 0 < alpha < 255 without.
// ---------------------------------------------------------------------------------
static u32 s_atRef = 0;   // alpha test ref (0 = off), latched at VBlank end with the texels

static inline int gxDs3dPolyAlpha(const POLY &p) { return (int)((p.polyAttr >> 16) & 0x1F); }
static inline bool gxDs3dTexFmtTrans(int fmt) { return fmt == 1 || fmt == 6; }
static inline u8 gxDs3dGxAlpha(int a) { return a >= 31 ? 255 : (a <= 0 ? 0 : (u8)(8 * (a + 1))); }
static inline u32 gxDs3dTexAMode(const POLY &p)
{
	if (!gxDs3dTexFmtTrans((int)((p.texParam >> 26) & 7))) return 0;
	return 0x80000000u | (s_atRef << 8) | (u32)gxDs3dPolyAlpha(p);
}

static void gxDs3dTexItemDeleted(TexCacheItem *item)
{
	for (int i = 0; i < s_texCount; ++i)
		if (s_tex[i].src == item) s_tex[i].src = NULL;
}

static int gxDs3dTexFind(u32 key, u32 pal, u32 amode)
{
	if (s_texLastHit < s_texCount && s_tex[s_texLastHit].key == key && s_tex[s_texLastHit].pal == pal &&
	    s_tex[s_texLastHit].amode == amode)
		return s_texLastHit;
	for (int i = 0; i < s_texCount; ++i)
		if (s_tex[i].key == key && s_tex[i].pal == pal && s_tex[i].amode == amode) return s_texLastHit = i;
	return -1;
}

static void gxDs3dTexFree(int i)
{
	free(s_tex[i].data);
	s_texBytes -= s_tex[i].bytes;
	s_tex[i] = s_tex[--s_texCount];
}

// Frees every texture the current frame doesn't use until `need` more bytes fit.
static bool gxDs3dTexMakeRoom(u32 need, u32 seq)
{
	for (int i = s_texCount - 1; i >= 0 && s_texBytes + need > kTexBudget; --i)
		if (s_tex[i].seq != seq) gxDs3dTexFree(i);
	return s_texBytes + need <= kTexBudget && s_texCount < kTexSlots;
}

static void gxDs3dTexConvert(GxDs3dTex &t, const TexCacheItem *item)
{
	const u32 *src = (const u32 *)item->decoded;   // RGB15TO6665: r bits 0-7, g 8-15, b 16-23, a 24-31
	u16 *dst = (u16 *)t.data;
	const int w = t.w, h = t.h;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const u32 c = src[y * w + x];
			u16 o = 0;   // alpha 0: dropped by the alpha compare, like the CPU's a == 0 fragment
			if (c >> 24)
				o = 0x8000 | (((c >> 1) & 0x1F) << 10) | (((c >> 9) & 0x1F) << 5) | ((c >> 17) & 0x1F);
			dst[(((y >> 2) * (w >> 2) + (x >> 2)) << 4) + ((y & 3) << 2) + (x & 3)] = o;
		}
	DCFlushRange(t.data, t.bytes);
	s_texDirty = true;
}

// A3I5/A5I3 (see the translucent section): GX RGBA8, 4x4 tiles of 32 bytes AR then 32 GB.
// Alpha is the final GX blend alpha of the modulated fragment, 0 where rasterize.cpp
// writes nothing (a == 0 or below the alpha test ref).
static void gxDs3dTexConvertTrans(GxDs3dTex &t, const TexCacheItem *item)
{
	const u32 *src = (const u32 *)item->decoded;
	u8 *dst = (u8 *)t.data;
	const int w = t.w, h = t.h;
	const int p6 = GFX3D_5TO6((int)(t.amode & 0x1F));
	const int ref = (int)((t.amode >> 8) & 0x1F);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const u32 c = src[y * w + x];
			const int t6 = GFX3D_5TO6((int)(c >> 24));
			const int a = (((t6 + 1) * (p6 + 1) - 1) >> 6) >> 1;
			u8 *tile = dst + (((y >> 2) * (w >> 2) + (x >> 2)) << 6) + ((((y & 3) << 2) + (x & 3)) << 1);
			tile[0]  = (a == 0 || a < ref) ? 0 : gxDs3dGxAlpha(a);
			tile[1]  = gxDs3d6To8Tex((u8)(c & 0x3F));
			tile[32] = gxDs3d6To8Tex((u8)((c >> 8) & 0x3F));
			tile[33] = gxDs3d6To8Tex((u8)((c >> 16) & 0x3F));
		}
	DCFlushRange(t.data, t.bytes);
	s_texDirty = true;
}

bool gxDs3dPrepareTextures()
{
	const u32 seq = g_gfx3dRenderSeq;
	s_texPrepSeq = 0xFFFFFFFF;
	const int polycount = gfx3d.polylist->count;
	for (int i = 0; i < polycount; ++i) {
		const POLY &p = gfx3d.polylist->list[i];
		if (gxDs3dTexFormat(p) == 0) continue;
		const u32 key = gxDs3dTexKey(p.texParam);
		const u32 amode = gxDs3dTexAMode(p);
		int k = gxDs3dTexFind(key, p.texPalette, amode);
		if (k >= 0 && s_tex[k].seq == seq && s_tex[k].src) continue;   // done this frame
		TexCacheItem *item = TexCache_SetTexture(TexFormat_15bpp, p.texParam, p.texPalette);
		if (!item || !item->decoded) return false;
		item->deleteCallback = gxDs3dTexItemDeleted;
		if (k >= 0 && s_tex[k].src == item) { s_tex[k].seq = seq; continue; }
		const u16 w = (u16)item->sizeX, h = (u16)item->sizeY;
		const u32 bytes = (u32)w * h * (amode ? 4 : 2);
		if (k >= 0 && s_tex[k].bytes != bytes) { gxDs3dTexFree(k); k = -1; }
		if (k < 0) {
			if (!gxDs3dTexMakeRoom(bytes, seq)) return false;
			void *data = memalign(32, bytes);
			if (!data) return false;
			k = s_texCount++;
			s_tex[k].key = key; s_tex[k].pal = p.texPalette; s_tex[k].amode = amode;
			s_tex[k].data = data; s_tex[k].bytes = bytes;
			s_texBytes += bytes;
		}
		GxDs3dTex &t = s_tex[k];
		t.w = w; t.h = h; t.src = item; t.seq = seq;
		if (amode) gxDs3dTexConvertTrans(t, item);
		else       gxDs3dTexConvert(t, item);
		GX_InitTexObj(&t.obj, t.data, w, h, amode ? GX_TF_RGBA8 : GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&t.obj, GX_NEAR, GX_NEAR);
	}
	s_texPrepSeq = seq;
	return true;
}

// The CPU modulates in 6 bits, ((tex+1)*(col+1)-1)>>6, then drops to 5; GX's TEV
// multiply of the 8-bit expansions agrees for every texel only at a few colours.
// 0 and 63 are exact whichever way GX rounds the product (checked exhaustively over
// all 32 texel values, for both truncating and +128-rounding TEV), so GxAccurate takes
// a textured polygon only when all its vertices share one colour whose channels are
// 0 or 63 (unlit/white textured geometry). Any other colour is within 1 LSB of the
// 5-bit output (30-36% of texel/colour pairs off by one), GxFast only.
static bool gxDs3dTexColorExact(const POLY &p)
{
	const VERT &v0 = gfx3d.vertlist->list[p.vertIndexes[0]];
	for (int c = 0; c < 3; ++c)
		if (v0.color[c] != 0 && v0.color[c] != 63) return false;
	for (int j = 1; j < p.type; ++j) {
		const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
		if (v.color[0] != v0.color[0] || v.color[1] != v0.color[1] || v.color[2] != v0.color[2])
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------
// Clipped polygons (gx-remaining-work.md section 1, "Clipped polygons").
//
// rasterize.cpp draws GFX3D_Clipper's output, not the polygon: Sutherland-Hodgman against
// the six planes (strict |coord| > w test, gfx3d.cpp), new vertices linearly interpolated in
// clip space (coords, texcoords, fcolor), the clipped coordinate snapped to +-w. Both
// producers take that same N-gon (up to MAX_CLIPPED_VERTS) from the same clipper code, so
// the vertices and attributes are the CPU's by construction:
//  - GxAccurate draws a clipped polygon through the span replay (gxDs3dDrawSpansAccurate,
//    textured or not): the CPU's edge walker over the CPU's N-gon. Flat colour only.
//  - GxFast draws it as a GX_TRIANGLEFAN in clip space: identity position matrix, position
//    (X, Y, -W) under the polygon's own canonical projection (gxDs3dFastXformBuild; a
//    clipped vertex still has Z = alpha*W + beta, so the depth row stays valid), or
//    (X, Y, Z)/W for an orthographic P. Unclipped polygons keep the hardware transform.
// Vertices behind the camera (w <= 0) are always outside (near plane), so they no longer
// bail. Only polygons with a vertex outside are clipped; others keep their own vertex
// order (the clipper would rotate it by one, which the existing paths never did).
// ---------------------------------------------------------------------------------
static GFX3D_Clipper s_clipper;
static GFX3D_Clipper::TClippedPoly s_clipOut;

static inline bool gxDs3dVertOutside(const VERT &v)
{
	const float w = v.coord[3];
	return v.coord[0] < -w || v.coord[0] > w || v.coord[1] < -w || v.coord[1] > w ||
	       v.coord[2] < -w || v.coord[2] > w;
}

// The polygon as rasterize.cpp draws it, in clip space, into out[MAX_CLIPPED_VERTS]:
// its own vertices when none is outside, else the clipper's N-gon. Returns the vertex
// count (< 3: clipped away, draws nothing). fcolor is set in both cases.
static int gxDs3dPolyVerts(POLY &p, VERT *out, bool &clipped)
{
	const int n = p.type;
	clipped = false;
	for (int j = 0; j < n; ++j) {
		out[j] = gfx3d.vertlist->list[p.vertIndexes[j]];
		out[j].color_to_float();   // rasterize.cpp does this to every vertex before clipping
		clipped |= gxDs3dVertOutside(out[j]);
	}
	if (!clipped) return n;
	VERT in[4];
	VERT *pin[4];
	for (int j = 0; j < n; ++j) { in[j] = out[j]; pin[j] = &in[j]; }
	if (n == 3) pin[3] = NULL;
	s_clipper.clippedPolys = &s_clipOut;
	s_clipper.clippedPolyCounter = 0;
	s_clipper.clipPoly(&p, pin);
	if (s_clipper.clippedPolyCounter == 0) return 0;
	const int m = s_clipOut.type;
	for (int j = 0; j < m; ++j) {
		out[j] = s_clipOut.clipVerts[j];
#ifdef DSA_GXGEOM_MUTATE_CLIPUV
		// mutation: a clipper-made vertex's texcoord one texel off, must fail a3_c38/39
		bool orig = false;
		for (int k = 0; k < n; ++k) orig |= memcmp(in[k].coord, out[j].coord, sizeof(in[k].coord)) == 0;
		if (!orig) { out[j].texcoord[0] += 1.0f; out[j].texcoord[1] += 1.0f; }
#endif
	}
	return m;
}

// rasterize.cpp's homogeneous divide + viewport + Y flip + screen clamp for x, y.
static inline void gxDs3dScreenXY(const VERT &v, const VIEWPORT &vp, float &x, float &y)
{
	x = (v.coord[0] + v.coord[3]) / (2.0f * v.coord[3]) * (float)vp.width + (float)vp.x;
	y = 192.0f - ((v.coord[1] + v.coord[3]) / (2.0f * v.coord[3]) * (float)vp.height + (float)vp.y);
	x = fmaxf(0.0f, fminf(256.0f, x));
	y = fmaxf(0.0f, fminf(192.0f, y));
}

// fcolor (0-63, fractional at clipper-made vertices) to 8 bits, = gxDs3d6To8Tex on integers.
static inline u8 gxDs3dF6To8(float f)
{
	f = fmaxf(0.0f, fminf(63.0f, f));
	const int lo = (int)f;
	const float a = gxDs3d6To8Tex((u8)lo), b = gxDs3d6To8Tex((u8)(lo < 63 ? lo + 1 : 63));
	return (u8)(a + (f - (float)lo) * (b - a) + 0.5f);
}

// Reason codes for gxDs3dFrameGate (first failing gate); names for DEBUGWHY/TEXSTATS.
enum {
	kGateOk = 0, kGateNoList, kGateEmpty, kGateWbuffer, kGateClearImage, kGateEdge, kGateFog,
	kGateTranslucent, kGatePolyMode, kGateTexNotReady, kGateTexColor, kGateType, kGateW, kGateClipColor,
	kGateFastProj, kGateDepthEqual, kGateTransClear, kGateTransBlend, kGateTransId, kGateQuadColor, kGateCount
};
static const char *const kGateNames[kGateCount] = {
	"OK", "nolist", "empty", "wbuffer", "clearimage", "edge", "fog", "translucent", "polymode",
	"texnotready", "texcolor", "type", "w<=0", "clipcolor", "fastproj", "depthequal", "transclear",
	"transblend", "transid", "quadcolor"
};

// Set by the last gxDs3dFrameGate: the frame has translucent polygons that will be drawn.
static bool s_gateHasTrans = false;

// A polygon's screen outline for the polygon-ID check and GxFast culling: rasterize.cpp's
// divide + viewport + clamp (gxDs3dScreenXY) of the vertices it draws (gxDs3dPolyVerts) and
// 28.4 truncation (as gxDs3dDrawSpansAccurate), in 1/16 px units. False if it draws nothing:
// clipped away, or culled by its facing (rasterize.cpp's backface test on the clipped
// outline and PolyAttr::isVisible, as gxDs3dPolyVisible).
struct GxDs3dTransShape { float x[MAX_CLIPPED_VERTS], y[MAX_CLIPPED_VERTS], x0, y0, x1, y1; int n; u8 id; };
static bool gxDs3dTransShapeBuild(const POLY &p, const VERT *cv, int n, GxDs3dTransShape &sh)
{
	if (n < 3) return false;
	VIEWPORT vp;
	vp.decode(p.viewport);
	float fx[MAX_CLIPPED_VERTS], fy[MAX_CLIPPED_VERTS];
	for (int j = 0; j < n; ++j) gxDs3dScreenXY(cv[j], vp, fx[j], fy[j]);
	float facing = (fy[0] + fy[n - 1]) * (fx[0] - fx[n - 1]);
	for (int j = 0; j < n - 1; ++j) facing += (fy[j + 1] + fy[j]) * (fx[j + 1] - fx[j]);
	const bool back = facing < 0;
	switch ((p.polyAttr >> 6) & 3) {
		case 0: return false;
		case 1: if (!back) return false; break;
		case 2: if (back) return false; break;
		default: break;
	}
	sh.n = n;
	sh.id = (u8)((p.polyAttr >> 24) & 0x3F);
	sh.x0 = sh.y0 = 1e30f; sh.x1 = sh.y1 = -1e30f;
	for (int j = 0; j < n; ++j) {
		sh.x[j] = floorf(16.0f * fx[j]);
		sh.y[j] = floorf(16.0f * fy[j]);
		sh.x0 = fminf(sh.x0, sh.x[j]); sh.x1 = fmaxf(sh.x1, sh.x[j]);
		sh.y0 = fminf(sh.y0, sh.y[j]); sh.y1 = fmaxf(sh.y1, sh.y[j]);
	}
	return true;
}

// Separating-axis test on two convex outlines: true if some edge normal of either separates
// them, touching counts as separate. Rasterize.cpp's fill convention gives a pixel on a
// shared edge to one side only, so interior-disjoint outlines share no pixel.
static bool gxDs3dTransShapesDisjoint(const GxDs3dTransShape &a, const GxDs3dTransShape &b)
{
	if (a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0) return true;
	for (int pass = 0; pass < 2; ++pass) {
		const GxDs3dTransShape &e = pass ? b : a;
		for (int j = 0; j < e.n; ++j) {
			const int k = (j + 1) % e.n;
			const double nx = e.y[k] - e.y[j], ny = e.x[j] - e.x[k];
			double amin = 1e300, amax = -1e300, bmin = 1e300, bmax = -1e300;
			for (int m = 0; m < a.n; ++m) { const double d = nx * a.x[m] + ny * a.y[m]; amin = fmin(amin, d); amax = fmax(amax, d); }
			for (int m = 0; m < b.n; ++m) { const double d = nx * b.x[m] + ny * b.y[m]; bmin = fmin(bmin, d); bmax = fmax(bmax, d); }
			if (amax <= bmin || bmax <= amin) return true;
		}
	}
	return false;
}

// Does this translucent polygon draw anything (see gxDs3dRenderFast)? Alpha-0 polygons
// never do; a uniform-alpha one (not A3I5/A5I3) is dropped whole by the alpha test.
static bool gxDs3dTransDraws(const POLY &p)
{
	const int a = gxDs3dPolyAlpha(p);
	if (a == 0) return false;
	return gxDs3dTexFmtTrans(gxDs3dTexFormat(p)) || a >= (int)s_atRef;
}

// Translucent polygon-ID rule: a translucent fragment (0 < a < 31) is dropped, with no
// colour or depth write, where the pixel's last translucent write had the same polygon ID;
// one that draws stamps its ID (rasterize.cpp; a == 31 fragments neither test nor stamp,
// opaque polygons only set the opaque ID). GX has no destination compare, so GxFast
// reproduces it per "run" (a maximal sequence of consecutive drawing translucent polygons
// in indexlist order with one ID) with a tag pass (see gxDs3dTagRunPrepass): per pixel the
// first polygon of the run whose fragment passes depth and alpha draws, the rest are dropped.
// That is exact for a run as long as nothing before it stamped the same ID on its pixels,
// so this returns -1 (bail) when a polygon's outline overlaps one of the same ID in an
// EARLIER run (conservative: another ID may have restamped the pixel in between), or when
// a run that needs tags contains an alpha-31 polygon (A3I5/A5I3 a == 31 texels: opaque
// writes, whose depth writes would change later run polygons' winners). Runs whose outlines
// are interior-disjoint (gxDs3dTransShapesDisjoint; mesh neighbours sharing an edge) need no
// tags. Else returns the number of drawing translucent polygons and fills s_tagRun/s_tagIdx.
// Clipped polygons use their clipped outline.
struct GxDs3dTagRun { int n0, n1; int x0, y0, x1, y1; int count; };   // indexlist range, 1/16 px box
static const int kMaxTrans = 2048;       // the DS's own per-frame polygon limit
static u16 s_tagRun[POLYLIST_SIZE];      // per polylist index: tagged run (1-based), 0 = plain draw
static u16 s_tagIdx[POLYLIST_SIZE];      // its 1-based position in that run (the tag it writes)
static GxDs3dTagRun s_tagRuns[kMaxTrans + 1];
static int s_tagRunCount = 0;
static int gxDs3dTransIdPlan()
{
	static GxDs3dTransShape s_sh[kMaxTrans];
	static s16 s_next[kMaxTrans];        // per-ID chains of shapes from closed runs
	static u16 s_shPoly[kMaxTrans];
	static int s_shN[kMaxTrans];
	s16 head[64];
	for (int k = 0; k < 64; ++k) head[k] = -1;
	int nt = 0, runStart = 0, curId = -1;
	bool runTag = false, runA31 = false;
	s_tagRunCount = 0;
	const int polycount = gfx3d.polylist->count;
	for (int n = 0; n <= polycount; ++n) {
		POLY *pp = NULL;
		GxDs3dTransShape *sh = NULL;
		if (n < polycount) {
			const int i = gfx3d.indexlist[n];
			POLY &p = gfx3d.polylist->list[i];
			s_tagRun[i] = 0;
			if (!p.isTranslucent() || !gxDs3dTransDraws(p)) continue;
			if (nt == kMaxTrans) return -1;
			sh = &s_sh[nt];
			VERT cv[MAX_CLIPPED_VERTS];
			bool clipped;
			const int nv = gxDs3dPolyVerts(p, cv, clipped);
			if (!gxDs3dTransShapeBuild(p, cv, nv, *sh)) continue;
			if (sh->x1 <= 0 || sh->x0 >= 16.0f * kScreenW || sh->y1 <= 0 || sh->y0 >= 16.0f * kScreenH) continue;   // off screen
			pp = &p;
			s_shPoly[nt] = (u16)i;
			s_shN[nt] = n;
		}
		if (!pp || sh->id != curId) {
			// close the current run [runStart, nt)
			if (runTag && s_tagRunCount < kMaxTrans) {
				GxDs3dTagRun &r = s_tagRuns[++s_tagRunCount];
				r.n0 = s_shN[runStart]; r.n1 = s_shN[nt - 1]; r.count = nt - runStart;
				float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
				for (int k = runStart; k < nt; ++k) {
					s_tagRun[s_shPoly[k]] = (u16)s_tagRunCount;
					s_tagIdx[s_shPoly[k]] = (u16)(k - runStart + 1);
					x0 = fminf(x0, s_sh[k].x0); y0 = fminf(y0, s_sh[k].y0);
					x1 = fmaxf(x1, s_sh[k].x1); y1 = fmaxf(y1, s_sh[k].y1);
				}
				r.x0 = (int)x0; r.y0 = (int)y0; r.x1 = (int)x1; r.y1 = (int)y1;
			}
			for (int k = runStart; k < nt; ++k) {
				s_next[k] = head[s_sh[k].id];
				head[s_sh[k].id] = (s16)k;
			}
			if (!pp) break;
			runStart = nt; curId = sh->id; runTag = false; runA31 = false;
		}
		for (int k = head[sh->id]; k >= 0; k = s_next[k])
			if (!gxDs3dTransShapesDisjoint(s_sh[k], *sh)) {
#ifdef DSA_GXGEOM_TRANSIDDBG
				static u32 s_dbg;
				if ((s_dbg++ & 31) == 0)
					harness_profile_emitf("transiddbg id=%d n=%d nv=%d attr=%08x tex=%08x box=%d,%d,%d,%d prev box=%d,%d,%d,%d ntrans=%d",
					                      sh->id, n, sh->n, (unsigned)pp->polyAttr, (unsigned)pp->texParam,
					                      (int)sh->x0 / 16, (int)sh->y0 / 16, (int)sh->x1 / 16, (int)sh->y1 / 16,
					                      (int)s_sh[k].x0 / 16, (int)s_sh[k].y0 / 16, (int)s_sh[k].x1 / 16, (int)s_sh[k].y1 / 16, nt);
#endif
				return -1;
			}
		if (!runTag)
			for (int k = runStart; k < nt; ++k)
				if (!gxDs3dTransShapesDisjoint(s_sh[k], *sh)) { runTag = true; break; }
		runA31 |= gxDs3dPolyAlpha(*pp) == 31;
		if (runTag && (runA31 || (rmode && rmode->aa))) return -1;
		++nt;
	}
	return nt;
}

// requireTex: textured polygons need a texture prepared for this frame (false only
// for the VBlank-end call that decides whether to prepare them).
static int gxDs3dFrameGate(bool requireTex)
{
	if (!gfx3d.polylist || !gfx3d.vertlist) return kGateNoList;
	const int polycount = gfx3d.polylist->count;
	if (polycount <= 0) return kGateEmpty;
	const bool fast = gxRenderModeIsFast();
	// W-buffer mode: GxFast only, drawn with the depth mapping of gxDs3dWDepthInv()
	// (see s_wK). GX's 24-bit D = 1 - K/w buckets w differently from the CPU's
	// floor(4096*w), so near-ties can resolve differently: not exact, GxAccurate bails.
	if (gfx3d.wbuffer && !fast) return kGateWbuffer;
	if (gfx3d.enableClearImage) return kGateClearImage;  // rear-plane per-pixel clear depth (Stage 2) not modelled
	// Antialiasing is deliberately not checked: rasterize.cpp never implements it.
	if (gfx3d.enableEdgeMarking) return kGateEdge;     // no polygon-ID infra yet (13f handoff item)
	if (gfx3d.enableFog) {
		// Task 13f-fog (see gx-next-steps-log.md's Task 13f-fog section): a GxFast-only
		// fog post-process (gxDs3dApplyFogFast()) was built -- EFB-Z masking and the LUT
		// and blend hardware are verified -- but the single-8-bit-indirect-texture
		// Z-lookup that should make the blend weight vary per pixel does NOT vary
		// (a3_c29: 19511/19516 fogged pixels off by up to 83/255). Not the small/bounded
		// kind of gap GxFast is meant to accept, so fog keeps bailing to the CPU
		// rasterizer; gxDs3dApplyFogFast() and its call site stay compiled in, unreached.
		return kGateFog;
	}

	bool anyTrans = false;
	s_gateHasTrans = false;
	for (int i = 0; i < polycount; ++i) {
		POLY &p = gfx3d.polylist->list[i];   // isTranslucent() is non-const in POLY
		if (p.isTranslucent()) {
			// GxFast only (see the translucent section): GX's 8-bit blend is +-1 LSB.
			if (!fast) return kGateTranslucent;
			if (((gfx3d_rasterClearColor() >> 16) & 0x1F) != 31) return kGateTransClear;
			if (!gfx3d.enableAlphaBlending) return kGateTransBlend;
			anyTrans = true;
		}
		// POLYGON_ATTR bit 14: depth test EQUAL (rasterize.cpp's decalMode). Not modelled
		// (GX_EQUAL on GX's own depth would not match the CPU's quantized equality).
		if (p.polyAttr & (1 << 14)) return kGateDepthEqual;
		// modulate only: decal/toon/highlight need their own TEV, shadow is 13g
		if (gxDs3dPolyMode(p) != 0) return kGatePolyMode;
		if (gxDs3dTexFormat(p) != 0) {
			// Every format goes through texcache's decoder (see the textured section);
			// A3I5/A5I3 (translucent, GxFast only) are baked to RGBA8 per polygon alpha.
			if (requireTex) {
				const int k = gxDs3dTexFind(gxDs3dTexKey(p.texParam), p.texPalette, gxDs3dTexAMode(p));
				if (s_texPrepSeq != g_gfx3dRenderSeq || k < 0 || s_tex[k].seq != s_texPrepSeq)
					return kGateTexNotReady;
			}
			if (!fast && !gxDs3dTexColorExact(p)) return kGateTexColor;
		}
		if (p.type != 3 && p.type != 4) return kGateType;
		// The DS interpolates a quad's colours natively along its edges and spans; GX splits
		// GX_QUADS into two triangles. Up to 6 steps apart for four different vertex colours
		// (a3_c37, 4653 px), so GxAccurate takes untextured quads only with one flat colour
		// (textured ones already need it, gxDs3dTexColorExact). GxFast keeps drawing them.
		if (!fast && p.type == 4 && gxDs3dTexFormat(p) == 0) {
			const VERT &v0 = gfx3d.vertlist->list[p.vertIndexes[0]];
			for (int j = 1; j < 4; ++j) {
				const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
				if (v.color[0] != v0.color[0] || v.color[1] != v0.color[1] || v.color[2] != v0.color[2])
					return kGateQuadColor;
			}
		}
	}

	// Translucent polygon-ID rule (gxDs3dTransIdPlan). Checked before the per-vertex loop
	// below: it is what rejects SM64DS's frames, and that loop is the gate's biggest cost.
	if (anyTrans) {
		const int nt = gxDs3dTransIdPlan();
		if (nt < 0) return kGateTransId;
		s_gateHasTrans = nt > 0;
	}

	// Clipping (see the clipped-polygons section): every polygon is taken as the CPU clipper
	// leaves it. A clipped vertex has w >= |x|, |y|, |z|, so w > 0 unless it is degenerate
	// (the polygon passes exactly through the eye). GxAccurate clips here to rule that out;
	// GxFast doesn't (SM64DS clips ~1500 polygons a frame, and the gate runs on every frame):
	// gxDs3dRenderFast skips such a polygon, where the CPU would divide by zero.
	const POLY *lastP = NULL;   // last polygon whose matrices passed fastproj
	for (int i = 0; i < polycount; ++i) {
		POLY &p = gfx3d.polylist->list[i];
		bool clipped = false;
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			clipped |= gxDs3dVertOutside(v);
		}
		if (!clipped) {
			for (int j = 0; j < p.type; ++j)
				if (gfx3d.vertlist->list[p.vertIndexes[j]].coord[3] <= 0.0f) return kGateW;
		} else if (!fast) {
			VERT cv[MAX_CLIPPED_VERTS];
			const int n = gxDs3dPolyVerts(p, cv, clipped);
			for (int j = 0; j < n; ++j)
				if (cv[j].coord[3] <= 0.0f) return kGateW;
			// GxAccurate spans carry one flat colour (textured polygons: gxDs3dTexColorExact).
			if (n >= 3 && gxDs3dTexFormat(p) == 0) {
				const VERT &v0 = gfx3d.vertlist->list[p.vertIndexes[0]];
				for (int j = 1; j < p.type; ++j) {
					const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
					if (v.color[0] != v0.color[0] || v.color[1] != v0.color[1] || v.color[2] != v0.color[2])
						return kGateClipColor;
				}
			}
		}
		if (fast && (!lastP || memcmp(lastP->mvMatrix, p.mvMatrix, sizeof(p.mvMatrix)) != 0 ||
		             memcmp(lastP->projMatrix, p.projMatrix, sizeof(p.projMatrix)) != 0)) {
			GxDs3dFastXform x;
			if (!gxDs3dFastXformBuild(p, x, gfx3d.wbuffer != 0, 1.0f)) return kGateFastProj;
			lastP = &p;
		}
	}

	return kGateOk;
}

bool gxDs3dGeomFrameHasTranslucent() { return s_gateHasTrans; }

// Result of this frame's gxDs3dGeomFramePrepare(). The line-191 call can only be stricter
// (requireTex), so a frame prepare rejected is rejected again without re-running the gate.
static u32 s_prepSeq = 0xFFFFFFFF;
static bool s_prepOk = false;

bool gxDs3dGeomFrameSupported()
{
	if (s_prepSeq == g_gfx3dRenderSeq && !s_prepOk) return false;
	const int g = gxDs3dFrameGate(true);
#ifdef DSA_GXGEOM_DEBUGWHY
	harness_profile_emitf("gxds3dwhy %s polycount=%d", kGateNames[g], gfx3d.polylist ? gfx3d.polylist->count : -1);
#endif
	return g == kGateOk;
}

bool gxDs3dGeomFramePrepare()
{
	s_atRef = gfx3d.enableAlphaTest ? gfx3d.alphaTestRef : 0;
	const int g = gxDs3dFrameGate(false);
#ifdef DSA_GXGEOM_TEXSTATS
	// Per-3D-frame first failing gate, plus per-format polygon counts, every 32 frames.
	// transidclash: the ID-rule check on every 3D frame regardless of earlier gates.
	// clippolys: polygons with a vertex outside the clip volume (clipped by the CPU clipper).
	// tagruns: runs needing the tag pass (gxDs3dTransIdPlan), tagmax their largest polygon
	// count, tagarea their summed box area in px.
	static u32 s_gate[kGateCount], s_fmt[8], s_frames, s_tidClash, s_tidPolys, s_clipPolys;
	static u32 s_tagRunsSum, s_tagMax, s_tagArea;
	++s_gate[g];
	++s_frames;
	if (gfx3d.polylist && gfx3d.vertlist && gfx3d.polylist->count > 0) {
		const int nt = gxDs3dTransIdPlan();
		if (nt < 0) ++s_tidClash; else s_tidPolys += (u32)nt;
		if (nt >= 0)
			for (int r = 1; r <= s_tagRunCount; ++r) {
				const GxDs3dTagRun &t = s_tagRuns[r];
				++s_tagRunsSum;
				if ((u32)t.count > s_tagMax) s_tagMax = (u32)t.count;
				s_tagArea += (u32)(((t.x1 - t.x0) >> 4) * ((t.y1 - t.y0) >> 4));
			}
	}
	if (gfx3d.polylist && gfx3d.vertlist)
		for (int i = 0; i < gfx3d.polylist->count; ++i) {
			const POLY &p = gfx3d.polylist->list[i];
			++s_fmt[gxDs3dTexFormat(p)];
			bool out = false;
			for (int j = 0; j < p.type; ++j) out |= gxDs3dVertOutside(gfx3d.vertlist->list[p.vertIndexes[j]]);
			s_clipPolys += out;
		}
	if ((s_frames & 31) == 0) {
		char buf[256];
		int n = 0;
		for (int k = 0; k < kGateCount; ++k)
			if (s_gate[k]) n += snprintf(buf + n, sizeof(buf) - n, " %s=%u", kGateNames[k], (unsigned)s_gate[k]);
		harness_profile_emitf("gxds3dstats frames=%u fmt=%u,%u,%u,%u,%u,%u,%u,%u texbytes=%u clippolys=%u transidclash=%u transpolys_ok=%u tagruns=%u tagmax=%u tagarea=%u gates:%s",
		                      (unsigned)s_frames, (unsigned)s_fmt[0], (unsigned)s_fmt[1], (unsigned)s_fmt[2],
		                      (unsigned)s_fmt[3], (unsigned)s_fmt[4], (unsigned)s_fmt[5], (unsigned)s_fmt[6],
		                      (unsigned)s_fmt[7], (unsigned)s_texBytes, (unsigned)s_clipPolys, (unsigned)s_tidClash, (unsigned)s_tidPolys,
		                      (unsigned)s_tagRunsSum, (unsigned)s_tagMax, (unsigned)s_tagArea, buf);
	}
#endif
	s_prepSeq = g_gfx3dRenderSeq;
	s_prepOk = g == kGateOk && gxDs3dPrepareTextures();
	return s_prepOk;
}

// Shared vertex-color / no-texture TEV+channel setup for both producers.
static void gxDs3dSetupCommonState()
{
	GX_SetCullMode(GX_CULL_NONE);
	// GX's clipper stays on: it only appeared to reject everything while Z was fed
	// the wrong way round (verified: enabling/disabling it gives identical output on
	// a3_c27/a3_c28 now), and every vertex it gets is in-frustum (polygons crossing the
	// clip volume come pre-clipped by GFX3D_Clipper, see the clipped-polygons section).
	GX_SetClipMode(GX_CLIP_ENABLE);
	// Both producers write depth = (nz+1)/2 * 0xFFFFFF: near 0, far max (the ortho
	// matrix's -z_eye cancels gxDs3dRenderAccurate()'s negation; see
	// gxDs3dFastXformBuild() for Fast). The CPU rasterizer rejects when
	// depth >= dest, i.e. passes on LESS. (The previous pass tried GEQUAL, which is
	// the wrong direction -- that, not the EFB clear, is why draws vanished.)
	GX_SetZMode(GX_TRUE, GX_LESS, GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
#ifdef DSA_GXGEOM_MUTATE_NOZ
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);   // mutation: depth test must matter on a3_c28
#endif
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetDither(GX_FALSE);

	// Vertex color only (material color already baked per-vertex by the DS geometry
	// engine/SetVertex, opaque scope this pass so alpha is always 255) -- no lighting,
	// no texture, one TEV stage passing the raster color straight through.
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	// Textured polygons: a texel with alpha 0 makes the CPU's modulated fragment alpha 0,
	// which rasterize.cpp drops before the depth write. Alpha compare > 0 with the Z test
	// moved after texturing reproduces that (untextured polys always pass, alpha 255).
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZCompLoc(GX_FALSE);
	if (s_texDirty) {
		GX_InvalidateTexAll();
		s_texDirty = false;
	}
}

// Undoes the state gxDs3dSetupCommonState sets that the 2D pass (gxDsBSetup2DState)
// assumes is at its GX default.
static void gxDs3dRestoreState()
{
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZCompLoc(GX_TRUE);
}

// Per-polygon texture state. `accurate` feeds texcoords as a projective normal
// (s/w, t/w, 1/w) with an ortho screen-space position, so GX's per-pixel s/q divide
// gives the CPU's perspective-correct u = (u/w)*w; GxFast lets GX's own perspective
// interpolation do it from plain (s, t). Both scale texel units to [0,1] in TEXMTX0.
struct GxDs3dTexState {
	bool textured;
	u32 texParam, texPal, amode;
};

static void gxDs3dBindPoly(const POLY &p, bool accurate, GxDs3dTexState &st, bool first)
{
	const bool textured = gxDs3dTexFormat(p) != 0;
	if (first || textured != st.textured) {
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		if (textured && accurate) GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		if (textured && !accurate) GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		if (textured) {
			GX_SetNumTexGens(1);
			if (accurate) GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0);
			else          GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		} else {
			GX_SetNumTexGens(0);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
			GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		}
		st.textured = textured;
		st.texParam = 0xFFFFFFFF;
	}
	const u32 amode = gxDs3dTexAMode(p);
	if (!textured || (p.texParam == st.texParam && p.texPalette == st.texPal && amode == st.amode)) return;
	const int k = gxDs3dTexFind(gxDs3dTexKey(p.texParam), p.texPalette, amode);
	if (k < 0) return;   // unreachable: gxDs3dFrameGate checked every textured poly
	GxDs3dTex &t = s_tex[k];
	// TEXIMAGE_PARAM bits 16/17 repeat S/T, 18/19 flip S/T (flip without repeat = clamp),
	// the same table as rasterize.cpp's Sampler::dowrap.
	const u8 ws = (p.texParam & (1 << 16)) ? ((p.texParam & (1 << 18)) ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
	const u8 wt = (p.texParam & (1 << 17)) ? ((p.texParam & (1 << 19)) ? GX_MIRROR : GX_REPEAT) : GX_CLAMP;
	GX_InitTexObjWrapMode(&t.obj, ws, wt);
	GX_LoadTexObj(&t.obj, GX_TEXMAP0);
	Mtx m;
	guMtxScale(m, 1.0f / (f32)t.w, 1.0f / (f32)t.h, 1.0f);
	GX_LoadTexMtxImm(m, GX_TEXMTX0, accurate ? GX_MTX3x4 : GX_MTX2x4);
	st.texParam = p.texParam;
	st.texPal = p.texPalette;
	st.amode = amode;
}

// rasterize.cpp: a polygon alpha of 0 (wireframe on hardware) makes every fragment's
// alpha 0, and those are never written. So it draws nothing; neither do we.
static inline bool gxDs3dPolyInvisible(const POLY &p) { return ((p.polyAttr >> 16) & 0x1F) == 0; }

// Found this pass: VERT::color is the DS geometry engine's native 6-bit-per-channel
// material color (0-63, see gfx3d.h's GFX3D_5TO6/rasterize.cpp's shader.materialColor
// clamp to [0,63]) -- feeding it directly to GX_Color4u8 (which expects 0-255) made
// every triangle render ~4x too dark. Standard 6-to-8-bit bit-replication expansion
// (matches this codebase's other N-bit-to-8-bit conventions, e.g. gfx3d.h's
// material_5bit_to_8bit table for the 5-bit case).
static inline u8 gxDs3d6To8(u8 v6) { return (u8)((v6 << 2) | (v6 >> 4)); }

static inline void gxDs3dSendColor(const VERT &v, u8 alpha = 255)
{
#ifdef DSA_GXGEOM_PROBE
	// Debug: paint each producer a flat marker colour so a capture shows exactly
	// which pixels GX drew (Accurate magenta, Fast cyan).
	(void)v;
	if (gxRenderModeIsFast()) GX_Color4u8(0, 255, 255, 255);
	else                      GX_Color4u8(255, 0, 255, 255);
#else
	GX_Color4u8(gxDs3d6To8(v.color[0]), gxDs3d6To8(v.color[1]), gxDs3d6To8(v.color[2]), alpha);
#endif
}

static void gxDs3dLoadScreenOrtho()
{
	Mtx44 proj;
	guOrtho(proj, 0, (f32)kScreenH, 0, (f32)kScreenW, 0, 1);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	Mtx mv;
	guMtxIdentity(mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

// ---------------------------------------------------------------------------------
// W-buffer depth (gfx3d.wbuffer; gx-remaining-work.md section 1, "W-buffer depth mode").
// GxFast only.
//
// The CPU tests depth = u32floor(4096*w) per pixel (LESS), with w = 1/invw and invw
// interpolated linearly in screen space. GX has no W buffer: its depth is post-divide
// z/w, interpolated linearly in screen space, so any GX depth is affine in 1/w. GxFast
// gives it D = 1 - K/w (z_clip = -K): strictly increasing in w, so GX picks the same
// winner as the CPU wherever neither side's quantization ties. K is the frame's smallest
// vertex w less 1% (w over a polygon lies between its vertices' w, so D stays in (0,1);
// GX clips at D = 0), which gives D the most resolution. What differs is the tie set:
// the CPU buckets w in steps of 1/4096, GX in steps of w^2/(K*2^24) -- finer than the
// CPU's for w < 64*sqrt(K), coarser beyond. Near-coplanar surfaces far from the camera
// can therefore z-fight on GX where the CPU separates them (and vice versa near it);
// that is the bounded GxFast inexactness, and why GxAccurate bails W-buffer frames.
// The CLEAR_DEPTH compare maps exactly: the CPU fails iff 4096*w >= clearDepth, i.e.
// D >= 1 - 4096*K/clearDepth. Orthographic polygons (w constant) tie everywhere on both
// sides (later fragment fails), which LESS reproduces.
// ---------------------------------------------------------------------------------
static float s_wK = 1.0f;

static void gxDs3dWSetup()
{
	float k = 0;
	bool any = false;
	const int polycount = gfx3d.polylist->count;
	for (int i = 0; i < polycount; ++i) {
		POLY &p = gfx3d.polylist->list[i];
		// The vertices it draws. A clipped polygon lies inside its own vertices' hull, so its
		// w >= their min unless the near plane cut it (a vertex with z < -w, which includes
		// every one behind the eye): only then does it need the clipper's own vertices.
		bool nearCut = false;
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			nearCut |= v.coord[2] < -v.coord[3] || v.coord[3] <= 0.0f;
		}
		if (!nearCut) {
			for (int j = 0; j < p.type; ++j) {
				const float w = gfx3d.vertlist->list[p.vertIndexes[j]].coord[3];
				if (!any || w < k) { k = w; any = true; }
			}
			continue;
		}
		VERT cv[MAX_CLIPPED_VERTS];
		bool clipped;
		const int n = gxDs3dPolyVerts(p, cv, clipped);
		for (int j = 0; j < (n >= 3 ? n : 0); ++j) {
			const float w = cv[j].coord[3];
			if (w > 0.0f && (!any || w < k)) { k = w; any = true; }
		}
	}
	// 1% margin: the nearest vertex would otherwise sit exactly on GX's near clip plane
	// (D = 0), and GxFast's own float transform can put it a hair outside.
	s_wK = (any && k > 0) ? k * 0.99f : 1.0f;   // gxDs3dFrameGate guarantees every w > 0
}

#ifdef DSA_GXGEOM_MUTATE_WREV
// mutation: W depth order reversed (far wins), must fail a3_c32/a3_c33
static inline float gxDs3dWDepthInv(float invw) { return invw > 0.0f ? s_wK * invw : 1.0f; }
#else
static inline float gxDs3dWDepthInv(float invw) { return 1.0f - s_wK * invw; }
#endif

// Seeds the EFB's Z with the DS CLEAR_DEPTH value (the CPU rasterizer's
// clearFragment.depth), independent of whatever GX_SetCopyClear or the 2D pass left
// there. Needs gxDs3dLoadScreenOrtho() bound; colour untouched.
static void gxDs3dClearDepth()
{
	float z = -(float)(gfx3d_rasterClearDepth() & 0xFFFFFF) / 16777215.0f;
	if (gfx3d.wbuffer) {
		const u32 cd = gfx3d_rasterClearDepth() & 0xFFFFFF;
#ifdef DSA_GXGEOM_MUTATE_WREV
		const float d = 1.0f; (void)cd;
#else
		const float d = cd ? gxDs3dWDepthInv(4096.0f / (float)cd) : 0.0f;
#endif
		z = -(d < 0.0f ? 0.0f : d);
	}
	GX_SetColorUpdate(GX_FALSE);
	GX_SetAlphaUpdate(GX_FALSE);
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32(0, 0, z);                           GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32((f32)kScreenW, 0, z);               GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32((f32)kScreenW, (f32)kScreenH, z);   GX_Color4u8(0, 0, 0, 255);
		GX_Position3f32(0, (f32)kScreenH, z);               GX_Color4u8(0, 0, 0, 255);
	GX_End();
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
#ifdef DSA_GXGEOM_MUTATE_NOZ
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
#else
	GX_SetZMode(GX_TRUE, GX_LESS, GX_TRUE);
#endif
}

// ---------------------------------------------------------------------------------
// GxAccurate textured polygons: span quads from rasterize.cpp's own edge walker.
//
// Measured on a3_c30: drawing a textured polygon as one GX primitive picks a different
// texel than the CPU on ~3200 px (whole texel-boundary columns). The CPU does not
// evaluate the attribute plane at a fixed sample point: each span linearly stretches the
// attributes at the TRUE left/right edge crossings (edge_fx_fl interpolates in y only)
// over the integer pixels [XStart, XEnd), a per-span sub-pixel offset GX's plane
// interpolation cannot reproduce. So GxAccurate replays the CPU's shape engine
// (rasterize.cpp: iround 28.4 vertices, Ceil28_4 / FloorDivMod DDA, sort_verts,
// shape_engine; same float expressions in the same order) and draws each span as a
// one-row GX quad whose end attributes are the CPU's span-start value and slope,
// pulled back half a pixel because GX samples pixel centres. Coverage is then the
// CPU's exactly (integer span ends), and u/w, v/w, 1/w go through the projective
// NRM texgen so GX's per-pixel s/q is the CPU's (u/w)*(1/(1/w)).
// ---------------------------------------------------------------------------------
struct GxDs3dSpanVert { float x, y, z, u, v, w; };
// GX (and Dolphin, which reproduces it) samples a quad's pixel x at x + 7/12, not x + 1/2
// (gx_ds_engine_impl.inc's kGxSamplePoint, tuned to 149/256 on the 2D engines). With 1/2
// here, a3_c30 kept 444 px on whole columns, every one GX = the CPU's next texel.
static const float kGxDs3dSpanSamplePoint = 149.0f / 256.0f;   // x, y in 28.4 (as rasterize.cpp keeps them)

static inline int gxDs3dCeil28_4(int value)
{
	const int n = value - 1 + 16;
	if (n >= 0) return n / 16;
	int r = -((-n) / 16);
	r -= ((-n) % 16) ? 1 : 0;
	return r;
}

static inline void gxDs3dFloorDivMod(long num, long den, long &fl, long &mod, bool &failure)
{
	if (den <= 0) failure = true;
	if (num >= 0) { fl = num / den; mod = num % den; }
	else {
		fl = -((-num) / den); mod = (-num) % den;
		if (mod) { fl--; mod = den - mod; }
	}
}

struct GxDs3dEdge {
	long X, XStep, Numerator, Denominator, ErrorTerm;
	int Y, Height;
	float curr[4], step[4];   // invw, z, u, v (edge_fx_fl's dx is always 0)

	void init(const GxDs3dSpanVert *top, const GxDs3dSpanVert *bot, bool &failure)
	{
		Y = gxDs3dCeil28_4((int)top->y);
		const int YEnd = gxDs3dCeil28_4((int)bot->y);
		Height = YEnd - Y;
		if (!Height) return;
		const long dN = long(bot->y - top->y);
		const long dM = long(bot->x - top->x);
		const long InitialNumerator = (long)(dM * 16 * Y - dM * top->y + dN * top->x - 1 + dN * 16);
		gxDs3dFloorDivMod(InitialNumerator, dN * 16, X, ErrorTerm, failure);
		gxDs3dFloorDivMod(dM * 16, dN * 16, XStep, Numerator, failure);
		Denominator = dN * 16;
		const float YPrestep = (float)((int)(Y * 16 - top->y)) / 16.0f;
		const float XPrestep = (float)((int)(X * 16 - top->x)) / 16.0f;
		const float dy = 1 / ((float)dN / 16.0f);
		const float t[4] = { 1 / top->w, top->z, top->u, top->v };
		const float b[4] = { 1 / bot->w, bot->z, bot->u, bot->v };
		for (int k = 0; k < 4; ++k) {
			const float dyk = dy * (b[k] - t[k]);
			curr[k] = t[k] + YPrestep * dyk + XPrestep * 0.0f;
			step[k] = XStep * 0.0f + dyk;
		}
	}
	void doStep()
	{
		X += XStep; Y++; Height--;
		for (int k = 0; k < 4; ++k) curr[k] += step[k];
		ErrorTerm += Numerator;
		if (ErrorTerm >= Denominator) { X++; ErrorTerm -= Denominator; }
	}
};

static void gxDs3dEmitSpan(const GxDs3dEdge &l, const GxDs3dEdge &r, const VERT &cv, bool textured)
{
	const int x0 = (int)l.X;
	const int width = (int)(r.X - l.X);
	if (width <= 0 || l.Y < 0 || l.Y > 191) return;   // rasterize.cpp draws nothing either
	const float invWidth = 1.0f / width;
	float a[4], d[4];
	for (int k = 0; k < 4; ++k) {
		d[k] = (r.curr[k] - l.curr[k]) * invWidth;
#ifdef DSA_GXGEOM_MUTATE_TEXSPANSHIFT
		a[k] = l.curr[k];   // mutation: no half-pixel pull-back, must fail a3_c30
#else
		a[k] = l.curr[k] - kGxDs3dSpanSamplePoint * d[k];
#endif
	}
	const float x1 = (float)(x0 + width), y0 = (float)l.Y, y1 = y0 + 1.0f;
	float e[4];
	for (int k = 0; k < 4; ++k) e[k] = a[k] + (float)width * d[k];
	// NRM = (u/w, v/w, 1/w); position z = -z (see gxDs3dRenderAccurate's gxz note).
	// Untextured spans (clipped polygons) have no NRM in the vertex descriptor.
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position3f32((float)x0, y0, -a[1]); if (textured) GX_Normal3f32(a[2], a[3], a[0]); gxDs3dSendColor(cv);
		GX_Position3f32(x1, y0, -e[1]);        if (textured) GX_Normal3f32(e[2], e[3], e[0]); gxDs3dSendColor(cv);
		GX_Position3f32(x1, y1, -e[1]);        if (textured) GX_Normal3f32(e[2], e[3], e[0]); gxDs3dSendColor(cv);
		GX_Position3f32((float)x0, y1, -a[1]); if (textured) GX_Normal3f32(a[2], a[3], a[0]); gxDs3dSendColor(cv);
	GX_End();
}

// rasterize.cpp's backface test and PolyAttr::isVisible (polygon attr bits 6-7: show back /
// front), on the viewport-transformed float coordinates (before the 28.4 conversion).
static bool gxDs3dPolyVisible(const POLY &p, const GxDs3dSpanVert *v, int type, bool &backfacing)
{
	const int n = type - 1;
	float facing = (v[0].y + v[n].y) * (v[0].x - v[n].x)
	             + (v[1].y + v[0].y) * (v[1].x - v[0].x)
	             + (v[2].y + v[1].y) * (v[2].x - v[1].x);
	for (int j = 2; j < n; j++)
		facing += (v[j + 1].y + v[j].y) * (v[j + 1].x - v[j].x);
	backfacing = facing < 0;
	switch ((p.polyAttr >> 6) & 3) {
		case 0: return false;
		case 1: return backfacing;
		case 2: return !backfacing;
		default: return true;
	}
}

// cv/type: the polygon as rasterize.cpp draws it (gxDs3dPolyVerts, clipped or not).
static void gxDs3dDrawSpansAccurate(const POLY &p, const VIEWPORT &vp, const VERT *cv, int type, bool textured)
{
	GxDs3dSpanVert sv[MAX_CLIPPED_VERTS];
	for (int j = 0; j < type; ++j) {
		const VERT &v = cv[j];
		const float w = v.coord[3];
		GxDs3dSpanVert &o = sv[j];
		o.x = (v.coord[0] + w) / (2 * w);
		o.y = (v.coord[1] + w) / (2 * w);
		o.z = (v.coord[2] + w) / (2 * w);
		o.u = v.texcoord[0] / w;
		o.v = v.texcoord[1] / w;
		o.w = w;
		o.x *= vp.width;  o.x += vp.x;
		o.y *= vp.height; o.y += vp.y;
		o.y = 192 - o.y;
		o.x = fmaxf(0.0f, fminf(256.0f, o.x));
		o.y = fmaxf(0.0f, fminf(192.0f, o.y));
	}
	bool backfacing;
	if (!gxDs3dPolyVisible(p, sv, type, backfacing)) return;
	{
		for (int j = 0; j < type; ++j) {
			sv[j].x = (float)(int)(16.0f * sv[j].x);
			sv[j].y = (float)(int)(16.0f * sv[j].y);
		}
		// shape_engine(type, !backfacing): sort_verts reverses when "backwards".
		const GxDs3dSpanVert *verts[MAX_CLIPPED_VERTS];
		for (int j = 0; j < type; ++j) verts[j] = &sv[j];
		if (!backfacing)
			for (int j = 0; j < type / 2; ++j) { const GxDs3dSpanVert *t = verts[j]; verts[j] = verts[type - j - 1]; verts[type - j - 1] = t; }
		for (;;) {
			bool swap = false;
			for (int j = 1; j < type; ++j) if (verts[0]->y > verts[j]->y) { swap = true; break; }
			if (!swap) break;
			for (int j = 1; j < type; ++j) { const GxDs3dSpanVert *t = verts[j - 1]; verts[j - 1] = verts[j]; verts[j] = t; }
		}
		while (verts[0]->y == verts[1]->y && verts[0]->x > verts[1]->x)
			for (int j = 1; j < type; ++j) { const GxDs3dSpanVert *t = verts[j - 1]; verts[j - 1] = verts[j]; verts[j] = t; }

		const VERT &fv = gfx3d.vertlist->list[p.vertIndexes[0]];   // flat colour (gxDs3dTexColorExact / clipcolor gate)
		bool failure = false;
		int lv = type, rv = 0;
		GxDs3dEdge left, right;
		bool stepLeft = true, stepRight = true;
		for (;;) {
			const int _lv = lv == type ? 0 : lv;
			if (stepLeft)  left.init(verts[_lv], verts[lv - 1], failure);
			if (stepRight) right.init(verts[rv], verts[rv + 1], failure);
			stepLeft = stepRight = false;
			if (failure) return;
			int h = left.Height < right.Height ? left.Height : right.Height;
			while (h--) {
				gxDs3dEmitSpan(left, right, fv, textured);
				left.doStep();
				right.doStep();
			}
			if (right.Height == 0) { stepRight = true; rv++; }
			if (left.Height == 0)  { stepLeft = true; lv--; }
			if (lv <= rv + 1) break;
		}
	}
}

// ---------------------------------------------------------------------------------
// GxAccurate: CPU replicates rasterize.cpp's own homogeneous-divide + viewport
// transform exactly (same formulas, same order), so the resulting screen-space
// position is byte-for-byte what the CPU rasterizer would compute for the same
// vertex. GX is fed plain screen-pixel coordinates under the same orthographic
// screen-space projection already used for 2D content elsewhere in this codebase
// (gxDsBSetup2DState's guOrtho(0,kDsBScreenH,0,kDsBScreenW,0,1) pattern) and an
// identity PNMTX -- see gx_ds_3d_render.h's deviation note for why this replaces
// the design doc's original "feed GX a caller-supplied clip-space w" mechanism
// (GX has no such vertex attribute).
// ---------------------------------------------------------------------------------
void gxDs3dRenderAccurate()
{
	gxDs3dSetupCommonState();
	gxDs3dLoadScreenOrtho();
	gxDs3dClearDepth();

	const int polycount = gfx3d.polylist->count;
	GxDs3dTexState st = {};
	bool first = true;
#ifdef DSA_GXGEOM_DEBUGWHY
	harness_profile_emitf("gxds3dACC enter polycount=%d", polycount);
#endif
	for (int n = 0; n < polycount; ++n) {
		const int i = gfx3d.indexlist[n];   // rasterize.cpp's order (gfx3d_doFlush)
		const POLY &p = gfx3d.polylist->list[i];
		VIEWPORT vp;
		vp.decode(p.viewport);
#ifdef DSA_GXGEOM_DEBUGWHY
		harness_profile_emitf("gxds3dACC poly i=%d type=%d vp=%d,%d,%d,%d", i, p.type, vp.x, vp.y, vp.width, vp.height);
#endif

		if (gxDs3dPolyInvisible(p)) continue;
		VERT cv[MAX_CLIPPED_VERTS];
		bool clipped;
		const int nv = gxDs3dPolyVerts(gfx3d.polylist->list[i], cv, clipped);
		if (nv < 3) continue;   // clipped away
		gxDs3dBindPoly(p, true, st, first);
		first = false;
		if (st.textured || clipped) {
			gxDs3dDrawSpansAccurate(p, vp, cv, nv, st.textured);
			continue;
		}
		GX_Begin(p.type == 4 ? GX_QUADS : GX_TRIANGLES, GX_VTXFMT0, p.type);
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			// rasterize.cpp's own formulas, verbatim (see gfx3d.h/rasterize.cpp's
			// clipping+viewport step): homogeneous divide remaps [-1,1] to [0,1],
			// then the polygon's own glViewport rect, then the DS's Y flip
			// (screen row 0 = top; see the .h file / log section for why GX's own
			// clip-space Y convention needs this same flip when GX does the divide
			// itself, which is exactly what makes this an *empirical* confirmation
			// of that finding once the fixture's silhouette lands in the right place).
			float sx = (v.coord[0] + v.coord[3]) / (2.0f * v.coord[3]);
			float sy = (v.coord[1] + v.coord[3]) / (2.0f * v.coord[3]);
			float sz = (v.coord[2] + v.coord[3]) / (2.0f * v.coord[3]);
			sx = sx * (float)vp.width + (float)vp.x;
			sy = sy * (float)vp.height + (float)vp.y;
			sy = 192.0f - sy;
			if (sx < 0.0f) sx = 0.0f; else if (sx > 256.0f) sx = 256.0f;
			if (sy < 0.0f) sy = 0.0f; else if (sy > 192.0f) sy = 192.0f;
			// rasterize.cpp's edge walker (Ceil28_4) samples each pixel at its top-left
			// corner; GX samples at the pixel centre, so shift by +0.5. Deliberately NOT
			// snapped to the CPU's truncated 1/16 grid: measured on a3_c28, that snap
			// makes GX break sample-on-edge ties differently (29 bad px vs 15 unsnapped).
			sx += kGxDs3dSampleOffset;
			sy += kGxDs3dSampleOffset;
			// Found this pass (see gx-next-steps-log.md's Task 13e-precision section):
			// guOrtho(mt,t,b,l,r,n,f)'s "-z axis" convention (per libogc's own gu.h doc
			// comment) means the visible eye-space Z range for near=0,far=1 is [-1,0],
			// NOT [0,1] -- feeding the DS's natural 0=near/1=far sz straight into
			// GX_Position3f32 (as this function did before this pass) puts every vertex
			// OUTSIDE that visible range, so nothing was ever actually rasterized: the
			// whole geometry-overlay draw was silently a no-op on real Dolphin, verified
			// with a forced full-screen probe triangle (0 pixels visible with positive Z,
			// full coverage with negated Z). Negating maps sz's [0,1] onto exactly the
			// [-1,0] guOrtho expects.
			const float gxz = -sz;

#ifdef DSA_GXGEOM_DEBUGWHY
			harness_profile_emitf("gxds3dACC vtx i=%d j=%d sx=%d sy=%d gxz=%d col=%d,%d,%d",
			                      i, j, (int)sx, (int)sy, (int)(gxz * 1000.0f),
			                      v.color[0], v.color[1], v.color[2]);
#endif
			GX_Position3f32(sx, sy, gxz);
			gxDs3dSendColor(v);
		}
		GX_End();
	}
	gxDs3dRestoreState();
}

// ---------------------------------------------------------------------------------
// GxFast: GX's transform hardware does the multiply + perspective divide against
// VERT::objcoord, using the polygon's own mvMatrix/projMatrix snapshot (a later
// polygon can have different matrices) folded per gxDs3dFastXformBuild(). No Y flip:
// GX and the DS both put NDC y=+1 at the top of the viewport. The DS viewport
// (bottom-left origin) becomes a GX viewport (top-left origin).
//
// Polygon-ID tag pass (gxDs3dTransIdPlan's runs). For a tagged run of ID X (nothing before
// it stamped X on its pixels, the plan guarantees that), rasterize.cpp draws a pixel from
// the FIRST run polygon whose fragment there passes depth (against the depth before the
// run: the only earlier writer at that pixel would be that same first polygon) and alpha
// (0 < a; the plan excludes a == 31), and drops every later one. So before the run:
//  1. copy the EFB colour out (RGBA8, exact for the RGB8 EFB), clear it to tag 0;
//  2. draw the run in REVERSE order, no blend, depth test LESS without update, writing the
//     polygon's 1-based position as colour (R = low byte, B = high byte), same alpha
//     compare as its real draw: each pixel ends with the lowest winning position;
//  3. copy R and B out as I8 tag textures, draw the saved colour back;
// then draw the run normally, with a TEV stage per tag byte that zeroes the alpha unless
// the tag texel (sampled at the fragment's own screen position: TEXMTX1 maps the position
// the polygon submits to screen pixel centres) equals the polygon's position, so the alpha
// compare drops the fragment's colour and depth exactly where the DS would. Same geometry,
// same state, so coverage and depth results of the two draws are identical. Needs an 8-bit
// R/B EFB (RGB8_Z24): RGB565 (rmode->aa) bails `transid`, RGBA6 only runs with holes, and a
// frame with translucent polygons has none. Buffers (288 KB) live only for the render.
// ---------------------------------------------------------------------------------
struct GxDs3dFastCtx {
	float lastMv[16], lastProj[16];
	u32 lastVp;
	bool haveLast, first, blendOn, ortho, wbuf;
	int curMtx;            // 0: PNMTX0 (the polygon's transform), 1: PNMTX1 (identity, clipped)
	GxDs3dTexState st;
	Mtx pos;               // PNMTX0's matrix (tag texgen)
	VIEWPORT vp;
	int tagMode;           // TEV/texgen set up for: 0 plain, 1 tag write, 2 tag test; -1 unknown
	bool tagTextured, tagTwo;
};

static void *s_tagSaveBuf = nullptr, *s_tagLoBuf = nullptr, *s_tagHiBuf = nullptr;
static GXTexObj s_tagSaveTex, s_tagLoTex, s_tagHiTex;

static bool gxDs3dTagBuffers()
{
	if (s_tagSaveBuf) return true;
	const u32 cb = GX_GetTexBufferSize(kScreenW, kScreenH, GX_TF_RGBA8, GX_FALSE, 0);
	const u32 ib = GX_GetTexBufferSize(kScreenW, kScreenH, GX_TF_I8, GX_FALSE, 0);
	s_tagSaveBuf = memalign(32, cb);
	s_tagLoBuf = memalign(32, ib);
	s_tagHiBuf = memalign(32, ib);
	if (!s_tagSaveBuf || !s_tagLoBuf || !s_tagHiBuf) {
		free(s_tagSaveBuf); free(s_tagLoBuf); free(s_tagHiBuf);
		s_tagSaveBuf = s_tagLoBuf = s_tagHiBuf = nullptr;
		return false;
	}
	// EFB copy targets: no dirty line of the heap's previous user may be written back over them.
	DCInvalidateRange(s_tagSaveBuf, cb);
	DCInvalidateRange(s_tagLoBuf, ib);
	DCInvalidateRange(s_tagHiBuf, ib);
	GX_InitTexObj(&s_tagSaveTex, s_tagSaveBuf, kScreenW, kScreenH, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&s_tagLoTex, s_tagLoBuf, kScreenW, kScreenH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&s_tagHiTex, s_tagHiBuf, kScreenW, kScreenH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_tagSaveTex, GX_NEAR, GX_NEAR);
	GX_InitTexObjFilterMode(&s_tagLoTex, GX_NEAR, GX_NEAR);
	GX_InitTexObjFilterMode(&s_tagHiTex, GX_NEAR, GX_NEAR);
	return true;
}

static void gxDs3dTagBuffersFree()
{
	if (!s_tagSaveBuf) return;
	GX_DrawDone();   // the FIFO may still read them
	free(s_tagSaveBuf); free(s_tagLoBuf); free(s_tagHiBuf);
	s_tagSaveBuf = s_tagLoBuf = s_tagHiBuf = nullptr;
	GX_InvalidateTexAll();
}

// TEV stage 1 (and 2) after gxDs3dBindPoly's stage 0, per tagMode (see the section comment).
static void gxDs3dTagTev(int mode, bool textured, bool two)
{
	if (mode == 0) {
		GX_SetNumTevStages(1);
		GX_SetNumTexGens(textured ? 1 : 0);
		return;
	}
	if (mode == 1) {
		GX_SetNumTexGens(textured ? 1 : 0);
		GX_SetNumTevStages(2);
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K0);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		return;
	}
	const u8 tc = textured ? GX_TEXCOORD1 : GX_TEXCOORD0;
	GX_SetNumTexGens(textured ? 2 : 1);
	GX_SetTexCoordGen(tc, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX1);
	GX_SetNumTevStages(two ? 3 : 2);
	for (int k = 0; k < (two ? 2 : 1); ++k) {
		const u8 stage = k ? GX_TEVSTAGE2 : GX_TEVSTAGE1;
		GX_SetTevOrder(stage, tc, k ? GX_TEXMAP2 : GX_TEXMAP1, GX_COLORNULL);
		GX_SetTevKAlphaSel(stage, k ? GX_TEV_KASEL_K1_A : GX_TEV_KASEL_K0_A);
		GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
		GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		// alpha = (tag texel == this polygon's tag byte) ? alpha : 0
		GX_SetTevAlphaIn(stage, GX_CA_TEXA, GX_CA_KONST, GX_CA_APREV, GX_CA_ZERO);
		GX_SetTevAlphaOp(stage, GX_TEV_COMP_A8_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
}

// TEXMTX1: the position a polygon submits -> (s*q, t*q, q) with s, t its screen pixel centre
// in the 256x192 tag texture (gxDs3dScreenXY's mapping, which GX's viewport, offset by
// kGxDs3dSampleOffset, puts at the pixel centre).
static void gxDs3dTagTexMtx(const GxDs3dFastCtx &c, bool clipped)
{
	float C[3][4];
	memset(C, 0, sizeof(C));
	if (clipped) {
		C[0][0] = 1; C[1][1] = 1;
		if (c.ortho) C[2][3] = 1; else C[2][2] = -1;   // submitted (X, Y, -W) or (X/W, Y/W, Z/W)
	} else {
		for (int k = 0; k < 4; ++k) {
			C[0][k] = c.pos[0][k];
			C[1][k] = c.pos[1][k];
			C[2][k] = c.ortho ? (k == 3 ? 1.0f : 0.0f) : -c.pos[2][k];
		}
	}
	const float hw = 0.5f * (float)c.vp.width, hh = 0.5f * (float)c.vp.height;
	const float ox = (float)c.vp.x + hw + 0.5f, oy = (float)(kScreenH - c.vp.y) - hh + 0.5f;
	Mtx T;
	for (int k = 0; k < 4; ++k) {
		T[0][k] = (ox * C[2][k] + hw * C[0][k]) / (float)kScreenW;
		T[1][k] = (oy * C[2][k] - hh * C[1][k]) / (float)kScreenH;
		T[2][k] = C[2][k];
	}
	GX_LoadTexMtxImm(T, GX_TEXMTX1, GX_MTX3x4);
}

// tag: 0 plain draw; > 0 test this tag (the run's real draw); < 0 write tag -tag (pass 2).
static void gxDs3dFastPoly(GxDs3dFastCtx &c, POLY &p, int tag)
{
	if (gxDs3dPolyInvisible(p)) return;
	VERT cv[MAX_CLIPPED_VERTS];
	bool clipped;
	const int nv = gxDs3dPolyVerts(p, cv, clipped);
	if (clipped) {   // degenerate (through the eye), see gxDs3dFrameGate
		bool bad = false;
		for (int j = 0; j < nv; ++j) bad |= cv[j].coord[3] <= 0.0f;
		if (bad) return;
	}
	// Back/front-face culling (POLYGON_ATTR bits 6-7) on the CPU's own (clipped) screen
	// outline; false also when clipped away.
	GxDs3dTransShape shape;
	if (!gxDs3dTransShapeBuild(p, cv, nv, shape)) return;
	const bool trans = p.isTranslucent();
	const bool tagWrite = tag < 0;
	u8 va = 255;
	bool zw = true, twoPass = false;
	if (trans) {
		if (!gxDs3dTransDraws(p)) return;
		const bool texTrans = gxDs3dTexFmtTrans(gxDs3dTexFormat(p));
		if (!texTrans) va = gxDs3dGxAlpha(gxDs3dPolyAlpha(p));
		zw = (p.polyAttr >> 11) & 1;
#ifdef DSA_GXGEOM_MUTATE_TRANSZW
		zw = true;   // mutation: translucent polygons always write depth, must fail a3_c34
#endif
		twoPass = !zw && texTrans && gxDs3dPolyAlpha(p) == 31;
		if (!c.blendOn) {
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
			c.blendOn = true;
		}
	}

	if (!c.haveLast || p.viewport != c.lastVp) {
		c.vp.decode(p.viewport);
		GX_SetViewport((f32)c.vp.x + kGxDs3dSampleOffset,
		               (f32)(kScreenH - c.vp.y - c.vp.height) + kGxDs3dSampleOffset,
		               (f32)c.vp.width, (f32)c.vp.height, 0, 1);
		c.lastVp = p.viewport;
	}
	if (!c.haveLast || memcmp(c.lastMv, p.mvMatrix, sizeof(c.lastMv)) != 0 ||
	    memcmp(c.lastProj, p.projMatrix, sizeof(c.lastProj)) != 0) {
		GxDs3dFastXform x;
		if (!gxDs3dFastXformBuild(p, x, c.wbuf, s_wK))
			return;   // unreachable: gxDs3dGeomFrameSupported() checked every poly
		GX_LoadPosMtxImm(x.pos, GX_PNMTX0);
		GX_LoadProjectionMtx(x.proj, x.projType);
		c.ortho = x.projType == GX_ORTHOGRAPHIC;
		memcpy(c.pos, x.pos, sizeof(c.pos));

		memcpy(c.lastMv, p.mvMatrix, sizeof(c.lastMv));
		memcpy(c.lastProj, p.projMatrix, sizeof(c.lastProj));
		c.haveLast = true;
		c.curMtx = -1;
	}
	// Clipped polygons: clip-space positions under PNMTX1 = identity (see the clipped section).
	if (c.curMtx != (int)clipped) {
		GX_SetCurrentMtx(clipped ? GX_PNMTX1 : GX_PNMTX0);
		c.curMtx = clipped;
	}

	const bool textured = gxDs3dTexFormat(p) != 0;
	const bool rebound = c.first || textured != c.st.textured;
	gxDs3dBindPoly(p, false, c.st, c.first);
	c.first = false;
	const int mode = tag == 0 ? 0 : (tagWrite ? 1 : 2);
	if (rebound || mode != c.tagMode || (mode && textured != c.tagTextured)) {
		gxDs3dTagTev(mode, textured, c.tagTwo);
		c.tagMode = mode;
		c.tagTextured = textured;
	}
	if (mode) {
		const int t = tagWrite ? -tag : tag;
		const GXColor k0 = { (u8)(t & 255), 0, (u8)(t >> 8), (u8)(t & 255) };
		const GXColor k1 = { 0, 0, 0, (u8)(t >> 8) };
		GX_SetTevKColor(GX_KCOLOR0, k0);
		GX_SetTevKColor(GX_KCOLOR1, k1);
		if (mode == 2) gxDs3dTagTexMtx(c, clipped);
	}
	for (int pass = twoPass ? 0 : 1; pass < 2; ++pass) {
		if (trans) {
			// pass 0: the a == 31 fragments of a non-depth-writing polygon (opaque
			// writes, with depth); pass 1: everything else it draws.
			GX_SetZMode(GX_TRUE, GX_LESS, (!tagWrite && (pass == 0 || zw)) ? GX_TRUE : GX_FALSE);
			if (pass == 0)    GX_SetAlphaCompare(GX_EQUAL, 255, GX_AOP_AND, GX_ALWAYS, 0);
			else if (twoPass) GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_LESS, 255);
			else              GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
		}
		if (clipped) {
			GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, nv);
			for (int j = 0; j < nv; ++j) {
				const VERT &v = cv[j];
				const float w = v.coord[3];
				if (c.ortho) GX_Position3f32(v.coord[0] / w, v.coord[1] / w, v.coord[2] / w);
				else         GX_Position3f32(v.coord[0], v.coord[1], -w);
#ifdef DSA_GXGEOM_PROBE
				gxDs3dSendColor(v, va);
#else
				GX_Color4u8(gxDs3dF6To8(v.fcolor[0]), gxDs3dF6To8(v.fcolor[1]), gxDs3dF6To8(v.fcolor[2]), va);
#endif
				if (c.st.textured) GX_TexCoord2f32(v.texcoord[0], v.texcoord[1]);
			}
			GX_End();
			continue;
		}
		GX_Begin(p.type == 4 ? GX_QUADS : GX_TRIANGLES, GX_VTXFMT0, p.type);
		for (int j = 0; j < p.type; ++j) {
			const VERT &v = gfx3d.vertlist->list[p.vertIndexes[j]];
			GX_Position3f32(v.objcoord[0], v.objcoord[1], v.objcoord[2]);
			gxDs3dSendColor(v, va);
			if (c.st.textured) GX_TexCoord2f32(v.texcoord[0], v.texcoord[1]);
		}
		GX_End();
	}
}

// A full-screen quad at the pass's EFB origin: the clear colour (tex NULL) or tex, no Z, no blend.
static void gxDs3dTagScreenQuad(GXTexObj *tex)
{
	gxDs3dLoadScreenOrtho();
	GX_SetViewport(0, 0, (f32)kScreenW, (f32)kScreenH, 0, 1);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetNumTevStages(1);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	if (tex) {
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GX_LoadTexObj(tex, GX_TEXMAP3);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP3, GX_COLORNULL);
		GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	} else {
		GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GX_SetNumTexGens(0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	static const float xy[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	for (int j = 0; j < 4; ++j) {
		GX_Position3f32(xy[j][0] * kScreenW, xy[j][1] * kScreenH, 0);
		if (tex) GX_TexCoord2f32(xy[j][0], xy[j][1]);
		else     GX_Color4u8(0, 0, 0, 255);
	}
	GX_End();
}

static void gxDs3dTagCopy(void *dst, u32 fmt)
{
	GX_SetTexCopySrc(0, 0, kScreenW, kScreenH);
	GX_SetTexCopyDst(kScreenW, kScreenH, fmt, GX_FALSE);
	GX_CopyTex(dst, GX_FALSE);
}

// Steps 1-3 of the section comment for tagged run r; leaves c forcing a full rebind.
static void gxDs3dTagRunPrepass(GxDs3dFastCtx &c, int r)
{
	const GxDs3dTagRun &run = s_tagRuns[r];
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	gxDs3dTagCopy(s_tagSaveBuf, GX_TF_RGBA8);
	gxDs3dTagScreenQuad(NULL);

	c.first = true; c.haveLast = false; c.curMtx = -1; c.blendOn = false; c.tagMode = -1;
	c.tagTwo = run.count > 255;
	GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	c.blendOn = true;   // i.e. "blend state owned here": gxDs3dFastPoly must not turn it on
#ifdef DSA_GXGEOM_MUTATE_TAGFWD
	for (int n = run.n0; n <= run.n1; ++n) {   // mutation: the LAST winning polygon keeps the pixel
#else
	for (int n = run.n1; n >= run.n0; --n) {
#endif
		const int i = gfx3d.indexlist[n];
		if (s_tagRun[i] == r) gxDs3dFastPoly(c, gfx3d.polylist->list[i], -(int)s_tagIdx[i]);
	}
	gxDs3dTagCopy(s_tagLoBuf, GX_CTF_R8);
	if (c.tagTwo) gxDs3dTagCopy(s_tagHiBuf, GX_CTF_B8);
	GX_PixModeSync();
	GX_InvalidateTexAll();
	gxDs3dTagScreenQuad(&s_tagSaveTex);
	GX_LoadTexObj(&s_tagLoTex, GX_TEXMAP1);
	GX_LoadTexObj(&s_tagHiTex, GX_TEXMAP2);

	c.first = true; c.haveLast = false; c.curMtx = -1; c.blendOn = false; c.tagMode = -1;
}

void gxDs3dRenderFast()
{
	gxDs3dWSetup();
	gxDs3dSetupCommonState();
	gxDs3dLoadScreenOrtho();
	gxDs3dClearDepth();

	GxDs3dFastCtx c;
	memset(&c, 0, sizeof(c));
	c.first = true;
	c.curMtx = -1;
	c.tagMode = -1;
	c.wbuf = gfx3d.wbuffer != 0;
	{
		Mtx id;
		guMtxIdentity(id);
		GX_LoadPosMtxImm(id, GX_PNMTX1);
	}
	int curRun = 0;
	bool tags = s_tagRunCount > 0 && gxDs3dTagBuffers();
#ifdef DSA_GXGEOM_MUTATE_TAGOFF
	tags = false;   // mutation: tagged runs drawn plainly (every same-ID layer blends), must fail a3_c36/46
#endif
	const int polycount = gfx3d.polylist->count;
	for (int n = 0; n < polycount; ++n) {
		// gfx3d.indexlist: opaque polygons first, then the translucent ones in the order
		// rasterize.cpp draws them (see the translucent section).
		const int i = gfx3d.indexlist[n];
		POLY &p = gfx3d.polylist->list[i];
		const int r = (tags && p.isTranslucent()) ? s_tagRun[i] : 0;
		if (r && r != curRun) {
			gxDs3dTagRunPrepass(c, r);
			curRun = r;
		}
		gxDs3dFastPoly(c, p, r ? (int)s_tagIdx[i] : 0);
	}
	if (c.blendOn) {
		GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		GX_SetZMode(GX_TRUE, GX_LESS, GX_TRUE);
	}
	if (c.tagMode > 0) GX_SetNumTevStages(1);
	gxDs3dTagBuffersFree();
	gxDs3dRestoreState();
}

// ---------------------------------------------------------------------------------
// Task 13f-fog: fog post-process, GxFast only (see gx_ds_3d_render.h's comment on
// gxDs3dApplyFogFast() and gx-13f-design.md section 2 for the full precision writeup).
// ---------------------------------------------------------------------------------

// Mirrors rasterize.cpp's own fast fogTable[] build (~line 1268) exactly -- this is the
// CPU's fully-resolved 15-bit-indexed LUT; GX never redoes the density interpolation
// itself, it only samples this already-correct table (downsampled to 8 bits below).
static u8 s_fogTable[32768];

static void gxDs3dFogBuildTable()
{
	const u8 *fogDensity = MMU.MMU_MEM[ARMCPU_ARM9][0x40] + 0x360;
	const int increment = ((1 << 10) >> gfx3d.fogShift);
	const int incrementDivShift = 10 - gfx3d.fogShift;
	u32 fogOffset = gfx3d.fogOffset;
	if (fogOffset > 32768) fogOffset = 32768;
	u32 iMin = ((1u + 1u) << incrementDivShift) + fogOffset + 1u - (u32)increment;
	u32 iMax = ((32u + 1u) << incrementDivShift) + fogOffset + 1u - (u32)increment;
	if (iMin > 32768) iMin = 32768;
	if (iMax > 32768) iMax = 32768;
	if (iMin > iMax) iMin = iMax;   // defensive; CPU asserts this never happens
	memset(s_fogTable, fogDensity[0], iMin);
	for (u32 i = iMin; i < iMax; i++) {
		int num = (int)(i - fogOffset) + (increment - 1);
		int j = (num >> incrementDivShift) - 1;
		if (j < 1) j = 1;
		if (j > 31) j = 31;
		u32 value = (u32)(num & ~(increment - 1)) + fogOffset;
		u32 diff = value - i;
		s_fogTable[i] = (u8)(((u32)diff * fogDensity[j - 1] + (u32)((u32)increment - diff) * fogDensity[j]) >> incrementDivShift);
	}
	memset(s_fogTable + iMax, fogDensity[31], 32768 - iMax);
}

// 256-entry indirect-lookup LUT: texel k holds the blend weight (0-255, i.e. the CPU's
// 0-128 fog value with the 127->128 snap applied, rescaled *2 so it can be fed straight
// to GX's hardware alpha blender instead of a manual TEV lerp -- see gxDs3dApplyFogFast).
// Indexed by the TOP 8 BITS of the GX Z-buffer's 24-bit depth, i.e. one entry per 128
// consecutive real fogIndex values (fogIndex = depth>>9 is 15-bit) -- the precision loss
// gx-13f-design.md section 2 characterizes as the real, accepted GxFast error source.
// I8's GX block shape is 8 wide x 4 tall (gx_texformat.h) -- a texture's stored bytes
// are tiled into 8x4 blocks, not row-major linear, so the LUT's actual texture height
// must be a multiple of 4 and its bytes swizzled with gxSwizzle8bpp (found this pass:
// a naive height=1 texture with a flat memcpy silently sampled as all-zero/garbage on
// real Dolphin -- same lesson class as the GX_TF_A8-vs-I8 sample-format trap this
// codebase already learned, just the tiling half of it instead of the format half).
// Four identical rows (v is always 0 at sample time, see gxDs3dApplyFogFast) satisfy
// the block-height requirement without adding a second real dimension of data.
static const int kFogLutH = 4;
static u8 s_fogLut[256];
static u8 s_fogLutLinear[256 * kFogLutH];
static GXTexObj s_fogLutTex;
static void *s_fogLutTexData = nullptr;
static bool s_fogLutValid = false;
// Cache key: only rebuild the (somewhat expensive, 32768-entry) table when something it
// depends on actually changed -- FOG_TABLE's 32 density bytes, fogOffset, fogShift, or
// fogColor (color doesn't affect the table, but is cheap to fold into the same key so a
// single memcmp covers "does the LUT texture need re-uploading" completely).
static u8 s_fogKey[32 + 4 + 4 + 4];

static void gxDs3dFogEnsureLut()
{
	u8 key[sizeof(s_fogKey)];
	memcpy(key, MMU.MMU_MEM[ARMCPU_ARM9][0x40] + 0x360, 32);
	memcpy(key + 32, &gfx3d.fogOffset, 4);
	memcpy(key + 36, &gfx3d.fogShift, 4);
	memcpy(key + 40, &gfx3d.fogColor, 4);

	if (s_fogLutValid && memcmp(key, s_fogKey, sizeof(key)) == 0)
		return;

	gxDs3dFogBuildTable();
	for (int k = 0; k < 256; ++k) {
		u32 idx = (u32)k * 128 + 64;
		if (idx > 32767) idx = 32767;
		u32 fog = s_fogTable[idx];
		if (fog == 127) fog = 128;
		u32 weight = fog * 2;
		if (weight > 255) weight = 255;
		s_fogLut[k] = (u8)weight;
	}

	for (int row = 0; row < kFogLutH; ++row)
		memcpy(s_fogLutLinear + row * 256, s_fogLut, 256);

	if (!s_fogLutTexData) {
		u32 size = GX_GetTexBufferSize(256, kFogLutH, GX_TF_I8, GX_FALSE, 0);
		s_fogLutTexData = memalign(32, size);
	}
	if (s_fogLutTexData) {
		gxSwizzle8bpp(s_fogLutLinear, (u8 *)s_fogLutTexData, 8, 4, 256, kFogLutH);
		DCFlushRange(s_fogLutTexData, GX_GetTexBufferSize(256, kFogLutH, GX_TF_I8, GX_FALSE, 0));
		GX_InitTexObj(&s_fogLutTex, s_fogLutTexData, 256, kFogLutH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&s_fogLutTex, GX_NEAR, GX_NEAR);
	}
	memcpy(s_fogKey, key, sizeof(key));
	s_fogLutValid = true;
}

// EFB Z-copy target: top 8 bits of the 24-bit GX Z-buffer, one texel per screen pixel.
// GX_TF_Z8 (0x11) is a "for texture copy" Z format, not one of the CTF_* families the
// gx_mask.h lesson was written about, but the same low-nibble-only trap applies: 0x11's
// low nibble is 0x1 = GX_TF_I8, so the texture object must be told to SAMPLE as GX_TF_I8
// even though the EFB copy itself specifies GX_TF_Z8 as the destination format.
static GXTexObj s_fogZTex;
static void *s_fogZTexData = nullptr;

static void gxDs3dFogCopyZ(u16 w, u16 h)
{
	if (!s_fogZTexData) {
		u32 size = GX_GetTexBufferSize(w, h, GX_TF_I8, GX_FALSE, 0);
		s_fogZTexData = memalign(32, size);
	}
	if (!s_fogZTexData) return;

	GX_SetTexCopySrc(0, 0, w, h);
	GX_SetTexCopyDst(w, h, GX_TF_Z8, GX_FALSE);
	GX_CopyTex(s_fogZTexData, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	GX_InitTexObj(&s_fogZTex, s_fogZTexData, w, h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_fogZTex, GX_NEAR, GX_NEAR);
}

void gxDs3dApplyFogFast()
{
	gxDs3dFogEnsureLut();
	gxDs3dFogCopyZ(kScreenW, kScreenH);

	// gxDs3dRenderFast() leaves whichever per-polygon perspective matrix its last
	// polygon used loaded (GX_PNMTX0 / the projection register) -- this quad is fed
	// plain 0..256/0..192 screen-pixel positions and needs the same identity-PNMTX
	// screen-space ortho gxDs3dRenderAccurate() uses, not whatever GxFast left bound.
	// Found this pass: without this call the quad's positions go through the wrong
	// (arbitrary, per-polygon) transform and land outside the clip volume, so the
	// fog draw was a silent, invisible no-op -- verified via a PEEK probe showing the
	// unfogged raster colour unchanged after this function ran.
	gxDs3dLoadScreenOrtho();

	// Mask: only pixels the geometry pass actually wrote get fogged. gxDs3dClearDepth()
	// seeded every pixel in this region to the same reference Z before the geometry draw;
	// any pixel a triangle covered now holds a smaller (nearer) Z than that reference
	// (the geometry pass's own GX_LESS test guarantees this), so comparing the incoming
	// quad's Z (fed the same reference value) GX_GREATER against the buffer catches
	// exactly "something nearer was drawn here" without needing a polygon-ID buffer.
	const float clearZ = -(float)(gfx3d_rasterClearDepth() & 0xFFFFFF) / 16777215.0f;

	GX_LoadTexObj(&s_fogZTex, GX_TEXMAP0);
	GX_LoadTexObj(&s_fogLutTex, GX_TEXMAP1);

	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

	GX_SetNumIndStages(1);
	GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
	GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);
	// ds = 0.5 * (Z8-128) * 2^-7 : maps Z8 in [0,255] to a LUT texcoord offset spanning
	// almost exactly [0,1) around the base (0.5,0) coordinate fed via TEXCOORD1 below --
	// see gx-next-steps-log.md's Task 13f-fog section for the derivation.
	f32 indMtx[2][3] = { { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
	GX_SetIndTexMatrix(GX_ITM_0, indMtx, -7);

	GX_SetNumChans(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
#ifdef DSA_GXGEOM_MUTATE_FOGNOINDIRECT
	GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_S, GX_ITM_OFF,
	                   GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
#else
	GX_SetTevIndirect(GX_TEVSTAGE0, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_S, GX_ITM_0,
	                   GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
#endif

	GXColor fogGx;
	fogGx.r = (u8)(GFX3D_5TO6(gfx3d.fogColor & 0x1F) << 2);
	fogGx.g = (u8)(GFX3D_5TO6((gfx3d.fogColor >> 5) & 0x1F) << 2);
	fogGx.b = (u8)(GFX3D_5TO6((gfx3d.fogColor >> 10) & 0x1F) << 2);
	fogGx.a = 255;
	GX_SetTevKColor(GX_KCOLOR0, fogGx);
	GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);

	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
#ifdef DSA_GXGEOM_MUTATE_FOGCONSTALPHA
	GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1_2);   // debug: constant 1/2 alpha, no texture/indirect involved
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
#else
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
#endif
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

#ifdef DSA_GXGEOM_MUTATE_FOGNOZMASK
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);   // debug: bypass the geometry Z-mask entirely
#else
	GX_SetZMode(GX_TRUE, GX_GREATER, GX_FALSE);
#endif
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDither(GX_FALSE);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
		GX_Position3f32(0, 0, clearZ);
		GX_TexCoord2f32(0.0f, 0.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32((f32)kScreenW, 0, clearZ);
		GX_TexCoord2f32(1.0f, 0.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32((f32)kScreenW, (f32)kScreenH, clearZ);
		GX_TexCoord2f32(1.0f, 1.0f); GX_TexCoord2f32(0.5f, 0.0f);
		GX_Position3f32(0, (f32)kScreenH, clearZ);
		GX_TexCoord2f32(0.0f, 1.0f); GX_TexCoord2f32(0.5f, 0.0f);
	GX_End();

	// Restore what this draw doesn't own the caller relying on (mirrors
	// gxDs3dSetupCommonState's own contract): the next thing to run in
	// gx_ds_engine_impl.inc is gxDsBSetup2DState(), which sets its own num-tex-gens,
	// tev stages, chan count and vertex format from scratch, so nothing further needs
	// resetting here beyond leaving indirect stages at a sane count.
	GX_SetNumIndStages(0);
	GX_SetNumTexGens(1);
}
