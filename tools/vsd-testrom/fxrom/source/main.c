// SPDX-License-Identifier: CC0-1.0
//
// DS Engine B (sub screen) WINDOW / COLOUR-EFFECT / MASTER_BRIGHT fixture for the
// GX 2D compositor (gx-next-steps-log.md task 9; source/gx/gx_ds_engineb_render.cpp).
//
// Everything is static after setup (except the HBlank-IRQ cases, which are
// deterministic per line), so a capture at any frame past boot is byte-stable and
// can be A/B-compared between the GX path and the forced-CPU reference
// (-DDSB_FORCE_CPU). One scenario per build, chosen by -DFX_CASE=N. Main engine
// shows a flat backdrop. Sub screen: mode 0, three 4bpp text BGs (BG0 prio 0 /
// BG1 prio 1 / BG2 prio 2) of noisy tiles with plenty of transparent texels so
// every layer shows through the ones above, plus up to 6 non-overlapping 32x32
// sprites (prio 0 unless noted).
//
//   0  WIN0 only: BG0|BG1|OBJ inside, BG1|BG2|OBJ outside
//   1  WIN0 + WIN1 overlapping: WIN0 BG0|BG2, WIN1 BG1|OBJ, outside BG2|OBJ
//   2  wrapped windows (X1>X2, Y1>Y2) + an empty window (X1==X2)
//   3  brighten (EVY 9) of BG0|BG1|BD|OBJ
//   4  darken (EVY 5) of BG1|BG2|OBJ
//   5  alpha, T1 = BG0, T2 = BG1|BG2|BD, EVA 10 / EVB 6      (native)
//   6  alpha, T2 missing BG2                                  (must bail: blendunder)
//   7  semi-transparent sprites (Mode 1), effect field 0, T2 = BG0|BG1|BG2|BD, 12/4
//   8  as 7 but two sprites overlap                           (must bail: blendobjoverlap)
//   9  windows + alpha: effect bit on inside WIN0 only, T1 = BG0|OBJ, 8/8
//  10  window + brighten                                      (must bail: winfade)
//  11  MASTER_BRIGHT up, factor 7
//  12  brighten (EVY 4) + MASTER_BRIGHT down factor 6
//  13  MASTER_BRIGHT changed at line 96 (up 8 above, down 10 below)
//  14  alpha coefficient sweep: BLDALPHA rewritten every 12 lines (EVA 0..15, EVB 16-EVA)
//  15  window off + on mid-frame: WIN0 above line 96, brighten below (2 bands)
//  16  alpha with EVA 12 / EVB 12                             (must bail: blendcoef)
//  17  OBJ window enabled                                     (must bail: objwin)
//  18  BG0 mosaic                                             (must bail: mosaic)
//  19  semi-transparent sprites + brighten with OBJ as 1st target
//  20  alpha, T1 = BG0, T2 = OBJ only (never blends: plain draw)
//  21  windows + semi sprites + alpha, effect bits differ per region
//  22  MASTER_BRIGHT up, factor 16 (all white) 
//  23  alpha with a mix of window regions where a layer is off inside WIN1

#include <nds.h>
#include <stdio.h>

#ifndef FX_CASE
#define FX_CASE 0
#endif

#define SUBVRAM ((u8 *)0x06200000)

#define R16(a) (*(volatile u16 *)(a))
#define SUB_DISPCNT R16(0x04001000)
#define SUB_WIN0H   R16(0x04001040)
#define SUB_WIN1H   R16(0x04001042)
#define SUB_WIN0V   R16(0x04001044)
#define SUB_WIN1V   R16(0x04001046)
#define SUB_WININ   R16(0x04001048)
#define SUB_WINOUT  R16(0x0400104A)
#define SUB_MOSAIC  R16(0x0400104C)
#define SUB_BLDCNT  R16(0x04001050)
#define SUB_BLDALPHA R16(0x04001052)
#define SUB_BLDY    R16(0x04001054)
#define SUB_MBRIGHT R16(0x0400106C)

