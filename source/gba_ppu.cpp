/*  Copyright (C) 2012 DeSmuMEWii team

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

// docs/PLAN.md §4.3 step 1 (peripherals): GBA PPU. Reference is GBATEK's
// documented register/tile/OBJ layout (cross-checked against mGBA's
// video/*.c behavior where GBATEK was ambiguous, per the plan's
// reference-first rule for the "actual GBA execution/peripheral"
// dimension); this is new code written from that spec, not a port of any
// existing DS or GBA emulator's renderer -- the DS 2D engine (GPU.cpp) is
// architecturally similar but not identical (different register layout,
// no GBA-style single-screen bitmap modes 3-5 on the DS engines, different
// affine-wrap/mosaic details), and mixing GBA-mode paths into it risked
// exactly the aliasing-class bugs the memory-map seam (MMU.cpp) already
// had to fix once.

#include "gba_ppu.h"
#include "MMU.h"
#include "mem.h"
#include "GPU.h"
#include "armcpu.h"
#include "gba_irq.h"
#include "readwrite.h"
#include "perf_zones.h"
#include "gx/gx_gba_render.h"
#include <string.h>

u16 GBA_screen[GBA_SCREEN_W * GBA_SCREEN_H];

GxFramePlan g_gbaFramePlan;

// I/O register offsets: see the enum in gba_ppu.h.

static inline u16 io16(u32 off) { return T1ReadWord(MMU.GBA_IOREG, off); }
static inline void io16w(u32 off, u16 v) { T1WriteWord(MMU.GBA_IOREG, off, v); }

// ---------------------------------------------------------------------
// DISPSTAT/VCOUNT: bits 0-2 of DISPSTAT and all of VCOUNT are hardware-
// owned; bits 3-5 (IRQ enables) and 8-15 (VCount-match setting) of
// DISPSTAT are software-writable. gbaPpuHDrawEnd/HBlankEnd keep the
// hardware bits authoritative in the IOREG buffer directly (so a plain
// read through the generic MMU path already sees the right value); writes
// route through gbaPpuIoWrite16/8/32 below to keep software from
// clobbering the hardware bits.
// ---------------------------------------------------------------------
static u16 s_vcount = 0;

// Affine BG (modes 1/2) reference-point accumulator -- declared here (ahead
// of its home comment/functions near sampleAffineBg below) purely so
// gxSnapshotBandRegs can read it; see that later comment for the
// latch/advance semantics and the mid-frame-reload simplification this
// implies for gx_gba_render.cpp's affine BG path too.
static s32 s_affX[2], s_affY[2]; // index 0 = BG2, 1 = BG3, 20.8 fixed point

// Per-scanline history of the above, captured (gbaPpuHDrawEnd) just before
// that line renders -- i.e. the exact reference-point value in effect for
// that line, before advanceAffine()'s post-render PB/PD step. Needed only
// for affine BG mosaic's V-snap (sampleAffineBg below): mosaic replaces the
// current line's reference point with whatever was in effect at the
// mosaic-block-aligned line above it, which (unlike text-mode mosaic's
// plain coordinate snap) can't be recovered from the current line's
// accumulator value alone since it's an iterative per-line accumulation,
// not a pure function of the line number.
static s32 s_affXHistory[GBA_SCREEN_H][2];
static s32 s_affYHistory[GBA_SCREEN_H][2];

static void setDispstatFlags(bool vblank, bool hblank)
{
	u16 v = io16(IO_DISPSTAT);
	u16 vcountSetting = (v >> 8) & 0xFF;
	bool vcounterMatch = (vcountSetting == s_vcount);
	v = (u16)((v & 0xFFF8) | (vblank ? 1 : 0) | (hblank ? 2 : 0) | (vcounterMatch ? 4 : 0));
	io16w(IO_DISPSTAT, v);
}

// ---------------------------------------------------------------------
// Stage 0 register-write classification (see gx_frameplan.h / gba_ppu.h's
// g_gbaFramePlan). Deliberately conservative: any write that lands inside
// one of these ranges records a band boundary regardless of whether the
// value actually changed (a same-value rewrite is rare and merely costs
// one redundant band split, never a missed one) - see gxIsLayoutRegister's
// range list, which is exactly Stage 4's 2D-compositor state: BG
// mode/size/priority, per-scanline scroll/affine rewrites, windows,
// blend, mosaic, and the master enable bits in DISPCNT.
// ---------------------------------------------------------------------
static bool gxIsLayoutRegister(u32 offset)
{
	if (offset <= 0x001) return true;             // DISPCNT
	if (offset >= 0x008 && offset <= 0x03F) return true; // BG0-3 CNT/HOFS/VOFS/affine
	if (offset >= 0x040 && offset <= 0x04D) return true; // WIN0/1 H/V, WININ/WINOUT, MOSAIC
	if (offset >= 0x050 && offset <= 0x055) return true; // BLDCNT/BLDALPHA/BLDY
	return false;
}

// GXHOT_OBJWIN now means specifically "OBJ window (DISPCNT's WINOBJ enable
// bit) is active this frame", not "any window feature" as it did before
// gx-next-steps-log.md's task 2: WIN0/WIN1 are implemented natively in
// gx_gba_render.cpp now (rectangular GX scissor decomposition, exact), so
// they no longer need to force a CPU-bail; OBJ window's sprite-shaped mask
// still does, since it isn't expressible as a GX scissor rect -- see that
// file's header comment. GXHOT_MOSAIC and GXHOT_BLEND keep their original
// "any use of this register group" meaning: gx_gba_render.cpp still bails
// the whole frame on any mosaic use (narrower reasoning documented in its
// header), and refines blend down to a real per-frame precondition check
// of its own (gxBlendPlanForFrame) rather than an unconditional bail.
static void gxUpdateHotFlags()
{
	u16 dispcnt = io16(IO_DISPCNT);
	bool winObj = (dispcnt >> 15) & 1;
	g_gbaFramePlan.setHot(GXHOT_OBJWIN, winObj);
	g_gbaFramePlan.setHot(GXHOT_MOSAIC, io16(IO_MOSAIC) != 0);
	g_gbaFramePlan.setHot(GXHOT_BLEND, (io16(IO_BLDCNT) & 0x3F3F) != 0);
}

// Snapshots the per-band register state gx_gba_render.cpp's Stage 4 needs
// (see gx_gba_render.h's GxGbaBandRegs comment) into g_gbaBandRegs[index].
// Called once at frame start (index 0) and once per band boundary
// thereafter (index == g_gbaFramePlan.bands.boundaryCount() right after
// recordChangeAtLine() runs) -- relies on register writes landing in
// non-decreasing scanline order within a frame, which is guaranteed here
// since this is driven live by the scanline executor, never replayed
// out of order.
static void gxSnapshotBandRegs(GxGbaBandRegs *r)
{
	r->dispcnt = io16(IO_DISPCNT);
	for (int i = 0; i < 4; ++i) {
		r->bgcnt[i] = io16(IO_BG0CNT + i * 2);
		r->hofs[i] = io16(IO_BG0HOFS + i * 4) & 0x1FF;
		r->vofs[i] = io16(IO_BG0HOFS + i * 4 + 2) & 0x1FF;
	}
	for (int w = 0; w < 2; ++w) {
		r->affX[w] = s_affX[w];
		r->affY[w] = s_affY[w];
		u32 base = w ? IO_BG3PA : IO_BG2PA;
		r->affPA[w] = (s16)io16(base);
		r->affPB[w] = (s16)io16(base + 2);
		r->affPC[w] = (s16)io16(base + 4);
		r->affPD[w] = (s16)io16(base + 6);
	}
	r->win0h = io16(IO_WIN0H);
	r->win1h = io16(IO_WIN1H);
	r->win0v = io16(IO_WIN0V);
	r->win1v = io16(IO_WIN1V);
	r->winIn = io16(IO_WININ);
	r->winOut = io16(IO_WINOUT);
	r->bldcnt = io16(IO_BLDCNT);
	r->bldalpha = io16(IO_BLDALPHA);
	r->bldy = io16(IO_BLDY);
}

// Called from every generic (non DISPSTAT/VCOUNT/IF) register write below,
// after the write has landed, so gxUpdateHotFlags() sees the new value.
static void gxOnRegisterWritten(u32 offset)
{
	if (!gxIsLayoutRegister(offset))
		return;
	g_gbaFramePlan.bands.recordChangeAtLine(s_vcount);
	gxUpdateHotFlags();
	int bc = g_gbaFramePlan.bands.boundaryCount();
	if (bc > 0 && bc < GX_GBA_MAX_BAND_REGS)
		gxSnapshotBandRegs(&g_gbaBandRegs[bc]);
}

void gbaPpuIoWrite8(u32 offset, u8 val)
{
	// Byte-granular writes to DISPSTAT/VCOUNT are rare in practice (both
	// are normally accessed as halfwords); handle them precisely via the
	// 16-bit path for correctness rather than duplicating the masking
	// logic here.
	if (offset == IO_DISPSTAT || offset == IO_DISPSTAT + 1) {
		u16 cur = io16(IO_DISPSTAT);
		u16 merged = (offset == IO_DISPSTAT) ? (u16)((cur & 0xFF00) | val) : (u16)((cur & 0x00FF) | (val << 8));
		gbaPpuIoWrite16(IO_DISPSTAT, merged);
		return;
	}
	if (offset == IO_VCOUNT || offset == IO_VCOUNT + 1)
		return; // VCOUNT is read-only.
	if (offset == IO_IF || offset == IO_IF + 1) {
		u16 cur = io16(IO_IF);
		u16 bit = (offset == IO_IF) ? val : (val << 8);
		io16w(IO_IF, (u16)(cur & ~bit)); // write-1-to-clear, see gbaPpuIoWrite16
		return;
	}
	T1WriteByte(MMU.GBA_IOREG, offset, val);
	gxOnRegisterWritten(offset);
}

void gbaPpuIoWrite16(u32 offset, u16 val)
{
	if (offset == IO_DISPSTAT) {
		u16 hw = io16(IO_DISPSTAT) & 0x0007;
		io16w(IO_DISPSTAT, (u16)(hw | (val & 0xFF38)));
		return;
	}
	if (offset == IO_VCOUNT)
		return; // read-only
	if (offset == IO_IF) {
		// GBATEK: IF is write-1-to-clear, not a plain store -- a bit
		// written as 1 clears that IF bit, a bit written as 0 leaves it
		// alone. This is not optional/cosmetic: the extremely common ISR
		// epilogue idiom "u16 f = REG_IF; ... REG_IF = f;" relies on
		// exactly this semantic to ack only the bits it just read. A
		// plain store here was found live (Minish Cap) to make that idiom
        // a no-op -- REG_IF reads back the same value it just "acked",
		// so gbaIrqDispatchIfPending() sees IE&IF still true on the very
		// next chunk boundary and redelivers the same interrupt forever,
		// starving the game's own main loop of any chance to run.
		io16w(IO_IF, (u16)(io16(IO_IF) & ~val));
		return;
	}
	io16w(offset, val);
	gxOnRegisterWritten(offset);
}

void gbaPpuIoWrite32(u32 offset, u32 val)
{
	if (offset == IO_DISPSTAT) {
		gbaPpuIoWrite16(IO_DISPSTAT, (u16)val);
		return; // high half is VCOUNT -- read-only, dropped.
	}
	if (offset == IO_IF) {
		gbaPpuIoWrite16(IO_IF, (u16)val); // high half is IME's low byte, at 0x204 -- out of scope for a 0x202-based 32-bit write on real hardware too (unaligned register straddle), dropped same as DISPSTAT/VCOUNT above.
		return;
	}
	if (offset == IO_IE) {
		// A very common libgba/agbcc-derived idiom writes IE and IF
		// together as one 32-bit store at 0x04000200 (low 16 = IE, high
		// 16 = IF) -- IE's low half is a plain store, IF's high half
		// still needs write-1-to-clear, same as the 16-bit path.
		io16w(IO_IE, (u16)val);
		gbaPpuIoWrite16(IO_IF, (u16)(val >> 16));
		return;
	}
	T1WriteLong(MMU.GBA_IOREG, offset, val);
	gxOnRegisterWritten(offset);
	gxOnRegisterWritten(offset + 2); // a 32-bit store spans two 16-bit registers
}

void gbaPpuReset()
{
	s_vcount = 0;
	memset(GBA_screen, 0, sizeof(GBA_screen));

	// Stage 0 dirty-bitmap sizing (once per GBA-mode boot, not per frame -
	// see gx_frameplan.h). Palette/OAM are both a flat 1KB (MMU.h): 32-byte
	// pages give 32 pages each, i.e. 16-color-palette-bank and
	// 4-OAM-entry granularity - fine enough that one sprite's attribute
	// write doesn't mark every sprite dirty. VRAM's 256-byte pages (96KB /
	// 256 = 384 pages, comfortably under GxDirtyBitmap::kMaxPages) land
	// mid-way between a single tile (32-64 bytes) and the DS bank
	// granularity nds-wii-render-pipeline.md's Stage 0 describes (16KB) -
	// proportional for VRAM two orders of magnitude smaller.
	g_gbaFramePlan.vram.init(sizeof(MMU.GBA_VRAM), 256);
	g_gbaFramePlan.palette.init(sizeof(MMU.GBA_PALETTE), 32);
	g_gbaFramePlan.oam.init(sizeof(MMU.GBA_OAM), 32);
	g_gbaFramePlan.beginFrame();

	gxGbaRenderInit();
}

// ---------------------------------------------------------------------
// Palette / VRAM / OAM helpers
// ---------------------------------------------------------------------
static inline u16 bgPaletteColor(int idx) { return T1ReadWord(MMU.GBA_PALETTE, idx * 2); }
static inline u16 objPaletteColor(int idx) { return T1ReadWord(MMU.GBA_PALETTE, 0x200 + idx * 2); }

// MMU.GBA_VRAM is 0x18000 (96 KB) bytes -- real hardware mirrors every
// 0x20000 with the last 32 KB of each period re-mirroring the previous
// 32 KB (same quirk gbaDecodeAddr() in MMU.cpp applies for CPU accesses).
// Tile/map addresses computed here are expected to stay well inside the
// real 96 KB, but a bogus tile number from a broken/adversarial ROM could
// walk one off the end -- this keeps every VRAM access in-bounds instead
// of relying on that never happening.
static inline u32 vramAddr(u32 addr)
{
	u32 m = addr & 0x1FFFF;
	if (m >= 0x18000) m -= 0x8000;
	return m;
}

struct Pixel { u16 color; bool opaque; int priority; bool semiTransparent; };

// Mosaic block size from a MOSAIC-register nibble field: field value is
// "size - 1" (GBATEK), so a raw 0 means a 1-pixel block, i.e. no snapping.
static inline int mosaicSize(u16 mosaicReg, int shift) { return ((mosaicReg >> shift) & 0xF) + 1; }

// ---------------------------------------------------------------------
// Text-mode background (modes 0/1/2's non-affine BGs).
// ---------------------------------------------------------------------
static Pixel sampleTextBg(int bg, int line, int x)
{
	Pixel out = { 0, false, 0, false };
	u16 cnt = io16(IO_BG0CNT + bg * 2);
	int priority   = cnt & 3;
	u32 charBase   = ((cnt >> 2) & 3) * 0x4000;
	bool colorMode = (cnt >> 7) & 1; // 0=4bpp/16pal, 1=8bpp/256pal
	u32 mapBase    = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel    = (cnt >> 14) & 3;
	static const int mapWtiles[4] = { 32, 64, 32, 64 };
	static const int mapHtiles[4] = { 32, 32, 64, 64 };
	int mapWpx = mapWtiles[sizeSel] * 8, mapHpx = mapHtiles[sizeSel] * 8;

	// Mosaic (BGxCNT bit6): snap the *screen* sample coordinate down to the
	// mosaic-cell-aligned coordinate before doing anything else -- GBATEK:
	// "a mosaic-pixel is colorized by the color of the upperleft covered
	// pixel". x/line are always >= 0 here so plain '%' is safe.
	int sx = x, sline = line;
	if ((cnt >> 6) & 1) {
		u16 mos = io16(IO_MOSAIC);
		int hSize = mosaicSize(mos, 0), vSize = mosaicSize(mos, 4);
		sx = x - (x % hSize);
		sline = line - (line % vSize);
	}

	u16 hofs = io16(IO_BG0HOFS + bg * 4) & 0x1FF;
	u16 vofs = io16(IO_BG0HOFS + bg * 4 + 2) & 0x1FF;

	int px = (sx + hofs) % mapWpx;
	int py = (sline + vofs) % mapHpx;
	int tx = px / 8, ty = py / 8;

	// Larger maps are laid out as 32x32-tile screen blocks (0x800 bytes
	// each); pick the right block for this tile's quadrant.
	int blockX = tx / 32, blockY = ty / 32;
	int blockIdx = blockX + blockY * (mapWtiles[sizeSel] / 32);
	u32 entryAddr = mapBase + blockIdx * 0x800 + ((ty % 32) * 32 + (tx % 32)) * 2;
	u16 entry = T1ReadWord(MMU.GBA_VRAM, vramAddr(entryAddr));

	int tileNum = entry & 0x3FF;
	bool hflip = (entry >> 10) & 1;
	bool vflip = (entry >> 11) & 1;
	int palNum = (entry >> 12) & 0xF;

	int subx = px % 8, suby = py % 8;
	if (hflip) subx = 7 - subx;
	if (vflip) suby = 7 - suby;

	int colorIdx;
	if (!colorMode) {
		u32 tileAddr = charBase + tileNum * 32 + suby * 4 + subx / 2;
		u8 byte = T1ReadByte(MMU.GBA_VRAM, vramAddr(tileAddr));
		colorIdx = (subx & 1) ? (byte >> 4) : (byte & 0xF);
		if (colorIdx == 0) return out;
		out.color = bgPaletteColor(palNum * 16 + colorIdx);
	} else {
		u32 tileAddr = charBase + tileNum * 64 + suby * 8 + subx;
		colorIdx = T1ReadByte(MMU.GBA_VRAM, vramAddr(tileAddr));
		if (colorIdx == 0) return out;
		out.color = bgPaletteColor(colorIdx);
	}
	out.opaque = true;
	out.priority = priority;
	return out;
}

// ---------------------------------------------------------------------
// Affine background (BG2/BG3 in modes 1/2). Per-scanline reference point
// is latched at frame start and advanced by PB/PD each line by the
// caller (see s_affX/s_affY below) -- a documented simplification: real
// hardware re-latches immediately on a mid-frame BGxX/Y write, which this
// pass does not special-case.
// ---------------------------------------------------------------------
static void latchAffine(int which /*0=BG2,1=BG3*/)
{
	u32 base = which ? IO_BG3X : IO_BG2X;
	u32 lo = io16(base), hi = io16(base + 2);
	s32 v = (s32)(((u32)hi << 16) | lo);
	v = (v << 4) >> 4; // sign-extend from 28 bits
	s_affX[which] = v;
	base = which ? IO_BG3Y : IO_BG2Y;
	lo = io16(base); hi = io16(base + 2);
	v = (s32)(((u32)hi << 16) | lo);
	v = (v << 4) >> 4;
	s_affY[which] = v;
}

