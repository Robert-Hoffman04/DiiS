// DS Engine B (sub screen) instance of the GX 2D compositor. The whole body lives
// in gx_ds_engine_impl.inc (shared with Engine A, see the header there); this file
// just selects the core and the public names. Behaviour is the tasks 7-11 code.
#define GXDS_CORE 1
#define GXDS_SCREEN SubScreen
#define GXDS_API(n) gxDsEngineB##n
#define GXDS_PLAN g_dsBFramePlan
#define GXDS_REGS g_dsBBandRegs
#define GXDS_TAG "dsb"
#ifdef DSB_FORCE_CPU
#define GXDS_FORCE_CPU 1
#endif
#include "gx_ds_engine_impl.inc"

// Shared write-funnel entry points (declared in gx_ds_engineb_render.h): one call from
// MMU.cpp / GPU.cpp tags both engines' dirty plans.
void gxDsMarkWrite(u32 adr, u32 size)     { gxDsEngineAMarkWrite(adr, size); gxDsEngineBMarkWrite(adr, size); }
void gxDsMarkVram(u32 lcdcOffset, u32 size) { gxDsEngineAMarkVram(lcdcOffset, size); gxDsEngineBMarkVram(lcdcOffset, size); }
void gxDsInvalidateAll()                  { gxDsEngineAInvalidateAll(); gxDsEngineBInvalidateAll(); }
