#include "gx_gba_render.h"
#include "gx_color.h"
#include "gx_swizzle.h"
#include "gx_texformat.h"
#include "../gba_ppu.h"
#include "../MMU.h"
#include "../readwrite.h"
#include <gccore.h>
#include <malloc.h>
#include <string.h>

GxGbaBandRegs g_gbaBandRegs[GX_GBA_MAX_BAND_REGS];

// ---------------------------------------------------------------------
// Small local duplicates of gba_ppu.cpp's private VRAM/palette helpers.
// Not shared via a header on purpose: gba_ppu.cpp's renderScanline() path
// stays the untouched, independently-verified CPU reference implementation
// this file's output is meant to match, not a library this file builds on
// top of -- see gx_gba_render.h's header comment.
// ---------------------------------------------------------------------
static inline u16 gxBgPalColor(int idx) { return T1ReadWord(MMU.GBA_PALETTE, idx * 2); }
static inline u16 gxObjPalColor(int idx) { return T1ReadWord(MMU.GBA_PALETTE, 0x200 + idx * 2); }

static inline u32 gxVramAddr(u32 addr)
{
	u32 m = addr & 0x1FFFF;
	if (m >= 0x18000) m -= 0x8000;
	return m;
}

// RGB5A3 -> GBA-native BGR555 (GBA_screen's documented layout, gba_ppu.h).
// Exact inverse of gx_color.h's gxPackRGB5A3Opaque/Alpha given the same
// GxRgb5 field convention (r = NDS bits0-4 etc.) -- see that file for why
// the bit positions look transposed at first glance. Every texel this file
// ever bakes is opaque-or-fully-transparent (never partial alpha), and a
// fully-transparent texel is never the final EFB pixel (something opaque --
// at minimum the backdrop quad -- is always drawn first), so the alpha
// sub-format branch below is a defensive fallback, not an expected path.
static inline u16 gxRgb5a3ToGbaBgr555(u16 v)
{
	u8 r, g, b;
	if (v & 0x8000) {
		r = (v >> 10) & 0x1F;
		g = (v >> 5) & 0x1F;
		b = v & 0x1F;
	} else {
		u8 r4 = (v >> 8) & 0xF, g4 = (v >> 4) & 0xF, b4 = v & 0xF;
		r = (u8)((r4 << 1) | (r4 >> 3));
		g = (u8)((g4 << 1) | (g4 >> 3));
		b = (u8)((b4 << 1) | (b4 >> 3));
	}
	return (u16)((b << 10) | (g << 5) | r);
}

static inline u16 gxOpaqueTexel(u16 ndsColor) { return gxPackRGB5A3Opaque(gxExtractBGR555(ndsColor)); }
static const u16 kTransparentTexel = 0; // RGB5A3 alpha sub-format, alpha=0, top bit clear

// ---------------------------------------------------------------------
// BG plane cache: one baked RGB5A3 texture per BG (0-3), sized to that BG's
// full tilemap extent (up to 64x64 tiles = 512x512px, text mode's max), so
// a whole frame's worth of per-band scroll can be expressed as GX_REPEAT
// texture-coordinate offsets rather than a re-bake per band.
// ---------------------------------------------------------------------
struct GxBgPlaneCache {
	GXTexObj texObj;
	void *texData;
	u16 mapWpx, mapHpx;
	bool valid;
};
static GxBgPlaneCache s_bgPlane[4];
static const int kBgPlaneMaxPx = 512;
static u16 *s_bgBakeScratch; // linear (pre-swizzle) scratch, reused per-BG

// ---------------------------------------------------------------------
// Affine BG plane cache (BG2/BG3 in modes 1/2), index 0=BG2, 1=BG3. Affine
// maps are square, up to 128x128 tiles = 1024x1024px (vs. text mode's
// 64x64-tile max), so this is sized/allocated independently from
// GxBgPlaneCache rather than reusing it. When the map's overflow bit
// (BGxCNT bit13) is clear, the baked texture carries a 1-texel transparent
// border (padded to a multiple of 4) and is addressed GX_CLAMP so an
// affine-transformed UV landing outside the map reads transparent instead
// of a clamped edge texel; when overflow wraps, it's baked at exactly the
// map's own size and addressed GX_REPEAT so hardware wraparound handles
// arbitrary UV. See gx_gba_render.h's header comment.
// ---------------------------------------------------------------------
struct GxAffineBgPlaneCache {
	GXTexObj texObj;
	void *texData;
	u32 texDataCap; // bytes currently allocated at texData; regrown on demand
	u16 mapPx;      // logical (unbordered) map size, square
	u16 bufPx;      // baked/allocated texture size (mapPx, or mapPx+4 if bordered)
	bool wrap;
	bool valid;
};
static GxAffineBgPlaneCache s_affBgPlane[2];
static const int kAffineBgPlaneMaxPx = 1024 + 4;
static u16 *s_affBgBakeScratch; // linear (pre-swizzle) scratch, reused per-BG

