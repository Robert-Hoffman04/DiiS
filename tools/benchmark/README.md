# Renderer benchmark

Measures how fast each of the three compositing paths runs — as a fraction of
real DS hardware (**59.8261 Hz**) — and diffs the result against the previous
run so a change's perf cost is visible.

| dol | renderer | build defines |
|---|---|---|
| `bench_sw`    | all-CPU software rasterizer | `DESMUME_FORCE_CORE=2` |
| `bench_gx`    | stock GX hardware 3D        | `DESMUME_FORCE_CORE=1` |
| `bench_merge` | GXMerge 3-draw sandwich     | `DESMUME_FORCE_CORE=1` + `DESMUME_FORCE_GXCOMPOSITE` |

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
   summary plus a diff against the most recent previous run under `results/`.

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
