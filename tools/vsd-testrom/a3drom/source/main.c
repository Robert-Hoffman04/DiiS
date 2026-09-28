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
//  28  item 13e-precision fixture (Task 13e-precision, gx-next-steps-log.md): FOUR flat-coloured
//      opaque untextured triangles, Z-buffer mode, an extra X-axis rotation stacked on the usual
//      27-degree Y rotation (genuinely off-axis, not aligned to either screen axis), spanning a
//      wide depth range (near/steeply-oblique, far/small, and a long thin sliver placed close to
//      the left frustum edge) -- built specifically to stress GxFast's real-GX-matrix-hardware
//      transform against the CPU fixed-point reference, since case 27's single small axis-aligned
//      triangle was proven too simple to expose any drift. Also exercises multi-polygon submission
//      (item 13e-slice's known gap: only ever tested with N=1). holes (clear alpha 0), no effects.
//  29  item 13f fog fixture (see the case body).
//  30  textured polygons (gx-remaining-work.md section 1): four textured quads + one textured
//      triangle, every opaque format (I4 colour-0 transparent, direct 16bpp with alpha-bit holes,
//      4x4 compressed in all four modes, I8, I2), clamp / repeat / repeat+flip / flip-only,
//      white vertex colour, Z-buffer mode, perspective tilt, isolated like 27/28.
//  31  case 30 with arbitrary per-vertex (Gouraud) colours: modulate precision (GxFast only).
//  32  case 28 in W-buffer mode (GL_WBUFFERING): intersecting untextured geometry, W depth.
//  33  case 30 in W-buffer mode: textured + intersecting, W depth (GxFast; GxAccurate bails).
//  34  translucent polygons (gx-remaining-work.md section 1): cover (clear alpha 31), blending +
//      alpha test (ref 5) on, one opaque Gouraud quad submitted between translucent ones (sorting),
//      untextured translucent triangles with depth-write (POLYGON_ATTR bit 11) on and off crossing
//      each other and the opaque quad in depth, an A3I5 quad of alpha 31 without depth write
//      (opaque texels write depth, translucent ones don't), an A5I3 quad of alpha 20 with depth
//      write, a direct-colour quad of alpha 16; all translucent IDs distinct. Z-buffer, manual sort.
//  35  case 34 in W-buffer mode.
//  36  case 34 plus two overlapping translucent quads with the SAME polygon ID (the second is
//      dropped where the first drew): GxFast bails `transid`.
//  37  diagnostic: case 34's opaque quad alone with four different vertex colours (a Gouraud
//      quad; the DS interpolates quads natively, GX splits them into two triangles).
//  38  clipped polygons (gx-remaining-work.md section 1): identity modelview, a textured floor
//      quad (direct, repeat) from behind the eye (w < 0) to beyond the far plane, also crossing
//      the left and bottom planes; a textured 4x4 quad (clamp) crossing the near, right and top
//      planes; a large flat untextured triangle crossing far, right, top and bottom, behind the
//      near floor and in front of the far floor (intersects it in depth). White/flat colours (GxAccurate's scope). Z-buffer.
//  39  case 38 in W-buffer mode.
//  40  case 38 with Gouraud colours on every vertex (GxFast: clipper-interpolated colours).
//  41-45  cases 1, 0, 11, 10, 2 in Z-buffer mode (3D below BG3/sprites: the GX pass as a texture).

#include <nds.h>
#include <stdio.h>

