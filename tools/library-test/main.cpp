// Host-side unit tests for source/library/ (menu rework phase 1: ROM library
// core). Pure stdio/dirent logic with no NDS/GX/Wii dependency, so this builds
// and runs with the host's own g++ - no devkitPPC, no Dolphin, no hardware.
//
// Build + run: ./build.sh [rom dir] [out dir]
//   rom dir: default ../../testdata (Phantom Hourglass is skipped: it is never
//            used as a test oracle in this repo)
//   out dir: default ../benchmark/results/library-test (gitignored); gets one
//            <id>.png per ROM icon (4x, on grey), icons.png (all of them) and
//            a scratch "sd" tree for the scan/cache/migration checks.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include "library/lib_rom.h"
#include "library/lib_png.h"
#include "library/lib_ini.h"
#include "library/library.h"

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		++g_failures; \
	} \
} while (0)

// ---------------------------------------------------------------------
// PNG writer (RGBA8, filter 0) for the icon dumps and the decoder test.
// ---------------------------------------------------------------------
static void put_be32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static void put_chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &body)
{
	put_be32(png, body.size());
	const size_t start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), body.begin(), body.end());
	put_be32(png, crc32(0, &png[start], png.size() - start));
}

static std::vector<uint8_t> encode_png(const uint8_t *rgba, int w, int h)
{
	std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }, ihdr, raw, z;
	put_be32(ihdr, w); put_be32(ihdr, h);
	ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	put_chunk(png, "IHDR", ihdr);
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), rgba + y * w * 4, rgba + (y + 1) * w * 4);
	}
	uLongf zl = compressBound(raw.size());
	z.resize(zl);
	compress(z.data(), &zl, raw.data(), raw.size());
	z.resize(zl);
	put_chunk(png, "IDAT", z);
	put_chunk(png, "IEND", {});
	return png;
}

static bool write_file(const std::string &path, const std::vector<uint8_t> &data)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fwrite(data.data(), 1, data.size(), f);
	return fclose(f) == 0;
}

static bool write_text(const std::string &path, const char *text)
{
	return write_file(path, std::vector<uint8_t>(text, text + strlen(text)));
}

// RGB5A3 -> RGBA8 (the inverse GX applies when sampling).
static void rgb5a3_to_rgba(uint16_t c, uint8_t *o)
{
	if (c & 0x8000) {
		o[0] = ((c >> 10) & 31) * 255 / 31; o[1] = ((c >> 5) & 31) * 255 / 31; o[2] = (c & 31) * 255 / 31; o[3] = 255;
	} else {
		o[0] = ((c >> 8) & 15) * 17; o[1] = ((c >> 4) & 15) * 17; o[2] = (c & 15) * 17; o[3] = ((c >> 12) & 7) * 255 / 7;
	}
}

// Icon at `scale`x over a grey checker (so transparency shows), into an RGBA canvas.
static void blit_icon(const uint16_t *icon, int scale, std::vector<uint8_t> &canvas, int cw, int ox, int oy)
{
	for (int y = 0; y < LIB_ICON_H * scale; y++)
		for (int x = 0; x < LIB_ICON_W * scale; x++) {
			uint8_t px[4];
			rgb5a3_to_rgba(icon[(y / scale) * LIB_ICON_W + x / scale], px);
			const uint8_t bg = (((x / 8) ^ (y / 8)) & 1) ? 0x90 : 0x70;
			uint8_t *o = &canvas[((size_t)(oy + y) * cw + ox + x) * 4];
			for (int k = 0; k < 3; k++) o[k] = (uint8_t)((px[k] * px[3] + bg * (255 - px[3])) / 255);
			o[3] = 255;
		}
}

