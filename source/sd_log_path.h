/****************************************************************************
 * DiiS - sd_log_path.h
 *
 * Per-game SD log paths for the hardware perf logs (perf_zones, gxstats):
 * sd:/perf/<rom name>_<kind>.log, so each game keeps its own file instead of
 * every run overwriting one global log. The ROM name is set once the ROM is
 * chosen (sdLogSetRom); before that, or for an empty name, it is "unknown".
 * Once the ROM library has given the game its folder (sdLogSetDir), the logs
 * go there instead, as <game folder>/<kind>.log.
 ***************************************************************************/
#ifndef DIIS_SD_LOG_PATH_H
#define DIIS_SD_LOG_PATH_H

#include <stddef.h>

// Remember the loaded ROM (full path); only its file name, minus extension, is kept.
void sdLogSetRom(const char *romPath);

// Per-game folder (with trailing '/'), or 0/"" to go back to sd:/perf/.
void sdLogSetDir(const char *dir);

// Write "<game folder>/<kind>.log", else "sd:/perf/<rom>_<kind>.log" into buf (creating sd:/perf if needed); returns buf.
const char *sdLogPath(char *buf, size_t n, const char *kind);

#endif // DIIS_SD_LOG_PATH_H
