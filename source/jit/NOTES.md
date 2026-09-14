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

   **Tried: `-DJIT_HASH_MULHWU` -- rejected, much worse.** Swapped `mullw`
   for `mulhwu` in both PPC hash stubs and `jitHashPC()`'s C mirror (one
   instruction, same downstream `rlwinm` extract). `-DJIT_HASH_HISTO` A/B,
   SM64DS, 30s smoke soak:

   | metric (ARM9)        | mullw (committed) | mulhwu |
   |-----------------------|-------------------:|-------:|
   | touched buckets       | 38/65536           | **1/65536** |
   | worst bucket          | ~815 (of ~5100 total evictions, 16%) | **saturated at the u16 cap (65535); is 100% of all ~155k evictions** |
   | dispx100               | ~650 (6.5x uniform) | 0 (single point, no dispersion to measure) |

   Total hash collapse, worse than even the original shift-xor (which spread
   across ~55-129 buckets). Root cause: `mulhwu`'s result is
   `floor(pc * GOLDEN / 2^32)`, which for GOLDEN ~ 0.618 * 2^32 is
   approximately `0.618 * pc` with no modular wraparound. SM64DS's ARM9 working
   set lives in a ~4 MB window of a 4 GB address space, so that linear scaling
   barely moves the *high* word at all across the whole working set -- nearly
   every hot PC lands in the same bucket after `>> 16`. `mullw`'s low-word
   product wraps modulo 2^32 many times over that same 4 MB span, which is
   exactly what makes it sensitive to every input bit. **`mulhwu` is only a
   good multiplicative-hash choice when keys span close to the full 32-bit
   range; it is the wrong tool for a narrow, clustered key space like a
   single DS title's code addresses.** Left in the tree as an off-by-default,
   documented-negative `-DJIT_HASH_HISTO` experiment (jit_cache.h /
   jit_cache.cpp), not enabled anywhere. Do not revisit without a
   fundamentally different key-space assumption.
2. **2-way set associativity on `getBlock()`** -- *reconsidered, now the
   fallback if the hash experiments stall.* Step 3 had removed the pressure
   that justified it, but (1) above shows ~14 buckets still hard-collide. A
   2-way bucket (check slot, then slot^1) is one extra load + compare + branch
   on the hot path and a doubled 2 MiB block table per core (MEM2 headroom
   ~11.6 MiB, fine per Step 2); the emitted stubs would need the second probe
   too. Bigger change than a hash swap -- do it only if no hash beats the
   collision rate.

   **Done.** `mulhwu` (above) failed, so this became the primary lever.
   `BLOCK_TABLE_WAYS=2` in jit_cache.h: each of the 65536 *sets* jitHashPC()
   names now holds 2 physical BasicBlock slots ("ways") instead of 1 --
   `blockTable[set*2 + way]`. `jitHashPC()` itself is unchanged (still a
   plain set index); only the physical array size (2x, 1 MiB -> 2 MiB/core)
   and every C-/PPC-side "set index -> slot address" site changed:
   `getBlock()` / `registerBlock()` (jit_cache.h/.cpp), the two hand-emitted
   PPC hash stubs (the static linker stub in `flushCache()` and
   `emitDynamicLinkerStub()`, both in jit_cache.cpp), `profCacheEvict()`
   (now takes the resolved physical slot as a parameter instead of
   recomputing it, since which way gets evicted is a runtime decision --
   see below), and the `blockTable`/`installFrame` allocation sites
   (jit_trace.cpp). `bucketEvict` (`-DJIT_HASH_HISTO`) stays indexed per
   *set*, matching its existing "touched buckets out of 65536" semantics.

   Insert/evict policy (`registerBlock()`, pure C, no asm changes needed
   here): prefer a way already holding this exact PC (overwrite in place --
   covers an ISA-mode flip at a shared address without evicting anything),
   then an untouched ("cold", `length==0`) way, else evict one of the two
   live occupants via a free-running round-robin counter (cheap, not true
   LRU, but keeps 3+-way contention from permanently starving one slot the
   way "always evict way0" would).

   Lookup (`getBlock()`, the two PPC stubs): probe way0's 3 guards (PC,
   mode, execute!=null); a PC or mode miss falls through to a way1 probe
   (same 3 guards) before giving up to the interpreter; an execute==nullptr
   "don't JIT" marker hit is definitive (PC+mode both matched) and does not
   check way1. The static linker stub's way1 probe reuses the *same*
   hit/patch/jump code as way0 via a backward branch (only ~5 extra
   instructions, since the self-patching logic runs once regardless of
   which way matched); the dynamic stub fully duplicates its 3-guard chain
   for way1 (~9 extra instructions per dynamic-exit site, since it has no
   shared patch tail to branch back into) -- both costs were anticipated
   and accepted above.

   Correctness: no `-DJIT_DIFFERENTIAL_TESTING` mismatches over ~450s of
   combined soak (mixed cold-boot and autoloaded-state SM64DS runs) vs a
   matching baseline run that showed the identical (pre-existing,
   unrelated) sd:/jit.log FAT-capture truncation artifact -- see
   `tools/benchmark/assoc-ab.sh`.

   Perf (`-DJIT_HASH_HISTO`, SM64DS, 150s, autoloaded gameplay state, same
   window class as Step 5):

   | metric (ARM9)        | 1-way (mult. hash) | 2-way |
   |-----------------------|--------------------:|------:|
   | touched buckets        | 82/65536            | **8/65536** |
   | worst bucket            | 10231 evictions      | **1 eviction** |
   | total evictions (run)  | ~43,775              | **8**  (-99.98%) |
   | ev_in_b16+ band          | 43,553 (99.5%)       | 0      |
   | samePCEvict / evictions | 28/43,775            | **8/8** (100%) |

   Essentially total elimination of ARM9 block-table collision churn: the
   ~14-30 hot hard-collision buckets Step 5's follow-up identified as
   structural (distinct-PC clustering the multiplicative hash could not
   separate) are resolved by giving each set 2 physical slots instead of
   fixing the hash further -- confirms the follow-up's own prediction. The
   8 residual evictions are *all* same-PC (ISA-mode flip) re-registrations,
   i.e. genuine third-visit collisions at an already-full 2-way set, not
   hash weakness. `mullw` stays the hash (no reason to revisit `mulhwu`
   now). Committed.
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

