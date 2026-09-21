@ winlayeroff.s - Synthetic GBA ROM for the GBA window "layer enabled
@ outside but disabled inside a window" bug (gx-next-steps-log.md task 16).
@ The old GX scheme drew each layer over the whole band for WINOUT then
@ overdrew scissored WIN1/WIN0 rects, so a layer ON in WINOUT and OFF in
@ WIN0/WIN1 was never erased inside the window.
@
@ Mode 0, no mosaic, no blend. BG0 = solid red (priority 0; solid, not a checker, because GX's
@ linear text-BG sampling bleeds 1/16 texel on differing tile edges, see task 16),
@ BG1 = solid green (priority 1).
@  WINOUT: BG0 only.
@  WIN0 (x 40..140, y 30..110): BG1 only.   -> solid green rect
@  WIN1 (x 100..200, y 60..140): nothing.   -> white backdrop; WIN0 wins
@                                              in the overlap (x100..140,y60..110)
@ Expected: red outside, green WIN0 rect, white WIN1 rect minus the overlap.

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
    and r4, r4, #0
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
    @ 32x32, priority 0. No mosaic bit this time (windowcheck.s's bit6).
    ldr r0, =0x04000008
    ldr r1, =(8 << 8)
    strh r1, [r0]

    @ BG1CNT (0x0400000A): screen base block 9, char base block 0, 4bpp,
    @ 32x32, priority 1
    ldr r0, =0x0400000A
    ldr r1, =(9 << 8) | 1
    strh r1, [r0]

    @ WIN0H (0x04000040): X1=40, X2=140; WIN1H (0x04000042): X1=100, X2=200
    ldr r0, =0x04000040
    ldr r1, =((40 << 8) | 140)
    strh r1, [r0]
    ldr r0, =0x04000042
    ldr r1, =((100 << 8) | 200)
    strh r1, [r0]

    @ WIN0V (0x04000044): Y1=30, Y2=110; WIN1V (0x04000046): Y1=60, Y2=140
    ldr r0, =0x04000044
    ldr r1, =((30 << 8) | 110)
    strh r1, [r0]
    ldr r0, =0x04000046
    ldr r1, =((60 << 8) | 140)
    strh r1, [r0]

    @ WININ (0x04000048): WIN0 enables BG1 only (bit1); WIN1 (high byte) nothing.
    ldr r0, =0x04000048
    mov r1, #0x0002
    strh r1, [r0]

    @ WINOUT (0x0400004A): outside all windows, enable BG0 only (bit0).
    ldr r0, =0x0400004A
    ldr r1, =0x0001
    strh r1, [r0]

    @ MOSAIC (0x0400004C): left at its power-on-default 0 -- no mosaic
    @ anywhere in this ROM (see header comment).

    @ DISPCNT: mode 0, BG0 (bit8), BG1 (bit9), WIN0 (bit13), WIN1 (bit14).
    ldr r0, =0x04000000
    ldr r1, =(1 << 8) | (1 << 9) | (1 << 13) | (1 << 14)
    strh r1, [r0]

hang:
    b hang
