// SPDX-License-Identifier: CC0-1.0
//
// DS Engine A (main screen) 3D-LAYER / DISPCNT / CAPTURE fixture for the GX 2D compositor
// (gx-next-steps-log.md task 13; source/gx/gx_ds_engine_impl.inc built as Engine A).
//
// Static scenes (the 3D list is re-submitted identically every frame, so a capture at any
// frame past boot is byte-stable), one per build (-DA3_CASE=N), A/B-compared GX vs
// forced-CPU (-DDSA_FORCE_CPU). The sub engine shows a flat backdrop.
// Main-engine layers (4bpp noisy text BGs with ~35% transparent texels, like fxrom):
//   BG0 = the 3D layer (BG0CNT priority given per case, default 1)
//   BG1 = priority 3   BEHIND the 3D           BG2 = priority 2   behind the 3D
//   BG3 = priority 0   IN FRONT of the 3D      + 4 sprites, 32x32, priority 0 / 2
// 3D scene (default): a large opaque gradient triangle pair, a smaller triangle in front, and
// a clear colour with alpha 0 (holes -> the 2D layers behind show through) unless the case
// says "cover" (clear colour alpha 31 = the 3D layer covers every pixel).
//
//   0  cover: BG3 alpha-blend 1st target over 3D only (BLDCNT t2 = BG0), EVA 10 / EVB 6
//      (SM64DS's in-game configuration)                              -> engaged, native alpha
//   1  holes, no effects                                              -> engaged
//   2  holes, BG0HOFS = 37                                            -> engaged (two spans)
//   3  holes, BG0HOFS = 300 (> 256)                                   -> engaged
//   4  holes, BG0HOFS 0 -> 100 at line 96 (HBlank IRQ)                -> engaged, 2 bands
//   5  translucent polygons (alpha 12), no 2nd targets                -> engaged (drawn opaque, as the CPU does)
//   6  translucent polygons + BLDCNT 2nd targets BG1|BG2|BD           -> BAIL 3dtrans
//   7  cover, brighten (BG0 1st target, EVY 8)                        -> engaged, faded 3D
//   8  holes, brighten BG0 + 2nd target = backdrop only               -> BAIL 3dfade
//   9  holes, brighten BG0 + 2nd targets BG1|BG2|OBJ|BD (all beneath) -> engaged, 3D NOT faded
//  10  holes, WIN0 (BG0|OBJ inside, BG1|BG2|BG3 outside)              -> engaged
//  11  holes, alpha effect with BG0 as 1st target (never applied to 3D)-> engaged
//  12  fog + edge marking + antialiasing in the 3D scene               -> engaged
//  13  holes, MASTER_BRIGHT down 8                                    -> engaged
//  14  3D at priority 3, BG1 priority 3 (same tier), BG2 priority 0   -> engaged (tier order)
//  15  DISPCNT BG0_3D cleared at line 96 (BG0 is then a text BG)      -> engaged, 2 bands
//  16  holes, POWCNT DISPSWAP (main engine on the BOTTOM screen)      -> engaged, offset swapped
//  17  holes, display capture armed every frame (dest bank C)         -> BAIL capture
//  18  DISPCNT display mode 2 (VRAM framebuffer, bank C)              -> BAIL dispmode
//  19  DISPCNT display mode 0 (white)                                 -> BAIL dispmode
//  20  holes, semi-transparent sprite over 3D, 2nd targets BG0|BD     -> BAIL blendunder
//  21  cover, BG3 alpha over 3D + a semi-transparent sprite (2nd = BG0|BG3) -> engaged (both native)
//  22  holes, BG1 (lowest priority) alpha, 2nd targets BG2|BD (control: nothing but the backdrop beneath BG1) -> engaged
//  23  DISPCNT display mode 3 (FIFO)                                  -> BAIL dispmode
//  24  mosaic on BG1                                                  -> BAIL mosaic
//  25  same as case 6 (BLDCNT 2nd targets BG1|BG2|BD, priorities/config identical) but the 3D
//      list is never submitted (BG0's 3D slot stays enabled in DISPCNT/BG0CNT, fully transparent
//      every pixel) -- isolates whether BG1-3's own GX draw is correct in this exact
//      window-region/2nd-target config with the 3D layer contributing nothing (gx-next-steps-log.md
//      task 13d-blendleaklead's next lead / this task, "13d-bg13only")
//  26  same as case 6's BLDCNT/priorities but DISPCNT BG0_3D is off entirely (BG0 is a plain text
//      BG, not routed through the 3D-layer/mask3d path at all) -- the other half of the "test it
//      both ways" instruction
//  27  item 13e minimal fixture: ONE flat-coloured opaque untextured triangle (no per-vertex
//      colour variation, no quad, no second polygon), Z-buffer mode (glFlush WITHOUT
//      GL_WBUFFERING -- every other case in this file uses W-buffering). Deliberately simpler
//      than case 0: this is the true first byte-exact target for the GX geometry pass
//      (gx-next-steps-log.md task 13e / gx-13e-design.md section 3's "suggested first
//      fixture"), proving vertex placement + depth before any texturing/lighting/translucency
//      plumbing is layered on. holes (clear alpha 0), no effects -> engaged like case 1.

