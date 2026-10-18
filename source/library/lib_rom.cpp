/****************************************************************************
 * DiiS - lib_rom.cpp   (see lib_rom.h)
 ***************************************************************************/
#include "lib_rom.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vector>

// DS header / banner layout (GBATEK "DS Cartridge Header", "DS Cartridge Icon/Title").
#define DS_HDR_SIZE        0x200
#define DS_HDR_CODE        0x0C
#define DS_HDR_BANNER_OFS  0x68
#define DS_HDR_CRC         0x15E
#define DS_BANNER_SIZE     0x840   // version 1: icon + 6 titles; later versions only add languages
#define DS_BANNER_ICON     0x20
#define DS_BANNER_TITLE    0x240   // 0x100 bytes (128 UTF-16LE chars) per language
#define DS_LANG_JAPANESE   0
#define DS_LANG_ENGLISH    1

// GBA header (GBATEK "GBA Cartridge Header").
#define GBA_HDR_SIZE       0xC0
#define GBA_HDR_TITLE      0xA0
#define GBA_HDR_CODE       0xAC
#define GBA_HDR_CHECKSUM   0xBD
#define GBA_HOMEBREW_CRC_SIZE 0x10000

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

uint16_t libCrc16(const uint8_t *data, size_t len, uint16_t crc)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
	}
	return crc;
}

bool libIsRomName(const char *name, bool *isGba)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return false;
	if (!strcasecmp(dot, ".nds")) { if (isGba) *isGba = false; return true; }
	if (!strcasecmp(dot, ".gba")) { if (isGba) *isGba = true; return true; }
	return false;
}

std::string libBaseName(const char *path)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	const char *dot = strrchr(base, '.');
	return dot && dot > base ? std::string(base, dot - base) : std::string(base);
}

// Game code -> 4 FAT-safe characters.
static void put_code(std::string &id, const uint8_t *code)
{
	for (int i = 0; i < 4; i++) {
		const uint8_t c = code[i];
		const bool ok = c > 0x20 && c < 0x7F && !strchr("\\/:*?\"<>|", c);
		id += ok ? (char)c : '_';
	}
}

static void put_hex(std::string &id, unsigned v, int digits)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%0*X", digits, v);
	id += buf;
}

static void put_utf8(std::string &s, uint32_t cp)
{
	if (cp < 0x80) s += (char)cp;
	else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
	else if (cp < 0x10000) {
		s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
	} else {
		s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
		s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
	}
}

// One banner title (128 UTF-16LE chars, NUL-terminated or full) -> up to 3
// lines split at U+000A. Returns the line count (0 for an empty title).
static int decode_title(const uint8_t *t, std::string lines[LIB_TITLE_LINES])
{
	int n = 0;
	std::string cur;
	bool any = false;
	for (int i = 0; i < 128; i++) {
		uint32_t c = rd16(t + i * 2);
		if (!c) break;
		any = true;
		if (c == 0x0A) {
			if (n < LIB_TITLE_LINES - 1) { lines[n++] = cur; cur.clear(); }
			else cur += ' ';   // a 4th line joins the 3rd rather than being lost
			continue;
		}
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < 128) {
			const uint32_t lo = rd16(t + (i + 1) * 2);
			if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
		}
		put_utf8(cur, c);
	}
	if (!any) return 0;
	lines[n++] = cur;
	return n;
}

static long file_size(FILE *f)
{
	if (fseek(f, 0, SEEK_END) != 0) return -1;
	const long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	return sz;
}