static void advanceAffine(int which)
{
	u32 base = which ? IO_BG3PA : IO_BG2PA;
	s16 pb = (s16)io16(base + 2);
	s16 pd = (s16)io16(base + 6);
	s_affX[which] += pb;
	s_affY[which] += pd;
}

static Pixel sampleAffineBg(int bg /*2 or 3*/, int line, int x)
{
	Pixel out = { 0, false, 0, false };
	int which = bg - 2;
	u16 cnt = io16(IO_BG0CNT + bg * 2);
	int priority = cnt & 3;
	u32 charBase = ((cnt >> 2) & 3) * 0x4000;
	u32 mapBase  = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel  = (cnt >> 14) & 3;
	bool wrap    = (cnt >> 13) & 1;
	int sizeTiles = 16 << sizeSel; // 16/32/64/128
	int sizePx = sizeTiles * 8;

	u32 base = which ? IO_BG3PA : IO_BG2PA;
	s16 pa = (s16)io16(base);
	s16 pc = (s16)io16(base + 4);

	// Mosaic (BGxCNT bit6): H-snap is a plain coordinate snap of the
	// screen column fed into the affine formula; V-snap has to substitute
	// the reference-point value that was actually in effect at the
	// mosaic-aligned line above this one (see s_affXHistory's comment) --
	// unlike text-mode BGs, the affine reference point is an iterative
	// per-line accumulator, not a pure function of (line, scroll).
	s32 originX = s_affX[which], originY = s_affY[which];
	int sx = x;
	if ((cnt >> 6) & 1) {
		u16 mos = io16(IO_MOSAIC);
		int hSize = mosaicSize(mos, 0), vSize = mosaicSize(mos, 4);
		sx = x - (x % hSize);
		int sline = line - (line % vSize);
		originX = s_affXHistory[sline][which];
		originY = s_affYHistory[sline][which];
	}

	s32 srcX = originX + sx * pa;
	s32 srcY = originY + sx * pc;
	int px = srcX >> 8, py = srcY >> 8;

	if (wrap) {
		px = ((px % sizePx) + sizePx) % sizePx;
		py = ((py % sizePx) + sizePx) % sizePx;
	} else if (px < 0 || py < 0 || px >= sizePx || py >= sizePx) {
		return out;
	}

	int tx = px / 8, ty = py / 8;
	u32 entryAddr = mapBase + (ty * sizeTiles + tx);
	u8 tileNum = T1ReadByte(MMU.GBA_VRAM, vramAddr(entryAddr));
	int subx = px % 8, suby = py % 8;
	u32 tileAddr = charBase + tileNum * 64 + suby * 8 + subx;
	u8 colorIdx = T1ReadByte(MMU.GBA_VRAM, vramAddr(tileAddr));
	if (colorIdx == 0) return out;
	out.color = bgPaletteColor(colorIdx);
	out.opaque = true;
	out.priority = priority;
	return out;
}

