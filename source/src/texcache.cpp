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
#include <string.h>
#include <algorithm>
#include <assert.h>
#include <map>
#ifdef GXRENDER_DEBUG_LOG
#include <stdio.h>
#endif
#include "texcache.h"
#include "bits.h"
#include "common.h"
#include "debug.h"
#include "gfx3d.h"
#include "GPU.h"
#include "NDSSystem.h"

//--DCN (what is this wizardry!?)
// It's shifting magic! Works for both signed and unsigned ints.
// This is FAR better than multiplication because multiplication:
// A) Takes up the entire Integer Unit the entire time
// B) Can only be performed on one Integer Unit (IU1)
// -- These can be pipelined and performed on IU2 as well. Score!
#define INTx3(i) ((i<<1) + i)
#define INTx5(i) ((i<<2) + i)
#define INTx6(i) ((i<<2) + (i<<1))
#define INTx7(i) ((i<<3) - i)
#define INTx9(i) ((i<<3) + i)
#define INTx10(i) ((i<<3) + (i<<1))

using std::min;
using std::max;

//only dump this from ogl renderer. for now, softrasterizer creates things in an incompatible pixel format
//#define DEBUG_DUMP_TEXTURE

#define CONVERT(color,alpha) ((TEXFORMAT == TexFormat_32bpp)?(RGB15TO32(color,alpha)):RGB15TO6665(color,alpha))

//This class represents a number of regions of memory which should be viewed as contiguous
class MemSpan
{
public:
	static const int MAXSIZE = 8;

	MemSpan() 
		: numItems(0), size(0)
	{}

	int numItems;

	struct Item {
		u32 start;
		u32 len;
		u8* ptr;
		u32 ofs; //offset within the memspan
	} items[MAXSIZE];

	int size;

	//this MemSpan shall be considered the first argument to a standard memcmp
	//the length shall be as specified in this MemSpan, unless you specify otherwise
	int memcmp(void* buf2, int size=-1)
	{
		if(size==-1){
			size = this->size;
		}else{
			size = min(this->size,size);
		}
		for(int i=0;i<numItems;i++)
		{
			Item &item = items[i];
			int todo = min((int)item.len,size);
			size -= todo;
			int temp = ::memcmp(item.ptr,((u8*)buf2)+item.ofs,todo);
			if(temp) return temp;
			if(size == 0) break;
		}
		return 0;
	}

	//TODO - get rid of duplication between these two methods.

	//dumps the memspan to the specified buffer
	//you may set size to limit the size to be copied
	int dump(void* buf, int size=-1)
	{
		if(size==-1) size = this->size;
		size = min(this->size,size);
		u8* bufptr = (u8*)buf;
		int done = 0;
		for(int i=0;i<numItems;i++)
		{
			Item item = items[i];
			int todo = min((int)item.len,size);
			size -= todo;
			done += todo;
			memcpy(bufptr,item.ptr,todo);
			bufptr += todo;
			if(size==0) return done;
		}
		return done;
	}

	// this function does the same than dump
	// but works for both little and big endian
	// when buf is an u16 array
	int dump16(void* buf, int size=-1)
	{
		if(size==-1) size = this->size;
		size = min(this->size,size);
		u16* bufptr = (u16*)buf;
		int done = 0;
		for(int i=0;i<numItems;i++)
		{
			Item item = items[i];
			u8 * src = (u8 *) item.ptr;
			int todo = min((int)item.len,size);
			size -= todo;
			done += todo;
			todo >>= 1;
			for(int j = 0;j < todo; ++j)
			{
				u16 tmp = *src++;
				tmp |= *(src++) << 8;
				*bufptr++ = tmp;
			}
			if(size==0) return done;
		}
		return done;
	}
};

//creates a MemSpan in texture memory
static MemSpan MemSpan_TexMem(u32 ofs, u32 len) 
{
	MemSpan ret;
	ret.size = len;
	u32 currofs = 0;
	while(len) {
		MemSpan::Item &curr = ret.items[ret.numItems++];
		curr.start = ofs&0x1FFFF;
		u32 slot = (ofs>>17)&3; //slots will wrap around
		curr.len = min(len,0x20000-curr.start);
		curr.ofs = currofs;
		len -= curr.len;
		ofs += curr.len;
		currofs += curr.len;
		u8* ptr = MMU.texInfo.textureSlotAddr[slot];
		
		//TODO - don't alert if the masterbrightnesses are max or min
		if(ptr == MMU.blank_memory) {
			PROGINFO("Tried to reference unmapped texture memory: slot %d\n",slot);
		}
		curr.ptr = ptr + curr.start;
	}
	return ret;
}