// ---------------------------------------------------------------------
// OBJ: one persistent texture slot per OAM index (128), sized to the max
// regular-OBJ box (64x64), rewritten every dirty frame for whichever
// sprites are visible. See header comment: not OAM-index dirty-cached yet.
// ---------------------------------------------------------------------
struct GxObjTexSlot {
	GXTexObj texObj;
	void *texData;
};
static GxObjTexSlot s_objTex[128];
static const int kObjTexMaxPx = 64; // max regular-OBJ box (visibility/size gate)
// Every OBJ texture is baked with a 1-texel transparent border (padded to a
// multiple of 4 for RGB5A3's block size) so affine sampling that lands
// outside the real sprite content clamps onto transparent border texels
// instead of smearing the edge row/column -- see gx_gba_render.h. Non-affine
// sprites never actually sample the border (their UV range is always exactly
// [0,w]x[0,h] in source-texel space) but are baked the same way for one
// code path.
static const int kObjTexBufMaxPx = kObjTexMaxPx + 4;
static u16 *s_objBakeScratch;

struct GxObjDraw {
	s16 x, y;             // on-screen top-left of the (possibly doubled) box
	u16 boxW, boxH;        // on-screen box size (doubled for double-size affine)
	u16 texW, texH;        // source sprite pixel size (<= kObjTexMaxPx)
	u8 priority;
	bool hflip, vflip;     // meaningful only when !affine
	bool affine;
	s16 pa, pb, pc, pd;    // meaningful only when affine, 8.8 fixed point
	u8 oamIndex;
};
static GxObjDraw s_objDraws[128];
static int s_objDrawCount;

// ---------------------------------------------------------------------
// Bitmap-mode (3/4/5) plane: one full-screen-sized RGB5A3 buffer, reused
// across all three modes (mode 5's 160x128 content just occupies the
// top-left corner of the same buffer/texture, matching the CPU path's
// same-buffer-different-visible-rect treatment).
// ---------------------------------------------------------------------
static GXTexObj s_bmpTexObj;
static void *s_bmpTexData;
static bool s_bmpValid;

// 1x1 solid-color texture used for the backdrop fill (see gxGbaRenderFrame).
static GXTexObj s_backdropTexObj;
static void *s_backdropTexData;

// Copy-back scratch: GX_CopyTex's destination is real system memory, but in
// GX's own block-tiled layout for the copy format (see gx_texformat.h) --
// gxUnswizzle16bpp turns that back into a plain row-major buffer before the
// RGB5A3->GBA-BGR555 conversion pass writes into GBA_screen.
static void *s_copyBackBuf;
static u16 *s_copyBackLinear;

static bool s_initDone = false;

bool gxGbaRenderInit()
{
	if (s_initDone)
		return true;

	for (int i = 0; i < 4; ++i) {
		u32 sz = kBgPlaneMaxPx * kBgPlaneMaxPx * sizeof(u16);
		s_bgPlane[i].texData = memalign(32, sz);
		if (!s_bgPlane[i].texData) return false;
		memset(s_bgPlane[i].texData, 0, sz);
		s_bgPlane[i].valid = false;
	}
	s_bgBakeScratch = (u16 *)malloc(kBgPlaneMaxPx * kBgPlaneMaxPx * sizeof(u16));
	if (!s_bgBakeScratch) return false;

	for (int i = 0; i < 2; ++i) {
		s_affBgPlane[i].texData = nullptr;
		s_affBgPlane[i].texDataCap = 0;
		s_affBgPlane[i].valid = false;
	}
	s_affBgBakeScratch = (u16 *)malloc((u32)kAffineBgPlaneMaxPx * kAffineBgPlaneMaxPx * sizeof(u16));
	if (!s_affBgBakeScratch) return false;

	for (int i = 0; i < 128; ++i) {
		u32 sz = kObjTexBufMaxPx * kObjTexBufMaxPx * sizeof(u16);
		s_objTex[i].texData = memalign(32, sz);
		if (!s_objTex[i].texData) return false;
		memset(s_objTex[i].texData, 0, sz);
	}
	s_objBakeScratch = (u16 *)malloc(kObjTexBufMaxPx * kObjTexBufMaxPx * sizeof(u16));
	if (!s_objBakeScratch) return false;

	u32 bmpSz = GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16);
	s_bmpTexData = memalign(32, bmpSz);
	if (!s_bmpTexData) return false;
	memset(s_bmpTexData, 0, bmpSz);
	s_bmpValid = false;

	s_backdropTexData = memalign(32, 32); // GX's minimum texture alloc granularity
	if (!s_backdropTexData) return false;
	memset(s_backdropTexData, 0, 32);
	GX_InitTexObj(&s_backdropTexObj, s_backdropTexData, 1, 1, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);

	u32 copySz = GX_GetTexBufferSize(GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_RGB5A3, GX_FALSE, 0);
	s_copyBackBuf = memalign(32, copySz);
	if (!s_copyBackBuf) return false;
	s_copyBackLinear = (u16 *)malloc(GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	if (!s_copyBackLinear) return false;

	s_initDone = true;
	return true;
}

