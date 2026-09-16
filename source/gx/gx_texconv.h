/*
    gx_texconv.h - NDS texture/tile format -> GX format conversion.

    Implements the mapping table in nds-wii-texture-format-mapping.md.
    Every function here is a pure buffer-to-buffer transform: callers
    decode NDS VRAM/palette data down to a plain word/byte array first
    (the "which bank, which offset" bookkeeping is Stage 1 - resource sync
    - territory, not this file's), and get back a linear (row-major, one
    array slot per texel) buffer plus, for indexed formats, a TLUT. Run the
    result through gx_swizzle.h to get the final GX block-ordered layout.

    Deliberate deviation from the doc: TEXMODE_I2/I4/I8 are palette-indexed
    on real NDS hardware (confirmed against the current, still-active
    software rasterizer's decoder in source/texcache.cpp), not raw
    intensity, so they are mapped to GX CI4/CI8 + TLUT here rather than the
    paletteless GX I4/I8 the doc's table literally names - the same
    treatment the doc already gives the 2D BG/OBJ tile formats a few rows
    below. Mapping straight to I4/I8 would silently discard the game's
    palette and render every I-format 3D texture as greyscale.
*/
#ifndef GX_TEXCONV_H
#define GX_TEXCONV_H

#include "types.h"
#include "gx_color.h"

// ---- TEXMODE_A3I5 (3-bit alpha, 5-bit index, independent) -> CI8 ----
// Source texel byte is already the combined (alpha<<5)|index value used
// directly as the CI8 index - only the 256-entry TLUT needs building.
// `pal` is the texture's 32-entry (or fewer; unused entries read as index
// 0) BGR555 palette.
void gxBuildA3I5Tlut(const u16 *pal, int palCount, u16 outTlut[256]);

// ---- TEXMODE_A5I3 (5-bit alpha, 3-bit index, independent) -> CI8 ----
// Same trick, 8 indices x 32 alphas = 256 combinations. The 5-bit source
// alpha is truncated to RGB5A3's 3-bit alpha field (top 3 bits kept) -
// this is a real, unavoidable precision loss imposed by the destination
// hardware format, not a bug.
void gxBuildA5I3Tlut(const u16 *pal, int palCount, u16 outTlut[256]);

// ---- TEXMODE_I2/I4/I8 -> CI4/CI8 (see file header deviation note) ----
// Unpack N-bit-per-texel indices into one byte per texel (values preserved
// as-is, e.g. I2 indices stay 0-3) ready for gxSwizzle4bpp/8bpp. `rawLen`
// is the source byte count; outLen (== texel count) must be sized by the
// caller (4x/2x/1x rawLen for I2/I4/I8 respectively).
void gxUnpackI2Indices(const u8 *raw, int rawLen, u8 *outIndices);
void gxUnpackI4Indices(const u8 *raw, int rawLen, u8 *outIndices);
// I8 needs no unpacking (already one byte/texel) - swizzle the raw bytes
// directly with gxSwizzle8bpp.

// Builds an RGB5A3 TLUT for any indexed format (2D 4bpp/8bpp tiles, or the
// unpacked I2/I4/I8 indices above). `palCount` entries are converted;
// remaining TLUT slots up to `tlutSize` are left as index-0's color so an
// out-of-range index (shouldn't happen, but see texcache.cpp's dead-texel
// handling for 4x4) still reads something defined. When
// `indexZeroTransparent` is set, TLUT[0] gets alpha=0 (RGB5A3 alpha
// sub-format); every other entry is opaque.
void gxBuildIndexedTlutRGB5A3(const u16 *pal, int palCount, bool indexZeroTransparent,
                               u16 *outTlut, int tlutSize);

// Same, but RGB565 (used per the doc when index-0-transparent semantics
// don't apply for a given bank, trading the alpha bit for one more bit of
// green precision).
void gxBuildIndexedTlutRGB565(const u16 *pal, int palCount, u16 *outTlut, int tlutSize);

// ---- TEXMODE_16BPP (15-bit direct color + 1-bit alpha-enable) -> RGB5A3 ----
// Bit 15 of `ndsColor` is the alpha-enable flag: set -> texel is fully
// opaque, encoded via RGB5A3's opaque 5-5-5 sub-format (no precision
// loss); clear -> texel is fully transparent, encoded as RGB5A3's alpha
// sub-format with alpha=0 (RGB value is irrelevant and not preserved).
u16 gxConvertDirect16bppTexel(u16 ndsColor);
void gxConvertDirect16bppBuffer(const u16 *ndsColors, u16 *outRGB5A3, int count);

// ---- Bitmap-mode direct-color BG (GBA modes 3-5 / DS extended-BG bitmaps) ----
// No per-pixel alpha in these formats; NDS bit 15 (if present) is ignored.
u16 gxConvertBitmapTexelRGB565(u16 ndsColor);
u16 gxConvertBitmapTexelRGB5A3Opaque(u16 ndsColor);
void gxConvertBitmapBufferRGB565(const u16 *ndsColors, u16 *outRGB565, int count);
void gxConvertBitmapBufferRGB5A3(const u16 *ndsColors, u16 *outRGB5A3, int count);

// ---- TEXMODE_4X4 (NDS block-compressed) -> uncompressed RGB5A3 ----
// Decodes straight to full RGB5A3 texels rather than GX's CMPR - see
// nds-wii-texture-format-mapping.md for why re-compressing into CMPR is a
// deliberate non-goal. Mirrors the interpolation modes from the real
// hardware's per-block palette-select scheme (mode 0: color0/1/2 direct +
// transparent; mode 1: color0/1 + their average, no slot 3; mode 2:
// color0/1/2/3 all direct; mode 3: color0/1 + two 5:3-weighted blends),
// matching the block palette/index encoding already exercised by the
// current software rasterizer (source/texcache.cpp TEXMODE_4X4) - that
// interpolation math is a hardware fact, not implementation detail of the
// removed GX renderer.
//
// `indices` is the 32-bit little-endian-already-swapped index word for one
// 4x4 block (2 bits/texel, row-major, LE_TO_LOCAL_32 already applied by
// the caller). `palBase` points at the palette RAM bank the block's
// pal-info offset is relative to; `palInfoWord` is slot1's 16-bit value
// (LE_TO_LOCAL_16 already applied): bits 0-13 are the palette offset
// (halfwords, <<1 for bytes), bits 14-15 select the interpolation mode.
// `outTexels` receives 16 RGB5A3 texels, row-major.
void gxDecode4x4Block(u32 indices, u16 palInfoWord, const u16 *palBase, u16 outTexels[16]);

#endif // GX_TEXCONV_H
