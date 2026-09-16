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
// Bake-time blend (BLDCNT/BLDALPHA/BLDY): see gx_gba_render.h's "Blend"
// section. A target-1 layer's opaque texels are recolored/alpha-scaled at
// bake time rather than via any per-frame TEV/blend state change; every
// bake call site below routes its per-texel opaque-color choice through
// gxTexel() with a GxTexelEffectParams that's GXTEXEFFECT_NONE for the
// ordinary (unblended) case, so this is a strict superset of the existing
// bake behavior, not a fork of it.
// ---------------------------------------------------------------------
enum GxTexelEffect { GXTEXEFFECT_NONE, GXTEXEFFECT_ALPHA, GXTEXEFFECT_BRIGHTEN, GXTEXEFFECT_DARKEN };
struct GxTexelEffectParams { GxTexelEffect mode; int eva; int evy; };
static const GxTexelEffectParams kTexelEffectNone = { GXTEXEFFECT_NONE, 0, 0 };

// Local port of gba_ppu.cpp's blendFade() (brighten toward white / darken
// toward black, EVY in 16ths) -- see that file for the reference formula.
// Not shared via a header on purpose, same rationale as this file's other
// small local duplicates (top of file).
static inline u16 gxBlendFade(u16 ndsColor, int evy, bool toWhite)
{
	int r = ndsColor & 0x1F, g = (ndsColor >> 5) & 0x1F, b = (ndsColor >> 10) & 0x1F;
	if (toWhite) { r += ((31 - r) * evy) / 16; g += ((31 - g) * evy) / 16; b += ((31 - b) * evy) / 16; }
	else         { r -= (r * evy) / 16;        g -= (g * evy) / 16;        b -= (b * evy) / 16; }
	if (r < 0) r = 0;
	if (r > 31) r = 31;
	if (g < 0) g = 0;
	if (g > 31) g = 31;
	if (b < 0) b = 0;
	if (b > 31) b = 31;
	return (u16)((b << 10) | (g << 5) | r);
}

// One opaque source texel (NDS BGR555, already palette-resolved), with
// this layer's this-frame texel effect applied. `opaque` false always
// yields fully transparent, regardless of fx -- effects never apply to a
// texel that wouldn't have drawn at all.
static inline u16 gxTexel(u16 ndsColor, bool opaque, const GxTexelEffectParams &fx)
{
	if (!opaque) return kTransparentTexel;
	switch (fx.mode) {
	case GXTEXEFFECT_ALPHA: {
		// RGB5A3's alpha sub-format has only 3 bits of alpha (0-7) and
		// 4 bits/channel of color (see gx_color.h) -- EVA/16 (17 possible
		// levels) is rounded to the nearest of 8 representable levels,
		// and color loses its LSB of precision. Documented near-match,
		// not a bug: see gx_gba_render.h's "Blend" section.
		u8 a3 = (u8)((fx.eva * 7 + 8) / 16);
		return gxPackRGB5A3AlphaFull(a3, ndsColor);
	}
	case GXTEXEFFECT_BRIGHTEN: return gxOpaqueTexel(gxBlendFade(ndsColor, fx.evy, true));
	case GXTEXEFFECT_DARKEN:   return gxOpaqueTexel(gxBlendFade(ndsColor, fx.evy, false));
	default:                   return gxOpaqueTexel(ndsColor);
	}
}

// ---------------------------------------------------------------------
// Windows (WIN0/WIN1): see gx_gba_render.h's "Windows" section. Local
// ports of gba_ppu.cpp's windowXRange/windowYRange/decodeWinByte (same
// GBATEK rules -- garbage X2>240/Y2>160 or X1>X2/Y1>Y2 clamps to 240/160,
// WININ/WINOUT bit layout), not shared via a header for the same reason
// as this file's other CPU-reference duplicates.
// ---------------------------------------------------------------------
struct GxWinMasks { bool bg[4]; bool obj; bool effect; };

static inline void gxWindowXRange(u16 winH, int &x1, int &x2)
{
	x1 = (winH >> 8) & 0xFF;
	x2 = winH & 0xFF;
	if (x2 > GBA_SCREEN_W || x1 > x2) x2 = GBA_SCREEN_W;
}
static inline void gxWindowYRange(u16 winV, int &y1, int &y2)
{
	y1 = (winV >> 8) & 0xFF;
	y2 = winV & 0xFF;
	if (y2 > GBA_SCREEN_H || y1 > y2) y2 = GBA_SCREEN_H;
}
static inline GxWinMasks gxDecodeWinByte(u16 v, int shift)
{
	u8 b = (u8)(v >> shift);
	GxWinMasks m;
	for (int i = 0; i < 4; i++) m.bg[i] = (b >> i) & 1;
	m.obj = (b >> 4) & 1;
	m.effect = (b >> 5) & 1;
	return m;
}

// Precomputed per-band window decomposition: up to 3 scissor-rect passes
// (WINOUT, WIN1, WIN0) in strict precedence order -- see gx_gba_render.h.
// `active` is false whenever neither WIN0 nor WIN1 is enabled this band,
// in which case callers must skip this machinery entirely and draw once,
// unclipped, with effect always enabled (matching gba_ppu.cpp's
// `!windowsActive` branch exactly).
struct GxWindowPlan {
	bool active;
	GxWinMasks out;
	bool win1On; int win1Y0, win1Y1, win1X0, win1X1; GxWinMasks win1;
	bool win0On; int win0Y0, win0Y1, win0X0, win0X1; GxWinMasks win0;
};

static GxWindowPlan gxBuildWindowPlan(const GxGbaBandRegs &r, int bandY0, int bandY1)
{
	GxWindowPlan p;
	bool win0Dc = (r.dispcnt >> 13) & 1, win1Dc = (r.dispcnt >> 14) & 1;
	p.active = win0Dc || win1Dc;
	p.out = gxDecodeWinByte(r.winOut, 0);
	p.win1On = false;
	p.win0On = false;
	if (!p.active)
		return p;
	if (win1Dc) {
		int y1, y2;
		gxWindowYRange(r.win1v, y1, y2);
		int oy0 = y1 > bandY0 ? y1 : bandY0, oy1 = y2 < bandY1 ? y2 : bandY1;
		if (oy1 > oy0) {
			p.win1On = true; p.win1Y0 = oy0; p.win1Y1 = oy1;
			gxWindowXRange(r.win1h, p.win1X0, p.win1X1);
			p.win1 = gxDecodeWinByte(r.winIn, 8);
		}
	}
	if (win0Dc) {
		int y1, y2;
		gxWindowYRange(r.win0v, y1, y2);
		int oy0 = y1 > bandY0 ? y1 : bandY0, oy1 = y2 < bandY1 ? y2 : bandY1;
		if (oy1 > oy0) {
			p.win0On = true; p.win0Y0 = oy0; p.win0Y1 = oy1;
			gxWindowXRange(r.win0h, p.win0X0, p.win0X1);
			p.win0 = gxDecodeWinByte(r.winIn, 0);
		}
	}
	return p;
}

// ---------------------------------------------------------------------
// BG plane cache: one baked RGB5A3 texture per BG (0-3), sized to that BG's
// full tilemap extent (up to 64x64 tiles = 512x512px, text mode's max), so
// a whole frame's worth of per-band scroll can be expressed as GX_REPEAT
// texture-coordinate offsets rather than a re-bake per band.
// ---------------------------------------------------------------------
struct GxBgPlaneCache {
	GXTexObj texObj;
	void *texData;
	// gx-next-steps-log.md task 6: this plane's dedicated TLUT (see "Native
	// CI4/CI8" in gx_gba_render.h) -- tlutData holds a 256-entry RGB5A3
	// table built from the live BG palette bank at bake time; tlutObj wraps
	// it for GX_LoadTlut(). Each BG index gets its OWN permanently-dedicated
	// GX_TLUTn name (GX_TLUT0+bg for text mode, GX_TLUT6/7 for the affine
	// variant of BG2/BG3 -- see gxBakeAffineBgPlane) rather than sharing one
	// slot between a BG index's text and affine bakes, specifically so a
	// mode switch (text <-> affine) can never leave a cache's `valid` flag
	// true while the TMEM slot it references was silently overwritten by
	// the other variant's bake in between -- see the "Deviations" section
	// of task 6's log entry for the cross-mode-clobber hazard this avoids.
	GXTlutObj tlutObj;
	void *tlutData;
	u16 mapWpx, mapHpx;
	bool valid;
	// gx-next-steps-log.md task 5: last-baked-configuration fingerprint
	// (mapWpx/mapHpx above double as the resolved map-size half of this
	// fingerprint -- the four BGxCNT size selectors produce four distinct
	// (mapWpx,mapHpx) pairs, so no separate size field is needed). See
	// gxBgPlaneNeedsRebake().
	u32 cfgCharBase, cfgMapBase;
	bool cfgColorMode;
};
static GxBgPlaneCache s_bgPlane[4];
static const int kBgPlaneMaxPx = 512;
static u16 *s_bgBakeScratch; // linear (pre-swizzle) scratch, reused per-BG (toEffectScratch RGB5A3 path only -- see gxBakeBgPlane)
static u8 *s_bgBakeIdxScratch; // gx-next-steps-log.md task 6: linear raw-index scratch for the persistent CI8 bake, reused per-BG

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
	// gx-next-steps-log.md task 6: dedicated TLUT, same pattern/rationale as
	// GxBgPlaneCache's tlutObj/tlutData above -- GX_TLUT6 (which=0, BG2) /
	// GX_TLUT7 (which=1, BG3), deliberately NOT shared with the text-mode
	// GX_TLUT0-3 slots, so a mode switch can't clobber a still-"valid"
	// cache's TMEM TLUT contents out from under it (see GxBgPlaneCache's
	// comment for the full hazard this avoids).
	GXTlutObj tlutObj;
	void *tlutData; // fixed 256-entry RGB5A3 table (affine BG is always 8bpp) -- allocated once at init, not regrown
	u16 mapPx;      // logical (unbordered) map size, square
	u16 bufPx;      // baked/allocated texture size (mapPx, or mapPx+8 if bordered -- see gxBakeAffineBgPlane's CI8 padding note)
	bool wrap;
	bool valid;
	// gx-next-steps-log.md task 5: last-baked-configuration fingerprint
	// (mapPx/wrap above already capture the size-selector and overflow-bit
	// half of this fingerprint -- see gxAffineBgPlaneNeedsRebake()).
	u32 cfgCharBase, cfgMapBase;
};
static GxAffineBgPlaneCache s_affBgPlane[2];
// gx-next-steps-log.md task 6: +8 (not +4) so the persistent CI8 bake's
// bordered buffer size (mapPx+8, see gxBakeAffineBgPlane) always fits --
// CI8's block shape is 8x4 (gx_texformat.h), wider than RGB5A3's 4x4, so a
// +4 pad (only ever a multiple of 4) isn't guaranteed to be a multiple of 8
// the way +8 is (mapPx itself is always a multiple of 8). The toEffectScratch
// RGB5A3 path is unaffected -- it still only ever needs mapPx+4.
static const int kAffineBgPlaneMaxPx = 1024 + 8;
static u16 *s_affBgBakeScratch; // linear (pre-swizzle) scratch, reused per-BG (toEffectScratch RGB5A3 path only)
static u8 *s_affBgBakeIdxScratch; // gx-next-steps-log.md task 6: linear raw-index scratch for the persistent CI8 bake, reused per-BG

