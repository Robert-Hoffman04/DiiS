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

//------------------------------------------------------------------------------
// GXMerge - hardware 3D/2D compositing path (see the plan / desmumewii-findings.md
// section 5).
//
// Instead of reading the GX-rendered 3D scene back to the CPU every frame and
// letting the software 2D compositor blend it in, the 2D compositor splits its
// output into two textures - everything BEHIND the 3D layer's priority slot and
// everything in FRONT of it - and the GX hardware performs the merge as a
// 3-draw sandwich:  behind-texture -> 3D-texture -> front-texture.
//
// The 3D EFB result is only ever *sampled* by GX (never CPU-read), so the
// per-frame de-swizzle/convert loop in GXRender.cpp disappears from the hot path.
//
// This whole path is opt-in (GXMerge_Enabled()) and only ever runs with the GX
// 3D core (current3Dcore == 1).  The legacy readback + software-composite path is
// left completely intact as the fallback.
//------------------------------------------------------------------------------

#ifndef GXMERGE_H
#define GXMERGE_H

#include <gctypes.h>
#include <gccore.h>

#ifdef __cplusplus
extern "C" {
#endif

// 192 is the theoretical maximum (one band per scanline); real frames use a
// handful.  Overflow => whole-frame fallback.
#define GXMERGE_MAX_BANDS 96

// A contiguous run of scanlines over which the 3D layer is active and handled by
// the hardware merge, all sharing one BG0 HOFS value (a band breaks on any change,
// same as it already breaks wherever a line drops out of the merge path entirely).
typedef struct {
	u8   yStart;        // inclusive DS scanline
	u8   yEnd;          // inclusive DS scanline
	u16  hofs;          // this band's BG0 X-scroll (GPU::getHOFS(0)), 0..511
	bool alphaOver;     // true: draw the 3D band with a real per-pixel alpha blend
	                    // against the behind bucket (MB_ALPHA_OVER); false: the
	                    // default opaque + alpha-keyed draw (MB_OPAQUE_MASKED).
	bool frontAlphaOver; // true: the front-bucket texture for this band holds a
	                    // 2D layer that alpha-blends against what is beneath it
	                    // (the 3D layer / behind bucket) using the global BLDALPHA
	                    // EVA constant - the GX front draw blends it over the EFB
	                    // instead of the default opaque + alpha-keyed draw.
	u8   frontEva;      // BLDALPHA_EVA (0..16) for the frontAlphaOver blend; a
	                    // change splits the band, same as hofs / alphaOver.
	u8   brightMode;   // MASTER_BRIGHT mode for this band: 0 none, 1 fade-to-white
	                    // (bright up), 2 fade-to-black (bright down). Applied as a
	                    // final full-width GX pass over the merged EFB (draw 4).
	u8   brightFactor; // MASTER_BRIGHT factor 1..16 for that pass (blend fraction
	                    // = brightFactor/16); a change of either splits the band.
} GXMergeBand;

typedef struct {
	int         nBands;
	GXMergeBand bands[GXMERGE_MAX_BANDS];
	bool        valid;          // this frame can be presented by the merge path
	bool        mainIsTop;      // MainScreen.offset == 0 when the frame was built
} GXMergeFrame;

// --- Step 5.1a: MAIN text BG layers composited on GX -------------------------
// A run of scanlines over which the full 2D composite is exactly:
//   opaque backdrop, then N enabled text BG layers back-to-front, then an
//   optional MASTER_BRIGHT pass - no sprites, no window, no BLDCNT effect, no
//   3D on the line.  A band breaks on any change of layer set / scroll /
//   backdrop / brightness.
#define GX2DBG_MAX_BANDS   64
#define GX2DBG_MAX_LAYERS  4
typedef struct {
	u8  yStart, yEnd;
	u8  nLayers;
	u8  layer[GX2DBG_MAX_LAYERS];   // BG index 0..3, painter's order (index 0 = bottom)
	u16 hofs[GX2DBG_MAX_LAYERS];
	u16 vofs[GX2DBG_MAX_LAYERS];
	u16 backdrop;                   // RGB555 | 0x8000
	u8  brightMode, brightFactor;
} GX2DBGBand;

typedef struct {
	int        nBands;
	GX2DBGBand bands[GX2DBG_MAX_BANDS];
	bool       valid;
} GX2DBGFrame;

// Recorder: called from GPU_RenderLine_layer for each scanline whose entire 2D
// composite is GX-expressible (see above).  layers/hofs/vofs are nLayers long,
// painter's order.  The line's CPU BG/sprite walk is then skipped.
void GXMerge_Record2DBGLine(int l, u16 backdrop, u8 brightMode, u8 brightFactor,
                            int nLayers, const u8 *layers,
                            const u16 *hofs, const u16 *vofs);

//--- toggle -------------------------------------------------------------------
void GXMerge_SetEnabled(bool en);
bool GXMerge_Enabled(void);

// Step 5.1a: also composite MAIN-engine text BG layers on GX (implies GXMerge).
void GXMerge_Set2DBG(bool en);
bool GXMerge_2DBGEnabled(void);

//--- lifecycle (GX must already be initialised) ------------------------------
void GXMerge_Init(void);
void GXMerge_Deinit(void);

//--- 3D texture ping-pong (called from GXRender.cpp, core thread, vidmutex) ---
// Tiled RGBA8 buffer that GXRender should GX_CopyTex the 3D EFB into this frame.
void *GXMerge_CopyDst(void);
// Called right after GX_CopyTex + GX_PixModeSync (flips the ping-pong slot).
void GXMerge_NoteGXRenderCopied(void);
// Called when GXRender was skipped this frame (no flip - reuse stale 3D).
void GXMerge_NoteGXRenderSkipped(void);

//--- per-frame compositor hooks (core thread, GPU_RenderLine, MAIN engine) ----
void GXMerge_BeginFrame(bool mainIsTop);
// At the 3D split point, for each 3D-active line.  behindContent = the behind
// bucket has real 2D (BG/sprite) content on this line, not just the backdrop.
// hofs = this line's BG0 X-scroll (GPU::getHOFS(0)); a change from the previous
// recorded line splits the band even though both lines stay 3D-active.
// alphaOver = draw this line's 3D content with a real per-pixel alpha blend
// against the behind bucket (see GXMergeBand::alphaOver); also splits the band
// on change.
// frontAlphaOver / frontEva = this line's front bucket holds a 2D layer that
// alpha-blends against what is beneath it, using BLDALPHA EVA = frontEva (see
// GXMergeBand::frontAlphaOver); a change of either also splits the band.
// brightMode / brightFactor = this line's MASTER_BRIGHT state (mode 1/2, factor
// 1..16); applied by the merge path as a final per-band GX pass over the merged
// EFB instead of the CPU GPU_RenderLine_MasterBrightness pass, which the caller
// suppresses for merged lines (see GXMerge_LineWasMerged). A change of either
// splits the band.
void GXMerge_RecordLine(int l, bool behindContent, u16 hofs, bool alphaOver,
                        bool frontAlphaOver, u8 frontEva,
                        u8 brightMode, u8 brightFactor);
void GXMerge_EndFrame(void);      // coalesces recorded lines into bands
// True if scanline l was recorded as a hardware-merge band this frame (used by
// GPU_RenderLine to skip the CPU master-brightness pass for merged lines - the
// merge path applies MASTER_BRIGHT itself as a per-band GX pass).
bool GXMerge_LineWasMerged(int l);
// True while the current frame's classification still permits the merge path.
bool GXMerge_FrameArmed(void);
// Force whole-frame fallback for the frame currently being built.
void GXMerge_Disarm(void);

//--- front-bucket line buffer (core thread, GPU_RenderLine_layer) -------------
// Returns the front-bucket destination for scanline l (256 * u16, 0x0000 = clear),
// after clearing it.  The caller redirects gpu->currDst here at the split point.
u8 *GXMerge_FrontLine(int l);

//--- present snapshot (core thread, Draw(), under vidmutex) ------------------
// behindSrc / frontSrc are RGB555 256x192 (behindSrc is the MAIN-engine half of
// GPU_screen; frontSrc is GXMerge's front screen).  Builds the GX textures and
// latches the descriptor + the 3D texture slot to sample.
void GXMerge_Present(void);
bool GXMerge_HasPresentFrame(void);

//--- the sandwich (draw_thread, under vidmutex) ------------------------------
// Draws behind -> 3D(bands) -> front into the model space already set up by the
// caller (same PNMTX0 / projection as the normal screen quad).  (x0,y0)-(x0+w,
// y0+h) is the on-screen quad rect for the MAIN engine's screen.
void GXMerge_DrawMainScreen(f32 x0, f32 y0, f32 w, f32 h);
void GXMerge_DrawStatusMarker(f32 x0, f32 y0, f32 w, f32 h);   // GXMERGE_DEBUG only

//--- Phase 4: lazy readback (no-op stub until then) -------------------------
void GXMerge_MaterializeConverted(void);

#ifdef __cplusplus
}
#endif

#endif // GXMERGE_H