#ifndef A3_CASE
#define A3_CASE 0
#endif
// 32/33: cases 28/30 with GL_WBUFFERING (gx-remaining-work.md section 1, W-buffer depth mode).
#define A3_WBUF 0
#if A3_CASE == 35
#undef A3_WBUF
#define A3_WBUF GL_WBUFFERING
#undef A3_CASE
#define A3_CASE 34
#endif
#define A3_SAMEID 0
#define A3_QUADPROBE 0
#if A3_CASE == 37
#undef A3_QUADPROBE
#define A3_QUADPROBE 1
#undef A3_CASE
#define A3_CASE 34
#endif
#if A3_CASE == 36
#undef A3_SAMEID
#define A3_SAMEID 1
#undef A3_CASE
#define A3_CASE 34
#endif
#define A3_CLIPGOURAUD 0
#if A3_CASE == 39 || A3_CASE == 40
#if A3_CASE == 39
#undef A3_WBUF
#define A3_WBUF GL_WBUFFERING
#else
#undef A3_CLIPGOURAUD
#define A3_CLIPGOURAUD 1
#endif
#undef A3_CASE
#define A3_CASE 38
#endif
#if A3_CASE == 32 || A3_CASE == 33
#undef A3_WBUF
#define A3_WBUF GL_WBUFFERING
#if A3_CASE == 32
#undef A3_CASE
#define A3_CASE 28
#else
#undef A3_CASE
#define A3_CASE 30
#endif
#endif

// 41-45: cases 1/0/11/10/2 in Z-buffer mode (gx-remaining-work.md section 1, "3D at its real
// priority": BG3 and sprites sit above BG0/3D, so the GX pass is drawn to a texture; Z-buffer so
// GxAccurate takes them too). 41 holes; 42 cover + BG3 alpha over 3D; 43 3D as alpha 1st target
// over BG1/backdrop; 44 WIN0; 45 BG0HOFS 37.
#define A3_ZBUF 0
#if A3_CASE >= 41 && A3_CASE <= 45
#undef A3_ZBUF
#define A3_ZBUF 1
#if A3_CASE == 41
#undef A3_CASE
#define A3_CASE 1
#elif A3_CASE == 42
#undef A3_CASE
#define A3_CASE 0
#elif A3_CASE == 43
#undef A3_CASE
#define A3_CASE 11
#elif A3_CASE == 44
#undef A3_CASE
#define A3_CASE 10
#else
#undef A3_CASE
#define A3_CASE 2
#endif
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

#if A3_CASE == 30 || A3_CASE == 31 || A3_CASE == 38
// Textured-polygon fixture (gx-remaining-work.md section 1, "Textured polygons"): every
// opaque texture format, noisy texels so any texel mis-selection shows, colour-0 and
// alpha-bit holes, clamp / repeat / flip, Z-buffer mode with one overlapping triangle.
// Textures are written straight to LCDC VRAM and selected with raw TEXIMAGE_PARAM /
// PLTT_BASE writes (libnds's allocator left every texture blank on DeSmuME).
// Bank C = texture slot 0, bank D = slot 1 (4x4 index data), bank E = palettes.
#define TEXP(addr, sz, tz, fmt) ((u32)((addr) >> 3) | ((u32)(sz) << 20) | ((u32)(tz) << 23) | ((u32)(fmt) << 26))
#define T_RS (1u << 16)
#define T_RT (1u << 17)
#define T_FS (1u << 18)
#define T_FT (1u << 19)
#define T_C0 (1u << 29)
static const u32 texI4   = TEXP(0x1000, 2, 2, 3) | T_C0;                        // 32x32 I4, colour 0 transparent, clamp
static const u32 texRGBA = TEXP(0x2000, 1, 1, 7) | T_RS | T_RT;                 // 16x16 direct, alpha-bit holes, repeat
static const u32 tex4x4  = TEXP(0x0000, 2, 2, 5);                               // 32x32 4x4-compressed, all 4 modes, clamp
static const u32 texI8   = TEXP(0x3000, 1, 2, 4) | T_RS | T_RT | T_FS | T_FT;   // 16x32 I8, repeat + flip
static const u32 texI2   = TEXP(0x4000, 0, 0, 2) | T_RS | T_FT;                 // 8x8 I2, repeat S, flip-only T (= clamp)
static const u32 palMain = 0x0000 >> 4, palI2 = 0x1000 >> 3;

