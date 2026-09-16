/*
    gx_gba_render.h - Stage 1 (Resource Sync) + Stage 4 (2D Compositing) GX
    implementation for the GBA PPU vertical slice (nds-wii-render-pipeline.md).

    Scope for this pass: DISPCNT modes 0 (text-only, all four BGs) and 3/4/5
    (bitmap), plus regular (non-affine) OBJ. gxGbaRenderFrame() bails out
    (returns false) for anything it doesn't cover -- modes 1/2 (BG2/3 affine
    text layers), or any affine OBJ present this frame -- and the caller
    (gba_ppu.cpp's gbaPpuEndFrame) falls back to the existing per-pixel CPU
    compositor (renderScanline()), which stays the sole reference
    implementation for those cases. The CPU path is untouched by this file.

    Design choices specific to this GX path (deviations/simplifications from
    an idealized Stage 1/4, documented rather than silent):
     - BG/OBJ/bitmap textures are baked as already palette-resolved RGB5A3
       (one CPU-side palette lookup per bake, the same lookup renderScanline()
       does per-pixel today) rather than uploaded as native GX CI4/CI8+TLUT.
       Simpler, and correct; revisit only if TMEM/upload bandwidth profiling
       ever shows it matters.
     - Stage 1 dirty-gating is coarse: a whole-VRAM-or-palette anyDirty()
       check gates re-baking every active layer, not per-layer/per-page
       precision. Correct (never renders stale data) but leaves cheap wins on
       the table; a follow-up can narrow this to gx_frameplan.h's actual
       per-page bits.
     - A BG plane's tile/map layout (char base, map base, size, color depth)
       is baked once per frame using the register value current at bake time
       (i.e. the frame's final value for that register) even though Stage 4
       draws it per-band with that band's own scroll (HOFS/VOFS) snapshot --
       only scroll is treated as varying per-band, not the underlying tile
       data layout. Mid-frame BGxCNT bank-switch tricks are rare and, if hit,
       degrade to slightly stale tile layout for the bands drawn before the
       final value, not a crash.
     - OBJ textures are re-decoded every dirty frame for every visible
       sprite, not cached per-OAM-index. Same tradeoff as above.
     - Output still round-trips through the CPU-side GBA_screen buffer (via
       GX_CopyTex) rather than presenting the EFB directly -- Stage 7 (final
       present) hasn't landed yet, so this keeps the existing harness/present
       code (which expects GBA_screen as a plain RGB565-family buffer)
       working unchanged. Goes away once Stage 7 lands.

    GX-only (libogc), like gx_mask.* -- verified by cross-compilation only,
    not a host-side unit test.
*/
#ifndef GX_GBA_RENDER_H
#define GX_GBA_RENDER_H

#include "../types.h"
#include "gx_frameplan.h"

// Per-band register snapshot the GX path needs to redraw a band with the
// state that was actually in effect when it was recorded, since by the time
// Stage 4 runs (end of frame, see gx_frameplan.h) every register already
// holds its final value for the frame. Populated by gba_ppu.cpp's register
// write funnel (gxOnRegisterWritten) in lockstep with
// g_gbaFramePlan.bands -- entry 0 is the frame-start snapshot, entry N is
// the state recorded at the write that created band-boundary N-1. This
// relies on register writes being recorded in non-decreasing scanline order
// within a frame (true: gba_ppu.cpp calls this from the real-time scanline
// executor, never out of order), so band index from
// GxBandTracker::finalize() lines up with this array's index without a
// separate remapping table -- see gxOnRegisterWritten's comment.
struct GxGbaBandRegs {
	u16 dispcnt;
	u16 bgcnt[4];
	u16 hofs[4];
	u16 vofs[4];
};
static const int GX_GBA_MAX_BAND_REGS = GxBandTracker::kMaxBands;
extern GxGbaBandRegs g_gbaBandRegs[GX_GBA_MAX_BAND_REGS];

bool gxGbaRenderInit();
void gxGbaRenderShutdown();

// Attempts the GX Stage 1+4 path for the frame that just finished executing
// (called from gbaPpuEndFrame(), after all of this frame's register/VRAM/OAM
// writes have landed and been traced into g_gbaFramePlan). Returns true if
// it drew and filled GBA_screen (the caller should skip its own CPU
// compositor for this frame); false if the current frame's mode/content
// isn't handled yet, in which case GBA_screen is left untouched and the
// caller must run its CPU fallback for the whole frame.
bool gxGbaRenderFrame();

#endif // GX_GBA_RENDER_H
