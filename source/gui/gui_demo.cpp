/****************************************************************************
 * DiiS - gui_demo.cpp
 *
 * GUI framework demo / test screen (-DDIIS_GUI_DEMO, see main.cpp): the
 * library scan as an icon grid with the focused game's three title lines,
 * a filter bar, text samples in both fonts, and a dialog on A. Placeholder
 * entries pad the grid past two rows so it scrolls. Scans sd:/DiiSdemo when
 * it exists (the Dolphin screenshot setup), else the configured folders.
 ***************************************************************************/
#ifdef DIIS_GUI_DEMO

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <string>

#include "gui.h"
#include "../library/library.h"

static const GuiColor kText = guiRgba(236, 240, 248), kDim = guiRgba(150, 160, 180);
static const GuiColor kPanel = guiRgba(30, 36, 52), kPanelEdge = guiRgba(58, 68, 92);
static const GuiColor kAccent = guiRgba(56, 120, 200);

struct DemoEntry {
	std::string lines[LIB_TITLE_LINES];
	std::string id;
	bool gba;
	GuiImage icon;   // empty: generic
};

#define GRID_COLS 7
#define CELL_W 80
#define CELL_H 76
#define TAB_BASE 1000
static const char *const kTabs[4] = { "All", "DS", "GBA", "Settings" };

class DemoDialog : public GuiScreen {
public:
	DemoDialog(const std::string &t) : text(t) {}
	void enter()
	{
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, 0, GuiRect(260, 282, 120, 40));
	}
	void draw()
	{
		guiRect(GuiRect(0, 0, GUI_W, GUI_H), guiRgba(0, 0, 0, 140));
		guiPanel(GuiRect(120, 150, 400, 190), 14, kPanel, kPanelEdge, 2);
		guiTextBox(GUI_FONT_TITLE, GuiRect(140, 166, 360, 40), "You picked", kText, GUI_ALIGN_CENTER);
		guiTextWrap(GUI_FONT_BODY, GuiRect(140, 212, 360, 56), text.c_str(), kDim, GUI_ALIGN_CENTER);
		const GuiRect b = focus.screenRect(0);
		guiRoundRect(b, 10, kAccent);
		guiTextBox(GUI_FONT_BODY, b, "OK", kText, GUI_ALIGN_CENTER);
		guiFocusFrame(focus.highlight());
	}
	void activate(int) { guiPop(); }
	bool opaque() const { return false; }
private:
	std::string text;
};

class DemoScreen : public GuiScreen {
public:
	DemoScreen() : filter(0), grid(0)
	{
		const std::vector<LibGame> &games = libScan(false);
		for (size_t i = 0; i < games.size(); i++) {
			DemoEntry e;
			for (int l = 0; l < LIB_TITLE_LINES; l++)
				e.lines[l] = games[i].info.lines[l];
			e.id = games[i].info.id;
			e.gba = games[i].info.isGba;
			if (games[i].hasIcon)
				guiImageCreate(e.icon, games[i].icon, LIB_ICON_W, LIB_ICON_H);
			all.push_back(e);
		}
		for (int i = (int)all.size(); i < 24; i++) {
			DemoEntry e;
			char buf[32];
			snprintf(buf, sizeof(buf), "Placeholder %d", i + 1);
			e.lines[0] = buf;
			e.lines[1] = "DiiS GUI demo";
			e.id = "----";
			e.gba = i & 1;
			all.push_back(e);
		}
	}
	~DemoScreen()
	{
		for (size_t i = 0; i < all.size(); i++)
			guiImageFree(all[i].icon);
	}

	void enter()
	{
		focus.clear();
		const int bar = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		// Sized to their labels, right-aligned to the grid's edge.
		int x = 40 + GRID_COLS * CELL_W;
		for (int t = 3; t >= 0; t--) {
			const int w = guiTextWidth(GUI_FONT_BODY, kTabs[t]) + 32;
			x -= w;
			focus.add(bar, TAB_BASE + t, GuiRect(x, 22, w, 36));
			x -= 8;
		}
		grid = focus.addGroup(GuiRect(40, 72, GRID_COLS * CELL_W, 2 * CELL_H), CELL_H);
		shown.clear();
		for (size_t i = 0; i < all.size(); i++)
			if (filter == 0 || (filter == 1 && !all[i].gba) || (filter == 2 && all[i].gba))
				shown.push_back((int)i);
		for (size_t n = 0; n < shown.size(); n++)
			focus.add(grid, shown[n], GuiRect((n % GRID_COLS) * CELL_W + 2, (n / GRID_COLS) * CELL_H + 2, CELL_W - 4, CELL_H - 4));
		if (focus.focused() < 0 && !shown.empty())
			focus.setFocus(shown[0], true);
	}

	void update(const GuiInput &in)
	{
		if (in.down & GUI_BTN_PLUS) focus.scrollBy(grid, 1);
		if (in.down & GUI_BTN_MINUS) focus.scrollBy(grid, -1);
		// Y steps a warped pointer through a few spots (pointer hover / hit
		// test without a Wiimote, e.g. Gecko-driven Dolphin runs).
		if (in.down & GUI_BTN_Y) {
			static const int kSpots[4][2] = { { 330, 190 }, { 170, 120 }, { 470, 40 }, { 620, 300 } };
			static int n = 0;
			guiWarpPointer(kSpots[n][0], kSpots[n][1]);
			n = (n + 1) & 3;
		}
	}

	void activate(int id)
	{
		if (id >= TAB_BASE) {
			if (id - TAB_BASE < 3) {
				filter = id - TAB_BASE;
				enter();
			}
			return;
		}
		const DemoEntry &e = all[id];
		std::string t = e.lines[0];
		for (int l = 1; l < LIB_TITLE_LINES; l++)
			if (!e.lines[l].empty())
				t += "\n" + e.lines[l];
		guiPush(new DemoDialog(t));
	}

