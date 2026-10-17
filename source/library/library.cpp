/****************************************************************************
 * DiiS - library.cpp   (see library.h)
 ***************************************************************************/
#include "library.h"
#include "lib_ini.h"
#include "lib_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <map>

#define LIB_CACHE_MAGIC "# DiiS library cache v1"
#define LIB_MAX_FOLDERS 16
#define LIB_MAX_DEPTH   8   // include-subfolders recursion limit

static std::string s_root;          // "<device>/DiiS"
static std::string s_legacy;        // old flat save dir
static LibConfig s_cfg;
static std::vector<LibGame> s_games;
static unsigned s_generation;

//---------------------------------------------------------------------------
// Small helpers
//---------------------------------------------------------------------------
static std::string join(const std::string &dir, const std::string &name)
{
	if (dir.empty()) return name;
	return dir[dir.size() - 1] == '/' ? dir + name : dir + "/" + name;
}

static bool is_dir(const std::string &p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_stat(const std::string &p, long long &size, long long &mtime)
{
	struct stat st;
	if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
	size = (long long)st.st_size;
	mtime = (long long)st.st_mtime;
	return true;
}

static std::string games_dir() { return join(s_root, "games"); }

const char *libCpuName(LibCpu c)
{
	switch (c) {
	case LIB_CPU_JIT:    return "JIT";
	case LIB_CPU_INTERP: return "Interpreter";
	default:             return "Default";
	}
}

const char *libRenderName(LibRender r)
{
	switch (r) {
	case LIB_RENDER_SOFTWARE:   return "Software";
	case LIB_RENDER_GXACCURATE: return "GxAccurate";
	case LIB_RENDER_GXFAST:     return "GxFast";
	default:                    return "Default";
	}
}

static LibCpu parse_cpu(const std::string &s)
{
	if (!strcasecmp(s.c_str(), "JIT")) return LIB_CPU_JIT;
	if (!strcasecmp(s.c_str(), "Interpreter")) return LIB_CPU_INTERP;
	return LIB_CPU_DEFAULT;
}

static LibRender parse_render(const std::string &s)
{
	if (!strcasecmp(s.c_str(), "Software")) return LIB_RENDER_SOFTWARE;
	if (!strcasecmp(s.c_str(), "GxAccurate")) return LIB_RENDER_GXACCURATE;
	if (!strcasecmp(s.c_str(), "GxFast")) return LIB_RENDER_GXFAST;
	return LIB_RENDER_DEFAULT;
}

//---------------------------------------------------------------------------
// config.ini
//---------------------------------------------------------------------------
static void default_config(LibConfig &c)
{
	c.cpu = LIB_CPU_JIT;
	c.render = LIB_RENDER_GXFAST;
	c.folders.clear();
	LibFolder f;
	f.subfolders = false;
	f.path = "sd:/ROMS/DS"; c.folders.push_back(f);
	f.path = "sd:/DS/ROMS"; c.folders.push_back(f);
}

static std::string config_path() { return join(s_root, "config.ini"); }

void libInit(const char *root, const char *legacySaves)
{
	s_root = root;
	s_legacy = legacySaves;
	s_games.clear();
	mkdir(s_root.c_str(), 0777);        // fails harmlessly when it already exists
	mkdir(games_dir().c_str(), 0777);

	default_config(s_cfg);
	LibIni ini;
	if (!ini.load(config_path().c_str())) {
		libSaveConfig();
		return;
	}
	const LibCpu cpu = parse_cpu(ini.get("cpu"));
	const LibRender render = parse_render(ini.get("render"));
	if (cpu != LIB_CPU_DEFAULT) s_cfg.cpu = cpu;
	if (render != LIB_RENDER_DEFAULT) s_cfg.render = render;
	std::vector<LibFolder> folders;
	bool anyFolderKey = false;
	for (int i = 1; i <= LIB_MAX_FOLDERS; i++) {
		char k[32];
		snprintf(k, sizeof(k), "folder%d", i);
		const std::string *p = ini.find(k);
		if (!p) continue;
		anyFolderKey = true;
		if (p->empty()) continue;
		LibFolder f;
		f.path = *p;
		snprintf(k, sizeof(k), "folder%d_subfolders", i);
		f.subfolders = atoi(ini.get(k, "0").c_str()) != 0;
		folders.push_back(f);
	}
	if (anyFolderKey) s_cfg.folders = folders;
}

const std::string &libRoot() { return s_root; }
LibConfig &libConfig() { return s_cfg; }

bool libSaveConfig()
{
	LibIni ini;
	ini.set("cpu", libCpuName(s_cfg.cpu));
	ini.set("render", libRenderName(s_cfg.render));
	for (size_t i = 0; i < s_cfg.folders.size() && i < LIB_MAX_FOLDERS; i++) {
		char k[32];
		snprintf(k, sizeof(k), "folder%d", (int)i + 1);
		ini.set(k, s_cfg.folders[i].path);
		snprintf(k, sizeof(k), "folder%d_subfolders", (int)i + 1);
		ini.set(k, s_cfg.folders[i].subfolders ? "1" : "0");
	}
	return ini.save(config_path().c_str(), "DiiS settings (cpu: JIT|Interpreter, render: Software|GxAccurate|GxFast)");
}

//---------------------------------------------------------------------------
// Game folders
//---------------------------------------------------------------------------
static bool id_matches(const char *name, const std::string &id)
{
	const size_t n = id.size();
	return !strncmp(name, id.c_str(), n) && (name[n] == 0 || name[n] == ' ');
}

// id -> folder name for everything under games/.
static void list_game_dirs(std::map<std::string, std::string> &out)
{
	out.clear();
	DIR *d = opendir(games_dir().c_str());
	if (!d) return;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] == '.') continue;
		const char *sp = strchr(e->d_name, ' ');
		const std::string id = sp ? std::string(e->d_name, sp - e->d_name) : std::string(e->d_name);
		if (!out.count(id)) out[id] = e->d_name;
	}
	closedir(d);
}