static void texSetup(void)
{
	vramSetBankC(VRAM_C_LCD);
	vramSetBankD(VRAM_D_LCD);
	vramSetBankE(VRAM_E_LCD);
	u8 *c = (u8 *)VRAM_C, *d = (u8 *)VRAM_D;
	u16 *e = (u16 *)VRAM_E;
	for (int i = 0; i < 256; i++) e[i] = RGB15(hashb(i, 3) & 31, hashb(i, 4) & 31, hashb(i, 5) & 31);
	for (int i = 0; i < 4; i++) e[0x800 + i] = RGB15(hashb(i, 6) & 31, hashb(i, 7) & 31, hashb(i, 8) & 31);
	for (int i = 0; i < 64 * 4; i++) c[i] = hashb(i, 31);                                   // 4x4 texels
	for (int i = 0; i < 64; i++) ((u16 *)d)[i] = (u16)(((i & 3) << 14) | ((i * 3) & 0x7F));  // mode, pal offset
	for (int i = 0; i < 32 * 32 / 2; i++) c[0x1000 + i] = hashb(i, 11);
	for (int i = 0; i < 16 * 16; i++)
		((u16 *)(c + 0x2000))[i] = (u16)((hashb(i, 21) | (hashb(i, 22) << 8)) & 0x7FFF) | ((hashb(i, 23) & 3) ? 0x8000 : 0);
	for (int i = 0; i < 16 * 32; i++) c[0x3000 + i] = hashb(i, 41);
	for (int i = 0; i < 8 * 8 / 4; i++) c[0x4000 + i] = hashb(i, 51);
	vramSetBankC(VRAM_C_TEXTURE_SLOT0);
	vramSetBankD(VRAM_D_TEXTURE_SLOT1);
	vramSetBankE(VRAM_E_TEX_PALETTE);
}

static int vcol;
static void tcol(void)
{
#if A3_CASE == 31 || A3_CLIPGOURAUD
	++vcol;
	glColor3b(hashb(vcol, 61), hashb(vcol, 62), hashb(vcol, 63));   // Gouraud, arbitrary colours
#else
	glColor3b(255, 255, 255);
#endif
}

static void texQuad(u32 tex, float x0, float y0, float x1, float y1, float z, int s0, int t0, int s1, int t1)
{
	GFX_TEX_FORMAT = tex;
	GFX_PAL_FORMAT = palMain;
	glBegin(GL_QUADS);
		tcol(); glTexCoord2t16(inttot16(s0), inttot16(t0)); glVertex3f(x0, y1, z);
		tcol(); glTexCoord2t16(inttot16(s0), inttot16(t1)); glVertex3f(x0, y0, z);
		tcol(); glTexCoord2t16(inttot16(s1), inttot16(t1)); glVertex3f(x1, y0, z);
		tcol(); glTexCoord2t16(inttot16(s1), inttot16(t0)); glVertex3f(x1, y1, z);
	glEnd();
}

static void texScene(void)
{
	vcol = 0;
	glRotatef(10.0f, 1.0f, 0.0f, 0.0f);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
	texQuad(texI4,   -1.5f,  0.05f, -0.05f, 1.0f, 0.0f,   0,   0, 32, 32);   // clamp, 1 texel ~ 1.5 px
	texQuad(texRGBA,  0.05f, 0.05f,  1.5f,  1.0f, 0.0f,   0,   0, 40, 28);   // repeat
	texQuad(tex4x4,  -1.5f, -1.0f,  -0.05f, -0.05f, 0.0f, -3,  -2, 35, 36);  // clamp beyond both edges
	texQuad(texI8,    0.05f, -1.0f,  1.5f, -0.05f, 0.0f, -16, -8, 40, 56);  // repeat + mirror
	// in front, crossing all four quads' depth (tilted in z): depth test between textured polys
	GFX_TEX_FORMAT = texI2;
	GFX_PAL_FORMAT = palI2;
	glBegin(GL_TRIANGLES);
		tcol(); glTexCoord2t16(inttot16(0),  inttot16(-4)); glVertex3f(-0.6f,  0.6f, 0.3f);
		tcol(); glTexCoord2t16(inttot16(0),  inttot16(12)); glVertex3f(-0.6f, -0.6f, -0.3f);
		tcol(); glTexCoord2t16(inttot16(29), inttot16(4));  glVertex3f( 0.8f,  0.0f, 0.3f);
	glEnd();
	glFlush(GL_TRANS_MANUALSORT | A3_WBUF);   // Z-buffer mode (W-buffer for case 33)
}

