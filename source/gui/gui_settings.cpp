/****************************************************************************
 * DiiS - gui_settings.cpp
 *
 * The menu's settings screens (menu rework phase 4):
 *   Settings       global CPU / render defaults, the ROM folder list (Add,
 *                  Remove, include-subfolders switch, Rescan), About.
 *   Game settings  one game's CPU / render, "Default (<global>)" first;
 *                  only overrides go to its game.ini.
 *   Folder picker  directory-only browser from sd:/ (and usb:/ when it
 *                  mounts) for Settings > Add folder.
 * Every change is saved at once (config.ini / game.ini); a folder change
 * rescans the library, and the Library screen reloads when it's back on
 * top (libGeneration()).
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fat.h>
#include <ogc/usbstorage.h>
#include <algorithm>
#include <string>
#include <vector>

#include "gui_menu.h"

static const GuiColor kText = GUI_COL_TEXT, kDim = GUI_COL_DIM;
static const GuiColor kPanel = GUI_COL_PANEL, kPanelEdge = GUI_COL_PANEL_EDGE;
static const GuiColor kRow = guiRgba(24, 29, 42), kRowFocus = guiRgba(44, 56, 82);

#define LABEL_W 220   // label column of the choice rows

static void addCpuValues(GuiChoice &c)
{
#ifdef DIIS_JIT
	c.add(libCpuName(LIB_CPU_JIT));
#endif
	c.add(libCpuName(LIB_CPU_INTERP));
}

//---------------------------------------------------------------------------
// About
//---------------------------------------------------------------------------
class AboutScreen : public GuiScreen {
public:
	void enter()
	{
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, 0, GuiRect(40, 412, 120, 44));
	}
	void draw()
	{
		menuBackground();
		menuHeader("About");
		// The old console menu's credits ("Press B to see the credits").
		const GuiRect a(40, 72, 560, 216);
		guiPanel(a, 12, kPanel, kPanelEdge, 1);
		guiText(GUI_FONT_TITLE, a.x + 14, a.y + 8, "About DiiS", kText);
		guiTextWrap(GUI_FONT_BODY, GuiRect(a.x + 14, a.y + 48, a.w - 28, a.h - 56),
		            "http://code.google.com/p/desmumewii\n"
		            "Written by: Arikado (arikadosblog.blogspot.com), scanff, DCN, firnis, baby.lueshi\n"
		            "With contributions from Cyan\n"
		            "Based on DeSmuME.", kDim);
		guiButton(focus.screenRect(0), "Back", true);
		guiFocusFrame(focus.highlight());
	}
	void activate(int) { guiPop(); }
};

//---------------------------------------------------------------------------
// Folder picker
//---------------------------------------------------------------------------
class SettingsScreen;
static void settingsAddFolder(SettingsScreen *s, const std::string &path);

#define PICK_ROWS 6
#define PICK_ROW_H 42
#define PICK_USE 1000
#define PICK_UP 1001
#define PICK_CANCEL 1002

static bool dirExists(const char *p)
{
	DIR *d = opendir(p);
	if (!d)
		return false;
	closedir(d);
	return true;
}

static bool nameLess(const std::string &a, const std::string &b)
{
	return strcasecmp(a.c_str(), b.c_str()) < 0;
}

class FolderPicker : public GuiScreen {
public:
	FolderPicker(SettingsScreen *o) : owner(o), list(0), nds(0), gba(0), files(0), pendingFocus(-2)
	{
#ifndef DIIS_GUI_TEST
		// usb: is only mounted at boot when a ROM folder is on it; one try
		// here so a stick plugged in for a new folder shows up.
		static bool triedUsb;
		if (!triedUsb && !dirExists("usb:/")) {
			triedUsb = true;
			fatMountSimple("usb", &__io_usbstorage);
		}
#endif
		open("", "");
	}

	void enter()
	{
		focus.clear();
		list = focus.addGroup(GuiRect(40, 108, 560, PICK_ROWS * PICK_ROW_H), PICK_ROW_H);
		for (size_t i = 0; i < dirs.size(); i++)
			focus.add(list, (int)i, GuiRect(0, (int)i * PICK_ROW_H, 560, PICK_ROW_H - 4));
		const int bar = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(bar, PICK_USE, GuiRect(40, 412, 200, 44), !cur.empty());
		focus.add(bar, PICK_UP, GuiRect(256, 412, 120, 44), !cur.empty());
		focus.add(bar, PICK_CANCEL, GuiRect(480, 412, 120, 44));
		if (pendingFocus != -2) {
			focus.setFocus(pendingFocus, true);
			pendingFocus = -2;
		}
	}

	void draw()
	{
		menuBackground();
		menuHeader("Add a ROM folder");
		const GuiRect p(40, 64, 560, 36);
		guiPanel(p, 10, kPanel, kPanelEdge, 1);
		guiTextBox(GUI_FONT_BODY, GuiRect(p.x + 12, p.y, p.w - 24, p.h), cur.empty() ? "Choose a device" : cur.c_str(), kText);

		const GuiRect view = focus.view(list);
		if (dirs.empty())
			guiTextBox(GUI_FONT_BODY, GuiRect(view.x, view.y, view.w, PICK_ROW_H), cur.empty() ? "No device found." : "No folders in here.", kDim, GUI_ALIGN_CENTER);
		guiPushClip(view);
		for (size_t i = 0; i < dirs.size(); i++) {
			if (!focus.visible((int)i))
				continue;
			const GuiRect r = focus.screenRect((int)i);
			guiRoundRect(r, 8, focus.isFocused((int)i) ? kRowFocus : kRow);
			guiTextBox(GUI_FONT_BODY, GuiRect(r.x + 14, r.y, r.w - 28, r.h), dirs[i].c_str(), kText);
		}
		guiPopClip();
		if (dirs.size() > PICK_ROWS) {
			const int rows = (int)dirs.size(), thumb = view.h * PICK_ROWS / rows;
			const int y = view.y + (view.h - thumb) * focus.scroll(list) / ((rows - PICK_ROWS) * PICK_ROW_H);
			guiRoundRect(GuiRect(view.x + view.w + 8, view.y, 6, view.h), 3, guiRgba(36, 42, 58));
			guiRoundRect(GuiRect(view.x + view.w + 8, y, 6, thumb), 3, kDim);
		}

		// What's in the current folder, to tell the right one.
		if (!cur.empty()) {
			char buf[128];
			snprintf(buf, sizeof(buf), "Here: %d .nds, %d .gba, %d other file%s, %d folder%s", nds, gba,
			         files, files == 1 ? "" : "s", (int)dirs.size(), dirs.size() == 1 ? "" : "s");
			guiText(GUI_FONT_BODY, 40, 372, buf, nds + gba ? kText : kDim, GUI_ALIGN_LEFT, 560);
		}
		guiButton(focus.screenRect(PICK_USE), "Use this folder", !cur.empty());
		guiButton(focus.screenRect(PICK_UP), "Up", false);
		guiButton(focus.screenRect(PICK_CANCEL), "Cancel", false);
		if (focus.focused() >= PICK_USE || focus.visible(focus.focused()))
			guiFocusFrame(focus.highlight());
	}

	void activate(int id)
	{
		if (id == PICK_USE) {
			settingsAddFolder(owner, cur);
			guiPop();
		} else if (id == PICK_UP) {
			up();
		} else if (id == PICK_CANCEL) {
			guiPop();
		} else if (id >= 0 && id < (int)dirs.size()) {
			open(cur.empty() ? dirs[id] : join(cur, dirs[id]), "");
			enter();
		}
	}

	bool back()
	{
		if (cur.empty())
			return true;
		up();
		return false;
	}

private:
	SettingsScreen *owner;
	std::string cur;                 // "" = the device list; "sd:/", "sd:/ROMS", ...
	std::vector<std::string> dirs;   // subfolders (the device list: "sd:/", "usb:/")
	int list;
	int nds, gba, files;
	int pendingFocus;                // applied by the next enter(); -2 = none

	static std::string join(const std::string &d, const std::string &n)
	{
		return d[d.size() - 1] == '/' ? d + n : d + "/" + n;
	}

	void up()
	{
		std::string parent, child;
		const size_t slash = cur.find_last_of('/');
		if (slash != std::string::npos && slash + 1 < cur.size()) {
			child = cur.substr(slash + 1);
			parent = cur.substr(0, slash);
			if (parent.find('/') == std::string::npos)
				parent += "/";   // "sd:" -> "sd:/"
		} else {
			child = cur;         // a device root: back to the device list
		}
		open(parent, child);
		enter();
	}

	// List `path`, focusing subfolder `focusName` (the one just left) or the first.
	void open(const std::string &path, const std::string &focusName)
	{
		cur = path;
		dirs.clear();
		nds = gba = files = 0;
		if (cur.empty()) {
			if (dirExists("sd:/")) dirs.push_back("sd:/");
			if (dirExists("usb:/")) dirs.push_back("usb:/");
		} else if (DIR *d = opendir(cur.c_str())) {
			while (struct dirent *e = readdir(d)) {
				if (e->d_name[0] == '.')
					continue;
				struct stat st;
				if (stat(join(cur, e->d_name).c_str(), &st) != 0)
					continue;
				if (S_ISDIR(st.st_mode)) {
					dirs.push_back(e->d_name);
					continue;
				}
				const char *ext = strrchr(e->d_name, '.');
				if (ext && !strcasecmp(ext, ".nds")) nds++;
				else if (ext && !strcasecmp(ext, ".gba")) gba++;
				else files++;
			}
			closedir(d);
			std::sort(dirs.begin(), dirs.end(), nameLess);
		}
		int f = dirs.empty() ? (cur.empty() ? PICK_CANCEL : PICK_USE) : 0;
		for (size_t i = 0; i < dirs.size(); i++)
			if (!focusName.empty() && !strcasecmp(dirs[i].c_str(), focusName.c_str()))
				f = (int)i;
		pendingFocus = f;
	}
};

//---------------------------------------------------------------------------
// Settings
//---------------------------------------------------------------------------
#define SET_CPU 0
#define SET_RENDER 1
#define SET_ADD 2
#define SET_RESCAN 3
#define SET_ABOUT 4
#define SET_BACK 5
#define SET_SUB 100   // + folder: include-subfolders toggle
#define SET_DEL 200   // + folder: Remove

#define FOLDER_ROWS 4
#define FOLDER_ROW_H 44

// Same folder: case-insensitive (FAT), trailing '/' ignored except on a root.
static std::string folderKey(const std::string &p)
{
	std::string k = p;
	while (k.size() > 1 && k[k.size() - 1] == '/' && k[k.size() - 2] != ':')
		k.erase(k.size() - 1);
	for (size_t i = 0; i < k.size(); i++)
		k[i] = (char)tolower((unsigned char)k[i]);
	return k;
}

class SettingsScreen : public GuiScreen {
public:
	SettingsScreen() : nfolders(0), list(0), scanPending(0), scanForce(false), scanDrawn(false)
	{
		const LibConfig &c = libConfig();
		addCpuValues(cpu);
		cpu.sel = (int)cpu.values.size() > 1 && c.cpu == LIB_CPU_INTERP ? 1 : 0;
		for (int r = LIB_RENDER_SOFTWARE; r <= LIB_RENDER_GXFAST; r++)
			render.add(libRenderName((LibRender)r));
		render.sel = c.render - LIB_RENDER_SOFTWARE;
		const size_t n = libGames().size();
		char buf[64];
		snprintf(buf, sizeof(buf), "%d game%s in the library", (int)n, n == 1 ? "" : "s");
		status = buf;
	}

	void enter()
	{
		focus.clear();
		const int top = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(top, SET_CPU, GuiRect(40, 68, 560, 40));
		focus.add(top, SET_RENDER, GuiRect(40, 114, 560, 40));
		list = focus.addGroup(GuiRect(40, 196, 560, FOLDER_ROWS * FOLDER_ROW_H), FOLDER_ROW_H);
		nfolders = (int)libConfig().folders.size();
		for (int i = 0; i < nfolders; i++) {
			const int y = i * FOLDER_ROW_H;
			focus.add(list, SET_SUB + i, GuiRect(276, y, 176, FOLDER_ROW_H - 4));
			focus.add(list, SET_DEL + i, GuiRect(460, y, 100, FOLDER_ROW_H - 4));
		}
		const int bar = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(bar, SET_ADD, GuiRect(40, 412, 150, 44));
		focus.add(bar, SET_RESCAN, GuiRect(198, 412, 120, 44));
		focus.add(bar, SET_ABOUT, GuiRect(326, 412, 120, 44));
		focus.add(bar, SET_BACK, GuiRect(480, 412, 120, 44));
		if (!focus.has(focus.focused()))
			focus.setFocus(SET_CPU, true);
	}

	void update(const GuiInput &)
	{
		// A scan blocks for a while: show "Scanning..." for a frame first.
		if (scanPending && scanDrawn) {
			const size_t n = libScan(scanForce).size();
			char buf[64];
			snprintf(buf, sizeof(buf), "%d game%s in the library", (int)n, n == 1 ? "" : "s");
			status = scanNote.empty() ? std::string(buf) : scanNote + ". " + buf;
			scanPending = 0;
			scanForce = false;
			scanDrawn = false;
		}
	}

	void draw()
	{
		menuBackground();
		menuHeader("Settings");
		guiChoiceDraw(focus.screenRect(SET_CPU), "CPU", cpu, focus.isFocused(SET_CPU), false, LABEL_W);
		guiChoiceDraw(focus.screenRect(SET_RENDER), "Render mode", render, focus.isFocused(SET_RENDER), false, LABEL_W);

		guiText(GUI_FONT_BODY, 40, 166, "ROM folders", kText);
		const LibConfig &c = libConfig();
		const GuiRect view = focus.view(list);
		if (c.folders.empty())
			guiTextBox(GUI_FONT_BODY, GuiRect(view.x, view.y, view.w, FOLDER_ROW_H),
			           "No ROM folders yet: Add folder picks one.", kDim, GUI_ALIGN_CENTER);
		guiPushClip(view);
		for (int i = 0; i < (int)c.folders.size() && i < nfolders; i++) {
			const GuiRect s = focus.screenRect(SET_SUB + i);
			const GuiRect pathR(view.x, s.y, s.x - view.x - 8, s.h);
			if (s.y + s.h < view.y || s.y > view.y + view.h)
				continue;
			guiRoundRect(pathR, 8, kRow);
			guiTextBox(GUI_FONT_BODY, GuiRect(pathR.x + 12, pathR.y, pathR.w - 24, pathR.h), c.folders[i].path.c_str(), kText);
			guiToggleDraw(s, "Subfolders", c.folders[i].subfolders, focus.isFocused(SET_SUB + i));
			const GuiRect d = focus.screenRect(SET_DEL + i);
			guiRoundRect(d, 8, focus.isFocused(SET_DEL + i) ? kRowFocus : kRow);
			guiTextBox(GUI_FONT_BODY, d, "Remove", kText, GUI_ALIGN_CENTER);
		}
		guiPopClip();
		if (nfolders > FOLDER_ROWS) {
			const int thumb = view.h * FOLDER_ROWS / nfolders;
			const int y = view.y + (view.h - thumb) * focus.scroll(list) / ((nfolders - FOLDER_ROWS) * FOLDER_ROW_H);
			guiRoundRect(GuiRect(view.x + view.w + 8, view.y, 6, view.h), 3, guiRgba(36, 42, 58));
			guiRoundRect(GuiRect(view.x + view.w + 8, y, 6, thumb), 3, kDim);
		}

		guiText(GUI_FONT_BODY, 40, 374, scanPending ? "Scanning..." : status.c_str(), kDim, GUI_ALIGN_LEFT, 560);
		if (scanPending)
			scanDrawn = true;

		guiButton(focus.screenRect(SET_ADD), "Add folder", false);
		guiButton(focus.screenRect(SET_RESCAN), "Rescan", false);
		guiButton(focus.screenRect(SET_ABOUT), "About", false);
		guiButton(focus.screenRect(SET_BACK), "Back", true);
		const int f = focus.focused();
		if (f < SET_SUB || focus.visible(f))
			guiFocusFrame(focus.highlight());
	}

	bool step(int id, int dir)
	{
		if (id == SET_CPU || id == SET_RENDER) {
			change(id, dir);
			return true;
		}
		return false;
	}

	void activate(int id)
	{
		if (id == SET_CPU)
			change(id, guiChoiceClick(focus.screenRect(id), LABEL_W));
		else if (id == SET_RENDER)
			change(id, guiChoiceClick(focus.screenRect(id), LABEL_W));
		else if (id >= SET_SUB && id < SET_SUB + nfolders)
			toggleSubfolders(id - SET_SUB);
		else if (id >= SET_DEL && id < SET_DEL + nfolders)
			removeFolder(id - SET_DEL);
		else if (id == SET_ADD)
			guiPush(new FolderPicker(this));
		else if (id == SET_RESCAN)
			scan(true, "Rescanned every ROM");
		else if (id == SET_ABOUT)
			guiPush(new AboutScreen);
		else if (id == SET_BACK)
			guiPop();
	}

	void addFolder(const std::string &path)
	{
		LibConfig &c = libConfig();
		const std::string k = folderKey(path);
		for (size_t i = 0; i < c.folders.size(); i++)
			if (folderKey(c.folders[i].path) == k) {
				status = c.folders[i].path + " is already in the list";
				focus.setFocus(SET_SUB + (int)i, true);
				return;
			}
		LibFolder f;
		f.path = path;
		f.subfolders = false;
		c.folders.push_back(f);
		save();
		enter();
		focus.setFocus(SET_SUB + (int)c.folders.size() - 1, true);
		scan(false, "Added " + path);
	}

private:
	GuiChoice cpu, render;
	int nfolders;                 // rows built by the last enter()
	std::string status, scanNote;   // scanNote: what led to the pending scan
	int list;
	int scanPending;
	bool scanForce, scanDrawn;

	void save()
	{
		if (!libSaveConfig())
			status = "Could not write " + libRoot() + "/config.ini";
	}

	void scan(bool force, const std::string &note)
	{
		scanPending = 1;
		scanForce |= force;
		scanDrawn = false;
		scanNote = note;
	}

	void change(int id, int dir)
	{
		LibConfig &c = libConfig();
		if (id == SET_CPU) {
			if (!cpu.step(dir))
				return;
			c.cpu = cpu.sel == (int)cpu.values.size() - 1 ? LIB_CPU_INTERP : LIB_CPU_JIT;
			save();
		} else if (id == SET_RENDER) {
			render.step(dir);
			c.render = (LibRender)(LIB_RENDER_SOFTWARE + render.sel);
			save();
		}
	}

	void toggleSubfolders(int i)
	{
		LibFolder &f = libConfig().folders[i];
		f.subfolders = !f.subfolders;
		save();
		scan(false, std::string(f.subfolders ? "Including" : "Not including") + " subfolders of " + f.path);
	}

	void removeFolder(int i)
	{
		LibConfig &c = libConfig();
		const std::string note = "Removed " + c.folders[i].path;
		c.folders.erase(c.folders.begin() + i);
		save();
		enter();
		const int n = (int)c.folders.size();
		focus.setFocus(n ? SET_DEL + (i < n ? i : n - 1) : SET_ADD, true);
		scan(false, note);
	}
};

static void settingsAddFolder(SettingsScreen *s, const std::string &path) { s->addFolder(path); }

GuiScreen *newSettingsScreen() { return new SettingsScreen; }

//---------------------------------------------------------------------------
// Game settings
//---------------------------------------------------------------------------
#define GS_CPU 0
#define GS_RENDER 1
#define GS_BACK 2

class GameSettingsScreen : public GuiScreen {
public:
	GameSettingsScreen(const LibGame *g, const GuiImage &i) : game(g), icon(i)
	{
		ov.cpu = LIB_CPU_DEFAULT;
		ov.render = LIB_RENDER_DEFAULT;
		if (!game->dir.empty())
			libLoadGameSettings(game->dir, ov);
		const LibConfig &c = libConfig();
#ifdef DIIS_JIT
		cpu.add(std::string("Default (") + libCpuName(c.cpu) + ")");
		addCpuValues(cpu);
		cpu.sel = ov.cpu;   // DEFAULT, JIT, INTERP
#else
		addCpuValues(cpu);   // Interpreter only: nothing to override
		ov.cpu = LIB_CPU_DEFAULT;
#endif
		render.add(std::string("Default (") + libRenderName(c.render) + ")");
		for (int r = LIB_RENDER_SOFTWARE; r <= LIB_RENDER_GXFAST; r++)
			render.add(libRenderName((LibRender)r));
		render.sel = ov.render;   // DEFAULT, SOFTWARE, GXACCURATE, GXFAST
	}

	void enter()
	{
		focus.clear();
		const int g = focus.addGroup(GuiRect(0, 0, GUI_W, GUI_H));
		focus.add(g, GS_CPU, GuiRect(40, 176, 560, 40));
		focus.add(g, GS_RENDER, GuiRect(40, 222, 560, 40));
		focus.add(g, GS_BACK, GuiRect(40, 412, 120, 44));
		if (!focus.has(focus.focused()))
			focus.setFocus(GS_CPU, true);
	}

	void draw()
	{
		menuBackground();
		menuHeader("Game settings");
		const GuiRect p(40, 68, 560, 92);
		guiPanel(p, 12, kPanel, kPanelEdge, 1);
		guiImage(icon, p.x + 14, p.y + 14, 2);
		const LibRomInfo &info = game->info;
		const int tx = p.x + 96, tw = p.x + p.w - 14 - tx;
		guiText(GUI_FONT_BODY, tx, p.y + 12, info.lines[0].c_str(), kText, GUI_ALIGN_LEFT, tw);
		guiText(GUI_FONT_BODY, tx, p.y + 40, info.lines[1].c_str(), kDim, GUI_ALIGN_LEFT, tw);
		guiText(GUI_FONT_BODY, p.x + p.w - 14, p.y + 12, info.id.c_str(), kDim, GUI_ALIGN_RIGHT);

		guiChoiceDraw(focus.screenRect(GS_CPU), "CPU", cpu, focus.isFocused(GS_CPU), ov.cpu != LIB_CPU_DEFAULT, LABEL_W);
		guiChoiceDraw(focus.screenRect(GS_RENDER), "Render mode", render, focus.isFocused(GS_RENDER),
		              ov.render != LIB_RENDER_DEFAULT, LABEL_W);

		std::string note;
		if (ov.cpu == LIB_CPU_DEFAULT && ov.render == LIB_RENDER_DEFAULT)
			note = "No overrides: this game follows the defaults in Settings.";
		else
			note = std::string("Overridden for this game: ") +
			       (ov.cpu != LIB_CPU_DEFAULT ? (ov.render != LIB_RENDER_DEFAULT ? "CPU, render mode" : "CPU") : "render mode") +
			       " (highlighted).";
		if (!error.empty())
			note = error;
		guiTextWrap(GUI_FONT_BODY, GuiRect(40, 280, 560, 80), note.c_str(), kDim);

		guiButton(focus.screenRect(GS_BACK), "Back", true);
		guiFocusFrame(focus.highlight());
	}

	bool step(int id, int dir)
	{
		if (id != GS_CPU && id != GS_RENDER)
			return false;
		change(id, dir);
		return true;
	}

	void activate(int id)
	{
		if (id == GS_BACK)
			guiPop();
		else
			change(id, guiChoiceClick(focus.screenRect(id), LABEL_W));
	}

private:
	const LibGame *game;
	const GuiImage &icon;   // the Game screen's
	LibGameSettings ov;
	GuiChoice cpu, render;
	std::string error;

	void change(int id, int dir)
	{
		if (id == GS_CPU) {
			if (!cpu.step(dir))
				return;
			ov.cpu = (LibCpu)cpu.sel;
		} else {
			render.step(dir);
			ov.render = (LibRender)render.sel;
		}
		error = libSaveGameOverrides(*game, ov) ? "" : "Could not write the game's game.ini.";
	}
};

GuiScreen *newGameSettingsScreen(const LibGame *game, const GuiImage &icon)
{
	return new GameSettingsScreen(game, icon);
}
