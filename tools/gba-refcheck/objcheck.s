@ Minimal synthetic GBA ROM exercising the GX vertical slice's affine paths:
@ mode 2, BG2 affine (8bpp, identity matrix) checkerboard tilemap, plus one
@ affine OBJ (identity matrix, matrix group 0) drawn on top. Identity
@ matrices mean the expected pixels are the same red/green checkerboard (and
@ white sprite square) an unrotated render would produce -- this is a
@ decode-path/pipeline-plumbing check (does the GX affine BG/OBJ code path
@ engage and read the right VRAM/OAM/registers at all), not a rotation/skew
@ check. See checkerboard.s for the mode-0 text-BG equivalent this mirrors.

    .arm
    .section .text
    .global _start
_start:
    @ BG palette: index1=red, index2=green
    ldr r0, =0x05000000
    ldr r1, =0x001F
    strh r1, [r0, #2]
    ldr r1, =0x03E0
    strh r1, [r0, #4]

    @ OBJ palette: index1=white
    ldr r0, =0x05000200
    ldr r1, =0x7FFF
    strh r1, [r0, #2]

    @ Affine BG tile 0 (8bpp, 64 bytes), all pixels = index1 (red)
    ldr r0, =0x06000000
    ldr r1, =0x01010101
    mov r2, #16
tile0fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile0fill

    @ Affine BG tile 1 (8bpp, 64 bytes), all pixels = index2 (green)
    ldr r0, =0x06000040
    ldr r1, =0x02020202
    mov r2, #16
tile1fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile1fill

    @ Affine map, screen base block 8 (0x06004000): 16x16 tiles (128x128px),
    @ 1 byte/entry, checkerboard of tile0/tile1.
    ldr r0, =0x06004000
    mov r2, #0
maprow:
    mov r3, #0
mapcol:
    add r4, r2, r3
    and r4, r4, #1
    strb r4, [r0], #1
    add r3, r3, #1
    cmp r3, #16
    blt mapcol
    add r2, r2, #1
    cmp r2, #16
    blt maprow

    @ BG2CNT: map base block 8, screen size sel 0 (128x128), priority 0
    ldr r0, =0x0400000C
    mov r1, #(8 << 8)
    strh r1, [r0]

    @ BG2PA/PB/PC/PD: identity (256, 0, 0, 256), 8.8 fixed point
    ldr r0, =0x04000020
    mov r1, #0x0100
    strh r1, [r0]
    mov r1, #0
    strh r1, [r0, #2]
    strh r1, [r0, #4]
    mov r1, #0x0100
    strh r1, [r0, #6]

    @ BG2X=0, BG2Y=0 (32-bit registers)
    ldr r0, =0x04000028
    mov r1, #0
    str r1, [r0]
    str r1, [r0, #4]

    @ OBJ tiles 2-5 (4bpp, 32 bytes each = 128 bytes), all pixels = index1
    @ (white) -- a 16x16 sprite is a 2x2 tile grid; with 1D OBJ mapping
    @ (DISPCNT bit6, set below) tiles 2,3,4,5 are contiguous, so one 128-byte
    @ fill covers the whole sprite (OBJ char base is fixed at VRAM+0x10000).
    ldr r0, =0x06010040
    ldr r1, =0x11111111
    mov r2, #32
objtilefill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne objtilefill

    @ OAM entries 1-3 only exist here to hold affine matrix group 0's
    @ PB/PC/PD (see below); disable them as sprites in their own right.
    ldr r1, =0x0200          @ attr0: disable bit9 set, affine bit8 clear
    ldr r0, =0x07000008
    strh r1, [r0]
    ldr r0, =0x07000010
    strh r1, [r0]
    ldr r0, =0x07000018
    strh r1, [r0]

    @ Affine matrix group 0, identity: PA=256 (entry0+6), PB=0 (entry1+6),
    @ PC=0 (entry2+6), PD=256 (entry3+6)
    mov r1, #0x0100
    ldr r0, =0x07000006
    strh r1, [r0]
    mov r1, #0
    ldr r0, =0x0700000E
    strh r1, [r0]
    ldr r0, =0x07000016
    strh r1, [r0]
    mov r1, #0x0100
    ldr r0, =0x0700001E
    strh r1, [r0]

    @ OAM entry 0: a REGULAR (non-affine) sprite -- 16x16 (shape0/size1),
    @ tile 2, palette 0, priority 0, at screen (100,80). Same tile/position
    @ as affinecheck.s's affine sprite, but affine bit8 clear -- isolates
    @ whether the border/quad rendering bug is affine-specific or affects
    @ regular OBJ too.
    ldr r0, =0x07000000
    mov r1, #80
    strh r1, [r0]
    ldr r1, =(1 << 14) | 100
    strh r1, [r0, #2]
    mov r1, #2
    strh r1, [r0, #4]

    @ DISPCNT: mode 2, 1D OBJ mapping (bit6), BG2 enable (bit10), OBJ enable
    @ (bit12)
    ldr r0, =0x04000000
    ldr r1, =(2 | (1 << 6) | (1 << 10) | (1 << 12))
    strh r1, [r0]

hang:
    b hang
