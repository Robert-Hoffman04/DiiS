/*  Copyright (C) 2006 yopyop
    Copyright (C) 2006-2007 Theo Berkau
    Copyright (C) 2007 shash
	Copyright (C) 2008-2009 DeSmuME team
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

#include <algorithm>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <iostream>
#include "MMU.h"
#include "GPU.h"

// Diagnostic log for the DISPCAPCNT display-capture path + per-engine
// DisplayMode, used to confirm/deny whether a game's dual-3D-screen trick
// (capture engine A's composited 3D output to a VRAM bank, then have engine
// B display that bank as DisplayMode==2 "VRAM framebuffer" on the other
// physical screen, flipping POWCNT's swap bit each frame) is what's actually
// running, and whether the two engines are reading/writing the banks the way
// that trick requires. Throttled - this is register-write/frame-boundary
// traffic, not per-pixel.
#ifdef GPU_DISPCAP_DEBUG_LOG
#include <stdio.h>
#include <stdarg.h>
static void gpudbg(const char *fmt, ...)
{
	static int n = 0;
	if (n >= 4000) return;
	n++;
	FILE *f = fopen("sd:/dispcap.log", "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}
#define GPUDBG(...) gpudbg(__VA_ARGS__)

// Ring buffer of recent frames' DISPCAPCNT/offset state (RAM only - no I/O,
// so this can run every frame for the whole session at negligible cost).
// GPU_DispCapDumpRing() (GPU.h), triggered from a GC pad button in
// main.cpp's DSExec, writes it out on demand - lets a trace be centered on
// a visual glitch the moment it's spotted on screen instead of a blind
// timed capture window. See the recorder at the GPU_RenderLine l==0 site.
struct DispCapRingEntry {
	u32 frame;
	u8 offsetA, offsetB;
	u8 dispModeA, dispModeB;
	u8 vramBlockA, vramBlockB;
	u8 bg0_3dA, bg0_3dB;
	u8 capEnabled, capArm;
	u8 writeBlock, readBlock;
	u8 capSrc, srcA, srcB;
};
#define DISPCAP_RING_CAP 1024
static DispCapRingEntry g_dispCapRing[DISPCAP_RING_CAP];
static u32 g_dispCapRingHead = 0; // next slot to write; entries wrap at DISPCAP_RING_CAP

void GPU_DispCapDumpRing()
{
	FILE *f = fopen("sd:/dispring.log", "w");
	if (!f) return;
	u32 count = (g_dispCapRingHead < DISPCAP_RING_CAP) ? g_dispCapRingHead : DISPCAP_RING_CAP;
	u32 start = (g_dispCapRingHead < DISPCAP_RING_CAP) ? 0 : g_dispCapRingHead;
	fprintf(f, "ring dump: %u entries, head=%u\n", count, g_dispCapRingHead);
	for (u32 i = 0; i < count; i++) {
		DispCapRingEntry *e = &g_dispCapRing[(start + i) % DISPCAP_RING_CAP];
		fprintf(f, "#%u A[disp=%d vram=%d bg0_3d=%d off=%d] B[disp=%d vram=%d bg0_3d=%d off=%d] cap[en=%d arm=%d wBlk=%d rBlk=%d src=%d/%d/%d]\n",
				e->frame,
				e->dispModeA, e->vramBlockA, e->bg0_3dA, e->offsetA,
				e->dispModeB, e->vramBlockB, e->bg0_3dB, e->offsetB,
				e->capEnabled, e->capArm, e->writeBlock, e->readBlock,
				e->capSrc, e->srcA, e->srcB);
	}
	fclose(f);
}
#else
#define GPUDBG(...)
#endif
#include "render3D.h"
#include "gfx3d.h"
#include "debug.h"
#include "NDSSystem.h"
#include "readwrite.h"
#include "guDesmume.h"
#include "GXMerge.h"
#include "GXDirty.h"
#include "GX2DBG.h"

// Per-scanline mergeability check (defined below, near GXMerge_FrameMergeable);
// forward-declared so GPU_RenderLine_layer's 3D split point can call it.
static bool GXMerge_LineMergeable(GPU * gpu, bool *outAlphaOver,
                                  bool *outFrontAlphaOver, u8 *outFrontEva,
                                  u8 *outBrightMode, u8 *outBrightFactor);

#ifdef FASTBUILD
	#undef FORCEINLINE
	#define FORCEINLINE
	//compilation speed hack (cuts time exactly in half by cutting out permutations)
	#define DISABLE_MOSAIC
#endif

extern BOOL click;
NDS_Screen MainScreen;
NDS_Screen SubScreen;

//instantiate static instance
GPU::MosaicLookup GPU::mosaicLookup;

//#define DEBUG_TRI

CACHE_ALIGN u8 GPU_screen[4*256*192];
CACHE_ALIGN u8 sprWin[256];

u16 gpu_angle = 0;

const size sprSizeTab[4][4] = 
{
     {{8, 8}, {16, 8}, {8, 16}, {8, 8}},
     {{16, 16}, {32, 8}, {8, 32}, {8, 8}},
     {{32, 32}, {32, 16}, {16, 32}, {8, 8}},
     {{64, 64}, {64, 32}, {32, 64}, {8, 8}},
};



const BGType GPU_mode2type[8][4] = 
{
      {BGType_Text, BGType_Text, BGType_Text, BGType_Text},
      {BGType_Text, BGType_Text, BGType_Text, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_Affine},
      {BGType_Text, BGType_Text, BGType_Text, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_Affine, BGType_AffineExt},
      {BGType_Text, BGType_Text, BGType_AffineExt, BGType_AffineExt},
      {BGType_Invalid, BGType_Invalid, BGType_Large8bpp, BGType_Invalid},
      {BGType_Invalid, BGType_Invalid, BGType_Invalid, BGType_Invalid}
};

//dont ever think of changing these to bits because you could avoid the multiplies in the main tile blitter.
//it doesnt really help any
const short sizeTab[8][4][2] =
{
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //Invalid
	{{256,256}, {512,256}, {256,512}, {512,512}}, //text
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine
	{{512,1024}, {1024,512}, {0,0}, {0,0}}, //large 8bpp
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //affine ext (to be elaborated with another value)
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine ext 256x16
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext 256x1
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext direct
};

static GraphicsInterface_struct *GFXCore=NULL;

// This should eventually be moved to the port specific code
GraphicsInterface_struct *GFXCoreList[] = {
&GFXDummy,
NULL
};

static const CACHE_ALIGN u8 win_empty[256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static CACHE_ALIGN u16 fadeInColors[17][0x8000];
CACHE_ALIGN u16 fadeOutColors[17][0x8000];

//this should be public, because it gets used somewhere else
CACHE_ALIGN u8 gpuBlendTable555[17][17][32][32];



/*****************************************************************************/
//			INITIALIZATION
/*****************************************************************************/


static void GPU_InitFadeColors()
{
	/*
	NOTE: gbatek (in the reference above) seems to expect 6bit values 
	per component, but as desmume works with 5bit per component, 
	we use 31 as top, instead of 63. Testing it on a few games, 
	using 63 seems to give severe color wraping, and 31 works
	nicely, so for now we'll just that, until proven wrong.

	i have seen pics of pokemon ranger getting white with 31, with 63 it is nice.
	it could be pb of alpha or blending or...

	MightyMax> created a test NDS to check how the brightness values work,
	and 31 seems to be correct. FactorEx is a override for max brighten/darken
	See: http://mightymax.org/gfx_test_brightness.nds
	The Pokemon Problem could be a problem with 8/32 bit writes not recognized yet,
	i'll add that so you can check back.

	*/

	for(int i = 0; i <= 16; i++)
	{
		float idiv16 = ((float)i) / 16;
		for(int j = 0x8000; j < 0x10000; j++)
		{
			COLOR cur;

			cur.val = j;
			cur.bits.red = (cur.bits.red + ((31 - cur.bits.red) * idiv16));
			cur.bits.green = (cur.bits.green + ((31 - cur.bits.green) * idiv16));
			cur.bits.blue = (cur.bits.blue + ((31 - cur.bits.blue) * idiv16));
			cur.bits.alpha = 0;
			fadeInColors[i][j & 0x7FFF] = cur.val;

			cur.val = j;
			cur.bits.red = (cur.bits.red - (cur.bits.red * idiv16));
			cur.bits.green = (cur.bits.green - (cur.bits.green * idiv16));
			cur.bits.blue = (cur.bits.blue - (cur.bits.blue * idiv16));
			cur.bits.alpha = 0;
			fadeOutColors[i][j & 0x7FFF] = cur.val;
		}
	}

	for(int eva=0;eva<=16;eva++){
		for(int evb=0;evb<=16;evb++){
			for(int c0=0;c0<=31;c0++){ 
				for(int c1=0;c1<=31;c1++){
					int final = std::min<int>(31,(((c0 * eva) + (c1 * evb)) >>4));
					gpuBlendTable555[eva][evb][c0][c1] = final;
				}
			}
		}
	}
}

static CACHE_ALIGN GPU GPU_main, GPU_sub;

GPU * GPU_Init(u8 l)
{
	GPU * g;

	if(l==0) g = &GPU_main;
	else g = &GPU_sub;

	GPU_Reset(g, l);
	GPU_InitFadeColors();

	g->curr_win[0] = win_empty;
	g->curr_win[1] = win_empty;
	g->need_update_winh[0] = true;
	g->need_update_winh[1] = true;
	g->setFinalColorBck_funcNum = 0;
	g->setFinalColor3d_funcNum = 0;
	g->setFinalColorSpr_funcNum = 0;

	return g;
}

void GPU_Reset(GPU *g, u8 l)
{
	memset(g, 0, sizeof(GPU));

	//important for emulator stability for this to initialize, since we have to setup a table based on it
	g->BLDALPHA_EVA = 0;
	g->BLDALPHA_EVB = 0;
	//make sure we have our blend table setup even if the game blends without setting the blend variables
	g->updateBLDALPHA();

	g->setFinalColorBck_funcNum = 0;
	g->setFinalColor3d_funcNum = 0;
	g->setFinalColorSpr_funcNum = 0;
	g->core = l;
	g->BGSize[0][0] = g->BGSize[1][0] = g->BGSize[2][0] = g->BGSize[3][0] = 256;
	g->BGSize[0][1] = g->BGSize[1][1] = g->BGSize[2][1] = g->BGSize[3][1] = 256;

	g->spriteRenderMode = GPU::SPRITE_1D;

	g->bgPrio[4] = 0xFF;

	//g->bg0HasHighestPrio = TRUE;

	if(g->core == GPU_SUB)
	{
		g->oam = (OAM *)(MMU.ARM9_OAM + ADDRESS_STEP_1KB);
		g->sprMem = MMU_BOBJ;
		// GPU core B
		g->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[REG_DISPB]);
	}
	else
	{
		g->oam = (OAM *)(MMU.ARM9_OAM);
		g->sprMem = MMU_AOBJ;
		// GPU core A
		g->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[0]);
	}
}

void GPU_DeInit(GPU * gpu)
{
	if(gpu==&GPU_main || gpu==&GPU_sub) return;
	free(gpu);
}

static void GPU_resortBGs(GPU *gpu)
{
	int i, prio;
	const struct _DISPCNT * cnt = &gpu->dispx_st->dispx_DISPCNT.bits;
	itemsForPriority_t * item;

	// we don't need to check for windows here...
// if we tick boxes, invisible layers become invisible & vice versa
#define OP ^ !
// if we untick boxes, layers become invisible
//#define OP &&
	gpu->LayersEnable[0] = CommonSettings.dispLayers[gpu->core][0] OP(cnt->BG0_Enable/* && !(cnt->BG0_3D && (gpu->core==0))*/);
	gpu->LayersEnable[1] = CommonSettings.dispLayers[gpu->core][1] OP(cnt->BG1_Enable);
	gpu->LayersEnable[2] = CommonSettings.dispLayers[gpu->core][2] OP(cnt->BG2_Enable);
	gpu->LayersEnable[3] = CommonSettings.dispLayers[gpu->core][3] OP(cnt->BG3_Enable);
	gpu->LayersEnable[4] = CommonSettings.dispLayers[gpu->core][4] OP(cnt->OBJ_Enable);

	// KISS ! lower priority first, if same then lower num
	for (i=0;i<NB_PRIORITIES;i++) {
		item = &(gpu->itemsForPriority[i]);
		item->nbBGs=0;
		item->nbPixelsX=0;
	}
	i = NB_BG; 
	do{
		--i;
		if (!gpu->LayersEnable[i]) continue;
		prio = (gpu->dispx_st)->dispx_BGxCNT[i].bits.Priority;
		item = &(gpu->itemsForPriority[prio]);
		item->BGs[item->nbBGs]=i;
		++item->nbBGs;
	}while(i > 0);

	/*
	const int bg0Prio = gpu->dispx_st->dispx_BGxCNT[0].bits.Priority;
	gpu->bg0HasHighestPrio = TRUE;
	for(i = 1; i < 4; i++)
	{
		if(gpu->LayersEnable[i])
		{
			if(gpu->dispx_st->dispx_BGxCNT[i].bits.Priority < bg0Prio)
			{
				gpu->bg0HasHighestPrio = FALSE;
				break;
			}
		}
	}
	*/
	
#if 0
//debug
	for (i=0;i<NB_PRIORITIES;i++) {
		item = &(gpu->itemsForPriority[i]);
		printf("%d : ", i);
		for (j=0; j<NB_PRIORITIES; j++) {
			if (j<item->nbBGs) 
				printf("BG%d ", item->BGs[j]);
			else
				printf("... ", item->BGs[j]);
		}
	}
	printf("\n");
#endif
}

static FORCEINLINE u16 _blend(u16 colA, u16 colB, GPU::TBlendTable* blendTable)
{
	const u8 r = (*blendTable)[colA&0x1F][colB&0x1F];
	const u8 g = (*blendTable)[(colA>>5)&0x1F][(colB>>5)&0x1F];
	const u8 b = (*blendTable)[(colA>>10)&0x1F][(colB>>10)&0x1F];

	return r|(g<<5)|(b<<10);
}

FORCEINLINE u16 GPU::blend(u16 colA, u16 colB)
{
	return _blend(colA, colB, blendTable);
}


void GPU_setMasterBrightness (GPU *gpu, u16 val)
{
	if(!nds.isInVblank()) {
		PROGINFO("Changing master brightness outside of vblank\n");
	}
 	gpu->MasterBrightFactor = (val & 0x1F);
	gpu->MasterBrightMode	= (val>>14);
}

void SetupFinalPixelBlitter (GPU *gpu)
{
	u8 windowUsed = (gpu->WIN0_ENABLED | gpu->WIN1_ENABLED | gpu->WINOBJ_ENABLED);
	u8 blendMode  = (gpu->BLDCNT >> 6)&3;
	u32 winUsedBlend = (windowUsed<<2) + blendMode;

	/*
	printf("-----------------------------\n");
	printf("windowUsed   : %d\n", windowUsed);
	printf("Blend mode   : %d\n", blendMode);
	printf("winUsedBlend : %d\n", winUsedBlend);
	// This all ends up coming down to the blend mode
	// windowUsed is always zero.
	//*/

	gpu->setFinalColorSpr_funcNum = winUsedBlend;
	gpu->setFinalColorBck_funcNum = winUsedBlend;
	gpu->setFinalColor3d_funcNum  = winUsedBlend;
	
}
    
//Sets up LCD control variables for Display Engines A and B for quick reading
void GPU_setVideoProp(GPU * gpu, u32 p)
{
	struct _DISPCNT * cnt;
	cnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	T1WriteLong((u8 *)&(gpu->dispx_st)->dispx_DISPCNT.val, 0, p);

	gpu->WIN0_ENABLED	= cnt->Win0_Enable;
	gpu->WIN1_ENABLED	= cnt->Win1_Enable;
	gpu->WINOBJ_ENABLED = cnt->WinOBJ_Enable;

	SetupFinalPixelBlitter (gpu);

	gpu->dispMode = cnt->DisplayMode & ((gpu->core)?1:3);

	gpu->vramBlock = cnt->VRAM_Block;
	
	switch (gpu->dispMode)
	{
		case 0: // Display Off
			break;
		case 1: // Display BG and OBJ layers
			break;
		case 2: // Display framebuffer
			gpu->VRAMaddr = (u8 *)MMU.ARM9_LCD + (gpu->vramBlock * 0x20000);
			break;
		case 3: // Display from Main RAM
			// nothing to be done here
			// see GPU_RenderLine who gets data from FIFO.
			break;
	}

	if(cnt->OBJ_Tile_mapping)
	{
		//1-d sprite mapping boundaries:
		//32k, 64k, 128k, 256k
		gpu->sprBoundary = 5 + cnt->OBJ_Tile_1D_Bound ;
		
		//do not be deceived: even though a sprBoundary==8 (256KB region) is impossible to fully address
		//in GPU_SUB, it is still fully legal to address it with that granularity.
		//so don't do this: //if((gpu->core == GPU_SUB) && (cnt->OBJ_Tile_1D_Bound == 3)) gpu->sprBoundary = 7;

		gpu->spriteRenderMode = GPU::SPRITE_1D;
	} else {
		//2d sprite mapping
		//boundary : 32k
		gpu->sprBoundary = 5;
		gpu->spriteRenderMode = GPU::SPRITE_2D;
	}
     
	if(cnt->OBJ_BMP_1D_Bound && (gpu->core == GPU_MAIN))
		gpu->sprBMPBoundary = 8;
	else
		gpu->sprBMPBoundary = 7;

	gpu->sprEnable = cnt->OBJ_Enable;
	
	GPU_setBGProp(gpu, 3, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 14));
	GPU_setBGProp(gpu, 2, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 12));
	GPU_setBGProp(gpu, 1, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 10));
	GPU_setBGProp(gpu, 0, T1ReadWord(MMU.ARM9_REG, gpu->core * ADDRESS_STEP_4KB + 8));
	
	//GPU_resortBGs(gpu);
}