// ---------------------------------------------------------------------
// Bitmap-mode BG2 (modes 3/4/5) -- the only BG that exists in those modes.
// Same mosaic treatment as the tiled BGs above: snap the screen coordinate
// before indexing, so a mosaic'd bitmap frame still shows blocky-but-
// correct content rather than being silently ignored.
// ---------------------------------------------------------------------
static Pixel sampleBitmapBg2(int mode, u16 dispcnt, int line, int x)
{
	Pixel out = { 0, false, 0, false };
	u16 cnt = io16(IO_BG2CNT);
	out.priority = cnt & 3;

	int sx = x, sline = line;
	if ((cnt >> 6) & 1) {
		u16 mos = io16(IO_MOSAIC);
		int hSize = mosaicSize(mos, 0), vSize = mosaicSize(mos, 4);
		sx = x - (x % hSize);
		sline = line - (line % vSize);
	}

	if (mode == 3) {
		out.color = T1ReadWord(MMU.GBA_VRAM, (sline * GBA_SCREEN_W + sx) * 2);
		out.opaque = true;
	} else if (mode == 4) {
		u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;
		u8 idx = T1ReadByte(MMU.GBA_VRAM, page + sline * GBA_SCREEN_W + sx);
		if (idx) { out.color = bgPaletteColor(idx); out.opaque = true; }
	} else { // mode 5
		u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;
		const int W = 160, H = 128;
		if (sx < W && sline < H) {
			out.color = T1ReadWord(MMU.GBA_VRAM, page + (sline * W + sx) * 2);
			out.opaque = true;
		}
	}
	return out;
}

