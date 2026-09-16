/*
    gx_color.h - NDS BGR555 <-> GX RGB565/RGB5A3 texel packing.

    NDS/GBA 15-bit color words are the canonical GBATEK "X BBBBB GGGGG
    RRRRR" layout: bit15 is a per-format flag (alpha-enable for 3D direct
    textures, unused for palette entries), bits 14-10 are blue, 9-5 green,
    4-0 red.

    GX RGB5A3 is itself two formats selected by its own top bit: 1RRRRRGGGGG
    BBBBB (opaque, full 5-5-5 color) or 0AAARRRRGGGGBBBB (3-bit alpha,
    4-4-4 color). Where the NDS source has real alpha, the natural match is
    RGB5A3's alpha sub-format, since its 3-bit alpha field is already the
    same width as the NDS's own 3-bit texture alpha (TEXMODE_A3I5) - no
    alpha resampling needed there, only RGB gets truncated from 5 to 4 bits.
*/
#ifndef GX_COLOR_H
#define GX_COLOR_H

#include "../types.h"

struct GxRgb5 { u8 r, g, b; };

inline GxRgb5 gxExtractBGR555(u16 c)
{
	GxRgb5 out;
	out.r = c & 0x1F;
	out.g = (c >> 5) & 0x1F;
	out.b = (c >> 10) & 0x1F;
	return out;
}

// GX RGB565: 5-bit R and B pass through unchanged; G needs 5->6 bit
// expansion, done by bit replication (top bit repeated into the new LSB)
// so 0 stays 0 and 31 maps to 63, the standard lossless-at-the-ends
// upscale for fixed-point color channels.
inline u16 gxPackRGB565(GxRgb5 c)
{
	u8 g6 = (c.g << 1) | (c.g >> 4);
	return (u16)((c.r << 11) | (g6 << 5) | c.b);
}

// RGB5A3 opaque sub-format: top bit set, then a direct 5-5-5 copy - this is
// bit-for-bit identical to the NDS's own 15-bit color word (sans the
// NDS-side flag bit), so no precision is lost here at all.
inline u16 gxPackRGB5A3Opaque(GxRgb5 c)
{
	return (u16)(0x8000 | (c.r << 10) | (c.g << 5) | c.b);
}

// RGB5A3 alpha sub-format: top bit clear, 3-bit alpha (0-7, caller-supplied
// - already the NDS's own alpha width for every case this engine hits: see
// nds-wii-texture-format-mapping.md), RGB truncated 5->4 bits by dropping
// the LSB (simple, deterministic; a rounding upgrade is a later tuning
// pass, not a correctness requirement).
inline u16 gxPackRGB5A3Alpha(u8 alpha3, GxRgb5 c)
{
	u8 r4 = c.r >> 1, g4 = c.g >> 1, b4 = c.b >> 1;
	return (u16)(((alpha3 & 0x7) << 12) | (r4 << 8) | (g4 << 4) | b4);
}

inline u16 gxPackRGB5A3AlphaFull(u8 alpha3, u16 ndsColor)
{
	return gxPackRGB5A3Alpha(alpha3, gxExtractBGR555(ndsColor));
}

#endif // GX_COLOR_H