// ---------------------------------------------------------------------
// OBJ: one persistent texture slot per OAM index (128), sized to the max
// regular-OBJ box (64x64). gx-next-steps-log.md task 4: each slot also
// remembers the OAM fields + DISPCNT 1D/2D bit its *current* bake was
// built from, so gxObjNeedsRebake() can skip re-decoding a sprite whose
// own config and dependency bytes (VRAM tiles + palette entries) are both
// unchanged since, instead of re-baking every visible sprite on any
// VRAM/palette write anywhere.
// ---------------------------------------------------------------------
struct GxObjTexSlot {
	GXTexObj texObj;
	void *texData;
	// gx-next-steps-log.md task 6: this sprite's dedicated TLUT source data
	// (up to 256 RGB5A3 entries -- 16 for CI4/4bpp, 256 for CI8/8bpp). All
	// 128 slots share ONE hardware TLUT name, GX_TLUT5 (unlike the BG planes
	// above, which each get their own permanently-dedicated slot): 128
	// sprites can't each have a dedicated TMEM TLUT (only 16 GX_TLUTn names
	// exist), so instead every draw of a persistent-cache OBJ texture
	// reloads GX_TLUT5 from this slot's own tlutData immediately before
	// GX_LoadTexObj (see gxDrawObjLayerWindowed) -- the index texture data
	// itself is still cached/reused across frames exactly as task 4 already
	// does; only the TLUT is reloaded (cheap: 32-512 bytes) every draw.
	GXTlutObj tlutObj;
	void *tlutData;
	bool valid;       // false until this slot has been baked at least once
	bool oneDim;       // DISPCNT bit6 at bake time
	bool colorMode;    // attribute0 bit13 (4bpp/8bpp) at bake time
	u16 tileNum;       // attribute2 bits0-9 at bake time
	u8  palNum;        // attribute2 bits12-15 at bake time (4bpp only, harmless for 8bpp)
	u16 texW, texH;    // resolved source pixel size at bake time (shape+size)
	u16 bufW, bufH;    // gx-next-steps-log.md task 6: actual baked/allocated CI4/CI8 texture size (texW/texH + 8 -- see gxBakeObjTexture's padding note), needed by gxDrawOneObj's UV normalization since it now differs from the toEffectScratch RGB5A3 path's own texW/texH+4 scheme
};
static GxObjTexSlot s_objTex[128];
static const int kObjTexMaxPx = 64; // max regular-OBJ box (visibility/size gate)
// Every OBJ texture is baked with a 1-texel transparent border so affine
// sampling that lands outside the real sprite content clamps onto
// transparent border texels instead of smearing the edge row/column -- see
// gx_gba_render.h. Non-affine sprites never actually sample the border
// (their UV range is always exactly [0,w]x[0,h] in source-texel space) but
// are baked the same way for one code path.
//
// gx-next-steps-log.md task 6: +8, not +4 -- same reasoning as
// kAffineBgPlaneMaxPx above (CI4's block shape is 8x8 and CI8's is 8x4,
// gx_texformat.h, both wider than RGB5A3's 4x4 that the old +4 padding was
// sized for). The toEffectScratch RGB5A3 path still uses its own local
// texW+4/texH+4 buffers (see gxBakeObjTexture), unaffected by this constant.
static const int kObjTexBufMaxPx = kObjTexMaxPx + 8;
static u16 *s_objBakeScratch; // toEffectScratch RGB5A3 path only
static u8 *s_objBakeIdxScratch; // gx-next-steps-log.md task 6: persistent CI4/CI8 path