//this handles writing in BGxCNT
void GPU_setBGProp(GPU * gpu, u16 num, u16 p)
{
	struct _BGxCNT * cnt = &((gpu->dispx_st)->dispx_BGxCNT[num].bits);
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	
	T1WriteWord((u8 *)&(gpu->dispx_st)->dispx_BGxCNT[num].val, 0, p);
	
	GPU_resortBGs(gpu);

	if(gpu->core == GPU_SUB)
	{
		gpu->BG_tile_ram[num] = MMU_BBG;
		gpu->BG_bmp_ram[num]  = MMU_BBG;
		gpu->BG_bmp_large_ram[num]  = MMU_BBG;
		gpu->BG_map_ram[num]  = MMU_BBG;
	} 
	else 
	{
		gpu->BG_tile_ram[num] = MMU_ABG +  dispCnt->CharacBase_Block * ADDRESS_STEP_64KB ;
		gpu->BG_bmp_ram[num]  = MMU_ABG;
		gpu->BG_bmp_large_ram[num] = MMU_ABG;
		gpu->BG_map_ram[num]  = MMU_ABG +  dispCnt->ScreenBase_Block * ADDRESS_STEP_64KB;
	}

	gpu->BG_tile_ram[num] += (cnt->CharacBase_Block * ADDRESS_STEP_16KB);
	gpu->BG_bmp_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_16KB);
	gpu->BG_map_ram[num]  += (cnt->ScreenBase_Block * ADDRESS_STEP_2KB);

	switch(num)
	{
		case 0:
		case 1:
			gpu->BGExtPalSlot[num] = cnt->PaletteSet_Wrap * 2 + num ;
			break;
			
		default:
			gpu->BGExtPalSlot[num] = (u8)num;
			break;
	}

	BGType mode = GPU_mode2type[dispCnt->BG_Mode][num];

	//clarify affine ext modes 
	if(mode == BGType_AffineExt)
	{
		//see: http://nocash.emubase.de/gbatek.htm#dsvideobgmodescontrol
		u8 affineModeSelection = (cnt->Palette_256 << 1) | (cnt->CharacBase_Block & 1) ;
		switch(affineModeSelection)
		{
		case 0:
		case 1:
			mode = BGType_AffineExt_256x16;
			break;
		case 2:
			mode = BGType_AffineExt_256x1;
			break;
		case 3:
			mode = BGType_AffineExt_Direct;
			break;
		}
	}

	gpu->BGTypes[num] = mode;

	gpu->BGSize[num][0] = sizeTab[mode][cnt->ScreenSize][0];
	gpu->BGSize[num][1] = sizeTab[mode][cnt->ScreenSize][1];
	
	gpu->bgPrio[num] = (p & 0x3);
}

/*****************************************************************************/
//			ENABLING / DISABLING LAYERS
/*****************************************************************************/

void GPU_remove(GPU * gpu, u8 num)
{
	CommonSettings.dispLayers[gpu->core][num] = false;
	GPU_resortBGs(gpu);
}
void GPU_addBack(GPU * gpu, u8 num)
{
	CommonSettings.dispLayers[gpu->core][num] = true;
	GPU_resortBGs(gpu);
}


/*****************************************************************************/
//		ROUTINES FOR INSIDE / OUTSIDE WINDOW CHECKS
/*****************************************************************************/

template<int WIN_NUM>
FORCEINLINE u8 GPU::withinRect(u16 x) const
{
	assert(x<256); //only way to be >256 is in debug views, and mosaic shouldnt be enabled for those
	return curr_win[WIN_NUM][x];
}



//  Now assumes that *draw and *effect are different from 0 when called, so we can avoid
// setting some values twice
FORCEINLINE void GPU::renderline_checkWindows(u16 x, bool &draw, bool &effect) const
{
	// Check if win0 if enabled, and only check if it is
	// howevever, this has already been taken care of by the window precalculation
	//if (WIN0_ENABLED)
	{
		// it is in win0, do we display ?
		// high priority	
		if (withinRect<0>(x))
		{
			//INFO("bg%i passed win0 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN0H0, gpu->WIN0V0, gpu->WIN0H1, gpu->WIN0V1);
			draw = (WININ0 >> currBgNum)&1;
			effect = (WININ0_SPECIAL);
			return;
		}
	}

	// Check if win1 if enabled, and only check if it is
	//if (WIN1_ENABLED)
	// howevever, this has already been taken care of by the window precalculation
	{
		// it is in win1, do we display ?
		// mid priority
		if(withinRect<1>(x))
		{
			//INFO("bg%i passed win1 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN1H0, gpu->WIN1V0, gpu->WIN1H1, gpu->WIN1V1);
			draw	= (WININ1 >> currBgNum)&1;
			effect = (WININ1_SPECIAL);
			return;
		}
	}

	//if(true) //sprwin test hack
	if (WINOBJ_ENABLED)
	{
		// it is in winOBJ, do we display ?
		// low priority
		if (sprWin[x])
		{
			draw	= (WINOBJ >> currBgNum)&1;
			effect	= (WINOBJ_SPECIAL);
			return;
		}
	}

	if (WINOBJ_ENABLED | WIN1_ENABLED | WIN0_ENABLED)
	{
		draw	= (WINOUT >> currBgNum) & 1;
		effect	= (WINOUT_SPECIAL);
	}
}

/*****************************************************************************/
//			PIXEL RENDERING
/*****************************************************************************/

template<BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL void GPU::_master_setFinal3dColor(int dstX, int srcX)
{
	int x = dstX;
	int passing = dstX<<1;
	u8* color = &_3dColorLine[srcX<<2];
	u8 red = color[3];
	u8 green = color[2];
	u8 blue = color[1];
	u8 alpha = color[0];
	u8* dst = currDst;
	u16 final;

	bool windowEffect = blend1; //bomberman land touch dialogbox will fail without setting to blend1

	//TODO - should we do an alpha==0 -> bail out entirely check here?

	if(WINDOW)
	{
		bool windowDraw = false;
		renderline_checkWindows(dstX, windowDraw, windowEffect);

		//we never have anything more to do if the window rejected us
		if(!windowDraw) return;
	}

	int bg_under = bgPixels[dstX];
	if(blend2[bg_under])
	{
		alpha++;
		if(alpha<32)
		{
			//if the layer underneath is a blend bottom layer, then 3d always alpha blends with it
			COLOR c2, cfinal;
			c2.val = HostReadWord(dst, passing);

			cfinal.bits.red		= ((red * alpha) + ((c2.bits.red<<1) * (32 - alpha)))>>6;
			cfinal.bits.green	= ((green * alpha) + ((c2.bits.green<<1) * (32 - alpha)))>>6;
			cfinal.bits.blue	= ((blue * alpha) + ((c2.bits.blue<<1) * (32 - alpha)))>>6;

			final = cfinal.val;
		}
		else final = R6G6B6TORGB15(red,green,blue);
	}
	else
	{
		final = R6G6B6TORGB15(red,green,blue);
		//perform the special effect
		if(windowEffect)
			switch(FUNC) {
				case Increase: final = currentFadeInColors[final&0x7FFF]; break;
				case Decrease: final = currentFadeOutColors[final&0x7FFF]; break;
				case None: 
				case Blend:
					break;
			}
	}

	HostWriteWord(dst, passing, (final | 0x8000));
	bgPixels[x] = 0;
}


template<bool BACKDROP, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL bool GPU::_master_setFinalBGColor(u16 &color, const u32 x)
{
	//no further analysis for no special effects. on backdrops. just draw it.
	//Blend backdrop with what? This doesn't make sense
	if(BACKDROP && (FUNC == None || FUNC == Blend)) return true;

	bool windowEffect = true;

	if(WINDOW){
		bool windowDraw = false;
		renderline_checkWindows(x, windowDraw, windowEffect);

		//backdrop must always be drawn
		if(BACKDROP) windowDraw = true;

		//we never have anything more to do if the window rejected us
		if(!windowDraw) return false;
	}

	//special effects rejected. just draw it.
	if(!(blend1 && windowEffect))
		return true;

	const u8 bg_under = bgPixels[x];

	//perform the special effect
	switch(FUNC) {
		case Blend: if(blend2[bg_under]) color = blend(color,HostReadWord(currDst, x<<1)); break;
		case Increase: color = currentFadeInColors[color]; break;
		case Decrease: color = currentFadeOutColors[color]; break;
		case None: break;
	}
	return true;
}

template<BlendFunc FUNC, bool WINDOW>
static FORCEINLINE void _master_setFinalOBJColor(GPU *gpu, u8 *dst, u16 color, u8 alpha, u8 type, u16 x)
{
	bool windowDraw = true, windowEffect = true;

	if(WINDOW)
	{
		gpu->renderline_checkWindows(x, windowDraw, windowEffect);
		if(!windowDraw)
			return;
	}

	const bool sourceEffectSelected = gpu->blend1;

	//note that the fadein and fadeout is done here before blending, 
	//so that a fade and blending can be applied at the same time (actually, I don't think that is legal..)
	bool forceBlendingForNormal = false;
	if(windowEffect && sourceEffectSelected)
		switch(FUNC) 
		{
			//zero 13-jun-2010 : if(allowBlend) was removed from these;
			//it should be possible to increase/decrease and also blend
			//(the effect would be increase, but the obj properties permit blending and the target layers are configured correctly)

			case Increase: color = gpu->currentFadeInColors[color&0x7FFF]; break;
			case Decrease: color = gpu->currentFadeOutColors[color&0x7FFF]; break;

			//only when blend color effect is selected, ordinarily opaque sprites are blended with the color effect params
			case Blend: forceBlendingForNormal = true; break;
			case None: break;
		}

	//this inspects the layer beneath the sprite to see if the current blend flags make it a candidate for blending
	const int bg_under = gpu->bgPixels[x];
	const bool allowBlend = (bg_under != 4) && gpu->blend2[bg_under];

	if(allowBlend)
	{
		u16 backColor = HostReadWord(dst,x<<1);
		//this hasn't been tested: this blending occurs without regard to the color effect,
		//but rather purely from the sprite's alpha
		if(type == GPU_OBJ_MODE_Bitmap)
			color = _blend(color,backColor,&gpuBlendTable555[alpha+1][15-alpha]);
		else if(type == GPU_OBJ_MODE_Transparent || forceBlendingForNormal)
			color = gpu->blend(color,backColor);
	}

	HostWriteWord(dst, x<<1, (color | 0x8000));
	gpu->bgPixels[x] = 4;	
}

//FUNCNUM is only set for backdrop, for an optimization of looking it up early
template<bool BACKDROP, int FUNCNUM> 
FORCEINLINE void GPU::setFinalColorBG(u16 color, const u32 x)
{
	//It is not safe to assert this here.
	//This is probably the best place to enforce it, since almost every single color that comes in here
	//will be pulled from a palette that needs the top bit stripped off anyway.
	//assert((color&0x8000)==0);
	if(!BACKDROP) color &= 0x7FFF; //but for the backdrop we can easily guarantee earlier that theres no bit here

	bool draw = false;

	// FUNCNUM >= 0 folds to a compile-time constant here, so the switch below
	// collapses to a single case. FUNCNUM < 0 keeps the per-pixel dispatch.
	const int test = (FUNCNUM >= 0) ? FUNCNUM : setFinalColorBck_funcNum;
	switch(test)
	{
		case 0x0: draw = _master_setFinalBGColor<BACKDROP,None,false>(color,x); break;
		case 0x1: draw = _master_setFinalBGColor<BACKDROP,Blend,false>(color,x); break;
		case 0x2: draw = _master_setFinalBGColor<BACKDROP,Increase,false>(color,x); break;
		case 0x3: draw = _master_setFinalBGColor<BACKDROP,Decrease,false>(color,x); break;
		case 0x4: draw = _master_setFinalBGColor<BACKDROP,None,true>(color,x); break;
		case 0x5: draw = _master_setFinalBGColor<BACKDROP,Blend,true>(color,x); break;
		case 0x6: draw = _master_setFinalBGColor<BACKDROP,Increase,true>(color,x); break;
		case 0x7: draw = _master_setFinalBGColor<BACKDROP,Decrease,true>(color,x); break;
	};

	if(BACKDROP || draw) //backdrop must always be drawn
	{
		HostWriteWord(currDst, x<<1, color | 0x8000);
		if(!BACKDROP) bgPixels[x] = currBgNum; //Let's do this in the backdrop drawing loop, should be faster
	}
}


FORCEINLINE void GPU::setFinalColor3d(int dstX, int srcX)
{
	switch(setFinalColor3d_funcNum)
	{
		case 0x0: _master_setFinal3dColor<None,false>(dstX,srcX); break;
		case 0x1: _master_setFinal3dColor<Blend,false>(dstX,srcX); break;
		case 0x2: _master_setFinal3dColor<Increase,false>(dstX,srcX); break;
		case 0x3: _master_setFinal3dColor<Decrease,false>(dstX,srcX); break;
		case 0x4: _master_setFinal3dColor<None,true>(dstX,srcX); break;
		case 0x5: _master_setFinal3dColor<Blend,true>(dstX,srcX); break;
		case 0x6: _master_setFinal3dColor<Increase,true>(dstX,srcX); break;
		case 0x7: _master_setFinal3dColor<Decrease,true>(dstX,srcX); break;
	};
}

// setFinalColorSpr() used to live here: a per-sprite-pixel switch on
// setFinalColorSpr_funcNum selecting one of 8 _master_setFinalOBJColor<>
// instantiations. The sole caller (the sprite-composite loop in
// GPU_RenderLine_layer) now hoists that switch out of the loop itself
// (desmumewii-2d-compositor-plan.md Step 2), so this wrapper is gone.

template<bool MOSAIC, bool BACKDROP, int FUNCNUM>
FORCEINLINE void GPU::__setFinalColorBck(u16 color, const u32 x, const int opaque)
{
	return ___setFinalColorBck<MOSAIC, BACKDROP, FUNCNUM>(color,x,opaque);
}

//this was forced inline because most of the time it just falls through to setFinalColorBck() and the function call
//overhead was ridiculous and terrible
template<bool MOSAIC, bool BACKDROP, int FUNCNUM>
FORCEINLINE void GPU::___setFinalColorBck(u16 color, const u32 x, const int opaque)
{
	//under ordinary circumstances, nobody should pass in something >=256
	//but in fact, someone is going to try. specifically, that is the map viewer debug tools
	//which try to render the enter BG. in cases where that is large, it could be up to 1024 wide.
	assert(x<256);

	//due to this early out, we will get incorrect behavior in cases where 
	//we enable mosaic in the middle of a frame. this is deemed unlikely.
	if(!MOSAIC) {
		if(opaque){
			setFinalColorBG<BACKDROP,FUNCNUM>(color,x);
		}
		return;
	}

	if(!opaque)
		color = 0xFFFF;
	else 
		color &= 0x7FFF;

	//due to the early out, enabled must always be true
	//x_int = enabled ? GPU::mosaicLookup.width[x].trunc : x;
	int x_int = GPU::mosaicLookup.width[x].trunc;

	if(GPU::mosaicLookup.width[x].begin && GPU::mosaicLookup.height[currLine].begin) {}
	else color = mosaicColors.bg[currBgNum][x_int];
	mosaicColors.bg[currBgNum][x] = color;

	if(color != 0xFFFF){
		setFinalColorBG<BACKDROP,FUNCNUM>(color,x);
	}
}

//this is fantastically inaccurate.
//we do the early return even though it reduces the resulting accuracy
//because we need the speed, and because it is inaccurate anyway
static void mosaicSpriteLinePixel(GPU * gpu, int x, u16 l, u8 * dst, u8 * dst_alpha, u8 * typeTab, u8 * prioTab)
{
	int x_int;
	int y = l;

	OAM * spriteInfo = (OAM *)(gpu->oam + gpu->sprNum[x]);
	bool enabled = spriteInfo->Mosaic;
	if(!enabled)
		return;

	bool opaque = prioTab[x] <= 4;

	GPU::MosaicColor::Obj objColor;
	objColor.color = T1ReadWord(dst,x<<1);
	objColor.alpha = dst_alpha[x];
	objColor.opaque = opaque;

	//--DCN: If enabled is false, then we don't need to check
	// if it's true, since it would have returned earlier
	x_int = GPU::mosaicLookup.width[x].trunc;
	//x_int = enabled ? GPU::mosaicLookup.width[x].trunc;

	if(GPU::mosaicLookup.width[x].begin && GPU::mosaicLookup.height[y].begin) {}
	else objColor = gpu->mosaicColors.obj[x_int];

	gpu->mosaicColors.obj[x] = objColor;

	T1WriteWord(dst,x<<1,objColor.color);
	dst_alpha[x] = objColor.alpha;
	if(!objColor.opaque) prioTab[x] = 0xFF;
}

FORCEINLINE static void mosaicSpriteLine(GPU * gpu, u16 l, u8 * dst, u8 * dst_alpha, u8 * typeTab, u8 * prioTab)
{
	//Don't even try this unless the mosaic is effective
	if(gpu->mosaicLookup.widthValue != 0 || gpu->mosaicLookup.heightValue != 0)
		for(int i=0;i<256;i++)
			mosaicSpriteLinePixel(gpu,i,l,dst,dst_alpha,typeTab,prioTab);
}

template<bool MOSAIC> void lineLarge8bpp(GPU * gpu)
{
	if(gpu->core == 1) {
		PROGINFO("Cannot use large 8bpp screen on sub core\n");
		return;
	}

	u8 num = gpu->currBgNum;
	u16 XBG = gpu->getHOFS(gpu->currBgNum);
	u16 YBG = gpu->currLine + gpu->getVOFS(gpu->currBgNum);
	u16 lg     = gpu->BGSize[num][0];
	u16 ht     = gpu->BGSize[num][1];
	u16 wmask  = (lg-1);
	u16 hmask  = (ht-1);
	YBG &= hmask;

	//TODO - handle wrapping / out of bounds correctly from rot_scale_op?

	u32 tmp_map = gpu->BG_bmp_large_ram[num] + lg * YBG;
	u8* map = (u8*)MMU_gpu_map(tmp_map);

	u8* pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;

	for(int x = 0; x < lg; ++x, ++XBG)
	{
		XBG &= wmask;
		u8 pixel = map[XBG];
		u16 color = T1ReadWord(pal, pixel<<1);
		gpu->__setFinalColorBck<MOSAIC,false>(color,x,color); // dead code path (no caller); left on the legacy per-pixel dispatch
	}
	
}

