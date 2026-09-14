# Renderer benchmark

Measures how fast each of the three compositing paths runs — as a fraction of
real DS hardware (**59.8261 Hz**) — and diffs the result against the previous
run so a change's perf cost is visible.

| dol | renderer | build defines |
|---|---|---|
| `bench_sw`      | all-CPU software rasterizer            | `DESMUME_FORCE_CORE=2` |
| `bench_gx`      | GX hardware 3D + GXMerge/GX2DBG        | `DESMUME_FORCE_CORE=1` |
| `bench_jitoff`  | ARM7 interpreter (JIT A/B baseline)    | `DESMUME_FORCE_CORE=2` |
| `bench_jiton`   | ARM7 JIT (JIT A/B)                     | `DESMUME_FORCE_CORE=2` + `DESMUME_JIT_ARM7` |
| `bench_jit9off` | ARM9 interpreter (JIT A/B baseline)    | `DESMUME_FORCE_CORE=2` |
| `bench_jit9on`  | ARM9 JIT (JIT A/B)                     | `DESMUME_FORCE_CORE=2` + `DESMUME_JIT_ARM7` + `DESMUME_JIT_ARM9_ON` |
| `bench_jitfull` | full JIT (ARM7 + ARM9) over GXMerge    | `DESMUME_FORCE_CORE=1` + `DESMUME_JIT_ARM7` + `DESMUME_JIT_ARM9_ON` |
| `bench_profile` | `jitfull` + per-zone frame-time breakdown | ...as `jitfull` + `DESMUME_PERFZONES` |

**GXMerge/GX2DBG compositing is mandatory whenever the GX core runs**
(`source/src/main.cpp`, tied 1:1 to `DESMUME_FORCE_CORE=1` /
`current3Dcore==1`) — there is no build flag or runtime switch left to get
"stock GX hardware 3D + CPU 2D compositor" as a separate configuration, so
`bench_gx` already includes it. The old `bench_merge`/`bench_profile2dbg`
modes, which used to build that config separately via
`DESMUME_FORCE_GXCOMPOSITE`/`DESMUME_FORCE_GX2DBG`, are **removed** (not kept
as aliases) - those flags no longer gate anything in the source, so building
a second dol under a different mode name would just be a duplicate. Any old
result directory or script still referencing `merge`/`profile2dbg` predates
this and should be treated as `gx`/`profile`.

The `jit*off`/`jit*on` pairs deliberately keep the software rasterizer fixed so
the only delta between the two builds is the CPU core under test. `jitfull`
isn't an A/B pair — it's both cores JIT'd, running the real GXMerge renderer,
so it reads as the "how fast does this actually go" number rather than an
isolated core comparison.

## How it works

`source/src/main.cpp` has a `-DDESMUME_BENCH` hook (behind `#ifdef`, no effect on
a normal build). It times every frame with the Wii timebase and, every 60
frames, appends a row to `sd:/bench.log`:

```
frame,wall_us,block_us,exec_us,draw_us
```

Nothing throttles — `Execute()` runs `DSExec()` back to back — so the logged rate
is the honest "as fast as this build goes" rate for whatever is on screen.

`benchmark.sh`:

1. builds the three dols into `dols/` (only `main.o` is rebuilt between them),
2. for every scene in [`scenes.conf`](scenes.conf) × every renderer: stages the
   ROM into the Dolphin SD image as `DS/ROMS/test.nds`, boots the dol in
   headless Dolphin (`-b`), waits, kills it, and pulls `sd:/bench.log` into
   `results/<stamp>_<sha>/raw/<scene>_<mode>.log`,
3. runs `analyze.py`, which writes `results.json` + `report.md` and prints a
   summary plus a diff against the most recent previous run under `results/`,
4. if any `profile`-mode capture is present, runs `perfzones.py` for the
   frame-time breakdown (below).

## `profile` mode — where the full-JIT frame actually goes

```sh
tools/benchmark/benchmark.sh --modes profile --scenes sm64
```

`profile` is `jitfull` plus `-DDESMUME_PERFZONES`, which arms the `perf_zones`
accountant (`source/src/perf_zones.{h,cpp}`). It keeps one "current zone" and
banks Wii-timebase intervals to it, switching zone at ~12 instrumented call
sites:

