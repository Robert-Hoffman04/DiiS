# JIT cache tuning notes

Scratch notes from the ARM9 JIT cache-pressure work (branch `arm9-jit-infra`).
Measurement ROM/scene is always the SM64DS autoload savestate
(`tools/benchmark/states/sm64.ds0`), full-JIT both cores + GX hw 3D + GXMerge +
GX2DBG, run to frame ~1200.

## Arena sizing

| ARM9 arena | p99 frame | worst | ARM9 build ms/f | flush interval | arena peak | collmiss |
|-----------:|----------:|------:|----------------:|---------------:|-----------:|---------:|
| 3 MB       | -         | -     | -               | ~90/3000 fr    | 99%        | -        |
| 6 MB       | ~41-48 ms | ~49 ms| 0.53            | ~1/130 fr      | 99%        | ~46.7k   |
| 12 MB (now)| ~28-34 ms | ~44 ms| ~0.38           | ~1/175 fr      | 99%        | ~38k     |

12 MB is committed (`d527dc9`). It fixes the p99 tail and the build cost but
NOT the saturation: arena still peaks at 99% and still flushes. Collision-miss
rate is unchanged by arena size (it is a block-table property, not an arena
property) - see the Step 3 note below.

## Which memory pool does the JIT cache live in?  (Step 2)

**MEM2.** Measured directly with `-DJIT_MEM_ACCOUNT` (SYS_GetArena1Size /
SYS_GetArena2Size logged to `sd:/jitmem.log` around `jitInit()` and again at
bench frames 300 / 1200):

```
want: arm7 arena=2048 KiB  arm9 arena=12288 KiB  per-core blockTable=1024 KiB  smcTables=320 KiB
before  SYS_arena1=6684 KiB  SYS_arena2=32444 KiB
after   SYS_arena1=6684 KiB  SYS_arena2=14900 KiB
f300    SYS_arena1=6684 KiB  SYS_arena2=11928 KiB
f1200   SYS_arena1=6684 KiB  SYS_arena2=11928 KiB
```

- `jitInit()` consumed **~17.1 MiB of MEM2** (32444 -> 14900 KiB): arm9 arena
  12 MiB + arm7 arena 2 MiB + 2x blockTable (2 MiB) + 2x smcTables (~0.63 MiB)
  + alignment/heap overhead. The MEM1 arena tail (`SYS_arena1`) never moved.
- libogc's default malloc heap is MEM2-backed on this build - `memalign()` in
  `jitInitSlot()` pulls from MEM2, not the scarce ~24 MiB MEM1. There is no
  project code that configures this (the only manual MEM2 carve in the tree is
  `gekko_utils/usb2storage.c`, for USB2 DMA buffers); it is stock libogc Wii
  behaviour.
- Steady-state (post game warmup) **MEM2 headroom is ~11.6 MiB** and stable -
  genuine idle space, nothing reclaims or grows it after frame ~300.
- `mallinfo()` fields on this libogc are unreliable (reported `arena` ~265 MiB,
  physically impossible); trust the `SYS_GetArenaN` deltas only.

Note: the `main.cpp:296` comment blaming an early freeze on "MEM1 exhaustion"
from "the JIT's own arena/table allocations plus this 8 MB" looks mis-attributed
in the pool - the JIT allocations are in MEM2. The freeze was still real memory
exhaustion (shrinking `expMemSize` fixed it); the 8 MB `ExpMemory` `new` and/or
total footprint was the trigger, but the pool blamed was probably wrong.

### Headroom for Step 4

A 64K -> 256K `HASH_TABLE_SIZE` grows `blockTable` from 1 MiB to 4 MiB **per
core** (the macro is shared by `jitCacheArm7` and `jitCacheArm9`), i.e. +6 MiB
MEM2, dropping steady-state headroom from ~11.6 MiB to ~5.6 MiB. Feasible but
tight. If only ARM9 needs it, splitting the constant per-core (+3 MiB, ~8.6 MiB
left) is the safer shape.

## Hash-collision distribution  (Step 3)

`-DJIT_HASH_HISTO` adds a per-bucket real-eviction tally to `profEmitReport`
(`jit hashhisto cache=...` line: touched buckets, max single bucket, dispersion
index x100 (==100 for a uniform/Poisson spread), and 1 / 2-3 / 4-7 / 8-15 / 16+
eviction-count bands).

### Old hash `((pc>>1) ^ (pc>>13)) & (HASH_TABLE_SIZE-1)` -- SM64DS, ARM9

```
evbuckets=129/65536  max=2707  dispx100=140939  (D ~ 1409, uniform = 1)
b1=49  b2_3=10  b4_7=9  b8_15=6  b16+=55
ev_in_b16+ = 86174 / 86367      (99.8% of all evictions in 55 buckets)
```

One bucket alone climbed by ~13 evictions per report for the whole run
(25 -> 2707). **This is case (a): a structural hash weakness, not a load
factor problem.** Only 129 of 65,536 buckets ever evict; the hot ARM9 working
set is small but the shift-xor mix folds those blocks onto ~55 buckets where
they permanently ping-pong recompile - which is also what keeps the 12 MB
arena pinned at 99% and drives the ARM9 JIT-build cost. Growing
`HASH_TABLE_SIZE` would not help (the colliding PCs would still collide).

ARM7 is unaffected (6 buckets, max 2) but ARM7 barely uses its cache (9
evictions total), so that says little.

