/*  Copyright (C) 2006 Normmatt
    Copyright (C) 2006 Theo Berkau
    Copyright (C) 2007 Pascal Giard
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

#define HAVE_LIBZ

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#include <stack>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fstream>
#include "saves.h"
#include "MMU.h"
#include "NDSSystem.h"
#include "render3D.h"
#include "cp15.h"
//#include "GPU_osd.h"
#include "version.h"

#include "readwrite.h"
#include "GPU.h"
#include "gfx3d.h"
#include "movie.h"
#include "mic.h"
#include "MMU_timing.h"

// PLAN.md §4.3 item 7 (GBA savestate support).
#include "gba_dma.h"
#include "gba_timers.h"
#include "gba_apu.h"
#include "gba_backup.h"
#include "gba_ppu.h"

#include "path.h"

#ifdef _WINDOWS
#include "windows/main.h"
#endif

int lastSaveState = 0;		//Keeps track of last savestate used for quick save/load functions

#ifdef DESMUME_SAVESTATE_DIAG
// Each call opens/appends/closes sd:/savestate_trace.log so a checkpoint
// survives even if the very next line hangs the emu thread -- printf() over
// gecko doesn't help when nobody's holding a Gecko socket open for a manual
// play session, but a flushed-to-disk file does.
#include <stdarg.h>
static void SSTRACE(const char* fmt, ...)
{
	FILE* f = fopen("sd:/savestate_trace.log", "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}
#else
#define SSTRACE(...) ((void)0)
#endif

// Upstream DeSmuME's movie system sets this around the NDS_Reset() a
// savestate load does, so a movie recording in progress isn't torn down by
// that reset. This Wii port never carried movie.cpp over, so nothing else
// ever reads it, but savestate_load() below still flips it -- define it here
// so that code links now that a real caller (the Z+L/Z+R quicksave hotkey in
// main.cpp) actually reaches it.
bool _HACK_DONT_STOPMOVIE = false;

//void*v is actually a void** which will be indirected before reading
//since this isnt supported right now, it is declared in here to make things compile
#define SS_INDIRECT            0x80000000

savestates_t savestates[NB_STATES];

#define SAVESTATE_VERSION       12
static const char* magic = "DeSmuME SState\0";

//a savestate chunk loader can set this if it wants to permit a silent failure (for compatibility)
static bool SAV_silent_fail_flag;

#ifndef MAX_PATH
#define MAX_PATH 256
#endif


SFORMAT SF_ARM7[]={
	{ "7INS", 4, 1, &NDS_ARM7.instruction },
	{ "7INA", 4, 1, &NDS_ARM7.instruct_adr },
	{ "7INN", 4, 1, &NDS_ARM7.next_instruction },
	{ "7REG", 4,16, NDS_ARM7.R },
	{ "7CPS", 4, 1, &NDS_ARM7.CPSR },
	{ "7SPS", 4, 1, &NDS_ARM7.SPSR },
	{ "7DUS", 4, 1, &NDS_ARM7.R13_usr },
	{ "7EUS", 4, 1, &NDS_ARM7.R14_usr },
	{ "7DSV", 4, 1, &NDS_ARM7.R13_svc },
	{ "7ESV", 4, 1, &NDS_ARM7.R14_svc },
	{ "7DAB", 4, 1, &NDS_ARM7.R13_abt },
	{ "7EAB", 4, 1, &NDS_ARM7.R14_abt },
	{ "7DUN", 4, 1, &NDS_ARM7.R13_und },
	{ "7EUN", 4, 1, &NDS_ARM7.R14_und },
	{ "7DIR", 4, 1, &NDS_ARM7.R13_irq },
	{ "7EIR", 4, 1, &NDS_ARM7.R14_irq },
	{ "78FI", 4, 1, &NDS_ARM7.R8_fiq },
	{ "79FI", 4, 1, &NDS_ARM7.R9_fiq },
	{ "7AFI", 4, 1, &NDS_ARM7.R10_fiq },
	{ "7BFI", 4, 1, &NDS_ARM7.R11_fiq },
	{ "7CFI", 4, 1, &NDS_ARM7.R12_fiq },
	{ "7DFI", 4, 1, &NDS_ARM7.R13_fiq },
	{ "7EFI", 4, 1, &NDS_ARM7.R14_fiq },
	{ "7SVC", 4, 1, &NDS_ARM7.SPSR_svc },
	{ "7ABT", 4, 1, &NDS_ARM7.SPSR_abt },
	{ "7UND", 4, 1, &NDS_ARM7.SPSR_und },
	{ "7IRQ", 4, 1, &NDS_ARM7.SPSR_irq },
	{ "7FIQ", 4, 1, &NDS_ARM7.SPSR_fiq },
	{ "7int", 4, 1, &NDS_ARM7.intVector },
	{ "7LDT", 1, 1, &NDS_ARM7.LDTBit },
	{ "7Wai", 4, 1, &NDS_ARM7.waitIRQ },
	{ "7wir", 4, 1, &NDS_ARM7.wirq, },
	{ 0 }
};

SFORMAT SF_ARM9[]={
	{ "9INS", 4, 1, &NDS_ARM9.instruction},
	{ "9INA", 4, 1, &NDS_ARM9.instruct_adr},
	{ "9INN", 4, 1, &NDS_ARM9.next_instruction},
	{ "9REG", 4,16, NDS_ARM9.R},
	{ "9CPS", 4, 1, &NDS_ARM9.CPSR},
	{ "9SPS", 4, 1, &NDS_ARM9.SPSR},
	{ "9DUS", 4, 1, &NDS_ARM9.R13_usr},
	{ "9EUS", 4, 1, &NDS_ARM9.R14_usr},
	{ "9DSV", 4, 1, &NDS_ARM9.R13_svc},
	{ "9ESV", 4, 1, &NDS_ARM9.R14_svc},
	{ "9DAB", 4, 1, &NDS_ARM9.R13_abt},
	{ "9EAB", 4, 1, &NDS_ARM9.R14_abt},
	{ "9DUN", 4, 1, &NDS_ARM9.R13_und},
	{ "9EUN", 4, 1, &NDS_ARM9.R14_und},
	{ "9DIR", 4, 1, &NDS_ARM9.R13_irq},
	{ "9EIR", 4, 1, &NDS_ARM9.R14_irq},
	{ "98FI", 4, 1, &NDS_ARM9.R8_fiq},
	{ "99FI", 4, 1, &NDS_ARM9.R9_fiq},
	{ "9AFI", 4, 1, &NDS_ARM9.R10_fiq},
	{ "9BFI", 4, 1, &NDS_ARM9.R11_fiq},
	{ "9CFI", 4, 1, &NDS_ARM9.R12_fiq},
	{ "9DFI", 4, 1, &NDS_ARM9.R13_fiq},
	{ "9EFI", 4, 1, &NDS_ARM9.R14_fiq},
	{ "9SVC", 4, 1, &NDS_ARM9.SPSR_svc},
	{ "9ABT", 4, 1, &NDS_ARM9.SPSR_abt},
	{ "9UND", 4, 1, &NDS_ARM9.SPSR_und},
	{ "9IRQ", 4, 1, &NDS_ARM9.SPSR_irq},
	{ "9FIQ", 4, 1, &NDS_ARM9.SPSR_fiq},
	{ "9int", 4, 1, &NDS_ARM9.intVector},
	{ "9LDT", 1, 1, &NDS_ARM9.LDTBit},
	{ "9Wai", 4, 1, &NDS_ARM9.waitIRQ},
	{ "9wir", 4, 1, &NDS_ARM9.wirq},
	{ 0 }
};

// NB: ARM9_ITCM/DTCM/MAIN_MEM/ARM9_REG/ARM9_VMEM became heap pointers (MMU.h,
// MMU_Alloc()) back in 2010, but this table stayed a static initializer -- so
// every `.v` here captured a NULL pointer at C++ static-init time (MMU_Alloc()
// runs much later) and every `sizeof(ptr)` collapsed to 4. SubWrite() then
// stopped at the very first entry (v == NULL) and chunk 4 was written with a
// body of 0 bytes: ARM9 main RAM / TCM / VRAM / I/O regs were never in a
// savestate at all, so every load resumed the CPU against freshly-reset memory
// and instantly wedged. The explicit sizes below are the real allocation sizes
// (MMU_Alloc); the live pointers are patched in by SF_MEM_rebind() before each
// save and load. MAIN_MEM is 4 MB on this port (retail DS), so the old second
// 4 MB "WRAX" mirror -- which indexed 4 MB past a 4 MB buffer -- is gone.
SFORMAT SF_MEM[]={
	{ "ITCM", 1, 0x8000,   NULL},   // MMU.ARM9_ITCM
	{ "DTCM", 1, 0x4000,   NULL},   // MMU.ARM9_DTCM
	{ "WRAM", 1, 0x400000, NULL},   // MMU.MAIN_MEM

	//NOTE - this is not as large as the allocated memory.
	//the memory is overlarge due to the way our memory map system is setup
	//but there are actually no more registers than this
	{ "9REG", 1, 0x2000,   NULL},   // MMU.ARM9_REG (0x40000 allocated)

	{ "VMEM", 1, 0x800,    NULL},   // MMU.ARM9_VMEM
	{ "OAMS", 1, sizeof(MMU.ARM9_OAM),    MMU.ARM9_OAM},   // inline array - static bind ok

	//this size is specially chosen to avoid saving the blank space at the end
	{ "LCDM", 1, 0xA4000,		MMU.ARM9_LCD},              // inline array - static bind ok
	{ 0 }
};

// Patch the heap-backed SF_MEM entries with their live MMU pointers. Must run
// after MMU_Alloc() and before any SubWrite()/ReadStateChunk() over SF_MEM.
static void SF_MEM_rebind()
{
	SF_MEM[0].v = MMU.ARM9_ITCM;
	SF_MEM[1].v = MMU.ARM9_DTCM;
	SF_MEM[2].v = MMU.MAIN_MEM;
	SF_MEM[3].v = MMU.ARM9_REG;
	SF_MEM[4].v = MMU.ARM9_VMEM;
}

// PLAN.md §4.3 item 7 (GBA savestate support): the flat GBA memory-map
// backing buffers (MMU.h, roadmap #20 §12.3 step 3) plus the GBA I/O
// register block. Inline arrays inside the global MMU struct (like
// ARM9_OAM/ARM9_LCD above), not heap pointers, so -- same as those -- no
// SF_MEM_rebind()-style pointer patch is needed; the static initializer
// below binds directly. GBA_BIOS is included even though it's fixed at
// ROM-load time and never written afterward: cheap (16 KB) and it means
// this table needs no asterisk about "everything except the BIOS".
// GBA_screen (the composited pixel output, gba_ppu.h) is deliberately
// NOT here -- see that header's savestate comment for why.
SFORMAT SF_GBA_MEM[]={
	{ "GBIO", 1, sizeof(MMU.GBA_BIOS),    MMU.GBA_BIOS},
	{ "GEWR", 1, sizeof(MMU.GBA_EWRAM),   MMU.GBA_EWRAM},
	{ "GIWR", 1, sizeof(MMU.GBA_IWRAM),   MMU.GBA_IWRAM},
	{ "GPAL", 1, sizeof(MMU.GBA_PALETTE), MMU.GBA_PALETTE},
	{ "GVRM", 1, sizeof(MMU.GBA_VRAM),    MMU.GBA_VRAM},
	{ "GOAM", 1, sizeof(MMU.GBA_OAM),     MMU.GBA_OAM},
	{ "GIOR", 1, sizeof(MMU.GBA_IOREG),   MMU.GBA_IOREG},
	{ 0 }
};

SFORMAT SF_NDS[]={
	{ "_WCY", 4, 1, &nds.wifiCycle},
	{ "_TCY", 8, 8, nds.timerCycle},
	{ "_VCT", 4, 1, &nds.VCount},
	{ "_OLD", 4, 1, &nds.old},
	{ "_TPX", 2, 1, &nds.touchX},
	{ "_TPY", 2, 1, &nds.touchY},
	{ "_TPB", 4, 1, &nds.isTouch},
	{ "_DBG", 4, 1, 0}, //No debug
	{ 0 }
};

SFORMAT SF_MMU[]={
	{ "M7BI", 1, sizeof(MMU.ARM7_BIOS), MMU.ARM7_BIOS},
	{ "M7ER", 1, sizeof(MMU.ARM7_ERAM), MMU.ARM7_ERAM},
	{ "M7RG", 1, sizeof(MMU.ARM7_REG), MMU.ARM7_REG},
	{ "M7WI", 1, sizeof(MMU.ARM7_WIRAM), MMU.ARM7_WIRAM},
	{ "MSWI", 1, sizeof(MMU.SWIRAM), MMU.SWIRAM},
	{ "MCRA", 1, sizeof(MMU.CART_RAM), MMU.CART_RAM},
	{ "M9RW", 1, 1,       &MMU.ARM9_RW_MODE},
	{ "MDTC", 4, 1,       &MMU.DTCMRegion},
	{ "MITC", 4, 1,       &MMU.ITCMRegion},
	{ "MTIM", 2, 8,       MMU.timer},
	{ "MTMO", 4, 8,       MMU.timerMODE},
	{ "MTON", 4, 8,       MMU.timerON},
	{ "MTRN", 4, 8,       MMU.timerRUN},
	{ "MTRL", 2, 8,       MMU.timerReload},
	{ "MIME", 4, 2,       MMU.reg_IME},
	{ "MIE_", 4, 2,       MMU.reg_IE},
	{ "MIF_", 4, 2,       MMU.reg_IF},

	{ "MGXC", 8, 1,       &MMU.gfx3dCycles},
	
	{ "M_SX", 1, 2,       &MMU.SPI_CNT},
	{ "M_SC", 1, 2,       &MMU.SPI_CMD},
	{ "MASX", 1, 2,       &MMU.AUX_SPI_CNT},
	{ "MASC", 1, 2,       &MMU.AUX_SPI_CMD},

	{ "MDV1", 4, 1,       &MMU.divRunning},
	{ "MDV2", 8, 1,       &MMU.divResult},
	{ "MDV3", 8, 1,       &MMU.divMod},
	{ "MDV4", 4, 1,       &MMU.divCnt},
	{ "MDV5", 8, 1,       &MMU.divCycles},

	{ "MSQ1", 4, 1,       &MMU.sqrtRunning},
	{ "MSQ2", 4, 1,       &MMU.sqrtResult},
	{ "MSQ3", 4, 1,       &MMU.sqrtCnt},
	{ "MSQ4", 8, 1,       &MMU.sqrtCycles},
	
	//begin memory chips
	{ "BUCO", 1, 1,       &MMU.fw.com},
	{ "BUAD", 4, 1,       &MMU.fw.addr},
	{ "BUAS", 1, 1,       &MMU.fw.addr_shift},
	{ "BUAZ", 1, 1,       &MMU.fw.addr_size},
	{ "BUWE", 4, 1,       &MMU.fw.write_enable},
	{ "BUWR", 4, 1,       &MMU.fw.writeable_buffer},
	//end memory chips

	{ "MC0A", 4, 1,       &MMU.dscard[0].address},
	{ "MC0T", 4, 1,       &MMU.dscard[0].transfer_count},
	{ "MC1A", 4, 1,       &MMU.dscard[1].address},
	{ "MC1T", 4, 1,       &MMU.dscard[1].transfer_count},
	//{ "MCHT", 4, 1,       &MMU.CheckTimers},
	//{ "MCHD", 4, 1,       &MMU.CheckDMAs},

	//fifos
	{ "F0TH", 1, 1,       &ipc_fifo[0].head},
	{ "F0TL", 1, 1,       &ipc_fifo[0].tail},
	{ "F0SZ", 1, 1,       &ipc_fifo[0].size},
	{ "F0BF", 4, 16,      ipc_fifo[0].buf},
	{ "F1TH", 1, 1,       &ipc_fifo[1].head},
	{ "F1TL", 1, 1,       &ipc_fifo[1].tail},
	{ "F1SZ", 1, 1,       &ipc_fifo[1].size},
	{ "F1BF", 4, 16,      ipc_fifo[1].buf},

	{ "FDHD", 4, 1,       &disp_fifo.head},
	{ "FDTL", 4, 1,       &disp_fifo.tail},
	{ "FDBF", 4, 0x6000,  disp_fifo.buf},
	
	{ 0 }
};

static void mmu_savestate(EMUFILE* os)
{
	u32 version = 3;
	write32le(version,os);
	
	//version 2:
	MMU_new.backupDevice.save_state(os);
	
	//version 3:
	MMU_new.gxstat.savestate(os);
	for(int i=0;i<2;i++)
		for(int j=0;j<4;j++)
			MMU_new.dma[i][j].savestate(os);

	MMU_timing.arm9codeFetch.savestate(os, version);
	MMU_timing.arm9dataFetch.savestate(os, version);
	MMU_timing.arm7codeFetch.savestate(os, version);
	MMU_timing.arm7dataFetch.savestate(os, version);
	MMU_timing.arm9codeCache.savestate(os, version);
	MMU_timing.arm9dataCache.savestate(os, version);
}

// PLAN.md §4.3 item 7 (GBA savestate support): each GBA peripheral
// module's own private runtime state (see each gba_*.h header's savestate
// comment for exactly what and why). Only ever written/read when
// gameInfo.isGBA (writechunks() below gates chunk 201 on it) -- ARM7's CPU
// registers/CPSR are NOT duplicated here, GBA mode reuses NDS_ARM7 (see
// bios_gba.cpp's `#define cpu (&NDS_ARM7)`) so chunk 2 (SF_ARM7) already
// covers them. Likewise gba_irq.cpp and gba_keypad.cpp own no state beyond
// what's already in MMU.GBA_IOREG/GBA_IWRAM (chunk 200, SF_GBA_MEM) or
// NDS_ARM7 (chunk 2) -- see those headers' file comments -- so neither
// module appears here.
static void gba_modules_savestate(EMUFILE* os)
{
	write32le(1, os); // version, in case a module's own sub-format ever needs a coordinated bump
	gbaDmaSaveState(os);
	gbaTimersSaveState(os);
	gbaApuSaveState(os);
	gbaBackupSaveState(os);
	gbaPpuSaveState(os);
}

static bool gba_modules_loadstate(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	if (!gbaDmaLoadState(is, size)) return false;
	if (!gbaTimersLoadState(is, size)) return false;
	if (!gbaApuLoadState(is, size)) return false;
	if (!gbaBackupLoadState(is, size)) return false;
	if (!gbaPpuLoadState(is, size)) return false;
	return true;
}

SFORMAT SF_WIFI[]={
	{ "W000", 4, 1, &wifiMac.powerOn},
	{ "W010", 4, 1, &wifiMac.powerOnPending},

	{ "W020", 2, 1, &wifiMac.rfStatus},
	{ "W030", 2, 1, &wifiMac.rfPins},

	{ "W040", 2, 1, &wifiMac.IE.val},
	{ "W050", 2, 1, &wifiMac.IF.val},

	{ "W060", 2, 1, &wifiMac.macMode},
	{ "W070", 2, 1, &wifiMac.wepMode},
	{ "W080", 4, 1, &wifiMac.WEP_enable},

	{ "W090", 2, 3, &wifiMac.TXSlot[0]},
	{ "W100", 2, 1, &wifiMac.TXCnt},
	{ "W110", 2, 1, &wifiMac.TXOpt},
	{ "W120", 2, 1, &wifiMac.TXStat},
	{ "W130", 2, 1, &wifiMac.BeaconAddr},
	{ "W140", 4, 1, &wifiMac.BeaconEnable},
	{ "W150", 1, 1, &wifiMac.txCurSlot},
	{ "W160", 1, 3, &wifiMac.txSlotBusy[0]},
	{ "W170", 4, 3, &wifiMac.txSlotAddr[0]},
	{ "W180", 4, 3, &wifiMac.txSlotLen[0]},
	{ "W190", 4, 3, &wifiMac.txSlotRemainingBytes[0]},

	{ "W200", 2, 1, &wifiMac.RXCnt},
	{ "W210", 2, 1, &wifiMac.RXCheckCounter},

	{ "W220", 1, 6, &wifiMac.mac.bytes},
	{ "W230", 1, 6, &wifiMac.bss.bytes},

	{ "W240", 2, 1, &wifiMac.aid},
	{ "W250", 2, 1, &wifiMac.pid},
	{ "W260", 2, 1, &wifiMac.retryLimit},

	{ "W270", 4, 1, &wifiMac.crystalEnabled},
	{ "W280", 8, 1, &wifiMac.usec},
	{ "W290", 4, 1, &wifiMac.usecEnable},
	{ "W300", 8, 1, &wifiMac.ucmp},
	{ "W310", 4, 1, &wifiMac.ucmpEnable},
	{ "W320", 2, 1, &wifiMac.eCount},
	{ "W330", 4, 1, &wifiMac.eCountEnable},

	{ "WR00", 4, 1, &wifiMac.RF.CFG1.val},
	{ "WR01", 4, 1, &wifiMac.RF.IFPLL1.val},
	{ "WR02", 4, 1, &wifiMac.RF.IFPLL2.val},
	{ "WR03", 4, 1, &wifiMac.RF.IFPLL3.val},
	{ "WR04", 4, 1, &wifiMac.RF.RFPLL1.val},
	{ "WR05", 4, 1, &wifiMac.RF.RFPLL2.val},
	{ "WR06", 4, 1, &wifiMac.RF.RFPLL3.val},
	{ "WR07", 4, 1, &wifiMac.RF.RFPLL4.val},
	{ "WR08", 4, 1, &wifiMac.RF.CAL1.val},
	{ "WR09", 4, 1, &wifiMac.RF.TXRX1.val},
	{ "WR10", 4, 1, &wifiMac.RF.PCNT1.val},
	{ "WR11", 4, 1, &wifiMac.RF.PCNT2.val},
	{ "WR12", 4, 1, &wifiMac.RF.VCOT1.val},

	{ "W340", 1, 105, &wifiMac.BB.data[0]},

	{ "W350", 2, 1, &wifiMac.rfIOCnt.val},
	{ "W360", 2, 1, &wifiMac.rfIOStatus.val},
	{ "W370", 4, 1, &wifiMac.rfIOData.val},
	{ "W380", 2, 1, &wifiMac.bbIOCnt.val},

	{ "W390", 1, 1, &wifiMac.bbDataToWrite},

	{ "W400", 2, 0x1000, &wifiMac.circularBuffer[0]},
	{ "W410", 2, 1, &wifiMac.RXRangeBegin},
	{ "W420", 2, 1, &wifiMac.RXRangeEnd},
	{ "W430", 2, 1, &wifiMac.RXHWWriteCursor},
	{ "W440", 2, 1, &wifiMac.RXHWWriteCursorReg},
	{ "W450", 2, 1, &wifiMac.RXHWWriteCursorLatched},
	{ "W460", 2, 1, &wifiMac.RXReadCursor},
	{ "W470", 2, 1, &wifiMac.RXUnits},
	{ "W480", 2, 1, &wifiMac.RXBufCount},
	{ "W490", 2, 1, &wifiMac.CircBufReadAddress},
	{ "W500", 2, 1, &wifiMac.CircBufWriteAddress},
	{ "W510", 2, 1, &wifiMac.CircBufRdEnd},
	{ "W520", 2, 1, &wifiMac.CircBufRdSkip},
	{ "W530", 2, 1, &wifiMac.CircBufWrEnd},
	{ "W540", 2, 1, &wifiMac.CircBufWrSkip},

	{ "W550", 4, 1, &wifiMac.curPacketSize[0]},
	{ "W560", 4, 1, &wifiMac.curPacketPos[0]},
	{ "W570", 4, 1, &wifiMac.curPacketSending[0]},

	{ "W580", 2, 0x800, &wifiMac.ioMem[0]},
	{ "W590", 2, 1, &wifiMac.randomSeed},

	{ "WX00", 8, 1, &wifiMac.SoftAP.usecCounter},
	{ "WX10", 1, 4096, &wifiMac.SoftAP.curPacket[0]},
	{ "WX20", 4, 1, &wifiMac.SoftAP.curPacketSize},
	{ "WX30", 4, 1, &wifiMac.SoftAP.curPacketPos},
	{ "WX40", 4, 1, &wifiMac.SoftAP.curPacketSending},

	{ 0 }
};

static bool mmu_loadstate(EMUFILE* is, int size)
{
	//read version
	u32 version;
	if(read32le(&version,is) != 1) return false;
	
	if(version == 0 || version == 1)
	{
		u32 bupmem_size;
		u32 addr_size;

		if(version == 0)
		{
			//version 0 was buggy and didnt save the type. 
			//it would silently fail if there was a size mismatch
			SAV_silent_fail_flag = true;
			if(read32le(&bupmem_size,is) != 1) return false;
			//if(bupmem_size != MMU.bupmem.size) return false; //mismatch between current initialized and saved size
			addr_size = BackupDevice::addr_size_for_old_save_size(bupmem_size);
		}
		else if(version == 1)
		{
			//version 1 reinitializes the save system with the type that was saved
			u32 bupmem_type;
			if(read32le(&bupmem_type,is) != 1) return false;
			if(read32le(&bupmem_size,is) != 1) return false;
			addr_size = BackupDevice::addr_size_for_old_save_type(bupmem_type);
			if(addr_size == 0xFFFFFFFF)
				addr_size = BackupDevice::addr_size_for_old_save_size(bupmem_size);
		}

		if(addr_size == 0xFFFFFFFF)
			return false;

		u8* temp = new u8[bupmem_size];
		is->fread((char*)temp,bupmem_size);
		MMU_new.backupDevice.load_old_state(addr_size,temp,bupmem_size);
		delete[] temp;
		if(is->fail()) return false;
	}

	if(version < 2)
		return true;

	bool ok = MMU_new.backupDevice.load_state(is);

	if(version < 3)
		return true;

	ok &= MMU_new.gxstat.loadstate(is);
	
	for(int i=0;i<2;i++)
		for(int j=0;j<4;j++)
			ok &= MMU_new.dma[i][j].loadstate(is);

	ok &= MMU_timing.arm9codeFetch.loadstate(is, version);
	ok &= MMU_timing.arm9dataFetch.loadstate(is, version);
	ok &= MMU_timing.arm7codeFetch.loadstate(is, version);
	ok &= MMU_timing.arm7dataFetch.loadstate(is, version);
	ok &= MMU_timing.arm9codeCache.loadstate(is, version);
	ok &= MMU_timing.arm9dataCache.loadstate(is, version);

	return ok;
}

static void cp15_saveone(armcp15_t *cp15, EMUFILE* os)
{
	write32le(cp15->IDCode,os);
	write32le(cp15->cacheType,os);
    write32le(cp15->TCMSize,os);
    write32le(cp15->ctrl,os);
    write32le(cp15->DCConfig,os);
    write32le(cp15->ICConfig,os);
    write32le(cp15->writeBuffCtrl,os);
    write32le(cp15->und,os);
    write32le(cp15->DaccessPerm,os);
    write32le(cp15->IaccessPerm,os);
    write32le(cp15->protectBaseSize0,os);
    write32le(cp15->protectBaseSize1,os);
    write32le(cp15->protectBaseSize2,os);
    write32le(cp15->protectBaseSize3,os);
    write32le(cp15->protectBaseSize4,os);
    write32le(cp15->protectBaseSize5,os);
    write32le(cp15->protectBaseSize6,os);
    write32le(cp15->protectBaseSize7,os);
    write32le(cp15->cacheOp,os);
    write32le(cp15->DcacheLock,os);
    write32le(cp15->IcacheLock,os);
    write32le(cp15->ITCMRegion,os);
    write32le(cp15->DTCMRegion,os);
    write32le(cp15->processID,os);
    write32le(cp15->RAM_TAG,os);
    write32le(cp15->testState,os);
    write32le(cp15->cacheDbg,os);
    for(int i=0;i<8;i++) write32le(cp15->regionWriteMask_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionWriteMask_SYS[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionReadMask_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionReadMask_SYS[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionExecuteMask_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionExecuteMask_SYS[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionWriteSet_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionWriteSet_SYS[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionReadSet_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionReadSet_SYS[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionExecuteSet_USR[i],os);
    for(int i=0;i<8;i++) write32le(cp15->regionExecuteSet_SYS[i],os);
}

static void cp15_savestate(EMUFILE* os)
{
	//version
	write32le(0,os);

	cp15_saveone((armcp15_t *)NDS_ARM9.coproc[15],os);
	//ARM7 does not have coprocessor
	//cp15_saveone((armcp15_t *)NDS_ARM7.coproc[15],os);
}

static bool cp15_loadone(armcp15_t *cp15, EMUFILE* is)
{
	if(!read32le(&cp15->IDCode,is)) return false;
	if(!read32le(&cp15->cacheType,is)) return false;
    if(!read32le(&cp15->TCMSize,is)) return false;
    if(!read32le(&cp15->ctrl,is)) return false;
    if(!read32le(&cp15->DCConfig,is)) return false;
    if(!read32le(&cp15->ICConfig,is)) return false;
    if(!read32le(&cp15->writeBuffCtrl,is)) return false;
    if(!read32le(&cp15->und,is)) return false;
    if(!read32le(&cp15->DaccessPerm,is)) return false;
    if(!read32le(&cp15->IaccessPerm,is)) return false;
    if(!read32le(&cp15->protectBaseSize0,is)) return false;
    if(!read32le(&cp15->protectBaseSize1,is)) return false;
    if(!read32le(&cp15->protectBaseSize2,is)) return false;
    if(!read32le(&cp15->protectBaseSize3,is)) return false;
    if(!read32le(&cp15->protectBaseSize4,is)) return false;
    if(!read32le(&cp15->protectBaseSize5,is)) return false;
    if(!read32le(&cp15->protectBaseSize6,is)) return false;
    if(!read32le(&cp15->protectBaseSize7,is)) return false;
    if(!read32le(&cp15->cacheOp,is)) return false;
    if(!read32le(&cp15->DcacheLock,is)) return false;
    if(!read32le(&cp15->IcacheLock,is)) return false;
    if(!read32le(&cp15->ITCMRegion,is)) return false;
    if(!read32le(&cp15->DTCMRegion,is)) return false;
    if(!read32le(&cp15->processID,is)) return false;
    if(!read32le(&cp15->RAM_TAG,is)) return false;
    if(!read32le(&cp15->testState,is)) return false;
    if(!read32le(&cp15->cacheDbg,is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionWriteMask_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionWriteMask_SYS[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionReadMask_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionReadMask_SYS[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionExecuteMask_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionExecuteMask_SYS[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionWriteSet_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionWriteSet_SYS[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionReadSet_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionReadSet_SYS[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionExecuteSet_USR[i],is)) return false;
    for(int i=0;i<8;i++) if(!read32le(&cp15->regionExecuteSet_SYS[i],is)) return false;

    return true;
}

static bool cp15_loadstate(EMUFILE* is, int size)
{
	//read version
	u32 version;
	if(read32le(&version,is) != 1) return false;
	if(version != 0) return false;

	if(!cp15_loadone((armcp15_t *)NDS_ARM9.coproc[15],is)) return false;

	// NB: cp15_savestate() writes exactly one cp15_saveone() (the ARM9 one;
	// the ARM7 has no coprocessor and its save was removed in 2012). Upstream's
	// old version-0 layout also stored a dummy ARM7 block, and the matching
	// "read a second cp15_loadone into a throwaway buffer" used to live here --
	// but against our single-block save that over-read ~492 bytes and desynced
	// the whole stream, so *every* state load aborted with "failed halfway
	// through". Load exactly what we save.

	return true;
}



/* Format time and convert to string */
static char * format_time(time_t cal_time)
{
  struct tm *time_struct;
  static char str[64];

  time_struct=localtime(&cal_time);
  strftime(str, sizeof str, "%d-%b-%Y %H:%M:%S", time_struct);

  return(str);
}

