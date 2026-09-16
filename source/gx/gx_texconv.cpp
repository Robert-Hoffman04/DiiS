#include "gx_texconv.h"

void gxBuildA3I5Tlut(const u16 *pal, int palCount, u16 outTlut[256])
{
	for (int v = 0; v < 256; ++v) {
		int idx = v & 0x1F;
		u8 alpha3 = (u8)(v >> 5);
		u16 ndsColor = (idx < palCount) ? pal[idx] : 0;
		outTlut[v] = gxPackRGB5A3AlphaFull(alpha3, ndsColor);
	}
}

void gxBuildA5I3Tlut(const u16 *pal, int palCount, u16 outTlut[256])
{
	for (int v = 0; v < 256; ++v) {
		int idx = v & 0x07;
		u8 alpha5 = (u8)(v >> 3);
		u8 alpha3 = alpha5 >> 2; // 5-bit source truncated to RGB5A3's 3-bit field
		u16 ndsColor = (idx < palCount) ? pal[idx] : 0;
		outTlut[v] = gxPackRGB5A3AlphaFull(alpha3, ndsColor);
	}
}

void gxUnpackI2Indices(const u8 *raw, int rawLen, u8 *outIndices)
{
	for (int i = 0; i < rawLen; ++i) {
		u8 b = raw[i];
		outIndices[i * 4 + 0] = b & 0x3;
		outIndices[i * 4 + 1] = (b >> 2) & 0x3;
		outIndices[i * 4 + 2] = (b >> 4) & 0x3;
		outIndices[i * 4 + 3] = (b >> 6) & 0x3;
	}
}

void gxUnpackI4Indices(const u8 *raw, int rawLen, u8 *outIndices)
{
	for (int i = 0; i < rawLen; ++i) {
		u8 b = raw[i];
		outIndices[i * 2 + 0] = b & 0xF;
		outIndices[i * 2 + 1] = (b >> 4) & 0xF;
	}
}

void gxBuildIndexedTlutRGB5A3(const u16 *pal, int palCount, bool indexZeroTransparent,
                               u16 *outTlut, int tlutSize)
{
	u16 zeroColor = (palCount > 0) ? gxPackRGB5A3AlphaFull(0, pal[0]) : 0;
	for (int i = 0; i < tlutSize; ++i) {
		if (i >= palCount) {
			outTlut[i] = zeroColor;
			continue;
		}
		if (i == 0 && indexZeroTransparent)
			outTlut[i] = gxPackRGB5A3AlphaFull(0, pal[0]);
		else
			outTlut[i] = gxPackRGB5A3Opaque(gxExtractBGR555(pal[i]));
	}
}

void gxBuildIndexedTlutRGB565(const u16 *pal, int palCount, u16 *outTlut, int tlutSize)
{
	u16 zeroColor = (palCount > 0) ? gxPackRGB565(gxExtractBGR555(pal[0])) : 0;
	for (int i = 0; i < tlutSize; ++i) {
		outTlut[i] = (i < palCount) ? gxPackRGB565(gxExtractBGR555(pal[i])) : zeroColor;
	}
}

u16 gxConvertDirect16bppTexel(u16 ndsColor)
{
	bool opaque = (ndsColor & 0x8000) != 0;
	if (opaque)
		return gxPackRGB5A3Opaque(gxExtractBGR555(ndsColor));
	return gxPackRGB5A3Alpha(0, gxExtractBGR555(ndsColor));
}

void gxConvertDirect16bppBuffer(const u16 *ndsColors, u16 *outRGB5A3, int count)
{
	for (int i = 0; i < count; ++i)
		outRGB5A3[i] = gxConvertDirect16bppTexel(ndsColors[i]);
}

u16 gxConvertBitmapTexelRGB565(u16 ndsColor)
{
	return gxPackRGB565(gxExtractBGR555(ndsColor));
}

