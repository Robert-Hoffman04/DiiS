# desmumewii benchmark

Runs the fps/renderer matrix and the correctness-probe test ROMs in one pass
against headless Dolphin, over the unified test harness's network transport,
and reports the result plus a diff against the previous run.

## Prerequisites

- devkitPPC toolchain - `DEVKITPPC` / `DEVKITPRO` in the environment (default
  `/opt/devkitpro`)
- devkitARM toolchain at `/opt/devkitpro/devkitARM` (only needed for the
  correctness probes - `run.sh --no-wrestlers` skips this requirement)
- flatpak Dolphin - `org.DolphinEmu.dolphin-emu`
- `mtools` (`mcopy`/`mdel`/`mdir`)
- `python3`, optionally with `Pillow` installed (screenshots save as `.png`
  with it, raw `.bin` without it)
- a Dolphin Wii SD image (default
  `~/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/desmume-sd.raw`)
  with `DS/ROMS/` and `DS/BIOS/{biosnds7.rom,biosnds9.rom,firmware.bin}`.
  Override with `DOLPHIN_SD=...`.
- **Dolphin's Wii network passthrough enabled** (Config → Wii → network
  settings - exact label varies by Dolphin version). This is a one-time
  setting check, not something these scripts configure. Every capture here
  connects to `tools/benchmark`'s listener over TCP; without passthrough the
  device never connects and every run times out with "device never
  connected".

Env overrides recognised by every script: `DOLPHIN_DATA`, `DOLPHIN_SD`,
`DEVKITPRO`, `DEVKITPPC`, `HARNESS_HOST` (default `127.0.0.1`), `HARNESS_PORT`
(default `4300`).

## Quick start

```sh
tools/benchmark/run.sh                       # full matrix + all correctness probes
tools/benchmark/run.sh --scenes vsd           # just one fps scene, all default modes
tools/benchmark/run.sh --modes "sw gx"        # the default mode set, spelled out
tools/benchmark/run.sh --modes jit9on         # isolate the ARM9 JIT A/B delta only
tools/benchmark/run.sh --no-wrestlers         # fps matrix only, skip correctness probes
tools/benchmark/run.sh --no-perf --wrestlers armwrestler   # one correctness probe only
```

## `run.sh` options

```
--modes "sw gx"      renderer/JIT modes to sweep (default "sw gx", both
                      running full JIT - see "Modes" below for why jitfull
                      isn't also in the default set).
                      Also available: jitoff jiton jit9off jit9on jitfull
--scenes "vsd sm64"  subset of scene ids from scenes.conf (default: all)
--wrestlers "id id"  subset of ids from wrestlers.conf (default: all)
--no-build           reuse tools/benchmark/dols/*.dol instead of rebuilding
--clean              `make clean` before the first build
--timeout N          per-run safety-net seconds (default 180); a run stops
                      on its own once it reaches its target frame, this is
                      only a ceiling for a hang/crash. scenes.conf's
                      'timeout' column overrides it per scene.
--no-perf            skip the fps matrix, run correctness probes only
--no-wrestlers       skip the correctness probes, run the fps matrix only
--no-compare         skip the diff against the previous run
```

### Modes

| mode | renderer | cores |
|---|---|---|
| `sw` | software rasterizer | both JITs on |
| `gx` | GX hardware 3D + GXMerge/GX2DBG | both JITs on |
| `jitoff` | software rasterizer | ARM7 interpreter (JIT A/B baseline) |
| `jiton` | software rasterizer | ARM7 JIT only |
| `jit9off` | software rasterizer | ARM9 interpreter (JIT A/B baseline) |
| `jit9on` | software rasterizer | both JITs on |
| `jitfull` | GX hardware 3D + GXMerge | both JITs on |

`sw`/`gx` are the emulator's actual fastest CPU config, not an
interpreter-only isolation baseline. `jitoff`/`jiton` and `jit9off`/`jit9on`
are the dedicated A/B pairs for isolating one core's JIT delta with
everything else held fixed (interpreter baseline included on purpose).
`jitfull` is build-for-build identical to `gx` (GXMerge/GX2DBG is mandatory
on the GX core, so "GX + both JITs" has no further dial to turn) - it isn't
in the default set to avoid printing the same row twice, but stays
available under its own name for a one-off `--modes jitfull` run.

### Scenes and correctness probes

`scenes.conf` lists the fps scenes (ROM, frame window to average over,
label, timeout, optional savestate) - see the comments at the top of that
file for the column format. `wrestlers.conf` lists the correctness probes
(armwrestler, arm7wrestler, RockWrestler) the same way. Add a scene or probe
by adding a line; both files are read fresh on every run.

## Output

Every run writes `results/<timestamp>_<git-sha>/`:

