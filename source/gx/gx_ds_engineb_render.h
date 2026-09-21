/*
    gx_ds_engineb_render.h - Stage 0 (Frame Analysis) + Stage 1 (Resource
    Sync) + Stage 4 (2D Compositing) GX implementation for the **DS 2D
    Engine B** instance (nds-wii-render-pipeline.md's "three engine
    instances" section).

    This is the second engine instance wired up, after the GBA PPU vertical
    slice (source/gx/gx_gba_render.*). It deliberately mirrors that file's
    ORIGINAL, pre-task-1..6 scope rather than its fully-evolved one -- see
    gx-next-steps-log.md's task 7 section and "Scope" below. Engine A (the
    DS main/top screen) is completely untouched by this file: it still runs
    the unmodified CPU compositor (GPU.cpp's GPU_RenderLine) exactly as
    before, and so does Engine B on every frame this file bails on.

    ------------------------------------------------------------------
    Which engine drives which screen (verified against GPU.cpp, not assumed)
    ------------------------------------------------------------------
    GPU.cpp's GPU_Init(0)/GPU_Init(1) build MainScreen.gpu (core 0 ==
    GPU_MAIN == Engine A) and SubScreen.gpu (core 1 == GPU_SUB == Engine B);
    that binding is FIXED -- an engine's core index never changes. What the
    POWCNT swap bit changes is only NDS_Screen::offset (NDSSystem.cpp swaps
    MainScreen.offset/SubScreen.offset), i.e. which half of the shared
    GPU_screen buffer (and therefore which physical screen) each engine's
    already-rendered scanlines land in. This file therefore always talks to
    SubScreen.gpu and always writes at SubScreen.offset, which is correct
    under either swap state with no special-casing.

    ------------------------------------------------------------------
    VRAM bank mapping: reused, never reimplemented
    ------------------------------------------------------------------
    Unlike the GBA (one flat 96KB VRAM with a fixed BG/OBJ split), the DS
    remaps nine physical banks A-I into logical roles per engine via
    VRAMCNT_A..VRAMCNT_I. This file does NOT re-derive any of that. It
    reuses, unchanged, exactly what the CPU reference already computes:

     - `SubScreen.gpu->BG_tile_ram[n]` / `BG_map_ram[n]` -- the NDS-space
       char/screen base addresses for BG n, already including Engine B's
       MMU_BBG (0x06200000) base and the per-BGxCNT 16KB/2KB block offsets.
       (Note the DS DISPCNT-wide 64KB CharacBase_Block/ScreenBase_Block
       offsets are Engine-A-only -- GPU_setBGProp's `if (gpu->core ==
       GPU_SUB)` branch omits them; this file inherits that by construction
       because it reads the field rather than recomputing it.)
     - `SubScreen.gpu->sprMem` (MMU_BOBJ, 0x06600000) and
       `SubScreen.gpu->sprBoundary` / `spriteRenderMode` for OBJ.
     - `MMU_gpu_map()` (MMU.h) for the actual NDS-address -> host-pointer
       translation. That is the single function that consults
       `vram_arm9_map[]` (the 16KB-page LUT MMU_VRAMmapControl maintains)
       and that correctly resolves an access to an unmapped page onto the
       blank page at the end of MMU.ARM9_LCD. Every VRAM byte this file
       reads goes through it, so bank remapping, mirroring and unmapped-page
       behaviour are identical to the CPU reference by construction.

    The one place bank mapping is observed rather than reused is the
    rebake gate: a VRAMCNT write can change what Engine B's BG/OBJ windows
    point at without any write landing in VRAM at all. gx_ds_engineb_render.
    cpp keeps a copy of the 256 `vram_arm9_map` entries that cover the BBG
    and BOBJ page ranges and re-bakes everything when they change (see
    gxDsBBankMapChanged()).

    ------------------------------------------------------------------
    Scope (phase 1 + task 8) -- everything outside it bails to the CPU compositor
    ------------------------------------------------------------------
    gxDsEngineBRenderFrame() returns false, leaving GPU_screen exactly as
    GPU_RenderLine() already rendered it, unless ALL of the following hold
    for every visible scanline of the frame:

     - DS mode (not GBA), Engine B display mode 1 ("display BG and OBJ
       layers"); mode 0 (white) has no GX work worth doing and mode 2/3 are
       Engine-A-only anyway.
     - Every ENABLED BG is one of (task 8 widened this from "BG_Mode == 0,
       all text"): a text BG; BGType_Affine (1-byte map entries, no flip);
       BGType_AffineExt_256x16 (2-byte TILEENTRY map entries with H/V flip
       + 8bpp tiles); BGType_AffineExt_256x1 (8bpp bitmap);
       BGType_AffineExt_Direct (15-bit direct colour, bit 15 = opaque);
       BGType_Large8bpp. i.e. any DISPCNT BG_Mode 0-6 whose enabled BGs are
       not BGType_Invalid. Wrap (BGxCNT overflow bit) on and off are both
       handled -- EXCEPT a non-wrapping 1024-texel-wide/tall plane, which
       cannot carry the 1-texel transparent border inside GX's 1024-texel
       texture limit (bails). A running affine reference point is tracked
       per scanline (see "Affine" below); a per-line rewrite of it (HDMA
       wave effects) exceeds the band budget and bails.
     - Every drawn sprite is a regular OBJ or (task 8) an affine one
       (OAM RotScale 1 = normal box, 3 = double-size box), both in Mode 0.
     - No windows (WIN0/WIN1/OBJ window all disabled), no colour special
       effect (BLDCNT effect field 0), no mosaic on any enabled BG or any
       drawn sprite, no MASTER_BRIGHT, no extended OBJ palettes, no forced
       blank. (Extended BG palettes ARE handled since task 8: 256-colour
       text BGs and the 256x16 extended-affine flavour, nothing else -- see
       "Extended BG palettes" below.)
     - No semi-transparent OBJ (Mode 1: blends against the layer beneath it
       regardless of BLDCNT's effect field -- see GPU.cpp's
       _master_setFinalOBJColor), no OBJ-window sprite (Mode 2 non-affine is
       skipped exactly; an AFFINE Mode-2 sprite bails), no bitmap OBJ
       (Mode 3), and no affine sprite whose box hangs past scanline 255 (the
       CPU's `(l - sprY) & 255` row test would also draw it wrapped at the
       top of the screen).
     - The user hasn't hidden a layer or the sub screen via
       CommonSettings.dispLayers / showGpu.
     - The frame's scanline-band count fits GxBandTracker's boundary budget.

    Engine B has no 3D layer, no capture unit and no shadow/fog/edge
    marking at all -- confirmed from the CPU reference, not from memory:
    GPU_RenderLine_layer()'s 3D-layer branch is guarded by `if (gpu->core ==
    GPU_MAIN)`, GPU_RenderLine_DispCapture() is only called under the same
    guard, and GPU_setVideoProp() masks Engine B's DisplayMode to 2 bits'
    worth of `(gpu->core)?1:3`. So those are Engine-A-only concerns and are
    simply absent here rather than "not implemented yet".

    ------------------------------------------------------------------
    Affine BG / OBJ (task 8): technique + the two hardware facts it needed
    ------------------------------------------------------------------
     - Same corner-UV-quad technique as gx_gba_render.cpp's affine BG/OBJ:
       one quad per band (BG) / per sprite (OBJ) whose 4 corner UVs are the
       texture coordinates the CPU's per-pixel `X += PA / Y += PC` walk
       reaches at those corners, so GX's linear interpolation reproduces the
       whole walk. Baked RGB5A3 (native CI is queue item 11), GX_NEAR.
       Out-of-map / out-of-sprite samples read TRANSPARENT via a 1-texel
       transparent border + GX_CLAMP; a wrapping BG plane is baked at its
       own size and sampled GX_REPEAT (== the CPU's `& (size-1)`).
     - The per-band reference point is the CPU's own running BGxX/BGxY latch:
       Stage 0 predicts it (X+PB, Y+PD, matrix unchanged) each line and
       records a band boundary when the prediction breaks. At line 0 it reads
       GPU::affineInfo[] directly, because GPU_RenderLine(line 0) -- which
       re-latches BGxX/BGxY from it -- runs AFTER the hook.
     - HARDWARE FACT 1 -- the GX sample point is not the pixel centre. The
       console GPU (and Dolphin, which reproduces it) samples at 7/12 of a
       pixel, quantised further by the host GPU's sub-pixel grid; the corner
       UVs are compensated for it (kGxSamplePoint, tuned to 149/256 on this
       Dolphin host; see the comment on gxDsBAffineCorner for the numbers).
       Pure translation and integer scaling are blind to it, which is why it
       only showed up on rotated/scaled content.
     - HARDWARE FACT 2 -- affine textures are baked at POWER-OF-TWO sizes.
       Dolphin's GX_NEAR sampling of a non-power-of-two texture through
       sub-texel-exact interpolated UVs mis-sampled (20-wide: a wrong texel
       every 5th column; 36/132/516-wide: a small negative bias flipping
       lattice-edge texels); the same UVs on 32/64/256-wide textures were
       byte-exact. Cost: a bordered 512-wide plane is a 1024-wide texture.
     - A CPU-reference bug on big-endian hosts was found and fixed on the way
       (GPU.cpp lineRot/lineExtRot): `BGxX += LE_TO_LOCAL_16(BGxPB)` added
       the byte-swapped value as an unsigned 0..65535 int, so a NEGATIVE
       PB/PD advanced the reference point by +65536-|PB| instead of -|PB|.
       Now `(s16)`-cast. Only affine BG lines with a negative PB/PD change.

    ------------------------------------------------------------------
    Extended BG palettes (task 8 scope extension -- see the log)
    ------------------------------------------------------------------
     SM64DS's in-game sub screen is BG_Mode 3 with BG3 = extended-affine
     256x16 AND DISPCNT.ExBGxPalette_Enable, so without ext-palette support
     the affine work would never have engaged on the primary real-content
     target. Handled exactly where GPU.cpp uses them: a 256-colour text BG
     (`ExtPal[SUB][BGExtPalSlot]`, palette = tile palNum << 8) and the
     BGType_AffineExt_256x16 flavour (same indexing); every other BG kind
     ignores the flag, as in GPU.cpp. Dependency tracking: writes to bank H's
     LCDC window (0x06898000-0x0689FFFF) arm the whole-region VRAM flag, and
     the four MMU.ExtPal[1][] slot pointers are part of the bank-map
     fingerprint (a VRAMCNT_H remap re-bakes). Extended OBJ palettes
     (MMU.ObjExtPal) still bail.

    ------------------------------------------------------------------
    Test hooks (compile-time, zero cost when undefined)
    ------------------------------------------------------------------
     -DDSB_FORCE_CPU  gxDsEngineBRenderFrame() always bails -> the CPU
                      compositor renders everything: the byte-compare
                      reference for A/B captures.
     -DDSB_STATS      counts engaged/bailed frames, affine-BG / affine-OBJ
                      frames, the last bail reason and DISPCNT/BG types into
                      the harness profile log every 30 frames (needs
                      DESMUME_HARNESS + HARNESS_PROFILE).

    ------------------------------------------------------------------
    Deliberate deviations from the pipeline doc's ideal Stage 0/1/4
    ------------------------------------------------------------------
     - **RGB5A3 baking, not native CI4/CI8+TLUT.** Every BG plane and OBJ
       texture is baked with the palette already resolved on the CPU, one
       16-bit texel per pixel -- the GBA slice's original approach, before
       gx-next-steps-log.md task 6 converted it. The native-CI conversion
       for Engine B (including the per-BG-plane dedicated-TLUT-slot hazard
       task 6 documents, which would be *worse* here since Engine B's text
       BGs have the same per-tile palNum problem) is queued as a follow-up.
     - **Coarse VRAM dirty-gating.** Stage 0's dirty trace for Engine B
       tracks palette and OAM at 32-byte page granularity but treats VRAM as
       a single whole-region flag: any write landing in the BBG or BOBJ
       address windows re-arms every BG plane and every sprite bake for that
       frame. This matches the GBA slice's original coarse `anyDirty()` gate
       (tasks 4/5 are what refined it there) and is correct, just wasteful.
       Writes to the ABG/AOBJ windows (the bulk of a 3D game's VRAM traffic)
       do NOT dirty Engine B, so this is coarse rather than useless.
     - **Bake from frame-final VRAM/OAM/palette.** Exactly like
       gx_gba_render.cpp, Stage 1 runs once at end of frame and reads
       whatever the VRAM/OAM/palette contents are at that point. Only
       *layout registers* are tracked per scanline and replayed per band.
       A game that rewrites tile/sprite pixel data or palette entries
       mid-frame (rather than mid-frame scroll registers) will therefore see
       the final frame's data applied to the whole frame. This is an
       inherited simplification of the GBA slice's design, not a new one,
       and is documented here rather than assumed away.
     - **Per-band replay covers the layout registers Stage 4 actually
       consumes** -- per-BG enable, per-BG priority, per-BG type, per-BG
       HOFS/VOFS (text) or reference point + PA..PD (affine), OBJ
       enable -- captured per scanline (gxDsEngineBScanline) rather than at
       a register-write funnel. Everything else a BG plane's *bake* depends
       on (char base, screen base, size selector, colour depth) is read once
       at bake time, i.e. the frame's final value, same cut gx_gba_render.h
       documents for the GBA. Any other Engine-B state change mid-frame that
       this file can't replay (a window turning on, a blend mode appearing,
       master brightness, a mode switch, ...) is detected by the same
       per-scanline hook and bails the whole frame, so it degrades to the
       CPU compositor rather than rendering something wrong.
     - **Sub-screen-only.** Stage 6 ("Engine B + display composition") is
       not addressed here: Engine B's finished frame is written back into
       GPU_screen at SubScreen.offset in the same native X-B5-G5-R5 layout
       GPU_RenderLine() would have produced, so main.cpp's Draw() /
       draw_thread / harness_frame.cpp / savestates are all completely
       unaffected. The GBA slice's task-3 direct-present fast path has no
       equivalent here yet.
     - **GX present state is saved/restored around this file's draw.**
       main.cpp establishes viewport/scissor/projection/Z-mode and the
       EFB-copy source+destination ONCE in InitVideo() and never re-applies
       them per frame; gxDsBSetup2DState() necessarily changes all of those.
       This file therefore calls main.cpp's GxRestorePresentState() after
       its EFB copy, and holds `vidmutex` for the whole GX sequence so
       draw_thread cannot interleave its own quads into the same FIFO
       mid-render. (gx_gba_render.cpp does neither today; not changed here,
       since GBA mode is explicitly out of this task's scope -- flagged in
       gx-next-steps-log.md task 7 as a real follow-up for that file.)

    GX-only (libogc), like gx_gba_render.* -- verified by cross-compilation
    and Dolphin capture, not by a host-side unit test.
*/
#ifndef GX_DS_ENGINEB_RENDER_H
#define GX_DS_ENGINEB_RENDER_H

