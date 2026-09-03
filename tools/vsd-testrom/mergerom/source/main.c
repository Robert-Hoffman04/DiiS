// SPDX-License-Identifier: CC0-1.0
//
// GX-merge two-bucket functional test ROM for desmumewii.
//
// Unlike the Volumetric Shadow Demo (which drives the main screen in framebuffer
// display mode + display capture, so every frame falls back to the legacy
// readback path), this ROM keeps the main engine in a plain tiled BG mode with
// the 3D layer on BG0 and NO display capture / master brightness / windows - the
// conditions under which GXMerge_FrameMergeable() arms the 3-draw sandwich.
//
// Layout (main engine, MODE_0_2D + ENABLE_3D):
//
//   BG0  = 3D          priority 1   spinning vertex-coloured quad; the 3D clear
//                                   colour is alpha 0 so wherever the quad is not
//                                   covering, the layers below show through.
//   BG1  = checker      priority 3  BEHIND the 3D - revealed wherever the 3D
//                                   layer is transparent (coverage-mask test).
//   BG2  = top band     priority 0  IN FRONT of the 3D - a solid strip across the
//                                   top 32 px that must sit over the 3D quad.
//   OBJ0 = red square    priority 0 IN FRONT of the 3D (front bucket).
//   OBJ1 = blue square   priority 2 BEHIND the 3D, over the checker (behind bucket).
//
// MT_FRONTBLEND rewires the mixing registers to exercise the Phase-2
// front-bucket "blend against beneath" path (GXMerge frontAlphaOver): BG2
// becomes an alpha-blend 1st target and BG0/BG1/OBJ/backdrop all become 2nd
// targets, so the DS blends the BG2 band 50/50 over whatever is beneath it
// (3D quad / checker / backdrop) - which is exactly what the GXMerge whole-band
// front blend reproduces.  OBJ0 is moved behind the 3D in this build because the
// frontAlphaOver path requires an empty front sprite bucket.
//
// Status: renders correctly under the software rasterizer (ds_sw).  desmumewii's
// GX 3D core currently stalls on this ROM (a plain BG0=3D scene with no display
// capture) - see tools/vsd-testrom/README.md; that is a pre-existing GX-core
// issue, not something the GX-merge redesign introduced, and it blocks GX-core
// functional testing of the sandwich until fixed.
//
// Build defines:
//   MT_DETERMINISTIC  - freeze the spin after MT_WARMUP frames and stop the
//                       per-frame console counter, so screenshots are byte-stable
//                       for A/B (merge ON vs OFF) comparison.  Still drivable.
//   MT_FREEZE         - also stop reading input after warmup.
//   MT_WARMUP         - warmup frame count (default 8).
//   MT_REGDUMP        - print the main-engine mixing registers to the sub screen
//                       (DISPCNT/BLDCNT/windows/capture) for debugging arming.
//   MT_FRONTBLEND     - alpha-blend BG2 (front) against BG0/BG1/OBJ/backdrop
//                       (all 2nd targets), EVA=EVB=8.  Exercises the GXMerge
//                       frontAlphaOver path; also moves OBJ0 behind the 3D.
//   MT_MASTERBRIGHT   - drive MASTER_BRIGHT (bright-down / fade-to-black,
//                       factor 8) over the whole main screen.  Exercises the
//                       Phase-3 per-band GX brightness pass (GXMerge draw 4):
//                       merge ON should match the software rasterizer's faded
//                       output within RGB8-vs-RGB555 precision.
//   MT_MB_SPLIT       - (with MT_MASTERBRIGHT) additionally rewrite the factor
//                       at scanline 96 via an HBlank IRQ (top half factor 8,
//                       bottom half factor 14), forcing a 2-band split so the
//                       per-band brightness path is exercised, not just one band.

#include <nds.h>
#include <stdio.h>

#ifndef MT_WARMUP
#define MT_WARMUP 8
#endif

#ifdef MT_FRONTBLEND
#define OBJ0_PRIO 2   // red sprite behind the 3D: frontAlphaOver needs an empty
                      // front sprite bucket (no sprite pixel with prio <= P3d)
#else
#define OBJ0_PRIO 0
#endif

static int bg1, bg2;
static u16 *objRedGfx, *objBlueGfx;

#ifdef MT_MASTERBRIGHT
// MASTER_BRIGHT: bits 0-4 = factor (0..16), bits 14-15 = mode (1 up, 2 down).
#define MB_DOWN(f)   ((2u << 14) | ((f) & 0x1F))
#define MB_TOP_F     8
#ifdef MT_MB_SPLIT
#define MB_BOTTOM_F  14
// Rewrite the factor mid-frame so the merge path has to split the screen into
// two brightness bands (top: MB_TOP_F, bottom from scanline 96: MB_BOTTOM_F),
// then restore the top value during vblank for the next frame's line 0.
static void hblankMB(void)
{
	int vc = REG_VCOUNT;
	if (vc == 95)       REG_MASTER_BRIGHT = MB_DOWN(MB_BOTTOM_F);
	else if (vc == 262) REG_MASTER_BRIGHT = MB_DOWN(MB_TOP_F);
}
#endif
#endif