#include <nds.h>
#include <stdio.h>

#ifndef A3_CASE
#define A3_CASE 0
#endif

#define R16(a) (*(volatile u16 *)(a))
#define R32(a) (*(volatile u32 *)(a))
#define M_DISPCNT R32(0x04000000)
#define M_BG0CNT  R16(0x04000008)
#define M_BG0HOFS R16(0x04000010)
#define M_WIN0H   R16(0x04000040)
#define M_WIN0V   R16(0x04000044)
#define M_WININ   R16(0x04000048)
#define M_WINOUT  R16(0x0400004A)
#define M_MOSAIC  R16(0x0400004C)
#define M_BLDCNT  R16(0x04000050)
#define M_BLDALPHA R16(0x04000052)
#define M_BLDY    R16(0x04000054)
#define M_DISPCAPCNT R32(0x04000064)
#define M_MBRIGHT R16(0x0400006C)

#define L_BG0 1
#define L_BG1 2
#define L_BG2 4
#define L_BG3 8
#define L_OBJ 16
#define L_BD  32

#define VRAMA ((u8 *)0x06000000)

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

static void setup2D(int bg0Prio, int bg1Prio, int bg2Prio, int bg3Prio)
{
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankB(VRAM_B_MAIN_SPRITE);
	for (int i = 0; i < 0x20000; i++) VRAMA[i] = 0;
	for (int i = 0; i < 256; i++) {
		BG_PALETTE[i]     = RGB15((i * 5 + 9) & 31, (i * 11 + 4) & 31, (i * 7 + 17) & 31);
		SPRITE_PALETTE[i] = RGB15((i * 13 + 3) & 31, (i * 3 + 20) & 31, (i * 9 + 1) & 31);
	}
	BG_PALETTE[0] = RGB15(6, 21, 13); // backdrop
	// tiles: char blocks 0..2 (16KB each: BG3+BG0-text / BG1 / BG2), maps at screen blocks 24..27 (2KB each, 0xC000..)
	fillTiles4(VRAMA + 0x0000, 64, 1);
	fillTiles4(VRAMA + 0x4000, 64, 2);
	fillTiles4(VRAMA + 0x8000, 64, 3);
	for (int b = 0; b < 4; b++) {
		u16 *m = (u16 *)(VRAMA + 0xC000 + b * 0x800);
		for (int i = 0; i < 32 * 32; i++)
			m[i] = (u16)((hashb(i, 31 + b) % 64) | (((hashb(i, 77 + b)) & 3) << 10) | (((hashb(i, 5 + b)) & 15) << 12));
	}
	// BG0's text form (used only when a case turns the 3D bit off for part of the frame)
	M_BG0CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(27) | BG_TILE_BASE(0) | BG_PRIORITY(bg0Prio);
	REG_BG1CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(24) | BG_TILE_BASE(1) | BG_PRIORITY(bg1Prio);
	REG_BG2CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(25) | BG_TILE_BASE(2) | BG_PRIORITY(bg2Prio);
	REG_BG3CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(26) | BG_TILE_BASE(0) | BG_PRIORITY(bg3Prio);
}

