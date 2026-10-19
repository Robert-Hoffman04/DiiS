/****************************************************************************
 * DiiS - gui_library.cpp
 *
 * The menu's home (menu rework phase 3): the Library screen (icon grid of
 * the scanned games, All / DS / GBA filter, Settings) and the game panel
 * (its dialog box grown up over the grid: Play, the game's CPU / render
 * overrides, Load state). Laid out in the look of the DS system menu, 1:1:
 * the games page's 4-wide grid of placeholder boxes over a dialog box with
 * the focused game's title, which grows like the DS menu's options box.
 * Settings is gui_settings.cpp. guiLibraryRun() returns the picked ROM and
 * savestate slot; main() boots it.
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

#include "gui_menu.h"

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

//---------------------------------------------------------------------------
// Library screen (home)
//---------------------------------------------------------------------------
// The DS menu's games page: 64x64 placeholder boxes (art at 7..57) 64
// apart across, 60 down, 4 x 2 on screen.
#define GRID_COLS 4
#define GRID_ROWS 2
#define CELL_W 64
#define CELL_H 60
#define BOX_ART 7     // the box art's inset in its 64x64 sprite
#define BOX_SIZE 50   // and its size; the focus rect
#define GRID_X ((GUI_W - GRID_COLS * CELL_W) / 2)
#define GRID_Y 52
#define SETTINGS_ICON 21   // icon_settings art: 21x21
#define TAB_BASE 1000
#define TAB_SETTINGS (TAB_BASE + 3)
static const char *const kTabs[4] = { "All", "DS", "GBA", "Settings" };

// The games page's dialog box, anchored to the bottom safe edge: closed
// under the grid; the game panel grows it up to just under the top bar.
#define DLG_BOTTOM (GUI_H - GUI_SAFE_Y)
#define DLG_CLOSED_H (DLG_BOTTOM - (GRID_Y + GRID_ROWS * CELL_H + 4))
#define DLG_OPEN_H (DLG_BOTTOM - (GUI_TOPBAR_H + 4))
// Opening / closing eases out: each frame covers this share of what's
// left (at least DLG_MIN_STEP px), about 18 frames end to end. Steps are
// even: guiDialogBox() draws even heights (stripes in phase), so an odd
// height would move the bottom edge by a pixel.
#define DLG_EASE 0.2f
#define DLG_MIN_STEP 2
#define DLG_HEADER_H 40
static const GuiColor kDlgDim = guiRgba(190, 190, 190);

static GuiRect dialogRect(int h)
{
	return GuiRect(6, DLG_BOTTOM - h, GUI_W - 12, h);
}

// A game's icon and title lines (the font's own height apart, as the DS
// menu's title box, the block centred on the icon) and its id, in the
// header strip at the top of the dialog box.
static void drawGameHeader(const LibEntry &e, const GuiRect &box)
{
	const GuiRect in(box.x + 12, box.y + 8, box.w - 24, DLG_HEADER_H);
	const LibRomInfo &ri = e.game->info;
	const GuiImage &icon = entryIcon(e);
	guiImage(icon, in.x, in.y + (in.h - icon.h) / 2);
	const int tx = in.x + icon.w + 8, tw = in.x + in.w - tx;
	const int idW = guiTextWidth(GUI_FONT_BODY, ri.id.c_str()), lh = guiFontHeight(GUI_FONT_BODY);
	const int ty = in.y + (in.h - LIB_TITLE_LINES * lh) / 2;
	for (int l = 0; l < LIB_TITLE_LINES; l++)
		guiText(GUI_FONT_BODY, tx, ty + l * lh, ri.lines[l].c_str(), l ? kDlgDim : GUI_COL_TEXT_LIGHT,
		        GUI_ALIGN_LEFT, tw - idW - 8);
	guiText(GUI_FONT_BODY, in.x + in.w, in.y, ri.id.c_str(), kDlgDim, GUI_ALIGN_RIGHT);
}

//---------------------------------------------------------------------------
// Game panel: the Library's dialog box grown up over the grid (the DS
// menu's per-game options box): Play, the game's CPU / render overrides,
// its savestates. A dialog over the Library; B shrinks it back.
//---------------------------------------------------------------------------
#define GP_PLAY 0
#define GP_CPU 1
#define GP_RENDER 2
#define GP_SLOT 100   // + slot
#define GP_LABEL_W 100

class GamePanel : public GuiScreen {
public:
	GamePanel(const LibEntry &e) : entry(e), h(DLG_CLOSED_H), closing(false), nslots(0)
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
				strftime(date[nslots], sizeof(date[0]), "%m-%d %H:%M", tm);
			else
				date[nslots][0] = 0;
			nslots++;
		}

		ov.cpu = LIB_CPU_DEFAULT;
		ov.render = LIB_RENDER_DEFAULT;
		if (!dir.empty())
			libLoadGameSettings(dir, ov);
		const LibConfig &c = libConfig();
#ifdef DIIS_JIT
		cpu.add(std::string("Default (") + libCpuName(c.cpu) + ")");
		menuAddCpuValues(cpu);
		cpu.sel = ov.cpu;   // DEFAULT, JIT, INTERP
#else
		menuAddCpuValues(cpu);   // Interpreter only: nothing to override
		ov.cpu = LIB_CPU_DEFAULT;
#endif
		render.add(std::string("Default (") + libRenderName(c.render) + ")");
		for (int r = LIB_RENDER_SOFTWARE; r <= LIB_RENDER_GXFAST; r++)
			render.add(libRenderName((LibRender)r));
		render.sel = ov.render;   // DEFAULT, SOFTWARE, GXACCURATE, GXFAST
	}

	bool opaque() const { return false; }

	void enter()
	{
		// Laid out in the open box.
		const GuiRect b = dialogRect(DLG_OPEN_H);
		const int x = b.x + 12, w = b.w - 24, y = b.y + 8 + DLG_HEADER_H;
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, GP_PLAY, GuiRect(x, y + 2, 100, 22));
		focus.add(g, GP_CPU, GuiRect(x, y + 28, w, 20));
		focus.add(g, GP_RENDER, GuiRect(x, y + 50, w, 20));
		for (int i = 0; i < nslots; i++)
			focus.add(g, GP_SLOT + slot[i], GuiRect(x + (i % 5) * 57, y + 89 + (i / 5) * 29, 55, 26));
		if (!focus.has(focus.focused()))
			focus.setFocus(GP_PLAY, true);
	}

	void update(const GuiInput &)
	{
		const int target = closing ? DLG_CLOSED_H : DLG_OPEN_H;
		const int left = target - h, d = (int)(left * DLG_EASE) & ~1;
		if (left > 0)
			h += d > DLG_MIN_STEP ? d : DLG_MIN_STEP < left ? DLG_MIN_STEP : left;
		else if (left < 0)
			h += d < -DLG_MIN_STEP ? d : -DLG_MIN_STEP > left ? -DLG_MIN_STEP : left;
		if (closing && h == DLG_CLOSED_H)
			guiPop();
	}

	void draw()
	{
		const GuiRect b = dialogRect(h);
		guiDialogBox(b);
		drawGameHeader(entry, b);

		// The items are laid out in the open box; they ride its top edge
		// (sliding up into view as it grows, down as it shrinks), clipped
		// to its inside.
		const int dy = b.y - dialogRect(DLG_OPEN_H).y;
		const int clipH = b.h - 16 - DLG_HEADER_H;
		if (clipH <= 0)
			return;
		guiPushClip(GuiRect(b.x, b.y + 8 + DLG_HEADER_H, b.w, clipH));
		guiButton(at(GP_PLAY), "Play", true);
		if (!error.empty()) {
			const GuiRect p = at(GP_PLAY);
			guiTextBox(GUI_FONT_BODY, GuiRect(p.x + p.w + 8, p.y, b.x + b.w - 12 - p.x - p.w - 8, p.h), error.c_str(), kDlgDim);
		}
		guiChoiceDraw(at(GP_CPU), "CPU", cpu, focus.isFocused(GP_CPU), ov.cpu != LIB_CPU_DEFAULT, GP_LABEL_W);
		guiChoiceDraw(at(GP_RENDER), "Render mode", render, focus.isFocused(GP_RENDER),
		              ov.render != LIB_RENDER_DEFAULT, GP_LABEL_W);

		const GuiRect r0 = at(GP_RENDER);
		guiText(GUI_FONT_BODY, r0.x, r0.y + r0.h + 6, "Load state", GUI_COL_TEXT_LIGHT);
		if (!nslots)
			guiText(GUI_FONT_BODY, r0.x + 60, r0.y + r0.h + 6, "none saved yet", kDlgDim);
		for (int i = 0; i < nslots; i++) {
			const GuiRect r = at(GP_SLOT + slot[i]);
			char label[16];
			snprintf(label, sizeof(label), "Slot %d", slot[i]);
			guiButton(r, "", false);
			guiTextBox(GUI_FONT_BODY, GuiRect(r.x, r.y + 2, r.w, 11), label, GUI_COL_TEXT, GUI_ALIGN_CENTER);
			guiTextBox(GUI_FONT_BODY, GuiRect(r.x, r.y + 13, r.w, 11), date[i], GUI_COL_DIM, GUI_ALIGN_CENTER);
		}
		const GuiRect hl = focus.highlight();
		guiFocusFrame(GuiRect(hl.x, hl.y + dy, hl.w, hl.h));
		guiPopClip();
	}

	bool step(int id, int dir)
	{
		if (!isOpen() || (id != GP_CPU && id != GP_RENDER))
			return !isOpen();   // no navigation while it moves
		change(id, dir);
		return true;
	}

	void activate(int id)
	{
		if (!isOpen())
			return;
		if (id == GP_CPU || id == GP_RENDER) {
			change(id, guiChoiceClick(focus.screenRect(id), GP_LABEL_W));
			return;
		}
		s_pickRom = entry.game->path;
		s_pickSlot = id >= GP_SLOT ? id - GP_SLOT : -1;
		guiQuit(0);
	}

	bool back()
	{
		closing = true;   // update() shrinks it, then pops
		return false;
	}

private:
	LibEntry entry;
	int h;          // the box's current height
	bool closing;
	int nslots;
	int slot[NUM_SLOTS];
	char date[NUM_SLOTS][20];
	LibGameSettings ov;
	GuiChoice cpu, render;
	std::string error;

	bool isOpen() const { return !closing && h == DLG_OPEN_H; }

	// An item's rect where it is drawn: moved with the box's top edge.
	GuiRect at(int id) const
	{
		GuiRect r = focus.screenRect(id);
		r.y += dialogRect(h).y - dialogRect(DLG_OPEN_H).y;
		return r;
	}

	void change(int id, int dir)
	{
		if (id == GP_CPU) {
			if (!cpu.step(dir))
				return;
			ov.cpu = (LibCpu)cpu.sel;
		} else {
			render.step(dir);
			ov.render = (LibRender)render.sel;
		}
		error = libSaveGameOverrides(*entry.game, ov) ? "" : "Could not write game.ini.";
	}
};

class LibraryScreen : public GuiScreen {
public:
	LibraryScreen() : filter(0), grid(0)
	{
		load(libScan(false));   // sorted by first title line
	}
	~LibraryScreen() { unload(); }

	void resume()
	{
		// Settings changed the ROM folders or rescanned: the entries point
		// into the old scan.
		if (generation != libGeneration()) {
			unload();
			load(libScan(false));
			enter();
		}
	}

	void enter()
	{
		focus.clear();
		const int bar = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		// Filters sized to their labels from the left; Settings is the DS
		// menu's spanner icon at the right.
		int x = GRID_X;
		for (int t = 0; t < 3; t++) {
			const int w = guiTextWidth(GUI_FONT_BODY, kTabs[t]) + 16;
			focus.add(bar, TAB_BASE + t, GuiRect(x, 29, w, 18));
			x += w + 6;
		}
		focus.add(bar, TAB_SETTINGS, GuiRect(GRID_X + GRID_COLS * CELL_W - SETTINGS_ICON, 27, SETTINGS_ICON, SETTINGS_ICON));
		grid = focus.addGroup(GuiRect(GRID_X, GRID_Y, GRID_COLS * CELL_W, GRID_ROWS * CELL_H), CELL_H);
		shown.clear();
		for (size_t i = 0; i < all.size(); i++) {
			const bool gba = all[i].game->info.isGba;
			if (filter == 0 || (filter == 1 && !gba) || (filter == 2 && gba))
				shown.push_back((int)i);
		}
		for (size_t n = 0; n < shown.size(); n++)
			focus.add(grid, shown[n], GuiRect((n % GRID_COLS) * CELL_W + BOX_ART, (n / GRID_COLS) * CELL_H + BOX_ART, BOX_SIZE, BOX_SIZE));
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
			guiPush(newSettingsScreen());
		} else if (id >= TAB_BASE) {
			filter = id - TAB_BASE;
			enter();
		} else {
			guiPush(new GamePanel(all[id]));
		}
	}

	bool back() { return false; }   // the root screen: Home leaves

	void draw()
	{
		menuBackground();
		menuHeader("Library");
		for (int t = 0; t < 3; t++)
			guiButton(focus.screenRect(TAB_BASE + t), kTabs[t], t == filter);
		const GuiRect sr = focus.screenRect(TAB_SETTINGS);
		guiImage(guiGfx(GUI_GFX_ICON_SETTINGS, focus.isFocused(TAB_SETTINGS) ? 1 : 0), sr.x, sr.y, GUI_DS_SCALE);

		// Grid, clipped to its view; a scroll bar when it scrolls.
		const GuiRect view = focus.view(grid);
		if (shown.empty()) {
			const char *msg = !all.empty() ? (filter == 1 ? "No DS games in the library." : "No GBA games in the library.") :
			                  "No games found.\n\nAdd the folders that hold your .nds and .gba files in Settings > ROM folders.";
			guiDialogBox(view);
			guiTextWrap(GUI_FONT_BODY, GuiRect(view.x + 20, view.y + 35, view.w - 40, view.h - 45), msg, GUI_COL_TEXT_LIGHT, GUI_ALIGN_CENTER);
		}
		guiPushClip(GuiRect(0, view.y, GUI_W, view.h));
		for (size_t n = 0; n < shown.size(); n++) {
			const int id = shown[n];
			if (!focus.visible(id))
				continue;
			const GuiRect r = focus.screenRect(id);
			guiImage(guiGfx(GUI_GFX_BOXART_PLACEHOLDER), r.x - BOX_ART, r.y - BOX_ART, GUI_DS_SCALE);
			guiImage(entryIcon(all[id]), r.x + (r.w - 32 * GUI_DS_SCALE) / 2, r.y + (r.h - 32 * GUI_DS_SCALE) / 2, GUI_DS_SCALE);
		}
		guiPopClip();
		const int rows = ((int)shown.size() + GRID_COLS - 1) / GRID_COLS;
		if (rows > GRID_ROWS) {
			const int track = view.h, thumb = track * GRID_ROWS / rows;
			const int y = view.y + (track - thumb) * focus.scroll(grid) / ((rows - GRID_ROWS) * CELL_H);
			guiRect(GuiRect(view.x + view.w + 4, view.y, 3, track), guiRgba(200, 200, 200));
			guiRect(GuiRect(view.x + view.w + 4, y, 3, thumb), GUI_COL_ACCENT);
		}
		if (focus.visible(focus.focused()))
			guiFocusFrame(focus.highlight());

		// The games page's dialog box: the focused game's title lines as
		// stored, and its id.
		const GuiRect info = dialogRect(DLG_CLOSED_H);
		guiDialogBox(info);
		const GuiRect in = GuiRect(info.x + 12, info.y + 8, info.w - 24, info.h - 16);
		const int f = focus.focused();
		if (f >= 0 && f < TAB_BASE) {
			drawGameHeader(all[f], info);
		} else if (f == TAB_SETTINGS) {
			guiTextBox(GUI_FONT_BODY, in, "Settings, ROM folders, About", GUI_COL_TEXT_LIGHT, GUI_ALIGN_CENTER);
		} else if (f >= TAB_BASE && !shown.empty()) {
			char buf[64];
			snprintf(buf, sizeof(buf), "%d game%s shown", (int)shown.size(), shown.size() == 1 ? "" : "s");
			guiTextBox(GUI_FONT_BODY, in, buf, GUI_COL_TEXT_LIGHT, GUI_ALIGN_CENTER);
		}
	}

private:
	std::vector<LibEntry> all;
	std::vector<int> shown;
	int filter;   // 0 all, 1 DS, 2 GBA
	int grid;
	unsigned generation;

	void load(const std::vector<LibGame> &games)
	{
		generation = libGeneration();
		for (size_t i = 0; i < games.size(); i++) {
			LibEntry e;
			e.game = &games[i];
			if (games[i].hasIcon)
				guiImageCreate(e.icon, games[i].icon, LIB_ICON_W, LIB_ICON_H);
			all.push_back(e);
		}
	}
	void unload()
	{
		for (size_t i = 0; i < all.size(); i++)
			guiImageFree(all[i].icon);
		all.clear();
	}
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