// ---------------------------------------------------------------------
// OBJ (sprites)
// ---------------------------------------------------------------------
static const u8 s_objSizeW[4][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32}, {0,0,0,0} };
static const u8 s_objSizeH[4][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64}, {0,0,0,0} };

// `winMask`, if non-null, is filled in for OBJ-window (objMode==2) entries:
// winMask[x] is set true wherever such a sprite's sampled texel is opaque.
// Those entries never touch `out[]` -- they mask, they don't draw (GBATEK).
// Passing null (windows inactive this frame, or DISPCNT's OBJ-window enable
// bit is off) makes objMode==2 entries a no-op, same as before this change.
static void renderObjLine(int line, Pixel out[GBA_SCREEN_W], bool *winMask)
{
	u16 dispcnt = io16(IO_DISPCNT);
	if (!((dispcnt >> 12) & 1)) return; // OBJ disabled
	bool oneDim = (dispcnt >> 6) & 1;
	u16 mosaicReg = io16(IO_MOSAIC);
	int objMosH = mosaicSize(mosaicReg, 8), objMosV = mosaicSize(mosaicReg, 12);

	for (int i = 0; i < 128; i++) {
		u32 oamOff = i * 8;
		u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
		u16 a1 = T1ReadWord(MMU.GBA_OAM, oamOff + 2);
		u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);

		bool affine = (a0 >> 8) & 1;
		bool doubleOrDisable = (a0 >> 9) & 1;
		if (!affine && doubleOrDisable) continue; // OBJ disable bit
		int objMode = (a0 >> 10) & 3;
		bool isWindowObj = (objMode == 2);
		if (isWindowObj && !winMask) continue; // OBJ-window feature inactive this frame
		bool semiTransparent = (objMode == 1);
		bool mosaicOn = (a0 >> 12) & 1;
		int shape = (a0 >> 14) & 3;
		if (shape == 3) continue; // prohibited

		int sizeSel = (a1 >> 14) & 3;
		int w = s_objSizeW[shape][sizeSel];
		int h = s_objSizeH[shape][sizeSel];
		if (w == 0) continue;

		int boxW = w, boxH = h;
		if (affine && doubleOrDisable) { boxW *= 2; boxH *= 2; } // double-size affine

		int y = a0 & 0xFF;
		if (y >= 160) y -= 256; // signed
		int rowInBox = line - y;
		if (rowInBox < 0 || rowInBox >= boxH) continue;

		int x = a1 & 0x1FF;
		if (x >= 240) x -= 512; // signed

		bool colorMode = (a0 >> 13) & 1; // 0=4bpp,1=8bpp
		int tileNum = a2 & 0x3FF;
		int priority = (a2 >> 10) & 3;
		int palNum = (a2 >> 12) & 0xF;

		s32 pa = 256, pb = 0, pc = 0, pd = 256; // identity, 8.8 fixed
		if (affine) {
			int grp = (a1 >> 9) & 0x1F;
			pa = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 6);
			pb = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 14);
			pc = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 22);
			pd = (s16)T1ReadWord(MMU.GBA_OAM, grp * 32 + 30);
		}

		bool hflip = false, vflip = false;
		if (!affine) { hflip = (a1 >> 12) & 1; vflip = (a1 >> 13) & 1; }

		int tileBytes = colorMode ? 64 : 32;
		u32 charBase = 0x10000; // OBJ tiles always start at VRAM+0x10000

		for (int col = 0; col < boxW; col++) {
			int sx = x + col;
			if (sx < 0 || sx >= GBA_SCREEN_W) continue;
			if (!isWindowObj && out[sx].opaque && out[sx].priority <= priority) continue;

			// OBJ mosaic (attribute0 bit12): like BG mosaic, blocks are
			// aligned to the top-left of the *screen*, not the sprite -- so
			// snap the screen coordinate (sx, line), then re-derive which
			// sprite-local column/row that corresponds to. A block can
			// straddle the sprite's own edge (its top-left isn't
			// necessarily mosaic-aligned); real hardware's exact behavior
			// there is underdocumented, so this clamps into the sprite's
			// own bounds rather than reading outside it -- a reasonable,
			// documented simplification, not a guess at exact edge pixels.
			int sampleCol = col, sampleRow = rowInBox;
			if (mosaicOn) {
				int snappedSx = sx - (sx % objMosH);
				int snappedLine = line - (line % objMosV);
				sampleCol = snappedSx - x;
				if (sampleCol < 0) sampleCol = 0;
				if (sampleCol >= boxW) sampleCol = boxW - 1;
				sampleRow = snappedLine - y;
				if (sampleRow < 0) sampleRow = 0;
				if (sampleRow >= boxH) sampleRow = boxH - 1;
			}

			int texx, texy;
			if (affine) {
				s32 cx = boxW / 2, cy = boxH / 2;
				s32 relx = sampleCol - cx, rely = sampleRow - cy;
				s32 origx = (relx * pa + rely * pb) >> 8;
				s32 origy = (relx * pc + rely * pd) >> 8;
				texx = origx + w / 2;
				texy = origy + h / 2;
				if (texx < 0 || texy < 0 || texx >= w || texy >= h) continue;
			} else {
				texx = hflip ? (w - 1 - sampleCol) : sampleCol;
				texy = vflip ? (h - 1 - sampleRow) : sampleRow;
			}

			int tileX = texx / 8, tileY = texy / 8;
			int subx = texx % 8, suby = texy % 8;

			// 1D mapping: tile index counts tileBytes-sized (32 or 64)
			// tiles linearly. 2D mapping: VRAM is a fixed 32-slots-per-row
			// grid of 32-byte (4bpp-tile-sized) slots regardless of color
			// depth, so an 8bpp tile spans two horizontally-adjacent slots.
			u32 addr;
			if (oneDim) {
				u32 tileIndex = tileNum + tileY * (w / 8) + tileX;
				addr = charBase + tileIndex * tileBytes;
			} else {
				u32 slotX = tileX * (colorMode ? 2 : 1);
				u32 slotIndex = tileNum + tileY * 32 + slotX;
				addr = charBase + slotIndex * 32;
			}

			int colorIdx;
			u16 color = 0;
			if (!colorMode) {
				u32 a = addr + suby * 4 + subx / 2;
				u8 byte = T1ReadByte(MMU.GBA_VRAM, vramAddr(a));
				colorIdx = (subx & 1) ? (byte >> 4) : (byte & 0xF);
				if (colorIdx == 0) continue;
				color = objPaletteColor(palNum * 16 + colorIdx);
			} else {
				u32 a = addr + suby * 8 + subx;
				colorIdx = T1ReadByte(MMU.GBA_VRAM, vramAddr(a));
				if (colorIdx == 0) continue;
				color = objPaletteColor(colorIdx);
			}

			if (isWindowObj) { winMask[sx] = true; continue; }
			out[sx].color = color;
			out[sx].opaque = true;
			out[sx].priority = priority;
			out[sx].semiTransparent = semiTransparent;
		}
	}
}

