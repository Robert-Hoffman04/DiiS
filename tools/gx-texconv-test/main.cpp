// Host-side unit tests for source/gx/gx_swizzle.* and source/gx/gx_texconv.*
// (PLAN next-step item 1: NDS->GX texture format/swizzle library). Pure
// buffer-in/buffer-out logic with no NDS/GX/Wii dependency, so this builds
// and runs with the host's own g++ - no devkitPPC, no Dolphin, no hardware.
//
// Build: g++ -std=gnu++17 -I../../source main.cpp ../../source/gx/gx_swizzle.cpp
//            ../../source/gx/gx_texconv.cpp -o gx_texconv_test
// Run:   ./gx_texconv_test

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "gx/gx_swizzle.h"
#include "gx/gx_texconv.h"
#include "gx/gx_texformat.h"

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		++g_failures; \
	} \
} while (0)

// ---------------------------------------------------------------------
// Swizzle round-trip: for every block shape this engine uses, a random
// linear image swizzled then unswizzled must come back byte-identical.
// This is the property that actually matters - GX fetches texels in block
// order, so any addressing bug here corrupts every texture uniformly.
// ---------------------------------------------------------------------

static void test_swizzle_roundtrip_8bpp(int blockW, int blockH, int width, int height)
{
	std::vector<u8> src(width * height), swz(width * height), back(width * height);
	for (int i = 0; i < width * height; ++i)
		src[i] = (u8)(i * 37 + 11);

	CHECK(gxSwizzle8bpp(src.data(), swz.data(), blockW, blockH, width, height));
	CHECK(gxUnswizzle8bpp(swz.data(), back.data(), blockW, blockH, width, height));
	CHECK(memcmp(src.data(), back.data(), src.size()) == 0);
}

static void test_swizzle_roundtrip_4bpp(int blockW, int blockH, int width, int height)
{
	std::vector<u8> src(width * height), back(width * height);
	std::vector<u8> swz(width * height / 2, 0);
	for (int i = 0; i < width * height; ++i)
		src[i] = (u8)(i * 5 + 3) & 0xF;

	CHECK(gxSwizzle4bpp(src.data(), swz.data(), blockW, blockH, width, height));
	CHECK(gxUnswizzle4bpp(swz.data(), back.data(), blockW, blockH, width, height));
	CHECK(memcmp(src.data(), back.data(), src.size()) == 0);
}

static void test_swizzle_roundtrip_16bpp(int blockW, int blockH, int width, int height)
{
	std::vector<u16> src(width * height), swz(width * height), back(width * height);
	for (int i = 0; i < width * height; ++i)
		src[i] = (u16)(i * 12345 + 6789);

	CHECK(gxSwizzle16bpp(src.data(), swz.data(), blockW, blockH, width, height));
	CHECK(gxUnswizzle16bpp(swz.data(), back.data(), blockW, blockH, width, height));
	CHECK(memcmp(src.data(), back.data(), src.size() * 2) == 0);
}

// A single 8x8 block, 8bpp: block 0 should just be the image verbatim.
static void test_swizzle_single_block_identity()
{
	std::vector<u8> src(8 * 4), swz(8 * 4);
	for (int i = 0; i < 8 * 4; ++i) src[i] = (u8)i;
	CHECK(gxSwizzle8bpp(src.data(), swz.data(), 8, 4, 8, 4));
	CHECK(memcmp(src.data(), swz.data(), src.size()) == 0);
}

