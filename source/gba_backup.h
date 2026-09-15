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

#ifndef GBA_BACKUP_H
#define GBA_BACKUP_H

#include "types.h"

class EMUFILE;

// docs/PLAN.md §4.3: GBA cartridge save memory (SRAM/EEPROM). See
// gba_backup.cpp's file comment for the protocol writeup and sourcing.

enum GbaBackupType { GBA_BACKUP_NONE, GBA_BACKUP_SRAM, GBA_BACKUP_EEPROM };

// Scans the just-loaded ROM image for the GBATEK-documented backup-type ID
// strings ("EEPROM_Vnnn", "SRAM_Vnnn", "FLASH(512|1M)_Vnnn") that every
// commercial cart's linker embeds verbatim -- the same technique every
// other GBA emulator uses to pick a save type, since nothing in the ROM
// header itself declares it. Sets the type gbaEepromAddrHit()/gbaDecodeAddr's
// callers (MMU.cpp, gba_dma.cpp) consult to route 0x0E000000/0x0D000000
// accesses. Flash carts are detected only so they're not silently
// misrouted as plain SRAM -- real chip emulation for them isn't
// implemented yet (see the .cpp file comment).
void gbaBackupDetect(const u8* rom, u32 romSize);

GbaBackupType gbaBackupType();

// Loads existing save data from `path` into the backup's RAM-resident
// buffer -- a missing file is a clean "no prior save", not an error. Call
// once, right after gbaBackupDetect(), from the GBA ROM-load path only --
// never from NDS_Reset()'s per-reset GBA block, or a soft reset would wipe
// the player's save.
void gbaBackupLoadFile(const char* path);

// Writes the backup buffer to `path` (remembered from gbaBackupLoadFile)
// if anything has changed since the last flush. Cheap to call
// unconditionally every frame (checks a dirty flag first before doing any
// I/O) -- gbaExecFrame() does exactly that.
void gbaBackupFlushIfDirty();

// --- SRAM: flat byte-addressable 32 KB window at 0x0E000000/0x0E010000+ -
u8   gbaSramRead8(u32 offset);
void gbaSramWrite8(u32 offset, u8 val);

// --- EEPROM: DMA-driven bit-serial protocol, wait-state-2 bank (0x0D) ---
// True when `addr` is within the cartridge's EEPROM-mapped window --
// GBA_BACKUP_EEPROM only, and (per GBATEK) restricted to the top 256 bytes
// of the 0x0D bank for >16 MB carts, the whole bank otherwise. Consulted by
// both MMU.cpp (to route a direct CPU/non-DMA touch to an inert stub --
// real hardware requires DMA for the actual protocol) and gba_dma.cpp (to
// special-case a whole DMA transfer that targets it).
bool gbaEepromAddrHit(u32 addr);

// One full protocol command each, framed by the DMA transfer that carries
// it -- `count` is that transfer's exact word count, which is what
// disambiguates a 6-bit-address chip (9/73-word commands, 512-byte total)
// from a 14-bit-address chip (17/81-word commands, 8 KB total); see the
// .cpp file comment for why per-access (rather than per-transfer) framing
// can't do this safely. `bits[i]`/`outBits[i]` carry one protocol bit each
// in bit 0, matching what a real 16-bit access to the EEPROM address reads
// or writes (only bit 0 is meaningful on real hardware).
void gbaEepromCommandWrite(const u16* bits, u32 count);
void gbaEepromCommandRead(u16* outBits, u32 count);

// PLAN.md §4.3 item 7 (GBA savestate support). Deliberately does NOT
// include s_sram/s_eeprom's contents: that's the persistent .sav payload,
// already written out separately (gbaBackupFlushIfDirty(), on its own
// dirty-flag-gated schedule) and reloaded from disk once at ROM-load time
// (gbaBackupLoadFile()) -- duplicating up to 32KB/8KB of it into every
// savestate would be redundant and, worse, would let a stale savestate
// silently roll back save-file contents a player already flushed to disk
// after the state was taken. What *is* saved here is the in-flight
// backup-controller protocol state that a plain register/memory replay
// can't reconstruct: s_type (so a load doesn't have to re-run ID-string
// detection against ROM bytes that are already fixed for a running
// session), the EEPROM read-request latch (s_eepromPendingAddr/
// s_eepromHavePending -- mid-protocol state if a savestate lands between a
// read-request command and its matching read-reply command), and s_dirty
// (so a reload doesn't spuriously suppress a flush that was pending before
// the state was saved, or vice versa).
void gbaBackupSaveState(EMUFILE* os);
bool gbaBackupLoadState(EMUFILE* is, int size);

#endif