#include <stddef.h>
#include "../types.h"
#include "gx_frameplan.h"

// Stage 0 draw plan for DS Engine B. Sized/cleared by this file itself
// (gxDsEngineBRenderInit / the end-of-frame reset in
// gxDsEngineBRenderFrame); populated by gxDsEngineBMarkWrite() from
// MMU.cpp's ARM9 write funnel and by gxDsEngineBScanline() from
// NDSSystem.cpp's per-scanline render site.
extern GxFramePlan g_dsBFramePlan;

// Per-band snapshot of exactly the Engine-B layout state Stage 4 replays.
// Entry 0 is the frame-start (scanline 0) snapshot; entry N is the state
// first seen at band boundary N-1, in lockstep with
// g_dsBFramePlan.bands -- the same index correspondence gx_gba_render.h's
// GxGbaBandRegs relies on, and valid for the same reason (scanlines are
// visited in strictly increasing order within a frame).
struct GxDsBBandRegs {
	// --- compared band-to-band with memcmp (a change => new band boundary) ---
	u8  bgEnable;     // bit n = BG n enabled this band (DISPCNT BGn_Enable)
	u8  objEnable;    // DISPCNT OBJ_Enable this band
	u8  bgPrio[4];    // BGxCNT Priority (0..3) this band
	u8  bgType[4];    // GPU::BGTypes[n] (BGType enum) this band; Stage 4 dispatches on it
	u16 hofs[4];      // BGxHOFS & 0x1FF this band (0 for a non-text BG: unused by affine)
	u16 vofs[4];      // BGxVOFS & 0x1FF this band (0 for a non-text BG)
	// --- NOT memcmp'd: task 8 affine state for BG2 (index 0) / BG3 (index 1).
	// Meaningful only for an enabled, non-text BG; zero otherwise. These are
	// the values *at this band's first scanline* (the CPU reference's running
	// BGxX/BGxY latch, which advances by PB/PD per rendered line) -- a boundary
	// is recorded when the per-line prediction X+PB / Y+PD or PA..PD stop
	// matching, i.e. on a reference-point/matrix rewrite, not on every line.
	s32 affX[2], affY[2];
	s16 affPA[2], affPB[2], affPC[2], affPD[2];
};
// Bytes of GxDsBBandRegs that are compared with memcmp (everything before the
// affine block; ends exactly at vofs's end so no padding bytes are included).
#define GX_DSB_BANDREGS_CMP_BYTES ((int)(offsetof(GxDsBBandRegs, vofs) + sizeof(((GxDsBBandRegs *)0)->vofs)))
static const int GX_DSB_MAX_BAND_REGS = GxBandTracker::kMaxBands;
extern GxDsBBandRegs g_dsBBandRegs[GX_DSB_MAX_BAND_REGS];

