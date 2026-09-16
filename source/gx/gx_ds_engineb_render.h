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
    Scope (phase 1) -- everything outside it bails to the CPU compositor
    ------------------------------------------------------------------
    gxDsEngineBRenderFrame() returns false, leaving GPU_screen exactly as
    GPU_RenderLine() already rendered it, unless ALL of the following hold
    for every visible scanline of the frame:

     - DS mode (not GBA), Engine B display mode 1 ("display BG and OBJ
       layers"); mode 0 (white) has no GX work worth doing and mode 2/3 are
       Engine-A-only anyway.
     - DISPCNT BG_Mode == 0, i.e. all four BGs are plain text-mode BGs
       (GPU.cpp's GPU_mode2type table row 0). No affine, no extended-affine,
       no large-8bpp, no bitmap BG.
     - No windows (WIN0/WIN1/OBJ window all disabled), no colour special
       effect (BLDCNT effect field 0), no mosaic on any enabled BG or any
       drawn sprite, no MASTER_BRIGHT, no extended BG/OBJ palettes, no
       forced blank.
     - Every drawn sprite is a regular (non-rot/scale) OBJ in Mode 0
       (normal): no affine OBJ, no semi-transparent OBJ (which blends
       against the layer beneath it regardless of BLDCNT's effect field --
       see GPU.cpp's _master_setFinalOBJColor), no OBJ-window sprites, no
       bitmap OBJ.
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
       consumes** -- per-BG enable, per-BG priority, per-BG HOFS/VOFS, OBJ
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
	u8  bgEnable;     // bit n = BG n enabled this band (DISPCNT BGn_Enable)
	u8  objEnable;    // DISPCNT OBJ_Enable this band
	u8  bgPrio[4];    // BGxCNT Priority (0..3) this band
	u16 hofs[4];      // BGxHOFS & 0x1FF this band
	u16 vofs[4];      // BGxVOFS & 0x1FF this band
};
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
