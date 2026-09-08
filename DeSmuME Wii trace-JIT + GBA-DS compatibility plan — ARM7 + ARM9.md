# DeSmuME Wii trace-JIT + GBA/DS compatibility plan — ARM7 + ARM9

One plan, one shared engine, two CPU front-ends, with a parallel hardware-compatibility and conformance track.

Port VBA-GX's THUMB trace-JIT to DeSmuME Wii and extend it with a new ARM (32-bit) front-end, as a single CPU-agnostic core that drives both the DS ARM7 (ARMv4T) and ARM9 (ARMv5TE). The interpreter is always the correctness fallback: anything a front-end doesn't recognise ends the trace at that PC and the interpreter takes over. No full CPU-core rewrite, no cross-block register allocation, and no optimizing IR unless measured evidence later demonstrates that the existing architecture cannot meet the target.

In parallel, implement and validate the Nintendo DS's native GBA backwards-compatibility path as an actual DS hardware mode rather than as an unrelated GBA emulator bolted onto the side. The same ARM7 implementation should be capable of operating in both normal DS ARM7 mode and GBA compatibility mode, with the relevant memory, cartridge, BIOS, interrupt, DMA, timer, video, audio, input, save, reset, and timing behavior modeled at the appropriate hardware boundary.

The project should use **reference-first testing** throughout. Before implementing obscure hardware behavior, run each relevant test ROM through an established reference emulator, such as melonDS for DS behavior or a suitable GBA emulator for actual GBA execution, and record the results. Then determine which remaining differences are required by conformance ROMs, hardware evidence, or commercial software, rather than spending disproportionate effort on quirks that established implementations and available evidence do not support.

There is an important limitation: melonDS is a DS emulator, not a GBA emulator. It supports loading a GBA ROM into the DS Slot-2 for DS games that access the cartridge, but does not execute GBA games. Therefore:

- **DS homebrew and DS hardware tests:** use melonDS as the first external reference.
- **Actual GBA CPU/peripheral execution:** compare against an established GBA emulator such as mGBA.
- **Original-hardware behavior:** use documentation, hardware observations, known test ROMs, and later hardware-originated test software when necessary.

The objective is not to make DeSmuME reproduce another emulator's quirks. The objective is to use established implementations as a baseline, identify meaningful differences, and then converge toward documented/original hardware behavior.

---

## 1. Why this JIT, and why both CPUs

- **Both cores are architecturally solvable.** DS ARM7 is an ARM7TDMI (ARMv4T), the same fundamental ISA used by the GBA. DS ARM9 is ARMv5TE and adds ARMv5 instructions and interworking behavior.
- **VBA-GX supplies most of the dynarec infrastructure:** Broadway emission, arena allocation, block caching, linking, ABI trampoline, register/flag tracking, SMC tracking, and differential testing.
- **ARM7 is the proving ground; ARM9 is the primary performance target.** ARM7 JIT work also directly supports the GBA compatibility effort because the GBA execution environment relies on ARM7-class CPU behavior.
- **One engine, not two.** THUMB and ARM emitters remain shared and CPU-agnostic. CPU-specific behavior is expressed through `JitCpuProfile`.
- **GBA compatibility should reuse the same ARM7 implementation.** Do not create a second independent ARM7 core solely for GBA mode unless a demonstrated architectural difference makes it unavoidable.

---

## 2. Architecture

### 2.1 Reused VBA-GX components

| File                      | Role                                         | Reuse                                          |
| ------------------------- | -------------------------------------------- | ---------------------------------------------- |
| `JITCache.{h,cpp}`        | arena, block hash, linker stub, SMC registry | CPU-agnostic                                   |
| `JITPPCEmitter.h`         | PowerPC-750 instruction encoding             | reused                                         |
| `JITTrampoline.S`         | C ABI ↔ JIT register bridge                  | near-verbatim                                  |
| `JIT.h`                   | `JITResult` and public JIT interface         | reused                                         |
| `JITCompiler.cpp`         | original THUMB scanner/emitter               | split into shared trace core + THUMB front-end |
| `JITDifferential.{h,cpp}` | interpreter/JIT comparison                   | reused and hardened                            |
| `jit_debug.h`             | debug/profiler compatibility                 | shim                                           |

### 2.2 Module layout

```text
jit_cache.{h,cpp}
jit_ppc_emitter.h
jit_trampoline.S
jit.h
jit_cpu_profile.h
jit_trace.{h,cpp}
jit_thumb.cpp
jit_arm.cpp
jit_arm7_profile.cpp
jit_arm9_profile.cpp
jit_exec.cpp
jit_differential.{h,cpp}
jit_selftest.cpp
jit_thumb_test.cpp
jit_debug.h
upstream/
```

Two independent JIT caches remain necessary:

```text
jitCacheArm7
jitCacheArm9
```

The JIT remains gated during development:

```text
DESMUME_JIT_ARM7
DESMUME_JIT_ARM9_ON
JIT_DIFFERENTIAL_TESTING
```

---

## 3. CPU-agnostic JIT seam

```c
struct JitCpuProfile {
    u32*  gpr;
    u32*  cpsr;

    u8**  readPage;
    u8**  writePage;

    u32   (*slowRead)(u32 addr, int size);
    void  (*slowWrite)(u32 addr, u32 val, int size);

    void  (*smcInvalidate)(u32 addr);

    bool  (*canEnterThumb)(u32 pc);
    bool  (*canEnterArm)(u32 pc);

    u32   (*swiHandler)(u32 comment);

    u8    cyclesForThumb(u16 opcode);
    u8    cyclesForArm(u32 opcode);

    s32   isaLevel;
};
```

A block remains single-mode and single-CPU.

Instructions that alter ISA state or banked-register state remain block terminators where necessary:

- `BX`
- `BLX`
- `SWI`
- `MOVS pc,lr`
- `LDM ^`
- control-field-changing `MSR`
- other mode/banked-register transitions

This keeps architectural corner cases in the interpreter until they are explicitly proven safe to compile.

---

## 4. JIT execution economics

Opcode coverage alone is not the primary performance question.

Measure and optimize:

```text
dispatches
blocks_compiled
blocks_executed
guest_instructions
jit_guest_instructions
interpreter_guest_instructions

guest_instructions_per_jit_entry
blocks_per_jit_entry

average_block_length
median_block_length
maximum_block_length

average_chain_length
median_chain_length
maximum_chain_length

static_exits
dynamic_exits
dynamic_exit_rate

jit_bailout_count
jit_bailout_rate

trampoline_entries
trampoline_exits

cycles_per_jit_entry
cycles_per_guest_instruction

jit_execution_time
jit_compilation_time

memory_fast_path_hits
memory_slow_path_hits
```

The first optimization priority should be reducing fixed cost per JIT entry:

1. static chaining
2. guarded dynamic chaining
3. measure actual chain lengths
4. amortize trampoline overhead
5. keep guest state resident longer
6. reduce unnecessary register save/restore
7. then optimize memory access

Do not introduce an optimizing IR merely because individual generated instructions appear inefficient. Escalate only if the simpler architecture demonstrably fails the measured target.

---

## 5. Persistent JIT state

Investigate keeping frequently used guest state resident across chained blocks.

Goals:

- reduce C ↔ JIT transitions
- reduce repeated register synchronization
- reduce trampoline overhead
- preserve CPSR/flags where safe
- make chained execution approach a continuous native-code path

Every optimization must preserve:

- interrupts
- scheduler boundaries
- DMA visibility
- shared-memory visibility
- SMC invalidation
- CP15 effects
- ARM/THUMB transitions
- interpreter bailout correctness

### Progress

Two pieces of guest state that need no per-block liveness analysis are now
resident in reserved non-volatile host registers for the whole trace, synced to
memory only by the trampoline (which every exit path funnels through):

- **P12 slice 1 — instruction count (r31).** Was a load/add/store through
  `out->instructions` per block; now one `addi`. `emitResultMetadata()` clean
  path 8 PPC instructions → 1. SM64DS A/B 12.61 → 12.67 fps.
- **P12 slice 2 — packed CPSR flags (r30).** Was loaded from `*cpsr` on first
  flag use per block, flushed on exit, and spilled/reloaded around every memory
  op. Now resident and callee-saved: no block loads or flushes it. SM64DS A/B
  12.67 → 12.71 fps.

Both validated 0-DIFF (ARM7 + ARM9), 0 CANARY / OVERRUN.