//creates a MemSpan in texture palette memory
static MemSpan MemSpan_TexPalette(u32 ofs, u32 len) 
{
	MemSpan ret;
	ret.size = len;
	u32 currofs = 0;
	while(len) {
		MemSpan::Item &curr = ret.items[ret.numItems++];
		curr.start = ofs&0x3FFF;
		u32 slot = (ofs>>14)&7; //this masks to 8 slots, but there are really only 6
		//--DCN: ...then why are we masking to 8 slots? Hmm?
		if(slot>5) {
			PROGINFO("Texture palette overruns texture memory. Wrapping at palette slot 0.\n");
			slot -= 5;
		}
		curr.len = min(len,0x4000-curr.start);
		curr.ofs = currofs;
		len -= curr.len;
		ofs += curr.len;
		//if(len != 0) 
			//here is an actual test case of bank spanning
		currofs += curr.len;
		u8* ptr = MMU.texInfo.texPalSlot[slot];
		
		//TODO - don't alert if the masterbrightnesses are max or min
		if(ptr == MMU.blank_memory) {
			PROGINFO("Tried to reference unmapped texture palette memory: 16k slot #%d\n",slot);
		}
		curr.ptr = ptr + curr.start;
	}
	return ret;
}

#if defined (DEBUG_DUMP_TEXTURE) && defined (WIN32)
#define DO_DEBUG_DUMP_TEXTURE
static void DebugDumpTexture(TexCacheItem* item)
{
	static int ctr=0;
	char fname[100];
	sprintf(fname,"c:\\dump\\%d.bmp", ctr);
	ctr++;

	NDS_WriteBMP_32bppBuffer(item->sizeX,item->sizeY,item->decoded,fname);
}
#endif

//notes on the cache:
//I am really unhappy with the ref counting. this needs to be automatic.
//We could do something better than a linear search through cache items, but it may not be worth it.
//Also we may need to rescan more often (every time a sample loops)
class TexCache
{
public:
	TexCache()
		: cache_size(0)
	{}

	TTexCacheItemMultimap index;

	//this ought to be enough for anyone
	static const u32 kMaxCacheSize = 4*1024*1024; // 4MB, Lowered for Desmume Wii.
	//this is not really precise, it is off by a constant factor
	u32 cache_size;

	void list_remove(TexCacheItem* item)
	{
		index.erase(item->iterator);
		cache_size -= item->decode_len;
	}

	void list_push_front(TexCacheItem* item)
	{
		item->iterator = index.insert(std::make_pair(item->texformat,item));
		cache_size += item->decode_len;
	}

