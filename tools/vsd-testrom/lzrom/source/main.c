// SPDX-License-Identifier: CC0-1.0
//
// DEFERRED-CPU-COMPOSITING TRANSITION fixture (gx-next-steps-log.md queue item 13j).
//
// The 2D CPU compositor is skipped on frames the GX path handles and only rendered (late) when a
// barrier / an out-of-scope line / a GX bail asks for it. Every case here changes behaviour
// FRAME BY FRAME (the frame counter f drives it), so a single run contains many transitions in
// both directions: quiet frames GX handles, frames with a mid-frame write, frames that bail. The
// test compares a per-frame hash of the whole output against the forced-CPU build (every frame
// must be byte-identical; the alpha effects are not used).
//
// Both engines always show content: main = 4 text BGs + 4 sprites (bank A BG, bank B OBJ), sub =
// 2 text BGs + 2 sprites (bank C BG, bank I OBJ); bank D = LCD (capture destination), bank E =
// LCD (invisible to both engines: writes there must NOT cost a barrier). The hblank IRQ handler
// performs the per-case mid-frame writes at fixed lines on the frames the case selects.
//
//   0  A: BG0HOFS write at line 96, every 8th frame               reg barrier, GX handles the bands
//   1  A: BG1 mosaic on for lines 100..149, every 6th frame        hook-time out-of-scope (mosaic)
//   2  A: WIN0 enabled lines 60..119, every 5th frame              window bands, GX handles
//   3  A: OBJ window on lines 80..149, every 5th frame             out-of-scope (objwin)
//   4  A: display capture armed mid-frame (line 96, every 4th frame) and in vblank (every 4th, offset 2)
//   5  A: DISPCNT display mode 0 for lines 100..149 (every 6th), mode 2 whole frame (every 6th, offset 3)
//   6  A: palette writes at line 90 (every other frame)
//   7  A: OAM (line 80), BG tile VRAM (line 70), BG map VRAM (line 130) writes on different frames
//   8  A: writes to bank E (invisible LCD VRAM) at lines 50 and 100 every frame  -> no barrier, all GX
//   9  A: VRAMCNT_A remapped to LCD for lines 80..139, every 4th frame
//  10  A: affine BG2 (mode 2): BG2X rewritten at line 100 every 3rd frame, mosaic reg set on others
//  11  A: MASTER_BRIGHT full white lines 0..99 every 4th frame; MOSAIC register set (unused) every 4th offset 2
//  12  B: BG0HOFS write at line 96, every 8th frame
//  13  B: BG1 mosaic on lines 100..149, every 6th frame
//  14  B: WIN0 lines 60..119, every 5th frame
//  15  B: palette (line 90), OAM (line 80), tile VRAM (line 70) writes, rotating
//  16  A+B: HOFS rewritten on EVERY line for 4 frames out of 16 (band overflow) 
//  17  A: DISPCNT BG_Mode 2 -> 0 at line 100, restored at 150, every 6th frame (BG2 affine <-> text)
//  18  A: POWCNT1 screen swap toggled at line 100, every 6th frame
//  19  A: forced blank lines 100..109, every 5th frame
//  20  A: ExBGxPalette_Enable toggled at line 100, every 6th frame
//  21  A: BLDCNT brighten on BG1 lines 40..89 (EVY 8) every 4th frame + alpha on BG3 lines 90..149 every 4th offset 2
//  22  A+B: every engine register written twice in one frame at different lines (A HOFS line 50, B VOFS line 60), every 3rd frame
//  23  A: BG1 tile + palette written in vblank ONLY (line 200), scene changes every 4th frame (dirty gate through GX)
//  24  A: odd frames = WIN0 (rows 0..169) + brighten (bail 'winfade', CPU quirk: the windowed backdrop pass reads the STALE `blend1`
//      left by the previous line's last layer); even frames = plain, BLDCNT with OBJ as 1st target (GX handles).
//      The CPU pass of an odd frame's line 0 therefore depends on the last layer of the previous (GX-handled) frame:
//      the deferred-line 'discard' path must replay that leftover (GPU_DiscardDeferredLines).

#include <nds.h>
#include <stdio.h>

#ifndef LZ_CASE
#define LZ_CASE 0
#endif