**GPR residency across chained blocks — deferred (§23).** The third piece of §5
would keep guest R0–R14 resident across a chain. That needs either cross-block
register allocation (out of scope) or a fixed guest→host mapping. Measured ARM7
chain length on SM64DS is ~2 blocks / ~5 guest instructions — short, in part
because predicated ARM branches bail to the interpreter by default and force a
trampoline round-trip (`-DJIT_ARM_PRED_BRANCH` compiles predicated `Bcc` +
`BLcc` — ARM9-validated, ARM7 pending the armwrestler gate; §16). A fixed mapping's
unconditional 15-register trampoline load/store
would lose against the current lazy allocator at that chain length. Not
justified by current evidence.

**Measured picture:** on SM64DS (ARM9-bound, ARM9 interpreted) the ARM7 JIT is
still a ~4.4% whole-frame regression. The dominant remaining cost is the sheer
number of trampoline round-trips (short chains) plus the trampoline's own
`stmw`/`lmw`. ARM7-JIT frame value is likely capped until (a) predicated ARM
branches compile by default for ARM7 (the `Bcc` + `BLcc` codegen exists behind
`-DJIT_ARM_PRED_BRANCH`, ARM9-validated; ARM7 default-on rides the armwrestler
gate; §16) and/or (b) the ARM9 JIT is viable in `jitfull` (also §16).

---

## 6. Memory fast paths

### Tier 0 — existing MMU path

Retain the existing C/MMU path as the correctness fallback.

### Tier 1 — direct mapped regions

Inline direct accesses for regions whose mapping and permissions are stable:

- main RAM
- ITCM
- DTCM
- shared WRAM where safely resolved
- other proven direct-memory regions

