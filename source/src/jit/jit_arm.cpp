/****************************************************************************
 * DeSmuMEWii ARM9 JIT
 *
 * jit_arm.cpp
 *
 * ARM (32-bit) emitter table, ARMv4T / ARMv5TE. Runs on the same
 * jit_trace.cpp scanner, register allocator, packed-flag helpers and
 * slow-memory path as jit_thumb.cpp -- it is a second decoder + emitter
 * table, not a new core. Currently wired only into the ARM9 dispatch
 * (jitRunArm9); the ARM7 profile's canEnterArm stays false.
 *
 * Group B1 (this file's current scope):
 *   - per-instruction predication wrapper (the one new control-flow shape)
 *   - data-processing with an immediate operand2 (all 16 ALU ops, S bit)
 *   - B / BL
 * Everything else ends the trace cleanly at that PC (the interpreter takes
 * it) -- never a guess. See desmumewii-arm9-jit-plan.md phase A5 / the plan
 * file, and jit_thumb.cpp for the shared idioms.
 ***************************************************************************/

#include "jit_trace.h"

#if defined(DESMUME_JIT_ARM7)

#include "jit_ppc_emitter.h"

namespace {

// Materialise a 32-bit constant into host register r (1 or 2 words).
inline void emitLoadImm32(u32*& p, u8 r, u32 v)
{
	if ((s32)(s16)v == (s32)v) { *p++ = PPC_LI(r, v & 0xFFFF); return; }
	*p++ = PPC_LIS(r, v >> 16);
	if (v & 0xFFFF) *p++ = PPC_ORI(r, r, v & 0xFFFF);
}

inline u32 ror32(u32 x, u32 n)
{
	n &= 31;
	return n ? ((x >> n) | (x << (32 - n))) : x;
}

// ARM condition field helpers.
enum { COND_AL = 0xE, COND_NV = 0xF };

// ------------------------------------------------------------------ B / BL
// cond .. 101 L[24] offset24         target = PC+8 + signext24(offset)<<2
// L selects BL (writes LR = return address). cond==NV in this space is
// ARMv5 BLX(imm) -- left to the interpreter in B1.
void emitBranch(JitTraceCtx& ctx, u32 op, u8 cond)
{
	u32*& p = ctx.emitPtr;
	const bool isBL   = (op >> 24) & 1;
	const s32  sOff   = (s32)(op << 8) >> 6;           // signext24 << 2
	const u32  target = ctx.currentPC + 8 + (u32)sOff;
	const u32  retLR  = ctx.currentPC + 4;

	ctx.ensureArena();

	if (cond == COND_AL) {
		u32 lockedMask = 0;
		if (isBL) {
			const u8 hLR = ctx.writeReg(14, true, lockedMask);
			emitLoadImm32(p, hLR, retLR);
		}
		ctx.emitStaticExit(target, ctx.instrCount + 1, 3);   // OP_B/OP_BL return 3
		ctx.instrCount++;
		ctx.currentPC += 4;
		ctx.endBlock = true;
		ctx.blockTerminatedEarly = true;
		return;
	}

	// Predicated: cond-false falls through and the block keeps compiling
	// (the THUMB Bcc shape). The taken path is a self-contained exit that
	// must not disturb the register/flag cache the fall-through relies on --
	// so use the non-clearing flushes and write LR straight to guest memory
	// rather than through writeReg().
	ctx.emitEvalCond(cond);
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* skip = p++;                                    // BEQ over the exit

	ctx.emitDirtyFlagFlush();                           // memory current, ctx still "dirty"
	ctx.emitDirtyRegisterFlush();
	if (isBL) {
		emitLoadImm32(p, PPC_R12, retLR);
		*p++ = PPC_STW(PPC_R12, 14, 14 * 4);
	}
	ctx.emitAddCycles(ctx.cyclesAccum + 3);
	ctx.emitResultMetadata(ctx.instrCount + 1, 0);
	const u32 pipe = target + 8;
	*p++ = PPC_LIS(PPC_R29, pipe >> 16);
	*p++ = PPC_ORI(PPC_R29, PPC_R29, pipe & 0xFFFF);
	*p++ = PPC_LIS(PPC_R4, target >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, target & 0xFFFF);
	{ s32 o = (s32)((u8*)ctx.cache.linkerReturnAddress - (u8*)p); *p++ = PPC_B(o); }

	*skip = PPC_BEQ((u32)((p - skip) * 4));
	// endBlock stays false: scanner advances PC/instrCount, block continues.
}

// ------------------------------------------------ data-processing, imm op2
// cond .. 00 1 opcode[24:21] S[20] Rn[19:16] Rd[15:12] rot[11:8] imm8[7:0]
//   opcode: 0 AND 1 EOR 2 SUB 3 RSB 4 ADD 5 ADC 6 SBC 7 RSC
//           8 TST 9 TEQ 10 CMP 11 CMN 12 ORR 13 MOV 14 BIC 15 MVN
// The ALU core matches THUMB Format 3/4 exactly (same ADDCO/SUBFCO/ADDEO/
// SUBFEO + emitCVfromXER + emitNZ idioms).
void emitDataProcImm(JitTraceCtx& ctx, u32 op, u8 cond)
{
	const u8   aluOp = (op >> 21) & 0xF;
	const bool S     = (op >> 20) & 1;
	const u8   rn    = (op >> 16) & 0xF;
	const u8   rd    = (op >> 12) & 0xF;
	const u32  rot   = ((op >> 8) & 0xF) * 2;
	const u32  k     = ror32(op & 0xFF, rot);
	const bool kCarry = (k >> 31) & 1;                  // shifter carry when rot != 0

	const bool testOnly = (aluOp >= 8 && aluOp <= 11); // TST/TEQ/CMP/CMN
	const bool ignoresRn = (aluOp == 13 || aluOp == 15); // MOV / MVN
	const bool isLogical =
		(aluOp <= 1) || (aluOp == 8) || (aluOp == 9) || (aluOp >= 12);
	const bool predicated = (cond != COND_AL);

	// bit25=1, opcode in 8..11, S=0  => MSR-immediate / undefined, not a test op.
	if (testOnly && !S) { ctx.endBlock = true; return; }            // MSR imm -> B6
	if (rd == 15)       { ctx.endBlock = true; return; }            // PC write -> B7

	// A PC-relative Rn folds to a compile-time constant. B1 only takes the
	// common ADR forms (ADD/SUB Rd, PC, #imm, always non-S); anything else with
	// Rn==PC ends the trace.
	const bool constResult =
		(aluOp == 13) || (aluOp == 15) ||
		(rn == 15 && !S && (aluOp == 4 || aluOp == 2));
	if (rn == 15 && !ignoresRn && !constResult) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	// Pre-allocate every guest register the body touches BEFORE the predication
	// skip -- an allocation (hence a possible spill store) inside the skipped
	// region would update guest memory only on the taken path while ctx drops
	// the host binding on both, losing the fall-through value.
	u8 hRn = 0;
	if (!ignoresRn && rn != 15) hRn = ctx.readReg(rn, lockedMask);
	u8 hRd = 0;
	if (!testOnly) hRd = ctx.writeReg(rd, !predicated, lockedMask);

	// ---- predication wrapper: skip the body when the condition is false ----
	u32* skip = nullptr;
	if (predicated) {
		ctx.emitEvalCond(cond);
		*p++ = PPC_CMPWI(0, PPC_R11, 0);
		skip = p++;
	}

	// ---- fully-constant results (MOV/MVN #k, and ADR) ----
	if (constResult) {
		u32 val;
		if      (aluOp == 13) val = k;                          // MOV
		else if (aluOp == 15) val = ~k;                         // MVN
		else if (aluOp == 4)  val = ctx.currentPC + 8 + k;      // ADD Rd,PC,#k
		else                  val = ctx.currentPC + 8 - k;      // SUB Rd,PC,#k
		emitLoadImm32(p, hRd, val);
		if (S) {   // only MOV/MVN reach here with S
			ctx.emitFlagConst(JITF_N, (val >> 31) & 1);
			ctx.emitFlagConst(JITF_Z, val == 0);
			if (rot != 0) ctx.emitFlagConst(JITF_C, kCarry);
		}
		if (skip) *skip = PPC_BEQ((u32)((p - skip) * 4));
		return;
	}

	// ---- general case: Rn in hRn, k in R12, result in `res` ----
	emitLoadImm32(p, PPC_R12, k);
	const u8 res = testOnly ? PPC_R11 : hRd;

	if (isLogical) {
		switch (aluOp) {
			case 0:  *p++ = PPC_AND (res, hRn, PPC_R12); break;   // AND
			case 1:  *p++ = PPC_XOR (res, hRn, PPC_R12); break;   // EOR
			case 8:  *p++ = PPC_AND (res, hRn, PPC_R12); break;   // TST
			case 9:  *p++ = PPC_XOR (res, hRn, PPC_R12); break;   // TEQ
			case 12: *p++ = PPC_OR  (res, hRn, PPC_R12); break;   // ORR
			case 14: *p++ = PPC_ANDC(res, hRn, PPC_R12); break;   // BIC (Rn & ~k)
		}
		if (S) {
			ctx.emitNZ(res);
			if (rot != 0) ctx.emitFlagConst(JITF_C, kCarry);
		}
	} else {
		// arithmetic
		if (aluOp == 5 || aluOp == 6 || aluOp == 7) {
			// ADC / SBC / RSC: preload XER.CA = CPSR.C
			const u8 fC = ctx.readFlag(JITF_C, PPC_R10);
			*p++ = PPC_ADDIC(PPC_R10, fC, -1);
		}
		switch (aluOp) {
			case 2:  *p++ = PPC_SUBFCO(res, PPC_R12, hRn); break;   // SUB  Rn - k
			case 10: *p++ = PPC_SUBFCO(res, PPC_R12, hRn); break;   // CMP
			case 3:  *p++ = PPC_SUBFCO(res, hRn, PPC_R12); break;   // RSB  k - Rn
			case 4:  *p++ = PPC_ADDCO (res, hRn, PPC_R12); break;   // ADD
			case 11: *p++ = PPC_ADDCO (res, hRn, PPC_R12); break;   // CMN
			case 5:  *p++ = PPC_ADDEO (res, hRn, PPC_R12); break;   // ADC
			case 6:  *p++ = PPC_SUBFEO(res, PPC_R12, hRn); break;   // SBC  Rn - k - !C
			case 7:  *p++ = PPC_SUBFEO(res, hRn, PPC_R12); break;   // RSC  k - Rn - !C
		}
		if (S) {
			ctx.emitCVfromXER(PPC_R11 == res ? PPC_R10 : PPC_R11);
			ctx.emitNZ(res);
		}
	}

	if (skip) *skip = PPC_BEQ((u32)((p - skip) * 4));
}

} // namespace

void jitArmEmitOne(JitTraceCtx& ctx, u32 op)
{
	const u8 cond = (u8)(op >> 28);

	if (cond == COND_NV) { ctx.endBlock = true; return; }   // ARMv5 PLD/BLX-imm -> interp

	// B / BL : bits 27..25 == 101
	if ((op & 0x0E000000u) == 0x0A000000u) { emitBranch(ctx, op, cond); return; }

	// Data-processing, immediate operand2 : bits 27..25 == 001
	if ((op & 0x0E000000u) == 0x02000000u) { emitDataProcImm(ctx, op, cond); return; }

	ctx.endBlock = true;   // everything else -> interpreter (later B-groups)
}

#endif // DESMUME_JIT_ARM7
