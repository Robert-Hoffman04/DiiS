@ Synthetic GBA ROM for PLAN.md §4.3 item 6's sustained-play tail: whether
@ armcpu_irqException()'s real exception dispatch (source/gba_irq.cpp's
@ trampoline path) interacts correctly with ARM7-JIT-compiled code under
@ SUSTAINED play (many thousands of real interrupt deliveries), not just
@ the first few captured by the Minish Cap title-screen run.
@
@ Enables VBlank IRQ (DISPSTAT bit3) and Timer0 IRQ (reload 0xE000,
@ prescaler /1 -> overflow every 0x2000 = 8192 cycles, ~34/frame at the
@ real 280896-cycles/frame GBA rate gbaExecFrame() uses), installs a real
@ ISR at the BIOS-convention handler pointer (0x03007FFC) so
@ gbaIrqDispatchIfPending() takes the genuine armcpu_irqException() path
@ (not the Halt/IntrWait flags-word HLE fallback), and runs a JIT-hot main
@ busy loop concurrently. Five u32 counters in EWRAM let the host-side
@ probe (-DDESMUME_GBA_IRQ_SOAK, source/main.cpp) verify, from outside,
@ that both the ISR and the interrupted main loop keep making correct,
@ uncorrupted forward progress across the whole run:
@   EWRAM+0x00 total_irq        every dispatched IRQ, any source
@   EWRAM+0x04 vblank_count     IRQs where IF had the VBlank bit
@   EWRAM+0x08 timer_count      IRQs where IF had the Timer0 bit
@   EWRAM+0x0C mainloop_iters   main loop's own counter (untouched by ISR)
@   EWRAM+0x10 bad_source_count IRQ dispatched with neither bit set
@                                (should stay 0 -- a real corruption signal)
@
@ Hand-written raw ARM asm, same style/toolchain as gradient.s/
@ checkerboard.s (build_roms.sh); see docs/PLAN.md §4.3 item 6.

    .arm
    .section .text
    .global _start
_start:
    @ Direct-boot leaves CPSR.I set (IRQs masked) by BIOS convention --
    @ every real cartridge unmasks it itself. System mode (0x1F), IRQ on.
    mrs r0, cpsr
    bic r0, r0, #0x80
    msr cpsr_c, r0

    @ Zero all five counters first (EWRAM is not guaranteed pre-zeroed by
    @ the harness across repeated runs against the same SD image).
    ldr r0, =0x02000000
    mov r1, #0
    str r1, [r0, #0]
    str r1, [r0, #4]
    str r1, [r0, #8]
    str r1, [r0, #12]
    str r1, [r0, #16]

    @ Install ISR handler pointer at the BIOS-convention address
    @ gbaIrqDispatchIfPending() requires be non-null before it will take
    @ the real armcpu_irqException() path (gba_irq.cpp).
    ldr r0, =0x03007FFC
    ldr r1, =isr_handler
    str r1, [r0]

    @ IE = VBlank(bit0) | Timer0(bit3) = 0x0009
    ldr r0, =0x04000200
    mov r1, #0x0009
    strh r1, [r0]

    @ IME = 1
    ldr r0, =0x04000208
    mov r1, #1
    strh r1, [r0]

    @ DISPSTAT: VBlank-IRQ-enable (bit3)
    ldr r0, =0x04000004
    mov r1, #0x0008
    strh r1, [r0]

    @ TM0CNT_L: reload 0xE000 (period 0x2000 cycles)
    ldr r0, =0x04000100
    ldr r1, =0xE000
    strh r1, [r0]
    @ TM0CNT_H: start(bit7) | IRQ-enable(bit6) | prescaler/1(bits0-1=00)
    ldr r0, =0x04000102
    mov r1, #0xC0
    strh r1, [r0]

    @ Main loop: JIT-hot, entirely independent of the ISR's own counters --
    @ if the JIT/IRQ interaction ever corrupts register state or drops the
    @ CPU into the wrong place on return from an exception, this counter
    @ stalls, jumps, or its low bits stop matching a clean +1-per-iteration
    @ progression when sampled from the host side.
mainloop:
    ldr r0, =0x02000000
    ldr r1, [r0, #12]
    add r1, r1, #1
    str r1, [r0, #12]
    mov r2, #16
spin:
    subs r2, r2, #1
    bne spin
    b mainloop

    .align 2
@ Called via the BIOS trampoline's `mov lr,pc; bx r0` (gba_irq.cpp) --
@ ordinary ARM subroutine, r0-r3/r12/lr already saved by the trampoline,
@ returns via bx lr, must NOT touch r4+.
isr_handler:
    ldr r0, =0x04000200
    ldrh r1, [r0, #2]       @ IF  (0x04000202)
    ldrh r2, [r0]           @ IE  (0x04000200)
    and r3, r1, r2           @ serviced = IE & IF

    ldr r0, =0x02000000
    ldr r1, [r0, #0]
    add r1, r1, #1
    str r1, [r0, #0]        @ total_irq++

    tst r3, #0x0001
    beq notvbl
    ldr r1, [r0, #4]
    add r1, r1, #1
    str r1, [r0, #4]        @ vblank_count++
notvbl:
    tst r3, #0x0008
    beq nottmr
    ldr r1, [r0, #8]
    add r1, r1, #1
    str r1, [r0, #8]        @ timer_count++
nottmr:
    tst r3, #0x0009
    bne knownsrc
    ldr r1, [r0, #16]
    add r1, r1, #1
    str r1, [r0, #16]       @ bad_source_count++
knownsrc:
    @ ack IF: write-1-to-clear only the bit(s) actually serviced
    ldr r0, =0x04000202
    strh r3, [r0]

    bx lr
