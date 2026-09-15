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

// docs/PLAN.md §4.3: GBA DMA. New code written from GBATEK's documented
// DMAxSAD/DAD/CNT_L/CNT_H layout, not a port of any existing emulator's
// DMA core. See gba_dma.h for the "fires instantly, not cycle-accurate"
// scope note.

#include "gba_dma.h"
#include "gba_irq.h"
#include "gba_backup.h"
#include "gba_apu.h"
#include "MMU.h"
#include "mem.h"
#include "armcpu.h"
#include "debug.h"
#include "readwrite.h"

enum { DMA_BASE = 0xB0, DMA_STRIDE = 0x0C };
static inline u32 offSAD(int c)  { return DMA_BASE + c * DMA_STRIDE + 0x0; }
static inline u32 offDAD(int c)  { return DMA_BASE + c * DMA_STRIDE + 0x4; }
static inline u32 offCNTL(int c) { return DMA_BASE + c * DMA_STRIDE + 0x8; }
static inline u32 offCNTH(int c) { return DMA_BASE + c * DMA_STRIDE + 0xA; }

struct GbaDmaChan
{
	u32 liveSrc, liveDst;
	bool enabled;
	bool repeat;
	bool size32;
	int destCtrl; // 0=inc,1=dec,2=fixed,3=inc+reload-per-repeat
	int srcCtrl;  // 0=inc,1=dec,2=fixed,3=prohibited (treated as fixed)
	int timing;   // 0=immediate,1=VBlank,2=HBlank,3=Special (unimplemented)
	bool irq;
};

static GbaDmaChan s_d[4];

void gbaDmaReset()
{
	for (int i = 0; i < 4; i++)
		s_d[i] = GbaDmaChan{ 0, 0, false, false, false, 0, 0, 0, false };
}

static u32 countMax(int chan) { return chan == 3 ? 0x10000u : 0x4000u; }