static void setupBackgrounds(void)
{
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankB(VRAM_B_MAIN_SPRITE);

	bg1 = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 0, 1);
	bg2 = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 2, 1);
	bgSetPriority(bg1, 3);   // behind the 3D and behind the blue sprite
	bgSetPriority(bg2, 0);   // in front of the 3D

	// Shared 4bpp tile set: tile 0 blank, tiles 1/2/3 solid palette index 1/2/3.
	u16 *tiles = (u16 *)bgGetGfxPtr(bg1);
	for (int i = 0; i < 4 * 16; i++) tiles[i] = 0x0000;
	for (int i = 0; i < 16; i++) {
		tiles[1 * 16 + i] = 0x1111;
		tiles[2 * 16 + i] = 0x2222;
		tiles[3 * 16 + i] = 0x3333;
	}

	BG_PALETTE[1] = RGB15(4, 6, 20);
	BG_PALETTE[2] = RGB15(6, 18, 6);
	BG_PALETTE[3] = RGB15(28, 24, 10);

	u16 *m1 = (u16 *)bgGetMapPtr(bg1);
	u16 *m2 = (u16 *)bgGetMapPtr(bg2);
	for (int ty = 0; ty < 32; ty++)
		for (int tx = 0; tx < 32; tx++) {
			m1[ty * 32 + tx] = ((tx ^ ty) & 1) ? 1 : 2;
			m2[ty * 32 + tx] = (ty < 4) ? 3 : 0;
		}
}

static void fillSprite(u16 *gfx, int palIdx)
{
	u16 v = (palIdx << 12) | (palIdx << 8) | (palIdx << 4) | palIdx;
	for (int i = 0; i < 64; i++) gfx[i] = v;   // 16x16 4bpp = 128 bytes
}

static void setupSprites(void)
{
	oamInit(&oamMain, SpriteMapping_1D_32, false);
	objRedGfx  = oamAllocateGfx(&oamMain, SpriteSize_16x16, SpriteColorFormat_16Color);
	objBlueGfx = oamAllocateGfx(&oamMain, SpriteSize_16x16, SpriteColorFormat_16Color);
	fillSprite(objRedGfx, 1);
	fillSprite(objBlueGfx, 1);
	SPRITE_PALETTE[0x01] = RGB15(31, 4, 4);   // pal 0 -> red  (obj 0)
	SPRITE_PALETTE[0x11] = RGB15(4, 4, 31);   // pal 1 -> blue (obj 1)
}

static void setup3D(void)
{
	// bgInit above left the main engine in a 2D mode; re-assert it with 3D on BG0.
	videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE |
	             DISPLAY_BG2_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | ENABLE_3D);
	REG_BG0CNT = BG_PRIORITY(1);

	glInit();
	glEnable(GL_ANTIALIAS);
	glClearColor(0, 0, 0, 0);      // alpha 0 -> transparent 3D backdrop
	glClearPolyID(63);
	glClearDepth(0x7FFF);
	glViewport(0, 0, 255, 191);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(70, 256.0 / 192.0, 0.1, 40);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

#ifdef MT_FRONTBLEND
	// BG2 (front, prio 0) alpha-blends as 1st target; BG0/BG1/OBJ/backdrop are
	// all 2nd targets so BG2 blends against whatever is beneath it everywhere in
	// its band.  EVA = EVB = 8  ->  exact 50/50 (no EVB = 16-EVA approximation).
	REG_BLDCNT   = BLEND_ALPHA | BLEND_SRC_BG2
	             | BLEND_DST_BG0 | BLEND_DST_BG1
	             | BLEND_DST_SPRITE | BLEND_DST_BACKDROP;
	REG_BLDALPHA = (8 << 0) | (8 << 8);
	REG_BLDY = 0;
	REG_MASTER_BRIGHT = 0;
#elif defined(MT_MASTERBRIGHT)
	// No blend; a whole-screen bright-down fade instead (Phase-3 GX pass test).
	REG_BLDCNT = 0;
	REG_BLDALPHA = 0;
	REG_BLDY = 0;
	REG_MASTER_BRIGHT = MB_DOWN(MB_TOP_F);
#ifdef MT_MB_SPLIT
	irqSet(IRQ_HBLANK, hblankMB);
	irqEnable(IRQ_HBLANK);