struct GxObjDraw {
	s16 x, y;             // on-screen top-left of the (possibly doubled) box
	u16 boxW, boxH;        // on-screen box size (doubled for double-size affine)
	u16 texW, texH;        // source sprite pixel size (<= kObjTexMaxPx)
	u8 priority;
	bool hflip, vflip;     // meaningful only when !affine
	bool affine;
	s16 pa, pb, pc, pd;    // meaningful only when affine, 8.8 fixed point
	u8 oamIndex;
	bool semiTransparent;  // attribute0 objMode==1 -- forces alpha-blend
	                       // 1st-target behavior regardless of BLDCNT's OBJ
	                       // bit, GBATEK; see gx_gba_render.h's "Blend".
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
// gx-next-steps-log.md task 6: mode 4 (8bpp palette-indexed bitmap) gets its
// own CI8+TLUT texture object, kept separate from s_bmpTexObj/s_bmpTexData
// (modes 3/5, direct 16bpp truecolor, stay RGB5A3 unchanged -- see
// gx_gba_render.h). s_bmpValid/s_bmpValidCI are mutually exclusive in
// practice (DISPCNT's mode field picks exactly one bitmap mode per frame)
// but kept as separate flags rather than one shared bool + format tag, to
// avoid touching the mode-3/5 code path's existing flag at all.
static GXTexObj s_bmpTexObjCI;
static void *s_bmpTexDataCI;
static GXTlutObj s_bmpTlutObj;
static void *s_bmpTlutData;
static bool s_bmpValidCI;
static u8 *s_bmpBakeIdxScratch;

// 1x1 solid-color texture used for the backdrop fill (see gxGbaRenderFrame).
static GXTexObj s_backdropTexObj;
static void *s_backdropTexData;
static bool s_backdropValid; // gx-next-steps-log.md task 5: false until first bake

// ---------------------------------------------------------------------
// Shared scratch texture for the bake-time blend effect's "effected"
// variant of whichever single layer currently needs one -- see
// gx_gba_render.h's "Blend" section. A layer's ordinary (unblended) bake
// stays in its own persistent cache above (s_bgPlane/s_affBgPlane/
// s_objTex/s_bmpTexObj) exactly as before this task; this one extra slot
// is re-baked transiently, immediately before whichever draw call needs
// the blended version, and never cached across draws or frames. Sized for
// the largest layer kind this file bakes (affine BG plane, up to
// kAffineBgPlaneMaxPx^2); grown on demand like s_affBgPlane's cache.
// ---------------------------------------------------------------------
struct GxEffectScratch { GXTexObj texObj; void *texData; u32 cap; };
static GxEffectScratch s_effectTex;

// Swizzles `linear` (w x h RGB5A3 texels, w/h already multiples of 4) into
// s_effectTex and initializes its GXTexObj with the given wrap mode.
static void gxUploadEffectTex(const u16 *linear, int w, int h, u8 wrapMode)
{
	u32 needed = (u32)w * h * sizeof(u16);
	if (needed > s_effectTex.cap) {
		if (s_effectTex.texData) free(s_effectTex.texData);
		s_effectTex.texData = memalign(32, needed);
		s_effectTex.cap = s_effectTex.texData ? needed : 0;
	}
	if (!s_effectTex.texData) return;
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(linear, (u16 *)s_effectTex.texData, blk.texelsWide, blk.texelsTall, w, h);
	DCFlushRange(s_effectTex.texData, needed);
	GX_InitTexObj(&s_effectTex.texObj, s_effectTex.texData, w, h, GX_TF_RGB5A3, wrapMode, wrapMode, GX_FALSE);
}

// ---------------------------------------------------------------------
// gx-next-steps-log.md task 6: fixed GX_TLUTn name assignment for every
// persistent CI4/CI8 texture this file bakes -- see gx_gba_render.h's
// "Native CI4/CI8" section. Text BG and affine BG each get their OWN
// dedicated slot (not shared between a BG index's text/affine variants)
// specifically to avoid a cross-mode-clobber hazard: see GxBgPlaneCache's
// comment above. 8 of the 16 available GX_TLUT0-15 names are used, well
// within budget (GX_Init()'s default TMEM layout reserves all 16).
// ---------------------------------------------------------------------
static inline u8 gxBgTlutSlot(int bg) { return (u8)(GX_TLUT0 + bg); }              // BG0-3 text mode
static const u8 kBmpTlutSlot = GX_TLUT4;                                          // bitmap mode 4
static const u8 kObjTlutSlot = GX_TLUT5;                                          // OBJ, shared/reloaded per-draw
static inline u8 gxAffineBgTlutSlot(int which) { return (u8)(GX_TLUT6 + which); }  // affine BG2/BG3

// Copy-back scratch: GX_CopyTex's destination is real system memory, but in
// GX's own block-tiled layout for the copy format (see gx_texformat.h) --
// gxUnswizzle16bpp turns that back into a plain row-major buffer before the
// RGB5A3->GBA-BGR555 conversion pass writes into GBA_screen.
static void *s_copyBackBuf;
static u16 *s_copyBackLinear;

// gx-next-steps-log.md task 3: true only while s_copyBackBuf holds *this*
// frame's real GX_CopyTex output (the actual draw+copy path just below,
// not the forced-blank or CPU-bail early returns in gxGbaRenderFrame()) --
// set true right before that function's final `return true`, and false at
// its very first line every call, so every other return path (there are
// several bail points) leaves it false with no need to touch each one.
// Consumed by gxGbaBlitNativeTop(), main.cpp's Draw() direct-present fast
// path -- see that function's comment and gx_gba_render.h's header.
static bool s_lastFrameNative = false;

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
		s_bgPlane[i].tlutData = memalign(32, 256 * sizeof(u16));
		if (!s_bgPlane[i].tlutData) return false;
		memset(s_bgPlane[i].tlutData, 0, 256 * sizeof(u16));
		s_bgPlane[i].valid = false;
	}
	s_bgBakeScratch = (u16 *)malloc(kBgPlaneMaxPx * kBgPlaneMaxPx * sizeof(u16));
	if (!s_bgBakeScratch) return false;
	s_bgBakeIdxScratch = (u8 *)malloc((u32)kBgPlaneMaxPx * kBgPlaneMaxPx);
	if (!s_bgBakeIdxScratch) return false;

	for (int i = 0; i < 2; ++i) {
		s_affBgPlane[i].texData = nullptr;
		s_affBgPlane[i].texDataCap = 0;
		s_affBgPlane[i].tlutData = memalign(32, 256 * sizeof(u16));
		if (!s_affBgPlane[i].tlutData) return false;
		memset(s_affBgPlane[i].tlutData, 0, 256 * sizeof(u16));
		s_affBgPlane[i].valid = false;
	}
	s_affBgBakeScratch = (u16 *)malloc((u32)kAffineBgPlaneMaxPx * kAffineBgPlaneMaxPx * sizeof(u16));
	if (!s_affBgBakeScratch) return false;
	s_affBgBakeIdxScratch = (u8 *)malloc((u32)kAffineBgPlaneMaxPx * kAffineBgPlaneMaxPx);
	if (!s_affBgBakeIdxScratch) return false;

	for (int i = 0; i < 128; ++i) {
		u32 sz = kObjTexBufMaxPx * kObjTexBufMaxPx * sizeof(u16);
		s_objTex[i].texData = memalign(32, sz);
		if (!s_objTex[i].texData) return false;
		memset(s_objTex[i].texData, 0, sz);
		s_objTex[i].tlutData = memalign(32, 256 * sizeof(u16));
		if (!s_objTex[i].tlutData) return false;
		memset(s_objTex[i].tlutData, 0, 256 * sizeof(u16));
		s_objTex[i].valid = false;
	}
	s_objBakeScratch = (u16 *)malloc(kObjTexBufMaxPx * kObjTexBufMaxPx * sizeof(u16));
	if (!s_objBakeScratch) return false;
	s_objBakeIdxScratch = (u8 *)malloc((u32)kObjTexBufMaxPx * kObjTexBufMaxPx);
	if (!s_objBakeIdxScratch) return false;

	u32 bmpSz = GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16);
	s_bmpTexData = memalign(32, bmpSz);
	if (!s_bmpTexData) return false;
	memset(s_bmpTexData, 0, bmpSz);
	s_bmpValid = false;

	u32 bmpCiSz = GBA_SCREEN_W * GBA_SCREEN_H; // CI8: 1 byte/texel
	s_bmpTexDataCI = memalign(32, bmpCiSz);
	if (!s_bmpTexDataCI) return false;
	memset(s_bmpTexDataCI, 0, bmpCiSz);
	s_bmpTlutData = memalign(32, 256 * sizeof(u16));
	if (!s_bmpTlutData) return false;
	memset(s_bmpTlutData, 0, 256 * sizeof(u16));
	s_bmpBakeIdxScratch = (u8 *)malloc(bmpCiSz);
	if (!s_bmpBakeIdxScratch) return false;
	s_bmpValidCI = false;

	s_backdropTexData = memalign(32, 32); // GX's minimum texture alloc granularity
	if (!s_backdropTexData) return false;
	memset(s_backdropTexData, 0, 32);
	GX_InitTexObj(&s_backdropTexObj, s_backdropTexData, 1, 1, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	s_backdropValid = false;

	u32 copySz = GX_GetTexBufferSize(GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_RGB5A3, GX_FALSE, 0);
	s_copyBackBuf = memalign(32, copySz);
	if (!s_copyBackBuf) return false;
	s_copyBackLinear = (u16 *)malloc(GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	if (!s_copyBackLinear) return false;

	s_effectTex.texData = nullptr;
	s_effectTex.cap = 0;

	s_initDone = true;
	return true;
}

void gxGbaRenderShutdown()
{
	if (!s_initDone)
		return;
	for (int i = 0; i < 4; ++i) { free(s_bgPlane[i].texData); s_bgPlane[i].texData = nullptr; free(s_bgPlane[i].tlutData); s_bgPlane[i].tlutData = nullptr; }
	free(s_bgBakeScratch); s_bgBakeScratch = nullptr;
	free(s_bgBakeIdxScratch); s_bgBakeIdxScratch = nullptr;
	for (int i = 0; i < 2; ++i) { free(s_affBgPlane[i].texData); s_affBgPlane[i].texData = nullptr; s_affBgPlane[i].texDataCap = 0; free(s_affBgPlane[i].tlutData); s_affBgPlane[i].tlutData = nullptr; }
	free(s_affBgBakeScratch); s_affBgBakeScratch = nullptr;
	free(s_affBgBakeIdxScratch); s_affBgBakeIdxScratch = nullptr;
	for (int i = 0; i < 128; ++i) { free(s_objTex[i].texData); s_objTex[i].texData = nullptr; free(s_objTex[i].tlutData); s_objTex[i].tlutData = nullptr; }
	free(s_objBakeScratch); s_objBakeScratch = nullptr;
	free(s_objBakeIdxScratch); s_objBakeIdxScratch = nullptr;
	free(s_bmpTexData); s_bmpTexData = nullptr;
	free(s_bmpTexDataCI); s_bmpTexDataCI = nullptr;
	free(s_bmpTlutData); s_bmpTlutData = nullptr;
	free(s_bmpBakeIdxScratch); s_bmpBakeIdxScratch = nullptr;
	free(s_backdropTexData); s_backdropTexData = nullptr;
	free(s_copyBackBuf); s_copyBackBuf = nullptr;
	free(s_copyBackLinear); s_copyBackLinear = nullptr;
	free(s_effectTex.texData); s_effectTex.texData = nullptr; s_effectTex.cap = 0;
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

// `fx`/`toEffectScratch`: see gx_gba_render.h's "Blend" section. The
// ordinary (unblended, GXTEXEFFECT_NONE) call sites behave exactly as
// before this task; `toEffectScratch` bakes this BG's blended variant
// into the shared transient s_effectTex scratch instead of its own
// persistent cache, for use alongside a plain-cache draw of the same BG
// within the same band (see gxDrawBgLayerWindowed).
static void gxBakeBgPlane(int bg, const GxTexelEffectParams &fx = kTexelEffectNone, bool toEffectScratch = false)
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
					int dstIdx = (ty * 8 + suby) * mapWpx + (tx * 8 + subx);
					if (!li.colorMode) {
						u32 tileAddr = li.charBase + tileNum * 32 + srcY * 4 + srcX / 2;
						u8 byte = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
						int idx = (srcX & 1) ? (byte >> 4) : (byte & 0xF);
						if (toEffectScratch)
							s_bgBakeScratch[dstIdx] = gxTexel(gxBgPalColor(palNum * 16 + idx), idx != 0, fx);
						else
							// gx-next-steps-log.md task 6: combined 8-bit index
							// (palNum*16+idx), not the raw 4-bit nibble -- see the
							// CI8 comment block below for why 4bpp text BG can't
							// use plain CI4.
							s_bgBakeIdxScratch[dstIdx] = (u8)(palNum * 16 + idx);
					} else {
						u32 tileAddr = li.charBase + tileNum * 64 + srcY * 8 + srcX;
						u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
						if (toEffectScratch)
							s_bgBakeScratch[dstIdx] = gxTexel(gxBgPalColor(idx), idx != 0, fx);
						else
							s_bgBakeIdxScratch[dstIdx] = idx;
					}
				}
			}
		}
	}

	if (toEffectScratch) {
		gxUploadEffectTex(s_bgBakeScratch, mapWpx, mapHpx, GX_REPEAT);
		return;
	}

	// gx-next-steps-log.md task 6: CI8 raw-index bake, replacing the old
	// pre-resolved RGB5A3 bake, for the persistent (non-effect) cache --
	// see gx_gba_render.h's "Native CI4/CI8" section. Both 4bpp and 8bpp
	// text BG content are baked into the SAME CI8 format (not CI4 for
	// 4bpp, despite the task brief's literal wording) using an 8-bit
	// COMBINED index (palNum*16+idx for 4bpp, idx directly for 8bpp) --
	// a deliberate, documented deviation: a text BG map entry carries its
	// OWN palNum per TILE (bits 12-15), so a single baked plane texture
	// routinely spans multiple different 16-color sub-banks (palNum
	// values) across its tiles, but GX binds exactly one TLUT per texture
	// object per draw call -- a 16-entry CI4 TLUT could only ever
	// correctly represent ONE of those sub-banks, silently corrupting
	// every tile using a different one. The combined index already
	// exactly matches gxBgPalColor()'s own existing palette-offset math
	// (see the toEffectScratch branch above, unchanged), and a 256-entry
	// CI8 TLUT spanning the *whole* BG palette bank makes every possible
	// combined-index value resolve correctly regardless of which
	// sub-banks this plane's tiles actually reference -- still a real,
	// correct bandwidth/TMEM win over RGB5A3 (8 bits/texel instead of 16),
	// just not the maximal 4-bit win a per-palette-group multi-draw
	// scheme (nds-wii-texture-format-mapping.md's "Extended-palette 2D
	// BGs" option) would have given for the 4bpp case -- that multi-draw
	// scheme is a real, larger follow-up, not attempted this pass.
	GxBgPlaneCache &pc = s_bgPlane[bg];
	GXBlockShape blk = gxBlockShape(GXTEXFMT_CI8);
	gxSwizzle8bpp(s_bgBakeIdxScratch, (u8 *)pc.texData, blk.texelsWide, blk.texelsTall, mapWpx, mapHpx);
	DCFlushRange(pc.texData, (u32)mapWpx * mapHpx);

	// Index-0 transparency (see gx_gba_render.h "Native CI4/CI8" and the
	// two correctness subtleties in this task's log entry): GBA hardware
	// treats palette index 0 WITHIN EACH 16-color sub-bank as transparent
	// for 4bpp tile texels -- i.e. every combined index that's a multiple
	// of 16 (palNum*16+0, for every palNum 0-15), not just combined index
	// 0 itself. For 8bpp there's only one bank, so only combined index 0
	// is transparent.
	u16 *tlut = (u16 *)pc.tlutData;
	for (int e = 0; e < 256; ++e) {
		bool transparentEntry = li.colorMode ? (e == 0) : ((e & 0xF) == 0);
		tlut[e] = transparentEntry ? kTransparentTexel : gxOpaqueTexel(gxBgPalColor(e));
	}
	DCFlushRange(pc.tlutData, 256 * sizeof(u16));
	u8 tlutSlot = gxBgTlutSlot(bg);
	GX_InitTlutObj(&pc.tlutObj, pc.tlutData, GX_TL_RGB5A3, 256);
	GX_LoadTlut(&pc.tlutObj, tlutSlot);
	GX_InitTexObjCI(&pc.texObj, pc.texData, mapWpx, mapHpx, GX_TF_CI8, GX_REPEAT, GX_REPEAT, GX_FALSE, tlutSlot);
	pc.mapWpx = (u16)mapWpx;
	pc.mapHpx = (u16)mapHpx;
	pc.cfgCharBase = li.charBase;
	pc.cfgMapBase = li.mapBase;
	pc.cfgColorMode = li.colorMode;
	pc.valid = true;
}

