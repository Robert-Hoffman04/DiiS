/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_arm7gba_profile.cpp
 *
 * Fills a JitCpuProfile for ARM7 running in GBA-compat mode (roadmap #20,
 * §12.3 step 5). GBA's ARM7TDMI is the exact same core family DS's own ARM7
 * profile (jit_arm7_profile.cpp) already targets -- both run on the same
 * NDS_ARM7 register file, through the same jit_arm.cpp/jit_thumb.cpp
 * front-end, sharing the same jitCacheArm7 arena/block table/SMC registry
 * (only one boot mode is ever live at a time, so there is no need for a
 * second cache slot -- see jit_trace.cpp's jitSetArm7GBAMode()). This file
 * only supplies a *different* JitCpuProfile so the shared machinery reaches
 * ARM7 with GBA-appropriate memory routing and page-fast-path settings.
 *
 * Memory routing itself needs nothing new here: §12.3 step 4 put the
 * MMU.isGBA guard as the first statement of MMU.h's six FORCEINLINE
 * _MMU_read/write08/16/32 dispatchers, and every one of this profile's
 * memory hooks below funnels through those same templated calls (traced
 * end to end: fetch16/32 and slowRead/Write call the one/two-template-param
 * overloads in MMU.h, which call straight into the guarded three-arg
 * dispatchers) -- so the mode-awareness already lives one layer down, in
 * the MMU, not in this file's function bodies.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

#include "../MMU.h"
#include "../armcpu.h"

// --- compile-time guest fetch (trace scanning / literal folding) ------------
// Identical in form to jit_arm7_profile.cpp's arm7_fetch16/32 -- see this
// file's banner comment for why no GBA-specific logic is needed here.
static u32 arm7gba_fetch16(u32 addr) { return _MMU_read16<ARMCPU_ARM7, MMU_AT_CODE>(addr & ~1u); }
static u32 arm7gba_fetch32(u32 addr) { return _MMU_read32<ARMCPU_ARM7, MMU_AT_CODE>(addr & ~3u); }

// --- runtime guest memory slow path ----------------------------------------
static u32 arm7gba_slowRead(u32 addr, u32 size)
{
	switch (size) {
		case 1:  return _MMU_read08<ARMCPU_ARM7>(addr);
		case 2:  return _MMU_read16<ARMCPU_ARM7>(addr);
		default: return _MMU_read32<ARMCPU_ARM7>(addr);
	}
}

static void arm7gba_slowWrite(u32 addr, u32 val, u32 size)
{
	switch (size) {
		case 1:  _MMU_write08<ARMCPU_ARM7>(addr, (u8)val);  break;
		case 2:  _MMU_write16<ARMCPU_ARM7>(addr, (u16)val); break;
		default: _MMU_write32<ARMCPU_ARM7>(addr, val);      break;
	}
}

// Same cache as the DS ARM7 profile -- both boot modes share jitCacheArm7.
static void arm7gba_smcInvalidate(u32 addr) { jitCacheArm7.invalidateSMCTarget(addr); }

// Dead code today, same as arm7_swiHandler/arm9_swiHandler: jit_thumb.cpp's
// SWI case (opcode & 0x0F00)==0x0F00 sets ctx.endBlock and never calls
// prof->swiHandler, so every SWI correctly falls out of the trace to the
// interpreter -- and, once §12.3 step 6 (bios_gba.cpp) lands, to its
// function-level HLE handlers, exactly like the DS SWI path today.
static u32  arm7gba_swiHandler(u32 comment) { (void)comment; return 0; }

// Unconditional true, same policy as the DS ARM7 profile: every region the
// CPU can fetch from is backed by a real buffer (per step 4/step 3), if
// presently zero-filled since no BIOS/ROM exists yet (steps 6/1). Inert
// either way this pass -- nothing sets gameInfo.isGBA from a real load.
static bool arm7gba_canEnterThumb(u32 pc) { (void)pc; return true; }
static bool arm7gba_canEnterArm(u32 pc)   { (void)pc; return true; }

// Verbatim duplicates of jit_arm7_profile.cpp's arm7_cyclesForThumb/
// arm7_cyclesForArm. Not shared via extern on purpose, matching the
// established one-file-per-profile convention (jit_arm9_profile.cpp already
// duplicates rather than shares its own, genuinely different ARMv5TE cost
// tables) -- but unlike that ARM9/ARM7 split, this content is presently
// *identical*: armcpu_exec<ARMCPU_ARM7>() (arm_instructions.cpp /
// thumb_instructions.cpp via MMU_aluMemCycles<PROCNUM>) is one template,
// not specialized per gameInfo.isGBA, so these are exactly what the
// interpreter also computes for GBA-mode ARM7 as of today.
//
// §4.3 item 4 update: WAITCNT-driven cartridge ROM/SRAM wait-state cost IS
// now modeled by the interpreter (MMU_timing.h's isGBA branch in
// _MMU_accesstime/Fetch<>/MMU_fetchExecuteCycles, gated on MMU.isGBA &&
// PROCNUM==ARMCPU_ARM7) -- but *not* here: this file's cost tables are a
// static per-opcode-class estimate baked in at JIT compile time, with no
// per-access runtime address lookup at all (unlike the interpreter's
// dynamic per-memory-access accounting), so a compiled block has no way to
// react to the cartridge ROM address it's actually touching, let alone to
// a WAITCNT rewrite happening after the block was compiled. Per §4.1's
// reference-first methodology ("implement and validate in the DeSmuME
// interpreter first... only then bring in the ARM7 JIT"), that's out of
// scope for this item -- teaching the JIT wait-state timing would need
// either a runtime cost callout per cart-ROM access (undoing much of the
// point of compiling the cost in) or WAITCNT-keyed block invalidation, and
// is left as a documented follow-up, not silently approximated as already
// done. Real GBA wait-state timing for regions outside cart ROM/SRAM
// (EWRAM 2-3 wait / IWRAM 0 wait, unlike DS's own numbers) still isn't
// modeled anywhere yet -- that's a separate, smaller gap. If either
// interpreter or JIT copy's cost model changes, keep this one in sync (or
// extract both to one shared file) rather than let them silently drift.
static u8 arm7gba_cyclesForThumb(u16 op)
{
	switch (op >> 12) {
		case 0x4:
			if ((op & 0x0FC0) == 0x0340) return 4;          // MUL (approx)
			if ((op & 0x0400) == 0) {                        // F4 ALU (not hi-reg/BX)
				const u16 aluOp = (op >> 6) & 0xF;
				if (aluOp == 2 || aluOp == 3 || aluOp == 4 || aluOp == 7)
					return 2;                                 // LSL/LSR/ASR/ROR by register
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
			return 1;
		case 0xE:                                            // F18 B
			return 1;
		case 0xF:                                            // F19 BL
			return 4;
		case 0xA:                                            // F12 ADD PC/SP
			return 1;
		default:                                             // F1/F2/F3
			return 1;
	}
}

static u8 arm7gba_cyclesForArm(u32 op)
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

static JitCpuProfile s_arm7GbaProfile;

JitCpuProfile* jitBuildArm7GBAProfile()
{
	s_arm7GbaProfile.state.gpr       = &NDS_ARM7.R[0];   // same register file as DS ARM7
	s_arm7GbaProfile.state.cpsr      = &NDS_ARM7.CPSR.val;
	s_arm7GbaProfile.state.readTable = nullptr;

	s_arm7GbaProfile.fetch16        = arm7gba_fetch16;
	s_arm7GbaProfile.fetch32        = arm7gba_fetch32;
	s_arm7GbaProfile.slowRead       = arm7gba_slowRead;
	s_arm7GbaProfile.slowWrite      = arm7gba_slowWrite;
	s_arm7GbaProfile.smcInvalidate  = arm7gba_smcInvalidate;

	s_arm7GbaProfile.swiHandler     = arm7gba_swiHandler;
	s_arm7GbaProfile.canEnterThumb  = arm7gba_canEnterThumb;
	s_arm7GbaProfile.canEnterArm    = arm7gba_canEnterArm;
	s_arm7GbaProfile.cyclesForThumb = arm7gba_cyclesForThumb;
	s_arm7GbaProfile.cyclesForArm   = arm7gba_cyclesForArm;

	// P13/P14 inline fast paths: deliberately left OFF (0). Both are DS-specific
	// by construction -- mainMemBase assumes MMU.MAIN_MEM at the DS
	// (addr&0x0F000000)==0x02000000 decode, and a non-zero pageDescBase would be
	// built from MMU.MMU_MEM[ARMCPU_ARM7]/MMU_MASK[ARMCPU_ARM7], the DS ARM7
	// page-lookup tables that §12.3 step 4 deliberately left untouched (only the
	// header-inline _MMU_read/write dispatchers got the MMU.isGBA guard). Turning
	// either on here would reintroduce, one layer up, exactly the DS/GBA address
	// aliasing bug step 4 fixed at the dispatcher level. Leaving both 0 routes
	// every GBA access through slowRead/slowWrite -- correct, and how the ARM9
	// profile already leaves them 0 today for its own (different) reasons. A
	// future optimization pass could build a real GBA-specific page-desc table
	// over GBA_EWRAM/GBA_IWRAM; out of scope here.
	s_arm7GbaProfile.mainMemBase  = 0;
	s_arm7GbaProfile.pageDescBase = 0;
	s_arm7GbaProfile.pageDescLo   = 0;
	s_arm7GbaProfile.pageDescHi   = 0;

	s_arm7GbaProfile.isaLevel    = 4;                       // ARMv4T, same as DS ARM7
	// GBA's writable/executable banks: EWRAM (0x02) and IWRAM (0x03) -- BIOS
	// (0x00) is excluded since GBA BIOS is ROM-like (writes to it are no-ops,
	// see _MMU_ARM7GBA_write08/16/32 in MMU.cpp), so it can never hold SMC.
	// Cartridge ROM (0x08+) isn't backed by a writable buffer yet (step 7).
	s_arm7GbaProfile.smcBankMask = (1u << 2) | (1u << 3);

	return &s_arm7GbaProfile;
}

#endif // DESMUME_JIT_ARM7
