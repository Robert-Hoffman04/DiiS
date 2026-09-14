# GX render test ROMs + harness

Tooling for eyeballing and diffing the GX 3D/2D pipeline (the GXMerge redesign,
`desmumewii-findings.md` §5) against the legacy readback path.

Two ROMs, built by `build.sh` into `out/` (both BlocksDS, built in
`skylyrac/blocksds:slim-latest` - **no host toolchain**; devkitARM produces a ROM
desmumewii can't boot):

| ROM | main engine | what it exercises |
|---|---|---|
| **VSD** (`vsd_*`) | `MODE_FB3` + display capture | the **legacy readback path** / fallback safety - the sandwich never arms here |
| **mergerom** (`merge_*`) | tiled BG mode, 3D on BG0, no capture | the **GX-merge 3-draw sandwich** (2D BGs + sprites bucketed around the 3D layer) |

`out/` and `mergerom/build/` are git-ignored - rerun `build.sh`.

---

## VSD - Volumetric Shadow Demo

R.H.L.'s demo (<https://codeberg.org/SkyLyrac/volumetric_shadow_demo>, pinned
commit `9ded33c`), patched by `deterministic.patch` (all behind
`-DVSD_DETERMINISTIC`, zero effect on a normal build):

| define | effect |
|---|---|
| `VSD_DETERMINISTIC` | freeze the doll, drop the live counters, force `DCAP_MODE` = source-A only -> pixel-stable frames. Prints `VSD READY (deterministic)`. Still pipe-drivable. |
| `VSD_FREEZE` | also stop calling `input()` after `VSD_WARMUP_FRAMES` (16). |

**Why a custom build:** the stock demo spins the doll and repaints live counters
every frame (never pixel-stable), and runs display-capture *motion blur*
(`REG_DISPCAPCNT` A+B blend) - that feedback loop, not a texture bug, is what
produced the "colour-scramble flicker". `DCAP_MODE(0)` kills it.

**What it tests:** the demo drives the main screen in `MODE_FB3` + display capture,
and `dispMode != 1` / capture each disqualify a frame from the sandwich
(`GXMerge_FrameMergeable`). So **every frame falls back to the legacy readback
path** - merge ON and merge OFF must render identically. It's a fallback-safety
regression test.

## mergerom - purpose-built sandwich test (`mergerom/`)

`MODE_0_2D + ENABLE_3D`, no capture / brightness / windows:

```
BG0 = 3D       prio 1   spinning vertex-coloured quad, alpha-0 clear (transparent backdrop)
BG1 = checker  prio 3   BEHIND the 3D  - shows through where the quad is transparent
BG2 = top band prio 0   FRONT of the 3D - strip over the top 32 px
OBJ0 = red     prio 0   FRONT of the 3D (front bucket)
OBJ1 = blue    prio 2   BEHIND the 3D, over the checker (behind bucket)
```

| define | effect |
|---|---|
| `MT_DETERMINISTIC` | freeze the spin after `MT_WARMUP` (8) frames, drop the per-frame counter. Prints `MT READY (deterministic)`. Still drivable over the gecko serial (`l`/`r` spin). |
| `MT_FREEZE` | also stop reading input after warmup. |
| `MT_REGDUMP` | dump DISPCNT/BLDCNT/BLDALPHA/window/capture regs to the sub screen. |
| `MT_FRONTBLEND` | rewire mixing: BG2 (front, prio 0) is an alpha-blend 1st target; BG0/BG1/OBJ/backdrop are all 2nd targets; `EVA = EVB = 8`. The DS blends the BG2 band 50/50 over whatever is beneath it (3D quad / checker / backdrop) - exercises the Phase-2 front-bucket "blend against beneath" path (`GXMerge` `frontAlphaOver`). Also moves OBJ0 behind the 3D (the path needs an empty front sprite bucket). Built as `merge_fblend.nds`. |
| `MT_MASTERBRIGHT` | drive `MASTER_BRIGHT` bright-down (fade-to-black, factor 8) over the whole main screen - exercises the Phase-3 per-band GX brightness pass (`GXMerge` draw 4). The CPU `GPU_RenderLine_MasterBrightness` pass is suppressed for merged lines; merge ON should match `ds_sw`'s faded output within RGB8-vs-RGB555 precision. Built as `merge_mbright.nds`. |
| `MT_MB_SPLIT` | with `MT_MASTERBRIGHT`: an HBlank IRQ rewrites the factor at scanline 96 (top half factor 8, bottom half factor 14), forcing a 2-band brightness split. Built as `merge_mbsplit.nds`. |

**Status:** renders correctly under `ds_sw` and `ds_gx` (the latter always
includes what used to be the separate `ds_merge` build). The GX-core
stall that used to wedge this ROM before `MT READY` is **fixed** (plan Phase 5
Symptom B - a six-bug chain in `GXRender.cpp`); `ds_mergedbg` now shows the merge
status marker **green** (reason 0) and the sandwich renders the scene
pixel-faithfully. `merge_fblend.nds` (`-DMT_FRONTBLEND`) additionally arms the
`frontAlphaOver` front-bucket blend path.

---

## Build