/*****************************************************************************/
//			BACKGROUND RENDERING -TEXT-
/*****************************************************************************/
// render a text background to the combined pixelbuffer
template<bool MOSAIC, int FUNCNUM> INLINE void renderline_textBG(GPU * gpu, u16 XBG, u16 YBG, u16 LG)
{
	u8 num = gpu->currBgNum;
	struct _BGxCNT *bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[num].bits;
	struct _DISPCNT *dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	TILEENTRY tileentry;
	u32 map;
	u32 tile;
	u32 x      = 0;
	u32 xfin;
	u32 mapinfo;
	u8 *pal, *line;	
	u16 lg     = gpu->BGSize[num][0];
	u16 ht     = gpu->BGSize[num][1];
	u16 wmask  = (lg-1);
	u16 hmask  = (ht-1);
	u16 tmp    = ((YBG & hmask) >> 3);
	u16 xoff;
	u16 yoff;
	u16 color;	
	u32 tmp_map = gpu->BG_map_ram[num] + (tmp&31) * 64;


	s8 line_dir = 1;
	
	if(tmp>31) 
		tmp_map+= ADDRESS_STEP_512B << bgCnt->ScreenSize ;

	map = tmp_map;
	tile = gpu->BG_tile_ram[num];

	xoff = XBG;
	pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;

	if(!bgCnt->Palette_256)    // color: 16 palette entries
	{
		yoff = ((YBG&7)<<2);
		xfin = 8 - (xoff&7);

		for(x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
		{
			tmp = ((xoff&wmask)>>3);
			mapinfo = map + ((tmp&0x1F) << 1);
			if(tmp>31) mapinfo += 32*32*2;
			tileentry.val = T1ReadWord(MMU_gpu_map(mapinfo), 0);

			u16 tilePalette = (tileentry.bits.Palette << 4);

			line = (u8*)MMU_gpu_map(tile + (tileentry.bits.TileNum * 0x20) + ((tileentry.bits.VFlip) ? (7*4)-yoff : yoff));
			
			if(tileentry.bits.HFlip)
			{
				line += (3 - ((xoff&7)>>1));

				for(; x < xfin; --line) 
				{	
					u8 currLine = *line;

					if(!(xoff&1))
					{
						color = T1ReadWord(pal, ((currLine>>4) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,x,currLine>>4);
						++x; ++xoff;
					}
					
					if(x<xfin)
					{
						color = T1ReadWord(pal, ((currLine&0xF) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,x,currLine&0xF);
						++x; ++xoff;
					}
				}
			} else {
				line += ((xoff&7)>>1);
				
				for(; x < xfin; ++line) 
				{
					u8 currLine = *line;

					if(!(xoff&1))
					{
						color = T1ReadWord(pal, ((currLine&0xF) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,x,currLine&0xF);
						++x; ++xoff;
					}

					if(x<xfin)
					{
						color = T1ReadWord(pal, ((currLine>>4) + tilePalette) << 1);
						gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,x,currLine>>4);
						++x; ++xoff;
					}
				}
			}
		}
		return;
	}

	//256-color BG

	if(dispCnt->ExBGxPalette_Enable)  // color: extended palette
	{
		pal = MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[num]];
		if(!pal) return;
	}

	yoff = ((YBG&7)<<3);

	xfin = 8 - (xoff&7);
	u32 extPalMask = -dispCnt->ExBGxPalette_Enable;
	for(x = 0; x < LG; xfin = std::min<u16>(x+8, LG))
	{
		tmp = (xoff & (lg-1))>>3;
		mapinfo = map + ((tmp & 31) << 1);
		if(tmp > 31) mapinfo += 32*32*2;
		tileentry.val = T1ReadWord(MMU_gpu_map(mapinfo), 0);
		u8 *tilePal = pal + ((tileentry.bits.Palette<<9) & extPalMask);

		line = (u8*)MMU_gpu_map(tile + (tileentry.bits.TileNum*0x40) + ((tileentry.bits.VFlip) ? (7*8)-yoff : yoff));

		if(tileentry.bits.HFlip)
		{
			line += (7 - (xoff&7));
			line_dir = -1;
		} else {
			line += (xoff&7);
			line_dir = 1;
		}
		for(; x < xfin; )
		{
			color = T1ReadWord(tilePal, (*line) << 1);
			gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,x,*line);
			
			++x; ++xoff;

			line += line_dir;
		}
	}
}

//------------------------------------------------------------------------------
// GX 2D-BG compositor (Step 5.1a) - resolve one 8x8 tile cell of a MAIN text BG
// plane into 64 GX RGB5A3 texels (row-major, out[0] = top-left of the cell).
// out == 0x0000 means transparent (BG colour index 0); every opaque texel gets
// the RGB5A3 opaque bit via RGB15_REVERSE.  tx/ty are tile coords in the BG
// plane; the caller need not pre-mask them.  This mirrors renderline_textBG()
// exactly (screenblock wrap, H/V flip, 16- vs 256-colour, standard vs extended
// palette); GX2DBG_VERIFY builds cross-check the two against each other.
//------------------------------------------------------------------------------
#ifndef RGB15_REVERSE
#define RGB15_REVERSE(col) ( 0x8000 | (((col) & 0x001F) << 10) | ((col) & 0x03E0) | (((col) & 0x7C00) >> 10) )
#endif

void GPU_ResolveTextTile8x8(GPU *gpu, u8 num, u32 tx, u32 ty, u16 out[64])
{
	const struct _BGxCNT  *bgCnt   = &(gpu->dispx_st)->dispx_BGxCNT[num].bits;
	const struct _DISPCNT *dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	const u32 lg  = gpu->BGSize[num][0];       // plane width  in px (pow2)
	const u32 ht  = gpu->BGSize[num][1];       // plane height in px (pow2)
	const u32 lgt = lg >> 3;                   // plane width  in tiles
	const u32 htt = ht >> 3;                   // plane height in tiles

	const u32 mrow = ty & (htt - 1);           // == (YBG & (ht-1)) >> 3
	const u32 mcol = tx & (lgt - 1);

	u32 rowbase = gpu->BG_map_ram[num] + (mrow & 31) * 64;
	if (mrow > 31) rowbase += ADDRESS_STEP_512B << bgCnt->ScreenSize;

	u32 mapinfo = rowbase + ((mcol & 31) << 1);
	if (mcol > 31) mapinfo += 32 * 32 * 2;

	TILEENTRY te;
	te.val = T1ReadWord(MMU_gpu_map(mapinfo), 0);

	const u32 tileBase = gpu->BG_tile_ram[num];

	if (!bgCnt->Palette_256)
	{
		// 16-colour: 32 bytes/tile, low nibble = even pixel
		u8 *pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;
		const u16 palOfs = (te.bits.Palette << 4);
		for (u32 r = 0; r < 8; r++) {
			const u32 sr = te.bits.VFlip ? (7 - r) : r;
			const u8 *line = (const u8 *)MMU_gpu_map(tileBase + te.bits.TileNum * 0x20 + sr * 4);
			for (u32 c = 0; c < 8; c++) {
				const u32 sc = te.bits.HFlip ? (7 - c) : c;
				u8 b = line[sc >> 1];
				u8 idx = (sc & 1) ? (b >> 4) : (b & 0xF);
				out[r * 8 + c] = idx ? RGB15_REVERSE(T1ReadWord(pal, (idx + palOfs) << 1)) : 0;
			}
		}
		return;
	}

	// 256-colour: 64 bytes/tile, one byte per pixel
	u8 *pal;
	if (dispCnt->ExBGxPalette_Enable) {
		pal = MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[num]];
		if (!pal) { for (int i = 0; i < 64; i++) out[i] = 0; return; }
		pal += (te.bits.Palette << 9);
	} else {
		pal = MMU.ARM9_VMEM + gpu->core * ADDRESS_STEP_1KB;
	}
	for (u32 r = 0; r < 8; r++) {
		const u32 sr = te.bits.VFlip ? (7 - r) : r;
		const u8 *line = (const u8 *)MMU_gpu_map(tileBase + te.bits.TileNum * 0x40 + sr * 8);
		for (u32 c = 0; c < 8; c++) {
			const u32 sc = te.bits.HFlip ? (7 - c) : c;
			u8 idx = line[sc];
			out[r * 8 + c] = idx ? RGB15_REVERSE(T1ReadWord(pal, idx << 1)) : 0;
		}
	}
}

//------------------------------------------------------------------------------
// GX sprite compositor (Step 5.2) - resolve one tiled, standard-palette OBJ
// (non-affine OR affine) into a padded RGB5A3 texture (transparent GX2OBJ_MARGIN
// border so GX_CLAMP outside the sprite reads as transparent) plus its on-screen
// field rect and the 4 quad-corner UVs (TL,BL,BR,TR - into the *padded* texture,
// 0..1).  Non-affine flips fold into the UVs; affine uses the OAM 2x2 matrix.
// Returns 0 = skip, 1 = baked, -1 = not GX-bakeable (bitmap / OBJ-window /
// ext-pal-256 / mosaic) - the frame then composites all its sprites on the CPU.
//------------------------------------------------------------------------------
#define GX2OBJ_MARGIN 2   // keeps padded dims a multiple of 4 (sprite dims are 8/16/32/64)

int GPU_ResolveObjSprite(GPU *gpu, int oamIndex, u16 *out, int outCap,
                         int *fx, int *fy, int *ox, int *oy,
                         u8 *oprio, u8 *osemi, u32 *okey,
                         float uv[8], int *texW, int *texH)
{
	const struct _DISPCNT *dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	// local, endian-corrected copy of the 8-byte OAM entry (see _spriteRender)
	OAM oe = gpu->oam[oamIndex];
	{
		u16 *w = (u16 *)&oe;
#ifdef WORDS_BIGENDIAN
		w[1] = (u16)((w[1] >> 1) | (w[1] << 15));
		w[2] = (u16)((w[2] >> 2) | (w[2] << 14));
#endif
	}

	if (oe.RotScale == 2)     return 0;    // disabled
#ifdef DESMUME_BENCH
	extern u32 g_gx2objRej[2][6];  // [eng][0 objwin 1 bitmap 2 mosaic 3 extpal256 4 size 5 cap]
	u32 *rj = g_gx2objRej[gpu->core ? 1 : 0];
#endif
	if (oe.Mode == 2 || oe.Mode == 3) {
#ifdef DESMUME_BENCH
		rj[oe.Mode == 2 ? 0 : 1]++;
#endif
		return -1;   // OBJ window / bitmap
	}
	if (oe.Mosaic) {
#ifdef DESMUME_BENCH
		rj[2]++;
#endif
		return -1;
	}
	// ext-pal 256-colour sprite: bake against MMU.ObjExtPal (one 256-entry bank
	// selected by PaletteIndex), mirroring _spriteRender.  If the bank isn't
	// mapped, fall back.
	const bool extPal256 = (oe.Depth && dispCnt->ExOBJPalette_Enable);
	if (extPal256 && !MMU.ObjExtPal[gpu->core][0]) {
#ifdef DESMUME_BENCH
		rj[3]++;
#endif
		return -1;
	}

	const bool affine = (oe.RotScale & 1) != 0;
	const size sz = sprSizeTab[oe.Size][oe.Shape];
	const int W = sz.x, H = sz.y;
	if (W <= 0 || H <= 0) {
#ifdef DESMUME_BENCH
		rj[4]++;
#endif
		return -1;
	}

	const int PW = W + 2 * GX2OBJ_MARGIN;
	const int PH = H + 2 * GX2OBJ_MARGIN;
	if (PW * PH > outCap) {
#ifdef DESMUME_BENCH
		rj[5]++;
#endif
		return -1;
	}

	// field (on-screen footprint): sprite size, doubled in double-size mode
	int FX = W, FY = H;
	if (affine && (oe.RotScale & 2)) { FX <<= 1; FY <<= 1; }

	s32 sx = (oe.X << 23) >> 23;                 // sign-extend 9-bit X
	s32 sy = oe.Y;
	if (sy >= 192) sy = (s32)((s8)oe.Y);
	// wrap: DS wraps sprites at 256/512; approximate with the closer copy
	if (sx + FX <= 0)   sx += 512;
	if (sx >= 256)      sx -= 512;
	if (sx + FX <= 0 || sx >= 256 || sy + FY <= 0 || sy >= 192) return 0;   // off-screen

	*ox = sx; *oy = sy;
	*fx = FX; *fy = FY;
	*oprio  = oe.Priority;
	*osemi  = (oe.Mode == 1) ? 1 : 0;
	*texW = PW; *texH = PH;
	*okey = ((u32)oe.TileIndex) | ((u32)oe.PaletteIndex << 10) | ((u32)oe.Depth << 14)
	      | ((u32)oe.Size << 15) | ((u32)oe.Shape << 17) | ((u32)oe.Mode << 19)
	      | ((u32)(oe.RotScale & 1) << 21) | ((u32)extPal256 << 22);

	// --- UVs -----------------------------------------------------------------
	const float m = (float)GX2OBJ_MARGIN;
	if (!affine) {
		float u0 = m / PW,       u1 = (m + W) / PW;
		float v0 = m / PH,       v1 = (m + H) / PH;
		if (oe.HFlip) { float t = u0; u0 = u1; u1 = t; }
		if (oe.VFlip) { float t = v0; v0 = v1; v1 = t; }
		uv[0] = u0; uv[1] = v0;   // TL
		uv[2] = u0; uv[3] = v1;   // BL
		uv[4] = u1; uv[5] = v1;   // BR
		uv[6] = u1; uv[7] = v0;   // TR
	} else {
		const int bp = (oe.RotScalIndex + (oe.HFlip << 3) + (oe.VFlip << 4)) * 4;
		const s16 dx  = (s16)LE_TO_LOCAL_16(((u16 *)&gpu->oam[bp + 0])[3]);
		const s16 dmx = (s16)LE_TO_LOCAL_16(((u16 *)&gpu->oam[bp + 1])[3]);
		const s16 dy  = (s16)LE_TO_LOCAL_16(((u16 *)&gpu->oam[bp + 2])[3]);
		const s16 dmy = (s16)LE_TO_LOCAL_16(((u16 *)&gpu->oam[bp + 3])[3]);
		const float Cx = W * 128.0f, Cy = H * 128.0f;      // sprite centre, 8.8
		const float hx = FX * 0.5f,  hy = FY * 0.5f;
		// texX/texY at field-local (a,b): Cx + (a-hx)*dx + (b-hy)*dmx  (8.8)
		const int cx[4] = { 0, 0, FX, FX };
		const int cy[4] = { 0, FY, FY, 0 };
		for (int k = 0; k < 4; k++) {
			float tX = (Cx + (cx[k] - hx) * dx + (cy[k] - hy) * dmx) / 256.0f;
			float tY = (Cy + (cx[k] - hx) * dy + (cy[k] - hy) * dmy) / 256.0f;
			uv[k * 2 + 0] = (tX + m) / PW;
			uv[k * 2 + 1] = (tY + m) / PH;
		}
	}

	// --- bake the W*H sprite pixels into the padded buffer ------------------
	for (int i = 0; i < PW * PH; i++) out[i] = 0;   // transparent border

	const u32 base   = gpu->sprMem;
	const u8  block  = gpu->sprBoundary;
	const bool map2d = (gpu->spriteRenderMode == GPU::SPRITE_2D);
	const bool d256  = oe.Depth;
	const int  Wt    = W >> 3;
	// tile-base shift: 2D mapping uses <<5, 1D uses <<sprBoundary - except the
	// affine 256-colour path, which _spriteRender always addresses with <<block.
	const u32 tbase  = (map2d && !(affine && d256))
	                   ? (base + ((u32)oe.TileIndex << 5))
	                   : (base + ((u32)oe.TileIndex << block));
	u8 *palBase = MMU.ARM9_VMEM + 0x200 + gpu->core * ADDRESS_STEP_1KB;
	u8 *pal16   = palBase + (oe.PaletteIndex << 5);
	// d256 palette base: standard 256 bank, or the selected ObjExtPal bank.
	u8 *pal256  = extPal256 ? (MMU.ObjExtPal[gpu->core][0] + (u32)oe.PaletteIndex * 0x200)
	                        : palBase;

	for (int py = 0; py < H; py++) {
		const int ty = py >> 3, yin = py & 7;
		u16 *dstrow = out + (py + GX2OBJ_MARGIN) * PW + GX2OBJ_MARGIN;
		for (int px = 0; px < W; px++) {
			const int tx = px >> 3, xin = px & 7;
			u8 idx;
			if (d256) {
				u32 ofs = map2d ? ((u32)ty << 10) + (u32)tx * 64 + (u32)yin * 8 + xin
				                : (u32)ty * (u32)Wt * 64 + (u32)tx * 64 + (u32)yin * 8 + xin;
				idx = *(u8 *)MMU_gpu_map(tbase + ofs);
				dstrow[px] = idx ? RGB15_REVERSE(T1ReadWord(pal256, idx << 1)) : 0;
			} else {
				u32 ofs = map2d ? ((u32)ty << 10) + (u32)tx * 32 + (u32)yin * 4 + (xin >> 1)
				                : (u32)ty * (u32)Wt * 32 + (u32)tx * 32 + (u32)yin * 4 + (xin >> 1);
				u8 b = *(u8 *)MMU_gpu_map(tbase + ofs);
				idx = (xin & 1) ? (b >> 4) : (b & 0xF);
				dstrow[px] = idx ? RGB15_REVERSE(T1ReadWord(pal16, idx << 1)) : 0;
			}
		}
	}
	return 1;
}

