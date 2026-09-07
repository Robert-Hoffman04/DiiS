/*
    Copyright (C) 2006,2007 DeSmuME Team
    Copyright (C) 2007 Pascal Giard (evilynux)
    Copyright (C) 2009 Yoshihiro (DsonPSP)
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
#include <stdio.h>
#include <unistd.h>
#include <fat.h>
#include <ogcsys.h>
#include <sdcard/wiisd_io.h>
#include <ogc/disc_io.h>
#include <sys/time.h>
#include <wiiuse/wpad.h>
#include <sys/dir.h>
#include <ogc/lwp_watchdog.h>
#include <sys/param.h>
#include "MMU.h"
#include "NDSSystem.h"
#include "cflash.h"
#include "sndogc.h"
#include "ctrlssdl.h"
#include "GPU.h"
#include "render3D.h"
#include "FrontEnd.h"
#include "version.h"
#include "log_console.h"
#include "GXRender.h"
#include "GXMerge.h"
#include "rasterize.h"

#ifdef DESMUME_ARMWRESTLER_PROBE
#include "addons.h"
#if defined(DESMUME_JIT_ARM7)
#include "jit/jit.h"
#endif
#endif

// See GXRender.cpp - same SD-card diagnostic log, used here to confirm/deny
// whether draw_thread keeps making progress while GXRender is on the core
// thread (i.e. whether the mergerom GX-core stall is GXRender itself wedged,
// starving draw_thread of vidmutex, vs. something in draw_thread). Throttled -
// draw_thread runs every vsync and we only need the first handful of frames.
#ifdef GXRENDER_DEBUG_LOG
#include <stdio.h>
static void gxdbg_main(const char *msg)
{
	static int n = 0;
	if (n >= 400) return;
	n++;
	FILE *f = fopen("sd:/gxdbg.log", "a");
	if (f) { fprintf(f, "[main] %s\n", msg); fclose(f); }
}
#define GXDBG_MAIN(msg) gxdbg_main(msg)
#else
#define GXDBG_MAIN(msg)
#endif
#include "filebrowser.h"
#include "gekko_utils/usb2storage.h"
#include "gekko_utils/mload.h"

#include <ogc/system.h>

#define NUM_FRAMES_TO_TIME 60
#define FPS_LIMITER_FRAME_PERIOD 8
#define DEFAULT_FIFO_SIZE (256*1024)

NDS_header * header;

GXRModeObj *rmode = NULL;
Mtx44 perspective;
Mtx GXmodelView2D;
unsigned int *xfb[2]; // Double framebuffer [frameBuffer[fb]]
int currfb;           // Current framebuffer (0 or 1)

static u8 gp_fifo[DEFAULT_FIFO_SIZE] __attribute__((aligned(32)));
static u16 TopScreen[256*192] __attribute__((aligned(32)));
static u16 BottomScreen[256*192] __attribute__((aligned(32)));

static GXTexObj TopTex;
static GXTexObj BottomTex;
static GXTexObj CursorTex;

// TODO: Make this fancier
static u16 CursorData[16] __attribute__((aligned(32))) = {
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
	0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
};
static int drawcursor = 1;
static lwp_t vidthread = LWP_THREAD_NULL;
mutex_t vidmutex = LWP_MUTEX_NULL;
static bool abort_thread = false;

static float nds_screen_size_ratio = 1.0f;
static u16 keypad;
static bool quit_game = false;
volatile bool execute = false;
static bool show_console = true;
static int SkipFrame = 0;
static int SkipFrameTracker = 0;
static u32 pad, wpad;

// Which rendering core we are using (SoftRast or GX)
u8 current3Dcore = 1;

SoundInterface_struct *SNDCoreList[] = {
	&SNDDummy,
	//&SNDFile,
	&SNDOGC,
	NULL
};

GPU3DInterface *core3DList[] = {
	&gpu3DNull,
	&gpu3Dgx,
	&gpu3DRasterize,
	NULL
};

//////////////////////////////////////////////////////////////////
////////////////////// FUNCTION PROTOTYPES ///////////////////////
//////////////////////////////////////////////////////////////////

void init();
void ShowCredits();
bool PickDevice();
static void Draw(void);
void ShowFPS();
void DSExec();
void Pause();
static void *draw_thread(void*);
void Execute();
void create_dummy_firmware();
bool CheckBios(bool);
static bool FindIOS(u32 ios);

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

#ifdef HW_RVL
static bool FindIOS(u32 ios)
{
	s32 ret;
	u32 n;
	
	u64 *titles = NULL;
	u32 num_titles=0;
	
	ret = ES_GetNumTitles(&num_titles);
	if (ret < 0)
		return false;
	
	if(num_titles < 1) 
		return false;
	
	titles = (u64 *)memalign(32, num_titles * sizeof(u64) + 32);
	if (!titles)
		return false;
	
	ret = ES_GetTitles(titles, num_titles);
	if (ret < 0)
	{
		free(titles);
		return false;
	}
	
	for(n=0; n < num_titles; n++)
	{
		if((titles[n] & 0xFFFFFFFF)==ios) 
		{
			free(titles); 
			return true;
		}
	}
	free(titles); 
	return false;
}
#endif

#ifdef __cplusplus
extern "C"
#endif

int main(int argc, char **argv){

	char filename[MAXPATHLEN];
	char *rom_filename = filename;
  
	init();

	log_console_init(rmode, 0, 20, 30, rmode->fbWidth - 40, rmode->xfbHeight - 60);
	log_console_enable_video(true);
	//log_console_enable_log(true);

	VIDEO_WaitVSync();
	
	bool device = PickDevice();
  
	VIDEO_WaitVSync();

	if(!device){
		fatUnmount("sd:/");
		__io_wiisd.shutdown();
		fatMountSimple("sd", &__io_wiisd);
		sprintf(rom_filename, "sd:/DS/ROMS");
	}
	else {
		fatUnmount("usb:/");
		for(int i = 0; i < 11; i++) {
			bool isMounted = fatMountSimple("usb", &__io_usbstorage);
			if (isMounted) break;
			sleep(1);
		}
		sprintf(rom_filename, "usb:/DS/ROMS");
	}

#ifdef DESMUME_FORCE_ROM
	// Hardcoded ROM path for automated testing (see Makefile TESTDEFS).
	strcpy(rom_filename, device ? "usb:/DS/ROMS/test.nds" : "sd:/DS/ROMS/test.nds");
#else
	if(FileBrowser(rom_filename) != 0)
		quit_game = true;
#endif

	cflash_disk_image_file = NULL;

#ifdef DESMUME_ARMWRESTLER_PROBE
	// Slot-2 = flat host RAM (see armwrestler_probe_tick() below) instead of
	// the default CFlash passthrough, before MMU_Init()/addonsInit() runs.
	addonsChangePak(NDS_ADDON_EXPMEMORY);
	// ExpMemory_reset() allocates expMemSize with `new` every NDS_Reset();
	// left at its 8MB default, a build with the JIT subsystem compiled in
	// (-DDESMUME_JIT_ARM7 -- the master flag, regardless of whether either
	// core's JIT is runtime-enabled) reliably freezes very early, before
	// even the first SMC-tracked memory access -- confirmed to be memory
	// exhaustion (the JIT's own arena/table allocations plus this 8MB
	// together overrun the Wii's MEM1), not a JIT execution bug: shrinking
	// this to what the probe actually uses (a few hundred bytes; 64 KB is
	// generous headroom) makes the freeze disappear outright, with no other
	// change. Left permanently small rather than only diagnostically --
	// there is no reason for this probe to ever want 8MB of slot-2 RAM.
	extern u32 expMemSize;
	expMemSize = 64 * 1024;
#if defined(DESMUME_JIT_ARM7)
	// armwrestler's ARM7 side (armwrestler-arm7.asm) is a one-instruction
	// stub -- `arm7_main: b arm7_main`, an intentional idle spin -- ARM7 is
	// never used for anything real here, so there is nothing for its JIT to
	// usefully compile. Leave it interpreted; free either way, and one less
	// variable when reading a result.
	jitArm7Enabled = false;
#endif
#endif

	printf("Initializing virtual Nintendo DS...\n");

	if (CheckBios(device)) // See if we have external bios files
		printf("Found external BIOS files.  Will Use!\n");
	else
		printf("No external BIOS files found.\n");

	// Initialize the DS!
	NDS_Init();
	create_dummy_firmware(); // Must do for some games!

	NDS_3D_ChangeCore(current3Dcore);

	// Hardware 3D/2D compositing path (see GXMerge.h).  Opt-in; GX core only.
	// Automated-test switch mirrors the DESMUME_FORCE_* pattern (see Makefile).
#ifdef DESMUME_FORCE_GXCOMPOSITE
	GXMerge_SetEnabled(current3Dcore == 1);
#endif

	printf("Initialization successful!\n");

	enable_sound = true;

	if ( enable_sound) {
		printf("Setting up for sound...\n");
		SPU_Init(SNDCORE_OGC, 768);	// audio samples count is 512 or 1024. Buffer is arg*2. 768*2 = 512*3.
	}
  
	printf("Placing ROM into virtual NDS...\n");
	if (NDS_LoadROM(rom_filename, cflash_disk_image_file) < 0) {
		printf("Error loading ROM\n");
		exit(0);
	}

	execute = true;

	log_console_enable_video(false);
	
	Execute();
	
	exit(0);
}

//////////////////////////////////////////////////////////////////
//////////////////////////// FUNCTIONS ///////////////////////////
//////////////////////////////////////////////////////////////////

void init(){
	u32 xfbHeight;
	f32 yscale;

	// Alpha 0, not 0xFF: GXRender.cpp's legacy compositor path depends on the
	// EFB clear alpha staying transparent for its entire life (see the long
	// comment in ReadFramebuffer()) - an opaque clear here would make every
	// pixel a 3D scene doesn't actually draw to read back as "3D content"
	// anyway once the clear used by 3D frames round-trips through
	// draw_thread's end-of-frame GX_CopyDisp(...,GX_TRUE), which re-clears
	// the EFB with this same global colour for the next 3D frame to draw
	// into. RGB is irrelevant to the final picture either way: the display
	// copy that actually reaches the screen always runs before whichever
	// clear prepares the EFB for next time.
	GXColor background = {0, 0, 0, 0};
	currfb = 0;

	// button initialization
	PAD_Init();
	WPAD_Init();
	GECKO_InputInit();   // USB Gecko / EXI debug-serial input routing (gekko_utils/geckoinput.h)
	VIDEO_Init();

	rmode = VIDEO_GetPreferredMode(NULL);

	switch (rmode->viTVMode >> 2)
	{
		case VI_NTSC: // 480 lines (NTSC 60hz)
			break;
		case VI_PAL: // 576 lines (PAL 50hz)
			rmode = &TVPal576IntDfScale;
			rmode->xfbHeight = 480;
			rmode->viYOrigin = (VI_MAX_HEIGHT_PAL - 480)/2;
			rmode->viHeight = 480;
			break;
		default: // 480 lines (PAL 60Hz)
			break;
	}

	
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(WPAD_CHAN_ALL,rmode->viWidth,rmode->viHeight);
	WPAD_SetIdleTimeout(200);

	VIDEO_Configure(rmode);

	xfb[0] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer (xfb[0]);

	VIDEO_SetBlack(FALSE);

	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	else while (VIDEO_GetNextField()) VIDEO_WaitVSync();

	memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);
	GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);

	GX_SetCopyClear(background, GX_MAX_Z24);
 
	// other gx setup
	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight,rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopySrc(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth,xfbHeight);
	GX_SetCopyFilter(rmode->aa,rmode->sample_pattern,GX_TRUE,rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,((rmode->viHeight==2*rmode->xfbHeight)?GX_ENABLE:GX_DISABLE));

	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[currfb],GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	guOrtho(perspective,0,479,0,639,0,300);
	GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

	guMtxIdentity(GXmodelView2D);
	guMtxTransApply (GXmodelView2D, GXmodelView2D, 0.0F, 0.0F, -5.0F);
	GX_LoadPosMtxImm(GXmodelView2D,GX_PNMTX0);

	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	GX_InvVtxCache();
	GX_ClearVtxDesc();
	GX_InvalidateTexAll();

	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);

	// In order to render the scene, we are taking all of the 
	// pixels and transforming them into a "texture" for the 
	// two quads that serve as our DS screens.
	GX_InitTexObj(&TopTex, TopScreen, 256, 192, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&BottomTex, BottomScreen, 256, 192, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObj(&CursorTex, CursorData, 4, 4, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);

	memset(TopScreen, 0, 256*192*sizeof(*TopScreen));
	memset(BottomScreen, 0, 256*192*sizeof(*BottomScreen));

	if (vidmutex == LWP_MUTEX_NULL)
		LWP_MutexInit(&vidmutex, false);

	GXMerge_Init();

	VIDEO_SetBlack(false);
}

#define RGB15_REVERSE(col) ( 0x8000 | (((col) & 0x001F) << 10) | ((col) & 0x03E0)  | (((col) & 0x7C00) >> 10) )

static void Draw(void) {
	// convert to 4x4 textels for GX
	u16 *sTop = (u16*)&GPU_screen;
	u16 *sBottom = sTop+256*192;
	u16 *dTop = TopScreen;
	u16 *dBottom = BottomScreen;
	LWP_MutexLock(vidmutex);

	for (int y = 0; y < 48; y++) {
		for (int h = 0; h < 4; h++) {
			for (int x = 0; x < 64; x++) {
				for (int w = 0; w < 4; w++) {
					*dTop++ = RGB15_REVERSE(sTop[w]);
					*dBottom++ = RGB15_REVERSE(sBottom[w]);
				}
				dTop+=12;     // next tile
				dBottom+=12;
				sTop+=4;
				sBottom+=4;
			}
			dTop-=1020;     // next line
			dBottom-=1020;
		}
		dTop+=1008;       // next row
		dBottom+=1008;
	}

	DCFlushRange(TopScreen, 256*192*2);
	DCFlushRange(BottomScreen, 256*192*2);

	if (GXMerge_Enabled())
		GXMerge_Present();

	LWP_MutexUnlock(vidmutex);

	return;
}

static bool change_screen_layout = true;

static void do_screen_layout()
{
	change_screen_layout = false;

	if(++screen_layout >= SCREEN_MAX)
		screen_layout = SCREEN_VERT_NORMAL;

	switch(screen_layout) 
	{
		case SCREEN_HORI_NORMAL:
			// not scaled

			topX = 	int((rmode->viWidth /2) - ((width*2) / 2));
			topY = 	int((rmode->viHeight /2) - (height / 2));
			bottomX = topX + width;
			bottomY = topY;
			scaley = scalex = 1.0;
			break;

		case SCREEN_HORI_STRETCH:
			//scaled

			scalex = float(rmode->viWidth / (width*2.0f));
			scaley = float(rmode->viHeight / height);
			topX = topY = 0;
			bottomY = 0;
			bottomX = topX + width;
			break;

		case SCREEN_VERT_NORMAL:
			// normal
			topX = 	int((rmode->viWidth / 2) - (width / 2.0f));
			topY = 	int((rmode->viHeight / 2) - ((height * 2.0f) / 2));
			bottomX = topX;
			bottomY = topY+height;
			scaley = scalex = 1.0;
			break;

        case SCREEN_VERT_SEPARATED:
			// normal
			topX =     int((rmode->viWidth / 2) - (width / 2.0f));
			topY =     int((rmode->viHeight / 2) - ((height * 2.0f) / 2) - 24);
			bottomX = topX;
			bottomY = topY+height+48;
			scaley = scalex = 1.0;
			break;

		case SCREEN_VERT_STRETCH:
			// stretched
			topX = topY = 0;
			bottomX = 0;
			scalex = float(rmode->viWidth / (width));
			scaley = float(rmode->viHeight / (height*2));
			bottomY = height;
			break;

		case SCREEN_MAIN_STRETCH:
		case SCREEN_SUB_STRETCH:
			topX = topY = 0;
			bottomX = bottomY = 0;
			scalex = float(rmode->viWidth / (width));
			scaley = float(rmode->viHeight / (height));
			break;

		case SCREEN_MAIN_NORMAL: 
		case SCREEN_SUB_NORMAL:
			topX = bottomX = int((rmode->viWidth /2) - (width / 2));
			topY = bottomY = int((rmode->viHeight /2) - (height / 2));
			scaley = scalex = 1.0;
			break;

		case SCREEN_VERT_SEPARATED_ROT_90:
			topX =    int((rmode->viWidth / 2) - (width / 2.0f));
			topY =    int((rmode->viHeight / 2) - ((height * 2.0f) / 2) - 24);
			bottomX = topX;
			bottomY = topY+height+48;
			scaley = scalex = 1.0;
			break;
	}
};

static void *draw_thread(void*){

	while(1){
		if (abort_thread)
			break;

		if(change_screen_layout)	// call it only when necessary.
			do_screen_layout();

		GXDBG_MAIN("draw_thread: waiting for vidmutex");
		LWP_MutexLock(vidmutex);
		GXDBG_MAIN("draw_thread: vidmutex acquired");

		// GXRender leaves the EFB in GX_PF_RGBA6_Z24 and never restores it; the
		// hardware-merge sandwich wants a plain RGB8 EFB.  Only switch it for a
		// present that actually runs the sandwich - otherwise match the legacy
		// path, which presents through the RGBA6 EFB GXRender left behind.
		if (GXMerge_HasPresentFrame()) {
			GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
			GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
		}

		// Transform for scaling and rotate

		Mtx m, m1, m2, mv;

		guMtxIdentity (m1);
		guMtxScaleApply(m1, m1, scalex, scaley, 1.0f);
		
		guVector axis =(guVector){0, 0, 1};
		guMtxRotAxisDeg (m2, &axis, rotate_angle);
		guMtxConcat(m2, m1, m);

		guMtxTransApply(m, m, 0, 0, 0);
		guMtxConcat (GXmodelView2D, m, mv);

		GX_LoadPosMtxImm (mv, GX_PNMTX0);

		GXDBG_MAIN("draw_thread: matrix set up");

		// TOP SCREEN
		if ((screen_layout != SCREEN_SUB_NORMAL) && (screen_layout != SCREEN_SUB_STRETCH)){
			GXDBG_MAIN("draw_thread: top screen quad start");
			GX_LoadTexObj(&TopTex, GX_TEXMAP0);
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(topX, topY);
				GX_TexCoord2f32(0, 0);
				GX_Position2f32(topX, topY+height);
				GX_TexCoord2f32(0, 1);
				GX_Position2f32(topX+width, topY+height);
				GX_TexCoord2f32(1, 1);
				GX_Position2f32(topX+width, topY);
				GX_TexCoord2f32(1, 0);
			GX_End();
			GXDBG_MAIN("draw_thread: top screen quad end");

			// TopTex is now the "behind" bucket in merge mode; overlay the 3D
			// bands and the front bucket on top of it.
			if (GXMerge_Enabled() && MainScreen.offset == 0) {
				if (GXMerge_HasPresentFrame())
					GXMerge_DrawMainScreen(topX, topY, width, height);
				GXMerge_DrawStatusMarker(topX, topY, width, height);
			}
		}
		// BOTTOM SCREEN
		if (screen_layout != SCREEN_MAIN_NORMAL && (screen_layout != SCREEN_MAIN_STRETCH)){
			GXDBG_MAIN("draw_thread: bottom screen quad start");
			GX_LoadTexObj(&BottomTex, GX_TEXMAP0);
			GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
				GX_Position2f32(bottomX, bottomY);
				GX_TexCoord2f32(0, 0);
				GX_Position2f32(bottomX, bottomY+height);
				GX_TexCoord2f32(0, 1);
				GX_Position2f32(bottomX+width, bottomY+height);
				GX_TexCoord2f32(1, 1);
				GX_Position2f32(bottomX+width, bottomY);
				GX_TexCoord2f32(1, 0);
			GX_End();
			GXDBG_MAIN("draw_thread: bottom screen quad end");

			if (GXMerge_Enabled() && MainScreen.offset != 0) {
				if (GXMerge_HasPresentFrame())
					GXMerge_DrawMainScreen(bottomX, bottomY, width, height);
				GXMerge_DrawStatusMarker(bottomX, bottomY, width, height);
			}

			// CURSOR
			if (drawcursor){
				GXDBG_MAIN("draw_thread: cursor quad start");
				GX_LoadTexObj(&CursorTex, GX_TEXMAP0);
				GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
					GX_Position2f32(bottomX+cursor.x-5, bottomY+cursor.y-5);
					GX_TexCoord2f32(0, 0);
					GX_Position2f32(bottomX+cursor.x-5, bottomY+cursor.y+5);
					GX_TexCoord2f32(0, 1);
					GX_Position2f32(bottomX+cursor.x+5, bottomY+cursor.y+5);
					GX_TexCoord2f32(1, 1);
					GX_Position2f32(bottomX+cursor.x+5, bottomY+cursor.y-5);
					GX_TexCoord2f32(1, 0);
				GX_End();
				GXDBG_MAIN("draw_thread: cursor quad end");
			}
		}

		GXDBG_MAIN("draw_thread: calling GX_DrawDone");
		GX_DrawDone();
		GXDBG_MAIN("draw_thread: GX_DrawDone returned");

		currfb ^= 1;

		GX_CopyDisp(xfb[currfb],GX_TRUE);
		GXDBG_MAIN("draw_thread: GX_CopyDisp done");
		VIDEO_SetNextFramebuffer(xfb[currfb]);
		VIDEO_Flush();
		GXDBG_MAIN("draw_thread: VIDEO_Flush done");

		LWP_MutexUnlock(vidmutex);

		VIDEO_WaitVSync();
		GXDBG_MAIN("draw_thread: VIDEO_WaitVSync done, loop end");
	}

	return NULL;
}

void Execute() {
	if(vidthread == LWP_THREAD_NULL)
		LWP_CreateThread(&vidthread, draw_thread, NULL, NULL, 0, 67);

	while(!quit_game){
		 
		if(SkipFrameTracker) NDS_SkipNextFrame(); 
	
		DSExec();

		SkipFrameTracker++;
		
		if(SkipFrameTracker > SkipFrame) SkipFrameTracker = 0;
		
	}
	
	/*
	int sys1 = SYS_GetArena1Size();
	int sys2 = SYS_GetArena2Size();
	//for(int i = 0; i < 500; i++)
	display_mallinfo();
	//
	printf("\n\n SYS1: %d || SYS2: %d", sys1,sys2);
	sleep(1);
	//*/
	

	abort_thread = true;
	LWP_MutexDestroy(vidmutex);
	vidmutex = LWP_MUTEX_NULL;
	LWP_JoinThread(vidthread, NULL);
	vidthread = LWP_THREAD_NULL;

	GXMerge_Deinit();

	NDS_DeInit();

	GX_AbortFrame();
	GX_Flush();

	VIDEO_Flush();
	VIDEO_WaitVSync();
	VIDEO_SetBlack(true);

	return;
}

