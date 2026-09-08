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
register allocation (out of scope) or a fixed guest→host mapping. A fixed
mapping's unconditional 15-register trampoline load/store only pays off once
chains are long enough to amortise it.

**Re-measured after predicated branches locked on (`94df464`, SM64DS
castle-courtyard, `DESMUME_JIT_TRACE_FIRST` a9 tally + ARM7 `alive:` lines):**

| core | guest-instrs / trampoline round-trip | blocks / round-trip | bail0 rate | compile churn |
| ---- | ----------------------------------- | ------------------- | ---------- | ------------- |
| ARM9 | ~15                                 | ~1.9                | ~0 %       | 0.35 %        |
| ARM7 | ~12 (per *successful* entry)        | —                   | ~19 %      | — |

Chains are **still ~1.9 blocks** even with predicated `Bcc`/`BLcc` compiling and
near-zero compile/bail churn. The limiter is **not** the cycle quota
(`JIT_YIELD_NUMBER`): raising it 64 → 128 was byte-identical on the benchmark
(ARM9 JIT 14.43, jitfull 36.17 both unchanged).

**Ceiling found — predicated-op coverage, not dispatch (`8f014e3`).** New
`DESMUME_JIT_TRACE_FIRST` telemetry (`jit_exec.cpp` classifies every non-bail
chain-end round-trip by what sits in the resume PC's block-table slot;
`jit_trace.cpp` histograms the opcodes that hit the "nothing compilable"
path). SM64DS castle-courtyard:

- **~64 %** of ARM9 JIT dispatches are non-bail trampoline round-trips.
- **99 %+** of those resume onto a **"don't JIT" marker** (slot has the right
  PC but `execute == null`, `len == 1`) — the emitter refused an opcode there,
  so it is interpreted one instruction at a time with a full C round-trip each
  visit. **Not** collisions (`other` slot ≈ 0.6 %), **not** SMC (11 kills all
  run), **not** the quota. The `emitDynamicLinkerStub` direct-mapped slot is
  **not** the bottleneck — a set-associative table / return-address stack would
  buy almost nothing.
- The refused opcodes are almost entirely **predicated (`cond != AL`) memory
  ops**: `BXcc lr`, `STRcc`/`LDRcc`/`LDRBcc`/`STRHcc`, `POPcc`/`PUSHcc`
  (`LDMcc`/`STMcc`), then a tail of genuinely-hard ones (`MCR p15` cache
  maintenance, `MSR cpsr`).

ARM data-processing already compiles predicated (`emitEvalCond` + conditional
skip, `jit_arm.cpp`); §16 did predicated `Bcc`/`BLcc`. **Next lever: extend
that pattern to the predicated non-branch memory ops** — `STRcc`/`LDRcc`/
`LDRBcc`/`STRHcc`, `POPcc`/`PUSHcc` (`LDMcc`/`STMcc`). The block continues past
them with no exit at all (the true `Bcc`-not-taken-style win): removes the
per-instruction interpreter round-trip *and* lengthens chains.

**`BXcc` tried and reverted (`c375880` → `5d00119`).** Compiling predicated
`BX` as taken-dynamic-exit + fall-through was differential-clean (1.6 B ARM9
insns) and the telemetry looked spectacular — chains **doubled** (blk0/entry
1.8 → 4.0, ins/entry 17 → 28), round-trips `edge` 64 % → 0 % — but SM64DS
**hung** in free-run (frame rate fell, `compiles` froze at ~1100: looping on a
small compiled block set), a hang the differential harness's per-block rollback
masks. Root cause: the taken path's `emitDynamicExit()` calls the *clearing*
`flushDirtyRegisters()`, but the emitted stores are branched over on the
cond-false path, so a guest reg written before the `BXcc` and spilled later in
the block loses its value. `emitBranch()`'s predicated path sidesteps this by
inlining a *non-clearing* `emitDirtyRegisterFlush()` and never calling
`emitDynamicExit`. Any predicated op whose body runs a real state flush must
use the `bailPreservingDirty()` save/restore idiom (`jit_trace.cpp`).
Separately, `BXcc lr` is a conditional *return* — its taken path is a guarded
dynamic-dispatch exit (polymorphic return address, no self-patch), no cheaper
than the interpreter round-trip it replaces — so the non-branch ops are the
better target regardless. Predicated `BX` can be revisited with the dirty-fix
once the memory ops land.

At a ~1.9-block chain a fixed-mapping trampoline's unconditional 15-register
load/store still loses to the lazy allocator — **GPR residency stays deferred
until chains lengthen.**

**Measured picture:** on SM64DS the ARM9 JIT is now a **+9.6 % whole-frame win**
(§16); the ARM7-only JIT A/B still needs re-measuring (was a ~4.4 % regression
pre-predicated-branch). The dominant remaining cost is the number of trampoline
round-trips (short chains → the predicated-op ceiling above) plus the
trampoline's own `stmw`/`lmw`.

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
`cpu.isaLevel`. Predicated ARM `Bcc` and `BLcc` **compile unconditionally**
(ARM9-validated at ~1.3 B instructions — §16). This was behind
`-DJIT_ARM_PRED_BRANCH` while it was proven out; the flag was removed once both
CPU-correctness gates cleared (armwrestler + arm7wrestler, roadmap #17/#18,
§8.2/§8.3 — 0 new failures on either). The broader §25 default-on bar is about
`jitArm9Enabled` as a whole, not this codegen path.

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

**JIT build** (`-DDESMUME_JIT_ARM7`, then still with the isolation flag
`-DJIT_ARM_PRED_BRANCH` that this gate cleared and that has since been
removed; ARM9 JIT off -- this ROM's ARM9 side is upstream's own trivial
idle/vram-copy stub):
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
`-DDESMUME_JIT_ARM7` to JITDEFS for the JIT path -- predicated `Bcc`/`BLcc`
compile unconditionally now, no separate flag; omit all JIT flags for the
interpreter baseline), pull `sd:/arm7wrestler.log`.

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
`-DDESMUME_JIT_ARM7` to JITDEFS for the JIT path -- predicated branches
compile unconditionally now; omit for the interpreter baseline), pull
`sd:/rockwrestler.log`.

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

### What's blocked -- root-caused to a melonDS bug, not ours

With EXMEMCNT fixed, armwrestler boots, runs to completion, and reaches its
terminal spin-loop in well under a second -- but roughly every other 32-bit
word of the slot-2 result block reads back as the pristine "never written"
fill (`CartRAMExpansion::Reset()`'s `memset(RAM, 0xFF, ...)`) instead of what
the ROM's own driver wrote, **reproducibly, byte-for-byte identical across
independent runs**. Three plausible causes on *our* side were tested and
falsified in turn:

* **melonDS's own write path is unconditional and address-independent** --
  read directly: `NDS::ARM9Write32`'s slot-2 case splits a 32-bit store into
  two `GBACartSlot::ROMWrite()` calls (`addr`, `addr+2`), which is a bare
  pass-through to `CartRAMExpansion::ROMWrite()` -- no masking, no timing
  gate, nothing that could plausibly drop specific words.
* **EXMEMCNT flipping back mid-run** (ARM7 silently reclaiming the bus) --
  read directly via the GDB stub after the ROM had already parked on its
  spin-loop: `0x0000`, bit 7 still clear. Stable throughout, not the cause.
* **CP15 write-buffer/cacheability (c2/c3) theory** -- tested by adding an
  explicit `mcr p15,0,r0,c7,c10,4` (drain write buffer) immediately before
  the sentinel write and rebuilding. Made **zero difference** to the
  corruption pattern; only a stray pointer-shaped value in the dump shifted
  by exactly the instruction's byte size (the same signature an earlier
  `nop`-spacing experiment had already produced), confirming that value is a
  live code/rodata address, not evidence of buffering. Falsified.
