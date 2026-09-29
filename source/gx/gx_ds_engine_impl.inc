// ---------------------------------------------------------------------------
// gx_ds_engine_impl.inc -- the DS 2D GX compositor body, compiled TWICE (queue item
// 13, gx-next-steps-log.md "Task 13"): once as Engine B (gx_ds_engineb_render.cpp,
// GXDS_CORE 1, the original tasks 7-11 code) and once as Engine A
// (gx_ds_enginea_render.cpp, GXDS_CORE 0). The two TUs each get their own copy of
// every file-static below (plane / sprite caches, band plan, dirty gates, TLUT owner
// table, EFB readback buffers), which is what makes "band tracking and read-set
// dirty gating per engine" true by construction; the only symbols the two share are
// the GX hardware itself (EFB, the 16 TLUT names -- Engine B owns GX_TLUT11-15,
// Engine A GX_TLUT6-10 -- and the vidmutex-serialized FIFO) and MMU/GPU state.
// The wrapper defines, before including this file:
//   GXDS_CORE   0 = Engine A (MainScreen, DISPCNT A, BG/OBJ VRAM ABG/AOBJ, ExtPal[0])
//               1 = Engine B (SubScreen,  DISPCNT B, BBG/BOBJ, ExtPal[1])
//   GXDS_API(n) the public entry-point name for `n`
//   GXDS_PLAN / GXDS_REGS   the two exported globals (frame plan, band regs)
//   GXDS_TAG    the harness-profile line prefix ("dsa" / "dsb")
//   GXDS_FORCE_CPU   defined when this engine must always bail (test hook)
// Everything Engine-A-specific (3D layer, capture, DISPCNT char/screen base) is under
// `#if GXDS_CORE == 0`, so the Engine B build is the pre-task-13 code plus a
// mid-frame screen-swap bail.
// ---------------------------------------------------------------------------
#include "gx_ds_engineb_render.h"
#include "gx_ds_enginea_render.h"
#include "gx_color.h"
#include "gx_swizzle.h"
#include "gx_texformat.h"
#include "../GPU.h"
#include "../MMU.h"
#if GXDS_CORE == 0
#include "../gfx3d.h"    // gfx3d_convertedScreen / g_gfx3dRenderSeq (Engine A's 3D layer input)
#include "gx_ds_3d_render.h"   // task 13e: GX geometry pass
#endif
#include "gx_ds_present.h"     // Task directpresent
#include "../NDSSystem.h"
#include "../perf_zones.h"
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#if (defined(DSB_STATS) || defined(DSLZ_STATS)) && defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
#include "../harness/harness_profile.h"
#endif
#include "gx_mask.h"
#include "gx_rendermode.h"

// Test hooks (compile-time only, zero cost when undefined):
//  -DDSB_FORCE_CPU : Engine B's RenderFrame always bails, so the CPU compositor
//                    renders every frame. The A/B reference for byte-comparing the GX
//                    path against GPU.cpp. -DDSA_FORCE_CPU is the same for Engine A
//                    (task 13); the two are independent so an A/B build can force
//                    exactly one engine. The remaining hooks apply to both engines.
//  -DDSB_NOCI      : (task 11) every bake uses the pre-task-11 pre-resolved RGB5A3
//                    format instead of native CI4/CI8+TLUT (the always-available
//                    fallback path for multi-slice extended-palette planes), the A/B
//                    reference for the CI conversion and the memory-saving baseline.
//  -DDSB_MUTATE_TLUTSHARE / -DDSB_MUTATE_TLUTNOBIND : (task 11) mutation checks for
//                    the TLUT-clobber hazard. SHARE: every plane and sprite uses ONE
//                    hardware TLUT slot, loaded at bake time only; NOBIND: per-BG /
//                    shared-OBJ slots but loaded at bake time only (no per-draw
//                    ownership check). The clobber fixture must FAIL under both.
//  -DDSB_STATS     : (needs the harness profile sink) counts engaged vs.
//                    bailed frames, the last bail reason and the BG-type mix
//                    of engaged frames, and emits one line per 30 frames.
#ifdef DSB_STATS
static const char *s_dsbWhy = "";
static u32 s_dsbOk = 0, s_dsbBail = 0, s_dsbAffOk = 0, s_dsbAffObjOk = 0, s_dsbDispcnt = 0, s_dsbTypes = 0;
// task 9: engaged-frame counts per feature (window / fade / alpha / semi-transparent OBJ / master brightness)
static u32 s_dsbWinOk = 0, s_dsbFadeOk = 0, s_dsbAlphaOk = 0, s_dsbSemiOk = 0, s_dsbMbOk = 0;
static bool s_dsbFrWin, s_dsbFrFade, s_dsbFrAlpha, s_dsbFrSemi, s_dsbFrMb;
// task 13 (Engine A): engaged frames that drew the 3D layer (bails are visible through the
// last-bail-reason string: "3dtrans" / "3dfade" name the 3D-specific ones).
#ifdef DSB_ABCMP
// -DDSB_ABCMP (needs -DDSB_STATS): in-process A/B. By the time RenderFrame() runs, GPU_screen
// still holds what the CPU compositor rendered for this engine's 192 lines, so the GX result
// can be compared against it on the very same emulated frame (no cross-run non-determinism).
// Per engaged frame: exact (identical), alpha (GX >= CPU, every channel delta <= 2: the
// documented alpha-blend near-exactness), bad (anything else). `mn`/`mp` are the extreme
// per-channel 5-bit deltas (GX - CPU) seen over all frames.
static u32 s_abExact, s_abAlpha, s_abBad, s_abFirstBad;
static int s_abMn, s_abMp;
static u16 s_abCpu[256 * 192];
#endif
static char s_dsbNote[96];   // free-form detail for the last bail (blendunder: the layer/target sets)
static u32 s_dsb3dOk = 0, s_dsb3dEnabled = 0;   // engaged frames drawing the 3D layer / frames with BG0_3D+BG0 enabled (any outcome)
static bool s_dsbFr3d, s_dsb3dSeen;
// bail-reason histogram (why -> frames), emitted as the `<tag>w` line
static struct { const char *why; u32 n; } s_dsbWhyHist[24];
// task 10: bake counters (every real texture bake, per BG plane / per OAM index; the
// *Fx counters are the brighten/darken variants) and, from the SAME run, how many bakes
// the pre-task-10 coarse gate (any Engine-B-visible VRAM write / any palette write /
// any OAM write / bank remap => re-bake every enabled plane and visible sprite) WOULD
// have done, so before/after is measured on identical inputs.
static u32 s_dsbBgBake[4], s_dsbBgFxBake[4], s_dsbObjBake[128], s_dsbObjFxBake[128];
static u32 s_dsbOldBg, s_dsbOldObj, s_dsbStale; // s_dsbStale: -DDSB_VERIFY only (cached texture != fresh bake)
static bool s_dsbOldVramArm; // the old whole-region VRAM flag (BBG/BOBJ page writes + bank H LCDC window)
// task 11: texture memory / format accounting (cumulative bakes, live gauge is computed at emit time)
static u32 s_dsbBakeCi, s_dsbBakeRgb, s_dsbBakeFallback, s_dsbTlutLoads;
static u64 s_dsbBakedBytes, s_dsbBakedBytesRgbEquiv;
static u64 s_dsbBakeTicks;   // emulated-timebase ticks spent inside bakes (Dolphin's PPC timing is approximate: indicative only)
// task 23: palette-only TLUT rebuilds (index texture untouched) counted separately from
// a full bake (s_dsbBakeCi/s_dsbBakeRgb above) -- see gxDsBEnsureBgPlane/gxDsBEnsureObjTex.
static u32 s_dsbTlutOnlyBg, s_dsbTlutOnlyObj;
static u64 s_dsbTlutOnlyTicks;
#define DSB_WHY(s_) do { s_dsbWhy = (s_); } while (0)
#else
#define DSB_WHY(s_) do { } while (0)
#endif

// main.cpp: re-applies the one-time GX present state InitVideo() sets up
// (viewport / scissor / projection / Z-mode / EFB-copy src+dst / vertex
// format), which this file's own gxDsBSetup2DState() necessarily clobbers.
// See gx_ds_engineb_render.h's "GX present state" deviation note.
extern void GxRestorePresentState(void);
extern GXRModeObj *rmode;   // main.cpp (EFB pixel format choice)
// main.cpp: the mutex already serializing Draw() (emulation thread) against
// draw_thread (video thread). Held across this file's whole GX sequence so
// draw_thread cannot interleave its present quads into the same FIFO while
// Engine B's frame is being composited.
extern mutex_t vidmutex;

GxFramePlan GXDS_PLAN;
GxDsBBandRegs GXDS_REGS[GX_DSB_MAX_BAND_REGS];

// ---------------------------------------------------------------------
// Per-engine fixed addressing constants, all read back out of the CPU
// reference rather than hardcoded from GBATEK:
//  - BG palette   : MMU.ARM9_VMEM + core*ADDRESS_STEP_1KB  (GPU.cpp's
//                   renderline_textBG / GPU_RenderLine_layer backdrop read)
//  - OBJ palette  : MMU.ARM9_VMEM + 0x200 + core*0x400     (GPU.cpp's
//                   _spriteRender 16/256-colour branches)
//  - OAM          : MMU.ARM9_OAM + core*ADDRESS_STEP_1KB   (GPU_Reset)
// with core == GXDS_CORE (0 = A/main, 1 = B/sub). The names keep their pre-task-13
// "DsB" prefix (they are internal to this file).
// The 2KB palette RAM / OAM dirty bitmaps cover BOTH engines' halves; an engine only
// ever records reads inside its own half, so the other engine's writes never dirty it.
// ---------------------------------------------------------------------
static const u32 kDsBBgPalOff  = (u32)GXDS_CORE * 0x400;
static const u32 kDsBObjPalOff = 0x200 + (u32)GXDS_CORE * 0x400;
static const u32 kDsBOamOff    = (u32)GXDS_CORE * 0x400;
// vram_arm9_map page ranges (16KB pages) that back this engine's BG / OBJ windows.
#if GXDS_CORE == 0
#define GXDS_PG_BG  VRAM_PAGE_ABG
#define GXDS_PG_OBJ VRAM_PAGE_AOBJ
#else
#define GXDS_PG_BG  VRAM_PAGE_BBG
#define GXDS_PG_OBJ VRAM_PAGE_BOBJ
#endif

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
static u8 s_frameExtObjPal = 0;   // task 11: same, for DISPCNT.ExOBJPalette_Enable
static bool s_frameExtBgPalKnown = false;
// Task 13: screen offset / (Engine A) DISPCNT base blocks as seen on line 0 of this frame.
static u16 s_frameOffset = 0;
static bool s_frameOffsetKnown = false;
#if GXDS_CORE == 0
static u8 s_frameBaseBlocks = 0;
static bool s_frameBaseBlocksKnown = false;
#endif
// Task 9: per-scanline MASTER_BRIGHT mode (0 none, 1 up, 2 down) and factor (0..16).
static u8 s_mbMode[192], s_mbFac[192];

// ---------------------------------------------------------------------
// Queue item 13j: DEFERRED CPU compositing ("lazy lines").
// ---------------------------------------------------------------------
// The CPU compositor (GPU_RenderLine) used to run for every visible line at its hblank
// because whether GX handles the frame is only known at the end of the frame. It is now
// DEFERRED: the per-line hook (GXDS_API(Scanline)) says "this line's CPU render is deferred"
// (returns true) and NDSSystem.cpp skips GPU_RenderLine for it. The deferred lines are
// [s_lzRendered, s_lzSeen). They are rendered later, by the CPU, only if something needs them:
//  - a BARRIER: any write that could change what a pending line would render (an engine
//    register, palette / OAM of this engine, a VRAM page this engine can see, a VRAMCNT / POWCNT
//    remap) flushes the pending lines BEFORE the write lands, so they are rendered with exactly
//    the state they would have had at their own hblank (gxDsLazy*Barrier, called from MMU.cpp /
//    the shared write funnel). Between two barriers the state is constant, so a deferred line
//    rendered late is byte-identical to one rendered at its own hblank;
//  - the trace says the frame can no longer be handled by GX (out-of-scope state seen at line
//    L): the pending lines are flushed and lines L.. render eagerly;
//  - the frame-end GX decision bails: the pending lines are flushed (frame-end state == pending
//    range state, by the barrier rule) and the CPU result stands.
// If GX handles the frame the pending lines are DISCARDED (never rendered) after replaying the
// few persistent side effects a CPU line pass has (GPU_DiscardDeferredLines: affine reference
// point latch/advance, `blend1` leftover, disp FIFO reset, currLine).
// Lines that cannot be deferred exactly (MOSAIC register != 0: it carries per-pixel state across
// lines/frames; MASTER_BRIGHT saturated: GPU_RenderLine skips the render and its parms advance)
// are rendered eagerly, in order, after flushing whatever is pending.
static bool s_lzOn = false;       // this frame defers its CPU lines
static int s_lzRendered = 0;      // lines [0, s_lzRendered) are CPU-rendered (eager or flushed) this frame
static int s_lzSeen = 0;          // lines [0, s_lzSeen) have passed the hook; pending = [s_lzRendered, s_lzSeen)
static bool s_lzFlushing = false; // re-entrancy guard (a flushed line's capture write hits the barrier again)
static bool s_lzDataHit = false;  // a data write (VRAM/palette/OAM) hit this engine's read set during the frame
extern u32 g_gxDsLazyPend;        // gx_ds_engineb_render.cpp: bit `core` = this engine has pending lines
extern u32 g_gxDsLazyOn;          // bit `core` = this engine is inside a lazy frame (data-write bail window)
#ifdef DSLZ_STATS
static u32 s_lzStFrames, s_lzStDeferredLines, s_lzStFlushRegLines, s_lzStFlushDataLines, s_lzStFlushBailLines,
           s_lzStFlushOosLines, s_lzStDiscardFrames, s_lzStEagerLines, s_lzStBarrierIo, s_lzStBarrierData, s_lzStDataBail;
#endif

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
	memcpy(cur, vram_arm9_map + GXDS_PG_BG, 128);
	memcpy(cur + 128, vram_arm9_map + GXDS_PG_OBJ, 128);
	// Task 8: Engine B's four extended-BG-palette slot pointers
	// (MMU_VRAMmapControl repoints them on a VRAMCNT_H write).
	static u8 *s_extPalCopy[4];
	bool extChanged = false;
	for (int i = 0; i < 4; ++i)
		if (s_extPalCopy[i] != MMU.ExtPal[GXDS_CORE][i]) { extChanged = true; s_extPalCopy[i] = MMU.ExtPal[GXDS_CORE][i]; }
	// Task 11: the extended OBJ palette pointer (VRAMCNT_I). Only slot [0] is ever read
	// (PaletteIndex*0x200 <= 0x1E00+0x200 < 8KB).
	static u8 *s_extObjPalCopy;
	if (s_extObjPalCopy != MMU.ObjExtPal[GXDS_CORE][0]) { extChanged = true; s_extObjPalCopy = MMU.ObjExtPal[GXDS_CORE][0]; }
	if (s_bankMapCopyValid && !extChanged && memcmp(cur, s_bankMapCopy, sizeof(cur)) == 0)
		return false;
	memcpy(s_bankMapCopy, cur, sizeof(cur));
	s_bankMapCopyValid = true;
	return true;
}

#if GXDS_CORE == 0
// Task 13: Engine A's 3D-layer input (see "the 3D layer as a composited input texture" below)
struct GxDsA3dTex {
	GXTexObj texObj;
	void *texData;
	u32 texDataCap;
	bool valid;
	u32 seq;       // g_gfx3dRenderSeq this texture was baked from
	u16 cfgFx;
};
static GxDsA3dTex s_3d, s_3dFx;
// gx-remaining-work.md section 1 ("3D at its real priority"): the GX geometry pass's own
// output, EFB-copied (RGB5A3, same 256x256 layout as s_3d) and drawn in BG0's slot like s_3d
// when the 3D layer is not frontmost/untargeted (gxDsA3dRenderGeomTex). texData is the copy
// target (memalign'd, never CPU-written after init); only texObj/texData are used.
static GxDsA3dTex s_3dGx;
static bool s_3dTexMode;   // this frame draws s_3dGx in BG0's slot
static u16 *s_3dLinear;            // 256*192: bit15 = present, low 15 = R6G6B6TORGB15 colour
static u16 *s_3dScratch;
static u8  s_3dLineTrans[192];     // line has a texel with alpha 1..30
static u8  s_3dLineHole[192];      // line has a texel with alpha 0 (the 3D layer does not cover the whole line)
static u32 s_3dScanSeq;             // (g_gfx3dRenderSeq << 1), or (clear generation << 1) | 1 for a clear-only scan (gxDsA3dScan): the key every 3D texture is baked against
static bool s_3dScanValid;
// Deferred-raster frames (gfx3d_VBlankEndSignal / gxDsA3dScan): when set, the 3D layer
// is read as a uniform clear-colour texel line instead of gfx3d_convertedScreen.
static bool s_3dClearOnly;
static u8 s_3dClearLine[256 * 4];
static const int k3dW = 256, k3dH = 192;
// The texture is 256 x 256 (rows 192.. transparent padding), NOT 256 x 192: Dolphin's GX_NEAR sampling of a
// non-power-of-two texture through interpolated UVs picks a neighbouring texel on scattered pixels (measured on
// this layer: SM64DS frames with hundreds of one-texel-off pixels at polygon edges; the same fact task 8 found
// for affine planes), while power-of-two heights are byte-exact.
static const int k3dTexH = 256;

// Task 13d: real per-texel alpha (0..31, not collapsed to present/absent) for the
// two-pass translucent-3D-over-mixed-2nd-target technique below. Kept as a separate
// 8-bit (GX_TF_I8, sampled TEXA = I) texture (no quantization loss) rather than trying to
// widen s_3d's RGB5A3 alpha sub-format (only 3 bits / 8 levels there). Built lazily
// -- only when a band actually needs the two-pass draw this frame -- from the same
// gfx3d_convertedScreen buffer gxDsA3dScan() already linearized, keyed the same way
// (g_gfx3dRenderSeq) so it never gets rebuilt more than once per rendered 3D frame.
static void *s_3dAlphaTexData;
static u32 s_3dAlphaTexDataCap;
static GXTexObj s_3dAlphaTexObj;
static bool s_3dAlphaValid;
static u32 s_3dAlphaSeq;

// Task 13d: the per-pixel "was the EFB pixel beneath BG0's 3D slot produced by a
// BLDCNT 2nd-target layer" mask (gx_mask.h primitive) plus the scratch EFB corner
// it is built in. One mask is (re)built per (band, window-region) that actually
// needs the two-pass draw -- see gxDsBBuildUnder3DMask() below.
static GXMaskTarget s_3dMask;
static bool s_3dMaskInit;
static int s_3dMaskBand = -1, s_3dMaskCfg = -1;   // which (band,cfg) s_3dMask currently holds; reset every frame
static const u16 k3dMaskEfbX = 256, k3dMaskEfbY = 0;   // beside the (0,0,256,192) main render corner

#endif

// Palette RAM and OAM are tracked here (pre-MMU_LCDmap address; they are not remapped).
// VRAM is NOT: it is tracked in LCDC space by gxDsEngineBMarkVram() below, fed by
// MMU.cpp AFTER MMU_LCDmap() resolved the write. Under -DDSB_STATS this still
// evaluates the pre-task-10 whole-region VRAM flag so the log can report what the
// coarse gate would have re-baked on identical inputs.
void GXDS_API(MarkWrite)(u32 adr, u32 size)
{
	switch ((adr >> 24) & 0xF) {
	case 5: // palette RAM, 2KB (Engine A 0x000-0x3FF, Engine B 0x400-0x7FF)
		GXDS_PLAN.palette.markRange(adr & 0x7FF, size);
		break;
#ifdef DSB_STATS
	case 6: {
		u32 page = (adr >> 14) & (VRAM_ARM9_PAGES - 1);
		if ((page >= GXDS_PG_BG && page < GXDS_PG_BG + 128) ||
		    (page >= GXDS_PG_OBJ && page < GXDS_PG_OBJ + 128))
			s_dsbOldVramArm = true;
		else if ((adr & 0x00FFFFFF) >= (GXDS_CORE ? 0x898000u : 0x880000u) && (adr & 0x00FFFFFF) < (GXDS_CORE ? 0x8A0000u : 0x898000u))
			s_dsbOldVramArm = true;
		break;
	}
#endif
	case 7: // OAM, 2KB (Engine A 0x000-0x3FF, Engine B 0x400-0x7FF)
		GXDS_PLAN.oam.markRange(adr & 0x7FF, size);
		break;
	default:
		break;
	}
}

// Task 10: VRAM dirtiness in LCDC space. `off` is a byte offset into MMU.ARM9_LCD
// (== the address MMU_LCDmap() returned minus 0x06000000), 1KB pages, 656 of them
// (0xA4000 bytes, inside GxDirtyBitmap::kMaxPages). Every Engine-B bake read goes
// through MMU_gpu_map(), whose result is a pointer into that same array, so a
// dependency recorded as an LCDC range and a write tagged as an LCDC range meet on
// the same bytes regardless of which ARM9 page / mirror / bank remap either used.
void GXDS_API(MarkVram)(u32 off, u32 size)
{
	GXDS_PLAN.vram.markRange(off, size);
}

// Reset / savestate load rewrite VRAM/palette/OAM without a single MMU write.
static bool s_forceRebakeAll = true;
static void gxDsLzReset();
void GXDS_API(InvalidateAll)()
{
	s_forceRebakeAll = true;
	gxDsLzReset();   // task 13j: a reset / savestate load abandons whatever the frame in flight had pending
#if GXDS_CORE == 0
	s_3dScanValid = false; s_3d.valid = false; s_3dFx.valid = false;   // task 13: reset / savestate load rewrite gfx3d_convertedScreen
#endif
}

