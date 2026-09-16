#include "gx_swizzle.h"

// Shared address math for all three bit depths: which block a texel falls
// in, and which texel index within that block (row-major, 0 = top-left).
// Blocks themselves are laid out row-major across the image.
static inline void gxBlockAddress(int x, int y, int blockW, int blockH, int width,
                                   int &blockIndex, int &inBlockIndex)
{
	int blocksPerRow = width / blockW;
	int blockX = x / blockW;
	int blockY = y / blockH;
	int inX = x % blockW;
	int inY = y % blockH;
	blockIndex = blockY * blocksPerRow + blockX;
	inBlockIndex = inY * blockW + inX;
}

static inline bool gxCheckDims(int blockW, int blockH, int width, int height)
{
	if (blockW <= 0 || blockH <= 0 || width <= 0 || height <= 0)
		return false;
	if ((width % blockW) != 0 || (height % blockH) != 0)
		return false;
	return true;
}

bool gxSwizzle4bpp(const u8 *linear, u8 *dst, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int texelsPerBlock = blockW * blockH;
	int bytesPerBlock = texelsPerBlock / 2;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			int byteOff = blockIndex * bytesPerBlock + inBlockIndex / 2;
			u8 nibble = linear[y * width + x] & 0xF;
			if ((inBlockIndex & 1) == 0)
				dst[byteOff] = (dst[byteOff] & 0x0F) | (nibble << 4);
			else
				dst[byteOff] = (dst[byteOff] & 0xF0) | nibble;
		}
	}
	return true;
}

bool gxUnswizzle4bpp(const u8 *src, u8 *linear, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int texelsPerBlock = blockW * blockH;
	int bytesPerBlock = texelsPerBlock / 2;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			int byteOff = blockIndex * bytesPerBlock + inBlockIndex / 2;
			u8 byte = src[byteOff];
			linear[y * width + x] = (inBlockIndex & 1) == 0 ? (byte >> 4) : (byte & 0xF);
		}
	}
	return true;
}

bool gxSwizzle8bpp(const u8 *linear, u8 *dst, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int bytesPerBlock = blockW * blockH;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			dst[blockIndex * bytesPerBlock + inBlockIndex] = linear[y * width + x];
		}
	}
	return true;
}

bool gxUnswizzle8bpp(const u8 *src, u8 *linear, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int bytesPerBlock = blockW * blockH;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			linear[y * width + x] = src[blockIndex * bytesPerBlock + inBlockIndex];
		}
	}
	return true;
}

bool gxSwizzle16bpp(const u16 *linear, u16 *dst, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int texelsPerBlock = blockW * blockH;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			dst[blockIndex * texelsPerBlock + inBlockIndex] = linear[y * width + x];
		}
	}
	return true;
}

bool gxUnswizzle16bpp(const u16 *src, u16 *linear, int blockW, int blockH, int width, int height)
{
	if (!gxCheckDims(blockW, blockH, width, height))
		return false;
	int texelsPerBlock = blockW * blockH;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int blockIndex, inBlockIndex;
			gxBlockAddress(x, y, blockW, blockH, width, blockIndex, inBlockIndex);
			linear[y * width + x] = src[blockIndex * texelsPerBlock + inBlockIndex];
		}
	}
	return true;
}