void gxGbaRenderShutdown()
{
	if (!s_initDone)
		return;
	for (int i = 0; i < 4; ++i) { free(s_bgPlane[i].texData); s_bgPlane[i].texData = nullptr; }
	free(s_bgBakeScratch); s_bgBakeScratch = nullptr;
	for (int i = 0; i < 2; ++i) { free(s_affBgPlane[i].texData); s_affBgPlane[i].texData = nullptr; s_affBgPlane[i].texDataCap = 0; }
	free(s_affBgBakeScratch); s_affBgBakeScratch = nullptr;
	for (int i = 0; i < 128; ++i) { free(s_objTex[i].texData); s_objTex[i].texData = nullptr; }
	free(s_objBakeScratch); s_objBakeScratch = nullptr;
	free(s_bmpTexData); s_bmpTexData = nullptr;
	free(s_backdropTexData); s_backdropTexData = nullptr;
	free(s_copyBackBuf); s_copyBackBuf = nullptr;
	free(s_copyBackLinear); s_copyBackLinear = nullptr;
	s_initDone = false;
}

// ---------------------------------------------------------------------
// GX pipeline state: texture-only TEV (replace), alpha blend (so a
// transparent/alpha-0 texel leaves the destination untouched -- exactly the
// CPU compositor's per-pixel opaque/transparent semantics), no depth test
// (painter's-algorithm draw order, same as renderScanline()), orthographic
// projection mapping model-space (x,y) 1:1 onto EFB pixel (x,y). Idempotent
// and cheap; called once per gxGbaRenderFrame() invocation rather than
// cached across frames, since nothing else in this build currently shares
// the GX FIFO with the GBA PPU (no concurrent DS 3D layer in GBA mode).
// ---------------------------------------------------------------------
static void gxSetup2DState()
{
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});

	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	Mtx44 proj;
	guOrtho(proj, 0, GBA_SCREEN_H, 0, GBA_SCREEN_W, 0, 1);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

	Mtx mv;
	guMtxIdentity(mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

static void gxDrawQuad(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 t0, f32 s1, f32 t1)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0); GX_TexCoord2f32(s0, t0);
		GX_Position2f32(x1, y0); GX_TexCoord2f32(s1, t0);
		GX_Position2f32(x1, y1); GX_TexCoord2f32(s1, t1);
		GX_Position2f32(x0, y1); GX_TexCoord2f32(s0, t1);
	GX_End();
}

// Same screen-space rectangle as gxDrawQuad, but with an independent UV pair
// per corner rather than a single (s0,t0)-(s1,t1) rectangle -- needed for
// affine BG/OBJ, where the quad's 4 corners generally map to a skewed
// parallelogram in texture space, not an axis-aligned rectangle. GX's
// per-pixel UV interpolation across the quad is linear, matching the affine
// transform's own linearity in screen (x,y) exactly.
static void gxDrawQuadFree(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1,
                           f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2, f32 u3, f32 v3)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0); GX_TexCoord2f32(u0, v0);
		GX_Position2f32(x1, y0); GX_TexCoord2f32(u1, v1);
		GX_Position2f32(x1, y1); GX_TexCoord2f32(u2, v2);
		GX_Position2f32(x0, y1); GX_TexCoord2f32(u3, v3);
	GX_End();
}

// ---------------------------------------------------------------------
// Stage 1: bake helpers
// ---------------------------------------------------------------------
struct GxBgLayout {
	u32 charBase, mapBase;
	bool colorMode; // 0=4bpp/16pal, 1=8bpp/256pal
	int mapWtiles, mapHtiles;
};

static GxBgLayout gxBgLayoutFromCnt(u16 cnt)
{
	GxBgLayout li;
	li.charBase = ((cnt >> 2) & 3) * 0x4000;
	li.colorMode = (cnt >> 7) & 1;
	li.mapBase = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel = (cnt >> 14) & 3;
	static const int mapWtiles[4] = { 32, 64, 32, 64 };
	static const int mapHtiles[4] = { 32, 32, 64, 64 };
	li.mapWtiles = mapWtiles[sizeSel];
	li.mapHtiles = mapHtiles[sizeSel];
	return li;
}

