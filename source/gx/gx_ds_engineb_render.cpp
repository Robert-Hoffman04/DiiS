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
static inline u16 gxDsBOpaqueTexel(u16 ndsColor) { return gxPackRGB5A3Opaque(gxExtractBGR555(ndsColor)); }
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
	if (s_bankMapCopyValid && memcmp(cur, s_bankMapCopy, sizeof(cur)) == 0)
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
};
static GxDsBBgPlaneCache s_bgPlane[4];
static const int kDsBBgPlaneMaxPx = 512; // text BG max (BGSize table, GPU.cpp sizeTab row 1)
static u16 *s_bgBakeScratch;

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
};
static GxDsBObjTexSlot s_objTex[128];
static const int kDsBObjMaxPx = 64;
static u16 *s_objBakeScratch;

static GXTexObj s_backdropTexObj;
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

	s_bgBakeScratch = (u16 *)malloc((u32)kDsBBgPlaneMaxPx * kDsBBgPlaneMaxPx * sizeof(u16));
	if (!s_bgBakeScratch) return false;
	s_objBakeScratch = (u16 *)malloc((u32)kDsBObjMaxPx * kDsBObjMaxPx * sizeof(u16));
	if (!s_objBakeScratch) return false;

	for (int i = 0; i < 4; ++i) {
		s_bgPlane[i].texData = NULL;
		s_bgPlane[i].texDataCap = 0;
		s_bgPlane[i].valid = false;
	}
	for (int i = 0; i < 128; ++i) {
		s_objTex[i].texData = NULL;
		s_objTex[i].texDataCap = 0;
		s_objTex[i].valid = false;
	}

	s_backdropTexData = memalign(32, 32); // GX minimum texture allocation granularity
	if (!s_backdropTexData) return false;
	memset(s_backdropTexData, 0, 32);
	GX_InitTexObj(&s_backdropTexObj, s_backdropTexData, 1, 1, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_backdropTexObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane

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
	for (int i = 0; i < 4; ++i) { free(s_bgPlane[i].texData); s_bgPlane[i].texData = NULL; s_bgPlane[i].texDataCap = 0; s_bgPlane[i].valid = false; }
	for (int i = 0; i < 128; ++i) { free(s_objTex[i].texData); s_objTex[i].texData = NULL; s_objTex[i].texDataCap = 0; s_objTex[i].valid = false; }
	free(s_bgBakeScratch); s_bgBakeScratch = NULL;
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
static void gxDsBSnapshotBandRegs(GPU *gpu, GxDsBBandRegs *r)
{
	const _DISPCNT &d = gpu->dispx_st->dispx_DISPCNT.bits;
	r->bgEnable = (u8)((d.BG0_Enable) | (d.BG1_Enable << 1) | (d.BG2_Enable << 2) | (d.BG3_Enable << 3));
	r->objEnable = (u8)d.OBJ_Enable;
	for (int i = 0; i < 4; ++i) {
		r->bgPrio[i] = (u8)gpu->dispx_st->dispx_BGxCNT[i].bits.Priority;
		r->hofs[i] = (u16)gpu->getHOFS(i);
		r->vofs[i] = (u16)gpu->getVOFS(i);
	}
}

// Everything this file can't reproduce natively. Checked per scanline (not
// once per frame) precisely because a game can turn any of these on
// mid-frame -- a frame-end-only check would silently render the first half
// of such a frame wrong.
static bool gxDsBScanlineOutOfScope(GPU *gpu)
{
	const _DISPCNT &d = gpu->dispx_st->dispx_DISPCNT.bits;

	if (gpu->dispMode != 1) return true;   // 0 = white; 2/3 are Engine-A-only
	if (d.BG_Mode != 0) return true;         // text BGs only (GPU_mode2type row 0)
	if (d.ForceBlank) return true;
	if (d.ExBGxPalette_Enable || d.ExOBJPalette_Enable) return true;
	if (gpu->WIN0_ENABLED || gpu->WIN1_ENABLED || gpu->WINOBJ_ENABLED) return true;
	if (((gpu->BLDCNT >> 6) & 3) != 0) return true;
	if (gpu->MasterBrightMode != 0 && gpu->MasterBrightFactor != 0) return true;
	if (!CommonSettings.showGpu.screens[GPU_SUB]) return true;

	// Mosaic: only matters when a layer actually enables it. Checked on the
	// BG side here (the OBJ side is checked per sprite in
	// gxDsBCollectVisibleObj, where the OAM Mosaic bit lives).
	for (int bg = 0; bg < 4; ++bg) {
		bool enabled = (bg == 0) ? d.BG0_Enable : (bg == 1) ? d.BG1_Enable :
		               (bg == 2) ? d.BG2_Enable : d.BG3_Enable;
		if (!enabled) continue;
		if (gpu->dispx_st->dispx_BGxCNT[bg].bits.Mosaic_Enable) return true;
		if (gpu->BGTypes[bg] != BGType_Text) return true;
		// GPU_resortBGs()'s `dispLayers OP enable` idiom inverts visibility
		// for a *hidden* layer; rather than reproduce that debug-only quirk,
		// bail whenever the user has toggled any Engine-B layer off.
		if (!CommonSettings.dispLayers[GPU_SUB][bg]) return true;
	}
	if (d.OBJ_Enable && !CommonSettings.dispLayers[GPU_SUB][4]) return true;

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
	} else if (!s_frameSawLine0) {
		// Mid-frame entry (e.g. right after a savestate load): no frame-start
		// snapshot exists, so this frame can't be replayed. Next frame's
		// line 0 re-arms it.
		s_frameOutOfScope = true;
		return;
	}

	if (gxDsBScanlineOutOfScope(gpu)) {
		s_frameOutOfScope = true;
		return;
	}
	if (s_frameOutOfScope)
		return; // already failed this frame; nothing to gain from banding it

	GxDsBBandRegs cur;
	gxDsBSnapshotBandRegs(gpu, &cur);
	if (line == 0) {
		g_dsBBandRegs[0] = cur;
		s_lastBandRegs = cur;
		return;
	}
	if (memcmp(&cur, &s_lastBandRegs, sizeof(cur)) == 0)
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

// ---------------------------------------------------------------------
// Stage 1: bakes
// ---------------------------------------------------------------------
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
static void gxDsBBakeBgPlane(int bg)
{
	GPU *gpu = SubScreen.gpu;
	const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
	const int lg = (int)gpu->BGSize[bg][0];
	const int ht = (int)gpu->BGSize[bg][1];
	const u32 tileBase = gpu->BG_tile_ram[bg];
	const u32 mapBase = gpu->BG_map_ram[bg];
	const bool c256 = cnt.Palette_256 != 0;
	const int screenSize = cnt.ScreenSize;
	u8 *const pal = MMU.ARM9_VMEM + kDsBBgPalOff;
	const int tileBytes = c256 ? 0x40 : 0x20;
	const int rowBytes = c256 ? 8 : 4;

	GxDsBBgPlaneCache &pc = s_bgPlane[bg];
	pc.valid = false;
	if (lg <= 0 || ht <= 0 || lg > kDsBBgPlaneMaxPx || ht > kDsBBgPlaneMaxPx)
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
						int palIdx = c256 ? idx : (palNum * 16 + idx);
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
	pc.cfgTileBase = tileBase;
	pc.cfgMapBase = mapBase;
	pc.cfgColorMode = (u8)(c256 ? 1 : 0);
	pc.cfgScreenSize = (u8)screenSize;
	pc.valid = true;
}

struct GxDsBObjDraw {
	s16 x, y;
	u16 w, h;
	u8 priority;
	bool hflip, vflip;
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

		if (rotScale != 0) return false;
		// Mode 2 (OBJ window) sprites only ever write GPU.cpp's sprWin[]
		// mask, which is read exclusively when WINOBJ_ENABLED -- and this
		// file already bails the frame for that (gxDsBScanlineOutOfScope).
		// With the OBJ window off they draw no pixels and take no part in
		// prioTab, so skipping them is exact, not a shortcut.
		if (mode == 2) continue;
		// Mode 1 (semi-transparent) blends against the layer beneath
		// whenever BLDCNT's 2nd-target bits select it, INDEPENDENTLY of
		// BLDCNT's effect field (GPU.cpp's _master_setFinalOBJColor tests
		// `type == GPU_OBJ_MODE_Transparent` outside the FUNC switch), so
		// the frame's "no colour effect" check does not cover it.
		// Mode 3 is bitmap OBJ. Both are out of scope.
		if (mode != 0) return false;
		if ((a0 >> 12) & 1) return false;

		GxDsBObjDraw &d = s_objDraws[s_objDrawCount++];
		d.x = (s16)sprX;
		d.y = (s16)sprY;
		d.w = (u16)w;
		d.h = (u16)h;
		d.priority = (u8)((a2 >> 10) & 3);
		d.hflip = ((a1 >> 12) & 1) != 0;
		d.vflip = ((a1 >> 13) & 1) != 0;
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
static void gxDsBBakeObjTexture(const GxDsBObjDraw &d)
{
	GPU *gpu = SubScreen.gpu;
	const bool oneDim = (gpu->spriteRenderMode == GPU::SPRITE_1D);
	const int rowBytes = d.depth ? 8 : 4;
	u8 *const pal = MMU.ARM9_VMEM + kDsBObjPalOff;

	GxDsBObjTexSlot &slot = s_objTex[d.oamIndex];
	slot.valid = false;

	for (int y = 0; y < d.h; ++y) {
		u32 addr;
		if (oneDim)
			addr = gpu->sprMem + ((u32)d.tileIndex << gpu->sprBoundary)
			       + (u32)(y >> 3) * (u32)d.w * rowBytes + (u32)(y & 7) * rowBytes;
		else
			addr = gpu->sprMem + ((u32)d.tileIndex << 5)
			       + ((u32)(y >> 3) << 10) + (u32)(y & 7) * rowBytes;
		const u8 *src = (const u8 *)MMU_gpu_map(addr);
		u16 *out = s_objBakeScratch + (u32)y * d.w;
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

	u32 needed = (u32)d.w * d.h * sizeof(u16);
	if (!gxDsBEnsureCap(&slot.texData, &slot.texDataCap, needed))
		return;

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_objBakeScratch, (u16 *)slot.texData, blk.texelsWide, blk.texelsTall, d.w, d.h);
	DCFlushRange(slot.texData, needed);
	GX_InitTexObj(&slot.texObj, slot.texData, d.w, d.h, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&slot.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
	slot.w = d.w;
	slot.h = d.h;
	slot.cfgTileIndex = d.tileIndex;
	slot.cfgPalIndex = d.palIndex;
	slot.cfgDepth = (u8)(d.depth ? 1 : 0);
	slot.cfgOneDim = (u8)(oneDim ? 1 : 0);
	slot.cfgBoundary = gpu->sprBoundary;
	slot.valid = true;
}

static void gxDsBBakeBackdrop()
{
	// GPU_RenderLine_layer(): backdrop colour is BG palette entry 0 of this
	// engine's own 1KB palette half, masked to 15 bits.
	u16 c = (u16)(T1ReadWord(MMU.ARM9_VMEM, kDsBBgPalOff) & 0x7FFF);
	((u16 *)s_backdropTexData)[0] = gxDsBOpaqueTexel(c);
	DCFlushRange(s_backdropTexData, 32);
}

// ---------------------------------------------------------------------
// Stage 4: per-band draw
// ---------------------------------------------------------------------
static void gxDsBDrawBgQuad(int bg, const GxDsBBandRegs &r, int y0, int y1)
{
	GxDsBBgPlaneCache &pc = s_bgPlane[bg];
	if (!pc.valid) return;
	f32 s0 = (f32)r.hofs[bg] / pc.wpx;
	f32 s1 = s0 + (f32)kDsBScreenW / pc.wpx;
	f32 t0 = (f32)(r.vofs[bg] + y0) / pc.hpx;
	f32 t1 = (f32)(r.vofs[bg] + y1) / pc.hpx;
	gxDsBDrawQuad(&pc.texObj, 0, (f32)y0, (f32)kDsBScreenW, (f32)y1, s0, t0, s1, t1);
}

static void gxDsBDrawObjLayer(int prio, int y0, int y1)
{
	// GPU.cpp resolves per-pixel sprite ownership by walking OAM from index
	// 127 down to 0 with a `prio <= prioTab[x]` test, so within one priority
	// tier the LOWER OAM index wins. Painter's algorithm reproduces that by
	// drawing high index first and letting low index overwrite.
	for (int i = s_objDrawCount - 1; i >= 0; --i) {
		const GxDsBObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.h <= y0) continue;
		GxDsBObjTexSlot &slot = s_objTex[d.oamIndex];
		if (!slot.valid) continue;
		f32 s0 = d.hflip ? 1.0f : 0.0f, s1 = d.hflip ? 0.0f : 1.0f;
		f32 t0 = d.vflip ? 1.0f : 0.0f, t1 = d.vflip ? 0.0f : 1.0f;
		gxDsBDrawQuad(&slot.texObj, (f32)d.x, (f32)d.y,
		              (f32)(d.x + d.w), (f32)(d.y + d.h), s0, t0, s1, t1);
	}
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

	if (gameInfo.isGBA)
		return false;
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
	if (g_dsBFramePlan.bands.boundaryCount() >= GxBandTracker::kMaxBoundaries)
		return false;

	if (!gxDsBCollectVisibleObj())
		return false;

	int bandStarts[GxBandTracker::kMaxBands], bandEnds[GxBandTracker::kMaxBands];
	int bandCount = g_dsBFramePlan.bands.finalize(kDsBScreenH, bandStarts, bandEnds);

	// Stage 1. Coarse gate (see the header): any Engine-B-visible VRAM
	// write, any Engine-B palette write, any Engine-B OAM write, or a
	// VRAMCNT bank remap re-bakes everything. `force` also covers the very
	// first frame, where nothing is baked yet.
	bool bankChanged = gxDsBBankMapChanged();
	bool palDirty = g_dsBFramePlan.palette.anyDirty();
	bool vramDirty = g_dsBFramePlan.vram.anyDirty();
	bool oamDirty = g_dsBFramePlan.oam.anyDirty();

	gxDsBBakeBackdrop(); // 1 texel; cheaper than deciding whether to skip it

	GPU *gpu = SubScreen.gpu;
	const _DISPCNT &dcFinal = gpu->dispx_st->dispx_DISPCNT.bits;
	bool bgEnabledAnyBand[4] = { false, false, false, false };
	bool objEnabledAnyBand = false;
	for (int b = 0; b < bandCount; ++b) {
		for (int bg = 0; bg < 4; ++bg)
			if ((g_dsBBandRegs[b].bgEnable >> bg) & 1) bgEnabledAnyBand[bg] = true;
		if (g_dsBBandRegs[b].objEnable) objEnabledAnyBand = true;
	}

	for (int bg = 0; bg < 4; ++bg) {
		if (!bgEnabledAnyBand[bg]) continue;
		GxDsBBgPlaneCache &pc = s_bgPlane[bg];
		const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
		bool needBake = !pc.valid || bankChanged || vramDirty || palDirty ||
		                pc.wpx != (u16)gpu->BGSize[bg][0] || pc.hpx != (u16)gpu->BGSize[bg][1] ||
		                pc.cfgTileBase != gpu->BG_tile_ram[bg] || pc.cfgMapBase != gpu->BG_map_ram[bg] ||
		                pc.cfgColorMode != (u8)(cnt.Palette_256 ? 1 : 0) ||
		                pc.cfgScreenSize != (u8)cnt.ScreenSize;
		if (needBake)
			gxDsBBakeBgPlane(bg);
	}
	if (objEnabledAnyBand) {
		const u8 oneDim = (u8)((gpu->spriteRenderMode == GPU::SPRITE_1D) ? 1 : 0);
		for (int i = 0; i < s_objDrawCount; ++i) {
			const GxDsBObjDraw &d = s_objDraws[i];
			GxDsBObjTexSlot &slot = s_objTex[d.oamIndex];
			bool needBake = !slot.valid || bankChanged || vramDirty || palDirty || oamDirty ||
			                slot.w != d.w || slot.h != d.h ||
			                slot.cfgTileIndex != d.tileIndex ||
			                slot.cfgPalIndex != d.palIndex ||
			                slot.cfgDepth != (u8)(d.depth ? 1 : 0) ||
			                slot.cfgOneDim != oneDim ||
			                slot.cfgBoundary != gpu->sprBoundary;
			if (needBake)
				gxDsBBakeObjTexture(d);
		}
	}
	(void)dcFinal;

	// ---- Stage 4 + EFB copy, under vidmutex ----------------------------
	// draw_thread (main.cpp) pushes its own present quads into this same GX
	// FIFO from the video thread and is already serialized against Draw()
	// by this mutex; taking it here extends that same contract to cover
	// Engine B's composite pass rather than inventing a new one.
	LWP_MutexLock(vidmutex);

	gxDsBSetup2DState();
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);

	for (int b = 0; b < bandCount; ++b) {
		const GxDsBBandRegs &r = g_dsBBandRegs[b];
		const int y0 = bandStarts[b], y1 = bandEnds[b];
		GX_SetScissor(0, y0, kDsBScreenW, y1 - y0);

		// Backdrop first, then ascending priority (3 = furthest back) with
		// OBJ of a tier drawn above the BGs of that same tier and BG3..BG0
		// within a tier -- exactly GPU_RenderLine_layer()'s own order
		// (itemsForPriority[].BGs is filled by GPU_resortBGs() walking
		// i = 3..0, and the sprite composite runs after the BG loop).
		gxDsBDrawQuad(&s_backdropTexObj, 0, (f32)y0, (f32)kDsBScreenW, (f32)y1, 0, 0, 1, 1);

		for (int prio = 3; prio >= 0; --prio) {
			for (int bg = 3; bg >= 0; --bg) {
				if (!((r.bgEnable >> bg) & 1)) continue;
				if (r.bgPrio[bg] != prio) continue;
				gxDsBDrawBgQuad(bg, r, y0, y1);
			}
			if (r.objEnable)
				gxDsBDrawObjLayer(prio, y0, y1);
		}
	}

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
	GX_CopyTex(s_copyBackBuf, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	// Put back everything gxDsBSetup2DState() + the copy-src/dst setup above
	// changed, before draw_thread's next present pass can observe it.
	GxRestorePresentState();

	LWP_MutexUnlock(vidmutex);

	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxUnswizzle16bpp((const u16 *)s_copyBackBuf, s_copyBackLinear, blk.texelsWide, blk.texelsTall, kDsBScreenW, kDsBScreenH);

	u16 *dst = (u16 *)(GPU_screen + (u32)SubScreen.offset * 512);
	for (int i = 0; i < kDsBScreenW * kDsBScreenH; ++i)
		dst[i] = gxDsBRgb5a3ToNds(s_copyBackLinear[i]);

	// Success path only (see BandResetter's comment): every write recorded
	// this frame has now been consumed by a bake, so the trace can start
	// clean. Writes landing after this point -- i.e. during lines 192-262's
	// VBlank -- accumulate into the next frame's trace, which is exactly
	// where they belong.
	g_dsBFramePlan.vram.clear();
	g_dsBFramePlan.palette.clear();
	g_dsBFramePlan.oam.clear();

	return true;
}
