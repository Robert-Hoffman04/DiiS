/****************************************************************************
 * DiiS - gui_text.cpp   (see gui.h)
 *
 * Bitmap text from gui_font_data.h (tools/gui/mkfont.py). Each glyph is a
 * quad of the I4 atlas at an integer pixel position, one texel per pixel;
 * the pen advances in 1/16 px (the font's advances, plus kerning if any) and is
 * rounded per glyph.
 ***************************************************************************/
#include <string.h>
#include <string>

#include "gui_internal.h"

struct GuiGlyph {
	u16 cp;
	u16 x, y;
	u8 w, h;
	s8 ox, oy;      // ink offset from the pen / baseline
	u16 adv;        // 1/16 px
};
struct GuiKern {
	u32 pair;       // left << 16 | right
	s8 k;           // 1/16 px
};

#include "gui_font_data.h"

struct FontDef {
	const GuiGlyph *g;
	int n;
	const GuiKern *k;
	int nk;
	int asc, desc, cap;
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const FontDef s_fonts[2] = {
	{ GuiFontBodyGlyphs, COUNT(GuiFontBodyGlyphs), GuiFontBodyKern, COUNT(GuiFontBodyKern),
	  GUI_FONT_BODY_ASCENT, GUI_FONT_BODY_DESCENT, GUI_FONT_BODY_CAP },
	{ GuiFontTitleGlyphs, COUNT(GuiFontTitleGlyphs), GuiFontTitleKern, COUNT(GuiFontTitleKern),
	  GUI_FONT_TITLE_ASCENT, GUI_FONT_TITLE_DESCENT, GUI_FONT_TITLE_CAP },
};

static GXTexObj s_atlasTex;

void guiTextInit()
{
	DCFlushRange(GuiFontAtlas, sizeof(GuiFontAtlas));
	GX_InitTexObj(&s_atlasTex, GuiFontAtlas, GUI_FONT_ATLAS_W, GUI_FONT_ATLAS_H, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_atlasTex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

int guiFontHeight(GuiFont f) { return s_fonts[f].asc + s_fonts[f].desc; }
int guiFontAscent(GuiFont f) { return s_fonts[f].asc; }
int guiFontCap(GuiFont f) { return s_fonts[f].cap; }

u32 guiUtf8Next(const char *&p)
{
	const u8 *s = (const u8 *)p;
	const u32 c = s[0];
	if (!c)
		return 0;
	int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
	if (n < 0) {
		p++;
		return 0xFFFD;
	}
	u32 cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xC0) != 0x80) {   // truncated sequence: resync on this byte
			p += i;
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	p += n + 1;
	return cp;
}

// Glyph index for a code point; -1 = draws nothing (control characters).
static int glyphIndex(const FontDef &f, u32 cp)
{
	if (cp == '\t' || cp == 0x3000)
		cp = ' ';
	else if (cp >= 0xFF01 && cp <= 0xFF5E)   // full-width ASCII
		cp -= 0xFEE0;
	if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
		return -1;
	int lo = 1, hi = f.n - 1;   // glyph 0 is the missing-glyph box
	while (lo <= hi) {
		const int mid = (lo + hi) >> 1;
		if (f.g[mid].cp == cp)
			return mid;
		if (f.g[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

static int kern(const FontDef &f, int a, int b)
{
	if (a <= 0 || b <= 0)
		return 0;
	const u32 key = ((u32)f.g[a].cp << 16) | f.g[b].cp;
	int lo = 0, hi = f.nk - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) >> 1;
		if (f.k[mid].pair == key)
			return f.k[mid].k;
		if (f.k[mid].pair < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

// Lays out [s, end) and, when draw is set, draws it with its pen starting at
// x (pixels) on baseline `base`. Returns the advance width in 1/16 px.
static int run(const FontDef &f, const char *s, const char *end, int x, int base, GuiColor c, bool draw)
{
	int pen = 0, prev = -1, quads = 0;
	if (draw) {
		// GX needs the vertex count up front.
		for (const char *p = s; p < end && *p;) {
			const int gi = glyphIndex(f, guiUtf8Next(p));
			if (gi >= 0 && f.g[gi].w)
				quads++;
		}
		if (!quads)
			draw = false;
		else {
			guiBindMask(&s_atlasTex);
			GX_Begin(GX_QUADS, GUI_VTXFMT, 4 * quads);
		}
	}
	const f32 iw = 1.0f / GUI_FONT_ATLAS_W, ih = 1.0f / GUI_FONT_ATLAS_H;
	for (const char *p = s; p < end && *p;) {
		const int gi = glyphIndex(f, guiUtf8Next(p));
		if (gi < 0)
			continue;
		pen += kern(f, prev, gi);
		const GuiGlyph &g = f.g[gi];
		if (draw && g.w) {
			const int gx = x + ((pen + 8) >> 4) + g.ox, gy = base + g.oy;
			guiQuadVerts(gx, gy, gx + g.w, gy + g.h, c,
			             g.x * iw, g.y * ih, (g.x + g.w) * iw, (g.y + g.h) * ih);
		}
		pen += g.adv;
		prev = gi;
	}
	if (draw)
		GX_End();
	return pen;
}

static int width16(const FontDef &f, const char *s, const char *end) { return run(f, s, end, 0, 0, GuiColor(), false); }

int guiTextWidth(GuiFont f, const char *utf8)
{
	return (width16(s_fonts[f], utf8, utf8 + strlen(utf8)) + 8) >> 4;
}

static const char kEllipsis[] = "\xE2\x80\xA6";   // U+2026

int guiText(GuiFont fi, int x, int y, const char *utf8, GuiColor c, GuiAlign align, int maxW)
{
	const FontDef &f = s_fonts[fi];
	const char *end = utf8 + strlen(utf8);
	int w = (width16(f, utf8, end) + 8) >> 4;
	std::string cut;
	if (maxW > 0 && w > maxW) {
		// Longest prefix that still fits with the ellipsis after it.
		const int room = maxW * 16 - width16(f, kEllipsis, kEllipsis + 3);
		const char *p = utf8, *keep = utf8;
		while (*p) {
			guiUtf8Next(p);
			if (width16(f, utf8, p) > room)
				break;
			keep = p;
		}
		while (keep > utf8 && keep[-1] == ' ')
			keep--;
		cut.assign(utf8, keep - utf8);
		cut += kEllipsis;
		utf8 = cut.c_str();
		end = utf8 + cut.size();
		w = (width16(f, utf8, end) + 8) >> 4;
	}
	const int x0 = align == GUI_ALIGN_CENTER ? x - w / 2 : align == GUI_ALIGN_RIGHT ? x - w : x;
	run(f, utf8, end, x0, y + f.asc, guiApplyAlpha(c), true);
	return w;
}

void guiTextBox(GuiFont fi, const GuiRect &r, const char *utf8, GuiColor c, GuiAlign align)
{
	const FontDef &f = s_fonts[fi];
	const int base = r.y + (r.h + f.cap) / 2;
	const int x = align == GUI_ALIGN_CENTER ? r.x + r.w / 2 : align == GUI_ALIGN_RIGHT ? r.x + r.w : r.x;
	guiText(fi, x, base - f.asc, utf8, c, align, r.w);
}

int guiTextWrap(GuiFont fi, const GuiRect &r, const char *utf8, GuiColor c, GuiAlign align)
{
	const FontDef &f = s_fonts[fi];
	const int lh = f.asc + f.desc;
	const int x = align == GUI_ALIGN_CENTER ? r.x + r.w / 2 : align == GUI_ALIGN_RIGHT ? r.x + r.w : r.x;
	int y = r.y;
	const char *p = utf8;
	while (*p && y + lh <= r.y + r.h) {
		// Greedy: extend the line word by word while it fits.
		const char *lineEnd = p, *q = p;
		while (*q && *q != '\n') {
			const char *w = q;
			while (*w == ' ')
				w++;
			while (*w && *w != ' ' && *w != '\n')
				w++;
			if (lineEnd != p && width16(f, p, w) > r.w * 16)
				break;
			lineEnd = q = w;
		}
		std::string line(p, lineEnd - p);
		guiText(fi, x, y, line.c_str(), c, align, r.w);
		y += lh;
		p = lineEnd;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
	}
	return y - r.y;
}
