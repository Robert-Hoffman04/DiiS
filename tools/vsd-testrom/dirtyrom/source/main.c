// SPDX-License-Identifier: CC0-1.0
//
// DS Engine B (sub screen) DIRTY-GATING fixture for the GX 2D compositor
// (gx-next-steps-log.md task 10; source/gx/gx_ds_engineb_render.cpp).
//
// The compositor bakes each BG plane / OBJ slot into a cached texture and re-bakes
// it only when a byte it actually read (recorded in LCDC space), its config, or the
// VRAMCNT mapping changed. This ROM drives one event schedule per build
// (-DDR_CASE=N), keyed off a vblank counter `f`, whose result is byte-compared
// GX-vs-CPU (-DDSB_FORCE_CPU) at a settle frame after the last event, and whose
// bake COUNTS are read from the `dsbk` line of the harness profile log (-DDSB_STATS)
// to prove what did and did not re-bake.
//
// Common layout (sub engine, VRAM C = BBG at 0x06200000, D = BOBJ at 0x06600000):
//   BG0/BG1/BG2  4bpp text, 32x32, prio 0/1/2. Tiles share char block 0 but use
//                disjoint tile-number ranges (BG0 0-63, BG1 64-127, BG2 128-191 =
//                LCDC bytes 0x0000/0x0800/0x1000, 2KB each); maps at 0x4000/0x4800/
//                0x5000. Map palNum is 4*b + (0..3), so each BG owns four private
//                16-colour sub-palettes (BG0 0-3, BG1 4-7, BG2 8-11).
//   6 sprites    32x32 4bpp 1D, sprite i = OAM i, gfx at D+i*1024 (one 1KB LCDC dirty page each), palette i.
// Main engine: backdrop only (plus, in case 0, per-frame Engine A traffic that the
// old coarse gate mistook for Engine B work).
//
//   0  isolation + scroll: BG1 scrolls every frame, Engine A palette/VRAM/OAM written
//      every frame, BG2's tiles rewritten at f=60.       expect bg=1/1/2/0 obj all 1
//   1  OBJ + palette deps: f=60 sprite1 gfx, f=90 OAM slot 2 retargeted (slot reuse),
//      f=110 sprite palette 3, f=130 unused VRAM_D bytes, f=145 BG sub-palette 5.
//                                                         expect bg=1/2/1 o=1,2,2,2,1,1
//   2  VRAMCNT remap: banks C and H both hold a full layout (different content);
//      f=60 BBG C -> H (same NDS addresses, different bytes, no CPU write to them),
//      f=120 back to C.                                   expect bg=3/3/3 (bank changes)
//   3  BG disabled-then-enabled: BG1 off f=40, its tiles f=50 / map f=52 / palette f=54
//      rewritten while off, BG1 back on f=70; BG2 off f=40..70 with NO writes.
//                                                         expect bg=1/2/1 (BG2 keeps its cache)
//   4  OBJ hidden-then-visible: slot 2 parked off-screen, slot 4 OBJ-disabled, slot 5
//      parked, all f=40..70; slots 2 and 4 get gfx+palette writes at f=50.
//                                                         expect o=1,1,2,1,2,1
//   5  fade variant staleness: brighten BG0|BG1|OBJ until f=40, off f=40..80 (variants
//      unused), BG0 tiles + sprite 0 gfx rewritten f=50, brighten again f=80.
//   6  affine planes (mode 2): BG2 512x512 wrap, BG3 128x128 wrap, BG0 text. f=60 BG3's
//      tiles, f=100 one BG2 map entry, f=140 BG palette entry 200.
//                                                         expect bg=1/-/3/3
//   7  extended BG palette: BG0 8bpp text with bank-H ext palette; f=60 ext palette
//      rewritten through H's LCDC window and remapped, all in one frame.
//                                                         expect bg=2/1
//   8  fuzz: two pseudo-random ops per frame (frames 21..DR_STOP) across every dependency
//      class (tiles, maps, palettes, sprite gfx/pal/OAM slot reuse, BG enable toggles,
//      brighten on/off, scroll). -DDR_SEED / -DDR_STOP pick the sequence; a bounded build
//      ends on a static image (A/B-comparable), the unbounded one is for the in-emulator
//      -DDSB_VERIFY soak, which needs no capture at all.

#include <nds.h>
#include <stdio.h>

