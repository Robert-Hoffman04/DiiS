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

#ifndef __CFLASH_H__
#define __CFLASH_H__

#include "common.h"

#ifndef EXPERIMENTAL_GBASLOT
#include "fat.h"

typedef struct {
	int level,parent,filesInDir;
} FILE_INFO;


bool cflash_init( const char *disk_image_filename);

unsigned int cflash_read(unsigned int address);

void cflash_write(unsigned int address,unsigned int data);

void cflash_close( void);
#endif

#endif