### Step 3 -- dispatch overhead vs emitted-code execution cost

`-DJIT_CORE_COST_HISTO` extended once more: a `gettime()` bracket around the
`ExecuteJITTrace()` call accumulates per-core "time strictly inside the
trampoline + emitted block" (`jitcorecost2` line, `exec_us`). `zone -
exec` is then the true dispatch/lookup/compile/pipeline-reprime overhead;
`exec / retired-insns` is the true per-instruction execution cost. (The two
extra timebase reads per dispatch inflate the zone by ~1%; `exec_us` itself
is clean.) Same SM64DS soak, windows to frame ~2400.

| per-frame, window f1200-2400          |     ARM7 |     ARM9 | ratio |
|---------------------------------------|---------:|---------:|------:|
| `arm{7,9}_jit` zone                    | 5.87 ms  | 5.56 ms  | 1.06  |
| -- inside ExecuteJITTrace (exec-only)  | 2.83 ms  | 3.51 ms  | 0.81  |
| -- dispatch/lookup/compile overhead   | 3.04 ms  | 2.05 ms  | 1.48  |
| **dispatch overhead / dispatch call** | **502 ns** | **515 ns** | **1.0** |
| dispatch overhead / *productive* call |   968 ns |   617 ns | 1.57  |
| exec-only / retired guest insn        |    78 ns |    40 ns | 1.94  |

Findings:

1. **The dispatch loop costs the same per call on both cores** (502 vs
   515 ns) -- canary poll, cache-report throttle, `canEnter*()`, `getBlock()`,
   trampoline setup and pipeline re-prime are not slower for ARM7. ARM7's
   aggregate dispatch overhead is higher only because it makes 1.5x the calls
   and ~48% of them are wasted: ARM7 pays for ~2.4 wasted calls per
   productive call, ARM9 for ~0.2.
2. **The per-retired-instruction execution cost is ~1.9x on ARM7 (78 vs
   40 ns), but it is fully consistent with fixed-per-block-overhead
   amortization, not an ARM7-specific per-instruction penalty.** Fitting
   `exec_per_productive_call = F + insns * p` to both cores (ARM7:
   F + 11.5 p = 902 ns; ARM9: F + 26.25 p = 1058 ns) with a shared `p`
   gives **p ~= 10.6 ns/insn (equal both cores)** and **F ~= 780 ns fixed
   per block** -- the trampoline prologue/epilogue (GPR+CPSR save/restore,
   `jit_cpu_state` setup, chain-guard checks, exit accounting) sitting inside
   the timed region. ARM7's blocks are half as long, so that 780 ns
   amortizes over 11.5 insns instead of 26.25. The guest-cycle rate confirms
   ARM7 instructions are not intrinsically heavier: 2.25 guest cyc/insn on
   ARM7 vs 2.92 on ARM9.
3. So there is **no third mechanism**. The gap is entirely Steps 1-2: (a)
   1.5x dispatch frequency, (b) ~48% wasted calls, (c) 2x shorter blocks
   amortizing the fixed ~780 ns trampoline worse. No evidence of the
   "ARM7 MMIO routes through heavier checked memory paths" hypothesis at the
   per-instruction level -- if ARM7's `p` were materially higher the fit
   would not close with a positive `F`.

### Step 4 -- idle-loop demotion coverage

`-DJIT_CORE_COST_HISTO` extended: the noBlock return is split into
"landed on a pre-existing matching-mode length-1 marker" vs "fresh compile /
collision / mode-flip", and two small open-addressing PC sets count distinct
PCs demoted via the exec-side length-1 path, and distinct PCs that bail0 with
`insnCount() != 1` (the shape the single-terminator check misses).

| window f1200-2400                       |    ARM7 |    ARM9 |
|----------------------------------------|--------:|--------:|
| noBlock -- pre-existing marker re-hit  | 2289/f  |   653/f |
| noBlock -- fresh compile / collision   |     0/f |     0/f |
| distinct PCs demoted (exec-side len-1) |       0 |       0 |
| bail0 with `insnCount() != 1` -- PCs   |    ~18 (full run) | 0 |
| bail0 with `insnCount() != 1` -- hits  |   624/f |     0/f |
| "don't JIT" markers registered (full run) | 157 |    ~360 |

Findings:

1. **Every ARM7 noBlock event is a clean re-hit on a marker that already
   exists** -- zero fresh compiles, zero hash-collision churn on ARM7. The
   fallback mechanism works exactly as designed; ARM7 simply hits ~2289
   uncompilable guest PCs per frame and the interpreter takes each one an
   instruction at a time.
2. **The exec-side length-1 demotion (`r.instructions == 0 &&
   b->insnCount() == 1`) essentially never fires** (0-4 distinct PCs in the
   whole run, both cores). ARM7's 157 markers are all registered by the
   *compile* side -- `jitCompileTrace()` scanning a PC, emitting nothing
   (predicated LDR/STR, MSR/MRS, coprocessor, ...) and caching a length-1
   fallback. The exec-side demote is dead code in practice.
3. **~18 distinct ARM7 PCs are compiled multi-instruction blocks that bail
   on their first instruction on every single entry** (~600 hits/frame) and
   are never demoted because the check demands `insnCount() == 1`. Confirmed
   Step 4 hypothesis. Cross-referenced with Step 2 these sit in the 2-4 insn
   bucket. Each costs a getBlock + trampoline entry + immediate bail +
   return, ~600 times a frame -> order 0.2-0.4 ms/f of pure waste.
   A demote-on-repeated-zero-progress rule (regardless of block length) would
   convert those into cheap marker re-hits. Small but real; flagged for the
   Step 5 recommendation, not fixed here.

### Step 5 -- summary and what to try next

Per-core numbers side by side (SM64DS, 12 MB arena, GX hw 3D + GX2DBG,
autoload savestate; the two established windows):

| metric                                  | ARM7 f780-1200 | ARM9 f780-1200 | ARM7 f1200-2400 | ARM9 f1200-2400 |
|-----------------------------------------|---------------:|---------------:|----------------:|----------------:|
| `arm{7,9}_jit` zone                      |   5.0-5.2 ms/f |   4.3-4.7 ms/f |    5.7-5.9 ms/f |    5.4-5.6 ms/f |
| dispatch calls / frame                   |           5497 |           3531 |            6050 |            3972 |
| wasted-call ratio                        |        **48%** |            17% |         **48%** |            16% |
| -- marker re-hits / frame                |           2031 |            597 |            2289 |             653 |
| -- bail0 / frame                         |            610 |              0 |             624 |               0 |
| executed block avg length (guest insns)  |        **5.9** |           11.8 |         **5.9** |            12.0 |
| -- length-1 blocks / frame               |            632 |             79 |             730 |              78 |
| dispatch overhead / call                 |         ~485 ns |        ~505 ns |          ~510 ns |         ~525 ns |
| exec-only / retired guest insn           |        ~78 ns  |        ~38 ns  |         ~79 ns  |         ~40 ns  |
| compiled blocks registered (window)      |            4   |          2982  |           21    |         11 833  |
| distinct exec-side demotions (whole run) |            0   |            4   |             --  |             --  |
| bail0-never-demoted PCs (whole run)      |          ~18   |            0   |             --  |             --  |

**Which hypothesis the data supports:** call/dispatch **frequency** plus
**short-trace amortization** -- and those two only. Concretely:

- The dispatch loop costs the *same* per call on both cores (~500 ns). ARM7 is
  not paying a heavier dispatch path; it runs the same path 1.5x as often and
  ~48% of those runs make zero progress (2289/f re-hits on uncompilable-PC
  markers + 624/f blocks that bail on instruction 1).
- When ARM7 does run a block it retires 5.9 guest insns vs ARM9's 12.0, so the
  fixed ~780 ns per-block trampoline cost (measured, shared across cores)
  amortizes ~2x worse -> the 78-vs-40 ns per-retired-instruction gap. A
  two-core fit gives an equal ~10.6 ns/insn execute rate, so there is **no
  ARM7-specific per-instruction (MMIO / memory-check) penalty** -- hypothesis 3
  is disproved.
- The idle-loop-demotion gap (hypothesis 4) is real but small: ~18 PCs,
  ~0.2-0.4 ms/f. Not the main story.

Caveat: the ~500 ns/call dispatch-overhead floor includes the `PZ_SCOPE`
zone-transition cost (two timebase reads per call) which only exists in a
`-DDESMUME_PERFZONES` build. The ARM7-vs-ARM9 comparison is unaffected (both
pay it) and the frame breakdown that motivated this investigation was itself a
perfzones build, but a release build's absolute dispatch overhead is lower.

**Most promising next experiment, for a human to prioritize** (against the
ARM9 hash-collision thrash and the 2D compositor, which are still the bigger
frame-time levers -- ARM7 JIT is ~5.9 ms of a ~27 ms frame and roughly half
of that is genuine work):

1. **Longer ARM7 block chains.** ARM7 retires 11.5 guest insns per productive
   dispatch vs ARM9's 26. If chaining kept ARM7 in JIT for ~25 insns/dispatch
   the ARM7 zone would roughly halve (fewer trampoline round-trips, the
   dominant cost per Step 3). Diagnostic: instrument *why* the ARM7 chain
   breaks -- `r.bailedOut` / dynamic-exit / IRQ-check / quota per dispatch,
   split by core. ARM7's IO-poll-heavy code almost certainly trips the
   dynamic-exit and IRQ paths far more often; some of those exits may be
   chainable (static targets the linker stub could patch) that currently
   aren't.
2. **Shrink the uncompilable-PC set.** 2289 marker re-hits/frame are 2289
   guest instructions/frame the interpreter runs one at a time. They cluster
   on a few hundred distinct PCs (Step 2: ARM7 registers only ~950 blocks +
   157 markers all run). A `-DDESMUME_JIT_TRACE_FIRST` `dontJIT top:` capture
   would name the offending opcodes; widening the emitter for the top few
   (predicated LDR/STR is the usual ARM7 offender) is the highest-leverage
   coverage work, though a bigger project than (1).
3. **Cheap:** demote any block that makes zero forward progress on N
   consecutive entries, not just `insnCount() == 1` blocks (Step 4). Converts
   ~600 failed dispatches/frame into marker re-hits. ~0.2-0.4 ms/f.

## Step 6 -- why do ARM7 (and ARM9) chains break? (TODO item 5)

Item (1) above asked for `r.bailedOut` / dynamic-exit / IRQ-check / quota
telemetry per dispatch, split by core. Added a `DESMUME_JIT_TRACE_FIRST`
classification block to `jitRunArm7()` mirroring the one that already existed
in `jitRunArm9()` (jit_exec.cpp): of every dispatch that reaches
`ExecuteJITTrace` and returns with `r.instructions != 0 && !r.bailedOut &&
!r.smcHit` (a genuine "edge" -- dynamic exit, quota trip, or block-table end,
as opposed to a mid-chain guard bail), classify the block-table slot sitting
at the resume PC (`resident` / `other` / `empty` / `dontJIT` / `smc`) plus
whether the exit was a quota trip (`r.cycles >= JIT_YIELD_NUMBER`) or a short
chain (`r.instructions < 4`).

Ran a 180s in-game SM64DS soak (autoload savestate, `-DDESMUME_JIT_TRACE_FIRST
-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_FORCE_GX2DBG`) and captured
`sd:/jit.log`. Also had to add a missing `#include <stdio.h>` to
`jit_trace.cpp` -- `DESMUME_JIT_TRACE_FIRST` alone (without
`JIT_CORE_COST_HISTO`, which pulls in an ogc header that happens to drag
`<cstdio>` in transitively) failed to build; pre-existing gap, fixed alongside.