#define R16(a) (*(volatile u16 *)(a))
#define R32(a) (*(volatile u32 *)(a))
#define A_DISPCNT R32(0x04000000)
#define A_BG0CNT  R16(0x04000008)
#define A_BG1CNT  R16(0x0400000A)
#define A_BG2CNT  R16(0x0400000C)
#define A_BG0HOFS R16(0x04000010)
#define A_BG0VOFS R16(0x04000012)
#define A_BG2X    R32(0x04000028)
#define A_WIN0H   R16(0x04000040)
#define A_WIN0V   R16(0x04000044)
#define A_WININ   R16(0x04000048)
#define A_WINOUT  R16(0x0400004A)
#define A_MOSAIC  R16(0x0400004C)
#define A_BLDCNT  R16(0x04000050)
#define A_BLDALPHA R16(0x04000052)
#define A_BLDY    R16(0x04000054)
#define A_DISPCAPCNT R32(0x04000064)
#define A_MBRIGHT R16(0x0400006C)
#define B_DISPCNT R32(0x04001000)
#define B_BG0HOFS R16(0x04001010)
#define B_BG0VOFS R16(0x04001012)
#define B_WIN0H   R16(0x04001040)
#define B_WIN0V   R16(0x04001044)
#define B_WININ   R16(0x04001048)
#define B_WINOUT  R16(0x0400104A)
#define B_MOSAIC  R16(0x0400104C)
#define B_BG1CNT  R16(0x0400100A)
#define R_POWCNT1 R16(0x04000304)
#define R_VRAMCNT_A (*(volatile u8 *)0x04000240)

#define L_BG0 1
#define L_BG1 2
#define L_BG2 4
#define L_BG3 8
#define L_OBJ 16
#define L_BD  32

#define VRAM_A_BASE ((u8 *)0x06000000)
#define VRAM_C_BASE ((u8 *)0x06200000)

static volatile int f;   // frame counter (advanced once per vblank in main)

static u8 hashb(int a, int b) { u32 h = (u32)a * 2654435761u ^ (u32)b * 40503u; h ^= h >> 13; h *= 0x5bd1e995; return (u8)(h >> 11); }

static void fillTiles4(u8 *dst, int nTiles, int seed)
{
	for (int t = 0; t < nTiles; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 4; x++) {
				u8 lo = hashb(t + seed * 977, y * 8 + x * 2) % 16, hi = hashb(t + seed * 977, y * 8 + x * 2 + 1) % 16;
				if (lo < 11) lo = 0;
				if (hi < 11) hi = 0;
				if (t == 0) lo = hi = 0;
				dst[t * 32 + y * 4 + x] = (u8)(lo | (hi << 4));
			}
}

static void fillMap(u16 *m, int seed)
{
	for (int i = 0; i < 32 * 32; i++)
		m[i] = (u16)((hashb(i, 31 + seed) % 64) | (((hashb(i, 77 + seed)) & 3) << 10) | (((hashb(i, 5 + seed)) & 15) << 12));
}

// affine BG2 (mode 1, 256x256 8bpp): one tile-map byte per 8x8 tile, tiles = 256-colour
static void setupAffine(void)
{
	u8 *tiles = VRAM_A_BASE + 0x10000;   // char block 4 (64KB..) : 64 tiles of 64 bytes
	for (int t = 0; t < 64; t++)
		for (int i = 0; i < 64; i++)
			tiles[t * 64 + i] = (u8)(((hashb(t, i) % 6) == 0) ? 0 : (1 + (hashb(t, i + 100) % 200)));
	u8 *map = VRAM_A_BASE + 0xE000;     // screen block 28 (0xE000): 16x16 tile map (128x128 px)
	for (int i = 0; i < 16 * 16; i++) map[i] = (u8)(hashb(i, 3) % 64);
}

