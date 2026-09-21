/*
    gx_ds_enginea_render.h - DS Engine A (main screen) instance of the GX 2D
    compositor, queue item 13 phase 1 (gx-next-steps-log.md "Task 13").

    THE CODE IS SHARED WITH ENGINE B: source/gx/gx_ds_engine_impl.inc is compiled once as
    Engine B (gx_ds_engineb_render.cpp) and once as Engine A (gx_ds_enginea_render.cpp).
    The durable write-up of the compositor itself (Stage 0 band trace, read-set dirty
    gating, CI4/CI8+TLUT, windows / effects / MASTER_BRIGHT, the exact-vs-near-exact
    rules) is gx_ds_engineb_render.h and applies to Engine A verbatim; this header
    documents only what is different for Engine A.
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
void gxDsEngineAScanline(int line);
bool gxDsEngineARenderFrame();
void gxDsEngineAMarkWrite(u32 adr, u32 size);
void gxDsEngineAMarkVram(u32 lcdcOffset, u32 size);
void gxDsEngineAInvalidateAll();

#endif // GX_DS_ENGINEA_RENDER_H