### The "edge" (clean-exit) breakdown

Last window of the soak, per-core:

| | ARM7 | ARM9 |
|---|---:|---:|
| edge exits, as % of ExecuteJITTrace calls | **64%** | **17%** |
| -- of those, next-PC slot is `dontJIT` | **99.98%** | **99.8%** |
| -- of those, next-PC slot is `resident`/`other` (cache pressure) | ~0% | ~0.01% |
| -- of those, `empty` (first visit / flush) | ~0.02% | ~0.2% |
| quota trips, as % of edges | ~5% | ~3.3% |
| zero-progress bail0, as % of ExecuteJITTrace calls | ~3% | ~0.0004% |

Two things fall out immediately:

- **Hash-collision / cache-pressure slot eviction is now a non-factor for
  chain breaks on both cores** (`other` is 0-687 hits against edge counts in
  the millions) -- confirms item 4's 2-way associativity fix from the *chain*
  angle as well as the eviction-count angle it was originally validated with.
- **When an ARM7 or ARM9 chain *does* end cleanly, the very next instruction
  is almost always already known to be uncompilable** (a resident `dontJIT`
  marker). Static chaining across that boundary is not possible without first
  shrinking the uncompilable-PC set (item 6) -- there's no "guard too strict /
  self-patch not sticking" bug to fix here, the target genuinely can't be
  compiled today.

