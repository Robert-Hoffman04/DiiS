/****************************************************************************
 * DeSmuMEWii ARM9 JIT
 *
 * jit_arm9_profile.cpp
 *
 * Fills a JitCpuProfile for the DS ARM9 (ARMv5TE). Companion to
 * jit_arm7_profile.cpp -- the only file that knows DeSmuME's ARM9 memory map
 * and CPU state layout; everything else in jit/ goes through the profile.
 *
 * A0 (infra split): state pointers, compile-time fetch and the slow
 * guest-memory path are real. canEnterThumb / canEnterArm returned false so the
 * ARM9 JIT compiled nothing.
 *
 * A2 (THUMB front-end): canEnterThumb is a real region check (ITCM / main RAM /
 * shared WRAM / ARM9 BIOS, DTCM window excluded) and arm9_cyclesForThumb
 * reproduces armcpu_exec<ARM9>()'s own per-instruction cost -- unlike the ARM7
 * (data timing mostly disabled) the ARM9 models data-access waitstates
 * (M32 = 2 on the 32-bit bus, main-RAM word = 4, slow-wait up to 16; combined as
 * max(alu, mem) for the 5-stage pipeline -- MMU_timing.h). v1 assumes the common
 * case (cached / main RAM); the hardened harness's per-block cycle compare
 * quantifies the drift. canEnterArm stays false until A5. See
 * desmumewii-arm9-jit-plan.md.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include "../MMU.h"
#include "../armcpu.h"

// --- compile-time guest fetch (trace scanning / literal folding) ------------
// Routes ITCM (addr < 0x02000000) and shared main RAM correctly via the ARM9
// MMU_AT_CODE fast path (MMU.h). DTCM-as-code returns garbage there -- excluded
// by canEnter* once those go live in A2.
static u32 arm9_fetch16(u32 addr) { return _MMU_read16<ARMCPU_ARM9, MMU_AT_CODE>(addr & ~1u); }
static u32 arm9_fetch32(u32 addr) { return _MMU_read32<ARMCPU_ARM9, MMU_AT_CODE>(addr & ~3u); }

// --- runtime guest memory slow path ---------------------------------------
// _MMU_*<ARMCPU_ARM9> handles DTCM (incl. the DTCM-over-main-RAM priority),
// main RAM, shared WRAM, VRAM bank mapping, palette/OAM and I/O.
static u32 arm9_slowRead(u32 addr, u32 size)
{
	switch (size) {
		case 1:  return _MMU_read08<ARMCPU_ARM9>(addr);
		case 2:  return _MMU_read16<ARMCPU_ARM9>(addr);
		default: return _MMU_read32<ARMCPU_ARM9>(addr);
	}
}

static void arm9_slowWrite(u32 addr, u32 val, u32 size)
{
	switch (size) {
		case 1:  _MMU_write08<ARMCPU_ARM9>(addr, (u8)val);  break;
		case 2:  _MMU_write16<ARMCPU_ARM9>(addr, (u16)val); break;
		default: _MMU_write32<ARMCPU_ARM9>(addr, val);      break;
	}
}

static void arm9_smcInvalidate(u32 addr) { jitCacheArm9.invalidateSMCTarget(addr); }

static u32  arm9_swiHandler(u32 comment) { (void)comment; return 0; }   // A5

// True iff a THUMB block may be compiled starting at `pc`. Mirrors the ARM9
// MMU_AT_CODE fast path (MMU.h): the JIT's compile-time fetch and the
// interpreter's own prefetch both go through _MMU_read*<ARM9, MMU_AT_CODE>, so
// any address that path decodes to a real backing store is safe to scan. The
// one exclusion is the DTCM window: DeSmuME's code path does NOT patch DTCM
// over main RAM (it returns MAIN_MEM there), so a block scanned from a
// DTCM-shadowed main-RAM address would compile the wrong bytes -- conservative
// bail, matching the plan. canEnterArm stays false until A5.
static bool arm9_canEnterThumb(u32 pc)
{
	// DTCM window (relocatable via CP15; default 0x027C0000). Excluded first
	// because it overlays the main-RAM range.
	if ((pc & ~0x3FFFu) == MMU.DTCMRegion) return false;

	if (pc < 0x02000000)                    return true;   // ITCM window
	if ((pc & 0x0F000000) == 0x02000000)    return true;   // main RAM (shared)
	if ((pc >> 24) == 0x03)                 return true;   // shared WRAM
	if ((pc & 0xFFFF0000u) == 0xFFFF0000u)  return true;   // ARM9 BIOS
	return false;
}
// ARM-mode entry: same region rule as canEnterThumb -- compile-time fetch and
// interpreter prefetch both go through _MMU_read32<ARM9, MMU_AT_CODE>, so any
// address that path backs with real memory is safe; the DTCM window is the sole
// exclusion (that path returns MAIN_MEM there, not DTCM). B1 emitters are
// ARMv4T-safe; ARMv5 edges (PC-interworking, CLZ/QADD/...) either bail or land
// in later B-groups.
static bool arm9_canEnterArm(u32 pc)
{
	if ((pc & ~0x3FFFu) == MMU.DTCMRegion) return false;
	if (pc < 0x02000000)                    return true;   // ITCM window
	if ((pc & 0x0F000000) == 0x02000000)    return true;   // main RAM (shared)
	if ((pc >> 24) == 0x03)                 return true;   // shared WRAM
	if ((pc & 0xFFFF0000u) == 0xFFFF0000u)  return true;   // ARM9 BIOS
	return false;
}

// Per-instruction cycle cost -- must reproduce armcpu_exec<ARM9>()'s own return
// (see thumb_instructions.cpp). Code-fetch cycles are off (Fetch() returns 1);
// data accesses go through max(alu, memCycles) (MMU_aluMemCycles<ARM9>). With
// this build's timing flags (ACCOUNT_FOR_NON_SEQUENTIAL_ACCESS /
// ENABLE_CACHE_CONTROLLER_EMULATION off, ACCOUNT_FOR_DATA_TCM_SPEED on) a
// main-RAM access via _MMU_accesstime falls through to MMU_WAIT[0x02] = M16,
// where M32 = 2 for the ARM9 32-bit bus: word = 4, half/byte = 2. v1 assumes
// main RAM (the common case; ITCM/DTCM/WRAM are cheaper, so this over-counts a
// bounded amount, surfaced by the harness's cycle-drift telemetry).
static u8 arm9_cyclesForThumb(u16 op)
{
	// main-RAM data-access costs after the max(alu, mem) combine
	enum { LDW = 4, STW = 4, LDHB = 3, STHB = 2 };

	switch (op >> 12) {
		case 0x4:
			if ((op & 0x0FC0) == 0x0340) return 3;          // MUL (2..5, data-dep; mid)
			if ((op & 0x0400) == 0) {                        // F4 ALU (not hi-reg/BX)
				const u16 aluOp = (op >> 6) & 0xF;
				if (aluOp == 2 || aluOp == 3 || aluOp == 4 || aluOp == 7)
					return 2;                                 // shift by register: 1S+1I
			} else if ((op & 0x0380) == 0x0380) {
				return 4;                                     // F5 BLX (reg): OP_BLX_THUMB
			} else if ((op & 0x0380) == 0x0300) {
				return 3;                                     // F5 BX (reg): OP_BX_THUMB
			}
			return 1;                                        // F4 ALU (rest) / F5 hi-reg
		case 0x5: {                                          // F7/F8 reg-offset ld/st
			switch (op & 0x0E00) {
				case 0x0000: return STW;                     // STR
				case 0x0200: return STHB;                    // STRH
				case 0x0400: return STHB;                    // STRB
				case 0x0600: return LDHB;                    // LDRSB
				case 0x0800: return LDW;                     // LDR
				case 0x0A00: return LDHB;                    // LDRH
				case 0x0C00: return LDHB;                    // LDRB
				default:     return LDHB;                    // LDRSH
			}
		}
		case 0x6: case 0x9:                                  // F9 word / F11 SP word
			return (op & 0x0800) ? LDW : STW;
		case 0x7:                                            // F9 byte
			return (op & 0x0800) ? LDHB : STHB;
		case 0x8:                                            // F10 half
			return (op & 0x0800) ? LDHB : STHB;
		case 0xB: {
			if ((op & 0x0F00) == 0x0000) return 1;           // F13 ADD/SUB SP
			if ((op & 0x0600) == 0x0400) {                   // F14 PUSH/POP
				const u32 regs = __builtin_popcount(op & 0xFF);
				const bool extra = (op & 0x0100) != 0;       // LR (PUSH) / PC (POP)
				if (op & 0x0800) {                           // POP
					if (extra) {                             // POP {..,PC}
						u32 c = 4 * (regs + 1);
						return (u8)(c < 5 ? 5 : c);
					}
					u32 c = 4 * regs;
					return (u8)(c < 2 ? 2 : c);
				}
				u32 c = 4 * (regs + (extra ? 1 : 0));        // PUSH
				const u32 alu = extra ? 4 : 3;
				return (u8)(c < alu ? alu : c);
			}
			return 1;
		}
		case 0xC: {                                          // F15 LDMIA/STMIA
			u32 n = __builtin_popcount(op & 0xFF);
			if (!n) n = 1;
			u32 c = 4 * n;
			const u32 alu = (op & 0x0800) ? 3 : 2;           // LDMIA 3 / STMIA 2
			return (u8)(c < alu ? alu : c);
		}
		case 0xD:                                            // F16 Bcc (not-taken)
			if ((op & 0x0F00) == 0x0F00) return 3;           // SWI (terminator)
			return 1;                                        // taken +3 hardcoded in jit_thumb
		case 0xE:                                            // F18 B / BLX suffix
			return (op & 0x0800) ? 3 : 1;                    // 0xE800 BLX suffix / 0xE000 B
		case 0xF:                                            // F19 BL prefix/suffix
			return 4;
		case 0xA:                                            // F12 ADD PC/SP
			return 1;
		default:                                             // F1/F2/F3
			return 1;
	}
}
// Per-ARM-instruction cycle cost, approximating armcpu_exec<ARM9>()'s return
// (arm_instructions.cpp). B1's opcode set is exact at a flat 1: data-processing
// (imm operand2) returns 1 (OP_*_IMM_VAL -> OP_xxx(1,3), Rd!=15); a predicated
// instruction whose condition fails is 1 cycle (armcpu.cpp: "condition=false:
// 1S cycle"); a NOT-taken conditional branch is 1 (its taken cost, 3, is
// hardcoded at the exit in jit_arm.cpp, matching OP_B/OP_BL). Grows per B-group
// (loads/stores add max(alu,mem); lists scale with the register count).
// Per-ARM-instruction cost, ARM9 max(alu, mem) model. Data-processing = 1
// (OP_xxx(1,3), Rd != 15); a failed predication = 1 (armcpu.cpp "1S cycle"); a
// not-taken conditional branch = 1 (taken cost 3 is hardcoded at the exit,
// matching OP_B/OP_BL). LDR/STR single transfer (bits 27..26 == 01) via
// MMU_aluMemAccessCycles, assuming main RAM (word mem = 4): LDR max(3,4) = 4,
// STR max(2,4) = 4, LDRB max(3,2) = 3, STRB max(2,2) = 2. Same "assume main
// RAM" coarseness as arm9_cyclesForThumb -- a runtime EA penalty (v2) is the
// real fix; the harness cycDrift telemetry tracks the error meanwhile.
static u8 arm9_cyclesForArm(u32 op)
{
	if (((op >> 26) & 3) == 1) {              // LDR / STR single data transfer
		const bool B = (op >> 22) & 1;
		const bool L = (op >> 20) & 1;
		if (!B) return 4;                     // word
		return L ? 3 : 2;                     // byte
	}
	// extra load/store LDRH/STRH/LDRSB/LDRSH (000, bit7&bit4, bits6..5 != 0)
	if ((op & 0x0E000000u) == 0 && (op & 0x90u) == 0x90u && (op & 0x60u) != 0)
		return ((op >> 20) & 1) ? 3u : 2u;   // load max(3,2)=3 / STRH max(2,2)=2
	// LDM / STM block data transfer (bits 27..25 == 100): MMU_aluMemCycles(2|1, c)
	// with c = sum of per-word access cost. Same "assume main RAM" word = 4 as the
	// single transfer above; c = 4*n dominates the alu term for any non-empty list.
	if ((op & 0x0E000000u) == 0x08000000u) {
		u32 n = (u32)__builtin_popcount(op & 0xFFFFu);
		if (!n) n = 1;
		return (u8)(4u * n);
	}
	return 1;
}

static JitCpuProfile s_arm9Profile;

JitCpuProfile* jitBuildArm9Profile()
{
	s_arm9Profile.state.gpr       = &NDS_ARM9.R[0];
	s_arm9Profile.state.cpsr      = &NDS_ARM9.CPSR.val;
	s_arm9Profile.state.readTable = nullptr;               // inline fast path: post-A3

	s_arm9Profile.fetch16        = arm9_fetch16;
	s_arm9Profile.fetch32        = arm9_fetch32;
	s_arm9Profile.slowRead       = arm9_slowRead;
	s_arm9Profile.slowWrite      = arm9_slowWrite;
	s_arm9Profile.smcInvalidate  = arm9_smcInvalidate;

	s_arm9Profile.swiHandler     = arm9_swiHandler;
	s_arm9Profile.canEnterThumb  = arm9_canEnterThumb;
	s_arm9Profile.canEnterArm    = arm9_canEnterArm;
	s_arm9Profile.cyclesForThumb = arm9_cyclesForThumb;
	s_arm9Profile.cyclesForArm   = arm9_cyclesForArm;

	s_arm9Profile.isaLevel    = 5;                          // ARMv5TE
	// ARM9 executes from ITCM (bank 0x00, CP15-relocatable but its default
	// window is low), main RAM (0x02, shared) and shared WRAM (0x03). DTCM
	// never executes. The static mask can't track relocatable TCM, so it is
	// backed by a full jitCacheArm9 flush on any CP15 TCM-config write
	// (cp15.cpp) -- a relocated DTCM sharing a tracked bank just costs a
	// wasted empty-bucket walk.
	s_arm9Profile.smcBankMask = (1u << 0) | (1u << 2) | (1u << 3);

	return &s_arm9Profile;
}

#endif // DESMUME_JIT_ARM7
