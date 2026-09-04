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
 * Current scope:
 *   B1 - per-instruction predication wrapper; data-processing with an
 *        immediate operand2 (all 16 ALU ops, S bit); B / BL.
 *   B2 - data-processing with a register operand2 shifted by an immediate
 *        (LSL/LSR/ASR/ROR #n, incl. RRX); the shifter-carry semantics match
 *        arm_instructions.cpp's *_IMM macros exactly. Shift-by-register
 *        (bit4 == 1) is deferred and ends the trace.
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

enum { COND_AL = 0xE, COND_NV = 0xF };

// operand2 result: value in PPC_R12, plus what the barrel shifter did to the
// carry flag (only consumed by the logical ALU ops + MOV/MVN when S is set).
enum ShiftCarry { SC_UNCHANGED, SC_KNOWN, SC_INREG };
struct Op2 { ShiftCarry carry; bool known; };   // value: always PPC_R12; carry reg: always PPC_R10

// ------------------------------------------------------------------ B / BL
// cond .. 101 L[24] offset24         target = PC+8 + signext24(offset)<<2
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
	// (the THUMB Bcc shape). The taken path is a self-contained exit that must
	// not disturb the register/flag cache the fall-through relies on -- use the
	// non-clearing flushes and write LR straight to guest memory.
	ctx.emitEvalCond(cond);
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* skip = p++;                                    // BEQ over the exit

	ctx.emitDirtyFlagFlush();
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
}

// ---------------------------------------------------- operand2 -> PPC_R12
// hRm is valid only for the register form. Returns the carry state; when
// SC_INREG the carry bit (0/1) is left in PPC_R10.
Op2 emitOp2(JitTraceCtx& ctx, u32 op, bool immForm, u8 hRm)
{
	u32*& p = ctx.emitPtr;
	Op2 r = { SC_UNCHANGED, false };

	if (immForm) {
		const u32 rot = ((op >> 8) & 0xF) * 2;
		const u32 k   = ror32(op & 0xFF, rot);
		emitLoadImm32(p, PPC_R12, k);
		if (rot != 0) { r.carry = SC_KNOWN; r.known = (k >> 31) & 1; }
		return r;
	}

	const u8 n    = (op >> 7) & 0x1F;
	const u8 type = (op >> 5) & 3;   // 0 LSL, 1 LSR, 2 ASR, 3 ROR

	switch (type) {
	case 0:  // LSL #n
		if (n == 0) { *p++ = PPC_OR(PPC_R12, hRm, hRm); }       // carry unchanged
		else {
			*p++ = PPC_RLWINM(PPC_R10, hRm, n, 31, 31);          // C = bit(32-n)
			*p++ = PPC_RLWINM(PPC_R12, hRm, n, 0, 31 - n);
			r.carry = SC_INREG;
		}
		break;
	case 1:  // LSR #n   (n==0 == LSR #32)
		if (n == 0) { *p++ = PPC_RLWINM(PPC_R10, hRm, 1, 31, 31); // C = bit31
		              *p++ = PPC_LI(PPC_R12, 0); }
		else { *p++ = PPC_RLWINM(PPC_R10, hRm, (33 - n) & 31, 31, 31); // C = bit(n-1)
		       *p++ = PPC_RLWINM(PPC_R12, hRm, (32 - n) & 31, n, 31); }
		r.carry = SC_INREG;
		break;
	case 2:  // ASR #n   (n==0 == ASR #32)
		if (n == 0) { *p++ = PPC_RLWINM(PPC_R10, hRm, 1, 31, 31); // C = bit31
		              *p++ = PPC_SRAWI(PPC_R12, hRm, 31); }
		else { *p++ = PPC_RLWINM(PPC_R10, hRm, (33 - n) & 31, 31, 31);
		       *p++ = PPC_SRAWI(PPC_R12, hRm, n); }
		r.carry = SC_INREG;
		break;
	default: // ROR #n   (n==0 == RRX)
		if (n == 0) {
			*p++ = PPC_RLWINM(PPC_R11, hRm, 1, 31, 31);          // stash bit0 -> R11
			*p++ = PPC_SRWI(PPC_R12, hRm, 1);
			const u8 fC = ctx.readFlag(JITF_C, PPC_R10);
			*p++ = PPC_RLWIMI(PPC_R12, fC, 31, 0, 0);            // op2 bit31 = old C
			*p++ = PPC_OR(PPC_R10, PPC_R11, PPC_R11);            // C = old bit0
		} else {
			*p++ = PPC_RLWINM(PPC_R10, hRm, (33 - n) & 31, 31, 31);
			*p++ = PPC_RLWINM(PPC_R12, hRm, (32 - n) & 31, 0, 31);
		}
		r.carry = SC_INREG;
		break;
	}
	return r;
}

