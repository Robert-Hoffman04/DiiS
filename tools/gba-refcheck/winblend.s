@ winblend.s - Synthetic GBA ROM exercising WIN0/WINOUT layer gating and
@ BLDCNT/BLDALPHA alpha-blend *without* mosaic (see gx-next-steps-log.md
@ task 2). Deliberately windowcheck.s minus mosaic: task 2 ports WIN0/WIN1
@ and BLDCNT/BLDALPHA/BLDY blend natively into the GX path
@ (gx_gba_render.cpp) but leaves mosaic (all forms) CPU-bailed, so
@ windowcheck.s's own mosaic bit means g_gbaFramePlan.isHot(GXHOT_MOSAIC)
@ is still set and it *still* bails to the CPU compositor under task 2 --
@ useless as a "does the new native GX path actually engage" check. This
@ ROM has no mosaic anywhere, so GXHOT_MOSAIC stays clear and
@ gxGbaRenderFrame() actually exercises the new window/blend code instead
@ of bailing.
@
@ Also widens BLDCNT's 2nd-target set from windowcheck.s's (backdrop only)
@ to (BG0 + backdrop), so gxComputeBandBlendPlan's native-alpha-blend
@ precondition (gx_gba_render.h's "Blend" section: every layer active this
@ band that isn't itself 1st-target must be 2nd-target, since a single
@ painter's-algorithm GX pass can't make hardware blending conditional on
@ which specific layer produced the destination pixel) is satisfied --
@ this content never actually needs BG0 to be a valid 2nd target (BG0 is
@ window-gated off everywhere BLDCNT's blend applies), but the
@ *precondition* can't know that without per-pixel reasoning, so it's
@ conservative; widening the 2nd-target mask here is how real content
@ would satisfy it too, not a special-case for this test.
@
@ Mode 0, two text BGs:
@  - BG0: 4bpp red/blue 8x8-tile checkerboard, no mosaic (fine, uniform).
@  - BG1: 4bpp solid green fullscreen.
@ WIN0 covers the left half of the screen (x 0-119, full height) and
@ enables ONLY BG0 inside it (WININ); WINOUT enables ONLY BG1 outside it,
@ plus the color-special-effect bit. BLDCNT alpha-blends BG1 (1st target)
@ against the backdrop (2nd target, white) at EVA=EVB=8 (50/50) -- this
@ only engages outside WIN0, since WININ's effect bit is clear there.
@
@ Expected correct output:
@  - left half:  fine, uniform red/blue 8px checker (BG0, window-selected,
@    NOT blended since WININ's effect bit is 0)
@  - right half: pale green ((123,255,123) at the alpha-blend midpoint --
@    same math as windowcheck.s's right half) (BG1 blended 50/50 against
@    the white backdrop, window-selected, blend engaged since WINOUT's
@    effect bit is 1)
@ A window/blend-unaware renderer would instead show one uniform fine
@ red/blue checkerboard across the whole screen (BG0, frontmost/opaque,
@ window/blend registers never consulted).

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
    @ 32x32, priority 0. No mosaic bit this time (windowcheck.s's bit6).
    ldr r0, =0x04000008
    ldr r1, =(8 << 8)
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

    @ MOSAIC (0x0400004C): left at its power-on-default 0 -- no mosaic
    @ anywhere in this ROM (see header comment).

    @ BLDCNT (0x04000050): 1st target BG1 (bit1), effect=alpha blend
    @ (bits6-7 = 01), 2nd target BG0 (bit8) + backdrop (bit13).
    ldr r0, =0x04000050
    ldr r1, =0x2142
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
