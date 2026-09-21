@ lztrans.s - Synthetic GBA ROM for gx-next-steps-log.md queue item 13j (deferred CPU scanline
@ compositing). Every frame runs one of six schedules (f mod 6), each writing video state MID-FRAME
@ at fixed scanlines (busy-polled VCOUNT), so a single run contains quiet frames the GX path handles
@ and frames that force a barrier flush, an out-of-scope line, or a GX bail -- in both directions.
@ The test compares a per-frame hash of the output against the forced-CPU build (every frame must be
@ byte-identical; -DDSLZ_FRAMECRC).
@
@ Mode 0, BG0 (prio 1) + BG1 (prio 0) text 4bpp with tiles 1..3 (solid / striped), 2 OBJs (16x16).
@   f%6 == 0  quiet
@   f%6 == 1  BG0HOFS 0 -> 16 at line 80, back to 0 at line 120            (register barrier, GX bands)
@   f%6 == 2  BG1 mosaic on at line 60 (MOSAIC=0x33, BG1CNT bit6), off at line 100  (out-of-scope line)
@   f%6 == 3  BG palette entry rewritten at line 70                        (data barrier)
@   f%6 == 4  OAM attr1 (sprite X) rewritten at line 90; OBJ layer off lines 100..129 (data barrier + out-of-scope)
@   f%6 == 5  BG tile word rewritten at line 50; forced blank lines 110..119 (data barrier + out-of-scope)
@ Every frame, in VBlank (line 170): a palette entry and a tile word are rewritten (normal
@ per-frame updates that must NOT cost the frame its GX pass).

    .arm
    .section .text
    .global _start

    .macro FILL addr, val, words
    ldr r0, =\addr
    ldr r1, =\val
    ldr r2, =\words
1:  str r1, [r0], #4
    subs r2, r2, #1
    bne 1b
    .endm

    .macro HW16 addr, val
    ldr r0, =\addr
    ldr r1, =\val
    strh r1, [r0]
    .endm

    .macro WAITLINE n
    ldr r0, =0x04000006
1:  ldrh r1, [r0]
    cmp r1, #\n
    bne 1b
    .endm

_start:
    HW16 0x05000000, 0x2A52          @ backdrop
    HW16 0x05000002, 0x001F          @ BG idx1 red
    HW16 0x05000004, 0x03E0          @ idx2 green
    HW16 0x05000006, 0x7C00          @ idx3 blue
    HW16 0x05000202, 0x03FF          @ OBJ idx1 yellow
    HW16 0x05000204, 0x7C1F          @ OBJ idx2 magenta
    @ BG tiles: char0 (BG0): tile1 solid idx1, tile2 stripes idx1/idx2, tile3 idx2/idx3 ; char1 (BG1) offset 0x4000
    FILL 0x06000020, 0x11111111, 8
    FILL 0x06000040, 0x21212121, 8
    FILL 0x06000060, 0x32323232, 8
    FILL 0x06004020, 0x22222222, 8
    FILL 0x06004040, 0x13131313, 8
    FILL 0x06004060, 0x31313131, 8
    @ BG0 map (sb24 = 0x0600C000): entries (i & 3); BG1 map (sb25 = 0x0600C800): entries ((i+1) & 3)
    ldr r0, =0x0600C000
    ldr r3, =0x0600C800
    mov r2, #0
1:  and r4, r2, #3
    strh r4, [r0], #2
    add r4, r2, #1
    and r4, r4, #3
    strh r4, [r3], #2
    add r2, r2, #1
    cmp r2, #1024
    bne 1b
    @ OBJ tiles (1D, 4bpp): tiles 0..3 solid idx1/idx2
    FILL 0x06010000, 0x11111111, 32
    FILL 0x06010080, 0x22222222, 32
    @ OAM0: y=30 x=20 16x16 tile0 ; OAM1: y=100 x=120 16x16 tile 4
    HW16 0x07000000, 30
    HW16 0x07000002, 20
    HW16 0x07000004, 0
    HW16 0x07000008, 100
    HW16 0x0700000A, 120
    HW16 0x0700000C, 4
    @ hide the other 126 OAM entries (attr0 bit9 = disable)
    ldr r0, =0x07000010
    ldr r1, =0x0200
    mov r2, #126
2:  strh r1, [r0], #8
    subs r2, r2, #1
    bne 2b
    HW16 0x04000008, 0x1801          @ BG0CNT: prio1 char0 sb24
    HW16 0x0400000A, 0x1904          @ BG1CNT: prio0 char1 sb25 (char base 1 = bit2)
    HW16 0x04000000, 0x1340          @ mode0, BG0+BG1+OBJ, 1D
    mov r8, #0                       @ frame counter
frame:
    WAITLINE 0
    mov r7, r8
3:  cmp r7, #6
    subge r7, r7, #6
    bge 3b
    cmp r7, #1
    beq case1
    cmp r7, #2
    beq case2
    cmp r7, #3
    beq case3
    cmp r7, #4
    beq case4
    cmp r7, #5
    beq case5
    b vblank
case1:
    WAITLINE 80
    HW16 0x04000010, 16
    WAITLINE 120
    HW16 0x04000010, 0
    b vblank
case2:
    WAITLINE 60
    HW16 0x0400004C, 0x0033
    HW16 0x0400000A, 0x1944
    WAITLINE 100
    HW16 0x0400004C, 0
    HW16 0x0400000A, 0x1904
    b vblank
case3:
    WAITLINE 70
    ldr r0, =0x05000004
    ldr r1, =0x03E0
    eor r1, r1, r8, lsl #3
    strh r1, [r0]
    b vblank
case4:
    WAITLINE 90
    ldr r0, =0x07000002
    and r1, r8, #63
    add r1, r1, #20
    strh r1, [r0]
    WAITLINE 100
    HW16 0x04000000, 0x0340        @ OBJ layer disabled ...
    WAITLINE 130
    HW16 0x04000000, 0x1340        @ ... and back on (out-of-scope: OBJ enable is read frame-final by the GX path)
    b vblank
case5:
    WAITLINE 50
    ldr r0, =0x06000024
    ldr r1, =0x12121212
    eor r1, r1, r8
    str r1, [r0]
    WAITLINE 110
    HW16 0x04000000, 0x1380
    WAITLINE 120
    HW16 0x04000000, 0x1340
vblank:
    WAITLINE 170
    ldr r0, =0x05000006
    ldr r1, =0x7C00
    add r1, r1, r8
    strh r1, [r0]
    ldr r0, =0x06000068
    ldr r1, =0x33333333
    eor r1, r1, r8
    str r1, [r0]
    add r8, r8, #1
    b frame