void clear_savestates()
{
  u8 i;
  for( i = 0; i < NB_STATES; i++ )
    savestates[i].exists = FALSE;
}

// Scan for existing savestates and update struct
void scan_savestates()
{
  struct stat sbuf;
  char filename[MAX_PATH+1];

  clear_savestates();

  for(int i = 0; i < NB_STATES; i++ ){
	path.getpathnoext(path.STATES, filename);
	
	if (strlen(filename) + strlen(".dst") + strlen("-2147483648") /* = biggest string for i */ >MAX_PATH) return ;
	sprintf(filename+strlen(filename), ".ds%d", i);
	if( stat(filename,&sbuf) == -1 ) continue;
	savestates[i].exists = TRUE;
	strncpy(savestates[i].date, format_time(sbuf.st_mtime),40);
	savestates[i].date[40-1] = '\0';
    }

  return ;
}

void savestate_slot(int num)
{
   struct stat sbuf;
   char filename[MAX_PATH+1];

	lastSaveState = num;		//Set last savestate used

    path.getpathnoext(path.STATES, filename);

   if (strlen(filename) + strlen(".dsx") + strlen("-2147483648") /* = biggest string for num */ >MAX_PATH) return ;
   sprintf(filename+strlen(filename), ".ds%d", num);

   SSTRACE("savestate_slot(%d): enter, file=%s", num, filename);
   if (savestate_save(filename))
   {
	   SSTRACE("savestate_slot(%d): savestate_save() returned true", num);
//	   osd->setLineColor(255, 255, 255);
//	   osd->addLine("Saved to %i slot", num);
   }
   else
   {
	   SSTRACE("savestate_slot(%d): savestate_save() returned FALSE", num);
//	   osd->setLineColor(255, 0, 0);
//	   osd->addLine("Error saving %i slot", num);
	   return;
   }

   if (num >= 0 && num < NB_STATES)
   {
	   SSTRACE("savestate_slot(%d): before stat()", num);
	   if (stat(filename,&sbuf) != -1)
	   {
		   savestates[num].exists = TRUE;
		   strncpy(savestates[num].date, format_time(sbuf.st_mtime),40);
		   savestates[num].date[40-1] = '\0';
	   }
	   SSTRACE("savestate_slot(%d): after stat(), done", num);
   }
}