/*****************************************************************************/
//			BACKGROUND RENDERING -ROTOSCALE-
/*****************************************************************************/

template<bool MOSAIC, int FUNCNUM> FORCEINLINE void rot_tiled_8bit_entry(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	u16 tileindex = *(u8*)MMU_gpu_map(map + ((auxX>>3) + (auxY>>3) * (lg>>3)));

	u16 x = (auxX&7); 
	u16 y = (auxY&7);

	u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileindex<<6)+(y<<3)+x));
	u16 color = T1ReadWord(pal, palette_entry << 1);
	//Log_fprintf("%s %d %d\n", __FUNCTION__, color, palette_entry);
	gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color,i,palette_entry);
}

template<bool MOSAIC, bool extPal, int FUNCNUM> FORCEINLINE void rot_tiled_16bit_entry(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	void* const map_addr = MMU_gpu_map(map + (((auxX>>3) + (auxY>>3) * (lg>>3))<<1));
	
	TILEENTRY tileentry;
	tileentry.val = T1ReadWord(map_addr, 0);

	const u16 x = ((tileentry.bits.HFlip) ? 7 - (auxX) : (auxX))&7;
	const u16 y = ((tileentry.bits.VFlip) ? 7 - (auxY) : (auxY))&7;

	const u8 palette_entry = *(u8*)MMU_gpu_map(tile + ((tileentry.bits.TileNum<<6)+(y<<3)+x));
	const u16 color = T1ReadWord(pal, (palette_entry + (extPal ? (tileentry.bits.Palette<<8) : 0)) << 1);
	gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color, i, palette_entry);
}

template<bool MOSAIC, int FUNCNUM> FORCEINLINE void rot_256_map(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	u8* adr = (u8*)MMU_gpu_map((map) + ((auxX + auxY * lg)));

	u8 palette_entry = *adr;
	u16 color = T1ReadWord(pal, palette_entry << 1);
	//Log_fprintf("%s %d %d\n", __FUNCTION__, color, palette_entry);
	gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color, i, palette_entry);
}

template<bool MOSAIC, int FUNCNUM> FORCEINLINE void rot_BMP_map(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i) {
	void* adr = MMU_gpu_map((map) + ((auxX + auxY * lg) << 1));
	u16 color = T1ReadWord(adr, 0);
	//Log_fprintf("%s %d\n", __FUNCTION__, color);
	gpu->__setFinalColorBck<MOSAIC,false,FUNCNUM>(color, i, color&0x8000);
}

typedef void (*rot_fun)(GPU * gpu, s32 auxX, s32 auxY, int lg, u32 map, u32 tile, u8 * pal, int i);

template<rot_fun fun, bool WRAP>
FORCEINLINE void rot_scale_op(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG, s32 wh, s32 ht, u32 map, u32 tile, u8 * pal)
{
	ROTOCOORD x, y;
	x.val = X;
	y.val = Y;

	const s32 dx = (s32)PA;
	const s32 dy = (s32)PC;

	// as an optimization, specially handle the fairly common case of
	// "unrotated + unscaled + no boundary checking required"
	if(dx==0x100 && dy==0)
	{
		s32 auxX = x.bits.Integer;
		s32 auxY = y.bits.Integer;
		if(WRAP || (auxX + LG < wh && auxX >= 0 && auxY < ht && auxY >= 0))
		{
			if(WRAP)
			{
				auxY = auxY & (ht-1);
				auxX = auxX & (wh-1);
			}
			for(int i = 0; i < LG; ++i)
			{
				fun(gpu, auxX, auxY, wh, map, tile, pal, i);
				auxX++;
				if(WRAP)
					auxX = auxX & (wh-1);
			}
			return;
		}
	}
	
	for(int i = 0; i < LG; ++i)
	{
		s32 auxX, auxY;
		auxX = x.bits.Integer;
		auxY = y.bits.Integer;
	
		if(WRAP)
		{
			auxX = auxX & (wh-1);
			auxY = auxY & (ht-1);
		}
		
		if(WRAP || ((auxX >= 0) && (auxX < wh) && (auxY >= 0) && (auxY < ht)))
			fun(gpu, auxX, auxY, wh, map, tile, pal, i);

		x.val += dx;
		y.val += dy;
	}
}

template<rot_fun fun>
FORCEINLINE void apply_rot_fun(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG, u32 map, u32 tile, u8 * pal)
{
	struct _BGxCNT * bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[gpu->currBgNum].bits;
	s32 wh = gpu->BGSize[gpu->currBgNum][0];
	s32 ht = gpu->BGSize[gpu->currBgNum][1];
	if(bgCnt->PaletteSet_Wrap)
		rot_scale_op<fun,true>(gpu, X, Y, PA, PB, PC, PD, LG, wh, ht, map, tile, pal);	
	else rot_scale_op<fun,false>(gpu, X, Y, PA, PB, PC, PD, LG, wh, ht, map, tile, pal);	
}


template<bool MOSAIC, int FUNCNUM> FORCEINLINE void rotBG2(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, u16 LG)
{
	u8 num = gpu->currBgNum;
	u8 * pal = MMU.ARM9_VMEM + gpu->core * 0x400;
//	printf("rot mode\n");
	apply_rot_fun<rot_tiled_8bit_entry<MOSAIC,FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
}

template<bool MOSAIC, int FUNCNUM> FORCEINLINE void extRotBG2(GPU * gpu, s32 X, s32 Y, s16 PA, s16 PB, s16 PC, s16 PD, s16 LG)
{
	u8 num = gpu->currBgNum;
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	
	u8 *pal;

	switch(gpu->BGTypes[num])
	{
	case BGType_AffineExt_256x16:
		if(dispCnt->ExBGxPalette_Enable)
			pal = MMU.ExtPal[gpu->core][gpu->BGExtPalSlot[num]];
		else
			pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		if (!pal) return;
		// 16  bit bgmap entries
		if(dispCnt->ExBGxPalette_Enable)
			apply_rot_fun<rot_tiled_16bit_entry<MOSAIC, true, FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
		else apply_rot_fun<rot_tiled_16bit_entry<MOSAIC, false, FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_map_ram[num], gpu->BG_tile_ram[num], pal);
		return;
	case BGType_AffineExt_256x1:
		// 256 colors
		pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		apply_rot_fun<rot_256_map<MOSAIC,FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_ram[num], 0, pal);
		return;
	case BGType_AffineExt_Direct:
		// direct colors / BMP
		apply_rot_fun<rot_BMP_map<MOSAIC,FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_ram[num], 0, NULL);
		return;
	case BGType_Large8bpp:
		// large screen 256 colors
		pal = MMU.ARM9_VMEM + gpu->core * 0x400;
		apply_rot_fun<rot_256_map<MOSAIC,FUNCNUM> >(gpu,X,Y,PA,PB,PC,PD,LG, gpu->BG_bmp_large_ram[num], 0, pal);
		return;
	default: break;
	}
}

/*****************************************************************************/
//			BACKGROUND RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

#if 0
static void lineNull(GPU * gpu)
{
}
#endif

template<bool MOSAIC, int FUNCNUM> void lineText(GPU * gpu)
{
//	if(gpu->debug)
//	{
//		const s32 wh = gpu->BGSize[gpu->currBgNum][0];
//		renderline_textBG<MOSAIC>(gpu, 0, gpu->currLine, wh);
//	}
//	else
//	{
		const u16 vofs = gpu->getVOFS(gpu->currBgNum);
		const u16 hofs = gpu->getHOFS(gpu->currBgNum);
		renderline_textBG<MOSAIC,FUNCNUM>(gpu, hofs, gpu->currLine + vofs, 256);
//	}
}

template<bool MOSAIC, int FUNCNUM> void lineRot(GPU * gpu)
{
	BGxPARMS * parms;
	if (gpu->currBgNum==2) {
		parms = &(gpu->dispx_st)->dispx_BG2PARMS;
	} else {
		parms = &(gpu->dispx_st)->dispx_BG3PARMS;		
	}

//	if(gpu->debug)
//	{
//		s32 wh = gpu->BGSize[gpu->currBgNum][0];
//		rotBG2<MOSAIC>(gpu, 0, (s16)gpu->currLine*256, 256,0, 0,-77, wh);
//	}
//	else
//	{

		 rotBG2<MOSAIC,FUNCNUM>(gpu,
			parms->BGxX,
			parms->BGxY,
			LE_TO_LOCAL_16(parms->BGxPA),
			LE_TO_LOCAL_16(parms->BGxPB),
			LE_TO_LOCAL_16(parms->BGxPC),
			LE_TO_LOCAL_16(parms->BGxPD),
			256);

		parms->BGxX += LE_TO_LOCAL_16(parms->BGxPB);
		parms->BGxY += LE_TO_LOCAL_16(parms->BGxPD);
//	}
}

template<bool MOSAIC, int FUNCNUM> void lineExtRot(GPU * gpu)
{
	BGxPARMS * parms;
	if (gpu->currBgNum==2) {
		parms = &(gpu->dispx_st)->dispx_BG2PARMS;
	} else {
		parms = &(gpu->dispx_st)->dispx_BG3PARMS;		
	}

//	if(gpu->debug)
//	{
//		s32 wh = gpu->BGSize[gpu->currBgNum][0];
//		extRotBG2<MOSAIC>(gpu, 0, (s16)gpu->currLine*256, 256,0, 0,-77, wh);
//	}
//	else
//	{
		extRotBG2<MOSAIC,FUNCNUM>(gpu,
			parms->BGxX,
			parms->BGxY,
			LE_TO_LOCAL_16(parms->BGxPA),
			LE_TO_LOCAL_16(parms->BGxPB),
			LE_TO_LOCAL_16(parms->BGxPC),
			LE_TO_LOCAL_16(parms->BGxPD),
			256);

		parms->BGxX += LE_TO_LOCAL_16(parms->BGxPB);
		parms->BGxY += LE_TO_LOCAL_16(parms->BGxPD);
//	}
}

/*****************************************************************************/
//			SPRITE RENDERING -HELPER FUNCTIONS-
/*****************************************************************************/

#define nbShow 128

/* if i understand it correct, and it fixes some sprite problems in chameleon shot */
/* we have a 15 bit color, and should use the pal entry bits as alpha ?*/
/* http://nocash.emubase.de/gbatek.htm#dsvideoobjs */
INLINE void render_sprite_BMP (GPU * gpu, u8 spriteNum, u16 l, u8 * dst, u16 * src, u8 * dst_alpha, u8 * typeTab, u8 * prioTab, 
							   u8 prio, int lg, int sprX, int x, int xdir, u8 alpha) 
{
	int i; u16 color;
	for(i = 0; i < lg; i++, ++sprX, x+=xdir)
	{
		color = LE_TO_LOCAL_16(src[x]);

		// alpha bit = invisible
		if ((color&0x8000)&&(prio<=prioTab[sprX]))
		{
			HostWriteWord(dst, (sprX<<1), color);
			dst_alpha[sprX] = alpha;
			typeTab[sprX] = 3;
			prioTab[sprX] = prio;
			gpu->sprNum[sprX] = spriteNum;
		}
	}
}

INLINE void render_sprite_256 (	GPU * gpu, u8 spriteNum, u16 l, u8 * dst, u8 * src, u16 * pal, 
								u8 * dst_alpha, u8 * typeTab, u8 * prioTab, u8 prio, int lg, int sprX, int x, int xdir, u8 alpha)
{
	int i; 
	u8 palette_entry; 
	u16 color;

	for(i = 0; i < lg; i++, ++sprX, x+=xdir)
	{
		palette_entry = src[(x&0x7) + ((x&0xFFF8)<<3)];
		color = LE_TO_LOCAL_16(pal[palette_entry]);

		// palette entry = 0 means backdrop
		if ((palette_entry>0)&&(prio<=prioTab[sprX]))
		{
			HostWriteWord(dst, (sprX<<1), color);
			dst_alpha[sprX] = 16;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
			gpu->sprNum[sprX] = spriteNum;
		}
	}
}

INLINE void render_sprite_16 (	GPU * gpu, u16 l, u8 * dst, u8 * src, u16 * pal, 
								u8 * dst_alpha, u8 * typeTab, u8 * prioTab, u8 prio, int lg, int sprX, int x, int xdir, u8 alpha)
{
	int i; 
	u8 palette, palette_entry;
	u16 color, x1;

	for(i = 0; i < lg; i++, ++sprX, x+=xdir)
	{
		x1 = x>>1;
		palette = src[(x1&0x3) + ((x1&0xFFFC)<<3)];
		if (x & 1) palette_entry = palette >> 4;
		else       palette_entry = palette & 0xF;
		color = LE_TO_LOCAL_16(pal[palette_entry]);

		// palette entry = 0 means backdrop
		if ((palette_entry>0)&&(prio<=prioTab[sprX]))
		{
			HostWriteWord(dst, (sprX<<1), color);
			dst_alpha[sprX] = 16;
			typeTab[sprX] = (alpha ? 1 : 0);
			prioTab[sprX] = prio;
		}
	}
}

INLINE void render_sprite_Win (GPU * gpu, u16 l, u8 * src,
	int col256, int lg, int sprX, int x, int xdir) {
	int i; u8 palette, palette_entry;
	u16 x1;
	if (col256) {
		for(i = 0; i < lg; i++, sprX++,x+=xdir)
			//sprWin[sprX] = (src[x])?1:0;
			if(src[(x&7) + ((x&0xFFF8)<<3)]) 
				sprWin[sprX] = 1;
	} else {
		for(i = 0; i < lg; i++, ++sprX, x+=xdir)
		{
			x1 = x>>1;
			palette = src[(x1&0x3) + ((x1&0xFFFC)<<3)];
			if (x & 1) palette_entry = palette >> 4;
			else       palette_entry = palette & 0xF;
			//sprWin[sprX] = (palette_entry)?1:0;
			if(palette_entry)
				sprWin[sprX] = 1;
		}
	}
}

// return val means if the sprite is to be drawn or not
FORCEINLINE BOOL compute_sprite_vars(OAM * spriteInfo, u16 l, 
	size &sprSize, s32 &sprX, s32 &sprY, s32 &x, s32 &y, s32 &lg, int &xdir) {

	x = 0;
	// get sprite location and size
	sprX = (spriteInfo->X/*<<23*/)/*>>23*/;
	sprY = spriteInfo->Y;
	sprSize = sprSizeTab[spriteInfo->Size][spriteInfo->Shape];

	lg = sprSize.x;
	
	if (sprY>=192)
		sprY = (s32)((s8)(spriteInfo->Y));
	
// FIXME: for rot/scale, a list of entries into the sprite should be maintained,
// that tells us where the first pixel of a screenline starts in the sprite,
// and how a step to the right in a screenline translates within the sprite

	//this wasn't really tested by anything. very unlikely to get triggered
	y = (l - sprY)&255;                        /* get the y line within sprite coords */
	if(y >= sprSize.y)
		return FALSE;

	if((sprX==256)||(sprX+sprSize.x<=0))	/* sprite pixels outside of line */
		return FALSE;				/* not to be drawn */

	// sprite portion out of the screen (LEFT)
	if(sprX<0)
	{
		lg += sprX;	
		x = -(sprX);
		sprX = 0;
	}
	// sprite portion out of the screen (RIGHT)
	if (sprX+sprSize.x >= 256)
		lg = 256 - sprX;

	// switch TOP<-->BOTTOM
	if (spriteInfo->VFlip)
		y = sprSize.y - y -1;
	
	// switch LEFT<-->RIGHT
	if (spriteInfo->HFlip) {
		x = sprSize.x - x -1;
		xdir  = -1;
	} else {
		xdir  = 1;
	}
	return TRUE;
}

/*****************************************************************************/
//			SPRITE RENDERING
/*****************************************************************************/


//TODO - refactor this so there isnt as much duped code between rotozoomed and non-rotozoomed versions

static u8* bmp_sprite_address(GPU* gpu, OAM * spriteInfo, size sprSize, s32 y)
{
	u8* src = 0;
	if (spriteInfo->Mode == 3) //sprite is in BMP format
	{
		if (gpu->dispCnt().OBJ_BMP_mapping)
		{
			//tested by buffy sacrifice damage blood splatters in corner
			src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<gpu->sprBMPBoundary) + (y*sprSize.x*2));
		}
		else
		{
			//2d mapping:
			//verified in rotozoomed mode by knights in the nightmare intro

			if (gpu->dispCnt().OBJ_BMP_2D_dim)
				//256*256, verified by heroes of mana FMV intro
				src = (u8 *)MMU_gpu_map(gpu->sprMem + (((spriteInfo->TileIndex&0x3E0) * 64  + (spriteInfo->TileIndex&0x1F) *8 + ( y << 8)) << 1));
			else 
				//128*512, verified by harry potter and the order of the phoenix conversation portraits
				src = (u8 *)MMU_gpu_map(gpu->sprMem + (((spriteInfo->TileIndex&0x3F0) * 64  + (spriteInfo->TileIndex&0x0F) *8 + ( y << 7)) << 1));
		}
	}

	return src;
}