void ShowFPS() {
	u32 fps_timing = 0;
	u32 fps_frame_counter = 0;
	u32 fps_previous_time = 0;
	u32 fps_temp_time;
	float fps;

	fps_frame_counter += 1;
	fps_temp_time = ticks_to_millisecs(gettime());
	fps_timing += fps_temp_time - fps_previous_time;
	fps_previous_time = fps_temp_time;

	if ( fps_frame_counter == NUM_FRAMES_TO_TIME) {
		fps = (float)fps_timing;
		fps /= NUM_FRAMES_TO_TIME * 1000.f;
		fps = 1.0f / fps;
		fps_frame_counter = 0;
		fps_timing = 0;
		
	}
}

#ifdef DESMUME_BENCH
//---------------------------------------------------------------------------
// Renderer runtime benchmark (-DDESMUME_BENCH).
//
// Measures how many NDS frames the emulator actually completes per second of
// real (Wii timebase) wall-clock time - i.e. emulation speed relative to a
// real DS's 59.8261 Hz.  Nothing here throttles: Execute() already runs
// DSExec() back to back with no frame limiter, so the rate we log is the
// honest "as fast as this build can go" rate for whatever scene is on screen.
//
// Every BENCH_BLOCK frames a CSV row is appended to sd:/bench.log:
//   frame,wall_us,block_us,exec_us,draw_us
//     frame     - NDS frame number at end of block
//     wall_us   - cumulative us since the first benched frame
//     block_us  - wall us for this block of BENCH_BLOCK frames
//     exec_us   - us spent in NDS_exec() over the block
//     draw_us   - us spent in Draw()  over the block
// After BENCH_FRAMES frames a SUMMARY row is written and the game quits so the
// Dolphin process can be reaped cleanly.
//---------------------------------------------------------------------------
#include <stdio.h>
#include <ogc/lwp_watchdog.h>

