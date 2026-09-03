# ARM7 THUMB trace-JIT — porting the VBA-GX recompiler to DeSmuME Wii

Status: **in progress — P0 landed** (branch `arm7-jit`; vendored infra compiles
behind `JITDEFS=-DDESMUME_JIT_ARM7`, unwired). Companion to
[desmumewii-perf-opportunities.md](desmumewii-perf-opportunities.md) §1.2(b).

External references: VBA-GX JIT source `dborth/vbagx` @ `07ee4af`
(`source/vba/gba/JIT*`, frozen copy in `source/src/jit/upstream/`);
instruction-conformance test ROMs `jsmolka/gba-tests` (MIT).

The goal: take VBA-GX's THUMB trace-JIT (`dborth/vbagx`, `source/vba/gba/JIT*`,
release 3.0.1 / Aug 2026, GPL-2.0+, © Daryl Borth) and stand it up as the DS
**ARM7** execution path in this emulator, keeping the C++ interpreter as the
always-correct fallback. The port covers **both** the THUMB path (a straight
adaptation of VBA's compiler) and a new **ARM (32-bit) mode** front-end — DS
ARM7 code is meaningfully more ARM-heavy than GBA code, so THUMB alone leaves too
much on the interpreter. Infrastructure is built CPU-agnostic so the same
machinery — plus both front-ends — later drives the ARM9 with an ARMv5 profile.

---

## 1. Why the ARM7 first, and why this JIT

* **The cores are identical.** GBA and DS-ARM7 are both ARM7TDMI (ARMv4T). Every
  THUMB opcode VBA-GX already compiles has bit-for-bit the same semantics on the
  DS ARM7 — no ISA porting for the THUMB path, only *environment* porting (memory
  map, timing, interrupts, register-state layout). The ARM-mode front-end is the
  same ARMv4T instruction set DeSmuME's `arm_instructions.cpp` already
  interprets — a new decoder + emitters, but no new *semantics* to reverse-engineer.
* **The hard parts are already solved and shipped.** VBA-GX's JIT has a working
  Broadway emitter, an 8 MB bump-arena + 64 K-bucket block cache, a self-patching
  linker stub for block chaining, a hand-written ABI trampoline, lazy
  per-block register/flag allocation, a deferred-bailout scheme, SMC tracking at
  1 KB-page granularity, and a differential test harness. That is roughly the
  90% of a dynarec that is tedious and bug-prone.
* **ARM7 is a good blast radius.** It runs the IPC/sound/RTC/wifi driver, not the
  game logic. A miscompile corrupts audio or a FIFO, not the whole game, and the
  differential harness + interpreter fallback contain it. ARM9 (the actual
  3× bottleneck, §1.2 of the opportunities doc) comes second, on hardened
  infrastructure.
* **Endianness already handled.** Both guests are little-endian, host is
  big-endian; VBA's emitter already uses `lwbrx`/`lhbrx`/`stwbrx`/`sthbrx` for
  every guest access. Same trick we just landed in `mem.h` (commit 691325e).

### What this is NOT

Not a full CPU core, no cross-block register allocation, no optimizing IR. THUMB
lands first (P2–P5), ARM mode second (P7), **both before any ARM9 work**.
Anything either front-end doesn't recognise silently falls to the C++
interpreter — the trace just ends at that PC. The THUMB path matches VBA-GX's own
scope; the ARM front-end is net-new work (VBA-GX has no ARM JIT) but reuses every
piece of the infrastructure below.

---

## 2. What VBA-GX gives us (survey of the vendored code)

| File | Role | Reuse |
|---|---|---|
| `JITCache.{h,cpp}` | arena bump-allocator, direct-mapped block hash table, self-patching linker stub, SMC page registry + `invalidateSMCTarget()` | **verbatim**, CPU-agnostic (only the bank check `bank==2||bank==3` is GBA-specific) |
| `JITPPCEmitter.h` | PowerPC-750 instruction encoder macros + host register contract (R14 regs-ptr, R15–R28 lazy guest-reg pool, R29 guest PC, R30 read-table, R6 packed N/Z/C/V) | **verbatim** |
| `JITTrampoline.S` | `ExecuteJITTrace` / `ExecuteJITTrace_Return` — C ABI ⇄ JIT register contract bridge | **near-verbatim** (arg list changes with our state struct) |
| `JIT.h` | `JITResult` handshake struct (cycles, nextPC, instr count, bailedOut, smcHit/smcAddress) | **verbatim** |
| `JITCompiler.cpp` (2400 ln) | the THUMB trace scanner + per-format emitters, lazy reg/flag trackers, deferred bailouts, prefetch/wait-state timing model, memory-guard template | **port & adapt** — structure kept, memory map + timing + flag storage swapped |
| `JITDifferential.{h,cpp}` | lockstep interpreter-vs-JIT state comparator, per-instruction mismatch log | **port** — rewire to DeSmuME state |
| `JITDebugStateLog.{h,cpp}`, `Profiler.*` | SD-card trace logging, cache hit/miss/evict counters | **port** — useful for bring-up and for measuring THUMB coverage |

Memory-access model in VBA: a `gbaReadTable` of page pointers passed into the
trace; the guard template is *bank check → page/mask lookup → null-pointer guard
→ mask/align → access*, with stores hard-restricted to EWRAM/IWRAM (banks 2/3)
and anything else deferring to a C++ bailout. This template is exactly what we
re-target.

---

## 3. The DeSmuME ARM7 environment (what actually changes)

### 3.1 Register + flag state

VBA keeps guest regs in `reg[16]` (`u32 reg[16].I`) and the four condition flags
as **four separate words** (`N_FLAG…V_FLAG`), passed as `&gbaFlags`.

DeSmuME keeps them in `armcpu_t` (`armcpu.h:176`): `u32 R[16]`, and NZCVQ
**packed into `CPSR.val` bits 28–31 (+ Q at 27)** via a bitfield union, plus
banked SPSR/R13/R14 per mode.

**Touchpoint:** the trampoline's "load 4 flag words" (`EnsureFlagsLoaded`) and
"store 4 flag words" (`EmitFlagUnpack`) become "load `NDS_ARM7.CPSR.val`, keep
working nibble in R6" / "rlwimi the nibble back into `CPSR.val` at exit". The
packed-flag emitter macros (`PPC_MERGE_FLAG_BIT`/`PPC_EXTRACT_FLAG_BIT`) already
assume a packed nibble — they need the bit positions aligned to CPSR
(N=31,Z=30,C=29,V=28) instead of VBA's (N=0,Z=1,C=2,V=3 in IBM numbering). One
header edit.

The JIT only ever runs in one CPU mode's register bank (whatever ARM7 is in when
the block starts); any instruction that could change mode (`MSR`, mode-changing
`BX`, SWI) is a block terminator / bailout, so banked-register handling stays
entirely in the interpreter. Keep it that way for v1.