**Progress (P13):**
- **Landed:** pc-relative literal loads (`LDR/LDRB/LDRH rX,[pc,#imm]`) whose
  compile-time-constant EA is in main RAM (0x0_2xxxxxx) → one `lwbrx`/`lhbrx`/
  `lbzx` into Rd's host reg, no C call, no runtime guard. `JitCpuProfile
  ::mainMemBase` holds `MMU.MAIN_MEM` (ARM7 only; the ARM9's relocatable TCM
  overlays this range). 0-DIFF validated.
- **Tried, reverted (§23):** a runtime-region-guarded general `LDR/STR` fast
  path. 0-DIFF but **-0.7% / +0.4pp cv** on the SM64DS ARM7 A/B — the
  `rlwinm+cmpwi+bne` guard is paid on every access and SM64DS's ARM7 is
  entirely shared-WRAM-resident (`0x037fxxxx` for code, literals and data), so
  the main-RAM fast path almost never fires.
- **The real ARM7 win** is a **WRAM/SWIRAM tier** (that is where the ARM7
  actually executes and works), but it needs WRAMCNT-aware base resolution and
  the interpreter's own ARM7 read fast path (MMU.h) is WRAMCNT-blind while its
  write path is not — reconcile that before inlining. Deferred.

### Tier 2 — cached page descriptors

Introduce descriptors containing approximately:

```text
host_base
permissions
timing_class
SMC/code flag
mapping generation
```

Invalidate/rebuild descriptors when mappings change.

**Progress (P14):**
- **Landed:** a flat `JitPageDesc { hostBase, mask }` table
  (`JitCpuProfile::pageDescBase/Lo/Hi`). The ARM7 captures DeSmuME's own
  `MMU.MMU_MEM` / `MMU.MMU_MASK` entries for pages `0x20..0x3F` (main RAM,
  shared WRAM, ARM7 ERAM) at `jitInit` -- in this fork those ARM7 page-table
  entries never move (`REG_WRAMCNT` only updates the WRAMSTAT mirror byte), so
  a one-time capture is safe; a comment flags the rebuild point if that ever
  changes. Left 0 on the ARM9 (CP15-relocatable TCM overlays 0x02xxxxxx).
- `JitTraceCtx::emitPageResolve()` emits a runtime page-window guard (an EA
  outside the window, or a run that straddles a 1 MB page, bails to the
  interpreter for that one instruction with the compile-time dirty
  bookkeeping preserved for the fall-through), one descriptor resolve, and
  hands back `hostBase` + aligned in-page offset.
- `emitInlineLoad()` (single `LDR/LDRB/LDRH/LDRSB/LDRSH`, ARM + THUMB
  F6/F9/F8/F10/F11) then emits one `lwbrx`/`lhbrx`/`lbzx` with LDR's
  unaligned-word ROR, **register cache intact, no memory prologue, no
  slowRead C call**.
- **Validated** (P14+P15 together, one soak): SM64DS 210 s differential soak
  (`JIT_DIFFERENTIAL_TESTING`) -- ARM7 `diff` 500 K blocks / 1.96 M insns,
  **0 mismatches / 0 CHAIN-DIFF**; ARM9 `diff9` 64.7 M blocks / 142 M insns,
  0 mismatches (`pageDescBase` is 0 on the ARM9 -- inert); selftest +
  journal-selftest PASS; 0 CANARY / 0 ARENA-OVERRUN; ARM7 cycDrift bounded
  (max 9, advisory).
- **Measured:** SM64DS ARM7-JIT A/B **12.62 -> 12.55 fps** (P14 alone), i.e.
  frame-neutral within cv (4.6 %). As with the §6-Tier-1 general path, the
  guard is real work and SM64DS is ARM9-bound (ARM7 is ~2 % of the frame, far
  below the benchmark's resolution), so this cannot be shown to pay off on the
  one available retail workload -- but unlike the Tier-1 general path it is
  *not* a regression, the fast path *hits* (WRAM-resident code), and it is the
  descriptor infrastructure the later phases (GBA mode §20+, longer ARM7
  chains once §16 lands) will actually exercise. Landed on that basis.

### Tier 3 — structured memory operations

Optimize:

- LDM
- STM
- PUSH
- POP
- sequential loads/stores

Do not sacrifice memory-map correctness for benchmark gains.

**Progress (P15):**
- **Landed:** `emitInlineBlockLoad()` -- one `emitPageResolve()` for the whole
  contiguous run (covering the last word too), then `n` sequential `lwbrx`
  straight into the registers' host slots, register cache intact. One page
  guard amortised over the whole list instead of `n` slowRead C calls. Wired
  into ARM `LDM` (non-pc), THUMB `LDMIA`, THUMB `POP` (non-pc).
- **Deferred:** `STM` / `PUSH` / `STMIA` keep the slow path -- an inline store
  needs a flushing multi-page SMC guard and a per-word `jitDiffJournalNote`
  call in `JIT_DIFFERENTIAL_TESTING` builds (the §16 trial-JIT rollback
  requirement that bit the Tier-1 store path). `LDM{...,pc}` keeps the slow
  path (block-terminator interworking).
- **Validated:** shares the P14 soak above (500 K ARM7 `diff` blocks /
  64.7 M ARM9 `diff9` blocks, 0 mismatches -- LDM/LDMIA/POP inline exercised
  during boot with per-instruction guest-state comparison).
- **Measured:** SM64DS ARM7-JIT A/B **12.55 -> 12.58 fps** (P15 on top of
  P14); combined P13->P15 is 12.62 -> 12.58, flat within cv. Same story as
  P14: correctness-clean, frame-neutral, below benchmark resolution on this
  ARM9-bound workload.

---

## 7. ARM7 JIT

### 7.1 ARM front-end

Wire the existing ARM front-end to ARM7 after ARM9 has established correctness.

ARM7 must support the ARMv4T subset actually used by the DS ARM7 and GBA compatibility environment.

**Landed (P11):** `jitRunArm7()` now dispatches ARM-mode blocks through the
shared `jit_arm.cpp` front-end (`arm7_canEnterArm` / a real `arm7_cyclesForArm`
3-stage add-timing model). The emitter gates every ARMv5-only encoding
(BLX imm/reg, CLZ, QADD/QSUB/QD*, the SM* DSP multiplies, LDRD/STRD, the whole
cond==NV space) and the ARMv4-vs-ARMv5 `LDR pc` / `LDM {..,pc}` interworking
difference (ARM7 `LDTBit == 0`: `R15 = word & ~3`, no mode switch) on
`cpu.isaLevel`. Predicated ARM branches bail to the interpreter by default;
`-DJIT_ARM_PRED_BRANCH` compiles predicated `Bcc` and `BLcc` (ARM9-validated at
~1.3 B instructions — §16). Both its CPU-correctness gates are now clear
(armwrestler + arm7wrestler, roadmap #17/#18, §8.2/§8.3 — 0 new failures on
either); still opt-in pending the broader §25 default-on bar, not because of
an open correctness gate on this flag specifically.

**Validated (SM64DS, headless Dolphin):**
- 210s differential soak: ARM7 `diff` 400K blocks / 1.67M insns, **0 mismatches**,
  0 `DIFF` / 0 `CHAIN-DIFF` / 0 `ARENA OVERRUN`. ARM9 `diff9` 59.8M blocks
  unchanged from baseline (the `isaLevel` gate is inert for ARMv5TE — no
  regression).
- 240s non-differential production-path soak (chaining on, no harness): 8.2M
  block runs / 39M guest insns, ARM-mode blocks confirmed executing (`blk A`),
  **canary poll never latched** (the host-heap tripwire from the ARM9
  predicated-Bcc investigation — it ruled out an arena/table overrun there; the
  bug itself was flagged by a PH crash, not by any detector), 0 SMC-kill
  anomalies over 50M checks.
- Cycle model is coarse v1: ~23% of ARM7 blocks show cycle drift, bounded at
  ≤13 cyc/block (harness treats ARM7 cycle drift as advisory). Refinement later.
- `armwrestler` and `arm7wrestler` both now run headless (§8.2/§17,
  §8.3/§18) -- ARM7 *differential* coverage from SM64DS is still thin (see
  §16's predicated-branch status above), but `arm7wrestler` now supplies the
  direct ARM7 CPU-correctness signal that gap was blocking: 0 new failures
  under `-DJIT_ARM_PRED_BRANCH` relative to the interpreter baseline.

### 7.2 ARM7-specific validation

Before trusting ARM7 JIT results:

1. boot `armwrestler`
2. boot `arm7wrestler`
3. run ARM/THUMB instruction tests
4. compare JIT against the DeSmuME interpreter
5. compare externally against a suitable reference where practical

The ARM7 JIT must not become the foundation for GBA compatibility until its CPU behavior has passed these gates.

---

# 8. DS homebrew conformance and hardware-test track

The project should maintain a dedicated DS test-ROM suite rather than relying exclusively on commercial games.

There is no single DS equivalent with the breadth of the GBA conformance suites, so use a complementary collection.

## 8.1 Reference-first rule

For each DS test ROM:

1. Run it on **melonDS** or another established DS emulator.
2. Record the observed result.
3. Run it on DeSmuME's interpreter.
4. Compare the two.
5. Determine whether a discrepancy is:
   - a likely DeSmuME bug,
   - a likely melonDS limitation/quirk,
   - an intentionally different implementation,
   - or an unresolved hardware question.
6. Only then use the test to drive JIT or hardware-model changes.

This prevents the JIT from accidentally becoming a second source of uncertainty.

---

## 8.2 `armwrestler`

Use `armwrestler` as an early low-level CPU gate.

Primary purpose:

- ARM instruction correctness
- arithmetic behavior
- condition handling
- CPU state behavior
- baseline ARM7/ARM CPU confidence

It should be run before interpreting higher-level DS test failures.

### Status: automated, headless, running (§17)

`armwrestler` is menu/button-driven and reports only by drawing to the DS
screen -- unusable in a headless soak by itself. `tools/armwrestler/` vendors
it (PROVENANCE.md) patched to auto-run every ARM9 ARM and THUMB test with no
input and report results through **slot-2 expansion RAM** (`0x09000000`+,
desmumewii's `ExpMemory` addon -- flat host RAM, no flash/SRAM protocol) --
the same "write results where the emulator can just read them" trick a
custom melonDS build could use identically, since melonDS emulates the same
GBA-slot address space. `-DDESMUME_ARMWRESTLER_PROBE` (`main.cpp`) selects
the addon, disables the ARM7 JIT (armwrestler's ARM7 side is a one-instruction
idle stub -- nothing to test there), polls slot-2 once per frame, and dumps
`sd:/armwrestler.log` (ARM + THUMB pass/fail counts, plus each failure's
instruction name and raw bitmask) on completion.

Building this surfaced and fixed one bug and found two more:

- **Fixed**: `ExpMemory`'s default 8MB allocation, combined with the JIT
  subsystem's arena/table allocations simply being compiled in
  (`-DDESMUME_JIT_ARM7`, the master flag -- independent of whether either
  core's JIT is runtime-enabled), exhausts the Wii's MEM1 and freezes the
  emulator before even the first SMC-tracked memory access. Confirmed by
  elimination, not guessed: reproduced identically with ARM9 JIT off, with
  ARM7 JIT off, and with predicated branches off -- and disappeared outright
  when `expMemSize` was cut from 8MB to 64KB (all this probe needs), with
  nothing else changed. Left shrunk permanently in `main.cpp`, not just for
  the diagnosis.
- **Found and fixed** (interpreter baseline: ARM 0/67 fail, THUMB 1/10 fail --
  the one THUMB fail is `ADD` with a `BAD_Rd` bitmask, almost certainly the
  `ADD Rd,PC,#imm` pipeline-offset test, a known-finicky case rather than a
  JIT-caused issue since the interpreter alone reproduces it, and is still
  present after both fixes below -- left alone, out of scope here): the ARM9
  JIT (`jit9on`) added two **new** failures beyond that baseline, both now
  fixed and reverified back down to the clean baseline (ARM 0/67, THUMB 1/10)
  under a full `-DJIT_ARM_PRED_BRANCH` build, plus a 12.2M-block / 365.6M-insn
  SM64DS differential soak at 0 mismatches:
  - **ARM `SMLAL`** (`BAD_Rd`, the B5 64-bit signed multiply-accumulate path).
    Root cause: `emitMultiply`'s accumulate chained `ADDCO`(RdLo) directly into
    `ADDEO`(RdHi), relying on XER[CA] surviving unaided between two adjacent
    instructions -- the carry was silently lost (confirmed instruction-by-
    instruction: a slot-2 dump of the exact host-register inputs feeding the
    test's `smlals` showed every input correct and RdLo exactly correct, but
    RdHi short by precisely the missing +1 carry). Every *other* `ADDEO`/
    `SUBFEO` consumer in this JIT (`emitAlu`'s ADC/SBC/RSC, `jit_thumb.cpp`'s
    ADC/SBC) already re-primes XER[CA] via `MFXER` + `ADDIC` immediately before
    consuming it rather than trusting raw adjacency; `emitMultiply` was the one
    place that didn't. Fixed by adding the same re-priming there.
  - **THUMB `LDR`** (`BAD_Rd` x2, register- and immediate-offset forms). Root
    cause: the shared THUMB load/store emitter (F9/F8/F10, `jit_thumb.cpp`)
    passed `wordRotate=false` unconditionally to both `emitInlineLoad` and its
    slow-load path, so a word `LDR` from an unaligned EA never got the
    ROR-by-`8*(EA&3)` that the interpreter (`OP_LDR_IMM_OFF`/`OP_LDR_REG_OFF`)
    and ARM mode's own `emitLoadStoreTail` both apply. `LDR Rd,[PC,#imm]` (F6)
    and `[SP,#imm]` (F11) are unaffected -- their EAs are always 4-byte
    aligned by construction, so the rotate would be a no-op there regardless.
    Fixed by computing `wordRotate = isLoad && size==4` and applying the same
    ROR sequence ARM mode uses (`emitLoadStoreTail`'s post-`emitSlowLoad`
    block) for both the inline-load and slow-load paths.

  Diagnostic note: an early attempt to catch these live via a
  `-DJIT_DIFFERENTIAL_TESTING` + armwrestler-probe combo build was unreliable
  for *counts* (ARM/THUMB totals came back inflated ~2-3x, non-uniformly) --
  the differential harness's guest-RAM journal/rollback (A1, `jit_differential.cpp`)
  has no cell decode for slot-2/GBA-cart addresses (`diffJournalCellArm9`
  returns null there), so the reference-interpreter dry run's writes to
  `armwrestler`'s slot-2 result buffer are never rolled back while the block's
  real, non-journalled second (and sometimes third, for CHAIN-DIFF) execution
  writes again -- a double/triple count, not a correctness issue. It also never
  produced a DIFF/CHAIN-DIFF log line for either bug, most likely because
  `armwrestler`'s slot-2 writes mark the block `s_journalUnrestorable` and
  drop it from the trusted comparison entirely. Both bugs were actually
  root-caused with a **non-differential** single-execution probe build plus a
  one-off ROM-side patch (`str`s right after the tested instruction, before
  `DrawResult`'s own prologue could clobber the result registers) shipping the
  raw pre/post register values out through slot-2 alongside the pass/fail
  bitmask -- not committed, reproducible from this note if needed again.

Run it: `tools/armwrestler/build.sh`, stage `out/armwrestler.nds` as
`sd:/DS/ROMS/test.nds`, boot a `-DDESMUME_ARMWRESTLER_PROBE
-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=2` build, pull `sd:/armwrestler.log`.

---

## 8.3 `arm7wrestler`

Use `arm7wrestler` as a dedicated ARM7 validation target.

It is particularly important because the ARM7 is shared conceptually between:

- normal DS ARM7 execution
- GBA compatibility execution
- ARM7 JIT execution

A failure here can contaminate several later workstreams.

### Status: automated, headless, running (§18) -- ARM7 predicated-branch gate cleared

`tools/arm7wrestler/` vendors [Arisotura/arm7wrestler](https://github.com/Arisotura/arm7wrestler)
(same author lineage/no-formal-license situation as §8.2's `armwrestler`;
see its PROVENANCE.md) -- the fork that moves the real ARM/THUMB test
content onto the ARM7 (armwrestler's own ARM7 side is a no-op stub; this
ROM's ARM9 side is the stub instead) and adds an `ExceptionHandler` install
so the ROM can observe `undefined-instruction` exceptions from ARMv5-only
opcodes on real ARM7TDMI hardware. Patched with the same auto-run + slot-2
result-I/O technique as §8.2/§17, reusing the identical header layout so
both ROMs' results are read by near-identical probe code
(`-DDESMUME_ARM7WRESTLER_PROBE`, `main.cpp`).

This needed its own from-scratch minimal ARM7 crt0/linker script
(`ds_arm7_crt0.S`/`ds_arm7.ld`, adapted from `tools/armwrestler`'s ARM9 ones
-- this devkitPro install's bundled ARM7 crt0 has a libnds version mismatch,
see PROVENANCE.md) and, more importantly, surfaced a real bug in the ROM's
own auto-run driver during bring-up:

- **Found and fixed** (in the vendoring patch itself, before this ever
  became a signal about desmumewii): the driver's THUMB test dispatch
  reused the vendored ARM9 armwrestler's `mov lr,pc; bx _runtest+1` idiom,
  which relies on the THUMB test routines' own `pop {..,pc}` return
  interworking back to ARM based on the popped address's bit0. That's true
  on ARMv5 (why it works for the ARM9 armwrestler) but **not** on
  ARMv4T/ARM7TDMI, where `LDM`/`POP` with `pc` in the list loads `pc` and
  stays in the *current* instruction set state -- only `BX` interworks
  pre-ARMv5. Confirmed empirically: ARM Test0..5 and THUMB `_test0`
  completed correctly, then execution silently died exactly at `_test0`'s
  own `pop {pc}`, landing back in the ARM driver's bytes still in THUMB
  state (decoded as garbage THUMB opcodes). desmumewii's ARM7 interpreter
  getting this ARMv4T-vs-ARMv5 distinction right is *exactly* the kind of
  thing this ROM exists to catch -- it just caught it in the test driver
  first. Fixed with `_thumb_trampoline` (thumbwrestler-ds.asm): entered via
  `bx` (which always interworks), stashes the ARM return address in r8
  (untouched by `_runtest`/every `_testN`/`_drawresult`/`_drawtext`), calls
  `_runtest` as an ordinary THUMB-to-THUMB `bl` (interworking-free, correct
  on ARMv4T), then `bx r8` to interwork back to ARM explicitly.

**Interpreter baseline** (no JIT compiled in): **ARM 11/67 fail, THUMB
1/20 fail**. Cross-checked against `UPSTREAM-README.md`'s documented
ARMv4T-vs-ARMv5 differences -- all 11 ARM fails are exactly the documented
set: `LDM` with base-in-writeback-list ×4 (interpreter correctly matches
real ARM7 hardware's "writeback never happens" quirk), `CLZ` ×1, `LDRD` ×1,
`QADD` ×1, `SMLABB`/`SMLABT`/`SMLATB`/`SMLATT` ×4. The `LDM` result matches
documented hardware behavior; `CLZ`/`LDRD`/`QADD`/`SMLAxy` executing instead
of raising `undefined-instruction` (or being silent no-ops, per the
README) is a plausible **interpreter**-level ARMv4T-conformance gap, not
scoped/fixed here -- flagged for later interpreter work, out of scope for
the JIT-gate purpose of this section. The one THUMB fail is the same
pre-existing `ADD Rd,PC,#imm` pipeline-offset quirk §8.2 already documents
for the ARM9 armwrestler (unrelated to any of the above).

**JIT build** (`-DDESMUME_JIT_ARM7 -DJIT_ARM_PRED_BRANCH`, ARM9 JIT off --
this ROM's ARM9 side is upstream's own trivial idle/vram-copy stub):
**byte-identical output to the interpreter baseline** -- same ARM 11/67,
THUMB 1/20, same 12 fail entries in the same order. **Zero new failures
from compiling predicated `Bcc`/`BLcc` on ARM7.** This is the ARM7 gate §16
was waiting on.

**SM64DS differential soak** (`JIT_DIFFERENTIAL_TESTING`, ARM7+ARM9 JIT,
predicated branches on, ~4 min headless): ARM9 `diff9` 20.3M blocks / 608.7M
instructions, **0 mismatches**. ARM7 `diff` again never reaches its 100K
report threshold on this workload (SM64DS's ARM7 side is boot-then-idle, as
previously documented) -- `predBcc a7=462` compiles logged, plateauing after
boot, consistent with §16's existing characterization of SM64DS as thin ARM7
ARM-mode coverage. `arm7wrestler` itself remains the real ARM7 predicated-
branch signal; SM64DS here is corroborating "no regression on the one retail
workload available", not the primary evidence.

Run it: `tools/arm7wrestler/build.sh`, stage `out/arm7wrestler.nds` as
`sd:/DS/ROMS/test.nds`, boot a `-DDESMUME_ARM7WRESTLER_PROBE
-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=2` build (add
`-DDESMUME_JIT_ARM7 -DJIT_ARM_PRED_BRANCH` to JITDEFS for the JIT path;
omit all JIT flags for the interpreter baseline), pull `sd:/arm7wrestler.log`.

---

## 8.4 RockWrestler

Make **RockWrestler the primary DS homebrew conformance/regression target**.

RockWrestler explicitly targets NDS emulator behavior and covers areas including ARMv4 condition codes, ARMv5 instructions, ARM/THUMB interworking, LDM/STM edge cases, IPCSYNC, IPCFIFO, IPC interrupts, DS math hardware, WRAMCNT, VRAMCNT, DTCM, ITCM, and TCM behavior.

Organize the automated results into categories:

### CPU

- ARMv4 condition codes
- ARMv5 instructions
- `CLZ`
- saturating arithmetic
- DSP multiplies
- `BLX`
- ARM/THUMB state transitions
- LDM/STM edge cases

### IPC

- IPCSYNC
- IPCSYNC IRQ
- IPCFIFO
- FIFO error handling
- FIFO IRQs

### Memory

- WRAMCNT
- VRAMCNT
- DTCM
- ITCM
- TCM load mode
- overlapping TCM regions

### DS hardware

- math-unit division
- square root
- startup register state
- CP15 state

These tests map directly onto project risks:

```text
condition codes       → ARM JIT predication
ARMv5                  → ARM9 front-end
ARM/THUMB interworking → JIT transitions
LDM/STM                → memory optimization
IPC                    → dual-CPU scheduling/state
WRAM/VRAM              → memory subsystem
TCM                    → ARM9 memory mapping
```

RockWrestler should become a **formal automated regression gate**, not merely a manual debugging ROM.

### Status: automated, headless, running (§19) -- 10/23 pre-existing interpreter fails found, 0 new from the JIT

Vendored + patched into `tools/rockwrestler/` (see `PROVENANCE.md`). Unlike
`armwrestler`/`arm7wrestler`, upstream RockWrestler is `-nostartfiles` C++
with its own trivial per-CPU `entry.s` and single-section linker scripts --
no libnds, so none of `arm7wrestler`'s crt0/linker version-mismatch work was
needed; the installed devkitARM toolchain builds it unmodified.

All real per-instruction CPU test content runs on the **ARM9**
(upstream's own design -- see UPSTREAM-README.md); the ARM7 side only
cooperates via its pre-existing, already non-interactive `arm7_waitloop`.
So only `src9/framework/menu.cpp`/`menu.s` needed the same treatment as
§17/§18: `cpp_menu()`'s interactive A/B/up/down loop replaced with a flat
walk over every real test across the five category submenus (skipping
"INITIAL STATE", which is register dumps with no pass/fail), results
tallied into the same slot-2/`ExpMemory` convention (running/failed counts
+ a capped fail log, this time storing each failure's own `MenuEntry.text`
pointer rather than a separate name table, since the test set doesn't
change at runtime), and `fail_test`/`timeout_test`/`timeout_rw` no longer
wait for a B-button press before returning to the menu. No interworking
hazard here of the kind §18 hit: every driver-level call and CPU-state
test in this ROM runs on ARM9 (ARMv5), where the naive `pop {..,pc}`
idiom is architecturally correct.

Interpreter baseline: **10/23 fail**, byte-identical against a
`-DJIT_ARM7 -DJIT_ARM_PRED_BRANCH` build (same 10 failures, same detail
codes) -- **zero new failures from the JIT**, the direct question this
gate exists to answer for the roadmap. Unlike §18, this needed no SM64DS
corroboration run: RockWrestler's own dedicated test matrix already
directly exercises the condition-code/LDM-STM/interworking edge cases a
soak could only exercise incidentally.

The 10 failures are pre-existing **interpreter**-level gaps, not a JIT
regression (they reproduce identically with no JIT flags at all) --
tracked here as concrete findings, not fixed in this pass:

- **`SMLALxy` (case 0x000)**: basic `smlalbb r1,r2,r3,r4` (7*18) gives the
  wrong 64-bit accumulate result. Same instruction family §18 already
  flagged as a plausible gap on ARM7 (there: `SMLABB`/`BT`/`TB`/`TT` not
  raising undefined-instruction); this is the accumulate-form failing to
  *compute* correctly on ARM9, suggesting a shared, aged multiply code path.
- **`LDM / STM` (case 0x004)**: `ldmia r1!, {r1}` (writeback register also
  in the load list) doesn't give real hardware's documented
  writeback-wins-over-loaded-value result. Notably, §18's ARM7 interpreter
  baseline got this *same* edge case *right* -- suggesting the ARM7 and
  ARM9 LDM interpreters diverge on this specific case, not just a single
  shared bug.
- **`IPCSYNC` (0x000)**, **`IPCFIFO` (0x001)**, **`IPCFIFO IRQ` (0x000)**:
  the most basic cross-CPU IPCSYNC value handshake and an IPCFIFO
  IRQ-driven wait both time out. `WRAM CNT`/`VRAM CNT` (both `timeout_rw`)
  depend on the same IPCFIFO "rw mode" channel these tests exercise
  directly, so plausibly the same underlying gap cascading downstream --
  not independently confirmed.
- **`DIV 32/32` (case 0x015=21)** / **`DIV 64/32` (case 0x011=17)**: both
  are the same *deliberate* div-by-zero-result-sign-extension test family
  (see the test data's own comments), but the two division modes expect
  *opposite* sign-extension behavior on real hardware and the emulated
  DIVCNT controller doesn't reproduce that mode-dependent asymmetry.
- **`TCM` (case 0x000)**: right at the first check, reading back
  `DTCMcontrol` after setting it doesn't match (masked to the
  architecturally-relevant bits) -- a CP15 TCM-control readback gap.

None of this closes the §25 default-on bar's "RockWrestler passes" line
(that means all 23, not just JIT-parity on the current 10 failures) --
it establishes RockWrestler as a running, byte-diffable gate with a known,
characterized starting baseline for whoever picks up those 10 findings.

Run it: `tools/rockwrestler/build.sh`, stage `out/rockwrestler.nds` as
`sd:/DS/ROMS/test.nds`, boot a `-DDESMUME_ROCKWRESTLER_PROBE
-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=2` build (add
`-DDESMUME_JIT_ARM7 -DJIT_ARM_PRED_BRANCH` to JITDEFS for the JIT path;
omit for the interpreter baseline), pull `sd:/rockwrestler.log`.

---

## 8.5 melonDS reference matrix (roadmap #16)

§8.2's own note ("the same 'write results where the emulator can just read
them' trick a custom melonDS build could use identically") turns out to be
literally true, with one caveat below. Status: **the automation path works;
the actual pass/fail numbers don't yet, for a reason specific to melonDS's
more hardware-accurate Slot-2 bus model** -- documented here rather than
worked around, since it's a real finding, not a harness bug.

### What works

* melonDS already ships the real hardware this project's `ExpMemory` addon
  is a simplified stand-in for -- `GBACart::CartRAMExpansion` (the actual DS
  Browser "Memory Expansion Pak"), selectable via
  `GBAAddon_RAMExpansion` -- so **no new pak needed**, just a way to select
  it headlessly (upstream only exposes it through
  `MainWindow::onInsertGBAAddon`'s menu). Added a `-G/--gba-addon` CLI flag
  to a local melonDS checkout (`~/Desktop/DS/melonDS`, its own separate git
  repo -- not committed there, that tree already carries unrelated
  in-progress local work of its own) -- three small, self-contained diffs to
  `CLI.h`/`CLI.cpp`/`main.cpp`, using `EmuInstance::getEmuThread()`'s
  existing `insertGBAAddon()` (the same call the menu action makes) right
  after `preloadROMs()` boots Slot-1.
* melonDS's Qt frontend runs fully headless under
  `QT_QPA_PLATFORM=offscreen` -- no Dolphin-style `-b` flag needed, no
  virtual framebuffer.
* melonDS already ships a GDB remote-serial stub (`GDBSTUB_ENABLED`,
  `Instance0.Gdb.*` in `melonDS.toml`), one TCP port per core (ARM7/ARM9).
  Since the stub's `ReadMem` goes through the exact same `BusRead32` path
  the CPU itself uses, polling it for the slot-2 sentinel (a plain `m`
  packet) is a drop-in replacement for Dolphin+the SD-card log file: no
  melonDS frontend/threading changes needed at all. A portable config
  directory (`<melonDS>/build/portable/melonDS.toml`) keeps all of this
  off the user's real `~/.config/melonDS`.
* One sharp edge worth recording: **reconnect once and stay connected.**
  The stub only accepts a new client after the previous `ConnFd` cleanly
  disconnects, but a client that opens a fresh TCP connection while an
  older one is still pending in the listen backlog gets serviced *behind*
  it -- so a naive "one connection per read" script intermittently reads a
  stale, never-completed handshake and desyncs the whole stream (`[GDB]
  Received unknown character`). Fix: one persistent connection per melonDS
  process for the whole session, not a reconnect per read.
* Found and fixed a real, hardware-accurate bug in **our own** ROMs while
  bringing this up: neither `armwrestler-arm9.asm` nor `arm7wrestler`'s ARM9
  stub ever touched `EXMEMCNT` bit 7 (which CPU owns the Slot-2 bus).
  desmumewii's `ExpMemory` addon never enforced that arbitration, so it went
  unnoticed; melonDS does enforce it (`NDS::ARM9Read/Write*` and
  `ARM7Read/Write*` both gate on it), and its post-`DirectBoot` default
  leaves the bit granting ARM7, not ARM9, ownership. Fixed by having each
  ROM's ARM9-side driver explicitly claim (`armwrestler-arm9.asm`) or
  confirm (`arm7wrestler`'s ARM9 stub) the bit it needs at the very start of
  `main`. Before this fix, ARM9-driven ROMs (armwrestler, and by extension
  RockWrestler) never got past a permanently-zero slot-2 region under
  melonDS at all.

### What's blocked

With EXMEMCNT fixed, armwrestler boots, runs to completion, and reaches its
terminal spin-loop in well under a second (confirmed via the GDB stub's `g`
register-read command: PC parked on a two-instruction self-loop, same
registers across repeated samples seconds apart) -- but roughly every other
32-bit word of the slot-2 result block still reads back as the pristine
"never written" fill (`CartRAMExpansion::Reset()`'s `memset(RAM, 0xFF,
...)`) instead of what the ROM's own driver wrote, **reproducibly, byte-for-
byte identical across independent runs** (ruled out as a race).

The pattern is internally consistent with *isolated* stores (one `ldr rX,=ADDR
/ str` per call, seconds apart -- `AW_RecordResult`'s counters) landing
correctly, while the initial 512-byte *clear loop*'s tight, back-to-back
`str r2,[r0],#4` sequence mostly doesn't: e.g. the ARM-fail counter
(`+0x08`) never leaves its pristine fill at all (consistent with the clear
loop failing to zero it, and this ROM having zero real ARM failures so
`AW_RecordResult`'s own conditional write to that address never fires
either) as tracked bit-for-bit precisely across two rebuilds where the only
change was inserting `nop`s into the clear loop (which shifted a *stray
pointer-shaped value* landing one word early -- see below -- by exactly the
same few bytes the `nop`s shifted the ROM's code, confirming that value
really is a live code/rodata address, not noise). Adding `nop`s between the
clear loop's stores did **not** fix the underlying drop, ruling out a pure
back-to-back-issue-rate theory as the whole story.

The leading hypothesis, not yet confirmed: our from-scratch `ds_arm9_crt0.S`
(written for §17 to route around a devkitPro/libnds crt0 mismatch, see
`tools/armwrestler/PROVENANCE.md`) sets the CP15 control register to
`0x00042078` -- global D-cache and I-cache both off -- but never touches the
separate CP15 write-buffer/cacheability-by-region registers (c2/c3), which
`DirectBoot` appears to pre-seed with firmware-realistic defaults for all
eight MPU regions (visible in melonDS's own boot log: `PU: region N = ...`).
On ARM946E-S, write buffering is independent of the cache-enable bits;
if `DirectBoot` leaves the Slot-2 region (`PU: region 3 = 08000035`, base
`0x08000000`) marked bufferable, a real ARM9 core's write buffer could
legitimately merge or reorder-relative-to-bus a tight run of stores to nearby
addresses the way we're seeing -- something desmumewii's `ExpMemory` addon,
which has no write-buffer model at all, simply cannot reproduce. Not
confirmed by directly reading back c2/c3 yet; the fix, if this is right, is
either configuring the region as non-bufferable in the crt0 or issuing an
explicit drain between the clear loop and the first read-back.

**Until this is resolved, the melonDS column below is not filled in with
numbers** -- reporting counts from a demonstrably-inconsistent readback
would be worse than leaving it blank. The automation path (CLI flag, GDB
polling, EXMEMCNT fix) is solid and reusable for all three ROMs the moment
the write-consistency issue is understood.

---

# 9. DS reference matrix

Maintain a machine-readable results table:

| Test                     | melonDS                             | DeSmuME interpreter | DeSmuME JIT (`-DJIT_ARM_PRED_BRANCH`) | Hardware/reference | Classification |
| ------------------------ | ------------------------------------ | -------------------- | -------------------------------------- | ------------------- | -------------- |
| armwrestler (ARM9)       | blocked -- see §8.5                  | ARM 0/67, THUMB 1/10 fail | byte-identical to interpreter          |                     | THUMB fail is a documented pipeline-offset quirk (§8.2) |
| arm7wrestler (ARM7)      | blocked -- see §8.5                  | ARM 11/67, THUMB 1/20 fail | byte-identical to interpreter          |                     | 11 ARM fails match documented ARMv4T-vs-ARMv5 differences (§8.3); THUMB fail same quirk as above |
| RockWrestler ARMv4       | not yet attempted                    | 0/1 fail             | byte-identical                         |                     | |
| RockWrestler ARMv5       | not yet attempted                    | 2/11 fail (SMLALxy, LDM/STM base-in-list) | byte-identical            |                     | interpreter multiply/LDM gaps (§8.4) |
| RockWrestler IPC         | not yet attempted                    | 3/3 fail (IPCSYNC, IPCFIFO, IPCFIFO IRQ) | byte-identical             |                     | interpreter gap, not yet root-caused (§8.4) |
| RockWrestler DS MATH     | not yet attempted                    | 2/5 fail (DIV 32/32, DIV 64/32) | byte-identical                  |                     | DIVCNT div-by-zero sign-extension asymmetry (§8.4) -- row added, absent from the original table |
| RockWrestler memory      | not yet attempted                    | 2/2 fail (WRAMCNT, VRAMCNT) | byte-identical                        |                     | plausibly downstream of the IPC gap above, not confirmed (§8.4) |
| RockWrestler TCM         | not yet attempted                    | 1/1 fail             | byte-identical                         |                     | CP15 DTCMcontrol readback gap (§8.4) |
| GBA compatibility tests  |                                       |                      |                                         |                     | not started (§20+) |

DeSmuME JIT is byte-identical to the interpreter on every row above -- this
table's own JIT correctness gate (§25's "ARM9 ARM/THUMB differential tests
pass" / RockWrestler line) reads as "no regressions found", not "clean" --
every interpreter fail listed is a **pre-existing** interpreter-level gap
(§8.2/§8.3/§8.4 have the per-fail detail), reproduced identically with no
JIT flags at all. "Hardware/reference" stays blank throughout: no real DS
hardware or melonDS numbers are available yet (§8.5).

The important invariant is:

```text
DeSmuME JIT
    must match
DeSmuME interpreter
```

and independently:

```text
DeSmuME interpreter
    should converge toward
hardware/reference behavior
```

Do not use another emulator's behavior as an unquestionable specification.

---

# 10. GBA backwards compatibility

Treat native GBA compatibility as a first-class DS architecture workstream.

Do **not** implement it as:

```text
DS emulator
    +
unrelated GBA emulator
```

Instead implement:

```text
Nintendo DS
    |
    +-- normal DS mode
    |      +-- ARM9
    |      +-- ARM7
    |
    +-- native GBA compatibility mode
           +-- ARM7-class CPU
           +-- GBA memory map
           +-- GBA cartridge interface
           +-- GBA peripherals
```

The ARM7 implementation should be shared whenever the architectural behavior is genuinely common.

---

## 11. GBA compatibility reference-first methodology

Before implementing each subsystem:

### Step 1 — DS reference

Run the relevant behavior through melonDS or another established DS implementation when the behavior is DS-side Slot-2/compatibility behavior.

### Step 2 — GBA reference

For actual GBA execution and GBA peripheral behavior, compare against a mature GBA emulator such as mGBA.

### Step 3 — DeSmuME interpreter

Implement and validate the behavior in the interpreter first.

### Step 4 — JIT

Only after the interpreter is correct should the ARM7 JIT be brought into the path.

### Step 5 — hardware evidence

Where references disagree, use:

- GBATEK/hardware documentation
- existing emulator implementations
- test ROM source
- controlled experiments
- original-hardware observations
- commercial software behavior

The goal is to explain discrepancies, not blindly copy whichever emulator produces the expected result.

---

# 12. GBA compatibility architecture

Investigate and implement, as required:

- GBA cartridge detection
- DS compatibility-mode entry
- ARM7 configuration/state
- GBA memory map
- internal WRAM
- external WRAM
- cartridge ROM address space
- save-memory devices
- BIOS behavior
- IRQs
- timers
- DMA
- display/video registers
- sound registers
- keypad/input
- wait states
- reset behavior
- cartridge bus behavior
- relevant timing behavior

Separate:

```text
known hardware requirements
```

from:

```text
emulator-specific approximations
```

so that later hardware discoveries do not require redesigning the entire compatibility layer.

---

# 13. GBA conformance suite

Use the existing GBA conformance ROM collection as a dedicated architectural test suite.

The suite is not merely another JIT benchmark.

It should validate:

- ARM7 CPU semantics
- flags
- instruction edge cases
- memory behavior
- DMA
- timers
- interrupts
- video behavior where covered
- peripheral behavior where covered
- save/cartridge behavior where covered

For every ROM, capture:

```text
reference result
DeSmuME interpreter result
DeSmuME ARM7 JIT result
```

Classify failures into:

```text
CPU
memory
timing
interrupt
DMA
peripheral
cartridge/save
JIT
unknown
```

A JIT failure must never be allowed to masquerade as an underlying GBA hardware-model failure.

---

# 14. The Legend of Zelda: The Minish Cap

Use the user-provided commercial **The Legend of Zelda: The Minish Cap** ROM as an integrated commercial GBA compatibility workload.

Do not redistribute the ROM or place it into the repository.

Use it locally for:

### Boot

- cartridge detection
- reset
- BIOS/entry behavior
- initial ARM7 execution

### Video

- title screen
- menus
- gameplay
- scrolling
- sprites
- transitions

### Audio

- startup audio
- music
- sound effects
- sustained playback

### Input

- buttons
- menus
- gameplay interaction

### Memory/cartridge

- ROM access
- RAM/save behavior
- long-running cartridge access

### Stability

- extended play sessions
- transitions
- save/load
- reset/relaunch

### JIT

Compare:

```text
GBA reference
      ↓
DeSmuME interpreter
      ↓
DeSmuME ARM7 JIT
```

Any JIT discrepancy must first be reproducible against the interpreter before being classified as a GBA compatibility bug.

---

# 15. Three-layer correctness model

Every compatibility result must be classified into three layers.

## Layer 1 — Architectural state

Examples:

- registers
- CPSR
- memory
- DMA state
- timer state
- interrupt state
- peripheral registers

## Layer 2 — Timing and scheduling

Examples:

- CPU cycle counts
- IRQ timing
- DMA timing
- VBlank behavior
- scheduler interleaving
- audio pacing
- wait states

## Layer 3 — Observable behavior

Examples:

- rendered output
- audio
- input response
- save behavior
- game progression
- hangs
- crashes

A test passing Layer 1 does not automatically prove Layers 2 or 3.

---

# 16. Host-memory safety

A JIT must never be considered production-ready merely because the guest-state differential harness passes.

Maintain host-side protections:

- heap canaries
- guard regions where practical
- allocation poisoning/debug builds
- cache-arena bounds checks
- executable-arena validation
- linker-stub validation
- ASan/host instrumentation where available
- repeated stress runs

### Predicated branch compilation — status (§16 re-open)

Predicated `Bcc` **and `BLcc`** (cond ≠ AL) now **compile**, gated behind
`-DJIT_ARM_PRED_BRANCH` (`jit_arm.cpp` `emitBranch`). Default build unchanged —
the flag is the isolation the bailout used to be, not a permanent hide.

`BLcc`'s taken path additionally writes guest R14 = return address. It does so
with a **direct store to the gpr backing slot**, emitted *after*
`emitDirtyRegisterFlush` (which would otherwise re-flush a stale cached R14 over
it) and touching only scratch r11 — so the taken path never mutates the
compile-time register cache and the cond-false fall-through keeps compiling with
its allocator state untouched, exactly like the register-write-free `Bcc` /
THUMB-F16 path. Coverage counters `g_jitPredBcc7` / `g_jitPredBcc9`
(`jit_exec.cpp`, split by `isaLevel`) are surfaced in the ARM7, ARM9 and
differential telemetry lines.

The 2026-09-04 corruption (`1973b69`: a compiled predicated branch eventually
smashed newlib `_malloc_r` state under sustained PH ARM9 execution; never
root-caused) was on the **pre-P12** emitter. That taken path did two
pointer-based host stores — packed CPSR → `*cpsr`, and an `out->instructions`
load/modify/store. P12 removed both (flags resident in r30, trampoline writes
them once; count is `addi r31`). The rebuilt path is shaped byte-for-byte like
the THUMB F16 conditional branch (`jit_thumb.cpp` case 26/27), which has soaked
clean on ARM9 for hundreds of millions of instructions, and has no such stores.

Validation — all on **SM64DS** (~99.99 % ARM mode on ARM9 → heavy
predicated-branch traffic; the sanctioned soak/bench ROM):

- **Differential, `Bcc` only** (`307854c`): 17.4 M `diff9` blocks / 521.7 M ARM
  instructions, **0 mismatches** (guest R[], CPSR N/Z/C/V, PC).
- **Differential, `Bcc` + `BLcc`**: 25.9 M `diff9` blocks / 788.7 M ARM
  instructions, **0 mismatches**. ARM9 is 99 % ARM mode → the `BLcc` taken path
  (guest-R14 write) is exercised heavily.
- **Host-memory instrumented soak, `Bcc` + `BLcc`** (`-DJIT_HEAP_WATCH`:
  buffer-canary poll every 1 K dispatches + after every compile, plus a
  96 × 4 KB scattered heap minefield, round-robin verified; `jit9on`):
  ~96 M ARM9 instructions / 7.97 M blocks / 9.8 M attempts / ~81 K predicated
  compiles, **0 CANARY / 0 HEAP MINE / 0 ARENA OVERRUN / 0 bad-resume**, no
  Broadway exception, arena stable at 716 KB, `arm bail0 = 0`. Earlier
  `Bcc`-only instrumented soaks add ~260 M more instructions, same result.
- **A/B vs bailing** (castle-courtyard bench, `jit9on`): predicated `Bcc`
  cuts ARM9 compile churn ~7× and zero-progress bails ~20× (bailing ends every
  block at the branch → block fragmentation + code-cache thrash). Frame effect
  is large: ARM9 interpreter **13.30** eff.fps → ARM9 JIT with predicated `Bcc`
  bailing **11.20** (a regression, cv 4.1 %) → ARM9 JIT with predicated `Bcc`
  compiled **14.48** (cv 1.1 %). Compiling predicated branches is what turns
  the ARM9 JIT from a −16 % loss into a +9 % win on this scene.

Status:

- **ARM7 gate: cleared (§8.3, roadmap #18).** `arm7wrestler` (the genuinely
  different, ARM7-focused ROM SM64DS's own thin ARM7 coverage couldn't
  substitute for) is now vendored, headless, and run against both an
  interpreter baseline and a `-DJIT_ARM_PRED_BRANCH` build: byte-identical
  results (ARM 11/67, THUMB 1/20, same 12 fail entries) -- **zero new
  failures from compiling predicated `Bcc`/`BLcc` on ARM7**. `emitBranch`'s
  predicated path is byte-identical for both cores (only `cyclesForArm` and
  `isaLevel` differ), and ARM9 was already validated at ~1.3 B instructions
  (below) -- ARM7 now has its own direct CPU-correctness signal rather than
  resting solely on that code-sharing argument. SM64DS's ARM7 side is still
  boot-then-idle (its own differential soak's `predBcc a7` plateaus at 462
  compiles after boot, `diff` never reaching its 100K report threshold) --
  unchanged, and no longer the blocker now that `arm7wrestler` covers it.
- `armwrestler` runs headless (roadmap #17, §8.2) and cleared for ARM9: 0 new
  failures under predicated `Bcc`/`BLcc` beyond the pre-existing, unrelated
  ARM9-JIT ARM `SMLAL` / THUMB `LDR` bugs it found (confirmed by ablation to
  reproduce identically with `-DJIT_ARM_PRED_BRANCH` undefined) -- both now
  root-caused and fixed (§8.2), `armwrestler` back to the clean interpreter
  baseline (ARM 0/67, THUMB 1/10) under a full predicated-branch build.

Both CPU-correctness gates for `-DJIT_ARM_PRED_BRANCH` are now clear. Making
it default-on is a separate decision from clearing its gates -- see §25 for
the full default-on bar (RockWrestler, retail soaks, host-memory hardening,
etc. are still open) rather than treating this flag in isolation.

The `JIT_HEAP_WATCH` instrumentation (`jit_trace.cpp` / `jit_exec.cpp`) is
`#ifdef`-gated, zero-cost when undefined, and stays in as standing §16 tooling.

---

# 17. SMC and TCM invalidation

Any code-bearing memory region must invalidate compiled blocks when guest code changes.

Track at minimum:

- main RAM
- shared WRAM
- ARM7 ERAM where executable
- ARM9 ITCM
- any future GBA compatibility code-bearing region

Invalidate for writes originating from:

- ARM7
- ARM9
- interpreter stores
- JIT stores
- DMA
- cross-CPU shared-memory writes

CP15 TCM remapping/enabling must invalidate affected ARM9 blocks because a cached block may otherwise refer to stale guest bytes.

---

# 18. Differential harness

The differential harness remains a major correctness instrument but is not the sole authority.

Maintain:

- write journal
- rollback
- guest-state comparison
- cycle comparison
- instruction-count comparison
- chained-dispatch verification
- deterministic seeds
- repeatable ROM/test selection

Run differential testing against:

```text
interpreter ↔ JIT
```

for:

- synthetic instruction tests
- armwrestler
- arm7wrestler
- RockWrestler where deterministic comparison is possible
- GBA conformance ROMs
- Minish Cap
- representative DS retail workloads

I/O-heavy or timing-sensitive workloads must also receive non-differential validation because the test harness itself can perturb execution.

---

# 19. Scheduler and cycle quotas

JIT execution must respect the existing DS scheduling model.

Do not allow long native traces to starve:

- ARM7
- ARM9
- DMA
- timers
- interrupts
- graphics
- audio
- IPC

Use a cycle quota or equivalent safe execution boundary.

Measure:

```text
requested guest cycles
executed guest cycles
scheduler overshoot
IRQ latency
DMA latency
```

This becomes particularly important once block chaining and persistent JIT state are introduced.

---

# 20. Workload hierarchy

Use several classes of workloads.

### Deterministic synthetic

`vsd` and dedicated instruction tests.

Purpose:

- repeatable performance
- clean A/B comparisons
- targeted regression testing

### DS CPU/hardware homebrew

- `armwrestler`
- `arm7wrestler`
- RockWrestler

Purpose:

- CPU correctness
- ARM/THUMB correctness
- memory correctness
- IPC correctness
- TCM correctness

### DS retail

Representative retail DS software.

Purpose:

- real instruction mix
- scheduler behavior
- graphics/audio interaction
- long-running stability

### GBA

- GBA conformance suite
- The Legend of Zelda: The Minish Cap

Purpose:

- GBA compatibility
- ARM7 JIT
- cartridge/memory/peripheral behavior
- commercial-software validation

---

# 21. Aging Card NTR — later hardware validation

Add **Aging Card NTR** only after the core DS hardware model and conformance infrastructure are mature.

It should not be an early JIT gate.

The reason is methodological: first establish correctness using the CPU-focused tests, RockWrestler, emulator-reference comparisons, and commercial workloads. Then use Aging Card NTR as a later hardware/debug-oriented validation target where its tests can provide information that ordinary homebrew and retail software do not.

The sequence should therefore be:

```text
armwrestler
    ↓
arm7wrestler
    ↓
RockWrestler
    ↓
DS reference comparison
    ↓
GBA conformance
    ↓
commercial GBA validation
    ↓
Aging Card NTR
    ↓
hardware-specific refinement
```

Aging Card NTR results should be recorded separately from ordinary emulator compatibility results and investigated against known hardware behavior rather than automatically treated as an absolute specification.

---

# 22. Performance methodology

Every performance comparison should report:

- build configuration
- JIT enabled/disabled
- ARM7 JIT enabled/disabled
- ARM9 JIT enabled/disabled
- resolution/rendering configuration
- scene/test ROM
- warm-up policy
- measurement duration
- frames or guest instructions
- JIT compilation time
- JIT execution time
- interpreter execution time
- dispatch count
- average block length
- chain length
- memory fast-path hit rate

Report both:

```text
whole-emulator frame time
```

and:

```text
CPU/JIT execution time
```

A JIT that improves CPU time but fails to improve frame time is not automatically a failure; it may indicate another bottleneck.

---

# 23. Escalation policy

Do not redesign the JIT prematurely.

### First

Optimize:

1. block length
2. static chaining
3. dynamic chaining
4. persistent guest state
5. trampoline overhead
6. memory fast paths

### Then

Measure again.

### Only if necessary

Consider:

- more aggressive register allocation
- larger traces
- trace stitching
- limited IR
- specialization
- more advanced host scheduling

The default architecture remains direct emission plus chaining.

---

# 24. Final roadmap

| #  | Phase                                                            | Status / target |
| -- | ---------------------------------------------------------------- | --------------- |
| 1  | Vendor shared JIT core                                           | done            |
| 2  | CPU-agnostic seam + ARM7 profile                                 | done            |
| 3  | THUMB front-end + synthetic differential vectors                 | done            |
| 4  | ARM7 live execution + SMC/DMA invalidation                       | done            |
| 5  | Differential-harness hardening                                   | done            |
| 6  | Dual-core split + ARM9 profile + CP15 integration                | done            |
| 7  | ARM9 THUMB bring-up                                              | done            |
| 8  | Benchmark gate 1                                                 | done            |
| 9  | ARM32 front-end on ARM9                                          | done            |
| 10 | Static/dynamic block chaining + scheduler quota                  | done            |
| 11 | ARM front-end on ARM7                                            | done (SM64DS soak; armwrestler + arm7wrestler both clear, §8.2/§8.3) |
| 12 | Persistent JIT state + trampoline amortization                   | done: r31 icount + r30 CPSR resident; GPR residency deferred (§5/§23) |
| 13 | Inline memory fast paths                                         | Tier-1 literal loads landed; general/WRAM tier deferred (§6/§23) |
| 14 | Cached page descriptors                                          | done: ARM7 RAM-window descriptor table + inline single loads; correctness-validated, frame-neutral on SM64DS (§6) |
| 15 | LDM/STM and sequential memory optimization                       | done: inline LDM/LDMIA/POP (loads); STM/PUSH + LDM{pc} deferred (§6/§16); correctness-validated, frame-neutral |
| 16 | DS CPU reference matrix: melonDS + DeSmuME interpreter           | in progress: §9 table filled in for DeSmuME interpreter/JIT (all 3 ROMs, byte-identical JIT results); melonDS column blocked on a write-consistency issue under melonDS's Slot-2 bus model, see §8.5 -- CLI hook + GDB-stub polling harness proven working once that's resolved |
| 17 | `armwrestler` automated regression gate                          | done: headless via slot-2 I/O (§8.2); found + fixed an 8MB-addon/JIT-arena OOM hang; found + fixed pre-existing ARM9 JIT bugs (SMLAL missing carry, THUMB LDR missing unaligned rotate) -- back to clean baseline (ARM 0/67, THUMB 1/10) |
| 18 | `arm7wrestler` automated regression gate                         | done: headless via slot-2 I/O (§8.3), same technique as #17 -- interpreter baseline ARM 11/67 fail (matches documented ARMv4T-vs-ARMv5 differences) / THUMB 1/20 fail; `-DJIT_ARM_PRED_BRANCH` build byte-identical, 0 new failures -- ARM7 predicated-branch gate cleared |
| 19 | RockWrestler automated DS conformance gate                       | done: headless via slot-2 I/O (§8.4), no crt0 workaround needed (upstream is `-nostartfiles`) -- interpreter baseline 10/23 fail (SMLALxy, LDM/STM base-in-list, IPCSYNC/IPCFIFO/IPCFIFO IRQ, DIV 32/32 + 64/32 sign-extension, TCM/CP15 readback -- all pre-existing interpreter gaps, characterized in §8.4); `-DJIT_ARM_PRED_BRANCH` build byte-identical, 0 new failures |
| 20 | GBA compatibility architecture                                   | next            |
| 21 | GBA reference baseline: DS-side melonDS + GBA reference emulator | next            |
| 22 | GBA memory/cartridge/BIOS/peripheral implementation              | next            |
| 23 | GBA conformance harness                                          | next            |
| 24 | ARM7 JIT vs GBA conformance                                      | next            |
| 25 | The Legend of Zelda: The Minish Cap validation                   | next            |
| 26 | Host-memory safety hardening                                     | next            |
| 27 | ARM9 correctness and retail DS soak                              | next            |
| 28 | Aging Card NTR hardware-validation phase                         | later           |
| 29 | Benchmark gate 2                                                 | next            |
| 30 | Broadway hardware validation                                     | next            |
| 31 | Default-on ARM9 JIT                                              | final           |

---

# 25. Default-on gate

`jitArm9Enabled` becomes default-on only when all of the following are true:

### Correctness

- ARM9 ARM/THUMB differential tests pass
- RockWrestler passes
- retail DS soaks show no unexplained corruption
- host-memory safety tests show no corruption
- SMC/TCM invalidation is validated
- no unresolved JIT correctness blocker remains

### Performance

- JIT beats the interpreter on representative retail workloads
- gains are measured with realistic Broadway conditions
- chaining and memory fast paths demonstrate sustained benefit
- compilation overhead is acceptable

### GBA / ARM7

- ARM7 JIT passes CPU conformance
- GBA compatibility interpreter path is stable
- GBA conformance suite has no unexplained architectural failures
- Minish Cap has stable boot, graphics, audio, input, save, and long-run behavior
- JIT does not introduce discrepancies relative to the interpreter

### Hardware confidence

- DS homebrew suite has been compared against melonDS/reference implementations
- known discrepancies have classifications
- Aging Card NTR has been investigated where relevant
- no unexplained hardware-model divergence blocks release confidence

---

# 26. Definition of success

The project succeeds when:

1. **One shared JIT engine** efficiently executes both DS ARM CPUs.
2. **ARM7 JIT** is reliable enough to support normal DS workloads and GBA compatibility.
3. **ARM9 JIT** provides a measured performance improvement on Broadway.
4. **RockWrestler, armwrestler, and arm7wrestler** provide automated DS CPU/hardware regression coverage.
5. **GBA conformance ROMs** provide systematic GBA architectural coverage.
6. **The Minish Cap** provides a realistic commercial GBA workload.
7. **melonDS and other established emulators** provide useful initial reference results without being treated as the ultimate specification.
8. **Aging Card NTR** provides a later hardware-oriented validation layer.
9. The interpreter remains a trustworthy fallback and reference implementation.
10. No known host-memory corruption or unresolved architectural JIT defect remains when JIT is enabled by default.

---

# Guiding principle

**First establish the reference. Then establish interpreter correctness. Then make the JIT match the interpreter. Then optimize.**

For DS behavior:

```text
melonDS / established DS reference
              ↓
      DeSmuME interpreter
              ↓
        DeSmuME JIT
```

For actual GBA execution:

```text
established GBA reference
              ↓
      DeSmuME GBA mode
              ↓
        ARM7 JIT
```

For hardware-specific questions:

```text
documentation
     +
test ROMs
     +
established emulators
     +
commercial software
     +
hardware-originated tests
              ↓
      best-supported model
```

The JIT should never be used to hide an architectural emulator bug, and another emulator should never be treated as the hardware specification merely because it passes a test. The project should converge from reproducible reference behavior toward the best-supported model of the original hardware.
