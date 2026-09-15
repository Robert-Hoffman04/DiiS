@ Synthetic GBA ROM for PLAN.md §4.3 item 2's remaining piece: DMA channel
@ 3's Video Capture Special-timing trigger. Per GBATEK: "Capture works
@ similar like HBlank DMA, however, the transfer is started when VCOUNT=2,
@ it is then repeated each scanline, and it gets stopped when VCOUNT=162."
@ i.e. it should fire once per HBlank for VCOUNT in [2,161] (160 scanlines)
@ and nowhere else -- not on plain VBlank/HBlank timing, not during VCOUNT
@ 0-1 or 162-227.
@
@ To verify the exact scanline set it fires on with no extra tooling, this
@ ROM arms DMA3 with:
@   SAD    = 0x04000006 (REG_VCOUNT), src fixed
@   DAD    = EWRAM 0x02010000, dst increment
@   CNT_L  = 1 halfword per firing
@   CNT_H  = 16-bit, dst-increment, src-fixed, repeat=1, Special timing (3)
@ so every time the engine fires this channel, it copies the CURRENT
@ VCOUNT value into the next halfword slot of an EWRAM buffer -- turning
@ "which scanlines did this fire on" into a plain array the host-side probe
@ can read back and check for the exact sequence 2,3,4,...,161 (160
@ entries, monotonic, no duplicates, nothing outside that range). Any bug
@ that also fires this channel from the plain HBlank/VBlank trigger paths
@ (timing 1/2) would show up as extra entries with VCOUNT values outside
@ [2,161] (e.g. 0, 1, or 162+), or more than 160 total entries in frame 1.
@
@ The main loop polls VCOUNT until it sees 163 (i.e. just past the video-
@ capture window's close for frame 1), then disables DMA3 itself so it
@ doesn't keep overwriting the buffer with frame 2's captures -- the host
@ probe only needs frame 1's 160 entries. A second u32 marker records how
@ many times the poll loop's "frame ended" branch was taken, purely as a
@ liveness signal for the host probe (proof the ROM's own logic reached
@ that point, distinct from whether the DMA fired correctly).
@
@ Hand-written raw ARM asm, same style/toolchain as irqsoak.s/waitcnt.s
@ (build_roms.sh).

    .arm
    .section .text
    .global _start
_start:
    @ System mode, IRQs don't matter for this test -- leave masked (BIOS
    @ default). No need to touch CPSR.

    @ Zero the capture buffer's first few slots and the liveness marker
    @ (EWRAM is not guaranteed pre-zeroed across repeated harness runs).
    ldr r0, =0x02010000
    mov r1, #0
    mov r2, #16
zeroloop:
    str r1, [r0]
    add r0, r0, #4
    subs r2, r2, #1
    bne zeroloop

    ldr r0, =0x0200FF00     @ liveness marker, well clear of the capture buffer
    mov r1, #0
    str r1, [r0]

    @ DMA3SAD = REG_VCOUNT (0x04000006)
    ldr r0, =0x040000D4
    ldr r1, =0x04000006
    str r1, [r0]

    @ DMA3DAD = EWRAM 0x02010000
    ldr r0, =0x040000D8
    ldr r1, =0x02010000
    str r1, [r0]

    @ DMA3CNT_L = 1 (one halfword per firing)
    ldr r0, =0x040000DC
    mov r1, #1
    strh r1, [r0]

    @ DMA3CNT_H: dst-increment(00)=bits5-6, src-fixed(10)=bits7-8,
    @ repeat(bit9)=1, 16-bit(bit10)=0, Special timing(bits12-13)=11,
    @ enable(bit15)=1. Src ctrl is a 2-bit field at bit7 (value 2 =
    @ Fixed, so bit8=1/bit7=0 -> 0x100, NOT 0x80 -- an earlier version of
    @ this ROM got this wrong (0x80 = value 1 = Decrement), which walked
    @ the "fixed" source address backwards through nearby I/O registers
    @ every firing instead of re-reading VCOUNT each time; caught via the
    @ host-side dmasoak_dbg readback showing mostly-zero captured values
    @ instead of the expected 2..161 ramp, root-caused by disassembling
    @ the .elf and checking the literal pool word by hand against GBATEK.
    @   bit8-7 = 10b (fixed)          -> 0x0100
    @   bit9   = 1 (repeat)           -> 0x0200
    @   bit12-13 = 11b (Special)      -> 0x3000
    @   bit15  = 1 (enable)           -> 0x8000
    @ total = 0x0100 | 0x0200 | 0x3000 | 0x8000 = 0xB300
    ldr r0, =0x040000DE
    ldr r1, =0xB300
    strh r1, [r0]

    @ Poll VCOUNT (0x04000006) until it reaches 163 -- one full pass past
    @ the video-capture window's close for this frame -- then disarm DMA3
    @ so it stops consuming the buffer, bump the liveness marker, and idle.
poll:
    ldr r0, =0x04000006
    ldrh r1, [r0]
    cmp r1, #163
    bne poll

    @ Disable DMA3 (clear enable bit, leave the rest -- harmless, channel
    @ won't be re-armed).
    ldr r0, =0x040000DE
    mov r1, #0
    strh r1, [r0]

    ldr r0, =0x0200FF00
    mov r1, #1
    str r1, [r0]

idle:
    b idle
