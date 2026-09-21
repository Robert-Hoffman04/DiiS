@ dirtybmp.s - Synthetic GBA ROM for gx-next-steps-log.md queue item 21, bitmap-mode layers.
@ Mode 4 bitmap (BG2), page 0: top half idx1 (red), bottom half idx3 (blue); page 1: idx4 (yellow).
@ Timeline (vblanks): 40 DISPCNT=mode 0 with nothing enabled (bitmap layer not drawn); 50 while
@ off: bottom half rewritten idx3->idx2 (green) and palette idx1 red->cyan; 60 mode 4 again
@ (must show cyan top / green bottom); 160 mode 3 (same bytes read as 16bpp, no writes);
@ 280 mode 5 (no writes); 400 mode 4 page 1 (DISPCNT bit4, no writes); 520 BG2 enable bit
@ off (mode 4 page 1), page-1 bytes + palette idx2 rewritten, 540 BG2 on again; hang.
@ The harness rounds a settle-frame up to a multiple of 60: capture with settle-frames
@ 115 / 235 / 355 / 475 / 595 (frames 120 / 240 / 360 / 480 / 600) for the steady states.

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
    HW16 0x05000002, 0x001F   @ idx1 red
    HW16 0x05000004, 0x03E0   @ idx2 green
    HW16 0x05000006, 0x7C00   @ idx3 blue
    HW16 0x05000008, 0x03FF   @ idx4 yellow
    FILL 0x06000000, 0x01010101, 4800   @ page 0 top 80 rows
    FILL 0x06004B00, 0x03030303, 4800   @ page 0 bottom 80 rows
    FILL 0x0600A000, 0x04040404, 9600   @ page 1
    HW16 0x04000000, 0x0404
    WAITVB 40
    HW16 0x04000000, 0x0000
    WAITVB 10
    FILL 0x06004B00, 0x02020202, 4800
    HW16 0x05000002, 0x7FE0
    WAITVB 10
    HW16 0x04000000, 0x0404
    WAITVB 100
    HW16 0x04000000, 0x0403
    WAITVB 120
    HW16 0x04000000, 0x0405
    WAITVB 120
    HW16 0x04000000, 0x0414
    WAITVB 120
    HW16 0x04000000, 0x0014          @ mode 4 page 1, BG2 (bit10) OFF: bitmap not drawn
    WAITVB 10
    FILL 0x0600A000, 0x02020202, 9600
    HW16 0x05000004, 0x7C1F          @ idx2 green -> magenta
    WAITVB 10
    HW16 0x04000000, 0x0414          @ BG2 back on: must show magenta
hang:
    b hang
    .ltorg