// ---------------------------------------------------------------------
// Task 10: bake read-set recording ("what did this texture actually read?")
// ---------------------------------------------------------------------
// The GBA slice derives each plane's/sprite's dependency ranges from its config
// (charBase + 1024 tiles worst case, whole palette bank, ...). Engine B instead
// RECORDS them while it bakes: every VRAM read the bake makes through
// MMU_gpu_map() is a host pointer into MMU.ARM9_LCD, so the bytes it touched are
// marked in a per-texture LCDC-space page bitmap (same 1KB pages as the write-side
// tracker), and every palette-RAM read is marked in a 32-byte-page palette bitmap.
// A texture is stale iff any recorded page was written since -- exact by
// construction (the tiles the tilemap really references, the 16-colour sub-palettes
// really used, the extended-palette slices really used, the physical bytes a sprite
// row really spans even where it runs off the end of a 16KB page), so nothing has to
// be re-derived and cannot drift from the bake. The config fingerprints (BGxCNT /
// OAM fields / VRAMCNT map) still cover everything that is not a memory byte; the
// tilemap bytes themselves are IN the read-set, so a tilemap edit re-bakes.
static const u32 kDsBLcdcBytes = 0xA4000;                       // sizeof(MMU.ARM9_LCD)
static const u32 kDsBVramPages = kDsBLcdcBytes >> 10;           // 656
static const u32 kDsBVramWords = (kDsBVramPages + 31) / 32;     // 21
struct GxDsBDeps {
	u32 vram[kDsBVramWords];  // LCDC 1KB pages read
	u32 pal[2];               // palette RAM 32B pages read (0x800 bytes = 64 pages; Engine B is 0x400-0x7FF)
};
static GxDsBDeps *s_rec;
static u32 s_recLastPage;
static inline void gxDsBRecBegin(GxDsBDeps *d)
{
	memset(d, 0, sizeof(*d));
	s_rec = d;
	s_recLastPage = ~0u;
}
struct GxDsBRecGuard { ~GxDsBRecGuard() { s_rec = NULL; } };
// Record a read of [host, host+len) where host is an MMU_gpu_map()-derived pointer.
// The read may run past the end of the 16KB page host lies in (a sprite row / an
// affine flat offset continues into the PHYSICALLY next LCDC page, exactly as the
// bake's own pointer arithmetic does), so the span is recorded linearly in LCDC space.
static inline void gxDsBRecVram(const void *host, u32 len)
{
	if (!s_rec || !len) return;
	const u32 off = (u32)((const u8 *)host - (const u8 *)MMU.ARM9_LCD);
	if (off >= kDsBLcdcBytes) return;   // blank page past the end: nothing can ever write it
	u32 last = off + len - 1;
	if (last >= kDsBLcdcBytes) last = kDsBLcdcBytes - 1;
	const u32 p0 = off >> 10, p1 = last >> 10;
	if (p0 == p1 && p0 == s_recLastPage) return;
	for (u32 p = p0; p <= p1; ++p)
		s_rec->vram[p >> 5] |= 1u << (p & 31);
	s_recLastPage = p1;
}
static inline void gxDsBRecPal(u32 off, u32 len)
{
	if (!s_rec || !len) return;
	const u32 p0 = off >> 5, p1 = (off + len - 1) >> 5;
	for (u32 p = p0; p <= p1 && p < 64; ++p)
		s_rec->pal[p >> 5] |= 1u << (p & 31);
}
static bool gxDsBDepsDirty(const GxDsBDeps &d)
{
#ifdef DSB_MUTATE_NODEPS
	(void)d; return false;   // mutation test: a dependency gate that never fires (fixtures must FAIL)
#endif
	if (GXDS_PLAN.vram.anyDirty()) {
		for (u32 w = 0; w < kDsBVramWords; ++w)
			for (u32 b = d.vram[w]; b; b &= b - 1)
				if (GXDS_PLAN.vram.isPageDirty((w << 5) + (u32)__builtin_ctz(b)))
					return true;
	}
	if (GXDS_PLAN.palette.anyDirty()) {
		for (u32 w = 0; w < 2; ++w)
			for (u32 b = d.pal[w]; b; b &= b - 1)
				if (GXDS_PLAN.palette.isPageDirty((w << 5) + (u32)__builtin_ctz(b)))
					return true;
	}
	return false;
}

// Task 23: same as gxDsBDepsDirty(), split into its two independent halves so a caller
// can tell "the recorded VRAM read-set is untouched" from "only the recorded palette
// read-set was written" -- the two are already tracked as separate bitmasks in
// GxDsBDeps (vram[]/pal[]), so this is a read-only decomposition of the existing
// dependency test, not new tracking. A palette-only-dirty cache whose format is `ci`
// (task 11) only needs its TLUT rebuilt (see gxDsBRebuildBgTlut/gxDsBRebuildObjTlut),
// not its index texture re-decoded/re-swizzled/re-uploaded, because the TLUT is the
// entire palette dependency: the index texture never encodes colour, only which
// palette entry a texel names (see gx_ds_engineb_render.h "Native CI4/CI8 + TLUT").
static void gxDsBDepsDirtySplit(const GxDsBDeps &d, bool *vramDirty, bool *palDirty)
{
#ifdef DSB_MUTATE_NODEPS
	*vramDirty = false; *palDirty = false; return;
#endif
	*vramDirty = false;
	if (GXDS_PLAN.vram.anyDirty()) {
		for (u32 w = 0; w < kDsBVramWords && !*vramDirty; ++w)
			for (u32 b = d.vram[w]; b; b &= b - 1)
				if (GXDS_PLAN.vram.isPageDirty((w << 5) + (u32)__builtin_ctz(b))) { *vramDirty = true; break; }
	}
	*palDirty = false;
	if (GXDS_PLAN.palette.anyDirty()) {
		for (u32 w = 0; w < 2 && !*palDirty; ++w)
			for (u32 b = d.pal[w]; b; b &= b - 1)
				if (GXDS_PLAN.palette.isPageDirty((w << 5) + (u32)__builtin_ctz(b))) { *palDirty = true; break; }
	}
}

// A cached texture that is NOT being (re)baked this frame -- disabled plane, invisible
// / parked sprite, fade variant not in use -- must still be dropped when its inputs
// changed, because the dirty bitmaps are cleared every frame (see Stage 1 below).
#ifdef DSB_MUTATE_NOINVAL
#define GX_DSB_DROP_IF_STALE(valid_, bank_, deps_) do { } while (0)   // mutation test: the task-9 hygiene removed
#else
#define GX_DSB_DROP_IF_STALE(valid_, bank_, deps_) do { if ((valid_) && ((bank_) || gxDsBDepsDirty(deps_))) (valid_) = false; } while (0)
#endif

// ---------------------------------------------------------------------
// Caches (Stage 1). Since task 11 a texture is native CI4/CI8 + TLUT (`ci`), or
// RGB5A3 with the palette already resolved where a TLUT cannot express the content
// (direct-colour bitmaps, multi-slice extended-palette planes; see the header's
// "Native CI4/CI8 + TLUT"). Buffers are grown on demand rather than
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
	GxDsBDeps deps;    // task 10: the VRAM / palette bytes the last bake read
	// task 11: native CI8 + TLUT. ci == false is the RGB5A3 (pre-resolved) form, used for
	// direct-colour planes, multi-slice extended-palette planes and -DDSB_NOCI builds.
	bool ci;
	u32 texBytes;      // bytes of texData actually used by the current bake
	GXTlutObj tlutObj;
	void *tlutData;    // 256 RGB5A3 entries (512B), owned by this cache, rebuilt by every bake
	u16 tlutEntries;
	// task 23: which 16-colour sub-banks (4bpp text, combined index) / which extended-
	// palette 256-colour slice (single-slice ext text / ext AffineExt_256x16) had at
	// least one opaque texel in the LAST full bake -- exactly gxDsBBakeBgPlane's/
	// gxDsBBakeAffineBgPlane's own local `usedPal`, kept around so a palette-only
	// rebuild (index bytes unchanged, so which banks/slice are referenced cannot have
	// changed either) can rebuild the TLUT without re-walking the tilemap. Unused
	// (0) for the plain-8bpp/plain-affine/bitmap cases, which always TLUT the whole
	// 256-entry bank regardless of usedPal -- see gxDsBRebuildBgTlut().
	u32 usedPal;
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
	GxDsBDeps deps;    // task 10: the VRAM / palette bytes the last bake read
	// task 11: native CI4 (16-colour) / CI8 (256-colour) + TLUT. ci == false only under -DDSB_NOCI.
	bool ci;
	u32 texBytes;
	const u8 *cfgExtPal; // extended OBJ palette slice baked in (256-colour sprite under ExOBJPalette_Enable), else NULL
	GXTlutObj tlutObj;
	void *tlutData;    // 16 or 256 RGB5A3 entries, owned by this slot
	u16 tlutEntries;
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


// ---------------------------------------------------------------------
// Task 11: TLUT hardware-slot budget and clobber protection
// ---------------------------------------------------------------------
// GX has 16 normal TLUT names (GX_TLUT0..15, 256 entries each) shared by every user, and
// binds exactly ONE TLUT per texture object, resolved by *name* when the texture is
// loaded. Engine B reserves FIVE names and no more:
//   GX_TLUT12..GX_TLUT15  BG0..BG3 (a plane's plain and brighten/darken variants share
//                         their BG's name; they are never simultaneously "the" texture)
//   GX_TLUT11             every sprite (128 x plain/fade textures -> one shared name)
// leaving GX_TLUT0..10 for Engine A / the 3D path (the GBA slice uses 0..7 but GBA and DS
// are mutually exclusive). Sharing a name is only safe if the TLUT is (re)loaded whenever
// the draw's owner differs from whoever loaded the name last, so every draw goes through
// gxDsBBindTlut(), which tracks the owner of each name and issues GX_LoadTlut on a change.
// The tracking is reset at the start of every Engine B GX sequence, so nothing assumes a
// TLUT survived from an earlier frame (another engine may have loaded the same name in
// between). A TLUT is 512B / 32B, so a reload is cheap; the ownership check keeps the
// common case (one plane drawn several times, one sprite over several window runs)
// down to a single load.
// Task 13: Engine A takes the disjoint set GX_TLUT6..9 = BG0..3, GX_TLUT10 = sprites,
// leaving GX_TLUT0..5 for the 3D path / toon / fog later; Engine B is unchanged.
#if GXDS_CORE == 0
static const u8 kDsBTlutObjSlot = GX_TLUT10;
static inline u8 gxDsBBgTlutSlot(int bg) { return (u8)(GX_TLUT6 + bg); }
#else
static const u8 kDsBTlutObjSlot = GX_TLUT11;
static inline u8 gxDsBBgTlutSlot(int bg) { return (u8)(GX_TLUT12 + bg); }
#endif
static const GXTlutObj *s_tlutOwner[20];
static inline void gxDsBTlutReset() { memset(s_tlutOwner, 0, sizeof(s_tlutOwner)); }
static inline void gxDsBBindTlut(GXTlutObj *obj, u8 slot)
{
#if defined(DSB_MUTATE_TLUTSHARE) || defined(DSB_MUTATE_TLUTNOBIND)
	(void)obj; (void)slot;   // mutation: the TLUT was loaded at bake time only, no per-draw ownership check
#else
	if (s_tlutOwner[slot] == obj) return;
	GX_LoadTlut(obj, slot);
	s_tlutOwner[slot] = obj;
#ifdef DSB_STATS
	++s_dsbTlutLoads;
#endif
#endif
}
// The slot a texture bakes its TLUT name for (the mutation builds collapse it to one).
static inline u8 gxDsBBgSlotFor(int bg)
{
#ifdef DSB_MUTATE_TLUTSHARE
	(void)bg; return kDsBTlutObjSlot;
#else
	return gxDsBBgTlutSlot(bg);
#endif
}
static inline u8 gxDsBObjSlotFor() { return kDsBTlutObjSlot; }
static inline void gxDsBMutantLoad(GXTlutObj *obj, u8 slot)
{
#if defined(DSB_MUTATE_TLUTSHARE) || defined(DSB_MUTATE_TLUTNOBIND)
	GX_LoadTlut(obj, slot);   // careless: TMEM at `slot` now holds THIS texture's TLUT until the next bake overwrites it
#else
	(void)obj; (void)slot;
#endif
}

static bool s_initDone = false;

