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
#include "GX2DBG.h"
#include "saves.h"
#include "rasterize.h"
#include "perf_zones.h"
#include "fps_overlay.h"
#include "harness/harness.h"

#ifdef DESMUME_ARMWRESTLER_PROBE
#include "addons.h"
#if defined(DESMUME_JIT_ARM7)
#include "jit/jit.h"
#endif
#endif

#ifdef DESMUME_ARM7WRESTLER_PROBE
#include "addons.h"
#if defined(DESMUME_JIT_ARM7)
#include "jit/jit.h"
#endif
#endif

#ifdef DESMUME_ROCKWRESTLER_PROBE
#include "addons.h"
#endif

#ifdef DESMUME_GBA_APU_SOAK
#include "addons.h"
#include "gba_apu.h"
#endif

#ifdef DESMUME_GBA_IRQ_SOAK
#include "addons.h"
#endif

#ifdef DESMUME_GBA_WAITCNT_SOAK
#include "addons.h"
#endif

#ifdef DESMUME_GBA_DMA_SOAK
#include "addons.h"
#endif

#ifdef DESMUME_GBA_SAVESTATE_SOAK
// PLAN.md §4.3 item 7 / item 9's gba-conformance sstest: addonsChangePak()/
// NDS_ADDON_NONE below (this soak's own CFlash-boot-hang sidestep, same as
// its siblings above) needs this declared -- missing here until the
// gba-conformance harness's first real build of this flag combo caught it
// (a plain compile error, not a behavior bug: every prior session that
// exercised DESMUME_GBA_SAVESTATE_SOAK apparently did so incrementally
// against a tree where another *_SOAK flag was still also defined and had
// already pulled this header in).
#include "addons.h"
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
#elif defined(DESMUME_HARNESS) && defined(HARNESS_BOOT)
	// §3.4 boot manifest: sd:/harness.cfg (or usb:/) with >=1 rom= line drives
	// the playlist; no manifest -> fall through to the FileBrowser path.
	if (harness_boot_load(device ? "usb:" : "sd:") > 0) {
		strncpy(rom_filename, harness_boot_rom_path(), MAXPATHLEN - 1);
		rom_filename[MAXPATHLEN - 1] = 0;
		if (harness_boot_core() > 0)
			current3Dcore = (u8)harness_boot_core();
		printf("harness: manifest %d ROM(s), frames_per_rom=%lu, core=%d\n",
			harness_boot_rom_count(),
			(unsigned long)harness_boot_frames_per_rom(), (int)current3Dcore);
	} else if(FileBrowser(rom_filename) != 0) {
		quit_game = true;
	}
#else
	if(FileBrowser(rom_filename) != 0)
		quit_game = true;
#endif

	// Harness transport bring-up (plan §3.1). Probes NET -> GECKO -> SD (or a
	// pinned backend). No-op without -DDESMUME_HARNESS; on failure it disables
	// itself and boot continues.
	harness_transport_init();

	// §3.6: install the PPC exception panic hook now that the transport is up,
	// so any later trap ships a PKT_CRASH register dump + backtrace before the
	// machine halts. No-op without -DDESMUME_HARNESS / -DHARNESS_CRASH.
	harness_crash_init();

	cflash_disk_image_file = NULL;

#ifdef DESMUME_GBA_IRQ_SOAK
	// PLAN.md §4.3 item 6 tail: sustained ARM7-JIT-vs-GBA-IRQ soak probe.
	// The default CFlash slot-2 passthrough (addonsInit()'s default pak)
	// has a known, separately-documented boot hang when a GBA ROM is
	// staged as sd:/DS/ROMS/test.nds under -DDESMUME_FORCE_ROM (see
	// PLAN.md §4.3 item 8's "Dolphin/DeSmuME-side result" -- reproduced
	// twice, console frozen at "Using CFlash directory:", not root-caused
	// there). Sidestepped here the same way DESMUME_ARMWRESTLER_PROBE
	// above sidesteps it for its own unrelated reason: switch slot-2 to
	// NDS_ADDON_NONE before MMU_Init()/addonsInit() runs. This synthetic
	// ROM never touches slot-2 either way, so the addon choice is inert
	// to the test itself.
	addonsChangePak(NDS_ADDON_NONE);
#endif

#ifdef DESMUME_GBA_APU_SOAK
	// PLAN.md §4.3 item 3 (APU): same default-CFlash-slot-2 boot hang under
	// -DDESMUME_FORCE_ROM documented above for DESMUME_GBA_IRQ_SOAK applies
	// to any GBA ROM staged that way -- sidestep it the same way. The
	// companion synthetic ROM (tools/gba-refcheck/dsound.s) never touches
	// slot-2 either.
	addonsChangePak(NDS_ADDON_NONE);
#endif

#ifdef DESMUME_GBA_WAITCNT_SOAK
	// PLAN.md §4.3 item 4: same default-CFlash-slot-2 boot hang under
	// -DDESMUME_FORCE_ROM sidestepped the same way. The companion synthetic
	// ROM (tools/gba-refcheck/waitcnt.s) never touches slot-2 either.
	addonsChangePak(NDS_ADDON_NONE);
#endif

#ifdef DESMUME_GBA_DMA_SOAK
	// PLAN.md §4.3 item 2: same default-CFlash-slot-2 boot hang under
	// -DDESMUME_FORCE_ROM sidestepped the same way. The companion synthetic
	// ROM (tools/gba-refcheck/dmavcap.s) never touches slot-2 either.
	addonsChangePak(NDS_ADDON_NONE);
