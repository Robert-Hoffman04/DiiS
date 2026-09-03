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

#include <queue>
#include "GXRender.h"
#include "GXTexManager.h"
#include "guDesmume.h"
#include "NDSSystem.h"
#include "gfx3d.h"
#include "texcache.h"
#include "GXMerge.h"

// Temporary bisection aid for the mergerom GX-core stall (Phase 5 symptom B):
// on-screen printf can't reach the display once GXRender is wedged holding
// vidmutex (draw_thread can never get the mutex to present anything new), so
// this appends unbuffered lines straight to the SD card instead - whatever
// made it to the file before the hang tells us exactly where execution
// stopped. Diagnostic only; not wired into any shipping build flag.
#if defined(GXRENDER_DEBUG_LOG) || defined(GXTEX_DUMP_TILED) || defined(GXTEX_LOG_POLYMODE)
#include <stdio.h>
#endif
#ifdef GXRENDER_DEBUG_LOG
static void gxdbg(const char *msg)
{
	// Cap total lines - thousands of frames' worth of unthrottled per-frame
	// logging grows the SD-card file into the megabytes, which the host-side
	// mtools-based harness then fails to read back ("Fat problem while
	// decoding"). We only ever need the first stretch of frames after boot.
	static int n = 0;
	if (n >= 4000) return;
	n++;
	FILE *f = fopen("sd:/gxdbg.log", "a");
	if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}
#define GXDBG(msg) gxdbg(msg)
#define GXDBGF(...) do { char _b[128]; snprintf(_b, sizeof(_b), __VA_ARGS__); gxdbg(_b); } while(0)
#else
#define GXDBG(msg)
#define GXDBGF(...)
#endif

#define SET_GQR(quantno, power, type) \
({	register uint32_t gqrValue = (((((u8)(power)) << 8)\
		| ((u8)(type))) << 16) | ((((u8)(power)) << 8) | ((u8)(type)));\
	asm volatile("mtspr %0, %1" : : "K"(912 + quantno), "r"(gqrValue)); })

// ------------------- EXTERNAL VARIABLES ------------------
// We need to keep it from continuing whilst we render our 3D
extern mutex_t vidmutex;
// We need to reset all the variables for video when we're done with 3D
extern GXRModeObj *rmode;

// ----------------------------------------------------------
static CACHE_ALIGN u8 GPU_screen3D[256*192*4];
static CACHE_ALIGN u8 tmp_texture[128*1024*4]; // Needed for temp. GX AAAARRRRGGGGBBB texture

static const u8  map3d_cull[4] = {GX_CULL_ALL, GX_CULL_FRONT, GX_CULL_BACK, GX_CULL_NONE};
static const int texEnv[4] = {GX_MODULATE, GX_DECAL, GX_MODULATE, GX_MODULATE};
static const int depthFunc[2] = {GX_LESS, GX_EQUAL};

//Derived values extracted from polyattr etc
/*
static u32 polyID = 0;
static u32 envMode = 0;
static u32 lightMask = 0;
static bool alpha31 = false;
//*/
static u32 depthFuncMode = GX_LESS;
static u32 cullingMask = 0;
static u32 textureFormat = 0;
static u32 texturePalette = 0;
static bool alphaDepthWrite;
static bool isTranslucent;
// POLYGON_ATTR bits 4-5 (0=Modulate,1=Decal,2/3=Toon-Highlight). Set per
// polygon in GXRender()'s poly loop, consumed by BeginRenderPoly() (TEV op
// selection) and the vertex-submission loop (toon-table colour lookup) -
// see BeginRenderPoly()'s texture branch for why this needed adding.
static u32 currPolyEnvMode = 0;

static std::queue<u32> freeTextureIds;
static TexCacheItem* currTexture = NULL;

//------------------------------------------------------------
// Texture Variables
//------------------------------------------------------------

// The number that we expand our texture array by
#define EXPAND_FREE_TEX_NUM 128

// Our texture manager (keeps track of which textures we're using)
TexManager* texMan;

// When we need to apply a texture, we use this matrix
static Mtx textureView;

GXTexObj shadowObj; // Shadow texture object

//------------------------------------------------------------
// Function Prototypes
//------------------------------------------------------------
static void expandFreeTextures();
static void texDeleteCallback(TexCacheItem* item);

static void setTexture(u32 format, u32 texpal);
static void BeginRenderPoly();
static void InstallPolygonAttrib(unsigned long val);
static void Set3DVideoSettings();

static void ReadFramebuffer();
static char GXInit();
static void GXClose();
static void GXReset();
static void GXRender();
static void GXVramReconfigureSignal();
static void ResetVideoSettings();

//------------------------------------------------------------
// Texture Functions (Special thanks to the fine people at gl2gx!)
//------------------------------------------------------------

//----------------------------------------
//
// Function: expandFreeTextures
//
// Increases the number of free texture IDs that we can use
//
// @pre     -
// @post    The available number of textures we can use has incresed
// @param   -
// @return  -
//
//----------------------------------------

static void expandFreeTextures(){

	u32 i = texMan->size();
	u32 newMax = i + EXPAND_FREE_TEX_NUM;
	for(; i < newMax; i++)
		freeTextureIds.push(i);

	// Resize our texture array to account for the new size
	if(!texMan->resize(newMax))
		printf("\n -- expandFreeTextures(): No more memory --\n");
}


//----------------------------------------
//
// Function: texDeleteCallback
//
// @pre     -
// @post    -
// @param   item: The texture
// @return  -
//
//----------------------------------------

static void texDeleteCallback(TexCacheItem* item){
	freeTextureIds.push(item->texid);
	if(currTexture == item){
		currTexture = NULL;
	}
}

//----------------------------------------
//
// Function: GXReset
//
// Reset our textures and variables to 0
//
// @pre     -
// @post    -
// @param   -
// @return  -
//
//----------------------------------------

static void GXReset(){

	TexCache_Reset();

	delete currTexture;
	currTexture = NULL;

	texMan->reset();

	memset(GPU_screen3D, 0, sizeof(GPU_screen3D));
}

//----------------------------------------
//
// Function: GXInit
//
// Set up all of our texture caches and variables
//
// @pre     -
// @post    -
// @param   -
// @return  char: Return 1 if everything went well
//
//----------------------------------------

