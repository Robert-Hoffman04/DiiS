@ Minimal synthetic GBA ROM: mode 3 bitmap, per-pixel diagonal gradient.
@ Hand-written raw ARM asm, assembled directly with devkitARM's
@ arm-none-eabi-as/ld (no libgba/BIOS runtime needed -- direct-boot style,
@ matching how this project's own GBA loader direct-boots a cartridge).
@ Built by tools/gba-refcheck/build_roms.sh; see docs/PLAN.md §4.3 item 8.

    .arm
    .section .text
    .global _start
_start:
    @ header occupies 0x08000000-0x080000BF; code starts at 0x080000C0
    ldr r0, =0x04000000        @ REG_DISPCNT
    ldr r1, =0x0403            @ mode 3 | BG2 enable
    strh r1, [r0]

    ldr r1, =0x06000000        @ VRAM base
    mov r2, #0                 @ y
yloop:
    mov r3, #0                 @ x
xloop:
    @ colour = x (5 bits) | (y<<5) (5 bits) | (((x+y)&0x1F)<<10)
    and r4, r3, #0x1F
    and r5, r2, #0x1F
    orr r6, r4, r5, lsl #5
    add r7, r3, r2
    and r7, r7, #0x1F
    orr r6, r6, r7, lsl #10
    strh r6, [r1], #2

    add r3, r3, #1
    cmp r3, #240
    blt xloop

    add r2, r2, #1
    cmp r2, #160
    blt yloop

halt:
    b halt
