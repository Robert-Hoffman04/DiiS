/*  Copyright (C) 2005 Theo Berkau
	Copyright (C) 2005-2006 Guillaume Duhamel
	Copyright (C) 2010 DeSmuME team
	Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii.

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

#ifndef MEM_H
#define MEM_H

#include <stdlib.h>
#include <assert.h>
#include "types.h"

//this was originally declared in MMU.h but we suffered some organizational problems and had to remove it
enum MMU_ACCESS_TYPE
{
	MMU_AT_CODE,    //used for cpu prefetches
	MMU_AT_DATA,    //used for cpu read/write
	MMU_AT_GPU,     //used for gpu read/write
	MMU_AT_DMA,     //used for dma read/write (blocks access to TCM)
	//MMU_AT_DEBUG, //used for emulator debugging functions (bypasses some debug handling)
};

/* Type 1 Memory, faster for byte (8 bits) accesses */

// Big-endian host (Wii / PowerPC): the DS is little-endian, so every word and
// halfword access to emulated memory has to be byte-reversed.  On an address
// that is aligned for its access width, __builtin_bswap on a direct pointer
// load/store compiles to a single lwbrx/lhbrx/stwbrx/sthbrx; the hand-written
// byte assembly below it used to compile to ~8-11 instructions per access, and
// this is on the hottest path in the emulator (instruction prefetch + every
// load/store).  Only the *_guaranteedAligned entry points and the callers that
// force alignment with an address mask use this fast path - a byte-reversed
// load/store raises an alignment interrupt on a misaligned effective address on
// the 750, so the un-guaranteed entry points keep the safe byte path.
#if defined(WORDS_BIGENDIAN) && defined(__GNUC__)
# define DESMUME_FAST_LE_ACCESS 1
#endif

static INLINE u8 T1ReadByte(u8* const mem, const u32 addr)
{
   return mem[addr];
}

static INLINE u16 T1ReadWord_guaranteedAligned(void* const mem, const u32 addr)
{
	assert((addr&1)==0);
#ifdef DESMUME_FAST_LE_ACCESS
   return __builtin_bswap16(*(const u16*)((const u8*)mem + addr));
#elif defined(WORDS_BIGENDIAN)
   return (((u8*)mem)[addr + 1] << 8) | ((u8*)mem)[addr];
#else
   return *(u16*)((u8*)mem + addr);
#endif
}

static INLINE u16 T1ReadWord(void* const mem, const u32 addr)
{
#ifdef WORDS_BIGENDIAN
   return (((u8*)mem)[addr + 1] << 8) | ((u8*)mem)[addr];
#else
   return *((u16 *) ((u8*)mem + addr));
#endif
}

static INLINE u32 T1ReadLong_guaranteedAligned(u8* const  mem, const u32 addr)
{
	assert((addr&3)==0);
#ifdef DESMUME_FAST_LE_ACCESS
   return __builtin_bswap32(*(const u32*)(mem + addr));
#elif defined(WORDS_BIGENDIAN)
   return (mem[addr + 3] << 24 | mem[addr + 2] << 16 |
           mem[addr + 1] << 8 | mem[addr]);
#else
	return *(u32*)(mem + addr);
#endif
}


static INLINE u32 T1ReadLong(u8* const  mem, u32 addr)
{
   addr &= ~3;
#ifdef DESMUME_FAST_LE_ACCESS
   return __builtin_bswap32(*(const u32*)(mem + addr));
#elif defined(WORDS_BIGENDIAN)
   return (mem[addr + 3] << 24 | mem[addr + 2] << 16 |
           mem[addr + 1] << 8 | mem[addr]);
#else
   return *(u32*)(mem + addr);
#endif
}