void loadstate_slot(int num)
{
   char filename[MAX_PATH];

   lastSaveState = num;		//Set last savestate used

    path.getpathnoext(path.STATES, filename);

   if (strlen(filename) + strlen(".dsx") + strlen("-2147483648") /* = biggest string for num */ >MAX_PATH) return ;
   sprintf(filename+strlen(filename), ".ds%d", num);
   if (savestate_load(filename))
   {
//	   osd->setLineColor(255, 255, 255);
//	   osd->addLine("Loaded from %i slot", num);
   }
   else
   {
//	   osd->setLineColor(255, 0, 0);
//	   osd->addLine("Error loading %i slot", num);
   }
}


// note: guessSF is so we don't have to do a linear search through the SFORMAT array every time
// in the (most common) case that we already know where the next entry is.
static const SFORMAT *CheckS(const SFORMAT *guessSF, const SFORMAT *firstSF, u32 size, u32 count, char *desc)
{
	const SFORMAT *sf = guessSF ? guessSF : firstSF;
	while(sf->v)
	{
		//NOT SUPPORTED RIGHT NOW
		//if(sf->size==~0)		// Link to another SFORMAT structure.
		//{
		//	SFORMAT *tmp;
		//	if((tmp= CheckS((SFORMAT *)sf->v, tsize, desc) ))
		//		return(tmp);
		//	sf++;
		//	continue;
		//}
		if(!memcmp(desc,sf->desc,4))
		{
			if(sf->size != size || sf->count != count)
				return 0;
			return sf;
		}

		// failed to find it, have to keep iterating
		if(guessSF)
		{
			sf = firstSF;
			guessSF = NULL;
		}
		else
		{
			sf++;
		}
	}
	return 0;
}


