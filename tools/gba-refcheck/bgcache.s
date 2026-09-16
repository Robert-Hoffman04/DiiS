@ bgcache.s - Synthetic GBA ROM for gx-next-steps-log.md task 5 (BG-plane
@ dirty-gating precision). Mode 0, three text BGs, each occupying its own
@ horizontal band of tile rows so all three are visible simultaneously in
@ one capture without occluding each other (unused rows/tile 0 are left
@ zero-filled, which is transparent regardless of palette bank -- text BG
@ color index 0 is always transparent):
@
@   BG0 (tile rows 2-5, ~y 16-47): solid red. Palette and VRAM tile/map
@       data are written once at boot and NEVER touched again, and BG0's
@       scroll registers are also never touched (HOFS=VOFS=0 always).
@       Proves the "genuinely static plane is never re-baked" case (see
@       gx-next-steps-log.md task 4's identical objcache.gba proof for
@       OBJ) -- bg0's cumulative bake count must stay 1 forever.
@   BG1 (tile rows 10-13, ~y 80-111): yellow/cyan vertical column stripes.
@       Tile/map/palette data are written once at boot and never touched
@       again, but BG1HOFS is incremented by 1 every single vblank forever
@       (including well past the checkpoint below) -- a continuous,
@       ordinary scrolling case. Proves the "scrolling alone must not
@       trigger a rebake" case (this task's main point for excluding
@       HOFS/VOFS from the fingerprint): bg1's cumulative bake count must
@       also stay 1 forever, despite the stripes visibly shifting frame to
@       frame.
@   BG2 (tile rows 17-19, ~y 136-159): solid magenta initially. Its
@       BG2CNT/map are never touched after boot, but at the ~60-vblank
@       checkpoint below its one tile's VRAM pixel bytes are rewritten in
@       place (index1 nibble -> index2 nibble, both sub-palette entries
@       already prepared at boot -- the checkpoint itself makes no
@       palette write at all, deliberately, so it isolates the pure
@       "dependency VRAM bytes changed" case from this task's
@       whole-BG-palette-bank dependency, which all three BGs here share
@       -- see gxBgPlaneNeedsRebake's header comment). This is the
@       "BG-config unchanged, but its dependency bytes changed" case,
@       same pattern as task 4's objcache.gba OAM1. bg2 must visibly turn
@       green and its bake count must go from 1 to 2, exactly once.
@
@ Backdrop (palette index 0) is set once at boot to a distinct dark gray
@ (not pure black, so a stuck-at-zero backdrop bake wouldn't accidentally
@ look "correct") and never touched again -- proves the backdrop rebake
@ gate (gxBackdropNeedsRebake) also correctly never re-fires.
@
@ Checkpoint timing: busy-polls DISPSTAT's VBlank flag for 60 real vblanks
@ (~1s at 60fps), then continues scrolling BG1 forever in the post-
@ checkpoint state -- so any capture at settle-frame > ~65 sees the
@ post-change frame (green BG2, BG1 stripes shifted further), and any
@ capture at settle-frame < 60 sees the pre-change frame, without needing
@ IRQs.

    .arm
    .section .text
    .global _start
_start:
    @ --- BG palette (0x05000000 base) ---
    @ idx0 (backdrop) = dark gray
    ldr r0, =0x05000000
    ldr r1, =0x18C6
    strh r1, [r0]
    @ idx1 (BG0 palNum0, tile1) = red
    ldr r1, =0x001F
    strh r1, [r0, #2]
    @ idx17 (BG1 palNum1, tile1) = yellow
    ldr r1, =0x03FF
    strh r1, [r0, #34]
    @ idx18 (BG1 palNum1, tile2) = cyan
    ldr r1, =0x7FE0
    strh r1, [r0, #36]
    @ idx33 (BG2 palNum2, tile1) = magenta (initial)
    ldr r1, =0x7C1F
    strh r1, [r0, #66]
    @ idx34 (BG2 palNum2, index2) = green. Deliberately written here at
    @ boot, NOT at the checkpoint below -- the checkpoint only rewrites
    @ BG2's tile1 VRAM bytes in place (nibble index1 -> index2), with no
    @ palette write at all, so it isolates the pure "VRAM dependency bytes
    @ changed" case from the whole-BG-palette-bank dependency this task's
    @ design deliberately shares across every BG plane (see
    @ gxBgPlaneNeedsRebake's header comment): a checkpoint that wrote any
    @ BG palette byte here would correctly (conservatively, not a bug)
    @ also re-arm BG0/BG1's rebake gate, since all three text BGs in this
    @ ROM share the same 512-byte BG palette bank dependency range, which
    @ would defeat this ROM's "another plane stays untouched" proof.
    ldr r1, =0x03E0
    strh r1, [r0, #68]

    @ --- BG0: char base block0 (0x06000000) ---
    @ tile0 (transparent, explicit zero for determinism)
    ldr r0, =0x06000000
    mov r1, #0
    mov r2, #8
bg0_tile0_zero:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg0_tile0_zero
    @ tile1 (+0x20): solid red (nibble index1)
    ldr r0, =0x06000020
    ldr r1, =0x11111111
    mov r2, #8
bg0_tile1_fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg0_tile1_fill

    @ BG0 tilemap: screen base block4 (0x06002000), 32x32. Rows 2-5 = tile1,
    @ else tile0.
    ldr r0, =0x06002000
    mov r2, #0                     @ row
bg0_maprow:
    mov r3, #0                     @ col
    cmp r2, #2
    blt bg0_zero_row
    cmp r2, #5
    bgt bg0_zero_row
    mov r6, #1
    b bg0_have_row
bg0_zero_row:
    mov r6, #0
bg0_have_row:
bg0_mapcol:
    strh r6, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt bg0_mapcol
    add r2, r2, #1
    cmp r2, #32
    blt bg0_maprow

    @ --- BG1: char base block0 (0x06000000) -- deliberately the SAME
    @ physical char base block as BG0, not block1. gxBgPlaneNeedsRebake's
    @ conservative char-VRAM dependency range is 1024*tileBytes = 32KB
    @ (4bpp) wide, which is *wider* than the 16KB spacing between adjacent
    @ char-base selector values -- so two BGs at adjacent char-base
    @ indices (e.g. 0 and 1) would have overlapping conservative ranges,
    @ and this ROM's whole point (BG1's own dependency bytes are never
    @ touched by BG2's checkpoint edit below) would break by construction,
    @ not because of any bug. Block0 and block2's 32KB ranges ([0,32K) and
    @ [32K,64K)) are exactly adjacent and non-overlapping, so BG0/BG1 share
    @ block0 (distinct tile *numbers* within it: BG0 uses 0/1, BG1 uses
    @ 2/3) and BG2 (below) uses block2 alone -- see that section for the
    @ matching other half of this reasoning.
    @ tile0 already zeroed by BG0's setup above (same physical address,
    @ shared transparent tile -- no need to zero it again).
    @ tile2 (+0x40): solid yellow (nibble index1)
    ldr r0, =0x06000040
    ldr r1, =0x11111111
    mov r2, #8
bg1_tile2_fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg1_tile2_fill
    @ tile3 (+0x60): solid cyan (nibble index2)
    ldr r0, =0x06000060
    ldr r1, =0x22222222
    mov r2, #8
bg1_tile3_fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg1_tile3_fill

    @ BG1 tilemap: screen base block9 (0x06004800), 32x32. Rows 10-13
    @ alternate tile2/tile3 by column parity (vertical stripes), else tile0.
    ldr r0, =0x06004800
    mov r2, #0
bg1_maprow:
    mov r3, #0
bg1_mapcol:
    cmp r2, #10
    blt bg1_zero_entry
    cmp r2, #13
    bgt bg1_zero_entry
    and r4, r3, #1
    add r6, r4, #2          @ tileNum 2 (yellow) or 3 (cyan)
    orr r6, r6, #(1 << 12)  @ map entry palNum field = 1 (BG1's own sub-palette,
                             @ idx16-31) -- without this the entry defaults to
                             @ palNum0 and reads BG0's red/undefined colors
                             @ instead of BG1's own yellow/cyan.
    b bg1_have_entry
bg1_zero_entry:
    mov r6, #0
bg1_have_entry:
    strh r6, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt bg1_mapcol
    add r2, r2, #1
    cmp r2, #32
    blt bg1_maprow

    @ --- BG2: char base block2 (0x06008000) ---
    @ tile0 (transparent, explicit zero)
    ldr r0, =0x06008000
    mov r1, #0
    mov r2, #8
bg2_tile0_zero:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg2_tile0_zero
    @ tile1 (+0x20): solid magenta (nibble index1) -- rewritten in place to
    @ nibble index2 (green) at the checkpoint below.
    ldr r0, =0x06008020
    ldr r1, =0x11111111
    mov r2, #8
bg2_tile1_fill:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg2_tile1_fill

    @ BG2 tilemap: screen base block17 (0x06008800), 32x32. Rows 17-19 =
    @ tile1, else tile0.
    ldr r0, =0x06008800
    mov r2, #0
bg2_maprow:
    mov r3, #0
    cmp r2, #17
    blt bg2_zero_row
    cmp r2, #19
    bgt bg2_zero_row
    ldr r6, =1 | (2 << 12)  @ tileNum1, palNum2 (BG2's own sub-palette,
                             @ idx32-47) -- see BG1's identical note above.
    b bg2_have_row
bg2_zero_row:
    mov r6, #0
bg2_have_row:
bg2_mapcol:
    strh r6, [r0], #2
    add r3, r3, #1
    cmp r3, #32
    blt bg2_mapcol
    add r2, r2, #1
    cmp r2, #32
    blt bg2_maprow

    @ --- BGxCNT ---
    @ BG0CNT (0x04000008): screen base4, char base0, 4bpp, 32x32, prio0
    ldr r0, =0x04000008
    ldr r1, =(4 << 8)
    strh r1, [r0]
    @ BG1CNT (0x0400000A): screen base9, char base0 (shared with BG0, see
    @ above), 4bpp, 32x32, prio1
    ldr r0, =0x0400000A
    ldr r1, =(9 << 8) | 1
    strh r1, [r0]
    @ BG2CNT (0x0400000C): screen base17, char base2, 4bpp, 32x32, prio2
    ldr r0, =0x0400000C
    ldr r1, =(17 << 8) | (2 << 2) | 2
    strh r1, [r0]

    @ DISPCNT: mode 0, BG0/BG1/BG2 enable (bits8-10)
    ldr r0, =0x04000000
    ldr r1, =(1 << 8) | (1 << 9) | (1 << 10)
    strh r1, [r0]

    @ --- Main loop: scroll BG1 every vblank forever; at the 60th vblank,
    @ perform the BG2 checkpoint edit once, then keep scrolling. ---
    mov r5, #0                     @ vblank counter
    mov r7, #0                     @ BG1 hofs value
mainloop:
wait_vbl_set:
    ldr r0, =0x04000004
    ldrh r1, [r0]
    tst r1, #1
    beq wait_vbl_set
wait_vbl_clr:
    ldrh r1, [r0]
    tst r1, #1
    bne wait_vbl_clr

    add r7, r7, #1
    ldr r0, =0x04000014            @ BG1HOFS
    strh r7, [r0]

    @ r5 increments by 1 every vblank and is never reset, so this compare
    @ is true on exactly one vblank (r5==60) -- a natural one-shot gate,
    @ no separate flag needed.
    cmp r5, #60
    bne skip_checkpoint
    @ No palette write here -- idx34 (green) was already prepared at boot
    @ (see above). Only BG2's tile1 VRAM bytes are rewritten, in place.
    ldr r0, =0x06008020              @ BG2 tile1 bytes, in place
    ldr r1, =0x22222222
    mov r2, #8
bg2_recolor:
    str r1, [r0], #4
    subs r2, r2, #1
    bne bg2_recolor
skip_checkpoint:
    add r5, r5, #1
    b mainloop
