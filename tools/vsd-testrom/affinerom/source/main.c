// SPDX-License-Identifier: CC0-1.0
//
// DS Engine B (sub screen) AFFINE fixture for the GX 2D compositor
// (gx-next-steps-log.md task 8; source/gx/gx_ds_engineb_render.cpp).
//
// Everything is static after setup, so a capture at any frame past boot is
// byte-stable and can be A/B-compared between the GX path and the forced-CPU
// reference (-DDSB_FORCE_CPU). One scenario per build, chosen by -DAT_CASE=N.
// The main engine just shows a flat colour. All content is procedurally
// generated noise-ish gradients (not pretty; the point is that every texel is
// distinguishable, so any sampling/addressing/flip/palette error shows up).
//
//   0  mode 1: BG3 = plain affine 256x256, wrap ON, rotation+scale matrix;
//              BG0 text 4bpp behind it
//   1  mode 2: BG2 = affine 128x128 wrap OFF zoomed out (outside-map = transparent),
//              BG3 = affine 512x512 wrap OFF, offset/rotated, behind BG2
//   2  mode 5: BG2 = ext 256x16 (16-bit map entries, H/V flip, palette bits,
//              regular palette), BG3 = ext DIRECT colour bitmap w/ alpha bit
//   3  mode 6: BG2 = large 8bpp 512x1024, wrap ON, rotated/scaled
//   4  mode 0: text BG + affine OBJ (single/double-size, 4bpp/8bpp, clipped,
//              overlapping) + one plain OBJ
//   5  mode 2: BG2 affine with a mid-frame (line 96) matrix + reference-point
//              rewrite from an HBlank IRQ -> two GX bands
//   6  mode 2: BG2 affine with a reference-point rewrite on EVERY line ->
//              exceeds the band budget, must bail to the CPU compositor
//   7  mode 3: BG3 = ext 256x16 + text BG0 8bpp, both WITH DISPCNT extended
//              BG palettes (VRAM H) -- the combination SM64DS's sub screen uses
//   8  mode 1: as 0 plus an alpha BLDCNT effect -> must bail
//   9  mode 5: BG2 = ext 256x1 (8bpp bitmap) 256x256 non-wrap, scaled, BG3 =
//              ext 256x16 with ext palette OFF
//  10  mode 0: text BG + affine OBJ with an OBJ-window/semitransparent mix ->
//              must bail (Mode != 0 affine sprite)

#include <nds.h>
#include <stdio.h>

#ifndef AT_CASE
#define AT_CASE 0
#endif

#define SUBVRAM ((u8 *)0x06200000)          // sub BG VRAM (bank C)
#define SUBOBJ  ((u8 *)0x06600000)          // sub OBJ VRAM (bank D)

static u8 hashb(int a, int b) { u32 h = (u32)a * 2654435761u ^ (u32)b * 40503u; h ^= h >> 13; h *= 0x5bd1e995; return (u8)(h >> 11); }

static void fillPalettes(void)
{
	for (int i = 0; i < 256; i++) {
		BG_PALETTE_SUB[i]     = RGB15(i & 31, (i * 3) & 31, (i * 7 + 5) & 31);
		SPRITE_PALETTE_SUB[i] = RGB15((i * 5 + 3) & 31, (i * 9) & 31, i & 31);
	}
	BG_PALETTE_SUB[0] = RGB15(3, 5, 9); // backdrop
}

// 8bpp tile set: 256 tiles at charbase (each 64 bytes). Tile t = hashed noise
// with a bright 1px border on the top/left so orientation is unambiguous.
static void fillTiles8(u8 *dst, int nTiles)
{
	for (int t = 0; t < nTiles; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				u8 v = (u8)(1 + (hashb(t, y * 8 + x) % 250));
				if ((t & 3) && (x == 0 || y == 0)) v = (u8)(200 + (t & 31));
				if (!(t & 15)) v = 0; // some fully transparent tiles
				dst[t * 64 + y * 8 + x] = v;
			}
}

static void fillTiles4(u8 *dst, int nTiles)
{
	for (int t = 0; t < nTiles; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 4; x++) {
				u8 lo = (u8)(hashb(t, y * 8 + x * 2) % 16), hi = (u8)(hashb(t, y * 8 + x * 2 + 1) % 16);
				if (x == 0 && y == 0) lo = 15;
				dst[t * 32 + y * 4 + x] = (u8)(lo | (hi << 4));
			}
}

static void setMatrix2(s16 pa, s16 pb, s16 pc, s16 pd, s32 x, s32 y)
{
	REG_BG2PA_SUB = pa; REG_BG2PB_SUB = pb; REG_BG2PC_SUB = pc; REG_BG2PD_SUB = pd;
	REG_BG2X_SUB = x; REG_BG2Y_SUB = y;
}
static void setMatrix3(s16 pa, s16 pb, s16 pc, s16 pd, s32 x, s32 y)
{
	REG_BG3PA_SUB = pa; REG_BG3PB_SUB = pb; REG_BG3PC_SUB = pc; REG_BG3PD_SUB = pd;
	REG_BG3X_SUB = x; REG_BG3Y_SUB = y;
}

