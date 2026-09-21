// SPDX-License-Identifier: CC0-1.0
//
// DS Engine B (sub screen) NATIVE CI4/CI8 + TLUT fixture for the GX 2D compositor
// (gx-next-steps-log.md task 11; source/gx/gx_ds_engineb_render.cpp).
//
// Static scenes (byte-stable after boot; case 7 has a schedule that ends at f=150), one
// per build (-DCI_CASE=N), A/B-compared GX vs forced-CPU (-DDSB_FORCE_CPU) and, for the
// TLUT-clobber cases, against the -DDSB_MUTATE_TLUTSHARE / _TLUTNOBIND mutants, which
// must FAIL. Every palette entry / texel is pseudo-random so a wrong TLUT slot, a wrong
// sub-bank or a wrong extended-palette slice changes visible pixels. Sub engine only;
// the main engine shows a flat backdrop.
//
//   0  mode 0, four text BGs: BG0 64x64 4bpp with ALL 16 palNum sub-banks in one plane;
//      BG1 8bpp regular palette; BG2 4bpp using only sub-banks {3,7,12}; BG3 4bpp, all 16.
//   1  mode 0 + 48 sprites, 4bpp (16 different palIndex, so 16 different 16-entry TLUTs)
//      and 8bpp, all shapes/sizes, H/V flips, priorities 0-3, clipped at every edge,
//      4 affine sprites (4bpp/8bpp, single/double size). BG2 text 4bpp behind.
//   2  as 1 but DISPCNT.ExOBJPalette_Enable with bank I as the sub extended OBJ palette:
//      8bpp sprites take 16 different 256-entry slices (palIndex), 4bpp ignore it.
//   3  mode 0, DISPCNT.ExBGxPalette_Enable, bank H: BG0 8bpp single slice (palNum 3, plus
//      an all-transparent tile with a different palNum), BG1 8bpp single slice (palNum 6),
//      BG2 8bpp with all 16 slices (multi-slice: RGB5A3 fallback), BG3 4bpp (ignores ext).
//   4  mode 5, ext BG palettes: BG0 8bpp text single slice, BG2 ext-affine 256x16 single
//      slice (palNum 5), BG3 ext-affine 256x16 multi-slice (16 slices).
//   5  TLUT clobber, everything at once: mode 1 = BG0/BG1/BG2 4bpp text (private sub-banks),
//      BG3 affine 8bpp (rot+zoom, wrap) + 44 sprites (4bpp all 16 palettes, 8bpp, affine),
//      priorities interleaved so BG0-3 and OBJ draws alternate TLUT owners constantly.
//   6  case 5 + brighten (BLDCNT mode 2) that switches layer sets at line 96: band 0 fades
//      BG0|OBJ, band 1 fades BG1|BG3, so BG0/BG1/BG3/OBJ each use their plain AND fade
//      texture (same TLUT name, different TLUT contents) within one frame.
//   7  dirty: ext OBJ palette + ext BG palette + sub-palettes rewritten at f=60/90/120/150.
//   8  mode 0: BG0 64x64 4bpp (all 16 palNum) + BG1 8bpp + 8bpp ext-palette BG2 (single
//      slice) + sprites 4bpp/8bpp, with WIN0 (layers differ inside/outside) and BLDCNT alpha
//      -- the CI path under windows and constant-alpha draws.

#include <nds.h>
#include <stdio.h>

#ifndef CI_CASE
#define CI_CASE 0
#endif

#define SUBVRAM ((u8 *)0x06200000)
#define SUBOBJ  ((u8 *)0x06600000)
#define R16(a) (*(volatile u16 *)(a))
#define SUB_DISPCNT R16(0x04001000)
#define SUB_WIN0H   R16(0x04001040)
#define SUB_WIN0V   R16(0x04001044)
#define SUB_WININ   R16(0x04001048)
#define SUB_WINOUT  R16(0x0400104A)
#define SUB_BLDCNT  R16(0x04001050)
#define SUB_BLDALPHA R16(0x04001052)
#define SUB_BLDY    R16(0x04001054)
#define SUBOAM      ((volatile u16 *)0x07000400)

static u8 hashb(int a, int b) { u32 h = (u32)a * 2654435761u ^ (u32)b * 40503u; h ^= h >> 13; h *= 0x5bd1e995; return (u8)(h >> 11); }
static u16 col(int a, int b) { return RGB15(hashb(a, b) & 31, hashb(a + 17, b) & 31, hashb(a, b + 91) & 31); }

