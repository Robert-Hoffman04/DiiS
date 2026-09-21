#include "gx_ds_engineb_render.h"
#include "gx_color.h"
#include "gx_swizzle.h"
#include "gx_texformat.h"
#include "../GPU.h"
#include "../MMU.h"
#include "../NDSSystem.h"
#include <gccore.h>
#include <malloc.h>
#include <string.h>
#include <stddef.h>
#if defined(DSB_STATS) && defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
#include "../harness/harness_profile.h"
#endif

// Test hooks (compile-time only, zero cost when undefined):
//  -DDSB_FORCE_CPU : gxDsEngineBRenderFrame() always bails, so the CPU
//                    compositor renders every frame. The A/B reference for
//                    byte-comparing the GX path against GPU.cpp.
//  -DDSB_STATS     : (needs the harness profile sink) counts engaged vs.
//                    bailed frames, the last bail reason and the BG-type mix
//                    of engaged frames, and emits one line per 30 frames.
#ifdef DSB_STATS
static const char *s_dsbWhy = "";
static u32 s_dsbOk = 0, s_dsbBail = 0, s_dsbAffOk = 0, s_dsbAffObjOk = 0, s_dsbDispcnt = 0, s_dsbTypes = 0;
// task 9: engaged-frame counts per feature (window / fade / alpha / semi-transparent OBJ / master brightness)
static u32 s_dsbWinOk = 0, s_dsbFadeOk = 0, s_dsbAlphaOk = 0, s_dsbSemiOk = 0, s_dsbMbOk = 0;
static bool s_dsbFrWin, s_dsbFrFade, s_dsbFrAlpha, s_dsbFrSemi, s_dsbFrMb;
#define DSB_WHY(s_) do { s_dsbWhy = (s_); } while (0)
#else
#define DSB_WHY(s_) do { } while (0)
#endif

// main.cpp: re-applies the one-time GX present state InitVideo() sets up
// (viewport / scissor / projection / Z-mode / EFB-copy src+dst / vertex
// format), which this file's own gxDsBSetup2DState() necessarily clobbers.
// See gx_ds_engineb_render.h's "GX present state" deviation note.
extern void GxRestorePresentState(void);
// main.cpp: the mutex already serializing Draw() (emulation thread) against
// draw_thread (video thread). Held across this file's whole GX sequence so
// draw_thread cannot interleave its present quads into the same FIFO while
// Engine B's frame is being composited.
extern mutex_t vidmutex;

GxFramePlan g_dsBFramePlan;
GxDsBBandRegs g_dsBBandRegs[GX_DSB_MAX_BAND_REGS];

// ---------------------------------------------------------------------
// Engine B fixed addressing constants, all read back out of the CPU
// reference rather than hardcoded from GBATEK:
//  - BG palette   : MMU.ARM9_VMEM + core*ADDRESS_STEP_1KB  (GPU.cpp's
//                   renderline_textBG / GPU_RenderLine_layer backdrop read)
//  - OBJ palette  : MMU.ARM9_VMEM + 0x200 + core*0x400     (GPU.cpp's
//                   _spriteRender 16/256-colour branches)
//  - OAM          : MMU.ARM9_OAM + ADDRESS_STEP_1KB        (GPU_Reset)
// with core == GPU_SUB == 1.
// ---------------------------------------------------------------------
static const u32 kDsBBgPalOff  = 0x400;
static const u32 kDsBObjPalOff = 0x600;
static const u32 kDsBOamOff    = 0x400;

static const int kDsBScreenW = 256;
static const int kDsBScreenH = 192;

// ---------------------------------------------------------------------
// Colour helpers. Local duplicates of gx_gba_render.cpp's, deliberately not
// shared through a header for the same reason that file gives: the CPU
// reference (GPU.cpp here) stays the independently-verified implementation
// this file's output is matched against, not a library it builds on.
// ---------------------------------------------------------------------
// Task 9: DS brighten/darken tables, per 5-bit channel, built with the SAME float
// expression GPU.cpp's GPU_InitFadeColors() uses (`c + (31-c)*i/16` for
// brighten, `c - c*i/16` for darken, truncated by the bitfield store), so a
// baked fade texel is exactly the CPU's fadeInColors/fadeOutColors entry.
// GPU.cpp's tables are per whole 15-bit colour but every channel is independent.
static u8 s_fadeUp[17][32], s_fadeDown[17][32];
static bool s_fadeTablesInit = false;
static void gxDsBInitFadeTables()
{
	if (s_fadeTablesInit) return;
	for (int i = 0; i <= 16; i++) {
		float idiv16 = ((float)i) / 16;
		for (int c = 0; c < 32; c++) {
			s_fadeUp[i][c]   = (u8)(c + ((31 - c) * idiv16));
			s_fadeDown[i][c] = (u8)(c - (c * idiv16));
		}
	}
	s_fadeTablesInit = true;
}
static inline u16 gxDsBFadeColor(u16 c, int mode /*2 up, 3 down*/, int lvl)
{
	const u8 *t = (mode == 2) ? s_fadeUp[lvl] : s_fadeDown[lvl];
	return (u16)(t[c & 31] | (t[(c >> 5) & 31] << 5) | (t[(c >> 10) & 31] << 10));
}
// The fade (if any) the bake currently in progress applies to every opaque texel
// (BLDCNT brighten/darken as a bake-time transform: exact, since the CPU applies
// the fade to the layer's own colour before it is written).
static int s_bakeFadeMode = 0, s_bakeFadeLvl = 0;
static inline u16 gxDsBOpaqueTexel(u16 ndsColor)
{
	if (s_bakeFadeMode)
		ndsColor = gxDsBFadeColor(ndsColor & 0x7FFF, s_bakeFadeMode, s_bakeFadeLvl);
	return gxPackRGB5A3Opaque(gxExtractBGR555(ndsColor));
}
static const u16 kDsBTransparentTexel = 0; // RGB5A3 alpha sub-format, alpha 0

// RGB5A3 -> DS-native X-B5-G5-R5 (GPU_screen's layout, GPU.h). Exact
// inverse of gxPackRGB5A3Opaque for the opaque sub-format; the alpha
// sub-format branch is defensive only (an opaque backdrop quad is always
// drawn first, so no final EFB pixel is ever transparent).
static inline u16 gxDsBRgb5a3ToNds(u16 v)
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

// ---------------------------------------------------------------------
// Stage 0 state
// ---------------------------------------------------------------------
// True as soon as any visible scanline this frame sees Engine-B state this
// file doesn't implement (see the header's scope list). Latched, never
// cleared mid-frame -- a frame that was ever out of scope stays out of
// scope, since Stage 4 replays the whole frame in one pass.
static bool s_frameOutOfScope = true;   // no scanline seen yet == don't render
static bool s_frameSawLine0 = false;
static GxDsBBandRegs s_lastBandRegs;
// DISPCNT.ExBGxPalette_Enable as seen on this frame's line 0 (see
// gxDsBScanlineOutOfScope); reset at every line 0.
static u8 s_frameExtBgPal = 0;
static bool s_frameExtBgPalKnown = false;
// Task 9: per-scanline MASTER_BRIGHT mode (0 none, 1 up, 2 down) and factor (0..16).
static u8 s_mbMode[192], s_mbFac[192];

// vram_arm9_map (MMU.h) page ranges that back Engine B's BG and OBJ
// windows. VRAM_ARM9_PAGES is 512 16KB pages covering 0x06000000-0x067FFFFF,
// split by MMU.h's VRAM_PAGE_* constants into ABG[0,128) BBG[128,256)
// AOBJ[256,384) BOBJ[384,512). A VRAMCNT rewrite can repoint those pages
// with no write ever landing in VRAM, so the map itself is part of the
// rebake dependency set.
static u8 s_bankMapCopy[256];
static bool s_bankMapCopyValid = false;

static bool gxDsBBankMapChanged()
{
	u8 cur[256];
	memcpy(cur, vram_arm9_map + VRAM_PAGE_BBG, 128);
	memcpy(cur + 128, vram_arm9_map + VRAM_PAGE_BOBJ, 128);
	// Task 8: Engine B's four extended-BG-palette slot pointers
	// (MMU_VRAMmapControl repoints them on a VRAMCNT_H write).
	static u8 *s_extPalCopy[4];
	bool extChanged = false;
	for (int i = 0; i < 4; ++i)
		if (s_extPalCopy[i] != MMU.ExtPal[1][i]) { extChanged = true; s_extPalCopy[i] = MMU.ExtPal[1][i]; }
	if (s_bankMapCopyValid && !extChanged && memcmp(cur, s_bankMapCopy, sizeof(cur)) == 0)
		return false;
	memcpy(s_bankMapCopy, cur, sizeof(cur));
	s_bankMapCopyValid = true;
	return true;
}

