@ Synthetic GBA ROM for PLAN.md §4.3 item 4: WAITCNT-driven cartridge ROM
@ wait-state cycle cost. Configures WAITCNT to two different settings in
@ turn -- the real hardware post-reset default (0x0000: WS0 First/Second
@ access = 4/2 cycles per GBATEK) and the fastest possible WS0 setting
@ (0x0018: WS0 First/Second access = 2/1 cycles) -- and under each config
@ times a fixed number of repeated cartridge-ROM reads with Timer0 (free-
@ running, prescaler /1, so its live count is a direct ARM7-cycle counter)
@ as the reference clock. If MMU_timing.h's WAITCNT-driven cost model
@ (source/gba_io.{h,cpp}, source/MMU_timing.h) is wired up correctly, the
@ 0x0000 (slow) run must measure MORE elapsed cycles than the 0x0018
@ (fast) run for the exact same fixed amount of work.
@
@ Each read targets the SAME fixed address every iteration (not an
@ incrementing pointer) specifically so every access is classified
@ non-sequential by the interpreter's sequential-access tracking (an
@ address that repeats can never satisfy "address == last address +
@ size") -- isolating WAITCNT's First-Access (N-cycle) field cleanly,
@ with no dependence on this test also getting sequential (S-cycle)
@ detection right.
@
@ Results (u32 elapsed Timer0 ticks each) land in EWRAM so the host-side
@ probe (-DDESMUME_GBA_WAITCNT_SOAK, source/main.cpp) can read them once
@ both runs finish:
@   EWRAM+0x00  slow_elapsed  (WAITCNT=0x0000)
@   EWRAM+0x04  fast_elapsed  (WAITCNT=0x0018)
@
@ Hand-written raw ARM asm, same style/toolchain as irqsoak.s/dsound.s
@ (build_roms.sh).

    .arm
    .section .text
    .global _start

NREADS = 1500   @ chosen so worst-case (slow config) elapsed ticks stays
                @ well under Timer0's 16-bit (0xFFFF) wrap at prescaler /1
                @ -- empirically, slow costs ~24 cycles/iteration (WS0
                @ N=4/S=2 dominating), so 1500 * ~24 =~ 36000, safely clear
                @ of the 65536 wrap (an earlier 3000-iteration run's slow
                @ config silently wrapped, reading back 71456 mod 65536 =
                @ 5920 -- caught by comparing against the expected GBATEK
                @ magnitude, not trusted blindly).

_start:
    @ Zero the results region first (EWRAM is not guaranteed pre-zeroed
    @ across repeated runs against the same SD image).
    ldr r0, =0x02000000
    mov r1, #0
    str r1, [r0, #0]
    str r1, [r0, #4]

    @ ---------------- Run 1: SLOW (WAITCNT = 0x0000, real hardware's
    @ post-reset default -- WS0 First/Second access = 4/2 cycles) --------
    ldr r0, =0x04000204
    mov r1, #0x0000
    strh r1, [r0]

    ldr r4, =fixedData
    ldr r6, =NREADS

    @ start Timer0: reload 0, prescaler /1, running, no IRQ
    ldr r0, =0x04000100
    mov r1, #0
    strh r1, [r0]
    ldr r0, =0x04000102
    mov r1, #0x80
    strh r1, [r0]

slow_loop:
    ldr r5, [r4]            @ fixed-address, non-sequential cartridge ROM read
    subs r6, r6, #1
    bne slow_loop

    @ latch live count while still running, then stop the timer
    ldr r0, =0x04000100
    ldrh r7, [r0]
    ldr r0, =0x04000102
    mov r1, #0
    strh r1, [r0]

    ldr r0, =0x02000000
    str r7, [r0, #0]        @ slow_elapsed

    @ ---------------- Run 2: FAST (WAITCNT = 0x0018 -- WS0 First=2(binary
    @ 10 in bits2-3)/Second=1(bit4) cycles, the fastest WS0 setting) ------
    ldr r0, =0x04000204
    mov r1, #0x0018
    strh r1, [r0]

    ldr r4, =fixedData
    ldr r6, =NREADS

    ldr r0, =0x04000100
    mov r1, #0
    strh r1, [r0]
    ldr r0, =0x04000102
    mov r1, #0x80
    strh r1, [r0]

fast_loop:
    ldr r5, [r4]
    subs r6, r6, #1
    bne fast_loop

    ldr r0, =0x04000100
    ldrh r7, [r0]
    ldr r0, =0x04000102
    mov r1, #0
    strh r1, [r0]

    ldr r0, =0x02000000
    str r7, [r0, #4]        @ fast_elapsed

    @ Done -- park here forever (no IRQs armed, nothing else to do; the
    @ host probe reads EWRAM directly, it doesn't need the CPU to signal
    @ completion any other way).
park:
    b park

    .align 2
fixedData:
    .word 0xDEADBEEF
