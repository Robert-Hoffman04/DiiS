# ARM9 trace-JIT — the priority target (ARMv5TE, THUMB first)

Status: **A2 landed** (branch `arm9-jit-infra`, 2026-09-03). A0 de-singletonised
the JIT core (`jitCacheArm7` / `jitCacheArm9`, `jitProfile[2]`), built a real ARM9
`JitCpuProfile` skeleton, spliced an inert `jitRunArm9()` into `armInnerLoop`'s
ARM9 arm, fanned the SMC hooks out to both caches and wired the CP15
TCM-relocation flush. A1 hardened the differential harness (write-log journal +
reverse rollback, `untrusted=`/`countDiv=`/`cycDrift=` telemetry, boot
self-test). A2 brought up the ARM9 THUMB front-end:

* `jit_arm9_profile.cpp` — `arm9_canEnterThumb` is a real region check (ITCM /
  main RAM / shared WRAM / ARM9 BIOS; DTCM window excluded because the ARM9
  `MMU_AT_CODE` path returns MAIN_MEM there, not DTCM, so a block scanned from a
  DTCM-shadowed address would compile the wrong bytes). `arm9_cyclesForThumb`
  reproduces `armcpu_exec<ARM9>()`'s own per-op cost with the ARM9 data-access
  model: `max(alu, mem)`, main-RAM word = 4 / half-byte = 2 (M32 = 2 on the
  32-bit bus), lists = `max(alu, 4·n)`. v1 assumes main RAM; the harness's
  `cycDrift` quantifies the error.
* THUMB `BLX` — `jit_thumb.cpp` case 8 (BLX reg, sibling of BX Rs + unconditional
  LR write) and case 30 (BLX imm, sibling of BL with word-aligned target + clear
  CPSR.T; exits to the interpreter for the ARM code until A5). Also fixes a
  latent ARM7 bug: `0x47xx` with bit 7 set was silently compiled as plain BX,
  dropping the LR write — DeSmuME's shared THUMB table executes BLX on ARM7 too.
* `jitRunArm9Checked` — the A1 journal generalised: `jitRunChecked()` is now
  shared by both cores (core threaded as an `armcpu_exec<>` fn-ptr), the journal
  decodes ARM9 RAM (DTCM / ITCM / main / shared WRAM), ARM9 stats report under
  the `diff9` tag. In a `JIT_DIFFERENTIAL_TESTING` build the ARM9 JIT runs
  regardless of `jitArm9Enabled` (still default-off for production) so a full
  capture exercises it.
* Profiler — `jit_exec.cpp` tallies every ARM9 step as ARM / THUMB-region-out /
  THUMB-JIT and reports the mix (the VBA `Profiler` port).

Verified: JIT-off `.dol` byte-identical (2246048); JIT-on 2306720 (+1728 over A0
— BLX + real cycle model, harness code still out); differential build links
clean (2303296). 150 s PH differential soak: `selftest PASS`, `journal selftest
PASS`, **0 DIFF / 0 diff9 DIFF / 0 ARENA OVERRUN**, clean exit. Profiler:
**PH's boot/intro is ~99–100 % ARM-mode on the ARM9** — THUMB rounds to 0 %, so
neither core ran ≥100 k THUMB blocks (no `diff alive` / `diff9 alive` line), the
same "boot is ARM-heavy" result the ARM7 P5 soak hit. The ARM9 THUMB path is
code-complete, non-regressing and crash-free, but **live THUMB coverage is still
unverified** — needs gameplay past the intro (input automation) or the jsmolka
`thumb` payload retargeted to the ARM9. Next: finish A2's coverage (jsmolka /
gameplay capture) then A3 (benchmark gate).

Reprioritised 2026-09-03: the ARM9 is where the
~3× bottleneck lives ([desmumewii-perf-opportunities.md](desmumewii-perf-opportunities.md)
§1.2), and the ARM7 JIT has so far shown only performance *parity* on Phantom
Hourglass (arm7-jit plan, P5). So ARM9 moves ahead of the remaining ARM7 phases.

This supersedes the "P8 *(separate effort)*" line in
[desmumewii-arm7-jit-plan.md](desmumewii-arm7-jit-plan.md). The ARM7 JIT is **not
abandoned** — it stays wired as a low-stakes continuous proving ground for the
shared emitter core while the ARM9 front-end comes up. ARM7 P5 (runaway) and P7
(ARM-mode front-end) continue on their own track but are **no longer on the ARM9
critical path**.

