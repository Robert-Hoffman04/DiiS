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

**Residual:** one bucket still climbs to ~2700-3400 evictions after a scene
transition, and it survives the hash change. Originally guessed to be an
ARM/THUMB ISA flip -- the "Step 5 follow-up" section below disproves that
(`samepc_evict = 14/22070`): it is distinct-PC collision clustering. ~14
buckets carry 99.5% of evictions.

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

## Step 5 follow-up -- ISA-flip probe and 16 MB A/B (both negative)

Two of the "what to try next" ideas below were tested directly and **both
failed to reproduce the predicted win.** The section that followed has been
rewritten accordingly.

### (1) The residual worst bucket is NOT an ARM/THUMB ISA flip

`-DJIT_HASH_HISTO` was extended with `samepc_evict` -- a tally of evictions
where the outgoing slot held the *same* guest PC as the incoming block (an
ISA flip re-registering over itself) -- plus the worst bucket's current
occupant PC / mode. SM64DS, ARM9, ~12.9M lookups:

```
jit hashhisto cache=arm9 evbuckets=56/65536 max=3416 dispx100=231506
  b1=21 b2_3=12 b4_7=6 b8_15=3 b16+=14  ev_in_b16+=21963/22070
  samepc_evict=14/22070   worstbucket_pc=0x02043dc4 worstbucket_thumb=0
```

**`samepc_evict = 14 / 22070` (0.06%).** The residual thrash is not one PC
flipping ISA -- it is genuine *distinct-PC* hash collisions: 14 buckets carry
99.5% of all evictions, the worst one (a plain ARM block at `0x02043dc4`)
takes 3400+ evictions in a run by colliding with one or more other hot PCs.
Folding the mode bit into the index would do nothing here. Idea dropped.

The multiplicative hash still leaves this structural clustering: `dispx100`
is ~2300x a uniform spread, only 56/65536 buckets ever evict, and a handful
of hot-loop PC pairs map to the same bucket and permanently recompile each
other. It is a real improvement over the shift-xor (which clustered onto
~55 buckets from boot) but it did not flatten the tail.

### (2) 16 MB ARM9 arena -- no measurable improvement over 12 MB

Clean A/B, `-DJIT_ARENA_SIZE_ARM9=16 MiB` vs the committed 12 MiB, same
SM64DS soak:

| metric (window f780-1200) | 12 MB, mult hash | 16 MB, mult hash |
|---------------------------|-----------------:|-----------------:|
| instr wall                | 21.52 ms/f       | 21.52 ms/f       |
| ARM9 JIT-build             | 94 us/f          | 98 us/f          |
| ARM9 JIT-exec              | ~4.25 ms/f       | 4.23 ms/f        |
| eff fps                   | 46.5             | 46.5             |
| full-cache flushes @ fr3360| 8                | 8                |
| arena peak fill            | 99%              | 99%              |
| collision-misses / M lk    | ~1715            | ~1752            |

The arena still pins at 99% and still flushes 8x at 16 MB, and the
`arena-capacity thrash` heuristic still fires. The reason: the ~25k live-block
evictions are themselves the arena-fill pressure (every collision eviction is
a wasted recompile that re-consumes arena), so growing the arena cannot drain
it while the collisions persist. NOTES' earlier guess that 16 MB "would
likely stop the remaining flushes" is wrong -- the flush driver is the hash,
not the arena size. **Do not grow the arena further.** 12 MB stays.

### What to try next (revised, in priority order)

1. **A better block-table hash for the ~14 hot collision buckets.** This is
   now the only cache lever with headroom. The multiplicative hash keeps the
   top 16 bits of `pc * GOLDEN` (low 32); for SM64DS's tightly-clustered ARM9
   code addresses the low input bits reach those top bits only weakly through
   carry. Candidates, cheapest first: `mulhwu` (top 32 of the full 64-bit
   product) instead of `mullw` -- one-instruction swap in both PPC stubs,
   different and usually stronger bit slice; or drop the always-zero low bits
   before the multiply; or a post-multiply xorshift finalizer (costs 2 extra
   PPC insns per stub, still fits the 2 scratch registers r11/r12). Each needs
   the same `-DJIT_HASH_HISTO` A/B: success = `b16+` bucket count and total
   evictions fall, `dispx100` moves toward 100.