* **melonDS's own memory/GDB-stub path itself** -- isolated by writing a
  128-word sequential pattern directly into the same slot-2 region via the
  GDB stub's `M` command (bypassing the ARM9 CPU and our ROM entirely), then
  reading it back the same way: **0/128 mismatches**. The storage and
  read/write plumbing are provably correct; the bug only appears when the
  *CPU* is the one issuing the writes.

That last result pointed the investigation at CPU-instruction execution
itself, so it was captured directly rather than inferred further: extended
the GDB client with breakpoint (`Z0`/`z0`) and single-step (`s`) support, set
a breakpoint at `aw_clear_loop`'s first instruction (address read from the
build's linked ELF, `arm-none-eabi-nm out/a9.out`), and single-stepped
through the loop one instruction at a time, reading `r0`/PC and re-reading
the just-written address after every step. (A temporary go-flag spin-wait at
the top of `main`, released by a GDB memory write once attached, was used to
synchronize with zero race against Qt/addon startup timing -- removed again
once the trace was captured; not part of the committed ROM.) The trace is
unambiguous:

```
before step 0: PC=0x20049fc r0=0x9000000   (str)
before step 1: PC=0x2004a00 r0=0x9000004   (cmp)      -- step 0 executed correctly
before step 2: PC=0x2004a04 r0=0x9000004   (blt)      -- step 1 executed correctly
before step 3: PC=0x20049fc r0=0x9000004   (str)      -- step 2 executed correctly (branch taken)
before step 4: PC=0x20049fc r0=0x9000004   (str, again) -- step 3: PC/r0 UNCHANGED. No-op.
before step 5: PC=0x2004a00 r0=0x9000008   (cmp)      -- step 4 executed correctly this time
...
before step 9: PC=0x2004a00 r0=0x900000c   (cmp, again) -- step 8: also a no-op
```

Roughly every third or fourth `s` command returns a normal stop-reply
(`S05`) while leaving PC and every register completely unchanged -- the stub
reports a step happened when it didn't. This isn't tied to a particular
instruction (`str`, `cmp`, and `blt` have all been caught no-opping) and it
isn't every-other -- the gap between dropped steps varies (3, then 4 in the
trace above), which matches the *non-uniform* word-loss pattern in the
continuous-execution dump far better than a clean alternating theory would.
**This is a melonDS bug** -- most likely a race between the GDB stub's
step/continue signaling and its emulation-thread scheduler, given the
symptom (an acknowledged step that doesn't advance the target at all) rather
than anything a CP15/cache/write-buffer model would produce. It is not
something in our ROM, our EXMEMCNT fix, or our crt0 to work around.

**Until melonDS fixes this (or someone chases the threading race further
upstream), the melonDS column below is not filled in with numbers** --
reporting counts from a target that demonstrably drops instruction effects
mid-run would be worse than leaving it blank. The automation path (CLI flag,
GDB polling, breakpoint/single-step support, EXMEMCNT fix) is solid and
fully reusable for all three ROMs the moment melonDS's side is fixed; this
finding is also independently worth reporting upstream.

---

# 9. DS reference matrix

Maintain a machine-readable results table:

| Test                     | melonDS                             | DeSmuME interpreter | DeSmuME JIT (predicated branches compiled) | Hardware/reference | Classification |
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

## 12.1 Starting-point audit (roadmap #20)

Before any design work, a full source audit (three parallel passes over
`source/src`, not the prose below) established the actual starting point,
since none of this had been verified against the real codebase before:

- **Zero native-GBA-execution scaffolding exists anywhere.**
  `source/src/addons/gbagame.cpp` is *slot-2 SRAM/Flash backup-memory*
  emulation for DS games that use a GBA cart for save transfer (Pokémon-style
  connectivity) -- not GBA CPU/PPU/APU execution. No GBA memory map, no GBA
  BIOS, no "halt ARM9, run ARM7 alone" mechanism, no GBA header check, no
  `GBA_MODE` flag of any kind exists.
- **Loading a real `.gba` file was actively unsafe, not just unsupported --
  now fixed.** `NDS_LoadROM`'s only type check was `path.isdsgba()`
  (`source/src/path.h:367`), which just checks the filename ends in
  `"ds.gba"` -- a flashcart *multiboot-wrapped-NDS-ROM* convention, not a
  real GBA header check. A genuine GBA ROM fell through and got parsed as a
  DS header. `DetectRomType`'s "invalid header" guard
  (`source/src/utils/decrypt/header.cpp`) was dead code (`header.unitcode`
  is `unsigned char`, so `< 0` could never be true), so nothing rejected it,
  and the header offset that decides ARM9/ARM7 copy sizes/addresses landed
  inside the GBA logo bitmap -- garbage in, garbage out. Two real bugs, both
  fixed this pass (§12.2).
- **The JIT already has the right seam.** `JitCpuProfile`
  (`source/src/jit/jit_cpu_profile.h`) is function-pointer-based, published
  into a runtime-indexed `jitProfile[2]` array, and the shared emitters
  (`jit_arm.cpp`/`jit_thumb.cpp`) never hardcode DS address ranges -- they
  only call through `ctx.cpu.slowRead/slowWrite/mainMemBase/...`. A new
  `jit_arm7gba_profile.cpp` returning a GBA-backed profile would plug into
  `jitBuildArm7Profile()`'s call site with no emitter changes.
- **The interpreter's memory path is not actually behind a live abstraction
  in this build.** `armcpu_memory_iface`/`mem_if` exists in `armcpu.h` but
  only under `#ifdef GDB_STUB`, which is never defined -- dead code.
  `READ32`/`WRITE32` (`MMU.h`) compile straight to `_MMU_read/write32<PROCNUM>`,
  and `PROCNUM` is a compile-time 0/1 template parameter baked in pervasively
  as `PROCNUM ? ARM7 : ARM9` (`MMU.h`, and throughout `MMU.cpp`/`armcpu.cpp`).
  Reusing the interpreter for GBA mode needs a real seam added -- cheapest is
  a runtime mode-flag branch inside the existing `_MMU_ARM7_read*/write*`
  functions (`MMU.cpp`), mirroring the early-out pattern those functions
  already use for the slot-2 `addon.read/write` vtable.
- **Slot-1 cartridge state is one hardcoded global, not addon-shaped.**
  `GameInfo gameInfo` / `NDS_header` (`NDSSystem.h`) is a fixed
  DS-header-shaped struct consumed by name throughout
  `NDSSystem.cpp`/`MMU.cpp`/`cheatSystem.cpp`/`movie.cpp`. Unlike slot-2
  (`ADDONINTERFACE`, a genuinely reusable vtable), there is no interface to
  extend here -- a GBA cart needs a parallel loading/reset path, gated by a
  new mode flag at the small number of slot-1 entry points, not a vtable
  swap.

A *complete* native GBA-compatibility mode (full memory map, PPU, APU,
timers, DMA, BIOS, save types, JIT + interpreter parity) is a multi-week
subsystem, not something to half-build speculatively. §12.2 covers what
landed this pass; §12.3 is the concrete, sequenced remainder.

## 12.2 Landed this pass: safe detection, not a crash

`NDS_LoadROM` -> `DecryptSecureArea` -> `decrypt_arm9` had a real,
independent-of-GBA-work memory-safety bug: `DecryptSecureArea`
(`utils/decrypt/decrypt.cpp`) copies a `0x4000`-byte window starting at file
offset `0x4000` out of the ROM-header scratch buffer, but that buffer
(`SMALL_READ`, `NDSSystem.h`) was only `1024*20 = 0x5000` bytes -- so the
copy's source range ran to offset `0x8000`, a 12 KB heap over-read past the
end of the allocation. This wasn't GBA-specific: it fires on essentially
every ROM classified `ROMTYPE_ENCRSECURE`/`ROMTYPE_MASKROM`, i.e. most
normal encrypted-secure-area loads, not just malformed input. Confirmed with
an isolated ASan host build exercising the real `decrypt.cpp`/`header.cpp`
unmodified: `heap-buffer-overflow ... READ of size 16384 ... 0 bytes after
20480-byte region`, pinned to the exact `memcpy` line. Fixed by sizing
`SMALL_READ` to `0x8000` (32 KB) -- the buffer now actually covers what the
existing code already assumed it covered; re-run of the same ASan harness
after the fix shows no report.

On top of that: `DetectRomType` now does a real GBA-vs-DS discrimination.
GBA cartridge headers carry a fixed magic byte (GBATEK: offset `0xB2` = 
`0x96`) that lands inside the DS header's *reserved* region (before the
Nintendo-logo bitmap at `0xC0`), so it reliably tells a genuine GBA ROM
apart from a real DS dump with no false positives. A new `ROMTYPE_GBA`
classification is checked first in `DetectRomType`
(`utils/decrypt/header.cpp`) and handled in `DecryptSecureArea`
(`utils/decrypt/decrypt.cpp`) exactly like the (now-actually-reachable)
`ROMTYPE_INVALID` case: a clean, logged rejection (`NDS_LoadROM` already had
an `if(!okRom) { ...; return -1; }` bail-out other callers check -- this is
a real, non-silent failure path, not new plumbing) instead of proceeding
into DS-shaped decrypt/copy logic with a GBA-shaped header. The previously
dead `unitcode`-range guard was fixed at the same time (was `< 0 && > 3` on
an `unsigned char`, i.e. never true; now correctly `> 3`).