#ifndef DESMUME_BENCH_FRAMES
#define DESMUME_BENCH_FRAMES 2400
#endif
#ifndef DESMUME_BENCH_BLOCK
#define DESMUME_BENCH_BLOCK 60
#endif

static void bench_tick(u64 exec_ticks, u64 draw_ticks)
{
	static bool  started = false;
	static u64   t_first = 0;
	static u64   t_block = 0;
	static u32   frame   = 0;
	static u64   acc_exec = 0;
	static u64   acc_draw = 0;

	u64 now = gettime();

	if (!started) {
		started = true;
		t_first = now;
		t_block = now;
		FILE *f = fopen("sd:/bench.log", "w");
		if (f) {
			fprintf(f, "# desmumewii bench  core=%d  gxmerge=%d  target_hz=59.8261\n",
			        (int)current3Dcore, (int)GXMerge_Enabled());
			fprintf(f, "frame,wall_us,block_us,exec_us,draw_us\n");
			fclose(f);
		}
	}

	frame++;
	acc_exec += ticks_to_microsecs(exec_ticks);
	acc_draw += ticks_to_microsecs(draw_ticks);

	if (frame % DESMUME_BENCH_BLOCK == 0) {
		u64 block_us = ticks_to_microsecs(now - t_block);
		u64 wall_us  = ticks_to_microsecs(now - t_first);
		FILE *f = fopen("sd:/bench.log", "a");
		if (f) {
			fprintf(f, "%u,%llu,%llu,%llu,%llu\n",
			        frame, (unsigned long long)wall_us, (unsigned long long)block_us,
			        (unsigned long long)acc_exec, (unsigned long long)acc_draw);
			fclose(f);
		}
		t_block  = now;
		acc_exec = 0;
		acc_draw = 0;
	}

	if (frame >= DESMUME_BENCH_FRAMES) {
		u64 wall_us = ticks_to_microsecs(now - t_first);
		double secs = wall_us / 1000000.0;
		double fps  = secs > 0.0 ? frame / secs : 0.0;
		FILE *f = fopen("sd:/bench.log", "a");
		if (f) {
			fprintf(f, "# SUMMARY frames=%u wall_s=%.3f eff_fps=%.3f pct_realtime=%.1f slowdown=%.2fx\n",
			        frame, secs, fps, 100.0 * fps / 59.8261,
			        fps > 0.0 ? 59.8261 / fps : 0.0);
			fclose(f);
		}
		quit_game = true;
	}
}
#endif // DESMUME_BENCH

