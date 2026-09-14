/*
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii
	
    Structures taken from Michael Chisholm's FAT library (2006)
  
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

#ifndef __FAT_H__
#define __FAT_H__

#include "types.h"
#include "PACKED.h"
#include "PACKED_END.h"

#define ATTRIB_DIR 0x10
#define ATTRIB_LFN 0x0F

#define FILE_FREE 0xE5
/* Name and extension maximum length */
#define NAME_LEN 8
#define EXT_LEN 3

// Boot Sector - must be packed
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif

#include "PACKED.h"
struct boot_record
{
        u8      jmpBoot[3];
        u8      OEMName[8];
	// BIOS Parameter Block
        u16     bytesPerSector;
        u8      sectorsPerCluster;
        u16     reservedSectors;
        u8      numFATs;
        u16     rootEntries;
        u16     numSectorsSmall;
        u8      mediaDesc;
        u16     sectorsPerFAT;
        u16     sectorsPerTrk;
        u16     numHeads;
        u32     numHiddenSectors;
        u32     numSectors;
	
	struct  
	{
		// Ext BIOS Parameter Block for FAT16
        u8      driveNumber;
        u8      reserved1;
        u8      extBootSig;
        u32     volumeID;
        u8      volumeLabel[11];
        u8      fileSysType[8];
		// Bootcode
        u8      bootCode[448];
        u16	signature;	
	} __PACKED fat16;

} __PACKED;
typedef struct boot_record BOOT_RECORD;
#include "PACKED_END.h"

// Directory entry - must be packed
#include "PACKED.h"
struct dir_ent
{
        u8      name[NAME_LEN];
        u8      ext[EXT_LEN];
        u8      attrib;
        u8      reserved;
        u8      cTime_ms;
        u16     cTime;
        u16     cDate;
        u16     aDate;
        u16     startClusterHigh;
        u16     mTime;
        u16     mDate;
        u16     startCluster;
        u32     fileSize;
} __PACKED;
typedef struct dir_ent DIR_ENT;
#include "PACKED_END.h"

#endif //
