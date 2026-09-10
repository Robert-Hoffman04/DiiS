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

(pending - `-DJIT_HASH_HISTO` run)
