/****************************************************************************
 * DiiS - library.h
 *
 * ROM library core (menu rework phase 1, no UI): global settings, ROM folder
 * scan + cache, one folder per game, per-game settings and the one-time move
 * of old saves into the game's folder. Plain stdio/dirent/stat, so it also
 * runs on the host (tools/library-test) against a scratch directory.
 *
 * On the app's device (sd: or usb:, whichever boot.dol was started from):
 *
 *   DiiS/config.ini               cpu=, render=, folderN= / folderN_subfolders=
 *   DiiS/library.cache            path, size, mtime, id, titles, icon per ROM
 *   DiiS/games/<id> <title>/      one per game, found by its id prefix:
 *       game.ini                  id=, rom= (last seen), cpu= / render= overrides only
 *       banner.ini, icon.png      GBA only, optional, user-written (line1=..line3=, 32x32)
 *       save.dsv | save.sav       backup memory
 *       state0.ds0 .. state9.ds9  savestates
 *       perfzones.log, gxstats.log
 *
 * Old saves (<legacy dir>/<rom name>.dsv / .sav / .ds0-.ds9) are moved into
 * the game folder the first time the game is launched (libPrepareLaunch).
 ***************************************************************************/
#ifndef DIIS_LIBRARY_H
#define DIIS_LIBRARY_H

#include <stdint.h>
#include <string>
#include <vector>
#include "lib_rom.h"

enum LibCpu    { LIB_CPU_DEFAULT, LIB_CPU_JIT, LIB_CPU_INTERP };
enum LibRender { LIB_RENDER_DEFAULT, LIB_RENDER_SOFTWARE, LIB_RENDER_GXACCURATE, LIB_RENDER_GXFAST };

struct LibFolder {
	std::string path;
	bool subfolders;
};

struct LibConfig {
	LibCpu cpu;         // never DEFAULT
	LibRender render;   // never DEFAULT
	std::vector<LibFolder> folders;
};

struct LibGameSettings {
	LibCpu cpu;         // DEFAULT = follow LibConfig
	LibRender render;
};

struct LibGame {
	std::string path;
	long long size;
	long long mtime;
	LibRomInfo info;
	std::string dir;                              // game folder ("<root>/games/<name>/"), "" until first launch
	bool hasIcon;                                 // false: draw the generic DS/GBA icon
	uint16_t icon[LIB_ICON_W * LIB_ICON_H];       // row-major RGB5A3
};

struct LibLaunch {
	LibRomInfo info;
	std::string dir;                              // game folder, with trailing '/'
	LibGameSettings game;                         // the game's own overrides (may be DEFAULT)
	LibCpu cpu;                                   // resolved: game override, else global
	LibRender render;
	std::vector<std::string> messages;            // migration notes for the screen
};

// root: "<device>/DiiS" (created if missing). legacySaves: the old flat save
// dir ("sd:/DS/SAVES"). Loads config.ini (writing the defaults if it's missing).
void libInit(const char *root, const char *legacySaves);
const std::string &libRoot();

LibConfig &libConfig();
bool libSaveConfig();

// Scan every configured folder; unchanged files (same path, size, mtime) come
// from library.cache. force=true re-reads every ROM. Sorted by first title line.
const std::vector<LibGame> &libScan(bool force);
// Bumped by every libScan() (which replaces the vector it returns): holders
// of LibGame pointers compare it to know theirs are stale.
unsigned libGeneration();
const std::vector<LibGame> &libGames();   // the last scan's result

// Existing game folder for `id` ("" if none), and create-on-demand.
std::string libFindGameDir(const std::string &id);
std::string libEnsureGameDir(const LibRomInfo &info);

bool libLoadGameSettings(const std::string &dir, LibGameSettings &out);
bool libSaveGameSettings(const std::string &dir, const LibRomInfo &info, const char *romPath, const LibGameSettings &s);
// A library game's overrides -> its game.ini, creating its folder (and
// setting its LibGame::dir) when it has none yet.
bool libSaveGameOverrides(const LibGame &g, const LibGameSettings &s);

// Move old saves for `romPath` into `dir`. Returns the number moved; notes
// (moves, and files left behind because the destination already exists or
// the copy failed) are appended to msgs.
int libMigrate(const LibRomInfo &info, const char *romPath, const std::string &dir, std::vector<std::string> &msgs);

// Everything a launch needs: probe the ROM, find/create its folder, record
// the ROM path in game.ini, migrate old saves, resolve settings.
bool libPrepareLaunch(const char *romPath, LibLaunch &out);

const char *libCpuName(LibCpu c);
const char *libRenderName(LibRender r);

#endif // DIIS_LIBRARY_H