#endif

#ifdef DESMUME_GBA_SAVESTATE_SOAK
	// PLAN.md §4.3 item 7: same default-CFlash-slot-2 boot hang under
	// -DDESMUME_FORCE_ROM sidestepped the same way -- this soak reuses
	// irqsoak.gba, which never touches slot-2 either.
	addonsChangePak(NDS_ADDON_NONE);
#endif

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

#ifdef DESMUME_ARM7WRESTLER_PROBE
	// §18 arm7wrestler gate -- same slot-2 addon trick as §17's armwrestler
	// probe (see the block above), same 8MB-vs-64KB MEM1 exhaustion fix
	// applies here for the same reason.
	addonsChangePak(NDS_ADDON_EXPMEMORY);
	extern u32 expMemSize;
	expMemSize = 64 * 1024;
#if defined(DESMUME_JIT_ARM7)
	// Opposite of §17's armwrestler probe: here the ARM7 side is the real
	// test content and the ARM9 side (armwrestler-arm9.asm) is upstream's
	// own trivial idle/vram-copy stub -- nothing for the ARM9 JIT to
	// usefully compile, so turn that off. jitArm7Enabled is deliberately
	// left alone (defaults true whenever -DDESMUME_JIT_ARM7 is compiled
	// in) -- exercising the ARM7 JIT is the entire point of this probe; an
	// interpreter-only baseline run means building without
	// -DDESMUME_JIT_ARM7 at all, not forcing this flag off here.
	jitArm9Enabled = false;
#endif
#endif

#ifdef DESMUME_ROCKWRESTLER_PROBE
	// §19 rockwrestler gate -- same slot-2 addon trick as §17/§18, same
	// 8MB-vs-64KB MEM1 exhaustion fix applies here for the same reason.
	addonsChangePak(NDS_ADDON_EXPMEMORY);
	extern u32 expMemSize;
	expMemSize = 64 * 1024;
	// Unlike §17/§18: both CPUs are real content here (IPCSYNC/IPCFIFO/
	// WRAMCNT/VRAMCNT/TCM genuinely exercise ARM7+ARM9 together), so
	// jitArm7Enabled/jitArm9Enabled are deliberately left at their
	// defaults -- an interpreter-only baseline run means building without
	// -DDESMUME_JIT_ARM7 at all, not forcing either flag off here.
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

	// Hardware 3D/2D compositing path (see GXMerge.h).  Mandatory for the GX
	// core as of this branch, not opt-in: MAIN-screen visual A/B and the
	// SUB-screen sprite-compositing fix (see BUGS.md "Graphics") are both
	// verified, and the -61%/+45fps win means GX2DBG is the only 2D
	// compositor the GX core runs - there is no build flag or runtime
	// toggle to fall back to the CPU compositor while on the GX core (the
	// old DESMUME_FORCE_GXCOMPOSITE/DESMUME_FORCE_GX2DBG bench flags no
	// longer gate anything here; they're vestigial in the benchmark scripts
	// that still pass them). One call, not two: GXMerge_Set2DBG(true) turns
	// the base merge path on internally, there's nothing left to enable
	// separately.
	GXMerge_Set2DBG(current3Dcore == 1);

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

#if defined(DESMUME_HARNESS) && defined(HARNESS_BOOT)
	// §3.4: manifest-driven savestate jump for the first playlist ROM (later
	// ROMs get it inside harness_boot_advance()). -1 / no manifest -> skipped.
	if (harness_boot_autoload_slot() >= 0)
		loadstate_slot(harness_boot_autoload_slot());
	if (harness_boot_frame_every() > 0)
		harness_frame_set_every(harness_boot_frame_every());
#endif

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

	FPSOverlay_Init();

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

	{ PZ_SCOPE(PZ_DRAW_CONVERT);
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
	} // PZ_DRAW_CONVERT

	if (GXMerge_Enabled()) {
		PZ_SCOPE(PZ_DRAW_PRESENT);
		GXMerge_Present();
	}

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
			// Step 5.1a: SUB engine on top (main on bottom) -> 2D-BG bands here
			if (GXMerge_Enabled() && MainScreen.offset != 0)
				GXMerge_DrawSubScreen(topX, topY, width, height);
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
			// Step 5.1a: SUB engine on bottom (main on top) -> 2D-BG bands here
			if (GXMerge_Enabled() && MainScreen.offset == 0)
				GXMerge_DrawSubScreen(bottomX, bottomY, width, height);

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

		// On-screen FPS counter, top-left corner (outside the DS screen area).
		FPSOverlay_Draw();

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

#if defined(DESMUME_HARNESS) && defined(HARNESS_BOOT)
	// §3.4 multi-ROM playlist: advance after frames_per_rom or a host "next_rom",
	// reloading via NDS_LoadROM each time. #ifdef'd (not just stubbed) so a
	// release build keeps the original loop's codegen byte-for-byte.
	do {
		harness_boot_announce();
		u32 romBudget = harness_boot_frames_per_rom();
		u32 romFrame  = 0;

		while(!quit_game){
			if(SkipFrameTracker) NDS_SkipNextFrame();
			DSExec();
			SkipFrameTracker++;
			if(SkipFrameTracker > SkipFrame) SkipFrameTracker = 0;

			++romFrame;
			if(harness_boot_next_requested())      break;
			if(romBudget && romFrame >= romBudget) break;
		}
	} while(!quit_game && harness_boot_advance());
