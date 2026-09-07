/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_arm7_profile.cpp
 *
 * Fills a JitCpuProfile for the DS ARM7 (ARM7TDMI, ARMv4T). This is the only
 * file that knows DeSmuME's ARM7 memory map and CPU state layout; everything
 * else in jit/ goes through the profile.
 *
 * P1: state pointers, compile-time fetch and the slow guest-memory path are
 * real; swiHandler is still a stub.
 * P11: canEnterArm / arm7_cyclesForArm are real -- the shared jit_arm.cpp
 * front-end now runs on the ARM7 (ARMv4T) too, with its ARMv5-only encodings
 * and the ARMv4-vs-v5 pc-load interworking difference gated on cpu.isaLevel.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include "../MMU.h"
#include "../armcpu.h"

// --- compile-time guest fetch (trace scanning / literal folding) ------------
static u32 arm7_fetch16(u32 addr) { return _MMU_read16<ARMCPU_ARM7, MMU_AT_CODE>(addr & ~1u); }
static u32 arm7_fetch32(u32 addr) { return _MMU_read32<ARMCPU_ARM7, MMU_AT_CODE>(addr & ~3u); }

// --- runtime guest memory slow path ----------------------------------------
static u32 arm7_slowRead(u32 addr, u32 size)
{
	switch (size) {
		case 1:  return _MMU_read08<ARMCPU_ARM7>(addr);
		case 2:  return _MMU_read16<ARMCPU_ARM7>(addr);
		default: return _MMU_read32<ARMCPU_ARM7>(addr);
	}
}

static void arm7_slowWrite(u32 addr, u32 val, u32 size)
{
	switch (size) {
		case 1:  _MMU_write08<ARMCPU_ARM7>(addr, (u8)val);  break;
		case 2:  _MMU_write16<ARMCPU_ARM7>(addr, (u16)val); break;
		default: _MMU_write32<ARMCPU_ARM7>(addr, val);      break;
	}
}

static void arm7_smcInvalidate(u32 addr) { jitCacheArm7.invalidateSMCTarget(addr); }

static u32  arm7_swiHandler(u32 comment) { (void)comment; return 0; }   // P2/P3
static bool arm7_canEnterThumb(u32 pc)   { (void)pc; return true; }
// P11: ARM-mode entry. Same unconditional rule as canEnterThumb -- the ARM7
// executes from main RAM / WRAM (and, with an external BIOS, low BIOS), all of
// which the MMU_AT_CODE fetch path backs with real memory; anything the ARM
// front-end can't compile ends the trace and the interpreter takes it. The
// jit_arm.cpp emitter gates every ARMv5-only encoding (BLX, CLZ, QADD, the DSP
// muls, LDRD/STRD) and the ARMv4-vs-v5 LDM/LDR pc interworking difference on
// cpu.isaLevel, so it is safe to run on ARMv4T here.
static bool arm7_canEnterArm(u32 pc)     { (void)pc; return true; }