static void doTransfer(int chan)
{
	GbaDmaChan &d = s_d[chan];
	u16 rawCount = T1ReadWord(MMU.GBA_IOREG, offCNTL(chan));
	u32 count = rawCount ? rawCount : countMax(chan);
	int size = d.size32 ? 4 : 2;
	int srcStep = (d.srcCtrl == 1) ? -size : (d.srcCtrl == 2 ? 0 : size);
	int dstStep = (d.destCtrl == 1) ? -size : (d.destCtrl == 2 ? 0 : size);

	// MMU_read32/write32(ARMCPU_ARM7, ...) route through _MMU_ARM7_read/
	// write32 -- the plain, non-template DS memory map (MMU.MMU_MEM[...]),
	// which has no isGBA guard at all (only the template _MMU_read/write
	// <PROCNUM,AT> used by the interpreter's own fetch/execute has that
	// guard -- see MMU.h's isGBA comment). Using the DS-only wrappers here
	// silently sent every DMA transfer's bytes into the DS ARM7 memory
	// table instead of MMU.GBA_IWRAM/EWRAM/etc -- found live: a game's
	// boot-time IWRAM init copy (almost universally done via DMA) never
	// actually reached GBA_IWRAM, so real cartridge code copied there
	// (e.g. an interrupt handler) read back as all zero. _MMU_ARM7GBA_
	// read/write* are the same GBA-region accessors the interpreter uses,
	// called directly (not through the isGBA runtime check) since this
	// function only ever runs when gameInfo.isGBA is already true.
	u32 src = d.liveSrc, dst = d.liveDst;

	// §4.3 step 4 (cartridge save memory): EEPROM (gba_backup.cpp) is a
	// DMA-framed bit-serial protocol, not real memory -- a whole transfer
	// targeting it has to be handed to gba_backup as one command (its
	// exact word count is what disambiguates a 6-bit-address chip from a
	// 14-bit-address one; see that file's comment for why per-word framing
	// can't do this safely), instead of looped word-by-word like every
	// other GBA DMA target. Only ever 16-bit on real hardware; a 32-bit
	// transfer here falls through to the generic loop below, where the
	// normal MMU accessors' own EEPROM special-case (MMU.cpp) makes it an
	// inert no-op rather than corrupting cart-ROM data sharing the address.
	if (!d.size32 && gbaBackupType() == GBA_BACKUP_EEPROM)
	{
		bool dstIsEeprom = gbaEepromAddrHit(dst);
		bool srcIsEeprom = gbaEepromAddrHit(src);

		if (dstIsEeprom && !srcIsEeprom)
		{
			u16 bits[81];
			u32 n = (count > 81) ? 81 : count;
			for (u32 i = 0; i < n; i++)
			{
				bits[i] = _MMU_ARM7GBA_read16(src) & 1;
				src += srcStep;
			}
			gbaEepromCommandWrite(bits, n);
			src = d.liveSrc + srcStep * (s32)count;
			dst += dstStep * (s32)count;
			d.liveSrc = src;
			d.liveDst = (d.destCtrl == 3) ? T1ReadLong(MMU.GBA_IOREG, offDAD(chan)) : dst;
			if (d.irq) gbaRequestIrq((u16)(1 << (8 + chan)));
			return;
		}
		if (srcIsEeprom && !dstIsEeprom)
		{
			u16 bits[68];
			u32 n = (count > 68) ? 68 : count;
			gbaEepromCommandRead(bits, n);
			for (u32 i = 0; i < n; i++)
			{
				_MMU_ARM7GBA_write16(dst, bits[i]);
				dst += dstStep;
			}
			src += srcStep * (s32)count;
			d.liveSrc = src;
			d.liveDst = (d.destCtrl == 3) ? T1ReadLong(MMU.GBA_IOREG, offDAD(chan)) : dst;
			if (d.irq) gbaRequestIrq((u16)(1 << (8 + chan)));
			return;
		}
	}

	for (u32 i = 0; i < count; i++)
	{
		if (d.size32) _MMU_ARM7GBA_write32(dst, _MMU_ARM7GBA_read32(src));
		else          _MMU_ARM7GBA_write16(dst, _MMU_ARM7GBA_read16(src));
		src += srcStep;
		dst += dstStep;
	}
	d.liveSrc = src;
	d.liveDst = (d.destCtrl == 3) ? T1ReadLong(MMU.GBA_IOREG, offDAD(chan)) : dst;

	if (d.irq) gbaRequestIrq((u16)(1 << (8 + chan))); // IE/IF bit8 = DMA0 .. bit11 = DMA3
}

static void clearEnableInBuffer(int chan)
{
	u16 cur = T1ReadWord(MMU.GBA_IOREG, offCNTH(chan));
	T1WriteWord(MMU.GBA_IOREG, offCNTH(chan), (u16)(cur & ~0x8000));
}