static bool ReadStateChunk(EMUFILE* is, const SFORMAT *sf, int size)
{
	const SFORMAT *tmp = NULL;
	const SFORMAT *guessSF = NULL;
	int temp = is->ftell();

	while(is->ftell()<temp+size)
	{
		u32 sz, count;

		char toa[4];
		is->fread(toa,4);
		if(is->fail())
			return false;

		if(!read32le(&sz,is)) return false;
		if(!read32le(&count,is)) return false;

		if((tmp=CheckS(guessSF,sf,sz,count,toa)))
		{
		#ifdef LOCAL_LE
			// no need to ever loop one at a time if not flipping byte order
			is->fread((char *)tmp->v,sz*count);
		#else
			if(sz == 1) {
				//special case: read a huge byte array
				is->fread((char *)tmp->v,count);
			} else {
				for(unsigned int i=0;i<count;i++)
				{
					is->fread((char *)tmp->v + i*sz,sz);
                    FlipByteOrder((u8*)tmp->v + i*sz,sz);
				}
			}
		#endif
			guessSF = tmp + 1;
		}
		else
		{
			is->fseek(sz*count,SEEK_CUR);
			guessSF = NULL;
		}
	} // while(...)
	return true;
}



static int SubWrite(EMUFILE* os, const SFORMAT *sf)
{
	uint32 acc=0;

#ifdef DEBUG
	std::set<std::string> keyset;
#endif

	const SFORMAT* temp = sf;
	while(temp->v) {
		const SFORMAT* seek = sf;
		while(seek->v && seek != temp) {
			if(!strcmp(seek->desc,temp->desc)) {
				printf("ERROR! duplicated chunk name: %s\n", temp->desc);
			}
			seek++;
		}
		temp++;
	}

	while(sf->v)
	{
		//not supported right now
		//if(sf->size==~0)		//Link to another struct
		//{
		//	uint32 tmp;

		//	if(!(tmp=SubWrite(os,(SFORMAT *)sf->v)))
		//		return(0);
		//	acc+=tmp;
		//	sf++;
		//	continue;
		//}

		int count = sf->count;
		int size = sf->size;

        //add size of current node to the accumulator
		acc += 4 + sizeof(sf->size) + sizeof(sf->count);
		acc += count * size;

		if(os)			//Are we writing or calculating the size of this block?
		{
			os->fwrite(sf->desc,4);
			write32le(sf->size,os);
			write32le(sf->count,os);

			#ifdef DEBUG
			//make sure we dont dup any keys
			if(keyset.find(sf->desc) != keyset.end())
			{
				printf("duplicate save key!\n");
				assert(false);
			}
			keyset.insert(sf->desc);
			#endif

		#ifdef LOCAL_LE
			// no need to ever loop one at a time if not flipping byte order
			os->fwrite((char *)sf->v,size*count);
		#else
			if(size == 1) {
				//special case: write a huge byte array
				os->fwrite((char *)sf->v,count);
			} else {
				for(int i=0;i<count;i++) {
					FlipByteOrder((u8*)sf->v + i*size, size);
					os->fwrite((char*)sf->v + i*size,size);
					//Now restore the original byte order.
					FlipByteOrder((u8*)sf->v + i*size, size);
				}
			}
		#endif
		}
		sf++;
	}

	return(acc);
}

