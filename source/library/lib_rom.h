/****************************************************************************
 * DiiS - lib_rom.h
 *
 * ROM library core, part 1: what one ROM file says about itself. Reads the
 * DS header + banner (or the GBA header) and turns it into the game id, the
 * three title lines and the 32x32 icon. Pure stdio + buffers, no Wii/NDS
 * dependency, so tools/library-test builds it on the host.
 *
 *   DS id:  <game code>-<header CRC16 at 0x15E, 4 hex>          ASMO-4F21
 *   GBA id: <game code>-<header checksum at 0xBD, 2 hex>         AXVE-7C
 *           + -<CRC16 of the first 64 KB, 4 hex> when the code is blank.
 *
 * Code characters FAT can't take (or blanks/control) become '_'.
 ***************************************************************************/
#ifndef DIIS_LIB_ROM_H
#define DIIS_LIB_ROM_H

#include <stdint.h>
#include <string>

#define LIB_ICON_W 32
#define LIB_ICON_H 32
#define LIB_ICON_RAW_SIZE (512 + 32)   // DS banner 4bpp bitmap + 16-colour palette
#define LIB_TITLE_LINES 3

struct LibRomInfo {
	bool isGba;
	std::string id;
	std::string lines[LIB_TITLE_LINES];   // UTF-8, exactly as stored; unused lines empty
	int nlines;
	bool hasIcon;                         // DS banner icon present (iconRaw valid)
	uint8_t iconRaw[LIB_ICON_RAW_SIZE];   // banner 0x20..0x23F as stored in the ROM
};

// CRC16 as the DS uses it (poly 0xA001 reflected, init 0xFFFF).
uint16_t libCrc16(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);

// Read `path` (.nds or .gba, by extension, case-insensitive) into `out`.
// A DS ROM without a usable banner gets the file name (no extension) as its
// only line and hasIcon=false. Returns false if the file can't be read or is
// too short to hold a header.
bool libProbeRom(const char *path, LibRomInfo &out);

// true for *.nds / *.gba (case-insensitive); *isGba says which.
bool libIsRomName(const char *name, bool *isGba = 0);

// File name of `path` without directory and extension.
std::string libBaseName(const char *path);

// DS banner icon (4bpp 8x8 tiles + BGR555 palette, colour 0 transparent) ->
// row-major RGB5A3 (GX bit layout, not yet block-swizzled; see
// libIconToGxTiles). dst holds LIB_ICON_W*LIB_ICON_H entries.
void libDecodeDsIcon(const uint8_t raw[LIB_ICON_RAW_SIZE], uint16_t *dst);

// RGBA8 (row-major, w*h*4 bytes) -> row-major RGB5A3.
void libRgbaToRgb5a3(const uint8_t *rgba, int w, int h, uint16_t *dst);

// Row-major RGB5A3 32x32 -> GX 4x4-block order (what GX_InitTexObj wants).
void libIconToGxTiles(const uint16_t *linear, uint16_t *tiled);

// Game-folder title text: every character FAT can't take (\ / : * ? " < > |
// and control characters) removed. Non-ASCII is folded to ASCII where it is
// an accented Latin letter and dropped otherwise (libfat's name conversion is
// not UTF-8 safe), runs of spaces collapse, leading/trailing spaces and
// trailing dots go, and the result is capped at 64 bytes.
std::string libCleanTitle(const std::string &utf8);

// "<id> <cleaned first line>", or just "<id>" when no title text is left.
std::string libFolderName(const LibRomInfo &info);

#endif // DIIS_LIB_ROM_H
