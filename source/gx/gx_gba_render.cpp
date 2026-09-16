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
// OBJ: one persistent texture slot per OAM index (128), sized to the max
// regular-OBJ box (64x64), rewritten every dirty frame for whichever
// sprites are visible. See header comment: not OAM-index dirty-cached yet.
// ---------------------------------------------------------------------
struct GxObjTexSlot {
	GXTexObj texObj;
	void *texData;
};
static GxObjTexSlot s_objTex[128];
static const int kObjTexMaxPx = 64;
static u16 *s_objBakeScratch;

struct GxObjDraw {
	s16 x, y;
	u16 w, h;
	u8 priority;
	bool hflip, vflip;
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

	for (int i = 0; i < 128; ++i) {
		u32 sz = kObjTexMaxPx * kObjTexMaxPx * sizeof(u16);
		s_objTex[i].texData = memalign(32, sz);
		if (!s_objTex[i].texData) return false;
		memset(s_objTex[i].texData, 0, sz);
	}
	s_objBakeScratch = (u16 *)malloc(kObjTexMaxPx * kObjTexMaxPx * sizeof(u16));
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

static void gxBakeBackdrop()
{
	u16 texel = gxOpaqueTexel(gxBgPalColor(0));
	((u16 *)s_backdropTexData)[0] = texel;
	DCFlushRange(s_backdropTexData, 32);
}

static const u8 s_objSizeW[4][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32}, {0,0,0,0} };
static const u8 s_objSizeH[4][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64}, {0,0,0,0} };

// Scans OAM for this frame's visible, drawable, non-window OBJ entries.
// Returns false (and leaves s_objDraws untouched) the instant it finds an
// affine sprite -- see gx_gba_render.h's header comment on why affine OBJ
// is a whole-frame bail for this pass rather than a per-sprite fallback.
static bool gxCollectVisibleObj(u16 dispcnt)
{
	s_objDrawCount = 0;
	if (!((dispcnt >> 12) & 1))
		return true; // OBJ disabled entirely: trivially "collected", zero sprites

	bool oneDim = (dispcnt >> 6) & 1;
	(void)oneDim; // consumed in gxBakeObjTexture via closure-free re-read below

	for (int i = 0; i < 128; ++i) {
		u32 oamOff = i * 8;
		u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
		u16 a1 = T1ReadWord(MMU.GBA_OAM, oamOff + 2);
		u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);

		bool affine = (a0 >> 8) & 1;
		if (affine)
			return false; // whole-frame bail, see above
		bool disabled = (a0 >> 9) & 1;
		if (disabled) continue;
		int objMode = (a0 >> 10) & 3;
		if (objMode == 2) continue; // OBJ-window entries don't draw pixels
		int shape = (a0 >> 14) & 3;
		if (shape == 3) continue;

		int sizeSel = (a1 >> 14) & 3;
		int w = s_objSizeW[shape][sizeSel];
		int h = s_objSizeH[shape][sizeSel];
		if (w == 0 || w > kObjTexMaxPx || h > kObjTexMaxPx) continue;

		int y = a0 & 0xFF;
		if (y >= 160) y -= 256;
		int x = a1 & 0x1FF;
		if (x >= 240) x -= 512;
		if (x + w <= 0 || x >= GBA_SCREEN_W || y + h <= 0 || y >= GBA_SCREEN_H) continue;

		GxObjDraw &d = s_objDraws[s_objDrawCount++];
		d.x = (s16)x; d.y = (s16)y; d.w = (u16)w; d.h = (u16)h;
		d.priority = (u8)((a2 >> 10) & 3);
		d.hflip = (a1 >> 12) & 1;
		d.vflip = (a1 >> 13) & 1;
		d.oamIndex = (u8)i;
	}
	return true;
}

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
	int w = d.w, h = d.h;

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
			s_objBakeScratch[texy * w + texx] = texel;
		}
	}

	GxObjTexSlot &slot = s_objTex[d.oamIndex];
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	// w/h are each a multiple of 8 (OBJ sizes are whole 8px tiles), and every
	// RGB5A3 block dimension (4x4) divides 8, so gxSwizzle16bpp's block-
	// multiple requirement always holds here.
	gxSwizzle16bpp(s_objBakeScratch, (u16 *)slot.texData, blk.texelsWide, blk.texelsTall, w, h);
	DCFlushRange(slot.texData, w * h * sizeof(u16));
	GX_InitTexObj(&slot.texObj, slot.texData, w, h, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
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
static void gxDrawObjForPriorityBand(int prio, int y0, int y1)
{
	for (int i = 0; i < s_objDrawCount; ++i) {
		const GxObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.h <= y0) continue;
		f32 s0 = d.hflip ? 1.0f : 0.0f, s1 = d.hflip ? 0.0f : 1.0f;
		f32 t0 = d.vflip ? 1.0f : 0.0f, t1 = d.vflip ? 0.0f : 1.0f;
		gxDrawQuad(&s_objTex[d.oamIndex].texObj, d.x, d.y, d.x + d.w, d.y + d.h, s0, t0, s1, t1);
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

bool gxGbaRenderFrame()
{
	if (!s_initDone)
		return false;

	u16 dispcnt = T1ReadWord(MMU.GBA_IOREG, IO_DISPCNT);
	int mode = dispcnt & 7;
	if (mode != 0 && mode != 3 && mode != 4 && mode != 5)
		return false; // modes 1/2 (affine BG): CPU fallback, see header

	if ((dispcnt >> 7) & 1) { // forced blank: no GX needed at all
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
			GBA_screen[i] = 0x7FFF;
		return true;
	}

	if (!gxCollectVisibleObj(dispcnt))
		return false; // affine OBJ present: CPU fallback, see header

	int outStarts[GxBandTracker::kMaxBands], outEnds[GxBandTracker::kMaxBands];
	int bandCount;
	if (mode == 0) {
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

	if (mode == 0) {
		if (dirty) {
			for (int bg = 0; bg < 4; ++bg)
				if ((dispcnt >> (8 + bg)) & 1)
					gxBakeBgPlane(bg);
		}
		for (int b = 0; b < bandCount; ++b) {
			const GxGbaBandRegs &r = g_gbaBandRegs[b];
			GX_SetScissor(0, outStarts[b], GBA_SCREEN_W, outEnds[b] - outStarts[b]);
			for (int prio = 3; prio >= 0; --prio) {
				for (int bg = 3; bg >= 0; --bg) {
					if (!((r.dispcnt >> (8 + bg)) & 1)) continue;
					if ((r.bgcnt[bg] & 3) != prio) continue;
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
