/*
    gx_ds_3d_render.h - item 13e vertical slice: DS 3D geometry (Stage 3) submitted
    through GX instead of the CPU rasterizer (source/rasterize.cpp).

    See gx-13e-design.md for the full survey/design and gx-next-steps-log.md's
    "Task 13e-slice" section for what this first pass actually implements/verifies.

    Scope of THIS pass (deliberately narrow -- see the design doc's section 3):
    opaque, untextured, flat- or Gouraud-shaded triangles/quads, Z-buffer mode only,
    no shadows, no fog, no translucency. Exact-or-bail: gxDs3dGeomFrameSupported()
    decides whether the current gfx3d.polylist/vertlist content is in scope; callers
    must check it before using either producer and fall back to the existing CPU path
    otherwise, same discipline as every other GX engine instance in this codebase.

    Two producers, matching the runtime render-mode switch (gx_rendermode.h):
      gxDs3dRenderAccurate() - CPU performs the DS's own homogeneous divide + viewport
                                transform (byte-for-byte the same arithmetic
                                rasterize.cpp's clipper/viewport step already does),
                                GX only rasterizes/interpolates/blends the already-
                                screen-space triangle. This is the byte-exactness
                                anchor for GxAccurate mode.
      gxDs3dRenderFast()      - converts the polygon's own captured mvMatrix/projMatrix
                                (POLY::mvMatrix/projMatrix) to real GX Mtx/Mtx44 and
                                submits VERT::objcoord (object-space) positions,
                                letting GX's own fixed-function transform hardware do
                                the multiply + perspective divide. Offloads the
                                per-vertex transform to GX at the cost of float-
                                rounding drift versus the CPU's fixed-point-accurate
                                reference -- see the log section for the measured
                                drift on the vertical-slice fixture.

    IMPORTANT deviation from gx-13e-design.md section 2/3 (flagged there too): the
    design doc's original GX-Accurate proposal ("submit pre-transformed clip-space
    positions via GX_Position4f32 under an identity PNMTX") is not implementable as
    written -- GX has no 4-component (homogeneous, caller-supplied-w) position vertex
    attribute (libogc's gx.h only defines GX_POS_XY / GX_POS_XYZ, and GX always
    derives clip-space w from its own fixed-function transform, never from per-vertex
    input). GxAccurate therefore does the FULL divide+viewport transform on the CPU
    (exactly mirroring rasterize.cpp) and feeds GX plain 3-component screen-space
    positions under the same orthographic screen-pixel projection already used
    elsewhere in this codebase for 2D content (gxDsBSetup2DState's
    guOrtho(0,kDsBScreenH,0,kDsBScreenW,0,1) pattern) -- GX's role shrinks to pure
    rasterization for this mode, which if anything is a MORE faithful reading of "GX
    only rasterizes, CPU keeps doing the DS-precise math" than the original mechanism
    would have been.

    ADDED 2026-09-22/23 (Task 13e-precision, see the log section): the prior slice's
    "GxAccurate verified byte-exact against CPU" claim on the case_27 fixture was a
    FALSE NEGATIVE -- the overlay draw was never actually visible in the captured
    output at all (proven with a forced full-screen probe triangle: 0 pixels showed
    up), so every prior comparison was silently comparing the CPU-rasterizer's
    pre-existing, already-correct texture-fed 3D layer against itself. Three real bugs
    found and fixed this pass (all in gxDs3dSetupCommonState()/gxDs3dRenderAccurate(),
    see their comments for detail):
      1. Positive Z (the DS's natural 0=near/1=far convention) falls entirely outside
         guOrtho's actual visible eye-space Z range ([-1,0], "-z axis" per libogc's own
         doc comment) -- GxAccurate now negates sz before feeding GX_Position3f32.
      2. (Superseded 2026-09-23, Task 13e-zfast.) The "clipper rejects everything"
         and "Z-test makes draws vanish" findings were one bug: the Z test was tried
         as GEQUAL, but guOrtho's -z_eye cancels the negation, so depth runs
         near=0..far=max and the right test is LESS (the CPU rejects depth >= dest).
         Z test is now on (LESS, depth cleared to gfx3d.clearDepth first) and the
         clipper is back on; a no-Z mutation build must and does get worse on a3_c28.
      3. VERT::color is a 6-bit-per-channel (0-63) DS material color, not 0-255 --
         GX_Color4u8 needs the standard 6-to-8-bit bit-replication expansion
         (gxDs3d6To8()); without it every triangle rendered ~4x too dark.
      4. (2026-09-23) GxFast now draws: GX_LoadProjectionMtx keeps only 6 entries and
         GX's clip Z range is [-w,0], so the DS projection is folded into the
         position matrix and GX gets a depth-only canonical projection (see
         gxDs3dFastXformBuild()); the old Y flip was wrong (GX and DS both put NDC
         +1 at the top). Both producers add a +0.5 px sample offset (GX samples pixel
         centres, the DS rasterizer pixel corners).
      Result (a3_c27 / a3_c28 vs the DSA_FORCE_CPU reference, 98304 px): 0 / 15 bad
      in BOTH modes -- the 15 are 5 diagonal-edge tie pixels and 10 at the triangle
      intersection line (24-bit GX depth vs the CPU's 15-bit quantized depth). So
      GxAccurate is NOT bit-exact on intersecting geometry yet.
*/
#ifndef GX_DS_3D_RENDER_H
#define GX_DS_3D_RENDER_H

#include <gctypes.h>

// True iff the current gfx3d.polylist/vertlist content (the just-flushed frame) is
// fully within this pass's supported scope: all polygons opaque (indexlist's opaque
// count == polylist->count), untextured (texParam's format field == 0), Z-buffer mode
// (!gfx3d.wbuffer), no shadow-mode polygons (POLYGON_ATTR mode bits != 3), and the
// polygon/vertex counts are small enough to submit in one GX_Begin/End run per batch
// without exceeding GX's FIFO in a single frame (no real DS scene gets close to that
// limit for opaque-only geometry, but the check is cheap so it's included).
bool gxDs3dGeomFrameSupported();

// Draws every opaque polygon in gfx3d.polylist (up to the translucent boundary) into
// whatever EFB region GX_SetViewport/GX_SetScissor currently target, GxAccurate style
// (CPU-transformed screen-space positions, byte-exact anchor). Caller owns all other
// GX state (Z mode, vertex format, blend) -- see the .cpp file's top comment for the
// exact state this function sets up and does NOT restore (caller must restore vertex
// format / Z mode / channel control before any subsequent 2D draw in the same frame).
void gxDs3dRenderAccurate();

// Same polygon walk, GxFast style: converts each polygon's own captured
// mvMatrix/projMatrix to GX Mtx/Mtx44 (baking in the Y-axis flip GX's clip-space
// convention needs relative to the DS's, see the .cpp file), loads them, and submits
// VERT::objcoord object-space positions -- GX's own transform hardware does the
// multiply + perspective divide instead of the CPU.
void gxDs3dRenderFast();

#endif // GX_DS_3D_RENDER_H
