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

static void gxUpdateHotFlags()
{
	u16 dispcnt = io16(IO_DISPCNT);
	bool win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winObj = (dispcnt >> 15) & 1;
	g_gbaFramePlan.setHot(GXHOT_OBJWIN, win0 || win1 || winObj);
	g_gbaFramePlan.setHot(GXHOT_MOSAIC, io16(IO_MOSAIC) != 0);
	g_gbaFramePlan.setHot(GXHOT_BLEND, (io16(IO_BLDCNT) & 0x3F3F) != 0);
}

// Called from every generic (non DISPSTAT/VCOUNT/IF) register write below,
// after the write has landed, so gxUpdateHotFlags() sees the new value.
static void gxOnRegisterWritten(u32 offset)
{
	if (!gxIsLayoutRegister(offset))
		return;
	g_gbaFramePlan.bands.recordChangeAtLine(s_vcount);
	gxUpdateHotFlags();
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

struct Pixel { u16 color; bool opaque; int priority; };

// ---------------------------------------------------------------------
// Text-mode background (modes 0/1/2's non-affine BGs).
// ---------------------------------------------------------------------
static Pixel sampleTextBg(int bg, int line, int x)
{
	Pixel out = { 0, false, 0 };
	u16 cnt = io16(IO_BG0CNT + bg * 2);
	int priority   = cnt & 3;
	u32 charBase   = ((cnt >> 2) & 3) * 0x4000;
	bool colorMode = (cnt >> 7) & 1; // 0=4bpp/16pal, 1=8bpp/256pal
	u32 mapBase    = ((cnt >> 8) & 0x1F) * 0x800;
	int sizeSel    = (cnt >> 14) & 3;
	static const int mapWtiles[4] = { 32, 64, 32, 64 };
	static const int mapHtiles[4] = { 32, 32, 64, 64 };
	int mapWpx = mapWtiles[sizeSel] * 8, mapHpx = mapHtiles[sizeSel] * 8;

	u16 hofs = io16(IO_BG0HOFS + bg * 4) & 0x1FF;
	u16 vofs = io16(IO_BG0HOFS + bg * 4 + 2) & 0x1FF;

	int px = (x + hofs) % mapWpx;
	int py = (line + vofs) % mapHpx;
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
static s32 s_affX[2], s_affY[2]; // index 0 = BG2, 1 = BG3, 20.8 fixed point

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

static Pixel sampleAffineBg(int bg /*2 or 3*/, int x)
{
	Pixel out = { 0, false, 0 };
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

	s32 srcX = s_affX[which] + x * pa;
	s32 srcY = s_affY[which] + x * pc;
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
// OBJ (sprites)
// ---------------------------------------------------------------------
static const u8 s_objSizeW[4][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32}, {0,0,0,0} };
static const u8 s_objSizeH[4][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64}, {0,0,0,0} };

static void renderObjLine(int line, Pixel out[GBA_SCREEN_W])
{
	u16 dispcnt = io16(IO_DISPCNT);
	if (!((dispcnt >> 12) & 1)) return; // OBJ disabled
	bool oneDim = (dispcnt >> 6) & 1;

	for (int i = 0; i < 128; i++) {
		u32 oamOff = i * 8;
		u16 a0 = T1ReadWord(MMU.GBA_OAM, oamOff);
		u16 a1 = T1ReadWord(MMU.GBA_OAM, oamOff + 2);
		u16 a2 = T1ReadWord(MMU.GBA_OAM, oamOff + 4);

		bool affine = (a0 >> 8) & 1;
		bool doubleOrDisable = (a0 >> 9) & 1;
		if (!affine && doubleOrDisable) continue; // OBJ disable bit
		int objMode = (a0 >> 10) & 3;
		if (objMode == 2) continue; // OBJ-window entries don't draw pixels themselves
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
			if (out[sx].opaque && out[sx].priority <= priority) continue;

			int texx, texy;
			if (affine) {
				s32 cx = boxW / 2, cy = boxH / 2;
				s32 relx = col - cx, rely = rowInBox - cy;
				s32 origx = (relx * pa + rely * pb) >> 8;
				s32 origy = (relx * pc + rely * pd) >> 8;
				texx = origx + w / 2;
				texy = origy + h / 2;
				if (texx < 0 || texy < 0 || texx >= w || texy >= h) continue;
			} else {
				texx = hflip ? (w - 1 - col) : col;
				texy = vflip ? (h - 1 - rowInBox) : rowInBox;
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
			if (!colorMode) {
				u32 a = addr + suby * 4 + subx / 2;
				u8 byte = T1ReadByte(MMU.GBA_VRAM, vramAddr(a));
				colorIdx = (subx & 1) ? (byte >> 4) : (byte & 0xF);
				if (colorIdx == 0) continue;
				out[sx].color = objPaletteColor(palNum * 16 + colorIdx);
			} else {
				u32 a = addr + suby * 8 + subx;
				colorIdx = T1ReadByte(MMU.GBA_VRAM, vramAddr(a));
				if (colorIdx == 0) continue;
				out[sx].color = objPaletteColor(colorIdx);
			}
			out[sx].opaque = true;
			out[sx].priority = priority;
		}
	}
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
	for (int x = 0; x < GBA_SCREEN_W; x++) dst[x] = backdrop;

	Pixel obj[GBA_SCREEN_W];
	for (int x = 0; x < GBA_SCREEN_W; x++) obj[x] = Pixel{ 0, false, 0 };
	renderObjLine(line, obj);

	if (mode == 3) {
		bool bg2on = (dispcnt >> 10) & 1;
		for (int x = 0; x < GBA_SCREEN_W; x++) {
			u16 c = backdrop;
			bool haveBg = false;
			if (bg2on) { c = T1ReadWord(MMU.GBA_VRAM, (line * GBA_SCREEN_W + x) * 2); haveBg = true; }
			int bgPriority = io16(IO_BG2CNT) & 3;
			if (obj[x].opaque && (!haveBg || obj[x].priority <= bgPriority)) c = obj[x].color;
			dst[x] = c;
		}
		return;
	}
	if (mode == 4) {
		bool bg2on = (dispcnt >> 10) & 1;
		u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;
		for (int x = 0; x < GBA_SCREEN_W; x++) {
			u16 c = backdrop;
			bool haveBg = false;
			if (bg2on) {
				u8 idx = T1ReadByte(MMU.GBA_VRAM, page + line * GBA_SCREEN_W + x);
				if (idx) { c = bgPaletteColor(idx); haveBg = true; }
			}
			int bgPriority = io16(IO_BG2CNT) & 3;
			if (obj[x].opaque && (!haveBg || obj[x].priority <= bgPriority)) c = obj[x].color;
			dst[x] = c;
		}
		return;
	}
	if (mode == 5) {
		bool bg2on = (dispcnt >> 10) & 1;
		u32 page = ((dispcnt >> 4) & 1) ? 0xA000 : 0;
		const int W = 160, H = 128;
		for (int x = 0; x < GBA_SCREEN_W; x++) {
			u16 c = backdrop;
			bool haveBg = false;
			if (bg2on && x < W && line < H) {
				c = T1ReadWord(MMU.GBA_VRAM, page + (line * W + x) * 2);
				haveBg = true;
			}
			int bgPriority = io16(IO_BG2CNT) & 3;
			if (obj[x].opaque && (!haveBg || obj[x].priority <= bgPriority)) c = obj[x].color;
			dst[x] = c;
		}
		return;
	}

	// Modes 0/1/2: tiled BGs, composited back-to-front by priority
	// (3 = furthest back, 0 = frontmost); OBJ of a given priority draws
	// over BG of the same priority.
	for (int prio = 3; prio >= 0; prio--) {
		for (int bg = 3; bg >= 0; bg--) {
			if (!((dispcnt >> (8 + bg)) & 1)) continue;
			bool bgIsAffineCapable = (mode == 2) ? (bg == 2 || bg == 3) : (mode == 1 ? bg == 2 : false);
			if (mode == 1 && bg == 3) continue; // mode 1 has no BG3
			if (mode == 2 && (bg == 0 || bg == 1)) continue; // mode 2 has no BG0/BG1
			u16 cnt = io16(IO_BG0CNT + bg * 2);
			if ((cnt & 3) != prio) continue;
			for (int x = 0; x < GBA_SCREEN_W; x++) {
				Pixel p = bgIsAffineCapable ? sampleAffineBg(bg, x) : sampleTextBg(bg, line, x);
				if (p.opaque) dst[x] = p.color;
			}
		}
		for (int x = 0; x < GBA_SCREEN_W; x++)
			if (obj[x].opaque && obj[x].priority == prio) dst[x] = obj[x].color;
	}
}

// ---------------------------------------------------------------------
// Frame/scanline sequencing hooks (called by NDSSystem.cpp's gbaExecFrame)
// ---------------------------------------------------------------------
void gbaPpuBeginFrame()
{
	latchAffine(0);
	latchAffine(1);
	g_gbaFramePlan.beginFrame();
}

void gbaPpuEndFrame()
{
	// Blit into GPU_screen (offset 0 = the "main screen" slot the DS side
	// uses -- see GPU.cpp's `GXMerge_BeginFrame(MainScreen.offset == 0)`)
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