static int savestate_WriteChunk(EMUFILE* os, int type, const SFORMAT *sf)
{
	write32le(type,os);
	if(!sf) return 4;
	int bsize = SubWrite((EMUFILE*)0,sf);
	write32le(bsize,os);

	if(!SubWrite(os,sf))
	{
		return 8;
	}
	return (bsize+8);
}

static void savestate_WriteChunk(EMUFILE* os, int type, void (*saveproc)(EMUFILE* os))
{
	u32 pos1 = os->ftell();

	//write the type, size(placeholder), and data
	write32le(type,os);
	os->fseek(4, SEEK_CUR); // skip the size, we write that later
	saveproc(os);

	//get the size
	u32 pos2 = os->ftell();
	assert(pos2 != (u32)-1); // if this assert fails, saveproc did something bad
	u32 size = (pos2 - pos1) - (2 * sizeof(u32));

	//fill in the actual size
	os->fseek(pos1 + sizeof(u32),SEEK_SET);
	write32le(size,os);
	os->fseek(pos2,SEEK_SET);

/*
// old version of this function,
// for reference in case the new one above starts misbehaving somehow:

	// - this is retarded. why not write placeholders for size and then write directly to the stream
	//and then go back and fill them in

	//get the size
	memorystream mstemp;
	saveproc(&mstemp);
	mstemp.flush();
	u32 size = mstemp.size();

	//write the type, size, and data
	write32le(type,os);
	write32le(size,os);
	os->write(mstemp.buf(),size);
*/
}