static void gxBakeBgPlane(int bg)
{
	u16 cnt = T1ReadWord(MMU.GBA_IOREG, IO_BG0CNT + bg * 2);
	GxBgLayout li = gxBgLayoutFromCnt(cnt);
	int mapWpx = li.mapWtiles * 8, mapHpx = li.mapHtiles * 8;

	for (int ty = 0; ty < li.mapHtiles; ++ty) {
		for (int tx = 0; tx < li.mapWtiles; ++tx) {
			int blockX = tx / 32, blockY = ty / 32;
			int blockIdx = blockX + blockY * (li.mapWtiles / 32);
			u32 entryAddr = li.mapBase + blockIdx * 0x800 + ((ty % 32) * 32 + (tx % 32)) * 2;
			u16 entry = T1ReadWord(MMU.GBA_VRAM, gxVramAddr(entryAddr));
			int tileNum = entry & 0x3FF;
			bool hflip = (entry >> 10) & 1;
			bool vflip = (entry >> 11) & 1;
			int palNum = (entry >> 12) & 0xF;

			for (int suby = 0; suby < 8; ++suby) {
				int srcY = vflip ? 7 - suby : suby;
				for (int subx = 0; subx < 8; ++subx) {
					int srcX = hflip ? 7 - subx : subx;
					u16 texel;
					if (!li.colorMode) {
						u32 tileAddr = li.charBase + tileNum * 32 + srcY * 4 + srcX / 2;
						u8 byte = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
						int idx = (srcX & 1) ? (byte >> 4) : (byte & 0xF);
						texel = idx ? gxOpaqueTexel(gxBgPalColor(palNum * 16 + idx)) : kTransparentTexel;
					} else {
						u32 tileAddr = li.charBase + tileNum * 64 + srcY * 8 + srcX;
						u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
						texel = idx ? gxOpaqueTexel(gxBgPalColor(idx)) : kTransparentTexel;
					}
					s_bgBakeScratch[(ty * 8 + suby) * mapWpx + (tx * 8 + subx)] = texel;
				}
			}
		}
	}

	GxBgPlaneCache &pc = s_bgPlane[bg];
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_bgBakeScratch, (u16 *)pc.texData, blk.texelsWide, blk.texelsTall, mapWpx, mapHpx);
	DCFlushRange(pc.texData, mapWpx * mapHpx * sizeof(u16));
	GX_InitTexObj(&pc.texObj, pc.texData, mapWpx, mapHpx, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
	pc.mapWpx = (u16)mapWpx;
	pc.mapHpx = (u16)mapHpx;
	pc.valid = true;
}

// Affine BG map entries are 1 byte (tile index 0-255 into a fixed 8bpp
// charset), no per-tile flip -- unlike text mode's 2-byte entries with
// hflip/vflip/palette bits. which: 0=BG2, 1=BG3.
static void gxBakeAffineBgPlane(int which, u16 cnt)
{
	u32 charBase = ((cnt >> 2) & 3) * 0x4000;
	u32 mapBase = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel = (cnt >> 14) & 3;
	int mapTiles = 16 << sizeSel; // 16/32/64/128
	int mapPx = mapTiles * 8;
	bool wrap = (cnt >> 13) & 1;
	int bufPx = wrap ? mapPx : mapPx + 4;
	int borderOff = wrap ? 0 : 1;

	for (int ty = 0; ty < mapTiles; ++ty) {
		for (int tx = 0; tx < mapTiles; ++tx) {
			u32 entryAddr = mapBase + (ty * mapTiles + tx);
			u8 tileNum = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(entryAddr));
			for (int suby = 0; suby < 8; ++suby) {
				for (int subx = 0; subx < 8; ++subx) {
					u32 tileAddr = charBase + tileNum * 64 + suby * 8 + subx;
					u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
					u16 texel = idx ? gxOpaqueTexel(gxBgPalColor(idx)) : kTransparentTexel;
					int dstX = tx * 8 + subx + borderOff;
					int dstY = ty * 8 + suby + borderOff;
					s_affBgBakeScratch[dstY * bufPx + dstX] = texel;
				}
			}
		}
	}
	if (!wrap) {
		for (int x = 0; x < bufPx; ++x) {
			s_affBgBakeScratch[x] = kTransparentTexel;
			s_affBgBakeScratch[(bufPx - 1) * bufPx + x] = kTransparentTexel;
		}
		for (int y = 0; y < bufPx; ++y) {
			s_affBgBakeScratch[y * bufPx] = kTransparentTexel;
			s_affBgBakeScratch[y * bufPx + (bufPx - 1)] = kTransparentTexel;
		}
	}

	GxAffineBgPlaneCache &pc = s_affBgPlane[which];
	u32 needed = (u32)bufPx * bufPx * sizeof(u16);
	if (needed > pc.texDataCap) {
		if (pc.texData) free(pc.texData);
		pc.texData = memalign(32, needed);
		pc.texDataCap = pc.texData ? needed : 0;
	}
	if (!pc.texData) { pc.valid = false; return; }

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_affBgBakeScratch, (u16 *)pc.texData, blk.texelsWide, blk.texelsTall, bufPx, bufPx);
	DCFlushRange(pc.texData, needed);
	u8 wm = wrap ? GX_REPEAT : GX_CLAMP;
	GX_InitTexObj(&pc.texObj, pc.texData, bufPx, bufPx, GX_TF_RGB5A3, wm, wm, GX_FALSE);
	pc.mapPx = (u16)mapPx;
	pc.bufPx = (u16)bufPx;
	pc.wrap = wrap;
	pc.valid = true;
}