#ifndef DR_CASE
#define DR_CASE 0
#endif

#define SUBVRAM ((u8 *)0x06200000)
#define SUBOBJ  ((u8 *)0x06600000)

#define R16(a) (*(volatile u16 *)(a))
#define SUB_DISPCNT R16(0x04001000)
#define SUB_BLDCNT  R16(0x04001050)
#define SUB_BLDY    R16(0x04001054)
#define SUBOAM      ((volatile u16 *)0x07000400)
#define MAINOAM     ((volatile u16 *)0x07000000)

#define DISPCNT_BG0 (1u << 8)
#define DISPCNT_BG1 (1u << 9)
#define DISPCNT_BG2 (1u << 10)
#define DISPCNT_BG3 (1u << 11)
#define DISPCNT_OBJ (1u << 12)

static u8 hashb(int a, int b) { u32 h = (u32)a * 2654435761u ^ (u32)b * 40503u; h ^= h >> 13; h *= 0x5bd1e995; return (u8)(h >> 11); }

static void fillPalettes(void)
{
	for (int i = 0; i < 256; i++) {
		BG_PALETTE_SUB[i]     = RGB15((i * 5 + 9) & 31, (i * 11 + 4) & 31, (i * 7 + 17) & 31);
		SPRITE_PALETTE_SUB[i] = RGB15((i * 13 + 3) & 31, (i * 3 + 20) & 31, (i * 9 + 1) & 31);
	}
	BG_PALETTE_SUB[0] = RGB15(6, 21, 13); // backdrop
}

// 4bpp tiles: ~35% texels index 0; local tile 0 fully transparent.
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
static void fillTiles8(u8 *dst, int nTiles, int seed)
{
	for (int t = 0; t < nTiles; t++)
		for (int i = 0; i < 64; i++) {
			u8 v = hashb(t + seed * 977, i);
			if (v < 80 || t == 0) v = 0;
			dst[t * 64 + i] = v;
		}
}

// Text BG b (0..2): tiles b*64 .. b*64+63, palNum 4b..4b+3.
static void fillTextMap(u16 *m, int b, int salt)
{
	for (int i = 0; i < 32 * 32; i++)
		m[i] = (u16)((b * 64 + hashb(i, 31 + b + salt) % 64) | (((hashb(i, 77 + b + salt)) & 3) << 10) |
		             ((4 * b + (hashb(i, 5 + b + salt) & 3)) << 12));
}

// Full compact text layout into a VRAM window (works for bank C or H mapped as LCD).
static void layoutText(u8 *v, int seedBase, int salt)
{
	for (int i = 0; i < 0x6000; i++) v[i] = 0;
	fillTiles4(v + 0x0000, 64, seedBase + 1);
	fillTiles4(v + 0x0800, 64, seedBase + 2);
	fillTiles4(v + 0x1000, 64, seedBase + 3);
	for (int b = 0; b < 3; b++) fillTextMap((u16 *)(v + 0x4000 + b * 0x800), b, salt);
}

static void setupTextRegs(void)
{
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(8)  | BG_TILE_BASE(0) | BG_PRIORITY(0);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(9)  | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG2CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(10) | BG_TILE_BASE(0) | BG_PRIORITY(2);
}

// --- sprites (raw OAM: no libnds shadow, so the ROM controls every byte) ---
static void oamPut(int i, int x, int y, int tile, int pal, int prio)
{
	SUBOAM[i * 4 + 0] = (u16)(y & 0xFF);                       // 4bpp, square, normal
	SUBOAM[i * 4 + 1] = (u16)((x & 0x1FF) | (2 << 14));        // 32x32
	SUBOAM[i * 4 + 2] = (u16)((tile & 0x3FF) | (prio << 10) | (pal << 12));
}
static const int spX[6] = { 8, 100, 180, 30, 130, 200 };
static const int spY[6] = { 10, 20, 8, 110, 120, 100 };
static void setupObjs(void)
{
	for (int i = 0; i < 128; i++) SUBOAM[i * 4 + 0] = (2 << 8);   // OBJ-disabled
	for (int i = 0; i < 6; i++) fillTiles4(SUBOBJ + i * 1024, 16, 20 + i);
	for (int i = 0; i < 6; i++) oamPut(i, spX[i], spY[i], i * 32, i, 0);
}

static int f = 0;