// ---------------------------------------------------------------------
// Windows (WIN0/WIN1/OBJ window) + color special effects (BLDCNT/
// BLDALPHA/BLDY). GBATEK-accurate semantics:
//  - Precedence WIN0 > WIN1 > OBJ window > outside (GBATEK: "Window 0 is
//    having highest priority, Window 1 medium, and Obj Window lowest
//    priority. Outside of Window is having zero priority.").
//  - Windows are entirely inactive (every BG/OBJ layer and effects always
//    enabled, WININ/WINOUT ignored) unless at least one of DISPCNT's
//    WIN0/WIN1/OBJWIN enable bits is set -- exactly gxUpdateHotFlags()'s
//    GXHOT_OBJWIN condition, so a frame with that flag clear provably hits
//    the plain/no-window path below.
//  - Special effects blend the frontmost visible pixel (if it's a BLDCNT
//    1st-target layer, or an OBJ drawn in semi-transparent mode -- which
//    forces alpha-blend behavior for that OBJ regardless of BLDCNT's OBJ
//    1st-target bit, per GBATEK) against the next visible layer down (for
//    alpha blend, only if that layer is a 2nd-target; brighten/darken
//    don't consult a 2nd layer at all -- they fade toward white/black).
// ---------------------------------------------------------------------
struct WindowMasks { bool bg[4]; bool obj; bool effect; };