// layer bits
#define L_BG0 1
#define L_BG1 2
#define L_BG2 4
#define L_OBJ 16
#define L_BD  32
#define FXB   32   // effect-enable bit in WININ/WINOUT

static u8 hashb(int a, int b) { u32 h = (u32)a * 2654435761u ^ (u32)b * 40503u; h ^= h >> 13; h *= 0x5bd1e995; return (u8)(h >> 11); }

static void fillPalettes(void)
{
	for (int i = 0; i < 256; i++) {
		BG_PALETTE_SUB[i]     = RGB15((i * 5 + 9) & 31, (i * 11 + 4) & 31, (i * 7 + 17) & 31);
		SPRITE_PALETTE_SUB[i] = RGB15((i * 13 + 3) & 31, (i * 3 + 20) & 31, (i * 9 + 1) & 31);
	}
	BG_PALETTE_SUB[0] = RGB15(6, 21, 13); // backdrop
}

// 4bpp tiles: ~35% of texels are index 0 (transparent), tile 0 is fully transparent.
static void fillTiles4(u8 *dst, int nTiles, int seed)
{
	for (int t = 0; t < nTiles; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 4; x++) {
				u8 lo = hashb(t + seed * 977, y * 8 + x * 2) % 16, hi = hashb(t + seed * 977, y * 8 + x * 2 + 1) % 16;
				if (lo < 6) lo = 0;
				if (hi < 6) hi = 0;
				if (t == 0) lo = hi = 0;
				dst[t * 32 + y * 4 + x] = (u8)(lo | (hi << 4));
			}
}

static void setupBgs(void)
{
	for (int i = 0; i < 0x20000; i++) SUBVRAM[i] = 0;
	fillTiles4(SUBVRAM + 0x0000, 64, 1);
	fillTiles4(SUBVRAM + 0x4000, 64, 2);
	fillTiles4(SUBVRAM + 0x8000, 64, 3);
	for (int b = 0; b < 3; b++) {
		u16 *m = (u16 *)(SUBVRAM + 0xC000 + b * 0x800);
		for (int i = 0; i < 32 * 32; i++)
			m[i] = (u16)((hashb(i, 31 + b) % 64) | (((hashb(i, 77 + b)) & 3) << 10) | (((hashb(i, 5 + b)) & 15) << 12));
	}
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(24) | BG_TILE_BASE(0) | BG_PRIORITY(0);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(25) | BG_TILE_BASE(1) | BG_PRIORITY(1);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(26) | BG_TILE_BASE(2) | BG_PRIORITY(2);
}

static void setupObjs(int semiMask, int overlap)
{
	oamInit(&oamSub, SpriteMapping_1D_32, false);
	u16 *g = oamAllocateGfx(&oamSub, SpriteSize_32x32, SpriteColorFormat_16Color);
	fillTiles4((u8 *)g, 16, 9);
	for (int i = 0; i < 16 * 32; i++) ((u8 *)g)[i] |= 0; // (keep as filled)
	static const int px[6] = { 8, 100, 180, 30, 130, 200 };
	static const int py[6] = { 10, 20, 8, 110, 120, 100 };
	for (int i = 0; i < 6; i++) {
		int x = px[i], y = py[i];
		if (overlap && i == 1) { x = 20; y = 22; }         // overlaps sprite 0
		oamSet(&oamSub, i, x, y, 0, i & 7, SpriteSize_32x32, SpriteColorFormat_16Color, g, -1, false, false, false, false, false);
		if (semiMask & (1 << i)) oamSub.oamMemory[i].blendMode = OBJMODE_BLENDED;
	}
	for (int i = 6; i < 128; i++) oamSub.oamMemory[i].isHidden = true;
}