	template<TexCache_TexFormat TEXFORMAT>
	TexCacheItem* scan(u32 format, u32 texpal)
	{
		//for each texformat, number of palette entries
		static const int palSizes[] = {0, 32, 4, 16, 256, 0, 8, 0};

		//for each texformat, multiplier from numtexels to numbytes (fixed point 30.2)
		static const int texSizes[] = {0, 4, 1, 2, 4, 1, 4, 8};

		//used to hold a copy of the palette specified for this texture
		u16 pal[256];

		u32 textureMode = (unsigned short)((format>>26)&0x07);
		u32 sizeX=(8 << ((format>>20)&0x07));
		u32 sizeY=(8 << ((format>>23)&0x07));
		u32 imageSize = sizeX*sizeY;

		u8 *adr;

		u32 paletteAddress;

		switch (textureMode)
		{
		case TEXMODE_I2:
			paletteAddress = texpal<<3;
			break;
		case TEXMODE_A3I5: //a3i5
		case TEXMODE_I4: //i4
		case TEXMODE_I8: //i8
		case TEXMODE_A5I3: //a5i3
		case TEXMODE_16BPP: //16bpp
		case TEXMODE_4X4: //4x4
		default:
			paletteAddress = texpal<<4;
			break;
		}

		//analyze the texture memory mapping and the specifications of this texture
		int palSize = palSizes[textureMode];
		int texSize = (imageSize*texSizes[textureMode])>>2; //shifted because the texSizes multiplier is fixed point
		MemSpan ms = MemSpan_TexMem((format&0xFFFF)<<3,texSize);
		MemSpan mspal = MemSpan_TexPalette(paletteAddress,palSize << 1);

		//determine the location for 4x4 index data
		u32 indexBase;
		if((format & 0xc000) == 0x8000) indexBase = 0x30000;
		else indexBase = 0x20000;

		u32 indexOffset = (format&0x3FFF)<<2;

		int indexSize = 0;
		MemSpan msIndex;
		if(textureMode == TEXMODE_4X4)
		{
			indexSize = imageSize>>3;
			msIndex = MemSpan_TexMem(indexOffset+indexBase,indexSize);
		}


		//dump the palette to a temp buffer, so that we don't have to worry about memory mapping.
		//this isnt such a problem with texture memory, because we read sequentially from it.
		//however, we read randomly from palette memory, so the mapping is more costly.
		#ifdef WORDS_BIGENDIAN
			mspal.dump16(pal);
		#else
			mspal.dump(pal);
		#endif

			//TODO - as a special optimization, keep the last item returned and check it first

		for(std::pair<TTexCacheItemMultimap::iterator,TTexCacheItemMultimap::iterator>
			iters = index.equal_range(format);
			iters.first != iters.second;
			++iters.first)
		{
			TexCacheItem* curr = iters.first->second;
			
			//conditions where we reject matches:
			//when the teximage or texpal params dont match 
			//(this is our key for identifying textures in the cache)
			//NEW: due to using format as a key we dont need to check this anymore
			//if(curr->texformat != format) continue;
			if(curr->texpal != texpal) continue;

			//we're being asked for a different format than what we had cached.
			//TODO - this could be done at the entire cache level instead of checking repeatedly
			if(curr->cacheFormat != TEXFORMAT) goto REJECT;

			//the texture matches params, but isnt suspected invalid. accept it.
			if (!curr->suspectedInvalid) return curr;

			//we suspect the texture may be invalid. we need to do a byte-for-byte comparison to re-establish that it is valid:

			//when the palettes dont match:
			//note that we are considering 4x4 textures to have a palette size of 0.
			//they really have a potentially HUGE palette, too big for us to handle like a normal palette,
			//so they go through a different system
			if(mspal.size != 0 && memcmp(curr->dump.palette,pal,mspal.size)) goto REJECT;

			//when the texture data doesn't match
			if(ms.memcmp(&curr->dump.texture[0],curr->dump.textureSize)) goto REJECT;

			//if the texture is 4x4 then the index data must match
			if(textureMode == TEXMODE_4X4)
			{
				if(msIndex.memcmp(curr->dump.texture + curr->dump.textureSize,curr->dump.indexSize)) goto REJECT; 
			}

			//we found a match. just return it
			//REMINDER to make it primary/newest when we have smarter code
			//list_remove(curr);
			//list_push_front(curr);
			curr->suspectedInvalid = false;
			return curr;

		REJECT:
			//we found a cached item for the current address, but the data is stale.
			//for a variety of complicated reasons, we need to throw it out right this instant.
			list_remove(curr);
			delete curr;
			break;
		}

		//item was not found. recruit an existing one (the oldest), or create a new one
		//evict(); //reduce the size of the cache if necessary
		//TODO - as a peculiarity of the texcache, eviction must happen after the entire 3d frame runs
		//to support separate cache and read passes
		TexCacheItem* newitem = new TexCacheItem();
		newitem->suspectedInvalid = false;
		newitem->texformat = format;
		newitem->cacheFormat = TEXFORMAT;
		newitem->texpal = texpal;
		newitem->sizeX=sizeX;
		newitem->sizeY=sizeY;
		newitem->invSizeX=1.0f/((float)(sizeX));
		newitem->invSizeY=1.0f/((float)(sizeY));
		newitem->decode_len = sizeX*sizeY*4;
		newitem->mode = textureMode;
		newitem->decoded = (u8*)memalign(32,newitem->decode_len);
		list_push_front(newitem);
		//printf("allocating: up to %d with %d items\n",cache_size,index.size());

#ifdef GXRENDER_DEBUG_LOG
		// Bisection aid for the "strange coloring" GX-only texture bug
		// (Phase 5 Symptom A): dump which DS texture format/mode and cache
		// TEXFORMAT (32bpp=GX, 15bpp=softrast) each newly-decoded texture
		// used, so we can tell whether the bad content is even hitting the
		// code path we suspect.
		{
			static int n = 0;
			if (n < 200) {
				n++;
				FILE *f = fopen("sd:/texdbg.log", "a");
				if (f) {
					fprintf(f, "tex: mode=%lu fmt32=%d sizeX=%lu sizeY=%lu texpal=%lu format=0x%08lX\n",
					        (unsigned long)textureMode, (int)(TEXFORMAT==TexFormat_32bpp),
					        (unsigned long)sizeX, (unsigned long)sizeY,
					        (unsigned long)texpal, (unsigned long)format);
					fclose(f);
				}
			}
		}
#endif

		u32 *dwdst = (u32*)newitem->decoded;
		
		//dump palette data for cache keying
		if(palSize)
		{
			memcpy(newitem->dump.palette, pal, palSize*2);
		}

		//dump texture and 4x4 index data for cache keying
		const int texsize = newitem->dump.textureSize = ms.size;
		const int indexsize = newitem->dump.indexSize = msIndex.size;
		newitem->dump.texture = new u8[texsize+indexsize];
		ms.dump(&newitem->dump.texture[0],newitem->dump.maxTextureSize); //dump texture
		if(textureMode == TEXMODE_4X4)
			msIndex.dump(newitem->dump.texture+newitem->dump.textureSize,newitem->dump.indexSize); //dump 4x4


		//============================================================================ 
		//Texture conversion
		//============================================================================ 

		const u32 opaqueColor = TEXFORMAT==TexFormat_32bpp?255:31;
		u32 palZeroTransparent = (1-((format>>29)&1))*opaqueColor;

		switch (newitem->mode)
		{
		case TEXMODE_A3I5:
			{
				for(int j = 0; j < ms.numItems; ++j) {
					adr = ms.items[j].ptr;
					for(u32 x = 0; x < ms.items[j].len; x++)
					{
						u16 c = pal[*adr&31];
						u8 alpha = *adr>>5;
						if(TEXFORMAT == TexFormat_15bpp)
							*dwdst++ = RGB15TO6665(c,material_3bit_to_5bit[alpha]);
						else
							*dwdst++ = RGB15TO32(c,material_3bit_to_8bit[alpha]);
						++adr;
					}
				}
				break;
			}

		case TEXMODE_I2:
			{
				for(int j = 0; j < ms.numItems; ++j) {
					adr = ms.items[j].ptr;
					for(u32 x = 0; x < ms.items[j].len; x++)
					{
						u8 bits;
						u16 c;

						bits = (*adr)&0x3;
						c = pal[bits];
						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);

						bits = ((*adr)>>2)&0x3;
						c = pal[bits];
						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);

						bits = ((*adr)>>4)&0x3;
						c = pal[bits];
						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);

						bits = ((*adr)>>6)&0x3;
						c = pal[bits];
						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);

						++adr;
					}
				}
				break;
			}
		case TEXMODE_I4:
			{
				for(int j = 0; j < ms.numItems; ++j) {
					adr = ms.items[j].ptr;
					for(u32 x = 0; x < ms.items[j].len; x++)
					{
						u8 bits = (*adr)&0xF;
						u16 c = pal[bits];

						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);

						bits = ((*adr)>>4);
						c = pal[bits];
						*dwdst++ = CONVERT(c,(bits == 0) ? palZeroTransparent : opaqueColor);
						++adr;
					}
				}
				break;
			}
		case TEXMODE_I8:
			{
				for(int j = 0; j < ms.numItems; ++j) {
					adr = ms.items[j].ptr;
					for(u32 x = 0; x < ms.items[j].len; ++x)
					{
						u16 c = pal[*adr];
						*dwdst++ = CONVERT(c,(*adr == 0) ? palZeroTransparent : opaqueColor);
						++adr;
					}
				}
			}
			break;
		case TEXMODE_4X4:
			{
				//RGB16TO32 is used here because the other conversion macros result in broken interpolation logic

				if(ms.numItems != 1) {
					PROGINFO("Your 4x4 texture has overrun its texture slot.\n");
				}
				//This check isn't necessary since the addressing is tied to the texture data which will also run out:
				//if(msIndex.numItems != 1) PROGINFO("Your 4x4 texture index has overrun its slot.\n");

	#define PAL4X4(offset) ( LE_TO_LOCAL_16( *(u16*)( MMU.texInfo.texPalSlot[((paletteAddress + ((offset) << 1))>>14)] + ((paletteAddress + ((offset) << 1))&0x3FFF) ) ))

				u16* slot1;
				u32* map = (u32*)ms.items[0].ptr;
				u32 limit = ms.items[0].len<<2;
				u32 d = 0;
				if ( (format & 0xc000) == 0x8000)
					// texel are in slot 2
					slot1=(u16*)&MMU.texInfo.textureSlotAddr[1][((format & 0x3FFF)<<2)+0x010000];
				else 
					slot1=(u16*)&MMU.texInfo.textureSlotAddr[1][(format & 0x3FFF)<<2];

				u16 yTmpSize = (sizeY>>2);
				u16 xTmpSize = (sizeX>>2);

				//This is flagged whenever a 4x4 overruns its slot.
				//I am guessing we just generate black in that case
				bool dead = false;

				for (u16 y = 0; y < yTmpSize; ++y)
				{
					// tmpPos[sy] is where row sy of this 4-row tile lands in the
					// output image; sy also selects which byte of currBlock (the
					// per-row 2-bit color indices) that row uses, a few lines
					// down (`currBlock>>(sy<<3)`). This used to run high-to-low
					// (tmpPos[0]=row 3, tmpPos[3]=row 0) to compensate for
					// currBlock being read as a raw big-endian load of
					// little-endian VRAM data - byte 0 of that misread value was
					// actually the source's LAST byte (row 3), so the reversal
					// here canceled it back out to the correct row, by accident.
					// Now that currBlock gets properly byte-swapped before use
					// (LE_TO_LOCAL_32, above - byte 0 is genuinely row 0 again),
					// this reversal is uncanceled and flips every tile's 4 rows
					// vertically. Ascending order matches sy directly to its own
					// row.
					u32 tmpPos[4]={(y<<2)*sizeX,((y<<2)+1)*sizeX,((y<<2)+2)*sizeX,((y<<2)+3)*sizeX};

					for (u16 x = 0; x < xTmpSize; ++x, ++d)
					{
						if(d >= limit)
							dead = true;

						if(dead) {
							for (int sy = 0; sy < 4; sy++)
							{
								u32 currentPos = (x<<2) + tmpPos[sy];
								dwdst[currentPos] = dwdst[currentPos+1] = dwdst[currentPos+2] = dwdst[currentPos+3] = 0;
							}
							continue;
						}

						// map[d] is the same kind of raw little-endian VRAM read
						// as TEXMODE_16BPP's map[x] above (see the comment
						// there) - byte 0 is meant to be block-row 0's 4 texel
						// indices, byte 3 block-row 3's, and a native big-endian
						// load of the 4 bytes reverses that order. slot1[d] right
						// below already goes through LE_TO_LOCAL_16 for the same
						// reason; this 32-bit sibling read didn't.
						u32 currBlock	= LE_TO_LOCAL_32(map[d]);
						u16 pal1		= LE_TO_LOCAL_16(slot1[d]);
						u16 pal1offset	= (pal1 & 0x3FFF)<<1;
						u8  mode		= pal1>>14;
						u32 tmp_col[4];
						
						tmp_col[0]=RGB16TO32(PAL4X4(pal1offset),255);
						tmp_col[1]=RGB16TO32(PAL4X4(pal1offset+1),255);

						switch (mode) 
						{
						case 0:
							tmp_col[2]=RGB16TO32(PAL4X4(pal1offset+2),255);
							tmp_col[3]=RGB16TO32(0x7FFF,0);
							break;
						case 1:
							tmp_col[2]=(((tmp_col[0]&0xFF)+(tmp_col[1]&0xff))>>1)|
								(((tmp_col[0]&(0xFF<<8))+(tmp_col[1]&(0xFF<<8)))>>1)|
								(((tmp_col[0]&(0xFF<<16))+(tmp_col[1]&(0xFF<<16)))>>1)|
								(0xff<<24);
							tmp_col[3]=RGB16TO32(0x7FFF,0);
							break;
						case 2:
							tmp_col[2]=RGB16TO32(PAL4X4(pal1offset+2),255);
							tmp_col[3]=RGB16TO32(PAL4X4(pal1offset+3),255);
							break;
						case 3: 
							{
								u16 tmp1, tmp2;
								COLOR32 color1, color2;

								color1.val = tmp_col[0];
								color2.val = tmp_col[1];

								u8 red1   = color1.bits.red;
								u8 green1 = color1.bits.green;
								u8 blue1  = color1.bits.blue;

								u8 red2   = color2.bits.red;
								u8 green2 = color2.bits.green;
								u8 blue2  = color2.bits.blue;

								tmp1 =  (((INTx5(red1)   + INTx3(red2))   >>6) <<  0) |
									(((INTx5(green1) + INTx3(green2)) >>6) <<  5) |
									(((INTx5(blue1)  + INTx3(blue2))  >>6) << 10);

								tmp2 =  (((INTx5(red2)   + INTx3(red1))   >>6) <<  0) |
									(((INTx5(green2) + INTx3(green1)) >>6) <<  5) |
									(((INTx5(blue2)  + INTx3(blue1))  >>6) << 10);

								tmp_col[2] = RGB16TO32(tmp1, 255);
								tmp_col[3] = RGB16TO32(tmp2, 255);
								break;
							}
						}

						if(TEXFORMAT==TexFormat_15bpp)
						{
							for(int i=0;i<4;i++)
							{
								tmp_col[i] >>= 2;
								tmp_col[i] &= 0x3F3F3F3F;
								u32 a = tmp_col[i]>>24;
								tmp_col[i] &= 0x00FFFFFF;
								tmp_col[i] |= (a>>1)<<24;
							}
						}

						//TODO - this could be more precise for 32bpp mode (run it through the color separation table)

						//set all 16 texels
						for (int sy = 0; sy < 4; sy++)
						{
							// Texture offset
							u32 currentPos = (x<<2) + tmpPos[sy];
							u8 currRow = (u8)((currBlock>>(sy<<3))&0xFF);

							dwdst[currentPos] = tmp_col[currRow&3];
							dwdst[currentPos+1] = tmp_col[(currRow>>2)&3];
							dwdst[currentPos+2] = tmp_col[(currRow>>4)&3];
							dwdst[currentPos+3] = tmp_col[(currRow>>6)&3];
						}


					}
				}


				break;
			}
		case TEXMODE_A5I3:
			{
				for(int j = 0; j < ms.numItems; ++j) {
					adr = ms.items[j].ptr;
					for(u32 x = 0; x < ms.items[j].len; ++x)
					{
						u16 c = pal[*adr&0x07];
						u8 alpha = (*adr>>3);
						if(TEXFORMAT == TexFormat_15bpp)
							*dwdst++ = RGB15TO6665(c,alpha);
						else
							*dwdst++ = RGB15TO32(c,material_5bit_to_8bit[alpha]);
						++adr;
					}
				}
				break;
			}
		case TEXMODE_16BPP:
			{
				// map[] casts a raw byte pointer into DS texture VRAM (always
				// little-endian - it's a straight copy of the DS's own memory)
				// to u16* and reads it with a native load. On the big-endian
				// PowerPC Wii CPU that reinterprets every texel's 16-bit color
				// word high-byte/low-byte swapped - a small change in the
				// source data's low byte (usually the least significant color
				// bits) lands in the *high* byte of the misread value and vice
				// versa, turning what should be a smoothly-varying texture
				// into near-random per-texel noise ("checkerboard where a
				// gradient should be"). Every other little-endian VRAM/palette
				// read in this file already
				// goes through LE_TO_LOCAL_16/32 or MemSpan::dump16's explicit
				// WORDS_BIGENDIAN swap (see mspal.dump16() above, and
				// slot1[d]'s LE_TO_LOCAL_16() in the TEXMODE_4X4 case below) -
				// this was simply missed.
				for(int j = 0; j < ms.numItems; ++j) {
					u16* map = (u16*)ms.items[j].ptr;
					int len = ms.items[j].len>>1;
					for(int x = 0; x < len; ++x)
					{
						u16 c = LE_TO_LOCAL_16(map[x]);
						int alpha = ((c&0x8000)?opaqueColor:0);
						*dwdst++ = CONVERT(c&0x7FFF,alpha);
					}
				}
				break;
			}
		} //switch(texture format)

