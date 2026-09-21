// DS Engine A (main screen) instance of the GX 2D compositor (queue item 13, phase 1).
// See gx_ds_enginea_render.h for scope and gx_ds_engine_impl.inc for the shared body.
#define GXDS_CORE 0
#define GXDS_SCREEN MainScreen
#define GXDS_API(n) gxDsEngineA##n
#define GXDS_PLAN g_dsAFramePlan
#define GXDS_REGS g_dsABandRegs
#define GXDS_TAG "dsa"
#ifdef DSA_FORCE_CPU
#define GXDS_FORCE_CPU 1
#endif
#include "gx_ds_engine_impl.inc"