static char GXInit(){

	SET_GQR(1,  0, 4);     //load/store and do nothing when using gqr1
	SET_GQR(2, -1, 4);     //load  and multiply by 2 when using gqr2
	SET_GQR(3, -6, 4);     //store and divide by 64 when using gqr3
	SET_GQR(4, -2, 4);     //store and divide by 2 when using gqr4

	// Create our Texture Manager
	texMan = new TexManager();
	expandFreeTextures();
	GXReset();
	return 1;
}

//----------------------------------------
//
// Function: GXClose
//
// Delete all the textures that we've used and clean up after ourselves
//
// @pre     -
// @post    -
// @param   -
// @return  -
//
//----------------------------------------

static void GXClose(){

	// Kill the texture cache to free all of the texture ids
	TexCache_Reset();

	while(!freeTextureIds.empty()){
		freeTextureIds.pop();	
	}
	// Kill our texture manager
	delete texMan;
	texMan = NULL;
}

//----------------------------------------
//
// Function: setTexture
//
// Loads (and initializes) textures to use with the polygon
//
// @pre     -
// @post    The texture is locked and loaded
// @param   format: The texture format (if we want to clamp or mirror it)
// @param   texpal: The texture palette
// @return  -
//
//----------------------------------------

static void setTexture(u32 format, u32 texpal){

	textureFormat = format;
	texturePalette = texpal;

	TexCacheItem* newTexture = TexCache_SetTexture(TexFormat_32bpp, format, texpal);
	
	if(newTexture != currTexture){
		currTexture = newTexture;
		//Has the renderer initialized the texture already?
		if(!currTexture->deleteCallback){
			currTexture->deleteCallback = texDeleteCallback;
			if(freeTextureIds.empty()) expandFreeTextures();
			currTexture->texid = freeTextureIds.front();
			freeTextureIds.pop();

			texMan->activateTex(currTexture->texid);

			// We must convert the texture into a GX-friendly format
			u8* src = currTexture->decoded;

			u32 curTexSizeY = currTexture->sizeY;
			u32 curTexSizeX = currTexture->sizeX;
			for(u32 y = 0; y < curTexSizeY; y++){
				u32 ybySizeX = (((y >> 2)<<4)*curTexSizeX);
				u32 yby2 = ((y%4) << 2);
				for(u32 x = 0; x < curTexSizeX; x++){
					const u8 a = *src++;
					const u8 b = *src++;
					const u8 g = *src++;
					const u8 r = *src++;

					const u32 offset = ybySizeX + ((x >> 2)<<6) + ((yby2 + x%4 ) <<1);

					tmp_texture[offset]    = a;
					tmp_texture[offset+1]  = r;
					tmp_texture[offset+32] = g;
					tmp_texture[offset+33] = b;
				
				}
			}

			memcpy(currTexture->decoded, tmp_texture, currTexture->decode_len);

#ifdef GXTEX_DUMP_TILED
			// One-shot hex dump of the first tile (64 bytes: the AR/GB pairs
			// for the top-left 4x4 texels) of the TILED buffer - exactly what
			// GX will upload/sample - for the same texture texcache.cpp's own
			// GXTEX_DUMP_PPM dumps pre-tile. The raw-binary version of this
			// dump (sd:/texdump_tiled.bin, fwrite of the whole buffer) hit
			// mtools' known "Fat problem while decoding" read-back failure
			// (see the SD-card-logging notes elsewhere in this file) even
			// though the write itself reported success and the file showed
			// up at the right size in an `mdir` listing - so route this
			// through the same small text-log append (sd:/texdbg.log) that's
			// proven reliable all session, instead of a fresh binary file.
			// Not wired into any shipping build flag.
			if (((format >> 26) & 0x07) == TEXMODE_4X4 && curTexSizeX == GXTEX_DUMP_TILED) {
				static bool dumped = false;
				if (!dumped) {
					dumped = true;
					FILE *f = fopen("sd:/texdbg.log", "a");
					if (f) {
						fprintf(f, "tiled sizeX=%lu sizeY=%lu texpal=%lu tile0:",
						        (unsigned long)curTexSizeX, (unsigned long)curTexSizeY,
						        (unsigned long)texpal);
						const u8 *t = currTexture->decoded;
						for (int i = 0; i < 64; i++)
							fprintf(f, " %02X", t[i]);
						fprintf(f, "\n");
						fclose(f);
					}
				}
			}
#endif

			// Make sure everything is finished before we move on.
			DCFlushRange(currTexture->decoded, currTexture->decode_len);

			// Put that data into a texture
			GX_InitTexObj(texMan->gxObj(currTexture->texid),
				currTexture->decoded,
				curTexSizeX,
				curTexSizeY,
				GX_TF_RGBA8,
				(BIT16(currTexture->texformat) ? (BIT18(currTexture->texformat)?GX_MIRROR:GX_REPEAT) : GX_CLAMP),
				(BIT17(currTexture->texformat) ? (BIT19(currTexture->texformat)?GX_MIRROR:GX_REPEAT) : GX_CLAMP),
				GX_FALSE
			);
		}else{
			// It's already been created, continue.
		}
		GX_LoadTexObj(texMan->gxObj(currTexture->texid), GX_TEXMAP0);

		// Configure the texture matrix
		/*
		guMtxScale(textureView, currTexture->invSizeX, currTexture->invSizeY, 1.0f);
		GX_LoadTexMtxImm(textureView, GX_TEXMTX0, GX_MTX2x4);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_POS, GX_TEXMTX0);
		//*/
		//*
		//Old version (possibly better?)
		//guMtxIdentity(textureView);
		guMtxScale(textureView, currTexture->invSizeX, currTexture->invSizeY, 1.0f);
		GX_LoadTexMtxImm(textureView, GX_TEXMTX0,GX_MTX3x4);
		GX_SetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX3x4, GX_TG_TEX0, GX_TEXMTX0);
		//*/
	}

}

//----------------------------------------
//
// Function: BeginRenderPoly
//
// Sets the variables specific to the polygon (texture, for example)
//
// @pre     -
// @post    The variables are set and the textures are loaded
// @param   -
// @return  -
//
//----------------------------------------