// --------------------------------------------------- data-processing core
// Shared by the immediate (bit25==1) and register-shifted-by-immediate
// (bit25==0, bit4==0) forms. operand2 is already in PPC_R12 (`o2`).
void emitAlu(JitTraceCtx& ctx, u8 aluOp, bool S, bool testOnly, bool isLogical,
             u8 hRn, u8 hRd, const Op2& o2)
{
	u32*& p = ctx.emitPtr;
	const u8 res = testOnly ? PPC_R11 : hRd;

	if (isLogical) {
		switch (aluOp) {
			case 0: case 8:  *p++ = PPC_AND (res, hRn, PPC_R12); break;   // AND / TST
			case 1: case 9:  *p++ = PPC_XOR (res, hRn, PPC_R12); break;   // EOR / TEQ
			case 12:         *p++ = PPC_OR  (res, hRn, PPC_R12); break;   // ORR
			case 14:         *p++ = PPC_ANDC(res, hRn, PPC_R12); break;   // BIC
			case 13:         *p++ = PPC_OR  (res, PPC_R12, PPC_R12); break; // MOV
			case 15:         *p++ = PPC_NOR (res, PPC_R12, PPC_R12); break; // MVN
		}
		if (S) {
			ctx.emitNZ(res);
			if      (o2.carry == SC_KNOWN) ctx.emitFlagConst(JITF_C, o2.known);
			else if (o2.carry == SC_INREG) ctx.emitFlagBit(JITF_C, PPC_R10, 0);
		}
	} else {
		if (aluOp == 5 || aluOp == 6 || aluOp == 7) {       // ADC/SBC/RSC: XER.CA = C
			const u8 fC = ctx.readFlag(JITF_C, PPC_R10);
			*p++ = PPC_ADDIC(PPC_R10, fC, -1);
		}
		switch (aluOp) {
			case 2: case 10: *p++ = PPC_SUBFCO(res, PPC_R12, hRn); break; // SUB / CMP
			case 3:          *p++ = PPC_SUBFCO(res, hRn, PPC_R12); break; // RSB
			case 4: case 11: *p++ = PPC_ADDCO (res, hRn, PPC_R12); break; // ADD / CMN
			case 5:          *p++ = PPC_ADDEO (res, hRn, PPC_R12); break; // ADC
			case 6:          *p++ = PPC_SUBFEO(res, PPC_R12, hRn); break; // SBC
			case 7:          *p++ = PPC_SUBFEO(res, hRn, PPC_R12); break; // RSC
		}
		if (S) {
			ctx.emitCVfromXER(res == PPC_R11 ? PPC_R10 : PPC_R11);
			ctx.emitNZ(res);
		}
	}
}

