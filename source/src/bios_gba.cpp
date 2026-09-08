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

// roadmap #20 (GBA compat), §12.3 step 6: real GBA BIOS SWI table
// (function-level HLE), superseding step 5.5's `swi_tab = NULL` safety net
// now that there's real behaviour to dispatch to instead of a trap.
//
// Most of the "unit ops" here (division, CpuSet/CpuFastSet, BitUnPack, the
// LZ77/RL/Huffman decompressors, the Diff filters, Sqrt, the wait4IRQ-style
// Halt/IntrWait park) are near-verbatim duplicates of the equivalent
// functions in bios.cpp, which are themselves VBA-derived and already
// route every memory access through _MMU_read/write<PROCNUM> -- MMU.isGBA-
// aware since step 4. They're fixed to ARMCPU_ARM7 here (not templated --
// GBA mode never runs on ARM9) and get the small number of GBA-specific
// constants substituted in (SoundBias's register, IntrWait's flag address).
// Duplicated on purpose, not shared by extern, matching the convention this
// plan already used for the JIT's per-profile cycle tables (step 5) --
// keep in sync (or extract) if either file's copy ever changes.
//
// Cross-referenced against mGBA's src/gba/hle-bios.s (MPL 2.0, (c) 2013-
// 2015 Jeffrey Pfau) while designing this file -- see the plan doc's step 6
// writeup for why that file's assembled binary isn't usable as a drop-in
// replacement here (several of its SWIs, including Div/Sqrt/ArcTan/LZ77,
// are stubs that depend on an mGBA-internal-only hook and would silently
// leave registers unmodified if executed by a different interpreter). Its
// real, self-contained implementations (SoftReset, Halt, IntrWait/
// VBlankIntrWait, CpuSet, CpuFastSet) and its SWI table ordering were used
// only as a hardware-accuracy reference, not copied.

#include <string.h>
#include <math.h>
#include "armcpu.h"
#include "bios_gba.h"
#include "MMU.h"
#include "debug.h"
#include "NDSSystem.h"

#define cpu (&NDS_ARM7)

// ----------------------------------------------------------------------
// Unimplemented-SWI logger. Same shape as bios.cpp's bios_nop() -- logs
// once per call rather than silently doing nothing, so a game relying on
// one of these (ArcTan/ArcTan2/BgAffineSet/ObjAffineSet, the Sound driver
// family, Diff8bitUnFilterVram) shows up in sd:/*.log instead of just
// misbehaving quietly.
// ----------------------------------------------------------------------
static u32 gba_nop()
{
	LOG("SWI: ARM7(GBA) unimplemented BIOS function %02X used. R0:%08X, R1:%08X, R2:%08X\n",
		(cpu->instruction) & 0x1F, cpu->R[0], cpu->R[1], cpu->R[2]);
	return 3;
}

// ----------------------------------------------------------------------
// 0x00 SoftReset
// ----------------------------------------------------------------------
static u32 gba_SoftReset()
{
	// Real hardware/BIOS: the reset-destination flag byte at the IWRAM-
	// mirror address 0x03007FFA is written once during cold boot (cart vs.
	// multiboot) and re-read here on every SoftReset. We never run real
	// BIOS cold-boot code (direct boot, §12.3 step 5.5), so NDS_Reset()'s
	// GBA branch seeds this byte itself to the "normal cart" value -- see
	// the comment there.
	u8 bootFlag = _MMU_read08<ARMCPU_ARM7>(0x03007FFA);

	// GBATEK: SoftReset clears the top 0x200 bytes of IWRAM
	// (0x03007E00-0x03007FFF and mirrors) -- the stack/BIOS-flag region,
	// not all of IWRAM.
	for (u32 a = 0x03007E00; a < 0x03008000; a++)
		_MMU_write08<ARMCPU_ARM7>(a, 0);

	armcpu_switchMode(cpu, SYS);
	for (int i = 0; i < 15; i++)
		cpu->R[i] = 0;
	cpu->R13_usr = cpu->R[13] = 0x03007F00;
	cpu->R14_usr = cpu->R[14] = 0;
	cpu->R13_irq = 0x03007FA0;
	cpu->R14_irq = 0;
	cpu->R13_svc = 0x03007FE0;
	cpu->R14_svc = 0;
	cpu->waitIRQ = FALSE;
	cpu->wirq = FALSE;
	cpu->CPSR.bits.T = 0;
	cpu->CPSR.bits.I = 0;
	cpu->CPSR.bits.F = 0;
	cpu->changeCPSR();

	// Manually re-prime the fetch pipeline (matching bios.cpp's wait4IRQ
	// convention) rather than calling armcpu_init()/armcpu_prefetch() --
	// the outer interpreter loop does its own prefetch immediately after
	// this SWI returns, so priming it here too would double-fetch and
	// skip an instruction of the freshly reset code.
	u32 dest = bootFlag ? 0x08000000 : 0x02000000;
	cpu->R[15] = dest + 8;
	cpu->next_instruction = dest;

	return 26;
}

