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
#ifndef _TEXCACHE_H_
#define _TEXCACHE_H_

#include "common.h"
#include <map>

enum TexCache_TexFormat
{
	TexFormat_None, //used when nothing yet is cached
	TexFormat_32bpp, //used by gx renderer
	TexFormat_15bpp //used by rasterizer
};

class TexCacheItem;

typedef std::multimap<u32,TexCacheItem*> TTexCacheItemMultimap;

class TexCacheItem
{
public:
	TexCacheItem() 
		: decode_len(0)
		, decoded(NULL)
		, suspectedInvalid(false)
		, deleteCallback(NULL)
		, cacheFormat(TexFormat_None)
	{}
	~TexCacheItem() {
		if(decoded){
			free(decoded);
			decoded = NULL;
		}
		if(deleteCallback) deleteCallback(this);
	}
	u32 decode_len;
	u32 mode;
	u8* decoded; //decoded texture data
	bool suspectedInvalid;
	TTexCacheItemMultimap::iterator iterator;

	u32 texformat, texpal;
	u32 sizeX, sizeY;
	float invSizeX, invSizeY;

	//--DCN: Changed to u32, maybe this will be better?
	u32 texid; //used by gx renderer for the texid
	void (*deleteCallback)(TexCacheItem*);

	TexCache_TexFormat cacheFormat;

	struct Dump {
		~Dump() {
			delete[] texture;
		}
		int textureSize, indexSize;
		static const int maxTextureSize = 128*1024;
		u8* texture;
		u8 palette[256*2];
	} dump;
};

void TexCache_Invalidate();
void TexCache_Reset();
void TexCache_EvictFrame();

TexCacheItem* TexCache_SetTexture(TexCache_TexFormat TEXFORMAT, u32 format, u32 texpal);

#endif