### 3.2 Memory map

ARM7 fast regions (from `MMU.h` `_MMU_read*` / `_MMU_ARM7_*`):

| Guest range | Backing | Mask | JIT treatment |
|---|---|---|---|
| `0x0200_0000–0x02FF_FFFF` | `MMU.MAIN_MEM` (4 MB shared) | `0x3F_FFFF` | inline fast path |
| `0x0380_0000–0x03FF_FFFF` | `MMU.ARM7_ERAM` (64 KB) | `0xFFFF` | inline fast path |
| `0x0300_0000–0x037F_FFFF` | `MMU.SWIRAM` (shared WRAM window) | `0x7FFF` | inline fast path *(note: WRAMCNT-dependent size/routing — see risk)* |
| `0x0000_0000–0x0000_3FFF` | ARM7 BIOS | — | read-only, inline or bail |
| `0x0400_xxxx` IO, `0x0480_xxxx` wifi, `0x0600_xxxx` VRAM-to-ARM7, `0x0800_xxxx` GBA slot | function-dispatched | **always** call `_MMU_read*<1>` / `_MMU_write*<1>` |

**Approach — correctness first, speed later:**

* **v1:** every guest load/store emits a call to the existing templated
  `_MMU_read{8,16,32}<1,MMU_AT_DATA>` / `_MMU_write{…}<1,…>` C functions (they
  already do region decode + endian swap + timing side-effects + Lua hooks). The
  JIT still wins big by amortizing fetch/decode/dispatch/prefetch; memory just
  isn't inlined yet. Zero chance of a memory-map divergence bug.
