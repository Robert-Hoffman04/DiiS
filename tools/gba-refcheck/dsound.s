@ Synthetic GBA ROM for PLAN.md §4.3 item 3 (APU): drives the DirectSound
@ FIFO A path end to end -- Timer0 for sample-rate cadence, DMA channel 1
@ (Special/Sound-FIFO timing, dst 0x040000A0) for refill, SOUNDCNT_H/X for
@ routing/volume/enable -- with a known, deterministic 8-bit PCM pattern so
@ the host-side probe (-DDESMUME_GBA_APU_SOAK, source/main.cpp) and the
@ mGBA reference driver (tools/gba-refcheck/mgba_headless.c) can both be
@ checked against a known-good expected value.
@
@ Waveform: EWRAM 0x02010000..+4096 filled with the repeating 32-bit word
@ 0x9C649C64 -- little-endian byte order makes every popped FIFO byte
@ alternate +100 (0x64) / -100 (0x9C, signed), i.e. a fixed-amplitude
@ square wave, independent of buffer position/alignment.
@
@ Sample cadence: Timer0 reload 0xFE00 (period 0x200 = 512 cycles),
@ prescaler /1 -> overflow every 512 of the real 16.777216 MHz GBA clock,
@ i.e. exactly 32768 Hz -- GBATEK's own suggested DirectSound rate.
@
@ Hand-written raw ARM asm, same style/toolchain as irqsoak.s/gradient.s
@ (build_roms.sh); see docs/PLAN.md §4.3 item 3.

    .arm
    .section .text
    .global _start
_start:
    mrs r0, cpsr
    bic r0, r0, #0x80
    msr cpsr_c, r0

    @ Fill the EWRAM source buffer: 1024 words of 0x9C649C64 at 0x02010000.
    ldr r0, =0x02010000
    ldr r1, =0x9C649C64
    mov r2, #1024
fillloop:
    str r1, [r0], #4
    subs r2, r2, #1
    bne fillloop

    @ DMA1: SAD=0x02010000, DAD=FIFO A (0x040000A0), CNT_L=4 (word count is
    @ ignored in Special/Sound-FIFO mode -- always moves exactly 4 words per
    @ trigger -- set anyway for a sane register dump), CNT_H = enable(15) |
    @ special-timing(12-13=3) | 32-bit(10) | dest-fixed(5-6=2) | src-inc(7-8=0).
    ldr r0, =0x040000BC
    ldr r1, =0x02010000
    str r1, [r0]
    ldr r0, =0x040000C0
    ldr r1, =0x040000A0
    str r1, [r0]
    ldr r0, =0x040000C4
    mov r1, #4
    strh r1, [r0]
    ldr r0, =0x040000C6
    ldr r1, =0xB440
    strh r1, [r0]

    @ SOUNDCNT_H: FIFO-A full volume(2)=1, FIFO-A enable R(8)/L(9)=1,
    @ FIFO-A timer-select(10)=0 (Timer0), FIFO-A reset(11)=1 (clears FIFO
    @ to empty before the first pop -- write-only, self-clears per GBATEK).
    ldr r0, =0x04000082
    ldr r1, =0x0B04
    strh r1, [r0]

    @ SOUNDCNT_X: master sound enable (bit7).
    ldr r0, =0x04000084
    mov r1, #0x80
    strh r1, [r0]

    @ TM0CNT_L: reload 0xFE00 (period 0x200 = 512 cycles -> 32768 Hz).
    ldr r0, =0x04000100
    ldr r1, =0xFE00
    strh r1, [r0]
    @ TM0CNT_H: start(7), prescaler/1(0-1=00), no IRQ needed.
    ldr r0, =0x04000102
    mov r1, #0x80
    strh r1, [r0]

    @ Idle forever -- all the interesting state is DMA/timer/APU-driven,
    @ no CPU-side work needed once armed.
idle:
    b idle
