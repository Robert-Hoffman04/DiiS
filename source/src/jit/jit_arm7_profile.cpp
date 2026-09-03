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
 * real; swiHandler / canEnterArm / the cycle model are stubs filled in by
 * later phases.
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

static void arm7_smcInvalidate(u32 addr) { jitCache.invalidateSMCTarget(addr); }

static u32  arm7_swiHandler(u32 comment) { (void)comment; return 0; }   // P2/P3
static bool arm7_canEnterThumb(u32 pc)   { (void)pc; return true; }
static bool arm7_canEnterArm(u32 pc)     { (void)pc; return false; }    // P7

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
			return 1;                                        // F4 ALU / F5 hi-reg
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
			return (op & 0x0F00) == 0x0F00 ? 3 : 3;          // taken cost (approx)
		case 0xE:                                            // F18 B
			return 3;
		case 0xF:                                            // F19 BL
			return 4;
		case 0xA:                                            // F12 ADD PC/SP
			return 1;
		default:                                             // F1/F2/F3
			return 1;
	}
}
static u8   arm7_cyclesForArm(u32 op)    { (void)op; return 1; }        // P7

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
