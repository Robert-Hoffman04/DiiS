@ windowcheck.s - Synthetic GBA ROM exercising WIN0/WINOUT layer gating,
@ BG mosaic, and BLDCNT/BLDALPHA alpha-blend together in one frame (see
@ gx-next-steps-log.md task 1). Mode 0, two text BGs:
@  - BG0: 4bpp red/blue 8x8-tile checkerboard, BG0CNT mosaic bit set,
@    MOSAIC = 12x12 blocks. 12 is deliberately NOT a multiple of the 8px
@    tile period: a mosaic size that divides evenly into 2*tile (e.g. 16)
@    would have every block's snapped sample coordinate land on the same
@    tile-index parity, collapsing the whole checker to one solid color --
@    still proof mosaic engaged (a fine checker would mean it didn't), but
@    a less legible screenshot. 12px blocks straddle tile boundaries
@    unevenly instead, producing a visibly blocky red/blue mix if mosaic
@    is honored, vs. a fine uniform 8px checker if it isn't.
@  - BG1: 4bpp solid green fullscreen.
@ WIN0 covers the left half of the screen (x 0-119, full height) and
@ enables ONLY BG0 inside it (WININ); WINOUT enables ONLY BG1 outside it,
@ plus the color-special-effect bit. BLDCNT alpha-blends BG1 (1st target)
@ against the backdrop (2nd target, white) at EVA=EVB=8 (50/50) -- this
@ only engages outside WIN0, since WININ's effect bit is clear there.
@
@ Expected correct output:
@  - left half:  big blocky red/blue checker (mosaic'd BG0, window-
@    selected, NOT blended since WININ's effect bit is 0)
@  - right half: pale green (BG1 green alpha-blended 50/50 against the
@    white backdrop, window-selected, blend engaged since WINOUT's
@    effect bit is 1)
@ If window/mosaic/blend were unimplemented (the pre-task-1 CPU
@ compositor), the whole screen would instead show one uniform
@ fine-grained red/blue checkerboard (BG0, frontmost and opaque,
@ unaffected by any of the three features, window/blend/mosaic registers
@ simply never consulted) -- a plainly different, uniform result. That
@ makes this a single-frame pass/fail visual check for all three features
@ at once: two flat-colored halves (one blocky, one pale green) vs. one
@ uniform fine checker everywhere.

    .arm
    .section .text
    .global _start
_start:
    @ BG palette (0x05000000): index0=white (backdrop), index1=red,
    @ index2=blue, index3=green
    ldr r0, =0x05000000
    ldr r1, =0x7FFF
    strh r1, [r0]
    ldr r1, =0x001F
    strh r1, [r0, #2]
    ldr r1, =0x7C00
    strh r1, [r0, #4]
    ldr r1, =0x03E0
    strh r1, [r0, #6]

    @ Tile 0 (char base block0 +0x00): solid red, 4bpp 8x8 = 32 bytes
    ldr r0, =0x06000000
    ldr r1, =0x11111111
    mov r2, #8
t0fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne t0fill

    @ Tile 1 (+0x20): solid blue
    ldr r0, =0x06000020
    ldr r1, =0x22222222
    mov r2, #8
t1fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne t1fill

    @ Tile 2 (+0x40): solid green (BG1's tile)
    ldr r0, =0x06000040
    ldr r1, =0x33333333
    mov r2, #8
t2fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne t2fill

    @ BG0 tilemap: screen base block 8 (0x06004000), 32x32, checkerboard
    @ of tile0/tile1
    ldr r0, =0x06004000
    mov r2, #0
bg0row:
    mov r3, #0
bg0col:
    add r4, r2, r3
    and r4, r4, #1
    strh r4, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt bg0col
    add r2, r2, #1
    cmp r2, #32
    blt bg0row

    @ BG1 tilemap: screen base block 9 (0x06004800), 32x32, all tile2
    ldr r0, =0x06004800
    mov r1, #2
    ldr r2, =(32 * 32)
bg1fill:
    strh r1, [r0], #2
    subs r2, r2, #1
    bne bg1fill

    @ BG0CNT (0x04000008): screen base block 8, char base block 0, 4bpp,
    @ 32x32, priority 0, mosaic enable (bit6)
    ldr r0, =0x04000008
    ldr r1, =(1 << 6) | (8 << 8)
    strh r1, [r0]

    @ BG1CNT (0x0400000A): screen base block 9, char base block 0, 4bpp,
    @ 32x32, priority 1
    ldr r0, =0x0400000A
    ldr r1, =(9 << 8) | 1
    strh r1, [r0]

    @ WIN0H (0x04000040): X1=0, X2=120 (left half of the 240px screen)
    ldr r0, =0x04000040
    mov r1, #120
    strh r1, [r0]

    @ WIN0V (0x04000044): Y1=0, Y2=160 (full height)
    ldr r0, =0x04000044
    mov r1, #160
    strh r1, [r0]

    @ WININ (0x04000048): inside WIN0, enable BG0 only (bit0); effect bit
    @ (bit5) left clear -- no blend inside the window.
    ldr r0, =0x04000048
    mov r1, #0x0001
    strh r1, [r0]

    @ WINOUT (0x0400004A): outside all windows, enable BG1 only (bit1) +
    @ the color special effect (bit5).
    ldr r0, =0x0400004A
    ldr r1, =0x0022
    strh r1, [r0]

    @ MOSAIC (0x0400004C): BG H-size=12 (field 11), BG V-size=12 (field
    @ 11); OBJ mosaic fields unused (0).
    ldr r0, =0x0400004C
    ldr r1, =0x00BB
    strh r1, [r0]

    @ BLDCNT (0x04000050): 1st target BG1 (bit1), effect=alpha blend
    @ (bits6-7 = 01), 2nd target backdrop (bit13, i.e. bit5 of the
    @ 2nd-target field).
    ldr r0, =0x04000050
    ldr r1, =0x2042
    strh r1, [r0]

    @ BLDALPHA (0x04000052): EVA=8, EVB=8 (both 8/16 = 0.5 -- an even
    @ 50/50 blend).
    ldr r0, =0x04000052
    ldr r1, =0x0808
    strh r1, [r0]

    @ DISPCNT: mode 0, BG0 enable (bit8), BG1 enable (bit9), WIN0 enable
    @ (bit13).
    ldr r0, =0x04000000
    ldr r1, =(1 << 8) | (1 << 9) | (1 << 13)
    strh r1, [r0]

hang:
    b hang
