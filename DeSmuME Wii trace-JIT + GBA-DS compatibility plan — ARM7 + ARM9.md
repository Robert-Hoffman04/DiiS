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
chain length on SM64DS is ~2 blocks / ~5 guest instructions — short, because
predicated ARM branches bail to the interpreter (§16) and force a trampoline
round-trip. A fixed mapping's unconditional 15-register trampoline load/store
would lose against the current lazy allocator at that chain length. Not
justified by current evidence.

**Measured picture:** on SM64DS (ARM9-bound, ARM9 interpreted) the ARM7 JIT is
still a ~4.4% whole-frame regression. The dominant remaining cost is the sheer
number of trampoline round-trips (short chains) plus the trampoline's own
`stmw`/`lmw`. ARM7-JIT frame value is likely capped until (a) predicated ARM
branches compile (unblocks longer chains — §16) and/or (b) the ARM9 JIT is
viable in `jitfull` (also §16).

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

### Tier 3 — structured memory operations

Optimize:

- LDM
- STM
- PUSH
- POP
- sequential loads/stores

Do not sacrifice memory-map correctness for benchmark gains.

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
`cpu.isaLevel`. Predicated ARM branches still bail to the interpreter (the same
unresolved concern as ARM9 — section 16).

**Validated (SM64DS, headless Dolphin):**
- 210s differential soak: ARM7 `diff` 400K blocks / 1.67M insns, **0 mismatches**,
  0 `DIFF` / 0 `CHAIN-DIFF` / 0 `ARENA OVERRUN`. ARM9 `diff9` 59.8M blocks
  unchanged from baseline (the `isaLevel` gate is inert for ARMv5TE — no
  regression).
- 240s non-differential production-path soak (chaining on, no harness): 8.2M
  block runs / 39M guest insns, ARM-mode blocks confirmed executing (`blk A`),
  **canary poll never latched** (the same host-heap-corruption detector that
  caught the ARM9 predicated-Bcc bug), 0 SMC-kill anomalies over 50M checks.
- Cycle model is coarse v1: ~23% of ARM7 blocks show cycle drift, bounded at
  ≤13 cyc/block (harness treats ARM7 cycle drift as advisory). Refinement later.
- Still outstanding: `armwrestler` / `arm7wrestler` instruction ROMs (not yet
  on hand).

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

---

## 8.3 `arm7wrestler`

Use `arm7wrestler` as a dedicated ARM7 validation target.

It is particularly important because the ARM7 is shared conceptually between:

- normal DS ARM7 execution
- GBA compatibility execution
- ARM7 JIT execution

A failure here can contaminate several later workstreams.

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

---

# 9. DS reference matrix

Maintain a machine-readable results table:

| Test                    | melonDS | DeSmuME interpreter | DeSmuME JIT | Hardware/reference | Classification |
| ----------------------- | ------- | ------------------- | ----------- | ------------------ | -------------- |
| armwrestler             |         |                     |             |                    |                |
| arm7wrestler            |         |                     |             |                    |                |
| RockWrestler ARMv4      |         |                     |             |                    |                |
| RockWrestler ARMv5      |         |                     |             |                    |                |
| RockWrestler IPC        |         |                     |             |                    |                |
| RockWrestler memory     |         |                     |             |                    |                |
| RockWrestler TCM        |         |                     |             |                    |                |
| GBA compatibility tests |         |                     |             |                    |                |

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

Predicated branch compilation remains a hard correctness concern until proven safe.

The fallback is acceptable during development:

```text
predicated Bcc
    → interpreter
```

but the bug should eventually be isolated rather than permanently hidden behind a bailout.

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
| 11 | ARM front-end on ARM7                                            | done (SM64DS soak; armwrestler pending) |
| 12 | Persistent JIT state + trampoline amortization                   | done: r31 icount + r30 CPSR resident; GPR residency deferred (§5/§23) |
| 13 | Inline memory fast paths                                         | Tier-1 literal loads landed; general/WRAM tier deferred (§6/§23) |
| 14 | Cached page descriptors                                          | next            |
| 15 | LDM/STM and sequential memory optimization                       | next            |
| 16 | DS CPU reference matrix: melonDS + DeSmuME interpreter           | next            |
| 17 | `armwrestler` automated regression gate                          | next            |
| 18 | `arm7wrestler` automated regression gate                         | next            |
| 19 | RockWrestler automated DS conformance gate                       | next            |
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
