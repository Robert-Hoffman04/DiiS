/*
    gx_ds_enginea_render.h - DS Engine A (main screen) instance of the GX 2D
    compositor, queue item 13 phase 1 (gx-next-steps-log.md "Task 13").

    THE CODE IS SHARED WITH ENGINE B: source/gx/gx_ds_engine_impl.inc is compiled once as
    Engine B (gx_ds_engineb_render.cpp) and once as Engine A (gx_ds_enginea_render.cpp).
    The durable write-up of the compositor itself (Stage 0 band trace, read-set dirty
    gating, CI4/CI8+TLUT, windows / effects / MASTER_BRIGHT, the exact-vs-near-exact
    rules) is gx_ds_engineb_render.h and applies to Engine A verbatim; this header
    documents only what is different for Engine A.

    Queue item 13j: Engine A's CPU lines are deferred exactly like Engine B's (see the
    "Task 13j" bullet in gx_ds_engineb_render.h). Engine-A-specific parts: a capture armed at
    a line's hook (or the DISPCAPCNT enable bit set) is an out-of-scope state, so the line
    renders eagerly and the capture unit runs on real composited lines; a DISPCAPCNT /
    DISPCNT / MASTER_BRIGHT / BLDCNT ... write is a register barrier; the 3D layer input
    (gfx3d_convertedScreen) only changes at VBlank end, so it needs no barrier.
*/
#ifndef GX_DS_ENGINEA_RENDER_H
#define GX_DS_ENGINEA_RENDER_H

#include "../types.h"
#include "gx_frameplan.h"
#include "gx_ds_engineb_render.h"   // GxDsBBandRegs (shared band-register snapshot type)

extern GxFramePlan g_dsAFramePlan;
extern GxDsBBandRegs g_dsABandRegs[GX_DSB_MAX_BAND_REGS];

bool gxDsEngineARenderInit();
void gxDsEngineARenderShutdown();
bool gxDsEngineAScanline(int line);   // true = CPU pass for this line deferred (queue item 13j)
bool gxDsEngineARenderFrame();
void gxDsEngineAMarkWrite(u32 adr, u32 size);
void gxDsEngineAMarkVram(u32 lcdcOffset, u32 size);
void gxDsEngineAInvalidateAll();

#endif // GX_DS_ENGINEA_RENDER_H