// ----------------------------------------------------------------------
// 0x01 RegisterRamReset
// ----------------------------------------------------------------------
static u32 gba_RegisterRamReset()
{
	u32 flags = cpu->R[0];

	if (flags & 0x01) memset(MMU.GBA_EWRAM,   0, sizeof(MMU.GBA_EWRAM));
	// Preserve the top 0x200 bytes (stack/SoftReset flag region) --
	// matches real hardware's RegisterRamReset, which spares exactly what
	// SoftReset itself would clear/re-read.
	if (flags & 0x02) memset(MMU.GBA_IWRAM,   0, sizeof(MMU.GBA_IWRAM) - 0x200);
	if (flags & 0x04) memset(MMU.GBA_PALETTE, 0, sizeof(MMU.GBA_PALETTE));
	if (flags & 0x08) memset(MMU.GBA_VRAM,    0, sizeof(MMU.GBA_VRAM));
	if (flags & 0x10) memset(MMU.GBA_OAM,     0, sizeof(MMU.GBA_OAM));
	// Bits 5-7 (SIO/Sound/other I/O registers): no-op -- the GBA I/O bank
	// isn't mapped yet (§12.3 step 7), so there's nothing to clear there.

	return 1;
}

// ----------------------------------------------------------------------
// 0x02 Halt / 0x03 Stop -- share the same wait4IRQ-style interpreter-level
// park bios.cpp's DS Halt/Sleep already use (not a raw-executed spin loop
// reading real MMIO -- HALTCNT/IE/IF aren't mapped for GBA yet either).
// Real Stop additionally halts sound/timers/serial and wakes on a smaller
// interrupt subset; since no interrupt source exists yet regardless
// (§12.3 step 7), that distinction is inert today and both share this
// body -- flagged here rather than silently merged without comment.
// ----------------------------------------------------------------------
static u32 gba_wait4IRQ()
{
	u32 instructAddr = cpu->instruct_adr;
	if (cpu->wirq)
	{
		if (!cpu->waitIRQ)
		{
			cpu->waitIRQ = 0;
			cpu->wirq = 0;
			return 1;
		}
		cpu->R[15] = instructAddr;
		cpu->next_instruction = instructAddr;
		return 1;
	}
	cpu->waitIRQ = 1;
	cpu->wirq = 1;
	cpu->R[15] = instructAddr;
	cpu->next_instruction = instructAddr;
	return 1;
}

static u32 gba_Halt()
{
	_MMU_write08<ARMCPU_ARM7>(0x04000301, 0x00); // HALTCNT (no-op today, step 7 gap)
	return gba_wait4IRQ();
}

static u32 gba_Stop()
{
	_MMU_write08<ARMCPU_ARM7>(0x04000301, 0x80); // HALTCNT, stop-mode bit
	return gba_wait4IRQ();
}

// ----------------------------------------------------------------------
// 0x04 IntrWait / 0x05 VBlankIntrWait
// ----------------------------------------------------------------------
static u32 gba_IntrWait()
{
	// Real GBA IntrWait flag variable, 0x03007FF8 (confirmed against
	// mGBA's hle-bios.s, which reaches the same byte via the
	// 0x04000000-8 IWRAM-mirror trick -- both decode to the same offset
	// through this codebase's 0x03-bank & 0x7FFF mirroring, step 4).
	const u32 intrFlagAdr = 0x03007FF8;

	_MMU_write32<ARMCPU_ARM7>(0x04000208, 1); // IME=1 (no-op today, step 7 gap)

	u32 intr = _MMU_read32<ARMCPU_ARM7>(intrFlagAdr);
	u32 intrFlag = (cpu->R[1] & intr);

	if (intrFlag)
	{
		intr ^= intrFlag;
		_MMU_write32<ARMCPU_ARM7>(intrFlagAdr, intr);
		return gba_wait4IRQ();
	}

	u32 instructAddr = cpu->instruct_adr;
	cpu->R[15] = instructAddr;
	cpu->next_instruction = instructAddr;
	cpu->waitIRQ = 1;
	cpu->wirq = 1;

	return 1;
}

