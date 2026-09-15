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

// docs/PLAN.md §4.3 step 4: GBA cartridge save memory. New code written
// directly from GBATEK's "GBA Cart Backup" pages (ID-string detection,
// EEPROM bit-serial command framing, the 6-bit/14-bit address-size ->
// chip-size correspondence) -- not a port of any existing emulator's
// backup-memory core. Two device classes implemented this pass:
//
//   SRAM  (0x0E000000, 32 KB flat) -- trivial: no protocol, just a byte-
//   addressable buffer. Covers every "SRAM_Vnnn"-stamped commercial cart.
//
//   EEPROM (0x0D000000 bank, DMA-driven bit-serial) -- covers every
//   "EEPROM_Vnnn"-stamped cart, which is what drove this: The Legend of
//   Zelda: The Minish Cap (USA) is stamped EEPROM_V124 (confirmed via
//   `strings` against the real dump), and its save screen was the first
//   concrete "can't save" report against this port.
//
// Flash carts ("FLASH_Vnnn"/"FLASH512_Vnnn"/"FLASH1M_Vnnn") are detected
// (so they're never misrouted through the SRAM flat-buffer path, which
// would silently accept writes a real Flash chip would reject without the
// unlock-command sequence) but not backed by real chip emulation yet --
// left as an explicit gap, tracked in docs/PLAN.md, rather than modeled
// incorrectly.
//
// EEPROM protocol framing -- why this is done per-DMA-transfer rather than
// per-16-bit-access:
//
// A command is clocked in one bit per 16-bit bus access (only bit 0 of
// each access is meaningful), always via DMA on real hardware. Three
// command shapes exist, and critically, *the total bit count alone*
// determines the address width, which real emulators (and this one) use
// for auto-detection instead of trusting the ID-string suffix:
//
//   read-request (a WRITE-direction DMA): "11" + N addr bits + "0" stop
//     N=6  -> 9 bits total  (512-byte / 4 Kbit chip)
//     N=14 -> 17 bits total (8 KB / 64 Kbit chip -- only the low 10 bits
//             of that 14-bit field address real storage; the chip has
//             1024 8-byte records, not 16384, so the extra high bits are
//             masked away below, matching real hardware)
//   full write (a WRITE-direction DMA): "10" + N addr bits + 64 data bits
//     + "0" stop -> 73 bits (N=6) or 81 bits (N=14)
//   read reply (a READ-direction DMA, always 68 units): 4 dummy bits + 64
//     data bits, for whichever address the preceding read-request set.
//
// Framing this per 16-bit MMU access (accumulate bits, guess when a
// command is "done") is genuinely ambiguous: a 14-bit-address chip's first
// 9 bits are indistinguishable from a complete 6-bit-address read-request.
// The DMA engine (gba_dma.cpp) already knows each transfer's exact word
// count before it starts copying a single word, so it hands this module
// the whole command at once instead -- unambiguous, and it's how the
// hardware protocol is actually framed (a command's length is fixed by
// which DMA the game configured to send it).
//
// Persistence: flat raw dump (no header), the same format real GBA backup
// devices and every mainstream emulator (mGBA, VBA, ...) write, so a save
// file is portable in either direction. Written next to where DS .dsv
// saves already go (path.BATTERY), as <romname>.sav instead of .dsv.

#include "gba_backup.h"
#include "MMU.h"
#include "readwrite.h"
#include <stdio.h>
#include <string.h>

static GbaBackupType s_type = GBA_BACKUP_NONE;

// Sized for the larger of the two EEPROM chips (8 KB / 1024 records); the
// smaller 512-byte / 64-record chip just uses the first 512 bytes of it.
// Also large enough to double as the 32 KB SRAM buffer's storage would
// need -- kept as two separate buffers below anyway, for clarity over the
// few hundred bytes it costs.
static u8 s_eeprom[8192];
static u8 s_sram[0x8000];

static bool s_dirty = false;
static char s_savePath[512];
static bool s_haveSavePath = false;

static u32 s_eepromPendingAddr = 0;   // record index set by the last read-request
static bool s_eepromHavePending = false;