// Affine BG map entries are 1 byte (tile index 0-255 into a fixed 8bpp
// charset), no per-tile flip -- unlike text mode's 2-byte entries with
// hflip/vflip/palette bits. which: 0=BG2, 1=BG3.
static void gxBakeAffineBgPlane(int which, u16 cnt, const GxTexelEffectParams &fx = kTexelEffectNone, bool toEffectScratch = false)
{
	u32 charBase = ((cnt >> 2) & 3) * 0x4000;
	u32 mapBase = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel = (cnt >> 14) & 3;
	int mapTiles = 16 << sizeSel; // 16/32/64/128
	int mapPx = mapTiles * 8;
	bool wrap = (cnt >> 13) & 1;

	if (toEffectScratch) {
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
						u16 texel = gxTexel(gxBgPalColor(idx), idx != 0, fx);
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
		gxUploadEffectTex(s_affBgBakeScratch, bufPx, bufPx, wrap ? GX_REPEAT : GX_CLAMP);
		return;
	}

	// gx-next-steps-log.md task 6: CI8 raw-index bake for the persistent
	// cache -- affine BG map entries are a full byte (0-255) indexing
	// directly into an always-8bpp charset with NO per-tile palette
	// selection (unlike text mode), so this is a clean, exact CI8
	// conversion: one 256-entry TLUT covering the whole BG palette bank,
	// no combined-index trick needed.
	//
	// Buffer padding: +8, not +4, when bordered (see kAffineBgPlaneMaxPx's
	// comment) -- CI8's block shape is 8x4 (gx_texformat.h), and mapPx+4
	// isn't guaranteed to be a multiple of 8 the way mapPx+8 is (mapPx
	// itself always is). The wrap (unbordered) case needs no padding at
	// all -- mapPx is already a multiple of 8.
	int bufPx = wrap ? mapPx : mapPx + 8;
	int borderOff = wrap ? 0 : 1;
	// Zero the WHOLE buffer up front (index 0 == transparent via the TLUT
	// built below) rather than writing an explicit 1-texel border loop
	// afterward -- this also correctly zeros the extra unused padding
	// columns/rows beyond the real 1-texel border (indices [mapPx+2,
	// bufPx) when bordered), which a border-only loop would leave as
	// stale scratch-buffer content; GX_CLAMP never samples out there, but
	// zeroing is cheap and removes any doubt.
	memset(s_affBgBakeIdxScratch, 0, (size_t)bufPx * bufPx);
	for (int ty = 0; ty < mapTiles; ++ty) {
		for (int tx = 0; tx < mapTiles; ++tx) {
			u32 entryAddr = mapBase + (ty * mapTiles + tx);
			u8 tileNum = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(entryAddr));
			for (int suby = 0; suby < 8; ++suby) {
				for (int subx = 0; subx < 8; ++subx) {
					u32 tileAddr = charBase + tileNum * 64 + suby * 8 + subx;
					u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(tileAddr));
					int dstX = tx * 8 + subx + borderOff;
					int dstY = ty * 8 + suby + borderOff;
					s_affBgBakeIdxScratch[dstY * bufPx + dstX] = idx;
				}
			}
		}
	}

	GxAffineBgPlaneCache &pc = s_affBgPlane[which];
	u32 needed = (u32)bufPx * bufPx; // CI8: 1 byte/texel
	if (needed > pc.texDataCap) {
		if (pc.texData) free(pc.texData);
		pc.texData = memalign(32, needed);
		pc.texDataCap = pc.texData ? needed : 0;
	}
	if (!pc.texData) { pc.valid = false; return; }

	GXBlockShape blk = gxBlockShape(GXTEXFMT_CI8);
	gxSwizzle8bpp(s_affBgBakeIdxScratch, (u8 *)pc.texData, blk.texelsWide, blk.texelsTall, bufPx, bufPx);
	DCFlushRange(pc.texData, needed);

	// Index-0 transparency: affine BG tile texels use idx!=0 as their
	// opacity test (see the toEffectScratch branch above), so TLUT entry 0
	// must be forced alpha=0 regardless of its real stored RGB -- this is
	// also what makes the border-clamp technique (border/padding filled
	// with raw index 0 above) keep working under CI8.
	u16 *tlut = (u16 *)pc.tlutData;
	for (int e = 0; e < 256; ++e)
		tlut[e] = (e == 0) ? kTransparentTexel : gxOpaqueTexel(gxBgPalColor(e));
	DCFlushRange(pc.tlutData, 256 * sizeof(u16));
	u8 tlutSlot = gxAffineBgTlutSlot(which);
	GX_InitTlutObj(&pc.tlutObj, pc.tlutData, GX_TL_RGB5A3, 256);
	GX_LoadTlut(&pc.tlutObj, tlutSlot);
	u8 wm = wrap ? GX_REPEAT : GX_CLAMP;
	GX_InitTexObjCI(&pc.texObj, pc.texData, bufPx, bufPx, GX_TF_CI8, wm, wm, GX_FALSE, tlutSlot);
	pc.mapPx = (u16)mapPx;
	pc.bufPx = (u16)bufPx;
	pc.wrap = wrap;
	pc.cfgCharBase = charBase;
	pc.cfgMapBase = mapBase;
	pc.valid = true;
}

static void gxBakeBackdrop()
{
	u16 texel = gxOpaqueTexel(gxBgPalColor(0));
	((u16 *)s_backdropTexData)[0] = texel;
	DCFlushRange(s_backdropTexData, 32);
	s_backdropValid = true;
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
		d.semiTransparent = (objMode == 1);
	}
	return true;
}

// ---------------------------------------------------------------------
// gx-next-steps-log.md task 4: per-OAM-index re-bake gate. A baked OBJ
// texture is a pure function of (DISPCNT's OBJ 1D/2D mapping bit, this
// OAM entry's own color-mode/shape/size/tile-index/palette-index fields,
// and the exact VRAM tile bytes + palette bytes that configuration reads)
// -- nothing else. Screen position/priority/flip and the affine matrix
// are read fresh every frame regardless (gxCollectVisibleObj/
// gxAffineObjTexCorner) and never affect the baked pixels, so they are
// deliberately NOT part of this dependency set -- including them would
// only defeat the cache for ordinary moving/animating-via-matrix sprites
// without buying any extra correctness.
// ---------------------------------------------------------------------

// Page-granularity range query built on top of GxDirtyBitmap's existing
// isPageDirty() -- see gx_frameplan.h; no new dirty-tracking infrastructure,
// just a range-shaped consumer of what's already there.
static bool gxRangeDirty(const GxDirtyBitmap &bm, u32 offset, u32 size)
{
	if (size == 0) return false;
	u32 pageSize = bm.pageSize();
	if (pageSize == 0) return bm.anyDirty(); // untracked bitmap: conservative fallback
	u32 first = offset / pageSize;
	u32 last = (offset + size - 1) / pageSize;
	for (u32 p = first; p <= last; ++p)
		if (bm.isPageDirty(p)) return true;
	return false;
}

// Wraps a raw VRAM byte range [rawAddr, rawAddr+len) through gxVramAddr()'s
// own mirror-wrap rule (see its comment) and queries dirty status for the
// resulting real-VRAM sub-range(s). Shared by every VRAM dependency check in
// this file (OBJ, task 4; BG/affine-BG, task 5) rather than duplicated per
// caller. A span landing entirely on one side of the 0x18000 mirror boundary
// just needs that one offset normalized; a span straddling it is checked as
// two sub-ranges. A span reaching past the 17-bit mask boundary gxVramAddr
// applies per-byte (addr & 0x1FFFF) is pathological (e.g. an OBJ tileNum near
// its 10-bit max combined with a large sprite, or a BG char-base index 3
// combined with 8bpp's full 1024-tile addressable range -- see
// gxBgPlaneNeedsRebake) and not worth reasoning about precisely -- bail
// conservative (treat as dirty) instead.
static bool gxVramSpanDirty(u32 rawAddr, u32 len)
{
	u32 end = rawAddr + len;
	if (end > 0x20000) return true;
	if (rawAddr < 0x18000 && end > 0x18000) {
		return gxRangeDirty(g_gbaFramePlan.vram, rawAddr, 0x18000 - rawAddr) ||
		       gxRangeDirty(g_gbaFramePlan.vram, 0x18000 - 0x8000, end - 0x18000);
	}
	u32 m = (rawAddr >= 0x18000) ? (rawAddr - 0x8000) : rawAddr;
	return gxRangeDirty(g_gbaFramePlan.vram, m, len);
}

// OBJ char-VRAM tile-grid byte range dirty check for a `tilesW`x`tilesH`
// (in whole 8px tiles) sprite starting at `tileNum`, reusing exactly the
// 1D/2D addressing math gxBakeObjTexture uses to decode it (see there)
// rather than re-deriving it independently.
static bool gxObjVramDepsDirty(bool oneDim, bool colorMode, int tileNum, int tilesW, int tilesH)
{
	const u32 charBase = 0x10000;
	u32 tileBytes = colorMode ? 64 : 32;

	if (oneDim) {
		// Contiguous: tileIndex = tileNum + tileY*tilesW + tileX enumerates
		// [tileNum, tileNum + tilesW*tilesH) with no gaps.
		u32 numTiles = (u32)tilesW * (u32)tilesH;
		return gxVramSpanDirty(charBase + (u32)tileNum * tileBytes, numTiles * tileBytes);
	}
	// 2D: each tile row is contiguous but rows are spaced by a fixed
	// 32-tile-slot stride regardless of sprite width -- check per row,
	// same slotIndex math as gxBakeObjTexture's 2D branch.
	int rowSlots = tilesW * (colorMode ? 2 : 1);
	for (int tileY = 0; tileY < tilesH; ++tileY) {
		u32 slotIndex = (u32)tileNum + (u32)tileY * 32;
		if (gxVramSpanDirty(charBase + slotIndex * 32, (u32)rowSlots * 32))
			return true;
	}
	return false;
}

// Palette dependency: a 4bpp sprite only ever reads its own 16-entry
// sub-palette (palNum*16..+15); an 8bpp sprite reads the full 256-entry
// OBJ palette bank (see gxBakeObjTexture's gxObjPalColor(idx) call, no
// palNum offset at all in that branch) -- so, contrary to a naive
// "whole OBJ palette bank is always a dependency" assumption, most
// sprites (4bpp is the overwhelmingly common case) only actually care
// about a 32-byte slice.
static bool gxObjPaletteDepsDirty(bool colorMode, int palNum)
{
	if (colorMode) // 8bpp
		return gxRangeDirty(g_gbaFramePlan.palette, 0x200, 512);
	return gxRangeDirty(g_gbaFramePlan.palette, 0x200 + (u32)palNum * 32, 32);
}

// True if s_objTex[d.oamIndex] is stale and gxBakeObjTexture() needs to
// run for it this frame -- either its own OAM fields changed since the
// last bake into this slot (including a completely different sprite now
// occupying this OAM index: its tile-index/shape/size/color-mode/
// palette-index will essentially always differ, and even in the
// coincidental case they don't, the resulting bake is provably identical
// bytes since both bakes read the same VRAM/palette bytes under the same
// field values), or its fields are unchanged but the exact VRAM/palette
// bytes that configuration depends on were written since.
static bool gxObjNeedsRebake(u16 dispcnt, const GxObjDraw &d)
{
	const GxObjTexSlot &slot = s_objTex[d.oamIndex];

	u32 oamOff = d.oamIndex * 8;
	u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
	u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);
	bool colorMode = (a0 >> 13) & 1;
	int tileNum = a2 & 0x3FF;
	int palNum = (a2 >> 12) & 0xF;
	bool oneDim = (dispcnt >> 6) & 1;

	bool sameConfig = slot.valid &&
		slot.oneDim == oneDim && slot.colorMode == colorMode &&
		slot.tileNum == tileNum && slot.palNum == (u8)palNum &&
		slot.texW == d.texW && slot.texH == d.texH;
	if (!sameConfig)
		return true;

	if (gxObjVramDepsDirty(oneDim, colorMode, tileNum, d.texW / 8, d.texH / 8))
		return true;
	return gxObjPaletteDepsDirty(colorMode, palNum);
}