static u32 gba_VBlankIntrWait()
{
	cpu->R[0] = 1;
	cpu->R[1] = 1;
	return gba_IntrWait();
}

// ----------------------------------------------------------------------
// 0x06 Div / 0x07 DivArm / 0x08 Sqrt
// ----------------------------------------------------------------------
static u32 gba_Div()
{
	s32 num  = (s32)cpu->R[0];
	s32 dnum = (s32)cpu->R[1];

	if (dnum == 0) return 0;

	s32 res = num / dnum;
	cpu->R[0] = (u32)res;
	cpu->R[1] = (u32)(num % dnum);
	cpu->R[3] = (u32)abs(res);

	return 6;
}

static u32 gba_DivArm()
{
	// DivArm(denom=R0, num=R1) -- same core division as Div, reversed
	// calling convention (kept by real hardware for compatibility with an
	// older ARM software-division routine).
	u32 t = cpu->R[1];
	cpu->R[1] = cpu->R[0];
	cpu->R[0] = t;
	return gba_Div();
}

static u32 gba_Sqrt()
{
	cpu->R[0] = (u32)sqrt((double)(cpu->R[0]));
	return 1;
}

// ----------------------------------------------------------------------
// 0x0B CpuSet / 0x0C CpuFastSet
// ----------------------------------------------------------------------
static u32 gba_CpuSet()
{
	u32 src = cpu->R[0];
	u32 dst = cpu->R[1];
	u32 cnt = cpu->R[2];

	switch (BIT26(cnt))
	{
		case 0:
			src &= 0xFFFFFFFE;
			dst &= 0xFFFFFFFE;
			switch (BIT24(cnt))
			{
				case 0:
					cnt &= 0x1FFFFF;
					while (cnt)
					{
						_MMU_write16<ARMCPU_ARM7>(dst, _MMU_read16<ARMCPU_ARM7>(src));
						cnt--;
						dst += 2;
						src += 2;
					}
					break;
				case 1:
				{
					u32 val = _MMU_read16<ARMCPU_ARM7>(src);
					cnt &= 0x1FFFFF;
					while (cnt)
					{
						_MMU_write16<ARMCPU_ARM7>(dst, val);
						cnt--;
						dst += 2;
					}
					break;
				}
			}
			break;
		case 1:
			src &= 0xFFFFFFFC;
			dst &= 0xFFFFFFFC;
			switch (BIT24(cnt))
			{
				case 0:
					cnt &= 0x1FFFFF;
					while (cnt)
					{
						_MMU_write32<ARMCPU_ARM7>(dst, _MMU_read32<ARMCPU_ARM7>(src));
						cnt--;
						dst += 4;
						src += 4;
					}
					break;
				case 1:
				{
					u32 val = _MMU_read32<ARMCPU_ARM7>(src);
					cnt &= 0x1FFFFF;
					while (cnt)
					{
						_MMU_write32<ARMCPU_ARM7>(dst, val);
						cnt--;
						dst += 4;
					}
					break;
				}
			}
			break;
	}
	return 1;
}