static void fillPalettes(void)
{
	for (int i = 0; i < 256; i++) {
		BG_PALETTE_SUB[i]     = col(i, 1);
		SPRITE_PALETTE_SUB[i] = col(i, 2);
	}
	BG_PALETTE_SUB[0] = RGB15(6, 21, 13); // backdrop
}

// 4bpp / 8bpp tiles: ~1/3 of the texels are index 0; local tile 0 is fully transparent.
static void fillTiles4(u8 *dst, int nTiles, int seed)
{
	for (int t = 0; t < nTiles; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 4; x++) {
				u8 lo = hashb(t + seed * 977, y * 8 + x * 2) % 16, hi = hashb(t + seed * 977, y * 8 + x * 2 + 1) % 16;
				if (lo < 5) lo = 0;
				if (hi < 5) hi = 0;
				if (t == 0) lo = hi = 0;
				dst[t * 32 + y * 4 + x] = (u8)(lo | (hi << 4));
			}
}
static void fillTiles8(u8 *dst, int nTiles, int seed)
{
	for (int t = 0; t < nTiles; t++)
		for (int i = 0; i < 64; i++) {
			u8 v = hashb(t + seed * 977, i);
			if (v < 85 || t == 0) v = 0;
			dst[t * 64 + i] = v;
		}
}

// text map: nEnt entries; tiles 1..nTiles-1 (tile 0 is the transparent one, used for 1 in 9 cells);
// palMode 0: palNum = hash(0..15); 1: hash over {3,7,12}; 2: fixed `fix`, the transparent tile gets `fix2`.
static void fillMap(u16 *m, int nEnt, int nTiles, int salt, int palMode, int fix, int fix2)
{
	static const u8 sub[3] = { 3, 7, 12 };
	for (int i = 0; i < nEnt; i++) {
		int t = (hashb(i, 31 + salt) % (nTiles - 1)) + 1;
		if (hashb(i, 3 + salt) % 9 == 0) t = 0;
		int pn = (palMode == 0) ? (hashb(i, 5 + salt) & 15) : (palMode == 1) ? sub[hashb(i, 5 + salt) % 3] : (t == 0 ? fix2 : fix);
		m[i] = (u16)(t | (((hashb(i, 77 + salt)) & 3) << 10) | (pn << 12));
	}
}

// --- sprites: raw OAM ---
static const u8 kW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const u8 kH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };
static void oamPlain(int i, int x, int y, int shape, int size, int depth, int tile, int pal, int prio, int hf, int vf)
{
	SUBOAM[i * 4 + 0] = (u16)((y & 0xFF) | (depth << 13) | (shape << 14));
	SUBOAM[i * 4 + 1] = (u16)((x & 0x1FF) | (hf << 12) | (vf << 13) | (size << 14));
	SUBOAM[i * 4 + 2] = (u16)((tile & 0x3FF) | (prio << 10) | (pal << 12));
}
static void oamAff(int i, int x, int y, int shape, int size, int depth, int tile, int pal, int prio, int group, int dbl)
{
	SUBOAM[i * 4 + 0] = (u16)((y & 0xFF) | ((dbl ? 3 : 1) << 8) | (depth << 13) | (shape << 14));
	SUBOAM[i * 4 + 1] = (u16)((x & 0x1FF) | (group << 9) | (size << 14));
	SUBOAM[i * 4 + 2] = (u16)((tile & 0x3FF) | (prio << 10) | (pal << 12));
}
static void oamMat(int g, int pa, int pb, int pc, int pd)
{
	SUBOAM[(4 * g + 0) * 4 + 3] = (u16)pa; SUBOAM[(4 * g + 1) * 4 + 3] = (u16)pb;
	SUBOAM[(4 * g + 2) * 4 + 3] = (u16)pc; SUBOAM[(4 * g + 3) * 4 + 3] = (u16)pd;
}