static u16 *gfxA;
static void setupObjs(int semiMask)
{
	oamInit(&oamMain, SpriteMapping_1D_32, false);
	gfxA = oamAllocateGfx(&oamMain, SpriteSize_32x32, SpriteColorFormat_16Color);
	fillTiles4((u8 *)gfxA, 16, 9);
	static const int px[4] = { 8, 100, 180, 30 };
	static const int py[4] = { 10, 20, 8, 110 };
	for (int i = 0; i < 4; i++) {
		oamSet(&oamMain, i, px[i], py[i], (i & 1) ? 2 : 0, i & 7, SpriteSize_32x32, SpriteColorFormat_16Color, gfxA, -1, false, false, false, false, false);
		if (semiMask & (1 << i)) oamMain.oamMemory[i].blendMode = OBJMODE_BLENDED;
	}
	for (int i = 4; i < 128; i++) oamMain.oamMemory[i].isHidden = true;
}

// --- the 3D scene ---
static int polyAlpha = 31;
static void draw3D(void)
{
	glResetMatrixStack();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(70, 256.0 / 192.0, 0.1, 40);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -2.4f);
	glRotatef(27.0f, 0.0f, 1.0f, 0.0f);

	glMaterialf(GL_AMBIENT,  RGB15(31, 31, 31));
	glMaterialf(GL_DIFFUSE,  RGB15(31, 31, 31));
	glMaterialf(GL_SPECULAR, RGB15(0, 0, 0));
	glMaterialf(GL_EMISSION, RGB15(0, 0, 0));

#if A3_CASE == 27
	// minimal fixture: one flat opaque untextured triangle, same colour on all three
	// vertices (no Gouraud), Z-buffer depth mode (see glFlush below).
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
	glBegin(GL_TRIANGLES);
		glColor3b(200, 80, 40); glVertex3f(-1.0f,  1.0f, 0.0f);
		glColor3b(200, 80, 40); glVertex3f(-1.0f, -1.0f, 0.0f);
		glColor3b(200, 80, 40); glVertex3f( 1.0f, -1.0f, 0.0f);
	glEnd();
	glFlush(GL_TRANS_MANUALSORT);   // no GL_WBUFFERING -- Z-buffer mode
	return;
#endif
#if A3_CASE != 25
	// big gradient quad (covers most of the screen), polygon id 1
		glPolyFmt(POLY_ALPHA(polyAlpha) | POLY_CULL_NONE | POLY_ID(1));
	glBegin(GL_TRIANGLES);
		glColor3b(255,   0,   0); glVertex3f(-2.0f,  1.4f, 0.0f);
		glColor3b(  0, 255,   0); glVertex3f(-2.0f, -1.4f, 0.0f);
		glColor3b(  0,   0, 255); glVertex3f( 2.0f, -1.4f, 0.0f);

		glColor3b(  0,   0, 255); glVertex3f( 2.0f, -1.4f, 0.0f);
		glColor3b(255, 255, 255); glVertex3f( 2.0f,  1.4f, 0.0f);
		glColor3b(255,   0,   0); glVertex3f(-2.0f,  1.4f, 0.0f);
	glEnd();
	// a smaller triangle in front, polygon id 2, its own alpha
	glPolyFmt(POLY_ALPHA(polyAlpha) | POLY_CULL_NONE | POLY_ID(2));
	glBegin(GL_TRIANGLES);
		glColor3b(255, 255,   0); glVertex3f(-1.4f,  1.1f, 0.4f);
		glColor3b(  0, 255, 255); glVertex3f(-1.4f, -1.7f, 0.4f);
		glColor3b(255,   0, 255); glVertex3f( 1.7f, -0.6f, 0.4f);
	glEnd();