```sh
tools/vsd-testrom/build.sh          # needs docker
```

## Harness (`harness/`)

Drives the flatpak Dolphin build; input over the USB Gecko serial (below). Paths default to the
flatpak layout; override `DOLPHIN_DATA`, `DOLPHIN_SD`, `DOL_DIR`, `WIN_NAME` (see
`harness/common.sh`). Put the desmumewii `.dol` builds under `$DOL_DIR` (default
`$DOLPHIN_DATA/mergetest`): `ds_sw` (`-DDESMUME_FORCE_CORE=2`), `ds_gx`
(`-DDESMUME_FORCE_CORE=1`), `ds_mergedbg` (`... -DGXMERGE_DEBUG`).

GXMerge/GX2DBG compositing is mandatory whenever the GX core runs (no build
flag or runtime toggle left to disable it - see `source/main.cpp`), so
`ds_gx` **is** what `ds_merge` used to be; a separate `ds_merge` build with
`-DDESMUME_FORCE_GXCOMPOSITE` would be byte-identical to `ds_gx` and isn't
built anymore. `abtoggle.sh`, which A/B'd merge-ON vs merge-OFF by flipping
the old runtime toggle (GC D-pad Down -> `GXMerge_SetEnabled`) mid-session,
is likewise **removed** - that toggle no longer exists, so the script could
only have produced a false-positive ON/ON comparison.

```sh
harness/setrom.sh   out/vsd_det.nds        # -> sd:/DS/ROMS/test.nds
harness/dettest.sh  ds_mergedbg            # frame-to-frame stability (want AE=0)
```

`ds_mergedbg` draws a corner marker on the MAIN screen: **green** = the sandwich
drew this frame, **red** = fell back to legacy; a coloured cell to its right
encodes `g_gxmergeFailReason` (1 dispMode, 2 capture, 3 blend2 / cross-boundary,
4 bright-1st, 5 window, 6 unused, 7 unused - master-bright is now handled
per-band by GXMerge draw 4, no longer a fallback reason).

### Input over the USB Gecko debug serial

The `.dol` builds carry [`source/gekko_utils/geckoinput.cpp`](../../source/gekko_utils/geckoinput.cpp):
it reads command bytes off USB Gecko channel 1 (EXI Slot B) and turns them into
synthetic pad presses that the normal `process_ctrls_event()` / `GetInput()`
paths pick up. No `GCPadNew.ini`, no `mkfifo`, no synthetic X11 events.

`dolphin_launch` passes `-C Dolphin.Core.SlotB=7` (`EXIDeviceType::Gecko`);
Dolphin then runs a TCP server on `0.0.0.0:55020` (`EXI_DeviceGecko`
`SERVER_PORT` = `0xd6ec`).
`gecko_open` in `common.sh` holds one connection open for the session;
`gecko_send <bytes>` / `gecko_tap <byte>` write to it.

Wire protocol (one byte per event, case-sensitive; whitespace ignored):

| byte | pad button | DS |
|---|---|---|
| `a` `b` `x` `y` | A / B / X / Y | A / B / X / Y |
| `s` | START | START |
| `u` `d` `l` `r` | D-pad U/D/L/R | D-pad (`r` = SELECT) |
| `z` | Z trigger | touch-screen tap |
| `L` `R` | L / R triggers | L / R |
| `d` | D-pad Down | D-pad Down only (no longer also toggles `GXMerge_SetEnabled` - that runtime toggle was removed; GXMerge/GX2DBG is mandatory on the GX core now) |

Each byte "taps" the button for `GECKO_TAP_FRAMES` (~6) frames then auto-releases;
resend to repeat (`ddddd` = five downs).

---

## Findings

**Phase-1 fallback-safety bug (fixed, verified).** The `GXRender.cpp` merge hooks
(`GX_SetDstAlpha` coverage mask, deferred EFB clear, RGB8 pixel format) were gated
on the global `GXMerge_Enabled()` flag, so they perturbed `gfx3d_convertedScreen`
even on un-armed fallback frames. On `vsd_det.nds` this was a pure green-channel
shift - legacy `srgb(0,33,0)` vs merge `srgb(0,0,0)` on the room's "black" texels
(24 225 px, `R:0 G:24225 B:0`). Gating the hooks on `GXMerge_FrameArmed()` /
`GXMerge_HasPresentFrame()`, and checking `dispMode`/capture *before* the "no 3D
this frame" early-out in `GXMerge_FrameMergeable`, makes an un-armed frame
byte-identical to legacy. Verified at the time via `abtoggle.sh ds_merge` on
`vsd_det.nds` -> **AE = 0** on all four comparisons (historical result -
`abtoggle.sh` and the runtime toggle it drove are since removed, see above).

**Pre-existing GX-core issues (not merge-related), tracked as Phase 5 of the plan:**
- the VSD room texture renders as a hard checker under both GX paths vs a smooth
  wall under `ds_sw` (direct-colour / `GL_RGB` polygon-texture decode). Still open
  (Phase 5 Symptom A).
- ~~desmumewii's GX core stalls on the minimal `merge_*` BG0=3D ROM~~ - **fixed**
  (Phase 5 Symptom B).
