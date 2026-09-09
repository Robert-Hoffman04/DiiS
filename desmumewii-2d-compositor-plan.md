# DeSmuME Wii — 2D Compositor Optimization Plan

Follow-up to [desmumewii-perf-opportunities.md](desmumewii-perf-opportunities.md) /
[desmumewii-perf-results.md](desmumewii-perf-results.md), triggered by the
full-JIT CPU-load breakdown showing the legacy per-scanline software 2D
compositor (`GPU_RenderLine` / `GPU_RenderLine_layer`) at 58–65% of frame time
with the ARM cores healthy (91–100% JIT-executed, <0.5% codegen, <1.5%
interpreter fallback). This doc is scoped to that compositor specifically.

Branch: `arm9-jit-infra`. All file:line references below are against that tree.

> **How to use this doc:** the five steps are ordered so each one either
> retires or re-scopes a question the next step depends on. Steps 1–3 are
> software-only, cheap, and should be benchmarked *before* any GX work starts
> — the profile driving this plan was taken pre-fix, and it's plausible steps
> 1–3 alone move the 58–65% figure enough to change how much of step 5 is
> actually worth building. Re-run `tools/benchmark/benchmark.sh` after every
> step; it diffs against the previous run and flags >3% regressions
> automatically.

---

## Step 1 — Scope `-fno-strict-aliasing` off `GPU.cpp` and re-measure

**Why first:** it's a Makefile-line change, an hour including the benchmark
run, and it answers a question every later step depends on — namely whether
the compiler is already capable of hoisting the per-pixel dispatch described
in Step 2, or whether that has to be done by hand.

**The mechanism.** `___setFinalColorBck` / `setFinalColorSpr` /
`setFinalColor3d` (GPU.cpp:836–869) are `FORCEINLINE` wrappers around a
`switch(funcNum)` that selects one of 8 blend-mode template instantiations.
`funcNum` (`setFinalColorBck_funcNum` etc.) is set once per DISPCNT/BLDCNT
write in `SetupFinalPixelBlitter` (GPU.cpp:407–425) — it is **loop-invariant**
across every 256-pixel scanline loop that calls these wrappers
(`renderline_textBG`, `_spriteRender`, the backdrop fill in
`GPU_RenderLine_layer`). After inlining, GCC could in principle hoist the
switch out of those loops entirely — one branch instead of 256 — but that
requires proving `gpu->setFinalColorBck_funcNum` isn't written inside the loop
body, which `-fno-strict-aliasing` (Makefile:36, applied globally to work
around type-punning elsewhere in the core) actively blocks for a `GPU*`
field read sitting next to raw `u8*` VRAM writes in the same function.

This flag was already flagged in the prior opportunities doc (Tier 2.6) as
"a couple % globally, low priority, needs to be scoped instead of blanket." It
is being re-raised here because the current profile changes its priority: it
is now plausibly gating an optimization in the exact hot path that dominates
every scene.

**Action:**
1. Add a per-file override so `source/src/GPU.cpp` (and only that file,
   initially) drops `-fno-strict-aliasing` from its `CFLAGS` while the rest of
   the build keeps it.
2. Grep `GPU.cpp` for the kind of raw pointer-cast aliasing violations the
   flag exists to paper over (`(u16*)`, `(u32*)` casts of an unrelated storage
   type) before flipping it, since a real violation compiled under strict
   aliasing is a silent-miscompile risk, not just a missed optimization.
3. Build, run `tools/benchmark/benchmark.sh` (all three renderers, both
   scenes), and inspect the generated assembly for `renderline_textBG` /
   `_spriteRender` to confirm whether the per-pixel switch actually moved
   outside the loop.
