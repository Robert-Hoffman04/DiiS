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
 * guest-memory path are real. canEnterThumb / canEnterArm return false so the
 * ARM9 JIT compiles nothing yet -- jitRunArm9() is wired but inert. The THUMB
 * region check + real cycle model land in A2, the ARM front-end + ARMv5TE
 * additions in A5. See desmumewii-arm9-jit-plan.md.
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

static u32  arm9_swiHandler(u32 comment) { (void)comment; return 0; }   // A2/A5
static bool arm9_canEnterThumb(u32 pc)   { (void)pc; return false; }    // A2
static bool arm9_canEnterArm(u32 pc)     { (void)pc; return false; }    // A5

// Per-instruction cycle cost. STUB for A0 (nothing compiles, so this is never
// called). A2 replaces it with a model that reproduces armcpu_exec<ARM9>()'s
// own return: ALU = 1; data access = max(alu, _MMU_accesstime) where cached /
// TCM / main-RAM word = 1..2 and slow-wait regions = up to 16 (MMU_timing.h).
// Unlike the ARM7 (timing mostly disabled), ARM9 data-access waitstates ARE
// modelled -- see the plan, "Cycle model".
static u8 arm9_cyclesForThumb(u16 op) { (void)op; return 1; }
static u8 arm9_cyclesForArm(u32 op)   { (void)op; return 1; }

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