static void writechunks(EMUFILE* os);

bool savestate_save(EMUFILE* outstream, int compressionLevel)
{
	#ifndef HAVE_LIBZ
	compressionLevel = Z_NO_COMPRESSION;
	#endif

	EMUFILE_MEMORY ms;
	EMUFILE* os;
	
	SSTRACE("savestate_save(EMUFILE*): enter, compressionLevel=%d", compressionLevel);
	if(compressionLevel != Z_NO_COMPRESSION)
	{
		//generate the savestate in memory first
		os = (EMUFILE*)&ms;
		writechunks(os);
	}
	else
	{
		os = outstream;
		os->fseek(32,SEEK_SET); //skip the header
		writechunks(os);
	}
	SSTRACE("savestate_save(EMUFILE*): writechunks() returned");

	//save the length of the file
	u32 len = os->ftell();
	SSTRACE("savestate_save(EMUFILE*): len=%u", (unsigned)len);

	u32 comprlen = 0xFFFFFFFF;
	u8* cbuf = 0;

	//compress the data
	int error = Z_OK;
	if(compressionLevel != Z_NO_COMPRESSION)
	{
		cbuf = ms.buf();
		uLongf comprlen2;
		//worst case compression.
		//zlib says "0.1% larger than sourceLen plus 12 bytes"
		comprlen = (len>>9)+12 + len;
		SSTRACE("savestate_save(EMUFILE*): before compress2, worst-case=%u", (unsigned)comprlen);
		cbuf = new u8[comprlen];
		// Workaround to make it compile under linux 64bit
		comprlen2 = comprlen;
		error = compress2(cbuf,&comprlen2,ms.buf(),len,compressionLevel);
		comprlen = (u32)comprlen2;
		SSTRACE("savestate_save(EMUFILE*): after compress2, err=%d comprlen=%u", error, (unsigned)comprlen);
	}

	//dump the header
	outstream->fseek(0,SEEK_SET);
	outstream->fwrite(magic,16);
	write32le(SAVESTATE_VERSION,outstream);
	write32le(EMU_DESMUME_VERSION_NUMERIC(),outstream); //desmume version
	write32le(len,outstream); //uncompressed length
	write32le(comprlen,outstream); //compressed length (-1 if it is not compressed)

	if(compressionLevel != Z_NO_COMPRESSION)
	{
		SSTRACE("savestate_save(EMUFILE*): before final fwrite of %u bytes", comprlen==(u32)-1?len:comprlen);
		outstream->fwrite((char*)cbuf,comprlen==(u32)-1?len:comprlen);
		SSTRACE("savestate_save(EMUFILE*): after final fwrite");
		delete[] cbuf;
	}

	SSTRACE("savestate_save(EMUFILE*): returning %d", error == Z_OK);
	return error == Z_OK;
}