#if AT_CASE == 5
static void hblank5(void)
{
	// Rewrite at the end of line 95 so lines 96.. use the new matrix/origin.
	if (REG_VCOUNT == 95) setMatrix2(0x0120, 0x0040, -0x0040, 0x0120, 0x1000, 0x3000);
	else if (REG_VCOUNT == 262 || REG_VCOUNT == 261) setMatrix2(0x0100, 0x0000, 0x0000, 0x0100, 0, 0);
}
#endif
#if AT_CASE == 6
static void hblank6(void)
{
	int v = REG_VCOUNT;
	if (v < 191) REG_BG2X_SUB = (s32)(v * 3) << 8;  // every line: new reference point
}
#endif

static void setupObjs(void)
{
	oamInit(&oamSub, SpriteMapping_1D_32, false);
	u16 *g4a = oamAllocateGfx(&oamSub, SpriteSize_32x32, SpriteColorFormat_16Color);
	u16 *g4b = oamAllocateGfx(&oamSub, SpriteSize_16x16, SpriteColorFormat_16Color);
	u16 *g8a = oamAllocateGfx(&oamSub, SpriteSize_32x32, SpriteColorFormat_256Color);
	u16 *g8b = oamAllocateGfx(&oamSub, SpriteSize_64x32, SpriteColorFormat_256Color);
	u16 *g4c = oamAllocateGfx(&oamSub, SpriteSize_16x16, SpriteColorFormat_16Color);
	fillTiles4((u8 *)g4a, 16); fillTiles4((u8 *)g4b, 4); fillTiles8((u8 *)g8a, 16);
	fillTiles8((u8 *)g8b, 32); fillTiles4((u8 *)g4c, 4);

	SpriteRotation *rot = (SpriteRotation *)oamSub.oamMemory;
	// id 0: single-size 32x32 4bpp, ~30 degree rotation, scale ~0.9
	rot[0].hdx = 0x00CD; rot[0].vdx = 0x0073;  // pa, pc ... (libnds naming; both rows filled below)
	rot[0].hdy = -0x0073; rot[0].vdy = 0x00CD;
	// id 1: double-size 16x16 4bpp, 2x magnify
	rot[1].hdx = 0x0080; rot[1].vdx = 0x0000; rot[1].hdy = 0x0000; rot[1].vdy = 0x0080;
	// id 2: 32x32 8bpp, non-uniform (x 0.6, y 1.4), sheared
	rot[2].hdx = 0x01AA; rot[2].vdx = 0x0030; rot[2].hdy = 0x0000; rot[2].vdy = 0x00B7;
	// id 3: 64x32 8bpp double size, rotated ~ -50 deg
	rot[3].hdx = 0x00A4; rot[3].vdx = -0x00C4; rot[3].hdy = 0x00C4; rot[3].vdy = 0x00A4;
	// id 4: 16x16 4bpp identity (exercises the exact-1:1 path)
	rot[4].hdx = 0x0100; rot[4].vdx = 0x0000; rot[4].hdy = 0x0000; rot[4].vdy = 0x0100;

	int prioB = 1;
#if AT_CASE == 10
	// affine sprite in semi-transparent mode -> must bail
	oamSet(&oamSub, 0, 20, 20, 0, 0, SpriteSize_32x32, SpriteColorFormat_16Color, g4a, 0, false, false, false, false, false);
	oamSub.oamMemory[0].blendMode = OBJMODE_BLENDED;
#else
	oamSet(&oamSub, 0, 20, 20, 0, 0, SpriteSize_32x32, SpriteColorFormat_16Color, g4a, 0, false, false, false, false, false);
#endif
	oamSet(&oamSub, 1, 90, 30, 0, 1, SpriteSize_16x16, SpriteColorFormat_16Color, g4b, 1, true, false, false, false, false);
	oamSet(&oamSub, 2, 150, 60, 1, 2, SpriteSize_32x32, SpriteColorFormat_256Color, g8a, 2, false, false, false, false, false);
	oamSet(&oamSub, 3, -20, 100, 0, 0, SpriteSize_64x32, SpriteColorFormat_256Color, g8b, 3, true, false, false, false, false);
	oamSet(&oamSub, 4, 200, 150, prioB, 3, SpriteSize_16x16, SpriteColorFormat_16Color, g4c, 4, false, false, false, false, false);
	// overlapping pair, lower index must win at equal priority
	oamSet(&oamSub, 5, 100, 100, 0, 2, SpriteSize_32x32, SpriteColorFormat_16Color, g4a, 0, false, false, false, false, false);
	oamSet(&oamSub, 6, 110, 110, 0, 4, SpriteSize_32x32, SpriteColorFormat_256Color, g8a, 2, false, false, false, false, false);
	// a plain (non-affine) sprite, hflipped
	oamSet(&oamSub, 7, 30, 150, 0, 5, SpriteSize_16x16, SpriteColorFormat_16Color, g4b, -1, false, false, true, false, false);
	// clipped at the bottom edge
	oamSet(&oamSub, 8, 220, 175, 0, 6, SpriteSize_32x32, SpriteColorFormat_256Color, g8a, 0, true, false, false, false, false);
}