// WIN0H/WIN1H, WIN0V/WIN1V: GBATEK - "Garbage values of X2>240 (Y2>160) or
// X1>X2 (Y1>Y2) are interpreted as X2=240 (Y2=160)."
static inline void windowXRange(u16 winH, int &x1, int &x2)
{
	x1 = (winH >> 8) & 0xFF;
	x2 = winH & 0xFF;
	if (x2 > GBA_SCREEN_W || x1 > x2) x2 = GBA_SCREEN_W;
}
static inline void windowYRange(u16 winV, int &y1, int &y2)
{
	y1 = (winV >> 8) & 0xFF;
	y2 = winV & 0xFF;
	if (y2 > GBA_SCREEN_H || y1 > y2) y2 = GBA_SCREEN_H;
}

// WININ/WINOUT: bits0-3 BG0-3 enable, bit4 OBJ enable, bit5 effect enable,
// for the low byte; WININ's high byte is WIN1's copy of the same layout,
// WINOUT's high byte is OBJ-window's copy.
static inline WindowMasks decodeWinByte(u16 v, int shift)
{
	u8 b = (u8)(v >> shift);
	WindowMasks m;
	for (int i = 0; i < 4; i++) m.bg[i] = (b >> i) & 1;
	m.obj = (b >> 4) & 1;
	m.effect = (b >> 5) & 1;
	return m;
}

static WindowMasks getPixelWindowMasks(int x,
	bool win0On, int w0x1, int w0x2, bool win0YIn,
	bool win1On, int w1x1, int w1x2, bool win1YIn,
	bool winObjOn, const bool *objWinMask, u16 winIn, u16 winOut)
{
	if (win0On && win0YIn && x >= w0x1 && x < w0x2) return decodeWinByte(winIn, 0);
	if (win1On && win1YIn && x >= w1x1 && x < w1x2) return decodeWinByte(winIn, 8);
	if (winObjOn && objWinMask && objWinMask[x]) return decodeWinByte(winOut, 8);
	return decodeWinByte(winOut, 0);
}

static inline u16 packColor(int r, int g, int b) { return (u16)((b << 10) | (g << 5) | r); }
static inline void unpackColor(u16 c, int &r, int &g, int &b) { r = c & 0x1F; g = (c >> 5) & 0x1F; b = (c >> 10) & 0x1F; }

// BLDALPHA: "I = MIN(31, I1st*EVA + I2nd*EVB)" per-channel, EVA/EVB in 16ths.
static u16 blendAlpha(u16 c1, u16 c2, int eva, int evb)
{
	int r1, g1, b1, r2, g2, b2;
	unpackColor(c1, r1, g1, b1);
	unpackColor(c2, r2, g2, b2);
	int r = (r1 * eva + r2 * evb) / 16; if (r > 31) r = 31;
	int g = (g1 * eva + g2 * evb) / 16; if (g > 31) g = 31;
	int b = (b1 * eva + b2 * evb) / 16; if (b > 31) b = 31;
	return packColor(r, g, b);
}
// BLDY: brighten "I = I1st + (31-I1st)*EVY", darken "I = I1st - I1st*EVY".
static u16 blendFade(u16 c, int evy, bool toWhite)
{
	int r, g, b;
	unpackColor(c, r, g, b);
	if (toWhite) { r += ((31 - r) * evy) / 16; g += ((31 - g) * evy) / 16; b += ((31 - b) * evy) / 16; }
	else         { r -= (r * evy) / 16;        g -= (g * evy) / 16;        b -= (b * evy) / 16; }
	if (r < 0) r = 0;
	if (r > 31) r = 31;
	if (g < 0) g = 0;
	if (g > 31) g = 31;
	if (b < 0) b = 0;
	if (b > 31) b = 31;
	return packColor(r, g, b);
}

// Layer bit indices, matching BLDCNT's target1/target2 field layout:
// BG0=0, BG1=1, BG2=2, BG3=3, OBJ=4, backdrop=5.
struct Layer { u16 color; int priority; int bit; bool isObj; bool semiTransparent; };

// Composes the final color for one pixel from its front-to-back visible
// layer list (list[0] is frontmost; callers only push layers that already
// passed that pixel's window BG/OBJ enable check) plus this frame's blend
// config. `count` is always >= 1 -- every caller pushes the backdrop last.
static u16 composePixel(const Layer *list, int count, bool effectEnabled, int effect,
                         int target1, int target2, int eva, int evb, int evy)
{
	const Layer &top = list[0];
	if (!effectEnabled || effect == 0) return top.color;
	bool bldTarget1 = (target1 >> top.bit) & 1;
	if (effect == 1) { // alpha blend
		bool forced = top.isObj && top.semiTransparent; // GBATEK: semi-transparent OBJ acts as 1st target regardless of BLDCNT's OBJ bit
		if (!(bldTarget1 || forced) || count < 2) return top.color;
		const Layer &sec = list[1];
		if (!((target2 >> sec.bit) & 1)) return top.color; // no valid 2nd target below -> draws normally
		return blendAlpha(top.color, sec.color, eva, evb);
	}
	// effect 2 = brighten, 3 = darken -- top layer only, no 2nd-target
	// lookup, and semi-transparent OBJ status does NOT force these (only
	// alpha-blend gets that GBATEK special case).
	if (!bldTarget1) return top.color;
	return blendFade(top.color, evy, effect == 2);
}