	bool back() { return false; }   // the root screen: Home leaves

	void draw()
	{
		guiRectGradient(GuiRect(0, 0, GUI_W, GUI_H), guiRgba(26, 32, 48), guiRgba(10, 12, 20));

		// Top bar.
		guiText(GUI_FONT_TITLE, 40, 20, "DiiS", kText);
		guiText(GUI_FONT_BODY, 112, 28, "GUI demo", kDim);
		for (int t = 0; t < 4; t++) {
			const GuiRect r = focus.screenRect(TAB_BASE + t);
			guiRoundRect(r, 10, t == filter ? kAccent : kPanel);
			guiTextBox(GUI_FONT_BODY, r, kTabs[t], kText, GUI_ALIGN_CENTER);
		}

		// Grid, clipped to its view; a scroll bar when it scrolls.
		const GuiRect view = focus.view(grid);
		guiPushClip(view);
		for (size_t n = 0; n < shown.size(); n++) {
			const int id = shown[n];
			if (!focus.visible(id))
				continue;
			const GuiRect r = focus.screenRect(id);
			guiRoundRect(r, 10, focus.isFocused(id) ? guiRgba(44, 56, 82) : guiRgba(24, 29, 42));
			const DemoEntry &e = all[id];
			guiImage(e.icon.data ? e.icon : guiGenericIcon(e.gba), r.x + (r.w - 64) / 2, r.y + (r.h - 64) / 2, 2);
		}
		guiPopClip();
		const int rows = ((int)shown.size() + GRID_COLS - 1) / GRID_COLS;
		if (rows > 2) {
			const int track = view.h, thumb = track * 2 / rows;
			const int y = view.y + (track - thumb) * focus.scroll(grid) / ((rows - 2) * CELL_H);
			guiRoundRect(GuiRect(view.x + view.w + 8, view.y, 6, track), 3, guiRgba(36, 42, 58));
			guiRoundRect(GuiRect(view.x + view.w + 8, y, 6, thumb), 3, kDim);
		}
		if (focus.visible(focus.focused()))
			guiFocusFrame(focus.highlight());

		// Focused game: icon and its three title lines as stored.
		const GuiRect info(40, 236, 560, 92);
		guiPanel(info, 12, kPanel, kPanelEdge, 1);
		const int f = focus.focused();
		if (f >= 0 && f < TAB_BASE) {
			const DemoEntry &e = all[f];
			guiImage(e.icon.data ? e.icon : guiGenericIcon(e.gba), info.x + 14, info.y + 14, 2);
			for (int l = 0; l < LIB_TITLE_LINES; l++)
				guiText(GUI_FONT_BODY, info.x + 92, info.y + 8 + l * 26, e.lines[l].c_str(), l ? kDim : kText,
				        GUI_ALIGN_LEFT, info.w - 92 - 110);
			guiText(GUI_FONT_BODY, info.x + info.w - 14, info.y + 8, e.id.c_str(), kDim, GUI_ALIGN_RIGHT);
		} else if (f >= TAB_BASE) {
			guiTextBox(GUI_FONT_BODY, info.inset(14), "Filter / settings buttons (A to pick)", kDim, GUI_ALIGN_CENTER);
		}

		// Text samples.
		const GuiRect s(40, 336, 560, 124);
		guiPanel(s, 12, kPanel, kPanelEdge, 1);
		guiText(GUI_FONT_TITLE, s.x + 14, s.y + 4, "Title 28: Pok\xC3\xA9mon\xE2\x84\xA2 Mario\xC2\xAE \xC2\xA9 AVA Tokyo", kText, GUI_ALIGN_LEFT, s.w - 28);
		guiText(GUI_FONT_BODY, s.x + 14, s.y + 40, "Body 20: \xC3\x80\xC3\xA9\xC3\xAE\xC3\xB5\xC3\xBC \xC3\xB1 \xC3\x9F \xC5\x92 \xE2\x80\x9Cquotes\xE2\x80\x9D \xE2\x80\x94 \xE2\x82\xAC 1/2 \xC2\xBD \xC2\xA9\xC2\xAE\xE2\x84\xA2", kText, GUI_ALIGN_LEFT, s.w - 28);
		guiText(GUI_FONT_BODY, s.x + 14, s.y + 66, "Missing: \xE6\x97\xA5\xE6\x9C\xAC \xE2\x86\x92 boxes; \xEF\xBC\xA6\xEF\xBD\x95\xEF\xBD\x8C\xEF\xBD\x8C-width \xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3", kDim, GUI_ALIGN_LEFT, s.w - 28);
		const char *kLong = "A long line cut with an ellipsis at the box edge";
		for (int a = 0; a < 3; a++) {
			const GuiRect b(s.x + 14 + a * 180, s.y + 94, 168, 24);
			guiRect(b, guiRgba(22, 26, 38));
			guiTextBox(GUI_FONT_BODY, b, a == 2 ? kLong : a ? "Centre" : "Left", kText, a == 1 ? GUI_ALIGN_CENTER : GUI_ALIGN_LEFT);
		}
	}

private:
	std::vector<DemoEntry> all;
	std::vector<int> shown;
	int filter;   // 0 all, 1 DS, 2 GBA
	int grid;
};

int guiDemoRun()
{
	// The screenshot setup's own folder, if it's there.
	DIR *d = opendir("sd:/DiiSdemo");
	if (d) {
		closedir(d);
		libConfig().folders.clear();
		LibFolder f = { "sd:/DiiSdemo", false };
		libConfig().folders.push_back(f);
	}
	return guiRun(new DemoScreen);
}

#endif // DIIS_GUI_DEMO