static void BeginRenderPoly(){
	bool enableDepthWrite = true;

	if (cullingMask != 0xC0){
		GX_SetCullMode(map3d_cull[cullingMask>>6]);
	}
	else{
		GX_SetCullMode(GX_CULL_NONE);
	}

	// Texture format field 0 (TEXMODE_NONE) means "no texture on this polygon"
	// on real DS hardware - it is not a legitimate 8x8 texture at VRAM offset 0.
	// setTexture()/TexCache_SetTexture() don't special-case it: the decode
	// switch in texcache.cpp has no TEXMODE_NONE case, so the "texture" it
	// hands back is sizeX*sizeY*4 bytes of freshly memalign()'d, never-written
	// (uninitialized) heap memory, bound to GX_TEXMAP0 and sampled anyway.
	// Skip the bind entirely for an untextured polygon and disable the TEV
	// stage's texture sampling, same as Set3DVideoSettings already does for a
	// !gfx3d.enableTexturing frame - just decided per polygon instead, since
	// individual polygons (not just whole frames) go untextured constantly.
	if (((textureFormat >> 26) & 0x07) == TEXMODE_NONE) {
		currTexture = NULL;
		GX_SetNumTexGens(0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		// GX_REPLACE (Set3DVideoSettings' default, and TEV's own reset state)
		// is Cv=Ct - the stage's OUTPUT COLOR IS THE TEXTURE COLOR, full stop;
		// rasterized/vertex colour never enters into it. With no texture bound
		// that's simply black. GX_PASSCLR is Cv=Cr - pass the rasterized colour
		// (our GX_Color4u8 vertex data) straight through instead, which is what
		// an untextured, vertex-coloured DS polygon actually needs.
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	} else {
		// Initialize and load the texture. Make it so!
		setTexture(textureFormat, texturePalette);
		GX_SetNumTexGens(1);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

		// POLYGON_ATTR's texture blend mode (bits 4-5) was never read anywhere
		// in this file outside a long-dead #ifdef TODO block (InstallPolygonAttrib
		// still has it commented out) - every textured polygon got GX_REPLACE
		// (Cv=Ct, texture colour only) regardless of what the DS polygon
		// actually specified. That's simply correct for mode 0 (Modulate is
		// approximated well enough by GX_REPLACE when the vertex colour is
		// white/unlit, the overwhelmingly common case) and mode 1 (Decal,
		// not yet handled here either - same GX_REPLACE approximation, still
		// TODO), but mode 2/3 (Toon/Highlight) is a real, common DS lighting
		// style that GX_REPLACE renders as raw unlit texture - producing
		// exactly the kind of "right shape, wrong colours entirely" corruption
		// this was chased as (see the GXTEX_LOG_POLYMODE bisection in this
		// session: the specific texture behind it is envMode==2, confirmed
		// against melonDS). Real Toon/Highlight shading is per-pixel: look up
		// a 32-entry TOON_TABLE colour using the INTERPOLATED vertex colour's
		// red channel (rasterize.cpp:540-573, the software core's own,
		// independently-verified-correct implementation), and either replace
		// (no texture) or modulate the texture by it (Toon), or modulate by
		// the material colour and then additively blend the toon colour on
		// top (Highlight). GX's fixed-function TEV can't do a table lookup
		// keyed by an interpolated per-pixel value, so this approximates by
		// doing the *lookup* once per VERTEX instead of per pixel (CPU-side,
		// in the vertex-submission loop below, substituting the toon colour
		// for the vertex colour actually sent) and then letting ordinary GX
		// modulate (Cv=Ct*Cr) interpolate and combine it exactly like Toon
		// mode's own formula - texture modulated by the toon colour. Table
		// lookups don't commute with interpolation in general, so this is an
		// approximation, but a good one for the fairly fine tessellation DS
		// toon-shaded models typically use; it's exact for Toon mode and
		// close for Highlight (only the additive specular term is missing).
		if (currPolyEnvMode == 2)
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		else
			GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	}

	if(isTranslucent)
		enableDepthWrite = alphaDepthWrite;

#ifdef TODO
	//--DCN: GX has no stencil buffer! We'll have to find another way.
	// We need to use shadow mapping

	// From Graphicos3D:
	
		// Earlier Set up:
		void * shadow_mem = memalign(32,size_textshadow);
		int screen_sx = rmode->fbWidth;
		int screen_sy = rmode->efbHeight;

	///////////////////////////////////////
	// Each frame:

	// From tg-shadow2:
	static const u1632 shadowSize = 256;
	GX_SetViewport(0, 0, shadowSize, shadowSize, 0.0F, 1.0F);
    GX_SetScissor(0, 0, (u32)shadowSize, (u32)shadowSize);

    SetCamera(&sc->light.cam); // Set it to where the light is
    
	//
	// Set render mode which only draws ID number as a color
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_CLAMP, GX_AF_NONE);
	// Set up ambient color
	GX_SetChanAmbColor(GX_COLOR0A0, (GxColor){0x00, 0x00, 0x00, 0x00});
	GX_SetNumTevStages(1);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetNumTexGens(0);
	//

    // Scale adjustment factor which can be used to enlarge
    // drawing area of each object in the first pass
    adjf = ( sc->adjMode ) ? 1.15F : 1.0F;

    // Draw models with ID
    guMtxIdentity(mtg);

	// Draw everything
    DrawModels(sc->light.cam.view, mtg, adjf, &sc->anim);


	
	//////////////////////////////////////////////
	// From graphicos 3D
	GX_SetScissor(1, 1, rmode->fbWidth-2, rmode->efbHeight-2);
	guPerspective(projection, 45,  1.3333f, 1.0F, 10000.0F);
	GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);

	guVecSub(&light_look, &light_pos, &light_up);