// ---------------------------------------------------------------------
// Scanline compositor
// ---------------------------------------------------------------------
static void renderScanline(int line)
{
	if (line < 0 || line >= GBA_SCREEN_H) return;
	u16 dispcnt = io16(IO_DISPCNT);
	u16 *dst = GBA_screen + line * GBA_SCREEN_W;

	if ((dispcnt >> 7) & 1) { // forced blank: white
		for (int x = 0; x < GBA_SCREEN_W; x++) dst[x] = 0x7FFF;
		return;
	}

	int mode = dispcnt & 7;
	u16 backdrop = bgPaletteColor(0);

	bool win0Dc = (dispcnt >> 13) & 1, win1Dc = (dispcnt >> 14) & 1, winObjDc = (dispcnt >> 15) & 1;
	bool windowsActive = win0Dc || win1Dc || winObjDc; // == GXHOT_OBJWIN's condition

	int w0x1 = 0, w0x2 = GBA_SCREEN_W, w1x1 = 0, w1x2 = GBA_SCREEN_W;
	bool win0YIn = false, win1YIn = false;
	u16 winIn = 0, winOut = 0;
	if (windowsActive) {
		windowXRange(io16(IO_WIN0H), w0x1, w0x2);
		windowXRange(io16(IO_WIN1H), w1x1, w1x2);
		int y1, y2;
		windowYRange(io16(IO_WIN0V), y1, y2); win0YIn = win0Dc && line >= y1 && line < y2;
		windowYRange(io16(IO_WIN1V), y1, y2); win1YIn = win1Dc && line >= y1 && line < y2;
		winIn = io16(IO_WININ);
		winOut = io16(IO_WINOUT);
	}

	bool haveObjWinMask = windowsActive && winObjDc;
	bool objWinMaskArr[GBA_SCREEN_W];
	if (haveObjWinMask) memset(objWinMaskArr, 0, sizeof(objWinMaskArr));

	Pixel obj[GBA_SCREEN_W];
	for (int x = 0; x < GBA_SCREEN_W; x++) obj[x] = Pixel{ 0, false, 0, false };
	renderObjLine(line, obj, haveObjWinMask ? objWinMaskArr : nullptr);

	u16 bldcnt = io16(IO_BLDCNT);
	int effect = (bldcnt >> 6) & 3;
	int target1 = bldcnt & 0x3F, target2 = (bldcnt >> 8) & 0x3F;
	u16 bldalpha = io16(IO_BLDALPHA);
	int eva = bldalpha & 0x1F; if (eva > 16) eva = 16;
	int evb = (bldalpha >> 8) & 0x1F; if (evb > 16) evb = 16;
	int evy = io16(IO_BLDY) & 0x1F; if (evy > 16) evy = 16;

	// Per-BG sample cache: filled for whichever BGs this mode actually has
	// active, using the bitmap sampler for modes 3-5's single BG2 layer and
	// the tiled/affine samplers for modes 0-2 -- this lets the priority-
	// sorted composite loop below treat every mode identically instead of
	// special-casing bitmap modes as a separate, narrower path (which is
	// how this looked before window/blend support: three near-duplicate
	// mode3/4/5 blocks each re-deriving the same obj-vs-bg priority rule).
	static Pixel bgLine[4][GBA_SCREEN_W];
	bool bgActive[4] = { false, false, false, false };
	for (int bg = 0; bg < 4; bg++) {
		bool active;
		if (mode >= 3) {
			active = (bg == 2) && ((dispcnt >> 10) & 1);
		} else {
			active = (dispcnt >> (8 + bg)) & 1;
			if (mode == 1 && bg == 3) active = false; // mode 1 has no BG3
			if (mode == 2 && (bg == 0 || bg == 1)) active = false; // mode 2 has no BG0/BG1
		}
		bgActive[bg] = active;
		if (!active) continue;
		if (mode >= 3) {
			for (int x = 0; x < GBA_SCREEN_W; x++) bgLine[bg][x] = sampleBitmapBg2(mode, dispcnt, line, x);
		} else {
			bool affineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);
			for (int x = 0; x < GBA_SCREEN_W; x++)
				bgLine[bg][x] = affineCapable ? sampleAffineBg(bg, line, x) : sampleTextBg(bg, line, x);
		}
	}

	// Composite: for each pixel, build the front-to-back visible layer
	// list (OBJ then BG0..BG3 within each priority tier, matching the
	// original last-write-wins draw order this replaces -- OBJ wins ties
	// against a same-priority BG, and lower-indexed BGs win ties against
	// higher-indexed ones), append the backdrop as the always-present
	// bottom layer, then resolve special effects against the top 1-2.
	for (int x = 0; x < GBA_SCREEN_W; x++) {
		WindowMasks wm;
		if (!windowsActive) {
			wm.bg[0] = wm.bg[1] = wm.bg[2] = wm.bg[3] = true;
			wm.obj = true;
			wm.effect = true;
		} else {
			wm = getPixelWindowMasks(x, win0Dc, w0x1, w0x2, win0YIn, win1Dc, w1x1, w1x2, win1YIn,
			                          winObjDc, haveObjWinMask ? objWinMaskArr : nullptr, winIn, winOut);
		}

		Layer list[6];
		int n = 0;
		for (int prio = 0; prio <= 3; prio++) {
			if (wm.obj && obj[x].opaque && obj[x].priority == prio)
				list[n++] = Layer{ obj[x].color, prio, 4, true, obj[x].semiTransparent };
			for (int bg = 0; bg < 4; bg++) {
				if (!bgActive[bg] || !wm.bg[bg]) continue;
				const Pixel &p = bgLine[bg][x];
				if (p.opaque && p.priority == prio)
					list[n++] = Layer{ p.color, prio, bg, false, false };
			}
		}
		list[n++] = Layer{ backdrop, 4, 5, false, false };

		dst[x] = composePixel(list, n, wm.effect, effect, target1, target2, eva, evb, evy);
	}
}