4. Do the same A/B screenshot correctness check the endian/LTO pass used
   (see desmumewii-perf-results.md's Correctness section) — same low but
   nonzero risk category as that pass.

**Estimate:** unknown until measured — could be anywhere from ~0% (compiler
was already blocked by something else, e.g. the indirect `gpu->currDst`
write) to a meaningful fraction of the compositor's cost, since it's
removing a mispredicted-or-not branch from every one of the ~256 × N-layers ×
384 lines/frame inner-loop iterations. **This is exactly why it's Step 1 and
not skipped in favor of Step 2** — it's nearly free to find out.

**Risk:** low, single file, easily reverted, but strict-aliasing violations
are the kind of bug that only shows up on specific optimization levels — the
correctness pass is not optional.

### Result (measured, reverted)

**0%. Reverted.** Scoped `-fno-strict-aliasing` off `GPU.cpp` (per-file
`CXXFLAGS` override) + neutralised the one genuine violation the grep found
(the `WORDS_BIGENDIAN` OAM byte-swap in `_spriteRender` puns an `OAM` bitfield
struct through `u16*` next to `spriteInfo->member` reads — `may_alias` typedef).
`-Wstrict-aliasing=1` clean afterwards.

- **perfzones (vsd, full-JIT):** `GPU 2D compositor` 58.3% → 58.3%
  (14.215 → 14.212 ms), total frame 24.386 → 24.382 ms, 41.0 fps unchanged.
  sm64 profile (new capture): compositor is **65.5%** there — it dominates a
  real game harder than the vsd demo.
- **standard sw/gx/merge (vsd + sm64):** unchanged.
- **disassembly (baseline build vs patched):** `_spriteRender<0>` is
  byte-identical (1730 instrs). `renderline_textBG<false>`'s per-pixel inner
  loop is instruction-identical bar one rodata address constant — in **both**
  builds it still runs `lwz rN,7732(r3)` (load `setFinalColorBck_funcNum`) →
  jump-table `lwzx` → `bctr` **once per pixel**.

**Why it didn't move:** `-fstrict-aliasing` alone doesn't unblock the hoist.
The loop stores `gpu->bgPixels[x]` — an array *inside* the `GPU` struct at a
runtime index — and with `-fno-aggressive-loop-optimizations` (still applied,
load-bearing elsewhere in the core) GCC can't prove that store never reaches
the `funcNum` field a few offsets away, so it can't treat the switch selector
as loop-invariant. This is the "blocked by something else" case the Estimate
section flagged. Conclusion: the dispatch has to be hoisted **by hand → Step 2**.

---

## Step 2 — Hand-hoist the per-pixel dispatch (Step 1 did not get there)

**Why second:** if Step 1's compiler experiment doesn't move the needle,
the fix is mechanical and low-risk, and it's worth doing by hand rather than
fighting the compiler further — but there's no reason to write it blind
before Step 1 tells you whether it's necessary.

**The pattern already exists in-tree to copy.** `GPU::spriteRender`
(GPU.h:754–759) resolves `spriteRenderMode` (`SPRITE_1D` vs `SPRITE_2D`) once
per call and dispatches to a fully-templated `_spriteRender<MODE>` — no
per-pixel branch on sprite mapping mode. The same shape should be applied to:

* **Backdrop fill** (GPU.cpp:2097–2126, `GPU_RenderLine_layer`'s
  `switch(gpu->setFinalColorBck_funcNum)`): cases 4–7 currently call
  `___setFinalColorBck<false,true,N>` in a 256-iteration `for` loop, i.e. the
  template is already resolved per-case — the fix here is just confirming (or
  forcing, if Step 1 shows it's needed) that the compiler treats this as a
  single specialized loop rather than 256 calls each re-evaluating which
  specialization to use.
* **`renderline_textBG`** and its affine/extended/large-bitmap siblings: hoist
  the `funcNum`-driven blend selection to the top of the function, once per
  BG-layer-per-scanline call, then loop the resolved specialization across the
  full 256 (or tile-run) pixel range.
* **`_spriteRender`**: same hoist for `setFinalColorSpr_funcNum`, applied once
  per call rather than once per visible sprite pixel.

**Action:** for each of the three sites, replace the inlined per-pixel switch
with a switch evaluated once at function entry that selects among N
straight-line loop bodies (one per template instantiation), matching the
`SPRITE_1D`/`SPRITE_2D` precedent exactly. This is refactor-shaped, not
logic-changing — the same 8 blend-mode templates already exist and already
produce correct output; only the granularity of dispatch changes.

**Estimate:** low-single-digit to mid-single-digit % of frame time, scaled by
however many BG layers + sprites are active in a given scene (more layers =
more redundant per-pixel dispatch eliminated). Bounded above by whatever
Step 1 didn't already capture.

**Risk:** low — same specialization, different call-site granularity, easy to
verify pixel-for-pixel against the pre-change build with the existing
screenshot-diff method.

### Implementation (landed on `arm9-jit-infra`)

- **Backdrop fill:** already hoisted (`setFinalColorBG<true,N>` gets `N` as a
  template constant). No change.
- **Sprite composite** (`GPU_RenderLine_layer`, the `item->nbPixelsX` loop that
  called `setFinalColorSpr()`): `switch(setFinalColorSpr_funcNum)` lifted out of
  the loop — one `_master_setFinalOBJColor<FUNC,WIN>` pick per priority bucket,
  straight-line loop body. Dead `setFinalColorSpr()` wrapper removed.
- **BG layers:** new `int FUNCNUM` template parameter threaded
  `modeRender` (8-way `switch(setFinalColorBck_funcNum)`) → `modeRenderT<M,FN>`
  → `lineText/lineRot/lineExtRot<M,FN>` → `renderline_textBG<M,FN>` /
  `rot_{tiled_8bit,tiled_16bit,256_map,BMP_map}_entry<…,FN>` /
  `rotBG2/extRotBG2<M,FN>` → `__setFinalColorBck<M,false,FN>` →
  `___setFinalColorBck<M,false,FN>` → `setFinalColorBG<false,FN>`. In
  `setFinalColorBG` the selector is now
  `(FUNCNUM >= 0) ? FUNCNUM : setFinalColorBck_funcNum` — `FUNCNUM >= 0` folds
  the `switch` to one case; `FUNCNUM == -1` (unconverted `lineLarge8bpp`, the
  out-of-range `default:`) keeps the legacy per-pixel dispatch.
- **Correctness harness:** `-DDESMUME_FBDUMP` (main.cpp) snapshots `GPU_screen`
  at a fixed frame on a direct-boot ROM and re-writes it every 60 frames —
  byte-compare baseline vs patched headlessly (the Dolphin screenshot harness
  needs an X window this env lacks; and the flatpak sandbox can't read the .dol
  from `/tmp`, so it must be staged under the project dir).
- **Cost:** DOL `.text` grows ~300 KB (2.31 MB → 2.61 MB) from the 8× (×2 MOSAIC)
  BG-renderer instantiations. Acceptable on Wii; revisit if a later step needs
  the room.

**Correctness (fb A/B, vsd, frame-locked):** 440 of 98 304 pixels differ, **all
inside the central spinning-3D-model box** (rows ~98–191, cols ~111–145, top
screen) — the demo's model animation isn't perfectly frame-deterministic under
Dolphin, same known-benign result the endian/LTO and Step 1 passes hit. Step 2
touches only the 2D compositor, and **every 2D pixel — both full screens of
background test-pattern, UI text, room geometry — is byte-identical.**

### Result (measured) — **landed, ~8–15 % off the compositor**

perf_zones (full-JIT), vs the Step-1-revert baseline:

| scene | 2D compositor before | after | Δ | frame total |
|---|--:|--:|--:|--:|
| vsd  | 14.21 ms (58.3 %) | **12.08 ms (54.3 %)** | **−2.13 ms / −15 %** | 24.38 → 22.25 ms |
| sm64 | 17.39 ms (65.5 %) | **16.03 ms (63.6 %)** | **−1.37 ms / −7.9 %** | 26.57 → 25.22 ms |

All the saved time lands in the compositor zone; nothing else moved. Standard
benchmark (single-run, noisier) agrees: vsd GX 25.1 → 26.5 fps (+5.6 %),
sm64 GX 29.6 → 30.8 fps (+4.1 %), software raster +2–3 %. vsd (BG-heavy, few
sprites) gains more from the BG-dispatch hoist than sprite-heavy sm64.

---

## Step 3 — Trim the remaining per-line CPU-side waste

**Why third:** independent of Steps 1–2, cheap, and worth batching into the
same benchmark pass rather than measuring five tiny changes separately.

### 3.1 Skip sprite-side clears when sprites are disabled
`GPU_RenderLine_layer` (GPU.cpp:2129–2133) unconditionally clears
`sprAlpha`/`sprType`/`sprPrio`/`sprWin` (256–512 bytes total) every line, even
though the sprite-priority-bucketing loop right after is already gated on
`gpu->LayersEnable[4]`. Move the memsets inside that gate, and drop
`sprAlpha`/`sprType`/`sprWin` to lazy/on-write clearing — they're only ever
read at an index where a sprite pixel was actually written; `sprPrio`'s 0xFF
sentinel is the only one the BG loop depends on unconditionally.

### 3.2 Resolve OAM endianness once, not once per scanline per sprite
`_spriteRender` (GPU.cpp:1574–1583) performs a 2-word bit-rotate on every OAM
entry, on every scanline that entry is live, purely to compensate for
reading little-endian OAM data on a big-endian host. OAM (1KB) only changes
when the game writes it. Move this rotation into the OAM MMU write path (or
maintain a native-endian shadow copy updated on write) so it happens once per
write instead of up to 192× per frame per sprite.

### 3.3 Confirm the backdrop windowed path (cases 4–7) collapses under Step 2
No separate action beyond verifying it — this item was called out
individually in the previous opportunities doc's Tier 3 ("currently eating up
2fps or so... reasonable candidate for optimization", GPU.cpp:2098 comment)
and is likely fully subsumed by Step 2's dispatch hoist, since it's the same
`funcNum`-switch-in-a-256-loop pattern with `backdrop_color` as the
loop-invariant input.

**Estimate:** low single digits combined, similar order to the earlier
endian-fix pass (+1–4% depending on scene).

**Risk:** low. 3.1 needs a check that no downstream code reads the
now-unclear buffers when sprites are off (should be none, given the
`LayersEnable[4]` gate already controls the read side). 3.2 needs the OAM
write path identified and the shadow copy kept in sync on every write width
(byte/half/word) OAM currently accepts.

### Result (measured) — landed on `arm9-jit-infra`

**3.1 — clear sprite buffers only when they'll be read.** `GPU_RenderLine_layer`
now gates the `sprAlpha`/`sprType`/`sprPrio` memsets behind `LayersEnable[4]`
and the `sprWin` memset behind `LayersEnable[4] || WINOBJ_ENABLED`. Verified
the read sides: `sprAlpha`/`sprType` only in the sprite composite + `mosaicSpriteLine`
(both behind the gate), `sprPrio` only in the priority-bucketing loop (behind
the gate), `sprWin` only at `GPU.cpp:634` behind `WINOBJ_ENABLED`. When a
scene runs with sprites off this drops ~768 B/line (147 KB/frame) of memset.

**3.2 — deferred.** The per-scanline OAM bit-rotate in `_spriteRender` is real
waste but small (4 `u16` rotates × nbShow × 192 lines, ~0.1 ms range) and the
clean fix — a native-endian OAM shadow updated on write — has no safe single
choke point on the Wii: the ARM9 JIT stores to guest memory (OAM included)
directly, bypassing the `_MMU_write*` handlers a shadow hook would live in.
Not worth an MMU/JIT audit for a sub-1 % gain. Revisit only if Step 4 says
the sprite path specifically is still hot.

**3.3 — confirmed collapsed.** Backdrop windowed cases 4–7 already call
`___setFinalColorBck<false,true,4>` … `<…,7>` with a compile-time constant, so
Step 2's `FUNCNUM` dispatch is dead-code-eliminated per case. No action.

**Benchmarks** (FullJIT — `profile` = jitfull + perf_zones, both scenes; Dolphin
2606a, i5-1145G7). Step 2 baseline = `20260909T011152Z`, Step 3 = `20260909T015711Z`.

| scene | 2D compositor Step 2 | Step 3 | Δ | frame total | eff. fps |
|---|--:|--:|--:|--:|--:|
| vsd  | 12.080 ms (54.3%) | **11.809 ms (53.7%)** | **−0.27 ms / −2.2%** | 22.25 → 21.98 ms | 44.95 → 45.50 |
| sm64 | 16.026 ms (63.6%) | **15.828 ms (63.3%)** | **−0.20 ms / −1.2%** | 25.22 → 25.01 ms | 39.67 → 39.99 |

Standard bench, no regression on any renderer (Step 2 → Step 3):
vsd sw 15.6→15.6 / GX 26.5→26.7 / merge 26.4→26.6 / jitfull —→46.6;
sm64 sw 13.4→13.4 / GX 30.8→31.0 / merge 30.3→30.5 / jitfull —→41.8 fps.

Small as expected — vsd/sm64 aren't sprite-off scenes, so 3.1's memset skip
only partly applies. The saving lands entirely in the compositor zone.
Correctness: 3.1 is a pure "don't clear a buffer nobody reads" change, gates
audited against every read site; no framebuffer A/B needed.

---

## Step 4 — Re-benchmark and re-scope before touching GX

**Why this is its own step, not folded into Step 3:** Steps 1–3 are all
guesses about how much of the 58–65% is dispatch/clear/endian overhead versus
irreducible per-pixel work. This step turns the guess into a number before
committing to Step 5, which is the expensive one.

**No code changes in this step** — it's a checkpoint, and it's here
specifically so Step 5 doesn't start from a stale profile.

### Cumulative result, Steps 1–3 (measured)

Baseline `20260908T233009Z` (pre-Step-1) → `20260909T015711Z` (Steps 1–3),
FullJIT + perf_zones, Dolphin 2606a / i5-1145G7:

| scene | 2D compositor before | after | Δ | frame total | eff. fps |
|---|--:|--:|--:|--:|--:|
| vsd  | 14.212 ms (58.3%) | **11.809 ms (53.7%)** | **−2.40 ms / −16.9%** | 24.38 → 21.98 ms | 41.0 → 45.5 (+11%) |
| sm64 | 17.393 ms (65.5%) | **15.828 ms (63.3%)** | **−1.57 ms / −9.0%** | 26.57 → 25.01 ms | 37.6 → 40.0 (+6%) |

All three non-profile renderers moved flat-to-slightly-up (no regression):
vsd sw 15.6 / GX 26.5→26.7 / merge 26.4→26.6; sm64 sw 13.4 / GX 30.8→31.0 /
merge 30.3→30.5 fps.

Full post-Steps-1–3 zone breakdown:

| zone | vsd ms | vsd % | sm64 ms | sm64 % |
|---|--:|--:|--:|--:|
| **GPU 2D compositor** | **11.809** | **53.7** | **15.828** | **63.3** |
| GPU geometry engine | 3.053 | 13.9 | 0.337 | 1.3 |
| ARM9 JIT execute | 4.179 | 19.0 | 2.289 | 9.2 |
| ARM7 JIT execute | 0.036 | 0.2 | 1.966 | 7.9 |
| GX present/Draw | 1.352 | 6.2 | 1.819 | 7.3 |
| GPU 3D render | 0.205 | 0.9 | 0.119 | 0.5 |
| other / glue | 1.215 | 5.5 | 1.870 | 7.5 |
| SPU | 0.104 | 0.5 | 0.169 | 0.7 |
| **TOTAL** | **21.978** | | **25.011** | |

### Assessment

**The premise of Step 5 still holds.** The compositor share fell only ~4–5 pp
(58→54, 65→63); it is still by a wide margin the single dominant cost and the
only zone large enough to close the gap to the 16.72 ms frame budget. vsd
needs another ~5.3 ms off *somewhere* to hit full speed and sm64 another
~8.3 ms — in both cases larger than every non-compositor zone combined. Full
speed is not reachable without a major compositor reduction.

**Steps 1–3 captured most of the cheap, non-GX headroom.** They removed the
per-pixel blend dispatch (§2), the per-pixel window-check call for the
no-window cases (subsumed into §2's `FUNCNUM` 0–3 vs 4–7 split), and the
redundant per-line clears (§3.1). What remains in the compositor is genuine
per-pixel work: tile/tilemap fetch, palette lookup, priority compare against
`bgPixels[x]`, the blend arithmetic, and the store — repeated for up to 4 BG
layers plus the sprite line, 256×192 times. Back-of-envelope on vsd:
11.8 ms / (49 152 px × ~2.5 effective layers) ≈ 95 ns ≈ ~70 Broadway cycles
per pixel-layer. There is *some* left in tighter inner loops / paired-single
stores / run-batching same-window spans, but it's incremental (§3-sized, low
single digits each) and it does not change the order of magnitude.

**The GX path is the only lever with the right ceiling.** GX present is at
6–7 % — the Wii GPU is nearly idle while Broadway spends 54–63 % of the frame
being a software rasterizer for a chip that is structurally a tile/sprite GPU.
Moving BG-layer compositing onto GX (Step 5.1) is the piece that can plausibly
take the compositor from ~12–16 ms to low single digits, because it removes
the per-pixel loop entirely rather than shaving cycles off it.

### Decision — proceed to Step 5, in this order

1. **5.0 — dirty tracking first.** Non-negotiable prerequisite; everything
   else in Step 5 is a stale-texture bug without it. Milestone-scale, own
   correctness pass. Start here.
2. **5.1 — BG layers as GX textures, before 5.2.** BG compositing dominates
   *both* test scenes (vsd is nearly sprite-free; sm64's cost is the castle's
   text/affine BGs, not the HUD sprites). The original plan hedged that §4
   "might" narrow the work to 5.2-only — it does not. 5.1 is the main event.
3. **5.2 / 5.3 / 5.4** after 5.1 lands and is measured — sprites-as-quads,
   windows-as-scissors, N-layer sandwich, in that order, each gated on the
   previous one's measured result.

**Secondary finding, out of scope for this doc:** on vsd the geometry engine
(CPU 3D vertex transform) is 13.9 % / 3.05 ms — the #2 cost after the
compositor and untouched by any step here. Worth its own investigation once
the compositor is on GX; noted so it isn't lost.

---

## Step 5 — Extend GXMerge from "3D sandwich" to "2D-on-GX"

**Why last:** this is the only step that requires new infrastructure rather
than refactoring existing code, and its payoff is contingent on what Step 4
finds still needs it. Do the dirty-tracking prerequisite before any drawing
code, since everything else in this step depends on it.

### Background — what GXMerge already proves
`GXMerge` (GXMerge.h/.cpp) already does the "don't composite in software,
let GX sandwich it" trick, but only for the BG0/3D split point
(`GPU_RenderLine_layer`, GPU.cpp:2196–2270): a behind-texture → 3D-texture →
front-texture 3-draw sandwich per recorded scanline band
(`GXMergeBand`, GXMerge.h:44–63). Every other BG layer, every sprite, and all
window logic still goes through the CPU per-pixel path this whole doc is
about. The DS 2D engine is structurally a tile/sprite GPU, so this
generalizes:

### 5.0 Prerequisite: VRAM/OAM/palette dirty tracking (build this first)
**There is currently no dirty-tracking anywhere in the codebase** (checked
`MMU.h`, `MMU.cpp`, `GPU.h` — nothing exists). GXMerge's 3D texture source is
cheap to keep current because `gfx3d` re-renders every frame regardless; a BG
tile/tilemap texture atlas is different — without knowing when the
underlying VRAM/palette/OAM actually changed, "cache the BG texture" becomes
"rebuild the texture every frame," which just relocates the per-pixel cost
into a texture-upload loop instead of eliminating it.

Build a write-barrier / dirty-flag layer on the BG VRAM banks, palette, and
OAM before writing any GX drawing code for this. This is comparable in scope
to the JIT infrastructure work, not to the endian-fix-sized items above —
treat it as its own milestone with its own correctness pass (stale-texture
bugs are visually obvious but easy to introduce subtly, e.g. a write path
that bypasses the barrier).

**Landed (`55d0436`) — `GXDirty.{h,cpp}`.** `u8 lcdPageDirty[64]` (one flag
per 16 KB LCDC physical page) + `palGen` / `oamGen` / `fullGen` counters, all
gated behind `g_gxDirtyArmed` (default off → each hook is a single
predicted-not-taken branch; `GXDIRTY_DISABLED` removes them). Hooks:
`_MMU_ARM9_write08/16/32` tails (interpreter + ARM9 JIT — its inline-store
fast path already excludes VRAM/pal/OAM — + DMA), `_MMU_ARM7_write08/16/32`
tails (VRAM C/D, region-gated), and `GPU_RenderLine_DispCapture` (marks the
128 KB write block — capture bypasses `_MMU_write*`). `GXDirty_FullInvalidate`
on bank remap (`MMU_VRAMmapControl`), reset, and savestate load. The consumer
(5.1's bake) clears the pages it reads → clear-on-consume *is* the epoch.
Benchmark with `armed=false`: byte-identical to the Step-4 baseline on every
renderer and every perf_zones zone — the dark path costs nothing measurable.
Also landed the shared `GPU_ResolveTextTile8x8()` tile-resolve helper (mirrors
`renderline_textBG`; 64 RGB5A3 texels per 8×8 cell), inert until 5.1 wires it.

### 5.1 BG layers as GX-sampled textures
Text/affine/extended BG layers are tileset + tilemap in VRAM — a texture
atlas + UV lookup. Build (and, via 5.0, incrementally update) an atlas per BG
layer, then draw it as a textured quad with HOFS/VOFS as a UV offset, using
GX's native wrap mode instead of the CPU's per-pixel `(x+hofs)&mask` walk
(GPU.cpp:2251). Mosaic becomes a texture-space scale/filter. Brighten/darken/
alpha-blend reuse the same TEV constant-color stages GXRender.cpp already
sets up for the 3D path (GXRender.cpp:~366) — no new blend math.

**Landed — 5.1a (`917eef7` GX2DBG bake · `ed65ccb` pipeline · `a281165`
coverage probe + validation · `34109fd` 3D-fold).** Chosen approach: not GX
indirect tile lookup (5.1b — Broadway indirect stages can't address a 10-bit
TileNum exactly) but a per-BG **resolved-plane texture** — the tilemap
resolved against tileset+palette in BG-native space (`GX_TF_RGB5A3`,
4×4-swizzled, ≤512 KB/layer, 1.5 MB budget), baked by `GX2DBG_FrameUpdate`
(core thread, l==0) only for the 8×8 cells whose 5.0 epochs changed, then
drawn every frame as a `GX_REPEAT` quad with `(HOFS,VOFS)` in the UVs — GX
does the wrap and the composite.
- **Recorder** (`GPU_RenderLine_layer` top): a scanline whose whole 2D
  composite is GX-expressible — MAIN, every enabled BG a baked `BGType_Text`
  layer, no sprites/window/mosaic/BLDCNT-effect — is gathered in CPU painter
  order into a `GX2DBGBand` and the CPU BG walk for that line is skipped.
  BG0-as-3D lines are handled too when `GXMerge_LineMergeable` accepts the 3D
  part (opaque, no front-translucent BG): a `KIND_3D` entry at BG0's slot
  draws the resident 3D texture between the GX BG quads (the "3D-fold").
- **Coalesce / replay**: `GXMerge_End2DBG` → `GX2DBGBand[64]` (overflow →
  whole-frame CPU fallback); `GXMerge_Draw2DBGBands` on `draw_thread` draws
  backdrop quad → BG/3D entries painter-order → the existing draw-4
  MASTER_BRIGHT pass. Legacy `GXMerge` 3D sandwich path untouched.
- **Measured** (`profile` vs `profile2dbg`): **pure-2D MAIN screen
  (`sm64boot`) — compositor 4.30 → 2.72 ms (−37 %), frame 23.25 → 21.71 ms,
  43.1 → 46.1 fps (+7 %)**, GX-present cost +0.05 ms. No regression on any
  path. vsd unmoved (frame `GXMerge_FrameMergeable`-disarmed upstream — the
  legacy sandwich doesn't run there either); sm64 gameplay unmoved (sprites
  on every frame → **5.2**).
- **Deferred**: affine BGs, windows (5.3), mosaic, non-mode-0 BLDCNT,
  front-translucent BG over 3D, `frontAlphaOver` — all documented per-line
  fallbacks. Incremental (dirty-only) cell re-bake landed; a
  full-vs-incremental assert harness is still TODO. Visual A/B on
  Dolphin/hardware still pending (FBDUMP can't see GX-composited output).

**5.2 landed (`56d5a97` non-affine, `0acfb1e` affine).**
`GPU_ResolveObjSprite` decodes an OAM entry (endian-corrected) and bakes a
tiled, standard-palette OBJ — non-affine *or* affine (Mode 0/1, 16/256-colour,
1D/2D map) — into a `GX2OBJ_MARGIN`-padded RGB5A3 texture (transparent border →
`GX_CLAMP` reads through outside the sprite). Non-affine flips and the affine
OAM 2×2 matrix (`dx/dmx/dy/dmy`, double-size field) both fold into the 4
field-rect corner UVs; the replay emits one per-corner-UV quad per sprite,
priority-ordered over the BG bands, both engines. Per-engine 128-slot pool
with a content-keyed cache (rebake on OAM/palette/VRAM epoch or key/size
change), latched for the draw thread. `GX2DBG_ObjFrameUpdate` early-outs on
any BLDCNT colour effect / active window so it doesn't bake sprites the
recorder can't use. Frame gate: any bitmap / OBJ-window / ext-pal-256 / mosaic
sprite → CPU sprite path for that engine.

**No regression** (`sm64boot` still −74 %). **Still does not clear `sm64`
gameplay** — and it's no longer the sprites. The recorder's frame-invariant
gates (`BLDCNT` blend mode 0, no window) fail on sm64 gameplay: its 3D layer
uses colour effects the fixed-function band replay can't express (fail reason
3/4/5 — the same wall the legacy `GXMerge` 3D sandwich hits there). Unblocking
the 63 % scene now needs **blend-mode handling** (the deferred 5.1a
"blend/brighten" item), a separate chunk.

### 5.2 Sprites as per-OBJ textured quads
Affine sprites already carry a 2×2 transform (`dx/dmx/dy/dmy`,
GPU.cpp:1636–1639) that `_spriteRender` currently applies via a per-pixel
fixed-point matrix walk (GPU.cpp:1660–1680) — this is a textured quad with a
matrix, i.e. GX's transform pipeline's actual job. One `GX_LoadPosMtx` + draw
per sprite replaces the per-pixel affine loop; unrotated sprites become the
identity-matrix special case rather than a separate code path. Depending on
what Step 4 shows, this may be the highest-value piece of Step 5 on its own —
it deletes the single most expensive inner loop in `_spriteRender` — and
could be sequenced ahead of 5.1 if BG layers turn out to be cheap enough
after Steps 1–3.

### 5.3 Windows as scissor rects (with a stencil fallback for WINOBJ)
WIN0/WIN1 are rectangular per-scanline regions already computed by
`setup_windows` (GPU.cpp:2595) — this maps directly onto
`GX_SetScissorBoxOffset` per band, reusing the same record/coalesce/replay
shape `GXMergeBand` already implements. WINOBJ (irregular, sprite-shaped
windows) doesn't fit a scissor rect and needs a stencil/alpha-mask texture
instead — keep it on the existing CPU fallback path from the start, the same
way `GXMerge_Disarm` already falls back to the legacy path for unmergeable
frames (GXMerge.h:118), rather than trying to solve it up front.

### 5.4 Generalize the sandwich to N layers
`GXMergeBand` currently encodes exactly one 3-draw sandwich
(behind/3D/front). Generalize to a per-band, priority-ordered draw list:
backdrop → BG(prio 3) → BG(prio 2) → OBJ(prio-sorted) → BG(prio 1) →
BG(prio 0) → ..., reusing the existing priority-bucketing
(`itemsForPriority[]`, GPU.cpp:2145–2166) unchanged — only what happens with
each bucket (a GX draw call vs. a CPU pixel loop) changes, so this can be
built incrementally: land 5.1/5.2/5.3 as opt-in per-bucket GX draws behind
the existing CPU path, the same opt-in/fallback structure `GXMerge_Enabled()`
already uses, rather than a flag-day cutover.

**Estimate:** the single biggest remaining lever if Step 4 shows the
compositor still dominates after Steps 1–3 — but sized and sequenced
correctly only after that checkpoint, and gated entirely on 5.0 existing
first.

**Risk:** high relative to Steps 1–3. New subsystem, new failure modes
(stale textures, scissor/stencil edge cases at window boundaries, affine
sprite wrap-around behavior noted as already fragile in `_spriteRender`'s
comments about Super Robot Wars K). Land behind the same opt-in toggle
pattern as `GXMerge_SetEnabled`/`GXMerge_Enabled`, keep the legacy CPU path
fully intact as fallback (as GXMerge already does for 3D), and expect this to
be a multi-milestone project, not a single patch.

---

## Summary table

| Step | Scope | Effort | Risk | Depends on |
|---|---|---|---|---|
| 1. ~~Scope `-fno-strict-aliasing` off `GPU.cpp`~~ | 1 file, Makefile | **Done — 0%, reverted** | Low | — |
| 2. Hand-hoist per-pixel dispatch | 3 call sites, mechanical | **Done — −8–15 % of compositor** | Low | ~~Step 1's result~~ (Step 1 confirmed needed) |
| 3. Trim per-line CPU waste (clears, OAM endian) | ~3 small sites | **Done — 3.1 −1–2 % of compositor; 3.2 deferred; 3.3 already collapsed** | Low | — |
| 4. Re-benchmark checkpoint | No code | **Done — cumulative −17 % (vsd) / −9 % (sm64) of compositor; +11 % / +6 % fps. Verdict: Step 5 justified, do 5.1 before 5.2** | — | Steps 1–3 |
| 5. GX-offload the 2D compositor | New subsystem (dirty-tracking + N-layer sandwich) | Multi-milestone | High | **5.0 / 5.1a / 5.2 done** (`55d0436`..`0acfb1e`, 15 commits). MAIN+SUB text BGs + 3D-fold + non-affine/affine sprites on GX, opt-in (`GXMerge_Set2DBG`), CPU path intact. **Validated −74 % compositor / +15 % fps on pure-2D screens** (`sm64boot`); **zero regression** on the heavy scenes. **Paused here.** The two profiled heavy scenes each need one more piece: `sm64` gameplay (63 %) is blend-mode-gated (deferred 5.1a "blend/brighten"); `vsd` (54 %) is dead composite work in dispMode 2 (guarded skip, declined). Then 5.3 (windows) / 5.4, and 5.1b if a prototype proves out. Visual A/B on Dolphin/hardware still pending. |