```
meta.json              host/toolchain/git metadata for this run
raw/perf_<scene>_<mode>/    one capture per scene x mode: harness.log,
                             profile.log, summary.json, dolphin.log
raw/wrestler_<id>/          one capture per correctness probe: harness.log,
                             profile.log, summary.json, frame.png, dolphin.log
results.json            everything analyze.py parsed out of raw/, machine-readable
report.md                human-readable report, includes a diff table vs the
                          previous run if one was found
```

## Re-analysing without re-running

```sh
tools/benchmark/analyze.py results/<stamp>_<sha>              # re-parse + re-diff
tools/benchmark/analyze.py --compare results/<old> results/<new>   # diff two finished runs
tools/benchmark/analyze.py results/<new> --baseline results/<specific-old>
```

## Reading the output

- **% real-time** - `eff_fps / 59.8261` (real DS refresh rate). Below 100% is
  slow motion.
- **slowdown** - `59.8261 / eff_fps`.
- **p95/worst frame** - 95th-percentile / worst single-frame time inside the
  window, in ms. A few slow outlier frames can hide behind a fine average
  fps; this catches them.
- **cv%** - coefficient of variation of the per-block fps inside the window.
  A few percent is normal; a large value means the window straddles a scene
  change - tighten it in `scenes.conf`.
- **zone breakdown** (in `report.md`, per mode) - the top CPU/GPU zones by
  share of frame time (ARM9/ARM7 interpreter vs JIT vs JIT-compile, GPU
  geometry vs render vs 2D compositor, SPU, present, ...).
- The diff flags a **regression** when a renderer's `eff_fps` drops more
  than 3% relative to the previous run's (and **improved** likewise).
- **JIT cache** (for any JIT-enabled mode) - hit rate, arena peak fill, and
  eviction/flush counts per core, sampled continuously through the run.
  `report.md` has the full table (plus average eviction lifetime); the
  terminal summary shows a compact one-liner under each mode, and a `!!`
  warning line if the device itself flagged bucket contention or
  arena-capacity thrash. A run-over-run drop of 5 percentage points or more
  in a core's hit rate is flagged in the diff, the same way an fps
  regression is.
- **Correctness probes** report pass/fail asserts if the ROM emits any, and
  always report a screenshot - embedded directly in `report.md`. Same ROM +
  same direct boot + no input is deterministic, so the screenshot is
  compared byte-for-byte against the previous run's: **unchanged** or
  **CHANGED**, no threshold - any pixel difference is a real signal, not
  noise.

## `ab.sh` - one-off A/B probes

For "does this specific change move the needle" questions that don't belong
in the standing scene matrix - comparing two `TESTDEFS`/`JITDEFS`
configurations (a debug flag on vs off, or the working tree against its own
`git stash`) on one ROM/window, and looking at the matching `profile.log`
lines from both.

```sh
tools/benchmark/ab.sh --help
```

Worked examples:

```sh
# A debug-flag A/B on a clean tree (both legs = current source, only the
# flag differs) - e.g. did switching the ARM9 block-table hash function
# change the collision histogram it reports:
tools/benchmark/ab.sh --label-a mulhi --label-b mulhwu \
  --jitdefs-a "-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DJIT_HASH_HISTO" \
  --jitdefs-b "-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DJIT_HASH_HISTO -DJIT_HASH_MULHWU" \
  --grep "hashhisto"

# A code change A/B: stash the working tree's uncommitted change out for the
# "before" leg, restore it for "after" - e.g. does a JIT codegen fix move
# the per-core dispatch cost histogram:
tools/benchmark/ab.sh --stash --label-a head --label-b working-tree \
  --jitdefs-a "-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DJIT_CORE_COST_HISTO" \
  --jitdefs-b "-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DJIT_CORE_COST_HISTO" \
  --grep "jitcorecost"
```

Both legs' captures land in `results/_ab_<stamp>/<label>/` (same
`harness.log`/`profile.log`/`summary.json` shape as `run.sh`'s captures).

## `soak.sh` - JIT differential-testing soak

A correctness gate, not a benchmark: runs the interpreter and both JITs side
by side against a ROM, comparing guest state after every instruction, for a
fixed duration, and reports any mismatch/canary/overrun lines (there should
be none).

```sh
tools/benchmark/soak.sh                        # SM64DS, 210s
tools/benchmark/soak.sh --duration 400 --rom /path/to/other.nds
```

## Caveats

Measured under Dolphin. The metric is on the emulated Broadway's own
timebase, so it is independent of how fast the Linux host ran - but
Dolphin's PPC JIT is not cycle-accurate (no cache/memory-latency modelling,
optimistic instruction timing) and runs GX on the host GPU almost for free.
Real Wii silicon is **slower**, and the gap is largest for the GX and merge
paths. Treat the absolute percentages as an optimistic ceiling; the
renderer-to-renderer and run-to-run deltas are the trustworthy part.

`results/` and `dols/` are gitignored; to keep a run as a comparison
baseline in git, `git add -f tools/benchmark/results/<stamp>/results.json`.
