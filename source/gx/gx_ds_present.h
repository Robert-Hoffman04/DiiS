/*
    gx_ds_present.h - DS direct present of the engines' EFB copies (Task directpresent,
    gx-next-steps-log.md; nds-wii-render-pipeline.md Stage 7).

    Before this, a DS engine frame handled on GX went EFB -> GX_CopyTex (tiled RGB5A3)
    -> CPU unswizzle + RGB5A3->X-B5-G5-R5 into GPU_screen -> Draw() re-swizzled
    GPU_screen back into a tiled RGB5A3 present texture. For an opaque copy the last
    texture is bit-identical to the first (RGB15_REVERSE is the exact inverse of the
    readback's conversion), so the round trip is skipped:

      - On a GX-handled frame with no MASTER_BRIGHT on any line and every copied texel
        in the opaque RGB5A3 sub-format (bit 15 set), the engine only records its copy
        buffer as "pending" for its physical GPU_screen slot (0 = top, 1 = bottom).
        (Task mbright: MASTER_BRIGHT frames too, see below.)
        GPU_screen for that slot is then stale.
      - Draw() binds a pending slot's copy buffer as that screen's present texture
        instead of converting GPU_screen (draw_thread's geometry, layout, swap and
        rotation handling are unchanged: it only picks the texture).
      - Every reader or partial writer of GPU_screen calls gxDsPresentResolve*() first,
        which does the old unswizzle + conversion (bit-exact) and clears the pending
        state. Readers: harness PKT_FRAME, DSLZ_FRAMECRC, DESMUME_FBDUMP, NDS_WritePNG,
        gpu_savestate. Writers: GPU_RenderLine (bail frames, skipped-line MASTER_BRIGHT,
        display-off/VRAM/FIFO modes, capture), gpu_loadstate / Screen_Reset (these
        overwrite the whole buffer, so they just drop the pending state).
      - Task mbright: a frame with MASTER_BRIGHT on any line first gets the fade applied
        to the EFB on the GPU (gxDsPresentMasterBright, exact per line), then goes direct
        like any other. Every copy is opaque (the EFB has no alpha channel at copy time),
        so the old per-frame opaque scan is gone.

    The copy buffers are only written by their engine's GX_CopyTex under vidmutex, and
    draw_thread samples them under vidmutex followed by GX_DrawDone, so a copy can never
    overwrite a buffer the GPU is still reading. Deviation from the pipeline doc: this
    is per-engine (the engines still copy separately), not the shared Stage 6
    composition.
*/
#ifndef GX_DS_PRESENT_H
#define GX_DS_PRESENT_H

#include "../types.h"

// Pending tiled-RGB5A3 copy buffer per GPU_screen slot, NULL when GPU_screen is current.
extern const void *g_gxDsPresentPending[2];

// Engine side: record `buf` (256x192 tiled RGB5A3, all texels opaque) as slot's content.
void gxDsPresentSet(int slot, const void *buf);
// Engine side, before its GX_CopyTex overwrites `buf`: resolve any slot still pointing at it,
// except keepSlot, which the caller rewrites whole right after (dropped unconverted; lazyfix).
void gxDsPresentReleaseBuffer(const void *buf, int keepSlot);

void gxDsPresentResolveSlotImpl(int slot);
static inline void gxDsPresentResolveSlot(int slot)
{
	if (g_gxDsPresentPending[slot]) gxDsPresentResolveSlotImpl(slot);
}
// slot from an NDS_Screen offset (0 or 192)
static inline void gxDsPresentResolveOffset(int offset) { gxDsPresentResolveSlot(offset ? 1 : 0); }
static inline void gxDsPresentResolveAll() { gxDsPresentResolveSlot(0); gxDsPresentResolveSlot(1); }
// GPU_screen is being overwritten wholesale: forget the pending state.
void gxDsPresentDropAll();

// Task mbright: apply MASTER_BRIGHT (per line: mode 0 none / 1 up / 2 down, factor 0..16)
// to the 256x192 EFB corner in place, before the engine's RGB5A3 copy. Under vidmutex, copy
// filter off, EFB RGB8/RGB565. Leaves TEV/blend state non-standard (caller resyncs). Returns
// false (EFB untouched) if its buffers can't be allocated.
bool gxDsPresentMasterBright(const u8 *mode, const u8 *fac);

#endif
