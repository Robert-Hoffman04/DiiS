@ Task 19 fixture: affine BG2 + two affine OBJs with NON-identity matrices
@ (rotation/zoom, fractional reference point). The identity-matrix fixtures
@ (affinecheck/objcheck) cannot see a GX sample-point error or bilinear bleed
@ except at borders; this one puts a 4-colour tile checker (every neighbour a
@ different colour, in both axes) under a rotated/zoomed transform, so any
@ half-texel mistake or bleed shows up as a wrong-colour pixel.
@ Variants share this file by editing the .equ block; see affinemag.s /
@ affineshrink.s (same code, different constants).
    .arm
    .section .text
    .global _start
    .equ BG_WRAP,   0
    .equ BG_PA,     0x01D0
    .equ BG_PB,     0xFF60
    .equ BG_PC,     0x00A0
    .equ BG_PD,     0x01D0
    .equ BG_X0,     0xFFFFD140
    .equ BG_Y0,     0xFFFFF0C0
    .equ OA_PA,     0x01A0
    .equ OA_PB,     0xFFA0
    .equ OA_PC,     0x0060
    .equ OA_PD,     0x01A0
    .equ OB_PA,     0x0080
    .equ OB_PB,     0x0000
    .equ OB_PC,     0x0000
    .equ OB_PD,     0x0080
_start:
    @ BG palette 1..4 = red, green, blue, yellow
    ldr r0, =0x05000000
    ldr r1, =0x001F
    strh r1, [r0, #2]
    ldr r1, =0x03E0
    strh r1, [r0, #4]
    ldr r1, =0x7C00
    strh r1, [r0, #6]
    ldr r1, =0x03FF
    strh r1, [r0, #8]
    @ OBJ palette 1..4 = white, cyan, magenta, orange
    ldr r0, =0x05000200
    ldr r1, =0x7FFF
    strh r1, [r0, #2]
    ldr r1, =0x7FE0
    strh r1, [r0, #4]
    ldr r1, =0x7C1F
    strh r1, [r0, #6]
    ldr r1, =0x02BF
    strh r1, [r0, #8]
    @ BG tiles 0..3 (8bpp, 64 bytes each): solid index 1..4
    ldr r0, =0x06000000
    ldr r1, =0x01010101
    mov r2, #16
t0: str r1, [r0], #4
    subs r2, r2, #1
    bne t0
    ldr r1, =0x02020202
    mov r2, #16
t1: str r1, [r0], #4
    subs r2, r2, #1
    bne t1
    ldr r1, =0x03030303
    mov r2, #16
t2: str r1, [r0], #4
    subs r2, r2, #1
    bne t2
    ldr r1, =0x04040404
    mov r2, #16
t3: str r1, [r0], #4
    subs r2, r2, #1
    bne t3
    @ Map (block 8): 16x16 tiles, tile = (row&1)*2 + (col&1)
    ldr r0, =0x06004000
    mov r2, #0
maprow:
    mov r3, #0
mapcol:
    and r4, r2, #1
    mov r4, r4, lsl #1
    and r5, r3, #1
    add r4, r4, r5
    strb r4, [r0], #1
    add r3, r3, #1
    cmp r3, #16
    blt mapcol
    add r2, r2, #1
    cmp r2, #16
    blt maprow
    @ BG2CNT: map base 8, size 0, wrap per BG_WRAP (bit 13)
    ldr r0, =0x0400000C
    ldr r1, =((8 << 8) | (BG_WRAP << 13))
    strh r1, [r0]
    ldr r0, =0x04000020
    ldr r1, =BG_PA
    strh r1, [r0]
    ldr r1, =BG_PB
    strh r1, [r0, #2]
    ldr r1, =BG_PC
    strh r1, [r0, #4]
    ldr r1, =BG_PD
    strh r1, [r0, #6]
    ldr r0, =0x04000028
    ldr r1, =BG_X0
    str r1, [r0]
    ldr r1, =BG_Y0
    str r1, [r0, #4]
    @ OBJ tiles (1D mapping, sprite 16x16 = tiles 2,3 / 4,5): solid 1,2 / 3,4
    ldr r0, =0x06010040
    ldr r1, =0x11111111
    mov r2, #8
o0: str r1, [r0], #4
    subs r2, r2, #1
    bne o0
    ldr r1, =0x22222222
    mov r2, #8
o1: str r1, [r0], #4
    subs r2, r2, #1
    bne o1
    ldr r1, =0x33333333
    mov r2, #8
o2: str r1, [r0], #4
    subs r2, r2, #1
    bne o2
    ldr r1, =0x44444444
    mov r2, #8
o3: str r1, [r0], #4
    subs r2, r2, #1
    bne o3
    @ OAM entries 0..7: 0 and 4 are sprites; 1-3,5-7 disabled (hold params)
    ldr r0, =0x07000008
    ldr r1, =0x0200
    strh r1, [r0]
    strh r1, [r0, #8]
    strh r1, [r0, #16]
    ldr r0, =0x07000028
    strh r1, [r0]
    strh r1, [r0, #8]
    strh r1, [r0, #16]
    @ matrix group 0 (entries 0-3 +6): OA
    ldr r0, =0x07000006
    ldr r1, =OA_PA
    strh r1, [r0]
    ldr r1, =OA_PB
    strh r1, [r0, #8]
    ldr r1, =OA_PC
    strh r1, [r0, #16]
    ldr r1, =OA_PD
    strh r1, [r0, #24]
    @ matrix group 1 (entries 4-7 +6): OB
    ldr r0, =0x07000026
    ldr r1, =OB_PA
    strh r1, [r0]
    ldr r1, =OB_PB
    strh r1, [r0, #8]
    ldr r1, =OB_PC
    strh r1, [r0, #16]
    ldr r1, =OB_PD
    strh r1, [r0, #24]
    @ sprite A: affine (bit8), y=20, x=30, 16x16, matrix 0, tile 2
    ldr r0, =0x07000000
    ldr r1, =((1 << 8) | 20)
    strh r1, [r0]
    ldr r1, =((1 << 14) | (0 << 9) | 30)
    strh r1, [r0, #2]
    mov r1, #2
    strh r1, [r0, #4]
    @ sprite B: affine, y=90, x=150, 16x16, matrix 1, tile 2
    ldr r0, =0x07000020
    ldr r1, =((1 << 8) | 90)
    strh r1, [r0]
    ldr r1, =((1 << 14) | (1 << 9) | 150)
    strh r1, [r0, #2]
    mov r1, #2
    strh r1, [r0, #4]
    @ mode 2, 1D OBJ, BG2 + OBJ
    ldr r0, =0x04000000
    ldr r1, =(2 | (1 << 6) | (1 << 10) | (1 << 12))
    strh r1, [r0]
hang:
    b hang