// ---------------------------------------------------------------------
// gx-next-steps-log.md task 5: backdrop + per-BG-plane re-bake gates, same
// fingerprint + gxRangeDirty()/gxVramSpanDirty() pattern as task 4's OBJ
// gate above -- replaces the single coarse `g_gbaFramePlan.vram.anyDirty()
// || palette.anyDirty()` check that used to gate the backdrop and all four
// BG planes together (see gxGbaRenderFrame).
//
// Dependency-range design (see this task's log section for the full
// writeup):
//  - Text BG char-VRAM range: this file's existing bake loop only ever
//    reads tile indices actually present in the current tilemap, so a
//    byte-exact dependency would need to walk the map first (same cost as
//    the bake itself) just to decide whether to bake. Deliberately NOT
//    done: instead this uses a conservative superset, [charBase, charBase +
//    1024*tileBytes) -- the full range any of the map's 10-bit tile indices
//    could possibly address at this BG's current color depth. Note this is
//    wider than "one 16KB char-base block": a 10-bit tile index can reach
//    up to 2 blocks (4bpp, 32KB) or 4 blocks (8bpp, 64KB) past the selected
//    base, so a single-block range would have been an UNDER-count (a real
//    correctness bug, not just an imprecision) -- widened here per this
//    task's "when in doubt, widen" rule rather than trusting a single-block
//    assumption that doesn't hold for every BGxCNT color-depth combination.
//  - Text BG map-VRAM range: [mapBase, mapBase + mapWtiles*mapHtiles*2) --
//    exact, not conservative: gxBakeBgPlane's own blockIdx addressing
//    (blockIdx = blockX + blockY*(mapWtiles/32)) always produces a
//    contiguous span of whole 0x800-byte blocks starting at mapBase for
//    every one of the four BGxCNT size selectors, so this range is a
//    precise description of what the bake loop reads, no map-walk needed.
//  - Affine BG char-VRAM range: [charBase, charBase + 256*64) -- affine
//    tile indices are a full byte (0-255) into an always-8bpp (64
//    bytes/tile) charset, so this is the exact full addressable range, not
//    an approximation.
//  - Affine BG map-VRAM range: [mapBase, mapBase + mapTiles*mapTiles) --
//    exact (1 byte per map entry, contiguous, no block-splitting like text
//    mode).
//  - Palette: every BG plane (4bpp text, 8bpp text, and affine, which is
//    always 8bpp) gates on the *whole* 512-byte BG palette bank rather than
//    the tightest-possible per-map-entry sub-palette. Unlike OBJ (task 4),
//    a sprite has one fixed OAM palNum field, so tracking its exact
//    16-color sub-palette is free; a text BG's map has a DIFFERENT palNum
//    per tile entry, so computing the exact set of referenced sub-palettes
//    would require the same full map walk the char-VRAM case above already
//    declined for the same reason (chicken-and-egg with the bake itself).
//    512 bytes is small next to the VRAM savings already made above, so
//    this is a deliberately simple, safe choice, not an oversight.
//
// HOFS/VOFS (scroll) and, for affine, PA/PB/PC/PD/reference-point are
// intentionally NOT part of any fingerprint here -- see gx_gba_render.h and
// task 4's identical note for OBJ's affine matrix: those are read fresh
// every frame via UV/quad math (gxDrawBgQuad/gxDrawAffineBgQuad) and never
// affect the baked texture's pixels, so including them would only defeat
// the cache for ordinary scrolling/rotation without buying correctness.
// ---------------------------------------------------------------------

static bool gxBackdropNeedsRebake()
{
	if (!s_backdropValid)
		return true;
	// gxBakeBackdrop() reads exactly gxBgPalColor(0) == palette offset 0,
	// 2 bytes (T1ReadWord(MMU.GBA_PALETTE, 0*2)).
	return gxRangeDirty(g_gbaFramePlan.palette, 0, 2);
}

// True if s_bgPlane[bg] is stale and gxBakeBgPlane(bg) needs to run for it
// this frame -- either its own tile/map layout changed since the last bake
// into this slot, or its fields are unchanged but the (conservative, see
// above) VRAM char/map ranges or the BG palette bank were written since.
static bool gxBgPlaneNeedsRebake(int bg, const GxBgLayout &li)
{
	const GxBgPlaneCache &pc = s_bgPlane[bg];
	int mapWpx = li.mapWtiles * 8, mapHpx = li.mapHtiles * 8;

	bool sameConfig = pc.valid &&
		pc.cfgCharBase == li.charBase && pc.cfgMapBase == li.mapBase &&
		pc.cfgColorMode == li.colorMode &&
		pc.mapWpx == (u16)mapWpx && pc.mapHpx == (u16)mapHpx;
	if (!sameConfig)
		return true;

	u32 tileBytes = li.colorMode ? 64 : 32;
	if (gxVramSpanDirty(li.charBase, 1024u * tileBytes))
		return true;
	u32 mapRangeBytes = (u32)li.mapWtiles * (u32)li.mapHtiles * 2;
	if (gxVramSpanDirty(li.mapBase, mapRangeBytes))
		return true;
	return gxRangeDirty(g_gbaFramePlan.palette, 0, 512);
}

// True if s_affBgPlane[which] is stale and gxBakeAffineBgPlane(which, cnt)
// needs to run for it this frame. `which`: 0=BG2, 1=BG3.
static bool gxAffineBgPlaneNeedsRebake(int which, u16 cnt)
{
	const GxAffineBgPlaneCache &pc = s_affBgPlane[which];
	u32 charBase = ((cnt >> 2) & 3) * 0x4000;
	u32 mapBase = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel = (cnt >> 14) & 3;
	int mapTiles = 16 << sizeSel;
	int mapPx = mapTiles * 8;
	bool wrap = (cnt >> 13) & 1;

	// pc.wrap changing also changes the baked buffer's border layout (see
	// gxBakeAffineBgPlane), not just which VRAM bytes it depends on, so it
	// must be part of the config fingerprint too, same as pc.mapPx.
	bool sameConfig = pc.valid &&
		pc.cfgCharBase == charBase && pc.cfgMapBase == mapBase &&
		pc.mapPx == (u16)mapPx && pc.wrap == wrap;
	if (!sameConfig)
		return true;

	if (gxVramSpanDirty(charBase, 256u * 64))
		return true;
	u32 mapRangeBytes = (u32)mapTiles * (u32)mapTiles;
	if (gxVramSpanDirty(mapBase, mapRangeBytes))
		return true;
	return gxRangeDirty(g_gbaFramePlan.palette, 0, 512);
}