#define Adjust_Up(v) do { \
							if(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) v.z=1.0f;\
							guVecNormalize(&v);v.y = 1.0f-v.y;\
						} while(0)

	Adjust_Up(light_up);

	guLookAt(lightView, &light_pos, &light_up, &light_look);


	guMtxCopy(lightView, camera);

	/////////////////////////////////////////////

	// Each frame set up:
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);

	GX_SetTexCopySrc(0, 0, screen_sx, screen_sy);
	GX_SetTexCopyDst(screen_sx, screen_sy, GX_CTF_R8 , GX_FALSE);
	GX_CopyTex(shadow_mem, GX_TRUE); // capture the texture and clear the framebuffer
	GX_PixModeSync(); 

	// This is the same as below:
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetViewport(0, 0, screen_sx, screen_sy, 0, 1.0f);
	GX_SetScissor(0, 0, screen_sx, screen_sy); //Useless?!

	GX_InitTexObj(&shadowObj, shadow_mem,screen_sx,screen_sy, GX_TF_I8, GX_CLAMP,GX_CLAMP,GX_FALSE);
	GX_InitTexObjLOD(&shadowObj, GX_NEAR, GX_NEAR, 0, 0, 0, 0, 0, GX_ANISO_1);
	GX_LoadTexObj(&shadowObj, GX_TEXMAP0);

		GX_SetTevOrder(tevstage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevColorIn(tevstage,GX_CC_ZERO, GX_CC_ZERO , GX_CC_ZERO,GX_CC_RASC ); // pass in the rasterized Alpha 
		GX_SetTevAlphaIn(tevstage,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_RASA);     // pass in the rasterized Color 
		GX_SetTevColorOp(tevstage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaOp(tevstage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
		tevstage++;
		data_engine.tevstage_for_lighting=tevstage;


		// texture
		GX_SetTevOrder(tevstage, GX_TEXCOORD0, GX_TEXMAP1,  GX_COLOR0A0);
		GX_SetTevColorIn(tevstage,GX_CC_ZERO,GX_CC_TEXA ,GX_CC_CPREV, GX_CC_ZERO ); // COLOR=TEXA*CPREV: we are only interested in the Alpha texture to make "holes"
		GX_SetTevAlphaIn(tevstage,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_TEXA); // we pass the alpha texture
		GX_SetTevColorOp(tevstage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaOp(tevstage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
		tevstage++;
		data_engine.tevstage_for_texture=tevstage;
		data_engine.max_tevstages=tevstage;
		GX_SetNumChans(1);
		GX_SetNumTevStages(data_engine.tevstage_for_lighting);
		GX_SetNumTexGens(1);

	////////////////////////////////////////


	////////////////////////////////////////
	//handle shadow polys
	if(envMode == 3){
		xglEnable(GL_STENCIL_TEST);
		if(polyID == 0) {
			enableDepthWrite = false;
			if(stencilStateSet!=0) {
				stencilStateSet = 0;
				//when the polyID is zero, we are writing the shadow mask.
				//set stencilbuf = 1 where the shadow volume is obstructed by geometry.
				//do not write color or depth information.

				// 2. The second parameter (65) is a reference value that we will test in glStensilOp,
				// 3. The third parameter is a mask.
				// If a pixel should have been drawn to the screen, we want that spot marked with a 1.
				// GL_ALWAYS does exactly that.
				glStencilFunc(GL_ALWAYS,65,255);

				// This tests for three different conditions based on the stencil function we decided to use.

				// 1. The first parameter tells OpenGL what to do if the test fails.
				// Because the first parameter is GL_KEEP, if the test fails
				// (which it can't because we have the funtion set to GL_ALWAYS),
				// we would leave the stencil value set at whatever it currently is.
				// 2. The second parameter tells OpenGL what do do if the stencil test passes, but the depth test fails.
				// 3. The third parameter tells OpenGL what to do if the test passes!
				// The value we put into the stencil buffer is our reference value ANDed with our mask value which is 255.
				glStencilOp(GL_KEEP,GL_REPLACE,GL_KEEP);

				// We don't want anything drawn to the screen at the moment, with all of the values set to 0 (GL_FALSE),
				// colors will not be drawn to the screen.
				glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);

			}
		} else {
			enableDepthWrite = true;
			if(stencilStateSet!=1) {
				stencilStateSet = 1;
				//when the polyid is nonzero, we are drawing the shadow poly.
				//only draw the shadow poly where the stencilbuf==1.
				//I am not sure whether to update the depth buffer here--so I chose not to.

				// We're using GL_EQUAL to get where in the buffer the test passed (where it equals 1)
				glStencilFunc(GL_EQUAL,65,255);

				// As long as stencil testing is enabled pixels will ONLY be drawn if the stencil buffer has a value of 1.
				// If the stencil value is not 1 where the current pixel is being drawn it will not show up! GL_KEEP just
				// tells OpenGL not to modify any values in the stencil buffer if the test passes OR fails!
				glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);

				// We want to draw colors to the screen now.
				glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
			}
		}
	} else {
		xglEnable(GL_STENCIL_TEST);
		if(isTranslucent){
			stencilStateSet = 3;
			glStencilFunc(GL_NOTEQUAL,polyID,255);
			glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
			glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
		}
		else{
			if(stencilStateSet!=2) {
				stencilStateSet=2;
				glStencilFunc(GL_ALWAYS,64,255);
				glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);
				glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
			}
		}
	}

#endif

	GX_SetZMode(GX_ENABLE, depthFuncMode, (enableDepthWrite ? GX_ENABLE : GX_DISABLE));
}

//----------------------------------------
//
// Function: InstallPolygonAttrib
//
// Sets the variables specific to the polygon (texture, for example)
// Note: Remnant of OGLRender
//
// @pre     -
// @post    -
// @param   -
// @return  -
//
//----------------------------------------
static void InstallPolygonAttrib(unsigned long val){

	//--DCN: Variables are not used (yet)
	/*
	// Light enable/disable
	lightMask = (val&0xF);

	// Texture environment
	envMode = (val&0x30)>>4;
	alpha31 = ((val>>16)&0x1F)==31;

	// Polygon ID (for shadows)
	polyID = (val>>24)&0x3F;
	//*/

	// Overwrite depth on alpha pass
	alphaDepthWrite = BIT11(val) != 0;

	// Depth test function
	depthFuncMode = depthFunc[BIT14(val)];

	// Back face culling
	cullingMask = (val & 0xC0);

}

//----------------------------------------
//
// Function: ReadFramebuffer
//
// Convert the rendered scene to a texture
//
// @pre     -
// @post    gfx3d_convertedScreen now contains the 3D scene
// @param   -
// @return  -
//
//----------------------------------------

static void ReadFramebuffer(){

	GXDBG("ReadFramebuffer: enter, calling GX_DrawDone");
	GX_DrawDone();
	GXDBG("ReadFramebuffer: GX_DrawDone returned");

	GX_SetTexCopySrc(0, 0, 256, 192);
	GX_SetTexCopyDst(256, 192, GX_TF_RGBA8, GX_FALSE);

	// Turn off vertical de-flicker filter temporary
	// (to avoid filtering during the framebuffer-to-texture copy)
	GX_SetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);

	// The global EFB clear colour is transparent black, {0,0,0,0} (see
	// main.cpp's boot setup) and NOTHING ever switches it to opaque again -
	// on purpose. GPU.cpp's legacy compositor treats any nonzero alpha nibble
	// in gfx3d_convertedScreen as "there is a 3D pixel here"
	// (`if(colorLine[(q<<2)]) gpu->setFinalColor3d(k,q);`), so whatever the
	// EFB reads back as in the areas this frame's polygons *didn't* draw to
	// matters just as much as what they did draw. An opaque clear would make
	// every untouched pixel read back as "opaque black 3D content" too - the
	// whole screen, not just the polygon silhouette, blotting out every 2D
	// layer underneath. A transparent clear alone isn't quite enough either:
	// draw_thread's own end-of-frame GX_CopyDisp(...,GX_TRUE) *re-clears* the
	// EFB using this same global colour to prepare it for the next 3D frame,
	// so if anything here ever switched it back to opaque "temporarily", that
	// swap would still be the one in effect by the time the next frame's
	// polygons start drawing. RGB is irrelevant either way - by the time a
	// clear actually happens, draw_thread has already copied that frame's
	// real, opaque 2D+3D composite out to the XFB; the clear only prepares an
	// EFB nobody has looked at yet.

	// Keep the resident ping-pong 3D texture fresh whenever the merge path is
	// switched on.  This copy must NOT clear the EFB - the legacy de-swizzle copy
	// below still needs to read it, and it also carries the real per-pixel 3D
	// alpha (Set3DVideoSettings no longer overrides it to a binary coverage
	// mask - see the GX_SetDstAlpha removal there), the same value the
	// de-swizzle turns into the DS alpha and GXMerge.cpp's sandwich draw
	// consumes directly, both for coverage (alpha>0) and, on an
	// MB_ALPHA_OVER band, as the real blend fraction.
	if(GXMerge_Enabled()){
		GXDBG("ReadFramebuffer: merge copy start");
		GX_SetTexCopyDst(256, 192, GX_TF_RGBA8, GX_FALSE);
		GX_CopyTex(GXMerge_CopyDst(), GX_FALSE);
		GX_PixModeSync();
		DCInvalidateRange(GPU_screen3D, sizeof(GPU_screen3D))
		GXDBG("ReadFramebuffer: merge GX_CopyTex + PixModeSync done");
		GX_SetTexCopyDst(256, 192, GX_TF_RGBA8, GX_FALSE);
		GXMerge_NoteGXRenderCopied();
		GXDBG("ReadFramebuffer: merge copy end");
	}

	// Legacy path: copy to GPU_screen3D for the de-swizzle loop, and clear the
	// EFB.  Runs unconditionally so gfx3d_convertedScreen is always a faithful
	// copy of what GX rendered - even when merge is enabled, since any frame that
	// does not arm falls back to this buffer.
	GXDBG("ReadFramebuffer: legacy GX_CopyTex start");
	GX_CopyTex((void*)GPU_screen3D, GX_TRUE);
	GX_PixModeSync();
	DCInvalidateRange(GPU_screen3D, sizeof(GPU_screen3D))
	GXDBG("ReadFramebuffer: legacy GX_CopyTex + PixModeSync done");

	// Bleh, another "conversion" problem. In order to make our GX scene
	// jive with Desmume, we need to convert it OUT of its native format.
	u8* dst = gfx3d_convertedScreen;

	u8 *truc = (u8*)GPU_screen3D;
	u8 r, g, b, a;
	u32 offset;

	for(u32 y = 0; y < 192; y++){
		u32 yshift = (y >> 2)<< 12;
		u32 ymod = (y%4) << 2;
		for(u32 x = 0; x < 256; x++){

			offset = yshift + ((x >> 2)<<6) + ((ymod + (x%4)) << 1);

			a = *(truc+offset);
			r = *(truc+offset+1);
			g = *(truc+offset+32);
			b = *(truc+offset+33);

			*dst++ = (a >> 3) & 0x1F; // 5 bits
			*dst++ = (b >> 2) & 0x3F; // 6 bits
			*dst++ = (g >> 2) & 0x3F; // 6 bits
			*dst++ = (r >> 2) & 0x3F; // 6 bits

		}
	}

	DCFlushRange(gfx3d_convertedScreen, 256*192*4);
	GXDBG("ReadFramebuffer: deswizzle loop done");
#ifdef GXRENDER_DEBUG_LOG
	{
		u32 nonzeroAlpha = 0, nonzeroRGB = 0;
		for (u32 i = 0; i < 256*192; i++) {
			if (gfx3d_convertedScreen[i*4+0] != 0) nonzeroAlpha++;
			if (gfx3d_convertedScreen[i*4+1] || gfx3d_convertedScreen[i*4+2] || gfx3d_convertedScreen[i*4+3]) nonzeroRGB++;
		}
		u32 centerOff = (96*256 + 128) * 4;
		GXDBGF("ReadFramebuffer: convertedScreen nonzeroAlpha=%lu nonzeroRGB=%lu center(a,b,g,r)=(%d,%d,%d,%d)",
		       (unsigned long)nonzeroAlpha, (unsigned long)nonzeroRGB,
		       gfx3d_convertedScreen[centerOff+0], gfx3d_convertedScreen[centerOff+1],
		       gfx3d_convertedScreen[centerOff+2], gfx3d_convertedScreen[centerOff+3]);
	}
#endif

	// Restore vertical de-flicker filter mode
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GXDBG("ReadFramebuffer: exit");

}

