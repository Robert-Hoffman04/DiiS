# DeSmuME Wii — Performance Opportunities

Investigation date: 2026-09-03. Branch `optimized-Interpretter` @ `c5eaf44`.

> **Update:** items 1.1 (endian access), 2.1 (`-flto`) and a `-funroll-loops`
> substitute for 2.2 have been implemented and benchmarked — **+13 % on the
> GX/merge paths, +8 % software**, correctness verified. See
> [desmumewii-perf-results.md](desmumewii-perf-results.md).

## TL;DR

The current build runs the two DS ARM cores with a **pure interpreter** (no
recompiler of any kind), and the benchmark confirms **97–98 % of every frame is
inside `NDS_exec()`** — the ARM interpreters plus the CPU‑side 2D compositor
(plus the software 3D rasterizer when the GX path isn't used). `Draw()` is 2–3 %.

Two facts dominate the analysis:

1. **There is no dynarec.** Every emulated instruction pays ~40–80 host
   instructions of dispatch/decode/prefetch/cycle‑accounting overhead before
   doing any real work. A Broadway recompiler is the only path to
   "full speed", and is a large project.
2. **Every single memory access in the whole emulator is ~10× slower than it
   needs to be** because of how big‑endian byte‑swapping is written. This is a
   confirmed, mechanical, low‑risk fix with a large blast radius. **Do this
   first.**

---

## 0. Where the time goes (from `tools/benchmark`)

| scene | renderer | % real‑time | `NDS_exec` | `Draw` |
|---|---|--:|--:|--:|
| Volumetric‑shadow demo | software raster | 23 % | 98 % | 2 % |
| Volumetric‑shadow demo | GX hardware 3D | 37 % | 97 % | 3 % |
| Phantom Hourglass intro | software raster | 16 % | 98 % | 2 % |
| Phantom Hourglass intro | GX hardware 3D | 25 % | 98 % | 2 % |

Reading this:

* **`Draw()` is not worth optimizing** (2–3 %). It's already double‑buffered on a
  second thread.
* In 3D‑heavy content the **software rasterizer costs ~40 % of the frame** (SW
  14 fps vs GX 22 fps on the shadow demo). GX already wins decisively there.
* Everything else — the 97 %+ that remains with GX active — is **ARM9 + ARM7
  interpretation + the 2D compositor + scheduler overhead.** That's the real
  target.

The benchmark runs under Dolphin, whose PPC JIT is optimistic and whose GX runs
on the host GPU nearly free. **Real Broadway silicon is slower, and the gap is
worst for the GX/merge paths** — so on hardware the CPU‑emulation share is even
more of the story than the table suggests, and the software rasterizer is
relatively even more expensive.

---

## Tier 1 — the big levers

### 1.1 Fix big‑endian memory access: `lwbrx`/`lhbrx`/`stwbrx` (do this first)

**File:** [source/src/mem.h](source/src/mem.h) — `T1ReadLong_guaranteedAligned`,
`T1ReadWord_guaranteedAligned`, `T1ReadLong`, `T1WriteLong`, `T1WriteWord`,
`T1ReadQuad`, `T1WriteQuad`, `HostReadTwoWords`, `HostWriteTwoWords`, …

Because the Wii is big‑endian and the DS is little‑endian, every one of these
helpers is written as a manual byte assembly:

```c
return (mem[addr+3] << 24 | mem[addr+2] << 16 | mem[addr+1] << 8 | mem[addr]);
```

I compiled this with the project's exact toolchain (`powerpc-eabi-gcc 16.1.0
-O3 -mcpu=750`). GCC does **not** recognize the byte‑swap idiom here:

```
T1ReadLong_guaranteedAligned:              rd32 (via __builtin_bswap32):
  add    r8,r3,r4                             lwbrx  r3,r3,r4
  lbzx   r7,r3,r4                             blr
  lbz    r9,3(r8)
  lbz    r10,2(r8)          11 instructions   →   1 instruction
  lbz    r3,1(r8)
  slwi   r9,r9,24
  slwi   r10,r10,16
  or     r9,r9,r10
  slwi   r3,r3,8
  or     r9,r9,r7
  or     r3,r9,r3
  blr
```

PowerPC has `lwbrx` / `lhbrx` / `stwbrx` — single‑instruction byte‑reversed
load/store. `__builtin_bswap32(*(const u32*)(mem+addr))` emits exactly those.
Stores go 8 instructions → 1.

**Why the blast radius is enormous.** These helpers are on *every* memory path:

* ARM instruction **prefetch** — one `_MMU_read32<…,MMU_AT_CODE>` per emulated
  instruction ([armcpu.cpp:286](source/src/armcpu.cpp#L286)), which bottoms out
  in `T1ReadLong_guaranteedAligned` for main‑RAM / ITCM execution.
* Every `LDR`/`STR`/`LDM`/`STM` operand
  ([arm_instructions.cpp](source/src/arm_instructions.cpp) — the `READ32`/`WRITE32`
  macros, and the `OP_L_IA` etc. block‑transfer macros).
* Every `_MMU_ARM9_read32` / `_MMU_ARM7_*` slow path in
  [MMU.cpp](source/src/MMU.cpp) (I/O registers, VRAM, palette, OAM).
* DMA, the 2D compositor's VRAM/BG/OAM reads in
  [GPU.cpp](source/src/GPU.cpp), the software rasterizer's texture fetches, the
  SPU's sample reads.

**Estimated gain:** an interpreted ARM instruction is roughly 40–80 Broadway
instructions; this removes ~10 from the guaranteed prefetch and ~10 more from
every load/store. Call it **10–25 % overall**, more on memory‑bound game code.

**Risk:** low for the `_guaranteedAligned` variants (aligned by contract) and the
`addr &= ~3` variants (forced aligned). `MAIN_MEM` / `ARM9_ITCM` / `ARM9_DTCM`
are `new u8[…]` (≥8‑byte aligned) and are already accessed as `*(u32*)` in
little‑endian builds, so alignment is already assumed. `-fno-strict-aliasing`
(already set) makes the pointer casts safe.
For the genuinely unaligned callers (`T1ReadWord` without the guarantee, the
`Host*` helpers), keep a `memcpy`‑based swap or the byte path — don't feed
unaligned addresses to `lwbrx` (it can raise an alignment exception).

Practical shape:

```c
static INLINE u32 T1ReadLong_guaranteedAligned(u8* const mem, const u32 addr) {
    return __builtin_bswap32(*(const u32*)(mem + addr));   // -> lwbrx
}
```

Verify with `tools/benchmark/benchmark.sh --scenes vsd --modes "gx"` before/after.

---

### 1.2 An ARM→Broadway dynarec (the real ceiling)

This is *the* structural fix and the reason the emulator is ~3× slow. The
interpreter's per‑instruction tax, all of which a recompiler amortizes to
near‑zero:

* `INSTRUCTION_INDEX(i)` table build + **indirect call** through a 4096‑entry
  function‑pointer table ([armcpu.cpp:419](source/src/armcpu.cpp#L419)) — the PPC
  750 has essentially no indirect‑branch prediction, so this mispredicts
  constantly.
* Condition‑code evaluation (`arm_cond_table` lookup) every instruction.
* `armcpu_prefetch()` — a full templated `_MMU_read32` **every** instruction,
  re‑decoding the address region from scratch each time.
* `MMU_codeFetchCycles` / `MMU_fetchExecuteCycles` / `MMU_aluMemCycles`
  bookkeeping per instruction.
* Re‑decoding immediates, shift types, register fields on every execution of the
  same instruction.

**Options, in increasing cost/reward:**

* **(a) Decoded‑instruction cache / threaded interpreter.** Keep the interpreter
  bodies, but cache the *decode* — a direct‑mapped table keyed by code address
  holding `{handler_ptr, predecoded operands}`, invalidated on writes to code
  pages (the JIT‑block invalidation machinery is the same either way). Removes
  the index computation and most of the re‑decode; keeps the indirect call.
  **Estimate 1.3–1.6×.** Weeks, not months. Good stepping stone.
* **(b) Block recompiler to Broadway.** Translate basic blocks of ARM/Thumb to
  PPC, keep the 15 ARM GPRs + CPSR flags in PPC registers across a block, inline
  the common memory fast paths (main RAM / TCM) with a `lwbrx` and a range
  check, call out only for I/O/VRAM. Upstream DeSmuME's JIT is a useful
  reference for the *IR, block cache, and code‑invalidation design*, but its
  backend is x86/x64‑only — the Broadway backend is new work. **Estimate 2–4×**
  (memory‑bound DS code limits the upside; this isn't Dolphin‑style 10×).
  Multi‑month, needs an executable‑memory allocator with icache flush
  (`__builtin_ppc_icbi` / libogc cache ops) and careful handling of
  self‑modifying code, which DS games do use.

**Risk:** high, and it's a correctness minefield (banked registers, mode
switches, exact flag semantics, SMC, the ARM9 vs ARM7 pipeline‑timing model the
scheduler depends on). But nothing else closes the 3× gap.

---

### 1.3 Make GX the default 3D path; retire the software rasterizer from the hot path

The software rasterizer ([rasterize.cpp](source/src/rasterize.cpp)) costs ~40 %
of the frame in 3D content and the GX path already produces equivalent or better
results at that cost. Actions:

* Ensure the shipping default is `current3Dcore = 1` (GX), not `2`
  ([main.cpp:928](source/src/main.cpp#L928) sets Soft Raster as the menu
  default).
* Land the GXMerge sandwich (the P4 lazy‑readback work is in‑tree; benchmark
  shows `merge` already == `gx`, i.e. the per‑frame GPU→CPU readback is no longer
  on the hot path — see [desmumewii-findings.md](desmumewii-findings.md)). Once
  merge is correct and default, the CPU 2D compositor stops doing per‑pixel 3D
  blend math too.
* Treat the software rasterizer as a compatibility fallback only.

**Estimate:** up to ~1.4× in 3D‑heavy titles; near zero in 2D‑only titles (which
already don't run it).

---

## Tier 2 — medium, mostly mechanical

### 2.1 Turn on LTO (`-flto`)

The interpreter dispatch ([armcpu.cpp](source/src/armcpu.cpp)), the ~7500
instruction handler bodies
([arm_instructions.cpp](source/src/arm_instructions.cpp),
[thumb_instructions.cpp](source/src/thumb_instructions.cpp)), and the MMU
slow‑path functions ([MMU.cpp](source/src/MMU.cpp)) are all in separate
translation units. `_MMU_ARM9_read32` and friends **cannot currently be inlined**
into the handlers. LTO fixes that, plus whole‑program dead‑code elimination.
`-fno-strict-aliasing -fwrapv -fno-aggressive-loop-optimizations` propagate
through LTO fine.

**Estimate:** 5–15 %. **Risk:** longer link, and LTO's cross‑module analysis can
surface latent UB the per‑file build hid — must re‑run the benchmark + a
correctness pass. **Effort:** add `-flto` to `OPTFLAGS` and `LDFLAGS` in the
[Makefile](Makefile), fix fallout.

### 2.2 ~~`-mcpu=750cl`~~ — not available in this toolchain

**Tried and rejected.** devkitPPC's GCC 16 has no `-mcpu=750cl` (valid values:
`… 740 750 7400 …`), and `-mcpu=750` is already the correct choice for Broadway.
`-funroll-loops` was substituted as the second codegen experiment — results in
[desmumewii-perf-results.md](desmumewii-perf-results.md).

### 2.3 Instruction‑fetch page cache

`armcpu_prefetch()` re‑resolves the code region on every instruction. PC is
overwhelmingly sequential within one region (main RAM or ITCM). Cache
`{page_base_host_ptr, page_lo, page_hi}` per core; on the common path a fetch
becomes a compare + `lwbrx`. Invalidate on branch to a new page / DTCM remap /
code write. Pairs naturally with 1.1 and is a subset of 1.2(a).
**Estimate:** 5–12 %.

### 2.4 `LDM`/`STM` block fast path

[arm_instructions.cpp:4653](source/src/arm_instructions.cpp#L4653) onward: each
register in a block transfer calls the full `_MMU_read32`/`_MMU_write32` address
decode **and** `MMU_memAccessCycles` bookkeeping — 8–16 times for a
function prologue/epilogue, even though the whole block is in one memory region.
Resolve the region + base host pointer once, loop with `lwbrx`/`stwbrx`, compute
the cycle cost in closed form. `LDM`/`STM` are among the most frequent ARM ops.
**Estimate:** 3–8 % on call‑heavy code.

### 2.5 Coarsen the ARM9/ARM7 interleave (accuracy‑for‑speed knob)

[NDSSystem.cpp:1580](source/src/NDSSystem.cpp#L1580) `armInnerLoop` interleaves
the two cores at single‑instruction granularity with a timestamp compare and a
`sequencer.reschedule` check every iteration. Running each core for a small block
of cycles before switching (a classic DeSmuME "sync mode" speed hack) cuts that
per‑instruction branch overhead. Expose it as a setting; some titles will
tolerate a much coarser sync than others. `kMaxWork` / `kIrqWait`
([NDSSystem.cpp:1559](source/src/NDSSystem.cpp#L1559)) are flagged
"not tuned very well yet" in‑code — worth sweeping with the benchmark.
**Estimate:** 3–10 % depending on how aggressive.

### 2.6 Scope the UB‑workaround flags instead of applying them globally

`-fno-strict-aliasing -fwrapv -fno-aggressive-loop-optimizations` are currently
in `OPTFLAGS` for the **whole** codebase to work around type‑punning in the
DeSmuME core (see the Makefile comment). They pessimize everything, including
files that don't need them. Move them to per‑file `CFLAGS` overrides for the
handful of offenders (GPU/renderer/MMU) and let the rest compile at full `-O3`.
**Estimate:** a couple %. **Risk:** need to be sure you've found every offender —
regression‑test hard. (Lower priority; the flags exist for a real bug.)

### 2.7 PGO

`tools/benchmark` already boots headless and runs deterministic scenes — it's
90 % of a PGO harness. A `-fprofile-generate` build, a benchmark run to collect
`.gcda`, then `-fprofile-use` would let GCC lay out the instruction‑dispatch
switch/branches by actual frequency. **Estimate:** 5–10 %. **Effort:** moderate
plumbing (getting `.gcda` files off the Wii / out of Dolphin).

---

## Tier 3 — small / local

* **`Draw()` swizzle** ([main.cpp:420](source/src/main.cpp#L420)): the 4‑deep
  nested `RGB15_REVERSE` tiling loop + two `DCFlushRange`s. Only 2–3 % of the
  frame, but could be a paired‑single / DMA job if you ever want it back.
* **Audio mix on CPU**: `SPU_MixAudio` ([SPU.cpp:904](source/src/SPU.cpp#L904))
  runs the mixer on Broadway. The Wii DSP is idle. Offloading is real work for a
  small (~few %) and title‑dependent win.
* **Backdrop write in the 2D compositor**
  ([GPU.cpp:2098](source/src/GPU.cpp#L2098)): in‑code comment says
  *"currently eating up 2fps or so … a reasonable candidate for optimization."*
* **Per‑scanline `memset`s** in `GPU_RenderLine_layer`
  ([GPU.cpp:2129](source/src/GPU.cpp#L2129)) — `sprAlpha`/`sprType`/`sprPrio`/
  `sprWin` cleared every line whether or not sprites are enabled.
* **`_MMU_accesstime`** ([MMU_timing.h:258](source/src/MMU_timing.h#L258)): with
  most accuracy `#define`s off, the `MMU_WAIT[16*16]` table + `FetchAccessUnit`
  object dispatch per data access could collapse to a small `switch` on
  `addr>>24`. Minor.
* **`-funroll-loops`** on the rasterizer / compositor TUs — measure.

---

## Suggested order of work

1. **1.1 endian fix** — days, low risk, 10–25 %. No reason not to.
2. **2.1 LTO** + **2.2 substitute codegen flag** — a day each to try, measure, keep if green.
3. **2.3 fetch page cache** + **2.4 LDM/STM fast path** — natural follow‑ons to 1.1.
4. **1.3** — make GX the default, finish GXMerge.
5. **2.5 / 2.7** — tuning passes with the benchmark.
6. **1.2** — decide between the decoded‑instruction cache (1.2a, weeks) and a
   full Broadway recompiler (1.2b, months). Everything above is worth doing
   regardless of that decision, and 1.2a shares infrastructure with 2.3.

Re‑run `tools/benchmark/benchmark.sh` after each step — it diffs against the
previous run and flags >3 % regressions automatically.