void gxDsEngineBMarkWrite(u32 adr, u32 size)
{
	switch ((adr >> 24) & 0xF) {
	case 5: // palette RAM, 2KB (Engine A 0x000-0x3FF, Engine B 0x400-0x7FF)
		g_dsBFramePlan.palette.markRange(adr & 0x7FF, size);
		break;
	case 6: {
		// Coarse by design (see the header's dirty-gating deviation note):
		// one whole-region flag, but only armed by writes that can actually
		// reach Engine B's own BG/OBJ windows. vram_arm9_map's page index is
		// exactly how MMU_gpu_map() resolves an access, so using the same
		// index here means "could this write be visible to Engine B" is
		// answered with the same arithmetic the reader uses.
		u32 page = (adr >> 14) & (VRAM_ARM9_PAGES - 1);
		if ((page >= VRAM_PAGE_BBG && page < VRAM_PAGE_BBG + 128) ||
		    (page >= VRAM_PAGE_BOBJ && page < VRAM_PAGE_BOBJ + 128))
			g_dsBFramePlan.vram.markRange(0, 1);
		// Engine B's extended BG palette lives in VRAM bank H, which a game
		// fills through H's LCDC window (0x06898000-0x0689FFFF) while H is
		// in LCDC mode and only then remaps to "BG ext palette" -- so the
		// write carries a BBG/BOBJ-free address and the page test above
		// can't see it. (A later remap that repoints MMU.ExtPal[1][] is
		// caught separately by gxDsBBankMapChanged.)
		else if ((adr & 0x00FFFFFF) >= 0x898000 && (adr & 0x00FFFFFF) < 0x8A0000)
			g_dsBFramePlan.vram.markRange(0, 1);
		break;
	}
	case 7: // OAM, 2KB (Engine A 0x000-0x3FF, Engine B 0x400-0x7FF)
		g_dsBFramePlan.oam.markRange(adr & 0x7FF, size);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------
// Caches (Stage 1). Every texture here is plain RGB5A3 with the palette
// already resolved -- this phase deliberately does NOT use native
// CI4/CI8+TLUT (see the header). Buffers are grown on demand rather than
// allocated at their theoretical maximum, since a DS sub screen very
// commonly uses only 256x256 text BGs and a handful of small sprites.
// ---------------------------------------------------------------------
struct GxDsBBgPlaneCache {
	GXTexObj texObj;
	void *texData;
	u32 texDataCap;
	u16 wpx, hpx;
	bool valid;
	// Last-baked configuration. The coarse VRAM/palette dirty flags below
	// can't see a pure BGxCNT rewrite (new char/screen base, new colour
	// depth, new size selector) that happens to touch no VRAM or palette
	// byte, so the config this bake was built from is compared explicitly,
	// the same way gx_gba_render.cpp's per-plane gate does.
	u32 cfgTileBase, cfgMapBase;
	u8 cfgColorMode, cfgScreenSize;
	// Task 8 (affine / extended-affine / large-8bpp planes):
	u16 bufW, bufH;    // baked texture size: wpx x hpx for text and wrapping
	                   // affine planes; (wpx+4) x (hpx+4) when a 1-texel
	                   // transparent border (+ pad to a 4-texel block) is baked
	u8 cfgType;        // BGType baked (text and each affine flavour differ)
	u8 cfgWrap;        // BGxCNT overflow-wrap bit at bake time (affine only)
	u32 cfgAuxBase;    // BG_bmp_ram / BG_bmp_large_ram at bake time (bitmap-ish types)
	const u8 *cfgExtPal; // extended-palette slot pointer baked in, NULL if the regular palette was used
	u16 cfgFx;         // task 9: (fadeMode << 8) | level baked in; 0 for the plain variant
};
static GxDsBBgPlaneCache s_bgPlane[4];
static GxDsBBgPlaneCache s_bgPlaneFx[4]; // task 9: brighten/darken variant of each plane (lazily baked)
static const int kDsBBgPlaneMaxPx = 512; // text BG max (BGSize table, GPU.cpp sizeTab row 1)
static u16 *s_bgBakeScratch;
static u32 s_bgBakeScratchCap; // in u16 elements
// Task 8: an affine/bitmap plane can be up to 1024x1024 (+ transparent
// border/pad), far past the 512x512 text-BG maximum this scratch used to be
// sized for, and most sub screens never need it -- so it grows on demand.
static bool gxDsBEnsureBgScratch(u32 elems)
{
	if (elems <= s_bgBakeScratchCap)
		return s_bgBakeScratch != NULL;
	u16 *n = (u16 *)malloc((size_t)elems * sizeof(u16));
	if (!n) return false;
	free(s_bgBakeScratch);
	s_bgBakeScratch = n;
	s_bgBakeScratchCap = elems;
	return true;
}

struct GxDsBObjTexSlot {
	GXTexObj texObj;
	void *texData;
	u32 texDataCap;
	u16 w, h;
	bool valid;
	// Same reasoning as GxDsBBgPlaneCache's config fingerprint: an OAM
	// rewrite that repoints this slot at a different tile/palette (a
	// different sprite reusing the index) must force a re-bake even if the
	// resulting box size happens to be identical.
	u16 cfgTileIndex;
	u8 cfgPalIndex, cfgDepth, cfgOneDim, cfgBoundary;
	// Task 8: an affine sprite is baked with a 1-texel transparent border
	// (padded to the 4x4 RGB5A3 block) and a plain one without, so which of the
	// two this slot currently holds is part of what a re-bake depends on.
	u16 bufW, bufH;
	u8 cfgBordered;
	u16 cfgFx;         // task 9: (fadeMode << 8) | level baked in; 0 for the plain variant
};
static GxDsBObjTexSlot s_objTex[128];
static GxDsBObjTexSlot s_objTexFx[128]; // task 9: brighten/darken variant of each sprite (lazily baked)
static const int kDsBObjMaxPx = 64;
static u16 *s_objBakeScratch;

// Task 9: one 1-texel backdrop per band (the colour can differ per band under
// BLDY fades, and GX draws read texture memory asynchronously, so the texel of an
// earlier band must not be rewritten while its draw is still queued).
static GXTexObj s_backdropTexObj[GX_DSB_MAX_BAND_REGS];
static void *s_backdropTexData;

static void *s_copyBackBuf;
static u16 *s_copyBackLinear;

static bool s_initDone = false;

bool gxDsEngineBRenderInit()
{
	if (s_initDone)
		return true;

	// Stage 0 dirty-bitmap sizing. Palette and OAM are each a flat 2KB on
	// the DS (MMU.h: ARM9_VMEM[0x800], ARM9_OAM[0x800]); 32-byte pages give
	// 64 pages each, i.e. 16-colour-sub-palette / 4-OAM-entry granularity --
	// the same proportional choice gbaPpuReset() makes for the GBA's 1KB
	// regions. VRAM is deliberately a single page: this phase's whole-region
	// coarse gate (see the header), sized so markRange(0,1)/anyDirty() is
	// the whole story and no caller is tempted to read a bogus page index.
	g_dsBFramePlan.palette.init(0x800, 32);
	g_dsBFramePlan.oam.init(0x800, 32);
	g_dsBFramePlan.vram.init(1, 1);
	g_dsBFramePlan.beginFrame();

	if (!gxDsBEnsureBgScratch((u32)kDsBBgPlaneMaxPx * kDsBBgPlaneMaxPx)) return false;
	// An affine OBJ bakes with a 1-texel transparent border padded up to a power
	// of two (see gxDsBBakeObjTexture), so a 64-texel sprite needs 128x128.
	s_objBakeScratch = (u16 *)malloc((u32)(2 * kDsBObjMaxPx) * (2 * kDsBObjMaxPx) * sizeof(u16));
	if (!s_objBakeScratch) return false;

	for (int i = 0; i < 4; ++i) {
		s_bgPlane[i].texData = NULL;
		s_bgPlane[i].texDataCap = 0;
		s_bgPlane[i].valid = false;
		s_bgPlaneFx[i].texData = NULL;
		s_bgPlaneFx[i].texDataCap = 0;
		s_bgPlaneFx[i].valid = false;
	}
	for (int i = 0; i < 128; ++i) {
		s_objTex[i].texData = NULL;
		s_objTex[i].texDataCap = 0;
		s_objTex[i].valid = false;
		s_objTexFx[i].texData = NULL;
		s_objTexFx[i].texDataCap = 0;
		s_objTexFx[i].valid = false;
	}

	s_backdropTexData = memalign(32, 32 * GX_DSB_MAX_BAND_REGS); // 32B = GX minimum texture allocation granularity
	if (!s_backdropTexData) return false;
	memset(s_backdropTexData, 0, 32 * GX_DSB_MAX_BAND_REGS);
	for (int b = 0; b < GX_DSB_MAX_BAND_REGS; ++b) {
		GX_InitTexObj(&s_backdropTexObj[b], (u8 *)s_backdropTexData + 32 * b, 1, 1, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&s_backdropTexObj[b], GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
	}
	gxDsBInitFadeTables();

	u32 copySz = GX_GetTexBufferSize(kDsBScreenW, kDsBScreenH, GX_TF_RGB5A3, GX_FALSE, 0);
	s_copyBackBuf = memalign(32, copySz);
	if (!s_copyBackBuf) return false;
	s_copyBackLinear = (u16 *)malloc((u32)kDsBScreenW * kDsBScreenH * sizeof(u16));
	if (!s_copyBackLinear) return false;

	s_initDone = true;
	return true;
}

void gxDsEngineBRenderShutdown()
{
	if (!s_initDone)
		return;
	for (int i = 0; i < 4; ++i) {
		free(s_bgPlane[i].texData); s_bgPlane[i].texData = NULL; s_bgPlane[i].texDataCap = 0; s_bgPlane[i].valid = false;
		free(s_bgPlaneFx[i].texData); s_bgPlaneFx[i].texData = NULL; s_bgPlaneFx[i].texDataCap = 0; s_bgPlaneFx[i].valid = false;
	}
	for (int i = 0; i < 128; ++i) {
		free(s_objTex[i].texData); s_objTex[i].texData = NULL; s_objTex[i].texDataCap = 0; s_objTex[i].valid = false;
		free(s_objTexFx[i].texData); s_objTexFx[i].texData = NULL; s_objTexFx[i].texDataCap = 0; s_objTexFx[i].valid = false;
	}
	free(s_bgBakeScratch); s_bgBakeScratch = NULL; s_bgBakeScratchCap = 0;
	free(s_objBakeScratch); s_objBakeScratch = NULL;
	free(s_backdropTexData); s_backdropTexData = NULL;
	free(s_copyBackBuf); s_copyBackBuf = NULL;
	free(s_copyBackLinear); s_copyBackLinear = NULL;
	s_bankMapCopyValid = false;
	s_initDone = false;
}

// ---------------------------------------------------------------------
// Stage 0: per-scanline layout snapshot + scope check
// ---------------------------------------------------------------------
// Per-BG affine running-state prediction (see GxDsBBandRegs's affine block).
// The CPU reference (GPU.cpp lineRot/lineExtRot) advances BGxX/BGxY by PB/PD
// after every line it renders the BG on, and re-latches them from
// affineInfo[] at the start of line 0 / on any reference-point register
// write. A frame whose affine BGs simply follow that recurrence is one band;
// a mid-frame reference-point or matrix rewrite (the classic HDMA
// wave/mode-7 effect) breaks the prediction and starts a new band.
struct GxDsBAffPred {
	bool valid;
	s32 x, y;
	s16 pa, pb, pc, pd;
};
static GxDsBAffPred s_affPred[2];

static inline bool gxDsBIsAffineType(u8 t)
{
	return t == BGType_Affine || t == BGType_AffineExt_256x16 || t == BGType_AffineExt_256x1 ||
	       t == BGType_AffineExt_Direct || t == BGType_Large8bpp;
}

// Fills `r` with the layout state this scanline is about to be drawn with.
// `affChanged` is set when an enabled affine BG's running state differs from
// what the previous line predicted (it is never set on line 0: line 0 is the
// band-0 baseline by definition).
static void gxDsBSnapshotBandRegs(GPU *gpu, GxDsBBandRegs *r, int line, bool *affChanged)
{
	memset(r, 0, sizeof(*r));
	*affChanged = false;
	const _DISPCNT &d = gpu->dispx_st->dispx_DISPCNT.bits;
	r->bgEnable = (u8)((d.BG0_Enable) | (d.BG1_Enable << 1) | (d.BG2_Enable << 2) | (d.BG3_Enable << 3));
	r->objEnable = (u8)d.OBJ_Enable;
	for (int i = 0; i < 4; ++i) {
		r->bgPrio[i] = (u8)gpu->dispx_st->dispx_BGxCNT[i].bits.Priority;
		r->bgType[i] = (u8)gpu->BGTypes[i];
		if (gpu->BGTypes[i] == BGType_Text) {
			r->hofs[i] = (u16)gpu->getHOFS(i);
			r->vofs[i] = (u16)gpu->getVOFS(i);
		}
	}
	// Task 9: window / colour-effect state (see GxDsBBandRegs).
	r->bldcnt = (u16)(gpu->BLDCNT & 0x3FFF);
	r->eva = gpu->BLDALPHA_EVA;
	r->evb = gpu->BLDALPHA_EVB;
	{
		const int fx = (r->bldcnt >> 6) & 3;
		r->evy = (fx == 2 || fx == 3) ? gpu->BLDY_EVY : 0;
		if (gpu->WIN0_ENABLED) {
			r->winEn |= 1;
			r->win0h0 = gpu->WIN0H0; r->win0h1 = gpu->WIN0H1; r->win0v0 = gpu->WIN0V0; r->win0v1 = gpu->WIN0V1;
		}
		if (gpu->WIN1_ENABLED) {
			r->winEn |= 2;
			r->win1h0 = gpu->WIN1H0; r->win1h1 = gpu->WIN1H1; r->win1v0 = gpu->WIN1V0; r->win1v1 = gpu->WIN1V1;
		}
		if (r->winEn) {
			r->winIn0 = gpu->WININ0 & 0x1F; r->winIn1 = gpu->WININ1 & 0x1F; r->winOut = gpu->WINOUT & 0x1F;
			r->winSp = (u8)((gpu->WININ0_SPECIAL ? 1 : 0) | (gpu->WININ1_SPECIAL ? 2 : 0) | (gpu->WINOUT_SPECIAL ? 4 : 0));
		}
	}
	for (int w = 0; w < 2; ++w) {
		const int bg = w + 2;
		GxDsBAffPred &pr = s_affPred[w];
		if (!((r->bgEnable >> bg) & 1) || !gxDsBIsAffineType(r->bgType[bg])) {
			pr.valid = false;
			continue;
		}
		const BGxPARMS *p = (bg == 2) ? &gpu->dispx_st->dispx_BG2PARMS : &gpu->dispx_st->dispx_BG3PARMS;
		// Line 0: the CPU re-latches BGxX/BGxY from affineInfo[] inside
		// GPU_RenderLine(l==0), which runs AFTER this hook, so the live
		// parms->BGxX still holds last frame's accumulated value here.
		s32 x = (line == 0) ? (s32)gpu->affineInfo[w].x : p->BGxX;
		s32 y = (line == 0) ? (s32)gpu->affineInfo[w].y : p->BGxY;
		// GPU.cpp reads the matrix through LE_TO_LOCAL_16 (host-endian fixup);
		// BGxX/BGxY are plain host s32 and are read unswapped, as it does.
		s16 pa = (s16)LE_TO_LOCAL_16(p->BGxPA), pb = (s16)LE_TO_LOCAL_16(p->BGxPB);
		s16 pc = (s16)LE_TO_LOCAL_16(p->BGxPC), pd = (s16)LE_TO_LOCAL_16(p->BGxPD);
		r->affX[w] = x; r->affY[w] = y;
		r->affPA[w] = pa; r->affPB[w] = pb; r->affPC[w] = pc; r->affPD[w] = pd;
		if (line != 0 && (!pr.valid || pr.x != x || pr.y != y || pr.pa != pa || pr.pb != pb || pr.pc != pc || pr.pd != pd))
			*affChanged = true;
		pr.valid = true;
		pr.x = (s32)((u32)x + (u32)(s32)pb);
		pr.y = (s32)((u32)y + (u32)(s32)pd);
		pr.pa = pa; pr.pb = pb; pr.pc = pc; pr.pd = pd;
	}
}

// Everything this file can't reproduce natively. Checked per scanline (not
// once per frame) precisely because a game can turn any of these on
// mid-frame -- a frame-end-only check would silently render the first half
// of such a frame wrong.
static bool gxDsBScanlineOutOfScope(GPU *gpu)
{
	const _DISPCNT &d = gpu->dispx_st->dispx_DISPCNT.bits;
#ifdef DSB_STATS
	s_dsbDispcnt = gpu->dispx_st->dispx_DISPCNT.val;
	s_dsbTypes = (u32)gpu->BGTypes[0] | ((u32)gpu->BGTypes[1] << 4) | ((u32)gpu->BGTypes[2] << 8) | ((u32)gpu->BGTypes[3] << 12);
#endif

	if (gpu->dispMode != 1) { DSB_WHY("dispmode"); return true; }   // 0 = white; 2/3 are Engine-A-only
	if (d.ForceBlank) { DSB_WHY("forceblank"); return true; }
	// Extended OBJ palettes stay out of scope. Extended BG palettes are IN scope
	// (task 8 -- SM64DS's in-game sub screen needs them; see gxDsBExtPalFor()),
	// but the flag is baked once from the frame-final state, so a mid-frame
	// toggle can't be replayed.
	if (d.ExOBJPalette_Enable) { DSB_WHY("objextpal"); return true; }
	if (s_frameExtBgPalKnown && d.ExBGxPalette_Enable != s_frameExtBgPal) { DSB_WHY("extpalchange"); return true; }
	s_frameExtBgPal = d.ExBGxPalette_Enable;
	s_frameExtBgPalKnown = true;
	// Task 9: WIN0/WIN1, BLDCNT effects, semi-transparent OBJ and MASTER_BRIGHT are
	// now in scope. Still out: the OBJ window (a sprite-shaped mask), and a
	// window combined with brighten/darken -- GPU.cpp's WINDOWED backdrop path
	// tests `blend1 && windowEffect` where `blend1` is stale state left by
	// whichever layer the PREVIOUS line composited last (it is not the backdrop's
	// own BLDCNT bit), a CPU quirk this file has no model for.
	if (gpu->WINOBJ_ENABLED) { DSB_WHY("objwin"); return true; }
	if ((gpu->WIN0_ENABLED || gpu->WIN1_ENABLED) && (((gpu->BLDCNT >> 6) & 3) >= 2)) { DSB_WHY("winfade"); return true; }
	if (!CommonSettings.showGpu.screens[GPU_SUB]) { DSB_WHY("hidden"); return true; }

	// Mosaic: only matters when a layer actually enables it. Checked on the
	// BG side here (the OBJ side is checked per sprite in
	// gxDsBCollectVisibleObj, where the OAM Mosaic bit lives).
	for (int bg = 0; bg < 4; ++bg) {
		bool enabled = (bg == 0) ? d.BG0_Enable : (bg == 1) ? d.BG1_Enable :
		               (bg == 2) ? d.BG2_Enable : d.BG3_Enable;
		if (!enabled) continue;
		const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
		if (cnt.Mosaic_Enable) { DSB_WHY("mosaic"); return true; }
		// Task 8: text plus every affine flavour is in scope. Anything else
		// (BGType_Invalid -- e.g. BG0/1 enabled in BG_Mode 6/7 -- or the
		// unresolved BGType_AffineExt placeholder) has no reference behaviour
		// worth reproducing here.
		const BGType t = gpu->BGTypes[bg];
		if (t != BGType_Text && !gxDsBIsAffineType((u8)t)) { DSB_WHY("bgtype"); return true; }
		// A non-wrapping affine plane is baked with a 1-texel transparent
		// border padded to a power of two, and GX textures top out at 1024
		// texels per side, so a non-wrapping 1024-wide/tall plane cannot
		// carry one (512 + border -> 1024 still fits).
		if (t != BGType_Text && !cnt.PaletteSet_Wrap &&
		    (gpu->BGSize[bg][0] >= 1024 || gpu->BGSize[bg][1] >= 1024)) { DSB_WHY("aff1024nowrap"); return true; }
		// GPU_resortBGs()'s `dispLayers OP enable` idiom inverts visibility
		// for a *hidden* layer; rather than reproduce that debug-only quirk,
		// bail whenever the user has toggled any Engine-B layer off.
		if (!CommonSettings.dispLayers[GPU_SUB][bg]) { DSB_WHY("hiddenlayer"); return true; }
	}
	if (d.OBJ_Enable && !CommonSettings.dispLayers[GPU_SUB][4]) { DSB_WHY("hiddenobj"); return true; }

	return false;
}

void gxDsEngineBScanline(int line)
{
	if (line < 0 || line >= kDsBScreenH)
		return;
	if (gameInfo.isGBA)
		return;

	GPU *gpu = SubScreen.gpu;
	if (!gpu)
		return;
	// Lazy first-use init: this is the earliest point in a DS frame that is
	// guaranteed to run after main.cpp's InitVideo()/GX_Init(), and it means
	// the Stage-0 dirty bitmaps are sized before the frame whose bakes will
	// consult them rather than one frame late.
	if (!s_initDone && !gxDsEngineBRenderInit()) {
		s_frameOutOfScope = true;
		return;
	}

	if (line == 0) {
		s_frameSawLine0 = true;
		s_frameOutOfScope = false;
		s_affPred[0].valid = s_affPred[1].valid = false;
		s_frameExtBgPalKnown = false;
	} else if (!s_frameSawLine0) {
		// Mid-frame entry (e.g. right after a savestate load): no frame-start
		// snapshot exists, so this frame can't be replayed. Next frame's
		// line 0 re-arms it.
		s_frameOutOfScope = true;
		return;
	}

	// Task 9: MASTER_BRIGHT is applied by the CPU reference to every finished
	// scanline (GPU_RenderLine_MasterBrightness), so it is sampled per line here
	// -- exactly what GPU_RenderLine(line) is about to read -- and replayed on
	// the CPU during readback (see gxDsEngineBRenderFrame), which is exact.
	{
		int f = (int)gpu->MasterBrightFactor;
		if (f > 16) f = 16;
		const int m = gpu->MasterBrightMode;
		s_mbMode[line] = (u8)((m == 1 || m == 2) ? m : 0);
		s_mbFac[line] = (u8)((m == 1 || m == 2) ? f : 0);
	}

	if (gxDsBScanlineOutOfScope(gpu)) {
		s_frameOutOfScope = true;
		return;
	}
	if (s_frameOutOfScope)
		return; // already failed this frame; nothing to gain from banding it

	GxDsBBandRegs cur;
	bool affChanged;
	gxDsBSnapshotBandRegs(gpu, &cur, line, &affChanged);
	if (line == 0) {
		g_dsBBandRegs[0] = cur;
		s_lastBandRegs = cur;
		return;
	}
	if (!affChanged && memcmp(&cur, &s_lastBandRegs, GX_DSB_BANDREGS_CMP_BYTES) == 0)
		return;

	g_dsBFramePlan.bands.recordChangeAtLine(line);
	int bc = g_dsBFramePlan.bands.boundaryCount();
	if (bc > 0 && bc < GX_DSB_MAX_BAND_REGS)
		g_dsBBandRegs[bc] = cur;
	s_lastBandRegs = cur;
}

// ---------------------------------------------------------------------
// GX pipeline state for Engine B's own composite pass. Same shape as
// gx_gba_render.cpp's gxSetup2DState(): texture-only TEV replace, alpha
// blend so a transparent texel leaves the destination untouched (exactly
// the CPU compositor's per-pixel opaque/transparent semantics), no depth
// test (painter's algorithm, same draw order GPU_RenderLine_layer uses),
// orthographic projection mapping model (x,y) 1:1 onto EFB pixel (x,y).
// Undone by main.cpp's GxRestorePresentState() once the EFB copy is done.
// ---------------------------------------------------------------------
static void gxDsBSetup2DState()
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
	guOrtho(proj, 0, kDsBScreenH, 0, kDsBScreenW, 0, 1);
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

static void gxDsBDrawQuad(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1, f32 s0, f32 t0, f32 s1, f32 t1)
{
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0); GX_TexCoord2f32(s0, t0);
		GX_Position2f32(x1, y0); GX_TexCoord2f32(s1, t0);
		GX_Position2f32(x1, y1); GX_TexCoord2f32(s1, t1);
		GX_Position2f32(x0, y1); GX_TexCoord2f32(s0, t1);
	GX_End();
}

// Same screen rectangle as gxDsBDrawQuad but with an independent UV per corner
// (order: top-left, top-right, bottom-right, bottom-left), which is what an
// affine BG/OBJ needs -- its screen rectangle maps to a rotated/scaled
// parallelogram in texture space. GX interpolates UVs linearly across the quad
// with w == 1, and the affine transform is itself linear in screen (x,y), so
// the 4 corner UVs reproduce GPU.cpp's per-pixel `X += PA` / `Y += PC` walk.
static void gxDsBDrawQuadFree(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1,
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

// The CPU reference samples texel floor(U) where U is the running 8.8 value
// at the pixel's own (integer) position. GX_NEAR samples at the pixel's
// SAMPLE POINT, and the quad's corner UVs are interpolated to there. That
// sample point is NOT the geometric pixel centre 0.5: the console GPU places
// it at 7/12 of a pixel (Dolphin's VertexShaderManager documents this as
// "pixel_center_correction = 7/12 - 0.5" and reproduces it). Found the hard
// way: assuming 0.5 made ~5% of texels of a rotated/scaled BG land one texel
// off in Dolphin; a pure translation or an integer-aligned scale can't see it,
// which is why SM64DS's scrolling clouds were byte-exact even before the fix.
//
// The corners are given the value that makes the interpolated sample at pixel
// (i,r) equal U(i,r) + 1/512 -- half a fixed-point step INTO the correct
// texel (every U the CPU can produce is a multiple of 1/256, so that is the
// maximum-margin choice; a 1:1 unrotated BG would otherwise sit exactly on
// texel boundaries):
//   corner = U(corner) + 1/512 - t*(dA + dB)/256          t = sample point
// `num` is U(corner) as the 8.8 numerator, dA/dB the per-column / per-row
// 8.8 steps. All in double, narrowed once.
//
// t: 149/256 = 0.58203125, NOT the nominal 7/12 = 0.58333. The host GPU snaps
// the sample point to its 8-bit sub-pixel grid, which turns Dolphin's 7/12
// into 0.5 + 21/256; with t = 7/12 the residual error (0.0013 px, x a 2.0
// matrix entry) exceeds the 1/512-texel margin and lattice-edge texels flip
// (measured: a 2x-zoomed BG and a small affine OBJ mismatched with 7/12 and
// were byte-exact with 149/256; see the log's task 8 section). On another
// host GPU or on real hardware the true value may differ by ~0.001 px, which
// can only ever move a texel that sits within ~0.004 of a texel edge.
static const double kGxSamplePoint = 149.0 / 256.0;
static inline f32 gxDsBAffineCorner(s32 num, s32 dA, s32 dB)
{
	return (f32)(((double)num + 0.5 - kGxSamplePoint * ((double)dA + (double)dB)) / 256.0);
}

// ---------------------------------------------------------------------
// Stage 1: bakes
// ---------------------------------------------------------------------
// Smallest power of two >= n. Every affine texture (BG plane or OBJ) is baked
// at a power-of-two size: Dolphin's GX_NEAR sampling of a NON-power-of-two
// texture through an interpolated, sub-texel-exact UV was measured to land
// texels wrongly (a 20-wide texture picked the neighbouring texel on every
// 5th column; 36/132/516-wide ones were off by a small negative bias that
// flips lattice-edge texels), whereas the identical UVs on a 32/64/256-wide
// texture were byte-exact. Exactness of the sample is the whole point of this
// path, so the (transparent) padding is worth the memory.
static inline int gxDsBPow2Ge(int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

static bool gxDsBEnsureCap(void **buf, u32 *cap, u32 needed)
{
	if (needed <= *cap)
		return *buf != NULL;
	if (*buf) free(*buf);
	*buf = memalign(32, needed);
	*cap = *buf ? needed : 0;
	return *buf != NULL;
}

// One Engine-B text BG plane -> one RGB5A3 texture the size of the BG's full
// tilemap extent (256x256 .. 512x512), so a whole frame of per-band scroll
// is expressible as GX_REPEAT texture-coordinate offsets instead of a
// re-bake per band.
//
// The tilemap/tile addressing below is a direct transcription of GPU.cpp's
// renderline_textBG(), including its two block-offset rules -- a tile row
// past 31 adds `ADDRESS_STEP_512B << ScreenSize` and a tile column past 31
// adds 32*32*2 -- and its exact flip handling (VFlip selects source row
// 7-y, HFlip source column 7-x; the CPU expresses both as reversed
// traversal, which is the same mapping). Every VRAM access goes through
// MMU_gpu_map() so bank mapping/unmapped pages behave identically.
// The extended BG palette slot a plane samples, or NULL when it uses the
// regular BG palette. Mirrors GPU.cpp exactly: the ext palette only applies
// when DISPCNT.ExBGxPalette_Enable is set AND the plane is a 256-colour text
// BG (renderline_textBG's `if(dispCnt->ExBGxPalette_Enable) pal =
// MMU.ExtPal[core][BGExtPalSlot[num]]`, reached only in the Palette_256
// branch) or a BGType_AffineExt_256x16 plane (extRotBG2). Every other BG kind
// -- 4bpp text, plain affine, 256x1, direct, large-8bpp -- ignores it.
static const u8 *gxDsBExtPalFor(int bg)
{
	GPU *gpu = SubScreen.gpu;
	if (!gpu->dispx_st->dispx_DISPCNT.bits.ExBGxPalette_Enable)
		return NULL;
	const BGType t = gpu->BGTypes[bg];
	const bool applies = (t == BGType_Text) ? (gpu->dispx_st->dispx_BGxCNT[bg].bits.Palette_256 != 0)
	                                        : (t == BGType_AffineExt_256x16);
	return applies ? MMU.ExtPal[1][gpu->BGExtPalSlot[bg]] : NULL;
}

static void gxDsBBakeBgPlane(int bg, GxDsBBgPlaneCache &pc)
{
	GPU *gpu = SubScreen.gpu;
	const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
	const int lg = (int)gpu->BGSize[bg][0];
	const int ht = (int)gpu->BGSize[bg][1];
	const u32 tileBase = gpu->BG_tile_ram[bg];
	const u32 mapBase = gpu->BG_map_ram[bg];
	const bool c256 = cnt.Palette_256 != 0;
	const int screenSize = cnt.ScreenSize;
	const u8 *extPal = gxDsBExtPalFor(bg);
	// 256-colour text with ext palettes: index = (tile palNum << 8) + texel
	// (GPU.cpp: `tilePal = pal + ((Palette<<9) & extPalMask)`, 2 bytes/entry).
	u8 *const pal = extPal ? (u8 *)extPal : MMU.ARM9_VMEM + kDsBBgPalOff;
	const int tileBytes = c256 ? 0x40 : 0x20;
	const int rowBytes = c256 ? 8 : 4;

	pc.valid = false;
	if (lg <= 0 || ht <= 0 || lg > kDsBBgPlaneMaxPx || ht > kDsBBgPlaneMaxPx)
		return;
	if (!gxDsBEnsureBgScratch((u32)lg * ht))
		return;

	for (int ty = 0; ty < ht / 8; ++ty) {
		u32 rowMap = mapBase + (u32)(ty & 31) * 64;
		if (ty > 31)
			rowMap += (u32)ADDRESS_STEP_512B << screenSize;
		for (int tx = 0; tx < lg / 8; ++tx) {
			u32 mapAddr = rowMap + (u32)((tx & 31) << 1);
			if (tx > 31)
				mapAddr += 32 * 32 * 2;
			u16 entry = T1ReadWord(MMU_gpu_map(mapAddr), 0);
			int tileNum = entry & 0x3FF;
			bool hflip = ((entry >> 10) & 1) != 0;
			bool vflip = ((entry >> 11) & 1) != 0;
			int palNum = (entry >> 12) & 0xF;

			for (int sy = 0; sy < 8; ++sy) {
				int srcY = vflip ? 7 - sy : sy;
				const u8 *line = (const u8 *)MMU_gpu_map(tileBase + (u32)tileNum * tileBytes + (u32)srcY * rowBytes);
				u16 *out = s_bgBakeScratch + (u32)(ty * 8 + sy) * lg + tx * 8;
				for (int sx = 0; sx < 8; ++sx) {
					int srcX = hflip ? 7 - sx : sx;
					int idx;
					if (c256) {
						idx = line[srcX];
					} else {
						u8 b = line[srcX >> 1];
						idx = (srcX & 1) ? (b >> 4) : (b & 0xF);
					}
					if (!idx) {
						out[sx] = kDsBTransparentTexel;
					} else {
						int palIdx = c256 ? (extPal ? (palNum << 8) + idx : idx) : (palNum * 16 + idx);
						out[sx] = gxDsBOpaqueTexel(T1ReadWord(pal, (u32)palIdx * 2));
					}
				}
			}
		}
	}

	u32 needed = (u32)lg * ht * sizeof(u16);
	if (!gxDsBEnsureCap(&pc.texData, &pc.texDataCap, needed))
		return;

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_bgBakeScratch, (u16 *)pc.texData, blk.texelsWide, blk.texelsTall, lg, ht);
	DCFlushRange(pc.texData, needed);
	GX_InitTexObj(&pc.texObj, pc.texData, lg, ht, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
	// GX_NEAR, not GX_InitTexObj's default GX_LINEAR: this is an emulated
	// 2D framebuffer sampled 1:1, so any bilinear tap mixes a texel with
	// its neighbours instead of reproducing it. Found by A/B rather than
	// by inspection -- see the copy-filter comment in
	// gxDsEngineBRenderFrame() for the measurement that exposed it.
	GX_InitTexObjFilterMode(&pc.texObj, GX_NEAR, GX_NEAR);
	pc.wpx = (u16)lg;
	pc.hpx = (u16)ht;
	pc.bufW = (u16)lg;
	pc.bufH = (u16)ht;
	pc.cfgTileBase = tileBase;
	pc.cfgMapBase = mapBase;
	pc.cfgColorMode = (u8)(c256 ? 1 : 0);
	pc.cfgScreenSize = (u8)screenSize;
	pc.cfgType = (u8)BGType_Text;
	pc.cfgWrap = 0;
	pc.cfgAuxBase = 0;
	pc.cfgExtPal = extPal;
	pc.cfgFx = (u16)((s_bakeFadeMode << 8) | s_bakeFadeLvl);
	pc.valid = true;
}

// Task 8: one affine-flavoured BG plane -> one RGB5A3 texture.
//
// Covers BGType_Affine (1-byte map entries, no flip), BGType_AffineExt_256x16
// (2-byte entries WITH per-tile H/V flip, 8bpp tiles; the entry's palette bits
// only matter with extended palettes, which are out of scope), _256x1 (an
// 8bpp bitmap), _Direct (a 15-bit direct-colour bitmap whose bit 15 is the
// per-pixel opaque flag) and BGType_Large8bpp (a big 8bpp bitmap). Every
// address/decode rule below is transcribed from GPU.cpp's rot_tiled_8bit_entry,
// rot_tiled_16bit_entry, rot_256_map and rot_BMP_map -- NOT from the GBA
// slice: in particular the DS extended-affine map entry is 16-bit TILEENTRY
// layout (TileNum 10 bits, HFlip bit 10, VFlip bit 11), which the GBA's
// 1-byte affine map has no analogue of.
//
// Transparent-border technique (same as gx_gba_render.cpp): when the plane's
// overflow-wrap bit is clear an out-of-map sample must read TRANSPARENT (the
// CPU simply skips such a pixel), so the map is baked inside a 1-texel
// transparent border and sampled GX_CLAMP; when it wraps, the plane is baked
// at exactly its own power-of-two size and sampled GX_REPEAT (== the CPU's
// `& (wh-1)`). The bordered size is padded up to the next power of two (see
// gxDsBPow2Ge for why non-power-of-two affine textures are avoided).
//
// Row/tile address mapping: the CPU calls MMU_gpu_map() once per *pixel* on
// (base + linear offset). Bases are 16KB-aligned (MMU_BBG + n*16KB), a 64-byte
// tile block never straddles a 16KB page, and a bitmap row (<= 1024 bytes,
// row starts at multiples of its own power-of-two length) never does either,
// so mapping once per tile / once per row resolves to exactly the same host
// bytes.
static void gxDsBBakeAffineBgPlane(int bg, GxDsBBgPlaneCache &pc)
{
	GPU *gpu = SubScreen.gpu;
	const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
	const BGType type = gpu->BGTypes[bg];
	const int lg = (int)gpu->BGSize[bg][0];
	const int ht = (int)gpu->BGSize[bg][1];
	const bool wrap = cnt.PaletteSet_Wrap != 0;
	u8 *const pal = MMU.ARM9_VMEM + kDsBBgPalOff;

	pc.valid = false;
	if (lg <= 0 || ht <= 0 || lg > 1024 || ht > 1024)
		return;
	const int border = wrap ? 0 : 1;
	const int bufW = wrap ? lg : gxDsBPow2Ge(lg + 2);
	const int bufH = wrap ? ht : gxDsBPow2Ge(ht + 2);
	if (bufW > 1024 || bufH > 1024)
		return; // scope check bails these frames; defensive
	if (!gxDsBEnsureBgScratch((u32)bufW * bufH))
		return;

	u16 *const buf = s_bgBakeScratch;
	if (!wrap)
		memset(buf, 0, (size_t)bufW * bufH * sizeof(u16)); // 0 == transparent RGB5A3

	// Index -> RGB5A3 lookup for the 8bpp flavours; index 0 is transparent.
	u16 lut[256];
	lut[0] = kDsBTransparentTexel;
	for (int i = 1; i < 256; ++i)
		lut[i] = gxDsBOpaqueTexel(T1ReadWord(pal, (u32)i * 2));
	// BGType_AffineExt_256x16 with DISPCNT.ExBGxPalette_Enable: the tile's
	// 4-bit palette number selects one of 16 256-entry palettes in the ext
	// slot (rot_tiled_16bit_entry<extPal=true>: `pal[(entry + (Palette<<8))<<1]`).
	const u8 *extPal = gxDsBExtPalFor(bg);

	const u32 tileBase = gpu->BG_tile_ram[bg];
	const u32 mapBase = gpu->BG_map_ram[bg];
	u32 auxBase = 0;

	if (type == BGType_Affine || type == BGType_AffineExt_256x16) {
		const int tw = lg >> 3, th = ht >> 3;
		for (int ty = 0; ty < th; ++ty) {
			for (int tx = 0; tx < tw; ++tx) {
				int tileNum, palNum = 0;
				bool hflip = false, vflip = false;
				if (type == BGType_Affine) {
					tileNum = *(const u8 *)MMU_gpu_map(mapBase + (u32)(tx + ty * tw));
				} else {
					u16 e = T1ReadWord(MMU_gpu_map(mapBase + (((u32)(tx + ty * tw)) << 1)), 0);
					tileNum = e & 0x3FF;
					hflip = ((e >> 10) & 1) != 0;
					vflip = ((e >> 11) & 1) != 0;
					palNum = (e >> 12) & 0xF;
				}
				const u8 *tile = (const u8 *)MMU_gpu_map(tileBase + ((u32)tileNum << 6));
				for (int sy = 0; sy < 8; ++sy) {
					const u8 *row = tile + (vflip ? 7 - sy : sy) * 8;
					u16 *out = buf + (u32)(ty * 8 + sy + border) * bufW + tx * 8 + border;
					if (extPal) {
						for (int sx = 0; sx < 8; ++sx) {
							int idx = row[hflip ? 7 - sx : sx];
							out[sx] = idx ? gxDsBOpaqueTexel(T1ReadWord((void *)extPal, (u32)((palNum << 8) + idx) * 2))
							              : kDsBTransparentTexel;
						}
					} else {
						for (int sx = 0; sx < 8; ++sx)
							out[sx] = lut[row[hflip ? 7 - sx : sx]];
					}
				}
			}
		}
	} else if (type == BGType_AffineExt_256x1 || type == BGType_Large8bpp) {
		auxBase = (type == BGType_Large8bpp) ? gpu->BG_bmp_large_ram[bg] : gpu->BG_bmp_ram[bg];
		for (int y = 0; y < ht; ++y) {
			const u8 *row = (const u8 *)MMU_gpu_map(auxBase + (u32)y * lg);
			u16 *out = buf + (u32)(y + border) * bufW + border;
			for (int x = 0; x < lg; ++x)
				out[x] = lut[row[x]];
		}
	} else if (type == BGType_AffineExt_Direct) {
		auxBase = gpu->BG_bmp_ram[bg];
		for (int y = 0; y < ht; ++y) {
			void *row = MMU_gpu_map(auxBase + (((u32)y * lg) << 1));
			u16 *out = buf + (u32)(y + border) * bufW + border;
			for (int x = 0; x < lg; ++x) {
				u16 c = T1ReadWord(row, (u32)x << 1);
				out[x] = (c & 0x8000) ? gxDsBOpaqueTexel(c) : kDsBTransparentTexel;
			}
		}
	} else {
		return;
	}

	u32 needed = (u32)bufW * bufH * sizeof(u16);
	if (!gxDsBEnsureCap(&pc.texData, &pc.texDataCap, needed))
		return;
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(buf, (u16 *)pc.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
	DCFlushRange(pc.texData, needed);
	const u8 wm = wrap ? GX_REPEAT : GX_CLAMP;
	GX_InitTexObj(&pc.texObj, pc.texData, bufW, bufH, GX_TF_RGB5A3, wm, wm, GX_FALSE);
	GX_InitTexObjFilterMode(&pc.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
	pc.wpx = (u16)lg;
	pc.hpx = (u16)ht;
	pc.bufW = (u16)bufW;
	pc.bufH = (u16)bufH;
	pc.cfgTileBase = tileBase;
	pc.cfgMapBase = mapBase;
	pc.cfgColorMode = (u8)(cnt.Palette_256 ? 1 : 0);
	pc.cfgScreenSize = (u8)cnt.ScreenSize;
	pc.cfgType = (u8)type;
	pc.cfgWrap = (u8)(wrap ? 1 : 0);
	pc.cfgAuxBase = auxBase;
	pc.cfgExtPal = extPal;
	pc.cfgFx = (u16)((s_bakeFadeMode << 8) | s_bakeFadeLvl);
	pc.valid = true;
}

struct GxDsBObjDraw {
	s16 x, y;
	u16 w, h;          // sprite (texture) size
	u16 boxW, boxH;    // on-screen box: == w,h, or 2x for a double-size affine sprite
	bool affine;
	s16 pa, pb, pc, pd; // 8.8 fixed point OAM rot/scale matrix (affine only)
	u8 priority;
	bool hflip, vflip;
	bool semi;       // task 9: OAM Mode == 1 (semi-transparent: blends with the layer beneath)
	bool depth;      // OAM attr0 bit13: 0 = 16-colour, 1 = 256-colour
	u8 palIndex;
	u16 tileIndex;
	u8 oamIndex;
};
static GxDsBObjDraw s_objDraws[128];
static int s_objDrawCount;

// Scans Engine B's OAM (MMU.ARM9_OAM + 0x400) for this frame's drawable,
// in-scope sprites. Mirrors GPU.cpp's _spriteRender()/compute_sprite_vars()
// field decode exactly -- including that RotScale==2 is the OBJ-disable
// encoding (skip, not a bail) while RotScale 1/3 are rot/scale (out of
// scope), and that a Y >= 192 is reinterpreted as a signed 8-bit offset.
// OAM words are read with T1ReadWord rather than through GPU.h's `OAM`
// struct on purpose: that struct needs GPU.cpp's WORDS_BIGENDIAN word
// rotations to be read correctly, whereas T1ReadWord already reads the
// little-endian DS-native OAM bytes directly on either host endianness
// (the same choice gx_gba_render.cpp makes for GBA OAM).
//
// Returns false if any sprite that would actually be drawn uses a feature
// outside this phase's scope, in which case the whole frame bails.
static bool gxDsBCollectVisibleObj()
{
	u8 *const oam = MMU.ARM9_OAM + kDsBOamOff;
	s_objDrawCount = 0;

	for (int i = 0; i < 128; ++i) {
		u32 off = (u32)i * 8;
		u16 a0 = T1ReadWord(oam, off);
		u16 a1 = T1ReadWord(oam, off + 2);
		u16 a2 = T1ReadWord(oam, off + 4);

		int rotScale = (a0 >> 8) & 3;
		if (rotScale == 2) continue;      // OBJ disabled (GPU.cpp: `RotScale == 2` -> continue)

		int mode = (a0 >> 10) & 3;
		int shape = (a0 >> 14) & 3;
		int sizeSel = (a1 >> 14) & 3;
		const size &ss = sprSizeTab[sizeSel][shape];
		int w = ss.x, h = ss.y;
		if (w <= 0 || h <= 0 || w > kDsBObjMaxPx || h > kDsBObjMaxPx) continue;

		// On-screen box. RotScale == 3 is the double-size rot/scale encoding,
		// whose box is twice the sprite -- accounted for here so the
		// visibility test below is never an under-count for a sprite this
		// pass would have to bail on anyway.
		int boxW = (rotScale == 3) ? w * 2 : w;
		int boxH = (rotScale == 3) ? h * 2 : h;
		if (boxW > 2 * kDsBObjMaxPx || boxH > 2 * kDsBObjMaxPx) continue;

		int sprY = a0 & 0xFF;
		if (sprY >= 192) sprY = (s32)(s8)(u8)sprY;
		int sprX = (s32)((s16)((a1 & 0x1FF) << 7)) >> 7; // sign-extend 9-bit X

		// Visibility is tested BEFORE the scope checks below on purpose: an
		// unused/parked OAM entry routinely holds whatever was last written
		// there (rot/scale bits, a stale Mode, a mosaic bit) while being
		// parked off-screen, and the CPU reference never draws it either. A
		// scope check ordered ahead of this would bail the whole frame on
		// sprites that contribute nothing.
		if (sprX + boxW <= 0 || sprX >= kDsBScreenW) continue;
		if (sprY + boxH <= 0 || sprY >= kDsBScreenH) continue;

		const bool affine = (rotScale & 1) != 0; // 1 or 3 (2 was skipped above)
		// GPU.cpp tests a sprite's rows with `(l - sprY) & 255 < fieldY`, i.e.
		// a box that hangs past line 255 wraps round and ALSO appears at the
		// top of the screen. Only a double-size 64-tall affine sprite can be
		// tall enough to reach that (sprY < 192, boxH <= 128); reproducing the
		// wrap isn't worth a quad split, so bail on it.
		if (sprY >= 0 && sprY + boxH > 256) return false;
		// Mode 2 (OBJ window) sprites only ever write GPU.cpp's sprWin[]
		// mask, which is read exclusively when WINOBJ_ENABLED -- and this
		// file already bails the frame for that (gxDsBScanlineOutOfScope).
		// With the OBJ window off they draw no pixels and take no part in
		// prioTab, so skipping them is exact, not a shortcut.
		// (An AFFINE Mode-2 sprite is different: GPU.cpp's rot/scale branch
		// has no Mode-2 special case and draws its texels as ordinary pixels
		// tagged type 2, a path this file has no model for -> bail.)
		if (mode == 2 && !affine) continue;
		// Mode 1 (semi-transparent) blends against the layer beneath
		// whenever BLDCNT's 2nd-target bits select it, INDEPENDENTLY of
		// BLDCNT's effect field (GPU.cpp's _master_setFinalOBJColor tests
		// `type == GPU_OBJ_MODE_Transparent` outside the FUNC switch). Task 9
		// brings it in scope: it becomes an alpha-blended draw when the layer
		// beneath is a 2nd target (see gxDsBPlanBand); the rot/scale branch of
		// _spriteRender tags its pixels with spriteInfo->Mode the same way the
		// plain branches tag them (alpha ? 1 : 0), so affine sprites qualify too.
		// Mode 3 is bitmap OBJ, and an affine Mode 2 (see above) has no model.
		if (mode == 2 || mode == 3) return false;
		if ((a0 >> 12) & 1) return false;

		GxDsBObjDraw &d = s_objDraws[s_objDrawCount++];
		d.x = (s16)sprX;
		d.y = (s16)sprY;
		d.w = (u16)w;
		d.h = (u16)h;
		d.boxW = (u16)boxW;
		d.boxH = (u16)boxH;
		d.affine = affine;
		d.pa = d.pb = d.pc = d.pd = 0;
		if (affine) {
			// Same selector GPU.cpp uses: the 5-bit group is RotScalIndex
			// (attr1 bits 9-11) + HFlip<<3 + VFlip<<4 == attr1 bits 9-13, and
			// group g's matrix lives in the attr3 words of OAM entries
			// 4g..4g+3 (PA, PB, PC, PD).
			u32 bp = (u32)((a1 >> 9) & 0x1F) * 4;
			d.pa = (s16)T1ReadWord(oam, (bp + 0) * 8 + 6);
			d.pb = (s16)T1ReadWord(oam, (bp + 1) * 8 + 6);
			d.pc = (s16)T1ReadWord(oam, (bp + 2) * 8 + 6);
			d.pd = (s16)T1ReadWord(oam, (bp + 3) * 8 + 6);
		}
		d.semi = (mode == 1);
		d.priority = (u8)((a2 >> 10) & 3);
		// For an affine sprite attr1 bits 12/13 are matrix-group bits, not flips.
		d.hflip = !affine && ((a1 >> 12) & 1) != 0;
		d.vflip = !affine && ((a1 >> 13) & 1) != 0;
		d.depth = ((a0 >> 13) & 1) != 0;
		d.palIndex = (u8)((a2 >> 12) & 0xF);
		d.tileIndex = (u16)(a2 & 0x3FF);
		d.oamIndex = (u8)i;
	}
	return true;
}

// One sprite -> one w x h RGB5A3 texture, unflipped (H/V flip is applied at
// draw time by swapping the quad's UVs, which is exactly the reversed
// traversal compute_sprite_vars() performs). Address arithmetic transcribed
// from GPU.cpp's _spriteRender() non-rotozoomed 16/256-colour branches for
// both SPRITE_1D and SPRITE_2D mapping, including the per-row
// MMU_gpu_map() call the CPU makes (so a row that straddles a 16KB VRAM
// page behaves identically to the reference).
static void gxDsBBakeObjTexture(const GxDsBObjDraw &d, GxDsBObjTexSlot &slot)
{
	GPU *gpu = SubScreen.gpu;
	const bool oneDim = (gpu->spriteRenderMode == GPU::SPRITE_1D);
	const int rowBytes = d.depth ? 8 : 4;
	u8 *const pal = MMU.ARM9_VMEM + kDsBObjPalOff;

	slot.valid = false;

	// Task 8: an affine sprite is sampled through an arbitrary matrix, so any
	// sample landing outside the w x h image must read transparent (the CPU
	// simply doesn't draw it). Same trick as the affine BG planes: bake inside
	// a 1-texel transparent border and sample GX_CLAMP. The border is padded to
	// the RGB5A3 4x4 block size; a plain sprite keeps its exact-size,
	// borderless bake so task 7's output is untouched.
	const bool bordered = d.affine;
	const int off = bordered ? 1 : 0;
	const int bufW = bordered ? gxDsBPow2Ge(d.w + 2) : d.w;
	const int bufH = bordered ? gxDsBPow2Ge(d.h + 2) : d.h;
	if (bordered)
		memset(s_objBakeScratch, 0, (size_t)bufW * bufH * sizeof(u16)); // 0 == transparent

	// Plain sprites: GPU.cpp resolves MMU_gpu_map() once per ROW (compute_
	// sprite_vars/_spriteRender's per-line `src = MMU_gpu_map(...)`), affine
	// sprites once per SPRITE (one `src = MMU_gpu_map(sprMem + (TileIndex <<
	// block))`, then flat offsets from that host pointer). Reproduce whichever
	// applies so a sprite whose data straddles two non-contiguous 16KB VRAM
	// pages reads the same bytes the reference does.
	const u8 *affBase = NULL;
	if (bordered)
		affBase = (const u8 *)MMU_gpu_map(gpu->sprMem + ((u32)d.tileIndex << (oneDim ? gpu->sprBoundary : 5)));

	for (int y = 0; y < d.h; ++y) {
		const u8 *src;
		if (oneDim) {
			u32 rel = (u32)(y >> 3) * (u32)d.w * rowBytes + (u32)(y & 7) * rowBytes;
			src = bordered ? affBase + rel
			               : (const u8 *)MMU_gpu_map(gpu->sprMem + ((u32)d.tileIndex << gpu->sprBoundary) + rel);
		} else {
			u32 rel = ((u32)(y >> 3) << 10) + (u32)(y & 7) * rowBytes;
			src = bordered ? affBase + rel
			               : (const u8 *)MMU_gpu_map(gpu->sprMem + ((u32)d.tileIndex << 5) + rel);
		}
		u16 *out = s_objBakeScratch + (u32)(y + off) * bufW + off;
		for (int x = 0; x < d.w; ++x) {
			int idx;
			if (d.depth) {
				idx = src[(x & 7) + ((x & 0xFFF8) << 3)];
			} else {
				int x1 = x >> 1;
				u8 b = src[(x1 & 3) + ((x1 & 0xFFFC) << 3)];
				idx = (x & 1) ? (b >> 4) : (b & 0xF);
			}
			if (!idx) {
				out[x] = kDsBTransparentTexel;
			} else {
				int palIdx = d.depth ? idx : (d.palIndex * 16 + idx);
				out[x] = gxDsBOpaqueTexel(T1ReadWord(pal, (u32)palIdx * 2));
			}
		}
	}

	u32 needed = (u32)bufW * bufH * sizeof(u16);
	if (!gxDsBEnsureCap(&slot.texData, &slot.texDataCap, needed))
		return;

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_objBakeScratch, (u16 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
	DCFlushRange(slot.texData, needed);
	GX_InitTexObj(&slot.texObj, slot.texData, bufW, bufH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&slot.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
	slot.w = d.w;
	slot.h = d.h;
	slot.bufW = (u16)bufW;
	slot.bufH = (u16)bufH;
	slot.cfgBordered = (u8)(bordered ? 1 : 0);
	slot.cfgTileIndex = d.tileIndex;
	slot.cfgPalIndex = d.palIndex;
	slot.cfgDepth = (u8)(d.depth ? 1 : 0);
	slot.cfgOneDim = (u8)(oneDim ? 1 : 0);
	slot.cfgBoundary = gpu->sprBoundary;
	slot.cfgFx = (u16)((s_bakeFadeMode << 8) | s_bakeFadeLvl);
	slot.valid = true;
}

static void gxDsBBakeBackdrop(int band, int fadeMode, int fadeLvl)
{
	// GPU_RenderLine_layer(): backdrop colour is BG palette entry 0 of this
	// engine's own 1KB palette half, masked to 15 bits. With no window and a
	// brighten/darken effect whose 1st-target set includes the backdrop
	// (BLDCNT bit 5) the CPU fades it too (case 2/3 of its backdrop switch).
	u16 c = (u16)(T1ReadWord(MMU.ARM9_VMEM, kDsBBgPalOff) & 0x7FFF);
	if (fadeMode) c = gxDsBFadeColor(c, fadeMode, fadeLvl);
	u16 *t = (u16 *)((u8 *)s_backdropTexData + 32 * band);
	t[0] = gxPackRGB5A3Opaque(gxExtractBGR555(c));
	DCFlushRange(t, 32);
}

// ---------------------------------------------------------------------
// Task 9: per-band window / colour-effect plan
// ---------------------------------------------------------------------
// Everything below is derived from GPU.cpp, not from the GBA slice:
//  - Painter order is GPU_RenderLine_layer's: backdrop, then priority 3..0,
//    within a tier BG3..BG0 and then that tier's sprites.
//  - A BG pixel is written iff the window test passes for ITS layer bit; the
//    test order is WIN0 > WIN1 > (OBJ window, out of scope) > WINOUT
//    (renderline_checkWindows). The backdrop is always drawn.
//  - BG colour effect (_master_setFinalBGColor): iff the layer is a BLDCNT
//    1st target AND the region's window "effect" bit allows it; brighten/darken
//    recolour the layer's own colour, alpha blends with the pixel ALREADY in the
//    line buffer only if the layer that produced that pixel is a 2nd target,
//    otherwise the pixel is written raw.
//  - OBJ (_master_setFinalOBJColor): same fade rule; a sprite pixel blends with
//    the pixel beneath iff that pixel's layer is a 2nd target AND it is not
//    another sprite (`bg_under != 4`), and only if the sprite is Mode 1
//    (semi-transparent, regardless of BLDCNT's effect/1st-target bits/window
//    effect bit) or the effect is alpha with OBJ as a 1st target.
//  - EVA/EVB are each clamped to 16 and combined per channel as
//    min(31, (a*EVA + b*EVB) >> 4).
//
// GX can only do "blend with whatever is in the EFB" (no per-pixel knowledge of
// which layer produced it -- the EFB carries no alpha, see task 2), so an alpha
// item is drawn natively only when EVERY layer that could lie beneath it in this
// region is a 2nd target (then "blend with dst" == the CPU's rule everywhere),
// or NONE is (then the CPU never blends it and it is an ordinary draw). Mixed
// cases bail the frame (queue item: dst-alpha mask via an RGBA6 EFB).
struct GxDsBCfg { u8 mask; u8 effect; };            // window region: layers drawn, effect allowed
struct GxDsBRun { s16 x0, x1; u8 cfg; };
struct GxDsBSlab { s16 y0, y1; u8 nrun; GxDsBRun run[10]; };
struct GxDsBBandPlan {
	u8 nslab;
	GxDsBSlab slab[10];
	GxDsBCfg cfg[3];       // 0 = outside all windows (or "no windows"), 1 = WIN1, 2 = WIN0
	u8 cfgUsed;            // bit c: some run uses cfg c
	u8 mode, t1, t2, eva, evb, evy;
	u8 fadeBg, fadeObj;    // BGs / OBJ that need their faded variant in this band
	u8 blendLive[3];       // per cfg, slot-indexed (BG b -> bit b, OBJ tier p -> bit 4+p): items that really alpha-blend
	u8 tierHas, tierSemi;  // bit p: a sprite / a semi-transparent sprite of priority p touches this band
	bool backdropFade;
	bool windows;
};
static GxDsBBandPlan s_bandPlan[GX_DSB_MAX_BAND_REGS];

// Which pixel columns / rows a DS window covers: GPU.cpp's update_winh() /
// setup_windows(). start > end wraps (inside = [0,end] U [start,255]); start ==
// end is empty. Returns the number of half-open intervals written to iv[].
static int gxDsBWinIv(u8 s, u8 e, int iv[2][2])
{
	if (s > e) { iv[0][0] = 0; iv[0][1] = (int)e + 1; iv[1][0] = s; iv[1][1] = 256; return 2; }
	if (e > s) { iv[0][0] = s; iv[0][1] = e; return 1; }
	return 0;
}
static bool gxDsBInIv(const int iv[2][2], int n, int v)
{
	for (int i = 0; i < n; ++i)
		if (v >= iv[i][0] && v < iv[i][1]) return true;
	return false;
}
static void gxDsBSortUniq(int *a, int &n)
{
	for (int i = 1; i < n; ++i) { int v = a[i], j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; } a[j + 1] = v; }
	int m = 0;
	for (int i = 0; i < n; ++i) if (m == 0 || a[i] != a[m - 1]) a[m++] = a[i];
	n = m;
}

// Decomposes the band [y0,y1) into disjoint rectangles, each classified with the
// window region (cfg) that wins there. Draw order alone cannot express "layer L
// is off inside WIN1" (an earlier full-band pass of L would still show through),
// so unlike the GBA slice's overdraw scheme every layer is drawn ONLY into the
// rectangles whose region enables it.
static void gxDsBBuildRegions(const GxDsBBandRegs &r, int y0, int y1, GxDsBBandPlan &p)
{
	const bool en[2] = { (r.winEn & 1) != 0, (r.winEn & 2) != 0 };
	int vy[2][2][2], hx[2][2][2], nvy[2] = { 0, 0 }, nhx[2] = { 0, 0 };
	if (en[0]) { nvy[0] = gxDsBWinIv(r.win0v0, r.win0v1, vy[0]); nhx[0] = gxDsBWinIv(r.win0h0, r.win0h1, hx[0]); }
	if (en[1]) { nvy[1] = gxDsBWinIv(r.win1v0, r.win1v1, vy[1]); nhx[1] = gxDsBWinIv(r.win1h0, r.win1h1, hx[1]); }
	p.windows = en[0] || en[1];
	if (!p.windows) {
		p.cfg[0].mask = 0x1F; p.cfg[0].effect = 1;
	} else {
		p.cfg[0].mask = r.winOut;  p.cfg[0].effect = (r.winSp >> 2) & 1;
		p.cfg[1].mask = r.winIn1;  p.cfg[1].effect = (r.winSp >> 1) & 1;
		p.cfg[2].mask = r.winIn0;  p.cfg[2].effect = r.winSp & 1;
	}

	int cuts[12], nc = 0;
	cuts[nc++] = y0; cuts[nc++] = y1;
	for (int w = 0; w < 2; ++w)
		for (int i = 0; i < nvy[w]; ++i)
			for (int e = 0; e < 2; ++e) {
				int v = vy[w][i][e];
				if (v > y0 && v < y1) cuts[nc++] = v;
			}
	gxDsBSortUniq(cuts, nc);

	p.nslab = 0;
	p.cfgUsed = 0;
	for (int k = 0; k + 1 < nc; ++k) {
		GxDsBSlab &sl = p.slab[p.nslab++];
		sl.y0 = (s16)cuts[k]; sl.y1 = (s16)cuts[k + 1]; sl.nrun = 0;
		bool act[2];
		for (int w = 0; w < 2; ++w) act[w] = en[w] && gxDsBInIv(vy[w], nvy[w], sl.y0);
		int xc[12], nx = 0;
		xc[nx++] = 0; xc[nx++] = 256;
		for (int w = 0; w < 2; ++w)
			if (act[w])
				for (int i = 0; i < nhx[w]; ++i)
					for (int e = 0; e < 2; ++e) {
						int v = hx[w][i][e];
						if (v > 0 && v < 256) xc[nx++] = v;
					}
		gxDsBSortUniq(xc, nx);
		for (int j = 0; j + 1 < nx; ++j) {
			int c = (act[0] && gxDsBInIv(hx[0], nhx[0], xc[j])) ? 2 : (act[1] && gxDsBInIv(hx[1], nhx[1], xc[j])) ? 1 : 0;
			if (sl.nrun && sl.run[sl.nrun - 1].cfg == c && sl.run[sl.nrun - 1].x1 == xc[j]) {
				sl.run[sl.nrun - 1].x1 = (s16)xc[j + 1];
			} else {
				GxDsBRun &ru = sl.run[sl.nrun++];
				ru.x0 = (s16)xc[j]; ru.x1 = (s16)xc[j + 1]; ru.cfg = (u8)c;
			}
			p.cfgUsed |= (u8)(1 << c);
		}
	}
}

// What BLDCNT asks of `layer` (0-3 BG, 4 OBJ) in window region `c`.
static inline void gxDsBResolveFx(const GxDsBBandPlan &p, const GxDsBCfg &c, int layer, bool semi, bool &fade, bool &want)
{
	const bool eff = c.effect && ((p.t1 >> layer) & 1);
	fade = eff && p.mode >= 2;
	want = (eff && p.mode == 1) || (layer == 4 && semi);
}

static bool gxDsBBoxOverlap(const GxDsBObjDraw &a, const GxDsBObjDraw &b)
{
	return a.x < b.x + b.boxW && b.x < a.x + a.boxW && a.y < b.y + b.boxH && b.y < a.y + a.boxH;
}

// Builds and validates one band's plan. Returns false (with DSB_WHY set) when
// the band needs something GX can't do exactly -> the whole frame bails.
static bool gxDsBPlanBand(const GxDsBBandRegs &r, int y0, int y1, GxDsBBandPlan &p)
{
	p.mode = (u8)((r.bldcnt >> 6) & 3);
	p.t1 = (u8)(r.bldcnt & 0x3F);
	p.t2 = (u8)((r.bldcnt >> 8) & 0x3F);
	p.eva = r.eva; p.evb = r.evb; p.evy = r.evy;
	p.fadeBg = p.fadeObj = 0;
	p.blendLive[0] = p.blendLive[1] = p.blendLive[2] = 0;
	p.tierHas = p.tierSemi = 0;
	for (int i = 0; i < s_objDrawCount; ++i) {
		const GxDsBObjDraw &d = s_objDraws[i];
		if (d.y >= y1 || d.y + d.boxH <= y0) continue;
		p.tierHas |= (u8)(1 << d.priority);
		if (d.semi) p.tierSemi |= (u8)(1 << d.priority);
	}
	gxDsBBuildRegions(r, y0, y1, p);
	p.backdropFade = !p.windows && p.mode >= 2 && (p.t1 & 0x20);

	bool needOverlapCheck = false, anyLive = false;
	for (int c = 0; c < 3; ++c) {
		if (!((p.cfgUsed >> c) & 1)) continue;
		const GxDsBCfg &cf = p.cfg[c];
		u8 under = 0x20; // the backdrop is always beneath everything
		for (int prio = 3; prio >= 0; --prio) {
			for (int bg = 3; bg >= 0; --bg) {
				if (!((r.bgEnable >> bg) & 1) || r.bgPrio[bg] != prio || !((cf.mask >> bg) & 1)) continue;
				bool fade, want;
				gxDsBResolveFx(p, cf, bg, false, fade, want);
				if (fade) p.fadeBg |= (u8)(1 << bg);
				if (want && (under & p.t2)) {
					if (under & ~p.t2) { DSB_WHY("blendunder"); return false; }
					p.blendLive[c] |= (u8)(1 << bg);
					anyLive = true;
				}
				under |= (u8)(1 << bg);
			}
			if (r.objEnable && ((cf.mask >> 4) & 1) && ((p.tierHas >> prio) & 1)) {
				bool fade, want;
				gxDsBResolveFx(p, cf, 4, false, fade, want);
				if (fade) p.fadeObj = 1;
				want = want || ((p.tierSemi >> prio) & 1);
				const u8 u = (u8)(under & ~0x10); // a sprite never blends with another sprite
				if (want && (u & p.t2)) {
					if (u & ~p.t2) { DSB_WHY("blendunder"); return false; }
					p.blendLive[c] |= (u8)(1 << (4 + prio));
					anyLive = true;
					needOverlapCheck = true;
				}
				under |= 0x10;
			}
		}
	}
	if (anyLive && (int)p.eva + (int)p.evb != 16) { DSB_WHY("blendcoef"); return false; }
	if (needOverlapCheck) {
		// A blended sprite must sit directly on the BG/backdrop line buffer. Painter
		// order would blend it with any sprite drawn earlier under it (the CPU
		// resolves sprite-vs-sprite first and blends only the winner), so any box
		// overlap with another sprite bails.
		for (int i = 0; i < s_objDrawCount; ++i) {
			const GxDsBObjDraw &a = s_objDraws[i];
			if (a.y >= y1 || a.y + a.boxH <= y0) continue;
			if (!(a.semi || (p.mode == 1 && (p.t1 & 0x10)))) continue;
			for (int j = 0; j < s_objDrawCount; ++j) {
				if (j == i) continue;
				const GxDsBObjDraw &o = s_objDraws[j];
				if (o.y >= y1 || o.y + o.boxH <= y0) continue;
				if (gxDsBBoxOverlap(a, o)) { DSB_WHY("blendobjoverlap"); return false; }
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------
// Stage 4: per-band draw
// ---------------------------------------------------------------------
// TEV state. Opaque/plain draws use GX_REPLACE (texel colour + texel alpha).
// An alpha-blended draw keeps the texel colour but replaces its alpha with a
// CONSTANT (the EVA coefficient) wherever the texel is opaque, using the TEV
// compare op: out = (texA > 0) ? A0 : 0. Constant-alpha rather than the RGB5A3
// alpha sub-format because that one only has 3 bits (8 levels) and EVA has 17.
static int s_tevK = -1; // -1: REPLACE; else the constant alpha currently loaded
static void gxDsBTevReplace()
{
	if (s_tevK == -1) return;
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	s_tevK = -1;
}
static inline u8 gxDsBAlphaK(int eva) { return (u8)(eva >= 16 ? 255 : eva * 16); }
static void gxDsBTevAlpha(int k)
{
	if (s_tevK == k) return;
	if (s_tevK == -1) {
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_A0, GX_CA_ZERO);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	GX_SetTevColor(GX_TEVREG0, (GXColor){ 0, 0, 0, (u8)k });
	s_tevK = k;
}

static void gxDsBDrawBgQuad(const GxDsBBgPlaneCache &pcc, const GxDsBBandRegs &r, int bg, int y0, int y1)
{
	GxDsBBgPlaneCache &pc = const_cast<GxDsBBgPlaneCache &>(pcc);
	if (!pc.valid) return;
	f32 s0 = (f32)r.hofs[bg] / pc.wpx;
	f32 s1 = s0 + (f32)kDsBScreenW / pc.wpx;
	f32 t0 = (f32)(r.vofs[bg] + y0) / pc.hpx;
	f32 t1 = (f32)(r.vofs[bg] + y1) / pc.hpx;
	gxDsBDrawQuad(&pc.texObj, 0, (f32)y0, (f32)kDsBScreenW, (f32)y1, s0, t0, s1, t1);
}

// Affine BG band draw. Band [y0,y1) starts at reference point (X0,Y0) -- the
// CPU's running latch value at line y0, recorded per band by Stage 0 -- and
// GPU.cpp's per-pixel/per-line walk is
//   U(i, r) = X0 + i*PA + r*PB     V(i, r) = Y0 + i*PC + r*PD   (8.8 fixed)
// with i the screen column and r the line offset within the band; texel =
// (floor(U/256), floor(V/256)), wrapped by `& (size-1)` if BGxCNT's overflow
// bit is set, skipped (transparent) otherwise. The 4 band corners are (i,r) =
// (0,0) (256,0) (256,H) (0,H).
static void gxDsBDrawAffineBgQuad(const GxDsBBgPlaneCache &pcc, const GxDsBBandRegs &r, int bg, int y0, int y1)
{
	GxDsBBgPlaneCache &pc = const_cast<GxDsBBgPlaneCache &>(pcc);
	if (!pc.valid) return;
	const int w = bg - 2;
	// ROTOCOORD's Integer field is 20 bits above 8 fraction bits (a 28-bit
	// signed fixed-point value); the top 4 bits are ignored by GPU.cpp.
	s32 X0 = (s32)((u32)r.affX[w] << 4) >> 4;
	s32 Y0 = (s32)((u32)r.affY[w] << 4) >> 4;
	const s32 pa = r.affPA[w], pb = r.affPB[w], pcm = r.affPC[w], pd = r.affPD[w];
	if (pc.cfgWrap) {
		// Wrapping: only the position modulo the map size matters, and the
		// wrapped value keeps the float UVs small (GX_REPEAT with a huge
		// coordinate would burn precision). Exact, since the CPU masks the
		// integer part after summing and adding a multiple of the size to the
		// integer part changes nothing.
		X0 = (X0 & 0xFF) | ((((X0 >> 8) & (pc.wpx - 1))) << 8);
		Y0 = (Y0 & 0xFF) | ((((Y0 >> 8) & (pc.hpx - 1))) << 8);
	}
	const s32 W = kDsBScreenW, H = y1 - y0;
	const f32 off = pc.cfgWrap ? 0.0f : 1.0f; // border texel offset
	const f32 nW = (f32)pc.bufW, nH = (f32)pc.bufH;

	f32 u[4], v[4];
	const s32 ci[4] = { 0, W, W, 0 }, ri[4] = { 0, 0, H, H };
	for (int k = 0; k < 4; ++k) {
		u[k] = (gxDsBAffineCorner(X0 + ci[k] * pa + ri[k] * pb, pa, pb) + off) / nW;
		v[k] = (gxDsBAffineCorner(Y0 + ci[k] * pcm + ri[k] * pd, pcm, pd) + off) / nH;
	}
	gxDsBDrawQuadFree(&pc.texObj, 0, (f32)y0, (f32)kDsBScreenW, (f32)y1,
	                  u[0], v[0], u[1], v[1], u[2], v[2], u[3], v[3]);
}

// One affine sprite. Box-relative pixel (col,row) samples texel
//   U = texW/2 + ((col-cx)*PA + (row-cy)*PB)/256 ,  V likewise with PC/PD,
// cx/cy = half the on-screen box -- GPU.cpp's realX/realY start value plus its
// per-pixel `+= dx/dy` (a double-size sprite's box is 2x the texture, which is
// why the centre is the BOX centre while the origin offset is the TEXTURE
// half-size). Texel coordinates are +1 for the baked transparent border.
static void gxDsBDrawAffineObj(const GxDsBObjDraw &d, const GxDsBObjTexSlot &slot)
{
	const s32 cx = d.boxW >> 1, cy = d.boxH >> 1;
	const s32 pa = d.pa, pb = d.pb, pcm = d.pc, pd = d.pd;
	const s32 baseU = (s32)d.w * 128, baseV = (s32)d.h * 128;
	const s32 ci[4] = { 0, d.boxW, d.boxW, 0 }, ri[4] = { 0, 0, d.boxH, d.boxH };
	f32 u[4], v[4];
	for (int k = 0; k < 4; ++k) {
		s32 dc = ci[k] - cx, dr = ri[k] - cy;
		u[k] = (gxDsBAffineCorner(baseU + dc * pa + dr * pb, pa, pb) + 1.0f) / (f32)slot.bufW;
		v[k] = (gxDsBAffineCorner(baseV + dc * pcm + dr * pd, pcm, pd) + 1.0f) / (f32)slot.bufH;
	}
	gxDsBDrawQuadFree(const_cast<GXTexObj *>(&slot.texObj), (f32)d.x, (f32)d.y,
	                  (f32)(d.x + d.boxW), (f32)(d.y + d.boxH),
	                  u[0], v[0], u[1], v[1], u[2], v[2], u[3], v[3]);
}

// One layer (a BG plane) of one band, drawn only into the window rectangles
// whose region enables it, with that region's colour effect applied.
static void gxDsBDrawBgLayer(int bg, const GxDsBBandRegs &r, const GxDsBBandPlan &p, int y0, int y1)
{
	for (int si = 0; si < p.nslab; ++si) {
		const GxDsBSlab &sl = p.slab[si];
		for (int ri = 0; ri < sl.nrun; ++ri) {
			const GxDsBRun &ru = sl.run[ri];
			const GxDsBCfg &cf = p.cfg[ru.cfg];
			if (!((cf.mask >> bg) & 1)) continue;
			bool fade, want;
			gxDsBResolveFx(p, cf, bg, false, fade, want);
			const bool alpha = want && ((p.blendLive[ru.cfg] >> bg) & 1);
			const GxDsBBgPlaneCache &pc = fade ? s_bgPlaneFx[bg] : s_bgPlane[bg];
			if (!pc.valid) continue;
			if (alpha) gxDsBTevAlpha(gxDsBAlphaK(p.eva)); else gxDsBTevReplace();
			GX_SetScissor(ru.x0, sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
			if (r.bgType[bg] == (u8)BGType_Text) gxDsBDrawBgQuad(pc, r, bg, y0, y1);
			else gxDsBDrawAffineBgQuad(pc, r, bg, y0, y1);
		}
	}
}

static void gxDsBDrawObjLayer(int prio, const GxDsBBandPlan &p, int y0, int y1)
{
	// GPU.cpp resolves per-pixel sprite ownership by walking OAM from index
	// 127 down to 0 with a `prio <= prioTab[x]` test, so within one priority
	// tier the LOWER OAM index wins. Painter's algorithm reproduces that by
	// drawing high index first and letting low index overwrite.
	for (int i = s_objDrawCount - 1; i >= 0; --i) {
		const GxDsBObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.boxH <= y0) continue;
		for (int si = 0; si < p.nslab; ++si) {
			const GxDsBSlab &sl = p.slab[si];
			if (d.y >= sl.y1 || d.y + d.boxH <= sl.y0) continue;
			for (int ri = 0; ri < sl.nrun; ++ri) {
				const GxDsBRun &ru = sl.run[ri];
				if (d.x >= ru.x1 || d.x + d.boxW <= ru.x0) continue;
				const GxDsBCfg &cf = p.cfg[ru.cfg];
				if (!((cf.mask >> 4) & 1)) continue;
				bool fade, want;
				gxDsBResolveFx(p, cf, 4, d.semi, fade, want);
				const bool alpha = want && ((p.blendLive[ru.cfg] >> (4 + prio)) & 1);
				const GxDsBObjTexSlot &slot = fade ? s_objTexFx[d.oamIndex] : s_objTex[d.oamIndex];
				if (!slot.valid) continue;
				if (alpha) gxDsBTevAlpha(gxDsBAlphaK(p.eva)); else gxDsBTevReplace();
				GX_SetScissor(ru.x0, sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
				if (d.affine) {
					gxDsBDrawAffineObj(d, slot);
					continue;
				}
				f32 s0 = d.hflip ? 1.0f : 0.0f, s1 = d.hflip ? 0.0f : 1.0f;
				f32 t0 = d.vflip ? 1.0f : 0.0f, t1 = d.vflip ? 0.0f : 1.0f;
				gxDsBDrawQuad(const_cast<GXTexObj *>(&slot.texObj), (f32)d.x, (f32)d.y,
				              (f32)(d.x + d.w), (f32)(d.y + d.h), s0, t0, s1, t1);
			}
		}
	}
}

// Bake-if-stale for one BG plane variant (plain, or the brighten/darken one).
static void gxDsBEnsureBgPlane(int bg, GxDsBBgPlaneCache &pc, int fadeMode, int fadeLvl,
                               bool bankChanged, bool vramDirty, bool palDirty)
{
	GPU *gpu = SubScreen.gpu;
	const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
	const BGType type = gpu->BGTypes[bg];
	const bool affine = (type != BGType_Text);
	u32 auxBase = 0;
	if (type == BGType_Large8bpp) auxBase = gpu->BG_bmp_large_ram[bg];
	else if (type == BGType_AffineExt_256x1 || type == BGType_AffineExt_Direct) auxBase = gpu->BG_bmp_ram[bg];
	const u16 fx = (u16)((fadeMode << 8) | fadeLvl);
	bool needBake = !pc.valid || bankChanged || vramDirty || palDirty || pc.cfgFx != fx ||
	                pc.wpx != (u16)gpu->BGSize[bg][0] || pc.hpx != (u16)gpu->BGSize[bg][1] ||
	                pc.cfgTileBase != gpu->BG_tile_ram[bg] || pc.cfgMapBase != gpu->BG_map_ram[bg] ||
	                pc.cfgColorMode != (u8)(cnt.Palette_256 ? 1 : 0) ||
	                pc.cfgScreenSize != (u8)cnt.ScreenSize ||
	                pc.cfgType != (u8)type || pc.cfgExtPal != gxDsBExtPalFor(bg) ||
	                (affine && (pc.cfgWrap != (u8)(cnt.PaletteSet_Wrap ? 1 : 0) || pc.cfgAuxBase != auxBase));
	if (!needBake) return;
	s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
	if (affine) gxDsBBakeAffineBgPlane(bg, pc);
	else gxDsBBakeBgPlane(bg, pc);
	s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
}

static void gxDsBEnsureObjTex(const GxDsBObjDraw &d, GxDsBObjTexSlot &slot, int fadeMode, int fadeLvl,
                              bool anyDirty)
{
	GPU *gpu = SubScreen.gpu;
	const u8 oneDim = (u8)((gpu->spriteRenderMode == GPU::SPRITE_1D) ? 1 : 0);
	const u16 fx = (u16)((fadeMode << 8) | fadeLvl);
	bool needBake = !slot.valid || anyDirty || slot.cfgFx != fx ||
	                slot.w != d.w || slot.h != d.h ||
	                slot.cfgTileIndex != d.tileIndex ||
	                slot.cfgPalIndex != d.palIndex ||
	                slot.cfgDepth != (u8)(d.depth ? 1 : 0) ||
	                slot.cfgOneDim != oneDim ||
	                slot.cfgBoundary != gpu->sprBoundary ||
	                slot.cfgBordered != (u8)(d.affine ? 1 : 0);
	if (!needBake) return;
	s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
	gxDsBBakeObjTexture(d, slot);
	s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
}

bool gxDsEngineBRenderFrame()
{
	// Whatever happens below, this frame's band trace is finished with here;
	// the next frame re-arms it from its own scanline 0.
	//
	// The VRAM/palette/OAM dirty bitmaps are deliberately NOT part of that
	// unconditional reset: they are cleared only on the success path at the
	// very bottom of this function. Clearing them on a bail would drop the
	// record of writes that no bake has consumed yet, leaving a cached
	// texture stale for the next frame that IS in scope. Retaining them
	// across a bail only ever costs a redundant re-bake later, never a
	// missed one -- the same "when in doubt, widen" rule
	// gx_gba_render.cpp's dependency ranges follow.
	struct BandResetter {
		~BandResetter() {
			g_dsBFramePlan.bands.beginFrame();
			s_frameSawLine0 = false;
			s_frameOutOfScope = true;
		}
	} bandResetter;

#ifdef DSB_FORCE_CPU
	return false;
#endif
	if (gameInfo.isGBA)
		return false;
#ifdef DSB_STATS
	bool statsOk = false;
	s_dsbFrWin = s_dsbFrFade = s_dsbFrAlpha = s_dsbFrSemi = s_dsbFrMb = false;
	struct StatsEmit {
		bool *ok;
		~StatsEmit() {
			if (*ok) {
				++s_dsbOk;
				s_dsbWinOk += s_dsbFrWin; s_dsbFadeOk += s_dsbFrFade; s_dsbAlphaOk += s_dsbFrAlpha;
				s_dsbSemiOk += s_dsbFrSemi; s_dsbMbOk += s_dsbFrMb;
			} else ++s_dsbBail;
			if (((s_dsbOk + s_dsbBail) % 30) == 0) {
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
				harness_profile_emitf("dsb n=%u ok=%u bail=%u affbg=%u affobj=%u win=%u fade=%u alpha=%u semi=%u mb=%u dispcnt=%08x types=%04x why=%s",
				                      (unsigned)(s_dsbOk + s_dsbBail), (unsigned)s_dsbOk, (unsigned)s_dsbBail,
				                      (unsigned)s_dsbAffOk, (unsigned)s_dsbAffObjOk,
				                      (unsigned)s_dsbWinOk, (unsigned)s_dsbFadeOk, (unsigned)s_dsbAlphaOk,
				                      (unsigned)s_dsbSemiOk, (unsigned)s_dsbMbOk,
				                      (unsigned)s_dsbDispcnt, (unsigned)s_dsbTypes, s_dsbWhy);
#endif
			}
		}
	} statsEmit = { &statsOk };
#endif
	if (!s_frameSawLine0 || s_frameOutOfScope)
		return false;
	if (!SubScreen.gpu)
		return false;
	if (!gxDsEngineBRenderInit())
		return false;
	if (vidmutex == LWP_MUTEX_NULL)
		return false;

	// A frame with more layout changes than GxBandTracker can record would
	// have its excess boundaries merged into the last band (gx_frameplan.cpp
	// documents that as a safe degrade for the GBA's rarer case). For a DS
	// engine, per-scanline HDMA scroll is common enough that silently
	// merging would mean visibly wrong output, so bail instead.
	if (g_dsBFramePlan.bands.boundaryCount() >= GxBandTracker::kMaxBoundaries) {
		DSB_WHY("bandoverflow");
		return false;
	}

	if (!gxDsBCollectVisibleObj()) {
		DSB_WHY("obj");
		return false;
	}

	int bandStarts[GxBandTracker::kMaxBands], bandEnds[GxBandTracker::kMaxBands];
	int bandCount = g_dsBFramePlan.bands.finalize(kDsBScreenH, bandStarts, bandEnds);

	GPU *gpu = SubScreen.gpu;
	bool bgEnabledAnyBand[4] = { false, false, false, false };
	bool objEnabledAnyBand = false;
	for (int b = 0; b < bandCount; ++b) {
		for (int bg = 0; bg < 4; ++bg)
			if ((g_dsBBandRegs[b].bgEnable >> bg) & 1) {
				bgEnabledAnyBand[bg] = true;
				// A plane is baked once, from the frame-final BGxCNT/mode. A
				// BG whose type changed between bands (a mid-frame BG_Mode
				// switch) can't be replayed from that one bake -> bail.
				if (g_dsBBandRegs[b].bgType[bg] != (u8)gpu->BGTypes[bg]) {
					DSB_WHY("bgtypechange");
					return false;
				}
			}
		if (g_dsBBandRegs[b].objEnable) objEnabledAnyBand = true;
	}

	// Task 9: plan + validate every band before any GX work or cache mutation,
	// so a bail is always clean. Brighten/darken planes/sprites are baked into a
	// single per-frame variant (one (mode,level) key), so bands needing
	// different fade keys for layers bail.
	u8 fadeBgAny = 0, fadeObjAny = 0;
	int fadeMode = 0, fadeLvl = 0;
	for (int b = 0; b < bandCount; ++b) {
		if (!gxDsBPlanBand(g_dsBBandRegs[b], bandStarts[b], bandEnds[b], s_bandPlan[b]))
			return false;
		const GxDsBBandPlan &pl = s_bandPlan[b];
		if (pl.fadeBg || pl.fadeObj) {
			if (fadeMode && (fadeMode != pl.mode || fadeLvl != pl.evy)) { DSB_WHY("fadekeys"); return false; }
			fadeMode = pl.mode; fadeLvl = pl.evy;
			fadeBgAny |= pl.fadeBg; fadeObjAny |= pl.fadeObj;
		}
	}
	// A fade/alpha item is only ever drawn if its layer is enabled in the band
	// (gxDsBPlanBand walked enabled layers only), so no extra gating is needed.

	// Stage 1. Coarse gate (see the header): any Engine-B-visible VRAM
	// write, any Engine-B palette write, any Engine-B OAM write, or a
	// VRAMCNT bank remap re-bakes everything. `force` also covers the very
	// first frame, where nothing is baked yet.
	bool bankChanged = gxDsBBankMapChanged();
	bool palDirty = g_dsBFramePlan.palette.anyDirty();
	bool vramDirty = g_dsBFramePlan.vram.anyDirty();
	bool oamDirty = g_dsBFramePlan.oam.anyDirty();
	const bool anyBgDirty = bankChanged || vramDirty || palDirty;

	for (int b = 0; b < bandCount; ++b)
		gxDsBBakeBackdrop(b, s_bandPlan[b].backdropFade ? s_bandPlan[b].mode : 0, s_bandPlan[b].evy);

	for (int bg = 0; bg < 4; ++bg) {
		// A plane/variant that is not (re)baked this frame while its inputs changed
		// must not survive to a later frame that needs it: the dirty flags are
		// cleared below, so a stale `valid` would otherwise be trusted forever.
		if (bgEnabledAnyBand[bg]) gxDsBEnsureBgPlane(bg, s_bgPlane[bg], 0, 0, bankChanged, vramDirty, palDirty);
		else if (anyBgDirty) s_bgPlane[bg].valid = false;
		if (bgEnabledAnyBand[bg] && ((fadeBgAny >> bg) & 1))
			gxDsBEnsureBgPlane(bg, s_bgPlaneFx[bg], fadeMode, fadeLvl, bankChanged, vramDirty, palDirty);
		else if (anyBgDirty) s_bgPlaneFx[bg].valid = false;
	}
	if (objEnabledAnyBand) {
		const bool anyObjDirty = bankChanged || vramDirty || palDirty || oamDirty;
		bool visible[128] = { false };
		for (int i = 0; i < s_objDrawCount; ++i) {
			const GxDsBObjDraw &d = s_objDraws[i];
			visible[d.oamIndex] = true;
			gxDsBEnsureObjTex(d, s_objTex[d.oamIndex], 0, 0, anyObjDirty);
			if (fadeObjAny) gxDsBEnsureObjTex(d, s_objTexFx[d.oamIndex], fadeMode, fadeLvl, anyObjDirty);
			else if (anyObjDirty) s_objTexFx[d.oamIndex].valid = false;
		}
		if (anyObjDirty)
			for (int i = 0; i < 128; ++i)
				if (!visible[i]) { s_objTex[i].valid = false; s_objTexFx[i].valid = false; }
	} else if (bankChanged || vramDirty || palDirty || oamDirty) {
		for (int i = 0; i < 128; ++i) { s_objTex[i].valid = false; s_objTexFx[i].valid = false; }
	}

	// ---- Stage 4 + EFB copy, under vidmutex ----------------------------
	// draw_thread (main.cpp) pushes its own present quads into this same GX FIFO
	// from the video thread and is already serialized against Draw() by this
	// mutex; taking it here extends that same contract to cover Engine B's
	// composite pass rather than inventing a new one.
	LWP_MutexLock(vidmutex);

	gxDsBSetup2DState();
	s_tevK = -1;
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);

	for (int b = 0; b < bandCount; ++b) {
		const GxDsBBandRegs &r = g_dsBBandRegs[b];
		const GxDsBBandPlan &pl = s_bandPlan[b];
		const int y0 = bandStarts[b], y1 = bandEnds[b];
		GX_SetScissor(0, y0, kDsBScreenW, y1 - y0);
		gxDsBTevReplace();

		// Backdrop first (always drawn, never windowed), then ascending priority
		// (3 = furthest back) with OBJ of a tier drawn above the BGs of that same
		// tier and BG3..BG0 within a tier -- exactly GPU_RenderLine_layer()'s own
		// order (itemsForPriority[].BGs is filled by GPU_resortBGs() walking
		// i = 3..0, and the sprite composite runs after the BG loop).
		gxDsBDrawQuad(&s_backdropTexObj[b], 0, (f32)y0, (f32)kDsBScreenW, (f32)y1, 0, 0, 1, 1);

		for (int prio = 3; prio >= 0; --prio) {
			for (int bg = 3; bg >= 0; --bg) {
				if (!((r.bgEnable >> bg) & 1)) continue;
				if (r.bgPrio[bg] != prio) continue;
				gxDsBDrawBgLayer(bg, r, pl, y0, y1);
			}
			if (r.objEnable)
				gxDsBDrawObjLayer(prio, pl, y0, y1);
		}
	}
	gxDsBTevReplace();

	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);
	GX_DrawDone();
	// Deflicker OFF for this copy. main.cpp's InitVideo() leaves
	// GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE,
	// rmode->vfilter) in force for the TV present; that vertical filter
	// blends each output row with its neighbours, which is right for a
	// 480-line video signal and wrong for an EFB->texture readback of an
	// emulated 256x192 framebuffer that has to come back 1:1.
	// Honest note: toggling this alone changed nothing in the Dolphin A/B
	// that motivated looking at it (Dolphin does not appear to model the
	// copy filter on texture copies), so it is kept as correctness insurance
	// for real hardware rather than as a fix for anything observed here --
	// the pixel difference that A/B actually found was GX_InitTexObj's
	// default GX_LINEAR filtering, fixed at every bake site instead.
	// GxRestorePresentState() puts rmode's real filter back before
	// draw_thread's next GX_CopyDisp().
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetTexCopySrc(0, 0, kDsBScreenW, kDsBScreenH);
	GX_SetTexCopyDst(kDsBScreenW, kDsBScreenH, GX_TF_RGB5A3, GX_FALSE);
	// clear=GX_TRUE (task 12): wipe the 256x192 EFB corner just drawn into so it
	// does not ghost under draw_thread's present (visible in the live window).
	GX_CopyTex(s_copyBackBuf, GX_TRUE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	// Put back everything gxDsBSetup2DState() + the copy-src/dst setup above
	// changed, before draw_thread's next present pass can observe it.
	GxRestorePresentState();

	LWP_MutexUnlock(vidmutex);

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxUnswizzle16bpp((const u16 *)s_copyBackBuf, s_copyBackLinear, blk.texelsWide, blk.texelsTall, kDsBScreenW, kDsBScreenH);

	// Readback + MASTER_BRIGHT. GPU_RenderLine_MasterBrightness() runs on the
	// finished 15-bit line, so applying the same per-channel tables here (with the
	// per-line mode/factor sampled by Stage 0) is exact -- no GX pass, no rounding.
	u16 *dst = (u16 *)(GPU_screen + (u32)SubScreen.offset * 512);
	bool anyMb = false;
	for (int y = 0; y < kDsBScreenH; ++y) {
		const u16 *src = s_copyBackLinear + (u32)y * kDsBScreenW;
		u16 *out = dst + (u32)y * kDsBScreenW;
		const int mode = s_mbMode[y], fac = s_mbFac[y];
		if (mode == 0 || fac == 0) {
			for (int x = 0; x < kDsBScreenW; ++x)
				out[x] = gxDsBRgb5a3ToNds(src[x]);
			continue;
		}
		anyMb = true;
		if (fac == 16) {
			const u16 v = (mode == 1) ? 0x7FFF : 0;
			for (int x = 0; x < kDsBScreenW; ++x) out[x] = v;
			continue;
		}
		for (int x = 0; x < kDsBScreenW; ++x)
			out[x] = gxDsBFadeColor(gxDsBRgb5a3ToNds(src[x]) & 0x7FFF, mode + 1 /* 1->2 up, 2->3 down */, fac);
	}

	// Success path only (see BandResetter's comment): every write recorded
	// this frame has now been consumed by a bake, so the trace can start
	// clean. Writes landing after this point -- i.e. during lines 192-262's
	// VBlank -- accumulate into the next frame's trace, which is exactly
	// where they belong.
	g_dsBFramePlan.vram.clear();
	g_dsBFramePlan.palette.clear();
	g_dsBFramePlan.oam.clear();

#ifdef DSB_STATS
	statsOk = true;
	for (int bg = 0; bg < 4; ++bg)
		if (bgEnabledAnyBand[bg] && gpu->BGTypes[bg] != BGType_Text) { ++s_dsbAffOk; break; }
	for (int i = 0; i < s_objDrawCount; ++i)
		if (s_objDraws[i].affine) { ++s_dsbAffObjOk; break; }
	for (int b = 0; b < bandCount; ++b) {
		const GxDsBBandPlan &pl = s_bandPlan[b];
		if (pl.windows) s_dsbFrWin = true;
		if (pl.fadeBg || pl.fadeObj || pl.backdropFade) s_dsbFrFade = true;
		if (pl.blendLive[0] | pl.blendLive[1] | pl.blendLive[2]) s_dsbFrAlpha = true;
	}
	for (int i = 0; i < s_objDrawCount; ++i)
		if (s_objDraws[i].semi) { s_dsbFrSemi = true; break; }
	s_dsbFrMb = anyMb;
#endif
	(void)anyMb;
	return true;
}