#if FX_CASE == 13
static void hb(void) { if (REG_VCOUNT == 95) SUB_MBRIGHT = (2u << 14) | 10; else if (REG_VCOUNT == 262 || REG_VCOUNT == 261) SUB_MBRIGHT = (1u << 14) | 8; }
#endif
#if FX_CASE == 14
static void hb(void)
{
	int v = REG_VCOUNT;
	// write for the NEXT line: this fires at the end of line v's visible part
	if (v < 191) { int band = (v + 1) / 12; if (band > 15) band = 15; SUB_BLDALPHA = (u16)(band | ((16 - band) << 8)); }
	else if (v == 261 || v == 262) SUB_BLDALPHA = 0x1000;
}
#endif
#if FX_CASE == 15
static void hb(void)
{
	if (REG_VCOUNT == 95) { SUB_DISPCNT &= ~(1u << 13); SUB_BLDCNT = (2 << 6) | L_BG0 | L_BG1 | L_BD; SUB_BLDY = 7; }
	else if (REG_VCOUNT == 261 || REG_VCOUNT == 262) { SUB_DISPCNT |= (1u << 13); SUB_BLDCNT = 0; }
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
	setupBgs();

	int objs = 1;
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);

	int semi = 0, overlap = 0;
#if FX_CASE == 0
	SUB_DISPCNT |= (1u << 13);
	SUB_WIN0H = (40 << 8) | 200; SUB_WIN0V = (30 << 8) | 150;
	SUB_WININ = (L_BG0 | L_BG1 | L_OBJ); SUB_WINOUT = (L_BG1 | L_BG2 | L_OBJ);
#elif FX_CASE == 1
	SUB_DISPCNT |= (3u << 13);
	SUB_WIN0H = (20 << 8) | 140; SUB_WIN0V = (20 << 8) | 120;
	SUB_WIN1H = (100 << 8) | 230; SUB_WIN1V = (60 << 8) | 170;
	SUB_WININ = (L_BG0 | L_BG2) | ((L_BG1 | L_OBJ) << 8); SUB_WINOUT = (L_BG2 | L_OBJ);
#elif FX_CASE == 2
	SUB_DISPCNT |= (3u << 13);
	SUB_WIN0H = (200 << 8) | 40; SUB_WIN0V = (150 << 8) | 30;      // wraps in X and Y
	SUB_WIN1H = (90 << 8) | 90;  SUB_WIN1V = (10 << 8) | 180;      // empty in X
	SUB_WININ = (L_BG0 | L_OBJ) | ((L_BG1 | L_BG2) << 8); SUB_WINOUT = (L_BG1 | L_BG2 | L_OBJ);
#elif FX_CASE == 3
	SUB_BLDCNT = (2 << 6) | L_BG0 | L_BG1 | L_BD | L_OBJ; SUB_BLDY = 9;
#elif FX_CASE == 4
	SUB_BLDCNT = (3 << 6) | L_BG1 | L_BG2 | L_OBJ; SUB_BLDY = 5;
