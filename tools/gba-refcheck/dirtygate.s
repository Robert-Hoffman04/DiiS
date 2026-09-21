@ dirtygate.s - Synthetic GBA ROM for gx-next-steps-log.md queue item 21 (GBA per-frame
@ dirty-bitmap hole). g_gbaFramePlan.beginFrame() clears the VRAM/palette dirty bitmaps
@ every frame and the GX path only ran a texture's rebake gate while that layer / sprite
@ was drawn, so a dependency byte written while a BG is disabled (or an OBJ is parked /
@ OBJ-disabled) was lost and the stale cached texture showed the frame it came back.
@
@ Mode 1 (BG0/BG1 text 4bpp, BG2 affine 8bpp wrap 128px), 1D OBJ. Each layer is a flat
@ colour band so nothing depends on texel-edge sampling:
@   BG0 rows 0-4  (y   0..39 ) tile1 red ; rewritten green (TILE bytes only)  while BG0 off
@   BG1 rows 5-9  (y  40..79 ) tile1 blue; rewritten yellow (PALETTE only)    while BG1 off
@   BG2 rows 10-14(y  80..119) tile1 red ; rewritten green (TILE bytes only)  while BG2 off (affine)
@   OBJ0 4bpp x=20  tile 2  red -> green (tile rewrite)    while PARKED (y=200)
@   OBJ1 4bpp x=60  tile 10 blue -> yellow (palette only)  while OBJ-DISABLED (DISPCNT bit12)
@   OBJ2 8bpp x=100 tile 64 magenta -> orange (tile rewrite) while OBJ-DISABLED
@ Timeline in vblanks (busy-polled DISPSTAT bit0): 20 BGs off + OBJ0 parked; 25 BG0 + OBJ0
@ tile rewrite; 30 BG2 tile rewrite; 35 BG pal1 rewrite; 40/50/60 BG0/BG1/BG2 back on; 70
@ OBJ0 unparked; 75 OBJ disabled; 80 OBJ1 palette + OBJ2 tile rewrite; 90 OBJ back on; hang.
@ Final state (capture at settle-frame >= 110): green/yellow/green bands, green/yellow/orange
@ sprites. A stale cache instead shows the OLD colours (red/blue/red, red/blue/magenta).

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

    .macro WAITVB n
    mov r5, #\n
1:  ldr r0, =0x04000004
2:  ldrh r1, [r0]
    tst r1, #1
    beq 2b
3:  ldrh r1, [r0]
    tst r1, #1
    bne 3b
    subs r5, r5, #1
    bne 1b
    .endm

_start:
    @ BG palette: idx0 black, idx1 red, idx2 green, pal1 idx1 blue
    HW16 0x05000002, 0x001F
    HW16 0x05000004, 0x03E0
    HW16 0x05000022, 0x7C00
    @ OBJ palette: idx1 red, idx2 green, idx5 magenta, idx6 orange, pal1 idx1 blue
    HW16 0x05000202, 0x001F
    HW16 0x05000204, 0x03E0
    HW16 0x0500020A, 0x7C1F
    HW16 0x0500020C, 0x021F
    HW16 0x05000222, 0x7C00

    @ BG tiles: BG0 tile1 @ char0+0x20 (4bpp idx1), BG1 tile1 @ char1+0x20, BG2 tile1 @ char2+0x40 (8bpp idx1)
    FILL 0x06000020, 0x11111111, 8
    FILL 0x06004020, 0x11111111, 8
    FILL 0x06008040, 0x01010101, 16
    @ maps: BG0 sb24 (0x0600C000) rows 0-4 tile1; BG1 sb25 (0x0600C800) rows 5-9 tile1 pal1;
    @ BG2 affine sb26 (0x0600D000, 16x16 bytes) rows 10-14 tile1
    FILL 0x0600C000, 0x00010001, 80
    FILL 0x0600C940, 0x10011001, 80
    FILL 0x0600D0A0, 0x01010101, 20

    @ OBJ tiles
    FILL 0x06010040, 0x11111111, 32
    FILL 0x06010140, 0x11111111, 32
    FILL 0x06011000, 0x05050505, 64
    @ OAM0..2 (16x16, y=130)
    HW16 0x07000000, 130
    HW16 0x07000002, ((1<<14)|20)
    HW16 0x07000004, 2
    HW16 0x07000008, 130
    HW16 0x0700000A, ((1<<14)|60)
    HW16 0x0700000C, (10|(1<<12))
    HW16 0x07000010, (130|(1<<13))
    HW16 0x07000012, ((1<<14)|100)
    HW16 0x07000014, 64

    @ BG regs: BG0CNT char0 sb24, BG1CNT char1 sb25, BG2CNT char2 sb26 wrap size0 (affine)
    HW16 0x04000008, (24<<8)
    HW16 0x0400000A, ((1<<2)|(25<<8))
    HW16 0x0400000C, ((2<<2)|(26<<8)|(1<<13))
    HW16 0x04000020, 0x100
    HW16 0x04000026, 0x100
    HW16 0x04000000, 0x1741

    WAITVB 20
    HW16 0x04000000, 0x1041          @ BGs off
    HW16 0x07000000, 200             @ OBJ0 parked
    WAITVB 5
    FILL 0x06000020, 0x22222222, 8   @ BG0 tile rewrite (disabled)
    FILL 0x06010040, 0x22222222, 32  @ OBJ0 tile rewrite (parked)
    WAITVB 5
    FILL 0x06008040, 0x02020202, 16  @ BG2 affine tile rewrite (disabled)
    WAITVB 5
    HW16 0x05000022, 0x03FF          @ BG pal1 idx1 -> yellow (BG1 palette-only)
    WAITVB 5
    HW16 0x04000000, 0x1141          @ BG0 on
    WAITVB 10
    HW16 0x04000000, 0x1341          @ BG1 on
    WAITVB 10
    HW16 0x04000000, 0x1741          @ BG2 on
    WAITVB 10
    HW16 0x07000000, 130             @ OBJ0 unparked
    WAITVB 5
    HW16 0x04000000, 0x0741          @ OBJ disabled
    WAITVB 5
    HW16 0x05000222, 0x03FF          @ OBJ pal1 idx1 -> yellow (OBJ1 palette-only)
    FILL 0x06011000, 0x06060606, 64  @ OBJ2 8bpp tile rewrite
    WAITVB 10
    HW16 0x04000000, 0x1741          @ OBJ back on
hang:
    b hang
    .ltorg
