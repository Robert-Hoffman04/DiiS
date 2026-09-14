/*  Copyright (C) 2009 CrazyMax
	Copyright (C) 2009 DeSmuME team
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

#include "../addons.h"
#include <string.h>

static u8	guitarKeyStatus = 0;

static BOOL guitarGrip_init(void) { return (TRUE); }
static void guitarGrip_reset(void)
{
	//INFO("GuitarGrip: Reset\n");
	guitarKeyStatus = 0;
}
static void guitarGrip_close(void) {}
static void guitarGrip_config(void) {}
static void guitarGrip_write08(u32 adr, u8 val) {}
static void guitarGrip_write16(u32 adr, u16 val) {}
static void guitarGrip_write32(u32 adr, u32 val) {}
static u8   guitarGrip_read08(u32 adr)
{
	//INFO("GuitarGrip: read 08 at 0x%08X\n", adr);
	if (adr == 0x0A000000) return (~guitarKeyStatus);
	return (0xFF);
}
static u16  guitarGrip_read16(u32 adr)
{
	//INFO("GuitarGrip: read 16 at 0x%08X\n", adr);
	if (adr == 0x080000BE) return (0xF9FF);
	if (adr == 0x0801FFFE) return (0xF9FF);

	return (0xFFFF);
}
static u32  guitarGrip_read32(u32 adr)
{
	//INFO("GuitarGrip: read 32 at 0x%08X\n", adr);
	return (0xFFFFFFFF);
}
static void guitarGrip_info(char *info) { strcpy(info, "Guitar Grip for Guitar Hero games"); }

void guitarGrip_setKey(bool green, bool red, bool yellow, bool blue)
{
	guitarKeyStatus = 0 | (green << 6) | (red << 5) | (yellow << 4) | (blue << 3);
}

ADDONINTERFACE addonGuitarGrip = {
				"Guitar Grip",
				guitarGrip_init,
				guitarGrip_reset,
				guitarGrip_close,
				guitarGrip_config,
				guitarGrip_write08,
				guitarGrip_write16,
				guitarGrip_write32,
				guitarGrip_read08,
				guitarGrip_read16,
				guitarGrip_read32,
				guitarGrip_info};