bool GXDS_API(RenderInit)()
{
	if (s_initDone)
		return true;

	// Stage 0 dirty-bitmap sizing. Palette and OAM are each a flat 2KB on
	// the DS (MMU.h: ARM9_VMEM[0x800], ARM9_OAM[0x800]); 32-byte pages give
	// 64 pages each, i.e. 16-colour-sub-palette / 4-OAM-entry granularity --
	// the same proportional choice gbaPpuReset() makes for the GBA's 1KB
	// regions. VRAM (task 10) is tracked in LCDC space: 0xA4000 bytes at 1KB
	// pages = 656 pages (inside GxDirtyBitmap::kMaxPages).
	GXDS_PLAN.palette.init(0x800, 32);
	GXDS_PLAN.oam.init(0x800, 32);
	GXDS_PLAN.vram.init(kDsBLcdcBytes, 1024);
	GXDS_PLAN.beginFrame();

	if (!gxDsBEnsureBgScratch((u32)kDsBBgPlaneMaxPx * kDsBBgPlaneMaxPx)) return false;
	// An affine OBJ bakes with a 1-texel transparent border padded up to a power
	// of two (see gxDsBBakeObjTexture), so a 64-texel sprite needs 128x128.
	s_objBakeScratch = (u16 *)malloc((u32)(2 * kDsBObjMaxPx) * (2 * kDsBObjMaxPx) * sizeof(u16));
	if (!s_objBakeScratch) return false;

	for (int i = 0; i < 4; ++i) {
		s_bgPlane[i].texData = NULL;
		s_bgPlane[i].texDataCap = 0;
		s_bgPlane[i].valid = false;
		s_bgPlane[i].tlutData = NULL;
		s_bgPlaneFx[i].texData = NULL;
		s_bgPlaneFx[i].texDataCap = 0;
		s_bgPlaneFx[i].valid = false;
		s_bgPlaneFx[i].tlutData = NULL;
	}
	for (int i = 0; i < 128; ++i) {
		s_objTex[i].texData = NULL;
		s_objTex[i].texDataCap = 0;
		s_objTex[i].valid = false;
		s_objTex[i].tlutData = NULL;
		s_objTexFx[i].texData = NULL;
		s_objTexFx[i].texDataCap = 0;
		s_objTexFx[i].valid = false;
		s_objTexFx[i].tlutData = NULL;
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

void GXDS_API(RenderShutdown)()
{
	if (!s_initDone)
		return;
	for (int i = 0; i < 4; ++i) {
		free(s_bgPlane[i].texData); s_bgPlane[i].texData = NULL; s_bgPlane[i].texDataCap = 0; s_bgPlane[i].valid = false;
		free(s_bgPlaneFx[i].texData); s_bgPlaneFx[i].texData = NULL; s_bgPlaneFx[i].texDataCap = 0; s_bgPlaneFx[i].valid = false;
		free(s_bgPlane[i].tlutData); s_bgPlane[i].tlutData = NULL;
		free(s_bgPlaneFx[i].tlutData); s_bgPlaneFx[i].tlutData = NULL;
	}
	for (int i = 0; i < 128; ++i) {
		free(s_objTex[i].texData); s_objTex[i].texData = NULL; s_objTex[i].texDataCap = 0; s_objTex[i].valid = false;
		free(s_objTexFx[i].texData); s_objTexFx[i].texData = NULL; s_objTexFx[i].texDataCap = 0; s_objTexFx[i].valid = false;
		free(s_objTex[i].tlutData); s_objTex[i].tlutData = NULL;
		free(s_objTexFx[i].tlutData); s_objTexFx[i].tlutData = NULL;
	}
	free(s_bgBakeScratch); s_bgBakeScratch = NULL; s_bgBakeScratchCap = 0;
	free(s_objBakeScratch); s_objBakeScratch = NULL;
	free(s_backdropTexData); s_backdropTexData = NULL;
	free(s_copyBackBuf); s_copyBackBuf = NULL;
	free(s_copyBackLinear); s_copyBackLinear = NULL;
#if GXDS_CORE == 0
	free(s_3d.texData); s_3d.texData = NULL; s_3d.texDataCap = 0; s_3d.valid = false;
	free(s_3dFx.texData); s_3dFx.texData = NULL; s_3dFx.texDataCap = 0; s_3dFx.valid = false;
	free(s_3dLinear); s_3dLinear = NULL; free(s_3dScratch); s_3dScratch = NULL; s_3dScanValid = false;
	free(s_3dGx.texData); s_3dGx.texData = NULL; s_3dGx.valid = false;
#endif
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
#if GXDS_CORE == 0
	r->bg3d = (u8)(d.BG0_3D ? 1 : 0);   // task 13: BG0 is the 3D layer (GPU.cpp: only for core A)
#endif
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
		// GPU.cpp reads the matrix through LE_TO_LOCAL_16 (host-endian fixup);
		// BGxX/BGxY are plain host s32 and are read unswapped, as it does.
		s16 pa = (s16)LE_TO_LOCAL_16(p->BGxPA), pb = (s16)LE_TO_LOCAL_16(p->BGxPB);
		s16 pc = (s16)LE_TO_LOCAL_16(p->BGxPC), pd = (s16)LE_TO_LOCAL_16(p->BGxPD);
		// Line 0: the CPU re-latches BGxX/BGxY from affineInfo[] inside
		// GPU_RenderLine(l==0), which runs AFTER this hook, so the live
		// parms->BGxX still holds last frame's accumulated value here.
		// Task 13j: with deferred CPU lines parms only advance when a line is really rendered
		// (s_lzRendered lines so far; s_lzRendered == 0 means not even the line-0 latch has run),
		// so the value in effect at `line` is the parms value as of line s_lzRendered plus
		// (line - s_lzRendered) steps of PB/PD (PB/PD/enable state are constant across the pending
		// range: any write flushes it). With eager rendering line == s_lzRendered, i.e. the old code.
		const bool fromInfo = (line == 0) || (s_lzRendered == 0);
		const u32 steps = (u32)(line - s_lzRendered);
		s32 x = fromInfo ? (s32)gpu->affineInfo[w].x : p->BGxX;
		s32 y = fromInfo ? (s32)gpu->affineInfo[w].y : p->BGxY;
		if (line != 0) {
			x = (s32)((u32)x + steps * (u32)(s32)pb);
			y = (s32)((u32)y + steps * (u32)(s32)pd);
		}
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
	if (GXDS_CORE == 0 && d.BG0_3D && d.BG0_Enable) s_dsb3dSeen = true;
	s_dsbTypes = (u32)gpu->BGTypes[0] | ((u32)gpu->BGTypes[1] << 4) | ((u32)gpu->BGTypes[2] << 8) | ((u32)gpu->BGTypes[3] << 12);
#endif

	if (gpu->dispMode != 1) { DSB_WHY("dispmode"); return true; }   // 0 = white; 2/3 (VRAM / FIFO display) are Engine-A-only
	if (d.ForceBlank) { DSB_WHY("forceblank"); return true; }
	// Task 13: which physical screen this engine's scanlines land on (POWCNT1 DISPSWAP
	// flips MainScreen.offset / SubScreen.offset). The frame is composited in one pass
	// and written at the frame-final offset, so a swap DURING the visible frame (half the
	// lines on each screen in the CPU reference) cannot be reproduced.
	if (s_frameOffsetKnown && GXDS_SCREEN.offset != s_frameOffset) { DSB_WHY("screenswap"); return true; }
	s_frameOffset = GXDS_SCREEN.offset;
	s_frameOffsetKnown = true;
#if GXDS_CORE == 0
	// Task 13: the display-capture unit writes guest VRAM line by line WHILE the frame
	// renders and reads this engine's own composited lines; a bank it writes may be
	// the very bank a BG/OBJ of this or the other engine reads (the classic dual-3D
	// trick). Baking from frame-final VRAM would not reproduce a line-interleaved
	// read/write, and the capture source (Engine A's composite) has to be the CPU's.
	// Armed = the register's enable bit (set by the game, latched into `enabled` at line
	// 0 and cleared after line 191) -- either form bails.
	if (gpu->dispCapCnt.enabled || (gpu->dispCapCnt.val & 0x80000000u)) { DSB_WHY("capture"); return true; }
	// DISPCNT's 64KB character / screen base blocks (Engine A only) move BG_tile_ram /
	// BG_map_ram; those are baked once from the frame-final value, like ExBGxPalette.
	{
		const u8 bb = (u8)((d.CharacBase_Block << 3) | d.ScreenBase_Block);
		if (s_frameBaseBlocksKnown && bb != s_frameBaseBlocks) { DSB_WHY("basechange"); return true; }
		s_frameBaseBlocks = bb;
		s_frameBaseBlocksKnown = true;
	}
#endif
	// Extended BG palettes are IN scope (task 8 -- SM64DS's in-game sub screen needs
	// them; see gxDsBExtPalFor()) and, since task 11, so are extended OBJ palettes
	// (gxDsBObjExtPalFor()). Both flags are baked once from the frame-final state, so a
	// mid-frame toggle can't be replayed.
	if (s_frameExtBgPalKnown && (d.ExBGxPalette_Enable != s_frameExtBgPal || d.ExOBJPalette_Enable != s_frameExtObjPal)) { DSB_WHY("extpalchange"); return true; }
	s_frameExtBgPal = d.ExBGxPalette_Enable;
	s_frameExtObjPal = d.ExOBJPalette_Enable;
	s_frameExtBgPalKnown = true;
	// Task 9: WIN0/WIN1, BLDCNT effects, semi-transparent OBJ and MASTER_BRIGHT are
	// now in scope. Still out: the OBJ window (a sprite-shaped mask), and a
	// window combined with brighten/darken -- GPU.cpp's WINDOWED backdrop path
	// tests `blend1 && windowEffect` where `blend1` is stale state left by
	// whichever layer the PREVIOUS line composited last (it is not the backdrop's
	// own BLDCNT bit), a CPU quirk this file has no model for.
	if (gpu->WINOBJ_ENABLED) { DSB_WHY("objwin"); return true; }
	if ((gpu->WIN0_ENABLED || gpu->WIN1_ENABLED) && (((gpu->BLDCNT >> 6) & 3) >= 2)) { DSB_WHY("winfade"); return true; }
	if (!CommonSettings.showGpu.screens[GXDS_CORE]) { DSB_WHY("hidden"); return true; }

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
		if (!CommonSettings.dispLayers[GXDS_CORE][bg]) { DSB_WHY("hiddenlayer"); return true; }
	}
	if (d.OBJ_Enable && !CommonSettings.dispLayers[GXDS_CORE][4]) { DSB_WHY("hiddenobj"); return true; }

	return false;
}

static void gxDsBScanlineTrace(int line)
{
	if (line < 0 || line >= kDsBScreenH)
		return;
	if (gameInfo.isGBA)
		return;

	GPU *gpu = GXDS_SCREEN.gpu;
	if (!gpu)
		return;
	// Lazy first-use init: this is the earliest point in a DS frame that is
	// guaranteed to run after main.cpp's InitVideo()/GX_Init(), and it means
	// the Stage-0 dirty bitmaps are sized before the frame whose bakes will
	// consult them rather than one frame late.
	if (!s_initDone && !GXDS_API(RenderInit)()) {
		s_frameOutOfScope = true;
		return;
	}

	if (line == 0) {
		s_frameSawLine0 = true;
		s_frameOutOfScope = false;
		s_affPred[0].valid = s_affPred[1].valid = false;
		s_frameExtBgPalKnown = false;
		s_frameOffsetKnown = false;
#if GXDS_CORE == 0
		s_frameBaseBlocksKnown = false;
#endif
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
		GXDS_REGS[0] = cur;
		s_lastBandRegs = cur;
		return;
	}
	if (!affChanged && memcmp(&cur, &s_lastBandRegs, GX_DSB_BANDREGS_CMP_BYTES) == 0)
		return;

	GXDS_PLAN.bands.recordChangeAtLine(line);
	int bc = GXDS_PLAN.bands.boundaryCount();
	if (bc > 0 && bc < GX_DSB_MAX_BAND_REGS)
		GXDS_REGS[bc] = cur;
	s_lastBandRegs = cur;
}

// ---------------------------------------------------------------------
// Queue item 13j: deferred CPU compositing -- flush / discard / per-line decision.
// (State variables are declared with the other Stage 0 state above.)
// ---------------------------------------------------------------------
#if defined(DS2D_NOLAZY)
#define GXDS_LAZY_COMPILED 0
#else
#define GXDS_LAZY_COMPILED 1
#endif

// Lazy lines only make sense while this engine's GX path could handle the frame at all.
static inline bool gxDsLzAllowed()
{
#if defined(GXDS_FORCE_CPU) && !defined(DSLZ_FORCE_LAZY)
	return false;   // forced-CPU reference builds keep the eager pre-13j behaviour
#else
	return !gameInfo.isGBA && s_initDone && GXDS_SCREEN.gpu && vidmutex != LWP_MUTEX_NULL;
#endif
}

// Renders every pending line on the CPU, in order (exactly the GPU_RenderLine call the eager path
// would have made at that line's hblank, with the state constant since then: see the header
// comment on s_lzOn). `why`: 0 register write, 1 data write, 2 frame-end bail, 3 out-of-scope line.
static void gxDsLzFlush(int why)
{
	if (s_lzFlushing || s_lzSeen <= s_lzRendered)
		return;
	s_lzFlushing = true;
	{
		PZ_SCOPE(PZ_GPU_2D);
		PZ_SUB_SCOPE(GXDS_CORE == 0 ? PZ_2DA_CPU : PZ_2DB_CPU);
		const int n = s_lzSeen - s_lzRendered;
#ifdef DSLZ_STATS
		if (why == 0) s_lzStFlushRegLines += n; else if (why == 1) s_lzStFlushDataLines += n;
		else if (why == 2) s_lzStFlushBailLines += n; else s_lzStFlushOosLines += n;
#endif
		(void)why; (void)n;
		while (s_lzRendered < s_lzSeen) {
			GPU_RenderLine(&GXDS_SCREEN, (u16)s_lzRendered, false);
			++s_lzRendered;
		}
	}
	g_gxDsLazyPend &= ~(1u << GXDS_CORE);
	s_lzFlushing = false;
}

// Barrier entry points (called through gx_ds_engineb_render.cpp's dispatchers, BEFORE the write lands).
void GXDS_API(LazyBarrier)(int data)
{
#if GXDS_LAZY_COMPILED && !defined(DSLZ_MUTATE_NOBARRIER)
	if (s_lzFlushing)
		return;
#ifdef DSLZ_STATS
	if (s_lzSeen > s_lzRendered) { if (data) ++s_lzStBarrierData; else ++s_lzStBarrierIo; }
#endif
#ifndef DSLZ_NODATABAIL
	if (data && s_lzOn)
		s_lzDataHit = true;   // the frame-end GX pass would bake frame-final data: bail (exact CPU result instead)
#endif
	gxDsLzFlush(data ? 1 : 0);
#else
	(void)data;
#endif
}

bool GXDS_API(LazyPending)()
{
	return s_lzSeen > s_lzRendered;
}

static void gxDsLzReset()
{
	s_lzOn = false; s_lzRendered = 0; s_lzSeen = 0; s_lzFlushing = false; s_lzDataHit = false;
	g_gxDsLazyPend &= ~(1u << GXDS_CORE);
	g_gxDsLazyOn &= ~(1u << GXDS_CORE);
}

// Frame ended with GX handling it: the pending lines are never rendered; replay the persistent side
// effects the CPU line pass would have had (see GPU_DiscardDeferredLines).
static void gxDsLzDiscard()
{
	if (s_lzSeen <= s_lzRendered)
		return;
#ifdef DSLZ_STATS
	++s_lzStDiscardFrames;
#endif
#ifndef DSLZ_MUTATE_NODISCARD   // mutation check: skip the side-effect replay (lz_c24 must then differ)
	GPU_DiscardDeferredLines(&GXDS_SCREEN, s_lzRendered, s_lzSeen - 1);
#endif
}

// Returns true when this line's CPU render is deferred (NDSSystem.cpp then skips GPU_RenderLine for
// it); false = the caller renders it now, exactly like before task 13j.
bool GXDS_API(Scanline)(int line)
{
	if (line == 0)
		gxDsLzReset();
	gxDsBScanlineTrace(line);
	if (line < 0 || line >= kDsBScreenH)
		return false;
#if GXDS_LAZY_COMPILED
	if (line == 0) {
		s_lzOn = gxDsLzAllowed();
		if (s_lzOn) g_gxDsLazyOn |= (1u << GXDS_CORE);
	}
	if (s_lzOn && line != s_lzSeen) {
		// Lost sync (a hook was skipped): never defer again this frame; whatever is pending stays exact.
		gxDsLzFlush(3);
		s_lzOn = false;
		g_gxDsLazyOn &= ~(1u << GXDS_CORE);
	}
	bool defer = s_lzOn && s_frameSawLine0;
#ifndef DSLZ_MUTATE_IGNOREOOS
	if (s_frameOutOfScope)
		defer = false;   // GX cannot handle this frame any more: the rest is CPU work, done now
#endif
	if (defer) {
		GPU *gpu = GXDS_SCREEN.gpu;
		// Lines whose CPU pass has state that outlives it and cannot be replayed cheaply stay eager.
		const u16 mosaic = T1ReadWord((u8 *)&gpu->dispx_st->dispx_MISC.MOSAIC, 0);
		const bool brightSat = gpu->MasterBrightFactor >= 16 && (gpu->MasterBrightMode == 1 || gpu->MasterBrightMode == 2);
		if (mosaic != 0 || brightSat)
			defer = false;
	}
	if (defer) {
		++s_lzSeen;
		g_gxDsLazyPend |= (1u << GXDS_CORE);
#ifdef DSLZ_STATS
		++s_lzStDeferredLines;
#endif
		return true;
	}
	gxDsLzFlush(3);
#ifdef DSLZ_STATS
	++s_lzStEagerLines;
#endif
	s_lzRendered = s_lzSeen = line + 1;   // the caller renders `line` right after this returns
#else
	(void)line;
#endif
	return false;
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
	// Task 13d: EFB dithering (libogc/GX default state, never touched here before)
	// silently perturbs every EFB write by a few LSBs -- invisible for ordinary BG/OBJ
	// colour that only ever gets read back within the already-accepted "near match"
	// alpha tolerance, but fatal for gx_mask.h's mask primitive, which needs an exact
	// binary (0/255) constant colour to round-trip through an EFB write + I8 copy.
	// Off unconditionally: nothing in this pixel-exact-targeting renderer wants noise
	// added to its output.
	GX_SetDither(GX_FALSE);

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
static inline int gxDsBPopcount(u32 v) { return __builtin_popcount(v); }
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
// ---------------------------------------------------------------------
// Task 11: native CI helpers
// ---------------------------------------------------------------------
// Every bake below first writes a linear u16 "palette index" scratch: 0 = transparent
// (palette index 0 of the tile / sprite, or the affine border / padding), otherwise the
// index the CPU reference would look up in the palette it selected (4bpp text:
// palNum*16+idx; 8bpp: idx; extended palette: (palNum<<8)+idx). That one representation
// is then either compacted to CI8/CI4 texels + a TLUT (the normal case) or resolved to
// RGB5A3 through the same palette (the fallback: direct-colour planes never come here;
// multi-slice extended-palette planes and -DDSB_NOCI builds do).
static bool gxDsBEnsureTlut(void **data)
{
	if (!*data) *data = memalign(32, 256 * sizeof(u16));
	return *data != NULL;
}
// tlut[0] transparent; tlut[e] (1 <= e < n) = opaque colour of the palette entry at base+e*2.
static void gxDsBTlutFromPal(u16 *tlut, const u8 *base, int n)
{
	tlut[0] = kDsBTransparentTexel;
	for (int e = 1; e < n; ++e)
		tlut[e] = gxDsBOpaqueTexel(T1ReadWord((void *)base, (u32)e * 2));
}
// Task 23: rebuild ONLY the TLUT of an already-CI, already-valid BG plane cache whose
// recorded VRAM read-set (tilemap + tiles + ext-palette-slice pointer bytes) is clean
// but whose recorded palette read-set was written -- e.g. a palette-fade effect that
// rewrites colours in place without touching a single tile or map byte. The index
// texture (`pc.texData`) is exactly what the last full bake left it as and does not
// need re-decoding, re-swizzling or re-uploading; only the 256-entry RGB5A3 TLUT next
// to it is out of date. Mirrors exactly the TLUT-building block inside
// gxDsBBakeBgPlane()/gxDsBBakeAffineBgPlane() (kept in sync with those, not a
// reimplementation with independent logic) but reads `pc.usedPal`/`pc.cfgExtPal`/
// `pc.cfgColorMode`/`pc.cfgType` (the last full bake's own results) instead of
// re-deriving them from a VRAM walk. Caller has already confirmed `pc.ci` (a
// multi-slice extended-palette plane's RGB5A3 fallback form has no TLUT at all: it
// must always take the full re-resolve path, same as before this task).
static void gxDsBRebuildBgTlut(GxDsBBgPlaneCache &pc, u8 slot)
{
	u8 *const pal = MMU.ARM9_VMEM + kDsBBgPalOff;
	u16 *const tlut = (u16 *)pc.tlutData;
	memset(tlut, 0, 256 * sizeof(u16));
	const bool text4bpp = (pc.cfgType == (u8)BGType_Text) && !pc.cfgColorMode;
	if (text4bpp) {
		for (int pn = 0; pn < 16; ++pn)
			if ((pc.usedPal >> pn) & 1)
				for (int e = 1; e < 16; ++e)
					tlut[pn * 16 + e] = gxDsBOpaqueTexel(T1ReadWord(pal, (u32)(pn * 16 + e) * 2));
	} else if (pc.cfgExtPal) {
		if (pc.usedPal)
			gxDsBTlutFromPal(tlut, pc.cfgExtPal + (u32)__builtin_ctz(pc.usedPal) * 512, 256);
	} else {
		gxDsBTlutFromPal(tlut, pal, 256);
	}
	DCFlushRange(pc.tlutData, 256 * sizeof(u16));
	s_tlutOwner[slot] = NULL;   // force the next gxDsBBindTlut() to reload this name from the new data
}

// Task 23: same idea as gxDsBRebuildBgTlut() for one OBJ slot. An OBJ's TLUT depends
// only on its own fixed (palIndex, depth, extPal-slice) fields -- already the slot's
// own cached fingerprint -- and the current palette bytes, never on the sprite's
// index texture, so this needs no per-sprite "usedPal"-style bookkeeping at all.
static void gxDsBRebuildObjTlut(GxDsBObjTexSlot &slot, u8 tlutSlot)
{
	u8 *const pal = MMU.ARM9_VMEM + kDsBObjPalOff;
	u16 *const tlut = (u16 *)slot.tlutData;
	if (slot.cfgDepth) gxDsBTlutFromPal(tlut, slot.cfgExtPal ? slot.cfgExtPal : pal, 256);
	else gxDsBTlutFromPal(tlut, pal + (u32)slot.cfgPalIndex * 32, 16);
	DCFlushRange(slot.tlutData, (u32)slot.tlutEntries * sizeof(u16));
	s_tlutOwner[tlutSlot] = NULL;
}

// In-place u16 -> u8 compaction (write index i lands at or before read index i).
static void gxDsBCompact8(u16 *buf, u32 n)
{
	u8 *p8 = (u8 *)buf;
	for (u32 i = 0; i < n; ++i) p8[i] = (u8)buf[i];
}
// In-place resolve of an index scratch to RGB5A3 (the fallback / -DDSB_NOCI form).
static void gxDsBResolveScratch(u16 *buf, u32 n, const u8 *pal)
{
	for (u32 i = 0; i < n; ++i) {
		const u16 v = buf[i];
		buf[i] = v ? gxDsBOpaqueTexel(T1ReadWord((void *)pal, (u32)v * 2)) : kDsBTransparentTexel;
	}
}
// Upload `scratch` (w x h palette indices, values < 256 after the caller's slice
// selection) as a CI8 texture object bound to TLUT name `slot`; `tlutData` (256 RGB5A3
// entries) is already filled.
static bool gxDsBUploadCI8(void **texData, u32 *cap, u32 *texBytes, u16 *scratch, int w, int h,
                           GXTexObj *tex, u8 wm, GXTlutObj *tlutObj, void *tlutData, u8 slot)
{
	const u32 needed = (u32)w * h;
	if (!gxDsBEnsureCap(texData, cap, needed))
		return false;
	gxDsBCompact8(scratch, needed);
	GXBlockShape blk = gxBlockShape(GXTEXFMT_CI8);
	gxSwizzle8bpp((const u8 *)scratch, (u8 *)*texData, blk.texelsWide, blk.texelsTall, w, h);
	DCFlushRange(*texData, needed);
	DCFlushRange(tlutData, 256 * sizeof(u16));
	GX_InitTlutObj(tlutObj, tlutData, GX_TL_RGB5A3, 256);
	GX_InitTexObjCI(tex, *texData, w, h, GX_TF_CI8, wm, wm, GX_FALSE, slot);
	GX_InitTexObjFilterMode(tex, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
	gxDsBMutantLoad(tlutObj, slot);
	*texBytes = needed;
	return true;
}
#ifdef DSB_STATS
static inline void gxDsBStatBake(bool ci, u32 bytes, u32 rgbEquiv, bool fallback)
{
	if (ci) ++s_dsbBakeCi; else ++s_dsbBakeRgb;
	if (fallback) ++s_dsbBakeFallback;
	s_dsbBakedBytes += bytes;
	s_dsbBakedBytesRgbEquiv += rgbEquiv;
}
#else
#define gxDsBStatBake(a, b, c, d) do { } while (0)
#endif

// The extended BG palette slot a plane samples, or NULL when it uses the
// regular BG palette. Mirrors GPU.cpp exactly: the ext palette only applies
// when DISPCNT.ExBGxPalette_Enable is set AND the plane is a 256-colour text
// BG (renderline_textBG's `if(dispCnt->ExBGxPalette_Enable) pal =
// MMU.ExtPal[core][BGExtPalSlot[num]]`, reached only in the Palette_256
// branch) or a BGType_AffineExt_256x16 plane (extRotBG2). Every other BG kind
// -- 4bpp text, plain affine, 256x1, direct, large-8bpp -- ignores it.
static const u8 *gxDsBExtPalFor(int bg)
{
	GPU *gpu = GXDS_SCREEN.gpu;
	if (!gpu->dispx_st->dispx_DISPCNT.bits.ExBGxPalette_Enable)
		return NULL;
	const BGType t = gpu->BGTypes[bg];
	const bool applies = (t == BGType_Text) ? (gpu->dispx_st->dispx_BGxCNT[bg].bits.Palette_256 != 0)
	                                        : (t == BGType_AffineExt_256x16);
	return applies ? MMU.ExtPal[GXDS_CORE][gpu->BGExtPalSlot[bg]] : NULL;
}

static void gxDsBBakeBgPlane(int bg, GxDsBBgPlaneCache &pc)
{
	GPU *gpu = GXDS_SCREEN.gpu;
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

	// Task 10: record the read-set (see gxDsBDeps). Tiles are recorded once per tile
	// number (a 64-byte tile never straddles a 16KB page, so its rows sit at fixed
	// offsets from its own start); sub-palettes once per palNum.
	gxDsBRecBegin(&pc.deps);
	GxDsBRecGuard recGuard;
	u32 seenTile[32] = { 0 };
	u32 seenPal = 0;
	u32 usedPal = 0;   // task 11: palNums with at least one opaque texel (bit palNum)
	if (c256 && !extPal)
		gxDsBRecPal(kDsBBgPalOff, 512);

	for (int ty = 0; ty < ht / 8; ++ty) {
		u32 rowMap = mapBase + (u32)(ty & 31) * 64;
		if (ty > 31)
			rowMap += (u32)ADDRESS_STEP_512B << screenSize;
		for (int tx = 0; tx < lg / 8; ++tx) {
			u32 mapAddr = rowMap + (u32)((tx & 31) << 1);
			if (tx > 31)
				mapAddr += 32 * 32 * 2;
			const void *const mapHost = MMU_gpu_map(mapAddr);
			gxDsBRecVram(mapHost, 2);
			u16 entry = T1ReadWord((void *)mapHost, 0);
			int tileNum = entry & 0x3FF;
			bool hflip = ((entry >> 10) & 1) != 0;
			bool vflip = ((entry >> 11) & 1) != 0;
			int palNum = (entry >> 12) & 0xF;
			if (!((seenTile[tileNum >> 5] >> (tileNum & 31)) & 1)) {
				seenTile[tileNum >> 5] |= 1u << (tileNum & 31);
				gxDsBRecVram(MMU_gpu_map(tileBase + (u32)tileNum * tileBytes), (u32)tileBytes);
			}
			if (!((seenPal >> palNum) & 1)) {
				seenPal |= 1u << palNum;
				if (extPal) gxDsBRecVram(extPal + (u32)palNum * 512, 512);       // ext slot: 16 palettes x 256 x 2B
				else if (!c256) gxDsBRecPal(kDsBBgPalOff + (u32)palNum * 32, 32); // 4bpp: its own 16-colour sub-palette
			}

			bool anyOpaque = false;
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
						out[sx] = 0;
					} else {
						anyOpaque = true;
						// palette index the CPU reference would look up (== the CI8 combined index for 4bpp)
						out[sx] = (u16)(c256 ? (extPal ? (palNum << 8) + idx : idx) : (palNum * 16 + idx));
					}
				}
			}
			if (anyOpaque) usedPal |= 1u << palNum;
		}
	}

	// Task 11: format decision. 4bpp text (combined index) and 8bpp text on the regular
	// palette are always CI8; 8bpp text on an extended palette is CI8 only if the plane
	// uses a single 256-colour slice (a tile's palNum picks the slice and GX binds one
	// TLUT per texture -- a multi-slice plane falls back to pre-resolved RGB5A3).
	bool ci = true;
	if (extPal && gxDsBPopcount(usedPal) > 1) ci = false;
#ifdef DSB_NOCI
	ci = false;
#endif
	const u32 texels = (u32)lg * ht;
	pc.wpx = (u16)lg;
	pc.hpx = (u16)ht;
	if (ci) {
		if (!gxDsBEnsureTlut(&pc.tlutData))
			return;
		u16 *tlut = (u16 *)pc.tlutData;
		memset(tlut, 0, 256 * sizeof(u16));
		if (!c256) {
			// 4bpp: TLUT entry palNum*16+e for every sub-bank with an opaque texel (the rest are
			// never sampled and stay 0, which keeps the TLUT a pure function of the recorded read-set).
			for (int pn = 0; pn < 16; ++pn)
				if ((usedPal >> pn) & 1)
					for (int e = 1; e < 16; ++e)
						tlut[pn * 16 + e] = gxDsBOpaqueTexel(T1ReadWord(pal, (u32)(pn * 16 + e) * 2));
		} else if (!extPal) {
			gxDsBTlutFromPal(tlut, pal, 256);
		} else if (usedPal) {
			gxDsBTlutFromPal(tlut, extPal + (u32)__builtin_ctz(usedPal) * 512, 256);
		}
		pc.tlutEntries = 256;
		if (!gxDsBUploadCI8(&pc.texData, &pc.texDataCap, &pc.texBytes, s_bgBakeScratch, lg, ht,
		                    &pc.texObj, GX_REPEAT, &pc.tlutObj, pc.tlutData, gxDsBBgSlotFor(bg)))
			return;
	} else {
		gxDsBResolveScratch(s_bgBakeScratch, texels, pal);
		u32 needed = texels * sizeof(u16);
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
		pc.texBytes = needed;
	}
	pc.ci = ci;
	gxDsBStatBake(ci, pc.texBytes, texels * 2, extPal && !ci);
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
	pc.usedPal = usedPal;   // task 23: which sub-banks/slice this bake's index data actually references
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
	GPU *gpu = GXDS_SCREEN.gpu;
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

	gxDsBRecBegin(&pc.deps);   // task 10: read-set (see gxDsBDeps)
	GxDsBRecGuard recGuard;
	u16 *const buf = s_bgBakeScratch;
	if (!wrap)
		memset(buf, 0, (size_t)bufW * bufH * sizeof(u16)); // 0 == transparent (index 0 / RGB5A3 alpha 0)

	// BGType_AffineExt_256x16 with DISPCNT.ExBGxPalette_Enable: the tile's
	// 4-bit palette number selects one of 16 256-entry palettes in the ext
	// slot (rot_tiled_16bit_entry<extPal=true>: `pal[(entry + (Palette<<8))<<1]`).
	const u8 *extPal = gxDsBExtPalFor(bg);
	// The 256-entry regular palette is read by every indexed flavour except an
	// extended-palette 256x16 (and a direct-colour bitmap never touches a palette).
	if (type != BGType_AffineExt_Direct && !(type == BGType_AffineExt_256x16 && extPal))
		gxDsBRecPal(kDsBBgPalOff, 512);
	u32 seenExtPal = 0;
	u32 usedPal = 0;   // task 11: ext-palette slices with an opaque texel

	const u32 tileBase = gpu->BG_tile_ram[bg];
	const u32 mapBase = gpu->BG_map_ram[bg];
	u32 auxBase = 0;
	const bool indexed = (type != BGType_AffineExt_Direct);

	// Task 11: every indexed flavour writes its palette index (0 = transparent) into the
	// scratch; format selection / TLUT / RGB5A3 resolve happen after the walk. (The
	// direct-colour bitmap writes RGB5A3 texels straight away and stays RGB5A3.)
	if (type == BGType_Affine || type == BGType_AffineExt_256x16) {
		const int tw = lg >> 3, th = ht >> 3;
		for (int ty = 0; ty < th; ++ty) {
			for (int tx = 0; tx < tw; ++tx) {
				int tileNum, palNum = 0;
				bool hflip = false, vflip = false;
				if (type == BGType_Affine) {
					const void *const mh = MMU_gpu_map(mapBase + (u32)(tx + ty * tw));
					gxDsBRecVram(mh, 1);
					tileNum = *(const u8 *)mh;
				} else {
					const void *const mh = MMU_gpu_map(mapBase + (((u32)(tx + ty * tw)) << 1));
					gxDsBRecVram(mh, 2);
					u16 e = T1ReadWord((void *)mh, 0);
					tileNum = e & 0x3FF;
					hflip = ((e >> 10) & 1) != 0;
					vflip = ((e >> 11) & 1) != 0;
					palNum = (e >> 12) & 0xF;
				}
				const u8 *tile = (const u8 *)MMU_gpu_map(tileBase + ((u32)tileNum << 6));
				gxDsBRecVram(tile, 64);   // (the recorder de-duplicates consecutive same-page marks)
				if (extPal && !((seenExtPal >> palNum) & 1)) {
					seenExtPal |= 1u << palNum;
					gxDsBRecVram(extPal + (u32)palNum * 512, 512);
				}
				const u16 base = (u16)(extPal ? (palNum << 8) : 0);
				bool anyOpaque = false;
				for (int sy = 0; sy < 8; ++sy) {
					const u8 *row = tile + (vflip ? 7 - sy : sy) * 8;
					u16 *out = buf + (u32)(ty * 8 + sy + border) * bufW + tx * 8 + border;
					for (int sx = 0; sx < 8; ++sx) {
						int idx = row[hflip ? 7 - sx : sx];
						out[sx] = idx ? (u16)(base + idx) : 0;
						anyOpaque |= (idx != 0);
					}
				}
				if (anyOpaque && extPal) usedPal |= 1u << palNum;
			}
		}
	} else if (type == BGType_AffineExt_256x1 || type == BGType_Large8bpp) {
		auxBase = (type == BGType_Large8bpp) ? gpu->BG_bmp_large_ram[bg] : gpu->BG_bmp_ram[bg];
		for (int y = 0; y < ht; ++y) {
			const u8 *row = (const u8 *)MMU_gpu_map(auxBase + (u32)y * lg);
			gxDsBRecVram(row, (u32)lg);
			u16 *out = buf + (u32)(y + border) * bufW + border;
			for (int x = 0; x < lg; ++x)
				out[x] = row[x];
		}
	} else if (type == BGType_AffineExt_Direct) {
		auxBase = gpu->BG_bmp_ram[bg];
		for (int y = 0; y < ht; ++y) {
			void *row = MMU_gpu_map(auxBase + (((u32)y * lg) << 1));
			gxDsBRecVram(row, (u32)lg << 1);
			u16 *out = buf + (u32)(y + border) * bufW + border;
			for (int x = 0; x < lg; ++x) {
				u16 c = T1ReadWord(row, (u32)x << 1);
				out[x] = (c & 0x8000) ? gxDsBOpaqueTexel(c) : kDsBTransparentTexel;
			}
		}
	} else {
		return;
	}

	const bool sliceOk = !(extPal && type == BGType_AffineExt_256x16 && gxDsBPopcount(usedPal) > 1);
	bool ci = indexed && sliceOk;
#ifdef DSB_NOCI
	ci = false;
#endif
	const u32 texels = (u32)bufW * bufH;
	const u8 wm = wrap ? GX_REPEAT : GX_CLAMP;
	if (ci) {
		if (!gxDsBEnsureTlut(&pc.tlutData))
			return;
		u16 *tlut = (u16 *)pc.tlutData;
		memset(tlut, 0, 256 * sizeof(u16));
		if (type == BGType_AffineExt_256x16 && extPal) {
			if (usedPal) gxDsBTlutFromPal(tlut, extPal + (u32)__builtin_ctz(usedPal) * 512, 256);
		} else {
			gxDsBTlutFromPal(tlut, pal, 256);   // index 0 transparent, 1..255 from the regular BG palette bank
		}
		pc.tlutEntries = 256;
		if (!gxDsBUploadCI8(&pc.texData, &pc.texDataCap, &pc.texBytes, buf, bufW, bufH,
		                    &pc.texObj, wm, &pc.tlutObj, pc.tlutData, gxDsBBgSlotFor(bg)))
			return;
	} else {
		if (indexed)
			gxDsBResolveScratch(buf, texels, (type == BGType_AffineExt_256x16 && extPal) ? extPal : pal);
		u32 needed = texels * sizeof(u16);
		if (!gxDsBEnsureCap(&pc.texData, &pc.texDataCap, needed))
			return;
		GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
		gxSwizzle16bpp(buf, (u16 *)pc.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
		DCFlushRange(pc.texData, needed);
		GX_InitTexObj(&pc.texObj, pc.texData, bufW, bufH, GX_TF_RGB5A3, wm, wm, GX_FALSE);
		GX_InitTexObjFilterMode(&pc.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
		pc.texBytes = needed;
	}
	pc.ci = ci;
	gxDsBStatBake(ci, pc.texBytes, texels * 2, indexed && !ci && !sliceOk);
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
	pc.usedPal = usedPal;   // task 23: see gxDsBBakeBgPlane's identical field for the rationale
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

// Task 11: the extended OBJ palette slice a sprite samples, or NULL for the regular OBJ
// palette. Mirrors GPU.cpp exactly: `if (dispCnt->ExOBJPalette_Enable) pal =
// MMU.ObjExtPal[core][0] + PaletteIndex*0x200` in BOTH the rot/scale and the plain
// 256-colour branches of _spriteRender; the 16-colour branch never looks at the flag
// (it always indexes the regular OBJ palette by `PaletteIndex<<4`). A sprite has ONE
// fixed PaletteIndex, so its slice is a fixed 256-entry palette: no per-tile selection
// problem, unlike an extended BG palette.
static const u8 *gxDsBObjExtPalFor(const GxDsBObjDraw &d)
{
	if (!d.depth || !GXDS_SCREEN.gpu->dispx_st->dispx_DISPCNT.bits.ExOBJPalette_Enable)
		return NULL;
	return MMU.ObjExtPal[GXDS_CORE][0] + (u32)d.palIndex * 0x200;
}

// One sprite -> one w x h texture, unflipped (H/V flip is applied at draw time by
// swapping the quad's UVs, which is exactly the reversed traversal
// compute_sprite_vars() performs). Since task 11 it is native CI4 (16-colour, the
// sprite's own sub-palette as a 16-entry TLUT) / CI8 (256-colour, a 256-entry TLUT of the
// regular or extended OBJ palette). Address arithmetic transcribed from GPU.cpp's
// _spriteRender() non-rotozoomed 16/256-colour branches for both SPRITE_1D and
// SPRITE_2D mapping, including the per-row MMU_gpu_map() call the CPU makes (so a row
// that straddles a 16KB VRAM page behaves identically to the reference).
static void gxDsBBakeObjTexture(const GxDsBObjDraw &d, GxDsBObjTexSlot &slot)
{
	GPU *gpu = GXDS_SCREEN.gpu;
	const bool oneDim = (gpu->spriteRenderMode == GPU::SPRITE_1D);
	const int rowBytes = d.depth ? 8 : 4;
	u8 *const pal = MMU.ARM9_VMEM + kDsBObjPalOff;
	const u8 *const extPal = gxDsBObjExtPalFor(d);

	slot.valid = false;

	// Task 8: an affine sprite is sampled through an arbitrary matrix, so any
	// sample landing outside the w x h image must read transparent (the CPU
	// simply doesn't draw it). Same trick as the affine BG planes: bake inside
	// a 1-texel transparent border (index 0) and sample GX_CLAMP. The border is padded to
	// a power of two; a plain sprite keeps its exact-size, borderless bake so
	// task 7's output is untouched.
	const bool bordered = d.affine;
	const int off = bordered ? 1 : 0;
	const int bufW = bordered ? gxDsBPow2Ge(d.w + 2) : d.w;
	const int bufH = bordered ? gxDsBPow2Ge(d.h + 2) : d.h;
	if (bordered)
		memset(s_objBakeScratch, 0, (size_t)bufW * bufH * sizeof(u16)); // index 0 == transparent

	// Plain sprites: GPU.cpp resolves MMU_gpu_map() once per ROW (compute_
	// sprite_vars/_spriteRender's per-line `src = MMU_gpu_map(...)`), affine
	// sprites once per SPRITE (one `src = MMU_gpu_map(sprMem + (TileIndex <<
	// block))`, then flat offsets from that host pointer). Reproduce whichever
	// applies so a sprite whose data straddles two non-contiguous 16KB VRAM
	// pages reads the same bytes the reference does.
	const u8 *affBase = NULL;
	if (bordered)
		affBase = (const u8 *)MMU_gpu_map(gpu->sprMem + ((u32)d.tileIndex << (oneDim ? gpu->sprBoundary : 5)));

	// Task 10: read-set (see gxDsBDeps). Each row reads from `src` up to the byte
	// index of its last texel, which is a physical (LCDC-linear) span even where it
	// runs past the 16KB page `src` was mapped in.
	gxDsBRecBegin(&slot.deps);
	GxDsBRecGuard recGuard;
	u32 rowSpan;
	{
		const int xm = d.w - 1;
		if (d.depth) rowSpan = (u32)((xm & 7) + ((xm & 0xFFF8) << 3) + 1);
		else { const int x1 = xm >> 1; rowSpan = (u32)((x1 & 3) + ((x1 & 0xFFFC) << 3) + 1); }
	}
	if (d.depth) {
		if (extPal) gxDsBRecVram(extPal, 512);                          // task 11: the extended-palette slice
		else gxDsBRecPal(kDsBObjPalOff, 512);                           // 256-colour: the whole OBJ palette
	} else gxDsBRecPal(kDsBObjPalOff + (u32)d.palIndex * 32, 32);       // 16-colour: its own sub-palette

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
		gxDsBRecVram(src, rowSpan);
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
			out[x] = (u16)idx;   // 0 = transparent; else the index into this sprite's own (sub-)palette
		}
	}

	const u32 texels = (u32)bufW * bufH;
	bool ci = true;
#ifdef DSB_NOCI
	ci = false;
#endif
	slot.w = d.w;
	slot.h = d.h;
	if (ci) {
		if (!gxDsBEnsureTlut(&slot.tlutData))
			return;
		u16 *tlut = (u16 *)slot.tlutData;
		const int entries = d.depth ? 256 : 16;
		// Index-0 transparency, as the CPU tests `color != 0` per texel, is TLUT entry 0; the
		// affine border / padding is raw index 0 and so is transparent through the same entry.
		if (d.depth) gxDsBTlutFromPal(tlut, extPal ? extPal : pal, 256);
		else gxDsBTlutFromPal(tlut, pal + (u32)d.palIndex * 32, 16);
		DCFlushRange(slot.tlutData, (u32)entries * sizeof(u16));
		GX_InitTlutObj(&slot.tlutObj, slot.tlutData, GX_TL_RGB5A3, entries);
		slot.tlutEntries = (u16)entries;
		if (d.depth) {
			if (!gxDsBUploadCI8(&slot.texData, &slot.texDataCap, &slot.texBytes, s_objBakeScratch, bufW, bufH,
			                    &slot.texObj, GX_CLAMP, &slot.tlutObj, slot.tlutData, gxDsBObjSlotFor()))
				return;
		} else {
			const u32 needed = texels / 2;
			if (!gxDsBEnsureCap(&slot.texData, &slot.texDataCap, needed))
				return;
			gxDsBCompact8(s_objBakeScratch, texels);
			GXBlockShape blk = gxBlockShape(GXTEXFMT_CI4);
			gxSwizzle4bpp((const u8 *)s_objBakeScratch, (u8 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
			DCFlushRange(slot.texData, needed);
			GX_InitTexObjCI(&slot.texObj, slot.texData, bufW, bufH, GX_TF_CI4, GX_CLAMP, GX_CLAMP, GX_FALSE, gxDsBObjSlotFor());
			GX_InitTexObjFilterMode(&slot.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
			gxDsBMutantLoad(&slot.tlutObj, gxDsBObjSlotFor());
			slot.texBytes = needed;
		}
	} else {
		gxDsBResolveScratch(s_objBakeScratch, texels, d.depth ? (extPal ? extPal : pal) : pal + (u32)d.palIndex * 32);
		u32 needed = texels * sizeof(u16);
		if (!gxDsBEnsureCap(&slot.texData, &slot.texDataCap, needed))
			return;
		GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
		gxSwizzle16bpp(s_objBakeScratch, (u16 *)slot.texData, blk.texelsWide, blk.texelsTall, bufW, bufH);
		DCFlushRange(slot.texData, needed);
		GX_InitTexObj(&slot.texObj, slot.texData, bufW, bufH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&slot.texObj, GX_NEAR, GX_NEAR); // see gxDsBBakeBgPlane
		slot.texBytes = needed;
	}
	slot.ci = ci;
	gxDsBStatBake(ci, slot.texBytes, texels * 2, false);
	slot.bufW = (u16)bufW;
	slot.bufH = (u16)bufH;
	slot.cfgBordered = (u8)(bordered ? 1 : 0);
	slot.cfgTileIndex = d.tileIndex;
	slot.cfgPalIndex = d.palIndex;
	slot.cfgDepth = (u8)(d.depth ? 1 : 0);
	slot.cfgOneDim = (u8)(oneDim ? 1 : 0);
	slot.cfgBoundary = gpu->sprBoundary;
	slot.cfgExtPal = extPal;
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

#if GXDS_CORE == 0
// ---------------------------------------------------------------------
// Task 13: the 3D layer as a composited input texture (Engine A only)
// ---------------------------------------------------------------------
// The 3D scene is produced by the unchanged CPU software rasterizer (rasterize.cpp,
// including fog / edge marking / antialias / shadow polygons: all of that is already
// inside its output) into gfx3d_convertedScreen, 256x192 x 4 bytes, one buffer per
// rendered frame. GPU_RenderLine_layer reads it per line (gfx3d_GetLineData) when
// BG0_3D is set and BG0 is enabled, and _master_setFinal3dColor turns each texel into
//     present   iff alpha (byte 0) != 0
//     colour    R6G6B6TORGB15(red = byte 3, green = byte 2, blue = byte 1), | 0x8000
//     blended   iff alpha < 31 AND the pixel beneath came from a BLDCNT 2nd-target layer
//               (cfinal = (red6*(a+1) + (c2<<1)*(31-a)) >> 6, per channel, 5-bit result)
//     faded     otherwise, iff BG0 is a 1st target and the effect is brighten/darken
//               (the BLDCNT alpha effect is NOT applied to the 3D layer)
// and the layer is scrolled horizontally by BG0HOFS (pixel k shows 3D column
// (k+hofs)&0x1FF if that is <= 255, else nothing; vertical scroll is ignored).
// So the layer is an ordinary "BG0" plane with two differences, both modelled:
//   - it is baked here as a 256x256 (192 rows used) RGB5A3 texture from the SAME buffer (opaque RGB5A3 =
//     the R6G6B6TORGB15 value, transparent where alpha == 0), plus a brighten/darken
//     variant through the same fade tables as every other plane;
//   - the one thing a texture cannot express exactly is the blend against a 2nd-target
//     layer beneath (its per-channel rounding is not GX's SRC/INVSRC alpha), so a frame
//     with a translucent 3D texel (alpha 1..30) over any possible 2nd target BAILS to the
//     CPU compositor ("3dtrans", gxDsBPlanBand); translucent texels with no 2nd target
//     beneath are drawn opaque, which is exactly what the CPU does.
// Bake gating: the CPU never re-renders the 3D buffer except at VBlank-end when a
// SWAP_BUFFERS was pending (g_gfx3dRenderSeq bumps), so the linear copy, the
// translucency scan and both textures are keyed on that counter (+ the fade key);
// gxDsEngineAInvalidateAll() (reset / savestate load) drops them.
//
// gx-remaining-work.md section 1: `clearOnly` (the caller has established that this frame's
// 3D is drawn by the GX geometry pass on top, and the software raster is still deferred)
// scans a uniform clear-colour layer instead of gfx3d_convertedScreen, so the raster never
// runs. The deferred raster's buffer is the clear fragment everywhere except under the
// opaque polygons (gxDs3dGeomFrameSupported: no fog, edge marking, clear image or
// translucency), and those are exactly the pixels the geometry overlay then overwrites
// with opaque colour, BG0/3D being frontmost and untargeted (gxDsA3dOverlaySafe). The one
// difference is a pixel the CPU covers and GX does not (edge tie rules): it shows the
// clear colour instead of the CPU's polygon colour (a3_c28: 6 silhouette-edge pixels on
// top of the overlay's own 15; a3_c27: none; see the log's Task 1a-rasterskip section).
// That is a characterized, edge-bounded inexactness, so this is GxFast only: GxAccurate
// keeps the eager raster underneath the overlay. Every other consumer (a bail to the CPU compositor, savestates, capture) goes
// through gfx3d_ensureRendered() and gets the real raster.
//
// Task a3layer: a clear-only layer depends on nothing but the latched clear colour, so it is
// keyed on (clear colour, clear alpha) instead of the render counter: s_3dClearGen only
// advances when those change (or the scan was dropped), and the key (s_3dClearGen << 1) | 1
// keeps the linear copy, the line maps and every texture keyed on s_3dScanSeq across 3D
// frames. SM64DS GxFast otherwise re-scanned and re-baked the same uniform layer on every
// recording frame (~3.4 ms).
static bool gxDsA3dScan(bool clearOnly = false)
{
	static u32 s_3dClearGen, s_3dClearCc;
	u32 key = (g_gfx3dRenderSeq << 1);
	if (clearOnly) {
		const u32 cc = gfx3d_rasterClearColor() & 0x1F7FFF;   // the bits the clear line reads
		if (!(s_3dScanValid && s_3dClearOnly && s_3dClearCc == cc)) { ++s_3dClearGen; s_3dClearCc = cc; }
		key = (s_3dClearGen << 1) | 1u;
	}
	if (s_3dScanValid && s_3dScanSeq == key)
		return true;
	if (!s_3dLinear) {
		s_3dLinear = (u16 *)malloc((size_t)k3dW * k3dH * sizeof(u16));
		if (!s_3dLinear) return false;
	}
	s_3dClearOnly = clearOnly;
	if (clearOnly) {
		// rasterize.cpp SoftRastRender's clearFragmentColor, in gfx3d_convertedScreen's byte
		// order (big-endian FragmentColor: a, b, g, r).
		const u32 cc = gfx3d_rasterClearColor();
		const u8 t[4] = { (u8)((cc >> 16) & 0x1F), (u8)GFX3D_5TO6((cc >> 10) & 0x1F),
		                  (u8)GFX3D_5TO6((cc >> 5) & 0x1F), (u8)GFX3D_5TO6(cc & 0x1F) };
		for (int x = 0; x < k3dW; ++x) memcpy(s_3dClearLine + x * 4, t, 4);
	} else {
		gfx3d_ensureRendered();
	}
	u16 *o = s_3dLinear;
	for (int y = 0; y < k3dH; ++y) {
		u8 trans = 0, hole = 0;
		const u8 *c = clearOnly ? s_3dClearLine : gfx3d_convertedScreen + (size_t)y * k3dW * 4;
		for (int x = 0; x < k3dW; ++x, c += 4, ++o) {
			const u8 a = c[0];
			if (a == 0) { *o = 0; hole = 1; continue; }
			if (a < 31) trans = 1;
			const u8 r = c[3], g = c[2], b = c[1];
			*o = (u16)(0x8000 | (r >> 1) | ((g & 0x3E) << 4) | ((b & 0x3E) << 9));
		}
		s_3dLineTrans[y] = trans;
		s_3dLineHole[y] = hole;
	}
	s_3dScanSeq = key;
	s_3dScanValid = true;
#ifdef DSB_STATS
	if (!clearOnly) {
		u32 nonzeroA = 0;
		const u8 *cc = gfx3d_convertedScreen;
		for (int i = 0; i < k3dW * k3dH; ++i, cc += 4) if (cc[0]) ++nonzeroA;
		harness_profile_emitf(GXDS_TAG "scandbg nonzeroA=%u total=%d", nonzeroA, k3dW * k3dH);
	}
#endif
	return true;
}

// Bake (or reuse) one variant. `fadeMode`/`fadeLvl` as for every other plane (0/0 = plain).
static void gxDsA3dEnsureTex(GxDsA3dTex &t, int fadeMode, int fadeLvl)
{
	const u16 fx = (u16)((fadeMode << 8) | fadeLvl);
	if (t.valid && t.seq == s_3dScanSeq && t.cfgFx == fx)
		return;
	t.valid = false;
	const u32 needed = (u32)k3dW * k3dTexH * sizeof(u16);
	if (!gxDsBEnsureCap(&t.texData, &t.texDataCap, needed))
		return;
	if (!s_3dScratch) s_3dScratch = (u16 *)memalign(32, needed);   // 32-aligned: also gxDsA3dRenderGeomTex's copy target
	if (!s_3dScratch) return;
	memset(s_3dScratch + (size_t)k3dW * k3dH, 0, (size_t)k3dW * (k3dTexH - k3dH) * sizeof(u16));   // padding rows: transparent
	s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
	for (int i = 0; i < k3dW * k3dH; ++i) {
		const u16 v = s_3dLinear[i];
		s_3dScratch[i] = (v & 0x8000) ? gxDsBOpaqueTexel((u16)(v & 0x7FFF)) : kDsBTransparentTexel;
	}
	s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxSwizzle16bpp(s_3dScratch, (u16 *)t.texData, blk.texelsWide, blk.texelsTall, k3dW, k3dTexH);
	DCFlushRange(t.texData, needed);
	GX_InitTexObj(&t.texObj, t.texData, k3dW, k3dTexH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&t.texObj, GX_NEAR, GX_NEAR);
	t.seq = s_3dScanSeq;
	t.cfgFx = fx;
	t.valid = true;
}

// One band [y0,y1) of the 3D layer, horizontally scrolled by `hofs` (0..511): pixel k
// shows column q = (k+hofs)&511 when q < 256. That is at most two screen spans (q runs
// 0..255 for k in [0, 256-hofs) when hofs < 256 and again from k = 512-hofs when
// hofs > 256), each a plain 1:1 quad.
static void gxDsA3dDrawBand(const GxDsA3dTex &t, int hofs, int y0, int y1)
{
	GXTexObj *tex = const_cast<GXTexObj *>(&t.texObj);
	const f32 v0 = (f32)y0 / (f32)k3dTexH, v1 = (f32)y1 / (f32)k3dTexH;
	if (hofs < 256) {
		const int xe = 256 - hofs;              // screen [0, xe) shows columns [hofs, 256)
		gxDsBDrawQuad(tex, 0, (f32)y0, (f32)xe, (f32)y1, (f32)hofs / 256.0f, v0, 1.0f, v1);
	}
	if (hofs > 256) {
		const int xb = 512 - hofs;              // screen [xb, 256) shows columns [0, hofs-256)
		gxDsBDrawQuad(tex, (f32)xb, (f32)y0, 256.0f, (f32)y1, 0.0f, v0, (f32)(hofs - 256) / 256.0f, v1);
	}
}

// Task 13d: real per-texel alpha (0..31 -> 0..255, no quantization) as a standalone
// GX_TF_I8 texture, same 256x256-with-transparent-padding layout as s_3d/s_3dFx (see
// k3dTexH's comment). Only built when a band's two-pass draw actually needs it
// (gxDsBBuildUnder3DMask / gxDsA3dDrawBandTwoPass below), keyed on the same
// g_gfx3dRenderSeq as everything else derived from gfx3d_convertedScreen.
static void gxDsA3dEnsureAlphaTex()
{
	if (!gxDsA3dScan(s_3dClearOnly)) return;   // same source as this frame's scan
	if (s_3dAlphaValid && s_3dAlphaSeq == s_3dScanSeq)
		return;
	s_3dAlphaValid = false;
	const u32 needed = (u32)k3dW * k3dTexH * sizeof(u8);
	if (!gxDsBEnsureCap(&s_3dAlphaTexData, &s_3dAlphaTexDataCap, needed))
		return;
	if (!s_3dScratch) s_3dScratch = (u16 *)memalign(32, (size_t)k3dW * k3dTexH * sizeof(u16));
	if (!s_3dScratch) return;
	u8 *lin = (u8 *)s_3dScratch;   // reuse the scratch buffer's storage, byte-sized here
	for (int y = 0; y < k3dH; ++y) {
		// Same source gxDsA3dScan() read (its key is s_3dScanSeq): the real raster or the clear line.
		const u8 *c = s_3dClearOnly ? s_3dClearLine : gfx3d_convertedScreen + (size_t)y * k3dW * 4;
		for (int x = 0; x < k3dW; ++x, c += 4)
			// CPU blend weight for texel alpha a (1..31, a==0 is "absent", never
			// blended) is (a+1)/32 (GPU.cpp _master_setFinal3dColor:
			// (src*(a+1)+dst*2*(31-a))>>6); GX_BL_SRCALPHA's hardware weight is
			// alpha8/255, so alpha8 = round(255*(a+1)/32) is the matching 8-bit
			// value for a present texel, not a plain 5->8 bit expansion (which
			// targets a/31, not (a+1)/32 -- the two diverge most at small a).
			// a==0 (absent) must stay exactly 0, not (0+1)*255/32 -- the formula
			// only models a present, blendable texel.
			lin[y * k3dW + x] = c[0] ? (u8)(((unsigned)c[0] + 1) * 255 / 32) : 0;
	}
	memset(lin + (size_t)k3dW * k3dH, 0, (size_t)k3dW * (k3dTexH - k3dH));   // padding rows: alpha 0
	GXBlockShape blk = gxBlockShape(GXTEXFMT_I8);
	gxSwizzle8bpp(lin, (u8 *)s_3dAlphaTexData, blk.texelsWide, blk.texelsTall, k3dW, k3dTexH);
	DCFlushRange(s_3dAlphaTexData, needed);
	// I8, not GX_TF_A8: the latter is a copy-only format that samples as invalid hw format 7.
	GX_InitTexObj(&s_3dAlphaTexObj, s_3dAlphaTexData, k3dW, k3dTexH, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_3dAlphaTexObj, GX_NEAR, GX_NEAR);
	s_3dAlphaSeq = s_3dScanSeq;
	s_3dAlphaValid = true;
#ifdef DSB_STATS
	{
		u32 nonzero = 0;
		const u8 *ad = (const u8 *)s_3dAlphaTexData;
		for (u32 i = 0; i < needed; ++i) if (ad[i]) ++nonzero;
		harness_profile_emitf(GXDS_TAG "alphatexdbg nonzero=%u needed=%u", nonzero, needed);
	}
#endif
}
#endif // GXDS_CORE == 0

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
	u8 fade3d;             // task 13 (Engine A): the 3D layer needs its faded variant in this band
	u8 fade3dCfg;          // ... per window-region cfg (bit c): drawn faded there
	u8 mask3dCfg;          // task 13d: ... per window-region cfg (bit c): 2nd-target set beneath BG0's
	                       // 3D slot is a MIX in this region -> two-pass mask-and-discard draw instead
	                       // of the single-pass draw (see gxDsBBuildUnder3DMask / the bg==0+r.bg3d branch
	                       // of gxDsBDrawBgLayer)
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
	p.fade3d = 0;
	p.fade3dCfg = 0;
	p.mask3dCfg = 0;
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
#if GXDS_CORE == 0
				if (bg == 0 && r.bg3d) {
					// The 3D layer (_master_setFinal3dColor): the alpha EFFECT is never applied to it;
					// over a 2nd-target layer an opaque texel is written UNFADED and a translucent one
					// is blended; over anything else it is faded iff BG0 is a 1st target.
					// `under` is every layer that can lie beneath: none a 2nd target => always faded /
					// never blended; all of them, with no translucent texel => never faded (plain
					// opaque), a single draw suffices; a mix, OR a pure 2nd-target set with a
					// genuinely translucent line, needs a per-pixel choice a single draw mode can't
					// give (task 13d) -> two-pass mask-and-discard draw instead (gxDsBBuildUnder3DMask /
					// the bg==0+r.bg3d branch of gxDsBDrawBgLayer). Pass A (GX_BM_BLEND, masked to the
					// "beneath is 2nd target" pixels, real per-texel alpha) reproduces the blend rule
					// exactly (and degenerates to an exact opaque overwrite where the texel itself is
					// opaque, since srcAlpha=255 there); pass B (the mask's complement) reproduces the
					// "everywhere else" rule, still faded iff `fade` below is set.
					if (under & p.t2) {
						bool trans = false;
						int transLines = 0, holeLines = 0;
						for (int y = y0; y < y1; ++y) {
							if (s_3dLineTrans[y]) { trans = true; ++transLines; }
							if (s_3dLineHole[y]) ++holeLines;
						}
#ifdef DSB_STATS
						harness_profile_emitf(GXDS_TAG "transdbg under=%02x t2=%02x trans=%d transLines=%d holeLines=%d y0=%d y1=%d scanValid=%d scanSeq=%u renderSeq=%u",
						                       under, p.t2, (int)trans, transLines, holeLines, y0, y1,
						                       (int)s_3dScanValid, s_3dScanSeq, g_gfx3dRenderSeq);
#endif
						if ((under & ~p.t2) || trans) {
							// Task 13d-texfmt / nds-wii-render-pipeline.md "three runtime-switchable
							// render modes": the two-pass draw (gxDsBBuildUnder3DMask /
							// gxDsA3dDrawBandTwoPass) is structurally correct, but NOT exact: a
							// translucent 3D texel over a 2nd target comes out +-1 LSB vs the CPU
							// (GX's /255 SRCALPHA lerp on a 5-bit-quantized source vs the CPU's
							// truncating /32 lerp on 6-bit 3D colour). GxAccurate mode is
							// exact-or-bail, so this stays a bail there (byte-identical to the old
							// default-off DSA_MASK3D_ENABLE build); GxFast accepts the +-1 LSB to
							// stay on GX instead of bailing to CPU. DSA_MASK3D_ENABLE itself is gone
							// -- the two-pass code is always compiled in now, real and tested.
							if (gxRenderModeIsFast()) {
								p.mask3dCfg |= (u8)(1 << c);
							} else {
								if (trans) { DSB_WHY("3dtrans"); return false; }
								DSB_WHY("3dfade"); return false;
							}
						}
						else
							fade = false;
					}
					if (fade) { p.fade3d = 1; p.fade3dCfg |= (u8)(1 << c); }
					// Coverage: where the 3D layer has a texel it overwrites whatever was beneath
					// (bgPixels := 0), so if it covers EVERY pixel of this band (no scroll, no alpha-0
					// texel on any of its lines) nothing drawn before it can show through, and the layers
					// above it blend against the 3D layer alone: `under` restarts at {BG0}. This is what
					// lets SM64DS's BG3-over-3D alpha blend (BLDCNT 2nd target = BG0 only) be native.
					//
					// Task 13d follow-up (2026-09-22, "gx_mask-fix" investigation's second bug --
					// see gx-next-steps-log.md): that restart is only sound for the OLD single-pass
					// draw, where a "covered" band really does leave the 3D layer's own drawn colour
					// as the pixel's complete history (an unconditional REPLACE, or an unconditional
					// blend, with no per-pixel split). The two-pass MASKED draw exists precisely
					// because this band's under-stack is a MIX (some 2nd-target, some not --
					// `(under & ~p.t2)` above, the very condition that routed us into mask3dCfg in the
					// first place) -- "covered" says nothing about that mix, only that every pixel has
					// SOME 3D texel. Collapsing `under` to {BG0} here throws away which of BG1..BG3
					// were 2nd-target, so a layer drawn ABOVE BG0 that wants to blend gets its
					// `want && (under & p.t2)` check evaluated against the wrong (collapsed) set --
					// wrongly classified as non-alpha (or alpha) native-blend-eligible -- and ends up
					// drawing with the wrong TEV mode entirely. Root cause of the a3_c6/a3_c8
					// whole-frame corruption this task chased under a "TEV state leak" theory: it
					// is not a leaked GX register, it is this CPU-side plan miscomputing a later
					// layer's blend eligibility. Fix: only take the {BG0} shortcut when this cfg's
					// under-stack was NOT the mixed case mask3dCfg exists for; otherwise keep the
					// full accumulated `under` set (with BG0 added) so later layers see the real
					// 2nd-target membership of everything beneath them.
					bool covered = (r.hofs[0] == 0);
					for (int y = y0; covered && y < y1; ++y)
						if (s_3dLineHole[y]) covered = false;
					const bool mixedUnder = (p.mask3dCfg >> c) & 1;
					under = (covered && !mixedUnder) ? (u8)1 : (u8)(under | 1);
					continue;
				}
#endif
				if (fade) p.fadeBg |= (u8)(1 << bg);
				if (want && (under & p.t2)) {
					if (under & ~p.t2) {
						DSB_WHY("blendunder");
#ifdef DSB_STATS
						snprintf(s_dsbNote, sizeof(s_dsbNote), "bu bg=%d under=%02x t1=%02x t2=%02x en=%02x prio=%d%d%d%d 3d=%d obj=%d", bg, under, p.t1, p.t2, r.bgEnable, r.bgPrio[0], r.bgPrio[1], r.bgPrio[2], r.bgPrio[3], r.bg3d, r.objEnable);
#endif
						return false;
					}
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
#if defined(DSB_STATS) && GXDS_CORE == 0
	// Task 13d-statefix follow-up lead: dump the per-cfg planning outputs
	// (mask3dCfg, blendLive, fade3dCfg/fadeBg/fadeObj) directly, not just the
	// final draw-time `alpha` bool, to check for a planning miscomputation
	// beyond the already-tried `under`-collapse fix (see
	// gx-next-steps-log.md Task 13d-statefix's "Outcome" section).
	harness_profile_emitf(GXDS_TAG "planband y0=%d y1=%d mask3dCfg=%02x blendLive=%02x,%02x,%02x fade3dCfg=%02x fadeBg=%02x fadeObj=%d t1=%02x t2=%02x mode=%d bg0prio=%d",
	                       y0, y1, p.mask3dCfg, p.blendLive[0], p.blendLive[1], p.blendLive[2],
	                       p.fade3dCfg, p.fadeBg, (int)p.fadeObj, p.t1, p.t2, p.mode, r.bgPrio[0]);
#endif
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
	// s_tevK < 0 covers BOTH -1 (plain REPLACE wiring) and -2 (task 13d's mask
	// build / two-pass 3D draw, which deliberately parks s_tevK at -2 to force
	// a resync here -- see gxDsBTevMaskConst()'s and gxDsA3dDrawBandTwoPass()'s
	// comments). A guard of `== -1` alone missed the -2 case: after a two-pass
	// 3D draw, stage0 is left in pass B's plain-REPLACE wiring (alpha = raw
	// texture alpha, no A8_GT compare), and the next gxDsBTevAlpha() call would
	// take this fast path, update only the GX_TEVREG0 constant, and leave that
	// stale REPLACE alpha wiring in place -- silently breaking constant-alpha
	// blending for the next alpha-blended layer in the band (root cause of the
	// 13d whole-frame corruption even after the mask primitive itself was
	// fixed; see gx-next-steps-log.md's gx_mask-fix follow-up).
	if (s_tevK < 0) {
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
	if (pc.cfgWrap) {
		// Task 25 (root cause of lz_c10/lz_c17 GX != CPU): the host GPU samples NEGATIVE texture
		// coordinates inexactly under GX_NEAR + GX_REPEAT (measured on Dolphin: with V in
		// [-0.16, 1.4) a sparse set of pixels sitting 2/256 texel below a texel edge picked the
		// next texel; the same UVs shifted by +1 were byte-exact). With wrapping, adding whole
		// texture periods is exact, so lift every corner to >= 0 (interpolation is linear, so the
		// corners bound the whole quad).
		f32 mu = u[0], mv = v[0];
		for (int k = 1; k < 4; ++k) { if (u[k] < mu) mu = u[k]; if (v[k] < mv) mv = v[k]; }
		const f32 su = mu < 0.0f ? ceilf(-mu) : 0.0f, sv = mv < 0.0f ? ceilf(-mv) : 0.0f;
		for (int k = 0; k < 4; ++k) { u[k] += su; v[k] += sv; }
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
#if GXDS_CORE == 0
// Forward declarations: task 13d's two-pass mask-and-discard draw (defined below,
// after gxDsBDrawObjLayer -- it needs that function, and the BG/OBJ quad helpers
// above, to replay the under-stack) is called from inside gxDsBDrawBgLayer.
static void gxDsBBuildUnder3DMask(int b, const GxDsBBandRegs &r, const GxDsBBandPlan &p, int cfgIdx, int y0, int y1);
static void gxDsA3dDrawBandTwoPass(const GxDsA3dTex &tB, int hofs, int y0, int y1);
#endif

static void gxDsBDrawBgLayer(int b, int bg, const GxDsBBandRegs &r, const GxDsBBandPlan &p, int y0, int y1)
{
	for (int si = 0; si < p.nslab; ++si) {
		const GxDsBSlab &sl = p.slab[si];
		for (int ri = 0; ri < sl.nrun; ++ri) {
			const GxDsBRun &ru = sl.run[ri];
			const GxDsBCfg &cf = p.cfg[ru.cfg];
			if (!((cf.mask >> bg) & 1)) continue;
			bool fade, want;
			gxDsBResolveFx(p, cf, bg, false, fade, want);
#if GXDS_CORE == 0
			if (bg == 0 && r.bg3d) {   // task 13: the 3D layer (never alpha-blended by BLDCNT)
				if ((p.mask3dCfg >> ru.cfg) & 1) {   // task 13d: mixed under -> two-pass draw
					// Task 13d-bg13only (2026-09-22) bisection points, inert unless defined,
					// left in place for whoever picks this back up (see
					// gx-next-steps-log.md#task-13d-bg13only): DSA_MASK3D_BISECT_SKIP skips
					// gxDsBBuildUnder3DMask()+gxDsA3dDrawBandTwoPass() entirely (proved bad=0 on
					// a3_c25); DSA_MASK3D_BISECT_SKIP_DRAW keeps the mask build but skips the
					// two-pass draw call (also bad=0 -- mask build is NOT the corruptor);
					// DSA_MASK3D_BISECT_NOPASSA/NOPASSB (in gxDsA3dDrawTwoPassQuad below) isolate
					// pass A as the actual corruptor (pass A alone reproduces the FULL bad=57/60,
					// pass B alone is bad=0); DSA_MASK3D_BISECT_PASSA_NOCOLOR/_NOALPHA (also below)
					// found GX_SetColorUpdate(GX_FALSE) around pass A's own draw call alone makes
					// a3_c25 bad=0 again, even though the real alpha data feeding that draw is
					// independently proven all-zero (scandbg/alphatexdbg stats) -- i.e. pass A's
					// GX_BM_BLEND draw is NOT behaving as the mathematical no-op its own alpha=0
					// implies. None of these are defined by default, so the shipped (gated-off)
					// behaviour is unchanged.
#ifndef DSA_MASK3D_BISECT_SKIP
					if (s_3dMaskBand != b || s_3dMaskCfg != ru.cfg) {
						gxDsBBuildUnder3DMask(b, r, p, ru.cfg, y0, y1);
						s_3dMaskBand = b; s_3dMaskCfg = ru.cfg;
						GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
					}
#ifndef DSA_MASK3D_BISECT_SKIP_DRAW
					const GxDsA3dTex &tB = ((p.fade3dCfg >> ru.cfg) & 1) ? s_3dFx : s_3d;
					if (tB.valid) {
						GX_SetScissor(ru.x0, sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
						gxDsA3dDrawBandTwoPass(tB, (int)r.hofs[0] & 0x1FF, y0, y1);
					}
#endif
#endif
					continue;
				}
				const GxDsA3dTex &t3 = s_3dTexMode ? s_3dGx : ((p.fade3dCfg >> ru.cfg) & 1) ? s_3dFx : s_3d;
				if (!t3.valid) continue;
				gxDsBTevReplace();
				GX_SetScissor(ru.x0, sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
				gxDsA3dDrawBand(t3, (int)r.hofs[0] & 0x1FF, y0, y1);
				continue;
			}
#endif
			const bool alpha = want && ((p.blendLive[ru.cfg] >> bg) & 1);
			const GxDsBBgPlaneCache &pc = fade ? s_bgPlaneFx[bg] : s_bgPlane[bg];
			if (!pc.valid) continue;
			if (alpha) gxDsBTevAlpha(gxDsBAlphaK(p.eva)); else gxDsBTevReplace();
			if (pc.ci) gxDsBBindTlut(const_cast<GXTlutObj *>(&pc.tlutObj), gxDsBBgSlotFor(bg));
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
				if (slot.ci) gxDsBBindTlut(const_cast<GXTlutObj *>(&slot.tlutObj), gxDsBObjSlotFor());
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

#if GXDS_CORE == 0
// ---------------------------------------------------------------------
// Task 13d: two-pass mask-and-discard draw for BG0's 3D slot when the
// 2nd-target set beneath it is a per-pixel mix (or a pure 2nd-target set with
// a genuinely translucent 3D texel) -- see the bg==0+r.bg3d branch of
// gxDsBPlanBand above for the rule this reproduces and why a single draw
// mode can't do it.
// ---------------------------------------------------------------------

// Paints a layer's real shape (holes and all) into the mask scratch corner as
// a flat colour saying whether it is a 2nd target: colour = C0 (255 if t2, else
// 0), alpha = 255 where a texel is present (GX_TEV_COMP_A8_GT, as in
// gxDsBTevAlpha), 0 in holes, so the global SRCALPHA blend overwrites on a
// texel and leaves the lower layer's flag in a hole. The flag lives in colour
// because the EFB (RGB565_Z16 / RGB8_Z24) has no alpha channel; the mask is
// copied out with GX_CTF_R8 (see gx_mask.h). Leaves s_tevK = -2 so the next
// gxDsBTevReplace()/gxDsBTevAlpha() fully reprograms stage0.
static void gxDsBTevMaskConst(bool t2)
{
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_A0, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	const u8 v = t2 ? 255 : 0;
	GX_SetTevColor(GX_TEVREG0, (GXColor){ v, v, v, 255 });
	s_tevK = -2;   // neither TevReplace's (-1) nor a TevAlpha k: force a resync next use
}

// Replays exactly the layers gxDsBPlanBand's `under` walk enumerates as lying
// beneath BG0's 3D slot in window-region `cfgIdx` of this band (backdrop, then
// priority 3..0 -- BG3..BG0 then that tier's OBJ -- stopping just before BG0's
// own priority tier's BG0/OBJ, the same stop point `under` is read at), but
// into the mask scratch corner with each layer's real shape (its true holes)
// painted a flat 2nd-target-or-not colour instead of its real colour. Always
// the *plain* (non-fade) texture cache: only presence/absence and the t2 bit
// matter here, and using the already-baked plain cache avoids depending on the
// fade variant being baked too (it may not be, if nothing else in the band
// needs it).
static void gxDsBDrawBgLayerForMask(int bg, const GxDsBBandRegs &r, const GxDsBBandPlan &p, int cfgIdx, int y0, int y1)
{
	const GxDsBCfg &cf = p.cfg[cfgIdx];
	if (!((cf.mask >> bg) & 1)) return;
	const GxDsBBgPlaneCache &pc = s_bgPlane[bg];
	if (!pc.valid) return;
	gxDsBTevMaskConst((bool)((p.t2 >> bg) & 1));
	if (pc.ci) gxDsBBindTlut(const_cast<GXTlutObj *>(&pc.tlutObj), gxDsBBgSlotFor(bg));
	for (int si = 0; si < p.nslab; ++si) {
		const GxDsBSlab &sl = p.slab[si];
		if (sl.y1 <= y0 || sl.y0 >= y1) continue;
		for (int ri = 0; ri < sl.nrun; ++ri) {
			const GxDsBRun &ru = sl.run[ri];
			if (ru.cfg != cfgIdx) continue;
			GX_SetScissor(k3dMaskEfbX + ru.x0, k3dMaskEfbY + sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
			if (r.bgType[bg] == (u8)BGType_Text) gxDsBDrawBgQuad(pc, r, bg, y0, y1);
			else gxDsBDrawAffineBgQuad(pc, r, bg, y0, y1);
		}
	}
}

static void gxDsBDrawObjLayerForMask(int prio, const GxDsBBandPlan &p, int cfgIdx, int y0, int y1)
{
	const GxDsBCfg &cf = p.cfg[cfgIdx];
	if (!((cf.mask >> 4) & 1)) return;
	for (int i = s_objDrawCount - 1; i >= 0; --i) {
		const GxDsBObjDraw &d = s_objDraws[i];
		if (d.priority != prio) continue;
		if (d.y >= y1 || d.y + d.boxH <= y0) continue;
		bool inCfg = false;
		for (int si = 0; si < p.nslab && !inCfg; ++si) {
			const GxDsBSlab &sl = p.slab[si];
			if (d.y >= sl.y1 || d.y + d.boxH <= sl.y0) continue;
			for (int ri = 0; ri < sl.nrun; ++ri) {
				const GxDsBRun &ru = sl.run[ri];
				if (ru.cfg != cfgIdx) continue;
				if (d.x >= ru.x1 || d.x + d.boxW <= ru.x0) continue;
				inCfg = true; break;
			}
		}
		if (!inCfg) continue;
		const GxDsBObjTexSlot &slot = s_objTex[d.oamIndex];
		if (!slot.valid) continue;
		gxDsBTevMaskConst((bool)((p.t2 >> 4) & 1));
		if (slot.ci) gxDsBBindTlut(const_cast<GXTlutObj *>(&slot.tlutObj), gxDsBObjSlotFor());
		for (int si = 0; si < p.nslab; ++si) {
			const GxDsBSlab &sl = p.slab[si];
			if (d.y >= sl.y1 || d.y + d.boxH <= sl.y0) continue;
			for (int ri = 0; ri < sl.nrun; ++ri) {
				const GxDsBRun &ru = sl.run[ri];
				if (ru.cfg != cfgIdx) continue;
				if (d.x >= ru.x1 || d.x + d.boxW <= ru.x0) continue;
				GX_SetScissor(k3dMaskEfbX + ru.x0, k3dMaskEfbY + sl.y0, ru.x1 - ru.x0, sl.y1 - sl.y0);
				if (d.affine) { gxDsBDrawAffineObj(d, slot); continue; }
				f32 s0 = d.hflip ? 1.0f : 0.0f, s1 = d.hflip ? 0.0f : 1.0f;
				f32 t0 = d.vflip ? 1.0f : 0.0f, t1 = d.vflip ? 0.0f : 1.0f;
				gxDsBDrawQuad(const_cast<GXTexObj *>(&slot.texObj), (f32)d.x, (f32)d.y,
				              (f32)(d.x + d.w), (f32)(d.y + d.h), s0, t0, s1, t1);
			}
		}
	}
}

// Builds (into s_3dMask) the per-pixel "was the EFB pixel beneath BG0's 3D
// slot produced by a BLDCNT 2nd-target layer" mask for band `b`'s window-region
// `cfgIdx`, by replaying that under-stack into the scratch EFB corner with the
// gx_mask.h primitive. Leaves viewport/scissor pointed at the scratch corner;
// callers restore both before drawing anything else (gxMaskEnd() only undoes
// the copy-src wiring, per its own contract).
static void gxDsBBuildUnder3DMask(int b, const GxDsBBandRegs &r, const GxDsBBandPlan &p, int cfgIdx, int y0, int y1)
{
	if (!s_3dMaskInit) {
		s_3dMaskInit = gxMaskTarget_Init(&s_3dMask, kDsBScreenW, kDsBScreenH, GX_CTF_R8);
		if (!s_3dMaskInit) return;
	}
	gxMaskBegin(&s_3dMask, k3dMaskEfbX, k3dMaskEfbY);

	// Backdrop: always beneath everything, never windowed, never has a hole.
	gxDsBTevMaskConst((bool)((p.t2 >> 5) & 1));
	GX_SetScissor(k3dMaskEfbX, k3dMaskEfbY + y0, kDsBScreenW, y1 - y0);
	gxDsBDrawQuad(&s_backdropTexObj[b], 0, (f32)y0, (f32)kDsBScreenW, (f32)y1, 0, 0, 1, 1);

	const int bg0Prio = r.bgPrio[0];
	for (int prio = 3; prio >= bg0Prio; --prio) {
		for (int bg = 3; bg >= 0; --bg) {
			if (prio == bg0Prio && bg == 0) break;   // stop at BG0/3D's own slot -- `under` is read just before it
			if (!((r.bgEnable >> bg) & 1) || r.bgPrio[bg] != prio) continue;
			gxDsBDrawBgLayerForMask(bg, r, p, cfgIdx, y0, y1);
		}
		if (prio != bg0Prio && r.objEnable)
			gxDsBDrawObjLayerForMask(prio, p, cfgIdx, y0, y1);
	}

	gxMaskEnd(&s_3dMask, k3dMaskEfbX, k3dMaskEfbY);
	s_tevK = -2;   // gxDsBTevMaskConst() calls above never kept s_tevK in sync
#ifdef DSB_STATS
	{
		const u8 *td = (const u8 *)s_3dMask.texData;
		u32 zero = 0, full = 0, other = 0;
		for (u32 i = 0; i < (u32)kDsBScreenW * kDsBScreenH; ++i) {
			u8 v = td[i];
			if (v == 0) ++zero; else if (v == 255) ++full; else ++other;
		}
		harness_profile_emitf(GXDS_TAG "mask3d b=%d cfg=%d bg0prio=%d zero=%u full=%u other=%u t2=%02x",
		                       b, cfgIdx, r.bgPrio[0], zero, full, other, (unsigned)p.t2);
	}
#endif
}

// Adds an explicit second per-vertex UV attribute (GX_VA_TEX1 -> GX_TEXCOORD1,
// identity texgen -- the same pass-through wiring gxDsBSetup2DState() already
// uses for TEXCOORD0/TEX0) for the mask sample: always plain absolute
// screen-pixel/[0,1] UV, independent of whatever scrolled/split UV the 3D
// texture itself is using at TEXCOORD0/TEX0. An explicit attribute rather
// than a GX_TG_POS-derived texgen so this only depends on the same
// vertex-attribute pass-through gxDsBDrawQuad's TEXCOORD0 already relies on
// (proven correct everywhere else in this file), not on a second, untested
// mechanism. Callers must GX_SetNumTexGens(1) and GX_SetVtxDesc(GX_VA_TEX1,
// GX_NONE) back down once done; nothing else in the per-band draw loop
// expects a second texcoord to be live.
static void gxDsBSetupMaskTexCoord1()
{
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
}

// Same screen rectangle as gxDsBDrawQuad, but also emits a second UV
// (mask-space, plain absolute screen-pixel/[0,1]) at GX_VA_TEX1/TEXCOORD1.
static void gxDsBDrawQuadMasked(GXTexObj *tex, f32 x0, f32 y0, f32 x1, f32 y1,
                                 f32 s0, f32 t0, f32 s1, f32 t1)
{
	const f32 mu0 = x0 / (f32)kDsBScreenW, mu1 = x1 / (f32)kDsBScreenW;
	const f32 mv0 = y0 / (f32)kDsBScreenH, mv1 = y1 / (f32)kDsBScreenH;
	GX_LoadTexObj(tex, GX_TEXMAP0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x0, y0); GX_TexCoord2f32(s0, t0); GX_TexCoord2f32(mu0, mv0);
		GX_Position2f32(x1, y0); GX_TexCoord2f32(s1, t0); GX_TexCoord2f32(mu1, mv0);
		GX_Position2f32(x1, y1); GX_TexCoord2f32(s1, t1); GX_TexCoord2f32(mu1, mv1);
		GX_Position2f32(x0, y1); GX_TexCoord2f32(s0, t1); GX_TexCoord2f32(mu0, mv1);
	GX_End();
}

// The two-pass draw itself: pass A (GX_BM_BLEND, masked to "beneath is a 2nd
// target", real per-texel alpha from s_3dAlphaTexObj) then pass B (the mask's
// complement, plain/fade opaque overwrite, same presence-only alpha the
// existing single-pass gxDsA3dDrawBand uses). Same hofs quad-splitting as
// gxDsA3dDrawBand (called per screen-space sub-quad, up to two).
static void gxDsA3dDrawTwoPassQuad(const GxDsA3dTex &tB, f32 x0, f32 y0, f32 x1, f32 y1,
                                    f32 s0, f32 t0, f32 s1, f32 t1)
{
	// Pass A: colour = s_3d (never faded -- the CPU never fades a blended 3D
	// texel), alpha = real per-texel alpha (s_3dAlphaTexObj) masked to the
	// "beneath is 2nd target" pixels. GX_BM_BLEND (global, SRCALPHA/INVSRCALPHA)
	// then reproduces the CPU's blend formula for a translucent texel, and
	// degenerates to an exact opaque overwrite for an opaque one (srcAlpha=255).
	// Elsewhere (mask=0, or the 3D texel itself absent) alpha=0 -> no-op, left
	// for pass B.
	GX_SetNumTevStages(3);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLORNULL);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	gxMaskApply(&s_3dMask, GX_TEVSTAGE2, GX_TEXMAP2, GX_TEXCOORD1);

	GX_LoadTexObj(const_cast<GXTexObj *>(&s_3dAlphaTexObj), GX_TEXMAP1);
#ifndef DSA_MASK3D_BISECT_NOPASSA
#ifdef DSA_MASK3D_BISECT_PASSA_NOCOLOR
	GX_SetColorUpdate(GX_FALSE);
#endif
#ifdef DSA_MASK3D_BISECT_PASSA_NOALPHA
	GX_SetAlphaUpdate(GX_FALSE);
#endif
	gxDsBDrawQuadMasked(const_cast<GXTexObj *>(&s_3d.texObj), x0, y0, x1, y1, s0, t0, s1, t1);
#ifdef DSA_MASK3D_BISECT_PASSA_NOCOLOR
	GX_SetColorUpdate(GX_TRUE);
#endif
#ifdef DSA_MASK3D_BISECT_PASSA_NOALPHA
	GX_SetAlphaUpdate(GX_TRUE);
#endif
#endif

	// Pass B: colour+presence-alpha = tB (plain or fade, caller's choice), then
	// multiplied by the mask's complement -- everywhere pass A didn't already
	// (correctly) claim.
	GX_SetNumTevStages(2);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	gxMaskApplyInverse(&s_3dMask, GX_TEVSTAGE1, GX_TEXMAP2, GX_TEXCOORD1);
#ifndef DSA_MASK3D_BISECT_NOPASSB
	gxDsBDrawQuadMasked(const_cast<GXTexObj *>(&tB.texObj), x0, y0, x1, y1, s0, t0, s1, t1);
#endif
}

static void gxDsA3dDrawBandTwoPass(const GxDsA3dTex &tB, int hofs, int y0, int y1)
{
	gxDsA3dEnsureAlphaTex();
#ifdef DSB_STATS
	harness_profile_emitf(GXDS_TAG "mask3ddraw avalid=%d tbvalid=%d s3valid=%d hofs=%d y0=%d y1=%d",
	                       (int)s_3dAlphaValid, (int)tB.valid, (int)s_3d.valid, hofs, y0, y1);
#endif
	if (!s_3dAlphaValid || !tB.valid || !s_3d.valid) return;
	gxDsBSetupMaskTexCoord1();
	const f32 v0 = (f32)y0 / (f32)k3dTexH, v1 = (f32)y1 / (f32)k3dTexH;
	if (hofs < 256) {
		const int xe = 256 - hofs;
		gxDsA3dDrawTwoPassQuad(tB, 0, (f32)y0, (f32)xe, (f32)y1, (f32)hofs / 256.0f, v0, 1.0f, v1);
	}
	if (hofs > 256) {
		const int xb = 512 - hofs;
		gxDsA3dDrawTwoPassQuad(tB, (f32)xb, (f32)y0, 256.0f, (f32)y1, 0.0f, v0, (f32)(hofs - 256) / 256.0f, v1);
	}
	// Restore the single-texgen / single-tev-stage / single-UV baseline
	// gxDsBSetup2DState() established: nothing else in the per-band draw loop
	// expects a second texcoord/texgen or more than one TEV stage to be live.
	GX_SetNumTevStages(1);
	GX_SetNumTexGens(1);
	GX_SetVtxDesc(GX_VA_TEX1, GX_NONE);
	s_tevK = -2;
}

// Task 13d follow-up (2026-09-22, "gx_mask-fix" investigation's second bug --
// see gx-next-steps-log.md): pass A's GX_BM_BLEND(SRCALPHA,INVSRCALPHA) blend
// of a GENUINELY translucent 3D texel (real per-texel alpha 1..254, not the
// old code's binary present/absent) mathematically leaves the EFB's OWN alpha
// channel below 255 at that pixel -- out_A = srcA*(srcA/255) + dstA*(1-srcA/255),
// which is < 255 for any dstA==255, 0<srcA<255. Every OTHER alpha-blended draw
// in this renderer (gxDsBTevAlpha's ordinary BG/OBJ blending) gets away with
// the exact same math because something opaque is painted on top of it later
// in painter's-algorithm order, which re-stamps that pixel's alpha back to 255
// (srcA=255 -> out_A = 255*1 + dst*0 = 255) before the frame's single
// GX_CopyTex readback below. BG0's 3D slot has no such guarantee -- it can be
// (and in a3_c6/a3_c8 IS) the topmost thing drawn for a pixel, so its
// sub-255 EFB alpha survives all the way to the copy. GX's own RGB5A3 EFB
// copy format silently drops to half precision (4-4-4 colour + 3-bit alpha,
// bit15=0) for any texel whose EFB alpha isn't 255, instead of the full 5-5-5
// (bit15=1) encoding -- exactly the "flat suspicious constant" symptom
// (dsaabd's gx=001f: small colour components rounding down to 0 in 4-bit).
// Fix: right after the whole frame's 2D/3D draw loop, before the single
// end-of-frame EFB->texture copy, stamp the EFB alpha channel back to a flat
// 255 everywhere (colour update off, so this cannot touch any pixel's colour,
// only its alpha) -- the same "make sure the EFB is fully opaque before this
// copy reads it" invariant every OTHER pixel already gets for free from a
// later opaque draw. Gated with the rest of 13d's two-pass machinery: the
// ordinary single-pass 3D draw and non-3D layers never leave fractional EFB
// alpha as the LAST word on a pixel, so this is a no-op cost the rest of the
// renderer does not need.
static void gxDsA3dStampOpaqueAlpha()
{
#ifdef DSB_STATS
	// Task 13d-blendleaklead (2026-09-22): confirms this runs every frame
	// (87/90 a3_c8 frames) -- see gx-next-steps-log.md for why that matters
	// (this pass proved the stamp's own effect on `dsaabd`'s printed value
	// is unmeasurable, not that it never executes).
	harness_profile_emitf(GXDS_TAG "stampalpha");
#endif
	GX_SetColorUpdate(GX_FALSE);
	GX_SetTevColor(GX_TEVREG0, (GXColor){ 0, 0, 0, 255 });
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(0, 0);                             GX_TexCoord2f32(0, 0);
		GX_Position2f32((f32)kDsBScreenW, 0);               GX_TexCoord2f32(0, 0);
		GX_Position2f32((f32)kDsBScreenW, (f32)kDsBScreenH); GX_TexCoord2f32(0, 0);
		GX_Position2f32(0, (f32)kDsBScreenH);               GX_TexCoord2f32(0, 0);
	GX_End();
	GX_SetColorUpdate(GX_TRUE);
	s_tevK = -2;   // stage0 wiring left non-standard -- force a resync next use
}
#endif // GXDS_CORE == 0

// Bake-if-stale for one BG plane variant (plain, or the brighten/darken one).
// Stale = never baked / VRAMCNT remap / config fingerprint mismatch / any byte of the
// recorded read-set written since (task 10; replaces the coarse whole-region flags).
static void gxDsBEnsureBgPlane(int bg, GxDsBBgPlaneCache &pc, int fadeMode, int fadeLvl,
                               bool bankChanged)
{
	GPU *gpu = GXDS_SCREEN.gpu;
	const _BGxCNT &cnt = gpu->dispx_st->dispx_BGxCNT[bg].bits;
	const BGType type = gpu->BGTypes[bg];
	const bool affine = (type != BGType_Text);
	u32 auxBase = 0;
	if (type == BGType_Large8bpp) auxBase = gpu->BG_bmp_large_ram[bg];
	else if (type == BGType_AffineExt_256x1 || type == BGType_AffineExt_Direct) auxBase = gpu->BG_bmp_ram[bg];
	const u16 fx = (u16)((fadeMode << 8) | fadeLvl);
	bool needBake = !pc.valid || bankChanged || pc.cfgFx != fx ||
	                pc.wpx != (u16)gpu->BGSize[bg][0] || pc.hpx != (u16)gpu->BGSize[bg][1] ||
	                pc.cfgTileBase != gpu->BG_tile_ram[bg] || pc.cfgMapBase != gpu->BG_map_ram[bg] ||
	                pc.cfgColorMode != (u8)(cnt.Palette_256 ? 1 : 0) ||
	                pc.cfgScreenSize != (u8)cnt.ScreenSize ||
	                pc.cfgType != (u8)type || pc.cfgExtPal != gxDsBExtPalFor(bg) ||
	                (affine && (pc.cfgWrap != (u8)(cnt.PaletteSet_Wrap ? 1 : 0) || pc.cfgAuxBase != auxBase));
	// Task 23: a config-fingerprint-clean, still-valid, CI cache whose recorded VRAM
	// read-set is untouched but whose recorded palette read-set was written needs
	// only its TLUT rebuilt, not a full re-bake (re-decode + re-swizzle + re-upload
	// of the index texture, which cannot have changed: the index texture never
	// encodes colour). A non-CI cache (RGB5A3 fallback: direct-colour or multi-slice
	// extended palette) has no separable TLUT and always takes the full path below,
	// same as before this task.
	if (!needBake) {
		bool vramDirty = false, palDirty = false;
		gxDsBDepsDirtySplit(pc.deps, &vramDirty, &palDirty);
		if (vramDirty) {
			needBake = true;
		} else if (palDirty) {
			if (pc.ci) {
#ifdef DSB_STATS
				const u64 t0 = gettime();
#endif
				PZ_SUB_SCOPE(GXDS_CORE == 0 ? PZ_2DA_BAKE_BG : PZ_2DB_BAKE_BG);
				gxDsBRebuildBgTlut(pc, gxDsBBgSlotFor(bg));
#ifdef DSB_STATS
				s_dsbTlutOnlyTicks += gettime() - t0;
				++s_dsbTlutOnlyBg;
#endif
			} else {
				needBake = true;
			}
		}
	}
#ifdef DSB_VERIFY
	// Test hook: a plane the gate says is still fresh is re-baked into a scratch copy
	// and compared byte-for-byte; any difference is a missed dependency.
	if (!needBake && pc.valid) {
		GxDsBBgPlaneCache tmp; memset(&tmp, 0, sizeof(tmp));
		s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
		if (affine) gxDsBBakeAffineBgPlane(bg, tmp); else gxDsBBakeBgPlane(bg, tmp);
		s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
		if (tmp.valid && (tmp.bufW != pc.bufW || tmp.bufH != pc.bufH || tmp.ci != pc.ci || tmp.texBytes != pc.texBytes ||
		                  memcmp(tmp.texData, pc.texData, tmp.texBytes) != 0 ||
		                  (tmp.ci && memcmp(tmp.tlutData, pc.tlutData, 256 * sizeof(u16)) != 0)))
			++s_dsbStale;
		free(tmp.texData); free(tmp.tlutData);
	}
#endif
	if (!needBake) return;
#ifdef DSB_STATS
	if (fadeMode) ++s_dsbBgFxBake[bg]; else ++s_dsbBgBake[bg];
#endif
	s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
#ifdef DSB_STATS
	const u64 t0 = gettime();
#endif
	PZ_SUB_SCOPE(GXDS_CORE == 0 ? PZ_2DA_BAKE_BG : PZ_2DB_BAKE_BG);
	if (affine) gxDsBBakeAffineBgPlane(bg, pc);
	else gxDsBBakeBgPlane(bg, pc);
#ifdef DSB_STATS
	s_dsbBakeTicks += gettime() - t0;
#endif
	s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
}

static void gxDsBEnsureObjTex(const GxDsBObjDraw &d, GxDsBObjTexSlot &slot, int fadeMode, int fadeLvl,
                              bool bankChanged)
{
	GPU *gpu = GXDS_SCREEN.gpu;
	const u8 oneDim = (u8)((gpu->spriteRenderMode == GPU::SPRITE_1D) ? 1 : 0);
	const u16 fx = (u16)((fadeMode << 8) | fadeLvl);
	bool needBake = !slot.valid || bankChanged || slot.cfgFx != fx ||
	                slot.w != d.w || slot.h != d.h ||
	                slot.cfgTileIndex != d.tileIndex ||
	                slot.cfgPalIndex != d.palIndex ||
	                slot.cfgDepth != (u8)(d.depth ? 1 : 0) ||
	                slot.cfgOneDim != oneDim ||
	                slot.cfgBoundary != gpu->sprBoundary ||
	                slot.cfgExtPal != gxDsBObjExtPalFor(d) ||
	                slot.cfgBordered != (u8)(d.affine ? 1 : 0);
	// Task 23: same palette-only fast path as gxDsBEnsureBgPlane() above.
	if (!needBake) {
		bool vramDirty = false, palDirty = false;
		gxDsBDepsDirtySplit(slot.deps, &vramDirty, &palDirty);
		if (vramDirty) {
			needBake = true;
		} else if (palDirty) {
			if (slot.ci) {
#ifdef DSB_STATS
				const u64 t0 = gettime();
#endif
				PZ_SUB_SCOPE(GXDS_CORE == 0 ? PZ_2DA_BAKE_OBJ : PZ_2DB_BAKE_OBJ);
				gxDsBRebuildObjTlut(slot, gxDsBObjSlotFor());
#ifdef DSB_STATS
				s_dsbTlutOnlyTicks += gettime() - t0;
				++s_dsbTlutOnlyObj;
#endif
			} else {
				needBake = true;
			}
		}
	}
#ifdef DSB_VERIFY
	if (!needBake && slot.valid) {
		GxDsBObjTexSlot tmp; memset(&tmp, 0, sizeof(tmp));
		s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
		gxDsBBakeObjTexture(d, tmp);
		s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
		if (tmp.valid && (tmp.bufW != slot.bufW || tmp.bufH != slot.bufH || tmp.ci != slot.ci || tmp.texBytes != slot.texBytes ||
		                  memcmp(tmp.texData, slot.texData, tmp.texBytes) != 0 ||
		                  (tmp.ci && memcmp(tmp.tlutData, slot.tlutData, (u32)tmp.tlutEntries * sizeof(u16)) != 0)))
			++s_dsbStale;
		free(tmp.texData); free(tmp.tlutData);
	}
#endif
	if (!needBake) return;
#ifdef DSB_STATS
	if (fadeMode) ++s_dsbObjFxBake[d.oamIndex]; else ++s_dsbObjBake[d.oamIndex];
#endif
	s_bakeFadeMode = fadeMode; s_bakeFadeLvl = fadeLvl;
#ifdef DSB_STATS
	const u64 t0 = gettime();
#endif
	PZ_SUB_SCOPE(GXDS_CORE == 0 ? PZ_2DA_BAKE_OBJ : PZ_2DB_BAKE_OBJ);
	gxDsBBakeObjTexture(d, slot);
#ifdef DSB_STATS
	s_dsbBakeTicks += gettime() - t0;
#endif
	s_bakeFadeMode = 0; s_bakeFadeLvl = 0;
}

#if GXDS_CORE == 0
// Task 13e: the GX geometry pass is drawn after every band, so it is only correct
// where the 3D layer is the frontmost, unmodified layer everywhere: BG0 is the 3D
// layer in every band, unscrolled, nothing (BG1-3 or a sprite of its tier or nearer)
// is drawn after it, no window can hide it, and no colour effect targets it.
static bool gxDsA3dOverlaySafe(int bandCount)
{
	for (int b = 0; b < bandCount; ++b) {
		const GxDsBBandRegs &r = GXDS_REGS[b];
		if (!r.bg3d || !(r.bgEnable & 1) || (r.hofs[0] & 0x1FF) || r.winEn) return false;
		const u8 p0 = r.bgPrio[0];
		for (int bg = 1; bg < 4; ++bg)
			if (((r.bgEnable >> bg) & 1) && r.bgPrio[bg] < p0) return false;
		if (r.objEnable)
			for (int i = 0; i < s_objDrawCount; ++i)
				if (s_objDraws[i].priority <= p0) return false;
		if (((r.bldcnt >> 6) & 3) && (r.bldcnt & 1)) return false;   // BG0/3D is a 1st target
	}
	return true;
}

// gx-remaining-work.md section 1: the geometry pass drawn at the 3D layer's real priority.
// Frames that fail gxDsA3dOverlaySafe (a BG/OBJ above BG0, a window, a scroll, BG0 targeted)
// render the pass first into the EFB corner the bands later use, on top of the same BG0
// texture the overlay would have had beneath it (s_3d: the clear colour, or the CPU raster in
// GxAccurate / a resolved frame), copy that out as s_3dGx and draw it in BG0's slot like s_3d.
// Per pixel it is exactly the overlay's BG0 result, so the band plan's rules for s_3d carry
// over, with the scan's per-line flags (s_3dLineTrans/Hole, from the same s_3d source) as the
// layer's: GX polygons only add opaque coverage (translucent ones exist only with clear alpha
// 31, i.e. no hole anywhere), so a line the scan calls covered stays covered. What the texture
// can't carry is the CPU-side fade bake (s_3dFx) and the 13d two-pass draw (s_3dAlphaTex from
// the scan), so the caller drops back to s_3d if the plan wants either, or a translucent line.
// Coverage: the EFB here is RGB8 (or RGB565) with no alpha, which is fine when s_3d has no
// alpha-0 texel (copied alpha is 255); with holes the pass runs in GX_PF_RGBA6_Z24 so the hole
// texels keep alpha 0 through the copy. RGBA6 truncates colour to 6 bits, which keeps the top
// 5 the RGB5A3 copy takes, and no blending happens in that case (translucent polygons need a
// hole-free clear). nds-wii-render-pipeline.md deviation: Stage 3 renders to texture, but the
// target is the Engine A EFB corner itself, copied before the 2D bands overwrite it.
static bool gxDsA3dEnsureGeomTex()
{
	const u32 bytes = (u32)k3dW * k3dTexH * sizeof(u16);
	if (!s_3dScratch) s_3dScratch = (u16 *)memalign(32, bytes);   // the copy's staging buffer
	if (!s_3dScratch) return false;
	if (s_3dGx.texData) return true;
	s_3dGx.texData = memalign(32, bytes);
	if (!s_3dGx.texData) return false;
	memset(s_3dGx.texData, 0, bytes);   // rows 192.. stay transparent padding (never copied into)
	DCFlushRange(s_3dGx.texData, bytes);
	GX_InitTexObj(&s_3dGx.texObj, s_3dGx.texData, k3dW, k3dTexH, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s_3dGx.texObj, GX_NEAR, GX_NEAR);
	s_3dGx.valid = true;
	return true;
}

// The geometry pass itself (overlay and texture forms). Leaves the 2D state set up.
static void gxDsA3dGeomDraw()
{
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);
	if (gxRenderModeIsFast()) {
		gxDs3dRenderFast();
		// Task 13f-fog: GxFast-only post-process (gxDs3dGeomFrameSupported() has
		// already confirmed fog is on, every in-scope polygon has it enabled, and
		// we're in Fast mode -- see gx_ds_3d_render.h's comment on this function).
		if (gfx3d.enableFog)
			gxDs3dApplyFogFast();
	} else {
		gxDs3dRenderAccurate();
	}
	// The 3D pass leaves a perspective projection, a POS+CLR vertex format, Z on and
	// blending off; everything after this (TevReplace, the GxFast alpha stamp, the
	// copy) assumes the 2D pass's state.
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	gxDsBSetup2DState();
	s_tevK = -2;
}

// Under vidmutex, before the bands. See gxDsA3dEnsureGeomTex's comment.
static void gxDsA3dRenderGeomTex()
{
	bool holes = false;
	for (int y = 0; y < k3dH && !holes; ++y) holes = s_3dLineHole[y] != 0;
#ifdef DSA_GXTEX_MUTATE_RGB8
	holes = false;   // mutation: hole texels come out opaque, must fail a hole fixture
#endif
	if (holes) GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);
	// s_3d as the pass's background, alpha included (no blend: an alpha-0 texel writes alpha 0).
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	s_tevK = -1;
	gxDsA3dDrawBand(s_3d, 0, 0, k3dH);
	gxDsA3dGeomDraw();
	{ PZ_SUB_SCOPE(PZ_GX_WAIT); GX_DrawDone(); }
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetTexCopySrc(0, 0, kDsBScreenW, kDsBScreenH);
	GX_SetTexCopyDst(kDsBScreenW, kDsBScreenH, GX_TF_RGB5A3, GX_FALSE);
	// Dolphin serves a texture overlapping an EFB copy from its own GPU-side copy, which keeps
	// the EFB's 8-bit colour instead of the RGB5A3 quantization (measured: a3_c42 GxAccurate,
	// 1097 px of BG3-over-3D blend off by 1 step against the identical CPU-baked texture). Real
	// hardware reads the quantized RAM, but everything here is verified in Dolphin, so the copy
	// lands in s_3dScratch (free again once s_3d is baked; fade and 13d, its other users,
	// never run in texture mode) and the CPU moves it into the texture: exact in both, at the
	// cost of a GX_DrawDone and a 96 KB copy. -DDSA_GXTEX_DIRECTCOPY copies straight into the
	// texture instead (right on hardware, not in Dolphin).
#ifdef DSA_GXTEX_DIRECTCOPY
	GX_CopyTex(s_3dGx.texData, GX_FALSE);   // the bands' backdrop quads repaint every pixel
	GX_PixModeSync();
#else
	{
		const u32 cb = (u32)k3dW * k3dH * sizeof(u16);
		DCFlushRange(s_3dScratch, cb);   // no dirty bake line may be written back over the copy
		GX_CopyTex(s_3dScratch, GX_FALSE);   // the bands' backdrop quads repaint every pixel
		GX_PixModeSync();
		{ PZ_SUB_SCOPE(PZ_GX_WAIT); GX_DrawDone(); }
		DCInvalidateRange(s_3dScratch, cb);
		memcpy(s_3dGx.texData, s_3dScratch, cb);
		DCFlushRange(s_3dGx.texData, cb);
	}
#endif
	if (holes) GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);   // main.cpp InitVideo's choice
	GX_InvalidateTexAll();
}
#endif

static bool gxDsBRenderFrameImpl()
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
			GXDS_PLAN.bands.beginFrame();
			s_frameSawLine0 = false;
			s_frameOutOfScope = true;
		}
	} bandResetter;

#ifdef GXDS_FORCE_CPU
	return false;
#endif
	// nds-wii-render-pipeline.md "three runtime-switchable render modes": Software mode
	// bails before any GX work, uniformly across both DS engine instances (this .inc is
	// shared by Engine A and Engine B), same effect as DSA_FORCE_CPU/DSB_FORCE_CPU but
	// selectable at runtime. Must precede everything below -- no GX state may be touched.
	if (gxRenderModeIsSoftware())
		return false;
	if (gameInfo.isGBA)
		return false;
#ifdef DSB_STATS
	bool statsOk = false;
	s_dsbFrWin = s_dsbFrFade = s_dsbFrAlpha = s_dsbFrSemi = s_dsbFrMb = false;
	s_dsbFr3d = false;
	struct StatsEmit {
		bool *ok;
		~StatsEmit() {
			if (*ok) {
				++s_dsbOk;
				s_dsbWinOk += s_dsbFrWin; s_dsbFadeOk += s_dsbFrFade; s_dsbAlphaOk += s_dsbFrAlpha;
				s_dsbSemiOk += s_dsbFrSemi; s_dsbMbOk += s_dsbFrMb;
				s_dsb3dOk += s_dsbFr3d;
			} else {
				++s_dsbBail;
				for (int i = 0; i < 24; ++i) {
					if (!s_dsbWhyHist[i].why) { s_dsbWhyHist[i].why = s_dsbWhy; s_dsbWhyHist[i].n = 1; break; }
					if (!strcmp(s_dsbWhyHist[i].why, s_dsbWhy)) { ++s_dsbWhyHist[i].n; break; }
				}
			}
			if (s_dsb3dSeen) { ++s_dsb3dEnabled; s_dsb3dSeen = false; }
			if (((s_dsbOk + s_dsbBail) % 30) == 0) {
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
				harness_profile_emitf(GXDS_TAG " n=%u ok=%u bail=%u affbg=%u affobj=%u win=%u fade=%u alpha=%u semi=%u mb=%u d3=%u d3en=%u dispcnt=%08x types=%04x why=%s %s",
				                      (unsigned)(s_dsbOk + s_dsbBail), (unsigned)s_dsbOk, (unsigned)s_dsbBail,
				                      (unsigned)s_dsbAffOk, (unsigned)s_dsbAffObjOk,
				                      (unsigned)s_dsbWinOk, (unsigned)s_dsbFadeOk, (unsigned)s_dsbAlphaOk,
				                      (unsigned)s_dsbSemiOk, (unsigned)s_dsbMbOk, (unsigned)s_dsb3dOk, (unsigned)s_dsb3dEnabled,
				                      (unsigned)s_dsbDispcnt, (unsigned)s_dsbTypes, s_dsbWhy, s_dsbNote);
				{
					char h[192]; int hn = 0;
					for (int i = 0; i < 24 && s_dsbWhyHist[i].why && hn < 150; ++i)
						hn += snprintf(h + hn, sizeof(h) - hn, "%s=%u ", s_dsbWhyHist[i].why, (unsigned)s_dsbWhyHist[i].n);
					harness_profile_emitf(GXDS_TAG "w n=%u %s", (unsigned)(s_dsbOk + s_dsbBail), h);
				}
#ifdef DSB_ABCMP
				harness_profile_emitf(GXDS_TAG "ab n=%u exact=%u alpha=%u bad=%u firstbad=%u mn=%d mp=%d", (unsigned)(s_dsbOk + s_dsbBail),
				                      (unsigned)s_abExact, (unsigned)s_abAlpha, (unsigned)s_abBad, (unsigned)s_abFirstBad, s_abMn, s_abMp);
#endif
				u32 ot = 0, oft = 0;
				for (int i = 0; i < 128; ++i) { ot += s_dsbObjBake[i]; oft += s_dsbObjFxBake[i]; }
				harness_profile_emitf(GXDS_TAG "k n=%u bg=%u/%u/%u/%u fxbg=%u/%u/%u/%u obj=%u fxobj=%u o=%u,%u,%u,%u,%u,%u,%u,%u old_bg=%u old_obj=%u stale=%u",
				                      (unsigned)(s_dsbOk + s_dsbBail),
				                      (unsigned)s_dsbBgBake[0], (unsigned)s_dsbBgBake[1], (unsigned)s_dsbBgBake[2], (unsigned)s_dsbBgBake[3],
				                      (unsigned)s_dsbBgFxBake[0], (unsigned)s_dsbBgFxBake[1], (unsigned)s_dsbBgFxBake[2], (unsigned)s_dsbBgFxBake[3],
				                      (unsigned)ot, (unsigned)oft,
				                      (unsigned)s_dsbObjBake[0], (unsigned)s_dsbObjBake[1], (unsigned)s_dsbObjBake[2], (unsigned)s_dsbObjBake[3],
				                      (unsigned)s_dsbObjBake[4], (unsigned)s_dsbObjBake[5], (unsigned)s_dsbObjBake[6], (unsigned)s_dsbObjBake[7],
				                      (unsigned)s_dsbOldBg, (unsigned)s_dsbOldObj, (unsigned)s_dsbStale);
				{
					// task 11: texture memory. live = every cached texture currently valid, `rgb` = what the
					// same textures would occupy as pre-resolved RGB5A3 (the pre-task-11 form).
					u32 live = 0, rgb = 0, nci = 0, ntex = 0;
					for (int i = 0; i < 4; ++i) {
						const GxDsBBgPlaneCache *c[2] = { &s_bgPlane[i], &s_bgPlaneFx[i] };
						for (int k = 0; k < 2; ++k) if (c[k]->valid) { live += c[k]->texBytes; rgb += (u32)c[k]->bufW * c[k]->bufH * 2; nci += c[k]->ci; ++ntex; }
					}
					for (int i = 0; i < 128; ++i) {
						const GxDsBObjTexSlot *c[2] = { &s_objTex[i], &s_objTexFx[i] };
						for (int k = 0; k < 2; ++k) if (c[k]->valid) { live += c[k]->texBytes; rgb += (u32)c[k]->bufW * c[k]->bufH * 2; nci += c[k]->ci; ++ntex; }
					}
					harness_profile_emitf(GXDS_TAG "m tex=%u ci=%u live=%u rgb5a3equiv=%u bakes_ci=%u bakes_rgb=%u fallback=%u baked_bytes=%u baked_equiv=%u tlutloads=%u bake_us=%u tlut_only_bg=%u tlut_only_obj=%u tlut_only_us=%u",
					                      (unsigned)ntex, (unsigned)nci, (unsigned)live, (unsigned)rgb,
					                      (unsigned)s_dsbBakeCi, (unsigned)s_dsbBakeRgb, (unsigned)s_dsbBakeFallback,
					                      (unsigned)s_dsbBakedBytes, (unsigned)s_dsbBakedBytesRgbEquiv, (unsigned)s_dsbTlutLoads,
					                      (unsigned)ticks_to_microsecs(s_dsbBakeTicks),
					                      (unsigned)s_dsbTlutOnlyBg, (unsigned)s_dsbTlutOnlyObj,
					                      (unsigned)ticks_to_microsecs(s_dsbTlutOnlyTicks));
				}
#endif
			}
		}
	} statsEmit = { &statsOk };
#endif
	if (!s_frameSawLine0 || s_frameOutOfScope)
		return false;
#if GXDS_LAZY_COMPILED && !defined(DSLZ_NODATABAIL)
	// Task 13j: a VRAM / palette / OAM write this engine can see landed while the visible frame was
	// in flight. The eager CPU compositor saw it only on the lines drawn after it; GX bakes the
	// frame-final data for the whole frame. That is the "inherited mid-frame VRAM write limitation"
	// (transient bad frames); the barrier already rendered the earlier lines with the old data, so
	// the exact answer is simply the CPU result.
	if (s_lzDataHit) {
#ifdef DSLZ_STATS
		++s_lzStDataBail;
#endif
		DSB_WHY("midframewrite");
		return false;
	}
#endif
	if (!GXDS_SCREEN.gpu)
		return false;
	if (!GXDS_API(RenderInit)())
		return false;
	if (vidmutex == LWP_MUTEX_NULL)
		return false;

	// A frame with more layout changes than GxBandTracker can record would
	// have its excess boundaries merged into the last band (gx_frameplan.cpp
	// documents that as a safe degrade for the GBA's rarer case). For a DS
	// engine, per-scanline HDMA scroll is common enough that silently
	// merging would mean visibly wrong output, so bail instead.
	if (GXDS_PLAN.bands.boundaryCount() >= GxBandTracker::kMaxBoundaries) {
		DSB_WHY("bandoverflow");
		return false;
	}

	if (!gxDsBCollectVisibleObj()) {
		DSB_WHY("obj");
		return false;
	}

	int bandStarts[GxBandTracker::kMaxBands], bandEnds[GxBandTracker::kMaxBands];
	int bandCount = GXDS_PLAN.bands.finalize(kDsBScreenH, bandStarts, bandEnds);

	GPU *gpu = GXDS_SCREEN.gpu;
	bool bgEnabledAnyBand[4] = { false, false, false, false };
	bool objEnabledAnyBand = false;
	bool any3dBand = false;   // task 13: some band draws BG0 as the 3D layer (Engine A)
	(void)any3dBand;
	for (int b = 0; b < bandCount; ++b) {
		for (int bg = 0; bg < 4; ++bg)
			if ((GXDS_REGS[b].bgEnable >> bg) & 1) {
#if GXDS_CORE == 0
				if (bg == 0 && GXDS_REGS[b].bg3d) {
					// BG0 as the 3D layer reads no tile/map/palette memory (GPU_RenderLine_layer
					// `continue`s before modeRender) so it must not bake or dirty-gate a text plane.
					any3dBand = true;
					continue;
				}
#endif
				bgEnabledAnyBand[bg] = true;
				// A plane is baked once, from the frame-final BGxCNT/mode. A
				// BG whose type changed between bands (a mid-frame BG_Mode
				// switch) can't be replayed from that one bake -> bail.
				if (GXDS_REGS[b].bgType[bg] != (u8)gpu->BGTypes[bg]) {
					DSB_WHY("bgtypechange");
					return false;
				}
			}
		if (GXDS_REGS[b].objEnable) objEnabledAnyBand = true;
	}

	// Task 9: plan + validate every band before any GX work or cache mutation,
	// so a bail is always clean. Brighten/darken planes/sprites are baked into a
	// single per-frame variant (one (mode,level) key), so bands needing
	// different fade keys for layers bail.
	u8 fadeBgAny = 0, fadeObjAny = 0;
	bool fade3dAny = false;
	(void)fade3dAny;
	int fadeMode = 0, fadeLvl = 0;
#if GXDS_CORE == 0
	// The 3D layer's translucency map (per line) feeds gxDsBPlanBand's exactness check, so
	// the scan runs first; it is a pure read of gfx3d_convertedScreen (cached on the
	// rasterizer's render counter).
	// gx-remaining-work.md section 1: decided here, before the scan, because a frame the
	// geometry pass draws scans a clear-colour layer and never runs the deferred raster.
	bool geomPass = false, geomOverlay = false;
	s_3dTexMode = false;
	bool pzGate;
	{ PZ_SUB_SCOPE(PZ_GX3D_GATE); pzGate = any3dBand && !gxRenderModeIsSoftware() && gxDs3dGeomFrameSupported(); }
	if (pzGate) {
		geomOverlay = gxDsA3dOverlaySafe(bandCount);
		// Not frontmost/untargeted: the pass is drawn to a texture in BG0's slot instead
		// (gxDsA3dRenderGeomTex), unless the plan below needs what only s_3d can give.
		s_3dTexMode = !geomOverlay && gxDsA3dEnsureGeomTex();
		geomPass = geomOverlay || s_3dTexMode;
	}
#endif
	for (;;) {
#if GXDS_CORE == 0
		// GxFast only (the raster is only ever deferred in GxFast; the mode check covers a switch since).
		// A frame with translucent polygons (GxFast) always takes the clear-only layer: the GX pass
		// blends them over it, and a raster resolved early (e.g. a bail's GetLineData) has them in already.
		bool pzScanOk;
		{
			PZ_SUB_SCOPE(PZ_2DA_3DSCAN);
			pzScanOk = !any3dBand || gxDsA3dScan(geomPass && gxRenderModeIsFast() &&
			                                     (gfx3d_renderDeferred() || gxDs3dGeomFrameHasTranslucent()));
		}
		if (!pzScanOk) { DSB_WHY("3dscan"); return false; }
#endif
		fadeBgAny = fadeObjAny = 0; fade3dAny = false; fadeMode = fadeLvl = 0;
		bool texModeOk = true;
		(void)texModeOk;
		for (int b = 0; b < bandCount; ++b) {
			if (!gxDsBPlanBand(GXDS_REGS[b], bandStarts[b], bandEnds[b], s_bandPlan[b]))
				return false;
			const GxDsBBandPlan &pl = s_bandPlan[b];
#if GXDS_CORE == 0
			if (pl.fade3d || pl.mask3dCfg) texModeOk = false;
#endif
			if (pl.fadeBg || pl.fadeObj || pl.fade3d) {
				if (fadeMode && (fadeMode != pl.mode || fadeLvl != pl.evy)) { DSB_WHY("fadekeys"); return false; }
				fadeMode = pl.mode; fadeLvl = pl.evy;
				fadeBgAny |= pl.fadeBg; fadeObjAny |= pl.fadeObj;
				if (pl.fade3d) fade3dAny = true;
			}
		}
#if GXDS_CORE == 0
		if (s_3dTexMode)
			for (int y = 0; y < k3dH && texModeOk; ++y) if (s_3dLineTrans[y]) texModeOk = false;
		if (s_3dTexMode && !texModeOk) {
			// s_3dGx can't stand in for s_3d here (see gxDsA3dEnsureGeomTex): plain CPU 3D
			// layer, re-scanned from the real raster if the first scan was clear-only.
			s_3dTexMode = false; geomPass = false;
			continue;
		}
#endif
		break;
	}
#if GXDS_CORE == 0
#ifdef DSA_GXGEOM_DEBUGWHY
	if (any3dBand) harness_profile_emitf("gxds3dmode overlay=%d tex=%d", (int)geomOverlay, (int)s_3dTexMode);
#endif
#endif
	// A fade/alpha item is only ever drawn if its layer is enabled in the band
	// (gxDsBPlanBand walked enabled layers only), so no extra gating is needed.

	// Stage 1 (task 10): per-plane / per-slot dependency gates.
	//
	// A cached texture is stale iff (a) a VRAMCNT remap (or reset / savestate load)
	// changed what the bake's addresses resolve to, (b) its config fingerprint no
	// longer matches the live registers / OAM entry, or (c) a byte of the read-set
	// recorded at its last bake (LCDC-space VRAM pages + palette pages) was written
	// since. The dirty bitmaps are cleared at the end of every successful frame, so
	// EVERY cached texture must be checked every frame, whether or not this frame
	// draws it: a plane/slot that is disabled / invisible / not needing its fade
	// variant this frame would otherwise miss a write made in this interval, keep
	// `valid` and show stale texels when it is next used (task 9 closed that gap
	// coarsely; the same rule is what makes the precise gates safe). Such a texture
	// is INVALIDATED here, and re-baked on demand.
	bool bankChanged = gxDsBBankMapChanged();
#ifdef DSB_MUTATE_NOBANK
	bankChanged = false; s_forceRebakeAll = false;   // mutation test: VRAMCNT remaps go unnoticed (case 2 must FAIL)
#endif
	if (s_forceRebakeAll) { bankChanged = true; s_forceRebakeAll = false; }
#ifdef DSB_STATS
	{
		// What the pre-task-10 whole-region gate would have re-baked this frame.
		const bool oldGate = bankChanged || s_dsbOldVramArm || GXDS_PLAN.palette.anyDirty();
		if (oldGate) for (int bg = 0; bg < 4; ++bg) if (bgEnabledAnyBand[bg]) ++s_dsbOldBg;
		if (objEnabledAnyBand && (oldGate || GXDS_PLAN.oam.anyDirty())) s_dsbOldObj += (u32)s_objDrawCount;
	}
#endif

	for (int b = 0; b < bandCount; ++b) {
		PZ_SUB_SCOPE(PZ_2D_BAKE_BD);
		gxDsBBakeBackdrop(b, s_bandPlan[b].backdropFade ? s_bandPlan[b].mode : 0, s_bandPlan[b].evy);
	}

#if GXDS_CORE == 0
	// Task 13: the 3D layer's textures (see gxDsA3dEnsureTex). Not dirty-gated against 2D VRAM
	// (it reads none); a stale variant (unused this frame) is simply re-keyed on its next use.
	if (any3dBand) {
		PZ_SUB_SCOPE(PZ_2DA_3DTEX);
		gxDsA3dEnsureTex(s_3d, 0, 0);
		if (fade3dAny) gxDsA3dEnsureTex(s_3dFx, fadeMode, fadeLvl);
	}
#endif

	for (int bg = 0; bg < 4; ++bg) {
		GxDsBBgPlaneCache &pl0 = s_bgPlane[bg], &pl1 = s_bgPlaneFx[bg];
		if (bgEnabledAnyBand[bg]) gxDsBEnsureBgPlane(bg, pl0, 0, 0, bankChanged);
		else GX_DSB_DROP_IF_STALE(pl0.valid, bankChanged, pl0.deps);
		if (bgEnabledAnyBand[bg] && ((fadeBgAny >> bg) & 1)) gxDsBEnsureBgPlane(bg, pl1, fadeMode, fadeLvl, bankChanged);
		else GX_DSB_DROP_IF_STALE(pl1.valid, bankChanged, pl1.deps);
	}
	{
		bool visible[128] = { false };
		if (objEnabledAnyBand) {
			for (int i = 0; i < s_objDrawCount; ++i) {
				const GxDsBObjDraw &d = s_objDraws[i];
				visible[d.oamIndex] = true;
				gxDsBEnsureObjTex(d, s_objTex[d.oamIndex], 0, 0, bankChanged);
				if (fadeObjAny) gxDsBEnsureObjTex(d, s_objTexFx[d.oamIndex], fadeMode, fadeLvl, bankChanged);
				else GX_DSB_DROP_IF_STALE(s_objTexFx[d.oamIndex].valid, bankChanged, s_objTexFx[d.oamIndex].deps);
			}
		}
		for (int i = 0; i < 128; ++i) {
			if (visible[i]) continue;
			GxDsBObjTexSlot &a = s_objTex[i], &f = s_objTexFx[i];
			GX_DSB_DROP_IF_STALE(a.valid, bankChanged, a.deps);
			GX_DSB_DROP_IF_STALE(f.valid, bankChanged, f.deps);
		}
	}

	// ---- Stage 4 + EFB copy, under vidmutex ----------------------------
	// draw_thread (main.cpp) pushes its own present quads into this same GX FIFO
	// from the video thread and is already serialized against Draw() by this
	// mutex; taking it here extends that same contract to cover Engine B's
	// composite pass rather than inventing a new one.
	{ PZ_SUB_SCOPE(PZ_VIDLOCK); LWP_MutexLock(vidmutex); }

	gxDsBSetup2DState();
	gxDsBTlutReset();   // task 11: assume nothing about TMEM's TLUT contents from an earlier frame / another engine
	s_tevK = -1;
#if GXDS_CORE == 0
	s_3dMaskBand = -1; s_3dMaskCfg = -1;   // task 13d: this frame's mask cache is empty
#endif
#if GXDS_CORE == 0
	if (s_3dTexMode) { PZ_SUB_SCOPE(PZ_2DA_3DGEOMTEX); gxDsA3dRenderGeomTex(); }   // the pass first, as BG0's texture (s_3dGx)
#endif
	GX_SetViewport(0, 0, (f32)kDsBScreenW, (f32)kDsBScreenH, 0, 1);
	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);

	for (int b = 0; b < bandCount; ++b) {
		const GxDsBBandRegs &r = GXDS_REGS[b];
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
				gxDsBDrawBgLayer(b, bg, r, pl, y0, y1);
			}
			if (r.objEnable)
				gxDsBDrawObjLayer(prio, pl, y0, y1);
		}
	}

#if GXDS_CORE == 0
	// Task 13e: GX geometry pass. Drawn as a direct EFB overlay on top of the finished
	// bands (the CPU-rasterizer 3D texture was already drawn in BG0's slot beneath it),
	// so it only engages when the 3D layer is the frontmost, unmodified thing on screen
	// (gxDsA3dOverlaySafe) and the polylist is in the slice's scope
	// (gxDs3dGeomFrameSupported); otherwise that CPU texture is the 3D layer, as before.
#ifdef DSA_GXGEOM_DEBUGWHY
	harness_profile_emitf("gxds3dgate any3dBand=%d sw=%d", (int)any3dBand, (int)gxRenderModeIsSoftware());
#endif
	if (geomOverlay) {
		gxDsA3dGeomDraw();
#ifdef DSA_GXGEOM_DEBUGWHY
		{
			{ PZ_SUB_SCOPE(PZ_GX_WAIT); GX_DrawDone(); }
			GXColor peek;
			GX_PeekARGB(132, 107, &peek);
			harness_profile_emitf("gxds3dPEEK (132,107) argb=%d,%d,%d,%d", peek.a, peek.r, peek.g, peek.b);
			GX_PeekARGB(10, 10, &peek);
			harness_profile_emitf("gxds3dPEEK (10,10) argb=%d,%d,%d,%d", peek.a, peek.r, peek.g, peek.b);
		}
#endif
	}
#endif

	gxDsBTevReplace();

	GX_SetScissor(0, 0, kDsBScreenW, kDsBScreenH);
#if GXDS_CORE == 0
	// Task 13d follow-up: re-opaque the EFB alpha channel before the frame's
	// one EFB->texture copy below -- see gxDsA3dStampOpaqueAlpha()'s comment.
	// Only meaningful (and only needed) when the two-pass mask3d draw actually ran this
	// frame, i.e. GxFast mode; gxDsA3dStampOpaqueAlpha() itself is a no-op cost-wise
	// otherwise but the mode check keeps GxAccurate's GX call sequence byte-identical to
	// the pre-switch DSA_MASK3D_ENABLE-off build.
	if (gxRenderModeIsFast())
		gxDsA3dStampOpaqueAlpha();
#endif
	{ PZ_SUB_SCOPE(PZ_GX_WAIT); GX_DrawDone(); }
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
	// Task mbright: MASTER_BRIGHT on the GPU (gxDsPresentMasterBright: per-line exact TLUT
	// lookup of the EFB's own channels), so fade frames present directly too.
	bool anyMb = false;
	for (int y = 0; y < kDsBScreenH; ++y)
		if (s_mbMode[y] != 0 && s_mbFac[y] != 0) { anyMb = true; break; }
	bool mbOnGpu = false;
#ifndef DSB_ABCMP
	if (anyMb) {
		mbOnGpu = gxDsPresentMasterBright(s_mbMode, s_mbFac);
		s_tevK = -2;   // stage0 wiring left non-standard -- force a resync next use
	}
#endif
	GX_SetTexCopySrc(0, 0, kDsBScreenW, kDsBScreenH);
	GX_SetTexCopyDst(kDsBScreenW, kDsBScreenH, GX_TF_RGB5A3, GX_FALSE);
	// Task directpresent: a GPU_screen slot may still point at last frame's copy (e.g. the
	// other slot after a screen swap); read it back before the copy below overwrites it.
	// Task lazyfix: not this frame's own slot -- every path below rewrites it whole (pending
	// copy or eager readback), so last frame's copy there is dropped unconverted.
	PZ_SUB_SCOPE(PZ_EFB_COPY);   // to the end of the function; the readbacks below re-zone themselves
	const int slot = GXDS_SCREEN.offset ? 1 : 0;
	gxDsPresentReleaseBuffer(s_copyBackBuf, slot);
	// clear=GX_TRUE (task 12): wipe the 256x192 EFB corner just drawn into so it
	// does not ghost under draw_thread's present (visible in the live window).
	GX_CopyTex(s_copyBackBuf, GX_TRUE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	// Put back everything gxDsBSetup2DState() + the copy-src/dst setup above
	// changed, before draw_thread's next present pass can observe it.
	GxRestorePresentState();

	LWP_MutexUnlock(vidmutex);

	// Readback + MASTER_BRIGHT. GPU_RenderLine_MasterBrightness() runs on the
	// finished 15-bit line, so applying the same per-channel tables here (with the
	// per-line mode/factor sampled by Stage 0) is exact -- no GX pass, no rounding.
	// Task directpresent: the readback is deferred (gx_ds_present.h): Draw() presents
	// s_copyBackBuf as is and GPU_screen is converted only if something reads it.
	// Task mbright: MASTER_BRIGHT frames too once the fade is in the copy (mbOnGpu); the
	// old eager CPU path stays only if that pass could not run (no buffers) and for the
	// in-process A/B. The copy is always opaque: the EFB is RGB8_Z24 / RGB565_Z16 here
	// (no alpha, so the copy's alpha is 255), which made the old per-frame scan redundant.
	u16 *dst = (u16 *)(GPU_screen + (u32)GXDS_SCREEN.offset * 512);
#ifndef DSB_ABCMP
	if (!anyMb || mbOnGpu) {
		gxDsPresentSet(slot, s_copyBackBuf);
	} else
#endif
	{
	PZ_SUB_SCOPE(PZ_2D_READBACK);
	g_gxDsPresentPending[slot] = NULL;
	GXBlockShape blk = gxBlockShape(GXTEXFMT_RGB5A3);
	gxUnswizzle16bpp((const u16 *)s_copyBackBuf, s_copyBackLinear, blk.texelsWide, blk.texelsTall, kDsBScreenW, kDsBScreenH);

#ifdef DSB_ABCMP
	memcpy(s_abCpu, dst, sizeof(s_abCpu));
#endif
	for (int y = 0; y < kDsBScreenH; ++y) {
		const u16 *src = s_copyBackLinear + (u32)y * kDsBScreenW;
		u16 *out = dst + (u32)y * kDsBScreenW;
		const int mode = s_mbMode[y], fac = s_mbFac[y];
		if (mode == 0 || fac == 0) {
			for (int x = 0; x < kDsBScreenW; ++x)
				out[x] = gxDsBRgb5a3ToNds(src[x]);
			continue;
		}
		if (fac == 16) {
			const u16 v = (mode == 1) ? 0x7FFF : 0;
			for (int x = 0; x < kDsBScreenW; ++x) out[x] = v;
			continue;
		}
		for (int x = 0; x < kDsBScreenW; ++x)
			out[x] = gxDsBFadeColor(gxDsBRgb5a3ToNds(src[x]) & 0x7FFF, mode + 1 /* 1->2 up, 2->3 down */, fac);
	}
	}

#ifdef DSB_ABCMP
	{
		int fmn = 0, fmp = 0;
		for (int i = 0; i < 256 * 192; ++i) {
			const u16 g = dst[i] & 0x7FFF, c = s_abCpu[i] & 0x7FFF;   // bit 15 is only the CPU compositor's "written" flag
			if (g == c) continue;
			for (int sh = 0; sh < 15; sh += 5) {
				const int d = (int)((g >> sh) & 31) - (int)((c >> sh) & 31);
				if (d < fmn) fmn = d;
				if (d > fmp) fmp = d;
			}
		}
		if (fmn < s_abMn) s_abMn = fmn;
		if (fmp > s_abMp) s_abMp = fmp;
		if (fmn == 0 && fmp == 0) ++s_abExact;
		else if (fmn >= 0 && fmp <= 2) ++s_abAlpha;
		else {
			if (!s_abBad) {
				s_abFirstBad = (u32)(s_dsbOk + s_dsbBail);
				int cnt = 0, shown = 0, bx0 = 999, by0 = 999, bx1 = -1, by1 = -1;
				for (int i = 0; i < 256 * 192; ++i) {
					if ((dst[i] & 0x7FFF) == (s_abCpu[i] & 0x7FFF)) continue;
					++cnt;
					const int x = i & 255, y = i >> 8;
					if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y;
					if (shown < 10) {
#if GXDS_CORE == 0
						const u16 t3 = s_3dLinear ? s_3dLinear[i] : 0xFFFF;
#else
						const u16 t3 = 0;
#endif
						harness_profile_emitf(GXDS_TAG "abd x=%d y=%d gx=%04x cpu=%04x t3d=%04x", x, y, (unsigned)dst[i], (unsigned)s_abCpu[i], (unsigned)t3);
						++shown;
					}
				}
				harness_profile_emitf(GXDS_TAG "abd frame=%u ndiff=%d bbox=%d,%d,%d,%d bands=%d", (unsigned)s_abFirstBad, cnt, bx0, by0, bx1, by1, bandCount);
			}
			++s_abBad;
		}
	}
#endif
	// Success path only (see BandResetter's comment): every write recorded
	// this frame has now been consumed by a bake, so the trace can start
	// clean. Writes landing after this point -- i.e. during lines 192-262's
	// VBlank -- accumulate into the next frame's trace, which is exactly
	// where they belong.
	GXDS_PLAN.vram.clear();
	GXDS_PLAN.palette.clear();
	GXDS_PLAN.oam.clear();
#ifdef DSB_STATS
	s_dsbOldVramArm = false;
#endif

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
	s_dsbFr3d = any3dBand;
#endif
	(void)anyMb;
	return true;
}

bool GXDS_API(RenderFrame)()
{
#if GXDS_LAZY_COMPILED
#ifdef DSB_ABCMP
	// The in-process A/B compare needs the CPU compositor's result in GPU_screen before the GX pass
	// overwrites it: replay the pending lines first (this also exercises the deferred-render path
	// on every frame). The discard path is then a no-op, so ABCMP validates GX-vs-CPU, and the
	// forced-lazy replay build (see DSLZ_FORCE_LAZY) validates the deferral itself.
	gxDsLzFlush(2);
#endif
	const bool handled = gxDsBRenderFrameImpl();
	if (handled) gxDsLzDiscard(); else gxDsLzFlush(2);
#ifdef DSLZ_STATS
	++s_lzStFrames;
	if ((s_lzStFrames % 30) == 0) {
#if defined(DESMUME_HARNESS) && defined(HARNESS_PROFILE)
		harness_profile_emitf(GXDS_TAG "lz n=%u defer=%u eager=%u disc=%u flushreg=%u flushdata=%u flushbail=%u flushoos=%u barrio=%u barrdata=%u databail=%u",
		                      (unsigned)s_lzStFrames, (unsigned)s_lzStDeferredLines, (unsigned)s_lzStEagerLines, (unsigned)s_lzStDiscardFrames,
		                      (unsigned)s_lzStFlushRegLines, (unsigned)s_lzStFlushDataLines, (unsigned)s_lzStFlushBailLines, (unsigned)s_lzStFlushOosLines,
		                      (unsigned)s_lzStBarrierIo, (unsigned)s_lzStBarrierData, (unsigned)s_lzStDataBail);
#endif
	}
#endif
	gxDsLzReset();
	return handled;
#else
	return gxDsBRenderFrameImpl();
#endif
}