### The bigger finding: most round-trips are NOT clean edges

The "edge" percentages above (64% ARM7, 17% ARM9) are of *all* dispatches,
and bail0 is negligible on both cores (3% / ~0%) -- so the remainder,
**~33% of ARM7 dispatches and ~83% of ARM9 dispatches, return with
`r.bailedOut == 1` and `r.instructions != 0`**: a mid-chain deferred-bailout
guard tripped (`JitTraceCtx::registerBailout` / the per-instruction predicate
check jit_arm.cpp/jit_thumb.cpp emit for every predicated ARM/THUMB
instruction the front end *does* compile), not a block-table-end or dynamic
exit at all.

This is a different, previously uninstrumented root cause from anything in
Step 5's list, and for ARM9 it is the dominant one by a wide margin: most
ARM9 chain terminations are a predicated instruction (Bcc/data-processing/
LDR-STR-with-S-bit-style condition check) resolving the *opposite* way from
whatever the compiled fast path assumed, not the chain reaching a real
boundary. Combined with item 6's `dontJIT` set (predicated LDR/STR that can't
be compiled *at all*), the picture is consistent: **conditional execution is
the ARM7/ARM9 JIT's central coverage gap**, at two severities -- fully
uncompilable (dontJIT, item 6) and compiled-but-guarded (deferred bailout,
this section).