#if DR_CASE == 0
static void step(void)
{
	if (f < 100) REG_BG1HOFS_SUB = f & 0x1FF;   // scrolls 100 frames, then holds (a moving capture would not be A/B-stable)
	// Engine A traffic: palette, VRAM (bank A as main BG), OAM -- none of it Engine B's.
	BG_PALETTE[1 + (f & 7)] = (u16)(f * 31);
	*(volatile u16 *)(0x06000000 + ((f * 2) & 0x3FFF)) = (u16)f;
	MAINOAM[(f & 63) * 4 + 3] = (u16)f;
	if (f == 60) fillTiles4(SUBVRAM + 0x1000, 64, 40);        // BG2's tile range only
}
#elif DR_CASE == 1
static void step(void)
{
	if (f == 60) fillTiles4(SUBOBJ + 1 * 1024, 16, 50);        // sprite 1's gfx
	if (f == 90) oamPut(2, 150, 60, 4 * 32, 6, 0);            // slot 2 now shows sprite 4's gfx, pal 6, elsewhere
	if (f == 110) for (int j = 0; j < 16; j++) SPRITE_PALETTE_SUB[3 * 16 + j] = RGB15((j * 2 + 1) & 31, 31 - j, (j * 3) & 31);
	if (f == 130) for (int j = 0; j < 256; j++) SUBOBJ[0x3000 + j] = (u8)j;   // bytes no sprite reads
	if (f == 145) for (int j = 0; j < 16; j++) BG_PALETTE_SUB[5 * 16 + j] = RGB15((j * 3 + 2) & 31, (j * 5) & 31, 31 - j); // BG1's sub-palette 5
}
#elif DR_CASE == 2
static void step(void)
{
	if (f == 60) { vramSetBankC(VRAM_C_LCD); vramSetBankH(VRAM_H_SUB_BG); }
	if (f == 120) { vramSetBankH(VRAM_H_LCD); vramSetBankC(VRAM_C_SUB_BG); }
}
#elif DR_CASE == 3
static void step(void)
{
	if (f == 40) SUB_DISPCNT &= ~(DISPCNT_BG1 | DISPCNT_BG2);
	if (f == 50) fillTiles4(SUBVRAM + 0x0800, 64, 60);        // BG1 tiles
	if (f == 52) fillTextMap((u16 *)(SUBVRAM + 0x4800), 1, 9);   // BG1 map
	if (f == 54) for (int j = 0; j < 16; j++) BG_PALETTE_SUB[5 * 16 + j] = RGB15((j * 7 + 3) & 31, (j * 2) & 31, 31 - j);
	if (f == 70) SUB_DISPCNT |= DISPCNT_BG1 | DISPCNT_BG2;
}
#elif DR_CASE == 4
static void step(void)
{
	if (f == 40) {
		oamPut(2, spX[2], 200, 2 * 32, 2, 0);                  // parked below the screen
		SUBOAM[4 * 4 + 0] |= (2 << 8);                         // slot 4: OBJ-disabled
		oamPut(5, spX[5], 200, 5 * 32, 5, 0);                  // parked, never written
	}
	if (f == 50) {
		fillTiles4(SUBOBJ + 2 * 1024, 16, 70);
		fillTiles4(SUBOBJ + 4 * 1024, 16, 71);
		for (int j = 0; j < 16; j++) SPRITE_PALETTE_SUB[4 * 16 + j] = RGB15((j * 5 + 9) & 31, (j * 3) & 31, 31 - j);
	}
	if (f == 70) {
		oamPut(2, spX[2], spY[2], 2 * 32, 2, 0);
		SUBOAM[4 * 4 + 0] &= ~(2 << 8);
		oamPut(5, spX[5], spY[5], 5 * 32, 5, 0);
	}
}
#elif DR_CASE == 5
static void step(void)
{
	if (f == 40) SUB_BLDCNT = 0;
	if (f == 50) { fillTiles4(SUBVRAM + 0x0000, 64, 80); fillTiles4(SUBOBJ, 16, 81); }
	if (f == 80) { SUB_BLDCNT = (2 << 6) | 1 | 2 | 16; SUB_BLDY = 9; }
}
#elif DR_CASE == 6
static void step(void)
{
	if (f == 60) fillTiles8(SUBVRAM + 0x14000, 64, 90);                // BG3's tile set only
	if (f == 100) ((volatile u16 *)(SUBVRAM + 0xC000))[7] = 0x0303;    // one BG2 map entry (bytes)
	if (f == 140) BG_PALETTE_SUB[200] = RGB15(31, 0, 31);
}
#elif DR_CASE == 7
static void extFill(int seed)
{
	vramSetBankH(VRAM_H_LCD);
	u16 *e = VRAM_H;
	for (int i = 0; i < 16 * 256; i++) e[i] = RGB15((i * 5 + seed) & 31, (i * 3 + seed * 2) & 31, (i + seed * 7) & 31);
	vramSetBankH(VRAM_H_SUB_BG_EXT_PALETTE);
}
static void step(void)
{
	if (f == 60) extFill(11);
}
#elif DR_CASE == 8
#ifndef DR_SEED
#define DR_SEED 0x1234567u
#endif
#ifndef DR_OPMASK
#define DR_OPMASK 0xFFFF   // bit k enables op kind k (see op())
#endif
#ifndef DR_STOP
#define DR_STOP 1000000   // last frame with ops (a bounded build has a static final image, so a capture is A/B-stable)
#endif
static u32 rng = DR_SEED;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int bgOn = 7, parked = 0, fade = 0;
static void op(void)
{
	u32 r = rnd();
	int k = (r >> 4) % 13, a = (r >> 12) & 0xFFF, b = (r >> 20) & 0xFF;
	if (!((DR_OPMASK >> k) & 1)) return;   // (the rng stream is consumed either way, so masked builds see the same op sequence)
	switch (k) {
	case 0: SUBVRAM[(a % 3) * 0x800 + (b % 64) * 32 + (a >> 3) % 32] = (u8)rnd(); break;       // a tile byte of BG a%3
	case 1: ((volatile u16 *)(SUBVRAM + 0x4000 + (a % 3) * 0x800))[b * 4 % 1024] = (u16)((a % 3) * 64 + b % 64 | (((b >> 6) & 3) << 10) | ((4 * (a % 3) + (b & 3)) << 12)); break;
	case 2: BG_PALETTE_SUB[a & 255] = (u16)rnd(); break;
	case 3: SUBOBJ[(a % 6) * 1024 + (b * 2 % 512)] = (u8)rnd(); break;                           // sprite gfx byte
	case 4: SPRITE_PALETTE_SUB[a & 127] = (u16)rnd(); break;
	case 5: { int s = a % 6, t = b % 6; oamPut(s, spX[t] + (a & 15), spY[t] + (b & 15), t * 32, (b >> 3) % 8, s & 1); parked &= ~(1 << s); break; } // slot reuse
	case 6: bgOn ^= 1 << (a % 3); SUB_DISPCNT = (SUB_DISPCNT & ~(7u << 8)) | ((u32)bgOn << 8); break;
	case 7: { int s = a % 6; parked ^= 1 << s; if (parked & (1 << s)) SUBOAM[s * 4 + 0] |= (2 << 8); else SUBOAM[s * 4 + 0] &= ~(2 << 8); break; }
	case 8: fade = !fade; if (fade) { SUB_BLDCNT = (2 << 6) | 1 | 2 | 16 | ((b & 1) ? 4 : 0); SUB_BLDY = 1 + (b % 15); } else SUB_BLDCNT = 0; break;
	case 9: REG_BG1HOFS_SUB = a & 0x1FF; REG_BG2VOFS_SUB = b; break;
	case 10: fillTiles4(SUBVRAM + (a % 3) * 0x800 + (b % 56) * 32, 8, (int)(r & 0xFF)); break;  // 8 tiles (kept short so every op finishes inside vblank)
	case 11: fillTiles4(SUBOBJ + (a % 6) * 1024 + (b % 8) * 64, 2, (int)(r & 0xFF)); break;      // 2 sprite tiles
	default: break;
	}
}
static void step(void) { if (f > 20 && f <= DR_STOP) { op(); op(); } }
#endif