2. **2-way set associativity on `getBlock()`** -- *reconsidered, now the
   fallback if the hash experiments stall.* Step 3 had removed the pressure
   that justified it, but (1) above shows ~14 buckets still hard-collide. A
   2-way bucket (check slot, then slot^1) is one extra load + compare + branch
   on the hot path and a doubled 2 MiB block table per core (MEM2 headroom
   ~11.6 MiB, fine per Step 2); the emitted stubs would need the second probe
   too. Bigger change than a hash swap -- do it only if no hash beats the
   collision rate.
3. **HASH_TABLE_SIZE 4x (Step 4)** -- still low-value on its own (56/65536
   buckets touched; more buckets do not separate two PCs the hash maps
   together) but *would* compound with a better hash. Not worth doing alone.
4. Bigger picture: ARM9 JIT-*execute* (~4.25 ms/f) and the GPU 2D compositor
   (~5 ms/f) now dominate the frame far more than anything cache-related. Even
   eliminating every remaining collision would save well under 0.1 ms/f. The
   next real frame-time lever is one of those, not the JIT cache.


## ARM7 vs ARM9 JIT execute-cost investigation

The per-zone frame breakdown shows `arm7_jit` (dispatch + execute) costing as
much wall time as `arm9_jit` -- ~5.7 ms/f each in the f1200-2400 window --
even though `armInnerLoop()` already credits ARM7 progress as `jitCycles << 1`,
i.e. gives ARM7 half the guest work per unit of the shared timeline. So ARM7
burns the same host time for half the guest instructions. This section is
diagnostic only (behind `-DJIT_CORE_COST_HISTO`, off by default); no fix is
proposed until the data says which mechanism dominates.

### Step 1 -- per-core dispatch accounting

`-DJIT_CORE_COST_HISTO` splits `jitRunArm7()`/`jitRunArm9()` bookkeeping per
core and emits a cumulative line every perfzones block (60 frames) via
`jitCoreCostEmit()`, so a window is the delta between two lines. SM64DS soak,
12 MB arena, GX hw 3D + GX2DBG, autoload savestate, to frame ~4000.

| per-frame, window f1200-2400        |    ARM7 |    ARM9 |
|-------------------------------------|--------:|--------:|
| dispatch calls / frame              |    6050 |    3972 |
| calls that ran >= 1 guest insn      |    3137 |    3319 |
| **wasted calls (zero progress)**    | **2913 (48.2%)** | **653 (16.4%)** |
| -- no compilable block / "don't JIT" |    2289 |     653 |
| -- ran a block, retired 0 insns      |     624 |       0 |
| guest insns / productive call        |    11.5 |    26.3 |
| guest cycles / productive call       |    25.9 |    76.6 |
| `arm{7,9}_jit` zone                   | 5.69 ms/f | 5.32 ms/f |
| host-ns / dispatch call              |     940 |    1340 |
| host-ns / productive call            |    1814 |    1604 |
| **host-ns / retired guest insn**    | **157** |  **61** |

Findings:

1. **ARM7 dispatches 1.5x as often as ARM9** (6050 vs 3972/f) despite its
   halved cycle budget -- the interpreter hands control to `jitRunArm7()`
   far more frequently, in shorter slices.
2. **~48% of ARM7 dispatch calls make zero forward progress**, vs ~16% for
   ARM9. The bulk (2289/f) is `getBlock()`/compile returning nothing usable
   -- either a cached "don't JIT" marker being re-hit, or a fresh scan that
   finds nothing compilable. Another 624/f are compiled blocks that bail on
   their own first instruction (`r.instructions == 0`).