int main(void)
{
	irqEnable(IRQ_VBLANK);
	videoSetMode(MODE_0_2D);                    // main: nothing enabled, flat backdrop
	BG_PALETTE[0] = RGB15(0, 0, 0);

	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_SUB_SPRITE);
	fillPalettes();

	for (int i = 0; i < 0x20000; i++) SUBVRAM[i] = 0;

#if AT_CASE == 0 || AT_CASE == 8
	fillTiles8(SUBVRAM + 0x0000, 256);                     // tiles
	{ u8 *map = SUBVRAM + 0x4000;                           // 32x32 tiles = 1KB entries; ScreenBase 8
	  for (int i = 0; i < 32 * 32; i++) map[i] = (u8)(hashb(i, 7)); }
	fillTiles4(SUBVRAM + 0x10000, 64);                     // text BG0 tiles @ 64KB
	{ u16 *m0 = (u16 *)(SUBVRAM + 0x8000);                 // text map, ScreenBase 16
	  for (int i = 0; i < 32 * 32; i++) m0[i] = (u16)((i % 60) | ((i & 1) ? 0x400 : 0) | (((i >> 3) & 15) << 12)); }
	videoSetModeSub(MODE_1_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(16) | BG_TILE_BASE(4) | BG_PRIORITY(3);
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(0);
	setMatrix3(0x00E0, 0x0060, -0x0060, 0x00E0, 0x1234, -0x2100);
#if AT_CASE == 8
	REG_BLDCNT_SUB = BLEND_ALPHA | BLEND_SRC_BG3 | BLEND_DST_BG0 | BLEND_DST_BACKDROP;
	REG_BLDALPHA_SUB = (8 << 0) | (8 << 8);
#endif

#elif AT_CASE == 1
	fillTiles8(SUBVRAM + 0x0000, 256);
	{ u8 *m2 = SUBVRAM + 0x4000;   for (int i = 0; i < 16 * 16; i++) m2[i] = (u8)hashb(i, 3); }      // 128x128 -> 16x16 tiles
	{ u8 *m3 = SUBVRAM + 0x8000;   for (int i = 0; i < 64 * 64; i++) m3[i] = (u8)hashb(i, 11); }     // 512x512 -> 64x64 tiles
	videoSetModeSub(MODE_2_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG2CNT_SUB = BG_RS_16x16 | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(0);                // 128x128, no wrap
	REG_BG3CNT_SUB = BG_RS_64x64 | BG_MAP_BASE(16) | BG_TILE_BASE(0) | BG_PRIORITY(1);               // 512x512, no wrap
	setMatrix2(0x0200, 0x0000, 0x0000, 0x0200, -0x1000, -0x0800);                                   // zoomed out 2x, origin shifted
	setMatrix3(0x0100, 0x0020, -0x0020, 0x0100, -100 * 256, 30 * 256);

#elif AT_CASE == 2 || AT_CASE == 9
	// ext-affine 256x16: 16-bit map entries at ScreenBase*2KB; tiles 8bpp at charbase
	fillTiles8(SUBVRAM + 0x0000, 1024 > 256 ? 256 : 256);
	{ u16 *m2 = (u16 *)(SUBVRAM + 0x4000);
	  for (int i = 0; i < 32 * 32; i++) m2[i] = (u16)((hashb(i, 5) % 200) | (hashb(i, 9) & 3) << 10 | ((hashb(i, 13) & 15) << 12)); }
#if AT_CASE == 2
	// BG3 direct colour bitmap 256x256 @ ScreenBase 4 (64KB): bit15 = opaque
	{ u16 *bm = (u16 *)(SUBVRAM + 0x10000);
	  for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++)
	      bm[y * 256 + x] = (u16)(((x ^ y) & 32) ? (0x8000 | RGB15(x & 31, y & 31, (x + y) & 31)) : 0); }
	videoSetModeSub(MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	// BG2: Palette_256 clear + charbase even => 256x16.   BG3: 256 colour + charbase odd => direct
	REG_BG2CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(4) | BG_TILE_BASE(1) | BG_COLOR_256 | BG_PRIORITY(0);
	setMatrix2(0x0120, 0x0040, -0x0040, 0x0120, 0x0800, 0x0400);
	setMatrix3(0x0100, 0x0000, 0x0000, 0x0100, 0x4000, 0x2000);
#else
	// case 9: BG2 = ext 256x1 (8bpp bitmap) 256x256, non-wrap, zoomed; BG3 = ext 256x16, no ext pal
	{ u8 *bm = SUBVRAM + 0x10000;     // ScreenBase 4 (64KB)
	  for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) bm[y * 256 + x] = (u8)(((x / 8 + y / 8) & 3) ? hashb(x / 4, y / 4) : 0); }
	videoSetModeSub(MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG2CNT_SUB = BG_RS_32x32 | BG_MAP_BASE(4) | BG_TILE_BASE(0) | BG_COLOR_256 | BG_PRIORITY(0);   // 256 colour + even charbase => 256x1
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(1);                    // 256x16, wrap off
	setMatrix2(0x00A0, 0x0000, 0x0000, 0x00A0, -0x1800, 0x0400);
	setMatrix3(0x00D0, -0x0040, 0x0040, 0x00D0, 0, 0);
#endif

#elif AT_CASE == 3
	// large 8bpp 512x1024 bitmap over the whole 128KB bank (mirrors beyond)
	for (int y = 0; y < 256; y++) for (int x = 0; x < 512; x++)
		SUBVRAM[y * 512 + x] = (u8)(((x / 16 + y / 16) & 1) ? hashb(x / 2, y / 2) : (x < 8 || y < 8 ? 250 : 0));
	videoSetModeSub(MODE_6_2D | DISPLAY_BG2_ACTIVE);
	REG_BG2CNT_SUB = BG_RS_16x16 | BG_WRAP_ON | BG_PRIORITY(0);   // size sel 0 => 512x1024
	setMatrix2(0x00B0, 0x0050, -0x0050, 0x00B0, 0x1000, 0x2000);

#elif AT_CASE == 4 || AT_CASE == 10
	fillTiles4(SUBVRAM + 0x0000, 64);
	{ u16 *m0 = (u16 *)(SUBVRAM + 0x4000);
	  for (int i = 0; i < 32 * 32; i++) m0[i] = (u16)((i % 60) | (((i >> 2) & 15) << 12)); }
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(2);
	setupObjs();

#elif AT_CASE == 5 || AT_CASE == 6
	fillTiles8(SUBVRAM + 0x0000, 256);
	{ u8 *m2 = SUBVRAM + 0x4000; for (int i = 0; i < 32 * 32; i++) m2[i] = (u8)hashb(i, 3); }
	videoSetModeSub(MODE_2_2D | DISPLAY_BG2_ACTIVE);
	REG_BG2CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(0);
	setMatrix2(0x0100, 0x0000, 0x0000, 0x0100, 0, 0);
#if AT_CASE == 5
	irqSet(IRQ_HBLANK, hblank5); irqEnable(IRQ_HBLANK);
#else
	irqSet(IRQ_HBLANK, hblank6); irqEnable(IRQ_HBLANK);
#endif

#elif AT_CASE == 7
	// bank H -> LCDC, write the ext palette (16 sets x 256), then map as sub BG ext palette
	vramSetBankH(VRAM_H_LCD);
	{ u16 *ep = (u16 *)0x06898000;
	  for (int i = 0; i < 4096; i++) ep[i] = RGB15((i * 3 + (i >> 8) * 7) & 31, (i * 5) & 31, ((i >> 4) * 11 + 2) & 31); }
	vramSetBankH(VRAM_H_SUB_BG_EXT_PALETTE);
	fillTiles8(SUBVRAM + 0x0000, 256);
	{ u16 *m3 = (u16 *)(SUBVRAM + 0x4000);
	  for (int i = 0; i < 32 * 32; i++) m3[i] = (u16)((hashb(i, 5) % 200) | ((hashb(i, 9) & 3) << 10) | ((hashb(i, 13) & 15) << 12)); }
	{ u16 *m0 = (u16 *)(SUBVRAM + 0x8000);
	  for (int i = 0; i < 32 * 32; i++) m0[i] = (u16)((hashb(i, 17) % 120) | ((hashb(i, 19) & 3) << 10) | ((hashb(i, 23) & 15) << 12)); }
	videoSetModeSub(MODE_3_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_DISPCNT_SUB |= (1u << 30);          // extended BG palettes
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(16) | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(2);
	setMatrix3(0x0100, 0x0000, 0x0000, 0x0100, 0x0800, 0x0400);
#endif

	printf("");   // keep stdio linked; no console on the sub screen (all BG VRAM is test data)

	for (;;) {
		swiWaitForVBlank();
#if AT_CASE == 4 || AT_CASE == 10
		oamUpdate(&oamSub);
#endif
	}
	return 0;
}