// nPlain plain sprites (OAM 0..nPlain-1) then nAff affine ones; gfx slot k at SUBOBJ + k*2KB, tile index k*64.
// palMode: 0 = palIndex i%16; the depth pattern is `dmask` (bit k of dmask for sprite k%8 => 8bpp).
static void setupObjs(int nPlain, int nAff, int dmask, int prioMod)
{
	for (int i = 0; i < 128; i++) SUBOAM[i * 4 + 0] = (2 << 8);   // OBJ-disabled
	for (int i = 0; i < nPlain; i++) {
		int shape = i % 3, size = (i / 3) & 3;
		int depth = (dmask >> (i & 7)) & 1;
		if (depth && kW[shape][size] * kH[shape][size] > 2048) size--;     // 8bpp: at most 2KB of gfx
		int w = kW[shape][size], h = kH[shape][size];
		if (depth) fillTiles8(SUBOBJ + i * 2048, w * h / 64, 20 + i); else fillTiles4(SUBOBJ + i * 2048, w * h / 64, 20 + i);
		int x = (hashb(i, 1) * 300 >> 8) - 40, y = (hashb(i, 2) * 214 >> 8) - 22;
		oamPlain(i, x, y, shape, size, depth, i * 64, i & 15, hashb(i, 4) % prioMod, (hashb(i, 5) & 3) == 0, (hashb(i, 6) & 3) == 1);
	}
	for (int k = 0; k < nAff; k++) {
		int i = nPlain + k, dbl = k & 1, depth = (k >> 1) & 1;
		int shape = 0, size = 1 + (k & 1);                     // 16x16 or 32x32
		int w = kW[shape][size], h = kH[shape][size];
		if (depth) fillTiles8(SUBOBJ + i * 2048, w * h / 64, 60 + i); else fillTiles4(SUBOBJ + i * 2048, w * h / 64, 60 + i);
		oamAff(i, 20 + k * 55, 30 + k * 35, shape, size, depth, i * 64, (3 * k + 5) & 15, k & 1, k, dbl);
	}
	// four distinct matrices (rotation / zoom / shear)
	oamMat(0, 0x00CD, 0x0073, -0x0073, 0x00CD);
	oamMat(1, 0x0080, 0x0000, 0x0000, 0x0080);
	oamMat(2, 0x01AA, 0x0030, 0x0000, 0x00B7);
	oamMat(3, 0x00A4, -0x00C4, 0x00C4, 0x00A4);
}

static void setMatrix3(s16 pa, s16 pb, s16 pc, s16 pd, s32 x, s32 y)
{
	REG_BG3PA_SUB = pa; REG_BG3PB_SUB = pb; REG_BG3PC_SUB = pc; REG_BG3PD_SUB = pd; REG_BG3X_SUB = x; REG_BG3Y_SUB = y;
}
static void setMatrix2(s16 pa, s16 pb, s16 pc, s16 pd, s32 x, s32 y)
{
	REG_BG2PA_SUB = pa; REG_BG2PB_SUB = pb; REG_BG2PC_SUB = pc; REG_BG2PD_SUB = pd; REG_BG2X_SUB = x; REG_BG2Y_SUB = y;
}

// Bank H = sub BG ext palette (4 slots x 16 palettes x 256), bank I = sub OBJ ext palette (16 x 256).
static void extBgFill(int seed)
{
	vramSetBankH(VRAM_H_LCD);
	u16 *e = VRAM_H;
	for (int i = 0; i < 4 * 16 * 256; i++) e[i] = col(i + seed * 31, 3);
	vramSetBankH(VRAM_H_SUB_BG_EXT_PALETTE);
}
static void extObjFill(int seed)
{
	vramSetBankI(VRAM_I_LCD);
	u16 *e = VRAM_I;
	for (int i = 0; i < 16 * 256; i++) e[i] = col(i + seed * 37, 4);
	vramSetBankI(VRAM_I_SUB_SPRITE_EXT_PALETTE);
}

// Layout (bank C, 128KB): maps 0x0000.. (BG0 64x64 uses 0x0000-0x1FFF), tiles at TILE_BASE(2..7) = 0x8000.. (16KB each).
#define T0 2
#define T1 3
#define T2 4
#define T3 5

#if CI_CASE == 6
static void hb(void)
{
	int v = REG_VCOUNT;
	if (v == 95) SUB_BLDCNT = (2 << 6) | 2 | 8;                              // band 1: BG1 | BG3 fade
	else if (v == 261 || v == 262) SUB_BLDCNT = (2 << 6) | 1 | 16;          // back to band 0: BG0 | OBJ fade
}
#endif

#if CI_CASE == 7
static int f = 0;
static void step(void)
{
	if (f == 60) extObjFill(9);
	if (f == 90) extBgFill(5);
	if (f == 120) for (int j = 0; j < 16; j++) SPRITE_PALETTE_SUB[5 * 16 + j] = col(j + 200, 9);
	if (f == 150) for (int j = 0; j < 16; j++) BG_PALETTE_SUB[9 * 16 + j] = col(j + 300, 8);
}
#endif

int main(void)
{
	irqEnable(IRQ_VBLANK);
	videoSetMode(MODE_0_2D);
	BG_PALETTE[0] = RGB15(0, 0, 0);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_SUB_SPRITE);
	fillPalettes();
	for (int i = 0; i < 0x20000; i++) SUBVRAM[i] = 0;

	u32 dcnt = 0;   // extra sub DISPCNT bits