// ---------------------------------------------------------------------
// Unit checks
// ---------------------------------------------------------------------
static void test_crc_and_clean()
{
	CHECK(libCrc16((const uint8_t *)"123456789", 9) == 0x4B37);   // CRC-16/MODBUS check value

	CHECK(libCleanTitle("Super Mario 64 DS") == "Super Mario 64 DS");
	CHECK(libCleanTitle("Zelda: Spirit Tracks") == "Zelda Spirit Tracks");
	CHECK(libCleanTitle("a\\b/c:d*e?f\"g<h>i|j") == "abcdefghij");
	CHECK(libCleanTitle("  two   spaces  ") == "two spaces");
	CHECK(libCleanTitle("Tab\tand\x01" "ctrl") == "Tabandctrl");
	CHECK(libCleanTitle("Pok\xC3\xA9mon") == "Pokemon");
	CHECK(libCleanTitle("\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3") == "");   // katakana only
	CHECK(libCleanTitle("Trailing dots...") == "Trailing dots");
	CHECK(libCleanTitle(std::string(100, 'x')).size() == 64);

	LibRomInfo i;
	i.id = "ABCD-1234"; i.nlines = 1; i.lines[0] = "?:*";
	CHECK(libFolderName(i) == "ABCD-1234");
	i.lines[0] = "Game: Name";
	CHECK(libFolderName(i) == "ABCD-1234 Game Name");
}

static void test_png_roundtrip()
{
	std::vector<uint8_t> src(32 * 32 * 4);
	for (int i = 0; i < 32 * 32 * 4; i++) src[i] = (uint8_t)(i * 37 + (i >> 7));
	const std::vector<uint8_t> png = encode_png(src.data(), 32, 32);
	std::vector<uint8_t> back;
	int w = 0, h = 0;
	CHECK(libPngDecode(png.data(), png.size(), back, w, h));
	CHECK(w == 32 && h == 32 && back == src);

	uint8_t px[4] = { 0xF8, 0x80, 0x08, 0xFF };   // opaque -> 1rrrrrgggggbbbbb
	uint16_t c;
	libRgbaToRgb5a3(px, 1, 1, &c);
	CHECK(c == (0x8000 | (31 << 10) | (16 << 5) | 1));
	px[3] = 0x40;                                  // translucent -> 0aaarrrrggggbbbb
	libRgbaToRgb5a3(px, 1, 1, &c);
	CHECK(c == ((2 << 12) | (15 << 8) | (8 << 4) | 0));
}

// ---------------------------------------------------------------------
// Per-ROM dump
// ---------------------------------------------------------------------
struct Probed { std::string file; LibRomInfo info; uint16_t icon[LIB_ICON_W * LIB_ICON_H]; };

static void dump_roms(const std::string &romDir, const std::string &outDir, std::vector<Probed> &probed)
{
	DIR *d = opendir(romDir.c_str());
	if (!d) { printf("FAIL: can't open %s\n", romDir.c_str()); ++g_failures; return; }
	std::vector<std::string> names;
	while (struct dirent *e = readdir(d))
		if (libIsRomName(e->d_name) && !strstr(e->d_name, "Phantom Hourglass")) names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());

	printf("\n%-16s | %-40s | %s\n", "id", "folder", "lines");
	for (size_t n = 0; n < names.size(); n++) {
		Probed p;
		p.file = names[n];
		const std::string path = romDir + "/" + names[n];
		CHECK(libProbeRom(path.c_str(), p.info));
		if (p.info.hasIcon) libDecodeDsIcon(p.info.iconRaw, p.icon);
		else memset(p.icon, 0, sizeof(p.icon));

		// The id's CRC must be the header's own CRC16 over 0x000..0x15D.
		if (!p.info.isGba) {
			uint8_t hdr[0x160];
			FILE *f = fopen(path.c_str(), "rb");
			CHECK(f && fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
			if (f) fclose(f);
			char want[8];
			snprintf(want, sizeof(want), "%04X", libCrc16(hdr, 0x15E));
			CHECK(p.info.id.substr(5) == want);
		}

		printf("%-16s | %-40s |", p.info.id.c_str(), libFolderName(p.info).c_str());
		for (int l = 0; l < p.info.nlines; l++) printf(" [%s]", p.info.lines[l].c_str());
		printf("%s\n", p.info.hasIcon ? "" : "  (no icon)");

		if (p.info.hasIcon) {
			std::vector<uint8_t> img(128 * 128 * 4);
			blit_icon(p.icon, 4, img, 128, 0, 0);
			write_file(outDir + "/" + p.info.id + ".png", encode_png(img.data(), 128, 128));
		}
		probed.push_back(p);
	}

	// Contact sheet: every icon at 4x, side by side.
	if (!probed.empty()) {
		const int cw = (int)probed.size() * 136 + 8, ch = 144;
		std::vector<uint8_t> sheet((size_t)cw * ch * 4, 0x30);
		for (size_t i = 0; i < probed.size(); i++) blit_icon(probed[i].icon, 4, sheet, cw, 8 + (int)i * 136, 8);
		write_file(outDir + "/icons.png", encode_png(sheet.data(), cw, ch));
	}
}