#define CV(x, y, z) glVertex3f((x) / 8.0f, (y) / 8.0f, (z) / 8.0f)
// Clipped-polygon fixture (case 38-40, see the header). Eye space: identity modelview under
// gluPerspective(70, 4:3, 0.1, 40), so near is z = -0.1, far z = -40, and |x| <= 0.93|z|,
// |y| <= 0.70|z| is inside.
static void clipScene(void)
{
	vcol = 0;
	glLoadIdentity();
	glScalef(8.0f, 8.0f, 8.0f);   // vertices are 4.12 (|v| < 8): CV() divides by 8
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
	// floor: behind the eye (z = +0.3, w < 0) to beyond far (z = -45); left and bottom planes
	GFX_TEX_FORMAT = texRGBA;
	GFX_PAL_FORMAT = palMain;
	glBegin(GL_QUADS);
		tcol(); glTexCoord2t16(inttot16(0),  inttot16(0));   CV(-2.5f, -0.6f,  0.3f);
		tcol(); glTexCoord2t16(inttot16(48), inttot16(0));   CV( 0.6f, -0.6f,  0.3f);
		tcol(); glTexCoord2t16(inttot16(48), inttot16(720)); CV( 0.6f, -0.6f, -45.0f);
		tcol(); glTexCoord2t16(inttot16(0),  inttot16(720)); CV(-2.5f, -0.6f, -45.0f);
	glEnd();
	// wall: crosses the near plane (z = -0.05), the right and top planes; clamp beyond edges
	GFX_TEX_FORMAT = tex4x4;
	glBegin(GL_QUADS);
		tcol(); glTexCoord2t16(inttot16(-3), inttot16(36)); CV(0.2f, -0.2f, -2.0f);
		tcol(); glTexCoord2t16(inttot16(-3), inttot16(-2)); CV(0.2f,  1.5f, -2.0f);
		tcol(); glTexCoord2t16(inttot16(35), inttot16(-2)); CV(2.0f,  1.5f, -0.05f);
		tcol(); glTexCoord2t16(inttot16(35), inttot16(36)); CV(2.0f, -0.2f, -0.05f);
	glEnd();
	// big untextured triangle: beyond far (top-left), outside right + top, outside bottom
	GFX_TEX_FORMAT = 0;
	glBegin(GL_TRIANGLES);
#if A3_CLIPGOURAUD
		glColor3b(240, 60, 30);  CV(-30.0f, 20.0f, -60.0f);
		glColor3b(30, 200, 90);  CV( 40.0f, 25.0f, -30.0f);
		glColor3b(60, 70, 250);  CV(  2.0f,-20.0f,  -8.0f);
#else
		glColor3b(200, 150, 40); CV(-30.0f, 20.0f, -60.0f);
		glColor3b(200, 150, 40); CV( 40.0f, 25.0f, -30.0f);
		glColor3b(200, 150, 40); CV(  2.0f,-20.0f,  -8.0f);
#endif
	glEnd();
	glFlush(GL_TRANS_MANUALSORT | A3_WBUF);   // Z-buffer mode (W-buffer for case 39)
}
#endif

#if A3_CASE == 34
// Translucent-polygon fixture (see the header, cases 34-36). Textures straight to LCDC VRAM
// like case 30: bank C = texture slot 0, bank E = palettes.
#define TEXP(addr, sz, tz, fmt) ((u32)((addr) >> 3) | ((u32)(sz) << 20) | ((u32)(tz) << 23) | ((u32)(fmt) << 26))
#define T_ZW (1u << 11)   // POLYGON_ATTR: translucent polygons update depth
static const u32 texA3I5 = TEXP(0x0000, 1, 1, 1) | (1u << 16) | (1u << 17);   // 16x16, repeat
static const u32 texA5I3 = TEXP(0x0400, 1, 1, 6);                              // 16x16, clamp
static const u32 texDir  = TEXP(0x0800, 1, 1, 7);                              // 16x16 direct, alpha-bit holes

