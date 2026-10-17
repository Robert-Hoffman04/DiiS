/****************************************************************************
 * DiiS - lib_png.h
 *
 * Minimal PNG reader for the user's optional per-game icon.png (32x32). Just
 * enough of the format for small icons: non-interlaced, grey / RGB / palette
 * (+tRNS) / grey+alpha / RGBA, 8-bit (and 1/2/4-bit grey or palette), all
 * five row filters. Inflate is zlib's. No Wii dependency (host-testable).
 ***************************************************************************/
#ifndef DIIS_LIB_PNG_H
#define DIIS_LIB_PNG_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

// Decode to row-major RGBA8. Images over 256x256 are refused.
bool libPngDecode(const uint8_t *data, size_t len, std::vector<uint8_t> &rgba, int &w, int &h);
bool libPngLoad(const char *path, std::vector<uint8_t> &rgba, int &w, int &h);

#endif // DIIS_LIB_PNG_H