Verified: a standalone ASan/UBSan host build linking the actual
`header.cpp`/`decrypt.cpp`/`crc.cpp` unmodified (no reimplementation)
against (a) a real DS ROM's actual header bytes (`armwrestler.nds`) --
classification and decrypt path unaffected, (b) a synthetic GBA-header blob
-- classified `ROMTYPE_GBA`, rejected cleanly, (c) a synthetic invalid
blob -- classified `ROMTYPE_INVALID`, rejected cleanly, (d) a
`MASKROM`-shaped blob at the fixed `SMALL_READ` size -- classification
unchanged, decrypt path runs with no ASan report. The Wii `.dol` build
(`make`, devkitPPC) also compiles clean with these changes. No GBA
execution, memory map, BIOS, or ARM9-halt mechanism was implemented this
pass -- see §12.3.

## 12.3 Remaining sequenced work

In dependency order (each is its own scoped pass, reference-first per §11
against melonDS's GBA mode and mGBA where hardware behavior is in
question):

1. **Boot-mode gate -- landed (partial).** `GameInfo::isGBA`
   (`NDSSystem.h`), default `false`, is now the flag `NDS_Reset()`/`NDS_exec()`
   consult. **Not yet wired to real detection**: `NDS_LoadROM` still hard-
   rejects `ROMTYPE_GBA` exactly as before (§12.2) -- nothing sets
   `gameInfo.isGBA` from a real ROM load yet, deliberately, since accepting a
   `.gba` file before step 3 (memory map) and step 6 (BIOS) exist would just
   run ARM7 against the DS memory map, not a functional step. A test-only
   `NDS_DebugForceGBAMode(bool)` (`NDSSystem.cpp`) sets the flag directly so
   the mechanism below is exercisable ahead of that wiring.
2. **ARM9-halt mechanism -- landed.** `NDS_exec`'s one `armInnerLoop` call
   site (`NDSSystem.cpp`, was always `armInnerLoop<true,true>`) now branches
   on `gameInfo.isGBA` to `armInnerLoop<false,true>` -- reusing the *existing*
   template rather than adding a new runtime check inside the hot loop:
   `armInnerLoop<doarm9,false>` (ARM7-off) was already instantiated as a
   mid-loop fallback (ARM7 IRQ-wait), so `<false,true>` (ARM9-off) is the
   same generic template, just a combination nothing had used yet. Because
   `doarm9`/`doarm7` are compile-time template parameters, `<false,true>`
   doesn't just skip ARM9 at runtime -- the entire branch containing
   `armcpu_exec<ARMCPU_ARM9>()`/`jitRunArm9()` is eliminated from that
   instantiation's machine code, which also means both the interpreter *and*
   JIT paths are covered by this one change (JIT dispatch is nested inside
   the same `if(doarm9 && ...)` block, not beside it). `NDS_Reset()`'s three
   boot-mode branches (patched-firmware/firmware/direct) each guard their
   ARM9-only work (secure-area copy, `armcpu_init(&NDS_ARM9,...)`, direct-
   boot ARM9 binary copy loop, ARM9 `REG_POSTFLG` write, ARM9 stack-pointer
   setup) behind `!gameInfo.isGBA`; ARM7's side of each stays unconditional
   and unchanged. ARM7 does not yet boot at a *meaningful* GBA vector in
   `isGBA` mode -- that needs steps 3+6 -- this step only proves ARM9 can be
   kept fully out of the boot/execution path.

   Verified: (a) `gameInfo.isGBA` defaults `false` and nothing in any real
   ROM-load path sets it, so every current code path is the pre-existing
   code verbatim -- a diff-reviewable no-op for every real ROM today; (b) a
   standalone host-native replica of `armInnerLoop`/`minarmtime` (can't link
   the real `static` template without the whole engine, so this copies the
   two template bodies verbatim and stubs the ~8 externals they touch, with
   call-counting fakes) run under ASan/UBSan: `<true,true>` calls both
   ARM9/ARM7 stand-ins; `<false,true>` calls ARM7 only, across a plain run,
   an ARM7-`waitIRQ` run that exercises the mid-loop recursion into
   `<false,false>`, and an ARM9-`waitIRQ`-set-anyway run -- ARM9 stand-in
   call count is 0 in all three; (c) full devkitPPC Wii `.dol` build
   compiles clean. Not exercised: `NDS_DebugForceGBAMode(true)` against real
   Dolphin/hardware -- Dolphin here is GUI-only, and there's nothing GBA-
   meaningful for ARM7 to run yet regardless (steps 3-6).
3. **GBA memory-map allocation -- landed.** Six flat `GBA_`-prefixed backing
   buffers added to `MMU_struct` (`MMU.h`): `GBA_BIOS` (16 KB), `GBA_EWRAM`
   (256 KB), `GBA_IWRAM` (32 KB), `GBA_PALETTE` (1 KB), `GBA_VRAM` (96 KB),
   `GBA_OAM` (1 KB) -- embedded fixed-size arrays (like `ARM7_BIOS`/
   `ARM7_ERAM`/`SWIRAM`), not heap-allocated, so `MMU_Init()`'s whole-struct
   `memset` zeroes them for free; `MMU_Reset()` now also re-zeroes each by
   name (`MMU.cpp`) so they stay reset-safe once live. Distinct from and not
   reusing the DS `MMU.MAIN_MEM`/`SWIRAM`/`ARM7_ERAM` buffers despite
   overlapping address *values*. Allocation only -- no address decode, no
   read/write functions, no mirroring rules, no BIOS non-fetch read
   protection, no savestate `SFORMAT` entries, no cartridge ROM/SRAM
   (peripherals, step 7): all deferred to step 4, which is where §7.1's
   `_MMU_ARM7_read/write*`-as-wholly-separate-functions precedent actually
   gets applied (a parallel `_MMU_ARM7GBA_read/write*` set, not a third
   `PROCNUM` value threaded through the pervasive `PROCNUM ? ARM7 : ARM9`
   idiom -- confirmed concretely this pass: PROCNUM is a compile-time
   template parameter from `armcpu_exec<0>/<1>` down through the opcode
   tables to the `READ`/`WRITE` macros, so a GBA backend must be ARM7's
   existing path behaving differently under `gameInfo.isGBA`, not a third
   PROCNUM).

   Verified: diff is purely additive (six new struct members, six new
   `memset` lines, nothing existing altered) -- a materially simpler, more
   directly diff-reviewable change than steps 1-2, not needing their
   ASan-replica-harness methodology; full devkitPPC Wii `.dol` build
   compiles clean, confirming no collision with `MMU.h`'s packed-struct
   block (new members sit outside it) and that the ~402 KB total addition
   is a non-issue against the existing multi-MB DS buffer budget. Not
   exercised: `NDS_DebugForceGBAMode(true)` still can't touch these buffers
   end-to-end this pass -- expected, since nothing reads/writes them until
   step 4.
