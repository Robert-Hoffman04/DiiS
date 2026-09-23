#include "gx_mask.h"
#include <malloc.h>
#include <string.h>

// main.cpp's InitVideo() sets GX_SetCopyFilter(rmode->aa, rmode->sample_pattern,
// GX_TRUE, rmode->vfilter) exactly once as part of the "present" state
// (gx-next-steps-log.md task 7/12's GxRestorePresentState() pattern) and
// nothing else in the renderer ever touches it again - every ordinary
// EFB->XFB display copy is meant to use that AA/deflicker filter. But
// GX_SetTexCopySrc/GX_SetTexCopyDst/GX_CopyTex here share the SAME hardware
// copy-filter registers, and a filtered copy is wrong for a mask: the
// 7-tap vertical deflicker filter (rmode->vfilter, coefficients summing to
// 64) blends up to +/-3 EFB scanlines into every output texel, and with AA
// on, each of those samples is itself a 3-subsample blend. For a full-frame
// display copy that is invisible (it is designed to blend the real
// picture's own neighbouring content). For gxMaskBegin/End's small scratch
// corner, the filter blends in whatever real scene content sits just
// outside the scratch region on every row near its top/bottom edge, and at
// AA sample density that contaminates far more than just the edge rows.
// This was Task 13d's "GX_TF_I8 came back majority neither-0-nor-255 even
// for a flat single-colour full-screen quad" symptom: rmode's filter was
// never disabled for the mask copy, so it was never actually an unfiltered
// 1:1 EFB->texture readback. Root-caused and fixed 2026-09-22 (see
// gx-next-steps-log.md "Task gx_mask-fix"): force the filter off for the
// mask's own copy and restore rmode's real filter immediately after, the
// same save/restore discipline GxRestorePresentState() already applies to
// the other present-state registers this primitive doesn't own.
extern GXRModeObj *rmode;

bool gxMaskTarget_Init(GXMaskTarget *mt, u16 width, u16 height, u32 texFmt)
{
	if (width == 0 || height == 0)
		return false;

	// GX_CTF_* are copy-only formats: GX_InitTexObj keeps only the low nibble,
	// so e.g. GX_CTF_A8 (0x27) would sample as nonexistent hw format 7. The
	// 8-bit single-channel copies write the I8 tile layout; sample them as I8.
	u32 sampleFmt = texFmt;
	if (texFmt == GX_CTF_R8 || texFmt == GX_CTF_G8 || texFmt == GX_CTF_B8 || texFmt == GX_CTF_A8)
		sampleFmt = GX_TF_I8;

	u32 size = GX_GetTexBufferSize(width, height, sampleFmt, GX_FALSE, 0);
	void *data = memalign(32, size);
	if (!data)
		return false;
	memset(data, 0, size);
	DCFlushRange(data, size);

	mt->texData = data;
	mt->width = width;
	mt->height = height;
	mt->texFmt = texFmt;
	GX_InitTexObj(&mt->texObj, data, width, height, sampleFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&mt->texObj, GX_NEAR, GX_NEAR);
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

	// Disable AA/deflicker for this one copy - see the note above extern
	// rmode at the top of this file. aa=GX_FALSE + vf=GX_FALSE means "all
	// sample points centered" / "default 1-line filter" per libogc's own
	// GX_SetCopyFilter() docs, i.e. an exact unfiltered 1:1 readback; the
	// sample_pattern/vfilter arrays are unused in that mode but GX_SetCopyFilter
	// still dereferences them, so pass zeroed scratch arrays rather than rmode's
	// (which are laid out for the AA-on case).
	static const u8 kNoSamplePattern[12][2] = {{0}};
	static const u8 kNoVFilter[7] = {0};
	GX_SetCopyFilter(GX_FALSE, (u8 (*)[2])kNoSamplePattern, GX_FALSE, (u8 *)kNoVFilter);

	GX_SetTexCopyDst(mt->width, mt->height, mt->texFmt, GX_FALSE);
	GX_CopyTex(mt->texData, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();

	// Restore the real display-copy filter immediately - draw_thread's own
	// EFB->XFB copy (and any other consumer) still needs it.
	if (rmode)
		GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
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

void gxMaskApplyInverse(GXMaskTarget *mt, u8 tevStage, u8 texMapSlot, u8 texCoordSlot)
{
	GX_LoadTexObj(&mt->texObj, texMapSlot);
	GX_SetTevOrder(tevStage, texCoordSlot, texMapSlot, GX_COLORNULL);

	// The standard TEV lerp combiner is out = A*(1-C) + B*C (+D, D=0 here).
	// Picking A = CPREV/APREV (the running value), B = ZERO, C = TEXC/TEXA
	// (the mask) gives out = running * (1 - mask) - the complement of
	// gxMaskApply's running * mask.
	GX_SetTevColorIn(tevStage, GX_CC_CPREV, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ZERO);
	GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

	GX_SetTevAlphaIn(tevStage, GX_CA_APREV, GX_CA_ZERO, GX_CA_TEXA, GX_CA_ZERO);
	GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