//----------------------------------------
//
// Function: GXRender
//
// Render the screen! Finally!
//
// @pre     -
// @post    The 3D has been rendered: no sprites
// @param   -
// @return  -
//
//----------------------------------------

static void GXRender(){

	GXDBG("GXRender: enter");

	// Lock our drawing thread
	LWP_MutexLock(vidmutex);
	GXDBG("GXRender: mutex locked");

	// Set our video settings for 3D.
	Set3DVideoSettings();
	GXDBG("GXRender: Set3DVideoSettings done");

	u32 lastTextureFormat = 0, lastTexturePalette = 0, lastPolyAttr = 0,
		polyListCount = gfx3d.polylist->count, lastViewport = 0xFFFFFFFF;

	GXDBGF("GXRender: polyListCount=%lu", (unsigned long)polyListCount);

	for(u32 i = 0; i < polyListCount; ++i) {

		POLY *poly = &gfx3d.polylist->list[gfx3d.indexlist[i]];

		int type = poly->type;
		u8 alpha = (poly->getAlpha() << 3);

		GXDBGF("GXRender: poly[%lu] type=%d texParam=%lu polyAttr=%lu alpha=%d",
		       (unsigned long)i, type, (unsigned long)poly->texParam,
		       (unsigned long)poly->polyAttr, alpha);

		// If we have a new polygon texture...
		if( lastTextureFormat != poly->texParam ||
			lastTexturePalette != poly->texPalette ||
			lastPolyAttr != poly->polyAttr || i == 0 ){

			isTranslucent = poly->isTranslucent();
			InstallPolygonAttrib(lastPolyAttr = poly->polyAttr);
			lastTextureFormat = textureFormat = poly->texParam;
			lastTexturePalette = texturePalette = poly->texPalette;
			currPolyEnvMode = (poly->polyAttr >> 4) & 0x03;

#ifdef GXTEX_LOG_POLYMODE
			// Does this polygon actually need a texture blend mode
			// (POLYGON_ATTR bits 4-5: 0=Modulate,1=Decal,2/3=Toon-Highlight)
			// other than plain replace? InstallPolygonAttrib's own comment
			// block above shows this field (envMode) has never been read at
			// all - BeginRenderPoly() always does GX_SetTevOp(GX_REPLACE)
			// regardless. If a polygon using a mode-5 (4x4) texture actually
			// needs Toon/Highlight or Modulate, forcing plain replace would
			// render it wrong in a way that has nothing to do with the
			// texture's own decoded data - independent of (and possibly the
			// real explanation on top of) the texture-pipeline bugs already
			// fixed this session. Not wired into any shipping build flag.
			if (((textureFormat >> 26) & 0x07) != TEXMODE_NONE) {
				static int n = 0;
				if (n < 200) {
					n++;
					u32 mode = (textureFormat >> 26) & 0x07;
					u32 sx = 8 << ((textureFormat >> 20) & 0x07);
					u32 sy = 8 << ((textureFormat >> 23) & 0x07);
					u32 envMode = (poly->polyAttr >> 4) & 0x03;
					FILE *f = fopen("sd:/texdbg.log", "a");
					if (f) {
						fprintf(f, "poly mode=%lu sizeX=%lu sizeY=%lu texpal=%lu envMode=%lu polyAttr=0x%08lX alpha=%d\n",
						        (unsigned long)mode, (unsigned long)sx, (unsigned long)sy,
						        (unsigned long)texturePalette, (unsigned long)envMode,
						        (unsigned long)poly->polyAttr, (int)poly->getAlpha());
						fclose(f);
					}
				}
			}
#endif

			GXDBG("GXRender: calling BeginRenderPoly");
			BeginRenderPoly();
			GXDBG("GXRender: BeginRenderPoly done");

			// Create our own, DS-to-Wii specific projection matrix
			Mtx44 projection;

			float* m = poly->projMatrix;
			// Copy the matrix from Column-Major to Row-Major format
			for(int j = 0; j < 4; ++j)
				for(int i = 0; i < 4; ++i){
					projection[i][j] = *m++;
				}
			
			// Convert the z clipping planes from -1/1 to -1/0
			projection[2][2] = (projection[2][2] - projection[3][2])*0.5f;
			projection[2][3] = (projection[2][3] - projection[3][3])*0.5f;

			if(projection[3][2] != 1){
				//Frustum or perspective ?
				/* -- Do we need this? Just comment it out for now to remind me in the future

				if(projection[0][2] != 0)
					//frustrum
				else
					//prespective
				//*/

				GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
			}else{
				GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
			}
		}

		//--DCN: I still don't see the point of this:
		//*
		if(lastViewport != poly->viewport){
			VIEWPORT viewport;
			viewport.decode(poly->viewport);
			GX_SetViewport((f32)viewport.x, (f32)viewport.y, (f32)viewport.width, (f32)viewport.height, 0.0f, 1.0f);
			lastViewport = poly->viewport;
		}
		//*/

		GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, type);

			int j = type - 1;
			do{
				VERT *vert = &gfx3d.vertlist->list[poly->vertIndexes[j]];

				// vert->color is 6-bit (0-63; gfx3d.cpp expands the DS's native
				// 5-bit vertex color with GFX3D_5TO6 before storing it here),
				// but GX_Color4u8 takes full 8-bit (0-255) channels. Passed
				// through unscaled, every vertex color tops out at 63/255 -
				// about a quarter of full brightness - so untextured/
				// vertex-coloured geometry (the only place this ever mattered:
				// textured polygons use GX_REPLACE, which ignores the colour
				// channel entirely) rendered visibly, but far too dark.
				// Replicate the 6 bits across the full 8-bit range the way the
				// alpha scaling a few lines up already does for 5-bit alpha.
				u8 r6 = vert->color[0], g6 = vert->color[1], b6 = vert->color[2];

				// Toon/Highlight (see BeginRenderPoly()'s long comment): swap
				// in the TOON_TABLE colour this vertex's own red channel
				// indexes, matching rasterize.cpp's `toonTable[materialColor.r
				// >>1]` exactly (vert->color is 6-bit here too, same scale).
				// BeginRenderPoly() already picked GX_MODULATE for a textured
				// poly in this mode, so GX multiplying this substituted colour
				// by the texture reproduces Toon mode's own texture*toonColor
				// formula; for an untextured poly (GX_PASSCLR, Cv=Cr) it comes
				// straight through, matching Toon's own "no texture -> just
				// use toon directly" case.
				if (currPolyEnvMode == 2) {
					const u16 toon = gfx3d.u16ToonTable[r6 >> 1];
					r6 = GFX3D_5TO6(toon & 0x1F);
					g6 = GFX3D_5TO6((toon >> 5) & 0x1F);
					b6 = GFX3D_5TO6((toon >> 10) & 0x1F);
				}

				GX_Position3f32(vert->x, vert->y, vert->z);
				GX_Color4u8((r6 << 2) | (r6 >> 4), (g6 << 2) | (g6 >> 4), (b6 << 2) | (b6 >> 4), alpha);
				// Set3DVideoSettings() only declares a TEX0 vertex attribute in
				// the VCD when gfx3d.enableTexturing is true for this frame (see
				// the if/else there) - it is a real per-frame flag, not always
				// true. Emitting GX_TexCoord2f32 here unconditionally, whether or
				// not the VCD was set up for it, desyncs the CPU's write-gather
				// pipe from what the GP's vertex parser expects: it silently
				// writes 8 extra bytes/vertex the parser was never told to
				// consume, corrupting the FIFO stream for the rest of the frame.
				// A scene with no textured polygons at all (e.g. flat-shaded
				// geometry) hits this on its very first draw and hangs forever at
				// the next GX_DrawDone(), waiting on data that was never sent in
				// the shape the GP was configured for.
				if (gfx3d.enableTexturing)
					GX_TexCoord2f32(vert->u, vert->v);

				--j;
			}while(j >= 0);

		GX_End();

	}

	GXDBG("GXRender: poly loop done");

	// GX_End() is a documented no-op in libogc - it does NOT flush the CPU's
	// write-gather pipe (WGPIPE). That pipe physically ships immediate-mode
	// vertex data to the GP in 32-byte bursts; our per-vertex format here is
	// POS_XYZ(12B) + CLR0_RGBA8(4B) + TEX0_ST(8B) = 24B/vertex, so a 3-vertex
	// triangle (72B) or any odd polygon count never lands on a 32B boundary.
	// The trailing partial burst then sits stuck in CPU cache, never physically
	// reaching the GP - which then hangs forever at the very next GX_DrawDone()
	// (inside ReadFramebuffer(), a few lines below) waiting on vertex bytes
	// that were never actually sent. GX_Flush() forces that pending partial
	// burst out and pads it to the boundary; call it once here, after every
	// polygon this frame has been submitted and before anything waits on the
	// GP. (draw_thread's own 2D screen-quad draws never hit this: their
	// vertex format is POS_XY(8B)+TEX0_ST(8B) = 16B/vertex x 4 verts = 64B,
	// already a clean 32B multiple.)
	GX_Flush();
	GXDBG("GXRender: GX_Flush done");

	// Copy everything to a texture for later use
	ReadFramebuffer();
	GXDBG("GXRender: ReadFramebuffer done");

	// Evict expired texture-cache entries only after ReadFramebuffer()'s own
	// GX_DrawDone() has confirmed the GPU is done with this frame's draws.
	// evict() can genuinely delete a TexCacheItem - freeing its `decoded`
	// buffer - for any entry that's aged out since the cache exceeded its 4MB
	// cap; GX rendering is asynchronous (FIFO-based), so freeing (and
	// potentially reusing, via memalign(), for an unrelated texture's decode
	// on a later frame) a texture's backing memory before the GPU has
	// actually finished sampling it for THIS frame's polygons is a real
	// use-after-free race: the polygon that used it can end up rendering
	// with whatever unrelated data got written into that memory afterward.
	// This call used to run right after GX_Flush() (which only flushes the
	// CPU's vertex write-gather pipe, not texture reads, and doesn't wait for
	// the GPU at all) and well before GX_DrawDone() - eviction was racing the
	// GPU on every frame it actually fired, silently, for however long the
	// freed memory happened to sit unreused. Reproduces as an on-screen
	// texture rendering as garbage unrelated to its own correctly-decoded,
	// correctly-tiled content (both independently verified byte-for-byte via
	// SD-card dumps against a real title) - never on the small test ROMs,
	// since none of them decode anywhere near enough distinct textures in one
	// session to make the 4MB cache cap (and therefore eviction) actually
	// engage.
	TexCache_EvictFrame();
	GXDBG("GXRender: TexCache_EvictFrame done");

	// Reset everything back to what it was
	ResetVideoSettings();
	GXDBG("GXRender: ResetVideoSettings done");

	// Unlock the thread
	LWP_MutexUnlock(vidmutex);
	GXDBG("GXRender: exit");

}

