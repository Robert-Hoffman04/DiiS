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

    gxGbaRenderFrame() still returns false (CPU fallback via
    renderScanline(), untouched by this file) for anything genuinely
    unhandled, but with modes 0-5 and both OBJ types covered, in practice
    that's only the DISPCNT mode field's two prohibited encodings (6/7),
    which shouldn't occur on real ROMs.

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
