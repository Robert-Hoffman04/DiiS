/*
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
//--DCN: Copied from earlier version of desmumeWii
#ifndef FRONTEND
#define FRONTEND

u16 arm9_gdb_port;
u16 arm7_gdb_port;

int enable_sound;
int disable_limiter;
int cpu_ratio;
int lang;
int showfps = 0;
int vertical = 1;
int frameskip;
const char *nds_file;
const char *cflash_disk_image_file;


// Screen layout/scale etc...
enum {
	SCREEN_VERT_NORMAL = 0,
	SCREEN_VERT_SEPARATED,
	SCREEN_HORI_NORMAL,
	SCREEN_VERT_STRETCH,
	SCREEN_HORI_STRETCH,
	SCREEN_MAIN_NORMAL,
	SCREEN_SUB_NORMAL,
	SCREEN_MAIN_STRETCH,
	SCREEN_SUB_STRETCH,
	SCREEN_VERT_SEPARATED_ROT_90,
	SCREEN_MAX
};

u32 screen_layout = SCREEN_MAX;

// positioning screen vars.
int bottomX, bottomY, topX, topY;
float scalex, scaley;
float width = 256;
float height = 192;
float rotate_angle = 0.0f;

void DoConfig();

void DSEmuGui(char *path,char *out);

#endif
//--DCN: End copy

