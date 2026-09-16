/*
    gx_mask.h - the "mask-texture" primitive from nds-wii-render-pipeline.md's
    design principle: render a shape into a private scratch corner of the
    EFB, copy that region out to a texture, then TEV-multiply it into a
    later draw. Used for both the shadow mask (Stage 3) and the OBJ window
    mask (Stage 4) - the doc's whole point in introducing this pattern is
    that both become the same two-step primitive instead of two different
    ad hoc live-alpha-channel tricks, so this file is that shared step.

    Deliberately narrow scope: this primitive owns the scratch EFB region
    and the EFB->texture copy only. It does not set up Z test, blend mode,
    or vertex draw state for whatever geometry is rendered *into* the mask
    - shadow-volume accumulation (front/back face additive/subtractive
    blending) and OBJ-window coverage (plain opaque fill) need different
    draw state, and that choice belongs to those stages, not here.

    GX-only (libogc), unlike gx_texconv/gx_swizzle - there is no meaningful
    host-side simulation of an EFB copy, so this is verified by actually
    driving it from a stage (shadow/OBJ-window) and comparing against
    Dolphin/hardware, not a standalone unit test.
*/
#ifndef GX_MASK_H
#define GX_MASK_H

#include <gccore.h>
#include "../types.h"

struct GXMaskTarget {
	GXTexObj texObj;
	void *texData;   // memalign(32, GX_GetTexBufferSize(...)) - owned
	u16 width, height;
	u32 texFmt;
};

// Allocates the backing texture buffer and initializes the GX texture
// object. `texFmt` defaults to GX_TF_I8 - a single 0-255 mask value per
// texel is exactly what both consumers need (shadow coverage, OBJ-window
// coverage), and it's the cheapest format that provides it. Returns false
// if the allocation fails or width/height are zero.
bool gxMaskTarget_Init(GXMaskTarget *mt, u16 width, u16 height, u32 texFmt = GX_TF_I8);
void gxMaskTarget_Free(GXMaskTarget *mt);

// Points viewport, scissor, and the GX texture-copy source at a
// (width x height) scratch region of the EFB at (efbX, efbY), so
// subsequent draws land there instead of the real display area. GX has no
// way to read back and save the caller's previous viewport/scissor (it's
// a write-only FIFO), so the caller is responsible for re-issuing its own
// GX_SetViewport/GX_SetScissor after gxMaskEnd().
void gxMaskBegin(GXMaskTarget *mt, u16 efbX, u16 efbY);

// Copies the scratch region into mt's texture (GX_CopyTex) and invalidates
// the texture cache so a subsequent GX_LoadTexObj picks up the fresh
// content rather than a stale TMEM copy.
void gxMaskEnd(GXMaskTarget *mt, u16 efbX, u16 efbY);

// Binds mt's texture at `texMapSlot`/`texCoordSlot` and configures
// `tevStage` to multiply the TEV pipeline's running color and alpha
// (GX_CC_CPREV / GX_CA_APREV - i.e. whatever the prior stage(s) produced)
// by the mask's sampled value - the "TEV-multiply it in later" half of the
// pattern. The caller must already have TEXCOORD generation set up for
// `texCoordSlot` (typically the same UV the masked geometry itself uses,
// since a mask is drawn 1:1 over the content it gates) and must call this
// after setting up every earlier TEV stage, since it reads GX_CC_CPREV.
void gxMaskApply(GXMaskTarget *mt, u8 tevStage, u8 texMapSlot, u8 texCoordSlot);

#endif // GX_MASK_H