bool gxDsEngineBRenderInit();
void gxDsEngineBRenderShutdown();

// Stage 0, called once per visible scanline (0..191) from NDSSystem.cpp's
// execHardware_hblank(), immediately before GPU_RenderLine(&SubScreen, l).
// Snapshots Engine B's per-band layout state, records a band boundary when
// it changed since the previous line, and latches an "out of scope this
// frame" flag if any scanline's configuration falls outside this file's
// documented scope (see the header comment above).
void gxDsEngineBScanline(int line);

// Stage 1 + Stage 4, called once per frame from the same site right after
// the last visible scanline (191) has been rendered -- the DS analogue of
// gbaPpuEndFrame()'s gxGbaRenderFrame() call. Returns true if it drew and
// overwrote Engine B's 192 scanlines in GPU_screen (superseding what
// GPU_RenderLine already put there), false if this frame is out of scope,
// in which case GPU_screen is left exactly as the CPU compositor wrote it.
// Always resets the frame plan for the next frame before returning, so that
// VBlank-period writes are attributed to the next frame's trace.
bool gxDsEngineBRenderFrame();

// Stage 0 dirty tagging, called from MMU.cpp's ARM9 write funnel (the
// single MMU_LCDmap() tail shared by write08/16/32, which every CPU *and*
// DMA write to palette RAM / VRAM / OAM passes through). `adr` is the
// already-masked 0x0FFFFFFF address; only 0x05 (palette), 0x06 (VRAM) and
// 0x07 (OAM) are of interest and everything else returns immediately.
void gxDsEngineBMarkWrite(u32 adr, u32 size);

#endif // GX_DS_ENGINEB_RENDER_H
