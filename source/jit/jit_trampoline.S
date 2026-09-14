/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_trampoline.S
 *
 * Hand-written PowerPC ABI bridge between the C++ emulator core and compiled
 * JIT traces. Derived from Visual Boy Advance GX source/vba/gba/JITTrampoline.S
 * (c) Daryl Borth, GPL v2+ -- see jit/upstream/PROVENANCE.md.
 *
 * P1 changes vs upstream:
 *   - New C signature: ExecuteJITTrace(JITBlockFunc execute, JITResult* out,
 *                                      jit_cpu_state* st).
 *     The three loose pointers VBA passed (gbaRegs, flags-array, readTable)
 *     plus its busPrefetchCount are replaced by one jit_cpu_state* whose
 *     layout is { u32* gpr; u32* cpsr; u8** readTable; } (see
 *     jit_cpu_profile.h -- LAYOUT IS ABI).
 *   - busPrefetchCount is gone: the DS ARM7 has no modelled prefetch buffer
 *     (MMU_timing.h leaves ARM7 access timing disabled), so R5 is free scratch.
 *   - Flags: VBA faulted four separate flag words from a base pointer; here a
 *     single guest CPSR word pointer sits at 84(r1) and the emitter (P2)
 *     masks/inserts the N/Z/C/V nibble (CPSR bits 31..28).
 *
 * Stack frame (128 bytes), offsets referenced by emitted trace code:
 *    80(r1)  guest gpr base pointer   (== r14 for the whole trace)
 *    84(r1)  guest cpsr word pointer
 *    88(r1)  JITResult* out
 *     8(r1)  saved host r14..r31
 *   132(r1)  saved host LR   (128 + 4, in the caller-provided linkage area)
 *
 * Register contract during a trace (see jit_ppc_emitter.h for the full map):
 *   r14      guest gpr base pointer (non-volatile, live entire trace)
 *   r15..r28 lazily-cached guest R0..R14
 *   r29      guest R15 / pipeline PC
 *   r30      guest read page table base (nullptr until P6)
 *   r30      packed N/Z/C/V == guest CPSR (P12): resident for the whole trace,
 *            non-volatile so it survives the slow-memory C calls. Loaded from
 *            *cpsr on entry, stored back once, here, on return -- every exit
 *            path lands here, so no block ever loads or flushes flags itself.
 *   r31      resident guest-instruction-count accumulator (P12): every block's
 *            clean/bail exit does `addi 31,31,count` instead of a load/add/store
 *            round-trip through out->instructions; the trampoline zeroes it on
 *            entry and writes it back to out->instructions once, here, on return.
 *   r3       cycle accumulator (out); r4  next PC (out) / scratch
 ***************************************************************************/

#if defined(DESMUME_JIT_ARM7)

.global ExecuteJITTrace
.type ExecuteJITTrace, @function

# ExecuteJITTrace(r3 = JITBlockFunc execute,
#                 r4 = JITResult*   out,
#                 r5 = jit_cpu_state* st)
ExecuteJITTrace:
    mflr 0
    stw 0, 4(1)                  # save LR into caller linkage area

    stwu 1, -128(1)             # new frame
    stmw 14, 8(1)               # save host non-volatiles r14..r31

    # GPR residency: guest R0..R15 live in r14..r29 for the whole (chained)
    # trace, CPSR in r30 (== &R[0] + 16, contiguous in armcpu_t), synced to
    # memory only here. One lmw covers all of it -- r31 briefly holds SPSR
    # (&R[0] + 17), immediately zeroed as the P12 instruction-count accumulator.
    # r3 = execute, r4 = JITResult* out, r5 = st -- keep all three until used.
    lwz 10, 0(5)              # r10 = st->gpr  (== &cpu.R[0])
    lwz 11, 4(5)              # r11 = st->cpsr

    stw 10, 80(1)            # [80] guest gpr base -- reloaded by the few cold paths
    stw 11, 84(1)            # [84] guest cpsr pointer (parity; return uses [80])
    stw 4, 88(1)             # [88] JITResult* out

    lmw 14, 0(10)            # r14..r29 = R0..R15 ; r30 = CPSR ; r31 = SPSR (discarded)
    li 31, 0                    # P12: resident guest-instruction-count accumulator

    mtctr 3                     # enter the compiled block via CTR (never a call:
    li 3, 0                     #   the trace may chain through further blocks
    bctr                       #   via the linker stub before returning here)

    # ---------------------------------------------------------------------
    # Every trace exit path (clean epilogue, deferred bailout, SMC bailout,
    # quota yield, linker-stub cache miss) ultimately lands here.
    # ---------------------------------------------------------------------
    .global ExecuteJITTrace_Return
    .type ExecuteJITTrace_Return, @function
ExecuteJITTrace_Return:
    lwz 10, 88(1)              # r10 = JITResult* out
    stw 3, 0(10)              # out->cycles       (r3)
    stw 4, 4(10)              # out->nextPC       (r4)
    stw 31, 8(10)            # out->instructions (r31, P12 resident accumulator)

    # GPR residency: flush guest R0..R15 (r14..r29) + CPSR (r30) back to memory.
    # One stmw covers gpr[0..17]; gpr[16] = CPSR <- r30 is exactly right, but
    # gpr[17] = SPSR would be clobbered by r31 (icount), so save/restore it.
    lwz 10, 80(1)             # r10 = guest gpr base
    lwz 0, 68(10)            # save SPSR  (gpr[17])
    stmw 14, 0(10)           # R0..R15 <- r14..r29 ; CPSR <- r30 ; SPSR <- r31 (bogus)
    stw 0, 68(10)            # restore SPSR

    lmw 14, 8(1)              # restore host non-volatiles
    lwz 0, 132(1)            # saved host LR (128 + 4)
    mtlr 0
    addi 1, 1, 128
    blr

#endif /* DESMUME_JIT_ARM7 */
