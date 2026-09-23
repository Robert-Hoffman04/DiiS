// gx_rendermode.cpp - see gx_rendermode.h. Deliberately tiny: one global, one getter,
// one setter. Default must be RenderMode::GxAccurate (nds-wii-render-pipeline.md's
// correctness anchor: with no "rendermode" command ever sent, every build behaves exactly
// as it did before this module existed).
#include "gx_rendermode.h"

static RenderMode s_gxRenderMode = RenderMode::GxAccurate;

RenderMode gxRenderMode()
{
	return s_gxRenderMode;
}

void gxSetRenderMode(RenderMode mode)
{
	s_gxRenderMode = mode;
}