#if CI_CASE == 0
	fillTiles4(SUBVRAM + 0x8000, 256, 1); fillTiles8(SUBVRAM + 0xC000, 192, 2);
	fillTiles4(SUBVRAM + 0x10000, 256, 3); fillTiles4(SUBVRAM + 0x14000, 256, 4);
	fillMap((u16 *)(SUBVRAM + 0x0000), 64 * 64, 256, 0, 0, 0, 0);
	fillMap((u16 *)(SUBVRAM + 0x2000), 32 * 32, 192, 1, 2, 0, 0);       // 8bpp regular: palNum ignored
	fillMap((u16 *)(SUBVRAM + 0x3000), 32 * 32, 256, 2, 1, 0, 0);
	fillMap((u16 *)(SUBVRAM + 0x4000), 32 * 32, 256, 3, 0, 0, 0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG0CNT_SUB = BG_64x64 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(0);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(4) | BG_TILE_BASE(T1) | BG_PRIORITY(1);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(6) | BG_TILE_BASE(T2) | BG_PRIORITY(2);
	REG_BG3CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(8) | BG_TILE_BASE(T3) | BG_PRIORITY(3);
	REG_BG1HOFS_SUB = 13; REG_BG1VOFS_SUB = 27; REG_BG2HOFS_SUB = 5; REG_BG3VOFS_SUB = 9;

#elif CI_CASE == 1 || CI_CASE == 2
	fillTiles4(SUBVRAM + 0x8000, 256, 1);
	fillMap((u16 *)(SUBVRAM + 0x0000), 32 * 32, 256, 0, 0, 0, 0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(2);
	setupObjs(44, 4, 0xB6, 4);
#if CI_CASE == 2
	extObjFill(1);
	dcnt = 1u << 31;
#endif

#elif CI_CASE == 3
	extBgFill(2);
	fillTiles8(SUBVRAM + 0x8000, 192, 1); fillTiles8(SUBVRAM + 0xC000, 192, 2);
	fillTiles8(SUBVRAM + 0x10000, 192, 3); fillTiles4(SUBVRAM + 0x14000, 256, 4);
	fillMap((u16 *)(SUBVRAM + 0x0000), 32 * 32, 192, 0, 2, 3, 9);       // single slice 3; the blank tile says slice 9
	fillMap((u16 *)(SUBVRAM + 0x2000), 32 * 32, 192, 1, 2, 6, 6);       // single slice 6
	fillMap((u16 *)(SUBVRAM + 0x3000), 32 * 32, 192, 2, 0, 0, 0);       // all 16 slices
	fillMap((u16 *)(SUBVRAM + 0x4000), 32 * 32, 256, 3, 0, 0, 0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(0);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(4) | BG_TILE_BASE(T1) | BG_PRIORITY(1);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(6) | BG_TILE_BASE(T2) | BG_PRIORITY(2);
	REG_BG3CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(8) | BG_TILE_BASE(T3) | BG_PRIORITY(3);
	REG_BG1HOFS_SUB = 11; REG_BG2VOFS_SUB = 19;
	dcnt = 1u << 30;

#elif CI_CASE == 4
	extBgFill(3);
	fillTiles8(SUBVRAM + 0x8000, 192, 1);            // BG0 tiles
	fillMap((u16 *)(SUBVRAM + 0x0000), 32 * 32, 192, 0, 2, 4, 4);       // BG0: single slice 4
	fillTiles8(SUBVRAM + 0x10000, 192, 2);           // BG2 tiles (charbase T2=4: even => 256x16)
	fillMap((u16 *)(SUBVRAM + 0x2000), 32 * 32, 192, 1, 2, 5, 11);      // BG2 ext-affine map: single slice 5
	fillTiles8(SUBVRAM + 0x18000, 192, 3);           // BG3 tiles (charbase 6)
	fillMap((u16 *)(SUBVRAM + 0x3000), 32 * 32, 192, 2, 0, 0, 0);       // BG3: 16 slices
	videoSetModeSub(MODE_5_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(3);
	REG_BG2CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(4) | BG_TILE_BASE(4) | BG_PRIORITY(1);
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(6) | BG_TILE_BASE(6) | BG_PRIORITY(0);
	setMatrix2(0x0120, 0x0040, -0x0040, 0x0120, 0x0800, 0x0400);
	setMatrix3(0x00E0, -0x0030, 0x0030, 0x00E0, 0x1200, 0x0000);
	dcnt = 1u << 30;

#elif CI_CASE == 5 || CI_CASE == 6
	fillTiles4(SUBVRAM + 0x8000, 256, 1); fillTiles4(SUBVRAM + 0xC000, 256, 2); fillTiles4(SUBVRAM + 0x10000, 256, 3);
	fillTiles8(SUBVRAM + 0x18000, 128, 4);
	// private sub-banks: BG0 {0-4}, BG1 {5-9}, BG2 {10-15}
	{ static const int lo[3] = { 0, 5, 10 }, n[3] = { 5, 5, 6 };
	  for (int b = 0; b < 3; b++) {
	      u16 *m = (u16 *)(SUBVRAM + b * 0x800);
	      fillMap(m, 32 * 32, 256, b, 0, 0, 0);
	      for (int i = 0; i < 32 * 32; i++) m[i] = (u16)((m[i] & 0x0FFF) | ((lo[b] + hashb(i, 40 + b) % n[b]) << 12));
	  } }
	{ u8 *am = SUBVRAM + 0x1800; for (int i = 0; i < 32 * 32; i++) am[i] = (u8)(1 + hashb(i, 7) % 127); }   // BG3 affine map (1 byte) 32x32 tiles
	videoSetModeSub(MODE_1_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(1);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(1) | BG_TILE_BASE(T1) | BG_PRIORITY(2);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(2) | BG_TILE_BASE(T2) | BG_PRIORITY(0);
	REG_BG3CNT_SUB = BG_RS_32x32 | BG_WRAP_ON | BG_MAP_BASE(3) | BG_TILE_BASE(6) | BG_PRIORITY(3);
	setMatrix3(0x00D0, 0x0050, -0x0050, 0x00D0, 0x1000, 0x0800);
	setupObjs(44, 4, 0xB6, 4);
#if CI_CASE == 6
	SUB_BLDCNT = (2 << 6) | 1 | 16; SUB_BLDY = 9;
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#endif

#elif CI_CASE == 7
	extBgFill(2); extObjFill(1);
	fillTiles8(SUBVRAM + 0x8000, 192, 1);
	fillMap((u16 *)(SUBVRAM + 0x0000), 32 * 32, 192, 0, 2, 3, 9);
	fillTiles4(SUBVRAM + 0xC000, 256, 2);
	fillMap((u16 *)(SUBVRAM + 0x2000), 32 * 32, 256, 1, 0, 0, 0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(1);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(4) | BG_TILE_BASE(T1) | BG_PRIORITY(2);
	setupObjs(24, 2, 0xB6, 3);
	dcnt = (1u << 30) | (1u << 31);

#elif CI_CASE == 8
	extBgFill(4);
	fillTiles4(SUBVRAM + 0x8000, 256, 1); fillTiles8(SUBVRAM + 0xC000, 192, 2); fillTiles8(SUBVRAM + 0x10000, 192, 3);
	fillMap((u16 *)(SUBVRAM + 0x0000), 64 * 64, 256, 0, 0, 0, 0);
	fillMap((u16 *)(SUBVRAM + 0x2000), 32 * 32, 192, 1, 2, 0, 0);
	fillMap((u16 *)(SUBVRAM + 0x3000), 32 * 32, 192, 2, 2, 7, 7);       // BG2: ext single slice 7
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	REG_BG0CNT_SUB = BG_64x64 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(T0) | BG_PRIORITY(0);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(4) | BG_TILE_BASE(T1) | BG_PRIORITY(1);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(6) | BG_TILE_BASE(T2) | BG_PRIORITY(2);
	setupObjs(20, 0, 0xB6, 1);                         // all sprites prio 0: drawn above BG0, so never beneath the blended layer
	dcnt = 1u << 30;
	SUB_DISPCNT |= (1u << 13);                      // WIN0
	SUB_WIN0H = (40 << 8) | 210; SUB_WIN0V = (30 << 8) | 150;
	SUB_WININ = (1 | 4 | 16 | 32); SUB_WINOUT = (1 | 2 | 16 | 32);          // inside: BG0|BG2|OBJ  outside: BG0|BG1|OBJ
	SUB_BLDCNT = (1 << 6) | 1 | ((2 | 4 | 32) << 8); SUB_BLDALPHA = 9 | (7 << 8);   // alpha: T1 = BG0, T2 = BG1|BG2|BD
#endif
	REG_DISPCNT_SUB |= dcnt;   // 32-bit: bits 30/31 are the ext BG / OBJ palette enables

	printf("");
	for (;;) {
		swiWaitForVBlank();
#if CI_CASE == 7
		f++;
		step();
#endif
	}
	return 0;
}
