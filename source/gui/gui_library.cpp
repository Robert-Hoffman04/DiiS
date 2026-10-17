/****************************************************************************
 * DiiS - gui_library.cpp
 *
 * The menu's home (menu rework phase 3): the Library screen (icon grid of
 * the scanned games, All / DS / GBA filter, Settings), the Game screen
 * (large icon, title lines, id, Play / Load state / Game settings) and
 * placeholders for Settings (with the About text) and Game settings until
 * phase 4. guiLibraryRun() returns the picked ROM and savestate slot; main()
 * boots it.
 *
 * -DDIIS_GUI_TEST (Dolphin screenshot builds) scans sd:/DiiStest when it
 * exists instead of the configured folders.
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>

#include "gui.h"
#include "../library/library.h"

static const GuiColor kText = guiRgba(236, 240, 248), kDim = guiRgba(150, 160, 180);
static const GuiColor kPanel = guiRgba(30, 36, 52), kPanelEdge = guiRgba(58, 68, 92);
static const GuiColor kAccent = guiRgba(56, 120, 200), kCell = guiRgba(24, 29, 42), kCellFocus = guiRgba(44, 56, 82);

#define NUM_SLOTS 10

// What the user picked (guiLibraryRun's result).
static std::string s_pickRom;
static int s_pickSlot;

struct LibEntry {
	const LibGame *game;
	GuiImage icon;   // empty: generic
};

static const GuiImage &entryIcon(const LibEntry &e)
{
	return e.icon.data ? e.icon : guiGenericIcon(e.game->info.isGba);
}

static void drawBackground()
{
	guiRectGradient(GuiRect(0, 0, GUI_W, GUI_H), guiRgba(26, 32, 48), guiRgba(10, 12, 20));
}

static void drawButton(const GuiRect &r, const char *label, bool primary)
{
	guiRoundRect(r, 10, primary ? kAccent : kPanel);
	guiTextBox(GUI_FONT_BODY, r, label, kText, GUI_ALIGN_CENTER);
}

static void drawHeader(const char *title)
{
	guiText(GUI_FONT_TITLE, 40, 20, "DiiS", kText);
	if (title)
		guiText(GUI_FONT_BODY, 112, 28, title, kDim);
}

//---------------------------------------------------------------------------
// Placeholders (phase 4 replaces them).
//---------------------------------------------------------------------------
class PlaceholderScreen : public GuiScreen {
public:
	PlaceholderScreen(const char *t, const char *m, bool about) : title(t), msg(m), showAbout(about) {}
	void enter()
	{
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, 0, GuiRect(40, 412, 120, 40));
	}
	void draw()
	{
		drawBackground();
		drawHeader(title);
		const GuiRect p(40, 72, 560, 96);
		guiPanel(p, 12, kPanel, kPanelEdge, 1);
		guiTextWrap(GUI_FONT_BODY, p.inset(14), msg, kDim);
		if (showAbout) {
			// The old console menu's credits ("Press B to see the credits").
			const GuiRect a(40, 180, 560, 216);
			guiPanel(a, 12, kPanel, kPanelEdge, 1);
			guiText(GUI_FONT_TITLE, a.x + 14, a.y + 8, "About DiiS", kText);
			guiTextWrap(GUI_FONT_BODY, GuiRect(a.x + 14, a.y + 48, a.w - 28, a.h - 56),
			            "http://code.google.com/p/desmumewii\n"
			            "Written by: Arikado (arikadosblog.blogspot.com), scanff, DCN, firnis, baby.lueshi\n"
			            "With contributions from Cyan\n"
			            "Based on DeSmuME.", kDim);
		}
		drawButton(focus.screenRect(0), "Back", true);
		guiFocusFrame(focus.highlight());
	}
	void activate(int) { guiPop(); }
private:
	const char *title, *msg;
	bool showAbout;
};

//---------------------------------------------------------------------------
// Game screen
//---------------------------------------------------------------------------
#define BTN_PLAY 0
#define BTN_SETTINGS 1
#define BTN_SLOT 100   // + slot

class GameScreen : public GuiScreen {
public:
	GameScreen(const LibEntry &e) : entry(e), nslots(0)
	{
		// Slots that exist in the game's folder (none until it has one).
		const std::string &dir = e.game->dir;
		for (int s = 0; !dir.empty() && s < NUM_SLOTS; s++) {
			char p[512];
			snprintf(p, sizeof(p), "%sstate%d.ds%d", dir.c_str(), s, s);
			struct stat st;
			if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			slot[nslots] = s;
			const time_t t = st.st_mtime;
			struct tm *tm = localtime(&t);
			if (tm)
				strftime(date[nslots], sizeof(date[0]), "%Y-%m-%d %H:%M", tm);
			else
				date[nslots][0] = 0;
			nslots++;
		}
	}

	void enter()
	{
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, BTN_PLAY, GuiRect(40, 236, 200, 48));
		focus.add(g, BTN_SETTINGS, GuiRect(256, 236, 200, 48));
		for (int i = 0; i < nslots; i++)
			focus.add(g, BTN_SLOT + slot[i], GuiRect(40 + (i % 5) * 114, 336 + (i / 5) * 64, 108, 56));
		if (focus.focused() < 0)
			focus.setFocus(BTN_PLAY, true);
	}

	void draw()
	{
		drawBackground();
		drawHeader(entry.game->info.isGba ? "GBA game" : "DS game");

		const GuiRect p(40, 68, 560, 152);
		guiPanel(p, 12, kPanel, kPanelEdge, 1);
		guiImage(entryIcon(entry), p.x + 12, p.y + 12, 4);
		const LibRomInfo &info = entry.game->info;
		const int tx = p.x + 160, tw = p.x + p.w - 14 - tx;
		for (int l = 0; l < LIB_TITLE_LINES; l++)
			guiText(GUI_FONT_BODY, tx, p.y + 14 + l * 28, info.lines[l].c_str(), l ? kDim : kText, GUI_ALIGN_LEFT, tw);
		guiText(GUI_FONT_BODY, tx, p.y + 14 + 3 * 28 + 10, info.id.c_str(), kDim, GUI_ALIGN_LEFT, tw);

		drawButton(focus.screenRect(BTN_PLAY), "Play", true);
		drawButton(focus.screenRect(BTN_SETTINGS), "Game settings", false);

		guiText(GUI_FONT_BODY, 40, 300, "Load state", kText);
		if (!nslots)
			guiText(GUI_FONT_BODY, 160, 300, "none saved yet", kDim);
		for (int i = 0; i < nslots; i++) {
			const GuiRect r = focus.screenRect(BTN_SLOT + slot[i]);
			char label[16];
			snprintf(label, sizeof(label), "Slot %d", slot[i]);
			guiRoundRect(r, 10, kPanel);
			guiTextBox(GUI_FONT_BODY, GuiRect(r.x, r.y + 4, r.w, 24), label, kText, GUI_ALIGN_CENTER);
			guiTextBox(GUI_FONT_BODY, GuiRect(r.x, r.y + 28, r.w, 22), date[i] + 5, kDim, GUI_ALIGN_CENTER);   // MM-DD HH:MM
		}
		guiFocusFrame(focus.highlight());
	}

	void activate(int id)
	{
		if (id == BTN_SETTINGS) {
			guiPush(new PlaceholderScreen("Game settings", "Per-game settings arrive in the next update.", false));
			return;
		}
		s_pickRom = entry.game->path;
		s_pickSlot = id >= BTN_SLOT ? id - BTN_SLOT : -1;
		guiQuit(0);
	}

private:
	LibEntry entry;
	int nslots;
	int slot[NUM_SLOTS];
	char date[NUM_SLOTS][20];
};

//---------------------------------------------------------------------------
// Library screen (home)
//---------------------------------------------------------------------------
#define GRID_COLS 7
#define GRID_ROWS 4
#define CELL_W 80
#define CELL_H 76
#define TAB_BASE 1000
#define TAB_SETTINGS (TAB_BASE + 3)
static const char *const kTabs[4] = { "All", "DS", "GBA", "Settings" };

class LibraryScreen : public GuiScreen {
public:
	LibraryScreen() : filter(0), grid(0)
	{
		const std::vector<LibGame> &games = libScan(false);   // sorted by first title line
		for (size_t i = 0; i < games.size(); i++) {
			LibEntry e;
			e.game = &games[i];
			if (games[i].hasIcon)
				guiImageCreate(e.icon, games[i].icon, LIB_ICON_W, LIB_ICON_H);
			all.push_back(e);
		}
	}
	~LibraryScreen()
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
		grid = focus.addGroup(GuiRect(40, 68, GRID_COLS * CELL_W, GRID_ROWS * CELL_H), CELL_H);
		shown.clear();
		for (size_t i = 0; i < all.size(); i++) {
			const bool gba = all[i].game->info.isGba;
			if (filter == 0 || (filter == 1 && !gba) || (filter == 2 && gba))
				shown.push_back((int)i);
		}
		for (size_t n = 0; n < shown.size(); n++)
			focus.add(grid, shown[n], GuiRect((n % GRID_COLS) * CELL_W + 2, (n / GRID_COLS) * CELL_H + 2, CELL_W - 4, CELL_H - 4));
		if (!focus.has(focus.focused()))
			focus.setFocus(shown.empty() ? TAB_BASE + filter : shown[0], true);
	}

	void update(const GuiInput &in)
	{
		if (in.down & GUI_BTN_PLUS) focus.scrollBy(grid, 1);
		if (in.down & GUI_BTN_MINUS) focus.scrollBy(grid, -1);
	}

	void activate(int id)
	{
		if (id == TAB_SETTINGS) {
			guiPush(new PlaceholderScreen("Settings",
			        "Global settings and ROM folders arrive in the next update. Until then, ROM folders "
			        "are the folderN= lines in DiiS/config.ini.", true));
		} else if (id >= TAB_BASE) {
			filter = id - TAB_BASE;
			enter();
		} else {
			guiPush(new GameScreen(all[id]));
		}
	}

	bool back() { return false; }   // the root screen: Home leaves

	void draw()
	{
		drawBackground();
		drawHeader("Library");
		for (int t = 0; t < 4; t++) {
			const GuiRect r = focus.screenRect(TAB_BASE + t);
			guiRoundRect(r, 10, t == filter ? kAccent : kPanel);
			guiTextBox(GUI_FONT_BODY, r, kTabs[t], kText, GUI_ALIGN_CENTER);
		}

		// Grid, clipped to its view; a scroll bar when it scrolls.
		const GuiRect view = focus.view(grid);
		if (shown.empty()) {
			guiPanel(view, 12, kPanel, kPanelEdge, 1);
			const char *msg = !all.empty() ? (filter == 1 ? "No DS games in the library." : "No GBA games in the library.") :
			                  "No games found.\n\nAdd the folders that hold your .nds and .gba files in Settings > ROM folders.";
			guiTextWrap(GUI_FONT_BODY, GuiRect(view.x + 40, view.y + 100, view.w - 80, view.h - 120), msg, kDim, GUI_ALIGN_CENTER);
		}
		guiPushClip(view);
		for (size_t n = 0; n < shown.size(); n++) {
			const int id = shown[n];
			if (!focus.visible(id))
				continue;
			const GuiRect r = focus.screenRect(id);
			guiRoundRect(r, 10, focus.isFocused(id) ? kCellFocus : kCell);
			guiImage(entryIcon(all[id]), r.x + (r.w - 64) / 2, r.y + (r.h - 64) / 2, 2);
		}
		guiPopClip();
		const int rows = ((int)shown.size() + GRID_COLS - 1) / GRID_COLS;
		if (rows > GRID_ROWS) {
			const int track = view.h, thumb = track * GRID_ROWS / rows;
			const int y = view.y + (track - thumb) * focus.scroll(grid) / ((rows - GRID_ROWS) * CELL_H);
			guiRoundRect(GuiRect(view.x + view.w + 8, view.y, 6, track), 3, guiRgba(36, 42, 58));
			guiRoundRect(GuiRect(view.x + view.w + 8, y, 6, thumb), 3, kDim);
		}
		if (focus.visible(focus.focused()))
			guiFocusFrame(focus.highlight());

		// Focused game: its three title lines as stored, and its id.
		const GuiRect info(40, 380, 560, 88);
		guiPanel(info, 12, kPanel, kPanelEdge, 1);
		const int f = focus.focused();
		if (f >= 0 && f < TAB_BASE) {
			const LibRomInfo &ri = all[f].game->info;
			const int idW = guiTextWidth(GUI_FONT_BODY, ri.id.c_str());
			for (int l = 0; l < LIB_TITLE_LINES; l++)
				guiText(GUI_FONT_BODY, info.x + 14, info.y + 4 + l * 27, ri.lines[l].c_str(), l ? kDim : kText,
				        GUI_ALIGN_LEFT, info.w - 28 - idW - 16);
			guiText(GUI_FONT_BODY, info.x + info.w - 14, info.y + 4, ri.id.c_str(), kDim, GUI_ALIGN_RIGHT);
		} else if (f == TAB_SETTINGS) {
			guiTextBox(GUI_FONT_BODY, info.inset(14), "Settings, ROM folders, About", kDim, GUI_ALIGN_CENTER);
		} else if (f >= TAB_BASE && !shown.empty()) {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d game%s shown", (int)shown.size(), shown.size() == 1 ? "" : "s");
			guiTextBox(GUI_FONT_BODY, info.inset(14), buf, kDim, GUI_ALIGN_CENTER);
		}
	}

private:
	std::vector<LibEntry> all;
	std::vector<int> shown;
	int filter;   // 0 all, 1 DS, 2 GBA
	int grid;
};

int guiLibraryRun(std::string &rom, int &stateSlot)
{
#ifdef DIIS_GUI_TEST
	// The Dolphin screenshot setup's own folder, if it's there.
	DIR *d = opendir("sd:/DiiStest");
	if (d) {
		closedir(d);
		libConfig().folders.clear();
		LibFolder f = { "sd:/DiiStest", false };
		libConfig().folders.push_back(f);
	}
#endif
	s_pickRom.clear();
	s_pickSlot = -1;
	const int r = guiRun(new LibraryScreen);
	if (r == GUI_EXIT || s_pickRom.empty())
		return GUI_EXIT;
	rom = s_pickRom;
	stateSlot = s_pickSlot;
	return 0;
}
