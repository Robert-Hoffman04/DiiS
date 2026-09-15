@ Minimal synthetic GBA ROM: mode 0, 4bpp text BG0 with two 8x8 tiles
@ (solid red, solid blue) laid out as a checkerboard tilemap.
@ Hand-written raw ARM asm; see gradient.s / build_roms.sh for rationale.

    .arm
    .section .text
    .global _start
_start:
    @ Palette (BG palette RAM 0x05000000): index1=red, index2=blue
    ldr r0, =0x05000000
    mov r1, #0x001F             @ red (BGR555)
    strh r1, [r0, #2]            @ palette index 1
    ldr r1, =0x7C00              @ blue
    strh r1, [r0, #4]            @ palette index 2

    @ Tile 0 (all pixels = palette index 1, red): CBB 4bpp, 8x8 = 32 bytes
    ldr r0, =0x06000000          @ char base block 0, tile 0
    ldr r1, =0x11111111
    mov r2, #8
tile0fill:
    str r1, [r0], #4
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile0fill

    @ Tile 1 (all pixels = palette index 2, blue)
    ldr r0, =0x06000020          @ tile 1 starts at +32 bytes
    ldr r1, =0x22222222
    mov r2, #8
tile1fill:
    str r1, [r0], #4
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile1fill

    @ Screen base block 8 (0x06004000), 32x32 tilemap: checkerboard of tile0/1
    ldr r0, =0x06004000
    mov r2, #0                   @ row
maprow:
    mov r3, #0                   @ col
mapcol:
    add r4, r2, r3
    and r4, r4, #1                @ (row+col)&1 -> 0 or 1 = tile index
    strh r4, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt mapcol
    add r2, r2, #1
    cmp r2, #32
    blt maprow

    @ BG0CNT (0x04000008): screen base block 8, char base block 0, 4bpp, 32x32
    ldr r0, =0x04000008
    mov r1, #(8 << 8)
    strh r1, [r0]

    @ DISPCNT: mode 0, BG0 enable
    ldr r0, =0x04000000
    mov r1, #0x0100
    strh r1, [r0]

halt:
    b halt