#endif
	// case 25: matrices/material state set up identically above, but zero triangles submitted --
	// BG0's 3D slot is enabled in DISPCNT/BG0CNT and takes part in the frame exactly like every
	// other case, it just never has any content (equivalent to an all-holes, alpha=0 3D layer).
	glFlush(GL_TRANS_MANUALSORT | GL_WBUFFERING);
}

static int f;
#if A3_CASE == 4
static void hb(void) { if (REG_VCOUNT == 95) M_BG0HOFS = 100; else if (REG_VCOUNT == 261 || REG_VCOUNT == 262) M_BG0HOFS = 0; }
#endif
#if A3_CASE == 15
static void hb(void)
{
	if (REG_VCOUNT == 95) M_DISPCNT &= ~(1u << 3);
	else if (REG_VCOUNT == 261 || REG_VCOUNT == 262) M_DISPCNT |= (1u << 3);
}
#endif

int main(void)
{
	irqEnable(IRQ_VBLANK);
	videoSetModeSub(MODE_0_2D);
	BG_PALETTE_SUB[0] = RGB15(0, 0, 0);

	int bg0Prio = 1, bg1Prio = 3, bg2Prio = 2, bg3Prio = 0;
#if A3_CASE == 14
	bg0Prio = 3; bg1Prio = 3; bg2Prio = 0; bg3Prio = 1;
#endif
	setup2D(bg0Prio, bg1Prio, bg2Prio, bg3Prio);

	int cover = 0, semi = 0;
	u32 dcnt = MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | ENABLE_3D;
	int fog = 0;

#if A3_CASE == 0
	cover = 1;
	M_BLDCNT = (1 << 6) | L_BG3 | (L_BG0 << 8); M_BLDALPHA = 10 | (6 << 8);
#elif A3_CASE == 1 || A3_CASE == 16 || A3_CASE == 17
	;
#elif A3_CASE == 2
	M_BG0HOFS = 37;
#elif A3_CASE == 3
	M_BG0HOFS = 300;
#elif A3_CASE == 4
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#elif A3_CASE == 5
	polyAlpha = 12;
#elif A3_CASE == 6
	polyAlpha = 12;
	M_BLDCNT = ((L_BG1 | L_BG2 | L_BD) << 8);
#elif A3_CASE == 7
	cover = 1;
	M_BLDCNT = (2 << 6) | L_BG0; M_BLDY = 8;
#elif A3_CASE == 8
	M_BLDCNT = (2 << 6) | L_BG0 | (L_BD << 8); M_BLDY = 8;
#elif A3_CASE == 9
	M_BLDCNT = (2 << 6) | L_BG0 | ((L_BG1 | L_BG2 | L_OBJ | L_BD) << 8); M_BLDY = 8;
#elif A3_CASE == 10
	dcnt |= (1u << 13);
	M_WIN0H = (40 << 8) | 200; M_WIN0V = (30 << 8) | 150;
	M_WININ = (L_BG0 | L_OBJ); M_WINOUT = (L_BG1 | L_BG2 | L_BG3);
#elif A3_CASE == 11
	M_BLDCNT = (1 << 6) | L_BG0 | ((L_BG1 | L_BD) << 8); M_BLDALPHA = 8 | (8 << 8);
#elif A3_CASE == 12
	fog = 1;
#elif A3_CASE == 13
	M_MBRIGHT = (2u << 14) | 8;
#elif A3_CASE == 14
	;
#elif A3_CASE == 15
	irqSet(IRQ_HBLANK, hb); irqEnable(IRQ_HBLANK);
#elif A3_CASE == 18
	;
#elif A3_CASE == 19
	;
#elif A3_CASE == 20
	semi = 0x0F;
	M_BLDCNT = ((L_BG0 | L_BD) << 8); M_BLDALPHA = 12 | (4 << 8);
#elif A3_CASE == 21
	cover = 1; semi = 0x01;
	M_BLDCNT = (1 << 6) | L_BG3 | ((L_BG0 | L_BG3) << 8); M_BLDALPHA = 10 | (6 << 8);
#elif A3_CASE == 22
	M_BLDCNT = (1 << 6) | L_BG1 | ((L_BG2 | L_BD) << 8); M_BLDALPHA = 10 | (6 << 8);
#elif A3_CASE == 24
	REG_BG1CNT |= BG_MOSAIC_ON; M_MOSAIC = 0x0033;
#elif A3_CASE == 25
	// identical BLDCNT/priority config to case 6, but draw3D() (below) submits zero triangles --
	// BG0's 3D slot stays enabled and takes part in mask3dCfg exactly as case 6 does.
	M_BLDCNT = ((L_BG1 | L_BG2 | L_BD) << 8);
#elif A3_CASE == 26
	// identical BLDCNT/priority config to case 6, but ENABLE_3D is left out of dcnt below -- BG0
	// is a plain text BG (its text-form BG0CNT from setup2D), never routed through the 3D-layer/
	// mask3d path at all.
	M_BLDCNT = ((L_BG1 | L_BG2 | L_BD) << 8);
	dcnt &= ~ENABLE_3D;
#elif A3_CASE == 27
	// Isolate the 3D layer completely: no other BG/OBJ visible, so a GX geometry-pass test
	// draw that composites on top of Engine A's normal 2D bands (rather than going through
	// the full 3D-layer/priority/hole compositing contract those other cases exercise) is
	// still position-correct with nothing else able to occlude it. Backdrop only + BG0/3D.
	dcnt &= ~(DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE);
#endif

	setupObjs(semi);
	videoSetMode(dcnt);
	M_BG0CNT = (M_BG0CNT & ~3) | bg0Prio;   // (videoSetMode does not touch BGxCNT; kept explicit)

	glInit();
#if A3_CASE == 12
	glEnable(GL_ANTIALIAS | GL_OUTLINE | GL_FOG);
	glSetOutlineColor(0, RGB15(31, 31, 0));
	glSetOutlineColor(1, RGB15(0, 31, 31));
	glFogColor(4, 8, 20, 12);
	glFogShift(3);
	glFogOffset(0x2000);
	for (int i = 0; i < 32; i++) glFogDensity(i, i * 4);
#else
	glEnable(GL_ANTIALIAS);
#endif
	(void)fog;
	if (cover) glClearColor(6, 10, 18, 31); else glClearColor(0, 0, 0, 0);
	glClearPolyID(63);
	glClearDepth(0x7FFF);
	glViewport(0, 0, 255, 191);

#if A3_CASE == 16
	lcdMainOnBottom();
#endif
#if A3_CASE == 18
	// display mode 2: bank C as a framebuffer full of noise
	vramSetBankC(VRAM_C_LCD);
	for (int i = 0; i < 128 * 1024 / 2; i++) ((u16 *)0x06840000)[i] = (u16)(hashb(i, 1) | (hashb(i, 2) << 8)) & 0x7FFF;
	M_DISPCNT = (M_DISPCNT & ~(3u << 16)) | (2u << 16);
	M_DISPCNT = (M_DISPCNT & ~(3u << 18)) | (2u << 18);   // VRAM_Block = 2 (bank C)
#endif
#if A3_CASE == 19
	M_DISPCNT = (M_DISPCNT & ~(3u << 16)) | (0u << 16);
#endif
#if A3_CASE == 23
	M_DISPCNT = (M_DISPCNT & ~(3u << 16)) | (3u << 16);
#endif
#if A3_CASE == 17
	vramSetBankC(VRAM_C_LCD);
#endif

	printf("");
	for (;;) {
		draw3D();
#if A3_CASE == 17
		// writeBlock 2 (C), 256x192, source A = the composited main screen, EVA 16; re-armed every frame
		M_DISPCAPCNT = (1u << 31) | (0u << 29) | (3u << 20) | (0u << 18) | (2u << 16) | 16;
#endif
		swiWaitForVBlank();
		oamUpdate(&oamMain);
		f++;
	}
	return 0;
}