static u32 gba_CpuFastSet()
{
	u32 src = cpu->R[0] & 0xFFFFFFFC;
	u32 dst = cpu->R[1] & 0xFFFFFFFC;
	u32 cnt = cpu->R[2];

	switch (BIT24(cnt))
	{
		case 0:
			cnt &= 0x1FFFFF;
			while (cnt)
			{
				_MMU_write32<ARMCPU_ARM7>(dst, _MMU_read32<ARMCPU_ARM7>(src));
				cnt--;
				dst += 4;
				src += 4;
			}
			break;
		case 1:
		{
			u32 val = _MMU_read32<ARMCPU_ARM7>(src);
			cnt &= 0x1FFFFF;
			while (cnt)
			{
				_MMU_write32<ARMCPU_ARM7>(dst, val);
				cnt--;
				dst += 4;
			}
			break;
		}
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x0D GetBiosChecksum
// ----------------------------------------------------------------------
static u32 gba_GetBiosChecksum()
{
	// Real, well-known GBA BIOS checksum constant -- some games sanity-
	// check this against a known-good value before proceeding.
	cpu->R[0] = 0xBAAE187F;
	return 1;
}

// ----------------------------------------------------------------------
// 0x10 BitUnPack
// ----------------------------------------------------------------------
static u32 gba_BitUnPack()
{
	u32 source, dest, header, base, d, temp;
	int len, bits, revbits, dataSize, data, bitwritecount, mask, bitcount, addBase;
	u8 b;

	source = cpu->R[0];
	dest = cpu->R[1];
	header = cpu->R[2];

	len = _MMU_read16<ARMCPU_ARM7>(header);
	bits = _MMU_read08<ARMCPU_ARM7>(header + 2);
	switch (bits)
	{
		case 1: case 2: case 4: case 8: break;
		default: return 0;
	}
	dataSize = _MMU_read08<ARMCPU_ARM7>(header + 3);
	switch (dataSize)
	{
		case 1: case 2: case 4: case 8: case 16: case 32: break;
		default: return 0;
	}

	revbits = 8 - bits;
	base = _MMU_read08<ARMCPU_ARM7>(header + 4);
	addBase = (base & 0x80000000) ? 1 : 0;
	base &= 0x7fffffff;

	data = 0;
	bitwritecount = 0;
	while (1)
	{
		len -= 1;
		if (len < 0) break;
		mask = 0xff >> revbits;
		b = _MMU_read08<ARMCPU_ARM7>(source);
		source++;
		bitcount = 0;
		while (1)
		{
			if (bitcount >= 8) break;
			d = b & mask;
			temp = d >> bitcount;
			if (!temp && addBase) temp += base;
			data |= temp << bitwritecount;
			bitwritecount += dataSize;
			if (bitwritecount >= 32)
			{
				_MMU_write08<ARMCPU_ARM7>(dest, data);
				dest += 4;
				data = 0;
				bitwritecount = 0;
			}
			mask <<= bits;
			bitcount += bits;
		}
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x11 LZ77UnCompWram / 0x12 LZ77UnCompVram
// ----------------------------------------------------------------------
static u32 gba_LZ77UnCompVram()
{
	int i1, i2;
	int byteCount, byteShift;
	u32 writeValue;
	int len;
	u32 source = cpu->R[0];
	u32 dest = cpu->R[1];
	u32 header = _MMU_read32<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	byteCount = 0;
	byteShift = 0;
	writeValue = 0;
	len = header >> 8;

	while (len > 0)
	{
		u8 d = _MMU_read08<ARMCPU_ARM7>(source++);
		if (d)
		{
			for (i1 = 0; i1 < 8; i1++)
			{
				if (d & 0x80)
				{
					int length, offset;
					u32 windowOffset;
					u16 data = _MMU_read08<ARMCPU_ARM7>(source++) << 8;
					data |= _MMU_read08<ARMCPU_ARM7>(source++);
					length = (data >> 12) + 3;
					offset = (data & 0x0FFF);
					windowOffset = dest + byteCount - offset - 1;
					for (i2 = 0; i2 < length; i2++)
					{
						writeValue |= (_MMU_read08<ARMCPU_ARM7>(windowOffset++) << byteShift);
						byteShift += 8;
						byteCount++;
						if (byteCount == 2)
						{
							_MMU_write16<ARMCPU_ARM7>(dest, writeValue);
							dest += 2;
							byteCount = 0;
							byteShift = 0;
							writeValue = 0;
						}
						len--;
						if (len == 0) return 0;
					}
				}
				else
				{
					writeValue |= (_MMU_read08<ARMCPU_ARM7>(source++) << byteShift);
					byteShift += 8;
					byteCount++;
					if (byteCount == 2)
					{
						_MMU_write16<ARMCPU_ARM7>(dest, writeValue);
						dest += 2;
						byteCount = 0;
						byteShift = 0;
						writeValue = 0;
					}
					len--;
					if (len == 0) return 0;
				}
				d <<= 1;
			}
		}
		else
		{
			for (i1 = 0; i1 < 8; i1++)
			{
				writeValue |= (_MMU_read08<ARMCPU_ARM7>(source++) << byteShift);
				byteShift += 8;
				byteCount++;
				if (byteCount == 2)
				{
					_MMU_write16<ARMCPU_ARM7>(dest, writeValue);
					dest += 2;
					byteShift = 0;
					byteCount = 0;
					writeValue = 0;
				}
				len--;
				if (len == 0) return 0;
			}
		}
	}
	return 1;
}

static u32 gba_LZ77UnCompWram()
{
	int i1, i2;
	int len;
	u32 source = cpu->R[0];
	u32 dest = cpu->R[1];
	u32 header = _MMU_read32<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	len = header >> 8;

	while (len > 0)
	{
		u8 d = _MMU_read08<ARMCPU_ARM7>(source++);
		if (d)
		{
			for (i1 = 0; i1 < 8; i1++)
			{
				if (d & 0x80)
				{
					int length, offset;
					u32 windowOffset;
					u16 data = _MMU_read08<ARMCPU_ARM7>(source++) << 8;
					data |= _MMU_read08<ARMCPU_ARM7>(source++);
					length = (data >> 12) + 3;
					offset = (data & 0x0FFF);
					windowOffset = dest - offset - 1;
					for (i2 = 0; i2 < length; i2++)
					{
						_MMU_write08<ARMCPU_ARM7>(dest++, _MMU_read08<ARMCPU_ARM7>(windowOffset++));
						len--;
						if (len == 0) return 0;
					}
				}
				else
				{
					_MMU_write08<ARMCPU_ARM7>(dest++, _MMU_read08<ARMCPU_ARM7>(source++));
					len--;
					if (len == 0) return 0;
				}
				d <<= 1;
			}
		}
		else
		{
			for (i1 = 0; i1 < 8; i1++)
			{
				_MMU_write08<ARMCPU_ARM7>(dest++, _MMU_read08<ARMCPU_ARM7>(source++));
				len--;
				if (len == 0) return 0;
			}
		}
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x13 HuffUnComp
// ----------------------------------------------------------------------
static u32 gba_HuffUnComp()
{
	u32 source, dest, writeValue, header, treeStart, mask;
	u32 data;
	u8 treeSize, currentNode, rootNode;
	int byteCount, byteShift, len, pos;
	int writeData;

	source = cpu->R[0];
	dest = cpu->R[1];

	header = _MMU_read08<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	treeSize = _MMU_read08<ARMCPU_ARM7>(source++);
	treeStart = source;
	source += ((treeSize + 1) << 1) - 1;

	len = header >> 8;
	mask = 0x80000000;
	data = _MMU_read08<ARMCPU_ARM7>(source);
	source += 4;

	pos = 0;
	rootNode = _MMU_read08<ARMCPU_ARM7>(treeStart);
	currentNode = rootNode;
	writeData = 0;
	byteShift = 0;
	byteCount = 0;
	writeValue = 0;

	if ((header & 0x0F) == 8)
	{
		while (len > 0)
		{
			if (pos == 0) pos++;
			else pos += (((currentNode & 0x3F) + 1) << 1);

			if (data & mask)
			{
				if (currentNode & 0x40) writeData = 1;
				currentNode = _MMU_read08<ARMCPU_ARM7>(treeStart + pos + 1);
			}
			else
			{
				if (currentNode & 0x80) writeData = 1;
				currentNode = _MMU_read08<ARMCPU_ARM7>(treeStart + pos);
			}

			if (writeData)
			{
				writeValue |= (currentNode << byteShift);
				byteCount++;
				byteShift += 8;
				pos = 0;
				currentNode = rootNode;
				writeData = 0;
				if (byteCount == 4)
				{
					byteCount = 0;
					byteShift = 0;
					_MMU_write08<ARMCPU_ARM7>(dest, writeValue);
					writeValue = 0;
					dest += 4;
					len -= 4;
				}
			}
			mask >>= 1;
			if (mask == 0)
			{
				mask = 0x80000000;
				data = _MMU_read08<ARMCPU_ARM7>(source);
				source += 4;
			}
		}
	}
	else
	{
		int halfLen = 0;
		int value = 0;
		while (len > 0)
		{
			if (pos == 0) pos++;
			else pos += (((currentNode & 0x3F) + 1) << 1);

			if (data & mask)
			{
				if (currentNode & 0x40) writeData = 1;
				currentNode = _MMU_read08<ARMCPU_ARM7>(treeStart + pos + 1);
			}
			else
			{
				if (currentNode & 0x80) writeData = 1;
				currentNode = _MMU_read08<ARMCPU_ARM7>(treeStart + pos);
			}

			if (writeData)
			{
				if (halfLen == 0) value |= currentNode;
				else value |= (currentNode << 4);
				halfLen += 4;
				if (halfLen == 8)
				{
					writeValue |= (value << byteShift);
					byteCount++;
					byteShift += 8;
					halfLen = 0;
					value = 0;
					if (byteCount == 4)
					{
						byteCount = 0;
						byteShift = 0;
						_MMU_write08<ARMCPU_ARM7>(dest, writeValue);
						dest += 4;
						writeValue = 0;
						len -= 4;
					}
				}
				pos = 0;
				currentNode = rootNode;
				writeData = 0;
			}
			mask >>= 1;
			if (mask == 0)
			{
				mask = 0x80000000;
				data = _MMU_read08<ARMCPU_ARM7>(source);
				source += 4;
			}
		}
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x14 RLUnCompWram / 0x15 RLUnCompVram
// ----------------------------------------------------------------------
static u32 gba_RLUnCompVram()
{
	int i, len, byteCount, byteShift;
	u32 writeValue;
	u32 source = cpu->R[0];
	u32 dest = cpu->R[1];
	u32 header = _MMU_read32<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	len = header >> 8;
	byteCount = 0;
	byteShift = 0;
	writeValue = 0;

	while (len > 0)
	{
		u8 d = _MMU_read08<ARMCPU_ARM7>(source++);
		int l = d & 0x7F;
		if (d & 0x80)
		{
			u8 data = _MMU_read08<ARMCPU_ARM7>(source++);
			l += 3;
			for (i = 0; i < l; i++)
			{
				writeValue |= (data << byteShift);
				byteShift += 8;
				byteCount++;
				if (byteCount == 2)
				{
					_MMU_write16<ARMCPU_ARM7>(dest, writeValue);
					dest += 2;
					byteCount = 0;
					byteShift = 0;
					writeValue = 0;
				}
				len--;
				if (len == 0) return 0;
			}
		}
		else
		{
			l++;
			for (i = 0; i < l; i++)
			{
				writeValue |= (_MMU_read08<ARMCPU_ARM7>(source++) << byteShift);
				byteShift += 8;
				byteCount++;
				if (byteCount == 2)
				{
					_MMU_write16<ARMCPU_ARM7>(dest, writeValue);
					dest += 2;
					byteCount = 0;
					byteShift = 0;
					writeValue = 0;
				}
				len--;
				if (len == 0) return 0;
			}
		}
	}
	return 1;
}

static u32 gba_RLUnCompWram()
{
	int i, len;
	u32 source = cpu->R[0];
	u32 dest = cpu->R[1];
	u32 header = _MMU_read32<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	len = header >> 8;

	while (len > 0)
	{
		u8 d = _MMU_read08<ARMCPU_ARM7>(source++);
		int l = d & 0x7F;
		if (d & 0x80)
		{
			u8 data = _MMU_read08<ARMCPU_ARM7>(source++);
			l += 3;
			for (i = 0; i < l; i++)
			{
				_MMU_write08<ARMCPU_ARM7>(dest++, data);
				len--;
				if (len == 0) return 0;
			}
		}
		else
		{
			l++;
			for (i = 0; i < l; i++)
			{
				_MMU_write08<ARMCPU_ARM7>(dest++, _MMU_read08<ARMCPU_ARM7>(source++));
				len--;
				if (len == 0) return 0;
			}
		}
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x16 Diff8bitUnFilterWram / 0x18 Diff16bitUnFilter
// (0x17 Diff8bitUnFilterVram left as a documented no-op -- bios.cpp's own
// DS implementation already skips this exact variant too.)
// ----------------------------------------------------------------------
static u32 gba_Diff8bitUnFilterWram()
{
	u32 source, dest, header;
	u8 data, diff;
	int len;

	source = cpu->R[0];
	dest = cpu->R[1];

	header = _MMU_read08<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	len = header >> 8;

	data = _MMU_read08<ARMCPU_ARM7>(source++);
	_MMU_write08<ARMCPU_ARM7>(dest++, data);
	len--;

	while (len > 0)
	{
		diff = _MMU_read08<ARMCPU_ARM7>(source++);
		data += diff;
		_MMU_write08<ARMCPU_ARM7>(dest++, data);
		len--;
	}
	return 1;
}

static u32 gba_Diff16bitUnFilter()
{
	u32 source, dest, header;
	u16 data;
	int len;

	source = cpu->R[0];
	dest = cpu->R[1];

	header = _MMU_read08<ARMCPU_ARM7>(source);
	source += 4;

	if (((source & 0xe000000) == 0) ||
		((source + ((header >> 8) & 0x1fffff)) & 0xe000000) == 0)
		return 0;

	len = header >> 8;

	data = _MMU_read16<ARMCPU_ARM7>(source);
	source += 2;
	_MMU_write16<ARMCPU_ARM7>(dest, data);
	dest += 2;
	len -= 2;

	while (len >= 2)
	{
		u16 diff = _MMU_read16<ARMCPU_ARM7>(source);
		source += 2;
		data += diff;
		_MMU_write16<ARMCPU_ARM7>(dest, data);
		dest += 2;
		len -= 2;
	}
	return 1;
}

// ----------------------------------------------------------------------
// 0x19 SoundBias
// ----------------------------------------------------------------------
static u32 gba_SoundBias()
{
	// Real GBA SOUNDBIAS register, 0x04000088 (DS's own SoundBias() in
	// bios.cpp uses 0x04000504 -- that's the DS-specific address, not
	// reusable here). No-op today either way (GBA I/O bank unmapped,
	// step 7 gap) -- kept for forward compatibility and consistency with
	// bios.cpp's already-established (deliberately approximate) shape.
	u32 curBias = _MMU_read32<ARMCPU_ARM7>(0x04000088);
	u32 newBias = (curBias == 0) ? 0x000 : 0x200;
	u32 delay = (newBias > curBias) ? (newBias - curBias) : (curBias - newBias);

	_MMU_write32<ARMCPU_ARM7>(0x04000088, newBias);
	return cpu->R[1] * delay;
}

// ----------------------------------------------------------------------
// Real GBA SWI table, GBATEK order. See bios_gba.h for why this stops at
// 0x1F (32 entries) instead of the full 0x00-0x2A real BIOS range.
// ----------------------------------------------------------------------
u32 (* ARM7GBA_swi_tab[32])() = {
	gba_SoftReset,             // 0x00
	gba_RegisterRamReset,      // 0x01
	gba_Halt,                  // 0x02
	gba_Stop,                  // 0x03
	gba_IntrWait,              // 0x04
	gba_VBlankIntrWait,        // 0x05
	gba_Div,                   // 0x06
	gba_DivArm,                // 0x07
	gba_Sqrt,                  // 0x08
	gba_nop,                   // 0x09 ArcTan (unimplemented -- see file header)
	gba_nop,                   // 0x0A ArcTan2
	gba_CpuSet,                // 0x0B
	gba_CpuFastSet,            // 0x0C
	gba_GetBiosChecksum,       // 0x0D
	gba_nop,                   // 0x0E BgAffineSet (unimplemented)
	gba_nop,                   // 0x0F ObjAffineSet (unimplemented)
	gba_BitUnPack,             // 0x10
	gba_LZ77UnCompWram,        // 0x11
	gba_LZ77UnCompVram,        // 0x12
	gba_HuffUnComp,            // 0x13
	gba_RLUnCompWram,          // 0x14
	gba_RLUnCompVram,          // 0x15
	gba_Diff8bitUnFilterWram,  // 0x16
	gba_nop,                   // 0x17 Diff8bitUnFilterVram (unimplemented)
	gba_Diff16bitUnFilter,     // 0x18
	gba_SoundBias,             // 0x19
	gba_nop,                   // 0x1A SoundDriverInit (unimplemented)
	gba_nop,                   // 0x1B SoundDriverMode
	gba_nop,                   // 0x1C SoundDriverMain
	gba_nop,                   // 0x1D SoundDriverVSync
	gba_nop,                   // 0x1E SoundChannelClear
	gba_nop,                   // 0x1F MidiKey2Freq
};