// Decodes one sprite's texW x texH source pixels into a (texW+4)x(texH+4)
// buffer with a 1-texel transparent border (see kObjTexBufMaxPx) -- baked
// the same way regardless of d.affine so both draw paths share one bake
// function; only affine sampling ever actually reaches the border.
static void gxBakeObjTexture(u16 dispcnt, const GxObjDraw &d, const GxTexelEffectParams &fx = kTexelEffectNone, bool toEffectScratch = false)
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

	if (toEffectScratch) {
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
					texel = gxTexel(gxObjPalColor(palNum * 16 + idx), idx != 0, fx);
				} else {
					u32 a = addr + suby * 8 + subx;
					u8 idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(a));
					texel = gxTexel(gxObjPalColor(idx), idx != 0, fx);
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
		gxUploadEffectTex(s_objBakeScratch, bufW, bufH, GX_CLAMP);
		return;
	}

	// gx-next-steps-log.md task 6: persistent (non-effect) OBJ bake now
	// bakes raw palette-index texels (CI4 for 4bpp, CI8 for 8bpp) instead
	// of pre-resolved RGB5A3 colors, uploaded via GX_InitTexObjCI with a
	// TLUT built from this sprite's own live GBA OBJ-palette bytes -- see
	// gx_gba_render.h's "Native CI4/CI8" section. Unlike text BG, a sprite
	// has exactly ONE fixed palNum (an OAM field, not per-texel), so a
	// plain CI4 TLUT (this sprite's own 16-color sub-bank) is exact here,
	// no combined-index trick needed.
	//
	// Buffer padding: +8, not +4 -- CI4's block shape is 8x8 and CI8's is
	// 8x4 (gx_texformat.h), both wider than RGB5A3's 4x4 that the old +4
	// padding was sized for; w/h+4 isn't guaranteed to be a multiple of 8
	// the way w/h+8 is (w/h themselves always are, being whole 8px OBJ
	// tiles). One padding scheme covers both CI4 and CI8 uniformly.
	int bufW = w + 8, bufH = h + 8;
	// Zero the whole buffer first (index 0 == transparent via the TLUT
	// below), same rationale as gxBakeAffineBgPlane's identical choice --
	// covers the real 1-texel border AND the extra unused padding beyond
	// it in one pass, rather than a border-only loop leaving the padding
	// as stale scratch-buffer content.
	memset(s_objBakeIdxScratch, 0, (size_t)bufW * bufH);
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
			u8 idx;
			if (!colorMode) {
				u32 a = addr + suby * 4 + subx / 2;
				u8 byte = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(a));
				idx = (subx & 1) ? (byte >> 4) : (byte & 0xF);
			} else {
				u32 a = addr + suby * 8 + subx;
				idx = T1ReadByte(MMU.GBA_VRAM, gxVramAddr(a));
			}
			s_objBakeIdxScratch[(texy + 1) * bufW + (texx + 1)] = idx;
		}
	}

	GxObjTexSlot &slot = s_objTex[d.oamIndex];
	GXTexFmt fmt = colorMode ? GXTEXFMT_CI8 : GXTEXFMT_CI4;
	GXBlockShape blk = gxBlockShape(fmt);
	if (colorMode)
		gxSwizzle8bpp(s_objBakeIdxScratch, (u8 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
	else
		gxSwizzle4bpp(s_objBakeIdxScratch, (u8 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
	u32 texBytes = colorMode ? (u32)bufW * bufH : (u32)(bufW * bufH) / 2;
	DCFlushRange(slot.texData, texBytes);

	// Index-0 transparency, and the sprite's own dedicated sub-palette for
	// 4bpp (see gx_gba_render.h): a sprite's tile texels use idx!=0 as
	// their opacity test, so TLUT entry 0 (this sprite's own palNum*16+0
	// for 4bpp, or OBJ-palette-bank index 0 for 8bpp) must be forced
	// alpha=0 regardless of its real stored RGB -- also what keeps the
	// affine border-clamp technique (border/padding filled with raw index
	// 0 above) working under CI4/CI8.
	u16 *tlut = (u16 *)slot.tlutData;
	int entries = colorMode ? 256 : 16;
	if (colorMode) {
		for (int e = 0; e < 256; ++e)
			tlut[e] = (e == 0) ? kTransparentTexel : gxOpaqueTexel(gxObjPalColor(e));
	} else {
		for (int e = 0; e < 16; ++e)
			tlut[e] = (e == 0) ? kTransparentTexel : gxOpaqueTexel(gxObjPalColor(palNum * 16 + e));
	}
	DCFlushRange(slot.tlutData, (u32)entries * sizeof(u16));
	GX_InitTlutObj(&slot.tlutObj, slot.tlutData, GX_TL_RGB5A3, entries);
	// Not loaded into TMEM here -- s_objTex[] slots all share ONE hardware
	// TLUT name (kObjTlutSlot); the actual GX_LoadTlut() happens at DRAW
	// time (gxDrawObjLayerWindowed), immediately before this slot's
	// texture is bound, since another sprite's draw in between would
	// otherwise have overwritten the shared slot's TMEM contents.
	// NOTE: fmt (GXTexFmt, this file's own local block-shape enum) is NOT
	// the same numbering as GX's own GX_TF_CI4/GX_TF_CI8 macros -- fmt is
	// only ever used for gxBlockShape() above; the actual GX API call
	// needs the real hardware format constant.
	GX_InitTexObjCI(&slot.texObj, slot.texData, bufW, bufH, colorMode ? GX_TF_CI8 : GX_TF_CI4, GX_CLAMP, GX_CLAMP, GX_FALSE, kObjTlutSlot);

	// Record the config this bake was built from (task 4's rebake-gate
	// fingerprint -- see gxObjNeedsRebake above).
	slot.valid = true;
	slot.oneDim = oneDim;
	slot.colorMode = colorMode;
	slot.tileNum = (u16)tileNum;
	slot.palNum = (u8)palNum;
	slot.texW = d.texW;
	slot.texH = d.texH;
	slot.bufW = (u16)bufW;
	slot.bufH = (u16)bufH;
}

// Bitmap-mode content is always opaque (no transparent texels -- every
// mode 3/4/5 pixel is a real color), so `fx` is applied unconditionally
// rather than gated by an idx!=0 check the way tiled modes are.
static void gxBakeBitmapMode(int mode, u16 dispcnt, const GxTexelEffectParams &fx = kTexelEffectNone, bool toEffectScratch = false)
{
	u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;

	if (toEffectScratch) {
		u16 *dst = s_bgBakeScratch;
		if (mode == 3) {
			for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
				dst[i] = gxTexel(T1ReadWord(MMU.GBA_VRAM, i * 2), true, fx);
		} else if (mode == 4) {
			for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i) {
				u8 idx = T1ReadByte(MMU.GBA_VRAM, page + i);
				dst[i] = gxTexel(gxBgPalColor(idx), true, fx);
			}
		} else { // mode 5: 160x128, rest of the buffer stays whatever it last held
			const int W = 160, H = 128;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					dst[y * GBA_SCREEN_W + x] = gxTexel(T1ReadWord(MMU.GBA_VRAM, page + (y * W + x) * 2), true, fx);
		}
		gxUploadEffectTex(s_bgBakeScratch, GBA_SCREEN_W, GBA_SCREEN_H, GX_CLAMP);
		return;
	}

	if (mode == 4) {
		// gx-next-steps-log.md task 6: mode 4 (8bpp palette-indexed bitmap
		// BG2, GBATEK) is a clean CI8 candidate -- direct idx 0-255 into
		// the whole BG palette bank, same as affine BG, no per-tile
		// palette indirection. Unlike tiled-mode BG/OBJ content, EVERY
		// mode-4 pixel is always opaque (gxTexel's old call above always
		// passed opaque=true, never gated by idx!=0) -- so, unlike every
		// other TLUT this file builds, entry 0 here is NOT forced
		// transparent; it keeps its real, opaque color. This is the
		// deliberate distinction called out in this task's log entry
		// ("bitmap mode 4... always opaque, unlike tile-indexed BG/OBJ").
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
			s_bmpBakeIdxScratch[i] = T1ReadByte(MMU.GBA_VRAM, page + i);
		GXBlockShape blk = gxBlockShape(GXTEXFMT_CI8);
		gxSwizzle8bpp(s_bmpBakeIdxScratch, (u8 *)s_bmpTexDataCI, blk.texelsWide, blk.texelsTall, GBA_SCREEN_W, GBA_SCREEN_H);
		DCFlushRange(s_bmpTexDataCI, GBA_SCREEN_W * GBA_SCREEN_H);

		u16 *tlut = (u16 *)s_bmpTlutData;
		for (int e = 0; e < 256; ++e)
			tlut[e] = gxOpaqueTexel(gxBgPalColor(e)); // no transparency -- see comment above
		DCFlushRange(s_bmpTlutData, 256 * sizeof(u16));
		GX_InitTlutObj(&s_bmpTlutObj, s_bmpTlutData, GX_TL_RGB5A3, 256);
		GX_LoadTlut(&s_bmpTlutObj, kBmpTlutSlot);
		GX_InitTexObjCI(&s_bmpTexObjCI, s_bmpTexDataCI, GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, kBmpTlutSlot);
		s_bmpValidCI = true;
		s_bmpValid = false; // stale/unused RGB5A3 buffer for this mode -- mode-exclusive, but keep the flags honest
		return;
	}

	// Modes 3/5: direct 16bpp truecolor, unchanged RGB5A3 path -- see
	// nds-wii-texture-format-mapping.md ("Bitmap-mode direct-color BG...
	// no palette involved") and gx_gba_render.h.
	u16 *dst = (u16 *)s_bmpTexData;
	if (mode == 3) {
		for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i)
			dst[i] = gxTexel(T1ReadWord(MMU.GBA_VRAM, i * 2), true, fx);
	} else { // mode 5: 160x128, rest of the buffer stays whatever it last held
		const int W = 160, H = 128;
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
				dst[y * GBA_SCREEN_W + x] = gxTexel(T1ReadWord(MMU.GBA_VRAM, page + (y * W + x) * 2), true, fx);
	}
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp((u16 *)s_bmpTexData, s_bgBakeScratch, blk.texelsWide, blk.texelsTall, GBA_SCREEN_W, GBA_SCREEN_H);
	memcpy(s_bmpTexData, s_bgBakeScratch, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	DCFlushRange(s_bmpTexData, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(u16));
	GX_InitTexObj(&s_bmpTexObj, s_bmpTexData, GBA_SCREEN_W, GBA_SCREEN_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	s_bmpValid = true;
	s_bmpValidCI = false;
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

// Single-sprite draw, shared by the windowed OBJ path (gxDrawObjLayerWindowed)
// with an explicit texture (the sprite's own persistent CI4/CI8 cache, or
// the shared s_effectTex RGB5A3 blend variant) and that texture's actual
// buffer size -- gx-next-steps-log.md task 6: these now differ (bufW/bufH
// passed in, not always texW/texH+4) since the persistent cache uses a
// wider CI4/CI8 block-alignment pad (+8) than the effect-scratch RGB5A3
// texture still does (+4) -- see GxObjTexSlot::bufW/bufH and
// gxBakeObjTexture's padding note.
static void gxDrawOneObj(const GxObjDraw &d, GXTexObj *tex, f32 bufW, f32 bufH)
{
	if (d.affine) {
		f32 tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
		gxAffineObjTexCorner(d, 0, 0, &tx0, &ty0);
		gxAffineObjTexCorner(d, d.boxW, 0, &tx1, &ty1);
		gxAffineObjTexCorner(d, d.boxW, d.boxH, &tx2, &ty2);
		gxAffineObjTexCorner(d, 0, d.boxH, &tx3, &ty3);
		gxDrawQuadFree(tex, d.x, d.y, d.x + d.boxW, d.y + d.boxH,
		               (tx0 + 1) / bufW, (ty0 + 1) / bufH,
		               (tx1 + 1) / bufW, (ty1 + 1) / bufH,
		               (tx2 + 1) / bufW, (ty2 + 1) / bufH,
		               (tx3 + 1) / bufW, (ty3 + 1) / bufH);
	} else {
		f32 u0 = 1.0f / bufW, u1 = (f32)(d.texW + 1) / bufW;
		f32 v0 = 1.0f / bufH, v1 = (f32)(d.texH + 1) / bufH;
		f32 s0 = d.hflip ? u1 : u0, s1 = d.hflip ? u0 : u1;
		f32 t0 = d.vflip ? v1 : v0, t1 = d.vflip ? v0 : v1;
		gxDrawQuad(tex, d.x, d.y, d.x + d.boxW, d.y + d.boxH, s0, t0, s1, t1);
	}
}

static void gxDrawBgQuad(int bg, const GxGbaBandRegs &r, int y0, int y1, GXTexObj *texOverride = nullptr)
{
	GxBgPlaneCache &pc = s_bgPlane[bg];
	if (!pc.valid) return;
	f32 s0 = (f32)r.hofs[bg] / pc.mapWpx;
	f32 s1 = s0 + (f32)GBA_SCREEN_W / pc.mapWpx;
	f32 t0 = (f32)(r.vofs[bg] + y0) / pc.mapHpx;
	f32 t1 = (f32)(r.vofs[bg] + y1) / pc.mapHpx;
	gxDrawQuad(texOverride ? texOverride : &pc.texObj, 0, (f32)y0, GBA_SCREEN_W, (f32)y1, s0, t0, s1, t1);
}

// which: 0=BG2, 1=BG3. Band's screen rect (0,y0)-(GBA_SCREEN_W,y1) maps
// through the affine transform (anchored at r.affX/affY, the accumulated
// reference point valid at scanline y0 -- see GxGbaBandRegs) to a
// parallelogram in texture space; computing that mapping at the 4 rect
// corners and letting GX interpolate reproduces sampleAffineBg()'s
// per-pixel formula (gba_ppu.cpp) exactly, since it's linear in (x, y-y0).
static void gxDrawAffineBgQuad(int which, const GxGbaBandRegs &r, int y0, int y1, GXTexObj *texOverride = nullptr)
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
	gxDrawQuadFree(texOverride ? texOverride : &pc.texObj, 0, (f32)y0, GBA_SCREEN_W, (f32)y1,
	               (tx0 + off) / norm, (ty0 + off) / norm,
	               (tx1 + off) / norm, (ty1 + off) / norm,
	               (tx2 + off) / norm, (ty2 + off) / norm,
	               (tx3 + off) / norm, (ty3 + off) / norm);
}

// ---------------------------------------------------------------------
// Blend classification (BLDCNT/BLDALPHA/BLDY): see gx_gba_render.h's
// "Blend" section. Layer bit indices match gba_ppu.cpp's Layer/BLDCNT
// convention: BG0=0, BG1=1, BG2=2, BG3=3, OBJ=4, backdrop=5 -- so a BG's
// own loop index doubles as its bit here.
// ---------------------------------------------------------------------
struct GxBandBlendPlan {
	int effect;            // BLDCNT bits 6-7: 0 none, 1 alpha, 2 brighten, 3 darken
	int target1, target2;  // BLDCNT bits 0-5 / 8-13
	int eva, evy;           // BLDALPHA EVA (evb is never needed -- see header), BLDY EVY, 16ths, clamped 0-16
	bool alphaNativeOk;     // precondition holds for effect==1 this band (see header); irrelevant for effect!=1
};

// `mode`/`dispcnt` are the frame-global values (DISPCNT's mode field and
// enable bits are treated as frame-static everywhere else in this file --
// see gxGbaRenderFrame's `mode`/`dispcnt` locals and the per-band BG loop
// that already reuses them instead of r.dispcnt's own mode bits); only the
// blend registers themselves and the per-BG/OBJ *enable* bits are read
// from the per-band snapshot `r`, consistent with how every other band
// register is treated.
static GxBandBlendPlan gxComputeBandBlendPlan(int mode, u16 dispcnt, const GxGbaBandRegs &r)
{
	GxBandBlendPlan p;
	p.effect = (r.bldcnt >> 6) & 3;
	p.target1 = r.bldcnt & 0x3F;
	p.target2 = (r.bldcnt >> 8) & 0x3F;
	int eva = r.bldalpha & 0x1F; if (eva > 16) eva = 16;
	int evb = (r.bldalpha >> 8) & 0x1F; if (evb > 16) evb = 16;
	p.eva = eva;
	p.evy = r.bldy & 0x1F; if (p.evy > 16) p.evy = 16;
	p.alphaNativeOk = true;
	if (p.effect != 1)
		return p; // brighten/darken/none never need the precondition

	bool anySemiTransparent = false;
	for (int i = 0; i < s_objDrawCount; ++i)
		if (s_objDraws[i].semiTransparent) { anySemiTransparent = true; break; }

	int activeMask = 0;
	for (int bg = 0; bg < 4; ++bg) {
		bool active;
		if (mode >= 3) {
			active = (bg == 2) && ((r.dispcnt >> 10) & 1);
		} else {
			active = (r.dispcnt >> (8 + bg)) & 1;
			if (mode == 1 && bg == 3) active = false;
			if (mode == 2 && (bg == 0 || bg == 1)) active = false;
		}
		if (active) activeMask |= (1 << bg);
	}
	if (((r.dispcnt >> 12) & 1) && s_objDrawCount > 0) activeMask |= (1 << 4);
	activeMask |= (1 << 5); // backdrop is always present as the bottom layer

	bool anyTarget1Active = (p.target1 & activeMask) != 0;
	if (!anyTarget1Active && !anySemiTransparent)
		return p; // nothing actually blends this band -- trivially fine

	// GBATEK: "I = MIN(31, I1st*EVA + I2nd*EVB)" -- GX_BL_SRCALPHA/
	// INVSRCALPHA's fixed dst factor (1-srcAlpha) only reproduces this
	// when EVB == 16-EVA (the standard translucency case); independent,
	// non-complementary coefficients are a real but rare, out-of-scope
	// GBA usage (see header).
	if (eva + evb != 16) { p.alphaNativeOk = false; return p; }

	// Conservative-but-safe precondition (see header's "Blend" section):
	// every layer that's active this band must be a BLDCNT 2nd-target,
	// since with a single painter's-algorithm GX pass there's no way to
	// make hardware blending conditional on which specific layer produced
	// the destination pixel a target-1 draw blends against.
	if ((activeMask & ~p.target2) != 0)
		p.alphaNativeOk = false;
	return p;
}

// Resolves layer `bit`'s bake-time texel effect from this band's blend
// plan. Returns GXTEXEFFECT_NONE for a layer that isn't BLDCNT 1st-target
// this band, or when effect==1 and the frame-level alpha-blend
// precondition failed (gxGbaRenderFrame() already bails the whole frame
// before this is ever reached in that case -- this branch is defensive,
// not a real path).
static GxTexelEffectParams gxResolveLayerEffect(int bit, const GxBandBlendPlan &bp)
{
	if (bp.effect == 0 || !((bp.target1 >> bit) & 1))
		return kTexelEffectNone;
	if (bp.effect == 1)
		return bp.alphaNativeOk ? GxTexelEffectParams{ GXTEXEFFECT_ALPHA, bp.eva, 0 } : kTexelEffectNone;
	return GxTexelEffectParams{ bp.effect == 2 ? GXTEXEFFECT_BRIGHTEN : GXTEXEFFECT_DARKEN, 0, bp.evy };
}

// Semi-transparent OBJ (GxObjDraw::semiTransparent) forces alpha-blend
// 1st-target behavior for that individual sprite regardless of BLDCNT's
// OBJ bit (GBATEK) -- but only when BLDCNT's effect field is actually 1
// (gba_ppu.cpp's composePixel checks `effect == 0` before ever looking at
// the forced-semi-transparent case, so effect 0/2/3 never force it).
static inline bool gxObjSpriteBlends(const GxObjDraw &d, const GxBandBlendPlan &bp)
{
	if (bp.effect != 1 || !bp.alphaNativeOk)
		return false;
	return ((bp.target1 >> 4) & 1) || d.semiTransparent;
}

// ---------------------------------------------------------------------
// Windows (WIN0/WIN1): draws one layer's slice of one band, decomposed
// into up to 3 scissor-rect passes in WIN0 > WIN1 > outside precedence --
// see gx_gba_render.h's "Windows" section for why this is exact for
// rectangular windows and gx_gba_render.h's "Blend" section for why each
// pass independently picks the plain or bake-time-effected texture based
// on that specific window region's effect-enable bit. `drawFull`/`drawBg`/
// `drawWin1`/`drawWin0` are avoided as a single functor parameter (no
// std::function in this GX-only, allocation-averse file) -- callers
// instead get a small dedicated wrapper per layer kind below.
// ---------------------------------------------------------------------
static void gxDrawBgLayerWindowed(int bg, const GxGbaBandRegs &r, int y0, int y1,
                                   const GxWindowPlan &wp, GXTexObj *effTex)
{
	if (!wp.active) {
		gxDrawBgQuad(bg, r, y0, y1, effTex);
		return;
	}
	if (wp.out.bg[bg]) {
		GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0);
		gxDrawBgQuad(bg, r, y0, y1, wp.out.effect ? effTex : nullptr);
	}
	if (wp.win1On && wp.win1.bg[bg] && wp.win1X1 > wp.win1X0) {
		GX_SetScissor(wp.win1X0, wp.win1Y0, wp.win1X1 - wp.win1X0, wp.win1Y1 - wp.win1Y0);
		gxDrawBgQuad(bg, r, y0, y1, wp.win1.effect ? effTex : nullptr);
	}
	if (wp.win0On && wp.win0.bg[bg] && wp.win0X1 > wp.win0X0) {
		GX_SetScissor(wp.win0X0, wp.win0Y0, wp.win0X1 - wp.win0X0, wp.win0Y1 - wp.win0Y0);
		gxDrawBgQuad(bg, r, y0, y1, wp.win0.effect ? effTex : nullptr);
	}
	GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0); // restore band scissor for later draws
}

static void gxDrawAffineBgLayerWindowed(int which, int bgBit, const GxGbaBandRegs &r, int y0, int y1,
                                         const GxWindowPlan &wp, GXTexObj *effTex)
{
	if (!wp.active) {
		gxDrawAffineBgQuad(which, r, y0, y1, effTex);
		return;
	}
	if (wp.out.bg[bgBit]) {
		GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0);
		gxDrawAffineBgQuad(which, r, y0, y1, wp.out.effect ? effTex : nullptr);
	}
	if (wp.win1On && wp.win1.bg[bgBit] && wp.win1X1 > wp.win1X0) {
		GX_SetScissor(wp.win1X0, wp.win1Y0, wp.win1X1 - wp.win1X0, wp.win1Y1 - wp.win1Y0);
		gxDrawAffineBgQuad(which, r, y0, y1, wp.win1.effect ? effTex : nullptr);
	}
	if (wp.win0On && wp.win0.bg[bgBit] && wp.win0X1 > wp.win0X0) {
		GX_SetScissor(wp.win0X0, wp.win0Y0, wp.win0X1 - wp.win0X0, wp.win0Y1 - wp.win0Y0);
		gxDrawAffineBgQuad(which, r, y0, y1, wp.win0.effect ? effTex : nullptr);
	}
	GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0);
}

