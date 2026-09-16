#include "gx_mask.h"
#include <malloc.h>
#include <string.h>

bool gxMaskTarget_Init(GXMaskTarget *mt, u16 width, u16 height, u32 texFmt)
{
	if (width == 0 || height == 0)
		return false;

	u32 size = GX_GetTexBufferSize(width, height, texFmt, GX_FALSE, 0);
	void *data = memalign(32, size);
	if (!data)
		return false;
	memset(data, 0, size);
	DCFlushRange(data, size);

	mt->texData = data;
	mt->width = width;
	mt->height = height;
	mt->texFmt = texFmt;
	GX_InitTexObj(&mt->texObj, data, width, height, texFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	return true;
}

void gxMaskTarget_Free(GXMaskTarget *mt)
{
	if (mt->texData) {
		free(mt->texData);
		mt->texData = nullptr;
	}
}

void gxMaskBegin(GXMaskTarget *mt, u16 efbX, u16 efbY)
{
	GX_SetViewport(efbX, efbY, mt->width, mt->height, 0, 1);
	GX_SetScissor(efbX, efbY, mt->width, mt->height);
	GX_SetTexCopySrc(efbX, efbY, mt->width, mt->height);
}

void gxMaskEnd(GXMaskTarget *mt, u16 efbX, u16 efbY)
{
	(void)efbX;
	(void)efbY;
	GX_SetTexCopyDst(mt->width, mt->height, mt->texFmt, GX_FALSE);
	GX_CopyTex(mt->texData, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();
}

void gxMaskApply(GXMaskTarget *mt, u8 tevStage, u8 texMapSlot, u8 texCoordSlot)
{
	GX_LoadTexObj(&mt->texObj, texMapSlot);
	GX_SetTevOrder(tevStage, texCoordSlot, texMapSlot, GX_COLORNULL);

	// output = TEXC * CPREV, output_alpha = TEXA * APREV - modulate
	// whatever the prior TEV stage(s) produced by the mask's sampled
	// value. See the TEV formula note in gx_mask.h.
	GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO);
	GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