// Two blocks side by side (16x4, block 8x4): block 1's data must land
// starting at byte 32 (bytesPerBlock), not interleaved with block 0.
static void test_swizzle_two_blocks_layout()
{
	const int w = 16, h = 4, bw = 8, bh = 4;
	std::vector<u8> src(w * h), swz(w * h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			src[y * w + x] = (x < 8) ? 0xAA : 0xBB;

	CHECK(gxSwizzle8bpp(src.data(), swz.data(), bw, bh, w, h));
	for (int i = 0; i < 32; ++i) CHECK(swz[i] == 0xAA);
	for (int i = 32; i < 64; ++i) CHECK(swz[i] == 0xBB);
}

// 4bpp packing order: even x -> high nibble, odd x -> low nibble.
static void test_swizzle_4bpp_nibble_order()
{
	u8 src[2] = {0x5, 0xA};
	u8 dst[1] = {0};
	CHECK(gxSwizzle4bpp(src, dst, 8, 8, 2, 1) == false); // not block-aligned (2x1 vs 8x8), must reject
	// Pad to a full 8x8 block instead.
	std::vector<u8> lin(64, 0);
	lin[0] = 0x5; lin[1] = 0xA;
	std::vector<u8> packed(32, 0);
	CHECK(gxSwizzle4bpp(lin.data(), packed.data(), 8, 8, 8, 8));
	CHECK((packed[0] >> 4) == 0x5);
	CHECK((packed[0] & 0xF) == 0xA);
}

static void test_swizzle_rejects_misaligned_dims()
{
	std::vector<u8> a(1), b(1);
	CHECK(gxSwizzle8bpp(a.data(), b.data(), 8, 4, 7, 4) == false);
	CHECK(gxSwizzle8bpp(a.data(), b.data(), 8, 4, 8, 3) == false);
}

// ---------------------------------------------------------------------
// gx_color.h packing
// ---------------------------------------------------------------------

static void test_color_rgb565_endpoints()
{
	GxRgb5 black{0, 0, 0}, white{31, 31, 31};
	CHECK(gxPackRGB565(black) == 0x0000);
	CHECK(gxPackRGB565(white) == 0xFFFF);
}

static void test_color_rgb5a3_opaque_is_bitexact_passthrough()
{
	// Opaque RGB5A3 should reproduce the source NDS 15-bit color exactly
	// (modulo the top flag bit, which becomes RGB5A3's own opaque marker).
	GxRgb5 c{17, 3, 29};
	u16 packed = gxPackRGB5A3Opaque(c);
	CHECK((packed & 0x8000) != 0);
	CHECK(((packed >> 10) & 0x1F) == 17);
	CHECK(((packed >> 5) & 0x1F) == 3);
	CHECK((packed & 0x1F) == 29);
}

static void test_color_rgb5a3_alpha_zero_is_fully_transparent()
{
	u16 packed = gxPackRGB5A3AlphaFull(0, 0x7FFF);
	CHECK((packed & 0x8000) == 0);
	CHECK(((packed >> 12) & 0x7) == 0);
}

// ---------------------------------------------------------------------
// gx_texconv.h - A3I5 / A5I3
// ---------------------------------------------------------------------

static void test_a3i5_tlut_alpha_zero_is_transparent_index_zero_is_index()
{
	u16 pal[32];
	for (int i = 0; i < 32; ++i) pal[i] = (u16)(i); // arbitrary distinct colors
	u16 tlut[256];
	gxBuildA3I5Tlut(pal, 32, tlut);

	// v = 0 -> alpha3=0, index=0 -> fully transparent
	CHECK((tlut[0] & 0x8000) == 0);
	CHECK(((tlut[0] >> 12) & 0x7) == 0);

	// v = (7<<5)|3 -> alpha3=7 (max), index=3
	int v = (7 << 5) | 3;
	CHECK((tlut[v] & 0x8000) == 0);
	CHECK(((tlut[v] >> 12) & 0x7) == 7);
}

static void test_a5i3_alpha_truncated_to_3_bits()
{
	u16 pal[8];
	for (int i = 0; i < 8; ++i) pal[i] = 0x7FFF;
	u16 tlut[256];
	gxBuildA5I3Tlut(pal, 8, tlut);

	// alpha5 = 31 (max) -> truncated top-3-bits -> alpha3 = 7
	int v = (31 << 3) | 5;
	CHECK(((tlut[v] >> 12) & 0x7) == 7);
	// alpha5 = 3 -> top 3 bits of 00011 = 0
	v = (3 << 3) | 5;
	CHECK(((tlut[v] >> 12) & 0x7) == 0);
}

// ---------------------------------------------------------------------
// gx_texconv.h - I2/I4 index unpacking (the doc-deviation path: these are
// palette indices, not raw intensity - see gx_texconv.h header comment)
// ---------------------------------------------------------------------

static void test_i2_unpack_preserves_index_values_lsb_first()
{
	u8 raw[1] = {0b11'01'10'00}; // texel0=00b=0, texel1=10b=2, texel2=01b=1, texel3=11b=3
	u8 out[4];
	gxUnpackI2Indices(raw, 1, out);
	CHECK(out[0] == 0);
	CHECK(out[1] == 2);
	CHECK(out[2] == 1);
	CHECK(out[3] == 3);
}

static void test_i4_unpack_preserves_index_values_lsb_first()
{
	u8 raw[1] = {0xA5}; // low nibble 0x5 = texel0, high nibble 0xA = texel1
	u8 out[2];
	gxUnpackI4Indices(raw, 1, out);
	CHECK(out[0] == 0x5);
	CHECK(out[1] == 0xA);
}

static void test_indexed_tlut_rgb5a3_zero_transparent()
{
	u16 pal[4] = {0x1234, 0x5678, 0x0F0F, 0x7FFF};
	u16 tlut[16];
	gxBuildIndexedTlutRGB5A3(pal, 4, true, tlut, 16);
	CHECK((tlut[0] & 0x8000) == 0);      // index 0 -> alpha sub-format
	CHECK(((tlut[0] >> 12) & 0x7) == 0); // alpha = 0
	CHECK((tlut[1] & 0x8000) != 0);      // index 1 -> opaque sub-format
	// out-of-range index (>= palCount) reads back as index 0's color, not garbage
	CHECK(tlut[15] == tlut[0]);
}

// ---------------------------------------------------------------------
// gx_texconv.h - TEXMODE_16BPP alpha-enable branching
// ---------------------------------------------------------------------

static void test_direct16bpp_opaque_bit_selects_subformat()
{
	u16 opaqueSrc = 0x8000 | (5 << 10) | (9 << 5) | 17; // bit15 set
	u16 transparentSrc = (5 << 10) | (9 << 5) | 17;      // bit15 clear, same RGB

	u16 opaqueOut = gxConvertDirect16bppTexel(opaqueSrc);
	u16 transparentOut = gxConvertDirect16bppTexel(transparentSrc);

	CHECK((opaqueOut & 0x8000) != 0);
	CHECK((transparentOut & 0x8000) == 0);
	CHECK(((transparentOut >> 12) & 0x7) == 0);
}

// ---------------------------------------------------------------------
// gx_texconv.h - 4x4-compressed block decode
// ---------------------------------------------------------------------

// Mode 2, every texel index 0: whole block should be a single solid color
// (color0, opaque) - the simplest possible correctness check that doesn't
// depend on getting the blend math right.
static void test_4x4_mode2_all_index0_is_solid_color0()
{
	u16 pal[4] = {
		(u16)((10) | (20 << 5) | (30 << 10)), // color0 (RGB555, no top bit needed here)
		0x0000, 0x0000, 0x0000,
	};
	u16 palInfo = (2 << 14) | 0; // mode 2, offset 0 halfwords
	u32 indices = 0; // every 2-bit index field is 0
	u16 outTexels[16];
	gxDecode4x4Block(indices, palInfo, pal, outTexels);

	u16 expected = gxPackRGB5A3Opaque(GxRgb5{10, 20, 30});
	for (int i = 0; i < 16; ++i) CHECK(outTexels[i] == expected);
}

// Mode 0, index 3 is the transparent sentinel regardless of which colors
// are in the palette.
static void test_4x4_mode0_index3_is_transparent()
{
	u16 pal[4] = {0x1111, 0x2222, 0x3333, 0x4444};
	u16 palInfo = (0 << 14) | 0;
	u32 indices = 0xFFFFFFFF; // every texel index = 3 (0b11)
	u16 outTexels[16];
	gxDecode4x4Block(indices, palInfo, pal, outTexels);
	for (int i = 0; i < 16; ++i) CHECK((outTexels[i] & 0x8000) == 0 && ((outTexels[i] >> 12) & 0x7) == 0);
}

// Mode 1, index 2 is the average of color0/color1, not a palette read -
// verified by putting garbage in pal[2] and confirming it's never sampled.
static void test_4x4_mode1_index2_is_average_not_palette_read()
{
	u16 pal[4] = {
		(u16)(0 | (0 << 5) | (0 << 10)),   // color0 = black
		(u16)(30 | (30 << 5) | (30 << 10)),// color1 = near-white
		0xDEAD, // should never be read in mode 1
		0x0000,
	};
	u16 palInfo = (1 << 14) | 0;
	u32 indices = 0xAAAAAAAA; // every texel index = 2 (0b10)
	u16 outTexels[16];
	gxDecode4x4Block(indices, palInfo, pal, outTexels);

	u16 expected = gxPackRGB5A3Opaque(GxRgb5{15, 15, 15});
	for (int i = 0; i < 16; ++i) CHECK(outTexels[i] == expected);
}

// Row/column addressing: mode 2 with distinct per-texel indices should
// place each color at the expected (x,y), not transposed or reversed.
static void test_4x4_addressing_matches_row_major_layout()
{
	u16 pal[4] = {0x0421, 0x0842, 0x0C63, 0x1084}; // 4 distinct colors
	u16 palInfo = (2 << 14) | 0;
	// row0: indices 0,1,2,3 ; rows 1-3: all 0
	u32 indices = 0x000000E4u; // 0b11100100 = idx3<<6|idx2<<4|idx1<<2|idx0, byte0=row0
	u16 outTexels[16];
	gxDecode4x4Block(indices, palInfo, pal, outTexels);

	CHECK(outTexels[0] == gxPackRGB5A3Opaque(gxExtractBGR555(pal[0])));
	CHECK(outTexels[1] == gxPackRGB5A3Opaque(gxExtractBGR555(pal[1])));
	CHECK(outTexels[2] == gxPackRGB5A3Opaque(gxExtractBGR555(pal[2])));
	CHECK(outTexels[3] == gxPackRGB5A3Opaque(gxExtractBGR555(pal[3])));
	// row 1 (indices 4-7) should all be color0 since byte1 of indices is 0
	for (int i = 4; i < 8; ++i) CHECK(outTexels[i] == gxPackRGB5A3Opaque(gxExtractBGR555(pal[0])));
}

int main()
{
	test_swizzle_roundtrip_8bpp(8, 4, 32, 16);
	test_swizzle_roundtrip_8bpp(8, 4, 8, 4);
	test_swizzle_roundtrip_4bpp(8, 8, 32, 16);
	test_swizzle_roundtrip_4bpp(8, 8, 8, 8);
	test_swizzle_roundtrip_16bpp(4, 4, 32, 16);
	test_swizzle_roundtrip_16bpp(4, 4, 4, 4);
	test_swizzle_single_block_identity();
	test_swizzle_two_blocks_layout();
	test_swizzle_4bpp_nibble_order();
	test_swizzle_rejects_misaligned_dims();

	test_color_rgb565_endpoints();
	test_color_rgb5a3_opaque_is_bitexact_passthrough();
	test_color_rgb5a3_alpha_zero_is_fully_transparent();

	test_a3i5_tlut_alpha_zero_is_transparent_index_zero_is_index();
	test_a5i3_alpha_truncated_to_3_bits();

	test_i2_unpack_preserves_index_values_lsb_first();
	test_i4_unpack_preserves_index_values_lsb_first();
	test_indexed_tlut_rgb5a3_zero_transparent();

	test_direct16bpp_opaque_bit_selects_subformat();

	test_4x4_mode2_all_index0_is_solid_color0();
	test_4x4_mode0_index3_is_transparent();
	test_4x4_mode1_index2_is_average_not_palette_read();
	test_4x4_addressing_matches_row_major_layout();

	if (g_failures == 0) {
		printf("All tests passed.\n");
		return 0;
	}
	printf("%d test(s) failed.\n", g_failures);
	return 1;
}