// Bitmap-mode (3/4/5) BG2 layer -- same 3-pass precedence, but drawing the
// single full-screen bitmap quad (clipped to 160x128 for mode 5) instead
// of a tile-plane quad.
static void gxDrawBitmapLayerWindowed(int mode, const GxWindowPlan &wp, GXTexObj *baseTex, GXTexObj *effTex)
{
	int h = (mode == 5) ? 128 : GBA_SCREEN_H;
	int w = (mode == 5) ? 160 : GBA_SCREEN_W;
	f32 s1 = (f32)w / GBA_SCREEN_W, t1 = (f32)h / GBA_SCREEN_H;
	auto draw = [&](GXTexObj *tex) { gxDrawQuad(tex, 0, 0, (f32)w, (f32)h, 0, 0, s1, t1); };
	const int bgBit = 2; // bitmap-mode content is always "BG2" for BLDCNT/window purposes
	if (!wp.active) { draw(effTex ? effTex : baseTex); return; }
	if (wp.out.bg[bgBit]) {
		GX_SetScissor(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);
		draw(wp.out.effect && effTex ? effTex : baseTex);
	}
	if (wp.win1On && wp.win1.bg[bgBit] && wp.win1X1 > wp.win1X0) {
		GX_SetScissor(wp.win1X0, wp.win1Y0, wp.win1X1 - wp.win1X0, wp.win1Y1 - wp.win1Y0);
		draw(wp.win1.effect && effTex ? effTex : baseTex);
	}
	if (wp.win0On && wp.win0.bg[bgBit] && wp.win0X1 > wp.win0X0) {
		GX_SetScissor(wp.win0X0, wp.win0Y0, wp.win0X1 - wp.win0X0, wp.win0Y1 - wp.win0Y0);
		draw(wp.win0.effect && effTex ? effTex : baseTex);
	}
	GX_SetScissor(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);
}

// OBJ layer for one priority tier of one band. Unlike BG, the blend
// effect choice is per-sprite (gxObjSpriteBlends), not per-layer, since
// semi-transparent OBJ forces it for an individual sprite regardless of
// BLDCNT's OBJ bit; a sprite that needs the effected texture gets one
// freshly baked right before its draw (see gx_gba_render.h -- rare enough
// combination that this isn't cached).
static void gxDrawObjLayerWindowed(int prio, int y0, int y1, const GxWindowPlan &wp, const GxBandBlendPlan &bp, u16 dispcnt)
{
	for (int i = 0; i < s_objDrawCount; ++i) {
		const GxObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.boxH <= y0) continue;

		bool blends = gxObjSpriteBlends(d, bp);
		GxObjTexSlot &slot = s_objTex[d.oamIndex];
		auto drawWith = [&](bool useEffect) {
			if (useEffect && blends) {
				GxTexelEffectParams fx = { GXTEXEFFECT_ALPHA, bp.eva, 0 };
				gxBakeObjTexture(dispcnt, d, fx, true);
				gxDrawOneObj(d, &s_effectTex.texObj, (f32)(d.texW + 4), (f32)(d.texH + 4));
			} else {
				// gx-next-steps-log.md task 6: reload the shared OBJ TLUT
				// slot from this sprite's own tlutData immediately before
				// binding its texture -- every persistent-cache OBJ draw
				// does this (not just on a rebake), since another sprite's
				// draw in between may have overwritten kObjTlutSlot's TMEM
				// contents with a different sprite's palette. See
				// GxObjTexSlot's comment.
				GX_LoadTlut(&slot.tlutObj, kObjTlutSlot);
				gxDrawOneObj(d, &slot.texObj, (f32)slot.bufW, (f32)slot.bufH);
			}
		};

		if (!wp.active) { drawWith(true); continue; }
		if (wp.out.obj) {
			GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0);
			drawWith(wp.out.effect);
		}
		if (wp.win1On && wp.win1.obj && wp.win1X1 > wp.win1X0) {
			GX_SetScissor(wp.win1X0, wp.win1Y0, wp.win1X1 - wp.win1X0, wp.win1Y1 - wp.win1Y0);
			drawWith(wp.win1.effect);
		}
		if (wp.win0On && wp.win0.obj && wp.win0X1 > wp.win0X0) {
			GX_SetScissor(wp.win0X0, wp.win0Y0, wp.win0X1 - wp.win0X0, wp.win0Y1 - wp.win0Y0);
			drawWith(wp.win0.effect);
		}
	}
	if (wp.active)
		GX_SetScissor(0, y0, GBA_SCREEN_W, y1 - y0);
}