static void gxBakeBackdrop()
{
	u16 texel = gxOpaqueTexel(gxBgPalColor(0));
	((u16 *)s_backdropTexData)[0] = texel;
	DCFlushRange(s_backdropTexData, 32);
}

static const u8 s_objSizeW[4][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32}, {0,0,0,0} };
static const u8 s_objSizeH[4][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64}, {0,0,0,0} };

// Scans OAM for this frame's visible, drawable, non-window OBJ entries
// (both regular and affine -- see gx_gba_render.h). Mirrors renderObjLine()'s
// OAM field decode (gba_ppu.cpp) exactly, including that attribute0 bit9 is
// the OBJ-disable bit for a regular sprite but the double-size flag for an
// affine one (they share the bit; GBATek's actual hardware layout, not a
// coincidence in this codebase).
static bool gxCollectVisibleObj(u16 dispcnt)
{
	s_objDrawCount = 0;
	if (!((dispcnt >> 12) & 1))
		return true; // OBJ disabled entirely: trivially "collected", zero sprites

	for (int i = 0; i < 128; ++i) {
		u32 oamOff = i * 8;
		u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
		u16 a1 = T1ReadWord(MMU.GBA_OAM, oamOff + 2);
		u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);

		bool affine = (a0 >> 8) & 1;
		bool doubleOrDisable = (a0 >> 9) & 1;
		if (!affine && doubleOrDisable) continue; // OBJ-disable bit
		int objMode = (a0 >> 10) & 3;
		if (objMode == 2) continue; // OBJ-window entries don't draw pixels
		int shape = (a0 >> 14) & 3;
		if (shape == 3) continue;

		int sizeSel = (a1 >> 14) & 3;
		int w = s_objSizeW[shape][sizeSel];
		int h = s_objSizeH[shape][sizeSel];
		if (w == 0 || w > kObjTexMaxPx || h > kObjTexMaxPx) continue;

		int boxW = w, boxH = h;
		if (affine && doubleOrDisable) { boxW *= 2; boxH *= 2; } // double-size affine

		int y = a0 & 0xFF;
		if (y >= 160) y -= 256;
		int x = a1 & 0x1FF;
		if (x >= 240) x -= 512;
		if (x + boxW <= 0 || x >= GBA_SCREEN_W || y + boxH <= 0 || y >= GBA_SCREEN_H) continue;

		GxObjDraw &d = s_objDraws[s_objDrawCount++];
		d.x = (s16)x; d.y = (s16)y;
		d.boxW = (u16)boxW; d.boxH = (u16)boxH;
		d.texW = (u16)w; d.texH = (u16)h;
		d.priority = (u8)((a2 >> 10) & 3);
		d.affine = affine;
		if (affine) {
			int grp = (a1 >> 9) & 0x1F;
			d.pa = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 6);
			d.pb = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 14);
			d.pc = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 22);
			d.pd = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 30);
			d.hflip = d.vflip = false;
		} else {
			d.hflip = (a1 >> 12) & 1;
			d.vflip = (a1 >> 13) & 1;
		}
		d.oamIndex = (u8)i;
	}
	return true;
}