// ---------------------------------------------------------------------
// Frame/scanline sequencing hooks (called by NDSSystem.cpp's gbaExecFrame)
// ---------------------------------------------------------------------
void gbaPpuBeginFrame()
{
	latchAffine(0);
	latchAffine(1);
	g_gbaFramePlan.beginFrame(); // clears all hot flags to false
	// Re-derive the hot flags from the register state already in effect at
	// frame start, rather than leaving them false until (if ever) a layout
	// register happens to be rewritten this frame. gxOnRegisterWritten()
	// alone only *updates* the flags on a write; without this, a frame with
	// no window/mosaic/blend register writes at all -- the common case
	// once a game has set them up and stops touching them -- would report
	// every hot flag false regardless of whether the feature is still
	// genuinely active, silently letting gxGbaRenderFrame() render (and
	// get it wrong) instead of bailing to the CPU compositor. Mid-frame
	// toggles are still caught live by gxOnRegisterWritten()'s own call.
	gxUpdateHotFlags();
	gxSnapshotBandRegs(&g_gbaBandRegs[0]);
}

void gbaPpuEndFrame()
{
	// Stage 1+4 GX path (gx_gba_render.cpp): if it handles this frame's mode
	// and content, it overwrites GBA_screen with its own result, superseding
	// whatever gbaPpuHDrawEnd()'s per-scanline CPU compositor already wrote
	// there this frame. If it bails (unsupported mode/feature -- see that
	// file's header), GBA_screen already holds the correct CPU-rendered
	// content and nothing further is needed. Either way this always runs:
	// which path handled the frame is only knowable once all of the frame's
	// register/OAM state is final, i.e. here, not per-scanline.
	gxGbaRenderFrame();

	// Blit into GPU_screen (offset 0 = the "main screen" slot the DS side
	// uses -- see MainScreen.offset in GPU.cpp)
	// so the existing screenshot/harness_frame.cpp capture path (which
	// reads GPU_screen directly, expecting its native X-B5-G5-R5 layout)
	// picks up GBA output with no changes of its own. GBA mode never
	// drives the DS GPU_RenderLine() path (NDS_exec's isGBA branch calls
	// gbaExecFrame() instead of the DS scheduler loop), so nothing else
	// writes GPU_screen's first 160 rows while in GBA mode.
	u16 *dst = (u16 *)GPU_screen;
	for (int y = 0; y < GBA_SCREEN_H; y++)
		memcpy(dst + y * 256, GBA_screen + y * GBA_SCREEN_W, GBA_SCREEN_W * sizeof(u16));
}

void gbaPpuHDrawEnd(int line)
{
	// perf_zones: this software scanline compositor (BG sampling + OBJ line
	// render, both per-pixel) is the GBA-mode counterpart of the DS side's
	// GPU_RenderLine() call (NDSSystem.cpp, wrapped in the same PZ_GPU_2D
	// zone) -- until this was added it had no zone of its own and fell into
	// PZ_OTHER by default, which is what made an early perf-zones capture of
	// Minish Cap misleadingly show ~95% "other" instead of naming this as
	// the actual cost.
	// Capture this line's affine reference point before it renders (and
	// before advanceAffine() below moves it on to the next line) -- see
	// s_affXHistory's comment: affine BG mosaic needs to look this back up
	// for whatever earlier line a mosaic block aligns to.
	if (line >= 0 && line < GBA_SCREEN_H) {
		s_affXHistory[line][0] = s_affX[0]; s_affXHistory[line][1] = s_affX[1];
		s_affYHistory[line][0] = s_affY[0]; s_affYHistory[line][1] = s_affY[1];
	}
	{
		PZ_SCOPE(PZ_GPU_2D);
		renderScanline(line);
	}
	setDispstatFlags(/*vblank*/ line >= GBA_SCREEN_H, /*hblank*/ true);
	if (line < GBA_SCREEN_H) { advanceAffine(0); advanceAffine(1); }
}

// ---------------------------------------------------------------------
// VBlank interrupt (HLE). Delivery mechanics (why this is a BIOS-IRQ-
// trampoline stand-in, not a real ARM-level IRQ exception, and what that
// does/doesn't cover) now live in gba_irq.cpp's gbaRequestIrq(), shared
// with the timer and keypad interrupt sources added alongside it (§4.3);
// this function only decides *whether* VBlank should request bit0, which
// stays PPU-local (it depends on DISPSTAT's VBlank-IRQ-enable bit).
// ---------------------------------------------------------------------
static void gbaFireVBlankIrqIfNeeded()
{
	if (!(io16(IO_DISPSTAT) & 0x0008)) return; // VBlank IRQ not enabled at the LCD level
	gbaRequestIrq(1); // bit0 = VBlank
}

void gbaPpuHBlankEnd(int line)
{
	s_vcount = (u16)((line + 1) % 228);
	io16w(IO_VCOUNT, s_vcount);
	setDispstatFlags(/*vblank*/ s_vcount >= GBA_SCREEN_H, /*hblank*/ false);
	if (s_vcount == GBA_SCREEN_H) gbaFireVBlankIrqIfNeeded();
}

// PLAN.md §4.3 item 7 (GBA savestate support). See gba_ppu.h's comment.
void gbaPpuSaveState(EMUFILE* os)
{
	write32le(1, os); // version
	write16le(s_vcount, os);
	for (int i = 0; i < 2; i++) write32le((u32)s_affX[i], os);
	for (int i = 0; i < 2; i++) write32le((u32)s_affY[i], os);
}

bool gbaPpuLoadState(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	if (!read16le(&s_vcount, is)) return false;
	for (int i = 0; i < 2; i++) { u32 v; if (!read32le(&v, is)) return false; s_affX[i] = (s32)v; }
	for (int i = 0; i < 2; i++) { u32 v; if (!read32le(&v, is)) return false; s_affY[i] = (s32)v; }
	return true;
}