// --------------------------------------------------------- data-processing
// cond .. 00 I opcode[24:21] S[20] Rn[19:16] Rd[15:12] <operand2>
//   opcode: 0 AND 1 EOR 2 SUB 3 RSB 4 ADD 5 ADC 6 SBC 7 RSC
//           8 TST 9 TEQ 10 CMP 11 CMN 12 ORR 13 MOV 14 BIC 15 MVN
void emitDataProc(JitTraceCtx& ctx, u32 op, u8 cond)
{
	const bool immForm = (op >> 25) & 1;
	const u8   aluOp   = (op >> 21) & 0xF;
	const bool S       = (op >> 20) & 1;
	const u8   rn      = (op >> 16) & 0xF;
	const u8   rd      = (op >> 12) & 0xF;
	const u8   rm      = op & 0xF;                       // register form only
	const bool predicated = (cond != COND_AL);

	const bool testOnly  = (aluOp >= 8 && aluOp <= 11);
	const bool ignoresRn = (aluOp == 13 || aluOp == 15);
	const bool isLogical = (aluOp <= 1) || (aluOp == 8) || (aluOp == 9) || (aluOp >= 12);

	if (testOnly && !S)  { ctx.endBlock = true; return; }   // MRS/MSR reg/imm -> B6
	if (rd == 15)        { ctx.endBlock = true; return; }   // PC write -> B7

	if (!immForm) {
		// Register form: bit4==1 is either a shift-by-register (deferred) or a
		// multiply / SWP / (signed|half) load-store / BX / CLZ / QADD which are
		// not data-processing at all -- all end the trace here.
		if ((op >> 4) & 1)         { ctx.endBlock = true; return; }
		if (rm == 15)              { ctx.endBlock = true; return; }   // PC operand
		if (rn == 15 && !ignoresRn){ ctx.endBlock = true; return; }
	}

	// The immediate form's ADR / MOV / MVN cases fold to a compile-time constant.
	const u32 rot = ((op >> 8) & 0xF) * 2;
	const u32 k   = ror32(op & 0xFF, rot);
	const bool constResult = immForm &&
		((aluOp == 13) || (aluOp == 15) ||
		 (rn == 15 && !S && (aluOp == 4 || aluOp == 2)));
	if (immForm && rn == 15 && !ignoresRn && !constResult) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	// Pre-allocate every guest register the body touches BEFORE the predication
	// skip (a spill inside the skipped region would write guest memory only on
	// the taken path).
	u8 hRm = 0;
	if (!immForm) hRm = ctx.readReg(rm, lockedMask);
	u8 hRn = 0;
	if (!ignoresRn && rn != 15) hRn = ctx.readReg(rn, lockedMask);
	u8 hRd = 0;
	if (!testOnly) hRd = ctx.writeReg(rd, !predicated, lockedMask);

	u32* skip = nullptr;
	if (predicated) {
		ctx.emitEvalCond(cond);
		*p++ = PPC_CMPWI(0, PPC_R11, 0);
		skip = p++;
	}

	if (constResult) {
		u32 val;
		if      (aluOp == 13) val = k;
		else if (aluOp == 15) val = ~k;
		else if (aluOp == 4)  val = ctx.currentPC + 8 + k;
		else                  val = ctx.currentPC + 8 - k;
		emitLoadImm32(p, hRd, val);
		if (S) {
			ctx.emitFlagConst(JITF_N, (val >> 31) & 1);
			ctx.emitFlagConst(JITF_Z, val == 0);
			if (rot != 0) ctx.emitFlagConst(JITF_C, (k >> 31) & 1);
		}
		if (skip) *skip = PPC_BEQ((u32)((p - skip) * 4));
		return;
	}

	const Op2 o2 = emitOp2(ctx, op, immForm, hRm);
	emitAlu(ctx, aluOp, S, testOnly, isLogical, hRn, hRd, o2);

	if (skip) *skip = PPC_BEQ((u32)((p - skip) * 4));
}

} // namespace

void jitArmEmitOne(JitTraceCtx& ctx, u32 op)
{
	const u8 cond = (u8)(op >> 28);

	if (cond == COND_NV) { ctx.endBlock = true; return; }   // ARMv5 PLD/BLX-imm

	// B / BL : bits 27..25 == 101
	if ((op & 0x0E000000u) == 0x0A000000u) { emitBranch(ctx, op, cond); return; }

	// Data-processing : bits 27..26 == 00 (bit 25 selects imm vs register op2)
	if ((op & 0x0C000000u) == 0x00000000u) { emitDataProc(ctx, op, cond); return; }

	ctx.endBlock = true;   // everything else -> interpreter (later B-groups)
}

#endif // DESMUME_JIT_ARM7