// ---------------------------------------------------------------------
// Scan / cache / game folder / migration, on a scratch "sd" tree
// ---------------------------------------------------------------------
static void rm_rf(const std::string &p)
{
	std::string cmd = "rm -rf '" + p + "'";
	if (system(cmd.c_str())) {}
}

static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static void test_library(const std::string &romDir, const std::string &outDir, const std::vector<Probed> &probed)
{
	if (probed.empty()) return;
	const std::string sd = outDir + "/sd";
	rm_rf(sd);
	mkdir(sd.c_str(), 0777);
	const std::string roms = sd + "/ROMS", sub = roms + "/sub", saves = sd + "/SAVES";
	mkdir(roms.c_str(), 0777); mkdir(sub.c_str(), 0777); mkdir(saves.c_str(), 0777);

	// First ROM in the top folder, the rest in a subfolder (symlinks to testdata).
	for (size_t i = 0; i < probed.size(); i++) {
		const std::string dst = (i == 0 ? roms : sub) + "/" + probed[i].file;
		CHECK(symlink((romDir + "/" + probed[i].file).c_str(), dst.c_str()) == 0);
	}

	libInit((sd + "/DiiS").c_str(), saves.c_str());
	CHECK(exists(sd + "/DiiS/config.ini"));
	CHECK(libConfig().cpu == LIB_CPU_JIT && libConfig().render == LIB_RENDER_GXFAST);
	CHECK(libConfig().folders.size() == 2 && libConfig().folders[0].path == "sd:/ROMS/DS");

	// Point the folder list at the scratch tree, non-recursive first.
	libConfig().folders.clear();
	libConfig().folders.push_back(LibFolder{ roms, false });
	libConfig().render = LIB_RENDER_GXACCURATE;
	CHECK(libSaveConfig());
	libInit((sd + "/DiiS").c_str(), saves.c_str());   // reload from disk
	CHECK(libConfig().folders.size() == 1 && libConfig().render == LIB_RENDER_GXACCURATE);
	CHECK(libScan(false).size() == 1);

	libConfig().folders[0].subfolders = true;
	const std::vector<LibGame> first = libScan(false);
	CHECK(first.size() == probed.size());
	CHECK(exists(sd + "/DiiS/library.cache"));

	// Second scan must come from the cache (no ROM reads) and match.
	const std::vector<LibGame> second = libScan(false);
	CHECK(second.size() == first.size());
	for (size_t i = 0; i < first.size() && i < second.size(); i++) {
		CHECK(first[i].path == second[i].path && first[i].info.id == second[i].info.id);
		CHECK(first[i].info.nlines == second[i].info.nlines);
		for (int l = 0; l < LIB_TITLE_LINES; l++) CHECK(first[i].info.lines[l] == second[i].info.lines[l]);
		CHECK(!memcmp(first[i].icon, second[i].icon, sizeof(first[i].icon)));
	}

	// Launch the first DS ROM: old saves are moved, game.ini written.
	const Probed *ds = 0, *gba = 0;
	for (size_t i = 0; i < probed.size(); i++) {
		if (!probed[i].info.isGba && !ds) ds = &probed[i];
		if (probed[i].info.isGba && !gba) gba = &probed[i];
	}
	if (ds) {
		const std::string base = libBaseName(ds->file.c_str());
		const std::string rom = (ds == &probed[0] ? roms : sub) + "/" + ds->file;
		write_text(saves + "/" + base + ".dsv", "battery");
		write_text(saves + "/" + base + ".ds0", "state zero");
		write_text(saves + "/" + base + ".ds9", "state nine");
		LibLaunch L;
		CHECK(libPrepareLaunch(rom.c_str(), L));
		CHECK(L.dir == sd + "/DiiS/games/" + libFolderName(ds->info) + "/");
		CHECK(L.cpu == LIB_CPU_JIT && L.render == LIB_RENDER_GXACCURATE);
		CHECK(exists(L.dir + "save.dsv") && exists(L.dir + "state0.ds0") && exists(L.dir + "state9.ds9"));
		CHECK(!exists(saves + "/" + base + ".dsv") && !exists(saves + "/" + base + ".ds0"));
		CHECK(L.messages.size() == 3);
		for (size_t m = 0; m < L.messages.size(); m++) printf("  launch 1: %s\n", L.messages[m].c_str());
		LibIni gi;
		CHECK(gi.load((L.dir + "game.ini").c_str()));
		CHECK(gi.get("id") == ds->info.id && gi.get("rom") == rom && !gi.find("cpu") && !gi.find("render"));

		// Per-game override + a conflicting old file: the old one stays.
		LibGameSettings s = { LIB_CPU_INTERP, LIB_RENDER_DEFAULT };
		CHECK(libSaveGameSettings(L.dir, L.info, rom.c_str(), s));
		write_text(saves + "/" + base + ".ds0", "another state zero");
		LibLaunch L2;
		CHECK(libPrepareLaunch(rom.c_str(), L2));
		CHECK(L2.dir == L.dir && L2.cpu == LIB_CPU_INTERP && L2.render == LIB_RENDER_GXACCURATE);
		CHECK(exists(saves + "/" + base + ".ds0"));
		CHECK(L2.messages.size() == 1 && L2.messages[0].find("Kept") == 0);
		for (size_t m = 0; m < L2.messages.size(); m++) printf("  launch 2: %s\n", L2.messages[m].c_str());

		// A renamed title still finds the folder by id prefix.
		LibRomInfo renamed = ds->info;
		renamed.lines[0] = "Something Else";
		CHECK(libEnsureGameDir(renamed) == L.dir);
	}

	// GBA: banner.ini / icon.png in the game folder override the defaults.
	if (gba) {
		const std::string rom = sub + "/" + gba->file;
		LibLaunch L;
		CHECK(libPrepareLaunch(rom.c_str(), L));
		CHECK(L.dir == sd + "/DiiS/games/" + libFolderName(gba->info) + "/");
		write_text(L.dir + "banner.ini", "line1=Custom Line One\nline2=Second\nline3=Third\n");
		std::vector<uint8_t> rgba(32 * 32 * 4);
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x++) {
				uint8_t *o = &rgba[(y * 32 + x) * 4];
				o[0] = x * 8; o[1] = y * 8; o[2] = 0x80; o[3] = (x < 4) ? 0 : 255;
			}
		write_file(L.dir + "icon.png", encode_png(rgba.data(), 32, 32));
		const std::vector<LibGame> &g = libScan(false);
		bool found = false;
		for (size_t i = 0; i < g.size(); i++) {
			if (g[i].info.id != gba->info.id) continue;
			found = true;
			CHECK(g[i].info.nlines == 3 && g[i].info.lines[0] == "Custom Line One" && g[i].info.lines[2] == "Third");
			CHECK(g[i].hasIcon && (g[i].icon[0] & 0xF000) == 0 && (g[i].icon[32 * 31 + 31] & 0x8000));
		}
		CHECK(found);
	}

	printf("\nscan ok: %u ROMs, cache %s\n", (unsigned)first.size(), (sd + "/DiiS/library.cache").c_str());
}

int main(int argc, char **argv)
{
	char abs[4096];
	const char *rd = argc > 1 ? argv[1] : "../../testdata";
	const std::string romDir = realpath(rd, abs) ? abs : rd;   // symlink targets must be absolute
	const std::string outDir = argc > 2 ? argv[2] : "../benchmark/results/library-test";
	mkdir(outDir.c_str(), 0777);

	test_crc_and_clean();
	test_png_roundtrip();
	std::vector<Probed> probed;
	dump_roms(romDir, outDir, probed);
	test_library(romDir, outDir, probed);

	if (g_failures) {
		printf("\n%d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("all library tests passed (icons in %s)\n", outDir.c_str());
	return 0;
}