#ifdef GXRENDER_DEBUG_LOG
		// Sample a handful of decoded texels straight out of texcache's own
		// buffer - before setTexture() ever tiles/uploads it to GX - so we can
		// tell whether a bad-looking texture is wrong here already (a decode
		// bug) or only once it reaches the screen (a GX upload/render bug).
		{
			static int n = 0;
			if (n < 150) {
				n++;
				FILE *f = fopen("sd:/texdbg.log", "a");
				if (f) {
					fprintf(f, "decoded mode=%lu sizeX=%lu sizeY=%lu texpal=%lu:",
					        (unsigned long)newitem->mode, (unsigned long)sizeX,
					        (unsigned long)sizeY, (unsigned long)texpal);
					const u8 *px = newitem->decoded;
					u32 total = sizeX * sizeY;
					for (int s = 0; s < 8; s++) {
						u32 idx = (total * s) / 8;
						const u8 *t = px + idx * 4;
						// packed layout is [alpha,blue,green,red] per texel
						fprintf(f, " (r%u g%u b%u a%u)", t[3], t[2], t[1], t[0]);
					}
					fprintf(f, "\n");
					fclose(f);
				}
			}
		}
#endif

#ifdef GXTEX_DUMP_PPM
		// One-shot full-image dump of texcache's own decoded buffer - before
		// setTexture() ever tiles/uploads it to GX - as a plain P6 PPM, so it
		// can be pulled off the SD card and viewed directly as an image. Lets
		// us see the WHOLE texture (not just 8 spot samples) and settle,
		// definitively, whether a bad-looking texture is already wrong here
		// (a decode bug) or only once it reaches the screen (a GX upload/
		// render bug). Not wired into any shipping build flag.
		if (newitem->mode == TEXMODE_4X4 && sizeX == GXTEX_DUMP_PPM) {
			static bool dumped = false;
			if (!dumped) {
				dumped = true;
				FILE *f = fopen("sd:/texdump.ppm", "wb");
				if (f) {
					fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)sizeX, (unsigned long)sizeY);
					const u8 *px = newitem->decoded;
					for (u32 i = 0; i < imageSize; i++) {
						const u8 *t = px + i * 4;   // [alpha,blue,green,red]
						u8 rgb[3] = { t[3], t[2], t[1] };
						fwrite(rgb, 1, 3, f);
					}
					fclose(f);
				}
			}
		}
