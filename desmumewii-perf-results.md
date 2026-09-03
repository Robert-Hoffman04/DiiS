# CPU-emulation optimization pass — results (2026-09-03)

Follow-up to [desmumewii-perf-opportunities.md](desmumewii-perf-opportunities.md).
Three independent changes, each measured on top of the previous with
`tools/benchmark` (all three renderers, both scenes).

## The changes

### 1. Big-endian memory access via `lwbrx` / `lhbrx` / `stwbrx` / `sthbrx`
`source/src/mem.h`, `source/src/MMU.h`.

The DS is little-endian, the Wii is big-endian, so every word/halfword access to
emulated memory is byte-reversed. The hand-written byte assembly
(`mem[a+3]<<24 | mem[a+2]<<16 | …`) compiled to 8–11 PowerPC instructions per
access; `__builtin_bswap32(*(u32*)p)` on an aligned pointer compiles to a single
`lwbrx`. This is on the hottest path in the emulator — instruction prefetch runs
one `_MMU_read32<CODE>` per emulated instruction, and every `LDR`/`STR`/`LDM`/
`STM` adds more.

Applied to the aligned-by-contract entry points
(`T1ReadWord_guaranteedAligned`, `T1ReadLong_guaranteedAligned`), the
self-masking `T1ReadLong`, and two new `T1WriteWord/Long_guaranteedAligned`
helpers wired into the ARM9/ARM7 main-RAM + DTCM write fast paths in `MMU.h`.
The un-guaranteed entry points keep the byte path — a byte-reversed load faults
on a misaligned effective address on the 750. `armcpu.o` went from 0 to 35
`lwbrx`/`lhbrx`.

### 2. `-flto=auto`
`Makefile` (compile **and** link; `$(OPTFLAGS)` added to `LDFLAGS` so the
UB-suppression flags `-fno-strict-aliasing -fwrapv
-fno-aggressive-loop-optimizations` carry into the LTO link step).

Lets the ~7,500 instruction-handler bodies, the MMU slow-path functions, and the
GPU compositor inline and specialise across translation units — impossible
before, since they are separate objects.

### 3. `-funroll-loops`
`Makefile`.

**This replaces the opportunities doc's `-mcpu=750cl` item — that flag does not
exist in devkitPPC's GCC 16** (valid `-mcpu` values stop at `740 / 750 / 7400`;
Broadway already builds as `750`, which is correct). `-funroll-loops` is the
nearest single codegen knob. It grows the `.dol` by ~118 KB (2.12 MB → 2.24 MB).
Dolphin does not model i-cache, but the Broadway's 32 KB L1-I does — treat this
one's gain as optimistic and worth a real-hardware A/B.

## Benchmark

Dolphin, emulated-Broadway timebase, % of real DS (59.8261 Hz). Baseline = run
`20260903T061903Z` (git `b63d904-dirty`; no functional code change since).

| scene | renderer   | baseline | +endian | +LTO   | +unroll | **total** |
|-------|------------|---------:|--------:|-------:|--------:|----------:|
| vsd   | software   |   23.35% |  23.60% | 24.77% |  25.21% | **+8.0%** |
| vsd   | GX hw 3D   |   36.97% |  37.62% | 40.99% |  42.00% | **+13.6%** |
| vsd   | GXMerge    |   36.82% |  37.47% | 40.79% |  41.90% | **+13.7%** |
| ph    | software   |   16.05% |  16.75% | 17.18% |  17.32% | **+7.9%** |
| ph    | GX hw 3D   |   25.28% |  27.13% | 28.32% |  28.63% | **+13.3%** |
| ph    | GXMerge    |   25.20% |  27.12% | 28.20% |  28.63% | **+13.6%** |

Marginal effect of each change (relative eff-fps):

| change          | vsd sw | vsd gx | vsd merge | ph sw | ph gx | ph merge |
|-----------------|-------:|-------:|----------:|------:|------:|---------:|
| endian access   |  +1.1% |  +1.8% |    +1.8%  | +4.4% | +7.3% |   +7.3%  |
| `-flto`         |  +5.0% |  +9.0% |    +8.9%  | +2.5% | +4.3% |   +4.2%  |
| `-funroll-loops`|  +1.8% |  +2.5% |    +2.7%  | +0.8% | +1.1% |   +1.2%  |

`vsd` cv ≈ 0.1 %, so every `vsd` delta is real. `ph` cv ≈ 2 %, so `ph`'s
`-funroll-loops` row is within run-to-run noise; the endian and LTO rows are
well clear of it. All three changes are net-positive in every cell.

**Slowdown vs real hardware (GX path): vsd 2.71× → 2.38×, ph 3.96× → 3.49×.**
GXMerge tracks the plain GX path to within 0.3 % the whole way — the merge
pipeline benefits equally.

`exec%` stays 96–98 % in every configuration. The frame is still almost entirely
CPU emulation; these changes make that work cheaper without changing its share.
The dynarec / decoded-instruction-cache items in the opportunities doc are still
where the remaining multiple is.

## Correctness

A/B screenshot check under Dolphin, patched build vs a clean baseline build,
both forced to the GX core:

- **VSD test ROM** (deterministic): identical render. The only differing pixels
  are the demo's self-animating character model and sub-pixel triangle-edge
  noise from non-frame-locked capture; the entire static test-pattern
  background, room geometry, colours and all UI text are pixel-matching.
- **Phantom Hourglass** (retail): intro cutscene renders correctly — seagull
  model textures, animated caustics, volumetric clouds, sky gradient, bottom-
  screen reflection all intact. The two captures land at different points in
  the cutscene (the faster build is further along at the same wall-clock), so a
  whole-frame `compare` is large, but every pixel outside the DS screens
  matches and both frames are visually correct.

No blank screen, no scrambled colour (the failure mode a broken byte-swap would
cause), no missing geometry (the failure mode the `-flto` UB hazard would
cause). ~600 k emulated frames ran across the benchmark matrix with no crash and
unchanged frame-pacing cv.

## Caveats

Dolphin's PPC JIT is not cycle-accurate and runs GX nearly free; real Broadway
is slower and the GX/merge share of the gap is larger there. Trust the
change-to-change deltas, not the absolute percentages. `-funroll-loops`
specifically may under-deliver or regress on real silicon because of its i-cache
cost — it is committed separately so it can be reverted on its own.
