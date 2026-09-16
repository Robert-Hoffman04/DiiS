@ Synthetic GBA ROM for gx-next-steps-log.md task 4 (per-OAM-index OBJ
@ texture caching). Three 4bpp regular (non-affine) 16x16 sprites, mode 0,
@ 1D OBJ mapping, no BG layer (pure black backdrop for contrast):
@
@   OAM index 0 (x=20,y=20):  tile 2,  pal0 (red).     Never touched again
@       after initial setup -- proves a sprite whose OAM fields AND
@       dependency VRAM/palette bytes are both untouched across many
@       frames is genuinely never re-baked (see profile.log's "objcache"
@       lines: oam0's cumulative bake count should stay 1 forever).
@   OAM index 1 (x=100,y=20): tile 10, pal1 (blue) initially. At the
@       60-vblank checkpoint below, its OAM attribute words are NOT
@       touched again, but tile 10's VRAM bytes are overwritten in place
@       (index1 nibble -> index2 nibble) and pal1's index2 entry is
@       written (green) -- this is the "OAM entry unchanged, but its
@       dependency bytes changed" case: the sprite must visibly turn
@       green and oam1's bake count must go from 1 to 2.
@   OAM index 2 (x=180,y=20 -> x=180,y=60): tile 30, pal3 (magenta)
@       initially. At the same checkpoint its attribute0/1/2 are
@       completely rewritten to point at a different, already-prepared
@       tile/palette (tile 40, pal4 = yellow) at a different position --
@       simulating a different sprite reusing this OAM index. Must
@       render correctly (yellow, moved), not stale magenta, and oam2's
@       bake count must go from 1 to 2.
@
@ Checkpoint timing: busy-polls DISPSTAT's VBlank flag for 60 real vblanks
@ (~1s at 60fps) before making the above changes, then hangs forever in
@ the new, final state -- so any capture at settle-frame > ~65 sees the
@ post-change frame, and any capture at settle-frame < 60 sees the
@ pre-change frame, without needing IRQs.

    .arm
    .section .text
    .global _start
_start:
    @ --- OBJ palette (0x05000200 + palNum*32 + idx*2) ---
    @ pal0 idx1 = red
    ldr r0, =0x05000202
    ldr r1, =0x001F
    strh r1, [r0]
    @ pal1 idx1 = blue (initial OAM1 color)
    ldr r0, =0x05000222
    ldr r1, =0x7C00
    strh r1, [r0]
    @ pal1 idx2 = green is written later, at the checkpoint (left zeroed
    @ until then) -- OAM1's post-checkpoint color reuses pal1's own idx2
    @ slot (not a different palNum), proving the dependency this task
    @ tracks is "pal1's bytes", not "which palNum the OAM entry names".
    @ pal3 idx1 = magenta (initial OAM2 color)
    ldr r0, =0x05000262
    ldr r1, =0x7C1F
    strh r1, [r0]
    @ pal4 idx1 = yellow (OAM2's post-checkpoint color, prepared up front)
    ldr r0, =0x05000282
    ldr r1, =0x03FF
    strh r1, [r0]

    @ --- OBJ tiles (4bpp, 32 bytes/tile, OBJ char base VRAM+0x10000, 1D
    @ mapping) -- each 16x16 sprite is a 2x2 tile grid = 4 contiguous
    @ tiles = 128 bytes, filled with nibble index1 (0x11111111 words). ---

    @ OAM0: tiles 2-5 @ 0x06010040
    ldr r0, =0x06010040
    ldr r1, =0x11111111
    mov r2, #32
tile_oam0:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile_oam0

    @ OAM1: tiles 10-13 @ 0x06010140
    ldr r0, =0x06010140
    ldr r1, =0x11111111
    mov r2, #32
tile_oam1:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile_oam1

    @ OAM2 initial: tiles 30-33 @ 0x060103C0
    ldr r0, =0x060103C0
    ldr r1, =0x11111111
    mov r2, #32
tile_oam2:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile_oam2

    @ OAM2's post-checkpoint tiles 40-43 @ 0x06010500 -- prepared now (not
    @ at the checkpoint) so the checkpoint only has to rewrite OAM2's
    @ attribute words, isolating the "OAM-fields-changed" case from the
    @ "dependency-bytes-changed" case OAM1 covers.
    ldr r0, =0x06010500
    ldr r1, =0x11111111
    mov r2, #32
tile_oam2b:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile_oam2b

    @ --- OAM entries ---
    @ OAM0 (index 0, offset 0x07000000): y=20, shape0/size1 (16x16), x=20,
    @ tile 2, pal0.
    ldr r0, =0x07000000
    mov r1, #20
    strh r1, [r0]
    ldr r1, =(1 << 14) | 20
    strh r1, [r0, #2]
    mov r1, #2
    strh r1, [r0, #4]

    @ OAM1 (index 1, offset 0x07000008): y=20, size1, x=100, tile10, pal1.
    ldr r0, =0x07000008
    mov r1, #20
    strh r1, [r0]
    ldr r1, =(1 << 14) | 100
    strh r1, [r0, #2]
    ldr r1, =10 | (1 << 12)
    strh r1, [r0, #4]

    @ OAM2 (index 2, offset 0x07000010): y=20, size1, x=180, tile30, pal3.
    ldr r0, =0x07000010
    mov r1, #20
    strh r1, [r0]
    ldr r1, =(1 << 14) | 180
    strh r1, [r0, #2]
    ldr r1, =30 | (3 << 12)
    strh r1, [r0, #4]

    @ DISPCNT: mode 0, 1D OBJ mapping (bit6), OBJ enable (bit12). No BG
    @ layer enabled -- pure black backdrop behind the sprites.
    ldr r0, =0x04000000
    ldr r1, =((1 << 6) | (1 << 12))
    strh r1, [r0]

    @ --- Wait 60 vblanks, busy-polling DISPSTAT (0x04000004) bit0 ---
    mov r5, #60
waitframes:
wait_vbl_set:
    ldr r0, =0x04000004
    ldrh r1, [r0]
    tst r1, #1
    beq wait_vbl_set
wait_vbl_clr:
    ldrh r1, [r0]
    tst r1, #1
    bne wait_vbl_clr
    subs r5, r5, #1
    bne waitframes

    @ --- Checkpoint: OAM1's dependency-bytes-dirty case ---
    @ pal1 idx2 = green (0x05000200 + 1*32 + 2*2 = 0x05000224)
    ldr r0, =0x05000224
    ldr r1, =0x03E0
    strh r1, [r0]
    @ Rewrite tile10-13's pixel nibbles from index1 to index2 in place
    @ (same VRAM addresses OAM1 already points at -- its OAM attribute
    @ words below are never touched).
    ldr r0, =0x06010140
    ldr r1, =0x22222222
    mov r2, #32
tile_oam1_recolor:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tile_oam1_recolor

    @ --- Checkpoint: OAM2's slot-reuse case (OAM fields rewritten, no new
    @ VRAM/palette write here -- tile40-43/pal4 were already prepared
    @ above) ---
    ldr r0, =0x07000010
    mov r1, #60
    strh r1, [r0]
    ldr r1, =(1 << 14) | 180
    strh r1, [r0, #2]
    ldr r1, =40 | (4 << 12)
    strh r1, [r0, #4]

hang:
    b hang