static void handleCntHWrite(int chan, u16 val)
{
	GbaDmaChan &d = s_d[chan];
	bool wasEnabled = d.enabled;

	d.destCtrl = (val >> 5) & 3;
	d.srcCtrl  = (val >> 7) & 3;
	d.repeat   = (val >> 9) & 1;
	d.size32   = (val >> 10) & 1;
	d.timing   = (val >> 12) & 3;
	d.irq      = (val >> 14) & 1;
	d.enabled  = (val >> 15) & 1;

	T1WriteWord(MMU.GBA_IOREG, offCNTH(chan), val);

	if (d.enabled && !wasEnabled)
	{
		d.liveSrc = T1ReadLong(MMU.GBA_IOREG, offSAD(chan));
		d.liveDst = T1ReadLong(MMU.GBA_IOREG, offDAD(chan));

		if (d.timing == 0) // immediate: fires right now, not at a scanline boundary
		{
			doTransfer(chan);
			if (!d.repeat) { d.enabled = false; clearEnableInBuffer(chan); }
		}
		else if (d.timing == 3 && chan == 3)
		{
			// DMA3 Video Capture: nothing to do at arm-time -- stays armed,
			// fired per-scanline from gbaDmaOnHblank() below (gated on VCOUNT
			// there, reading s_d[3] directly).
		}
		else if (d.timing == 3 && d.liveDst != 0x040000A0u && d.liveDst != 0x040000A4u)
		{
			// Sound-FIFO Special DMA (dst 0x040000A0/0x040000A4, DMA channels
			// 1/2) is handled below by gbaDmaOnFifoTrigger(), called from
			// gba_apu.cpp whenever the selected timer overflows and that FIFO
			// drops to half empty (docs/PLAN.md item 3, APU). DMA3 Special is
			// Video Capture (see gbaDmaOnHblank() below), handled above. Any
			// other Special-timing arm (DMA0, which GBATEK marks Prohibited)
			// is still a documented, never-fires gap. Throttled to avoid
			// flooding the log if a game spin-waits on one.
			static u32 s_armCount[4] = {0, 0, 0, 0};
			if ((++s_armCount[chan] % 500) == 1)
				INFO("gba_dma: channel %d armed with Special timing (unimplemented target, never fires) -- "
					"src=0x%08X dst=0x%08X count=%u size32=%d repeat=%d irq=%d arm#=%u\n",
					chan, (unsigned)d.liveSrc, (unsigned)d.liveDst,
					(unsigned)T1ReadWord(MMU.GBA_IOREG, offCNTL(chan)),
					(int)d.size32, (int)d.repeat, (int)d.irq, (unsigned)s_armCount[chan]);
		}
		// timing 1 (VBlank) / 2 (HBlank): stays armed, fired by
		// gbaDmaOnVblank()/gbaDmaOnHblank() below. timing 3 (Special) is
		// handled for the Sound-FIFO A/B destinations (gbaDmaOnFifoTrigger)
		// and for DMA3's Video Capture (gbaDmaOnHblank); any other
		// Special-timing target stays armed but is never fired -- a
		// documented gap, not a silent wrong-behavior approximation.
	}
}

void gbaDmaIoWrite16(u32 offset, u16 val)
{
	int chan = (offset - DMA_BASE) / DMA_STRIDE;
	if (chan < 0 || chan > 3) return;
	u32 sub = offset - (DMA_BASE + chan * DMA_STRIDE);

	if (sub == 0xA) { handleCntHWrite(chan, val); return; }
	T1WriteWord(MMU.GBA_IOREG, offset, val); // SAD/DAD halves, CNT_L: plain storage, read back at fire-time
}

void gbaDmaIoWrite8(u32 offset, u8 val)
{
	int chan = (offset - DMA_BASE) / DMA_STRIDE;
	if (chan < 0 || chan > 3) return;
	u32 sub = offset - (DMA_BASE + chan * DMA_STRIDE);

	if (sub == 0xA || sub == 0xB)
	{
		u32 base = offCNTH(chan);
		u16 cur = T1ReadWord(MMU.GBA_IOREG, base);
		u16 merged = (sub == 0xA) ? (u16)((cur & 0xFF00) | val) : (u16)((cur & 0x00FF) | (val << 8));
		handleCntHWrite(chan, merged);
		return;
	}
	T1WriteByte(MMU.GBA_IOREG, offset, val);
}

void gbaDmaIoWrite32(u32 offset, u32 val)
{
	int chan = (offset - DMA_BASE) / DMA_STRIDE;
	if (chan < 0 || chan > 3) return;
	u32 sub = offset - (DMA_BASE + chan * DMA_STRIDE);

	if (sub == 0x8) // combined CNT_L (low 16) + CNT_H (high 16) write
	{
		T1WriteWord(MMU.GBA_IOREG, offCNTL(chan), (u16)val);
		handleCntHWrite(chan, (u16)(val >> 16));
		return;
	}
	T1WriteLong(MMU.GBA_IOREG, offset, val); // SAD/DAD
}

static void fireArmed(int wantTiming)
{
	for (int i = 0; i < 4; i++)
	{
		if (!s_d[i].enabled || s_d[i].timing != wantTiming) continue;
		doTransfer(i);
		if (!s_d[i].repeat) { s_d[i].enabled = false; clearEnableInBuffer(i); }
	}
}