#elif FX_CASE == 5
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 10 | (6 << 8);
#elif FX_CASE == 6
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BD) << 8); SUB_BLDALPHA = 10 | (6 << 8);
#elif FX_CASE == 7 || FX_CASE == 8
	semi = 0x1B; overlap = (FX_CASE == 8);
	SUB_BLDCNT = ((L_BG0 | L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 12 | (4 << 8);
#elif FX_CASE == 9
	SUB_DISPCNT |= (3u << 13);
	SUB_WIN0H = (30 << 8) | 170; SUB_WIN0V = (20 << 8) | 140;
	SUB_WIN1H = (150 << 8) | 250; SUB_WIN1V = (100 << 8) | 190;
	SUB_WININ = (L_BG0 | L_BG1 | L_OBJ | FXB) | ((L_BG0 | L_BG1 | L_BG2 | L_OBJ) << 8);
	SUB_WINOUT = (L_BG0 | L_BG1 | L_BG2 | L_OBJ | FXB);
	SUB_BLDCNT = (1 << 6) | L_BG0 | L_OBJ | ((L_BG0 | L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 8 | (8 << 8);
#elif FX_CASE == 10
	SUB_DISPCNT |= (1u << 13);
	SUB_WIN0H = (40 << 8) | 200; SUB_WIN0V = (30 << 8) | 150;
	SUB_WININ = (L_BG0 | L_BG1 | L_OBJ | FXB); SUB_WINOUT = (L_BG1 | L_BG2 | L_OBJ | FXB);
	SUB_BLDCNT = (2 << 6) | L_BG0 | L_BG1 | L_BD; SUB_BLDY = 8;
#elif FX_CASE == 11
	SUB_MBRIGHT = (1u << 14) | 7;
#elif FX_CASE == 12
	SUB_BLDCNT = (2 << 6) | L_BG0 | L_BG2 | L_BD; SUB_BLDY = 4;
	SUB_MBRIGHT = (2u << 14) | 6;
#elif FX_CASE == 13
	SUB_MBRIGHT = (1u << 14) | 8;
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#elif FX_CASE == 14
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 0 | (16 << 8);
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#elif FX_CASE == 15
	SUB_DISPCNT |= (1u << 13);
	SUB_WIN0H = (40 << 8) | 200; SUB_WIN0V = (30 << 8) | 150;
	SUB_WININ = (L_BG0 | L_BG1 | L_OBJ); SUB_WINOUT = (L_BG1 | L_BG2 | L_OBJ);
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#elif FX_CASE == 16
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 12 | (12 << 8);
#elif FX_CASE == 17
	SUB_DISPCNT |= (1u << 15);
	SUB_WINOUT = (L_BG0 | L_BG1 | L_BG2 | L_OBJ) | ((L_BG1) << 8);
#elif FX_CASE == 18
	REG_BG0CNT_SUB |= BG_MOSAIC_ON; SUB_MOSAIC = 0x0033;
#elif FX_CASE == 19
	semi = 0x0F;
	SUB_BLDCNT = (2 << 6) | L_OBJ | ((L_BG0 | L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDY = 6; SUB_BLDALPHA = 9 | (7 << 8);
#elif FX_CASE == 20
	SUB_BLDCNT = (1 << 6) | L_BG0 | (L_OBJ << 8); SUB_BLDALPHA = 8 | (8 << 8);
#elif FX_CASE == 21
	semi = 0x0A;
	SUB_DISPCNT |= (3u << 13);
	SUB_WIN0H = (10 << 8) | 130; SUB_WIN0V = (5 << 8) | 130;
	SUB_WIN1H = (110 << 8) | 246; SUB_WIN1V = (80 << 8) | 186;
	SUB_WININ = (L_BG0 | L_BG1 | L_OBJ | FXB) | ((L_BG1 | L_BG2 | L_OBJ) << 8);
	SUB_WINOUT = (L_BG0 | L_BG1 | L_BG2 | L_OBJ | FXB);
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG0 | L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 11 | (5 << 8);
#elif FX_CASE == 22
	SUB_MBRIGHT = (1u << 14) | 16;
#elif FX_CASE == 23
	SUB_DISPCNT |= (3u << 13);
	SUB_WIN0H = (16 << 8) | 120; SUB_WIN0V = (16 << 8) | 100;
	SUB_WIN1H = (90 << 8) | 240; SUB_WIN1V = (70 << 8) | 180;
	SUB_WININ = (L_BG0 | L_BG1 | L_BG2 | FXB) | ((L_BG0 | L_BG2 | FXB) << 8);     // BG1 absent inside WIN1
	SUB_WINOUT = (L_BG0 | L_BG1 | L_BG2 | FXB);
	SUB_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BG2 | L_BD) << 8); SUB_BLDALPHA = 6 | (10 << 8);
#endif

	if (objs) setupObjs(semi, overlap);

	printf("");
	for (;;) {
		swiWaitForVBlank();
		if (objs) oamUpdate(&oamSub);
	}
	return 0;
}
