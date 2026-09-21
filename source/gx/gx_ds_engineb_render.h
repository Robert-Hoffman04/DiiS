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
    and BOBJ page ranges (plus the four MMU.ExtPal[1][] slot pointers) and
    re-bakes/invalidates EVERY cached texture when they change (see
    gxDsBBankMapChanged()). That is still the only whole-cache trigger.

    ------------------------------------------------------------------
    Dirty gating (task 10) -- per-plane / per-OBJ-slot read-set gates
    ------------------------------------------------------------------
    Replaces phase 1's single whole-region VRAM flag (any Engine-B-visible VRAM
    write, any palette write, any OAM write => re-bake all four planes and every
    visible sprite -- for a 1024x1024 affine plane a 2MB bake). Shape follows the
    GBA slice's tasks 4/5 (config fingerprint + `GxDirtyBitmap::isPageDirty` over
    exactly the bytes the bake reads) with three deliberate differences:
     - WRITES are tracked in LCDC SPACE: 0xA4000 bytes at 1KB pages = 656 pages.
       MMU.cpp tags each mapped VRAM write with the address MMU_LCDmap() just
       RESOLVED (post-map, `adr - 0x06000000` = a byte offset into MMU.ARM9_LCD), so
       ARM9 mirrors, bank remaps and the LCDC window (bank H's ext palette is filled
       through it) all land on the same bytes. The display-capture unit, which writes
       guest VRAM without the MMU funnel, tags its destination line too (GPU.cpp).
       Palette RAM (32B pages) and OAM stay tagged pre-map as before.
     - DEPENDENCIES ARE RECORDED, NOT DERIVED. While a plane / sprite bakes, every
       VRAM read (a host pointer out of MMU_gpu_map(), i.e. into MMU.ARM9_LCD) and
       palette read is marked in a per-texture read-set (`GxDsBDeps`: a 656-bit VRAM
       bitmap + 64-bit palette bitmap, 92 bytes). So the gate is exact by construction
       and cannot drift from the bake: the tiles the tilemap actually references (not
       the GBA's "1024 tiles worst case"), the tilemap bytes themselves, the 16-colour
       sub-palettes actually used (4bpp text/OBJ) or the whole 512-byte bank (8bpp /
       affine lut), the extended-palette slices actually used (LCDC-space, so a fill
       through bank H's LCD window is caught), bitmap rows, and sprite rows as a
       PHYSICAL span (a row that runs off the end of its 16KB page reads the next LCDC
       page, as the bake's own pointer arithmetic does). Engine A's palette/VRAM/OAM
       traffic no longer re-arms anything.
     - EVERY cached texture is checked EVERY frame, drawn or not. The dirty bitmaps
       are cleared at the end of each successful frame, so a plane that is disabled, a
       sprite that is parked/invisible/OBJ-disabled, or a brighten/darken variant not
       in use this frame would otherwise miss a write made in that interval and show
       stale texels when next used. Such a texture is invalidated (`valid = false`) on
       the frame its read-set was dirtied (task 9 did this with the coarse flag; task
       10 keeps it with the precise one). This hole is documented for the GBA slice
       (task 5) and is NOT fixed there -- see queue item 21.
     - Config fingerprints are unchanged (BGxCNT/type/wrap/size, OAM tile/pal/depth/
       size/mapping/boundary, fade key) and still force a re-bake on a mismatch; a
       scroll (HOFS/VOFS/affine origin/matrix) and a sprite move never re-bake.
     - NDS_Reset() (hence savestate load) invalidates everything: those rewrite
       VRAM/palette/OAM without any MMU write.
    Not tracked, by construction: ARM7 writes to VRAM (an ARM7-mapped bank cannot also
    be BBG/BOBJ; the VRAMCNT remap that would expose it is caught by the bank-map
    fingerprint). Mid-frame VRAM/palette writes still resolve to frame-final data
    (inherited, see below).

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
     - No mosaic on any enabled BG or any drawn sprite, no forced blank, no OBJ
       window (DISPCNT WinOBJ_Enable). (Extended BG palettes ARE handled since
       task 8: 256-colour text BGs and the 256x16 extended-affine flavour, nothing
       else -- see "Extended BG palettes" below; extended OBJ palettes since task
       11, see "Native CI4/CI8 + TLUT".)
       TASK 9 brought WIN0/WIN1 windows, all BLDCNT effects, semi-transparent
       OBJ and MASTER_BRIGHT IN scope -- see "Windows / colour effects /
       MASTER_BRIGHT (task 9)" below for exactly what is exact, what is
       near-exact, and the narrow conditions that still bail (window +
       brighten/darken, alpha over a mixed 2nd-target set, blended sprite
       overlapping a sprite, EVA+EVB != 16, different fade keys per band).
     - No OBJ-window sprite (Mode 2 non-affine is skipped exactly; an AFFINE
       Mode-2 sprite bails), no bitmap OBJ (Mode 3), and no affine sprite whose
       box hangs past scanline 255 (the CPU's `(l - sprY) & 255` row test would
       also draw it wrapped at the top of the screen). Semi-transparent
       (Mode 1) sprites, plain and affine, are IN scope since task 9.
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
       whole walk. Baked as native CI8 + TLUT since task 11 (RGB5A3 for the direct-colour
       bitmap and multi-slice extended-palette planes), GX_NEAR.
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
    Windows / colour effects / MASTER_BRIGHT (task 9)
    ------------------------------------------------------------------
    Semantics were read from GPU.cpp (renderline_checkWindows,
    _master_setFinalBGColor / _master_setFinalOBJColor, setup_windows /
    update_winh, GPU_RenderLine_MasterBrightness), not from the GBA slice.
     - WINDOWS (WIN0/WIN1; exact, byte-identical to the CPU). Each band is cut
       into disjoint rectangles (gxDsBBuildRegions: y-slabs x x-runs, honouring
       the DS wrap rule start>end => [0,end] U [start,255] and start==end =>
       empty) and every layer is drawn ONLY into rectangles whose window
       region (WIN0 > WIN1 > outside) enables it -- NOT the GBA slice's
       "outside pass then scissored overdraw" scheme, which cannot express "layer
       off inside WIN1" when it is on outside (see the log: a latent GBA bug).
       The backdrop is never windowed (GPU.cpp always draws it).
     - BRIGHTEN / DARKEN (exact): a bake-time texel transform through the SAME
       float tables GPU_InitFadeColors builds (gxDsBFadeColor), baked into a second
       "fade variant" texture per plane / sprite. The backdrop is faded per band
       when it is a 1st target and no window is active.
     - ALPHA BLEND / SEMI-TRANSPARENT OBJ (near-exact: GX-high by exactly 1 LSB
       of one 5-bit channel on roughly 40% of blended pixel-channels, never more).
       Drawn as constant-alpha (EVA*16, via a TEV compare op so the RGB5A3 3-bit
       alpha sub-format is not used) SRCALPHA/INVSRCALPHA over the EFB. The 1-LSB
       bias is the 8-bit expansion (c<<3|c>>2 = 8.25c) of the two operands
       being blended and truncated back to 5 bits; it cannot be removed on the
       GX side (a fix-up pass would still leave ~5%, see the log). Preconditions,
       checked per band and window region BEFORE any GX work (gxDsBPlanBand):
       GX can only blend with "whatever is in the EFB", so an alpha item is
       native only if EVERY layer that can lie beneath it is a 2nd target, or
       NONE is (then the CPU never blends it = an ordinary draw). Mixed sets
       bail ("blendunder"), as does EVA+EVB != 16 ("blendcoef"; SRC/INVSRC
       alpha can only express complementary weights) and a blended sprite whose
       box overlaps another sprite ("blendobjoverlap": the CPU resolves
       sprite-vs-sprite first and blends only the winner). A sprite never blends
       with another sprite pixel (`bg_under != 4`), and a Mode-1 sprite blends
       independently of BLDCNT's effect field / OBJ 1st-target bit / window
       effect bit -- all reproduced.
     - MASTER_BRIGHT (exact): sampled per scanline by Stage 0 and applied on the
       CPU in the EFB readback loop through the same per-channel tables (factor
       16 => all white / all black), i.e. after the GX composite exactly like
       GPU_RenderLine_MasterBrightness runs after the CPU one. No GX pass.
     - Still bails: OBJ window; mosaic (BG and OBJ; unchanged); a window
       combined with brighten/darken ("winfade": GPU.cpp's windowed backdrop tests
       a STALE `blend1` left by the previous line's last layer, a quirk with no
       model here); two bands needing different (mode, EVY) fade variants
       ("fadekeys").
     - Two big-endian bugs in the CPU reference's backdrop were fixed on the way
       (GPU.cpp GPU_RenderLine_layer): the brighten/darken backdrop fill indexed
       the fade table with a byte-swapped colour, and the windowed per-pixel
       backdrop path stored a byte-swapped colour. Both were garbage on this
       Wii host only; identical on little-endian. Engine A is affected the same
       way (it now renders windowed / faded backdrops correctly).

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
     (MMU.ObjExtPal) bailed until task 11, which handles them (below).

    ------------------------------------------------------------------
    Native CI4/CI8 + TLUT (task 11)
    ------------------------------------------------------------------
     Every base bake (plain and brighten/darken fade variant) is now a palette-INDEX
     texture + a TLUT instead of pre-resolved RGB5A3. The bake first writes a u16
     scratch of "palette indices" (0 = transparent: index 0 of the tile/sprite, the
     affine border and pow2 padding; else the index the CPU would look up), then
     either compacts it to CI8/CI4 and builds a TLUT (RGB5A3 entries, entry 0 = alpha
     0, so transparency and the clamp-border technique are unchanged) or resolves it
     through the same palette to RGB5A3 (the fallback). Formats:
       - text BG 4bpp: CI8 with the COMBINED index palNum*16+idx (a plane spans
         several 16-colour sub-banks; GX binds one TLUT per texture, and a CI4 TLUT
         could only be one sub-bank), TLUT = the sub-banks that have an opaque texel
         (the rest are 0, keeping the TLUT a pure function of the recorded read-set).
         Same answer task 6 gave the GBA.
       - text BG 8bpp / affine / 256x1 / Large8bpp on the regular palette: CI8,
         256-entry TLUT of the BG palette bank (entry 0 transparent).
       - extended-palette BG (256-colour text, AffineExt_256x16): the per-TILE palNum
         picks one of 16 256-colour slices, i.e. a 12-bit index. CI8 only if the plane
         uses a SINGLE slice (blank tiles' palNum is ignored: only slices with an
         opaque texel count); a multi-slice plane falls back to RGB5A3 (exact, 2x
         the bytes). A CI14X2 / per-slice multi-draw scheme was not attempted.
       - AffineExt_Direct (15-bit colour bitmap): RGB5A3, not palette-indexed.
       - OBJ 4bpp: CI4 with the sprite's own 16-entry TLUT (one palIndex per sprite).
         OBJ 8bpp: CI8, 256-entry TLUT of the OBJ palette, or (DISPCNT
         ExOBJPalette_Enable) of the sprite's own extended slice
         MMU.ObjExtPal[1][0] + palIndex*0x200 -- exactly GPU.cpp's rule, in both
         its plain and rot/scale 256-colour branches; a 16-colour sprite ignores the
         flag. A sprite has one fixed palIndex, so unlike an extended BG palette
         there is no per-tile problem and the conversion is always exact.
       - brighten/darken: NOT a separate texture design -- the same CI bake with the
         fade applied to the TLUT entries (gxDsBOpaqueTexel), byte-identical to the
         RGB5A3 fade bake by construction. Alpha / semi-transparent OBJ never bake
         colour (constant-alpha TEV compare on the texel alpha, which the TLUT
         supplies), so they are format-independent.
     TLUT hardware-slot budget (documented in the code, gxDsBBindTlut): Engine B
     reserves FIVE of the 16 names -- GX_TLUT12..15 for BG0..3 (a plane's plain and
     fade variants share their BG's name) and GX_TLUT11 for every sprite -- leaving
     GX_TLUT0..10 for Engine A / 3D. Sharing a name is only safe if the TLUT is
     (re)loaded whenever the drawer differs from the last loader, so every draw calls
     gxDsBBindTlut(), which tracks the owner of each name and issues GX_LoadTlut on a
     change; the tracking is reset at the start of every Engine B GX sequence, so
     nothing assumes a TLUT survived from an earlier frame or another engine.
     Dirty gating is UNCHANGED (task 10): the read-set recorded for the RGB5A3 bake is
     exactly what the CI bake reads, so a palette write still invalidates exactly the
     textures that read it; a palette-only change re-bakes the index texture too
     (correct, ~free at these sizes; a TLUT-only rebuild is a possible refinement).
     Extended OBJ palettes: DISPCNT.ExOBJPalette_Enable no longer bails; toggling it
     (or ExBGxPalette_Enable) mid-frame bails (`extpalchange`); the VRAMCNT_I pointer
     MMU.ObjExtPal[1][0] joined the bank-map fingerprint; the slice a sprite read is
     in its read-set as an LCDC range, so a bank-I LCD-window fill is caught.
     Memory: ~50% of the RGB5A3 bytes (CI8), ~25% for 4bpp sprites (CI4).

    ------------------------------------------------------------------
    Test hooks (compile-time, zero cost when undefined)
    ------------------------------------------------------------------
     -DDSB_FORCE_CPU  gxDsEngineBRenderFrame() always bails -> the CPU
                      compositor renders everything: the byte-compare
                      reference for A/B captures.
     -DDSB_STATS      counts engaged/bailed frames, affine-BG / affine-OBJ
                      frames, the last bail reason and DISPCNT/BG types into
                      the harness profile log every 30 frames (needs
                      DESMUME_HARNESS + HARNESS_PROFILE). Task 10 adds a `dsbk`
                      line: bakes per BG plane (plain / fade variant) and per
                      OAM index, plus `old_bg` / `old_obj`, the bakes the
                      pre-task-10 whole-region gate WOULD have done on the same
                      frames (in stats builds MMU tags still feed that flag).
     -DDSB_VERIFY     (with DSB_STATS) every cached texture the gate calls
                      fresh is re-baked into scratch and byte-compared; the
                      `stale=` counter of the `dsbk` line must stay 0.
     -DDSB_NOCI       (task 11) every bake uses the old pre-resolved RGB5A3 form
                      (the always-present fallback path): the A/B reference for
                      the CI conversion and the memory-saving baseline. -DDSB_STATS
                      adds a `dsbm` line (live/baked texture bytes CI vs the RGB5A3
                      equivalent, fallback count, TLUT loads, bake time).
     -DDSB_MUTATE_TLUTSHARE / _TLUTNOBIND   (task 11) TLUT-clobber mutations for
                      tools/vsd-testrom/cirom: every texture shares ONE TLUT name /
                      per-BG names + one OBJ name but loaded at bake time only.
                      The fixtures must FAIL (they do; see the task 11 log section).
     -DDSB_MUTATE_NOINVAL / _NODEPS / _NOBANK
                      mutation tests for the tools/vsd-testrom/dirtyrom fixtures: drop the
                      not-drawn-texture invalidation / make the dependency gate
                      never fire / ignore VRAMCNT remaps. The fixtures must
                      then FAIL (they do; see the task 10 log section).

    ------------------------------------------------------------------
    Deliberate deviations from the pipeline doc's ideal Stage 0/1/4
    ------------------------------------------------------------------
     - **Native CI4/CI8+TLUT: RESOLVED by task 11** (see "Native CI4/CI8 + TLUT"
       above). Remaining RGB5A3 bakes: the direct-colour bitmap plane, multi-slice
       extended-palette planes (per-tile slice selection cannot be one TLUT), the
       1x1 backdrop (as in the GBA slice), and -DDSB_NOCI builds.
     - **Dirty gating: RESOLVED by task 10** (see "Dirty gating" above; this used
       to be a single whole-region VRAM flag). Deviations from the GBA pattern it
       was modelled on: dependencies are recorded during the bake (a read-set)
       instead of derived from config, and the check runs for every cached texture
       every frame, invalidating the ones not being drawn (the GBA slice has a
       documented hole there, queue item 21). Granularity: 1KB LCDC pages, so two
       sprites/tile sets closer than 1KB share a page (a write to one re-bakes the
       other; conservative, never stale).
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
       enable, and (task 9) BLDCNT/BLDALPHA/BLDY + WIN0/WIN1 geometry and
       masks; MASTER_BRIGHT is a per-line array instead of a band -- captured per scanline (gxDsEngineBScanline) rather than at
       a register-write funnel. Everything else a BG plane's *bake* depends
       on (char base, screen base, size selector, colour depth) is read once
       at bake time, i.e. the frame's final value, same cut gx_gba_render.h
       documents for the GBA. Any other Engine-B state change mid-frame that
       this file can't replay (an OBJ window turning on, mosaic appearing, a mode
       switch, ...) is detected by the same
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
	// --- task 9: window / colour-effect registers (compared with memcmp too).
	// Coordinates are zeroed for a disabled window and the window masks for a
	// frame-region with no window at all, so an unused register being rewritten
	// does not spawn a needless band boundary.
	u16 bldcnt;       // BLDCNT & 0x3FFF (effect field, 1st-target bits 0-5, 2nd-target bits 8-13)
	u8  winEn;        // bit0 WIN0 enabled, bit1 WIN1 enabled (OBJ window bails)
	u8  win0h0, win0h1, win0v0, win0v1;   // WIN0 X1/X2/Y1/Y2 as GPU.cpp stores them (0 if disabled)
	u8  win1h0, win1h1, win1v0, win1v1;
	u8  winIn0, winIn1, winOut;           // 5-bit layer-enable masks (BG0-3, OBJ)
	u8  winSp;        // bit0 WININ0 effect, bit1 WININ1 effect, bit2 WINOUT effect
	u8  eva, evb;     // BLDALPHA EVA/EVB (already clamped to 16 by GPU.h)
	u8  evy;          // BLDY EVY (already clamped to 16); 0 unless the effect is brighten/darken
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
// affine block; ends exactly at evy's end so no trailing padding bytes are included).
#define GX_DSB_BANDREGS_CMP_BYTES ((int)(offsetof(GxDsBBandRegs, evy) + sizeof(((GxDsBBandRegs *)0)->evy)))
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
// already-masked 0x0FFFFFFF address; only 0x05 (palette) and 0x07 (OAM) are
// tracked here (task 10: VRAM moved to gxDsEngineBMarkVram) and everything
// else returns immediately.
void gxDsEngineBMarkWrite(u32 adr, u32 size);

// Task 10: VRAM dirty tagging in LCDC space. Called from MMU.cpp right AFTER
// MMU_LCDmap() has resolved a mapped ARM9 VRAM write (with `adr - 0x06000000`,
// a byte offset into MMU.ARM9_LCD, 0 .. 0xA3FFF), and from GPU.cpp's display
// capture, which writes guest VRAM without going through the MMU.
void gxDsEngineBMarkVram(u32 lcdcOffset, u32 size);

// Task 10: force every cached Engine B texture to re-bake on the next frame
// (NDS_Reset(), hence savestate load: VRAM/palette/OAM are rewritten wholesale).
void gxDsEngineBInvalidateAll();

#endif // GX_DS_ENGINEB_RENDER_H
