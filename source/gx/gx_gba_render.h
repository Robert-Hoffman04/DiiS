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

    ### Native CI4/CI8 (gx-next-steps-log.md task 6)

    The BASE (non-effect, toEffectScratch==false) persistent-cache bake path
    now uploads raw GBA palette-index texels via GX_InitTexObjCI + a TLUT
    built from the live GBA palette bank at bake time, instead of resolving
    each texel to RGB5A3 at bake time -- see nds-wii-texture-format-mapping.md
    ("2D BG/OBJ 4bpp/8bpp tiles"). Converted:
     - Affine BG planes (gxBakeAffineBgPlane): always CI8 -- affine map
       entries are a full byte with no per-tile palette selection, direct
       idx 0-255 into the whole 512-byte BG palette bank, exact.
     - OBJ, regular and affine (gxBakeObjTexture): CI4 for 4bpp, CI8 for
       8bpp -- a sprite has exactly one fixed OAM palNum field (not
       per-texel), so CI4's 16-entry TLUT is exact for 4bpp.
     - Text BG planes (gxBakeBgPlane): CI8 for BOTH 4bpp and 8bpp -- a
       deliberate, documented deviation from the task brief's literal
       "4bpp -> CI4" wording. A text BG map entry carries its OWN palNum
       per TILE (bits 12-15), so a single baked plane texture routinely
       spans multiple different 16-color sub-banks; GX binds exactly one
       TLUT per texture object per draw call, so a 16-entry CI4 TLUT could
       only ever represent ONE of those sub-banks correctly. 4bpp text BG
       instead bakes an 8-bit COMBINED index (palNum*16+idx) into CI8 with
       a 256-entry TLUT spanning the whole BG palette bank -- still a real,
       correct bandwidth/TMEM win over RGB5A3 (8 bits/texel vs 16), just not
       CI4's maximal 4-bit win. A per-palette-group multi-draw scheme (see
       nds-wii-texture-format-mapping.md's "Extended-palette 2D BGs") would
       get the maximal win but is a materially larger change, not attempted
       this pass -- documented follow-up.
     - Bitmap mode 4 (gxBakeBitmapMode, mode==4 only): CI8, direct idx 0-255
       into the whole BG palette bank, same as affine BG. Unlike every tiled
       format above, mode-4 pixels are ALWAYS opaque (no idx!=0 test), so
       its TLUT does NOT force entry 0 transparent -- entry 0 keeps its real
       color, same treatment as every other entry.

    Left as RGB5A3, deliberately out of this task's scope:
     - Bitmap modes 3/5: direct 16bpp truecolor, not palette-indexed at all
       -- forcing them into a CI format would be a format-mismatch, not a
       conversion.
     - The backdrop (gxBakeBackdrop): a single 1x1 non-tile-indexed texel,
       lowest priority per the task brief; converting a 1-texel texture to
       CI+TLUT has no bandwidth benefit and adds a TLUT slot for no gain.
     - The toEffectScratch==true bake-time-blend path (gxTexel's
       GXTEXEFFECT_ALPHA/BRIGHTEN/DARKEN, see "Blend" above): completely
       unchanged, still RGB5A3 -- explicit scope boundary per the task
       brief, kept cleanly separable (every bake function's toEffectScratch
       branch is untouched code, still calling gxTexel/gxUploadEffectTex
       exactly as before task 6).

    Two correctness subtleties, both load-bearing for this conversion:
     - **Index-0 transparency**: GBA hardware treats palette index 0 WITHIN
       EACH 16-color sub-bank as transparent for a 4bpp tile texel (i.e.
       every combined index that's a multiple of 16, not just combined
       index 0), and plain index 0 as transparent for 8bpp -- every TLUT
       built for a BG/OBJ TILE texture (NOT the backdrop, and NOT mode 4's
       bitmap TLUT -- see above) forces those specific entries' alpha to 0
       regardless of the real RGB stored at that GBA palette slot, matching
       the old `idx != 0` per-texel opacity test this file's gxTexel() call
       sites already used.
     - **Affine border-clamp technique**: affine BG planes and ALL OBJ
       textures still pad a 1-texel transparent border around real content
       (see this header's intro) so out-of-range affine UVs GX_CLAMP onto
       transparency instead of smearing the edge -- under CI4/CI8 the
       border/padding is filled with RAW INDEX 0, which the same TLUT
       (index-0-transparent, see above) resolves to alpha=0, keeping the
       technique exact. Buffer padding widened from the old RGB5A3 scheme's
       +4 (a 1-texel real border rounded up to RGB5A3's 4x4 block multiple)
       to +8 for the persistent CI4/CI8 caches specifically, since CI4's
       block shape is 8x8 and CI8's is 8x4 (gx_texformat.h) -- both wider
       than RGB5A3's 4x4, so +4 (only ever a multiple of 4) isn't guaranteed
       to satisfy either format's alignment the way +8 is (the underlying
       map/sprite dimension is always itself a multiple of 8). The
       toEffectScratch RGB5A3 path is unaffected, still uses +4.

    TLUT hardware-slot assignment (gxBgTlutSlot/kBmpTlutSlot/kObjTlutSlot/
    gxAffineBgTlutSlot, gx_gba_render.cpp): text BG0-3 and affine BG2/BG3
    each get their OWN permanently-dedicated GX_TLUTn name (GX_TLUT0-3 and
    GX_TLUT6/7 respectively) rather than sharing one slot between a BG
    index's text and affine bakes -- deliberately, so a mode switch (text
    <-> affine) can never leave a cache's `valid` flag true while the TMEM
    slot it references was silently overwritten by the other variant's bake
    in between (each cache's own dirty-gate has no visibility into the
    other cache's writes to a shared TMEM slot). Bitmap mode 4 gets its own
    slot (GX_TLUT4). OBJ (128 possible sprites, only 16 GX_TLUTn names
    exist) shares ONE slot (GX_TLUT5), reloaded via GX_LoadTlut immediately
    before every persistent-cache OBJ draw (not just on a rebake) since a
    different sprite's draw in between may have overwritten it -- the index
    TEXTURE data is still cached/reused across frames exactly as task 4
    already does; only the (cheap, 32-512 byte) TLUT reload happens every
    draw. 8 of the 16 available slots are used, well within GX_Init()'s
    default 16-slot TMEM TLUT budget.

    Known limitation, not fixed this pass: a rebake gate (gxObjNeedsRebake/
    gxBgPlaneNeedsRebake/gxAffineBgPlaneNeedsRebake, tasks 4-5) still
    triggers a FULL re-bake (VRAM tile decode AND TLUT rebuild) whenever
    EITHER the tile-index/config dependency OR the palette dependency is
    dirty -- it doesn't distinguish a palette-only change (which, now that
    index and color are decoupled, only needs a cheap TLUT rebuild, no VRAM
    re-decode at all) from a VRAM/config change. This was a deliberate
    choice to keep task 6 additive on top of tasks 4/5's existing
    dirty-gating contract rather than restructuring it into two independent
    triggers -- a real, cheap follow-up win (skip the VRAM decode on a
    palette-only dirty), not attempted here.

    Design choices specific to this GX path (deviations/simplifications from
    an idealized Stage 1/4, documented rather than silent):
     - Stage 1 dirty-gating: OBJ (task 4) and now the backdrop + all four
       BG planes (task 5) use per-target fine-grained gates built on
       gx_frameplan.h's GxDirtyBitmap::isPageDirty() -- see the next bullet
       and the "OBJ textures" bullet below. Bitmap-mode (DISPCNT mode 3/4/5)
       textures are the one remaining consumer of the original coarse
       whole-VRAM-or-palette anyDirty() gate; not narrowed by task 5 (out of
       its BG/backdrop-specific scope) or task 4 (OBJ-specific scope) --
       still correct (never renders stale data), just leaves a cheap win on
       the table for a future task.
     - A BG plane's tile/map layout (char base, map base, size, color depth)
       is baked once per frame using the register value current at bake time
       (i.e. the frame's final value for that register) even though Stage 4
       draws it per-band with that band's own scroll (HOFS/VOFS) snapshot --
       only scroll is treated as varying per-band, not the underlying tile
       data layout. Mid-frame BGxCNT bank-switch tricks are rare and, if hit,
       degrade to slightly stale tile layout for the bands drawn before the
       final value, not a crash. Task 5's per-plane rebake gate
       (gxBgPlaneNeedsRebake/gxAffineBgPlaneNeedsRebake) reads this same
       frame-final register value to decide dirtiness, consistent with what
       gxBakeBgPlane/gxBakeAffineBgPlane themselves bake from -- it inherits
       this simplification rather than working around it.
     - OBJ textures: gx-next-steps-log.md task 4 closed this gap for OBJ
       specifically -- each OAM index's baked texture (s_objTex[]) now
       carries the OAM fields (tile-index/shape/size/color-mode/
       palette-index) and DISPCNT 1D/2D bit it was baked from, and is only
       re-decoded when gxObjNeedsRebake() finds either those fields
       changed or the exact VRAM tile bytes/palette entries that
       configuration depends on were written (via gx_frameplan.h's
       GxDirtyBitmap::isPageDirty(), not just the coarse anyDirty() the
       bullet above still describes for bitmap-mode planes).
     - BG textures: gx-next-steps-log.md task 5 closed this gap for the
       backdrop and all four BG planes, replicating task 4's OBJ pattern.
       The backdrop (gxBackdropNeedsRebake) gates on exactly its one
       dependency, BG palette color 0 (2 bytes). Each BG plane
       (gxBgPlaneNeedsRebake for text mode, gxAffineBgPlaneNeedsRebake for
       affine) carries a last-baked fingerprint (char base, map base, color
       depth / map size, and for affine, the overflow-wrap bit -- piggy-
       backing on fields the cache already stored for drawing, e.g.
       mapWpx/mapHpx/mapPx/wrap, rather than duplicating them) and is only
       re-baked when that fingerprint changes or its dependency bytes were
       written. Two deliberate precision/simplicity tradeoffs, both
       documented in detail at gx_gba_render.cpp's gxBackdropNeedsRebake()
       comment block (not repeated in full here):
        - Text BG char-VRAM dependency is a conservative superset --
          [charBase, charBase + 1024*tileBytes), the full range any of the
          map's 10-bit tile indices could reach at the BG's current color
          depth -- not a byte-exact "only the tiles this map actually
          references" range (which would require walking the tilemap
          before baking, at roughly the bake's own cost, just to decide
          whether to bake). Map-VRAM dependency IS exact (gxBakeBgPlane's
          own block addressing always produces one contiguous span).
          Affine BG's dependency ranges are both exact (byte-per-tile map,
          full-byte tile index into an always-8bpp charset).
        - Every BG plane (4bpp text, 8bpp text, and affine) gates on the
          whole 512-byte BG palette bank, not the tightest-possible
          per-map-entry sub-palette -- unlike OBJ, a text BG map entry
          carries its OWN palNum per tile (not one fixed OAM field), so a
          byte-exact version has the same chicken-and-egg map-walk problem
          as the char-VRAM case above. This means multiple BG planes that
          use different sub-palettes still all re-bake together on any BG
          palette write, even one none of them actually reads -- a
          deliberate, documented conservative simplification, not an
          oversight (see gx-next-steps-log.md task 5's own section for the
          synthetic-ROM proof this surfaced during testing).
       HOFS/VOFS (scroll) and, for affine, PA/PB/PC/PD/reference-point are
       deliberately NOT part of any BG fingerprint, for the same reason
       task 4 excludes OBJ's affine matrix: both are read fresh every frame
       via UV/quad math and never affect the baked texture's pixels.
       Per-frame dirty tracking's inherent limitation (both task 4's OBJ
       gate and task 5's BG/backdrop gates share this): the dirty bitmaps
       are cleared every frame (GxFramePlan::beginFrame()), so a plane/
       sprite that goes temporarily invisible/disabled (and is therefore
       skipped by the rebake check entirely while it stays that way) could
       in principle miss a dependency write that happened only during that
       invisible interval, then render stale once visible again with no
       further writes to re-trigger it. The old coarse anyDirty() gate
       accidentally avoided this in practice (virtually any VRAM/palette
       write anywhere re-armed it), which the new fine-grained gates no
       longer do by construction. Not fixed here -- doing so would mean
       either accumulating dirty state across multiple frames (a bigger
       change to gx_frameplan.h's contract) or force-rebaking on every
       enable-after-disable transition (inconsistent with task 4's existing
       OBJ design, which has the identical characteristic and was left
       as-is) -- flagged as a known, shared limitation of this whole
       per-frame dirty-tracking approach rather than silently assumed away.
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
       working unchanged.

       gx-next-steps-log.md task 3 narrowed, rather than removed, this gap:
       a genuinely direct EFB->XFB present (skipping GBA_screen entirely)
       was ruled out as unsafe for this task -- gxGbaRenderFrame() runs on
       the emulation thread (called from gbaPpuEndFrame(), itself called
       from DSExec()), while the actual EFB->XFB copy (GX_CopyDisp) and the
       cursor/FPS overlay draws happen on main.cpp's separate draw_thread
       LWP, synchronized only at frame granularity via vidmutex. Two
       independently-configured GX state setups (this file's
       gxSetup2DState() vs. draw_thread's own matrix/TEV/Z state) would
       have to agree on EFB ownership and draw ordering with the cursor/FPS
       overlay layered on top, entirely across that thread boundary, to
       make a same-EFB same-copy scheme safe -- judged too large a
       restructuring (a real frame-boundary handshake between the
       emulation thread and draw_thread doesn't exist today) to fold into
       this task safely. See "Future follow-up" below.

       What task 3 *did* land, still GBA_screen/GPU_screen-preserving (so
       harness_frame.cpp capture and GPU_screen-based savestate blobs are
       unaffected) but removing the double-conversion penalty from the
       actual on-screen present: gxGbaRenderFrame() keeps this frame's
       still-swizzled RGB5A3 GX_CopyTex output around
       (gxGbaBlitNativeTop()); main.cpp's Draw() uses that directly for the
       top-screen present via a block-aligned memcpy instead of re-driving
       it through GBA_screen's BGR555 downconvert + Draw()'s own
       RGB15_REVERSE per-pixel reconversion back up to RGB5A3. GBA_screen
       and GPU_screen are still populated exactly as before, unconditionally,
       every frame -- this is strictly additive, not a replacement of that
       path. See gxGbaBlitNativeTop()'s own comment below and
       gx-next-steps-log.md's task 3 section for the full reasoning,
       including why draw_thread and Draw() being already serialized by
       vidmutex is what makes even this narrower change safe.

       Future follow-up (not started): give draw_thread and the emulation
       thread a real per-frame handshake (e.g. draw_thread waits on a
       frame-ready condvar/semaphore posted at the end of
       gbaPpuEndFrame()/GPU_RenderLine(), instead of the current
       free-running "draw_thread renders whatever's currently in
       TopScreen/BottomScreen, whenever vidmutex is free" model) so that a
       true direct-EFB present (this file draws straight into the frame
       draw_thread is about to copy out, cursor/FPS overlay drawn on top of
       it in the same pass, no separate GX_CopyTex/CPU round trip at all)
       becomes safe to build. That's a cross-cutting change to main.cpp's
       threading model, not scoped to this file, and affects DS-mode
       present too -- a separate task.

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

// gx-next-steps-log.md task 3 (Stage 7 partial present win): when the most
// recent gxGbaRenderFrame() call rendered this frame via the real GX
// draw+GX_CopyTex path (not a CPU-bail return, and not the forced-blank
// early return -- see gxGbaRenderFrame()'s header comment), that call's
// GX_CopyTex output is still sitting in this file's internal scratch
// buffer, already in RGB5A3-swizzled (4x4 texel block) form -- the exact
// format a GX texture object needs, no per-pixel conversion required.
//
// dst256x192Rgb5a3 must point at a 256*192 u16 buffer already holding a
// GX_TF_RGB5A3-swizzled, cleared/transparent (all-zero) border -- this
// function only ever overwrites the top-left [0,240)x[0,160) texel region
// (block-row-aligned memcpy, 4-texel-aligned since GBA_SCREEN_W/H are both
// multiples of 4) to match the same top-left placement
// gbaPpuEndFrame()'s GPU_screen blit already uses, leaving the right
// 16-texel and bottom 32-texel border untouched. Caller (main.cpp's
// InitVideo/Draw()) is responsible for that one-time clear; this function
// never clears anything itself, so it stays a cheap block memcpy with no
// per-frame memset.
//
// Returns false (does nothing to dst) if the last gxGbaRenderFrame() call
// did not render natively this frame -- caller must fall back to its
// regular GPU_screen-based top-screen conversion in that case.
bool gxGbaBlitNativeTop(void *dst256x192Rgb5a3);

#endif // GX_GBA_RENDER_H