**What would actually lengthen chains**, in priority order:
1. Item 6 (shrink the `dontJIT` set) still stands and now matters for ARM9
   too, not just ARM7 -- the `dontJIT tot=... top:` opcode capture in
   `jit_trace.cpp` (fires on a *fresh* zero-length compile) didn't trigger in
   this run because the markers were already cache-resident from earlier in
   the soak; a longer soak from a *cold* JIT cache (flush first, or capture
   from process start) is needed to get the top-offender opcode list.
2. **New, larger project (not scoped for this pass):** real conditional-
   execution support -- compile both predicate outcomes (or the common
   AL/never-taken fast path plus a compiled, not interpreted, fallback for
   the guard) instead of always bailing to the interpreter on a predicate
   miss. This is what would move the ~33%/~83% guard-bail share into chained
   execution. Substantially bigger and riskier than items 1-4 (touches the
   condition-check emitter for every predicated opcode); needs its own scope
   discussion before starting.
3. Quota trips are a minor factor on both cores (~3-5% of edges) --
   raising `JIT_YIELD_NUMBER` would help only that slice and risks longer
   uninterruptible stretches (IRQ latency); not worth pursuing on its own.

See `tools/benchmark` scratch capture pattern (mcopy the autoload-savestate
ROM+state, run, `mcopy` back `sd:/jit.log`, `grep -a "a7 edge:"` / `"a9
edge:"`) -- not checked in as a script since it's a one-shot diagnostic
capture, not an A/B.