u16 gxConvertBitmapTexelRGB5A3Opaque(u16 ndsColor)
{
	return gxPackRGB5A3Opaque(gxExtractBGR555(ndsColor));
}

void gxConvertBitmapBufferRGB565(const u16 *ndsColors, u16 *outRGB565, int count)
{
	for (int i = 0; i < count; ++i)
		outRGB565[i] = gxConvertBitmapTexelRGB565(ndsColors[i]);
}

void gxConvertBitmapBufferRGB5A3(const u16 *ndsColors, u16 *outRGB5A3, int count)
{
	for (int i = 0; i < count; ++i)
		outRGB5A3[i] = gxConvertBitmapTexelRGB5A3Opaque(ndsColors[i]);
}

// 5:3-weighted blend in 5-bit-channel domain: (5*a + 3*b) / 8, matching the
// current software rasterizer's mode-3 interpolation (there computed in
// 8-bit-channel domain as (5*a+3*b)>>6 - equivalent ratio, same truncation).
static inline u8 gxBlend53(u8 a, u8 b)
{
	return (u8)((5 * a + 3 * b) >> 3);
}

static inline GxRgb5 gxAvgRgb5(GxRgb5 a, GxRgb5 b)
{
	GxRgb5 out;
	out.r = (u8)((a.r + b.r) >> 1);
	out.g = (u8)((a.g + b.g) >> 1);
	out.b = (u8)((a.b + b.b) >> 1);
	return out;
}

static inline GxRgb5 gxBlend53Rgb5(GxRgb5 a, GxRgb5 b)
{
	GxRgb5 out;
	out.r = gxBlend53(a.r, b.r);
	out.g = gxBlend53(a.g, b.g);
	out.b = gxBlend53(a.b, b.b);
	return out;
}

void gxDecode4x4Block(u32 indices, u16 palInfoWord, const u16 *palBase, u16 outTexels[16])
{
	u32 colorHalfwordBase = (u32)(palInfoWord & 0x3FFF) * 2;
	u8 mode = (u8)(palInfoWord >> 14);

	GxRgb5 color0 = gxExtractBGR555(palBase[colorHalfwordBase + 0]);
	GxRgb5 color1 = gxExtractBGR555(palBase[colorHalfwordBase + 1]);

	u16 slot[4];
	slot[0] = gxPackRGB5A3Opaque(color0);
	slot[1] = gxPackRGB5A3Opaque(color1);

	switch (mode) {
		case 0:
			slot[2] = gxPackRGB5A3Opaque(gxExtractBGR555(palBase[colorHalfwordBase + 2]));
			slot[3] = gxPackRGB5A3Alpha(0, GxRgb5{0, 0, 0}); // fully transparent
			break;
		case 1:
			slot[2] = gxPackRGB5A3Opaque(gxAvgRgb5(color0, color1));
			slot[3] = gxPackRGB5A3Alpha(0, GxRgb5{0, 0, 0});
			break;
		case 2:
			slot[2] = gxPackRGB5A3Opaque(gxExtractBGR555(palBase[colorHalfwordBase + 2]));
			slot[3] = gxPackRGB5A3Opaque(gxExtractBGR555(palBase[colorHalfwordBase + 3]));
			break;
		case 3:
			slot[2] = gxPackRGB5A3Opaque(gxBlend53Rgb5(color0, color1));
			slot[3] = gxPackRGB5A3Opaque(gxBlend53Rgb5(color1, color0));
			break;
	}

	for (int row = 0; row < 4; ++row) {
		u8 rowBits = (u8)((indices >> (row * 8)) & 0xFF);
		outTexels[row * 4 + 0] = slot[rowBits & 3];
		outTexels[row * 4 + 1] = slot[(rowBits >> 2) & 3];
		outTexels[row * 4 + 2] = slot[(rowBits >> 4) & 3];
		outTexels[row * 4 + 3] = slot[(rowBits >> 6) & 3];
	}
}