std::string libFindGameDir(const std::string &id)
{
	DIR *d = opendir(games_dir().c_str());
	if (!d) return std::string();
	std::string found;
	while (struct dirent *e = readdir(d)) {
		if (id_matches(e->d_name, id)) { found = join(games_dir(), e->d_name) + "/"; break; }
	}
	closedir(d);
	return found;
}

std::string libEnsureGameDir(const LibRomInfo &info)
{
	std::string dir = libFindGameDir(info.id);
	if (!dir.empty()) return dir;
	mkdir(s_root.c_str(), 0777);
	mkdir(games_dir().c_str(), 0777);
	const std::string p = join(games_dir(), libFolderName(info));
	if (mkdir(p.c_str(), 0777) != 0 && !is_dir(p)) return std::string();
	return p + "/";
}

bool libLoadGameSettings(const std::string &dir, LibGameSettings &out)
{
	out.cpu = LIB_CPU_DEFAULT;
	out.render = LIB_RENDER_DEFAULT;
	LibIni ini;
	if (!ini.load(join(dir, "game.ini").c_str())) return false;
	out.cpu = parse_cpu(ini.get("cpu"));
	out.render = parse_render(ini.get("render"));
	return true;
}

bool libSaveGameSettings(const std::string &dir, const LibRomInfo &info, const char *romPath, const LibGameSettings &s)
{
	LibIni ini;
	ini.set("id", info.id);
	ini.set("rom", romPath ? romPath : "");
	if (s.cpu != LIB_CPU_DEFAULT) ini.set("cpu", libCpuName(s.cpu));
	if (s.render != LIB_RENDER_DEFAULT) ini.set("render", libRenderName(s.render));
	return ini.save(join(dir, "game.ini").c_str(), "DiiS per-game settings (cpu/render only when not the global default)");
}