static void texSetup34(void)
{
	vramSetBankC(VRAM_C_LCD);
	vramSetBankE(VRAM_E_LCD);
	u8 *c = (u8 *)VRAM_C;
	u16 *e = (u16 *)VRAM_E;
	for (int i = 0; i < 32; i++) e[i] = RGB15(hashb(i, 13) & 31, hashb(i, 14) & 31, hashb(i, 15) & 31);
	for (int i = 0; i < 256; i++) c[0x0000 + i] = (u8)((hashb(i, 71) & 31) | ((hashb(i, 72) & 7) << 5));   // A3I5
	for (int i = 0; i < 256; i++) c[0x0400 + i] = (u8)((hashb(i, 73) & 7) | ((hashb(i, 74) & 31) << 3));   // A5I3
	for (int i = 0; i < 256; i++)
		((u16 *)(c + 0x0800))[i] = (u16)((hashb(i, 75) | (hashb(i, 76) << 8)) & 0x7FFF) | ((hashb(i, 77) & 3) ? 0x8000 : 0);
	vramSetBankC(VRAM_C_TEXTURE_SLOT0);
	vramSetBankE(VRAM_E_TEX_PALETTE);
}

static void tQuad(u32 tex, float x0, float y0, float x1, float y1, float zl, float zr, int s1, int t1)
{
	GFX_TEX_FORMAT = tex;
	GFX_PAL_FORMAT = 0;
	glBegin(GL_QUADS);
		glColor3b(255, 255, 255); glTexCoord2t16(inttot16(0),  inttot16(0));  glVertex3f(x0, y1, zl);
		glColor3b(255, 255, 255); glTexCoord2t16(inttot16(0),  inttot16(t1)); glVertex3f(x0, y0, zl);
		glColor3b(255, 255, 255); glTexCoord2t16(inttot16(s1), inttot16(t1)); glVertex3f(x1, y0, zr);
		glColor3b(255, 255, 255); glTexCoord2t16(inttot16(s1), inttot16(0));  glVertex3f(x1, y1, zr);
	glEnd();
}