#endif

#ifdef GXTEX_FORCE_MODE_MAGENTA
		// Temporary bisection aid: flood-fill every texture of one specific
		// DS format with solid magenta so its on-screen footprint is
		// unmistakable. Not wired into any shipping build flag.
#ifdef GXTEX_FORCE_SIZEX
		if (newitem->mode == GXTEX_FORCE_MODE_MAGENTA && sizeX == GXTEX_FORCE_SIZEX) {
#else
		if (newitem->mode == GXTEX_FORCE_MODE_MAGENTA) {
#endif
			u32 *fill = (u32*)newitem->decoded;
			u32 magenta = RGB16TO32(0x7C1F, 255);   // DS BGR555 full R + full B
			for (u32 i = 0; i < imageSize; i++)
				fill[i] = magenta;
		}
#endif

#ifdef DO_DEBUG_DUMP_TEXTURE
	DebugDumpTexture(newitem);
#endif

		return newitem;
	} //scan()

	void invalidate()
	{
		for(TTexCacheItemMultimap::iterator it(index.begin()); it != index.end(); ++it)
			it->second->suspectedInvalid = true;
	}

	void evict(u32 target = kMaxCacheSize)
	{
		//Don't do anything unless we're over the target
		if(cache_size < target) return;

		//aim at cutting the cache to half of the max size
		target >>= 1;

		//evicts items in an arbitrary order until it is less than the max cache size
		//TODO - do this based on age and not arbitrarily
		while(cache_size > target)
		{
			if(index.size()==0) break; //just in case.. doesnt seem possible, cache_size wouldve been 0

			TexCacheItem* item = index.begin()->second;
			list_remove(item);
			//printf("evicting! totalsize:%d\n",cache_size);
			delete item;
		}
	}
} texCache;

void TexCache_Reset()
{
	texCache.evict(0);
}

void TexCache_Invalidate()
{
	//note that this gets called whether texdata or texpalette gets reconfigured.
	texCache.invalidate();
}

TexCacheItem* TexCache_SetTexture(TexCache_TexFormat TEXFORMAT, u32 format, u32 texpal)
{
	switch(TEXFORMAT)
	{
	case TexFormat_32bpp: return texCache.scan<TexFormat_32bpp>(format,texpal);
	case TexFormat_15bpp: return texCache.scan<TexFormat_15bpp>(format,texpal);
	default: assert(false); return NULL;
	}
}

//call this periodically to keep the tex cache clean
void TexCache_EvictFrame()
{
	texCache.evict();
}