bool libSaveGameOverrides(const LibGame &g, const LibGameSettings &s)
{
	std::string dir = g.dir;
	if (dir.empty()) {
		dir = libEnsureGameDir(g.info);
		if (dir.empty()) return false;
		for (size_t i = 0; i < s_games.size(); i++)
			if (s_games[i].info.id == g.info.id) s_games[i].dir = dir;
	}
	return libSaveGameSettings(dir, g.info, g.path.c_str(), s);
}

// GBA user overrides from the game folder: banner.ini lines, 32x32 icon.png.
static void apply_user_files(LibGame &g)
{
	if (g.dir.empty() || !g.info.isGba) return;
	LibIni ini;
	if (ini.load(join(g.dir, "banner.ini").c_str())) {
		int n = 0;
		std::string l[LIB_TITLE_LINES];
		for (int i = 0; i < LIB_TITLE_LINES; i++) {
			char k[8];
			snprintf(k, sizeof(k), "line%d", i + 1);
			if (const std::string *v = ini.find(k)) { l[i] = *v; n = i + 1; }
		}
		if (n) {
			for (int i = 0; i < LIB_TITLE_LINES; i++) g.info.lines[i] = l[i];
			g.info.nlines = n;
		}
	}
	std::vector<uint8_t> rgba;
	int w, h;
	if (libPngLoad(join(g.dir, "icon.png").c_str(), rgba, w, h) && w == LIB_ICON_W && h == LIB_ICON_H) {
		libRgbaToRgb5a3(rgba.data(), w, h, g.icon);
		g.hasIcon = true;
	}
}

//---------------------------------------------------------------------------
// Scan + library.cache
//
// One line per ROM, tab-separated:
//   path size mtime D|G id nlines line1 line2 line3 icon
// lines escape \ tab CR LF as \\ \t \r \n; icon is the 544-byte DS banner
// icon+palette in hex, or "-".
//---------------------------------------------------------------------------
static std::string cache_path() { return join(s_root, "library.cache"); }

static std::string esc(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		switch (s[i]) {
		case '\\': o += "\\\\"; break;
		case '\t': o += "\\t"; break;
		case '\r': o += "\\r"; break;
		case '\n': o += "\\n"; break;
		default:   o += s[i];
		}
	}
	return o;
}

static std::string unesc(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '\\' || i + 1 == s.size()) { o += s[i]; continue; }
		const char c = s[++i];
		o += c == 't' ? '\t' : c == 'r' ? '\r' : c == 'n' ? '\n' : c;
	}
	return o;
}

static void split_tabs(const std::string &l, std::vector<std::string> &f)
{
	f.clear();
	size_t a = 0;
	for (;;) {
		const size_t b = l.find('\t', a);
		f.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
		if (b == std::string::npos) break;
		a = b + 1;
	}
}

static void load_cache(std::map<std::string, LibGame> &out)
{
	out.clear();
	FILE *f = fopen(cache_path().c_str(), "rb");
	if (!f) return;
	std::string line;
	std::vector<std::string> fld;
	bool first = true;
	char buf[2048];
	while (fgets(buf, sizeof(buf), f)) {
		line += buf;
		if (line.empty() || line[line.size() - 1] != '\n') continue;   // long line: keep reading
		line.erase(line.size() - 1);
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		if (first) { first = false; if (line != LIB_CACHE_MAGIC) break; line.clear(); continue; }
		split_tabs(line, fld);
		line.clear();
		if (fld.size() != 10) continue;
		LibGame g;
		g.path = unesc(fld[0]);
		g.size = atoll(fld[1].c_str());
		g.mtime = atoll(fld[2].c_str());
		g.info.isGba = fld[3] == "G";
		g.info.id = fld[4];
		g.info.nlines = atoi(fld[5].c_str());
		if (g.info.nlines < 0 || g.info.nlines > LIB_TITLE_LINES) continue;
		for (int i = 0; i < LIB_TITLE_LINES; i++) g.info.lines[i] = unesc(fld[6 + i]);
		g.info.hasIcon = fld[9].size() == LIB_ICON_RAW_SIZE * 2;
		memset(g.info.iconRaw, 0, sizeof(g.info.iconRaw));
		if (g.info.hasIcon)
			for (int i = 0; i < LIB_ICON_RAW_SIZE; i++)
				g.info.iconRaw[i] = (uint8_t)strtoul(fld[9].substr(i * 2, 2).c_str(), 0, 16);
		out[g.path] = g;
	}
	fclose(f);
}