static INLINE u64 T1ReadQuad(u8* const mem, const u32 addr)
{
#ifdef WORDS_BIGENDIAN
   return (u64(mem[addr + 7]) << 56 | u64(mem[addr + 6]) << 48 |
           u64(mem[addr + 5]) << 40 | u64(mem[addr + 4]) << 32 |
           u64(mem[addr + 3]) << 24 | u64(mem[addr + 2]) << 16 |
           u64(mem[addr + 1]) << 8  | u64(mem[addr    ]));
#else
   return *((u64 *) (mem + addr));
#endif
}

static INLINE void T1WriteByte(u8* const mem, const u32 addr, const u8 val)
{
   mem[addr] = val;
}

static INLINE void T1WriteWord(u8* const mem, const u32 addr, const u16 val)
{

#ifdef WORDS_BIGENDIAN
   mem[addr + 1] = val >> 8;
   mem[addr] = val & 0xFF;
#else
   *((u16 *) (mem + addr)) = val;
#endif
}

// see the note above T1ReadWord_guaranteedAligned - caller guarantees (addr&1)==0
static INLINE void T1WriteWord_guaranteedAligned(u8* const mem, const u32 addr, const u16 val)
{
	assert((addr&1)==0);
#ifdef DESMUME_FAST_LE_ACCESS
   *(u16*)(mem + addr) = __builtin_bswap16(val);
#elif defined(WORDS_BIGENDIAN)
   mem[addr + 1] = val >> 8;
   mem[addr] = val & 0xFF;
#else
   *((u16 *) (mem + addr)) = val;
#endif
}

static INLINE void T1WriteLong(u8* const mem, const u32 addr, const u32 val)
{
#ifdef WORDS_BIGENDIAN
   mem[addr + 3] = val >> 24;
   mem[addr + 2] = (val >> 16) & 0xFF;
   mem[addr + 1] = (val >> 8) & 0xFF;
   mem[addr] = val & 0xFF;
#else
   *((u32 *) (mem + addr)) = val;
#endif
}

// see the note above T1ReadWord_guaranteedAligned - caller guarantees (addr&3)==0
static INLINE void T1WriteLong_guaranteedAligned(u8* const mem, const u32 addr, const u32 val)
{
	assert((addr&3)==0);
#ifdef DESMUME_FAST_LE_ACCESS
   *(u32*)(mem + addr) = __builtin_bswap32(val);
#elif defined(WORDS_BIGENDIAN)
   mem[addr + 3] = val >> 24;
   mem[addr + 2] = (val >> 16) & 0xFF;
   mem[addr + 1] = (val >> 8) & 0xFF;
   mem[addr] = val & 0xFF;
#else
   *((u32 *) (mem + addr)) = val;
#endif
}

static INLINE void T1WriteQuad(u8* const mem, const u32 addr, const u64 val)
{
#ifdef WORDS_BIGENDIAN
	mem[addr + 7] = (val >> 56);
	mem[addr + 6] = (val >> 48) & 0xFF;
	mem[addr + 5] = (val >> 40) & 0xFF;
	mem[addr + 4] = (val >> 32) & 0xFF;
	mem[addr + 3] = (val >> 24) & 0xFF;
    mem[addr + 2] = (val >> 16) & 0xFF;
    mem[addr + 1] = (val >> 8) & 0xFF;
    mem[addr] = val & 0xFF;
#else
	*((u64 *) (mem + addr)) = val;
#endif
}


static INLINE u16 HostReadWord(u8* const mem, const u32 addr)
{
   return *((u16 *) (mem + addr));
}


static INLINE void HostWriteWord(u8* const mem, const u32 addr, const u16 val)
{
   *((u16 *) (mem + addr)) = val;
}

static INLINE void HostWriteTwoWords(u8* const mem, const u32 addr, const u32 val)
{
#ifdef WORDS_BIGENDIAN
   *((u16 *) (mem + addr + 2)) = val >> 16;
   *((u16 *) (mem + addr)) = val & 0xFFFF;
#else
   *((u32 *) (mem + addr)) = val;
#endif
}

#endif