3. **ARM9 has essentially zero first-instruction bailouts** (0/f). Its wasted
   calls are all "no block", none are "ran but retired nothing".
4. The length-1 idle-loop demotion (`b->insnCount() == 1` in the bail path)
   almost never fires -- 3 new demotions in the whole 4000-frame run. The
   624/f ARM7 bail0 calls are therefore multi-instruction blocks bailing on
   instruction 1 and never getting demoted (this is exactly Step 4's
   hypothesis).
5. **Per *productive* call the two cores cost about the same** (~1.8 vs
   ~1.6 us). Solving the two zone-time equations with a ~300 ns wasted-call
   cost puts the productive-call cost at ~1.54 us for *both* cores. So the
   gap is NOT intrinsically slower ARM7 dispatch -- it is (a) 1.5x the call
   frequency, (b) half of those calls buying nothing, and (c) each productive
   call retiring 2.3x fewer guest instructions, so the fixed per-block
   entry/exit (trampoline, `ExecuteJITTrace` prologue, pipeline re-prime)
   amortizes 2.6x worse -> 157 vs 61 host-ns per retired guest instruction.

Steps 2-4 quantify (b) block length, (c) demotion coverage, and whether any
residual per-instruction cost is left once those are accounted for.

### Step 2 -- compiled- and executed-block length per core

`-DJIT_CORE_COST_HISTO` extended: `registerBlock()` tallies compiled-block
insnCount() per cache (`jitblocklen` line), and the dispatch path buckets the
entry block's insnCount() over every productive call (`elen*` on the
`jitcorecost` line -- the length of the block actually *run*, which is what
matters for amortization). Same SM64DS soak.

**Executed entry-block length, window f1200-2400:**

| bucket (guest insns) | ARM7 /frame |  %  | ARM9 /frame |  %  |
|----------------------|------------:|----:|------------:|----:|
| 1                    |         730 | 23% |          78 |  2% |
| 2-4                  |        1331 | 42% |         911 | 27% |
| 5-8                  |         378 | 12% |         464 | 14% |
| 9-16                 |         434 | 14% |         746 | 22% |
| 17+                  |         263 |  8% |        1120 | 34% |
| **weighted avg**     |    **~5.9** |     |   **~12.0** |     |

**Compiled-block churn, window f1200-2400:** ARM7 registers **21** compiled
blocks + 1 "don't JIT" marker; ARM9 registers **11 833** compiled blocks + 76
markers. (Full run: ARM7 955 / ARM9 44 252.)

Findings:

1. **ARM7 executed blocks are half the length of ARM9's** (~5.9 vs ~12.0
   guest insns) and the distribution is sharply bimodal toward the bottom:
   **65% of ARM7 dispatched blocks are <= 4 instructions**, and 23% are a
   single instruction. ARM9's mass is at the top -- 34% are 17+.
2. A length-1 block that runs and returns retires one guest instruction for a
   full trampoline round-trip + pipeline re-prime -- the worst possible
   amortization, and ARM7 does 730 of them per frame vs ARM9's 78.
3. **ARM7 barely compiles anything in steady state** -- 21 blocks over 1200
   frames vs ARM9's ~12 k. ARM7's hot working set is small and stable; the
   cost is not compile churn (that is an ARM9 problem, tracked in the
   hash-collision section). ARM7's problem is that its stable set is mostly
   tiny blocks plus uncompilable spots the interpreter picks up one
   instruction at a time.
4. This is the "(c) short-block amortization" mechanism from Step 1's
   summary, now quantified: the fixed ~1.5 us productive-call cost is spread
   over 2x fewer retired instructions on ARM7, which is most of the 2.6x
   host-ns-per-retired-instruction gap. Step 3 checks whether anything is
   left once block length and wasted calls are subtracted.
