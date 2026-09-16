@ gx-next-steps-log.md task 6: DISPCNT mode 4 (8bpp palette-indexed bitmap
@ BG2) coverage -- no existing tools/gba-refcheck ROM exercises any bitmap
@ mode at all. Fills the mode-4 front buffer (VRAM page 0) with a left/right
@ split: left half = palette index 1 (red), right half = palette index 2
@ (green). Mode 4 pixels are always opaque (GBATEK), unlike tiled BG/OBJ --
@ this specifically exercises gxBakeBitmapMode's mode==4 branch, whose CI8
@ TLUT deliberately does NOT force index 0 transparent (see that function's
@ comment) -- index 0 is never used by this ROM's own fill, but a real
@ regression there would show as a black band if some other decode path
@ mistakenly zeroed entry 0's alpha and something sampled it.

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

    @ Mode 4 front buffer (VRAM 0x06000000), 240x160 bytes, 1 byte/pixel.
    @ Left half (x<120) = index1, right half = index2. Fill 4 pixels/store.
    ldr r0, =0x06000000
    mov r5, #0                   @ row
rowloop:
    mov r6, #0                   @ col (in units of 4 pixels)
collloop:
    cmp r6, #30                  @ 30*4 = 120 = left half width
    ldrlt r1, =0x01010101
    ldrge r1, =0x02020202
    str r1, [r0], #4
    add r6, r6, #1
    cmp r6, #60                  @ 60*4 = 240 = full row width
    blt collloop
    add r5, r5, #1
    cmp r5, #160
    blt rowloop

    @ DISPCNT: mode 4, BG2 enable (bit10)
    ldr r0, =0x04000000
    ldr r1, =(4 | (1 << 10))
    strh r1, [r0]

halt:
    b halt
