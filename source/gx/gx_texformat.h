/*
    gx_texformat.h - GX destination texture/TLUT formats and block shapes,
    for the from-scratch GX render engine (see nds-wii-render-pipeline.md
    and nds-wii-texture-format-mapping.md at the project root).

    GX textures are not stored linearly - the hardware fetches them as an
    array of fixed-size rectangular blocks (Hollywood/Flipper TEV texture
    fetch units). Every conversion in gx_texconv.* has to write into that
    block order; gx_swizzle.* does the block-address arithmetic once, here,
    rather than each converter re-deriving it.
*/
#ifndef GX_TEXFORMAT_H
#define GX_TEXFORMAT_H

#include "types.h"

// Destination GX texture formats actually used by the NDS->GX mapping table.
// (GX also defines C4/C8/C14X2/CMPR/RGBA8 variants not needed here - see
// nds-wii-texture-format-mapping.md for why CMPR is deliberately not used.)
enum GXTexFmt {
	GXTEXFMT_I4,       // 4bpp intensity, no alpha
	GXTEXFMT_I8,       // 8bpp intensity, no alpha
	GXTEXFMT_CI4,      // 4bpp palette index (+ separate TLUT)
	GXTEXFMT_CI8,      // 8bpp palette index (+ separate TLUT)
	GXTEXFMT_RGB565,   // 16bpp, 5-6-5, opaque
	GXTEXFMT_RGB5A3,   // 16bpp, mode-selected: 1-5-5-5 opaque or 0-3-4-4-4 w/ alpha
};

// GX TLUT (palette) formats. RGB5A3 is the only one this engine's mapping
// table needs (see gx_texconv.h) but IA8/RGB565 exist on the hardware too.
enum GXTlutFmt {
	GXTLUTFMT_IA8,
	GXTLUTFMT_RGB565,
	GXTLUTFMT_RGB5A3,
};

struct GXBlockShape {
	u8 texelsWide;
	u8 texelsTall;
};

// Texel block dimensions per destination format (bits-per-texel implied by
// the format enum itself - see gxBitsPerTexel()).
inline GXBlockShape gxBlockShape(GXTexFmt fmt)
{
	switch (fmt) {
		case GXTEXFMT_I4:      return GXBlockShape{8, 8};
		case GXTEXFMT_I8:      return GXBlockShape{8, 4};
		case GXTEXFMT_CI4:     return GXBlockShape{8, 8};
		case GXTEXFMT_CI8:     return GXBlockShape{8, 4};
		case GXTEXFMT_RGB565:  return GXBlockShape{4, 4};
		case GXTEXFMT_RGB5A3:  return GXBlockShape{4, 4};
	}
	return GXBlockShape{1, 1};
}

// Bits occupied by one texel in its packed (pre-swizzle) linear buffer.
inline u8 gxBitsPerTexel(GXTexFmt fmt)
{
	switch (fmt) {
		case GXTEXFMT_I4:
		case GXTEXFMT_CI4:
			return 4;
		case GXTEXFMT_I8:
		case GXTEXFMT_CI8:
			return 8;
		case GXTEXFMT_RGB565:
		case GXTEXFMT_RGB5A3:
			return 16;
	}
	return 8;
}

#endif // GX_TEXFORMAT_H
