/*
    gx_swizzle.h - generic linear-to-GX-block (and back) texel reordering.

    Every GX texture format used by gx_texconv.* (I4/I8/CI4/CI8/RGB565/
    RGB5A3) is stored as rows of fixed-size rectangular blocks rather than
    linearly - see gx_texformat.h for the per-format block shapes. This file
    does that reordering once, generically, parameterized only by block
    width/height and bits-per-texel, so every converter in gx_texconv.cpp
    just decodes NDS texel data into a plain row-major (one array slot per
    texel) buffer and hands it here.

    Source/destination width and height must each be a multiple of the
    format's block width/height - true for every caller in this engine
    (NDS textures are powers of two >= 8; 2D tile caches are built in
    whole-8x8-tile units).

    Un-swizzle (GX block order -> linear) is provided for host-side testing
    (tools/gx-texconv-test) and for any future debug readback; the render
    engine itself never needs to read a GX texture back.
*/
#ifndef GX_SWIZZLE_H
#define GX_SWIZZLE_H

#include "../types.h"

// 4 bits/texel (I4, CI4). Two texels per byte: even x in the high nibble,
// odd x in the low nibble (matches GX/libogc I4/CI4 byte packing).
// `linear` holds one texel value (0-15) per byte, row-major, width*height
// entries. `dst` must be width*height/2 bytes.
bool gxSwizzle4bpp(const u8 *linear, u8 *dst, int blockW, int blockH, int width, int height);
bool gxUnswizzle4bpp(const u8 *src, u8 *linear, int blockW, int blockH, int width, int height);

// 8 bits/texel (I8, CI8). One texel value per byte.
// `dst`/`src` must be width*height bytes.
bool gxSwizzle8bpp(const u8 *linear, u8 *dst, int blockW, int blockH, int width, int height);
bool gxUnswizzle8bpp(const u8 *src, u8 *linear, int blockW, int blockH, int width, int height);

// 16 bits/texel (RGB565, RGB5A3). One packed texel value per u16, already
// in its final destination bit layout (see gx_texconv.*) - this function
// only reorders, it never touches bits within a texel.
// `dst`/`src` must be width*height u16s.
bool gxSwizzle16bpp(const u16 *linear, u16 *dst, int blockW, int blockH, int width, int height);
bool gxUnswizzle16bpp(const u16 *src, u16 *linear, int blockW, int blockH, int width, int height);

#endif // GX_SWIZZLE_H