static void transScene(void)
{
	glRotatef(10.0f, 1.0f, 0.0f, 0.0f);
	// A: translucent, alpha 12, no depth write, crossing the opaque quad's depth
	GFX_TEX_FORMAT = 0;
#if !A3_QUADPROBE
	glPolyFmt(POLY_ALPHA(12) | POLY_CULL_NONE | POLY_ID(10));
	glBegin(GL_TRIANGLES);
		glColor3b(250, 40, 40);  glVertex3f(-1.2f,  0.9f,  0.3f);
		glColor3b(40, 250, 40);  glVertex3f(-1.2f, -0.9f, -0.3f);
		glColor3b(40, 40, 250);  glVertex3f( 0.9f,  0.0f,  0.3f);
	glEnd();
#endif
	// opaque quad (list position 2; drawn first by the rasterizer's sort). Flat colour: a
	// 4-colour Gouraud quad is not reproduced by GX's two-triangle split (case 37).
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
	glBegin(GL_QUADS);
#if A3_QUADPROBE
		glColor3b(200, 200, 60); glVertex3f(-1.4f,  0.6f, 0.0f);
		glColor3b(60, 200, 200); glVertex3f(-1.4f, -0.7f, 0.0f);
		glColor3b(200, 60, 200); glVertex3f( 0.2f, -0.7f, 0.0f);
		glColor3b(220, 220, 220); glVertex3f( 0.2f,  0.6f, 0.0f);
	glEnd();
	glFlush(GL_TRANS_MANUALSORT | A3_WBUF);
	return;
#else
		glColor3b(200, 190, 70); glVertex3f(-1.4f,  0.6f, 0.0f);
		glColor3b(200, 190, 70); glVertex3f(-1.4f, -0.7f, 0.0f);
		glColor3b(200, 190, 70); glVertex3f( 0.2f, -0.7f, 0.0f);
		glColor3b(200, 190, 70); glVertex3f( 0.2f,  0.6f, 0.0f);
	glEnd();
#endif
	// B: translucent, alpha 20, depth write on, in front of A on the right
	glPolyFmt(POLY_ALPHA(20) | POLY_CULL_NONE | POLY_ID(11) | T_ZW);
	glBegin(GL_TRIANGLES);
		glColor3b(250, 250, 40); glVertex3f(-0.2f,  0.8f, 0.5f);
		glColor3b(40, 250, 250); glVertex3f(-0.2f, -0.5f, 0.5f);
		glColor3b(250, 40, 250); glVertex3f( 1.3f,  0.2f, 0.4f);
	glEnd();
	// C: translucent, alpha 16, no depth write, behind B (rejected where B wrote depth) and
	// partly behind A (blended over it, A wrote no depth)
	glPolyFmt(POLY_ALPHA(16) | POLY_CULL_NONE | POLY_ID(12));
	glBegin(GL_TRIANGLES);
		glColor3b(120, 250, 120); glVertex3f(-1.0f,  0.3f, 0.15f);
		glColor3b(250, 120, 60);  glVertex3f( 1.2f,  0.9f, 0.15f);
		glColor3b(60, 120, 250);  glVertex3f( 0.8f, -0.6f, 0.15f);
	glEnd();
	// A3I5, alpha 31, no depth write: opaque texels (alpha 7) write colour + depth
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(13));
	tQuad(texA3I5, 0.1f, -1.0f, 1.4f, -0.1f, 0.2f, 0.25f, 24, 24);
	// A5I3, alpha 20, depth write on
	glPolyFmt(POLY_ALPHA(20) | POLY_CULL_NONE | POLY_ID(14) | T_ZW);
	tQuad(texA5I3, 0.3f, -0.3f, 1.45f, 0.95f, -0.2f, 0.35f, 16, 16);
	// direct colour (opaque format), polygon alpha 16
	glPolyFmt(POLY_ALPHA(16) | POLY_CULL_NONE | POLY_ID(15));
	tQuad(texDir, -1.45f, -1.0f, -0.2f, -0.2f, 0.35f, 0.1f, 16, 16);
#if A3_SAMEID
	// two overlapping translucent quads, same ID 20: the second is dropped where the first drew
	glPolyFmt(POLY_ALPHA(10) | POLY_CULL_NONE | POLY_ID(20));
	GFX_TEX_FORMAT = 0;
	glBegin(GL_QUADS);
		glColor3b(255, 255, 255); glVertex3f(-1.3f, 1.0f, 0.45f);
		glColor3b(255, 255, 255); glVertex3f(-1.3f, 0.2f, 0.45f);
		glColor3b(255, 255, 255); glVertex3f(-0.3f, 0.2f, 0.45f);
		glColor3b(255, 255, 255); glVertex3f(-0.3f, 1.0f, 0.45f);
		glColor3b(40, 40, 40);    glVertex3f(-0.8f, 0.7f, 0.45f);
		glColor3b(40, 40, 40);    glVertex3f(-0.8f, -0.1f, 0.45f);
		glColor3b(40, 40, 40);    glVertex3f( 0.2f, -0.1f, 0.45f);
		glColor3b(40, 40, 40);    glVertex3f( 0.2f, 0.7f, 0.45f);
	glEnd();
