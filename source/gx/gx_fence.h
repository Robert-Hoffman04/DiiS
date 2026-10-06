/*
    gx_fence.h - non-blocking GPU fences (Task gpu-overlap, gx-next-steps-log.md).

    A fence is a PE draw-sync token (GX_SetDrawSync) pushed into the FIFO after a batch of GX
    work; the GPU writes it to the PE token register once everything before it is through the
    pixel engine (texture reads, display-list reads, EFB copies). The CPU then waits for it
    (gxFenceWait) only right before it next writes or reads something that batch used, instead
    of draining the whole GPU with GX_DrawDone at the end of every compositor pass.

    Rules: issue only where GX FIFO writes are allowed (the emulation thread under vidmutex, or
    draw_thread under vidmutex) so the tokens enter the FIFO in counter order; waiting needs no
    lock (it only reads a PE register). Tokens are never 0, so 0 means "nothing to wait for".
    The residual waits show up as the perf_zones zone `gxfence`.
*/
#ifndef GX_FENCE_H
#define GX_FENCE_H

#include "../types.h"
#include <gccore.h>

extern u16 g_gxFenceNext;      // next token to issue (0: not initialised yet)
extern u16 g_gxFence3d;        // last batch that read the GX 3D pass's buffers (Engine A / readback)
extern u16 g_gxFencePresent;   // last draw_thread present batch (reads TopTex/BottomTex/GbaTopTex)

static inline u16 gxFenceIssue()
{
	if (!g_gxFenceNext) {   // start just past whatever the register holds, so no token reads as done early
		g_gxFenceNext = (u16)(GX_GetDrawSync() + 1);
		if (!g_gxFenceNext) g_gxFenceNext = 1;
	}
	const u16 t = g_gxFenceNext++;
	if (!g_gxFenceNext) g_gxFenceNext = 1;
	GX_SetDrawSync(t);
	GX_Flush();
	return t;
}

static inline bool gxFenceReached(u16 t)
{
	return t == 0 || (s16)(u16)(GX_GetDrawSync() - t) >= 0;
}

// Spins (PE register poll) until t is reached; time spent is zone `gxfence`.
void gxFenceWait(u16 t);
// Same, sleeping between polls (for draw_thread, which must not starve the emulation thread).
void gxFenceWaitSleep(u16 t);

#endif
