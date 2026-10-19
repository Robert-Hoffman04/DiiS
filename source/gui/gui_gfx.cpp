/****************************************************************************
 * DiiS - gui_gfx.cpp
 *
 * The menu's textures: the PNGs in data/gui (imported from res/gfx by
 * tools/gui/importgfx.py, colour keys already turned into alpha), linked in
 * by the Makefile's bin2o rule. Decoded on first use, one GuiImage per frame
 * (sheets are frames stacked vertically).
 ***************************************************************************/
#include "gui.h"
#include "../library/lib_png.h"

#define GFX(name) extern "C" const u8 name##_png[], name##_png_end[];
GFX(boxart_placeholder) GFX(cornericons) GFX(cursor) GFX(dialogbox)
GFX(folder_icon) GFX(icon_dscard) GFX(icon_empty) GFX(icon_gba)
GFX(iconphat_gba) GFX(icon_gbamode) GFX(icon_settings) GFX(icon_settings_away)
GFX(icon_unk) GFX(iconbox) GFX(iconbox_pressed) GFX(launch_dot)
GFX(scroll_window) GFX(start_border) GFX(wirelessicons)
#undef GFX

struct GfxSrc { const u8 *data, *end; int frames; };
#define S(name, frames) { name##_png, name##_png_end, frames }
static const GfxSrc kSrc[GUI_GFX_COUNT] = {   // same order as GuiGfx
	S(boxart_placeholder, 1), S(cornericons, 4), S(cursor, 4), S(dialogbox, 1),
	S(folder_icon, 1), S(icon_dscard, 2), S(icon_empty, 1), S(icon_gba, 1),
	S(iconphat_gba, 1), S(icon_gbamode, 2), S(icon_settings, 2), S(icon_settings_away, 1),
	S(icon_unk, 1), S(iconbox, 2), S(iconbox_pressed, 1), S(launch_dot, 8),
	S(scroll_window, 1), S(start_border, 8), S(wirelessicons, 2),
};
#undef S

static std::vector<GuiImage> s_img[GUI_GFX_COUNT];
static bool s_tried[GUI_GFX_COUNT];
static const GuiImage s_none;

static u16 toRgb5a3(const u8 *p)
{
	if (p[3] >= 0xE0)   // opaque: 1rrrrrgggggbbbbb
		return 0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
	return ((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4);
}

static void load(int id)
{
	s_tried[id] = true;
	std::vector<u8> rgba;
	int w, h;
	if (!libPngDecode(kSrc[id].data, kSrc[id].end - kSrc[id].data, rgba, w, h))
		return;
	const int n = kSrc[id].frames, fh = h / n;
	std::vector<u16> px(w * fh);
	s_img[id].resize(n);
	for (int f = 0; f < n; f++) {
		for (int i = 0; i < w * fh; i++)
			px[i] = toRgb5a3(&rgba[(f * w * fh + i) * 4]);
		guiImageCreate(s_img[id][f], px.data(), w, fh);
	}
}

int guiGfxFrames(GuiGfx id)
{
	return id >= 0 && id < GUI_GFX_COUNT ? kSrc[id].frames : 0;
}

const GuiImage &guiGfx(GuiGfx id, int frame)
{
	if (id < 0 || id >= GUI_GFX_COUNT)
		return s_none;
	if (!s_tried[id])
		load(id);
	return frame >= 0 && frame < (int)s_img[id].size() ? s_img[id][frame] : s_none;
}