static void setupEngines(void)
{
	// main: banks A (BG) / B (OBJ); sub: C (BG) / I (OBJ); D, E = LCD
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankB(VRAM_B_MAIN_SPRITE);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankI(VRAM_I_SUB_SPRITE);
	vramSetBankD(VRAM_D_LCD);
	vramSetBankE(VRAM_E_LCD);
	for (int i = 0; i < 0x20000; i++) VRAM_A_BASE[i] = 0;
	for (int i = 0; i < 0x20000; i++) VRAM_C_BASE[i] = 0;
	for (int i = 0; i < 256; i++) {
		BG_PALETTE[i]         = RGB15((i * 5 + 9) & 31, (i * 11 + 4) & 31, (i * 7 + 17) & 31);
		SPRITE_PALETTE[i]     = RGB15((i * 13 + 3) & 31, (i * 3 + 20) & 31, (i * 9 + 1) & 31);
		BG_PALETTE_SUB[i]     = RGB15((i * 3 + 5) & 31, (i * 9 + 14) & 31, (i * 5 + 2) & 31);
		SPRITE_PALETTE_SUB[i] = RGB15((i * 7 + 11) & 31, (i * 5 + 8) & 31, (i * 13 + 6) & 31);
	}
	BG_PALETTE[0] = RGB15(6, 21, 13);
	BG_PALETTE_SUB[0] = RGB15(20, 6, 9);
	// main: char blocks 0..2 (16KB), maps at screen blocks 24..27
	fillTiles4(VRAM_A_BASE + 0x0000, 64, 1);
	fillTiles4(VRAM_A_BASE + 0x4000, 64, 2);
	fillTiles4(VRAM_A_BASE + 0x8000, 64, 3);
	for (int b = 0; b < 4; b++) fillMap((u16 *)(VRAM_A_BASE + 0xC000 + b * 0x800), b);
	A_BG0CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(27) | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG1CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(24) | BG_TILE_BASE(1) | BG_PRIORITY(3);
	REG_BG2CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(25) | BG_TILE_BASE(2) | BG_PRIORITY(2);
	REG_BG3CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(26) | BG_TILE_BASE(0) | BG_PRIORITY(0);
#if LZ_CASE == 10 || LZ_CASE == 17
	setupAffine();
#endif
	// sub: char blocks 0..1, maps at screen blocks 24, 25
	fillTiles4(VRAM_C_BASE + 0x0000, 64, 5);
	fillTiles4(VRAM_C_BASE + 0x4000, 64, 6);
	fillMap((u16 *)(VRAM_C_BASE + 0xC000), 7);
	fillMap((u16 *)(VRAM_C_BASE + 0xC800), 8);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(24) | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(25) | BG_TILE_BASE(1) | BG_PRIORITY(2);
}

static u16 *gfxA, *gfxB;
static void setupObjs(void)
{
	oamInit(&oamMain, SpriteMapping_1D_32, false);
	oamInit(&oamSub, SpriteMapping_1D_32, false);
	gfxA = oamAllocateGfx(&oamMain, SpriteSize_32x32, SpriteColorFormat_16Color);
	fillTiles4((u8 *)gfxA, 16, 9);
	gfxB = oamAllocateGfx(&oamSub, SpriteSize_32x32, SpriteColorFormat_16Color);
	fillTiles4((u8 *)gfxB, 16, 12);
	static const int px[4] = { 8, 100, 180, 30 };
	static const int py[4] = { 10, 20, 8, 110 };
	for (int i = 0; i < 4; i++)
		oamSet(&oamMain, i, px[i], py[i], (i & 1) ? 2 : 0, i & 7, SpriteSize_32x32, SpriteColorFormat_16Color, gfxA, -1, false, false, false, false, false);
	for (int i = 4; i < 128; i++) oamMain.oamMemory[i].isHidden = true;
	for (int i = 0; i < 2; i++)
		oamSet(&oamSub, i, 20 + i * 90, 30 + i * 40, i & 1, i & 7, SpriteSize_32x32, SpriteColorFormat_16Color, gfxB, -1, false, false, false, false, false);
	for (int i = 2; i < 128; i++) oamSub.oamMemory[i].isHidden = true;
}

// ---- per-case hblank handler: writes at fixed lines (the write lands for the NEXT line) ----
static int vc(void) { return REG_VCOUNT; }
// direct hardware OAM write (libnds' oamMemory is only a RAM shadow copied at vblank): entry attr1.X
static void hwOamX(u32 base, int idx, int x) { volatile u16 *a = (volatile u16 *)(base + idx * 8 + 2); *a = (u16)((*a & 0xFE00) | (x & 0x1FF)); }