**Decisions locked (2026-09-03):**
* **Dual-core.** Do the de-singletonise refactor; keep `jitCacheArm7` running.
* **THUMB-only ARM9 first.** Reuse `jit_thumb.cpp` almost as-is (ARMv4T THUMB ==
  ARMv5 THUMB + `BLX`), get a real perf number, *then* decide how hard to push
  the ARM-mode front-end based on measured coverage.

---

## 1. Why this is safe enough to move up

The blast-radius argument that put ARM7 first still stands — an ARM9 miscompile
corrupts the whole game, not just audio. What changed is that the mitigations are
now concrete and the ARM7 work already de-risked the hard 90 %:

* **The shared core is battle-tested.** `JITCache` + arena + linker stub +
  trampoline + `jit_trace.*` scanner + lazy register allocator + deferred
  bailouts ran ~143 M ARM7 instructions with zero genuine differential
  mismatches. `jit_thumb.cpp` passes 59/59 synthetic vectors and every THUMB
  format is implemented.
* **THUMB is ISA-identical.** ARMv4T THUMB and ARMv5 THUMB differ only by `BLX`.
  `jit_thumb.cpp` is reused essentially unchanged — this is not a new front-end,
  it is a new *profile* plus one opcode.
* **The `JitCpuProfile` seam was built for exactly this.** `jit_cpu_state`
  already carries per-core `gpr`/`cpsr`/`readTable` pointers; the trampoline and
  emitted code are already CPU-neutral.
* **Correctness is contained by construction:** every mode-affecting op is a
  block terminator, the interpreter fallback is one branch away, and
  `jitArm9Enabled` stays **default-off** through a multi-ROM differential soak.

**The one real precondition** (§2, phase A1): the differential harness gets
guest-memory snapshotting before it is trusted to sign off ARM9. The ARM7
harness's store-double-apply and mode-switch blind spots are tolerable on the
sound core; they are not on the game core. This same hardening is also the best
remaining tool for the ARM7 P5 runaway (that investigation stalled on
*tooling* — instrument/rebuild/rerun with no debugger — not on ideas).

---