void gbaDmaOnHblank(int vcount)
{
	fireArmed(2);

	// DMA3 Video Capture (Special timing, channel 3 only -- see gba_dma.h's
	// gbaDmaOnHblank comment for the GBATEK quote). Fires at the same
	// per-scanline point as plain HBlank timing above, gated to VCOUNT
	// 2..161 inclusive (160 scanlines: "started when VCOUNT=2 ... stopped
	// when VCOUNT=162" -- i.e. the last firing is at VCOUNT=161). Reuses
	// doTransfer(), which re-reads DMA3CNT_L fresh every call, so the
	// per-scanline word count GBATEK describes falls out naturally with no
	// extra state. Repeat-bit handling matches every other trigger site in
	// this file (fireArmed/gbaDmaOnFifoTrigger): if the game didn't set the
	// repeat bit, this only fires once (at VCOUNT==2) and then disarms,
	// same as a one-shot HBlank/VBlank DMA would.
	GbaDmaChan &d = s_d[3];
	if (d.enabled && d.timing == 3 && vcount >= 2 && vcount <= 161)
	{
		doTransfer(3);
		if (!d.repeat) { d.enabled = false; clearEnableInBuffer(3); }
	}
}
void gbaDmaOnVblank() { fireArmed(1); }

void gbaDmaOnFifoTrigger(u32 fifoAddr)
{
	int which = (fifoAddr == 0x040000A4u) ? 1 : 0;
	// Only DMA channels 1/2 can drive Sound FIFO A/B on real hardware.
	for (int i = 1; i <= 2; i++)
	{
		GbaDmaChan &d = s_d[i];
		if (!d.enabled || d.timing != 3 || d.liveDst != fifoAddr) continue;

		int srcStep = (d.srcCtrl == 1) ? -4 : (d.srcCtrl == 2 ? 0 : 4);
		for (int w = 0; w < 4; w++)
		{
			u32 word = _MMU_ARM7GBA_read32(d.liveSrc);
			gbaApuFifoPushByte(which, (u8)word);
			gbaApuFifoPushByte(which, (u8)(word >> 8));
			gbaApuFifoPushByte(which, (u8)(word >> 16));
			gbaApuFifoPushByte(which, (u8)(word >> 24));
			d.liveSrc += srcStep;
		}
		// Destination is always fixed for Sound-FIFO Special DMA on real
		// hardware regardless of DMAxCNT_H's dest-control bits, so d.liveDst
		// is left untouched. DMAxCNT_L's count and the repeat bit are not
		// consulted either: this mode always moves exactly 4 words and stays
		// armed until the game disables the channel itself.
		if (d.irq) gbaRequestIrq((u16)(1 << (8 + i)));
	}
}

// PLAN.md §4.3 item 7 (GBA savestate support). See gba_dma.h's comment.
void gbaDmaSaveState(EMUFILE* os)
{
	write32le(1, os); // version
	for (int i = 0; i < 4; i++)
	{
		GbaDmaChan &d = s_d[i];
		write32le(d.liveSrc, os);
		write32le(d.liveDst, os);
		write8le(d.enabled ? 1 : 0, os);
		write8le(d.repeat ? 1 : 0, os);
		write8le(d.size32 ? 1 : 0, os);
		write32le((u32)d.destCtrl, os);
		write32le((u32)d.srcCtrl, os);
		write32le((u32)d.timing, os);
		write8le(d.irq ? 1 : 0, os);
	}
}

bool gbaDmaLoadState(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	for (int i = 0; i < 4; i++)
	{
		GbaDmaChan &d = s_d[i];
		u8 b; u32 v;
		if (!read32le(&d.liveSrc, is)) return false;
		if (!read32le(&d.liveDst, is)) return false;
		if (!read8le(&b, is)) return false; d.enabled = b != 0;
		if (!read8le(&b, is)) return false; d.repeat  = b != 0;
		if (!read8le(&b, is)) return false; d.size32  = b != 0;
		if (!read32le(&v, is)) return false; d.destCtrl = (int)v;
		if (!read32le(&v, is)) return false; d.srcCtrl  = (int)v;
		if (!read32le(&v, is)) return false; d.timing   = (int)v;
		if (!read8le(&b, is)) return false; d.irq = b != 0;
	}
	return true;
}