## Step 7 -- dontJIT top-opcode capture and one fix (TODO item 6)

The `dontJIT tot=... uniq=... top:` opcode-frequency dump in `jit_trace.cpp`
(fires on a fresh zero-length compile, i.e. the *first* time a PC is found
uncompilable) never triggered during Step 6's 180s soak -- the modulo
threshold (`s_tot & 0xFFF`) needs 4096 such fresh discoveries and the
autoload-savestate soak's cache was already warm by then. Lowered the mask to
`0xFF` for this capture (kept -- it's diagnostic-only, gated behind
`DESMUME_JIT_TRACE_FIRST`, and a smaller threshold is strictly more useful for
a short soak) and re-ran 90s. Two dumps fired (tot=256, tot=512); top ARM-mode
opcodes by raw 32-bit encoding:

```
908ff100=18 e1a0e00f=16 e121f003=14 012fff1e=14 e121f001=13 e121f002=11
e121f000=8 e25ef004=8 08bd8000=8 908ff101=8 e08ff102=7 e08fc00c=6
```

Decoded, these collapse into a handful of instruction *shapes* (condition
codes and register operands fragment what would otherwise be a few offenders
into many distinct literal opcode words -- masking the condition field before
dedup would give a cleaner top-N next time):

| shape | example | why it bails today |
|---|---|---|
| `BXcc lr` (conditional return) | `012fff1e` = BXEQ lr | see fix below |
| `MSR CPSR_c, Rn` (mode switch) | `e121f00{0-3}` | control-field MSR is categorically unsupported (jit_arm.cpp:1392, only the flags-only field mask compiles) -- touches CPU mode/register banking, correctly out of scope for a quick fix |
| `ADD{cc} pc, pc, Rm, LSL#2` (jump table) | `908ff100/101`, `e08ff102` | register-form data-proc-to-pc requires `rn != 15` (jit_arm.cpp:450); only the *immediate*-operand2 form of `ADD/SUB pc,#k` is special-cased for `rn==15` (line 462-475). The function's own header comment already claims to cover "`ADD pc,pc,rN` jump tables" -- it doesn't, for the register-source case. Real fix: special-case `rn==15` in the register path the same way, materializing `currentPC+8` as the ALU operand instead of reading a live PC register (jump tables are a **known dynamic target** either way, so this only removes the round-trip, not a differential-testing risk from a wrong static target). Not attempted this pass -- flagged for the next one. |
| `SUBS pc, lr, #4` (IRQ return) | `e25ef004` | S-form (SPSR->CPSR exception return) is deliberately unsupported -- correctness-sensitive, correctly out of scope |
| `LDMcc ...,{pc}` (conditional epilogue) | `08bd8000` | `predicated && pcInList` bails by design (jit_arm.cpp:993) -- same *shape* as `BXcc lr` (conditional interworking return) but via LDM; a same-style guarded-exit widening is plausible future work, not attempted this pass |
| predicated `STM`/other DP | `b8a10001` | general predicated multi-register store; not investigated further this pass |

### Fix applied: `BXcc lr` on ARM7 too