int main(void)
{
	irqEnable(IRQ_VBLANK);
	BG_PALETTE[0] = RGB15(0, 0, 0);
	fillPalettes();

#if DR_CASE == 6
	videoSetMode(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_SUB_SPRITE);
	videoSetModeSub(MODE_2_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	for (int i = 0; i < 0x20000; i++) SUBVRAM[i] = 0;
	fillTiles4(SUBVRAM + 0x0000, 64, 1);
	fillTextMap((u16 *)(SUBVRAM + 0x4000), 0, 0);
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(0);
	// (screen base is a 5-bit field: maps must sit below 64KB; tile data may go higher)
	// BG2: 512x512 affine, tiles char block 4 (0x10000, 128 of 256 used), map at 0xC000 (block 24)
	fillTiles8(SUBVRAM + 0x10000, 128, 3);
	for (int i = 0; i < 64 * 64; i++) SUBVRAM[0xC000 + i] = (u8)(hashb(i, 12) % 128);
	REG_BG2CNT_SUB = BG_RS_64x64 | BG_WRAP_ON | BG_COLOR_256 | BG_MAP_BASE(24) | BG_TILE_BASE(4) | BG_PRIORITY(1);
	REG_BG2PA_SUB = 0xF0; REG_BG2PB_SUB = 0x40; REG_BG2PC_SUB = -0x40; REG_BG2PD_SUB = 0xF0;
	REG_BG2X_SUB = 40 << 8; REG_BG2Y_SUB = 60 << 8;
	// BG3: 128x128 affine, tiles char block 5 (0x14000, 64 used), map at 0xE000 (block 28)
	fillTiles8(SUBVRAM + 0x14000, 64, 4);
	for (int i = 0; i < 16 * 16; i++) SUBVRAM[0xE000 + i] = (u8)(hashb(i, 13) % 64);
	REG_BG3CNT_SUB = BG_RS_16x16 | BG_WRAP_ON | BG_COLOR_256 | BG_MAP_BASE(28) | BG_TILE_BASE(5) | BG_PRIORITY(0);
	REG_BG3PA_SUB = 0x80; REG_BG3PB_SUB = 0; REG_BG3PC_SUB = 0; REG_BG3PD_SUB = 0x90;
	REG_BG3X_SUB = 0; REG_BG3Y_SUB = 0;
#elif DR_CASE == 7
	videoSetMode(MODE_0_2D);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_SUB_SPRITE);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
	for (int i = 0; i < 0x8000; i++) SUBVRAM[i] = 0;
	fillTiles8(SUBVRAM + 0x0000, 128, 5);                                 // BG0: 8bpp tiles 0..127
	{ u16 *m = (u16 *)(SUBVRAM + 0x4000); for (int i = 0; i < 1024; i++) m[i] = (u16)((hashb(i, 3) % 128) | ((hashb(i, 4) & 3) << 12)); }
	fillTiles4(SUBVRAM + 0x2000, 64, 6);                                  // BG1: 4bpp tiles 0x2000/32 = 256..
	{ u16 *m = (u16 *)(SUBVRAM + 0x4800); for (int i = 0; i < 1024; i++) m[i] = (u16)((256 + hashb(i, 5) % 64) | ((4 + (hashb(i, 6) & 3)) << 12)); }
	REG_BG0CNT_SUB = BG_32x32 | BG_COLOR_256 | BG_MAP_BASE(8) | BG_TILE_BASE(0) | BG_PRIORITY(1);
	REG_BG1CNT_SUB = BG_32x32 | BG_COLOR_16  | BG_MAP_BASE(9) | BG_TILE_BASE(0) | BG_PRIORITY(0);
	REG_DISPCNT_SUB |= (1u << 30);                                            // BG extended palettes on
	extFill(3);
#else
	videoSetMode(MODE_0_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_SUB_SPRITE);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D);
#if DR_CASE == 2
	// bank C: content X, bank H: content Y, both filled through their LCDC windows, then C is BBG
	vramSetBankC(VRAM_C_LCD);
	vramSetBankH(VRAM_H_LCD);
	layoutText((u8 *)VRAM_C, 0, 0);
	layoutText((u8 *)VRAM_H, 10, 17);
	vramSetBankC(VRAM_C_SUB_BG);
#else
	layoutText(SUBVRAM, 0, 0);
#endif
	setupTextRegs();
#if DR_CASE == 5
	SUB_BLDCNT = (2 << 6) | 1 | 2 | 16; SUB_BLDY = 9;                       // brighten BG0|BG1|OBJ
#endif
#endif
	setupObjs();

	printf("");
	for (;;) {
		swiWaitForVBlank();
		f++;
		step();
	}
	return 0;
}