static void hb(void)
{
	const int L = vc(), ff = f;
#if LZ_CASE == 0
	if ((ff & 7) == 3 && L == 95) A_BG0HOFS = 100;      // (reset in vblank by main)
#elif LZ_CASE == 1
	if ((ff % 6) == 2) { if (L == 99) { A_BG1CNT |= BG_MOSAIC_ON; A_MOSAIC = 0x0033; } else if (L == 149) { A_BG1CNT &= ~BG_MOSAIC_ON; A_MOSAIC = 0; } }
#elif LZ_CASE == 2
	if ((ff % 5) == 1) { if (L == 59) A_DISPCNT |= (1u << 13); else if (L == 119) A_DISPCNT &= ~(1u << 13); }
#elif LZ_CASE == 3
	if ((ff % 5) == 1) { if (L == 79) A_DISPCNT |= (1u << 15); else if (L == 149) A_DISPCNT &= ~(1u << 15); }
#elif LZ_CASE == 4
	if ((ff & 3) == 0 && L == 95) A_DISPCAPCNT = (1u << 31) | (0u << 29) | (3u << 20) | (0u << 18) | (3u << 16) | 16;
#elif LZ_CASE == 5
	if ((ff % 6) == 2) { if (L == 99) A_DISPCNT &= ~(3u << 16); else if (L == 149) A_DISPCNT |= (1u << 16); }
#elif LZ_CASE == 6
	if ((ff & 1) == 0 && L == 89) { for (int i = 1; i < 16; i++) BG_PALETTE[i] = RGB15((i * 2 + ff) & 31, (i * 3) & 31, 31 - i); }
#elif LZ_CASE == 7
	if ((ff & 3) == 1 && L == 79) { hwOamX(0x07000000, 1, 40 + (ff & 15)); }
	if ((ff & 3) == 2 && L == 69) { for (int i = 0; i < 32; i++) VRAM_A_BASE[0x4000 + 32 * 5 + i] ^= 0xFF; }
	if ((ff & 3) == 3 && L == 129) { ((u16 *)(VRAM_A_BASE + 0xC000))[40] = (u16)(hashb(ff, 1) % 64); }
#elif LZ_CASE == 8
	if (L == 49 || L == 99) { for (int i = 0; i < 16; i++) ((volatile u16 *)0x06880000)[i] = (u16)(ff * 7 + i + L); }
#elif LZ_CASE == 9
	if ((ff & 3) == 0) { if (L == 79) R_VRAMCNT_A = 0x80; /* bank A -> LCD (MST 0) */ else if (L == 139) R_VRAMCNT_A = 0x81; /* main BG, offset 0 */ }
#elif LZ_CASE == 10
	if ((ff % 3) == 1 && L == 99) A_BG2X = 40 * 256 + (ff & 255);
	if ((ff % 3) == 2 && L == 40) A_MOSAIC = 0x0011;
	if ((ff % 3) == 2 && L == 41) A_MOSAIC = 0;
#elif LZ_CASE == 11
	if ((ff & 3) == 1 && L == 99) A_MBRIGHT = 0;     // white (set in vblank) for lines 0..99
	if ((ff & 3) == 3 && L == 5) A_MOSAIC = 0;       // (MOSAIC register nonzero with no layer using it, set in vblank, cleared at line 6)
#elif LZ_CASE == 12
	if ((ff & 7) == 3 && L == 95) B_BG0HOFS = 100;
#elif LZ_CASE == 13
	if ((ff % 6) == 2) { if (L == 99) { B_BG1CNT |= BG_MOSAIC_ON; B_MOSAIC = 0x0033; } else if (L == 149) { B_BG1CNT &= ~BG_MOSAIC_ON; B_MOSAIC = 0; } }
#elif LZ_CASE == 14
	if ((ff % 5) == 1) { if (L == 59) B_DISPCNT |= (1u << 13); else if (L == 119) B_DISPCNT &= ~(1u << 13); }
#elif LZ_CASE == 15
	if ((ff % 3) == 0 && L == 89) { for (int i = 1; i < 16; i++) BG_PALETTE_SUB[i] = RGB15((i * 2 + ff) & 31, (i * 3) & 31, 31 - i); }
	if ((ff % 3) == 1 && L == 79) { hwOamX(0x07000400, 0, 30 + (ff & 15)); }
	if ((ff % 3) == 2 && L == 69) { for (int i = 0; i < 32; i++) VRAM_C_BASE[0x4000 + 32 * 5 + i] ^= 0xFF; }
#elif LZ_CASE == 16
	if ((ff & 15) >= 8 && (ff & 15) < 12 && L < 191) { A_BG0HOFS = (u16)(L * 3); B_BG0VOFS = (u16)(L * 2); }
#elif LZ_CASE == 17
	if ((ff % 6) == 2) { if (L == 99) A_DISPCNT = (A_DISPCNT & ~7u) | 0u; else if (L == 149) A_DISPCNT = (A_DISPCNT & ~7u) | 2u; }
#elif LZ_CASE == 18
	if ((ff % 6) == 1) { if (L == 99) R_POWCNT1 ^= (1u << 15); else if (L == 149) R_POWCNT1 ^= (1u << 15); }
#elif LZ_CASE == 19
	if ((ff % 5) == 0) { if (L == 99) A_DISPCNT |= (1u << 7); else if (L == 109) A_DISPCNT &= ~(1u << 7); }
#elif LZ_CASE == 20
	if ((ff % 6) == 2) { if (L == 99) A_DISPCNT ^= (1u << 30); else if (L == 149) A_DISPCNT ^= (1u << 30); }
#elif LZ_CASE == 21
	if ((ff & 3) == 0) { if (L == 39) { A_BLDCNT = (2 << 6) | L_BG1; A_BLDY = 8; } else if (L == 89) A_BLDCNT = 0; }
	if ((ff & 3) == 2) { if (L == 89) { A_BLDCNT = (1 << 6) | L_BG3 | ((L_BG1 | L_BD) << 8); A_BLDALPHA = 9 | (7 << 8); } else if (L == 149) A_BLDCNT = 0; }
#elif LZ_CASE == 22
	if ((ff % 3) == 0) { if (L == 49) A_BG0HOFS = 33; else if (L == 149) A_BG0HOFS = 0; if (L == 59) B_BG0VOFS = 17; else if (L == 159) B_BG0VOFS = 0; }
#endif
	(void)L; (void)ff;
}