| zone | site |
|---|---|
| `arm9_interp` / `arm7_interp` | `armcpu_exec<…>()` interpreter fallback (`NDSSystem.cpp`) |
| `arm9_jit` / `arm7_jit` | `jitRunArm9/7()` — dispatch + trampoline + `ExecuteJITTrace` (`jit_exec.cpp`) |
| `arm9_build` / `arm7_build` | `jitCompileTrace()` — trace scan + codegen (`jit_trace.cpp`) |
| `gpu_ge` | `gfx3d_execute3D()` — geometry-engine FIFO (`gfx3d.cpp`) |
| `gpu_render` | `gpu3D->NDS_3D_Render()` — GXRender / soft raster |
| `gpu_2d` | `GPU_RenderLine()` ×2 per scanline — 2D compositor / merge line walk |
| `spu` | `SPU_Emulate_core()` |
| `draw` | `Draw()` — screen convert + `GXMerge_Present` + VI present (`main.cpp`) |
| `other` | everything else — sequencer, DMA, MMU, IRQ dispatch, glue |

`pzSet()` only reads the timebase when the zone *changes*, so a run of a
million same-zone dispatches is one branch each. Cost is ~4 % of `eff_fps`
vs a plain `jitfull` build — treat the shares as the signal, not the absolute
ms (same caveat as `DESMUME_ARM_TIME_SPLIT`).

The build dumps `sd:/perfzones.log` every 60 frames:

```
frame,wall_us,<zone>_us…,<zone>_hits…
```

`perfzones.py` windows it (same `scenes.conf` window as `analyze.py`), rolls the
zones into CPU-ARM9 / CPU-ARM7 / GPU / SPU / GX-present / other, and writes
`report_perfzones.md` + `results.perfzones.json`. Re-analyse or diff without
re-running:

```sh
tools/benchmark/perfzones.py results/<stamp>
tools/benchmark/perfzones.py results/<new> --compare results/<old>
tools/benchmark/perfzones.py results/<stamp>/raw/sm64_profile.perfzones.log --window 300-1200
```

To add a zone: add the enum in `perf_zones.h`, a name in `perf_zones.cpp`'s
`k_name[]`, a `PZ_SCOPE(PZ_x)` at the call site, and (optionally) a row in
`perfzones.py`'s `GROUPS`.

The original SD `test.nds` is backed up and restored afterwards. `results/` and
`dols/` are gitignored; to keep a run as a comparison baseline in git,
`git add -f tools/benchmark/results/<stamp>/results.json`.

## Prerequisites

- devkitPPC toolchain — `DEVKITPPC` / `DEVKITPRO` in the environment
- flatpak Dolphin — `org.DolphinEmu.dolphin-emu`
- `mtools` (`mcopy`, `mdel`)
- a Dolphin Wii SD image (default `~/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/desmume-sd.raw`)
  with `DS/ROMS/` and `DS/BIOS/{biosnds7.rom,biosnds9.rom,firmware.bin}` — the
  same image the `vsd-testrom` harness uses. Override with `DOLPHIN_SD=…`.

## Usage

```sh
tools/benchmark/benchmark.sh                 # full matrix, ~13 min for 2 scenes
tools/benchmark/benchmark.sh --scenes vsd    # just one scene
tools/benchmark/benchmark.sh --modes "sw gx" --no-build
tools/benchmark/benchmark.sh --modes jitfull  # full JIT, GXMerge renderer
tools/benchmark/benchmark.sh --duration 120  # longer runs (slow host / long windows)
```

Re-analyse an existing capture, or diff two finished runs, without re-running:

```sh
tools/benchmark/analyze.py results/20260903T101500Z_ad89c9e
tools/benchmark/analyze.py --compare results/<old> results/<new>
tools/benchmark/analyze.py results/<new> --baseline results/<specific-old>
```

## Reading the output

- **% real-time** — `eff_fps / 59.8261`. Below 100 % the game is in slow motion.
- **slowdown** — `59.8261 / eff_fps`.
- **exec% / draw%** — share of frame time in `NDS_exec()` (CPU: ARM cores + 2D
  compositor + software raster) vs `Draw()` (GX texture upload + present).
- **cv%** — coefficient of variation of the per-block fps inside the window. A
  few % is normal; a large value means the window straddles a scene change —
  tighten it in `scenes.conf`.
- the diff flags a **regression** when a renderer's `eff_fps` drops more than 3 %
  relative to the baseline (and **improved** likewise).

## Caveats

Measured under Dolphin. The metric is on the emulated Broadway's own timebase,
so it is independent of how fast the Linux host ran — but Dolphin's PPC JIT is
not cycle-accurate (no cache/memory-latency modelling, optimistic instruction
timing) and runs GX on the host GPU almost for free. Real Wii silicon is
**slower**, and the gap is largest for the GX and merge paths. Treat the
absolute percentages as an optimistic ceiling; the renderer-to-renderer and
run-to-run deltas are the trustworthy part.