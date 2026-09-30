// gx_rendermode.cpp - see gx_rendermode.h. Deliberately tiny: one global, one getter,
// one setter. Default must be RenderMode::GxAccurate (nds-wii-render-pipeline.md's
// correctness anchor: with no "rendermode" command ever sent, every build behaves exactly
// as it did before this module existed).
#include "gx_rendermode.h"

#ifdef DESMUME_FORCE_RENDERMODE
// Bench/test-only compile-time override: -DDESMUME_FORCE_RENDERMODE=2 (0 Software, 1 GxAccurate, 2 GxFast)
static RenderMode s_gxRenderMode = (RenderMode)DESMUME_FORCE_RENDERMODE;
#else
static RenderMode s_gxRenderMode = RenderMode::GxAccurate;
#endif

RenderMode gxRenderMode()
{
	return s_gxRenderMode;
}

void gxSetRenderMode(RenderMode mode)
{
	s_gxRenderMode = mode;
}
