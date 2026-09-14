/*
    harness_boot.cpp - see harness_boot.h. Compiles to nothing without
    -DDESMUME_HARNESS -DHARNESS_BOOT.
*/
#include "harness.h"
#include "harness_boot.h"

#if defined(DESMUME_HARNESS) && defined(HARNESS_BOOT)

#include "harness_wire.h"
#include "../NDSSystem.h"
#include "../saves.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef HARNESS_BOOT_MAX_ROMS
#define HARNESS_BOOT_MAX_ROMS 16
#endif
#define HARNESS_BOOT_PATHLEN 256

static struct {
	int  loaded;
	int  nroms;
	int  idx;
	char rom[HARNESS_BOOT_MAX_ROMS][HARNESS_BOOT_PATHLEN];
	u32  frames_per_rom;
	u32  frame_every;
	int  core;
	int  usb;
	int  autoload_slot;
	volatile int next_req;
} s_m = { 0, 0, 0, {{0}}, 0, 0, -1, -1, -1, 0 };

static const char *hb_basename(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++)
		if (*q == '/' || *q == '\\' || *q == ':')
			b = q + 1;
	return b;
}

// Trim leading/trailing ASCII whitespace in place; also drops a trailing '#'
// comment. Returns the (possibly advanced) start pointer.
static char *hb_trim(char *s)
{
	char *h = s;
	while (*h == ' ' || *h == '\t' || *h == '\r' || *h == '\n') h++;
	for (char *p = h; *p; p++)
		if (*p == '#') { *p = 0; break; }
	char *e = h + strlen(h);
	while (e > h && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
	return h;
}

int harness_boot_load(const char *mount)
{
	if (s_m.loaded) return s_m.nroms;
	s_m.loaded = 1;

	char cfg[HARNESS_BOOT_PATHLEN];
	snprintf(cfg, sizeof cfg, "%s/harness.cfg", mount ? mount : "sd:");
	FILE *f = fopen(cfg, "r");
	if (!f) return 0;

	char line[HARNESS_BOOT_PATHLEN + 16];
	while (fgets(line, sizeof line, f)) {
		char *s = hb_trim(line);
		if (!*s) continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = hb_trim(s);
		char *val = hb_trim(eq + 1);
		if (!strcmp(key, "rom")) {
			if (s_m.nroms < HARNESS_BOOT_MAX_ROMS && *val) {
				strncpy(s_m.rom[s_m.nroms], val, HARNESS_BOOT_PATHLEN - 1);
				s_m.rom[s_m.nroms][HARNESS_BOOT_PATHLEN - 1] = 0;
				s_m.nroms++;
			}
		} else if (!strcmp(key, "frames_per_rom")) {
			s_m.frames_per_rom = (u32)strtoul(val, 0, 10);
		} else if (!strcmp(key, "frame_every")) {
			s_m.frame_every = (u32)strtoul(val, 0, 10);
		} else if (!strcmp(key, "core")) {
			s_m.core = (int)strtol(val, 0, 10);
		} else if (!strcmp(key, "usb")) {
			s_m.usb = (int)strtol(val, 0, 10);
		} else if (!strcmp(key, "autoload_slot")) {
			s_m.autoload_slot = (int)strtol(val, 0, 10);
		}
	}
	fclose(f);
	return s_m.nroms;
}

int  harness_boot_have(void)            { return s_m.nroms > 0; }
int  harness_boot_rom_index(void)       { return s_m.idx; }
int  harness_boot_rom_count(void)       { return s_m.nroms; }
u32  harness_boot_frames_per_rom(void)  { return s_m.frames_per_rom; }
u32  harness_boot_frame_every(void)     { return s_m.frame_every; }
int  harness_boot_core(void)            { return s_m.core; }
int  harness_boot_usb(void)             { return s_m.usb; }
int  harness_boot_autoload_slot(void)   { return s_m.autoload_slot; }

const char *harness_boot_rom_path(void)
{
	if (s_m.nroms == 0) return 0;
	return s_m.rom[s_m.idx];
}

const char *harness_boot_rom_name(void)
{
	if (s_m.nroms == 0) return "";
	return hb_basename(s_m.rom[s_m.idx]);
}

void harness_boot_request_next(void)   { s_m.next_req = 1; }
int  harness_boot_next_requested(void) { return s_m.next_req; }

int harness_boot_advance(void)
{
	s_m.next_req = 0;
	if (s_m.nroms == 0) return 0;
	if (s_m.idx + 1 >= s_m.nroms) return 0;
	s_m.idx++;
	if (NDS_LoadROM(s_m.rom[s_m.idx], 0) < 0)
		return 0;
	int slot = s_m.autoload_slot;
	if (slot >= 0)
		loadstate_slot(slot);
	return 1;
}

void harness_boot_announce(void)
{
	if (s_m.nroms == 0) return;
	char buf[HARNESS_BOOT_PATHLEN];
	int n = snprintf(buf, sizeof buf, "rom=%s", hb_basename(s_m.rom[s_m.idx]));
	if (n > 0)
		harness_send(HARNESS_PKT_CTRL, buf, (u32)n);
}

#endif // DESMUME_HARNESS && HARNESS_BOOT