int main(void)
{
	irqEnable(IRQ_VBLANK);
	setupEngines();
	setupObjs();
	u32 dcnt = MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D;
#if LZ_CASE == 10 || LZ_CASE == 17
	// affine BG2 in mode 2 (BG2 = 256-colour rotscale, 16x16 tile map): replaces BG2's text form
	dcnt = MODE_2_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D;
#endif
	videoSetMode(dcnt);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
#if LZ_CASE == 10 || LZ_CASE == 17
	// BG2 as rotscale: 128x128 map (16x16 tiles) from screen base block 28, char base block 4, wrap
	REG_BG2CNT = BG_RS_16x16 | BG_MAP_BASE(28) | BG_TILE_BASE(4) | BG_WRAP_ON | BG_PRIORITY(2);
	REG_BG2PA = 200; REG_BG2PB = 30; REG_BG2PC = -30; REG_BG2PD = 220;
	A_BG2X = 12 * 256; REG_BG2Y = 9 * 256;
#endif
#if LZ_CASE == 4
	// nothing armed yet; the hblank handler arms mid-frame, main arms in vblank on other frames
#endif
#if LZ_CASE == 20
	// extended BG palettes on bank E as main BG ext palette? bank E is LCD here: keep the bit toggle meaningful with an empty slot
#endif
	irqSet(IRQ_HBLANK, hb);
	irqEnable(IRQ_HBLANK);
	for (;;) {
		swiWaitForVBlank();
		f++;
#if LZ_CASE == 0 || LZ_CASE == 16
		A_BG0HOFS = 0;
#endif
#if LZ_CASE == 12
		B_BG0HOFS = 0;
#endif
#if LZ_CASE == 16
		B_BG0VOFS = 0;
#endif
#if LZ_CASE == 11
		if ((f & 3) == 1) A_MBRIGHT = (1u << 14) | 16;
		if ((f & 3) == 3) A_MOSAIC = 0x0022;
#endif
#if LZ_CASE == 24
		if (f & 1) {
			A_DISPCNT |= (1u << 13);
			A_WIN0H = (0 << 8) | 255; A_WIN0V = (0 << 8) | 170;
			A_WININ = 0x3F; A_WINOUT = 0x1F;
			A_BLDCNT = (2 << 6) | L_BG1; A_BLDY = 6;
		} else {
			A_DISPCNT &= ~(1u << 13);
			A_BLDCNT = L_OBJ; A_BLDY = 0;
		}
#endif
#if LZ_CASE == 4
		if ((f & 3) == 2) A_DISPCAPCNT = (1u << 31) | (0u << 29) | (3u << 20) | (0u << 18) | (3u << 16) | 16;
#endif
#if LZ_CASE == 5
		if ((f % 6) == 3) A_DISPCNT = (A_DISPCNT & ~(3u << 16)) | (2u << 16) | (0u << 18);
		else if ((f % 6) == 4) A_DISPCNT = (A_DISPCNT & ~(3u << 16)) | (1u << 16);
#endif
#if LZ_CASE == 23
		if ((f & 3) == 0) { for (int i = 0; i < 32; i++) VRAM_A_BASE[0x4000 + 32 * 7 + i] = (u8)hashb(f, i); BG_PALETTE[3] = RGB15(f & 31, 10, 20); }
#endif
		oamUpdate(&oamMain);
		oamUpdate(&oamSub);
	}
	return 0;
}