//----------------------------------------
//
// Function: Set3DVideoSettings
//
// Sets the variables specific to our 3D scene
//
// @pre     -
// @post    -
// @param   -
// @return  -
//
//----------------------------------------

static void Set3DVideoSettings(){

	//Mtx44 projection; // Projection matrix
	Mtx modelview;

	// Set up the viewpoint (one screen)
	GX_SetViewport(0,0,256,192,0,1);
	GX_SetScissor(0,0,256,192);

	guMtxIdentity(modelview);
	// Load in an identity matrix to be our position matrix
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);

	//The only EFB pixel format supporting an alpha buffer is GX_PF_RGBA6_Z24
	GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);

	//See: GX_SetTevAlphaOp  and GX_SetTevColorOp
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	GX_InvVtxCache();
	GX_InvalidateTexAll();
	GX_ClearVtxDesc();

	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

	// The DS 3D engine has already fully resolved each vertex's final color (lit
	// or not - lighting itself isn't implemented on this core) before GXRender
	// ever sees it, in gfx3d's VERT::color. Nothing here ever calls
	// GX_SetChanCtrl, so the colour channel is left at whatever GX_Init() /
	// main.cpp's one-time boot setup defaulted it to; if that default sources
	// the channel from a register instead of the per-vertex GX_CLR0 data, every
	// untextured polygon renders as a flat register colour (black, if nothing
	// ever set GX_SetChanMatColor either) regardless of the real vertex colours
	// pushed via GX_Color4u8. Force it explicitly, every 3D frame: no lighting,
	// material colour from the vertex, so untextured/flat-shaded geometry
	// actually shows the DS-computed vertex colour instead of going black.
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	if(gfx3d.enableTexturing){

		GX_SetNumTexGens(1);

		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_VA_TEX0, GX_IDENTITY);

		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

	}else{
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	}

	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	GXDBGF("Set3DVideoSettings: enableTexturing=%d enableAlphaTest=%d enableAlphaBlending=%d alphaTestRef=%d",
	       (int)gfx3d.enableTexturing, (int)gfx3d.enableAlphaTest,
	       (int)gfx3d.enableAlphaBlending, (int)gfx3d.alphaTestRef);

	if(gfx3d.enableAlphaTest){

		// OGLRender comment: FIXME: alpha test should pass gfx3d.alphaTestRef==poly->getAlpha

		// We need two comparisons, so we ignore the second parameter (for now)
		GX_SetAlphaCompare(GX_GREATER, s32(s32(gfx3d.alphaTestRef)/31.f), GX_AOP_OR, GX_NEVER, 0);
	}else{
		// We might be able to just ignore this instruction,
		// since we're not alpha blending.
		GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_OR , GX_NEVER, 0);
	}
	// Always write real alpha. gfx3d_convertedScreen's alpha nibble is not
	// just a blending input - GPU.cpp's legacy compositor uses it as the
	// per-pixel "did the 3D scene draw here at all" signal
	// (`if(colorLine[(q<<2)]) gpu->setFinalColor3d(k,q);`), which every frame
	// depends on regardless of whether this particular scene uses real alpha
	// blending. With alpha updates off (the old `enableAlphaBlending || merge-
	// Armed` gate), the EFB alpha for every pixel - both the parts a polygon
	// drew and the parts ReadFramebuffer() clears to transparent - freezes at
	// whatever the clear set it to, so a fully-opaque scene either reads as
	// "3D everywhere" (opaque clear) or "3D nowhere, including the polygon
	// itself" (transparent clear). Writing real alpha here keeps it faithful
	// to what actually got drawn either way.
	//
	// Phase 1 used to follow this with GX_SetDstAlpha(GX_TRUE,0xFF) on an
	// armed frame, forcing every drawn polygon's alpha to a binary coverage
	// flag once it reached the EFB - the sandwich's MB_OPAQUE_MASKED draw
	// only ever needed "did 3D draw here at all", not the real value. Phase
	// 2b's MB_ALPHA_OVER (GXMerge.cpp's GXMergeBand::alphaOver) needs that
	// real per-pixel alpha intact - it's the same polygon alpha
	// _master_setFinal3dColor already blends the legacy 2D/3D layer with -
	// so that override is gone entirely now. It's safe to drop unconditionally,
	// not just on an armed frame: the destination texture's "no polygon drew
	// here" pixels are already alpha 0 from ReadFramebuffer's transparent EFB
	// clear (GX_SetDstAlpha never touched those - only fragments an actual
	// draw call wrote to), so plain alpha-greater-than-zero is exactly as
	// good a coverage test as the old binary mask, and every visible polygon
	// writes an alpha well above the sandwich's coverage thresholds anyway
	// (DS content overwhelmingly renders "opaque" polygons at alpha 31/31).
	GX_SetAlphaUpdate(GX_TRUE);

	//--DCN: For reasons that I could not find, "Vanilla" does not
	// have any fog in its OpenGL implementation.

	if(gfx3d.enableFog && CommonSettings.GFX3D_Fog){

		/*
		//TODO: Make fogColor a GXColor so we won't have to convert it
		GXColor col = {
			GFX3D_5TO6((gfx3d.fogColor)&0x1F),
			GFX3D_5TO6((gfx3d.fogColor>>5)&0x1F),
			GFX3D_5TO6((gfx3d.fogColor>>10)&0x1F),
			(gfx3d.fogColor>>16)&0x1F
		};
		
		//--DCN: I just picked random numbers here.
		GX_SetFog(GX_FOG_LIN, 16.0f, 1000.0f, 0.0f, 1.0f, col);
		//*/
		/*
		// There is no function to initialize the GXFogAdjTable.
		// If it DID exist, we would call it like so:
		GXFogAdjTbl table;
		//
		// Function: GX_InitFogAdjTable
		//
		// @param: GXFogAdjTbl* table: The Fog adjustment table
		// @param: u16 width:     The width of our current viewport
		// @param: Mtx44 projmtx: The projection matrix that we're using
		GX_InitFogAdjTable(&table, 256, projection);
		// 
		// I believe that GX_SetFogRangeAdj does not do
		// what it is supposed to do, seeing as how none of
		// the variables passed are used in the function.
		GX_SetFogRangeAdj(GX_ENABLE, 256/2 , &table);
		//*/
	}


	// In general, if alpha compare is enabled, Z-buffering
	// should occur AFTER texture lookup.
	GX_SetZCompLoc(GX_FALSE);

}