// Per-instruction cycle cost, approximating armcpu_exec<ARM7>()'s own return
// (MMU_fetchExecuteCycles ignores the code fetch; ALU=1; data access adds
// 3 + memcycles for loads, 2 + memcycles for stores; ARM7 main-RAM word
// access = 2, everything else = 1 -- we assume main RAM, the common case).
// Exact per-address timing is a P5 refinement; the differential harness
// measures the drift.
static u8 arm7_cyclesForThumb(u16 op)
{
	switch (op >> 12) {
		case 0x4:
			if ((op & 0x0FC0) == 0x0340) return 4;          // MUL (approx)
			if ((op & 0x0400) == 0) {                        // F4 ALU (not hi-reg/BX)
				const u16 aluOp = (op >> 6) & 0xF;
				if (aluOp == 2 || aluOp == 3 || aluOp == 4 || aluOp == 7)
					return 2;                                 // LSL/LSR/ASR/ROR by
					                                          // register: 1S+1I on real
					                                          // ARM7TDMI (the extra
					                                          // internal cycle fetches
					                                          // the shift amount from a
					                                          // register) -- was
					                                          // undercounted at 1 like
					                                          // every other F4 op.
			}
			return 1;                                        // F4 ALU (rest) / F5 hi-reg
		case 0x5: {                                          // F10 reg-offset ld/st
			const u16 s = op & 0x0E00;
			const bool load = (s == 0x0800 || s == 0x0A00 || s == 0x0C00 ||
			                   s == 0x0600 || s == 0x0E00);
			return load ? 5 : 3;
		}
		case 0x6: case 0x9:                                  // F9 word / F11 SP
			return (op & 0x0800) ? 5 : 3;
		case 0x7: case 0x8:                                  // F9 byte / F8 half
			return (op & 0x0800) ? 4 : 3;
		case 0xB: {
			if ((op & 0x0F00) == 0x0000) return 1;           // F13 ADD/SUB SP
			if ((op & 0x0600) == 0x0400) {                   // F14 PUSH/POP
				u32 n = __builtin_popcount(op & 0xFF) + ((op & 0x0100) ? 1 : 0);
				return (u8)((op & 0x0800 ? 2 : 3) + 2 * n);
			}
			return 1;
		}
		case 0xC: {                                          // F15 LDMIA/STMIA
			u32 n = __builtin_popcount(op & 0xFF);
			if (!n) n = 1;
			return (u8)(2 + 2 * n);
		}
		case 0xD:                                            // F16 Bcc
			// Interpreter's OP_B_COND returns 1 when not taken, 3 when
			// taken (thumb_instructions.cpp) -- this value feeds the trace
			// scanner's per-instruction cyclesAccum, which only ever
			// applies on the NOT-taken/fall-through path (the taken exit
			// in jit_thumb.cpp's case 26/27 hardcodes its own +3 to match
			// the taken cost directly, since a single opcode->cycles
			// function can't distinguish the two outcomes at compile
			// time). Was flatly 3 for both, overcounting every
			// not-taken conditional branch by 2 cycles -- extremely
			// common (every loop-continuation check) and a real source
			// of cumulative JIT-vs-interpreter timing drift.
			return 1;
		case 0xE:                                            // F18 B
			// OP_B_UNCOND returns 1 (thumb_instructions.cpp) -- was
			// flatly 3 here, overcounting every unconditional branch
			// (also extremely common) by 2 cycles.
			return 1;
		case 0xF:                                            // F19 BL
			return 4;
		case 0xA:                                            // F12 ADD PC/SP
			return 1;
		default:                                             // F1/F2/F3
			return 1;
	}
}
// Per-ARM-instruction cost, approximating armcpu_exec<ARM7>()'s own return
// (arm_instructions.cpp). ARM7 is a 3-stage pipeline: MMU_aluMemCycles<ARM7>
// ADDS the ALU and MEM cycle counts (unlike the ARM9 max()). Code-fetch cycles
// are off (Fetch() returns 1). Same "assume main RAM" coarseness as
// arm7_cyclesForThumb -- main-RAM read: word = 2, byte/half = 1; write = 1 --
// so this over-counts a bounded amount for the cheaper banks (ITCM has none on
// ARM7; WRAM is 1). The differential harness's cycDrift telemetry measures the
// error; exact per-address timing is a later refinement.
static u8 arm7_cyclesForArm(u32 op)
{
	// cond == NV space: ARMv4T undefined -> interpreter (the emitter bails), but
	// keep a sane value for the scanner's running sum in case it is ever reached.
	if ((op >> 28) == 0xF) return 1;

	// LDR / STR single data transfer (bits 27..26 == 01).
	if (((op >> 26) & 3) == 1) {
		const bool B = (op >> 22) & 1;
		const bool L = (op >> 20) & 1;
		if (L && !B && ((op >> 12) & 0xFu) == 15) return 7;   // LDR pc: OP_LDR(_,5) + word mem 2
		if (L) return B ? 4u : 5u;                            // LDRB 3+1 / LDR 3+2
		return 3u;                                            // STR / STRB : 2 + write 1
	}
	// extra load/store LDRH / STRH / LDRSB / LDRSH (000, bit7 & bit4, bits6..5 != 0)
	if ((op & 0x0E000000u) == 0 && (op & 0x90u) == 0x90u && (op & 0x60u) != 0) {
		return ((op >> 20) & 1) ? 4u : 3u;                    // load 3+1 / store 2+1
	}
	// LDM / STM block data transfer (bits 27..25 == 100).
	if ((op & 0x0E000000u) == 0x08000000u) {
		u32 n = (u32)__builtin_popcount(op & 0xFFFFu);
		if (!n) n = 1;
		const bool L = (op >> 20) & 1;
		if (L) return (u8)((((op >> 15) & 1) ? 4u : 2u) + 2u * n);  // 2 + read(2)*n  (pc: +4)
		return (u8)(2u + n);                                        // STM: 2 + write(1)*n
	}
	// BX / BLX register (misc space) -- OP_BX / OP_BLX_REG return 3.
	if ((op & 0x0FFFFFD0u) == 0x012FFF10u) return 3;
	// SWP / SWPB : MMU_aluMemCycles(4, read + write) -- word 4+2+1, byte 4+1+1.
	if ((op & 0x0FB00FF0u) == 0x01000090u) return ((op >> 22) & 1) ? 6u : 7u;
	// Multiply / multiply-long (bits 27..23 == 0000x, bits 7..4 == 1001).
	// Data-dependent on Rs in the interpreter; a fixed per-form mid estimate.
	if ((op & 0x0FC000F0u) == 0x00000090u || (op & 0x0F8000F0u) == 0x00800090u) {
		const bool longForm = (op >> 23) & 1;
		const bool accum    = (op >> 21) & 1;
		return (u8)(longForm ? (accum ? 5 : 4) : (accum ? 4 : 3));
	}
	// Data-processing, register operand2 shifted by a register (class 00, bit25 == 0,
	// bit7 == 0, bit4 == 1): OP_xxx_<shift>_REG returns 2. MRS/MSR (10x0 control
	// space) is excluded so it keeps its flat 1.
	if (((op >> 26) & 3) == 0 && (op & 0x02000090u) == 0x00000010u) return 2;
	// data-processing (class 00) writing Rd == 15, non-control-space: OP_xxx's
	// PC-write path returns 3 (imm / shift-by-imm) or 4 (shift-by-register).
	if (((op >> 26) & 3) == 0 && ((op >> 12) & 0xFu) == 15 &&
	    !(((op >> 23) & 3) == 2 && !((op >> 20) & 1))) {
		const bool regShift = (op & 0x02000010u) == 0x00000010u && !((op >> 7) & 1);
		return regShift ? 4u : 3u;
	}
	// data-processing (imm / shift-by-imm), B/BL not-taken, MRS, MSR, predication
	// failure: all 1S.
	return 1;
}

