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

#ifndef GX_TEXMANAGER_H
#define GX_TEXMANAGER_H

#include <gctypes.h>
#include <gccore.h>
#include <stdlib.h>

#include "common.h"
#include "types.h"
#include "render3D.h"
#include "MMU.h"
#include "bits.h"

//------------------------------------------------------------
// ------------ GX-specfic Texture Manager ------------------
//------------------------------------------------------------

class TexManager{

public:

	TexManager();
	~TexManager();
	void reset();
	void activateTex(u32 texID);
	void deleteTex(u32 texID);
	bool resize(size_t n);
	u32 size();
	GXTexObj* gxObj(u32 id);
	
private:
	GXTexObj* textures;	// Array of texture "slots"
	bool* used;			// Which texture slots are used?
	size_t numTotal;	// How many texture slots we have total
	size_t numUsed;		// How many slots are used
};


#endif