bool gxGbaRenderFrame()
{
	// See s_lastFrameNative's comment: reset unconditionally here, only
	// set true right before the real draw path's final `return true` below,
	// so every bail (including the forced-blank early return, which never
	// touches s_copyBackBuf) leaves it correctly false.
	s_lastFrameNative = false;

	if (!s_initDone)
		return false;

	// OBJ window and mosaic remain permanent bails -- narrower than task
	// 1's original blanket "any window/mosaic/blend" bail, but still real,
	// documented remaining gaps, not stubs to be removed later. See
	// gx_gba_render.h's header comment for exactly why each one still
	// bails. WIN0/WIN1 and BLDCNT/BLDALPHA/BLDY are now implemented
	// natively below (gxDraw*LayerWindowed / gxResolveLayerEffect), so
	// GXHOT_BLEND no longer forces an unconditional bail here -- the
	// per-band alpha-blend precondition check below decides that case by
	// case, per frame.
	if (g_gbaFramePlan.isHot(GXHOT_OBJWIN) || g_gbaFramePlan.isHot(GXHOT_MOSAIC))
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

	// Blend precondition (see gx_gba_render.h's "Blend" section /
	// gxComputeBandBlendPlan): computed for every band up front, before
	// any GX drawing happens, so an unsatisfiable band bails the *whole*
	// frame to the CPU compositor cleanly -- nothing has been copied back
	// into GBA_screen yet at this point (that only happens at the very
	// end of this function), so an early return here is always safe,
	// never a partially-drawn frame.
	GxBandBlendPlan blendPlan[GxBandTracker::kMaxBands];
	int blendBandCount = tiled ? bandCount : 1;
	for (int b = 0; b < blendBandCount; ++b) {
		const GxGbaBandRegs &r = tiled ? g_gbaBandRegs[b] : g_gbaBandRegs[0];
		blendPlan[b] = gxComputeBandBlendPlan(mode, dispcnt, r);
		if (blendPlan[b].effect == 1 && !blendPlan[b].alphaNativeOk)
			return false;
	}

	// `dirty`: still used below to gate bitmap-mode (DISPCNT mode 3/4/5)
	// baking -- out of this task's scope (gx-next-steps-log.md task 5 is
	// backdrop + BG-plane precision specifically; bitmap mode keeps the
	// coarse gate, documented in gx_gba_render.h). Backdrop and the four
	// tiled-mode BG planes below now use their own per-target fine-grained
	// gates instead of this aggregate.
	bool dirty = g_gbaFramePlan.vram.anyDirty() || g_gbaFramePlan.palette.anyDirty();

	gxSetup2DState();
	GX_SetViewport(0, 0, GBA_SCREEN_W, GBA_SCREEN_H, 0, 1);
	GX_SetScissor(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);
	GX_SetTexCopySrc(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);

	if (gxBackdropNeedsRebake())
		gxBakeBackdrop();
	gxDrawQuad(&s_backdropTexObj, 0, 0, GBA_SCREEN_W, GBA_SCREEN_H, 0, 0, 1, 1);

	// Task 4 (gx-next-steps-log.md): per-sprite re-bake gate, deliberately
	// NOT gated by the coarse `dirty` aggregate above -- a sprite is
	// (re)baked exactly when gxObjNeedsRebake() says its own dependencies
	// (OAM fields, or the VRAM/palette bytes they point at) actually
	// changed, independent of whether some unrelated VRAM/palette byte
	// changed elsewhere this frame. This also correctly picks up an
	// OAM-only change (e.g. a reassigned tile index with no VRAM/palette
	// write at all this frame) that the old `dirty`-gated loop would have
	// missed entirely.
	for (int i = 0; i < s_objDrawCount; ++i) {
		const GxObjDraw &d = s_objDraws[i];
		if (gxObjNeedsRebake(dispcnt, d))
			gxBakeObjTexture(dispcnt, d);
	}

	if (tiled) {
		// gx-next-steps-log.md task 5: per-BG-plane re-bake gate, replacing
		// the old `if (dirty) { bake all 4 enabled planes }` block -- each
		// plane is now (re)baked exactly when its own gx{,Affine}BgPlane
		// NeedsRebake() says its tile/map layout changed or its actual
		// dependency bytes (VRAM char/map range, BG palette bank) were
		// written, independent of unrelated VRAM/palette writes elsewhere
		// (e.g. another BG's tiles, or OBJ VRAM/palette) this frame. See
		// the comment block above gxBackdropNeedsRebake() for the exact
		// per-plane dependency-range design.
		for (int bg = 0; bg < 4; ++bg) {
			if (!((dispcnt >> (8 + bg)) & 1)) continue;
			if (mode == 1 && bg == 3) continue; // mode 1 has no BG3
			if (mode == 2 && (bg == 0 || bg == 1)) continue; // mode 2 has no BG0/BG1
			bool affineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);
			u16 cnt = T1ReadWord(MMU.GBA_IOREG, IO_BG0CNT + bg * 2);
			if (affineCapable) {
				if (gxAffineBgPlaneNeedsRebake(bg - 2, cnt))
					gxBakeAffineBgPlane(bg - 2, cnt);
			} else {
				GxBgLayout li = gxBgLayoutFromCnt(cnt);
				if (gxBgPlaneNeedsRebake(bg, li))
					gxBakeBgPlane(bg);
			}
		}
		for (int b = 0; b < bandCount; ++b) {
			const GxGbaBandRegs &r = g_gbaBandRegs[b];
			const GxBandBlendPlan &bp = blendPlan[b];
			GxWindowPlan wp = gxBuildWindowPlan(r, outStarts[b], outEnds[b]);
			GX_SetScissor(0, outStarts[b], GBA_SCREEN_W, outEnds[b] - outStarts[b]);
			for (int prio = 3; prio >= 0; --prio) {
				for (int bg = 3; bg >= 0; --bg) {
					if (!((r.dispcnt >> (8 + bg)) & 1)) continue;
					if (mode == 1 && bg == 3) continue;
					if (mode == 2 && (bg == 0 || bg == 1)) continue;
					if ((r.bgcnt[bg] & 3) != prio) continue;
					bool affineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);

					GxTexelEffectParams fx = gxResolveLayerEffect(bg, bp);
					GXTexObj *effTex = nullptr;
					if (fx.mode != GXTEXEFFECT_NONE) {
						if (affineCapable) gxBakeAffineBgPlane(bg - 2, r.bgcnt[bg], fx, true);
						else gxBakeBgPlane(bg, fx, true);
						effTex = &s_effectTex.texObj;
					}
					if (affineCapable)
						gxDrawAffineBgLayerWindowed(bg - 2, bg, r, outStarts[b], outEnds[b], wp, effTex);
					else
						gxDrawBgLayerWindowed(bg, r, outStarts[b], outEnds[b], wp, effTex);
				}
				if ((r.dispcnt >> 12) & 1)
					gxDrawObjLayerWindowed(prio, outStarts[b], outEnds[b], wp, bp, dispcnt);
			}
		}
	} else {
		if (dirty)
			gxBakeBitmapMode(mode, dispcnt);

		const GxGbaBandRegs &r = g_gbaBandRegs[0];
		const GxBandBlendPlan &bp = blendPlan[0];
		GxWindowPlan wp = gxBuildWindowPlan(r, 0, GBA_SCREEN_H);
		GX_SetScissor(0, 0, GBA_SCREEN_W, GBA_SCREEN_H);

		if (s_bmpValid || s_bmpValidCI) {
			GxTexelEffectParams fx = gxResolveLayerEffect(2, bp); // bitmap BG2 == layer bit 2
			GXTexObj *effTex = nullptr;
			if (fx.mode != GXTEXEFFECT_NONE) {
				gxBakeBitmapMode(mode, dispcnt, fx, true);
				effTex = &s_effectTex.texObj;
			}
			// gx-next-steps-log.md task 6: mode 4's CI8 texture object
			// (s_bmpTexObjCI) already has its TLUT permanently dedicated
			// (kBmpTlutSlot, reloaded only when gxBakeBitmapMode() actually
			// rebakes it) -- no per-draw GX_LoadTlut needed here, unlike OBJ.
			GXTexObj *baseTex = (mode == 4 && s_bmpValidCI) ? &s_bmpTexObjCI : &s_bmpTexObj;
			gxDrawBitmapLayerWindowed(mode, wp, baseTex, effTex);
		}
		if ((dispcnt >> 12) & 1)
			for (int prio = 3; prio >= 0; --prio)
				gxDrawObjLayerWindowed(prio, 0, GBA_SCREEN_H, wp, bp, dispcnt);
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

	// s_copyBackBuf (still RGB5A3-swizzled, pre-unswizzle) stays valid for
	// gxGbaBlitNativeTop() until the next gxGbaRenderFrame() call overwrites
	// it next frame -- main.cpp's Draw() (same thread, called immediately
	// after gbaPpuEndFrame() within DSExec()) consumes it before that
	// happens, so there's no lifetime hazard here.
	s_lastFrameNative = true;

	return true;
}

bool gxGbaBlitNativeTop(void *dst256x192Rgb5a3)
{
	if (!s_lastFrameNative || !s_copyBackBuf)
		return false;

	// Block-row memcpy, not a per-pixel loop: both GBA_SCREEN_W (240) and
	// GBA_SCREEN_H (160) are multiples of the RGB5A3 block shape (4x4), so
	// each 4x4 texel block is self-contained in both the 240-wide source
	// and 256-wide destination block-tiled layouts (gx_swizzle.cpp's
	// gxBlockAddress: blockIndex = blockY*(width/blockW) + blockX, blocks
	// stored contiguously within a block-row) -- copying whole block-rows
	// reproduces the swizzled data exactly with no unswizzle/reswizzle
	// needed. Destination block-columns 60-63 (texels 240-255) and
	// block-rows 40-47 (texels 160-191) are never written here, by design
	// -- see this function's header comment.
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	const int bytesPerBlock = blk.texelsWide * blk.texelsTall * (int)sizeof(u16);
	const int blocksPerRowSrc = GBA_SCREEN_W / blk.texelsWide;
	const int blocksPerRowDst = 256 / blk.texelsWide;
	const int blockRows = GBA_SCREEN_H / blk.texelsTall;
	const u8 *src = (const u8 *)s_copyBackBuf;
	u8 *dst = (u8 *)dst256x192Rgb5a3;
	const size_t rowBytes = (size_t)blocksPerRowSrc * bytesPerBlock;
	for (int by = 0; by < blockRows; ++by) {
		memcpy(dst + (size_t)by * blocksPerRowDst * bytesPerBlock,
		       src + (size_t)by * rowBytes,
		       rowBytes);
	}
	return true;
}