#ifdef DESMUME_ARMWRESTLER_PROBE
//---------------------------------------------------------------------------
// §17 armwrestler gate (-DDESMUME_ARMWRESTLER_PROBE).
//
// The patched armwrestler ROM (see the plan doc / tools/) auto-runs every
// ARM9 ARM and THUMB CPU test with no input and harvests pass/fail results
// into slot-2 expansion RAM (0x09000000+, the "Memory Expansion Pak" addon --
// see addonsChangePak() below) rather than only drawing them to a screen no
// headless run can see. expMemory (addons/expMemory.cpp) backs that guest
// range with flat host RAM 1:1 (ExpMemory_write32 at guest 0x09000000+N is
// exactly expMemory[N]), so the header/counts are readable directly; the
// per-failure log stores each test's *name string pointer* as a live ARM9
// address (it points into the loaded ROM image, ordinary guest memory), so
// pulling the readable name back out goes through _MMU_read08<ARMCPU_ARM9>
// like any other guest memory access.
//
// Polled once per frame from DSExec(); "AWR1" (0x31525741 LE) is written by
// the ROM's auto-run driver only after every counter write has landed, so a
// single sentinel check per frame can't observe a half-written header.
//---------------------------------------------------------------------------
extern u8* expMemory;

// expMemory is guest (little-endian ARM9) memory; the Wii host is
// big-endian, so a raw memcpy into a u32 reads the bytes in the wrong order.
// The guest itself writes correctly (ARM STR is just a byte-addressable
// store) -- this is purely about reassembling those bytes on this host.
static inline u32 le32(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// Dolphin's own SD-card emulation appears to cache writes and only commit
// them through to the backing host .raw file periodically, not synchronously
// on the guest's fclose() -- a single late write followed immediately by
// killing the process can leave the directory entry allocated but the data
// clusters unflushed (reads back as erased 0xFF). Reopen-and-rewrite the
// same file every ~30 frames for a while after the result is known, instead
// of writing once and quitting immediately, so there are several chances to
// land inside whatever flush cadence Dolphin actually uses; quit only after
// that grace window.
static char s_awLine[2048];
static int  s_awLineLen = 0;
static u32  s_awQuitAtFrame = 0;

static void armwrestler_probe_tick()
{
	static bool haveResult = false;
	static u32  frame = 0;
	if (quit_game) return;
	frame++;

	if (!haveResult) {
		u32 sentinel = expMemory ? le32(expMemory) : 0;
		if (sentinel != 0x31525741u && frame < 600) return;   // not ready yet

		char* p = s_awLine;
		char* end = s_awLine + sizeof(s_awLine);
		if (!expMemory) {
			p += snprintf(p, end - p, "[armwrestler] expMemory is NULL (addon not selected) at frame %u\n", frame);
		} else if (sentinel != 0x31525741u) {
			u32 armTotal = le32(expMemory + 0x04);
			u32 armFail  = le32(expMemory + 0x08);
			u32 tmbTotal = le32(expMemory + 0x0C);
			u32 tmbFail  = le32(expMemory + 0x10);
			p += snprintf(p, end - p, "[armwrestler] TIMEOUT at frame %u, sentinel=0x%08x (want 0x31525741)\n", frame, sentinel);
			p += snprintf(p, end - p, "  partial: ARM %u/%u fail, THUMB %u/%u fail\n", armFail, armTotal, tmbFail, tmbTotal);
		} else {
			u32 armTotal = le32(expMemory + 0x04);
			u32 armFail  = le32(expMemory + 0x08);
			u32 tmbTotal = le32(expMemory + 0x0C);
			u32 tmbFail  = le32(expMemory + 0x10);
			u32 logCount = le32(expMemory + 0x14);
			if (logCount > 32) logCount = 32;
			p += snprintf(p, end - p, "[armwrestler] ARM %u/%u fail, THUMB %u/%u fail\n",
			              armFail, armTotal, tmbFail, tmbTotal);
			for (u32 i = 0; i < logCount && end - p > 48; i++) {
				u32 namePtr = le32(expMemory + 0x18 + i * 8);
				u32 mask    = le32(expMemory + 0x18 + i * 8 + 4);
				char name[32]; u32 n = 0;
				while (n < sizeof(name) - 1) {
					u8 c = _MMU_read08<ARMCPU_ARM9>(namePtr + n);
					if (!c) break;
					name[n++] = (char)c;
				}
				name[n] = 0;
				p += snprintf(p, end - p, "  FAIL %-8s mask=0x%08x nameptr=0x%08x\n", name, mask, namePtr);
			}
		}
		s_awLineLen = (int)(p - s_awLine);
		haveResult = true;
		s_awQuitAtFrame = frame + 600;   // ~10s of grace at 60fps before quitting
	}

	if ((frame & 31) == 0 || frame >= s_awQuitAtFrame) {
		FILE* f = fopen("sd:/armwrestler.log", "w");
		if (f) { fwrite(s_awLine, 1, (size_t)s_awLineLen, f); fclose(f); }
	}
	if (frame >= s_awQuitAtFrame) quit_game = true;
}
#endif // DESMUME_ARMWRESTLER_PROBE

void DSExec(){

	PAD_ScanPads();
	WPAD_ScanPads();
	
	wpad = WPAD_ButtonsDown(WPAD_CHAN_0);
	pad = PAD_ButtonsDown(0);

	process_ctrls_event(&keypad, nds_screen_size_ratio);

	// process_ctrls_event() has just polled the USB Gecko debug-serial channel;
	// fold its edge events into `pad` too so the emulator-level controls below
	// (console toggle, layout, GXMerge A/B toggle, ...) are drivable over it.
	pad |= GECKO_ButtonsDown();
	
	// Update cursor position and click
	if(cursor.down) {
		NDS_setTouchPos(cursor.x, cursor.y);//ir.x, ir.y
	}
	
	if(cursor.click){ 
		NDS_releaseTouch();
		cursor.click = false;
	}

	update_keypad(keypad);     /* Update keypad */

	if ((wpad & WPAD_BUTTON_1) || (pad & PAD_BUTTON_LEFT)){
		show_console = !show_console;
		log_console_enable_video(show_console);
	}
	
	if ((wpad & WPAD_BUTTON_2) || (pad & PAD_BUTTON_UP)){
		change_screen_layout = true;
	}
	
	if ((wpad & WPAD_BUTTON_B) || (pad & PAD_BUTTON_RIGHT)){
		drawcursor ^= 1;
	}

	// A/B toggle for the hardware 3D/2D merge path (GC D-pad Down; GX core only).
	if (pad & PAD_BUTTON_DOWN){
		if (current3Dcore == 1)
			GXMerge_SetEnabled(!GXMerge_Enabled());
	}

#ifdef GPU_DISPCAP_DEBUG_LOG
	// Dump the DISPCAPCNT/offset ring buffer (GPU.cpp) - GC L trigger for an
	// interactive session, or automatically every ~5s of NDS execution
	// (overwriting the same file) so a capture centered on any visually-
	// spotted glitch is never more than a few seconds stale - no working
	// controller input needed at all; just pull the file right after seeing
	// the glitch on screen. See GPU_DispCapDumpRing().
	{
		static u32 execFrames = 0;
		execFrames++;
		if ((pad & PAD_TRIGGER_L) || (execFrames % (5*60) == 0)) {
			GPU_DispCapDumpRing();
		}
	}
#endif

	if (wpad & WPAD_BUTTON_PLUS)
		SkipFrame++;
	
	if (wpad &WPAD_BUTTON_MINUS){
		SkipFrame--;
		
		if(SkipFrame < 0)
			SkipFrame = 0;
	}

	if(	(wpad & WPAD_BUTTON_HOME) || ((pad & PAD_TRIGGER_Z) && (pad  & PAD_TRIGGER_R) && (pad & PAD_TRIGGER_L)) || 
		(wpad & WPAD_CLASSIC_BUTTON_HOME))
		quit_game = true;

#ifdef DESMUME_BENCH
	u64 _b0 = gettime();
	NDS_exec<TRUE>(0);
	u64 _b1 = gettime();
	if (!SkipFrameTracker) Draw();
	u64 _b2 = gettime();
	bench_tick(_b1 - _b0, _b2 - _b1);
#else
	NDS_exec<TRUE>(0);

	if (!SkipFrameTracker) Draw(); // only update when !Frame skip tracker
#endif

#ifdef DESMUME_ARMWRESTLER_PROBE
	armwrestler_probe_tick();
#endif

	if(showfps) ShowFPS();
}

void Pause(){
	for(;;){
		WPAD_ScanPads();
		if(WPAD_ButtonsDown(WPAD_CHAN_0)&WPAD_BUTTON_A)
			break;
	}
}

bool PickDevice(){
	bool device = false;
	bool useGX = false;
	current3Dcore = 2; //Soft Raster

#ifdef DESMUME_FORCE_CORE
	// Hardcoded selection for automated testing (see Makefile TESTDEFS).
	// DESMUME_FORCE_CORE: 1 = GX, 2 = software raster
	// DESMUME_FORCE_USB:  0 = SD, 1 = USB
	current3Dcore = DESMUME_FORCE_CORE;
#ifdef DESMUME_FORCE_USB
	return DESMUME_FORCE_USB;
#else
	return false;
#endif
#endif

	while(true){
		PAD_ScanPads();
		WPAD_ScanPads();

		printf("\x1b[2J");
		printf("\x1b[2;0H");
		printf("Welcome to DeSmuME Wii!!!\n\n");
		printf("Select Device: << ");
		printf("%s", device ? "USB >>" : "SD >>");
		printf("\nSelect Renderer \\/ ");
		printf("%s", useGX ? " GX  /\\" : "Soft /\\");
		printf("\n\nPress B to see the credits.");

		//
		//
		//--DCN: This is so I don't have to do this every time I test
		/*
		{
		current3Dcore = 1;
		break;
		}
		//*/
		//
		//
		//
		
		if(GetInput(LEFT, LEFT, LEFT) || GetInput(RIGHT, RIGHT, RIGHT)) {
			device = !device;
		}

		if(GetInput(UP, UP, UP) || GetInput(DOWN, DOWN, DOWN)) {
			useGX = !useGX;
		}

		if(GetInput(A, A, A)){
			if(useGX){
				current3Dcore = 1; // We want to use GX!
			}
			break;
		}
			
		if(GetInput(B, B, B))
		    ShowCredits();
			
		VIDEO_WaitVSync();
	}

	return device;
}

void ShowCredits() {

	printf("\x1b[2J");
	printf("\x1b[2;0H");
	
	printf("DeSmuME Wii\n\n");
	printf("http://code.google.com/p/desmumewii\n\n");
	printf("Written By:\n\n");
	printf("Arikado - http://arikadosblog.blogspot.com\n");
	printf("scanff\n");
	printf("DCN\n");	
	printf("firnis\n");
	printf("baby.lueshi\n");
	printf("With contributions from Cyan\n\n");

	printf("Press A to return to the menu.");
	
	while(true){
	    PAD_ScanPads();
		WPAD_ScanPads();
		if(GetInput(A, A, A))
		    break;
	}

}

//needed for some games
void create_dummy_firmware(){
	// Create the dummy firmware
	NDS_fw_config_data dummy;
	
	NDS_FillDefaultFirmwareConfigData(&dummy);

	NDS_CreateDummyFirmware( &dummy);
}

/*
	As we don't have a menu right now this function is used to see if the user
	has external bios files.  If they do we mark them to be used
*/
bool CheckBios(bool device){
	char path[256] = {0};

	if (!device) strcat(path,"sd:/DS/BIOS/");
	else strcat(path,"usb:/DS/BIOS/");

	FILE* biosfile = 0;

	// Check arm7 bios
	sprintf(CommonSettings.ARM7BIOS,"%sbiosnds7.rom",path);

	biosfile = fopen(CommonSettings.ARM7BIOS,"rb");
	if (!biosfile){
		printf("No ARM7 BIOS\n");
		memset(CommonSettings.ARM7BIOS,0,256);
		return false;		
	}

	fclose(biosfile);
	biosfile = 0;

	// Check arm9 bios
	sprintf(CommonSettings.ARM9BIOS,"%sbiosnds9.rom",path);

	biosfile = fopen(CommonSettings.ARM9BIOS,"rb");
	if (!biosfile){
		printf("No ARM9 BIOS\n");
		memset(CommonSettings.ARM9BIOS,0,256);
		return false;		
	}

	fclose(biosfile);
	biosfile = NULL;

	CommonSettings.UseExtBIOS = true;

	return true;
}