template<GPU::SpriteRenderMode MODE>
void GPU::_spriteRender(u8 * dst, u8 * dst_alpha, u8 * typeTab, u8 * prioTab)
{
	u16 l = currLine;
	GPU *gpu = this;

	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	OAM* spriteInfo = (OAM *)(gpu->oam + (nbShow-1));// + 127;
	u8 block = gpu->sprBoundary;
	u8 i;

#ifdef WORDS_BIGENDIAN
	*(((u16*)spriteInfo)+1) = (*(((u16*)spriteInfo)+1) >> 1) | *(((u16*)spriteInfo)+1) << 15;
	*(((u16*)spriteInfo)+2) = (*(((u16*)spriteInfo)+2) >> 2) | *(((u16*)spriteInfo)+2) << 14;
#endif

	for(i = 0; i<nbShow; ++i, --spriteInfo
#ifdef WORDS_BIGENDIAN    
	,*(((u16*)(spriteInfo+1))+1) = (*(((u16*)(spriteInfo+1))+1) << 1) | *(((u16*)(spriteInfo+1))+1) >> 15
	,*(((u16*)(spriteInfo+1))+2) = (*(((u16*)(spriteInfo+1))+2) << 2) | *(((u16*)(spriteInfo+1))+2) >> 14
	,*(((u16*)spriteInfo)+1) = (*(((u16*)spriteInfo)+1) >> 1) | *(((u16*)spriteInfo)+1) << 15
	,*(((u16*)spriteInfo)+2) = (*(((u16*)spriteInfo)+2) >> 2) | *(((u16*)spriteInfo)+2) << 14
#endif
	)     
	{
		//for each sprite:

		// Check if sprite is disabled before everything
		if (spriteInfo->RotScale == 2)
			continue;

		size sprSize;
		s32 sprX, sprY, x, y, lg;
		int xdir;
		u8* src;
		u16 j;
		u8 prio = spriteInfo->Priority;


		if (spriteInfo->RotScale & 1) 
		{
			s32		fieldX, fieldY, auxX, auxY, realX, realY, offset;
			u8		blockparameter, *pal;
			u16		color;

			// Get sprite positions and size
			sprX = (spriteInfo->X<<23)>>23;
			sprY = spriteInfo->Y;
			sprSize = sprSizeTab[spriteInfo->Size][spriteInfo->Shape];

			lg = sprSize.x;
			
			if (sprY>=192)
				sprY = (s32)((s8)(spriteInfo->Y));

			// Copy sprite size, to check change it if needed
			fieldX = sprSize.x;
			fieldY = sprSize.y;

			// If we are using double size mode, double our control vars
			if (spriteInfo->RotScale & 2)
			{
				fieldX <<= 1;
				fieldY <<= 1;
				lg <<= 1;
			}

			// Check if the sprite is visible y-wise. unfortunately our logic for x and y is different due to our scanline based rendering
			//tested thoroughly by many large sprites in Super Robot Wars K which wrap around the screen
			y = (l - sprY)&255;
			if(y >= fieldY)
				continue;

			// Check if sprite is visible x-wise.
			if((sprX==256) || (sprX+fieldX<=0))
				continue;


			// Get which four parameter block is assigned to this sprite
			blockparameter = (spriteInfo->RotScalIndex + (spriteInfo->HFlip<< 3) + (spriteInfo->VFlip << 4))*4;

			// Get rotation/scale parameters
			s16 dx  = LE_TO_LOCAL_16((s16)(gpu->oam + blockparameter+0)->attr3);
			s16 dmx = LE_TO_LOCAL_16((s16)(gpu->oam + blockparameter+1)->attr3);
			s16 dy  = LE_TO_LOCAL_16((s16)(gpu->oam + blockparameter+2)->attr3);
			s16 dmy = LE_TO_LOCAL_16((s16)(gpu->oam + blockparameter+3)->attr3);

			// Calculate fixed point 8.8 start offsets
			realX = ((sprSize.x) << 7) - (fieldX >> 1)*dx - (fieldY>>1)*dmx + y * dmx;
			realY = ((sprSize.y) << 7) - (fieldX >> 1)*dy - (fieldY>>1)*dmy + y * dmy;
			
			if(sprX<0)
			{
				// If sprite is not in the window
				if(sprX + fieldX <= 0)
					continue;

				// Otherwise, is partially visible
				lg += sprX;
				realX -= sprX*dx;
				realY -= sprX*dy;
				sprX = 0;
			}
			else
			{
				if(sprX+fieldX>256)
					lg = 256 - sprX;
			}

			// If we are using 1 palette of 256 colors
			if(spriteInfo->Depth)
			{
				src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex << block));

				// If extended palettes are set, use them
				if (dispCnt->ExOBJPalette_Enable)
					pal = (MMU.ObjExtPal[gpu->core][0]+(spriteInfo->PaletteIndex*0x200));
				else
					pal = (MMU.ARM9_VMEM + 0x200 + gpu->core *0x400);
				
				for(j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX>>8);
					auxY = (realY>>8);
					
					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(MODE == SPRITE_2D)
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*8);
						else
							offset = (auxX&0x7) + ((auxX&0xFFF8)<<3) + ((auxY>>3)*sprSize.x*8) + ((auxY&0x7)*8);

						color = src[offset];

						if (color && (prioTab[sprX]>=prio))
						{ 
							HostWriteWord(dst, (sprX<<1), LE_TO_LOCAL_16(HostReadWord(pal, (color<<1))));
							dst_alpha[sprX] = 16;
							typeTab[sprX] = spriteInfo->Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
			// Rotozoomed direct color
			else if(spriteInfo->Mode == 3)
			{
				// Transparent (I think, don't bother to render?) if alpha is 0
				if(spriteInfo->PaletteIndex == 0)
					continue;

				src = bmp_sprite_address(this,spriteInfo,sprSize,0);

				for(j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX>>8);
					auxY = (realY>>8);

					//this is all very slow, and so much dup code with other rotozoomed modes.
					//dont bother fixing speed until this whole thing gets reworked

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(dispCnt->OBJ_BMP_2D_dim)
							//tested by knights in the nightmare
							offset = (bmp_sprite_address(this,spriteInfo,sprSize,auxY)-src)/2+auxX;
						else //tested by lego indiana jones (somehow?)
							//tested by buffy sacrifice damage blood splatters in corner
							offset = auxX + (auxY*sprSize.x);


						color = T1ReadWord (src, offset<<1);
						
						if((color&0x8000) && (prioTab[sprX]>=prio))
						{
							HostWriteWord(dst, (sprX<<1), color);
							dst_alpha[sprX] = spriteInfo->PaletteIndex;
							typeTab[sprX] = spriteInfo->Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
			// Rotozoomed 16/16 palette
			else
			{
				if(MODE == SPRITE_2D)
				{
					src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<5));
					pal = MMU.ARM9_VMEM + 0x200 + (gpu->core*0x400 + (spriteInfo->PaletteIndex*32));
				}
				else
				{
					src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<gpu->sprBoundary));
					pal = MMU.ARM9_VMEM + 0x200 + gpu->core*0x400 + (spriteInfo->PaletteIndex*32);
				}

				for(j = 0; j < lg; ++j, ++sprX)
				{
					// Get the integer part of the fixed point 8.8, and check if it lies inside the sprite data
					auxX = (realX>>8);
					auxY = (realY>>8);

					if (prioTab[sprX]< prio) continue;

					if (auxX >= 0 && auxY >= 0 && auxX < sprSize.x && auxY < sprSize.y)
					{
						if(MODE == SPRITE_2D)
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)<<10) + ((auxY&0x7)*4);
						else
							offset = ((auxX>>1)&0x3) + (((auxX>>1)&0xFFFC)<<3) + ((auxY>>3)*sprSize.x)*4 + ((auxY&0x7)*4);
						
						color = src[offset];

						// Get 4bits value from the readed 8bits
						if (auxX&1)	color >>= 4;
						else		color &= 0xF;

						if(color && (prioTab[sprX]>=prio))
						{
							HostWriteWord(dst, (sprX<<1), LE_TO_LOCAL_16(HostReadWord(pal, color << 1)));
							dst_alpha[sprX] = 16;
							typeTab[sprX] = spriteInfo->Mode;
							prioTab[sprX] = prio;
						}
					}

					//  Add the rotation/scale coeficients, here the rotation/scaling
					// is performed
					realX += dx;
					realY += dy;
				}

				continue;
			}
		}
		else //NOT rotozoomed
		{
	
			if (!compute_sprite_vars(spriteInfo, l, sprSize, sprX, sprY, x, y, lg, xdir))
				continue;

			if (spriteInfo->Mode == 2)
			{
				if(MODE == SPRITE_2D)
				{
					if (spriteInfo->Depth)
						src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4));
				}
				else
				{
					if (spriteInfo->Depth)
						src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8));
					else
						src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4));
				}

				render_sprite_Win (gpu, l, src, spriteInfo->Depth, lg, sprX, x, xdir);
				continue;
			}

			if (spriteInfo->Mode == 3)              //sprite is in BMP format
			{
				src = bmp_sprite_address(this,spriteInfo,sprSize, y);

				//transparent (i think, dont bother to render?) if alpha is 0
				if(spriteInfo->PaletteIndex == 0)
					continue;
				
				render_sprite_BMP (gpu, i, l, dst, (u16*)src, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->PaletteIndex);
				continue;
			}
			
			u16* pal;
			
			if(spriteInfo->Depth) //256 colors
			{
				if(MODE == SPRITE_2D)
					src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*8));
				else
					src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*8) + ((y&0x7)*8));
				
				if (dispCnt->ExOBJPalette_Enable)
					pal = (u16*)(MMU.ObjExtPal[gpu->core][0]+(spriteInfo->PaletteIndex*0x200));
				else
					pal = (u16*)(MMU.ARM9_VMEM + 0x200 + gpu->core *0x400);
		
				render_sprite_256 (gpu, i, l, dst, src, pal, 
					dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->Mode == 1);

				continue;
			}
			// 16 colors 
			if(MODE == SPRITE_2D)
			{
				src = (u8 *)MMU_gpu_map(gpu->sprMem + ((spriteInfo->TileIndex)<<5) + ((y>>3)<<10) + ((y&0x7)*4));
			}
			else
			{
				src = (u8 *)MMU_gpu_map(gpu->sprMem + (spriteInfo->TileIndex<<block) + ((y>>3)*sprSize.x*4) + ((y&0x7)*4));
			}
				
			pal = (u16*)(MMU.ARM9_VMEM + 0x200 + gpu->core * 0x400);
			
			pal += (spriteInfo->PaletteIndex<<4);
			
			render_sprite_16 (gpu, l, dst, src, pal, dst_alpha, typeTab, prioTab, prio, lg, sprX, x, xdir, spriteInfo->Mode == 1);
		}
	}

#ifdef WORDS_BIGENDIAN
	*(((u16*)spriteInfo)+1) = (*(((u16*)spriteInfo)+1) << 1) | *(((u16*)spriteInfo)+1) >> 15;
	*(((u16*)spriteInfo)+2) = (*(((u16*)spriteInfo)+2) << 2) | *(((u16*)spriteInfo)+2) >> 14;
#endif
}


/*****************************************************************************/
//			SCREEN FUNCTIONS
/*****************************************************************************/

int Screen_Init(int coreid)
{
	MainScreen.gpu = GPU_Init(0);
	SubScreen.gpu = GPU_Init(1);

	memset(GPU_screen, 0, sizeof(GPU_screen));
	//				width*height* two screens
	for(int i = 0; i < (256*192*2); i++)
		((u16*)GPU_screen)[i] = 0x7FFF;
	disp_fifo.head = disp_fifo.tail = 0;

	//if (osd)  {delete osd; osd =NULL; }
	//osd  = new OSDCLASS(-1);

	return GPU_ChangeGraphicsCore(coreid);
}

void Screen_Reset(void)
{
	GPU_Reset(MainScreen.gpu, 0);
	GPU_Reset(SubScreen.gpu, 1);

	memset(GPU_screen, 0, sizeof(GPU_screen));
	for(int i = 0; i < (256*192*2); i++)
		((u16*)GPU_screen)[i] = 0x7FFF;

	disp_fifo.head = disp_fifo.tail = 0;
	//osd->clear();
}

void Screen_DeInit(void)
{
	GPU_DeInit(MainScreen.gpu);
	GPU_DeInit(SubScreen.gpu);

	if (GFXCore)
		GFXCore->DeInit();

	//if (osd)  {delete osd; osd =NULL; }
}

/*****************************************************************************/
//			GRAPHICS CORE
/*****************************************************************************/

// This is for future graphics core switching. This is by no means set in stone

int GPU_ChangeGraphicsCore(int coreid)
{
   int i;

   // Make sure the old core is freed
   if (GFXCore)
      GFXCore->DeInit();

   // So which core do we want?
   if (coreid == GFXCORE_DEFAULT)
      coreid = 0; // Assume we want the first one

   // Go through core list and find the id
   for (i = 0; GFXCoreList[i] != NULL; i++)
   {
      if (GFXCoreList[i]->id == coreid)
      {
         // Set to current core
         GFXCore = GFXCoreList[i];
         break;
      }
   }

   if (GFXCore == NULL)
   {
      GFXCore = &GFXDummy;
      return -1;
   }

   if (GFXCore->Init() == -1)
   {
      // Since it failed, instead of it being fatal, we'll just use the dummy
      // core instead
      GFXCore = &GFXDummy;
   }

   return 0;
}

int GFXDummyInit();
void GFXDummyDeInit();
void GFXDummyResize(int width, int height, BOOL fullscreen);
void GFXDummyOnScreenText(char *string, ...);

GraphicsInterface_struct GFXDummy = {
	GFXCORE_DUMMY,
	"Dummy Graphics Interface",
	0,
	GFXDummyInit,
	GFXDummyDeInit,
	GFXDummyResize,
	GFXDummyOnScreenText
};

int GFXDummyInit()
{
   return 0;
}

void GFXDummyDeInit()
{
}

void GFXDummyResize(int width, int height, BOOL fullscreen)
{
}

void GFXDummyOnScreenText(char *string, ...)
{
}


/*****************************************************************************/
//			GPU_RenderLine
/*****************************************************************************/

void GPU_set_DISPCAPCNT(u32 val)
{
	GPU * gpu = MainScreen.gpu;	
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;

	gpu->dispCapCnt.val = val;
	gpu->dispCapCnt.EVA = std::min((u32)16, (val & 0x1F));
	gpu->dispCapCnt.EVB = std::min((u32)16, ((val >> 8) & 0x1F));
	gpu->dispCapCnt.writeBlock =  (val >> 16) & 0x03;
	gpu->dispCapCnt.writeOffset = (val >> 18) & 0x03;
	gpu->dispCapCnt.readBlock = dispCnt->VRAM_Block;

	if (dispCnt->DisplayMode == 2)
		gpu->dispCapCnt.readOffset = 0;
	else
		gpu->dispCapCnt.readOffset = (val >> 26) & 0x03;
	
	gpu->dispCapCnt.srcA = (val >> 24) & 0x01;
	gpu->dispCapCnt.srcB = (val >> 25) & 0x01;
	gpu->dispCapCnt.capSrc = (val >> 29) & 0x03;

	switch((val >> 20) & 0x03)
	{
		case 0:
			gpu->dispCapCnt.capx = DISPCAPCNT::_128;
			gpu->dispCapCnt.capy = 128;
			break;
		case 1:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 64;
			break;
		case 2:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 128;
			break;
		case 3:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 192;
			break;
	}

	GPUDBG("[dispcapcnt] val=0x%08X EVA=%d EVB=%d wBlock=%d wOff=%d capx=%d capy=%d rBlock=%d rOff=%d capSrc=%d srcA=%d srcB=%d mainDisplayMode=%d mainVramBlock=%d mainBG0_3D=%d\n",
			(unsigned)val, gpu->dispCapCnt.EVA, gpu->dispCapCnt.EVB, gpu->dispCapCnt.writeBlock, gpu->dispCapCnt.writeOffset,
			gpu->dispCapCnt.capx, gpu->dispCapCnt.capy, gpu->dispCapCnt.readBlock, gpu->dispCapCnt.readOffset,
			gpu->dispCapCnt.capSrc, gpu->dispCapCnt.srcA, gpu->dispCapCnt.srcB,
			dispCnt->DisplayMode, dispCnt->VRAM_Block, dispCnt->BG0_3D);

	/*INFO("Capture 0x%X:\n EVA=%i, EVB=%i, wBlock=%i, wOffset=%i, capX=%i, capY=%i\n rBlock=%i, rOffset=%i, srcCap=%i, dst=0x%X, src=0x%X\n srcA=%i, srcB=%i\n\n",
			val, gpu->dispCapCnt.EVA, gpu->dispCapCnt.EVB, gpu->dispCapCnt.writeBlock, gpu->dispCapCnt.writeOffset,
			gpu->dispCapCnt.capx, gpu->dispCapCnt.capy, gpu->dispCapCnt.readBlock, gpu->dispCapCnt.readOffset,
			gpu->dispCapCnt.capSrc, gpu->dispCapCnt.dst - MMU.ARM9_LCD, gpu->dispCapCnt.src - MMU.ARM9_LCD,
			gpu->dispCapCnt.srcA, gpu->dispCapCnt.srcB);*/
}