static JitCpuProfile s_arm7Profile;

JitCpuProfile* jitBuildArm7Profile()
{
	s_arm7Profile.state.gpr       = &NDS_ARM7.R[0];
	s_arm7Profile.state.cpsr      = &NDS_ARM7.CPSR.val;
	s_arm7Profile.state.readTable = nullptr;               // inline fast path: P6

	s_arm7Profile.fetch16        = arm7_fetch16;
	s_arm7Profile.fetch32        = arm7_fetch32;
	s_arm7Profile.slowRead       = arm7_slowRead;
	s_arm7Profile.slowWrite      = arm7_slowWrite;
	s_arm7Profile.smcInvalidate  = arm7_smcInvalidate;

	s_arm7Profile.swiHandler     = arm7_swiHandler;
	s_arm7Profile.canEnterThumb  = arm7_canEnterThumb;
	s_arm7Profile.canEnterArm    = arm7_canEnterArm;
	s_arm7Profile.cyclesForThumb = arm7_cyclesForThumb;
	s_arm7Profile.cyclesForArm   = arm7_cyclesForArm;

	s_arm7Profile.isaLevel    = 4;                          // ARMv4T
	// DS ARM7 executes from main RAM (0x02xxxxxx) and WRAM / shared WRAM
	// (0x03xxxxxx); those are the banks the SMC registry must track.
	s_arm7Profile.smcBankMask = (1u << 2) | (1u << 3);

	return &s_arm7Profile;
}

#endif // DESMUME_JIT_ARM7