4. **Interpreter memory-backend seam -- landed.** The GBA-side of the mode-
   flag branch described in §12.1. `MMU.h`'s six `_MMU_read/write08/16/32`
   FORCEINLINE dispatchers (the true hot path -- confirmed by tracing the
   `READ8`/`WRITE8`/etc. macros and the template overloads down to these
   exact functions, not assumed) now check `PROCNUM==ARMCPU_ARM7 &&
   MMU.isGBA` as their very first statement and, when true, hand off to a
   new parallel `_MMU_ARM7GBA_read/write08/16/32` function set (`MMU.cpp`,
   declared in `MMU.h` next to `_MMU_ARM7_*`) -- placed first because GBA's
   EWRAM (`0x02000000`)/IWRAM (`0x03000000`) address ranges alias existing
   inline DS ARM7 fast paths already living in these same dispatchers
   (`MAIN_MEM`/`ARM7_ERAM`/`SWIRAM`), so checking after them would silently
   misroute GBA reads/writes into DS buffers. `MMU.isGBA` is a plain `bool`
   on `MMU_struct`, added specifically because `NDSSystem.h` includes
   `MMU.h`, so `MMU.h` cannot include `NDSSystem.h` back to see
   `gameInfo.isGBA` -- it's a synchronized mirror, not a second source of
   truth (`gameInfo.isGBA` stays authoritative), kept in sync by
   `NDS_DebugForceGBAMode()` (`NDSSystem.cpp`).

   The actual address decode is one shared pure function,
   `gbaDecodeAddr()` (`MMU.cpp`) -- a deliberate, noted deviation from
   §7.1's "wholly separate functions" precedent (that precedent is about
   ARM9-vs-ARM7 being genuinely different address spaces, not an argument
   for re-deriving identical mirror math six times within one CPU's own
   map) -- covering BIOS (16 KB, `0x00000000-0x00003FFF`, not mirrored),
   EWRAM (mirrors every `0x40000` across the `0x02` bank), IWRAM (mirrors
   every `0x8000` across `0x03`), Palette (mirrors every `0x400` across
   `0x05`), OAM (mirrors every `0x400` across `0x07`), and VRAM (mirrors
   every `0x20000`/128 KB across `0x06`, with the documented GBATEK quirk
   that each period's last 32 KB re-mirrors its own previous 32 KB instead
   of reading blank). Everything else (I/O `0x04000000`, cartridge ROM
   `0x08000000+`, SRAM `0x0E000000`) decodes to an explicit "unmapped"
   case: reads return 0, writes are no-ops -- a placeholder, not real
   behavior, deferred to step 7. BIOS writes are dropped (real BIOS is
   ROM). **Not implemented this pass, a known simplification**: real
   hardware's BIOS read-protection-when-PC-isn't-in-BIOS quirk (returns the
   last-fetched BIOS opcode instead of real BIOS content) -- flagged
   explicitly rather than silently approximated, per this section's own
   "known hardware requirements vs. emulator-specific approximations"
   principle.

   Verified: a host-native unit test of `gbaDecodeAddr()` alone (copied
   verbatim -- it's a pure function, touches no globals, so unlike steps
   1-2's replica test this needed zero stubbing) drives every region's low/
   high/mirror-wrap boundary under ASan/UBSan, 30/30 passing, including the
   VRAM quirk-mirror edges and every unmapped gap; diff review confirms the
   six dispatcher guards are inserted before, not after, the DS checks they
   must preempt, and that `MMU.isGBA` defaulting `false` leaves every
   current DS code path verbatim-unchanged; full devkitPPC Wii `.dol` build
   compiles clean. Not exercised: still no real GBA ROM boots end-to-end
   this pass -- no `ROMTYPE_GBA` wiring, no BIOS, no JIT profile yet.
5. **JIT `jit_arm7gba_profile.cpp` -- landed.** A second `JitCpuProfile`
   (`isaLevel=4`, GBA's ARM7TDMI is the same core family the existing ARM7
   profile already targets -- zero changes needed in `jit_arm.cpp`/
   `jit_thumb.cpp`), built by `jitBuildArm7GBAProfile()` and swapped onto the
   *existing* `JIT_ARM7` slot rather than added as a third slot: both boot
   modes run on the same `NDS_ARM7` register file and share one
   `jitCacheArm7` arena/block table/SMC registry, since only one mode is
   ever live at a time.

   **Corrected a sketch inaccuracy in this section before implementing**:
   the sketch above described wiring the choice "into `jitBuildArm7Profile()`'s
   call site behind the boot-mode flag" -- tracing that call site
   (`jit_trace.cpp`'s `jitInit()`, called once from `NDS_Init()`) found it
   runs exactly once, well before any ROM is loaded and before
   `gameInfo.isGBA` can ever be meaningfully true, so gating *that* call on
   the flag would have just always built the DS profile. The actual, already-
   established hook is `jitRunArm7()` (`jit_exec.cpp`) re-reading
   `jitProfile[JIT_ARM7]` fresh on every call -- so `jitInit()` now builds
   *both* profiles once (`s_arm7DsProfile`/`s_arm7GbaProfile`, `jit_trace.cpp`),
   and a new `jitSetArm7GBAMode(bool)` swaps which one `jitProfile[JIT_ARM7]`
   points at, called from `NDS_DebugForceGBAMode()` alongside its existing
   `gameInfo.isGBA`/`MMU.isGBA` mirror updates -- the same synchronized-
   mirror pattern step 4 established, now with a third mirror.

   The GBA profile's `fetch16/32`/`slowRead`/`slowWrite` are thin wrappers
   identical in form to the DS ARM7 profile's, calling the same templated
   `_MMU_*<ARMCPU_ARM7,...>` functions -- traced end to end through `MMU.h`'s
   template chain to confirm the routing difference already lives one layer
   down, inside step 4's `MMU.isGBA`-guarded dispatchers, not in the profile
   itself. `swiHandler` is a stub (SWI already ends every trace to the
   interpreter today, for both existing profiles -- dead field, matched for
   consistency). `canEnterThumb`/`canEnterArm` are unconditionally `true`,
   mirroring the DS ARM7 profile's own policy (every GBA region is backed by
   a real, if presently zero-filled, buffer per steps 3-4). `cyclesForThumb`/
   `cyclesForArm` are verbatim duplicates of the DS ARM7 profile's cost
   tables, explicitly commented as such -- `armcpu_exec<ARMCPU_ARM7>()`'s
   cost model is one template, not specialized per boot mode, so these are
   exactly what the interpreter also computes for GBA-mode ARM7 today; real
   GBA wait-state timing (differs from DS's own numbers) isn't modeled by
   the interpreter either yet and stays deferred to step 7.

   **The one correctness-critical setting**: `mainMemBase` and `pageDescBase`
   (the P13/P14 inline fast-path enablers) are left `0`. Both are DS-specific
   by construction -- enabling either would resolve through `MMU.MAIN_MEM`/
   `MMU.MMU_MEM[ARMCPU_ARM7]`, the DS-only tables step 4 deliberately left
   untouched, reintroducing the same DS/GBA address-aliasing risk step 4
   fixed at the dispatcher level, just one layer up. Leaving both `0` forces
   every GBA access through `slowRead`/`slowWrite`, which correctly reaches
   the GBA-aware dispatchers -- matching how the ARM9 profile already leaves
   them `0` today for its own, different reasons.

   Verified: diff review of all four touched files (new `jit_arm7gba_profile.cpp`
   plus additive-only changes to `jit_trace.cpp`/`jit.h`/`NDSSystem.cpp`),
   confirming `mainMemBase`/`pageDescBase` are `0` and that
   `jitSetArm7GBAMode(false)` (the only path exercised by any real ROM today)
   leaves `jitProfile[JIT_ARM7]` exactly as it always was; a clean full
   devkitPPC Wii `.dol` build (the new translation unit is picked up
   automatically -- confirmed the Makefile's `SOURCES` lists whole
   directories, not an explicit file list, so no Makefile change was needed).
   Not exercised: still no real GBA ROM boots end-to-end -- no `ROMTYPE_GBA`
   wiring, no BIOS (step 6 implementation itself), no peripherals (step 7).
   `NDS_DebugForceGBAMode(true)` now correctly points the ARM7 JIT at the GBA
   profile and (via step 4) the GBA memory map, but there is still no
   meaningful GBA code anywhere for it to execute (`GBA_BIOS` is
   zero-filled).

5.5. **Real ROM load + direct boot + cartridge ROM -- landed; first real cart
   code verified executing.** Closes exactly the "no real GBA ROM boots
   end-to-end yet" gap every step above flagged. `NDS_LoadROM`
   (`NDSSystem.cpp`) now peeks the GBATEK offset-0xB2 magic byte before any
   DS-shaped decrypt/copy logic runs, and routes a real `.gba` cart to its
   own load path: the whole file (at most 32 MB) loads flat into one
   power-of-two-masked buffer, stored via `NDS_SetROM` -- a deliberate
   dual-purpose reuse of `MMU.CART_ROM`/`CART_ROM_MASK` (normally the DS
   card-controller's storage; safe because every ARM7 access is intercepted
   by the `isGBA` guard before reaching any DS-only code path, slot-1
   controller emulation included), which also backs a new
   `GBA_REGION_CART_ROM` case in `gbaDecodeAddr()`/the three
   `_MMU_ARM7GBA_read*` accessors (`MMU.cpp`) for `0x08000000+` (the three
   wait-state regions mirror one 32 MB window; content-identical, since this
   pass doesn't model wait-state timing -- step 7), and incidentally keeps
   `NDS_getROMHeader()` from returning `NULL` and aborting `NDS_Reset()`.
   `NDS_Reset()` gets a self-contained GBA direct-boot branch instead of
   more `isGBA` checks threaded through DS's firmware-vs-direct logic (whose
   "direct" branch would otherwise *write* through the `isGBA`-guarded
   dispatcher at a bogus DS-header-derived destination, corrupting real
   `GBA_EWRAM`/`GBA_IWRAM` before the game even started): `armcpu_init(&NDS_ARM7,
   0x08000000)` straight into the cartridge with no BIOS execution at all
   (matching step 6's function-level-HLE strategy -- `armcpu_init`'s default
   CPSR already happens to match the documented post-BIOS handoff state) and
   GBA's own post-BIOS stack pointers. A real safety gap was found and fixed
   while verifying this: `NDS_ARM7.swi_tab` was being set to DS's own
   `ARM7_swi_tab` unconditionally, regardless of `isGBA` -- a real SWI would
   have run a DS-shaped handler against GBA state. Forced to `NULL` for
   `isGBA`, so the interpreter's existing real-SWI-trap fallback takes over
   instead (jump to `0x00000008`, landing in the legitimately zero-filled
   `GBA_BIOS`) -- safe, not a step 6 implementation.

   Verified against real commercial content, not a synthetic test: staged
   The Legend of Zelda: The Minish Cap (a real, unmodified 16 MB `.gba`
   dump) and ran it through this repo's existing headless-Dolphin harness
   (`tools/benchmark/`'s flatpak+mtools mechanism, reused as-is) via a new
   `-DDESMUME_GBA_BOOT_PROBE` instrumentation mode (`main.cpp`) that samples
   ARM7's PC once per frame (the armwrestler-family probes' slot-2/ExpMemory
   convention doesn't apply -- a real commercial ROM can't cooperate with
   it) and writes `sd:/gba_boot.log`. Result: the CPU executed real cart
   code from the boot vector (sampled exactly at `0x08000008` on its first
   pass), ran forward through EWRAM/IWRAM-resident code (distinct-page and
   content-hash tracking confirmed real forward progress and real memory
   writes), then correctly took the real-SWI-trap path above once the ROM's
   own boot code issued its first SWI -- no crash, no corruption, exactly
   the known, honestly-flagged limitation of no BIOS/HLE content yet, not a
   surprise. This check deliberately used an interpreter-only build (no
   `JITDEFS`) so the JIT isn't a confounding variable in this first
   real-content result -- the JIT path itself is not yet exercised against
   real GBA content. Clean interpreter-only and plain (no `TESTDEFS`/
   `JITDEFS`) `.dol` builds both confirmed.
6. **BIOS strategy -- decided: function-level HLE, matching this codebase's
   existing DS pattern.** Real GBA BIOS is copyrighted and can't be vendored
   in this repo. Compared mGBA's approach (an independent clean-room ARM
   binary assembled to run *at* the BIOS address, `hle-bios.s` -> `hle-
   bios.c`, covering `SoftReset`/`RegisterRamReset`/`Halt`/`Stop`/
   `IntrWait`/`VBlankIntrWait`/`Div`/`DivArm`/`Sqrt`/`ArcTan`/`ArcTan2`/
   `CpuSet`/`CpuFastSet`/`BgAffineSet`/`ObjAffineSet`/`BitUnPack`/the LZ77/
   Huffman/RL decompression routines/`Diff8BitUnFilter`/`Diff16BitUnFilter`/
   `MultiBoot`/sound-driver SWIs, on by default, with an optional real-BIOS-
   file override for the handful of exact-timing/checksum edge cases it
   doesn't reproduce bit-for-bit) against this repo's own `source/src/
   bios.cpp`, which already does function-level HLE for the DS side today:
   SWI opcodes are intercepted in the interpreter/JIT and a C++ handler runs
   directly, nothing executes at the BIOS address at all, with
   `CommonSettings.UseExtBIOS` (`NDSSystem.cpp`) already providing the
   optional-real-BIOS-file layer on top. Decision: build `bios_gba.cpp` the
   same way as the existing `bios.cpp` -- SWI-intercept C++ handlers for the
   list above -- rather than mGBA's assembled-binary-at-0x0 approach, since
   it reuses this codebase's established pattern instead of introducing a
   new one. No BIOS file required by default; real-BIOS-file support can be
   added later as an optional accuracy layer mirroring `UseExtBIOS`, not
   required for this slice. Steps 3-5.5 (memory map, interpreter/JIT
   backends, real ROM load + direct boot) didn't depend on this decision and
   proceeded first, as planned.

   **Landed.** Before implementing, checked whether mGBA's own HLE BIOS
   (`hle-bios.s`/`.c`, MPL 2.0, (c) Jeffrey Pfau) was usable directly or
   worth vendoring, per explicit request. License isn't the blocker -- MPL
   2.0 combines cleanly into this GPLv3 project as a larger work, same
   precedent already set by VBA-GX's vendored JIT
   (`jit/upstream/PROVENANCE.md`). Architecture is: reading `hle-bios.s`
   closely, only six functions are real, self-contained ARM code
   (`SoftReset`/`Halt`/`IntrWait`/`VBlankIntrWait`/`CpuSet`/`CpuFastSet`).
   `Div`/`DivArm`/`Sqrt`/`ArcTan`/`ArcTan2`/`Lz77UnCompWram`/`Vram` are
   listed as labels with *no code body* -- they fall through into a
   `StallCall` whose cycle count is injected by a `swieq 0xF00000`
   mGBA-internal-only hook (their own native interpreter intercepts that
   SWI number specially). Run outside mGBA, those "implementations" would
   silently leave R0-R3 unmodified -- wrong results with no signal anything
   failed, worse than an explicit trap. **Not usable as a drop-in binary.**
   Genuinely useful as a reference, though: confirmed the real GBA SWI
   table ordering/numbers end-to-end against GBATEK, and cross-checked
   hardware-exact register addresses (`HALTCNT`=`0x04000301`,
   `IME`=`0x04000208`, the real `IntrWait` flag variable at `0x03007FF8`,
   reached in their code via an IWRAM-mirror trick that decodes to the same
   offset as this codebase's own `0x03`-bank `& 0x7FFF` mirroring from
   step 4).

   Built `bios_gba.cpp`/`ARM7GBA_swi_tab` (32 entries, real GBA SWI order)
   reusing this codebase's own `bios.cpp` DS function bodies (same
   VBA-derived lineage, already `_MMU_read/write<PROCNUM>`-routed and
   `MMU.isGBA`-aware since step 4) for `Div`/`CpuSet`/`CpuFastSet`/
   `BitUnPack`/the LZ77/Huffman/RL decompressors/`Diff8bitUnFilterWram`/
   `Diff16bitUnFilter`/`Sqrt`/`Halt`/`IntrWait`/`VBlankIntrWait`, duplicated
   (not shared by extern) and fixed to `ARMCPU_ARM7`, with the handful of
   GBA-specific constants swapped in. New real implementations not present
   in `bios.cpp` at all: `SoftReset`, `RegisterRamReset`, `Stop`, `DivArm`,
   `GetBiosChecksum`. Left as documented no-op stubs, matching both mGBA's
   own "Unimplemented" list and this codebase's DS `bios.cpp` precedent
   (never implemented even for DS): `ArcTan`/`ArcTan2`/`BgAffineSet`/
   `ObjAffineSet`, `Diff8bitUnFilterVram`, the Sound driver/`MusicPlayer`
   family, `MidiKey2Freq`, `MultiBoot`, `HardReset`, `CustomHalt`.

   Found mid-implementation: `arm_instructions.cpp`/`thumb_instructions.cpp`
   mask the SWI number with `& 0x1F` before indexing `swi_tab`, so SWI
   0x20+ can never reach a table entry at all (aliases down into
   0x00-0x0A) -- a pre-existing, shared DS/GBA interpreter limitation (DS's
   own tables are also only `[32]`), not introduced here; sized
   `ARM7GBA_swi_tab[32]` to match reality instead of the full 0x00-0x2A
   real-BIOS range. Also seeds `MMU.GBA_IWRAM[0x7FFA] = 1` in
   `NDS_Reset()`'s GBA direct-boot branch (the "normal cart boot" flag real
   BIOS would have written at cold boot, which direct boot otherwise never
   sets) so a game's own `SoftReset` call resolves back to the cartridge
   instead of empty EWRAM.

   Verified via the same headless-Dolphin methodology as step 5.5 (real,
   unmodified Minish Cap dump, interpreter-only build). Dramatic
   improvement over the step 5.5 baseline (`finalPC=0x00004e30`, drifting
   through zero-filled BIOS, 1 cart page ever visited):

   ```
   [gba_boot] frames=600 finalPC=0x080b063e CPSR=0x8000003f mode=0x1f T=1
   [gba_boot] samples: bios=0 ewram=0 iwram=0 cart=600 other=0
   [gba_boot] cart PC range: 0x08000008 - 0x080b0640, distinct 4K pages: 2
   ```

   All 600 sampled frames landed in cartridge code (T=1, Thumb --
   plausibly Minish Cap's Sappy sound-engine init/main loop), PC ranging
   across ~0xB0638 bytes of real game code, no crash, no corruption -- real
   sustained forward progress through actual gameplay-adjacent code, not a
   one-shot trap. Still explicitly not done: the no-op stubs above; §12.3
   step 7 (I/O/PPU/timers/DMA/keypad/wait-states) is entirely
   unimplemented, so nothing renders and `Halt`/`Stop`/`IntrWait`
   correctly but permanently park the CPU once a game reaches its first
   real wait-for-interrupt point; JIT path unexercised against real GBA
   content; no GBA savestate support.

   **Post-landing regression fix.** User asked to run the DS regression
   suite (armwrestler/arm7wrestler/RockWrestler) and the perf benchmark to
   confirm step 6 hadn't impacted DS mode. armwrestler (ARM 0/67, THUMB
   1/10 fail) and arm7wrestler (ARM 11/67, THUMB 1/20 fail) matched their
   established baselines exactly. RockWrestler did not: it consistently hit
   its probe's own 600-frame timeout with a virgin (never-written) slot-2
   result buffer, meaning the ROM never actually ran. Bisected via a
   `git worktree` control build at the pre-§12.2 commit (31fff7a) -- which
   reproduced the *same* failure under this session's contended host, ruling
   out a code regression as the first hypothesis -- then traced the real
   cause directly: `NDS_LoadROM`'s GBA-routing peek (`NDSSystem.cpp`, added
   in §12.2/935f61d) gated on nothing but the single GBATEK offset-0xB2
   fixed byte (0x96). `tools/rockwrestler`'s own hand-built (non-ndstool) DS
   header happens to carry `0x96` at that exact file offset by coincidence
   (confirmed with a hex dump), so it was silently misrouted into the GBA
   load path on every load and never booted as DS content at all -- a real
   false-positive, not a JIT or memory-map issue.

   Fixed by requiring the real GBA header's full 156-byte Nintendo-logo
   bitmap (GBATEK offset 0x04-0x9F -- the same blob the real GBA BIOS
   itself checksums before allowing boot) to match, in addition to the
   0xB2 byte, before classifying a file as `ROMTYPE_GBA`. The 156-byte
   table was taken from a real, unmodified commercial dump (The Legend of
   Zelda: The Minish Cap (USA).gba), verified byte-for-byte via `xxd`, not
   transcribed from memory. Applied to `NDSSystem.cpp`'s actual live check
   (`NDS_LoadROM`) -- while investigating, found that `utils/decrypt/
   header.cpp`'s own `DetectRomType`/`ROMTYPE_GBA` (referenced by
   `NDS_LoadROM`'s comment as "kept in sync") is **not presently compiled
   into this port at all**: `source/src/utils/decrypt/` isn't one of the
   Makefile's `SOURCES` directories, so it's dead code. Hardened it with
   the same logo check anyway (documented, not load-bearing) so the two
   copies stay in sync in case that ever changes, and left a comment
   explaining why it's currently inert.

   Verified the fix two ways: (1) the original, unmodified
   `tools/rockwrestler/out/rockwrestler.nds` now loads and runs correctly,
   reproducing the exact established baseline -- **10/23 fail**, same 10
   failing test names/detail codes as before (SMLALxy, LDM/STM, IPCSYNC,
   IPCFIFO, IPCFIFO IRQ, DIV 32/32, DIV 64/32, WRAM CNT, VRAM CNT, TCM); (2)
   re-ran the step 6 Minish Cap boot probe against the same fixed build --
   byte-identical result to step 6's own verification, confirming the
   stricter check doesn't regress real GBA routing.

   Perf: `tools/benchmark/benchmark.sh --scenes sm64 --modes "jit9off
   jit9on"` against the SM64DS castle-courtyard scene (the sanctioned
   soak/bench ROM -- Phantom Hourglass stays excluded from all soak/bench/
   diff runs per standing policy). ARM9 interpreter 13.16 eff.fps (22.0%
   realtime) vs. the pre-GBA-work baseline's 13.30 (22.2%); ARM9 JIT 10.98
   (18.4%) vs. 11.20 (18.7%) -- both -1.1%/-1.9%, under the harness's own
   3% regression threshold and consistent with ordinary run-to-run noise
   (this session's host was under heavy memory pressure for stretches, and
   the JIT capture's raw log had its tail corrupted by an unrelated,
   separately-confirmed SD-image write-back race -- see below -- so that
   number comes from a thinner 3-block in-window sample than usual). No
   perf regression found.

   **Separate finding, not a code bug:** every `tools/benchmark/
   benchmark.sh` capture for the `jit9on` mode this session (3 attempts)
   had its `bench.log` tail corrupted after a clean, valid prefix -- always
   at the same ~341-byte offset, always right after the last row the guest
   had genuinely written. The valid CSV rows are legitimate (sane,
   monotonic timings); the corrupted tail is structured (sequential
   32-bit values), not random flash-erase bytes, consistent with FAT
   cluster-slack from a stale prior occupant, exposed because the guest
   process is forcibly `pkill`'d mid-run and Dolphin's SD-card emulation
   doesn't always commit the directory entry's updated file-size field
   before the write-back races the kill (the same class of issue this
   plan already documented for the armwrestler-family probes' `sd:/*.log`
   writes -- see §8.2's `AW_RecordResult` comment -- those work around it
   with a reopen-and-rewrite grace window; `-DDESMUME_BENCH`'s plain
   append-every-60-frames path has no such mitigation). Recovered the
   real captured data by truncating each raw log at the first invalid
   UTF-8 byte and re-running `analyze.py` against the repaired file rather
   than re-running the capture blind; `jit9off` (no ARM9 JIT --
   deterministically faster, apparently outrunning the race) captured
   cleanly all three times. Not fixed this pass (harness-only, no
   emulator-code impact) -- worth a small `benchmark.sh` hardening later:
   either a longer post-append settle window before the kill, or have
   `analyze.py` tolerate/report a truncated tail instead of throwing.

