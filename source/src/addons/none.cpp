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

static BOOL None_init(void) { return (TRUE); }
static void None_reset(void) {}
static void None_close(void) {}
static void None_config(void) {}
static void None_write08(u32 adr, u8 val) {}
static void None_write16(u32 adr, u16 val) {}
static void None_write32(u32 adr, u32 val) {}
static u8   None_read08(u32 adr){ return (0xFF); }
static u16  None_read16(u32 adr){ return (0xFFFF); }
static u32  None_read32(u32 adr){ return (0xFFFFFFFF); }
static void None_info(char *info) { strcpy(info, "Nothing in GBA slot"); }

ADDONINTERFACE addonNone = {
				"NONE",
				None_init,
				None_reset,
				None_close,
				None_config,
				None_write08,
				None_write16,
				None_write32,
				None_read08,
				None_read16,
				None_read32,
				None_info};