void gbaBackupDetect(const u8* rom, u32 romSize)
{
	s_type = GBA_BACKUP_NONE;
	s_eepromHavePending = false;

	// Manual substring search (not strstr/memmem): `rom` is a raw binary
	// buffer, not a NUL-terminated string, and newlib on this toolchain
	// doesn't reliably provide memmem.
	struct { const char* id; GbaBackupType type; } table[] = {
		{ "EEPROM_V",    GBA_BACKUP_EEPROM },
		{ "SRAM_V",      GBA_BACKUP_SRAM   },
		// Detected only to avoid misrouting as SRAM -- see file comment.
		{ "FLASH1M_V",   GBA_BACKUP_NONE   },
		{ "FLASH512_V",  GBA_BACKUP_NONE   },
		{ "FLASH_V",     GBA_BACKUP_NONE   },
	};

	for (size_t t = 0; t < sizeof(table) / sizeof(table[0]); t++)
	{
		size_t idLen = strlen(table[t].id);
		if (romSize < idLen) continue;
		for (u32 i = 0; i + idLen <= romSize; i++)
		{
			if (memcmp(rom + i, table[t].id, idLen) == 0)
			{
				s_type = table[t].type;
				goto found;
			}
		}
	}
found:
	memset(s_eeprom, 0xFF, sizeof(s_eeprom));   // erased EEPROM reads as all-1 on real hardware
	memset(s_sram, 0xFF, sizeof(s_sram));       // erased SRAM/Flash conventionally reads as all-1 too
	s_dirty = false;
}

GbaBackupType gbaBackupType() { return s_type; }

void gbaBackupLoadFile(const char* path)
{
	s_haveSavePath = false;
	if (!path || s_type == GBA_BACKUP_NONE) return;

	strncpy(s_savePath, path, sizeof(s_savePath) - 1);
	s_savePath[sizeof(s_savePath) - 1] = 0;
	s_haveSavePath = true;

	FILE* f = fopen(path, "rb");
	if (!f) return;   // no prior save -- not an error

	if (s_type == GBA_BACKUP_SRAM)
		fread(s_sram, 1, sizeof(s_sram), f);
	else if (s_type == GBA_BACKUP_EEPROM)
		fread(s_eeprom, 1, sizeof(s_eeprom), f);

	fclose(f);
	s_dirty = false;
}

void gbaBackupFlushIfDirty()
{
	if (!s_dirty || !s_haveSavePath) return;

	FILE* f = fopen(s_savePath, "wb");
	if (!f) return;

	if (s_type == GBA_BACKUP_SRAM)
		fwrite(s_sram, 1, sizeof(s_sram), f);
	else if (s_type == GBA_BACKUP_EEPROM)
		fwrite(s_eeprom, 1, sizeof(s_eeprom), f);

	fclose(f);
	s_dirty = false;
}

u8 gbaSramRead8(u32 offset)
{
	return s_sram[offset & 0x7FFF];
}

void gbaSramWrite8(u32 offset, u8 val)
{
	offset &= 0x7FFF;
	if (s_sram[offset] == val) return;
	s_sram[offset] = val;
	s_dirty = true;
}

bool gbaEepromAddrHit(u32 addr)
{
	if (s_type != GBA_BACKUP_EEPROM) return false;
	if ((addr >> 24) != 0x0D) return false;

	// GBATEK: carts >16 MB only expose EEPROM at the top 256 bytes of the
	// wait-state-2 bank (0x0DFFFF00-0x0DFFFFFF); <=16 MB carts expose it
	// across the whole bank (the cart's own address decode doesn't need
	// the extra bits to disambiguate ROM from EEPROM once it knows the
	// ROM itself never gets that large).
	u32 romSize = MMU.CART_ROM_MASK + 1;
	if (romSize > 0x01000000)
		return (addr & 0x00FFFFFF) >= 0x00FFFF00;
	return true;
}