* **v2 (Phase 6):** build an `arm7ReadPage[]` / `arm7WritePage[]` table (256
  entries over `addr>>24`, or finer) mirroring VBA's `gbaReadTable`, populate it
  for MAIN_MEM / ARM7_ERAM / SWIRAM, and emit VBA's inline guard template for
  those; keep the C-call for everything else. Rebuild the table on WRAMCNT
  changes (see risk §7).

Stores must still route through `JITCache::invalidateSMCTarget()` — see 3.4.

### 3.3 Timing / scheduler integration — the real design work

VBA runs one CPU with a full wait-state + prefetch-buffer model
(`EmitPrefetchSync`, `EmitPrefetchDataWait`, `EmitDynamicNCyclePenalty`, …).
DeSmuME's ARM7 timing is **almost entirely disabled** (`MMU_timing.h`: code
fetch returns 1, most access timing #defined out). This is a *simplification*:
**delete VBA's prefetch/wait-state machinery for v1** and count cycles the way
`armcpu_exec<1>()` does — a small per-instruction constant, `<<1` into the shared
timebase (matching `armInnerLoop`'s `arm7 += armcpu_exec<ARMCPU_ARM7>()<<1`).

The scheduler (`NDSSystem.cpp` `armInnerLoop`, line 1576) interleaves ARM9/ARM7
at **single-instruction granularity**, picking whichever is behind on cycles. A
block JIT must not run 42 THUMB instructions while ARM9 is starved. Design:

1. New entry point `template<int PROCNUM> u32 armcpu_exec_block(s32 cycleQuota)`
   (or fold into `armcpu_exec`). When `PROCNUM==1` and JIT is enabled — for the
   THUMB path once P2 lands, for the ARM path once P7 lands, selected by CPSR.T:
   * look up / compile the block at `next_instruction` (scanner uses the T-bit
     to pick the THUMB or ARM emitter table);
   * `ExecuteJITTrace(...)` runs the block *and chains* through subsequent
     compiled blocks via the linker stub, but the emitted epilogue checks a
     **cycle quota** each block boundary (VBA's "quota-shield yield" — already
     in the emitter) and returns cleanly when `cycles >= quota`;
   * return aggregate cycles to `armInnerLoop`, which advances `arm7` and
     re-evaluates the interleave.
2. `cycleQuota` = `min(remaining-slice, kArm7JitQuota)` with `kArm7JitQuota`
   small (start at 32 ARM7 cycles ≈ what the interleave would run anyway) and
   tune upward while watching audio/IPC regressions.
3. **Immediate bailout conditions** compiled into every block exit / checked on
   return: `sequencer.reschedule`, `NDS_ARM7.waitIRQ` becoming set, an IRQ
   becoming deliverable (`IF & IE & IME`). A block that writes an ARM7 IO
   register (IE/IF/IME/IPCSYNC/FIFO/HALTCNT/DMA/timer regs) already exits via
   the C-call store path → cheap to force a yield there.
4. IRQ delivery (`armcpu_irqException`) and `armcpu_Wait4IRQ` stay in C, entered
   only between blocks — never mid-block.

### 3.4 Self-modifying / DMA-written code

VBA tracks which compiled blocks live on which 1 KB guest page (EWRAM/IWRAM) and
patches a block's first instruction to a bail-branch the moment a guest write
lands on it. For ARM7 the "code can live here" banks are **MAIN_MEM, ARM7_ERAM,
SWIRAM**. Every write path that can hit those must call `invalidateSMCTarget()`:

* JIT's own inline store guards (v2) — VBA already emits `EmitSMCWriteCheck`;
* interpreter ARM7 stores — one hook in `_MMU_write*<1>` (or `_MMU_ARM7_write*`);
* **ARM7 DMA** (`MMU.cpp` DMA engine) writing to those regions;
* ARM9 writes to **shared main RAM / shared WRAM** that alias ARM7 code pages
  (cross-CPU SMC — GBA never had this; DS does). Simplest safe answer for v1:
  the SMC registry/flags are global, and `_MMU_write*<0>` to `0x02xxxxxx` /
  `0x03xxxxxx` also calls `invalidateSMCTarget()`. Measure the cost; most ARM9
  main-RAM writes miss the page-flag bitmap in one load.

DMA into a code page mid-frame is rare on ARM7; correctness > speed — just
invalidate.

---

## 4. Module layout

```
source/src/jit/
  jit_cache.{h,cpp}       <- vendored JITCache, bank check parameterized
  jit_ppc_emitter.h       <- vendored JITPPCEmitter, flag-bit positions -> CPSR
  jit_trampoline.S        <- vendored, arg list -> jit_cpu_state*
  jit.h                   <- JITResult + public entry points
  jit_cpu_profile.h       <- NEW: the CPU-agnostic seam (see below)
  jit_trace.{h,cpp}       <- NEW: shared trace scanner / chunk tracker / bailout
                              plumbing / cycle accounting, front-end agnostic
  jit_thumb.cpp           <- ported JITCompiler.cpp — THUMB emitters (P2)
  jit_arm.cpp             <- NEW: ARM (32-bit) emitters + predication (P7)
  jit_arm7_profile.cpp    <- ARM7 JitCpuProfile: memory maps, cost model, SWI hook
  jit_differential.{h,cpp}<- ported harness, DeSmuME state, hooks both dispatchers
  jit_debuglog.{h,cpp}, jit_profiler.{h,cpp}  <- ported
```

`jit_thumb.cpp` and `jit_arm.cpp` are two emitter tables over the *same* trace
scanner, register/flag allocator, memory-guard template and bailout machinery in
`jit_trace.*`; the scanner picks the table from the block's T-bit. A block is
single-mode — a mode switch (`BX`, `POP {pc}` to an ARM address, etc.) ends the
trace and the next block is scanned in the new mode.

Add `source/src/jit` to `SOURCES` in the Makefile; add `jit_trampoline.S` (the
build already globs `*.S`). Gate the whole subsystem behind `-DDESMUME_JIT_ARM7`
so `master` builds are unaffected until it's proven.

### The CPU-agnostic seam (`jit_cpu_profile.h`)

```c
struct JitCpuProfile {
    // state
    u32*  gpr;                 // -> armcpu_t::R[0]
    u32*  cpsr;                // -> armcpu_t::CPSR.val   (packed NZCVQ)
    // memory
    u8**  readPage;  u8** writePage;   // page tables (null => C fallback)
    u32   (*slowRead)(u32 addr, int size);
    void  (*slowWrite)(u32 addr, u32 val, int size);
    void  (*smcInvalidate)(u32 addr);
    // control
    bool  (*canEnter)(u32 pc);         // T-bit set, region compilable, JIT on
    u32   (*swiHandler)(u32 comment);  // always a block terminator
    // timing
    u8    cyclesForThumb(u16 opcode);  // DeSmuME per-instr cost model
    u8    cyclesForArm(u32 opcode);    //   "
    s32   isaLevel;                    // 4 = ARMv4T (ARM7), 5 = ARMv5TE (ARM9)
};
```

Both front-ends are `#include`-driven by this one struct. The ARM7 profile sets
`isaLevel=4`. ARM9 later supplies a second profile with `isaLevel=5`, TCM-aware
page tables, and the ARMv5 additions (`BLX`, `CLZ`, `QADD`, saturating/DSP
multiplies, `LDRD`/`STRD`, `BKPT`) gated on `isaLevel` inside the *same*
`jit_thumb.cpp` / `jit_arm.cpp` — **the cache, arena, emitter, trampoline,
linker stub, SMC registry, trace scanner and differential harness do not
change.**

---

## 5. Opcode coverage (port order)

### 5.1 THUMB (P2, P4)

VBA-GX already implements all of these; port in this order, differential-testing
each group before the next:

1. **Format 1–2** shift/add/sub immediate; **Format 3** mov/cmp/add/sub imm8 —
   pure ALU + N/Z/C/V. Smoke test the emitter + trampoline + flag plumbing.
2. **Format 4** ALU reg-reg (incl. the shift-by-register carry cases).
3. **Format 5** hi-reg ops + `BX` (BX = block terminator; if target T-bit clears,
   bail to interpreter).
4. **Format 6** PC-relative literal load (compile-time address → foldable).
5. **Format 9/10/11** loads/stores (imm & reg offset, byte/half/word, LDRSB/H) —
   via the C-call memory path in v1.
6. **Format 16** conditional branch, **Format 18** unconditional branch — the
   payoff (hot loops stay native, block chaining kicks in).
7. **Format 13** SP-adjust, **Format 14** PUSH/POP (POP-with-PC = terminator).
8. **Format 15** LDMIA/STMIA.
9. **Format 12** ADD PC/SP, **Format 7/8** already covered by 9/10/11.
10. **Format 17** SWI, **Format 19** BL — terminators / interpreter hand-off.

Anything not in a ported group: the scanner stops the trace and emits a clean
exit at that PC (VBA's `endBlock = true`). Never a guess.

### 5.2 ARM / 32-bit (P7 — new work)

ARMv4T ARM mode, ~15 encoding classes. The structural additions over the THUMB
emitter:

* **Per-instruction predication.** Every ARM instruction carries a 4-bit
  condition. For `cond != AL`, emit a flag test (reuse VBA's `ReadFlag` +
  composite-condition helpers, already written for THUMB `Bcc`) and a short
  forward branch over the instruction body. `cond == NV` → treat as a
  bail/terminator (rare, `PLD`-space on ARMv5). This is the one genuinely new
  control-flow pattern; everything else is a wider version of THUMB work.
* **Full barrel shifter on operand 2** — immediate rotate, register-shift, and
  register-specified shift amount (`Rs`), plus `RRX`. THUMB only had a subset;
  the C-flag semantics per shift type are the fiddly part and must match
  `arm_instructions.cpp` exactly.
* **`S`-bit flag updates** on the data-processing group (incl. the
  `MOVS pc, lr` / `LDM ^` return-from-exception forms → **terminator/bail**,
  they touch SPSR/mode).
* **`MRS`/`MSR`** — `MSR CPSR_f` (flags only) is compilable; anything touching
  control bits (mode/I/F/T) is a terminator that bails to the interpreter.
* **Multiplies** — `MUL`/`MLA` and the 64-bit `UMULL`/`SMULL`/`UMLAL`/`SMLAL`
  (PPC `mullw`/`mulhw`/`mulhwu`).
* **`LDR`/`STR`** with pre/post-index, write-back, `+/-` register offset with
  shift, byte/word (and the `LDRH`/`LDRSB`/`LDRSH` "extra load/store" class);
  same C-call memory path as THUMB in the first cut.
* **`LDM`/`STM`** — all four addressing modes (`IA/IB/DA/DB`), write-back, and
  `pc` in the list (= terminator). The `S`-bit (user-mode bank) form → bail.
* **`B`/`BL`** — PC-relative, foldable to a compiled target; `BL` sets `lr` then
  behaves like `B`.
* **`SWP`/`SWPB`** — compile as an ordered load+store pair.
* **`SWI`** — terminator, hand to `swiHandler` / interpreter.
* **`BX`** — terminator; the mode of the target (bit 0) decides which table the
  next block uses.
* **Coprocessor ops / undefined** — ARM7 has no coprocessor: `MRC`/`MCR`/`CDP`/
  `LDC`/`STC` → undefined-instruction exception. Emit the interpreter bail; it
  already raises the exception correctly.

Port order: (1) predication wrapper + data-processing (imm operand) — smoke test;
(2) barrel shifter; (3) `LDR`/`STR`; (4) `B`/`BL` + block chaining; (5)
`LDM`/`STM`; (6) multiplies; (7) `MRS`/`MSR`/`SWP`; (8) `SWI`/`BX`/undefined
terminators. Differential-test each before the next, same as THUMB.

### 5.3 Test corpus

The DS ARM7 *is* an ARM7TDMI, so GBA CPU-conformance suites are directly
applicable — same core, same ARMv4T semantics, same NZCV behaviour.

* **`jsmolka/gba-tests`** (MIT) — `arm.gba` / `thumb.gba` are the reference
  ARMv4T instruction-conformance ROMs (every data-processing form, shifter
  carry case, multiply, `LDM`/`STM` edge case, PC-relative quirk); `memory.gba`
  covers access widths/alignment. They run all cases and display the number of
  the first failure (BG mode 4). This is exactly the per-instruction coverage
  the differential harness wants.
* **Getting the code onto the ARM7 is the catch.** This Wii port has no DS
  "GBA mode" (Slot-2 cart on the ARM7 with a GBA memory map, ARM9 halted), so
  `arm.gba` won't just boot. Two workable routes:
  1. *Harness ROM* — a small `.nds` whose ARM7 side copies a test payload into
     ARM7 WRAM, jumps to it, and reports the first-failure number to a fixed
     main-RAM word the ARM9 prints / writes to `sd:/jit_test.log`. jsmolka's
     tests already branch to a fail handler with the case number in a register
     — trivial to trap.
  2. *Re-target the sources* — the tests are FASMARM assembly; assemble the
     `arm`/`thumb` test bodies for the DS ARM7 directly into a permanent
     in-tree regression ROM, keeping the run-all / report-first-failure logic.
  Route 1 for P2 bring-up (fast), route 2 as the durable regression asset
  before P5 sign-off.
* Retain the existing pair — the deterministic `vsd` test ROM and retail
  Phantom Hourglass — for whole-system soak; the jsmolka suite is the
  instruction-level net.

---

## 6. Phasing

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **P0** | Vendor files into `source/src/jit/`, Makefile wiring, `-DDESMUME_JIT_ARM7` off by default. Provenance/licence headers preserved. | tree builds clean with and without the flag |
| **P1** | CPU-agnostic refactor: parameterize `JITCache` bank check, introduce `JitCpuProfile`, split VBA's `JITCompiler.cpp` into `jit_trace.*` (scanner/allocator/bailout) + `jit_thumb.cpp` (emitters), retarget flag-bit positions to CPSR, adapt trampoline arg list. | linker stub + trampoline round-trip an empty block, differential harness compiles |
| **P2** | ARM7 THUMB front-end: scanner + opcode groups 1–6 (§5.1), **C-call memory path only**, VBA timing machinery deleted, DeSmuME `cyclesForThumb`. Stand up the differential harness + a jsmolka `thumb` harness ROM (§5.3 route 1). | groups 1–6 pass differential test on ARM7 BIOS boot + `test.nds` + the jsmolka THUMB cases they cover |
| **P3** | Scheduler integration: `armcpu_exec_block<1>`, cycle-quota yield, IRQ/reschedule bailout, aggregate-cycle return into `armInnerLoop`. | full boot to menu with JIT on, no audio/IPC regression vs interpreter |
| **P4** | SMC/DMA write invalidation wired through all ARM7 + aliasing ARM9 write paths. THUMB groups 7–10. | SMC torture: a ROM that rewrites ARM7 IWRAM code runs identically |
| **P5** | Differential soak + benchmark (THUMB only). `tools/benchmark` with a JIT-on column across sw/gx/merge × scenes. Menu toggle (`GCSettings` analogue). **Ported `Profiler` reports THUMB-vs-ARM-vs-fallback instruction mix on 4+ retail ROMs.** | jsmolka `thumb`/`memory` suites all-pass under JIT; zero differential mismatches over a full PH intro + gameplay capture; benchmark delta + coverage numbers reported |
| **P6** | Inline memory fast paths (MAIN_MEM/ERAM/SWIRAM) via `arm7*Page[]`, WRAMCNT rebuild hook. Block chaining tuning, quota tuning. | measurable ARM7-share reduction, still zero mismatches |
| **P7** | **ARM (32-bit) front-end** (`jit_arm.cpp`): predication wrapper + barrel shifter + the encoding classes in §5.2, in the stated port order, each differential-tested. Reuses P1–P6 infra unchanged. Add the jsmolka `arm` harness ROM. | jsmolka `arm` suite all-pass under JIT; ARM groups pass differential test; combined THUMB+ARM coverage and benchmark delta reported |
| **P8** *(separate effort)* | ARM9 front-end: second `JitCpuProfile` (`isaLevel=5`), TCM-aware page tables, ARMv5 opcode additions in the existing `jit_thumb.cpp`/`jit_arm.cpp`, ARM9 pipeline/cache timing model. | its own plan |

P0–P3 is the credible "is this worth it" gate. Stop and measure there; the P5
coverage numbers then decide how hard to push P7 before P8.

---

## 7. Risks & mitigations

* **THUMB-only coverage on ARM7 is probably not enough on its own.** DS ARM7
  vendor sound/IPC/wifi code has a real amount of ARM-mode content (far more than
  GBA, where the hot loops are almost all THUMB in IWRAM). This is *why ARM mode
  is P7, not deferred with ARM9.* → The P5 `Profiler` run quantifies the
  THUMB/ARM/fallback mix on 4+ retail ROMs; if THUMB coverage is weak, P7 is the
  priority and P6 tuning can wait. If it turns out even THUMB+ARM on ARM7 is
  marginal, that data also tells us — cheaply, before ARM9 work starts.
* **Predication overhead in ARM mode.** Wrapping every non-`AL` ARM instruction
  in a flag-test + skip-branch can bloat blocks and add mispredicts. → keep the
  test cheap (packed-flag register is already resident), and for runs of
  same-condition instructions emit one test guarding the whole run where the
  scanner can prove the flags aren't touched between them.
* **Audio pitch/crackle from cycle drift.** ARM7 timing feeds SPU sample pacing.
  → `cyclesForThumb`/`cyclesForArm` must reproduce `armcpu_exec<1>`'s own
  per-instruction counts exactly, not VBA's GBA wait-state model. Differential
  harness compares `cpuTotalTicks`-equivalent per block.
* **WRAMCNT / shared-WRAM routing.** `0x0300_0000` window size and ownership
  flips at runtime via WRAMCNT; a stale JIT page table or inlined mask =
  corruption. → v1 routes `0x03xxxxxx` through the C-call path (which already
  honours WRAMCNT). v2 rebuilds `arm7*Page[]` from the WRAMCNT write handler and
  flushes the block cache on change.
* **Cross-CPU SMC (ARM9 writes ARM7 code in shared RAM).** New vs GBA. → global
  SMC registry + invalidate from `_MMU_write*<0>` to `0x02/0x03` ranges;
  page-flag bitmap makes the common (non-code) case a single load+branch.
* **Broadway I-cache vs 8 MB arena.** Real 750CL has 32 KB L1-I; Dolphin doesn't
  model it, so Dolphin numbers will be optimistic (same caveat as
  `-funroll-loops`). → shrink `JIT_ARENA_SIZE` (try 1–2 MB), keep blocks dense,
  **validate on real hardware** before believing the speedup. Put the arena in a
  fixed, cache-inhibited-free region with proper `DCStoreRange`/`ICInvalidateRange`
  (VBA already does the icbi/isync dance).
* **`-flto` + hand-emitted code.** The emitter output is *data*, not compiled TU
  code — LTO can't touch it. The trampoline is `.S`, also opaque to LTO. Low
  risk, but build both `-DDESMUME_JIT_ARM7` configs under LTO in P0.
* **GPL provenance.** VBA-GX JIT is GPL-2.0+, DeSmuME Wii is GPL-2.0+ —
  compatible. Keep Daryl Borth's file headers, note the port in each file and in
  `README`/`LICENSE.txt` credits.
* **Correctness minefield (banked regs, mode switch, exact flags, SMC).** →
  every mode-affecting op is a block terminator; differential harness is
  non-optional and runs in CI-style soak before each phase sign-off; interpreter
  fallback is always one branch away.

---

## 8. First concrete step

`git checkout -b arm7-jit` off `optimized-Interpretter`, vendor the seven
`JIT*` files into `source/src/jit/`, wire the Makefile behind
`-DDESMUME_JIT_ARM7`, and get a clean build of an *unreferenced* JIT module
(P0). No behaviour change, fully revertible, and it makes the emitter/trampoline
real so P1 can start round-tripping blocks.
