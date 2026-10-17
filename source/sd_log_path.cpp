/****************************************************************************
 * DiiS - sd_log_path.cpp   (see sd_log_path.h)
 ***************************************************************************/
#include "sd_log_path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static char s_rom[64] = "unknown";
static char s_dir[256];

void sdLogSetDir(const char *dir)
{
	strncpy(s_dir, dir ? dir : "", sizeof(s_dir) - 1);
	s_dir[sizeof(s_dir) - 1] = 0;
}

void sdLogSetRom(const char *romPath)
{
	if (!romPath || !*romPath) return;
	const char *base = strrchr(romPath, '/');
	base = base ? base + 1 : romPath;
	const char *dot = strrchr(base, '.');
	size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
	if (len >= sizeof(s_rom)) len = sizeof(s_rom) - 1;
	for (size_t i = 0; i < len; i++) {
		const char c = base[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		                c == '-' || c == '_' || c == '.';
		s_rom[i] = ok ? c : '_';
	}
	s_rom[len] = 0;
	if (!len) strcpy(s_rom, "unknown");
}

const char *sdLogPath(char *buf, size_t n, const char *kind)
{
	if (s_dir[0]) {
		snprintf(buf, n, "%s%s.log", s_dir, kind);
		return buf;
	}
	mkdir("sd:/perf", 0777);   // fails harmlessly when it already exists
	snprintf(buf, n, "sd:/perf/%s_%s.log", s_rom, kind);
	return buf;
}