// Decodes one sprite's texW x texH source pixels into a (texW+4)x(texH+4)
// buffer with a 1-texel transparent border (see kObjTexBufMaxPx) -- baked
// the same way regardless of d.affine so both draw paths share one bake
// function; only affine sampling ever actually reaches the border.
static void gxBakeObjTexture(u16 dispcnt, const GxObjDraw &d)
{
	u32 oamOff = d.oamIndex * 8;
	u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
	u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);
	bool colorMode = (a0 >> 13) & 1;
	int tileNum = a2 & 0x3FF;
	int palNum = (a2 >> 12) & 0xF;
	bool oneDim = (dispcnt >> 6) & 1;

	int tileBytes = colorMode ? 64 : 32;
	u32 charBase = 0x10000;
	int w = d.texW, h = d.texH;
	int bufW = w + 4, bufH = h + 4;

	for (int texy = 0; texy < h; ++texy) {
		int tileY = texy / 8, suby = texy % 8;
		for (int texx = 0; texx < w; ++texx) {
			int tileX = texx / 8, subx = texx % 8;
			u32 addr;
			if (oneDim) {
				u32 tileIndex = tileNum + tileY * (w / 8) + tileX;
				addr = charBase + tileIndex * tileBytes;
			} else {
				u32 slotX = tileX * (colorMode ? 2 : 1);
				u32 slotIndex = tileNum + tileY * 32 + slotX;
				addr = charBase + slotIndex * 32;
			}
			u16 texel;
			if (!colorMode) {
				u32 a = addr + suby * 4 + subx / 2;
				u8 byte = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(a));
				int idx = (subx & 1) ? (byte >> 4) : (byte & 0xF);
				texel = idx ? gxOpaqueTexel(gxObjPalColor(palNum * 16 + idx)) : kTransparentTexel;
			} else {
				u32 a = addr + suby * 8 + subx;
				u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(a));
				texel = idx ? gxOpaqueTexel(gxObjPalColor(idx)) : kTransparentTexel;
			}
			s_objBakeScratch[(texy + 1) * bufW + (texx + 1)] = texel;
		}
	}
	for (int x = 0; x < bufW; ++x) {
		s_objBakeScratch[x] = kTransparentTexel;
		s_objBakeScratch[(bufH - 1) * bufW + x] = kTransparentTexel;
	}
	for (int y = 0; y < bufH; ++y) {
		s_objBakeScratch[y * bufW] = kTransparentTexel;
		s_objBakeScratch[y * bufW + (bufW - 1)] = kTransparentTexel;
	}

	GxObjTexSlot &slot = s_objTex[d.oamIndex];
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	// w/h are each a multiple of 8 (OBJ sizes are whole 8px tiles), so
	// bufW/bufH (w/h + 4) are each a multiple of 4, satisfying
	// gxSwizzle16bpp's RGB5A3 (4x4) block-multiple requirement.
	gxSwizzle16bpp(s_objBakeScratch, (u16 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
	DCFlushRange(slot.texData, bufW * bufH * sizeof(u16));
	GX_InitTexObj(&slot.texObj, slot.texData, bufW, bufH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
}

static void gxBakeBitmapMode(int mode, u16 dispcnt)
{
	u16 *dst = (u16 *)s_bmpTexData;
	u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;

	if (mode == 3) {
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
			dst[i] = gxOpaqueTexel(T1ReadWord(MMU.GBA_VRAM, i * 2));
	} else if (mode == 4) {
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i) {
			u8 idx = T1ReadByte(MMU.GBA_VRAM, page + i);
			dst[i] = gxOpaqueTexel(gxBgPalColor(idx));
		}
	} else { // mode 5: 160x128, rest of the buffer stays whatever it last held
		const int W = 160, H = 128;
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
				dst[y * GBA_SCREEN_W + x] = gxOpaqueTexel(T1ReadWord(MMU.GBA_VRAM, page + (y * W + x) * 2));
	}

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp((u16 *)s_bmpTexData, s_bgBakeScratch, blk.texelsWide, blk.texelsTall, GBA_SCREEN_W, GBA_SCREEN_H);
	memcpy(s_bmpTexData, s_bgBakeScratch, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	DCFlushRange(s_bmpTexData, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	GX_InitTexObj(&s_bmpTexObj, s_bmpTexData, GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	s_bmpValid = true;
}

// ---------------------------------------------------------------------
// Stage 4: per-band draw
// ---------------------------------------------------------------------
// Texel coordinate (not yet border-offset/normalized) an affine OBJ's
// screen-space box position (col,row), box-relative, maps back to in source
// texture space -- exact port of renderObjLine()'s (gba_ppu.cpp) per-pixel
// inverse-affine formula. Linear in (col,row), so calling this at a box's 4
// corners and letting GX interpolate the resulting UVs reproduces the same
// per-pixel mapping the CPU path computes one pixel at a time.
static void gxAffineObjTexCorner(const GxObjDraw &d, s32 col, s32 row, f32 *tx, f32 *ty)
{
	s32 cx = d.boxW / 2, cy = d.boxH / 2;
	s32 relx = col - cx, rely = row - cy;
	s32 origx = (relx * d.pa + rely * d.pb) >> 8;
	s32 origy = (relx * d.pc + rely * d.pd) >> 8;
	*tx = (f32)(origx + d.texW / 2);
	*ty = (f32)(origy + d.texH / 2);
}

static void gxDrawObjForPriorityBand(int prio, int y0, int y1)
{
	for (int i = 0; i < s_objDrawCount; ++i) {
		const GxObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.boxH <= y0) continue;

		f32 bufW = (f32)(d.texW + 4), bufH = (f32)(d.texH + 4);
		if (d.affine) {
			f32 tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
			gxAffineObjTexCorner(d, 0, 0, &tx0, &ty0);
			gxAffineObjTexCorner(d, d.boxW, 0, &tx1, &ty1);
			gxAffineObjTexCorner(d, d.boxW, d.boxH, &tx2, &ty2);
			gxAffineObjTexCorner(d, 0, d.boxH, &tx3, &ty3);
			gxDrawQuadFree(&s_objTex[d.oamIndex].texObj, d.x, d.y, d.x + d.boxW, d.y + d.boxH,
			               (tx0 + 1) / bufW, (ty0 + 1) / bufH,
			               (tx1 + 1) / bufW, (ty1 + 1) / bufH,
			               (tx2 + 1) / bufW, (ty2 + 1) / bufH,
			               (tx3 + 1) / bufW, (ty3 + 1) / bufH);
		} else {
			f32 u0 = 1.0f / bufW, u1 = (f32)(d.texW + 1) / bufW;
			f32 v0 = 1.0f / bufH, v1 = (f32)(d.texH + 1) / bufH;
			f32 s0 = d.hflip ? u1 : u0, s1 = d.hflip ? u0 : u1;
			f32 t0 = d.vflip ? v1 : v0, t1 = d.vflip ? v0 : v1;
			gxDrawQuad(&s_objTex[d.oamIndex].texObj, d.x, d.y, d.x + d.boxW, d.y + d.boxH, s0, t0, s1, t1);
		}
	}
}

static void gxDrawBgQuad(int bg, const GxGbaBandRegs &r, int y0, int y1)
{
	GxBgPlaneCache &pc = s_bgPlane[bg];
	if (!pc.valid) return;
	f32 s0 = (f32)r.hofs[bg] / pc.mapWpx;
	f32 s1 = s0 + (f32)GBA_SCREEN_W / pc.mapWpx;
	f32 t0 = (f32)(r.vofs[bg] + y0) / pc.mapHpx;
	f32 t1 = (f32)(r.vofs[bg] + y1) / pc.mapHpx;
	gxDrawQuad(&pc.texObj, 0, (f32)y0, GBA_SCREEN_W, (f32)y1, s0, t0, s1, t1);
}

// which: 0=BG2, 1=BG3. Band's screen rect (0,y0)-(GBA_SCREEN_W,y1) maps
// through the affine transform (anchored at r.affX/affY, the accumulated
// reference point valid at scanline y0 -- see GxGbaBandRegs) to a
// parallelogram in texture space; computing that mapping at the 4 rect
// corners and letting GX interpolate reproduces sampleAffineBg()'s
// per-pixel formula (gba_ppu.cpp) exactly, since it's linear in (x, y-y0).
static void gxDrawAffineBgQuad(int which, const GxGbaBandRegs &r, int y0, int y1)
{
	GxAffineBgPlaneCache &pc = s_affBgPlane[which];
	if (!pc.valid) return;

	s32 pa = r.affPA[which], pb = r.affPB[which];
	s32 pcMat = r.affPC[which], pd = r.affPD[which];
	s32 X0 = r.affX[which], Y0 = r.affY[which];
	int W = GBA_SCREEN_W, H = y1 - y0;

	f32 tx0 = X0 / 256.0f,                    ty0 = Y0 / 256.0f;
	f32 tx1 = (X0 + W * pa) / 256.0f,          ty1 = (Y0 + W * pcMat) / 256.0f;
	f32 tx2 = (X0 + W * pa + H * pb) / 256.0f, ty2 = (Y0 + W * pcMat + H * pd) / 256.0f;
	f32 tx3 = (X0 + H * pb) / 256.0f,          ty3 = (Y0 + H * pd) / 256.0f;

	f32 norm = (f32)pc.bufPx;
	f32 off = pc.wrap ? 0.0f : 1.0f;
	gxDrawQuadFree(&pc.texObj, 0, (f32)y0, GBA_SCREEN_W, (f32)y1,
	               (tx0 + off) / norm, (ty0 + off) / norm,
	               (tx1 + off) / norm, (ty1 + off) / norm,
	               (tx2 + off) / norm, (ty2 + off) / norm,
	               (tx3 + off) / norm, (ty3 + off) / norm);
}

bool gxGbaRenderFrame()
{
	if (!s_initDone)
		return false;

	u16 dispcnt = T1ReadWord(MMU.GBA_IOREG, IO_DISPCNT);
	int mode = dispcnt & 7;
	if (mode > 5)
		return false; // prohibited DISPCNT mode value; shouldn't occur on real ROMs

	if ((dispcnt >> 7) & 1) { // forced blank: no GX needed at all
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
			GBA_screen[i] = 0x7FFF;
		return true;
	}

	if (!gxCollectVisibleObj(dispcnt))
		return false;

	bool tiled = (mode == 0 || mode == 1 || mode == 2);

	int outStarts[GxBandTracker::kMaxBands], outEnds[GxBandTracker::kMaxBands];
	int bandCount;
	if (tiled) {
		bandCount = g_gbaFramePlan.bands.finalize(GBA_SCREEN_H, outStarts, outEnds);
	} else {
		bandCount = 1;
		outStarts[0] = 0;
		outEnds[0] = GBA_SCREEN_H;
	}

	bool dirty = g_gbaFramePlan.vram.anyDirty() || g_gbaFramePlan.palette.anyDirty();

	gxSetup2DState();
	GX_SetViewport(0, 0, GBA_SCREEN_W, GBA_SCREEN_H, 0, 1);
	GX_SetScissor(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);
	GX_SetTexCopySrc(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);

	if (dirty)
		gxBakeBackdrop();
	gxDrawQuad(&s_backdropTexObj, 0, 0, GBA_SCREEN_W, GBA_SCREEN_H, 0, 0, 1, 1);

	if (dirty) {
		for (int i = 0; i < s_objDrawCount; ++i)
			gxBakeObjTexture(dispcnt, s_objDraws[i]);
	}

	if (tiled) {
		if (dirty) {
			for (int bg = 0; bg < 4; ++bg) {
				if (!((dispcnt >> (8 + bg)) & 1)) continue;
				if (mode == 1 && bg == 3) continue; // mode 1 has no BG3
				if (mode == 2 && (bg == 0 || bg == 1)) continue; // mode 2 has no BG0/BG1
				bool affineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);
				if (affineCapable) {
					u16 cnt = T1ReadWord(MMU.GBA_IOREG, IO_BG0CNT + bg * 2);
					gxBakeAffineBgPlane(bg - 2, cnt);
				} else {
					gxBakeBgPlane(bg);
				}
			}
		}
		for (int b = 0; b < bandCount; ++b) {
			const GxGbaBandRegs &r = g_gbaBandRegs[b];
			GX_SetScissor(0, outStarts[b], GBA_SCREEN_W, outEnds[b] - outStarts[b]);
			for (int prio = 3; prio >= 0; --prio) {
				for (int bg = 3; bg >= 0; --bg) {
					if (!((r.dispcnt >> (8 + bg)) & 1)) continue;
					if (mode == 1 && bg == 3) continue;
					if (mode == 2 && (bg == 0 || bg == 1)) continue;
					if ((r.bgcnt[bg] & 3) != prio) continue;
					bool affineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);
					if (affineCapable)
						gxDrawAffineBgQuad(bg - 2, r, outStarts[b], outEnds[b]);
					else
						gxDrawBgQuad(bg, r, outStarts[b], outEnds[b]);
				}
				if ((r.dispcnt >> 12) & 1)
					gxDrawObjForPriorityBand(prio, outStarts[b], outEnds[b]);
			}
		}
	} else {
		if (dirty)
			gxBakeBitmapMode(mode, dispcnt);
		if (s_bmpValid) {
			int h = (mode == 5) ? 128 : GBA_SCREEN_H;
			int w = (mode == 5) ? 160 : GBA_SCREEN_W;
			f32 s1 = (f32)w / GBA_SCREEN_W, t1 = (f32)h / GBA_SCREEN_H;
			gxDrawQuad(&s_bmpTexObj, 0, 0, (f32)w, (f32)h, 0, 0, s1, t1);
		}
		if ((dispcnt >> 12) & 1)
			for (int prio = 3; prio >= 0; --prio)
				gxDrawObjForPriorityBand(prio, 0, GBA_SCREEN_H);
	}

	GX_DrawDone();
	GX_SetTexCopyDst(GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_RGB5A3, GX_FALSE);
	GX_CopyTex(s_copyBackBuf, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxUnswizzle16bpp((const u16 *)s_copyBackBuf, s_copyBackLinear, blk.texelsWide, blk.texelsTall, GBA_SCREEN_W, GBA_SCREEN_H);
	for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
		GBA_screen[i] = gxRgb5a3ToGbaBgr555(s_copyBackLinear[i]);

	return true;
}