7. **Peripherals**, each its own reference-first slice per §11/§13: PPU
   (display/video registers), APU (sound registers), timers, DMA, keypad/
   input, wait states, cartridge bus/save-memory devices, relevant timing
   behavior -- the original unordered checklist below, now sequenced after
   the CPU/memory-map foundation instead of alongside it.

Original subsystem checklist (still the reference for what "done" covers,
just no longer the plan for *how* to get there): GBA cartridge detection
(done, §12.2) -- DS compatibility-mode entry -- ARM7 configuration/state --
GBA memory map -- internal/external WRAM -- cartridge ROM address space --
save-memory devices -- BIOS behavior -- IRQs -- timers -- DMA --
display/video registers -- sound registers -- keypad/input -- wait states --
reset behavior -- cartridge bus behavior -- relevant timing behavior.

Throughout, separate `known hardware requirements` from
`emulator-specific approximations` so that later hardware discoveries do not
require redesigning the entire compatibility layer.

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

### Predicated branch compilation — status (§16 re-open) — LANDED, gate removed

Predicated `Bcc` **and `BLcc`** (cond ≠ AL) **compile unconditionally**
(`jit_arm.cpp` `emitBranch`). This was gated behind `-DJIT_ARM_PRED_BRANCH`
while it was being proven out; both CPU-correctness gates then cleared with zero
new failures (armwrestler ARM9, arm7wrestler ARM7) on top of ~1.3 B-instruction
ARM9 differential/soak coverage, so the flag was **deleted** — the codegen is
now permanent, no build knob. The old bailout (`ctx.endBlock = true` at any
predicated branch) is gone.

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
- **Post-flag-removal re-measure** (`94df464`, `benchmark.sh --scenes sm64
  --modes "jit9off jit9on jitfull"`, frames 300-1200, cv ≤ 1.4 %): ARM9
  interpreter **13.16** → ARM9 JIT **14.43** eff.fps (**+9.6 %**, was −16 %
  with the flag off / predicated branches bailing → +31 % swing on the ARM9
  JIT number vs the prior run); full JIT over GXMerge **36.17** eff.fps
  (60.5 % real-time). The default JIT build now gets the predicated-branch win
  automatically — no build knob. Recorded as
  `results/20260908T061701Z_94df464-dirty/`.