bool savestate_save (const char *file_name)
{
	EMUFILE_MEMORY ms;
	size_t elems_written;
	SSTRACE("savestate_save(\"%s\"): enter", file_name);
#ifdef HAVE_LIBZ
	if(!savestate_save(&ms, Z_DEFAULT_COMPRESSION))
#else
	if(!savestate_save(&ms, 0))
#endif
	{
		SSTRACE("savestate_save(\"%s\"): inner savestate_save() FAILED", file_name);
		return false;
	}
	SSTRACE("savestate_save(\"%s\"): before fopen, total size=%u", file_name, (unsigned)ms.size());
	FILE* file = fopen(file_name,"wb");
	if(file)
	{
		SSTRACE("savestate_save(\"%s\"): before fwrite", file_name);
		elems_written = fwrite(ms.buf(), 1, ms.size(), file);
		SSTRACE("savestate_save(\"%s\"): after fwrite, wrote=%u", file_name, (unsigned)elems_written);
		fclose(file);
		SSTRACE("savestate_save(\"%s\"): after fclose, returning %d", file_name, elems_written == (size_t)(ms.size()));
		return (elems_written == (size_t)(ms.size()));
	} else {
		SSTRACE("savestate_save(\"%s\"): fopen FAILED", file_name);
		return false;
	}
}

extern SFORMAT SF_RTC[];