static bool read_at(FILE *f, long ofs, void *dst, size_t n)
{
	return fseek(f, ofs, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

static bool probe_ds(FILE *f, long size, const char *path, LibRomInfo &out)
{
	uint8_t hdr[DS_HDR_SIZE];
	if (size < DS_HDR_SIZE || !read_at(f, 0, hdr, sizeof(hdr))) return false;
	put_code(out.id, hdr + DS_HDR_CODE);
	out.id += '-';
	put_hex(out.id, rd16(hdr + DS_HDR_CRC), 4);

	const uint32_t bofs = rd32(hdr + DS_HDR_BANNER_OFS);
	static uint8_t ban[DS_BANNER_SIZE];
	if (bofs >= DS_HDR_SIZE && (long)bofs + DS_BANNER_SIZE <= size && read_at(f, bofs, ban, sizeof(ban))) {
		const uint16_t ver = rd16(ban);
		if (ver == 0x0001 || ver == 0x0002 || ver == 0x0003 || ver == 0x0103) {
			out.nlines = decode_title(ban + DS_BANNER_TITLE + DS_LANG_ENGLISH * 0x100, out.lines);
			if (!out.nlines)
				out.nlines = decode_title(ban + DS_BANNER_TITLE + DS_LANG_JAPANESE * 0x100, out.lines);
			memcpy(out.iconRaw, ban + DS_BANNER_ICON, LIB_ICON_RAW_SIZE);
			out.hasIcon = true;
		}
	}
	if (!out.nlines) { out.lines[0] = libBaseName(path); out.nlines = 1; }
	return true;
}

static bool probe_gba(FILE *f, long size, const char *path, LibRomInfo &out)
{
	uint8_t hdr[GBA_HDR_SIZE];
	if (size < GBA_HDR_SIZE || !read_at(f, 0, hdr, sizeof(hdr))) return false;
	const uint8_t *code = hdr + GBA_HDR_CODE;
	bool blank = true;
	for (int i = 0; i < 4; i++) if (code[i] != 0 && code[i] != ' ') blank = false;
	put_code(out.id, code);
	out.id += '-';
	put_hex(out.id, hdr[GBA_HDR_CHECKSUM], 2);
	if (blank) {
		std::vector<uint8_t> buf(size < GBA_HOMEBREW_CRC_SIZE ? size : GBA_HOMEBREW_CRC_SIZE);
		if (!read_at(f, 0, buf.data(), buf.size())) return false;
		out.id += '-';
		put_hex(out.id, libCrc16(buf.data(), buf.size()), 4);
	}

	// Internal title: 12 bytes, NUL/space padded; padding isn't content.
	std::string title;
	for (int i = 0; i < 12 && hdr[GBA_HDR_TITLE + i]; i++) {
		const uint8_t c = hdr[GBA_HDR_TITLE + i];
		if (c >= 0x20 && c < 0x7F) title += (char)c;
	}
	while (!title.empty() && title[title.size() - 1] == ' ') title.erase(title.size() - 1);
	out.lines[0] = title;
	out.lines[1] = libBaseName(path);
	out.nlines = 2;
	return true;
}

bool libProbeRom(const char *path, LibRomInfo &out)
{
	out.isGba = false;
	out.id.clear();
	for (int i = 0; i < LIB_TITLE_LINES; i++) out.lines[i].clear();
	out.nlines = 0;
	out.hasIcon = false;
	memset(out.iconRaw, 0, sizeof(out.iconRaw));

	bool gba;
	if (!libIsRomName(path, &gba)) return false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	const long size = file_size(f);
	out.isGba = gba;
	const bool ok = size > 0 && (gba ? probe_gba(f, size, path, out) : probe_ds(f, size, path, out));
	fclose(f);
	return ok;
}

void libDecodeDsIcon(const uint8_t raw[LIB_ICON_RAW_SIZE], uint16_t *dst)
{
	uint16_t pal[16];
	// Colour 0 is transparent on the DS, but banner art is drawn (and hand-antialiased) over the
	// DS menu's white, so it is drawn opaque white here rather than over our own background.
	pal[0] = 0xFFFF;
	for (int i = 1; i < 16; i++) {
		const uint16_t c = rd16(raw + 512 + i * 2);   // BGR555
		pal[i] = (uint16_t)(0x8000 | ((c & 0x1F) << 10) | (c & 0x03E0) | ((c >> 10) & 0x1F));
	}
	for (int t = 0; t < 16; t++) {
		const uint8_t *tile = raw + t * 32;
		const int tx = (t & 3) * 8, ty = (t >> 2) * 8;
		for (int i = 0; i < 32; i++) {
			const int x = tx + (i & 3) * 2, y = ty + (i >> 2);
			dst[y * LIB_ICON_W + x]     = pal[tile[i] & 0xF];
			dst[y * LIB_ICON_W + x + 1] = pal[tile[i] >> 4];
		}
	}
}

void libRgbaToRgb5a3(const uint8_t *rgba, int w, int h, uint16_t *dst)
{
	for (int i = 0; i < w * h; i++) {
		const uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
		if (a >= 0xE0)
			dst[i] = (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
		else
			dst[i] = (uint16_t)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
	}
}

void libIconToGxTiles(const uint16_t *linear, uint16_t *tiled)
{
	int o = 0;
	for (int by = 0; by < LIB_ICON_H; by += 4)
		for (int bx = 0; bx < LIB_ICON_W; bx += 4)
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					tiled[o++] = linear[(by + y) * LIB_ICON_W + bx + x];
}

// U+00C0..U+00FF folded to ASCII ("" = dropped).
static const char *const k_latin1_fold[64] = {
	"A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
	"D","N","O","O","O","O","O","x","O","U","U","U","U","Y","TH","ss",
	"a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
	"d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y",
};

std::string libCleanTitle(const std::string &in)
{
	std::string s;
	const uint8_t *p = (const uint8_t *)in.c_str();
	while (*p) {
		uint32_t cp = *p++;
		if (cp >= 0x80) {   // decode one UTF-8 sequence (malformed bytes are dropped)
			int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : -1;
			if (extra < 0) continue;
			cp &= 0x3F >> extra;
			for (; extra > 0 && (*p & 0xC0) == 0x80; extra--) cp = (cp << 6) | (*p++ & 0x3F);
			if (extra) continue;
		}
		if (cp < 0x20 || cp == 0x7F) continue;
		if (cp < 0x80) {
			if (!strchr("\\/:*?\"<>|", (int)cp)) s += (char)cp;
			continue;
		}
		if (cp >= 0xC0 && cp <= 0xFF) s += k_latin1_fold[cp - 0xC0];
	}
	std::string o;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == ' ' && (o.empty() || o[o.size() - 1] == ' ')) continue;
		o += s[i];
	}
	if (o.size() > 64) o.resize(64);
	while (!o.empty() && (o[o.size() - 1] == ' ' || o[o.size() - 1] == '.')) o.erase(o.size() - 1);
	return o;
}

std::string libFolderName(const LibRomInfo &info)
{
	const std::string t = libCleanTitle(info.nlines ? info.lines[0] : std::string());
	return t.empty() ? info.id : info.id + " " + t;
}