### Fix tried: multiplicative (Fibonacci) hash

`jitHashPC(pc) = (pc * 0x9E3779B1u) >> (32 - log2(HASH_TABLE_SIZE))`, mirrored
in the two hand-emitted PPC stubs (`lis`/`ori`/`mullw` + one `rlwinm`, same
instruction count as the old `srwi`/`srwi`/`xor` + `rlwinm`). All C++ sites go
through the shared `jitHashPC()` inline in `jit_cache.h`.

Result (SM64DS, `-DJIT_HASH_HISTO` build, matched to ~11M ARM9 lookups):

| metric (ARM9)                | old shift-xor | multiplicative |
|------------------------------|--------------:|---------------:|
| collision-misses             | ~86,700       | **~18,700**  (-78%) |
| evictions (wasted recompiles)| ~86,400       | **~18,700**  (-78%) |
| touched buckets              | 129           | 48             |
| buckets in the 16+ band      | 55            | 11             |
| full-cache flushes @ ~fr1600 | 12            | **6**          |
| steady-state ARM9 JIT-build  | ~0.45-0.50 ms/f | **~0.10 ms/f** (-78%) |
| instr wall, window f780-1380 | 25.15 ms/f    | 24.81 ms/f     |
| eff fps, same window         | 39.8          | 40.3           |

Before the first in-game scene transition the ARM9 cache is essentially
collision-free with the new hash (9 collisions / 1.3M lookups, arena 14%),
where the old hash was already thrashing one bucket from boot.

**Residual:** one bucket still climbs to ~2700 evictions, but only *after* a
scene transition, and it survives the hash change (max 2707 -> 2713). That is
almost certainly a single guest PC executed in both ARM and THUMB mode: same
PC -> same bucket under any PC-only hash, and each mode flip re-registers over
the other mode's block. Folding the ISA bit into the index would fix it; not
worth the extra emitted-code complexity for one bucket. Left as a note.

### Step 4 (grow HASH_TABLE_SIZE) -- not pursued

Step 3 showed the collision misses were a hash-quality problem, not a
load-factor problem (129/65536 buckets touched). A 4x table would not have
moved the colliding PCs apart. Skipped; the multiplicative hash is the fix.
`JIT_MEM_ACCOUNT` still stands as the headroom reference if a future
associativity change wants it.

## Step 5 -- summary and recommendation

Clean apples-to-apples, 12 MB ARM9 arena both sides, plain harness build (no
histo overhead), SM64DS window f780-1200:

| metric              | 6 MB, old hash | 12 MB, old hash | 12 MB, mult. hash |
|---------------------|---------------:|----------------:|------------------:|
| p99 frame time      | ~41-48 ms      | ~28-34 ms       | ~28-34 ms         |
| worst frame         | ~49 ms         | ~44 ms          | ~34-40 ms         |
| ARM9 JIT-build ms/f  | 0.53           | 0.475           | **0.094** (-80%)  |
| ARM9 JIT-exec ms/f   | ~4.3           | ~4.26           | ~4.25 (unchanged) |
| eff fps (window)    | ~40.6          | 45.8            | 46.5              |
| ARM9 collision-miss | ~46.7k         | ~86k / 11M lk   | **~17.8k** (-79%) |
| ARM9 evictions      | -              | ~86k            | **~17.8k**        |
| full-cache flushes  | ~1/130 fr      | 12 @ fr1600     | **6-7 @ fr1600**  |
| arena peak fill     | 99%            | 99%             | 89-99%            |

- **Step 1 (12 MB arena, `d527dc9`):** confirmed - kills the 6 MB p99 tail
  (~44 -> ~30 ms) and cuts flush frequency; +6 MiB MEM2, which Step 2 shows is
  affordable. Median frame time unchanged (not flush-bound).
- **Step 2 (`1e0957a`):** the JIT backing store is in **MEM2**, not MEM1
  (~17 MiB, ~11.6 MiB MEM2 left, MEM1 arena untouched). Removes the memory
  objection to further growth.
- **Step 3 (`6bcd977`):** the collision misses were a bad hash, not load
  factor. Multiplicative hash: -78-80% collision-misses / evictions / ARM9
  JIT-build cost, flushes roughly halved, ~+1.5% fps in the steady window,
  zero code-size cost, cannot miscompile.
- **Step 4:** not needed / not done.

### What to try next (in rough priority order)

1. **The residual ARM/THUMB same-PC bucket.** One bucket still takes ~2700
   evictions/run after a scene transition. If a cheap `-DJIT_HASH_HISTO +
   JIT_LOG_CACHE_EVENT` capture confirms it is one PC flipping ISA, fold the
   mode bit into the index (`pc ^ (thumb << 1)` before the multiply, mirrored
   in the 2 stubs). Small, contained, removes the last thrash source.
2. **Arena still peaks at 99%.** With the eviction churn now gone, the 99% is
   mostly genuine distinct-block volume. A 16 MB arena would likely stop the
   remaining 6-7 flushes/run outright; ~8.6 MiB MEM2 would remain. Low-risk,
   worth a single A/B.
3. **N-way associativity** is now clearly *not* worth it - Step 3 removed the
   collision pressure that would have justified the extra branch on the
   getBlock() hot path.
4. Bigger picture: ARM9 JIT-*execute* (~4.25 ms/f) and the GPU 2D compositor
   (~5 ms/f) now dominate the frame far more than anything cache-related. The
   next real frame-time lever is one of those, not the JIT cache.
