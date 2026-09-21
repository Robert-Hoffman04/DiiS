// DS Engine B (sub screen) instance of the GX 2D compositor. The whole body lives
// in gx_ds_engine_impl.inc (shared with Engine A, see the header there); this file
// just selects the core and the public names. Behaviour is the tasks 7-11 code.
#define GXDS_CORE 1
#define GXDS_SCREEN SubScreen
#define GXDS_API(n) gxDsEngineB##n
#define GXDS_PLAN g_dsBFramePlan
#define GXDS_REGS g_dsBBandRegs
#define GXDS_TAG "dsb"
#ifdef DSB_FORCE_CPU
#define GXDS_FORCE_CPU 1
#endif
#include "gx_ds_engine_impl.inc"

// Shared write-funnel entry points (declared in gx_ds_engineb_render.h): one call from
// MMU.cpp / GPU.cpp tags both engines' dirty plans.
// ---------------------------------------------------------------------------------------------
// Queue item 13j: deferred CPU line rendering -- barrier dispatch (see gx_ds_engine_impl.inc).
// ---------------------------------------------------------------------------------------------
u32 g_gxDsLazyPend = 0;
u32 g_gxDsLazyOn = 0;

// Which LCDC 16KB pages (MMU.ARM9_LCD, 41 of them) can be read by which engine's 2D compositor:
// the pages behind its BG and OBJ windows (vram_arm9_map) plus the extended BG / OBJ palette
// slots (MMU.ExtPal / ObjExtPal point straight into ARM9_LCD). A superset of any bake's/line's
// real read set, so a write that misses this map cannot change a deferred line. Recomputed
// lazily after a VRAMCNT write (which is itself a barrier), before the next VRAM query.
static u8 s_lzVis[48];        // bit 0 = Engine A, bit 1 = Engine B
static bool s_lzVisDirty = true;

static void gxDsLzVisMark(int e, const u8 *p, u32 len)
{
	const s32 off = (s32)(p - MMU.ARM9_LCD);
	if (p < MMU.ARM9_LCD || off >= 0xA4000) return;
	for (u32 pg = (u32)off >> 14; pg <= ((u32)off + len - 1) >> 14 && pg < 41; ++pg)
		s_lzVis[pg] |= (u8)(1u << e);
}

static void gxDsLzVisRecompute()
{
	memset(s_lzVis, 0, sizeof(s_lzVis));
	for (int e = 0; e < 2; ++e) {
		const int bg = e ? VRAM_PAGE_BBG : VRAM_PAGE_ABG, obj = e ? VRAM_PAGE_BOBJ : VRAM_PAGE_AOBJ;
		for (int i = 0; i < 128; ++i) {
			u32 lp = vram_arm9_map[bg + i];
			if (lp < 41) s_lzVis[lp] |= (u8)(1u << e);
			lp = vram_arm9_map[obj + i];
			if (lp < 41) s_lzVis[lp] |= (u8)(1u << e);
		}
		for (int i = 0; i < 4; ++i)
			if (MMU.ExtPal[e][i]) gxDsLzVisMark(e, MMU.ExtPal[e][i], 8192);
		if (MMU.ObjExtPal[e][0]) gxDsLzVisMark(e, MMU.ObjExtPal[e][0], 8192);
	}
	s_lzVisDirty = false;
}

void gxDsLazyFlushAll() { gxDsEngineALazyBarrier(0); gxDsEngineBLazyBarrier(0); }
void gxDsLazyVramRemapped() { s_lzVisDirty = true; }

// Register / remap writes (adr = 0x04xxxxxx, called before the write lands).
void gxDsLazyIoWriteSlow(u32 adr)
{
	if ((adr & 0xFFFFEF80u) == 0x04000000u) {
		// 0x04000000.. = Engine A, 0x04001000.. = Engine B. DISPSTAT/VCOUNT (0x04-0x07) and DISP3DCNT
		// (0x60-0x63) are not compositor state.
		const u32 off = adr & 0x7Fu;
		if (off < 4 || (off >= 8 && off < 0x60) || (off >= 0x64 && off < 0x70)) {
			if (adr & 0x1000u) gxDsEngineBLazyBarrier(0); else gxDsEngineALazyBarrier(0);
		}
		return;
	}
	if (adr - 0x04000240u < 0xAu) {          // VRAMCNT_A..I / WRAMCNT: remaps what the engines see
		s_lzVisDirty = true;
		// A remap during a visible frame changes what the CPU compositor reads from that line on, whereas
		// the GX pass bakes the frame-final mapping: treat it like a data write (flush + GX bails).
		gxDsEngineALazyBarrier(1);
		gxDsEngineBLazyBarrier(1);
		return;
	}
	if ((adr & ~3u) == 0x04000304u)          // POWCNT1: screen swap / engine enables
		gxDsLazyFlushAll();
}

// Palette RAM (0x05) / OAM (0x07): 2KB, Engine A = first KB, Engine B = second KB.
static void gxDsLazyDataWrite(u32 adr)
{
	const u32 region = (adr >> 24) & 0xF;
	if (region != 5 && region != 7) return;
	if ((adr >> 10) & 1) gxDsEngineBLazyBarrier(1); else gxDsEngineALazyBarrier(1);
}

// VRAM, by LCDC byte offset (post MMU_LCDmap).
static void gxDsLazyVramWrite(u32 off)
{
	if (s_lzVisDirty) gxDsLzVisRecompute();
	const u32 pg = off >> 14;
	if (pg >= 41) return;
	const u8 m = s_lzVis[pg];
	if (m & 1) gxDsEngineALazyBarrier(1);
	if (m & 2) gxDsEngineBLazyBarrier(1);
}

void gxDsMarkWrite(u32 adr, u32 size)     { if (g_gxDsLazyOn) gxDsLazyDataWrite(adr); gxDsEngineAMarkWrite(adr, size); gxDsEngineBMarkWrite(adr, size); }
void gxDsMarkVram(u32 lcdcOffset, u32 size) { if (g_gxDsLazyOn) gxDsLazyVramWrite(lcdcOffset); gxDsEngineAMarkVram(lcdcOffset, size); gxDsEngineBMarkVram(lcdcOffset, size); }
void gxDsInvalidateAll()                  { gxDsEngineAInvalidateAll(); gxDsEngineBInvalidateAll(); s_lzVisDirty = true; }