// Extracts an N-bit field starting at bits[startBit], MSB-first (i.e.
// bits[startBit] is the most-significant bit of the result) -- matches how
// both the address and the 64-bit data field are clocked onto the bus.
static u32 extractBits(const u16* bits, u32 startBit, u32 n)
{
	u32 v = 0;
	for (u32 i = 0; i < n; i++)
		v = (v << 1) | (bits[startBit + i] & 1u);
	return v;
}

void gbaEepromCommandWrite(const u16* bits, u32 count)
{
	// Address width and command shape are both fully determined by the
	// transfer's exact length -- see file comment for why this framing
	// (rather than per-access bit accumulation) is required for
	// correctness, not just convenience.
	u32 addrBits;
	bool isReadRequest;
	if (count == 9)       { addrBits = 6;  isReadRequest = true;  }
	else if (count == 17) { addrBits = 14; isReadRequest = true;  }
	else if (count == 73) { addrBits = 6;  isReadRequest = false; }
	else if (count == 81) { addrBits = 14; isReadRequest = false; }
	else return;   // not a recognized command shape -- ignore rather than misparse

	u32 rawAddr = extractBits(bits, 2, addrBits);
	// The 8 KB / 14-bit-address chip only has 1024 real 8-byte records
	// (1024*8 = 8192); the extra high address bits real games still send
	// (bus width is a fixed chip property, not sized to this cart's
	// actual usage) are masked away here exactly as the real chip's
	// address decode would ignore them.
	u32 recordCount = (addrBits == 6) ? 64 : 1024;
	u32 record = rawAddr & (recordCount - 1);

	if (isReadRequest)
	{
		s_eepromPendingAddr = record;
		s_eepromHavePending = true;
		return;
	}

	// GBATEK: despite the 64-bit data field being clocked out MSB-first as
	// one big-endian integer, real hardware (and every game written for
	// it) treats the *bytes* within that integer as transmitted highest-
	// index-first -- i.e. the first 8 bits on the wire are byte 7 of the
	// in-memory record, not byte 0. Confirmed empirically against a real
	// save: storing bytes in transmission order round-tripped Minish
	// Cap's own EEPROM validation string as the scrambled
	// "ADLEZBGANIM EHT:" instead of the correct ":THE MINISH CAP:ZELDA ".
	u8 data[8];
	for (int b = 0; b < 8; b++)
		data[7 - b] = (u8)extractBits(bits, 2 + addrBits + (u32)b * 8, 8);

	u8* dst = &s_eeprom[record * 8];
	if (memcmp(dst, data, 8) != 0)
	{
		memcpy(dst, data, 8);
		s_dirty = true;
	}
}

void gbaEepromCommandRead(u16* outBits, u32 count)
{
	u32 record = s_eepromHavePending ? s_eepromPendingAddr : 0;
	const u8* src = &s_eeprom[record * 8];

	for (u32 i = 0; i < count; i++)
	{
		if (i < 4) { outBits[i] = 0; continue; }   // dummy bits
		u32 dataBit = i - 4;                        // 0..63, MSB-first
		u8 byte = src[7 - (dataBit >> 3)];           // see gbaEepromCommandWrite's comment
		outBits[i] = (byte >> (7 - (dataBit & 7))) & 1;
	}
}

// PLAN.md §4.3 item 7 (GBA savestate support). See gba_backup.h's comment
// for why s_sram/s_eeprom's contents are deliberately excluded.
void gbaBackupSaveState(EMUFILE* os)
{
	write32le(1, os); // version
	write32le((u32)s_type, os);
	write32le(s_eepromPendingAddr, os);
	write8le(s_eepromHavePending ? 1 : 0, os);
	write8le(s_dirty ? 1 : 0, os);
}

bool gbaBackupLoadState(EMUFILE* is, int size)
{
	u32 version;
	if (!read32le(&version, is)) return false;
	if (version != 1) return false;

	u32 v; u8 b;
	if (!read32le(&v, is)) return false; s_type = (GbaBackupType)v;
	if (!read32le(&s_eepromPendingAddr, is)) return false;
	if (!read8le(&b, is)) return false; s_eepromHavePending = b != 0;
	if (!read8le(&b, is)) return false; s_dirty = b != 0;
	return true;
}
