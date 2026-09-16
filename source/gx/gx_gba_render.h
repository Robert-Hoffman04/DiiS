/*
    gx_gba_render.h - Stage 1 (Resource Sync) + Stage 4 (2D Compositing) GX
    implementation for the GBA PPU vertical slice (nds-wii-render-pipeline.md).

    Scope: all DISPCNT display modes (0-5) and both regular and affine OBJ.
    Affine BG (modes 1/2) and affine OBJ are drawn as a single textured quad
    per band/sprite whose 4 corner UVs are computed from the same affine
    formula the CPU reference (gba_ppu.cpp's sampleAffineBg/renderObjLine)
    uses per-pixel -- valid because that formula is linear in screen (x,y),
    so GX's per-pixel UV interpolation across a quad reproduces it exactly.
    Pixels the affine transform maps outside the source texture (common with
    rotation, and always possible with OBJ double-size) must read as fully
    transparent rather than a clamped edge texel: every affine-sampled
    texture (affine BG planes, all OBJ textures) is baked with a 1-texel
    transparent border and addressed with GX_CLAMP, so any UV that would
    fall outside the real content clamps onto that transparent border
    instead of smearing the edge row/column.

    gx-next-steps-log.md task 2: WIN0/WIN1 rectangular windows and
    BLDCNT/BLDALPHA/BLDY blend (alpha-blend, brighten, darken) are now
    implemented natively in this file -- see "Windows" and "Blend" below.
    gxGbaRenderFrame() still returns false (CPU fallback via
    renderScanline(), untouched by this file) for:
     - the DISPCNT mode field's two prohibited encodings (6/7, which
       shouldn't occur on real ROMs);
     - OBJ window (g_gbaFramePlan.isHot(GXHOT_OBJWIN), now meaning
       specifically DISPCNT's WINOBJ enable bit, not "any window feature"
       -- see gba_ppu.cpp's gxUpdateHotFlags()). OBJ window's mask is
       sprite-shaped, not a rectangle, so it isn't expressible as a GX
       scissor the way WIN0/WIN1 are; reproducing it natively would need a
       stencil-style sprite-mask render pass, a materially bigger and
       separately-scoped piece of work than the rectangular-window case.
       Narrow, permanent bail -- not started by this change.
     - mosaic (g_gbaFramePlan.isHot(GXHOT_MOSAIC)), for all layer kinds
       (text BG, affine BG, bitmap BG2, OBJ). Mosaic snaps the *screen*
       sample coordinate to a coarser grid before the normal per-pixel
       sample, which (unlike window/blend) changes what texture content a
       layer even needs baked, not just how already-baked content is
       composited -- and affine BG mosaic on top of that needs the
       historical per-line reference-point substitution gba_ppu.cpp's
       sampleAffineBg() implements (s_affXHistory/s_affYHistory, internal
       to that file), while OBJ mosaic has its own documented sprite-edge
       judgment call (see that file's renderObjLine()). A native GX
       technique exists in principle (bake each mosaic'd band at reduced
       resolution, matching CPU's block-start-snap sampling exactly, then
       upsample with GX_NEAR/point-filtered magnification instead of
       linear interpolation) but combining it correctly with the affine
       history lookup and OBJ edge handling, on top of the window/blend
       work already landed this task, was judged a separately-scoped
       follow-up rather than something to force into this change --
       narrower and more precisely bounded than task 1's original
       blanket bail, but still a real remaining gap, not a convenience
       shortcut. See gx-next-steps-log.md's task 2 section for the full
       reasoning.
     - alpha blend specifically (BLDCNT effect field == 1, including
       semi-transparent OBJ's forced-1st-target case) when this frame's
       blend configuration doesn't satisfy the precondition documented
       below under "Blend". Brighten/darken (effect == 2/3) never bails on
       this account -- they're always exactly reproducible natively.

    ### Windows (WIN0/WIN1)

    Implemented as GX scissor-rectangle decomposition, not a stencil
    buffer: WININ/WINOUT's BG0-3/OBJ/effect enable bits and the WIN0 > WIN1
    > outside precedence (gxGbaRenderFrame()'s window helpers,
    GxWindowPlan/gxBuildWindowPlan) are static per band (WIN0H/V, WIN1H/V,
    WININ, WINOUT are all layout registers -- gxIsLayoutRegister -- so a
    write to any of them already forces a new GxGbaBandRegs snapshot, same
    as BGxCNT/scroll), so each layer's draw for a given band is turned into
    up to 3 scissored sub-draws in *precedence* order: WINOUT first (using
    the full band's Y range -- window-disabled layers just don't get a
    WINOUT pass at all, correctly leaving whatever's already in the
    framebuffer there untouched, since these are the "layer that shouldn't
    show here" case, same underlying trick this file already relies on for
    ordinary per-pixel transparency), then WIN1 scissored to
    [max(bandY0,win1Y0), min(bandY1,win1Y1)) x [win1X0,win1X1), then WIN0
    the same way -- each later pass overwrites the earlier one only within
    its own smaller rectangle, so precedence falls out of plain draw order,
    no stencil/mask buffer needed. This is exact for WIN0/WIN1 (both are
    genuinely axis-aligned rectangles in GBATEK), unlike OBJ window.

    ### Blend (BLDCNT/BLDALPHA/BLDY)

    Implemented as a *bake-time* texel transform, not per-frame TEV/blend
    state: brighten/darken (BLDY, effect 2/3) recolor a target-1 layer's
    opaque texels toward white/black at bake time (gxApplyTexelEffect,
    reusing the same EVY/16 formula as gba_ppu.cpp's blendFade) -- exactly
    reproducible with no precondition, since Y-effect only ever looks at
    the topmost layer, never what's beneath it. Alpha blend (effect 1)
    instead bakes the target-1 layer's opaque texels with alpha scaled by
    EVA/16 (RGB5A3's alpha sub-format only has 3 bits, so this is a
    documented near-match: EVA/16 is rounded to the nearest of 8 levels,
    and RGB truncates 5->4 bits same as any other alpha-subformat texel --
    see gx_color.h) and lets this file's existing GX_BM_BLEND/SRCALPHA/
    INVSRCALPHA compositing (already used today so a transparent texel
    leaves the destination pixel untouched) do the actual per-pixel blend
    against whatever's already been painted there -- which is only
    guaranteed to equal GBATEK's specific "2nd-target layer directly
    beneath" when *every other active layer this frame (including the
    backdrop) is itself flagged 2nd-target in BLDCNT*, since with a single
    painter's-algorithm pass there's no way to make hardware blending
    conditional on which specific layer produced the destination pixel.
    gxGbaRenderFrame() computes this precondition once per frame
    (gxBlendPlanForFrame) -- along with requiring EVA+EVB==16, the standard
    translucency case that GX_BL_SRCALPHA/INVSRCALPHA's fixed dst factor
    (1-EVA/16) can represent exactly; GBA content that deliberately uses
    independent, non-complementary EVA/EVB coefficients is a rare, real,
    but out-of-scope case -- and bails the whole frame to the CPU
    compositor if it doesn't hold (rather than rendering some layers right
    and others wrong). Semi-transparent OBJ's forced-alpha-blend case is
    covered by the same precondition check, treating each such sprite as
    an implicit extra 1st-target layer for that computation.

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
     - Affine BG's per-band reference point (GxGbaBandRegs::affX/affY)
       inherits gba_ppu.cpp's own documented simplification: it's an
       accumulator latched once at frame start and advanced by PB/PD per
       scanline, not re-latched on a mid-frame BGxX/Y write. This GX path
       matches whatever the CPU reference does here by construction (it
       reads that same accumulator), so it can't diverge from it, but both
       inherit the same inaccuracy against real hardware.
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
	// Affine BG2/BG3 state (modes 1/2 only), index 0=BG2, 1=BG3. affX/affY
	// are gba_ppu.cpp's already-accumulated internal reference point
	// (s_affX/s_affY, 20.8 fixed point) at this band's starting scanline --
	// not a fresh read of BG2X/Y -- so this snapshot inherits that file's
	// documented simplification of not re-latching immediately on a
	// mid-frame BGxX/Y write.
	s32 affX[2], affY[2];
	s16 affPA[2], affPB[2], affPC[2], affPD[2];
	// Window/blend registers, snapshotted for the same reason as the rest
	// of this struct (Stage 4 runs at end-of-frame, after every register
	// already holds its final value) -- WIN0H/WIN1H/WIN0V/WIN1V/WININ/
	// WINOUT/BLDCNT/BLDALPHA/BLDY are all layout registers
	// (gxIsLayoutRegister, gba_ppu.cpp) so a write to any of them already
	// forces a new band boundary/snapshot, same as BGxCNT or scroll.
	u16 win0h, win1h, win0v, win1v, winIn, winOut;
	u16 bldcnt, bldalpha, bldy;
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
