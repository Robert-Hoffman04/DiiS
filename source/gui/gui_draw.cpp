/****************************************************************************
 * DiiS - gui_draw.cpp   (see gui.h)
 *
 * One TEV stage, one vertex format (position XY, colour, texcoord) for
 * everything: solid shapes are a white mask texture, rounded corners are
 * coverage masks made here per radius (exact 1:1, anti-aliased), text is the
 * font atlas mask, icons/pointer are RGBA textures. Positions are integer
 * canvas pixels and textures are sampled 1:1 (or integer-scaled) with a
 * nearest filter, so nothing is resampled.
 ***************************************************************************/
#include <malloc.h>
#include <string.h>
#include <math.h>

#include "gui_internal.h"
#include "../cursor_tex.h"   // Wii pointer + shadow (same art as the in-game cursor)
#include "../xfb.h"

// main.cpp's video setup.
extern GXRModeObj *rmode;
extern int currfb;
extern Mtx44 perspective;
extern Mtx GXmodelView2D;
void GxRestorePresentState(void);

static bool s_ready = false;
static u8 s_alpha = 255;
static GXTexObj *s_boundTex = 0;
static int s_boundMode = -1;

static u8 s_white[8 * 8 / 2] __attribute__((aligned(32)));   // I4, all 0xF
static GXTexObj s_whiteTex;
static GXTexObj s_ptrTex, s_ptrShadowTex;

// The canvas is drawn 1:1 into the EFB's top-left GUI_W x GUI_H, copied out
// to this texture and drawn back over the whole 640x480 screen with a
// nearest filter: every canvas pixel is exactly a 2x2 block. The pointer is
// drawn after, at full resolution.
static u16 s_canvas[GUI_W * GUI_H] __attribute__((aligned(32)));   // RGB565
static GXTexObj s_canvasTex;
static Mtx44 s_canvasProj;
static bool s_ptrShow;
static int s_edgeFills;      // XFBs whose edges show the settled backdrop (of 2)
static GuiColor s_fadeCol;   // this frame's guiFade()
static u8 s_fadeA;
static void fillEdges(int i);
static int s_ptrX, s_ptrY;

static std::vector<GuiRect> s_clip;