static void writechunks(EMUFILE* os) {
	SF_MEM_rebind();   // heap MMU buffers -> SF_MEM[].v (see SF_MEM above)
#ifdef DESMUME_SAVESTATE_DIAG
	// ftell() delta around each chunk -- works uniformly for both
	// savestate_WriteChunk() overloads (one returns a byte count, the other
	// void) and is what actually correlates with the file growing between
	// otherwise-identical saves in the same session.
	#define WC(call) do { u32 _p0 = os->ftell(); call; u32 _p1 = os->ftell(); \
		SSTRACE("writechunks: %-28s = %u bytes", #call, (unsigned)(_p1 - _p0)); } while(0)
#else
	#define WC(call) call
#endif
	WC(savestate_WriteChunk(os,1,SF_ARM9));
	WC(savestate_WriteChunk(os,2,SF_ARM7));
	WC(savestate_WriteChunk(os,3,cp15_savestate));
	WC(savestate_WriteChunk(os,4,SF_MEM));
	WC(savestate_WriteChunk(os,5,SF_NDS));
	WC(savestate_WriteChunk(os,51,nds_savestate));
	WC(savestate_WriteChunk(os,60,SF_MMU));
	WC(savestate_WriteChunk(os,61,mmu_savestate));
	WC(savestate_WriteChunk(os,7,gpu_savestate));
	WC(savestate_WriteChunk(os,8,spu_savestate));
	WC(savestate_WriteChunk(os,81,mic_savestate));
	WC(savestate_WriteChunk(os,90,SF_GFX3D));
	WC(savestate_WriteChunk(os,91,gfx3d_savestate));
	WC(savestate_WriteChunk(os,110,SF_WIFI));
	WC(savestate_WriteChunk(os,120,SF_RTC));
	// PLAN.md §4.3 item 7 (GBA savestate support): only emitted for a GBA
	// session -- a DS savestate carries no GBA chunks at all, so loading an
	// old (pre-this-feature) or DS-mode savestate is unaffected either way.
	if (gameInfo.isGBA) {
		WC(savestate_WriteChunk(os,200,SF_GBA_MEM));
		WC(savestate_WriteChunk(os,201,gba_modules_savestate));
	}
	savestate_WriteChunk(os,0xFFFFFFFF,(SFORMAT*)0);
	SSTRACE("writechunks: done, terminator written");
	#undef WC
}

static bool ReadStateChunks(EMUFILE* is, s32 totalsize)
{
	bool ret = true;
	while(totalsize > 0)
	{
		uint32 size;
		u32 t;
		if(!read32le(&t,is))  { ret=false; break; }
		if(t == 0xFFFFFFFF) goto done;
		if(!read32le(&size,is))  { ret=false; break; }
		switch(t)
		{
			case 1: if(!ReadStateChunk(is,SF_ARM9,size)) ret=false; break;
			case 2: if(!ReadStateChunk(is,SF_ARM7,size)) ret=false; break;
			case 3: if(!cp15_loadstate(is,size)) ret=false; break;
			case 4: if(!ReadStateChunk(is,SF_MEM,size)) ret=false; break;
			case 5: if(!ReadStateChunk(is,SF_NDS,size)) ret=false; break;
			case 51: if(!nds_loadstate(is,size)) ret=false; break;
			case 60: if(!ReadStateChunk(is,SF_MMU,size)) ret=false; break;
			case 61: if(!mmu_loadstate(is,size)) ret=false; break;
			case 7: if(!gpu_loadstate(is,size)) ret=false; break;
			case 8: if(!spu_loadstate(is,size)) ret=false; break;
			case 81: if(!mic_loadstate(is,size)) ret=false; break;
			// writechunks() always emits chunk 90 (SF_GFX3D); this case was
			// left commented out, so every load hit `default:` and aborted
			// with "failed halfway through". Restore the upstream reader.
			case 90: if(!ReadStateChunk(is,SF_GFX3D,size)) ret=false; break;
			case 91: if(!gfx3d_loadstate(is,size)) ret=false; break;
			// No movies
			//case 100: if(!ReadStateChunk(is,SF_MOVIE, size)) ret=false; break;
			//case 101: if(!mov_loadstate(is, size)) ret=false; break;
			case 110: if(!ReadStateChunk(is,SF_WIFI,size)) ret=false; break;
			case 120: if(!ReadStateChunk(is,SF_RTC,size)) ret=false; break;
			// PLAN.md §4.3 item 7 (GBA savestate support): only present when
			// the chunk was written by a GBA session (see writechunks()).
			case 200: if(!ReadStateChunk(is,SF_GBA_MEM,size)) ret=false; break;
			case 201: if(!gba_modules_loadstate(is,size)) ret=false; break;
			default:
				ret=false;
				break;
		}
		if(!ret) {
#ifdef DESMUME_SAVESTATE_DIAG
			printf("[ss] ReadStateChunks FAILED at chunk t=%u size=%u\n", t, size);
#endif
			return false;
		}
#ifdef DESMUME_SAVESTATE_DIAG
		printf("[ss] chunk t=%u size=%u ok\n", t, size);
#endif
	}
done:

	return ret;
}

static void loadstate()
{
    // This should regenerate the vram banks
    for (int i = 0; i < 0xA; i++)
       _MMU_write08<ARMCPU_ARM9>(0x04000240+i, _MMU_read08<ARMCPU_ARM9>(0x04000240+i));

    // This should regenerate the graphics power control register
    _MMU_write16<ARMCPU_ARM9>(0x04000304, _MMU_read16<ARMCPU_ARM9>(0x04000304));

	// This should regenerate the graphics configuration
	//zero 27-jul-09 : was formerly up to 7F but that wrote to dispfifo which is dumb (one of nitsuja's desynch bugs [that he found, not caused])
	//so then i brought it down to 66 but this resulted in a conceptual bug with affine start registers, which shouldnt get regenerated
	//so then i just made this exhaustive list
 //   for (int i = REG_BASE_DISPA; i<=REG_BASE_DISPA + 0x66; i+=2)
	//_MMU_write16<ARMCPU_ARM9>(i, _MMU_read16<ARMCPU_ARM9>(i));
 //   for (int i = REG_BASE_DISPB; i<=REG_BASE_DISPB + 0x7F; i+=2)
	//_MMU_write16<ARMCPU_ARM9>(i, _MMU_read16<ARMCPU_ARM9>(i));
	static const u8 mainRegenAddr[] = {0x00,0x02,0x08,0x0a,0x0c,0x0e,0x40,0x42,0x44,0x46,0x48,0x4a,0x4c,0x50,0x52,0x54,0x64,0x66,0x6c};
	static const u8 subRegenAddr[] =  {0x00,0x02,0x08,0x0a,0x0c,0x0e,0x40,0x42,0x44,0x46,0x48,0x4a,0x4c,0x50,0x52,0x54,0x6c};
	for(u32 i=0;i<ARRAY_SIZE(mainRegenAddr);i++)
		_MMU_write16<ARMCPU_ARM9>(REG_BASE_DISPA+mainRegenAddr[i], _MMU_read16<ARMCPU_ARM9>(REG_BASE_DISPA+mainRegenAddr[i]));
	for(u32 i=0;i<ARRAY_SIZE(subRegenAddr);i++)
		_MMU_write16<ARMCPU_ARM9>(REG_BASE_DISPB+subRegenAddr[i], _MMU_read16<ARMCPU_ARM9>(REG_BASE_DISPB+subRegenAddr[i]));
	// no need to restore 0x60 since control and MMU.ARM9_REG are both in the savestates, and restoring it could mess up the ack bits anyway

	SetupMMU();

#ifdef DESMUME_JIT_ARM7
	// P4: a state load (or rewind) overwrites MAIN_MEM/SWIRAM/ARM7_ERAM in bulk
	// via direct buffer copies (ReadStateChunks -> mmu_loadstate), completely
	// bypassing every _MMU_write* SMC hook above. Any block already compiled
	// against the pre-load bytes is now stale -- there's no address-range
	// classification possible here (fixed-size correctness > incremental
	// perf), so just drop the whole cache -- both cores'.
	jitFlushAllCaches();
#endif

	execute = 1;//!driver->EMU_IsEmulationPaused();
}

bool savestate_load(EMUFILE* is)
{
	SAV_silent_fail_flag = false;
	char header[16];
	is->fread(header,16);
	if(is->fail() || memcmp(header,magic,16))
		return false;

	u32 ssversion,dversion,len,comprlen;
	if(!read32le(&ssversion,is)) return false;
	if(!read32le(&dversion,is)) return false;
	if(!read32le(&len,is)) return false;
	if(!read32le(&comprlen,is)) return false;

	if(ssversion != SAVESTATE_VERSION) return false;

	std::vector<u8> buf(len);

	if(comprlen != 0xFFFFFFFF) {
#ifndef HAVE_LIBZ
		//without libz, we can't decompress this savestate
		return false;
#endif
		std::vector<char> cbuf(comprlen);
		is->fread(&cbuf[0],comprlen);
		if(is->fail()) return false;

#ifdef HAVE_LIBZ
		uLongf uncomprlen = len;
		int error = uncompress((uint8*)&buf[0],&uncomprlen,(uint8*)&cbuf[0],comprlen);
		if(error != Z_OK || uncomprlen != len)
			return false;
#endif
	} else {
		is->fread((char*)&buf[0],len-32);
	}

	//GO!! READ THE SAVESTATE
	//THERE IS NO GOING BACK NOW
	//reset the emulator first to clean out the host's state

	//while the series of resets below should work,
	//we are testing the robustness of the savestate system with this full reset.
	//the full reset wipes more things, so we can make sure that they are being restored correctly
	extern bool _HACK_DONT_STOPMOVIE;
	_HACK_DONT_STOPMOVIE = true;
	NDS_Reset();
	_HACK_DONT_STOPMOVIE = false;

	//GPU_Reset(MainScreen.gpu, 0);
	//GPU_Reset(SubScreen.gpu, 1);
	//gfx3d_reset();
	//gpu3D->NDS_3D_Reset();
	//SPU_Reset();

	SF_MEM_rebind();   // NDS_Reset() above may have realloc'd the MMU buffers

	EMUFILE_MEMORY mstemp(&buf);
	bool x = ReadStateChunks(&mstemp,(s32)len);

	if(!x && !SAV_silent_fail_flag)
	{
		printf("Error loading savestate. It failed halfway through;\nSince there is no savestate backup system, your current game session is wrecked");
		return false;
	}

	loadstate();

	return true;
}

bool savestate_load(const char *file_name)
{
	EMUFILE_FILE f(file_name,"rb");
	if(f.fail()) return false;

	return savestate_load(&f);
}

static std::stack<EMUFILE_MEMORY*> rewindFreeList;
static std::vector<EMUFILE_MEMORY*> rewindbuffer;

int rewindstates = 16;
int rewindinterval = 4;

void rewindsave () {

	//printf("rewindsave"); printf("%d%s", currFrameCounter, "\n");

	
	EMUFILE_MEMORY *ms;
	if(!rewindFreeList.empty()) {
		ms = rewindFreeList.top();
		rewindFreeList.pop();
	} else {
		ms = new EMUFILE_MEMORY(1024*1024*12);
	}

	if(!savestate_save(ms, Z_NO_COMPRESSION))
		return;

	rewindbuffer.push_back(ms);
	
	if((int)rewindbuffer.size() > rewindstates) {
		delete *rewindbuffer.begin();
		rewindbuffer.erase(rewindbuffer.begin());
	}
}

void dorewind()
{
	//printf("rewind\n");

	int size = rewindbuffer.size();

	if(size < 1) {
		printf("rewind buffer empty\n");
		return;
	}

	printf("%d", size);

	EMUFILE_MEMORY* loadms = rewindbuffer[size-1];
	loadms->fseek(32, SEEK_SET);

	ReadStateChunks(loadms,loadms->size()-32);
	loadstate();

	if(rewindbuffer.size()>1)
	{
		rewindFreeList.push(loadms);
		rewindbuffer.pop_back();
	}

}