`012fff1e` (BXEQ lr) was independently the single hottest raw opcode in the
capture. `jit_arm.cpp`'s BX/BLX dispatch already had a guarded taken-exit +
cond-false-fall-through compilation for exactly this shape (`isBlx==false &&
Rm==14`, i.e. `BXcc lr`) -- added for ARM9 during Step 5 per the file's own
comment ("the hottest refused opcode") -- but gated behind `v5` (ARMv5TE,
i.e. ARM9-only). Nothing in the guarded-dispatch mechanism itself
(`emitEvalCond`, the CPSR.T bit-0 interworking check, the dynamic exit) is a
v5 feature: predicated execution is a base ARMv4T property, so ARM7's `BXcc
lr` qualifies exactly the same way. Dropped the `!v5` term from the bail
condition (BLX still correctly requires v5 -- that gate is separate and
untouched).

Validated: differential-testing soak (180s, in-game SM64DS via autoload
savestate) -- no DIFF/MISMATCH/CANARY/OVERRUN lines. The `sd:/jit.log`
capture hit the same pre-existing 192-byte FAT-truncation artifact noted in
Step 6 (confirmed non-regression the same way: an identical-size, only-tail-
differs artifact reproduces with the change stashed out).

### Next up for item 6

The jump-table `ADD{cc} pc,pc,Rm,LSL#2` widening (register-form rn==15 in
`emitDataProcToPc`) is the next concrete, scoped, low-risk candidate --
purely a dynamic-exit target computation, no CPU-mode/exception semantics
involved. `MSR CPSR_c` / `SUBS pc,lr,#4` / predicated LDM{...,pc} touch real
mode-switch and exception-return semantics and need their own scope
discussion before attempting (same category as item 12's predication
project) -- do not start unprompted.

## Step 8 -- jump-table widening: `ADD/SUB pc,pc,Rm` register form

Implemented the item announced above. `emitDataProcToPc()`'s header comment
already claimed to cover `ADD pc,pc,rN` jump tables, but the code only
special-cased `rn==15` for the *immediate*-operand2 form (`ADD/SUB pc,#k` --
a compile-time-constant target, static exit). The register-operand form
(`ADD pc,pc,Rm,LSL#2`, the actual jump-table dispatch idiom -- `Rm` is the
runtime table index) unconditionally bailed at `rn == 15` alongside every
other data-processing opcode's PC-as-source-operand restriction.