GuiRect GuiRect::intersect(const GuiRect &o) const
{
	const int x0 = x > o.x ? x : o.x, y0 = y > o.y ? y : o.y;
	const int x1 = x + w < o.x + o.w ? x + w : o.x + o.w;
	const int y1 = y + h < o.y + o.h ? y + h : o.y + o.h;
	return GuiRect(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

//---------------------------------------------------------------------------
// Rounded-corner coverage masks, one per (radius, ring width); ring 0 = the
// filled quarter disc. I8, top-left corner (other corners flip texcoords).
//---------------------------------------------------------------------------
struct CornerMask {
	int r, ring, size;
	u8 *data;
	GXTexObj tex;
};
static std::vector<CornerMask *> s_corners;   // pointers: bound GXTexObjs must not move

static const CornerMask &cornerMask(int r, int ring)
{
	for (size_t i = 0; i < s_corners.size(); i++)
		if (s_corners[i]->r == r && s_corners[i]->ring == ring)
			return *s_corners[i];
	CornerMask &m = *new CornerMask;
	m.r = r;
	m.ring = ring;
	m.size = (r + 7) & ~7;
	m.data = (u8 *)memalign(32, m.size * m.size);
	memset(m.data, 0, m.size * m.size);
	const float ro = (float)r, ri = ring ? (float)(r - ring) : -1.0f;
	for (int y = 0; y < r; y++)
		for (int x = 0; x < r; x++) {
			int in = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					const float dx = r - (x + (sx + 0.5f) / 4.0f), dy = r - (y + (sy + 0.5f) / 4.0f);
					const float d2 = dx * dx + dy * dy;
					if (d2 <= ro * ro && (ri < 0.0f || d2 > ri * ri))
						in++;
				}
			// I8 tiles are 8x4 texels.
			const int idx = ((y >> 2) * (m.size >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7);
			m.data[idx] = (u8)(in * 255 / 16);
		}
	DCFlushRange(m.data, m.size * m.size);
	GX_InitTexObj(&m.tex, m.data, m.size, m.size, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&m.tex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_InvalidateTexAll();
	s_corners.push_back(&m);
	return m;
}

//---------------------------------------------------------------------------
// Setup and frames
//---------------------------------------------------------------------------
static void initOnce()
{
	if (s_ready)
		return;
	memset(s_white, 0xFF, sizeof(s_white));
	DCFlushRange(s_white, sizeof(s_white));
	GX_InitTexObj(&s_whiteTex, s_white, 8, 8, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_whiteTex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);

	DCFlushRange(CursorData, sizeof(CursorData));
	DCFlushRange(CursorShadowData, sizeof(CursorShadowData));
	GX_InitTexObj(&s_ptrTex, CursorData, CURSOR_TEX_W, CURSOR_TEX_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_ptrTex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_InitTexObj(&s_ptrShadowTex, CursorShadowData, CURSOR_TEX_W, CURSOR_TEX_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_ptrShadowTex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);

	GX_InitTexObj(&s_canvasTex, s_canvas, GUI_W, GUI_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_canvasTex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	guOrtho(s_canvasProj, 0, GUI_H, 0, GUI_W, 0, 300);

	guiTextInit();
	s_ready = true;
}

static void applyScissor(const GuiRect &r)
{
	// Canvas pixels are EFB pixels while the canvas is drawn.
	GX_SetScissor(r.x, r.y, r.w, r.h);
}

void guiBeginFrame(GuiColor clear)
{
	initOnce();

	GX_SetViewport(0, 0, GUI_W, GUI_H, 0, 1);
	GX_LoadProjectionMtx(s_canvasProj, GX_ORTHOGRAPHIC);
	GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GUI_VTXFMT, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GUI_VTXFMT, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GUI_VTXFMT, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_InvalidateTexAll();
	s_boundTex = 0;
	s_boundMode = -1;

	s_ptrShow = false;
	s_fadeA = 0;
	s_clip.clear();
	applyScissor(GuiRect(0, 0, GUI_W, GUI_H));

	const u8 a = s_alpha;
	s_alpha = 255;
	guiRect(GuiRect(0, 0, GUI_W, GUI_H), guiRgba(clear.r, clear.g, clear.b));
	s_alpha = a;
}

void guiEndFrame()
{
	// Canvas -> texture, unfiltered (the display copy's vertical filter
	// would blur it).
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
	GX_SetTexCopySrc(0, 0, GUI_W, GUI_H);
	GX_SetTexCopyDst(GUI_W, GUI_H, GX_TF_RGB565, GX_FALSE);
	GX_CopyTex(s_canvas, GX_FALSE);
	GX_PixModeSync();
	GX_InvalidateTexAll();
	s_boundTex = 0;
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);

	// Scaled up over the whole screen (the 640x480 projection main.cpp
	// set up, stretched to the EFB's height), then the pointer.
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	guiBindRgba(&s_canvasTex);
	GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
	guiQuadVerts(0, 0, 640, 480, guiRgba(255, 255, 255), 0, 0, 1, 1);
	GX_End();
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	if (s_ptrShow) {
		const int x = s_ptrX * 640 / GUI_W, y = s_ptrY * 480 / GUI_H;
		for (int pass = 0; pass < 2; pass++) {
			// Shadow first, offset (+1,+2), as the in-game cursor.
			const int px = x - CURSOR_HOT_X + (pass ? 0 : 1), py = y - CURSOR_HOT_Y + (pass ? 0 : 2);
			guiBindRgba(pass ? &s_ptrTex : &s_ptrShadowTex);
			GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
			guiQuadVerts(px, py, px + CURSOR_TEX_W, py + CURSOR_TEX_H, guiRgba(255, 255, 255), 0, 0, 1, 1);
			GX_End();
		}
	}

	currfb ^= 1;
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(XFB_W, GX_SetDispCopyYScale(GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight)));
	GX_CopyDisp(xfbCopyDst(currfb), GX_TRUE);
	GX_DrawDone();
	// The XFB's edge columns continue the backdrop, under the same fade:
	// every frame a fade runs, then once more per XFB.
	if (s_fadeA) {
		fillEdges(currfb);
		s_edgeFills = 0;
	} else if (s_edgeFills < 2) {
		fillEdges(currfb);
		s_edgeFills++;
	}
	VIDEO_SetNextFramebuffer(xfb[currfb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

static u32 yuyvPair(GuiColor c)   // two pixels of c (BT.601, as the XFB copy)
{
	const int r = c.r, g = c.g, b = c.b;
	const u32 y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
	const u32 u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
	const u32 v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
	return y << 24 | u << 16 | y << 8 | v;
}

static GuiColor faded(GuiColor c)   // c under this frame's guiFade()
{
	const int a = s_fadeA;
	c.r = (u8)((c.r * (255 - a) + s_fadeCol.r * a) / 255);
	c.g = (u8)((c.g * (255 - a) + s_fadeCol.g * a) / 255);
	c.b = (u8)((c.b * (255 - a) + s_fadeCol.b * a) / 255);
	return c;
}

// The XFB's XFB_PAD columns either side of the 640 picture: canvas columns
// -XFB_PAD/2.. and GUI_W.. at 2x, one YUYV pair per canvas pixel. A row's
// backdrop only varies with x's parity (the top bar's dither).
static void fillEdges(int i)
{
	const int pairs = XFB_PAD / 2, stride = XFB_W / 2, rows = viMode.xfbHeight;
	for (int yy = 0; yy < rows; yy++) {
		u32 *row = (u32 *)xfb[i] + yy * stride;
		const int y = yy * GUI_H / rows;
		u32 l[2], r[2];
		for (int p = 0; p < 2; p++) {
			l[p] = yuyvPair(faded(guiDsEdgeColor(p - pairs, y)));
			r[p] = yuyvPair(faded(guiDsEdgeColor(GUI_W + p, y)));
		}
		for (int k = 0; k < pairs; k++) {
			row[k] = l[k & 1];
			row[stride - pairs + k] = r[k & 1];
		}
	}
}

void guiRestoreGx()
{
	GX_DrawDone();
	s_edgeFills = 0;
	// The screen stays as the last frame left it (black after guiQuit()'s
	// fade) until draw_thread presents the game.
	GxRestorePresentState();
}

void guiSetAlpha(u8 a) { s_alpha = a; }
u8 guiAlpha() { return s_alpha; }

GuiColor guiApplyAlpha(GuiColor c)
{
	if (s_alpha != 255)
		c.a = (u8)(c.a * s_alpha / 255);
	return c;
}

void guiBindMask(GXTexObj *tex)
{
	if (s_boundMode != 0) {
		GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
		GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
		GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		s_boundMode = 0;
	}
	if (s_boundTex != tex) {
		GX_LoadTexObj(tex, GX_TEXMAP0);
		s_boundTex = tex;
	}
}

void guiBindRgba(GXTexObj *tex)
{
	if (s_boundMode != 1) {
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		s_boundMode = 1;
	}
	if (s_boundTex != tex) {
		GX_LoadTexObj(tex, GX_TEXMAP0);
		s_boundTex = tex;
	}
}

//---------------------------------------------------------------------------
// Clipping
//---------------------------------------------------------------------------
void guiPushClip(const GuiRect &r)
{
	const GuiRect c = s_clip.empty() ? r.intersect(GuiRect(0, 0, GUI_W, GUI_H)) : r.intersect(s_clip.back());
	s_clip.push_back(c);
	applyScissor(c);
}

void guiPopClip()
{
	if (!s_clip.empty())
		s_clip.pop_back();
	applyScissor(s_clip.empty() ? GuiRect(0, 0, GUI_W, GUI_H) : s_clip.back());
}

//---------------------------------------------------------------------------
// Shapes
//---------------------------------------------------------------------------
static void solid(int x, int y, int w, int h, GuiColor c)
{
	if (w <= 0 || h <= 0)
		return;
	GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
	guiQuadVerts(x, y, x + w, y + h, c, 0, 0, 1, 1);
	GX_End();
}

void guiRect(const GuiRect &r, GuiColor c)
{
	guiBindMask(&s_whiteTex);
	solid(r.x, r.y, r.w, r.h, guiApplyAlpha(c));
}

void guiTriangle(int x0, int y0, int x1, int y1, int x2, int y2, GuiColor c)
{
	c = guiApplyAlpha(c);
	guiBindMask(&s_whiteTex);
	GX_Begin(GX_TRIANGLES, GUI_VTXFMT, 3);
	guiVtx(x0, y0, c, 0, 0);
	guiVtx(x1, y1, c, 0, 0);
	guiVtx(x2, y2, c, 0, 0);
	GX_End();
}

void guiRectGradient(const GuiRect &r, GuiColor top, GuiColor bottom)
{
	if (r.empty())
		return;
	top = guiApplyAlpha(top);
	bottom = guiApplyAlpha(bottom);
	guiBindMask(&s_whiteTex);
	GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
	guiVtx(r.x, r.y, top, 0, 0);
	guiVtx(r.x + r.w, r.y, top, 1, 0);
	guiVtx(r.x + r.w, r.y + r.h, bottom, 1, 1);
	guiVtx(r.x, r.y + r.h, bottom, 0, 1);
	GX_End();
}

// The four r x r corners of rect `b` from mask m.
static void corners(const GuiRect &b, int r, const CornerMask &m, GuiColor c)
{
	guiBindMask((GXTexObj *)&m.tex);
	const f32 e = (f32)r / m.size;
	const int x0 = b.x, y0 = b.y, x1 = b.x + b.w - r, y1 = b.y + b.h - r;
	GX_Begin(GX_QUADS, GUI_VTXFMT, 16);
	guiQuadVerts(x0, y0, x0 + r, y0 + r, c, 0, 0, e, e);   // top-left
	guiQuadVerts(x1, y0, x1 + r, y0 + r, c, e, 0, 0, e);   // top-right (s flipped)
	guiQuadVerts(x0, y1, x0 + r, y1 + r, c, 0, e, e, 0);   // bottom-left (t flipped)
	guiQuadVerts(x1, y1, x1 + r, y1 + r, c, e, e, 0, 0);   // bottom-right
	GX_End();
}

static int clampRadius(const GuiRect &r, int radius)
{
	int m = (r.w < r.h ? r.w : r.h) / 2;
	return radius < 0 ? 0 : radius > m ? m : radius;
}

void guiRoundRect(const GuiRect &b, int radius, GuiColor c)
{
	if (b.empty())
		return;
	const int r = clampRadius(b, radius);
	c = guiApplyAlpha(c);
	if (!r) {
		guiBindMask(&s_whiteTex);
		solid(b.x, b.y, b.w, b.h, c);
		return;
	}
	guiBindMask(&s_whiteTex);
	solid(b.x + r, b.y, b.w - 2 * r, b.h, c);
	solid(b.x, b.y + r, r, b.h - 2 * r, c);
	solid(b.x + b.w - r, b.y + r, r, b.h - 2 * r, c);
	corners(b, r, cornerMask(r, 0), c);
}

static void ring(const GuiRect &b, int radius, int t, GuiColor c)
{
	const int r = clampRadius(b, radius);
	if (t > r && r)
		t = r;
	guiBindMask(&s_whiteTex);
	if (!r) {
		solid(b.x, b.y, b.w, t, c);
		solid(b.x, b.y + b.h - t, b.w, t, c);
		solid(b.x, b.y + t, t, b.h - 2 * t, c);
		solid(b.x + b.w - t, b.y + t, t, b.h - 2 * t, c);
		return;
	}
	solid(b.x + r, b.y, b.w - 2 * r, t, c);
	solid(b.x + r, b.y + b.h - t, b.w - 2 * r, t, c);
	solid(b.x, b.y + r, t, b.h - 2 * r, c);
	solid(b.x + b.w - t, b.y + r, t, b.h - 2 * r, c);
	corners(b, r, cornerMask(r, t), c);
}

void guiPanel(const GuiRect &b, int radius, GuiColor fill, GuiColor border, int borderW)
{
	if (b.empty())
		return;
	if (borderW <= 0 || !border.a) {
		guiRoundRect(b, radius, fill);
		return;
	}
	// The fill stops one pixel inside the border's outer edge so its own
	// anti-aliased rim never shows past the border's.
	if (fill.a)
		guiRoundRect(b.inset(1), radius - 1, fill);
	ring(b, radius, borderW, guiApplyAlpha(border));
}

void guiFade(u8 a, GuiColor c)
{
	s_fadeA = a;
	s_fadeCol = c;
	if (!a)
		return;
	guiBindMask(&s_whiteTex);
	solid(0, 0, GUI_W, GUI_H, guiRgba(c.r, c.g, c.b, a));
}

//---------------------------------------------------------------------------
// Images
//---------------------------------------------------------------------------
bool guiImageCreate(GuiImage &img, const u16 *src, int w, int h)
{
	guiImageFree(img);
	if (w <= 0 || h <= 0 || (w & 3) || (h & 3))
		return false;
	img.data = (u16 *)memalign(32, w * h * 2);
	if (!img.data)
		return false;
	u16 *d = img.data;
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 4)
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					*d++ = src[(ty + y) * w + tx + x];
	img.w = w;
	img.h = h;
	DCFlushRange(img.data, w * h * 2);
	GX_InitTexObj(&img.tex, img.data, w, h, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&img.tex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	return true;
}

void guiImageFree(GuiImage &img)
{
	if (img.data)
		free(img.data);
	img.data = 0;
	img.w = img.h = 0;
}

void guiImage(const GuiImage &img, int x, int y, int scale, GuiColor tint)
{
	if (!img.data)
		return;
	guiBindRgba((GXTexObj *)&img.tex);
	GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
	guiQuadVerts(x, y, x + img.w * scale, y + img.h * scale, guiApplyAlpha(tint), 0, 0, 1, 1);
	GX_End();
}

void guiImagePart(const GuiImage &img, int sx, int sy, int sw, int sh, int x, int y, int scale, GuiColor tint)
{
	if (!img.data || sw <= 0 || sh <= 0)
		return;
	const f32 iw = 1.0f / img.w, ih = 1.0f / img.h;
	guiBindRgba((GXTexObj *)&img.tex);
	GX_Begin(GX_QUADS, GUI_VTXFMT, 4);
	guiQuadVerts(x, y, x + sw * scale, y + sh * scale, guiApplyAlpha(tint), sx * iw, sy * ih, (sx + sw) * iw, (sy + sh) * ih);
	GX_End();
}

const GuiImage &guiGenericIcon(bool gba)
{
	initOnce();
	return gba ? guiGfx(GUI_GFX_ICON_GBA) : guiGfx(GUI_GFX_ICON_DSCARD);
}

void guiPointer(int x, int y)
{
	// Drawn by guiEndFrame(), over the scaled-up canvas.
	s_ptrShow = true;
	s_ptrX = x;
	s_ptrY = y;
}

void guiFocusFrame(const GuiRect &r)
{
	// The DS menu's cursor: four corner brackets (GUI_GFX_CURSOR frames TL,
	// TR, BL, BR, 11x11 art in the frame's top-left), each sitting 2 px
	// outside r, as the DS menu frames its boxes.
	const int s = GUI_DS_SCALE, k = 11 * s, o = 2 * s;
	const int x0 = r.x - o, y0 = r.y - o, x1 = r.x + r.w + o - k, y1 = r.y + r.h + o - k;
	guiImagePart(guiGfx(GUI_GFX_CURSOR, 0), 0, 0, 11, 11, x0, y0, s);
	guiImagePart(guiGfx(GUI_GFX_CURSOR, 1), 0, 0, 11, 11, x1, y0, s);
	guiImagePart(guiGfx(GUI_GFX_CURSOR, 2), 0, 0, 11, 11, x0, y1, s);
	guiImagePart(guiGfx(GUI_GFX_CURSOR, 3), 0, 0, 11, 11, x1, y1, s);
}