#endif
#else
	// No colour-special-effects: the Phase-1 sandwich is opaque + alpha-key only,
	// so the 3D layer must not be a blend / brightness target.
	REG_BLDCNT = 0;
	REG_BLDALPHA = 0;
	REG_BLDY = 0;
	REG_MASTER_BRIGHT = 0;
#endif
}

static void draw3D(int angle)
{
	glResetMatrixStack();

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(70, 256.0 / 192.0, 0.1, 40);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -2.4f);
	glRotatef((float)angle, 0.0f, 1.0f, 0.0f);

	glMaterialf(GL_AMBIENT,  RGB15(31, 31, 31));
	glMaterialf(GL_DIFFUSE,  RGB15(31, 31, 31));
	glMaterialf(GL_SPECULAR, RGB15(0, 0, 0));
	glMaterialf(GL_EMISSION, RGB15(0, 0, 0));

	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));

	glBegin(GL_TRIANGLES);
		glColor3b(255,   0,   0); glVertex3f(-1.0f,  1.0f, 0.0f);
		glColor3b(  0, 255,   0); glVertex3f(-1.0f, -1.0f, 0.0f);
		glColor3b(  0,   0, 255); glVertex3f( 1.0f, -1.0f, 0.0f);

		glColor3b(  0,   0, 255); glVertex3f( 1.0f, -1.0f, 0.0f);
		glColor3b(255, 255, 255); glVertex3f( 1.0f,  1.0f, 0.0f);
		glColor3b(255,   0,   0); glVertex3f(-1.0f,  1.0f, 0.0f);
	glEnd();

	// glFlush(0) leaves the 3D layer blank on both cores; manual-sort is needed.
	glFlush(GL_TRANS_MANUALSORT | GL_WBUFFERING);
}

int main(void)
{
	irqEnable(IRQ_VBLANK);
	consoleDemoInit();   // sub engine console (VRAM C)

	setupBackgrounds();
	setupSprites();
	setup3D();

	printf("\x1b[2J");
	printf("desmumewii GX-merge test\n-----------------------\n");
	printf("BG1 checker : behind 3D\n");
	printf("BG2 band    : front of 3D\n");
	printf("red  sprite : front of 3D\n");
	printf("blue sprite : behind 3D\n\n");
	printf("dpad L/R : spin the quad\n");
	printf("GC dpad-down toggles merge\n");
#ifdef MT_DETERMINISTIC
	printf("\n[deterministic build]\n");
#endif
#ifdef MT_FRONTBLEND
	printf("\n[frontblend: BG2 alpha-blend\n over 3D/checker/backdrop]\n");
#endif
#ifdef MT_MASTERBRIGHT
#ifdef MT_MB_SPLIT
	printf("\n[masterbright: fade-to-black\n top f8 / bottom f14 @ line 96]\n");
#else
	printf("\n[masterbright: fade-to-black\n factor 8, whole screen]\n");
#endif
#endif

	int angle = 25;
	int frame = 0;

	while (1)
	{
#ifdef MT_FREEZE
		if (frame < MT_WARMUP)
#endif
		{
			scanKeys();
			int k = keysHeld();
			if (k & KEY_LEFT)  angle -= 3;
			if (k & KEY_RIGHT) angle += 3;
		}

#ifdef MT_DETERMINISTIC
		if (frame < MT_WARMUP) angle++;
#else
		angle++;
#endif

		draw3D(angle);

		oamSet(&oamMain, 0,  40,  60, OBJ0_PRIO, 0, SpriteSize_16x16, SpriteColorFormat_16Color,
		       objRedGfx,  -1, false, false, false, false, false);
		oamSet(&oamMain, 1, 190, 110, 2, 1, SpriteSize_16x16, SpriteColorFormat_16Color,
		       objBlueGfx, -1, false, false, false, false, false);

		swiWaitForVBlank();
		oamUpdate(&oamMain);

#ifndef MT_DETERMINISTIC
		printf("\x1b[17;0Hframe %d  angle %d   ", frame, angle);
#else
		if (frame == MT_WARMUP)
			printf("\x1b[17;0HMT READY (deterministic)");
#endif
#ifdef MT_REGDUMP
		if (frame == MT_WARMUP) {
			printf("\x1b[19;0HDISPCNT=%08lX", (unsigned long)REG_DISPCNT);
			printf("\x1b[20;0HBLDCNT=%04X MBRIGHT=%04X", REG_BLDCNT, REG_MASTER_BRIGHT);
			printf("\x1b[21;0HWIN0H=%04X WININ=%04X", REG_WIN0H, REG_WININ);
			printf("\x1b[22;0HBG0CNT=%04X DISPCAP=%08lX",
			       REG_BG0CNT, (unsigned long)REG_DISPCAPCNT);
		}
#endif
		if (frame <= MT_WARMUP) frame++;
	}

	return 0;
}