//----------------------------------------
//
// Function: ResetVideoSettings
//
// Reset the video settings to what main.cpp needs
//
// @pre     -
// @post    The video settings are reset back to what they were
// @param   -
// @return  -
//
//----------------------------------------

static void ResetVideoSettings(){

	Mtx44 perspective;
	
	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	GX_SetScissor(0,0,rmode->fbWidth,rmode->efbHeight);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
	// A 3D frame with only untextured polygons (BeginRenderPoly's TEXMODE_NONE
	// branch) leaves this at 0 - draw_thread's own TopTex/BottomTex quads below
	// need it back at 1, or the texture unit has no coordinates to sample with
	// (GX_SetTexCoordGen a few lines down configures a generator, but it's
	// inert with zero active tex-gens) and the "textured" quad comes out black.
	GX_SetNumTexGens(1);

	guOrtho(perspective,0,479,0,639,0,300);
	GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

	GX_SetCullMode(GX_CULL_NONE);
	GX_SetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX3x4, GX_TG_TEX0, GX_IDENTITY);

	GX_SetZCompLoc(GX_TRUE);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

	if(GXMerge_Enabled()){
		// Only hand an RGB8 EFB to the present pass on a frame the sandwich runs.
		// An unarmed frame presents exactly like legacy, which leaves it RGBA6.
		if(GXMerge_FrameArmed())
			GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	}

}

static void GXVramReconfigureSignal(){
	TexCache_Invalidate();
}

GPU3DInterface gpu3Dgx = {
	"GX",
	GXInit,
	GXReset,
	GXClose,
	GXRender,
	GXVramReconfigureSignal,
};