static bool save_cache(const std::vector<LibGame> &games)
{
	const std::string tmp = cache_path() + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	fprintf(f, "%s\n", LIB_CACHE_MAGIC);
	for (size_t i = 0; i < games.size(); i++) {
		const LibGame &g = games[i];
		fprintf(f, "%s\t%lld\t%lld\t%c\t%s\t%d", esc(g.path).c_str(), g.size, g.mtime,
		        g.info.isGba ? 'G' : 'D', g.info.id.c_str(), g.info.nlines);
		for (int l = 0; l < LIB_TITLE_LINES; l++) fprintf(f, "\t%s", esc(g.info.lines[l]).c_str());
		fputc('\t', f);
		if (g.info.hasIcon) for (int b = 0; b < LIB_ICON_RAW_SIZE; b++) fprintf(f, "%02X", g.info.iconRaw[b]);
		else fputc('-', f);
		fputc('\n', f);
	}
	if (fclose(f) != 0) { remove(tmp.c_str()); return false; }
	remove(cache_path().c_str());
	return rename(tmp.c_str(), cache_path().c_str()) == 0;
}

static void scan_dir(const std::string &dir, bool recurse, int depth, std::vector<std::string> &out)
{
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	std::vector<std::string> subs;
	while (struct dirent *e = readdir(d)) {
		if (e->d_name[0] == '.') continue;
		const std::string p = join(dir, e->d_name);
		if (libIsRomName(e->d_name)) out.push_back(p);
		else if (recurse && depth < LIB_MAX_DEPTH && is_dir(p)) subs.push_back(p);
	}
	closedir(d);
	for (size_t i = 0; i < subs.size(); i++) scan_dir(subs[i], recurse, depth + 1, out);
}

static bool game_less(const LibGame &a, const LibGame &b)
{
	const int c = strcasecmp(a.info.lines[0].c_str(), b.info.lines[0].c_str());
	return c ? c < 0 : a.path < b.path;
}

const std::vector<LibGame> &libScan(bool force)
{
	std::map<std::string, LibGame> cache;
	if (!force) load_cache(cache);

	std::vector<std::string> files;
	for (size_t i = 0; i < s_cfg.folders.size(); i++)
		scan_dir(s_cfg.folders[i].path, s_cfg.folders[i].subfolders, 0, files);
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());

	std::map<std::string, std::string> dirs;
	list_game_dirs(dirs);

	s_games.clear();
	std::vector<LibGame> raw;   // what the ROMs say, for the cache (no banner.ini/icon.png)
	bool dirty = force || cache.size() != files.size();
	for (size_t i = 0; i < files.size(); i++) {
		LibGame g;
		if (!file_stat(files[i], g.size, g.mtime)) continue;
		std::map<std::string, LibGame>::iterator c = cache.find(files[i]);
		if (c != cache.end() && c->second.size == g.size && c->second.mtime == g.mtime) {
			g.info = c->second.info;
		} else {
			if (!libProbeRom(files[i].c_str(), g.info)) continue;
			dirty = true;
		}
		g.path = files[i];
		std::map<std::string, std::string>::iterator dn = dirs.find(g.info.id);
		g.dir = dn != dirs.end() ? join(games_dir(), dn->second) + "/" : std::string();
		g.hasIcon = g.info.hasIcon;
		if (g.hasIcon) libDecodeDsIcon(g.info.iconRaw, g.icon);
		else memset(g.icon, 0, sizeof(g.icon));
		raw.push_back(g);
		apply_user_files(g);
		s_games.push_back(g);
	}
	if (dirty) save_cache(raw);
	std::sort(s_games.begin(), s_games.end(), game_less);
	s_generation++;
	return s_games;
}

