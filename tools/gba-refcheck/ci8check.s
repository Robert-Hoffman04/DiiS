@ gx-next-steps-log.md task 6: 8bpp coverage. checkerboard.s/objcheck.s/
@ affinecheck.s/windowcheck.s/winblend.s/objcache.s/bgcache.s are all 4bpp
@ (BGxCNT bit7 clear / OAM attribute0 bit13 clear) -- this ROM specifically
@ exercises the 8bpp CI8 bake paths task 6 added: an 8bpp text BG0
@ checkerboard (red/blue, mirroring checkerboard.s's 4bpp one) plus one
@ regular (non-affine) 8bpp OBJ sprite (green, 16x16) drawn on top-left of
@ it, so both gxBakeBgPlane's 8bpp branch and gxBakeObjTexture's 8bpp
@ branch are actually engaged by a real capture, not just inspected by
@ reading the code.

    .arm
    .section .text
    .global _start
_start:
    @ BG palette (BG palette RAM 0x05000000): index1=red, index2=blue.
    @ 8bpp text BG uses the combined index directly (no palNum sub-bank
    @ indirection), same address space checkerboard.s's 4bpp tiles use.
    ldr r0, =0x05000000
    mov r1, #0x001F              @ red (BGR555)
    strh r1, [r0, #2]             @ palette index 1
    ldr r1, =0x7C00               @ blue
    strh r1, [r0, #4]             @ palette index 2

    @ OBJ palette (0x05000200): index1=green
    ldr r0, =0x05000200
    ldr r1, =0x03E0
    strh r1, [r0, #2]

    @ BG0 tile 0 (8bpp, 64 bytes/tile): all pixels = palette index 1 (red)
    ldr r0, =0x06000000
    ldr r1, =0x01010101
    mov r2, #16
bgtile0fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bgtile0fill

    @ BG0 tile 1 (8bpp, 64 bytes), all pixels = palette index 2 (blue)
    ldr r0, =0x06000040
    ldr r1, =0x02020202
    mov r2, #16
bgtile1fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bgtile1fill

    @ BG0 screen base block 8 (0x06004000), 32x32 tilemap, checkerboard of
    @ tile0/tile1 -- same layout as checkerboard.s, 2-byte entries (palNum
    @ bits are simply unused/ignored for an 8bpp BG's map entries).
    ldr r0, =0x06004000
    mov r2, #0
maprow:
    mov r3, #0
mapcol:
    add r4, r2, r3
    and r4, r4, #1
    strh r4, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt mapcol
    add r2, r2, #1
    cmp r2, #32
    blt maprow

    @ BG0CNT (0x04000008): screen base block 8, char base block 0, 8bpp
    @ (bit7 set), 32x32
    ldr r0, =0x04000008
    ldr r1, =((8 << 8) | (1 << 7))
    strh r1, [r0]

    @ OBJ tiles 0-3 (8bpp, 64 bytes each = 256 bytes total; a 16x16 sprite
    @ is a 2x2 tile grid, contiguous under 1D OBJ mapping), OBJ char base
    @ fixed at VRAM+0x10000 = 0x06010000. All pixels = palette index 1
    @ (green).
    ldr r0, =0x06010000
    ldr r1, =0x01010101
    mov r2, #64
objtilefill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne objtilefill

    @ OAM entry 0: regular (non-affine) 16x16 (shape0/size1) sprite, 8bpp
    @ (attribute0 bit13 set), tile 0, at screen (40,40) -- overlaps BG0's
    @ checkerboard so a wrong TLUT/index bake would visibly corrupt either
    @ the sprite or the tiles underneath it, not just render "some color".
    ldr r0, =0x07000000
    ldr r1, =((1 << 13) | 40)     @ attr0: 8bpp, y=40
    strh r1, [r0]
    ldr r1, =40                   @ attr1: x=40, shape0/size1 default (size sel bits14-15=0 -> handled by attr0 shape bits14-15=0 too, see below)
    strh r1, [r0, #2]
    mov r1, #0                    @ attr2: tile 0, palette 0, priority 0
    strh r1, [r0, #4]

    @ attr0 shape bits14-15 = 0 (square) already clear from the strh above;
    @ attr1 size bits14-15 = 1 (16x16 for a square shape) -- set them here
    @ since the attr1 strh above only wrote the low x bits.
    ldr r0, =0x07000002
    ldr r1, =((1 << 14) | 40)
    strh r1, [r0]

    @ DISPCNT: mode 0, BG0 enable (bit8), 1D OBJ mapping (bit6), OBJ enable
    @ (bit12)
    ldr r0, =0x04000000
    ldr r1, =((1 << 8) | (1 << 6) | (1 << 12))
    strh r1, [r0]

halt:
    b halt