Fix: allow `rn==15` through for `aluOp == SUB(2) || ADD(4)` in the register
path (the only two ops a real jump table uses `pc` as the base for), and
materialize `currentPC+8` into `PPC_R9` (confirmed unused anywhere else in
`jit_arm.cpp`, so safe to hardcode as a scratch reg here) instead of trying
to `readReg(15, ...)` -- there's no live host-backed slot for the guest PC
register. Materializing happens *before* `emitOp2()` runs (its own shift
paths use `R8`/`R11`/`R12` as scratch), so nothing clobbers it. `Rm` is still
a runtime register, so this stays a dynamic exit -- no static-target risk,
just one fewer trampoline round-trip per jump-table dispatch, on both cores
(this shape isn't gated by ISA level -- ARM7 and ARM9 both hit it).

Validated the same way as Step 6's BXcc-lr fix: 180s in-game differential-
testing soak, no DIFF/MISMATCH/CANARY/OVERRUN lines.

## Step 9 -- performance write-up: items 6's two chain-length fixes

A/B methodology: `tools/benchmark/item6-ab.sh`. Since the BXcc-lr half of
this pair was already committed (77175ae) by the time the jump-table half
was ready, a plain `git stash` baseline can't reach "before either fix" --
the script instead checks `jit_arm.cpp` out from `f92719a` (the commit right
after item 4, i.e. immediately before items 5/6 touched anything) for the
baseline leg, and restores the working tree for the "after" leg. Both legs:
`-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_FORCE_GX2DBG
-DDESMUME_PERFZONES -DJIT_CORE_COST_HISTO` over the unified harness's
`PKT_PROFILE` net sink (`wii_control.py` -> `profile.log`), 150s in-game
SM64DS soak (autoload savestate) each.

### Dispatch-level effect (jitcorecost, ARM7)

Rates from the two runs' first (frame=60) and last jitcorecost line,
averaged over the whole ~4920-4980-frame run (this averages out any single
60-frame window's game-state noise -- see the ARM9 caveat below):

| metric (per DS frame)         | baseline (f92719a) | after (both fixes) | delta |
|--------------------------------|--------------------:|--------------------:|------:|
| `jitRunArm7()` calls            |               5440  |               4673  | **-14.1%** |
| wasted calls (noblock+bail0)    |               2610  |               2199  | **-15.8%** |
| productive ("ran") calls        |               2829  |               2474  | -12.6% |
| guest cycles retired            |             72 470  |             73 516  | +1.4% |
| guest instructions retired      |             32 227  |             32 552  | +1.0% |
| exec time inside `ExecuteJITTrace` |         2258 us  |             2236 us | -1.0% |

Read together: **the same guest work (cycles/instructions retired per frame,
flat within noise) now takes ~14% fewer dispatcher round-trips.** That's
exactly the "longer chains" effect items 5/6 were aimed at -- each call to
`jitRunArm7()` now more often runs a longer chained sequence before
returning, instead of bailing out to the interpreter at a `BXcc lr` or a
jump-table dispatch it used to refuse. `exec_us`/frame staying flat while
call count drops confirms the savings are in per-call dispatch overhead
(`PZ_SCOPE` transition, canary poll, `getBlock` lookup, occasional compile),
not in the emitted code itself running any faster per retired instruction.

Block-length-bucket shift (entries per frame, `elen2_4` -> `elen5_8`):
`elen2_4` -412 (-26% mid-window; the short conditional-return-terminated
blocks that used to end 2-4 instructions in), `elen5_8` +33 (+10%) -- blocks
that used to hand off at a `BXcc lr` boundary now continue a few
instructions further into what used to be the *next* block.

### Frame-time effect (perf_zones, `arm7_jit` / `arm9_jit`)

Matched 60-frame perfzones windows (same nominal frame range, both runs --
`grep -E "^[0-9]+," profile.log`, columns `wall_us`/`arm7_jit_us`/
`arm9_jit_us`), 7 windows spanning frame 4620-4980:

| window (frame) | wall_us delta | `arm7_jit` delta | `arm9_jit` delta |
|---:|---:|---:|---:|
| 4620 | -3.1% | -12.7% | -0.1% |
| 4680 | -3.1% | -11.2% | -0.3% |
| 4740 | -3.7% | -15.7% | +1.8% |
| 4800 | -3.5% | -14.2% | +1.5% |
| 4860 | -3.6% | -9.5%  | -2.9% |
| 4920 | -2.7% | -5.8%  | -4.0% |
| 4980 | -0.8% | -3.7%  | -0.4% |
| **average** | **-2.9%** | **-10.4%** | **-0.6%** |

**`arm7_jit` zone time down ~10% on average, consistently across every
window** (range -3.7% to -15.7%, never a regression). `arm9_jit` is flat
within noise (-4.0% to +1.8%, averaging -0.6%) -- expected, since the
dominant fix (BXcc-lr) was ARM9-only-already; the jump-table widening
applies to both cores but is evidently a much smaller contributor than
BXcc-lr was. Total frame wall time down ~2.9% on average -- consistent with
`arm7_jit` (~5 ms of a ~25-29 ms frame per Step 5's table, so a 10% cut
there is ~0.5 ms/frame, ~1.7-2% of total -- the observed ~2.9% total-wall
drop is in the same ballpark, plus whatever the fewer-round-trips effect
does to cache locality / branch prediction elsewhere, which this data can't
separate out).

**Caveat on window selection:** the ARM9 corecost rates in particular showed
a ~4-5x swing between adjacent 60-frame windows in the raw data (SM64DS's
scripted/no-input demo trajectory has genuinely uneven per-frame ARM9 load --
cutscene triggers, area loads). The whole-run-average rates in the first
table and the matched-window perfzones table above both average across many
windows for exactly this reason; a single-window snapshot would have been
misleading in either direction depending on which window got sampled.

**Bottom line:** items 5+6 (BXcc-lr on ARM7, jump-table register-form
widening on both cores) measurably shrink the ARM7 JIT zone by roughly a
tenth, for zero guest-work-per-frame cost and no differential-testing
regressions -- a real, if modest (~0.5 ms/frame, ~2-3% of total frame time),
win on top of item 4's cache-pressure fix. The bigger remaining levers are
still item 12 (real predicate-path compilation, which per Step 6 would
address ~33%/~83% of ARM7/ARM9 dispatches, an order of magnitude more than
this) and the 2D compositor (~5 ms/frame, untouched by any of this work).