#endif
	glFlush(GL_TRANS_MANUALSORT | A3_WBUF);
}
#endif

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
#if A3_CASE == 28
	// precision-stress fixture: extra off-axis rotation stacked on the standard 27-degree
	// Y rotation from above (genuinely oblique on two axes at once, not aligned to either
	// screen edge), four flat-coloured opaque triangles spanning a wide depth range.
	glRotatef(15.0f, 1.0f, 0.0f, 0.0f);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
	glBegin(GL_TRIANGLES);
		// 1: near-ish, oblique in depth
		glColor3b(220, 40, 40);
		glVertex3f(-0.9f,  0.55f, -0.5f);
		glVertex3f(-0.9f, -0.55f,  0.35f);
		glVertex3f( 0.9f, -0.55f, -0.1f);
		// 2: far, smaller
		glColor3b(40, 220, 40);
		glVertex3f(-0.3f,  0.25f,  0.9f);
		glVertex3f(-0.3f, -0.25f,  1.05f);
		glVertex3f( 0.3f, -0.25f,  0.95f);
		// 3: long thin sliver placed close to the left frustum edge (NDC x ~ -0.89,
		// verified in Python against this exact projection/modelview before landing
		// on these numbers -- gluPerspective(70, 4:3, 0.1, 40) + translate(-2.4) +
		// rotate(27,Y) + rotate(15,X), worst-case |NDC| ~0.94 on all three vertices
		// of this fixture, comfortable margin below the [-1,1] clip-avoidance check
		// in gxDs3dGeomFrameSupported() while still stressing near-edge precision)
		glColor3b(40, 40, 220);
		glVertex3f(-1.5f,  0.5f,  0.2f);
		glVertex3f(-1.5f, -0.5f,  0.25f);
		glVertex3f(-1.3f,  0.0f, -0.15f);
		// 4: mid triangle, different winding/orientation, depth-overlaps #1
		glColor3b(220, 200, 20);
		glVertex3f( 0.1f,  0.7f, -0.2f);
		glVertex3f( 0.75f, 0.1f,  0.4f);
		glVertex3f( 0.3f, -0.65f, 0.05f);
	glEnd();
	glFlush(GL_TRANS_MANUALSORT | A3_WBUF);   // Z-buffer mode (W-buffer for case 32)
	return;
#endif
#if A3_CASE == 29
	// item 13f minimal fog fixture: one flat mid-grey quad (under the same unconditional
	// 27-degree Y rotation every case gets), tilted steeply in Z so its
	// near edge (bottom of screen) sits close to the camera and its far edge (top of screen)
	// sits deep in the frustum -- z spans -0.6 (near, same sign convention as case 28's "near"
	// triangles) to 0.9 (far, matching case 28's "far" triangle), covering most of the DS's
	// 15-bit fog index range across the fixture's own visible rows. Flat grey colour so any
	// fog-colour contamination is maximally visible against it (no per-channel colour of its
	// own to mask a wrong blend). POLY_FOG is required per-polygon (glPolyFmt's raw
	// GFX_POLY_FORMAT write has no default fog-enable bit -- found this pass: case 12's
	// own gradient-quad glPolyFmt call is missing it too, so that fixture's fog test
	// coverage was never actually exercising fogged polygons either; not fixed here,
	// out of scope, flagged in the log).
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1) | POLY_FOG);
	glBegin(GL_TRIANGLES);
		glColor3b(180, 180, 180); glVertex3f(-1.4f,  1.0f,  0.9f);
		glColor3b(180, 180, 180); glVertex3f(-1.4f, -1.0f, -0.6f);
		glColor3b(180, 180, 180); glVertex3f( 1.4f, -1.0f, -0.6f);

		glColor3b(180, 180, 180); glVertex3f( 1.4f, -1.0f, -0.6f);
		glColor3b(180, 180, 180); glVertex3f( 1.4f,  1.0f,  0.9f);
		glColor3b(180, 180, 180); glVertex3f(-1.4f,  1.0f,  0.9f);
	glEnd();
	glFlush(GL_TRANS_MANUALSORT);   // no GL_WBUFFERING -- Z-buffer mode
	return;
#endif
#if A3_CASE == 30 || A3_CASE == 31
	texScene();
	return;
#endif
#if A3_CASE == 38
	clipScene();
	return;
#endif
#if A3_CASE == 34
	transScene();
	return;