#else
	while(!quit_game){

		if(SkipFrameTracker) NDS_SkipNextFrame();

		DSExec();

		SkipFrameTracker++;

		if(SkipFrameTracker > SkipFrame) SkipFrameTracker = 0;

	}
#endif

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

	GX2DBG_Reset();
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

#ifdef JIT_MEM_ACCOUNT
	{ extern void jitMemAccountTick(u32 frame); jitMemAccountTick(frame); }
#endif

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

#ifdef DESMUME_ARM7WRESTLER_PROBE
//---------------------------------------------------------------------------
// §18 arm7wrestler gate (-DDESMUME_ARM7WRESTLER_PROBE).
//
// Mechanically identical to armwrestler_probe_tick() above (see its comment
// for the slot-2/ExpMemory mechanism) -- the two differences are: the
// per-failure log's name-string pointer is a live **ARM7** address here
// (this ROM's real test content runs on ARM7, not ARM9 -- see
// tools/arm7wrestler/PROVENANCE.md), so it's read back through
// _MMU_read08<ARMCPU_ARM7>; and the result file is sd:/arm7wrestler.log so
// the two probes' outputs never collide if both happen to be staged at once.
//---------------------------------------------------------------------------
extern u8* expMemory;

// Local copy of the armwrestler probe's le32() (that one is scoped inside
// -DDESMUME_ARMWRESTLER_PROBE, which this build may not have) -- same
// byte-reassembly reasoning, see that comment.
static inline u32 le32_7(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static char s_aw7Line[2048];
static int  s_aw7LineLen = 0;
static u32  s_aw7QuitAtFrame = 0;

static void arm7wrestler_probe_tick()
{
	static bool haveResult = false;
	static u32  frame = 0;
	if (quit_game) return;
	frame++;

	if (!haveResult) {
		u32 sentinel = expMemory ? le32_7(expMemory) : 0;
		if (sentinel != 0x31525741u && frame < 600) return;   // not ready yet

		char* p = s_aw7Line;
		char* end = s_aw7Line + sizeof(s_aw7Line);
		if (!expMemory) {
			p += snprintf(p, end - p, "[arm7wrestler] expMemory is NULL (addon not selected) at frame %u\n", frame);
		} else if (sentinel != 0x31525741u) {
			u32 armTotal = le32_7(expMemory + 0x04);
			u32 armFail  = le32_7(expMemory + 0x08);
			u32 tmbTotal = le32_7(expMemory + 0x0C);
			u32 tmbFail  = le32_7(expMemory + 0x10);
			p += snprintf(p, end - p, "[arm7wrestler] TIMEOUT at frame %u, sentinel=0x%08x (want 0x31525741)\n", frame, sentinel);
			p += snprintf(p, end - p, "  partial: ARM %u/%u fail, THUMB %u/%u fail\n", armFail, armTotal, tmbFail, tmbTotal);
		} else {
			u32 armTotal = le32_7(expMemory + 0x04);
			u32 armFail  = le32_7(expMemory + 0x08);
			u32 tmbTotal = le32_7(expMemory + 0x0C);
			u32 tmbFail  = le32_7(expMemory + 0x10);
			u32 logCount = le32_7(expMemory + 0x14);
			if (logCount > 32) logCount = 32;
			p += snprintf(p, end - p, "[arm7wrestler] ARM %u/%u fail, THUMB %u/%u fail\n",
			              armFail, armTotal, tmbFail, tmbTotal);
			for (u32 i = 0; i < logCount && end - p > 48; i++) {
				u32 namePtr = le32_7(expMemory + 0x18 + i * 8);
				u32 mask    = le32_7(expMemory + 0x18 + i * 8 + 4);
				char name[32]; u32 n = 0;
				while (n < sizeof(name) - 1) {
					u8 c = _MMU_read08<ARMCPU_ARM7>(namePtr + n);
					if (!c) break;
					name[n++] = (char)c;
				}
				name[n] = 0;
				p += snprintf(p, end - p, "  FAIL %-8s mask=0x%08x nameptr=0x%08x\n", name, mask, namePtr);
			}
		}
		s_aw7LineLen = (int)(p - s_aw7Line);
		haveResult = true;
		s_aw7QuitAtFrame = frame + 600;   // ~10s of grace at 60fps before quitting
	}

	if ((frame & 31) == 0 || frame >= s_aw7QuitAtFrame) {
		FILE* f = fopen("sd:/arm7wrestler.log", "w");
		if (f) { fwrite(s_aw7Line, 1, (size_t)s_aw7LineLen, f); fclose(f); }
	}
	if (frame >= s_aw7QuitAtFrame) quit_game = true;
}
#endif // DESMUME_ARM7WRESTLER_PROBE

#ifdef DESMUME_ROCKWRESTLER_PROBE
//---------------------------------------------------------------------------
// §19 RockWrestler gate (-DDESMUME_ROCKWRESTLER_PROBE).
//
// Same slot-2/ExpMemory mechanism as armwrestler_probe_tick()/
// arm7wrestler_probe_tick() above, with RockWrestler's own layout (see the
// autorun block in tools/rockwrestler/src9/framework/menu.cpp): a single
// unified running/failed count (RockWrestler has no ARM/THUMB split -- all
// tests here run in ARM state on ARM9) and a fail log whose name pointer
// is a live **ARM9** address, read back through _MMU_read08<ARMCPU_ARM9>
// like armwrestler's (not arm7wrestler's ARM7 one).
//---------------------------------------------------------------------------
extern u8* expMemory;

// Local copy of the other probes' le32() (each is scoped inside its own
// -DDESMUME_*_PROBE, which this build may not have) -- same byte-
// reassembly reasoning, see armwrestler_probe_tick()'s comment.
static inline u32 le32_rw(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static char s_rwLine[2048];
static int  s_rwLineLen = 0;
static u32  s_rwQuitAtFrame = 0;

static void rockwrestler_probe_tick()
{
	static bool haveResult = false;
	static u32  frame = 0;
	if (quit_game) return;
	frame++;

	if (!haveResult) {
		u32 sentinel = expMemory ? le32_rw(expMemory) : 0;
		if (sentinel != 0x31574B52u && frame < 600) return;   // not ready yet

		char* p = s_rwLine;
		char* end = s_rwLine + sizeof(s_rwLine);
		if (!expMemory) {
			p += snprintf(p, end - p, "[rockwrestler] expMemory is NULL (addon not selected) at frame %u\n", frame);
		} else if (sentinel != 0x31574B52u) {
			u32 total = le32_rw(expMemory + 0x04);
			u32 fail  = le32_rw(expMemory + 0x08);
			p += snprintf(p, end - p, "[rockwrestler] TIMEOUT at frame %u, sentinel=0x%08x (want 0x31574b52)\n", frame, sentinel);
			p += snprintf(p, end - p, "  partial: %u/%u fail\n", fail, total);
		} else {
			u32 total = le32_rw(expMemory + 0x04);
			u32 fail  = le32_rw(expMemory + 0x08);
			u32 logCount = le32_rw(expMemory + 0x0C);
			if (logCount > 32) logCount = 32;
			p += snprintf(p, end - p, "[rockwrestler] %u/%u fail\n", fail, total);
			for (u32 i = 0; i < logCount && end - p > 48; i++) {
				u32 namePtr = le32_rw(expMemory + 0x10 + i * 8);
				s32 detail  = (s32)le32_rw(expMemory + 0x10 + i * 8 + 4);
				char name[32]; u32 n = 0;
				while (n < sizeof(name) - 1) {
					u8 c = _MMU_read08<ARMCPU_ARM9>(namePtr + n);
					if (!c) break;
					name[n++] = (char)c;
				}
				name[n] = 0;
				p += snprintf(p, end - p, "  FAIL %-24s detail=0x%03x nameptr=0x%08x\n", name, (unsigned)detail, namePtr);
			}
		}
		s_rwLineLen = (int)(p - s_rwLine);
		haveResult = true;
		s_rwQuitAtFrame = frame + 600;   // ~10s of grace at 60fps before quitting
	}

	if ((frame & 31) == 0 || frame >= s_rwQuitAtFrame) {
		FILE* f = fopen("sd:/rockwrestler.log", "w");
		if (f) { fwrite(s_rwLine, 1, (size_t)s_rwLineLen, f); fclose(f); }
	}
	if (frame >= s_rwQuitAtFrame) quit_game = true;
}
#endif // DESMUME_ROCKWRESTLER_PROBE

#ifdef DESMUME_GBA_BOOT_PROBE
//---------------------------------------------------------------------------
// roadmap #20 (GBA compat) boot probe (-DDESMUME_GBA_BOOT_PROBE).
//
// Unlike the armwrestler-family probes above, a real commercial GBA ROM has
// no idea this emulator or its slot-2/ExpMemory reporting convention exist
// -- it can't cooperate. So this probe instruments the emulator itself:
// sampled once per frame, it tracks how far ARM7's PC has actually moved
// through real cartridge/EWRAM/IWRAM code (proof the interpreter is
// fetching and executing genuine GBA instructions across more than one
// address, not stuck or silently misrouted), plus a simple EWRAM+IWRAM
// content hash (proof real memory writes are landing -- BSS clears, stack
// setup, etc.), and whether PC ever reached GBA_BIOS (0x00000000-
// 0x00003FFF) -- which would mean the game hit an SWI and fell into the
// safe-but-inert real-SWI-trap path (see NDS_Reset()'s swi_tab override,
// NDSSystem.cpp) rather than real BIOS/HLE SWI content, since neither
// exists yet (§12.3 step 6).
//---------------------------------------------------------------------------
static u32 gbaFnv1a(const u8* buf, u32 n)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
	return h;
}

static u32  s_gbaSeenBios = 0, s_gbaSeenEwram = 0, s_gbaSeenIwram = 0, s_gbaSeenCart = 0, s_gbaSeenOther = 0;
static u32  s_gbaMinCartPC = 0xFFFFFFFFu, s_gbaMaxCartPC = 0;
static u32  s_gbaDistinctPages[64];
static u32  s_gbaDistinctPageCount = 0;
static bool s_gbaHaveResult = false;
static u32  s_gbaQuitAtFrame = 0;
static char s_gbaLine[4096];
static int  s_gbaLineLen = 0;

static void gba_boot_probe_tick()
{
	static u32 frame = 0;
	if (quit_game) return;
	frame++;

	u32 pc = NDS_ARM7.R[15];
	u32 bank = pc >> 24;
	if (bank == 0x00) s_gbaSeenBios++;
	else if (bank == 0x02) s_gbaSeenEwram++;
	else if (bank == 0x03) s_gbaSeenIwram++;
	else if (bank >= 0x08 && bank <= 0x0D) {
		s_gbaSeenCart++;
		if (pc < s_gbaMinCartPC) s_gbaMinCartPC = pc;
		if (pc > s_gbaMaxCartPC) s_gbaMaxCartPC = pc;
		u32 page = pc & ~0xFFFu;
		bool known = false;
		for (u32 i = 0; i < s_gbaDistinctPageCount; i++)
			if (s_gbaDistinctPages[i] == page) { known = true; break; }
		if (!known && s_gbaDistinctPageCount < 64) s_gbaDistinctPages[s_gbaDistinctPageCount++] = page;
	}
	else s_gbaSeenOther++;

	const u32 QUIT_FRAME = 600;   // ~10s at 60fps -- generous for a boot sequence
	if (!s_gbaHaveResult && frame >= QUIT_FRAME) {
		u32 ewramHash = gbaFnv1a(MMU.GBA_EWRAM, sizeof(MMU.GBA_EWRAM));
		u32 iwramHash = gbaFnv1a(MMU.GBA_IWRAM, sizeof(MMU.GBA_IWRAM));

		char* p = s_gbaLine;
		char* end = s_gbaLine + sizeof(s_gbaLine);
		p += snprintf(p, end - p, "[gba_boot] frames=%u finalPC=0x%08x CPSR=0x%08x mode=0x%02x T=%d\n",
		              frame, pc, NDS_ARM7.CPSR.val, NDS_ARM7.CPSR.bits.mode, NDS_ARM7.CPSR.bits.T);
		p += snprintf(p, end - p, "[gba_boot] samples: bios=%u ewram=%u iwram=%u cart=%u other=%u\n",
		              s_gbaSeenBios, s_gbaSeenEwram, s_gbaSeenIwram, s_gbaSeenCart, s_gbaSeenOther);
		p += snprintf(p, end - p, "[gba_boot] cart PC range: 0x%08x - 0x%08x, distinct 4K pages visited: %u\n",
		              s_gbaSeenCart ? s_gbaMinCartPC : 0, s_gbaSeenCart ? s_gbaMaxCartPC : 0, s_gbaDistinctPageCount);
		p += snprintf(p, end - p, "[gba_boot] EWRAM hash=0x%08x IWRAM hash=0x%08x\n", ewramHash, iwramHash);
		s_gbaLineLen = (int)(p - s_gbaLine);
		s_gbaHaveResult = true;
		s_gbaQuitAtFrame = frame + 60;   // brief grace window for the SD flush cadence
	}

	if (s_gbaHaveResult && ((frame & 15) == 0 || frame >= s_gbaQuitAtFrame)) {
		FILE* f = fopen("sd:/gba_boot.log", "w");
		if (f) { fwrite(s_gbaLine, 1, (size_t)s_gbaLineLen, f); fclose(f); }
	}
	if (s_gbaHaveResult && frame >= s_gbaQuitAtFrame) quit_game = true;
}
#endif // DESMUME_GBA_BOOT_PROBE

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
	// §3.5: same for the transport-agnostic remote-input core (network feed);
	// process_ctrls_event() above already advanced it. Self-stubs to nothing
	// without -DDESMUME_HARNESS -DHARNESS_INPUT.
	pad |= harness_input_down();
	
	// Update cursor position and click
	if(cursor.down) {
		NDS_setTouchPos(cursor.x, cursor.y);//ir.x, ir.y
	}
	
	if(cursor.click){ 
		NDS_releaseTouch();
		cursor.click = false;
	}

	// §3.5: deterministic keypad record/replay. Record appends this frame's
	// keypad; replay overwrites it. No-op without -DDESMUME_HARNESS -DHARNESS_INPUT
	// or when no movie is active.
	harness_input_movie_tick(&keypad);

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

	// DS-level quicksave/quickload (saves.cpp): Z held + L/R edge, fixed
	// slot 0. Deliberately NOT a rewind buffer -- this is a single on-demand
	// synchronous savestate_slot()/loadstate_slot() file write/read when the
	// chord fires, nothing runs every frame, so there's no per-frame cost.
	// The "held" side folds in GECKO_ButtonsHeld() so the chord is drivable
	// over the USB-Gecko debug-input channel (z byte held 6 frames, then L/R)
	// -- that's how the headless benchmark harness jumps a run to a savestate.
	if ((PAD_ButtonsHeld(0) | GECKO_ButtonsHeld()) & PAD_TRIGGER_Z) {
		if (pad & PAD_TRIGGER_L)
			savestate_slot(0);
		else if (pad & PAD_TRIGGER_R)
			loadstate_slot(0);
	}
#ifdef DESMUME_AUTOLOADSTATE
	// Headless benchmark savestate jump (-DDESMUME_AUTOLOADSTATE): rather than
	// direct-boot + a frame window chosen to sit past the intro, load a
	// savestate slot exactly once, early, so every benched frame is real
	// in-game content from a fixed known point. The benchmark harness stages
	// the state file as sd:/DS/SAVES/test.ds<slot> (tools/benchmark/benchmark.sh);
	// a missing file makes loadstate_slot() a clean no-op, so a bench dol built
	// with this flag still runs scenes that ship no state. The load waits a few
	// frames so NDS_LoadROM()/direct boot has fully brought up the MMU before
	// the NDS_Reset() inside savestate_load() runs.
	#ifndef DESMUME_AUTOLOADSTATE_FRAME
	#define DESMUME_AUTOLOADSTATE_FRAME 90
	#endif
	#ifndef DESMUME_AUTOLOADSTATE_SLOT
	#define DESMUME_AUTOLOADSTATE_SLOT 0
	#endif
	{
		static unsigned alsFrame = 0;
		static bool alsDone = false;
		if (!alsDone && alsFrame++ >= (unsigned)DESMUME_AUTOLOADSTATE_FRAME) {
			alsDone = true;
			loadstate_slot(DESMUME_AUTOLOADSTATE_SLOT);
		}
	}
#endif
#ifdef DESMUME_SAVESTATE_DIAG
	{
		static unsigned dframe = 0;
		unsigned gh = GECKO_ButtonsHeld(), gd = GECKO_ButtonsDown();
		if ((dframe++ % 60) == 0 || gh || gd) {
			FILE* f = fopen("sd:/gecko_diag.log", "a");
			if (f) {
				fprintf(f, "f=%u alive=%d held=0x%08x down=0x%08x padHeld=0x%08x\n",
					dframe, GECKO_Available(), gh, gd, (unsigned)PAD_ButtonsHeld(0));
				fclose(f);
			}
		}
	}
#endif

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
	if (!SkipFrameTracker) { PZ_SCOPE(PZ_DRAW); Draw(); }
	u64 _b2 = gettime();
	bench_tick(_b1 - _b0, _b2 - _b1);
#else
	NDS_exec<TRUE>(0);

	if (!SkipFrameTracker) { PZ_SCOPE(PZ_DRAW); Draw(); } // only update when !Frame skip tracker
#endif
	pzFrameTick();

#ifdef DESMUME_GBA_IRQ_SOAK
	// PLAN.md §4.3 item 6 tail: sustained ARM7-JIT-vs-GBA-IRQ soak probe.
	// The companion synthetic ROM (tools/gba-refcheck/irqsoak.s) maintains
	// five u32 counters at GBA_EWRAM+0x0/0x4/0x8/0xC/0x10 (total_irq,
	// vblank_count, timer_count, mainloop_iters, bad_source_count) --
	// total_irq/vblank_count/timer_count from its ISR (called through the
	// real armcpu_irqException()/BIOS-trampoline path, gba_irq.cpp),
	// mainloop_iters from its main busy loop, entirely independent of the
	// ISR. Emitted every 256 frames as one PKT_PROFILE line so a long soak
	// run over the network harness produces periodic checkpoints instead
	// of one silent black box; also stamps ARM7's live PC (stuck-PC / JIT
	// runaway detection -- a real GBA cart's PC should never sit still
	// across two consecutive 256-frame checkpoints once past setup).
	{
		static u32 soakFrame = 0;
		if (gameInfo.isGBA && (++soakFrame % 256) == 0) {
			u32 total = T1ReadLong(MMU.GBA_EWRAM, 0x0);
			u32 vbl   = T1ReadLong(MMU.GBA_EWRAM, 0x4);
			u32 tmr   = T1ReadLong(MMU.GBA_EWRAM, 0x8);
			u32 loop  = T1ReadLong(MMU.GBA_EWRAM, 0xC);
			u32 bad   = T1ReadLong(MMU.GBA_EWRAM, 0x10);
			harness_profile_emitf(
				"irqsoak frame=%lu total=%lu vbl=%lu tmr=%lu loop=%lu bad=%lu pc=0x%08lx\n",
				(unsigned long)soakFrame, (unsigned long)total, (unsigned long)vbl,
				(unsigned long)tmr, (unsigned long)loop, (unsigned long)bad,
				(unsigned long)NDS_ARM7.instruct_adr);
		}
	}
#endif

#ifdef DESMUME_GBA_SAVESTATE_SOAK
	// PLAN.md §4.3 item 7: GBA savestate determinism test. Rides on the same
	// irqsoak.gba ROM/counters as DESMUME_GBA_IRQ_SOAK above (timers +
	// VBlank IRQ + a main-loop counter, all continuously active) instead of
	// a new synthetic ROM. `sframe` is a host-side counter, untouched by
	// savestate_load()'s internal NDS_Reset()/chunk restore, so it keeps
	// counting monotonically straight across the reload below -- that's
	// what lets a single run compare "N frames executed from the checkpoint
	// the first time" against "N frames executed from the checkpoint after
	// an explicit save+reload round-trip" without needing two separate
	// process runs:
	//   sframe==300: savestate_save() a checkpoint (mid-run: timers and
	//                VBlank IRQ already active for hundreds of dispatches).
	//   sframe==600: emit "sstest phase=truth ..." (the 5 EWRAM counters +
	//                live ARM7 PC) -- this is "what should happen" 300
	//                frames past the checkpoint, then immediately
	//                savestate_load() the checkpoint back.
	//   300 frames later (own counter, since sframe keeps climbing through
	//                the reload): emit "sstest phase=reload ..." with the
	//                same fields. A correct, deterministic
	//                save/restore makes this line byte-for-byte identical
	//                to phase=truth (same counters, same PC) since both are
	//                exactly 300 emulated frames past the identical
	//                checkpoint state.
	{
		static u32 sframe = 0;
		static bool saved = false, didTruth = false, reloaded = false, didReload = false;
		static u32 framesSinceReload = 0;
		if (gameInfo.isGBA) {
			sframe++;
			if (!saved && sframe == 300) {
				saved = savestate_save("sd:/gba_sstest.dst");
				harness_profile_emitf("sstest checkpoint saved=%d frame=%lu\n",
					(int)saved, (unsigned long)sframe);
			}
			if (saved && !didTruth && sframe == 600) {
				u32 total = T1ReadLong(MMU.GBA_EWRAM, 0x0);
				u32 vbl   = T1ReadLong(MMU.GBA_EWRAM, 0x4);
				u32 tmr   = T1ReadLong(MMU.GBA_EWRAM, 0x8);
				u32 loop  = T1ReadLong(MMU.GBA_EWRAM, 0xC);
				u32 bad   = T1ReadLong(MMU.GBA_EWRAM, 0x10);
				harness_profile_emitf(
					"sstest phase=truth frame=%lu total=%lu vbl=%lu tmr=%lu loop=%lu bad=%lu pc=0x%08lx\n",
					(unsigned long)sframe, (unsigned long)total, (unsigned long)vbl,
					(unsigned long)tmr, (unsigned long)loop, (unsigned long)bad,
					(unsigned long)NDS_ARM7.instruct_adr);
				didTruth = true;
				reloaded = savestate_load("sd:/gba_sstest.dst");
				harness_profile_emitf("sstest reload_ok=%d\n", (int)reloaded);
			}
			// sframe > 600 guards against double-counting: the reload above
			// happens *after* sframe==600's own NDS_exec already ran (on the
			// pre-reload state), so the first NDS_exec actually run against
			// the reloaded checkpoint is the one that bumps sframe to 601,
			// not 600 itself.
			if (reloaded && !didReload && sframe > 600) {
				framesSinceReload++;
				if (framesSinceReload == 300) {
					u32 total = T1ReadLong(MMU.GBA_EWRAM, 0x0);
					u32 vbl   = T1ReadLong(MMU.GBA_EWRAM, 0x4);
					u32 tmr   = T1ReadLong(MMU.GBA_EWRAM, 0x8);
					u32 loop  = T1ReadLong(MMU.GBA_EWRAM, 0xC);
					u32 bad   = T1ReadLong(MMU.GBA_EWRAM, 0x10);
					harness_profile_emitf(
						"sstest phase=reload frame=%lu total=%lu vbl=%lu tmr=%lu loop=%lu bad=%lu pc=0x%08lx\n",
						(unsigned long)sframe, (unsigned long)total, (unsigned long)vbl,
						(unsigned long)tmr, (unsigned long)loop, (unsigned long)bad,
						(unsigned long)NDS_ARM7.instruct_adr);
					didReload = true;
				}
			}
		}
	}
#endif

#ifdef DESMUME_GBA_APU_SOAK
	// PLAN.md §4.3 item 3 (APU): DirectSound FIFO reference-check probe.
	// The companion synthetic ROM (tools/gba-refcheck/dsound.s) arms Timer0
	// (32768 Hz cadence) + DMA1 Special-timing FIFO-A refill with a known,
	// deterministic +-100 (0x64/0x9C) 8-bit PCM square wave and enables
	// DirectSound FIFO A at full volume, both channels. Calls the same
	// public gbaApuMixAudio() SPU.cpp's isGBA branch uses, so this reads
	// exactly the mixed L/R sample a real audio callback would get right
	// now (zero-order hold of the last DMA'd byte -- see gba_apu.h) --
	// emitted every frame for the first 32 frames only (one soak run's
	// worth of setup + a handful of Timer0 periods per frame at ~549
	// samples/frame @32768Hz/59.7fps is plenty to see the known waveform
	// land) so the harness's PKT_PROFILE stream stays small.
	{
		static u32 apuFrame = 0;
		apuFrame++;
		if (gameInfo.isGBA && apuFrame <= 32) {
			s16 buf[2];
			gbaApuMixAudio(buf, 1);
			harness_profile_emitf("apusoak frame=%lu left=%d right=%d\n",
				(unsigned long)apuFrame, (int)buf[0], (int)buf[1]);
		}
	}
#endif

#ifdef DESMUME_GBA_WAITCNT_SOAK
	// PLAN.md §4.3 item 4: WAITCNT-driven cartridge ROM wait-state cost
	// verification. The companion synthetic ROM (tools/gba-refcheck/
	// waitcnt.s) times a fixed number of non-sequential cartridge-ROM
	// reads against Timer0 under two WAITCNT configs in turn (0x0000
	// slow-default, then 0x0018 fastest-WS0) and lands both elapsed-tick
	// counts in EWRAM once both runs finish. Emitted once, the first
	// frame both are observed non-zero, so the harness's PKT_PROFILE
	// stream stays a single line instead of spamming every frame.
	{
		static bool emitted = false;
		if (!emitted && gameInfo.isGBA) {
			u32 slow = T1ReadLong(MMU.GBA_EWRAM, 0x0);
			u32 fast = T1ReadLong(MMU.GBA_EWRAM, 0x4);
			if (slow != 0 && fast != 0) {
				harness_profile_emitf("waitcntsoak slow=%lu fast=%lu\n",
					(unsigned long)slow, (unsigned long)fast);
				emitted = true;
			}
		}
	}
#endif

#ifdef DESMUME_GBA_DMA_SOAK
	// PLAN.md §4.3 item 2: DMA3 Video Capture Special-timing trigger
	// verification. The companion synthetic ROM (tools/gba-refcheck/
	// dmavcap.s) arms DMA3 with src fixed at REG_VCOUNT and dst incrementing
	// into EWRAM 0x02010000, Special timing, repeat=1 -- so each firing
	// records the VCOUNT value it fired at into the next buffer slot, then
	// disarms itself and sets a liveness marker at EWRAM 0x0200FF00 once it
	// observes VCOUNT==163 (just past frame 1's capture window). Once that
	// marker is set, dump the first 165 recorded halfwords (safely past the
	// expected 160) as one PKT_PROFILE line so the host side can check the
	// exact fired-scanline sequence is 2,3,...,161 with nothing else mixed
	// in (which would indicate a bug firing this channel from the plain
	// HBlank/VBlank trigger paths too, or firing outside the documented
	// VCOUNT range).
	{
		static bool emitted = false;
		if (!emitted && gameInfo.isGBA) {
			u32 live = T1ReadLong(MMU.GBA_EWRAM, 0x0FF00);
			if (live != 0) {
				// harness_profile_emitf's internal buffer is 512 bytes
				// (harness_profile.cpp), too small for all 165 entries in
				// one line -- split into 80-value chunks.
				for (int chunk = 0; chunk < 165; chunk += 80) {
					char line[600];
					int off = snprintf(line, sizeof(line), "dmasoak part=%d vals=", chunk / 80);
					int end = (chunk + 80 < 165) ? chunk + 80 : 165;
					for (int i = chunk; i < end && off < (int)sizeof(line) - 8; i++) {
						u16 v = T1ReadWord(MMU.GBA_EWRAM, 0x10000 + i * 2);
						off += snprintf(line + off, sizeof(line) - off, "%u,", (unsigned)v);
					}
					harness_profile_emitf("%s\n", line);
				}
				emitted = true;
			}
		}
	}
#endif

	FPSOverlay_Tick();

#ifdef DESMUME_HARNESS
	{
		// Drain any host -> device packets (§3.4 playlist control, §3.5 input).
		u8 _pt;
		char _rx[128];
		u32 _rn;
		while ((_rn = harness_recv(&_pt, _rx, sizeof(_rx) - 1)) != 0) {
			if (_pt == HARNESS_PKT_CTRL) {
				_rx[_rn] = 0;
				if (!strncmp(_rx, "next_rom", 8)) {
					harness_boot_request_next();
				} else if (!strncmp(_rx, "capture_frame", 13)) {
					const char *_a = _rx + 13;
					while (*_a == ' ') _a++;
					harness_frame_request(*_a ? _a : 0);
				} else if (!strncmp(_rx, "movie", 5)) {
					harness_input_movie_cmd(_rx + 5);
				}
			} else if (_pt == HARNESS_PKT_INPUT) {
				// §3.5: drive the shared transport-agnostic input core.
				harness_input_feed_bytes(_rx, (int)_rn);
			}
		}

		// One heartbeat/sec, tagged with the current playlist ROM so the desktop
		// watchdog (§3.4) knows which ROM a stall belongs to.
		static u32 _hb = 0;
		++_hb;
		if ((_hb % 60) == 0) {
			const char *_rname = harness_boot_rom_name();
			char _l[96];
			snprintf(_l, sizeof(_l), "heartbeat frame=%lu%s%s",
				(unsigned long)_hb,
				(_rname && *_rname) ? " rom=" : "",
				(_rname && *_rname) ? _rname : "");
			harness_send(HARNESS_PKT_CTRL, _l, strlen(_l));
		}

		// §3.4 on-demand video readout (off unless frame_every set or requested).
		harness_frame_tick(_hb);
	}
#endif

#ifdef DESMUME_FBDUMP
	// Renderer correctness A/B (-DDESMUME_FBDUMP): on a deterministic direct-boot
	// ROM, snapshot the composited framebuffer at a fixed frame and write it to
	// sd:/fb.dump. Two builds that render identically produce a byte-identical
	// dump. Pair with -DDESMUME_BENCH so the run has a frame budget; pull
	// sd:/fb.dump after killing the emulator.
	//   make TESTDEFS="-DDESMUME_FORCE_ROM -DDESMUME_BENCH -DDESMUME_FBDUMP [-DDESMUME_FBDUMP_FRAME=N] -DDESMUME_FORCE_CORE=1"
#ifndef DESMUME_FBDUMP_FRAME
#define DESMUME_FBDUMP_FRAME 600
#endif
	{
		// Snapshot GPU_screen at a fixed frame (the ROM's frame counter is
		// deterministic, so an A/B pair captures the identical emulated frame),
		// then keep re-writing that frozen snapshot every 60 frames so Dolphin's
		// periodic SD sync has flushed a complete copy before the harness kills it.
		static u32 _fbframe = 0;
		static u8  _fbsnap[sizeof(GPU_screen)];
		static bool _fbhave = false;
		++_fbframe;
		if (_fbframe == DESMUME_FBDUMP_FRAME) {
			memcpy(_fbsnap, GPU_screen, sizeof(_fbsnap));
			_fbhave = true;
		}
		if (_fbhave && (_fbframe % 60) == 0) {
			FILE *f = fopen("sd:/fb.dump", "w");
			if (f) { fwrite(_fbsnap, 1, sizeof(_fbsnap), f); fflush(f); fclose(f); }
		}
	}
#endif

#ifdef DESMUME_ARMWRESTLER_PROBE
	armwrestler_probe_tick();
#endif
#ifdef DESMUME_ARM7WRESTLER_PROBE
	arm7wrestler_probe_tick();
#endif
#ifdef DESMUME_ROCKWRESTLER_PROBE
	rockwrestler_probe_tick();
#endif
#ifdef DESMUME_GBA_BOOT_PROBE
	gba_boot_probe_tick();
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
