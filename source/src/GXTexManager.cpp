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

#include "GXTexManager.h"

//------------------------------------------------------------
// ------------- GX-specfic Texture Manager -----------------
//------------------------------------------------------------

//----------------------------------------
//
// Constructor
//
// @pre     -
// @post    We have a new, empty, texture manager
// @param   -
// @return  -
//
//----------------------------------------

TexManager::TexManager(): textures(NULL), used(NULL), 
	numTotal(0), numUsed(0){
}

//----------------------------------------
//
// Destructor
//
// @pre     -
// @post    Gasp! The texture manager is no more!
// @param   -
// @return  -
//
//----------------------------------------

TexManager::~TexManager(){

	delete[] used;
	used = NULL;

	delete[] textures;
	textures = NULL;
}

//----------------------------------------
//
// Function: reset
//
// Reset and allocate space for our texture manager's variables
//
// @pre     -
// @post    The texture manager variables have been cleaned out
// @param   -
// @return  -
//
//----------------------------------------

void TexManager::reset(){

	// The array will stay the same size, it will just be cleared out.

	memset(used, 0, sizeof(bool) * numTotal);
	memset(textures, 0, sizeof(GXTexObj) * numTotal);

	// Not using any textures now!
	numUsed = 0;
}

//----------------------------------------
//
// Function: resize
//
// Change the capacity of the texture manager
//
// @pre     Make sure there's space to hold the extra textures!
// @post    Extra texture slots will be added to the manager
// @param   n: The number of textures we want it to hold
// @return  bool: true if everything went well, false if not.
//
//----------------------------------------

bool TexManager::resize(size_t n){

	if(numUsed > numTotal){
		numUsed = 0;
		for(size_t i = 0; i < numTotal; i++){
			if(used[i])
				++numUsed;
		}
	}
	
	// If we have NULL pointers, we have no textures
	if(!textures || !used){
		numTotal = 0;
		numUsed = 0;
	}
	
	GXTexObj* new_arr = (GXTexObj*)realloc(textures, sizeof(GXTexObj)*n);
	if(!new_arr){
		return false;
	}
	
	bool* new_used = (bool*)realloc(used, sizeof(bool)*n);
	
	if(!new_used){
		textures = new_arr;
		return false;
	}
	
	// Mark the new spots as unused
	for(size_t i = numTotal; i < n; ++i){
		new_used[i] = false;
	}
	
	numTotal = n;
	textures = new_arr;
	used = new_used;
	
	return true;
}

//----------------------------------------
//
// Function: activateTex
//
// Set the texture as being in use and prep it for loading
//
// @pre     -
// @post    The texture is now being "used"
// @param   texID: The texture ID of the texture to to add
// @return  -
//
//----------------------------------------

void TexManager::activateTex(u32 texID){
	// texID is 0-based - it comes straight from expandFreeTextures()'s
	// `freeTextureIds.push(i)` loop (i starting at 0), and gxObj() below
	// indexes textures[id] directly, with no -1. This used to subtract 1
	// here, disagreeing with both of those: activateTex(0) - guaranteed on
	// the very first texture ever allocated, every run - computed
	// i = (size_t)(0 - 1), a wraparound to the largest size_t value, and then
	// wrote used[i] = true through it: a one-byte out-of-bounds write
	// (used[-1], immediately before the array) on every single boot. For
	// every texID after the first, activateTex(texID) and gxObj(texID) were
	// also then bookkeeping two different slots (index texID-1 vs texID) even
	// though both actually only ever exchange data through gxObj()'s slot -
	// harmless to rendering itself (GX_InitTexObj and GX_LoadTexObj always
	// go through the same gxObj(texID)), but it permanently desynced
	// used[]/numUsed from what's actually allocated.
	size_t i = texID;

	if(texID >= numTotal){ // We need a texture ID that doesn't exist yet!

		// We must allocate more memory. Need slots 0..texID, i.e. texID+1 of
		// them - resizing to exactly texID (as this used to) leaves slot
		// texID itself out of bounds, one short of what we're about to index.
		if(!resize(texID + 1)){
			// If resize returns false, that means that we couldn't
			// allocate enough memory! PANIC! PANIIIIIIIC!
			printf("\n -- TexManager::activateTex: No more memory --\n");
			return;
		}
	}

	if(used[i]){ // We're already using it!
		return;
	}

	// Should we zero it out? If we did everything right, we shouldn't have to.
	//memset(&textures[i], 0, sizeof(GXTexObj));

	// Set the spot in our "used" array to true, since we're using it
	used[i] = true;

	// Decrease the number of used textures
	++numUsed;
}

//----------------------------------------
//
// Function: deleteTex [UNNEEDED(?)]
//
// Delete the texture and adjust the texture manager
//
// @pre     -
// @post    The texture is gone! 
// @param   texID: The texture ID of the texture to to delete
// @return  -
//
//----------------------------------------

void TexManager::deleteTex(u32 texID){
/*
	u32 i = texID - 1;
	
	if(!used[i]) // If it's not in use, don't delete it!
		return;

	// Clean that spot in the array for later use
	memset(&textures[i], 0, sizeof(GXTexObj));

	// Set the spot in our "used" array to false
	used[i] = false;

	// Decrease the number of used textures
	--numUsed;
//*/
}

//----------------------------------------
//
// Function: size
//
// Returns the size of the TOTAL texture array, not the number used.
//
// @pre     -
// @post    - 
// @param   -
// @return  The total allocated size of the textures[] array 
//
//----------------------------------------

u32 TexManager::size(){
	return numTotal;
}

//----------------------------------------
//
// Function: gxObj
//
// Returns a pointer to the Texture Object (For loading into GX)
//
// @pre     id must be within the range of the textures array!
// @post    - 
// @param   id: The index of the textures array that we want
// @return  GXTexObj* : Pointer to the specific Texture Object
//
//----------------------------------------

GXTexObj* TexManager::gxObj(u32 id){
	return (&textures[id]);
}