#endif
#if A3_ZBUF
	// 41-45: inside the view volume (no clipping) with flat colours, GxAccurate's scope
	glScalef(0.6f, 0.6f, 0.6f);
#define glColor3b(r, g, b) glColor3b(A3_FLAT)
#endif
#if A3_CASE != 25
	// big gradient quad (covers most of the screen), polygon id 1
#define A3_FLAT 200, 80, 40
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
#undef A3_FLAT
#define A3_FLAT 40, 200, 160
	glPolyFmt(POLY_ALPHA(polyAlpha) | POLY_CULL_NONE | POLY_ID(2));
	glBegin(GL_TRIANGLES);
		glColor3b(255, 255,   0); glVertex3f(-1.4f,  1.1f, 0.4f);
		glColor3b(  0, 255, 255); glVertex3f(-1.4f, -1.7f, 0.4f);
		glColor3b(255,   0, 255); glVertex3f( 1.7f, -0.6f, 0.4f);
	glEnd();
#endif
#undef glColor3b
	// case 25: matrices/material state set up identically above, but zero triangles submitted --
	// BG0's 3D slot is enabled in DISPCNT/BG0CNT and takes part in the frame exactly like every
	// other case, it just never has any content (equivalent to an all-holes, alpha=0 3D layer).
	glFlush(GL_TRANS_MANUALSORT | (A3_ZBUF ? 0 : GL_WBUFFERING));
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
#elif A3_CASE == 28
	// same isolation as case 27 -- nothing but the backdrop + BG0/3D visible, so the GX
	// overlay draw is position-correct with nothing else able to occlude it.
	dcnt &= ~(DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE);
#elif A3_CASE == 29
	// item 13f minimal fog fixture: same isolation as 27/28 (nothing but backdrop + BG0/3D
	// visible), one flat-shaded opaque untextured quad tilted steeply in Z so its depth
	// sweeps a wide range across the screen -- a real, non-degenerate fog gradient, not a
	// flat all-0/all-max case. holes (clear alpha 0) so the backdrop is visible where the
	// quad doesn't cover, same as case 1/27.
	dcnt &= ~(DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE);
	fog = 1;
#elif A3_CASE == 30 || A3_CASE == 31 || A3_CASE == 38
	// textured fixture: same isolation as 27/28 (backdrop + BG0/3D only).
	dcnt &= ~(DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE | DISPLAY_SPR_ACTIVE);
#elif A3_CASE == 34
	// translucent fixture: same isolation, clear colour alpha 31 (GxFast's translucent scope).
	cover = 1;
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
#elif A3_CASE == 29
	// fog-only, simplified from case 12 (no antialiasing/outline -- item 13e's own fixture
	// philosophy: case 27 was deliberately simpler than case 0, this is the same idea for
	// fog). Distinct fog colour (deep blue-green) so a wrong blend is easy to see by eye.
	// fogShift=0 / fogOffset=0 spreads the 32 density control points across the FULL
	// 0..32767 index range (32 * (1024>>0) == 32768) -- found this pass: case 12's own
	// fogShift(3)/fogOffset(0x2000) values only cover a narrow index band, and this
	// fixture's first attempt at similar values saturated to near-max fog across the
	// whole quad (not visibly degenerate by eye, but not the real gradient the task
	// asked for either) -- spanning the whole index space guarantees whatever depth
	// range the quad's vertices actually produce lands inside a real, non-flat slope.
	glEnable(GL_FOG);
	glFogColor(2, 6, 24, 24);
	glFogShift(0);
	glFogOffset(0);
	for (int i = 0; i < 32; i++) glFogDensity(i, i * 4);
#elif A3_CASE == 30 || A3_CASE == 31 || A3_CASE == 38
	glEnable(GL_TEXTURE_2D);
	texSetup();
#elif A3_CASE == 34
	glEnable(GL_TEXTURE_2D | GL_BLEND | GL_ALPHA_TEST);
	glAlphaFunc(5);
	texSetup34();
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