static void GPU_RenderLine_layer(NDS_Screen * screen, u16 l)
{
	CACHE_ALIGN u8 spr[512];
	CACHE_ALIGN u8 sprAlpha[256];
	CACHE_ALIGN u8 sprType[256];
	CACHE_ALIGN u8 sprPrio[256];

	GPU * gpu = screen->gpu;
	struct _DISPCNT * dispCnt = &(gpu->dispx_st)->dispx_DISPCNT.bits;
	itemsForPriority_t * item;
	u16 i16;
	BOOL BG_enabled  = TRUE;

	gpu->currentFadeInColors = &fadeInColors[gpu->BLDY_EVY][0];
	gpu->currentFadeOutColors = &fadeOutColors[gpu->BLDY_EVY][0];

	u16 backdrop_color = LE_TO_LOCAL_16(T1ReadWord(MMU.ARM9_VMEM, gpu->core * 0x400) & 0x7FFF);

	// Step 5.1a: if this whole scanline's 2D composite is GX-expressible - MAIN,
	// every enabled BG a baked text layer, no sprites / window / mosaic / BLDCNT
	// effect - record it for the GX band replay and skip the CPU walk.  A
	// BG0-as-3D line is handled too when the existing GXMerge_LineMergeable
	// accepts the 3D part (opaque, no front translucent BG): the resident 3D
	// texture is drawn as a KIND_3D entry at BG0's priority slot.
	{
	const int gxeng = (gpu->core == GPU_MAIN) ? 0 : 1;
	// BLDCNT colour-effect handling (§5.1a blend): a non-zero
	// setFinalColorBck_funcNum only forces the CPU path when the effect actually
	// changes a *recorded* pixel.  The per-pixel templates
	// (GPU::_master_setFinalBGColor) are a straight passthrough for any BG whose
	// 1st-target bit is clear, and a Blend on the backdrop is a no-op; the 3D
	// layer's own blend is expressed by the KIND_3D sandwich path
	// (GXMerge_LineMergeable) exactly as before.  So the recorder now accepts a
	// blended line as long as no enabled BG is a blend 1st target, OBJ isn't in
	// the blend while sprites are on, and (for Increase/Decrease) the backdrop
	// tint is folded into the recorded backdrop colour here.  funcNum >= 4
	// (a window in the DISPCNT config) still falls back.
	const u8  s5_bm   = (gpu->BLDCNT >> 6) & 3;
	const u16 s5_t1st = gpu->BLDCNT & 0x3F;              // BG0..3, OBJ(4), BD(5)
	bool s5_blendOK = (s5_bm == 0);
	if (!s5_blendOK) {
		s5_blendOK = true;
		for (int bg = 0; bg < 4; bg++)
			if (gpu->LayersEnable[bg] && (s5_t1st & (1 << bg))) s5_blendOK = false;
		if ((s5_t1st & 0x10) && gpu->LayersEnable[4]) s5_blendOK = false;   // OBJ 1st tgt
		if (s5_bm == 1 && (s5_t1st & 0x20)) { /* Blend on backdrop: no-op */ }
	}
#ifdef DESMUME_BENCH
	// Step 5 diagnostic: per-scanline tally of why the 2D-BG recorder bails.
	extern u32 g_gx2dbgBail[2][8];
	if (!GXMerge_2DBGLineArmed(gxeng))                                  g_gx2dbgBail[gxeng & 1][1]++;
	else if (gpu->WIN0_ENABLED || gpu->WIN1_ENABLED || gpu->WINOBJ_ENABLED) g_gx2dbgBail[gxeng & 1][3]++;
	else if (gpu->setFinalColorBck_funcNum >= 4)                       g_gx2dbgBail[gxeng & 1][4]++;  // window-in-config
	else if (!s5_blendOK)                                              g_gx2dbgBail[gxeng & 1][5]++;  // BG/OBJ blend 1st-target
	else if (gpu->LayersEnable[4] && !GX2DBG_ObjGXable(gxeng))          g_gx2dbgBail[gxeng & 1][2]++;
	/* else: entered the gate - reason 0/6/7 tallied inside */
#endif

	if (GXMerge_2DBGLineArmed(gxeng)
	    && (!gpu->LayersEnable[4] || GX2DBG_ObjGXable(gxeng))
	    && !gpu->WIN0_ENABLED && !gpu->WIN1_ENABLED && !gpu->WINOBJ_ENABLED
	    && gpu->setFinalColorBck_funcNum < 4
	    && s5_blendOK)
	{
		// Fold a backdrop brighten/darken (Increase/Decrease with BD as 1st
		// target) into the recorded backdrop colour - GX draws the backdrop as a
		// flat quad, so a pre-faded colour reproduces it exactly.  (Local copy:
		// the CPU fall-through path must still see the raw backdrop.)
		u16 s5_backdrop = backdrop_color & 0x7FFF;
		if (s5_bm >= 2 && (s5_t1st & 0x20))
			s5_backdrop = (s5_bm == 2) ? gpu->currentFadeInColors[s5_backdrop]
			                           : gpu->currentFadeOutColors[s5_backdrop];
		// This line cleared every frame-invariant gate; tell GX2DBG the BG-plane
		// bake is worth doing (even if this particular line still bails below on
		// an unbaked layer or a 3D-shape check).
		GX2DBG_NoteWouldRecord(gxeng);
		const bool has3d = (gxeng == 0) && dispCnt->BG0_3D && gpu->LayersEnable[0];
		bool ao = false, fao = false; u8 feva = 0, bmode = 0, bfac = 0;
		bool ok = true;

		if (has3d) {
			if (!GXMerge_LineMergeable(gpu, &ao, &fao, &feva, &bmode, &bfac) || fao)
				ok = false;   // front translucent BG / other 3D disqualifier -> CPU path
		} else if ((gpu->MasterBrightMode == 1 || gpu->MasterBrightMode == 2)
		           && gpu->MasterBrightFactor) {
			bmode = gpu->MasterBrightMode;
			bfac  = gpu->MasterBrightFactor > 16 ? 16 : gpu->MasterBrightFactor;
		}

		u8  gxkind[GX2DBG_MAX_LAYERS], gxlay[GX2DBG_MAX_LAYERS];
		u16 gxhofs[GX2DBG_MAX_LAYERS], gxvofs[GX2DBG_MAX_LAYERS];
		int gxn = 0;
		int threeDAt = -1;
		for (int prio = NB_PRIORITIES - 1; prio >= 0 && ok; prio--) {
			itemsForPriority_t *it = &gpu->itemsForPriority[prio];
			for (int i = 0; i < it->nbBGs; i++) {
				const int bg = it->BGs[i];
				if (!gpu->LayersEnable[bg]) continue;
				if (gxn >= GX2DBG_MAX_LAYERS) { ok = false; break; }
				if (bg == 0 && has3d) {
					gxkind[gxn] = GX2DBG_KIND_3D;
					gxlay[gxn]  = 0;
					gxhofs[gxn] = (u16)gpu->getHOFS(0);
					gxvofs[gxn] = 0;
					threeDAt = gxn;
					gxn++;
					continue;
				}
				if (gpu->BGTypes[bg] != BGType_Text ||
				    gpu->dispx_st->dispx_BGxCNT[bg].bits.Mosaic_Enable ||
				    !GX2DBG_LayerReady(gxeng, bg)) { ok = false; break; }
				gxkind[gxn] = GX2DBG_KIND_BG;
				gxlay[gxn]  = (u8)bg;
				gxhofs[gxn] = (u16)gpu->getHOFS(bg);
				gxvofs[gxn] = (u16)gpu->getVOFS(bg);
				gxn++;
			}
		}
		if (has3d && threeDAt < 0) ok = false;   // 3D expected but BG0 not in the walk

		if (ok) {
			const u8 behindContent = (threeDAt > 0) ? 1 : 0;
			GXMerge_Record2DBGLine(gxeng, l, (u16)(s5_backdrop | 0x8000), bmode, bfac,
			                       ao ? 1 : 0, behindContent,
			                       gxn, gxkind, gxlay, gxhofs, gxvofs);
#ifdef DESMUME_BENCH
			g_gx2dbgBail[gxeng & 1][0]++;
#endif
			return;
		}
#ifdef DESMUME_BENCH
		g_gx2dbgBail[gxeng & 1][(has3d && (threeDAt < 0 || !ok)) ? 6 : 7]++;
#endif
	}
	}

	//we need to write backdrop colors in the same way as we do BG pixels in order to do correct window processing
	//this is currently eating up 2fps or so. it is a reasonable candidate for optimization. 
	gpu->currBgNum = 5;

	switch(gpu->setFinalColorBck_funcNum) {
		//for backdrops, (even with window enabled) none and blend are both the same: just copy the color
		case 0:
		case 1:
			memset_u16_le<256>(gpu->currDst,backdrop_color); 
			break;

		//for backdrops, fade in and fade out can be applied if it's a 1st target screen
		case 2:
			if(gpu->BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16_le<256>(gpu->currDst,gpu->currentFadeInColors[backdrop_color]);
			else 
				memset_u16_le<256>(gpu->currDst,backdrop_color); 
			break;
		case 3:
			if(gpu->BLDCNT & 0x20) //backdrop is selected for color effect
				memset_u16_le<256>(gpu->currDst,gpu->currentFadeOutColors[backdrop_color]);
			else
				memset_u16_le<256>(gpu->currDst,backdrop_color); 
			break;

		//windowed cases apparently need special treatment? why? can we not render the backdrop? how would that even work?
		case 4: for(int x=0;x<256;x++) gpu->___setFinalColorBck<false,true,4>(backdrop_color,x,1); break;
		case 5: for(int x=0;x<256;x++) gpu->___setFinalColorBck<false,true,5>(backdrop_color,x,1); break;
		case 6: for(int x=0;x<256;x++) gpu->___setFinalColorBck<false,true,6>(backdrop_color,x,1); break;
		case 7: for(int x=0;x<256;x++) gpu->___setFinalColorBck<false,true,7>(backdrop_color,x,1); break;
	}
	
	memset(gpu->bgPixels,5,256);

	// init background color & priorities
	// sprAlpha/sprType/sprPrio are only ever read behind the LayersEnable[4]
	// gate below (sprite bucketing + composite); sprWin only when the OBJ
	// window is active. Skip ~1 KB of memsets per scanline when neither is on.
	// (desmumewii-2d-compositor-plan.md Step 3.1)
	if (gpu->LayersEnable[4])
	{
		memset(sprAlpha, 0, 256);
		memset(sprType, 0, 256);
		memset(sprPrio, 0xFF, 256);
	}
	if (gpu->LayersEnable[4] || gpu->WINOBJ_ENABLED)
		memset(sprWin, 0, 256);

	// init pixels priorities
	assert(NB_PRIORITIES==4);
	gpu->itemsForPriority[0].nbPixelsX = 0;
	gpu->itemsForPriority[1].nbPixelsX = 0;
	gpu->itemsForPriority[2].nbPixelsX = 0;
	gpu->itemsForPriority[3].nbPixelsX = 0;

	// for all the pixels in the line
	if (gpu->LayersEnable[4]) 
	{
		//n.b. - this is clearing the sprite line buffer to the background color,
		//but it has been changed to write u32 instead of u16 for a little speedup
		for(int i = 0; i< 128; ++i) HostWriteTwoWords(spr, i << 2, backdrop_color | (backdrop_color<<16));
		//zero 06-may-09: I properly supported window color effects for backdrop, but I am not sure
		//how it interacts with this. I wish we knew why we needed this
		
		gpu->spriteRender(spr, sprAlpha, sprType, sprPrio);
		mosaicSpriteLine(gpu, l, spr, sprAlpha, sprType, sprPrio);

		for(int i = 0; i<256; i++) 
		{
			// assign them to the good priority item
			int prio = sprPrio[i];
			if (prio >=4) continue;
			
			item = &(gpu->itemsForPriority[prio]);
			item->PixelsX[item->nbPixelsX]=i;
			item->nbPixelsX++;
		}
	}

	
	if (!gpu->LayersEnable[0] && !gpu->LayersEnable[1] && !gpu->LayersEnable[2] && !gpu->LayersEnable[3])
		BG_enabled = FALSE;

	for(int j=0;j<8;j++)
		gpu->blend2[j] = (gpu->BLDCNT & (0x100 << j))!=0;

	// paint lower priorities first
	// then higher priorities on top
	for(int prio=NB_PRIORITIES; prio > 0; )
	{
		prio--;
		item = &(gpu->itemsForPriority[prio]);
		// render BGs
		if (BG_enabled)
		{
			for (int i=0; i < item->nbBGs; i++) 
			{
				i16 = item->BGs[i];
				if (gpu->LayersEnable[i16])
				{
					gpu->currBgNum = (u8)i16;
					gpu->blend1 = (gpu->BLDCNT & (1 << gpu->currBgNum))!=0;

					struct _BGxCNT *bgCnt = &(gpu->dispx_st)->dispx_BGxCNT[i16].bits;
					gpu->curr_mosaic_enabled = bgCnt->Mosaic_Enable;

					if (gpu->core == GPU_MAIN)
					{
						if (i16 == 0 && dispCnt->BG0_3D)
						{
							gpu->currBgNum = 0;

							// Hardware-merge path: everything drawn so far this
							// scanline is the "behind" bucket (already in
							// GPU_screen); redirect the rest of the walk - the
							// "front" bucket - into GXMerge's front line buffer
							// and let the GX hardware sandwich the 3D layer in.
							bool alphaOver = false;
							bool frontAlphaOver = false;
							u8   frontEva = 0;
							u8   brightMode = 0;
							u8   brightFactor = 0;
#ifdef GXMERGE_FORCE
							if (GXMerge_FrameArmed() && gpu->core == GPU_MAIN)
#else
							if (GXMerge_FrameArmed() && gpu->core == GPU_MAIN &&
							    GXMerge_LineMergeable(gpu, &alphaOver, &frontAlphaOver, &frontEva,
							                          &brightMode, &brightFactor))
#endif
							{
								// Is there any 2D layer that can sit behind the 3D
								// layer (>= BG0's priority), i.e. real content to
								// reveal through transparent 3D?  Cheap register
								// check - no per-pixel work.
								const u32 p3d = gpu->dispx_st->dispx_BGxCNT[0].bits.Priority;
								bool behindContent = gpu->LayersEnable[4];   // sprites: assume some behind
								for (int bg = 1; bg <= 3 && !behindContent; bg++)
									if (gpu->LayersEnable[bg] &&
									    gpu->dispx_st->dispx_BGxCNT[bg].bits.Priority >= p3d)
										behindContent = true;

								GXMerge_RecordLine(l, behindContent, gpu->getHOFS(0), alphaOver,
								                   frontAlphaOver, frontEva,
								                   brightMode, brightFactor);
								gpu->tempScanline = gpu->currDst = GXMerge_FrontLine(l);
								memset(gpu->bgPixels, 0, 256);   // 3D (BG0) is now "below"
								// The front line buffer never holds a 3D/behind pixel, so a
								// CPU blend of a front layer against the bg_under==0 sentinel
								// is always wrong - it would blend against the (empty) front
								// buffer.  Clear BG0's 2nd-target bit for the rest of this
								// line's walk; the GX front draw does that blend instead
								// (frontAlphaOver band).  blend2[] is rebuilt from BLDCNT
								// every line (see above), so this is scoped to this line.
								gpu->blend2[0] = 0;
								continue;
							}

							const u16 hofs = gpu->getHOFS(i16);

							// Hardware-merge mode keeps the 3D scene only as a GX
							// texture; de-swizzle it into gfx3d_convertedScreen now,
							// on demand, for this fallback line's legacy per-pixel
							// composite (idempotent within the frame).
							if (GXMerge_Enabled())
								GXMerge_MaterializeConverted();

							gfx3d_GetLineData(l, &gpu->_3dColorLine);
							u8* colorLine = gpu->_3dColorLine;

							for(int k = 0; k < 256; k++)
							{
								int q = ((k + hofs) & 0x1FF);

								if((q < 0) || (q > 255))
									continue;

								if(colorLine[(q<<2)])
									gpu->setFinalColor3d(k, q);
							}

							continue;
						}
					}

					//useful for debugging individual layers
					//if(gpu->core == 1 || i16 != 2) continue;




#ifndef DISABLE_MOSAIC
					if(gpu->curr_mosaic_enabled)
						gpu->modeRender<true>(i16);
					else 
#endif
						gpu->modeRender<false>(i16);
				} //layer enabled
			}
		}

		// render sprite Pixels
		if (gpu->LayersEnable[4])
		{
			gpu->currBgNum = 4;
			gpu->blend1 = (gpu->BLDCNT & (1 << gpu->currBgNum))!=0;

			// setFinalColorSpr_funcNum is loop-invariant across this bucket (set
			// once per BLDCNT/DISPCNT write in SetupFinalPixelBlitter), but the
			// switch inside setFinalColorSpr() would otherwise be an indirect
			// jump-table dispatch on every sprite pixel. Hoist it: pick the
			// blend-mode template once, run a straight-line loop.
			// desmumewii-2d-compositor-plan.md Step 2.
			const int nbPixelsX = item->nbPixelsX;
			#define DESMUME_SPR_COMPOSITE(FUNC, WIN) \
				for (int i=0; i < nbPixelsX; i++) { \
					const u16 px = item->PixelsX[i]; \
					_master_setFinalOBJColor<FUNC, WIN>(gpu, gpu->currDst, \
						HostReadWord(spr, (px<<1)), sprAlpha[px], sprType[px], px); \
				}
			switch(gpu->setFinalColorSpr_funcNum)
			{
				case 0x0: DESMUME_SPR_COMPOSITE(None,     false); break;
				case 0x1: DESMUME_SPR_COMPOSITE(Blend,    false); break;
				case 0x2: DESMUME_SPR_COMPOSITE(Increase, false); break;
				case 0x3: DESMUME_SPR_COMPOSITE(Decrease, false); break;
				case 0x4: DESMUME_SPR_COMPOSITE(None,     true);  break;
				case 0x5: DESMUME_SPR_COMPOSITE(Blend,    true);  break;
				case 0x6: DESMUME_SPR_COMPOSITE(Increase, true);  break;
				case 0x7: DESMUME_SPR_COMPOSITE(Decrease, true);  break;
			}
			#undef DESMUME_SPR_COMPOSITE
		}
	}
}

template<bool SKIP> static void GPU_RenderLine_DispCapture(u16 l)
{
	//this macro takes advantage of the fact that there are only two possible values for capx
	//
	// DST is always cap_dst - real guest VRAM (MMU.ARM9_LCD-backed), which by
	// DS-hardware contract is always little-endian, the same as every other
	// VRAM bank this port touches elsewhere (see LE_TO_LOCAL_16/T1ReadWord use
	// throughout the BG-bitmap and dispMode==2 paths below). It must be
	// written with an endian-safe store (T1WriteWord), not HostWriteWord,
	// which is a raw native 16-bit store - a silent byte-swap on this
	// (big-endian) target. Previously every capture wrote its pixels
	// byte-swapped into VRAM; any consumer that later read that bank back
	// correctly as little-endian (a BG layer pointed at the captured bank -
	// e.g. Phantom Hourglass's dual-3D-screen trick, which captures Engine
	// A's composited 3D+2D output to VRAM every frame and displays it via an
	// ordinary bitmap BG layer on the other physical screen - or dispMode==2)
	// would then see every pixel's RGB555 channels scrambled: a clean blue
	// water texture on the "live" screen came out as the pink/black/yellow
	// blob on the "captured" screen. READFN is HostReadWord when SRC is one
	// of this file's own internal, native-order buffers (gpu->tempScanline,
	// gfx3d_GetLineData15bpp's static buf - both written with HostWriteWord
	// elsewhere, so reading them back with the matching native accessor is
	// correct), or T1ReadWord for the "Capture VRAM" (srcB==0) case, where
	// SRC is cap_src - itself real little-endian guest VRAM.
	#define CAPCOPY(SRC,DST,SETALPHABIT,READFN) \
	switch(gpu->dispCapCnt.capx) { \
		case DISPCAPCNT::_128: \
			for (int i = 0; i < 128; i++)  \
				T1WriteWord(DST, i << 1, READFN(SRC, i << 1) | (SETALPHABIT?(1<<15):0)); \
			break; \
		case DISPCAPCNT::_256: \
			for (int i = 0; i < 256; i++)  \
				T1WriteWord(DST, i << 1, READFN(SRC, i << 1) | (SETALPHABIT?(1<<15):0)); \
			break; \
			default: assert(false); \
		}
	
	GPU * gpu = MainScreen.gpu;

	if (l == 0)
	{
		if (gpu->dispCapCnt.val & 0x80000000)
		{
			gpu->dispCapCnt.enabled = TRUE;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
			// capture writes go straight into MMU.ARM9_LCD, bypassing the GX
			// dirty hooks in _MMU_write* - mark the destination 128 KB block.
			GXDirty_MarkCaptureBlock(gpu->dispCapCnt.writeBlock);
		}
	}

	bool skip = SKIP;

	if (gpu->dispCapCnt.enabled)
	{
		//128-wide captures should write linearly into memory, with no gaps
		//this is tested by hotel dusk
		u32 ofsmul = gpu->dispCapCnt.capx==DISPCAPCNT::_128?256:512;
		u32 cap_src_adr = gpu->dispCapCnt.readOffset * 0x8000 + (l * 512);
		u32 cap_dst_adr = gpu->dispCapCnt.writeOffset * 0x8000 + (l * ofsmul);

		//Read/Write block wrap to 00000h when exceeding 1FFFFh (128k)
		//this has not been tested yet (I thought I needed it for hotel dusk, but it was fixed by the above)
		cap_src_adr &= 0x1FFFF;
		cap_dst_adr &= 0x1FFFF;

		cap_src_adr += gpu->dispCapCnt.readBlock * 0x20000;
		cap_dst_adr += gpu->dispCapCnt.writeBlock * 0x20000;

		u8* cap_src = MMU.ARM9_LCD + cap_src_adr;
		u8* cap_dst = MMU.ARM9_LCD + cap_dst_adr;

		//we must block captures when the capture dest is not mapped to LCDC
		if(vramConfiguration.banks[gpu->dispCapCnt.writeBlock].purpose != VramConfiguration::LCDC)
			skip = true;

		//we must return zero from reads from memory not mapped to lcdc
		if(vramConfiguration.banks[gpu->dispCapCnt.readBlock].purpose != VramConfiguration::LCDC)
			cap_src = MMU.blank_memory;

		if(!skip)
		if (l < gpu->dispCapCnt.capy)
		{
			switch (gpu->dispCapCnt.capSrc)
			{
				case 0:		// Capture source is SourceA
					{
						//INFO("Capture source is SourceA\n");
						switch (gpu->dispCapCnt.srcA)
						{
							case 0:			// Capture screen (BG + OBJ + 3D)
								{
									//INFO("Capture screen (BG + OBJ + 3D)\n");

									u8 *src = (u8*)(gpu->tempScanline);
									CAPCOPY(src,cap_dst,true,HostReadWord);
								}
							break;
							case 1:			// Capture 3D
								{
									//INFO("Capture 3D\n");
									u16* colorLine;
									gfx3d_GetLineData15bpp(l, &colorLine);
									CAPCOPY(((u8*)colorLine),cap_dst,false,HostReadWord);
								}
							break;
						}
					}
				break;
				case 1:		// Capture source is SourceB
					{
						//INFO("Capture source is SourceB\n");
						switch (gpu->dispCapCnt.srcB)
						{
							case 0:
								//Capture VRAM
								CAPCOPY(cap_src,cap_dst,true,T1ReadWord);
								break;
							case 1:
								//capture dispfifo
								//(not yet tested)
								for(int i=0; i < 128; i++)
									T1WriteLong(cap_dst, i << 2, DISP_FIFOrecv());
								break;
						}
					}
				break;
				default:	// Capture source is SourceA+B blended
					{
						//INFO("Capture source is SourceA+B blended\n");
						u16 *srcA = NULL;
						u16 *srcB = NULL;

						if (gpu->dispCapCnt.srcA == 0)
						{
							// Capture screen (BG + OBJ + 3D)
							srcA = (u16*)(gpu->tempScanline);
						}
						else
						{
							gfx3d_GetLineData15bpp(l, &srcA);
						}

						static u16 fifoLine[256];

						// srcB, when it's cap_src (real guest VRAM) or fifoLine
						// (filled via the endian-safe T1WriteLong just below),
						// must be read back with T1ReadWord rather than raw
						// u16[] indexing - same little-endian-VRAM-vs-native-
						// accessor mismatch as CAPCOPY above. srcA is always
						// one of this file's own native-order buffers
						// (tempScanline / gfx3d_GetLineData15bpp's buf), so
						// raw indexing there is correct and left as-is.
						bool srcBIsLE;

						if (gpu->dispCapCnt.srcB == 0)			// VRAM screen
						{
							srcB = (u16 *)cap_src;
							srcBIsLE = true;
						}
						else
						{
							//fifo - tested by splinter cell chaos theory thermal view
							srcB = fifoLine;
							for (int i=0; i < 128; i++)
								T1WriteLong((u8*)srcB, i << 2, DISP_FIFOrecv());
							srcBIsLE = true;
						}


						const int todo = (gpu->dispCapCnt.capx==DISPCAPCNT::_128?128:256);

						for(u16 i = 0; i < todo; i++)
						{
							u16 a,r,g,b;

							const u16 srcAv = srcA[i];
							const u16 srcBv = srcBIsLE ? T1ReadWord((u8*)srcB, i << 1) : srcB[i];

							u16 a_alpha = srcAv & 0x8000;
							u16 b_alpha = srcBv & 0x8000;

							if(a_alpha)
							{
								a = 0x8000;
								r = ((srcAv & 0x1F) * gpu->dispCapCnt.EVA);
								g = (((srcAv >>  5) & 0x1F) * gpu->dispCapCnt.EVA);
								b = (((srcAv >>  10) & 0x1F) * gpu->dispCapCnt.EVA);
							}
							else
								a = r = g = b = 0;

							if(b_alpha)
							{
								a = 0x8000;
								r += ((srcBv & 0x1F) * gpu->dispCapCnt.EVB);
								g += (((srcBv >>  5) & 0x1F) * gpu->dispCapCnt.EVB);
								b += (((srcBv >> 10) & 0x1F) * gpu->dispCapCnt.EVB);
							}

							r >>= 4;
							g >>= 4;
							b >>= 4;

							//freedom wings sky will overflow while doing some fsaa/motionblur effect without this
							r = std::min((u16)31,r);
							g = std::min((u16)31,g);
							b = std::min((u16)31,b);

							T1WriteWord(cap_dst, i << 1, a | (b << 10) | (g << 5) | r);
						}
					}
				break;
			}
		}

		if (l>=191)
		{
			gpu->dispCapCnt.enabled = FALSE;
			gpu->dispCapCnt.val &= 0x7FFFFFFF;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
			return;
		}
	}
}

static INLINE void GPU_RenderLine_MasterBrightness(NDS_Screen * screen, u16 l)
{
	GPU * gpu = screen->gpu;

	u8 * dst =  GPU_screen + (screen->offset + l) * 512;
	u16 i16;

	//isn't it odd that we can set uselessly high factors here?
	//factors above 16 change nothing. curious.
	int factor = gpu->MasterBrightFactor;
	if(factor==0) return;
	if(factor>16) factor=16;


	//Apply final brightness adjust (MASTER_BRIGHT)
	//http://nocash.emubase.de/gbatek.htm#dsvideo (Under MASTER_BRIGHTNESS)
	
	switch (gpu->MasterBrightMode)
	{
		// Disabled
		case 0:
			break;

		// Bright up
		case 1:
		{
			if(factor != 16)
			{
				for(i16 = 0; i16 < 256; ++i16)
				{
					((u16*)dst)[i16] = fadeInColors[factor][((u16*)dst)[i16]&0x7FFF];
				}
			}
			else
			{
				// all white (optimization)
				for(i16 = 0; i16 < 256; ++i16)
					((u16*)dst)[i16] = 0x7FFF;
			}
			break;
		}

		// Bright down
		case 2:
		{
			if(factor != 16)
			{
				for(i16 = 0; i16 < 256; ++i16)
				{
					((u16*)dst)[i16] = fadeOutColors[factor][((u16*)dst)[i16]&0x7FFF];
				}
			}
			else
			{
				// all black (optimization)
				memset(dst, 0, 512);
			}
			break;
		}

		// Reserved
		case 3:
			break;
	 }

}

template<int WIN_NUM>
FORCEINLINE void GPU::setup_windows()
{
	u8 y = currLine;
	u16 startY,endY;

	if(WIN_NUM==0)
	{
		startY = WIN0V0;
		endY = WIN0V1;
	}
	else
	{
		startY = WIN1V0;
		endY = WIN1V1;
	}

	if(WIN_NUM == 0 && !WIN0_ENABLED) goto allout;
	if(WIN_NUM == 1 && !WIN1_ENABLED) goto allout;

	if(startY > endY)
	{
		if((y < startY) && (y > endY)) goto allout;
	}
	else
	{
		if((y < startY) || (y >= endY)) goto allout;
	}

	//the x windows will apply for this scanline
	curr_win[WIN_NUM] = h_win[WIN_NUM];
	return;
	
allout:
	curr_win[WIN_NUM] = win_empty;
}

void GPU::update_winh(int WIN_NUM)
{
	//Don't even waste any time in here if the window isn't enabled
	if(WIN_NUM==0 && !WIN0_ENABLED) return;
	if(WIN_NUM==1 && !WIN1_ENABLED) return;

	need_update_winh[WIN_NUM] = false;
	u16 startX,endX;

	if(WIN_NUM==0)
	{
		startX = WIN0H0;
		endX = WIN0H1;
	}
	else
	{
		startX = WIN1H0;
		endX = WIN1H1;
	}

	//the original logic: if you doubt the window code, please check it against the newer implementation below
	//if(startX > endX)
	//{
	//	if((x < startX) && (x > endX)) return false;
	//}
	//else
	//{
	//	if((x < startX) || (x >= endX)) return false;
	//}

	if(startX > endX)
	{
		for(int i=0;i<=endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX+1;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<256;i++)
			h_win[WIN_NUM][i] = 1;
	} else
	{
		for(int i=0;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX;i<256;i++)
			h_win[WIN_NUM][i] = 0;
	}
}

// Whether the MAIN engine's frame can be handed to the hardware-merge path this
// frame.  Two tiers:
//  - Frame-level (GXMerge_FrameMergeable, checked once at line 0): things that
//    don't vary per scanline and make the sandwich meaningless outright for the
//    whole frame - a non-2D-compositor display mode, or display capture (which
//    needs the legacy readback intact).
//  - Line-level (GXMerge_LineMergeable, Phase 2: re-checked at every 3D split
//    point): blend/window state that a game can rewrite mid-frame via HBlank IRQ
//    raster effects. BG0 HOFS (see GXMerge_HofsSegment) and master-bright
//    mode/factor (see GXMergeBand::brightMode, applied by GXMerge draw 4) are
//    also read fresh per line but, unlike a window or a mixed blend target,
//    don't disqualify the line - they just split the band.
//    GPU_RenderLine_layer already
//    re-reads all of these fresh every scanline for the legacy per-pixel path
//    (see gpu->blend2[] just above), so checking them once at line 0 and freezing
//    the decision for the whole frame was needlessly pessimistic: one scanline
//    using a window or a blend effect used to fall the *entire screen* back to
//    the legacy path. Now only the disqualified scanlines do - everything else
//    still gets the hardware sandwich, coalesced into bands by GXMerge_EndFrame.
// diagnostic: last reason a mergeability check returned false
// 0 ok/armed  1 dispMode  2 capture
// 3 blend2 - a cross-boundary blend GX fixed-function can't express as one
//   whole-band operation: either the behind-bucket layers that could sit under
//   3D don't share one blend2 eligibility, or BG0 is a blend 2nd target and the
//   front bucket isn't the one clean shape the front-vs-beneath blend handles
//   (a single blend1 BG, no front sprites - see GXMerge_LineMergeable).
// 4 bright1st  5 window
// 6 unused (was hofs - now handled per-band, see GXMerge_HofsSegment)
// 7 unused (was masterbright - now handled per-band via GXMerge draw 4, see
//   GXMergeBand::brightMode)
int g_gxmergeFailReason = 0;

#ifdef DESMUME_BENCH
// Step 5 diagnostic: [engine][reason] scanline tally for the 2D-BG recorder.
// reason: 0 recorded, 1 not-armed, 2 obj, 3 window, 4 funcNum, 5 blendmode,
//         6 3D-line-not-mergeable, 7 layer-not-bakeable.
u32 g_gx2dbgBail[2][8] = {{0}};
u32 g_gx2objRej[2][6] = {{0}};
u32 g_gx2objDis[2][4] = {{0}};   // ObjFrameUpdate disable cause: [0]bld [1]win [2]winobj [3]budget
#endif

static bool GXMerge_FrameMergeable(GPU * gpu)
{
	g_gxmergeFailReason = 0;

	// dispMode != 1: the main engine is showing a VRAM framebuffer / FIFO / off,
	// not the 2D compositor output, so there is nothing to sandwich into.
	if (gpu->dispMode != 1)
		{ g_gxmergeFailReason = 1; return false; }
	// display capture reads the 3D / 2D-composite directly and needs the legacy
	// readback intact this frame.
	if (gpu->dispCapCnt.enabled || (gpu->dispCapCnt.val & 0x80000000))
		{ g_gxmergeFailReason = 2; return false; }

	return true;
}

// Per-scanline disqualifiers, checked fresh at the 3D split point for every line
// that has BG0_3D active. A line that fails falls through to the legacy
// per-pixel setFinalColor3d loop right below the caller's redirect - i.e. just
// that scanline (and whichever band it ends up in) is composited the old way;
// every other line's redirect is untouched.
//
// *outAlphaOver reports whether the hardware sandwich should draw this line's
// 3D band with a real per-pixel alpha blend against the behind bucket
// (GXMerge_HofsSegment's caller, GXMerge_DrawMainScreen, keys off this per
// band) instead of the default opaque-and-alpha-keyed draw.
static bool GXMerge_LineMergeable(GPU * gpu, bool *outAlphaOver,
                                  bool *outFrontAlphaOver, u8 *outFrontEva,
                                  u8 *outBrightMode, u8 *outBrightFactor)
{
	*outAlphaOver = false;
	*outFrontAlphaOver = false;
	*outFrontEva = 0;
	*outBrightMode = 0;
	*outBrightFactor = 0;
	const u16 bld = gpu->BLDCNT;
	const u32 p3d = gpu->dispx_st->dispx_BGxCNT[0].bits.Priority;
	const u8  blendMode = (bld >> 6) & 3;

	// --- front-bucket shape check (BG0/3D as a blend 2nd target) ----------
	// BG0 selected as a blend 2nd target means a FRONT-bucket 2D layer above
	// 3D blends against what is beneath it.  On real hardware
	// (_master_setFinalBGColor case Blend) that is a constant-fraction blend
	// by the global BLDALPHA EVA/EVB, so it maps onto one GX blend per band -
	// but only when (a) the front bucket is a single blend1 BG in alpha-blend
	// mode with nothing stacked on it (a stacked front layer or a front
	// sprite would be wrongly translucified by a whole-band blend of the
	// front composite), and (b) everything the front layer can sit on is
	// uniformly blend2-eligible (checked with the behind-bucket uniformity
	// below) - otherwise the DS draws it opaque over the non-eligible parts
	// and a whole-band blend would over-blend those.
	// GPU_RenderLine_layer's redirect clears blend2[0] for the front walk, so
	// the front layer is composited opaque and the GX front draw
	// (GXMergeBand::frontAlphaOver) does the EVA blend against the EFB.
	bool wantFrontBlend = false;
	if (bld & 0x0100)
	{
		int frontBG = -1, nFrontBG = 0;
		for (int bg = 1; bg <= 3; bg++)
			if (gpu->LayersEnable[bg] &&
			    gpu->dispx_st->dispx_BGxCNT[bg].bits.Priority < p3d)
				{ nFrontBG++; frontBG = bg; }

		bool frontSprite = false;
		if (gpu->LayersEnable[4])
			for (u32 pr = 0; pr <= p3d; pr++)
				if (gpu->itemsForPriority[pr].nbPixelsX) { frontSprite = true; break; }

		if (nFrontBG == 0 && !frontSprite) {
			// BG0 is a 2nd target but nothing in front actually blends against
			// it this line - merge normally, no front blend.
		} else if (nFrontBG == 1 && !frontSprite &&
		           blendMode == 1 && (bld & (1 << frontBG))) {
			wantFrontBlend = true;
		} else {
			g_gxmergeFailReason = 3; return false;
		}
	}

	// The other five blend-2nd-target bits (BG1,BG2,BG3,OBJ,Backdrop) govern
	// whether 3D blends against whatever is directly beneath it in the BEHIND
	// bucket. Real hardware rule, already reproduced in this file's own
	// _master_setFinal3dColor(): 3D blends with blend2[bg_under] using the 3D
	// polygon's OWN per-pixel alpha - entirely independent of BLDCNT's blend
	// mode bits or whether BG0 itself is selected as a 1st target. Which
	// layer is "bg_under" varies per screen column (whichever behind-bucket
	// layer happens to be topmost there), so a single GX draw for the whole
	// band is only correct if every layer that could possibly be "under" 3D
	// on this line shares the same blend2 eligibility - mixed eligibility is
	// a genuinely per-pixel condition GX fixed-function can't express in one
	// band, and falls back same as before.
	bool underBlend2 = gpu->blend2[5];   // backdrop is always a potential "under"
	{
		bool uniform = true;
		for (int bg = 1; bg <= 3 && uniform; bg++)
			if (gpu->LayersEnable[bg] &&
			    gpu->dispx_st->dispx_BGxCNT[bg].bits.Priority >= p3d &&
			    gpu->blend2[bg] != underBlend2)
				uniform = false;
		if (uniform && gpu->LayersEnable[4] && gpu->blend2[4] != underBlend2)
			uniform = false;   // sprites: conservatively assume some may be behind
		if (!uniform)
			{ g_gxmergeFailReason = 3; return false; }
		*outAlphaOver = underBlend2;
	}

	// Front-vs-beneath blend is only whole-band-correct if the front layer
	// sits on uniformly blend2-eligible content everywhere (backdrop + every
	// behind layer, checked just above, plus BG0 itself which the 0x0100 test
	// guarantees).  Otherwise the DS would draw it opaque over the parts that
	// aren't 2nd targets - fall back for that line.
	if (wantFrontBlend)
	{
		if (!underBlend2)
			{ g_gxmergeFailReason = 3; return false; }
		*outFrontAlphaOver = true;
		*outFrontEva = gpu->BLDALPHA_EVA;   // pre-clamped 0..16
	}

	// brightness increase/decrease with BG0 as 1st target => 3D gets brightened
	if ((bld & 0x0001) && (blendMode >= 2))
		{ g_gxmergeFailReason = 4; return false; }
	if (gpu->setFinalColor3d_funcNum >= 4)   // a window is active
		{ g_gxmergeFailReason = 5; return false; }
	// BG0 HOFS (3D layer horizontal scroll) no longer disqualifies a line: the
	// hardware sandwich now reproduces it exactly by drawing each band's 3D quad
	// from a sub-rect of the resident 3D texture (GXMerge_HofsSegment) instead of
	// the full 256 columns - see GXMerge_RecordLine's hofs argument and
	// GXMerge_EndFrame's band-split-on-hofs-change.
	// MASTER_BRIGHT (mode 1 bright-up / mode 2 bright-down) no longer disqualifies
	// a line: it applies to the *final* composited scanline, which for a merged
	// line is the whole behind/3D/front sandwich, so the merge path reproduces it
	// as a per-band full-width GX pass over the merged EFB (GXMerge draw 4) and
	// GPU_RenderLine skips the CPU GPU_RenderLine_MasterBrightness pass for merged
	// lines.  A mid-frame HBlank-IRQ rewrite of the factor/mode just splits the
	// band (GXMerge_EndFrame), same as HOFS.
	if (gpu->MasterBrightFactor != 0 &&
	    (gpu->MasterBrightMode == 1 || gpu->MasterBrightMode == 2)) {
		*outBrightMode   = gpu->MasterBrightMode;
		u32 f = gpu->MasterBrightFactor;
		*outBrightFactor = (u8)(f > 16 ? 16 : f);
	}

	g_gxmergeFailReason = 0;
	return true;
}

void GPU_RenderLine(NDS_Screen * screen, u16 l, bool skip)
{
	GPU * gpu = screen->gpu;

	//here is some setup which is only done on line 0
	if(l == 0) {
		if (gpu->core == GPU_MAIN && GXMerge_Enabled()) {
			GXMerge_BeginFrame(MainScreen.offset == 0);
#ifndef GXMERGE_FORCE
			if (!GXMerge_FrameMergeable(gpu))
				GXMerge_Disarm();
#endif
			// Step 5.1a: bake this frame's MAIN text BG planes now (before the
			// per-line recorder can consult GX2DBG_LayerReady), on the core
			// thread. GX_InitTexObj/DCFlushRange are thread-safe (no FIFO cmds).
			if (GXMerge_2DBGEnabled()) {
				GX2DBG_FrameUpdate(gpu);
				GX2DBG_ObjFrameUpdate(gpu);
			}
		}
		// Step 5.1a: SUB engine (no 3D) - arm + bake its own 2D-BG record.
		if (gpu->core == GPU_SUB && GXMerge_2DBGEnabled()) {
			GXMerge_Begin2DBGSub(gpu->dispMode);
			GX2DBG_FrameUpdate(gpu);
			GX2DBG_ObjFrameUpdate(gpu);
		}
		//this is speculative. the idea is as follows:
		//whenever the user updates the affine start position regs, it goes into the active regs immediately
		//(this is handled on the set event from MMU)
		//maybe it shouldnt take effect until the next hblank or something..
		//this is a based on a combination of:
		//heroes of mana intro FMV
		//SPP level 3-8 rotoscale room
		//NSMB raster fx backdrops
		//bubble bobble revolution classic mode
		//NOTE:
		//I am REALLY unsatisfied with this logic now. But it seems to be working..
		gpu->refreshAffineStartRegs(-1,-1);

		// Ring-buffer history recorder: RAM only, no I/O, so it's cheap to run
		// every frame for the whole session. GPU_DispCapDumpRing() (wired to
		// a GC pad button in main.cpp) writes it out on demand, so a trace
		// can be centered on a visual glitch the instant it's spotted on
		// screen instead of guessing at a timed capture window or a specific
		// anomaly signature to detect (a prior stall-detector approach here
		// found no stalls in screen-offset/writeBlock alternation even
		// across a session where the desync was directly visible on screen -
		// so whatever's wrong is in the VRAM bank *content* correspondence,
		// not the swap timing, and needs the raw sequence to find).
#ifdef GPU_DISPCAP_DEBUG_LOG
		{
			static u32 frameCounter = 0;
			int c = gpu->core;
			DispCapRingEntry *e = &g_dispCapRing[g_dispCapRingHead % DISPCAP_RING_CAP];
			if (c == 0) {
				e->frame = frameCounter;
				e->offsetA = (u8)screen->offset;
				e->dispModeA = gpu->dispMode;
				e->vramBlockA = gpu->vramBlock;
				e->bg0_3dA = gpu->dispCnt().BG0_3D;
				e->capEnabled = gpu->dispCapCnt.enabled;
				e->capArm = (gpu->dispCapCnt.val >> 31) & 1;
				e->writeBlock = gpu->dispCapCnt.writeBlock;
				e->readBlock = gpu->dispCapCnt.readBlock;
				e->capSrc = gpu->dispCapCnt.capSrc;
				e->srcA = gpu->dispCapCnt.srcA;
				e->srcB = gpu->dispCapCnt.srcB;
			} else {
				e->offsetB = (u8)screen->offset;
				e->dispModeB = gpu->dispMode;
				e->vramBlockB = gpu->vramBlock;
				e->bg0_3dB = gpu->dispCnt().BG0_3D;
				// B is always processed right after A for the same frame
				// (NDSSystem.cpp calls MainScreen then SubScreen per line),
				// so this is the point at which one full frame's entry is
				// complete - advance the ring.
				g_dispCapRingHead++;
				frameCounter++;
			}
		}
#endif
	}

	if(skip)
	{
		gpu->currLine = l;
		if (gpu->core == GPU_MAIN) 
		{
			GPU_RenderLine_DispCapture<true>(l);
			if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
		}
		return;
	}

	//blacken the screen if it is turned off by the user
	if(!CommonSettings.showGpu.screens[gpu->core])
	{
		u8 * dst =  GPU_screen + (screen->offset + l) * 512;
		memset(dst,0,512);
		return;
	}

	// skip some work if master brightness makes the screen completely white or completely black
	if(gpu->MasterBrightFactor >= 16 && (gpu->MasterBrightMode == 1 || gpu->MasterBrightMode == 2))
	{
		// except if it could cause any side effects (for example if we're capturing), then don't skip anything
		// - and, in hardware-merge mode, the MAIN engine's line still has to run
		//   the layer walk so GXMerge records the band; the full-white/black
		//   result is produced by GXMerge draw 4 instead (factor 16 => alpha 255).
		if(!(gpu->core == GPU_MAIN && (gpu->dispCapCnt.enabled || l == 0 || l == 191))
		   && !(gpu->core == GPU_MAIN && GXMerge_FrameArmed()))
		{
			gpu->currLine = l;
			GPU_RenderLine_MasterBrightness(screen, l);
			return;
		}
	}

	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	gpu->currLine = l;
	u16 mosaic_control = T1ReadWord((u8 *)&gpu->dispx_st->dispx_MISC.MOSAIC, 0);
	u16 mosaic_width = (mosaic_control & 0xF);
	u16 mosaic_height = ((mosaic_control>>4) & 0xF);

	//mosaic test hacks
	//mosaic_width = mosaic_height = 3;

	GPU::mosaicLookup.widthValue = mosaic_width;
	GPU::mosaicLookup.heightValue = mosaic_height;
	GPU::mosaicLookup.width = &GPU::mosaicLookup.table[mosaic_width][0];
	GPU::mosaicLookup.height = &GPU::mosaicLookup.table[mosaic_height][0];

	if(gpu->need_update_winh[0]) gpu->update_winh(0);
	if(gpu->need_update_winh[1]) gpu->update_winh(1);

	gpu->setup_windows<0>();
	gpu->setup_windows<1>();

	//generate the 2d engine output
	if(gpu->dispMode == 1) {
		//Optimization: render straight to the output buffer when that's what we are going to end up displaying anyway
		gpu->tempScanline = screen->gpu->currDst = (u8 *)(GPU_screen) + (screen->offset + l) * 512;
	} else {
		//otherwise, we need to go to a temp buffer
		gpu->tempScanline = screen->gpu->currDst = (u8 *)gpu->tempScanlineBuffer;
	}

	GPU_RenderLine_layer(screen, l);

	switch (gpu->dispMode)
	{
		case 0: // Display Off(Display white)
			{
				u8 * dst =  GPU_screen + (screen->offset + l) * 512;

				for (int i=0; i<256; i++)
					HostWriteWord(dst, i << 1, 0x7FFF);
			}
			break;

		case 1: // Display BG and OBJ layers
			//do nothing: it has already been generated into the right place
			break;

		case 2: // Display vram framebuffer
			{
				u16 * dst = (u16*)(GPU_screen + (screen->offset + l) * 512);
				u16 * src = (u16*)(gpu->VRAMaddr + (l*512));
				for(int i=0; i<256;i++) {
					dst[i] = LE_TO_LOCAL_16(src[i]);
				}
			}
			break;
		case 3: // Display memory FIFO
			{
				//this has not been tested since the dma timing for dispfifo was changed around the time of
				//newemuloop. it may not work.
				u8 * dst =  GPU_screen + (screen->offset + l) * 512;
				for (int i=0; i < 128; i++)
					T1WriteLong(dst, i << 2, DISP_FIFOrecv() & 0x7FFF7FFF);
			}
			break;
	}

	//capture after displaying so that we can safely display vram before overwriting it here
	if (gpu->core == GPU_MAIN)
	{
		// Hardware-merge mode: display capture of the 3D layer (srcA == 3D) reads
		// gfx3d_convertedScreen via gfx3d_GetLineData15bpp. The layer walk above
		// only materialises it when BG0/3D is actually composited; a capture with
		// 3D off would otherwise read a stale buffer. (A capture frame is already
		// whole-frame-disarmed by GXMerge_FrameMergeable, so this is a cheap
		// belt-and-braces call - usually a no-op after the walk.)
		if (GXMerge_Enabled() && gpu->dispCapCnt.enabled)
			GXMerge_MaterializeConverted();

		//BUG!!! if someone is capturing and displaying both from the fifo, then it will have been
		//consumed above by the display before we get here
		//(is that even legal? I think so)
		GPU_RenderLine_DispCapture<false>(l);
		if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
	}


	// Hardware-merge mode: this scanline's behind bucket is only one third of the
	// final composite (3D + front are added later by the GX sandwich), so
	// MASTER_BRIGHT must be applied to the whole merged result, not just the
	// behind bucket sitting in GPU_screen right now.  GXMerge draw 4 does that
	// per band; skip the CPU pass here for merged lines or the behind bucket
	// would be brightened twice.
	if (!(gpu->core == GPU_MAIN && GXMerge_FrameArmed() && GXMerge_LineWasMerged(l)))
		GPU_RenderLine_MasterBrightness(screen, l);
}

void gpu_savestate(EMUFILE* os)
{
	//version
	write32le(1,os);
	
	os->fwrite((char*)GPU_screen,sizeof(GPU_screen));
	
	write32le(MainScreen.gpu->affineInfo[0].x,os);
	write32le(MainScreen.gpu->affineInfo[0].y,os);
	write32le(MainScreen.gpu->affineInfo[1].x,os);
	write32le(MainScreen.gpu->affineInfo[1].y,os);
	write32le(SubScreen.gpu->affineInfo[0].x,os);
	write32le(SubScreen.gpu->affineInfo[0].y,os);
	write32le(SubScreen.gpu->affineInfo[1].x,os);
	write32le(SubScreen.gpu->affineInfo[1].y,os);
}

bool gpu_loadstate(EMUFILE* is, int size)
{
	//read version
	u32 version;

	//sigh.. shouldve used a new version number
	if(size == 256*192*2*2)
		version = 0;
	else if(size== 0x30024)
	{
		read32le(&version,is);
		version = 1;
	}
	else
		if(read32le(&version,is) != 1) return false;
		

	if(version<0||version>1) return false;

	is->fread((char*)GPU_screen,sizeof(GPU_screen));

	if(version==1)
	{
		read32le(&MainScreen.gpu->affineInfo[0].x,is);
		read32le(&MainScreen.gpu->affineInfo[0].y,is);
		read32le(&MainScreen.gpu->affineInfo[1].x,is);
		read32le(&MainScreen.gpu->affineInfo[1].y,is);
		read32le(&SubScreen.gpu->affineInfo[0].x,is);
		read32le(&SubScreen.gpu->affineInfo[0].y,is);
		read32le(&SubScreen.gpu->affineInfo[1].x,is);
		read32le(&SubScreen.gpu->affineInfo[1].y,is);
		//removed per nitsuja feedback. anyway, this same thing will happen almost immediately in gpu line=0
		//MainScreen.gpu->refreshAffineStartRegs(-1,-1);
		//SubScreen.gpu->refreshAffineStartRegs(-1,-1);
	}

	MainScreen.gpu->updateBLDALPHA();
	SubScreen.gpu->updateBLDALPHA();
	return !is->fail();
}

u32 GPU::getAffineStart(int layer, int xy)
{
	if(xy==0) return affineInfo[layer-2].x;
	else return affineInfo[layer-2].y;
}

void GPU::setAffineStartWord(int layer, int xy, u16 val, int word)
{
	u32 curr = getAffineStart(layer,xy);
	if(word==0) curr = (curr&0xFFFF0000)|val;
	else curr = (curr&0x0000FFFF)|(((u32)val)<<16);
	setAffineStart(layer,xy,curr);
}

void GPU::setAffineStart(int layer, int xy, u32 val)
{
	if(xy==0)
		affineInfo[layer-2].x = val;
	else
		affineInfo[layer-2].y = val;
	refreshAffineStartRegs(layer,xy);
}

void GPU::refreshAffineStartRegs(const int num, const int xy)
{
	if(num==-1)
	{
		refreshAffineStartRegs(2,xy);
		refreshAffineStartRegs(3,xy);
		return;
	}

	if(xy==-1)
	{
		refreshAffineStartRegs(num,0);
		refreshAffineStartRegs(num,1);
		return;
	}

	BGxPARMS * parms;
	if (num==2)
		parms = &(dispx_st)->dispx_BG2PARMS;
	else
		parms = &(dispx_st)->dispx_BG3PARMS;		

	if(xy==0)
		parms->BGxX = affineInfo[num-2].x;
	else
		parms->BGxY = affineInfo[num-2].y;
}

template<bool MOSAIC, int FUNCNUM> static FORCEINLINE void modeRenderT(GPU * gpu, int layer)
{
	switch(GPU_mode2type[gpu->dispCnt().BG_Mode][layer])
	{
		case BGType_Text: lineText<MOSAIC,FUNCNUM>(gpu); break;
		case BGType_Affine: lineRot<MOSAIC,FUNCNUM>(gpu); break;
		case BGType_AffineExt: lineExtRot<MOSAIC,FUNCNUM>(gpu); break;
		case BGType_Large8bpp: lineExtRot<MOSAIC,FUNCNUM>(gpu); break;
		case BGType_Invalid:
			PROGINFO("Attempting to render an invalid BG type\n");
			break;
		default:
			break;
	}
}

template<bool MOSAIC> void GPU::modeRender(int layer)
{
	// setFinalColorBck_funcNum (blend mode + window-in-use, set once per
	// BLDCNT/DISPCNT write) is loop-invariant for the whole scanline. Resolve
	// it once here and thread it into the BG renderers as a template constant,
	// so the per-pixel switch in setFinalColorBG() collapses to one blend path
	// instead of an indirect jump-table dispatch per pixel.
	// desmumewii-2d-compositor-plan.md Step 2.
	switch(setFinalColorBck_funcNum)
	{
		case 0x0: modeRenderT<MOSAIC,0x0>(this, layer); break;
		case 0x1: modeRenderT<MOSAIC,0x1>(this, layer); break;
		case 0x2: modeRenderT<MOSAIC,0x2>(this, layer); break;
		case 0x3: modeRenderT<MOSAIC,0x3>(this, layer); break;
		case 0x4: modeRenderT<MOSAIC,0x4>(this, layer); break;
		case 0x5: modeRenderT<MOSAIC,0x5>(this, layer); break;
		case 0x6: modeRenderT<MOSAIC,0x6>(this, layer); break;
		case 0x7: modeRenderT<MOSAIC,0x7>(this, layer); break;
		default:  modeRenderT<MOSAIC, -1>(this, layer); break; // out of range: legacy per-pixel dispatch
	}
}

void gpu_SetRotateScreen(u16 angle)
{
	gpu_angle = angle;
}