- **Post-flag-removal differential soak** (`53d82bd`,
  `tools/benchmark/diff-soak.sh`, `JIT_DIFFERENTIAL_TESTING`, SM64DS ~210 s):
  `diff9` **16.8 M ARM9 blocks / 503.7 M instructions, 0 mismatches / 0
  logged**, selftest + journal-selftest PASS, 0 CANARY / 0 ARENA-OVERRUN / 0
  CHAIN-DIFF / 0 bad-resume, `predBcc a9=158` compiles exercised. Removing the
  `#if` wrapper produces byte-identical codegen to the previously
  1.3 B-instruction-validated flagged build, and this soak reconfirms it.

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

Both CPU-correctness gates were clear, so `-DJIT_ARM_PRED_BRANCH` was removed
and predicated-branch codegen is now permanent (`f140278`+). This is
independent of the §25 `jitArm9Enabled` default-on bar (RockWrestler, retail
soaks, host-memory hardening, etc. still open) — those gate turning the ARM9
JIT on by default, not this one codegen path, which is exercised whenever the
JIT runs at all.

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

**Current position (`8f014e3`): item 1.** ARM9 telemetry (§5) shows the ~1.9-
block chain ceiling is the emitter refusing predicated (`cond != AL`) memory
ops — `LDRcc`/`STRcc`/`STRHcc`, `LDMcc`/`STMcc` (`POPcc`/`PUSHcc`) — so blocks
terminate and the next instruction is interpreted one at a time. Extending the
existing predicated-data-proc codegen (`emitEvalCond` + conditional skip) to
those forms is a block-length win and comes before any dispatch-table work
(item 3). Predicated `BXcc` was tried first and reverted (§5) — a conditional
return's taken path is a dynamic exit, not a block continuation.

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
| 16 | DS CPU reference matrix: melonDS + DeSmuME interpreter           | in progress: §9 table filled in for DeSmuME interpreter/JIT (all 3 ROMs, byte-identical JIT results); melonDS column blocked -- root-caused (§8.5) to a melonDS bug (its GDB stub/scheduler intermittently no-ops an acknowledged single-step/instruction, roughly every 3-4 steps, confirmed by direct single-step trace after ruling out our own write path, EXMEMCNT, and CP15 write-buffering), not anything in our ROMs or harness; automation path (CLI hook, GDB polling, breakpoint/single-step support) is proven and reusable the moment melonDS's side is fixed |
| 17 | `armwrestler` automated regression gate                          | done: headless via slot-2 I/O (§8.2); found + fixed an 8MB-addon/JIT-arena OOM hang; found + fixed pre-existing ARM9 JIT bugs (SMLAL missing carry, THUMB LDR missing unaligned rotate) -- back to clean baseline (ARM 0/67, THUMB 1/10) |
| 18 | `arm7wrestler` automated regression gate                         | done: headless via slot-2 I/O (§8.3), same technique as #17 -- interpreter baseline ARM 11/67 fail (matches documented ARMv4T-vs-ARMv5 differences) / THUMB 1/20 fail; `-DJIT_ARM_PRED_BRANCH` build byte-identical, 0 new failures -- ARM7 predicated-branch gate cleared |
| 19 | RockWrestler automated DS conformance gate                       | done: headless via slot-2 I/O (§8.4), no crt0 workaround needed (upstream is `-nostartfiles`) -- interpreter baseline 10/23 fail (SMLALxy, LDM/STM base-in-list, IPCSYNC/IPCFIFO/IPCFIFO IRQ, DIV 32/32 + 64/32 sign-extension, TCM/CP15 readback -- all pre-existing interpreter gaps, characterized in §8.4); `-DJIT_ARM_PRED_BRANCH` build byte-identical, 0 new failures |
| 20 | GBA compatibility architecture                                   | in progress: full source audit done (§12.1, no native-GBA-execution scaffolding existed anywhere); found + fixed a real, GBA-independent 12 KB heap over-read in `DecryptSecureArea` (`SMALL_READ` undersized, hit on most normal encrypted-ROM loads, confirmed via isolated ASan repro against the unmodified real source and fixed); added real GBA-header detection (`ROMTYPE_GBA`, GBATEK offset-0xB2 magic) so `NDS_LoadROM` cleanly rejects a `.gba` file instead of misparsing it as a DS header (§12.2). §12.3 steps 1-2 landed: `GameInfo::isGBA` boot-mode flag + ARM9-halt mechanism (`armInnerLoop<false,true>`, reusing the existing per-CPU-gateable template rather than adding a new runtime check) -- proven ARM9-inert via a standalone ASan/UBSan replica test and a clean full build, zero behavior change for real DS ROMs since nothing yet sets the flag from a real load. Not yet wired to `ROMTYPE_GBA` detection (deliberate -- see §12.3 step 1) and no GBA execution yet. §12.3 step 6 decided: BIOS strategy is function-level HLE (`bios_gba.cpp`, matching the existing DS `bios.cpp` SWI-intercept pattern), compared against mGBA's assembled-binary-at-0x0 approach; not yet implemented. §12.3 step 3 landed: six `GBA_`-prefixed backing buffers (`GBA_BIOS`/`GBA_EWRAM`/`GBA_IWRAM`/`GBA_PALETTE`/`GBA_VRAM`/`GBA_OAM`) added to `MMU_struct`, reset-safe, purely additive diff, clean full build -- allocation only, inert until step 4 wires read/write address decoding to them. §12.3 step 4 landed: `_MMU_ARM7GBA_read/write08/16/32` (`MMU.cpp`) now back those buffers with real address decode/mirroring (BIOS/EWRAM/IWRAM/Palette/OAM/VRAM, the last with the documented GBATEK quirk-mirror), reached via a first-statement guard in `MMU.h`'s six hot-path dispatchers (placed first specifically because GBA's EWRAM/IWRAM ranges alias existing DS ARM7 fast-path checks in those same functions) gated on a new `MMU.isGBA` mirror of `gameInfo.isGBA` (needed only because `MMU.h` can't include `NDSSystem.h` back to see `GameInfo`) -- verified via a 30/30-passing ASan/UBSan host-native unit test of the pure decode function plus a clean full build. I/O/cartridge/SRAM still unmapped placeholders (step 7); BIOS read-protection-when-PC-not-in-BIOS quirk explicitly not implemented (flagged, not silent). §12.3 step 5 landed: `jit_arm7gba_profile.cpp` builds a second `JitCpuProfile` swapped onto the existing `JIT_ARM7` slot (not a third slot -- both boot modes share one `NDS_ARM7`/`jitCacheArm7`) via a new `jitSetArm7GBAMode()` (`jit_trace.cpp`), called from `NDS_DebugForceGBAMode()` as a third mirror of the boot-mode flag alongside `gameInfo.isGBA`/`MMU.isGBA`; corrected the earlier sketch's assumption that `jitBuildArm7Profile()`'s one-time `jitInit()` call site (which runs once, pre-ROM-load, from `NDS_Init()`) could itself gate on the flag -- traced that it can't, and hooked the swap at `jitRunArm7()`'s per-call `jitProfile[JIT_ARM7]` read instead. Memory-access hooks reuse the same `MMU.isGBA`-guarded templated calls step 4 wired; `mainMemBase`/`pageDescBase` deliberately left `0` to avoid reintroducing step 4's DS/GBA aliasing risk via the P13/P14 inline fast paths; cycle-cost tables are verbatim, documented duplicates of the DS ARM7 profile's (same un-diverged interpreter cost model). Verified via diff review + clean full build (new file auto-picked-up by the Makefile's directory-level `SOURCES` globbing, no Makefile edit needed). §12.3 step 5.5 landed: **first real GBA cart code verified executing.** `NDS_LoadROM` now actually loads a real `.gba` file (magic-byte-routed before DS decrypt/copy logic runs) into a masked cartridge-ROM buffer backing a new `GBA_REGION_CART_ROM` case in the memory decoder; `NDS_Reset()` direct-boots ARM7 straight into the cartridge (no BIOS execution, matching step 6's decided strategy) with GBA's real post-BIOS register state; found and fixed a real safety gap (`NDS_ARM7.swi_tab` was defaulting to DS's own SWI table even in GBA mode) so a real SWI now safely traps to zero-filled `GBA_BIOS` instead of misfiring a DS handler against GBA state. Verified against real commercial content (The Legend of Zelda: The Minish Cap, unmodified 16 MB dump) via a new headless boot-probe instrumentation mode (`-DDESMUME_GBA_BOOT_PROBE`, `main.cpp`) run through this repo's existing flatpak/mtools Dolphin harness (`tools/benchmark/`): the CPU executed real cartridge code from the boot vector, ran forward through EWRAM/IWRAM-resident code, and safely hit the expected SWI-trap limitation -- no crash, no corruption. Interpreter-only this pass (JIT path not yet exercised against real GBA content). §12.3 step 6 landed: `bios_gba.cpp`/`ARM7GBA_swi_tab` -- real GBA BIOS SWI table (function-level HLE), superseding step 5.5's trap. Checked mGBA's own HLE BIOS (`hle-bios.s`/`.c`, MPL 2.0) for direct use per explicit request: not usable as a drop-in binary (its `Div`/`Sqrt`/`ArcTan`/LZ77 "implementations" are empty stubs that depend on an mGBA-internal-only cycle-stall hook and would silently leave registers unmodified outside mGBA), but valuable as a reference for real SWI table ordering and hardware-exact register addresses. Reuses this codebase's own DS `bios.cpp` function bodies (same VBA lineage, `MMU.isGBA`-aware since step 4) for the shared ops, fixed to ARM7 with GBA-specific constants swapped in; adds real `SoftReset`/`RegisterRamReset`/`Stop`/`DivArm`/`GetBiosChecksum` not present in `bios.cpp` at all; leaves `ArcTan`/`ArcTan2`/`BgAffineSet`/`ObjAffineSet`/sound-driver family/`MultiBoot`/`HardReset`/`CustomHalt` as documented no-ops, matching both mGBA's own unimplemented list and DS `bios.cpp`'s own precedent. Found the interpreter's swi_tab dispatch masks the SWI number `& 0x1F` (pre-existing, shared DS/GBA limitation, not introduced here) -- sized the table `[32]` to match. Verified via the same headless-Dolphin Minish Cap methodology: dramatic improvement over step 5.5's one-shot trap -- all 600 sampled frames now land in real cartridge code (Thumb state, ~0xB0638 bytes of PC range covered), no crash, no corruption -- genuine sustained forward progress, not just a safe dead end. Remaining: peripherals (step 7 -- I/O registers/PPU/timers/DMA/keypad/wait-states, so nothing renders yet and Halt/IntrWait now correctly but permanently park once a game reaches its first real wait-for-interrupt point); JIT path still unexercised against real GBA content; no GBA savestate support. Post-landing regression fix: DS regression suite + perf benchmark run per user request found `NDS_LoadROM`'s §12.2 GBA-routing check (single fixed byte at offset 0xB2) false-positived on `tools/rockwrestler`'s own hand-built DS header, silently misrouting it into the GBA load path and breaking the RockWrestler gate outright -- fixed by requiring the real 156-byte GBA Nintendo-logo match too (verified against a real Minish Cap dump); RockWrestler back to its exact 10/23-fail baseline, Minish Cap GBA routing reverified unchanged. armwrestler/arm7wrestler and the SM64DS perf benchmark (ARM9 interpreter/JIT) both matched their established baselines within noise -- no other DS-mode regression found |
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