unsigned libGeneration() { return s_generation; }
const std::vector<LibGame> &libGames() { return s_games; }

//---------------------------------------------------------------------------
// Migration of old saves
//---------------------------------------------------------------------------

// Copy src -> dst, check the size, then delete src. On any failure dst is
// removed and src stays.
static bool move_file(const std::string &src, const std::string &dst, long long srcSize)
{
	FILE *in = fopen(src.c_str(), "rb");
	if (!in) return false;
	FILE *out = fopen(dst.c_str(), "wb");
	if (!out) { fclose(in); return false; }
	static char buf[64 * 1024];
	bool ok = true;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
	if (ferror(in)) ok = false;
	fclose(in);
	if (fclose(out) != 0) ok = false;
	long long dsz, dmt;
	if (!ok || !file_stat(dst, dsz, dmt) || dsz != srcSize) { remove(dst.c_str()); return false; }
	return remove(src.c_str()) == 0;
}

int libMigrate(const LibRomInfo &info, const char *romPath, const std::string &dir, std::vector<std::string> &msgs)
{
	if (dir.empty() || s_legacy.empty()) return 0;
	const std::string stem = join(s_legacy, libBaseName(romPath));
	std::vector<std::pair<std::string, std::string> > pairs;
	pairs.push_back(std::make_pair(stem + (info.isGba ? ".sav" : ".dsv"), std::string(info.isGba ? "save.sav" : "save.dsv")));
	for (int i = 0; i < 10; i++) {
		char ext[8], name[16];
		snprintf(ext, sizeof(ext), ".ds%d", i);
		snprintf(name, sizeof(name), "state%d.ds%d", i, i);
		pairs.push_back(std::make_pair(stem + ext, std::string(name)));
	}
	int moved = 0;
	for (size_t i = 0; i < pairs.size(); i++) {
		const std::string &src = pairs[i].first;
		const std::string dst = join(dir, pairs[i].second);
		long long sz, mt;
		if (!file_stat(src, sz, mt)) continue;
		if (file_stat(dst, sz, mt) || is_dir(dst)) {
			msgs.push_back("Kept " + src + ": " + pairs[i].second + " already exists in the game folder");
			continue;
		}
		file_stat(src, sz, mt);
		if (move_file(src, dst, sz)) {
			msgs.push_back("Moved " + src + " -> " + pairs[i].second);
			moved++;
		} else {
			msgs.push_back("Could not move " + src + " (left in place)");
		}
	}
	return moved;
}

//---------------------------------------------------------------------------
// Launch
//---------------------------------------------------------------------------
bool libPrepareLaunch(const char *romPath, LibLaunch &out)
{
	out.dir.clear();
	out.messages.clear();
	out.game.cpu = LIB_CPU_DEFAULT;
	out.game.render = LIB_RENDER_DEFAULT;
	out.cpu = s_cfg.cpu;
	out.render = s_cfg.render;
	if (!libProbeRom(romPath, out.info)) return false;
	out.dir = libEnsureGameDir(out.info);
	if (out.dir.empty()) {
		out.messages.push_back("Could not create the game folder under " + games_dir());
		return false;
	}
	LibIni ini;
	const bool had = ini.load(join(out.dir, "game.ini").c_str());
	libLoadGameSettings(out.dir, out.game);
	if (!had || ini.get("rom") != romPath || ini.get("id") != out.info.id)
		libSaveGameSettings(out.dir, out.info, romPath, out.game);
	libMigrate(out.info, romPath, out.dir, out.messages);
	if (out.game.cpu != LIB_CPU_DEFAULT) out.cpu = out.game.cpu;
	if (out.game.render != LIB_RENDER_DEFAULT) out.render = out.game.render;
	return true;
}