## 2. Phasing

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **A0** | **De-singletonise the JIT core.** `jitCacheArm7` + `jitCacheArm9` (each its own arena, block hash, SMC registry, page-flags, linker-stub pair); `jitActiveProfile` → `jitProfile[2]`; `jitInit()` builds both; SMC hooks fan out to both caches for shared regions; add the CP15 TCM-relocation `flushCache` hook (even though nothing compiles yet). `jitRunArm9()` wired into the ARM9 arm of `armInnerLoop`, inert. | ARM7 differential soak **byte-identical** to pre-refactor; ARM9 JIT compiled, `jitArm9Enabled=false`, `canEnter*` false; JIT-off `.dol` == 2246048 |
| **A1** ✅ | **Harden the differential harness.** *Done:* write-log + reverse rollback (`jitDiffJournalNote()` in the three `_MMU_write*` choke points, replayed in `jitRunArm7Checked()`); non-RAM writes bounded via `s_journalUnrestorable` + `untrusted=` counter (replaces the blanket "any store opcode ⇒ skip"); instruction-count divergence surfaced as `countDiv=`; per-block cycle compare → `cycDrift=N blk (sum= max=)`. Boot-time `jitDiffJournalSelfTest()` proves record/overlap/rollback/overflow/unrestorable since the PH boot path runs too few ARM7 THUMB blocks to exercise it live. | ✅ journal selftest PASS; 90 s PH soak identical to A0; JIT-off/JIT-on `.dol` sizes unchanged |
| **A2** 🟡 | **ARM9 THUMB front-end.** *Done:* `jit_arm9_profile.cpp` region check + real `max(alu,mem)` cycle model; THUMB `BLX` imm+reg in `jit_thumb.cpp` (also fixes a latent ARM7 BLX-reg-as-BX bug); `jitRunArm9Checked` sharing `jitRunChecked()` with the ARM7 path, journal generalised to ARM9 RAM, `diff9` telemetry; Profiler (ARM/THUMB-region-out/THUMB-JIT mix). ARM9 JIT runs in the differential build regardless of `jitArm9Enabled`. *Pending:* live THUMB coverage — PH intro is ~99–100 % ARM-mode, so needs a gameplay capture or the jsmolka `thumb` payload retargeted to ARM9. | ✅ builds clean, JIT-off byte-identical, 150 s PH soak 0 DIFF / 0 diff9 DIFF / 0 overruns, both self-tests PASS; ⬜ jsmolka `thumb` on ARM9; ⬜ genuine THUMB mismatch count from a THUMB-bearing capture; `jitArm9Enabled` still off |
| **A3** 🟡 | **Measure — the real "is it worth it" gate.** *Done (`657008d` infra, `bf95a03` diag):* `benchmark.sh` `jit9off`/`jit9on` modes (sw renderer); `DESMUME_JIT_ARM9_ON` compile guard (start `jitArm9Enabled` true without changing the default); `DESMUME_JIT_ARM9_THUMB_ONLY` isolation knob; per-dispatch ARM9 diagnostics. | **VERDICT: not a win — do NOT flip.** vsd (ARM9 ~85 % THUMB): `jit9on` stable but **~1.5× slower** than the interpreter at steady state (5.79 vs 3.97 s / 60-frame block). Phantom Hourglass (ARM9 ~99 % ARM-mode): `jit9on` **effectively hangs** — 0 bench frames in 300 s (interp: 60 frames / 3.4 s), stuck cycling a fixed ~347-block boot spin-loop at `0x02000940`, ~94 k guest-insn/s (**~70–90× slower**), no wild PC / no compile thrash / no bail storm. **Root cause is architectural, not a B1–B7c bug** (B1–B7c are differential-clean; a first-pass "B7c regressed vsd" was host-contention noise — a re-run of the same build is stable): each block does a full trampoline round-trip (`stmw`/`lmw` of 18 host non-volatiles + guest-reg reload + flag/reg flush) for often ~1 guest instruction, because `JIT_ENABLE_CHAINING = 0` and `armInnerLoop` runs one block per interleave turn. Same wall the ARM7 P5 JIT hit. jsmolka `arm.gba` harness ROM still unbuilt. |
| **A5·B1–B7c** 🟡 | **ARM-mode front-end — DP (imm / reg-shift-imm / reg-shift-reg) + branches + LDR/STR + halfword/signed ld/st + LDRD/STRD + LDM/STM + multiplies (incl. DSP) + misc + CLZ/BLXimm/PLD + QADD family + PC-write interworking.** B7c (`074c769`): `LDR pc,[...]` (word) is a block terminator with ARMv5 `LDTBit` interworking — rotate the loaded word, bit0 → resume ISA (THUMB path sets CPSR.T in the packed flags), `R15 = word & ~1` — the LDM{pc}/BX two-path shape; done for the general **and** the `[pc,#imm]` literal forms (`STR pc` / `LDRB pc` still bail). Non-S data-processing writing `Rd == 15` (`MOV pc,lr`, `ADD pc,pc,rN` jump tables) = a plain ARM-mode jump to the ALU result — **no mask, no mode switch** (matches `OP_xxx`'s `next_instruction = result` exactly, not word-aligned); a compile-time-constant target (`MOV`/`MVN #imm`, `ADD`/`SUB pc,#imm`) takes a static exit so the block chains. S-form (`MOVS`/`SUBS pc,lr` = SPSR→CPSR exception return) and any predicated PC write end the trace. `arm9_cyclesForArm`: `LDR pc` 5 (`OP_LDR`'s b path), data-proc→pc 3 (imm / shift-by-imm operand2) or 4 (shift-by-register); the 10x0 / S=0 control space (MRS/MSR) is excluded so it keeps its flat 1. 210 s PH soak: 0 DIFF / 0 ARENA OVERRUN, 72.9 M blk / 171.7 M insns checked, both self-tests PASS, countDiv 1, cycDrift flat (~24 M blk, unchanged THUMB coarseness); arm-jit 39 %→2 % over the run as PH crosses into THUMB gameplay. B7 (`7cd8068`): CLZ (`cntlzw`, exact incl. `cntlzw(0)==32`); BLX imm (the `cond==NV` branch routes it — `R14=currentPC+4`, `CPSR.T=1`, compile-time-constant THUMB target via a dynamic exit); PLD (hint → emit nothing; `arm9_cyclesForArm` returns 1 for the whole `cond==NV` space); LDRD/STRD (two word accesses at addr / addr+4 into `{Rd,Rd+1}`, no unaligned rotate, SMC guard on the base page, writeback committed after the access; bails on odd `Rd`, `Rd>=14`, pc base, register-offset pc, and `Rn`/`Rd`/`Rd+1` aliasing; `!P && W` → interp). `cyclesForArm`: LDRD/STRD 8, CLZ 2. 210 s soak: 0 DIFF / 0 ARENA OVERRUN, 63.8 M blk / 150.5 M insns, self-tests PASS. B7b (`6fad866`): QADD/QSUB/QDADD/QDSUB — PPC add/subf with OE give `XER[OV]` == the ARM signed over/underflow, saturate to `0x80000000 + (raw >> 31 arith)` (== `0x80000000 - BIT31(res)`), **Q sticky** at CPSR bit 27 (rlwimi into IBM bit 4, forward-skip so Q is untouched with no saturation), the QD* doubling step saturates + sets Q independently; operand order matches the interpreter (`Rn` = bits 19..16, `Rm` = bits 3..0, QSUB/QDSUB = `Rm - (Rn term)`). SMUL/SMLA/SMLAL/SMULW/SMLAW `<x><y>` — `x` = bit5 (Rm half), `y` = bit6 (Rs half), `EXTSH` / `SRAWI #16` pick the half, `mullw` low word is the exact half*half; SM*W = `(mullw + mulhw) >> 16` rotate-merge; SMLA/SMLAW set Q on `XER[OV]`; SMLAL replicates `OP_SMLAL_*`'s exact (bug-compatible) accumulate (`RdLo_new = tmpLo + RdLo`; `RdHi_new = RdHi + RdLo_new + (tmp<0 ? -1 : 0)`). cond == AL only; pc operands / `RdHi==RdLo` / `Rm`-aliases-`Rd{Lo,Hi}` bail. The harness compares only NZCV, not Q — Q correctness is by construction (surfaces indirectly via a later `MRS`). **PC-writer audit:** every B1–B6 pc-writing emitter ends the trace (data-proc → pc, LDR → pc, extra ld/st → pc, multiply → pc, QADD → pc); only `LDM{pc}` (B4) and `BX`/`BLX reg` (B6) compile a pc write, both through the ARMv5 bit0 interworking branch. `LDR pc` / `MOV pc,lr` interworking landed in B7c (above). `cyclesForArm`: QADD family + all SM* = 2. 210 s soak: 0 DIFF / 0 ARENA OVERRUN, 57.6 M blk / 136 M insns, self-tests PASS. B6b: data-processing register form with `bit4==1 && bit7==0` — operand2 = `Rm` shifted by `Rs & 0xFF`, routed through the shared ALU core (`emitOp2ShiftReg`). The runtime shift amount forces runtime branches for the shifter carry; the emitter reproduces `arm_instructions.cpp`'s `LSL/LSR/ASR/ROR _REG` macros exactly — amount 0 (value = `Rm`, C unchanged), LSL/LSR 32 (0, C = bit0/bit31 of `Rm`), > 32 (0, 0), ASR ≥ 32 (sign-fill, C = bit31), ROR (`&= 0x1F`, then 0 → value = `Rm`, C = bit31). `≥ 32` is `(sh & 0xE0)` since `sh = Rs & 0xFF`; PPC `SLW`/`SRW`/`SRAW` self-zero/self-sign-fill for a 6-bit count but `Rs & 0xFF` can exceed 63, so the count is still range-checked. Non-S forms skip the carry work. Bails: `Rn`/`Rm`/`Rs == pc` (a register-shifted instruction reads pc as pipeline + 12) plus everything B2 bailed on; `bit7==1` stays end-of-trace (multiply / SWP / extra ld-st / BX space). `arm9_cyclesForArm` register-shift DP row = 2 (1S + 1I). New PPC macro `PPC_SUBFIC`. 210 s PH soak: 0 DIFF / 0 ARENA OVERRUN, 79.9 M ARM9 blocks / 188 M insns, 0 mismatches, self-tests PASS, countDiv 1, cycDrift ~26.4 M blk (max 24 — up from B6's ~21.7 M only because shift-by-register now extends traces instead of ending them; same per-instruction coarseness). Commit `5a0988e`. B6: `BX`/`BLX reg` (block terminator, ARMv5 bit0 interworking — LDM{pc} two-path shape; BLX writes R14; dirty regs flushed once before the exit split so a per-path flush can't drop the writeback on the second path — this was caught in soak as `AND R0,R0,#x ; BX lr` leaving R0 stale); `SWP`/`SWPB` (ordered load-then-store at [Rn], word rotates, SMC guard before either access, not a terminator); `MRS Rd,CPSR` (= the packed flags word); `MSR CPSR_f` (insert operand[31:24] into the packed flags; only the flags-byte field + CPSR compiles, skips the interpreter's changeCPSR()/NDS_Reschedule since a flags-only write has no I/F/mode edge). SPSR forms, non-flag MSR fields, BKPT, CLZ, QADD, DSP muls, SWI, coprocessor all end the trace. cond == AL only. Shift-by-register operand2 stays deferred to B6b. 210 s PH soak: 0 DIFF / 0 ARENA OVERRUN, 66.0 M ARM9 blocks / 155.6 M insns, 0 mismatches, self-tests PASS, cycDrift flat vs B5 (~21.7 M blk, max 24), countDiv 1. Commit `9a4eb78`. B5: `MUL`/`MLA` (32-bit, `PPC_MULLW`) and `UMULL`/`SMULL`/`UMLAL`/`SMLAL` (64-bit — `PPC_MULHW`/`PPC_MULHWU` added); the long accumulate is an `addc`/`adde` pair on the {lo,hi} register pair (interpreter carry-from-low model); `S` sets N/Z only (C, V untouched, as `OP_MUL_S`/`OP_UMULL_S`), long Z = both halves zero. Bails: predicated (`cond != AL`), any operand/destination == pc, long form with `RdHi == RdLo` or `Rm` aliasing either destination (ARM-unpredictable). Fixed per-form cycle estimate (interpreter cost is data-dependent on Rs). 200 s PH soak: 0 DIFF / 0 ARENA OVERRUN, 62.1 M ARM9 blocks / 146.5 M insns, 0 mismatches, both self-tests PASS, `cycDrift` flat vs B4 (~20.4 M blk, max 24), `countDiv` 1. Commit `cf1d302`. B4: `LDM`/`STM` all four addressing modes (`IA`/`IB`/`DA`/`DB`), optional writeback; the touched addresses are one ascending block (lowest register → lowest address) with writeback = base ± 4·n; `pc` in an `LDM` list is a block terminator with ARMv5 bit0 interworking (the common function return — mirrors the THUMB `POP{pc}` shape); the `S` (user-bank/CPSR-restore) form, empty list, `STM{pc}`, base == pc and base-in-list + writeback all end the trace; `cond == AL` only. 200 s PH soak: 0 DIFF / 0 ARENA OVERRUN, 62.9 M ARM9 blocks / 148 M insns checked, both self-tests PASS, `cycDrift` flat vs B3b (~20 M blk, the known THUMB "assume main RAM" coarseness), `countDiv` 1 (benign, carried from B3b). B3: `LDR`/`STR` word/byte, imm or register(shift-by-imm) offset, pre/post-index, writeback, unaligned-word-load rotate, pc-relative literal → constant EA; `cond == AL` only (predicated ends the trace); halfword/signed → B3b. Harness gained `jitDiffJournalNoteRead()` (I/O-bank data read during the reference run → block untrusted; IPC-FIFO-pop is a read side effect, the analogue of A1's store-double-apply). PH now boots **past the ARM intro into THUMB gameplay** under the JIT (arm-jit 99 %→0 %). `cycDrift` ≈ 18 M blocks (avg 1) — the *THUMB* "assume main RAM" cycle model exercised for the first time; a v2 runtime-EA penalty is the real fix.<br>**B1–B2 core:** Mode-aware scanner (`jitCompileTrace(..., bool thumb)`, `JitTraceCtx::thumbMode`, ARM `fetch32`/`PC+=4`/`cyclesForArm`, ARM reserve constant); `BasicBlock::length` bit-31 = compiled ISA, mode-mismatched hit recompiles; SMC walk uses `byteLength()`. `jit_arm.cpp`: shared exit helpers hoisted to `jit_trace.cpp`; `emitEvalCond` (14 ARM conditions → 0/1); per-instruction **predication wrapper** (skip-body for non-terminators with destinations pre-allocated; THUMB-`Bcc`-shape conditional exit for `B`/`BL`; `NV`→interp); **data-processing** — immediate operand2 (B1) **and register operand2 shifted by an immediate** (B2: LSL/LSR/ASR/ROR #n + `RRX`, C-flag matches the `*_IMM` macros exactly); all 16 ALU ops × `S`; `MOV`/`MVN`/ADR constant-folded; `Rd==15`→end; shift-**by-register** (bit4==1) deferred → end. `B`/`BL`. Wired into `jitRunArm9()` only (`arm7_canEnterArm` stays false); `jit_differential.cpp` reference run mode-aware. `arm9_canEnterArm` region check + flat `arm9_cyclesForArm`. | ✅ JIT-off byte-identical (2246048); JIT-on 2325856; differential links clean (2321888). 150–280 s PH soak: selftest PASS, journal selftest PASS, **>170 M ARM9 insns through the *checked* ARM-mode JIT, 0 DIFF / 0 countDiv / 0 ARENA OVERRUN**, clean exit; `cycDrift` 297 blk (sum 839, max 20) all one-shot boot code, flat thereafter; **arm9 mix arm-jit ≈ 99–100 %** (THUMB was 0 %). Insns/block **1.0 (B1) → 2.0 (B2)**. B3 (ld/st) + B4 (LDM/STM) grow blocks further. |
| **A4** ⛔ BLOCKED | **Build the perf layer, THEN flip.** A3 showed the JIT is trampoline-bound, so default-on is off the table until, in leverage order: **(1)** block chaining (`JIT_ENABLE_CHAINING`; the infra is already present — `linkerStub`/`linkerReturn`, `emitStaticExit`'s `#if` BL); **(2)** `armcpu_exec_block(quota)` — run many chained blocks per `armInnerLoop` turn instead of one (this doc's own "Scheduler" item, never built); **(3)** inline main-RAM load/store fast path (v2, never built); **(4)** skip the full 18-register save/restore for small blocks. Then: multi-ROM differential soak; hardware validation of the arena. | chaining + block-quota land and PH `jit9on` reaches gameplay at ≥ interpreter speed; zero genuine mismatches across 4+ ROMs; runs on real hardware; only then `jitArm9Enabled` → default-on |
| **A5** | **ARM-mode front-end** (`jit_arm.cpp`, the old ARM7 P7) — now serving both cores, ARM9 primarily — in the port order from the ARM7 plan §5.2, each group differential-tested. Then the **ARMv5TE ARM delta** (§4): CLZ, BLX, DSP multiplies, QADD family + Q flag, LDRD/STRD, PLD, BKPT, CP15/coproc = hard terminator, and the ARMv5 PC-interworking switch on LDM/LDR/data-proc. | jsmolka `arm` payload passes on ARM9; ARMv5 conformance payload passes; combined THUMB+ARM coverage and benchmark delta reported |

Phases A0 and A1 have no dependency on each other or on any open ARM7 item and
can run in parallel starting now.

---

## 3. `jit_arm9_profile.cpp` — the ARM9 JitCpuProfile

Mirror `jit_arm7_profile.cpp`. The only file that knows DeSmuME's ARM9 memory
map and state layout.

### 3.1 State

```
state.gpr  = &NDS_ARM9.R[0];
state.cpsr = &NDS_ARM9.CPSR.val;   // packed NZCV bits 31..28 (== ARM), Q at 27
isaLevel   = 5;                     // ARMv5TE
```

Banked-register handling stays entirely in the interpreter, same rule as ARM7:
any mode-affecting op (`MSR` control bits, mode-changing `BX`/`BLX`, `SWI`,
`MOVS pc,lr`, `LDM ^`) is a block terminator.

### 3.2 Compile-time fetch

`fetch16/32` → `_MMU_read{16,32}<ARMCPU_ARM9, MMU_AT_CODE>(addr & ~mask)`.
Already routes correctly (`MMU.h` 689–698, 731–738):

* `(addr & 0x0F000000) == 0x02000000` → main RAM (shared)
* `addr < 0x02000000` → **ITCM**, crudely modelled as
  `MMU.ARM9_ITCM[addr & 0x7FFE/0x7FFC]` — the whole `0x00000000–0x01FFFFFF` range
  mirrors a 32 KB ITCM, CP15 ITCM size ignored. The JIT inherits this for free.
* else → `_MMU_ARM9_read*` (I/O, VRAM, palette, OAM, GBA slot, BIOS)
* **DTCM as code returns garbage** → `canEnter*` must exclude the DTCM window.

### 3.3 Runtime guest memory (slow path)

`slowRead/slowWrite` → `_MMU_{read,write}{08,16,32}<ARMCPU_ARM9>` (MMU_AT_DATA):
routes DTCM (incl. DTCM-patched-over-main-RAM priority), main RAM, shared WRAM,
VRAM bank mapping, I/O. **v1: every ARM9 load/store emits a C-call here**,
exactly as ARM7 v1 did. Inline fast paths are a later refinement (analogue of the
ARM7 plan's P6), scoped after A3.

`smcInvalidate` → `jitCacheArm9.invalidateSMCTarget`.

### 3.4 canEnter

True iff `pc` is in: ITCM window (`pc < 0x02000000`), main RAM
(`(pc & 0x0F000000) == 0x02000000`), shared WRAM (`(pc >> 24) == 0x03`), or ARM9
BIOS (`(pc & 0xFFFF0000) == 0xFFFF0000`). False for the DTCM window and
everything else. For A2, `canEnterThumb` only — `canEnterArm` stays false until A5.

### 3.5 Cycle model — the ARM9-specific hard part

ARM7 could delete VBA's timing model wholesale; **ARM9 cannot**:

* Code-fetch cycles: still off (`ACCOUNT_FOR_CODE_FETCH_CYCLES` undefined →
  `Fetch()` returns 1). The dominant term is flat on both sides — good.
* **Data-access cycles: modelled.** `_MMU_accesstime` (`MMU_timing.h` 258):
  `MC=1` ITCM/DTCM/cached, `M32=2` for the ARM9 32-bit bus, `M16`, `MSLW=16` for
  slow-wait regions. Combined via `MMU_aluMemCycles<0>` = `max(alu, mem)` (5-stage
  pipeline approximation).
* So an ARM9 load/store's interpreter cost is `max(alu, accesstime(runtime EA))`
  — address-dependent, unknowable at compile time for register-indexed
  addressing.

Approach:

* **v1 (A2):** assume the common case (cached / main RAM), cost
  `max(alu, 2)` word / `max(alu, 1..2)` half/byte. Because the model is
  `max(alu, mem)`, many loads cost just `alu` and the coarse error is often
  *zero*; where wrong it is bounded (`MSLW` regions). The hardened harness's
  per-block cycle compare (A1) quantifies the drift.
* **v2 (post-A3, if drift matters):** revive VBA's `EmitDynamicNCyclePenalty`
  (deleted for ARM7, still in `jit/upstream/`) — a short range check on the
  computed EA adding `MSLW − M32` when the access hit a slow region.

---

## 4. Opcode delta over `jit_thumb.cpp`

### THUMB (A2 — the only additions needed to run ARM9 THUMB)

The DS ARM9 THUMB set is ARMv4T + `BLX`. Both encodings are siblings of code
`jit_thumb.cpp` already has:

* `BLX (imm)` — sibling of `BL` (Format 19, done in P2b) with H==01: adds "clear
  CPSR.T, word-align target". **Terminator**, next block is ARM mode → until A5
  lands, the block just exits and the interpreter takes the ARM code. Still a
  net win (the THUMB run up to the call is native).
* `BLX (reg)` in Format 5 — sibling of `BX Rs` (P2b) with an LR write.
  Terminator; target bit0 picks the next block's mode.

Everything else in THUMB is ARMv4T-identical on ARMv5 — `jit_thumb.cpp` is reused
verbatim. THUMB is already interworking-aware (`POP {pc}`, `BX` consult bit0), so
the ARMv5 PC-interworking change (§ below) does **not** affect the THUMB path.

### ARM mode ARMv5TE delta (A5 — deferred)

`isaLevel == 5` branches inside `jit_arm.cpp`. Interpreter already implements all
of these (`instruction_tabdef.inc`: `OP_CLZ`, `OP_BLX_REG/IMM`, `OP_QADD/QSUB`,
`OP_SMLA_x_y`, `OP_SMLAL_x_y`, `OP_SMLAW_x`, `OP_SMULW_x`, LDRD/STRD).

| Op | Emit | Notes |
|---|---|---|
| `BLX (imm/reg)` | LR write + `BX`-style exit | terminator; on ARMv4T this was `cond==NV`/undefined so purely additive |
| `CLZ Rd,Rm` | one PPC `cntlzw` | trivial |
| `QADD/QSUB/QDADD/QDSUB` | saturating add/sub + **Q sticky flag** | PPC has no saturating add — overflow-detect + clamp, then `rlwimi` Q into CPSR **bit 27** (below the N/Z/C/V nibble the flag emitter touches — needs a dedicated merge in `jit_trace.cpp`) |
| `SMLA<x><y>`, `SMLAW<y>`, `SMULW<y>`, `SMUL<x><y>`, `SMLAL<x><y>` | sign-extend selected halves, `mullw`/`mulhw`; accumulate forms can set Q | 16×16 / 32×16 signed DSP multiplies |
| `LDRD`/`STRD` | even/odd pair as two 32-bit slow-path calls | check v5 alignment-fault behaviour |
| `PLD` | nothing, advance PC | hint |
| `MCR/MRC p15` | **hard terminator / bail** | CP15 writes reconfigure TCM/cache/protection — must go through `armcp15_moveARM2CP` (`cp15.cpp`) |
| `MCR/MRC` other, `CDP/LDC/STC`, `BKPT` | interpreter bail | undefined-instruction / prefetch-abort |

**ARMv5 PC-interworking (ARM mode, A5):** on ARMv5, `LDM`/`LDR`/data-proc writing
PC interworks — bit0 of the value selects THUMB — whereas ARMv4 forces ARM. The
`jit_arm.cpp` emitters (written for ARM7/ARMv4T in P7) get an `isaLevel` switch on
every PC-writing path. Same silent-control-flow-corruption class as the ARM7
`POP{...,PC}` bug — audit every PC-writing emitter.

---

## 5. SMC / cache coherency (ARM9)

Cache-controller emulation is off — guest writes are immediately visible to the
interpreter, so only the JIT block cache must be invalidated. ARM9 code lives in
**ITCM, main RAM, shared WRAM**. Hooks:

* **main RAM write, any PROCNUM** — the P4 hook (`MMU.h` 797–808) already fires
  for ARM9 stores; A0 fans it out to `jitCacheArm9`. Covers ARM7↔ARM9 cross-CPU
  SMC and DMA (both funnel through `_MMU_write*`).
* **shared WRAM** — add an ARM9 analogue of the P4 `_MMU_ARM7_write*` bank-3 hook
  in the `_MMU_ARM9_write*` path.
* **ITCM writes** — new, ARM9-only, in `_MMU_ARM9_write*` for `addr < 0x02000000`;
  mirror the crude `addr & 0x7FFF` model. Games load ITCM at boot then rarely
  rewrite it, but overlay systems exist.
* **CP15 TCM relocation / enable** (`cp15.cpp` 575–580 — `DTCMRegion`/`ITCMRegion`
  writes; also TCM-enable bits in the control register): **flush `jitCacheArm9`
  entirely.** A moved ITCM window silently changes which guest bytes back every
  cached block. **Land this in A0.**
* ARM9 DMA into ITCM/DTCM is already discarded by the MMU (`addr < 0x02000000`
  guard in the `MMU_AT_DMA` branches) — one less path.

`smcBankMask` start = `(1<<0)|(1<<2)|(1<<3)` plus the CP15-write full-flush (the
static mask can't track relocatable TCM; a relocated DTCM sharing a tracked bank
just costs a wasted empty-bucket walk).

---

## 6. Scheduler integration and blast radius

`armInnerLoop` already interleaves per-instruction. Add `jitRunArm9()` to the
ARM9 arm mirroring the ARM7 splice: one block/turn, `JIT_ENABLE_CHAINING` off,
small cycle quota, immediate bail on `sequencer.reschedule` / `waitIRQ` /
deliverable IRQ. IRQ delivery and `Wait4IRQ` stay in C, between blocks only.

Because ARM9 is the game-logic core: `jitArm9Enabled` **default-off** until A4;
the differential harness stays on far longer and compares per-block cycle counts;
the A1 memory-snapshot hardening is non-negotiable.

---

## 7. Risks (ARM9-specific, on top of the ARM7 plan's list)

* **Blast radius** — ARM9 miscompile = whole game. Mitigations: hardened harness
  (A1), long default-off, per-phase retail soak.
* **The P5-class runaway is still open** in the shared emitters. Mitigation: A1
  gives it a real tool; A4 gates default-on behind resolving/bounding it; the
  ARM7 JIT keeps exercising the same code in the meantime.
* **ARMv5 PC-interworking** (A5) — silent control-flow corruption. Audit every
  PC-writing emitter's `isaLevel` branch. Does not affect the A2 THUMB path.
* **TCM relocation** invalidating cached blocks — CP15-write full-flush, landed
  in A0.
* **Cycle-model drift** feeding VCount/DMA/IRQ pacing → visual glitches, not just
  audio. Per-block cycle compare from A1; v2 penalty model if A3 shows it matters.
* **Two arenas** — MEM1/MEM2 headroom on Wii. Measure with both allocated before
  sizing the ARM9 arena up. Broadway 32 KB I-cache now has two arenas competing —
  keep ARM9 modest (2–4 MB), validate on hardware (A4).
* **jsmolka payload on ARM9** — no "GBA mode" on the DS ARM9; needs the
  harness-ROM route (ARM7 plan §5.3 route 1) retargeted to the ARM9 side.
* **THUMB-only coverage might be lower than hoped even on ARM9.** The Profiler
  run in A2 quantifies it *before* A3's benchmark; if THUMB alone is marginal,
  A5 (ARM mode) jumps the queue.
* **GPL provenance** unchanged — keep Daryl Borth's headers.

---

## 8. First concrete step

**A0**, on a parallel branch `arm9-jit-infra` off current `arm7-jit` HEAD. Pure
refactor: split `JITCache`/`jitActiveProfile`/`jitInit` two ways, fan out the SMC
hooks, add the CP15 TCM-relocation flush, wire an inert `jitRunArm9()`. No
emitter work, no behaviour change with the ARM9 JIT disabled, validated entirely
by re-running the existing ARM7 differential soak. **A1** (harness memory
snapshot + cycle compare) can start the same day, independently.
