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
 *        arm_instructions.cpp's *_IMM macros exactly.
 *   B3  - LDR / STR (word / byte), immediate or register(shift-by-imm) offset,
 *        pre/post-index, writeback, unaligned-word-load rotate; pc-relative
 *        literal folds to a constant EA. cond == AL only (predicated ends the
 *        trace).
 *   B3b - extra load/store: LDRH / STRH / LDRSB / LDRSH (imm8 or unshifted
 *        register offset). LDRD / STRD -> B7. Shares emitLoadStoreTail.
 *   B4  - block data transfer: LDM / STM, all four addressing modes
 *        (IA/IB/DA/DB), optional writeback. pc in the register list makes an
 *        LDM a block terminator with ARMv5 bit0 interworking (the common
 *        function-return shape); STM with pc, the S (user-bank / CPSR-restore)
 *        form, an empty list, base == pc, and base-in-list + writeback all
 *        end the trace. cond == AL only.
 *   B5  - multiplies: MUL / MLA (32-bit) and UMULL / SMULL / UMLAL / SMLAL
 *        (64-bit), S bit sets N/Z only. PPC mullw + mulhw/mulhwu; the long
 *        accumulate is an addc/adde pair. cond == AL only; pc operands and the
 *        ARM-unpredictable long-form aliasings end the trace.
 *   B6  - misc: BX / BLX reg (block terminator, ARMv5 bit0 interworking, same
 *        shape as LDM{pc}); SWP / SWPB (ordered load+store at [Rn], word form
 *        rotates); MRS Rd,CPSR (= the packed flags word); MSR CPSR_f (flags
 *        byte only). SPSR forms, any non-flag MSR field, coprocessor, SWI and
 *        BKPT end the trace. cond == AL only.
 *   B7  - ARMv5TE delta, part 1: CLZ (cntlzw), BLX immediate (NV-space, always
 *        ARM->THUMB), PLD (hint, no-op), LDRD / STRD (register pair, two word
 *        accesses).
 *   B7b - ARMv5TE delta, part 2: QADD / QSUB / QDADD / QDSUB with saturation and
 *        the sticky CPSR.Q flag (bit 27); the SM* signed-halfword DSP multiplies
 *        (SMUL / SMLA / SMLAL / SMULW / SMLAW <x><y>). Q on overflow is set via
 *        PPC add/subf XER[OV]; SMLAL replicates OP_SMLAL_*'s exact accumulate.
 *        The B1-B6 PC-writer audit: every pc-writing emitter (data-proc -> pc,
 *        LDR -> pc, extra ld/st -> pc, multiply -> pc, QADD -> pc) ends the
 *        trace; only LDM{pc} (B4) and BX/BLX (B6) compile a pc write, both with
 *        the ARMv5 bit0 interworking branch. LDR{pc} / MOV pc,lr interworking
 *        landed in B7c.
 *   B7c - PC-write interworking: LDR pc,[...] (ARMv5 LDTBit -- bit0 of the
 *        loaded word selects the resume ISA, LDM{pc} shape) and non-S
 *        data-processing writing Rd == 15 (MOV pc,lr / ADD pc,pc,rN -- a plain
 *        ARM-mode jump to the ALU result, no mask, no mode switch, matching
 *        OP_xxx's next_instruction = result path). Both block terminators.
 *        S-form data-proc -> pc (SPSR restore) and any predicated PC write end
 *        the trace.
 *   B6b - data-processing with a register operand2 shifted by a register
 *        (LSL/LSR/ASR/ROR by Rs&0xFF, bit4 == 1 && bit7 == 0). Runtime amount,
 *        so the shifter carry needs runtime branches; the value/carry edges at
 *        0, 32, >32 and (ROR) the &0x1F wrap match arm_instructions.cpp's *_REG
 *        macros exactly. Routed through the shared ALU core. Rn/Rm/Rs == pc
 *        (pipeline+12 read) ends the trace.
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

// -------------------------------------- operand2 = Rm shifted by a register
// The register-specified shift form (bit4 == 1, bit7 == 0). The shift amount is
// the low byte of Rs, read at runtime, so the shifter carry needs runtime
// branches: the value/carry edges at amount 0, 32, >32 and (ROR) the &0x1F wrap
// reproduce arm_instructions.cpp's LSL/LSR/ASR/ROR _REG macros exactly. Value ->
// PPC_R12; when wantCarry, the shifter carry (0/1) -> PPC_R10. Clobbers R8/R11.
Op2 emitOp2ShiftReg(JitTraceCtx& ctx, u32 op, u8 hRm, u8 hRs, bool wantCarry)
{
	u32*& p = ctx.emitPtr;
	const u8 type = (op >> 5) & 3;   // 0 LSL, 1 LSR, 2 ASR, 3 ROR
	Op2 r = { wantCarry ? SC_INREG : SC_UNCHANGED, false };

	if (wantCarry) ctx.ensureFlagsLoaded();          // force the CPSR load onto the linear path
	*p++ = PPC_RLWINM(PPC_R11, hRs, 0, 24, 31);      // sh = Rs & 0xFF

	if (!wantCarry) {
		switch (type) {
		case 0:      // LSL: sh >= 32 -> 0
		case 1: {    // LSR: sh >= 32 -> 0
			*p++ = PPC_ANDI_(PPC_R8, PPC_R11, 0xE0);
			u32* toZero = p++;                                  // BNE -> zero
			*p++ = (type == 0) ? PPC_SLW(PPC_R12, hRm, PPC_R11)
			                   : PPC_SRW(PPC_R12, hRm, PPC_R11);
			u32* done = p++;                                    // B -> done
			*toZero = PPC_BNE((u32)((p - toZero) * 4));
			*p++ = PPC_LI(PPC_R12, 0);
			*done = PPC_B((u32)((p - done) * 4));
			break;
		}
		case 2: {    // ASR: SRAW covers 0..63 incl. sign-fill >= 32; clamp sh >= 32 -> 31
			*p++ = PPC_ANDI_(PPC_R8, PPC_R11, 0xE0);
			u32* skip = p++;                                    // BEQ -> shift
			*p++ = PPC_LI(PPC_R11, 31);
			*skip = PPC_BEQ((u32)((p - skip) * 4));
			*p++ = PPC_SRAW(PPC_R12, hRm, PPC_R11);
			break;
		}
		default: {   // ROR: (sh & 0x1F) == 0 -> identity, else ROR(Rm, sh & 0x1F)
			*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 0, 27, 31);     // sh & 0x1F
			*p++ = PPC_CMPWI(0, PPC_R11, 0);
			u32* ident = p++;                                   // BEQ -> identity
			*p++ = PPC_SUBFIC(PPC_R8, PPC_R11, 32);             // 32 - k
			*p++ = PPC_RLWNM(PPC_R12, hRm, PPC_R8, 0, 31);      // ROL(Rm, 32-k) == ROR(Rm, k)
			u32* done = p++;                                    // B -> done
			*ident = PPC_BEQ((u32)((p - ident) * 4));
			*p++ = PPC_OR(PPC_R12, hRm, hRm);
			*done = PPC_B((u32)((p - done) * 4));
			break;
		}
		}
		return r;
	}

	// ---- carry-producing (S && logical). sh == 0 -> value = Rm, carry = old C ----
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* nz = p++;                                              // BNE -> nonzero
	*p++ = PPC_OR(PPC_R12, hRm, hRm);
	*p++ = PPC_EXTRACT_FLAG_BIT(PPC_R10, JITF_C);
	u32* d0 = p++;                                              // B -> done
	*nz = PPC_BNE((u32)((p - nz) * 4));

	switch (type) {
	case 0: {    // S_LSL_REG
		*p++ = PPC_CMPWI(0, PPC_R11, 32);
		u32* bGt = p++;                                         // BGT -> above32
		u32* bEq = p++;                                         // BEQ -> eq32
		// 1..31: val = Rm << sh ; c = bit(32-sh) = (Rm >> (32-sh)) & 1
		*p++ = PPC_SLW(PPC_R12, hRm, PPC_R11);
		*p++ = PPC_SUBFIC(PPC_R8, PPC_R11, 32);
		*p++ = PPC_SRW(PPC_R10, hRm, PPC_R8);
		*p++ = PPC_RLWINM(PPC_R10, PPC_R10, 0, 31, 31);
		u32* d1 = p++;                                          // B -> done
		*bEq = PPC_BEQ((u32)((p - bEq) * 4));
		// sh == 32: val = 0 ; c = bit0(Rm)
		*p++ = PPC_LI(PPC_R12, 0);
		*p++ = PPC_RLWINM(PPC_R10, hRm, 0, 31, 31);
		u32* d2 = p++;                                          // B -> done
		*bGt = PPC_BGT((u32)((p - bGt) * 4));
		// sh > 32: val = 0 ; c = 0
		*p++ = PPC_LI(PPC_R12, 0);
		*p++ = PPC_LI(PPC_R10, 0);
		*d1 = PPC_B((u32)((p - d1) * 4));
		*d2 = PPC_B((u32)((p - d2) * 4));
		break;
	}
	case 1: {    // S_LSR_REG
		*p++ = PPC_CMPWI(0, PPC_R11, 32);
		u32* bGt = p++;                                         // BGT -> above32
		u32* bEq = p++;                                         // BEQ -> eq32
		// 1..31: val = Rm >> sh ; c = bit(sh-1)
		*p++ = PPC_SRW(PPC_R12, hRm, PPC_R11);
		*p++ = PPC_ADDI(PPC_R8, PPC_R11, -1);
		*p++ = PPC_SRW(PPC_R10, hRm, PPC_R8);
		*p++ = PPC_RLWINM(PPC_R10, PPC_R10, 0, 31, 31);
		u32* d1 = p++;                                          // B -> done
		*bEq = PPC_BEQ((u32)((p - bEq) * 4));
		// sh == 32: val = 0 ; c = bit31(Rm)
		*p++ = PPC_LI(PPC_R12, 0);
		*p++ = PPC_RLWINM(PPC_R10, hRm, 1, 31, 31);
		u32* d2 = p++;                                          // B -> done
		*bGt = PPC_BGT((u32)((p - bGt) * 4));
		// sh > 32: val = 0 ; c = 0
		*p++ = PPC_LI(PPC_R12, 0);
		*p++ = PPC_LI(PPC_R10, 0);
		*d1 = PPC_B((u32)((p - d1) * 4));
		*d2 = PPC_B((u32)((p - d2) * 4));
		break;
	}
	case 2: {    // S_ASR_REG
		*p++ = PPC_CMPWI(0, PPC_R11, 32);
		u32* bGe = p++;                                         // BGE -> ge32
		// 1..31: val = (s32)Rm >> sh ; c = bit(sh-1)
		*p++ = PPC_SRAW(PPC_R12, hRm, PPC_R11);
		*p++ = PPC_ADDI(PPC_R8, PPC_R11, -1);
		*p++ = PPC_SRW(PPC_R10, hRm, PPC_R8);
		*p++ = PPC_RLWINM(PPC_R10, PPC_R10, 0, 31, 31);
		u32* d1 = p++;                                          // B -> done
		*bGe = PPC_BGE((u32)((p - bGe) * 4));
		// sh >= 32: val = sign-fill ; c = bit31(Rm)
		*p++ = PPC_SRAWI(PPC_R12, hRm, 31);
		*p++ = PPC_RLWINM(PPC_R10, hRm, 1, 31, 31);
		*d1 = PPC_B((u32)((p - d1) * 4));
		break;
	}
	default: {   // S_ROR_REG : sh != 0 here
		// sh5 = sh & 0x1F ; sh5 == 0 -> val = Rm, c = bit31(Rm)
		//                   else     -> val = ROR(Rm, sh5), c = bit(sh5-1)
		*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 0, 27, 31);
		*p++ = PPC_CMPWI(0, PPC_R11, 0);
		u32* rot = p++;                                         // BNE -> rot
		*p++ = PPC_OR(PPC_R12, hRm, hRm);
		*p++ = PPC_RLWINM(PPC_R10, hRm, 1, 31, 31);
		u32* d1 = p++;                                          // B -> done
		*rot = PPC_BNE((u32)((p - rot) * 4));
		*p++ = PPC_SUBFIC(PPC_R8, PPC_R11, 32);
		*p++ = PPC_RLWNM(PPC_R12, hRm, PPC_R8, 0, 31);
		*p++ = PPC_ADDI(PPC_R8, PPC_R11, -1);
		*p++ = PPC_SRW(PPC_R10, hRm, PPC_R8);
		*p++ = PPC_RLWINM(PPC_R10, PPC_R10, 0, 31, 31);
		*d1 = PPC_B((u32)((p - d1) * 4));
		break;
	}
	}
	*d0 = PPC_B((u32)((p - d0) * 4));
	return r;
}

// ---------------------------------------------------- operand2 -> PPC_R12
// hRm/hRs are valid only for the register form. Returns the carry state; when
// SC_INREG the carry bit (0/1) is left in PPC_R10.
Op2 emitOp2(JitTraceCtx& ctx, u32 op, bool immForm, u8 hRm, u8 hRs, bool wantCarry)
{
	u32*& p = ctx.emitPtr;
	Op2 r = { SC_UNCHANGED, false };

	if (!immForm && ((op >> 4) & 1))                 // register-specified shift
		return emitOp2ShiftReg(ctx, op, hRm, hRs, wantCarry);

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

// ------------------------------------------------- data-processing -> PC (B7c)
// A non-S data-processing op with Rd == 15: OP_xxx's `next_instruction = result`
// path -- a plain ARM-mode jump to the ALU result, with NO word-align and NO
// mode switch (unlike LDR pc / BX). The S-form (SPSR -> CPSR exception return)
// and any predicated form bail at the dispatch site. Covers `MOV pc,lr`,
// `ADD pc,pc,rN` jump tables and the like -- a real block-length win in
// function-return-heavy ARM code. Block terminator.
void emitDataProcToPc(JitTraceCtx& ctx, u32 op)
{
	const bool immForm   = (op >> 25) & 1;
	const u8   aluOp     = (op >> 21) & 0xF;
	const u8   rn        = (op >> 16) & 0xF;
	const u8   rm        = op & 0xF;
	const u8   rs        = (op >> 8) & 0xF;
	const bool ignoresRn = (aluOp == 13 || aluOp == 15);
	const bool isLogical = (aluOp <= 1) || (aluOp >= 12);
	const bool regShift  = !immForm && ((op >> 4) & 1) && !((op >> 7) & 1);

	if (!immForm) {
		if (((op >> 4) & 1) && ((op >> 7) & 1)) { ctx.endBlock = true; return; }  // not a DP encoding
		if (rm == 15)                           { ctx.endBlock = true; return; }
		if (!ignoresRn && rn == 15)             { ctx.endBlock = true; return; }
		if (regShift && rs == 15)               { ctx.endBlock = true; return; }
	}

	const u32 rot = ((op >> 8) & 0xF) * 2;
	const u32 k   = ror32(op & 0xFF, rot);

	ctx.ensureArena();
	u32 lockedMask = 0;

	// compile-time-constant target (MOV/MVN #imm, ADD/SUB pc,#imm) -> static
	// exit so the block can chain to the successor.
	if (immForm && (aluOp == 13 || aluOp == 15 ||
	                (rn == 15 && (aluOp == 2 || aluOp == 4)))) {
		u32 val;
		if      (aluOp == 13) val = k;
		else if (aluOp == 15) val = ~k;
		else if (aluOp == 4)  val = ctx.currentPC + 8 + k;
		else                  val = ctx.currentPC + 8 - k;
		ctx.emitStaticExit(val, ctx.instrCount + 1, 3);         // OP_xxx(_,3)
		ctx.instrCount++;
		ctx.currentPC += 4;
		ctx.endBlock = true;
		ctx.blockTerminatedEarly = true;
		return;
	}
	if (immForm && rn == 15 && !ignoresRn) { ctx.endBlock = true; return; }

	u8 hRm = 0;
	if (!immForm)   hRm = ctx.readReg(rm, lockedMask);
	u8 hRs = 0;
	if (regShift)   hRs = ctx.readReg(rs, lockedMask);
	u8 hRn = 0;
	if (!ignoresRn) hRn = ctx.readReg(rn, lockedMask);

	const Op2 o2 = emitOp2(ctx, op, immForm, hRm, hRs, /*wantCarry=*/false);
	emitAlu(ctx, aluOp, /*S=*/false, /*testOnly=*/false, isLogical, hRn, PPC_R12, o2);

	// result in PPC_R12 -> dynamic exit (no mask, no T change: stays ARM)
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForArm(op), /*targetThumb=*/false);
	ctx.instrCount++;
	ctx.currentPC += 4;
	ctx.endBlock = true;
	ctx.blockTerminatedEarly = true;
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
	if (rd == 15) {
		// ARMv5 PC write. S-form (MOVS/SUBS pc,...) restores CPSR from SPSR --
		// an exception return, interpreter only. A predicated PC write is a
		// conditional-branch shape (deferred). The plain non-S form is a
		// straight ARM-mode jump to the ALU result (B7c).
		if (S || predicated) { ctx.endBlock = true; return; }
		emitDataProcToPc(ctx, op);
		return;
	}

	const bool regShift = !immForm && ((op >> 4) & 1) && !((op >> 7) & 1);
	const u8   rs       = (op >> 8) & 0xF;

	if (!immForm) {
		// bit4 == 1 && bit7 == 1 is the multiply / SWP / (signed|half) load-store
		// / BX / CLZ / QADD encoding space -- routed before here, so a stray one
		// ends the trace. bit4 == 1 && bit7 == 0 is a shift-by-register (B6b).
		if (((op >> 4) & 1) && ((op >> 7) & 1)) { ctx.endBlock = true; return; }
		if (rm == 15)                           { ctx.endBlock = true; return; }   // PC operand
		if (rn == 15 && !ignoresRn)             { ctx.endBlock = true; return; }
		// shift-by-register reading pc: the interpreter sees pipeline + 12 for a
		// pc operand of a register-shifted instruction. Rare / UNPREDICTABLE -> bail.
		if (regShift && rs == 15)               { ctx.endBlock = true; return; }
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
	u8 hRs = 0;
	if (regShift) hRs = ctx.readReg(rs, lockedMask);
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

	const Op2 o2 = emitOp2(ctx, op, immForm, hRm, hRs, S && isLogical);
	emitAlu(ctx, aluOp, S, testOnly, isLogical, hRn, hRd, o2);

	if (skip) *skip = PPC_BEQ((u32)((p - skip) * 4));
}

// The shared load/store tail: EA is already in PPC_R11, the writeback value (if
// any) in PPC_R10, and the store source pre-read into hVal. Stashes them on the
// host stack, flushes state, does the slow C-call access (+ optional word
// rotate / sign-extend), then writes the result and the Rn writeback straight
// to guest memory -- writeback committed AFTER the access so a store's SMC
// guard bail is a clean interpreter re-run (no double-writeback).
void emitLoadStoreTail(JitTraceCtx& ctx, u8 hVal, u32 size, bool isLoad,
                       bool signExt, bool wordRotate, bool writeback, u8 rn, u8 rd)
{
	u32*& p = ctx.emitPtr;
	*p++ = PPC_STW(PPC_R11, 1, 96);                 // EA
	if (writeback) *p++ = PPC_STW(PPC_R10, 1, 104); // WB
	if (!isLoad)   *p++ = PPC_STW(hVal,   1, 100);  // store value

	ctx.emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);

	if (isLoad) {
		ctx.emitSlowLoad(PPC_R10, PPC_R12, size, signExt);
		if (wordRotate) {                              // ROR(R10, 8*(EA&3))
			*p++ = PPC_LWZ(PPC_R12, 1, 96);
			*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31); // EA & 3
			*p++ = PPC_LI(PPC_R11, 4);
			*p++ = PPC_SUBF(PPC_R12, PPC_R12, PPC_R11);     // 4 - (EA&3)
			*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28); // ((4-x)&3)<<3 in {0,24,16,8}
			*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
		}
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		if (writeback) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }
		*p++ = PPC_STW(PPC_R10, 14, rd * 4);        // result last: wins if rd == rn
	} else {
		ctx.emitSmcCheckAndBail(PPC_R12);
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_LWZ(PPC_R10, 1, 100);
		ctx.emitSlowStore(PPC_R12, PPC_R10, size);
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		if (writeback) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }
	}
}

// --------------------------------------------------- LDR -> PC interwork (B7c)
// A word LDR with Rd == 15. `valReg` holds the already-rotated loaded word. The
// ARM9 (LDTBit == 1) path of OP_LDR: CPSR.T = bit0(word) ; R15 = word & ~1. Same
// shape as the LDM{...,pc} exit -- bit0 selects the resume ISA, T is set in the
// packed flags on the THUMB path so the C++ resume uses 16-bit pipeline math.
// Block terminator; caller has run the memory epilogue, invalidated the reg
// cache and committed any base writeback.
void emitLdrPcExit(JitTraceCtx& ctx, u8 valReg, u32 op)
{
	u32*& p = ctx.emitPtr;
	const u32 term = ctx.cpu.cyclesForArm(op);

	*p++ = PPC_OR(PPC_R12, valReg, valReg);              // capture before any flush
	*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);      // R11 = bit0 (mode select)
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* toArm = p++;                                     // BEQ -> stay ARM

	// bit0 == 1: ARM -> THUMB. Set CPSR.T so the resume path fetches 16-bit.
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);       // word & ~1
	ctx.ensureFlagsLoaded();
	*p++ = PPC_LI(PPC_R10, 0x20);                         // CPSR.T (bit 5)
	*p++ = PPC_OR(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
	ctx.flagsDirty = true;
	ctx.flushDirtyFlags();
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/true);

	*toArm = PPC_BEQ((u32)((p - toArm) * 4));
	// bit0 == 0: stay ARM. OP_LDR still masks R15 &= 0xFFFFFFFE.
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);       // word & ~1
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/false);

	ctx.instrCount++;
	ctx.currentPC += 4;
	ctx.endBlock = true;
	ctx.blockTerminatedEarly = true;
}

// LDR into PC, general (non-literal) form: the load half of emitLoadStoreTail
// (word access, always rotate) but the loaded word interworks instead of being
// written to a GPR slot. EA in PPC_R11, writeback value (if any) in PPC_R10.
void emitLoadPcTail(JitTraceCtx& ctx, bool writeback, u8 rn, u32 op)
{
	u32*& p = ctx.emitPtr;
	*p++ = PPC_STW(PPC_R11, 1, 96);                 // EA
	if (writeback) *p++ = PPC_STW(PPC_R10, 1, 104); // WB

	ctx.emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
	*p++ = PPC_LWZ(PPC_R12, 1, 96);                 // ROR(loaded, 8*(EA&3))
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);
	*p++ = PPC_LI(PPC_R11, 4);
	*p++ = PPC_SUBF(PPC_R12, PPC_R12, PPC_R11);
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);
	*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
	ctx.emitMemEpilogue();
	ctx.invalidateRegCache();
	if (writeback) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }

	emitLdrPcExit(ctx, PPC_R10, op);
}

// -------------------------------------------------- LDR / STR (word / byte)
// cond 01 I P U B W L Rn Rd <offset>
//   I : 0 imm12 offset, 1 register offset shifted by an immediate (bit4 == 0)
//   P : 1 pre-index, 0 post-index (post always writes back; P0+W1 = LDRT/STRT)
//   U : add / subtract    B : byte / word    W : writeback    L : load / store
// Word loads rotate for an unaligned EA (ROR by 8*(EA&3)) -- matches OP_LDR's
// `ROR(READ32(adr), 8*(adr&3))`. Stores and byte accesses do not. Predicated
// transfers end the trace (B1-B3: cond == AL only) -- the memory prologue's
// state flush + reg-cache invalidation are awkward to make conditional.
void emitSingleDataTransfer(JitTraceCtx& ctx, u32 op)
{
	const bool I = (op >> 25) & 1;
	const bool P = (op >> 24) & 1;
	const bool U = (op >> 23) & 1;
	const bool B = (op >> 22) & 1;
	const bool W = (op >> 21) & 1;
	const bool L = (op >> 20) & 1;
	const u8   rn = (op >> 16) & 0xF;
	const u8   rd = (op >> 12) & 0xF;
	const u32  size = B ? 1u : 4u;
	const bool writeback = (!P) || W;

	if (!P && W)              { ctx.endBlock = true; return; }   // LDRT / STRT
	if (rd == 15 && (!L || B)){ ctx.endBlock = true; return; }   // STR pc / LDRB pc -> interp
	if (I && ((op >> 4) & 1)) { ctx.endBlock = true; return; }   // undefined
	// LDR pc (word): a block terminator with ARMv5 LDTBit interworking (B7c),
	// handled after EA computation via emitLdrPcExit / emitLoadPcTail.

	u32*& p = ctx.emitPtr;
	const s32 immOff = U ? (s32)(op & 0xFFF) : -(s32)(op & 0xFFF);

	// ---- pc-relative literal: [pc, #imm], I=0 P=1 W=0 -> EA is constant ----
	if (rn == 15) {
		if (I || W || !P) { ctx.endBlock = true; return; }
		const u32 ea = ctx.currentPC + 8 + (u32)immOff;
		ctx.ensureArena();
		ctx.emitMemPrologue();
		if (L) {
			emitLoadImm32(p, PPC_R12, ea);
			ctx.emitSlowLoad(PPC_R10, PPC_R12, size, false);
			if (size == 4 && (ea & 3)) {
				const u32 rl = (32u - 8u * (ea & 3)) & 31;
				*p++ = PPC_RLWINM(PPC_R10, PPC_R10, rl, 0, 31);
			}
			ctx.emitMemEpilogue();
			ctx.invalidateRegCache();
			if (rd == 15) { emitLdrPcExit(ctx, PPC_R10, op); return; }   // LDR pc,[pc,#imm]
			*p++ = PPC_STW(PPC_R10, 14, rd * 4);
		} else {
			emitLoadImm32(p, PPC_R12, ea);
			*p++ = PPC_STW(PPC_R12, 1, 96);
			ctx.emitSmcCheckAndBail(PPC_R12);
			*p++ = PPC_LWZ(PPC_R12, 1, 96);
			*p++ = PPC_LWZ(PPC_R10, 14, rd * 4);
			ctx.emitSlowStore(PPC_R12, PPC_R10, size);
			ctx.emitMemEpilogue();
			ctx.invalidateRegCache();
		}
		return;
	}

	ctx.ensureArena();
	u32 lockedMask = 0;
	const u8 rm = op & 0xF;
	if (I && rm == 15) { ctx.endBlock = true; return; }

	const u8 hRn = ctx.readReg(rn, lockedMask);
	u8 hRm = 0;
	if (I) hRm = ctx.readReg(rm, lockedMask);
	u8 hVal = 0;
	if (!L) hVal = ctx.readReg(rd, lockedMask);

	// offset -> PPC_R12 (register form); the immediate form folds into ADDI
	if (I) (void)emitOp2(ctx, op, /*immForm=*/false, hRm, /*hRs=*/0, /*wantCarry=*/false);

	// EA (access address) -> R11 ; WB (writeback into Rn) -> R10.
	// register offset in R12: EA = U ? Rn + R12 : Rn - R12  (SUBF rD,rA,rB = rB-rA)
	if (P) {                                   // pre-index
		if      (!I) *p++ = PPC_ADDI(PPC_R11, hRn, immOff);
		else if (U)  *p++ = PPC_ADD (PPC_R11, hRn, PPC_R12);
		else         *p++ = PPC_SUBF(PPC_R11, PPC_R12, hRn);
		if (writeback) *p++ = PPC_OR(PPC_R10, PPC_R11, PPC_R11);
	} else {                                   // post-index (always writes back)
		*p++ = PPC_OR(PPC_R11, hRn, hRn);
		if      (!I) *p++ = PPC_ADDI(PPC_R10, hRn, immOff);
		else if (U)  *p++ = PPC_ADD (PPC_R10, hRn, PPC_R12);
		else         *p++ = PPC_SUBF(PPC_R10, PPC_R12, hRn);
	}

	if (rd == 15) { emitLoadPcTail(ctx, writeback, rn, op); return; }   // LDR pc (B7c)

	emitLoadStoreTail(ctx, hVal, size, L, /*signExt=*/false,
	                  /*wordRotate=*/(L && size == 4), writeback, rn, rd);
}

// ------------------------------------------------ extra load/store (B3b)
// cond 000 P U I W L Rn Rd hi 1 SH 1 lo   (bits 27..25 == 000, bit7 == 1, bit4 == 1)
//   SH: 01 unsigned halfword, 10 signed byte, 11 signed halfword (00 = SWP)
//   I : 1 immediate offset (imm8 = hi<<4 | lo), 0 register offset (Rm = lo, no shift)
// L=0 SH=10/11 is LDRD/STRD (ARMv5E) -> deferred to B7. No unaligned rotate.
void emitExtraDataTransfer(JitTraceCtx& ctx, u32 op)
{
	const bool P = (op >> 24) & 1;
	const bool U = (op >> 23) & 1;
	const bool I = (op >> 22) & 1;
	const bool W = (op >> 21) & 1;
	const bool L = (op >> 20) & 1;
	const u8   sh = (op >> 5) & 3;
	const u8   rn = (op >> 16) & 0xF;
	const u8   rd = (op >> 12) & 0xF;
	const bool writeback = (!P) || W;

	const u8 rm = op & 0xF;
	if (!P && W)                     { ctx.endBlock = true; return; }  // translated access
	if (rd == 15 || rn == 15)        { ctx.endBlock = true; return; }
	if (!L && (sh == 2 || sh == 3))  { ctx.endBlock = true; return; }  // LDRD/STRD -> B7
	if (!I && rm == 15)              { ctx.endBlock = true; return; }

	const bool signExt = L && (sh == 2 || sh == 3);
	const u32  size     = (sh == 2) ? 1u : 2u;                  // signed byte : halfword
	const s32  immOff   = (s32)(((op >> 4) & 0xF0) | (op & 0xF));
	const s32  soff     = U ? immOff : -immOff;

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRn = ctx.readReg(rn, lockedMask);
	u8 hRm = 0;
	if (!I) hRm = ctx.readReg(rm, lockedMask);
	u8 hVal = 0;
	if (!L) hVal = ctx.readReg(rd, lockedMask);

	// EA -> R11, WB -> R10 (register offset is unshifted: EA = U ? Rn+Rm : Rn-Rm)
	if (P) {
		if      (I) *p++ = PPC_ADDI(PPC_R11, hRn, soff);
		else if (U) *p++ = PPC_ADD (PPC_R11, hRn, hRm);
		else        *p++ = PPC_SUBF(PPC_R11, hRm, hRn);
		if (writeback) *p++ = PPC_OR(PPC_R10, PPC_R11, PPC_R11);
	} else {
		*p++ = PPC_OR(PPC_R11, hRn, hRn);
		if      (I) *p++ = PPC_ADDI(PPC_R10, hRn, soff);
		else if (U) *p++ = PPC_ADD (PPC_R10, hRn, hRm);
		else        *p++ = PPC_SUBF(PPC_R10, hRm, hRn);
	}

	emitLoadStoreTail(ctx, hVal, size, L, signExt, /*wordRotate=*/false, writeback, rn, rd);
}

// ------------------------------------------------ block data transfer (B4)
// cond 100 P U S W L Rn register_list[16]
//   P : 1 pre / 0 post   U : 1 increment / 0 decrement   W : writeback
//   S : PSR / force-user-bank -> interpreter (B6)         L : load / store
// The touched addresses are one contiguous ascending block of `n` words no
// matter the direction; the lowest-numbered register always maps to the lowest
// address (register i -> lowAddr + 4*slot). We always walk the list ascending;
// the interpreter walks DA/DB descending (highest address first), so the access
// *order* can differ -- immaterial for RAM (same addresses, same values, same
// final state) and I/O-bank accesses are already flagged untrusted by the
// harness. Word loads do NOT rotate an unaligned base (unlike the single LDR) --
// OP_L_IA
// passes the raw address to READ32 and _MMU_read32 masks it. Writeback value is
// base +/- 4*n regardless of P. `pc` in an LDM list is a BX-style terminator on
// ARMv5 (cpu->LDTBit): loaded_value bit0 selects the resume mode.
void emitBlockDataTransfer(JitTraceCtx& ctx, u32 op)
{
	const bool P = (op >> 24) & 1;
	const bool U = (op >> 23) & 1;
	const bool S = (op >> 22) & 1;
	const bool W = (op >> 21) & 1;
	const bool L = (op >> 20) & 1;
	const u8   rn   = (op >> 16) & 0xF;
	const u32  list = op & 0xFFFF;
	const u32  n    = (u32)__builtin_popcount(list);
	const bool pcInList = (list & 0x8000u) != 0;

	if (S)                         { ctx.endBlock = true; return; }  // user-bank / SPSR -> B6
	if (n == 0)                    { ctx.endBlock = true; return; }  // empty list -> interp
	if (rn == 15)                  { ctx.endBlock = true; return; }  // base = pc, unpredictable
	if (W && (list & (1u << rn)))  { ctx.endBlock = true; return; }  // base in list + WB -> interp
	if (pcInList && !L)            { ctx.endBlock = true; return; }  // STM{pc} (rare) -> interp

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRn = ctx.readReg(rn, lockedMask);

	// low address of the contiguous block -> slot 96
	//   IA base | IB base+4 | DA base-4*(n-1) | DB base-4*n
	const s32 lowOff = U ? (P ? 4 : 0)
	                     : (P ? -(s32)(4 * n) : -(s32)(4 * (n - 1)));
	if (lowOff) *p++ = PPC_ADDI(PPC_R12, hRn, lowOff);
	else        *p++ = PPC_OR  (PPC_R12, hRn, hRn);
	*p++ = PPC_STW(PPC_R12, 1, 96);

	// writeback value (base +/- 4*n) -> slot 104; committed to guest memory only
	// AFTER the access so a store's SMC-guard bail re-runs the whole LDM/STM.
	if (W) {
		*p++ = PPC_ADDI(PPC_R12, hRn, U ? (s32)(4 * n) : -(s32)(4 * n));
		*p++ = PPC_STW(PPC_R12, 1, 104);
	}

	ctx.emitMemPrologue();
	if (!L) { *p++ = PPC_LWZ(PPC_R12, 1, 96); ctx.emitSmcCheckAndBail(PPC_R12); }

	// r0..r14 in ascending order at lowAddr + 4*slot
	u32 slot = 0;
	for (int i = 0; i < 15; i++) {
		if (!(list & (1u << i))) continue;
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		if (slot) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));
		if (L) {
			ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
			*p++ = PPC_STW(PPC_R10, 14, i * 4);
		} else {
			*p++ = PPC_LWZ(PPC_R10, 14, i * 4);
			ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
		}
		slot++;
	}

	if (!pcInList) {
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		if (W) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }
		return;                                 // not a terminator: block continues
	}

	// ---- LDM{...,pc}: load the top word and interwork (ARMv5 LDTBit) ----
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	if (slot) *p++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));   // pc slot == n-1
	ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
	*p++ = PPC_STW(PPC_R10, 1, 100);                                // stash raw popped pc
	ctx.emitMemEpilogue();
	ctx.invalidateRegCache();
	if (W) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }

	const u32 term = ctx.cpu.cyclesForArm(op);
	*p++ = PPC_LWZ(PPC_R12, 1, 100);                    // raw popped pc
	*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);     // R11 = bit0 (mode select)
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* toArm = p++;                                    // BEQ -> stay ARM (bit0 == 0)

	// bit0 == 1: switch to THUMB -- set CPSR.T so the C++ resume path uses
	// 16-bit fetch/pipeline math (the ARM7 POP{pc} bug: dropping this mode
	// switch misdecodes the target and free-runs).
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);      // & ~1
	ctx.ensureFlagsLoaded();
	*p++ = PPC_LI(PPC_R10, 0x20);                        // CPSR.T (bit 5)
	*p++ = PPC_OR(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
	ctx.flagsDirty = true;
	ctx.flushDirtyFlags();
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/true);

	*toArm = PPC_BEQ((u32)((p - toArm) * 4));
	// bit0 == 0: stay ARM (CPSR.T already 0)
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 29);      // & ~3
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/false);

	ctx.instrCount++;
	ctx.currentPC += 4;
	ctx.endBlock = true;
	ctx.blockTerminatedEarly = true;
}

// -------------------------------------------------------------- multiply (B5)
// short : cond 0000 00 A S  Rd   Rn   Rs 1001 Rm      Rd = Rm*Rs (+Rn if A)
// long  : cond 0000 1U A S  RdHi RdLo Rs 1001 Rm      {RdHi:RdLo} = Rm*Rs (+acc)
//   U : 1 signed (SMULL/SMLAL) / 0 unsigned (UMULL/UMLAL) ; A : accumulate
// PPC mullw gives the low 32; mulhw/mulhwu the high 32. The long accumulate is
// addc/adde on the {lo,hi} pair -- matches the interpreter's `RdHi = hiprod +
// RdHi + CarryFrom(loprod, RdLo); RdLo += loprod`. S sets N/Z only (C, V are
// left untouched, as in OP_MUL_S / OP_UMULL_S): short from Rd, long from the
// 64-bit result (Z = both halves zero). Cycle cost is data-dependent on Rs in
// the interpreter; the profile uses a fixed per-form mid estimate.
//
// Bails (end the trace, interpreter takes it): any operand or destination == pc;
// long form with RdHi == RdLo or Rm aliasing either destination (ARM-unpredictable);
// predicated (handled at the dispatch site, cond == AL only).
void emitMultiply(JitTraceCtx& ctx, u32 op)
{
	const bool longForm = (op >> 23) & 1;
	const bool sign     = (op >> 22) & 1;   // long form only
	const bool accum    = (op >> 21) & 1;
	const bool S        = (op >> 20) & 1;
	const u8   rs = (op >> 8) & 0xF;
	const u8   rm = op & 0xF;

	if (rm == 15 || rs == 15) { ctx.endBlock = true; return; }

	if (!longForm) {
		const u8 rd = (op >> 16) & 0xF;
		const u8 rn = (op >> 12) & 0xF;      // accumulator operand (MLA)
		if (rd == 15 || (accum && rn == 15)) { ctx.endBlock = true; return; }

		ctx.ensureArena();
		u32*& p = ctx.emitPtr;
		u32 lockedMask = 0;

		const u8 hRm = ctx.readReg(rm, lockedMask);
		const u8 hRs = ctx.readReg(rs, lockedMask);
		u8 hRn = 0;
		if (accum) hRn = ctx.readReg(rn, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);

		*p++ = PPC_MULLW(PPC_R12, hRm, hRs);
		if (accum) *p++ = PPC_ADD(hRd, PPC_R12, hRn);
		else       *p++ = PPC_OR (hRd, PPC_R12, PPC_R12);
		if (S) ctx.emitNZ(hRd);
		return;
	}

	const u8 rdhi = (op >> 16) & 0xF;
	const u8 rdlo = (op >> 12) & 0xF;
	if (rdhi == 15 || rdlo == 15)           { ctx.endBlock = true; return; }
	if (rdhi == rdlo)                       { ctx.endBlock = true; return; }
	if (rm == rdhi || rm == rdlo)           { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRm = ctx.readReg(rm, lockedMask);
	const u8 hRs = ctx.readReg(rs, lockedMask);
	const u8 hLo = ctx.writeReg(rdlo, !accum, lockedMask);
	const u8 hHi = ctx.writeReg(rdhi, !accum, lockedMask);

	*p++ = PPC_MULLW(PPC_R11, hRm, hRs);                        // low 32
	*p++ = sign ? PPC_MULHW(PPC_R12, hRm, hRs)
	            : PPC_MULHWU(PPC_R12, hRm, hRs);                // high 32

	if (accum) {
		*p++ = PPC_ADDCO(hLo, PPC_R11, hLo);                    // RdLo += loprod ; XER[CA]
		*p++ = PPC_ADDEO(hHi, PPC_R12, hHi);                    // RdHi += hiprod + CA
	} else {
		*p++ = PPC_OR(hLo, PPC_R11, PPC_R11);
		*p++ = PPC_OR(hHi, PPC_R12, PPC_R12);
	}

	if (S) {
		*p++ = PPC_OR(PPC_R8, hHi, hLo);                        // Z = (RdHi | RdLo) == 0
		ctx.emitFlagBit(JITF_N, hHi, 1);
		*p++ = PPC_CNTLZW(PPC_R8, PPC_R8);
		ctx.emitFlagBit(JITF_Z, PPC_R8, 27);
	}
}

// ---------------------------------------------------------- BX / BLX reg (B6)
// cond 0001 0010 1111 1111 1111 00L1 Rm   (L: 0 = BX, 1 = BLX). Block terminator
// with ARMv5 bit0 interworking -- identical shape to LDM{...,pc} (B4): bit0 of Rm
// selects the resume ISA. BLX also writes R14 = the ARM return address first.
void emitBranchExchange(JitTraceCtx& ctx, u32 op, bool isBlx)
{
	const u8 rm = op & 0xF;
	if (rm == 15) { ctx.endBlock = true; return; }          // BX pc: unpredictable

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRm = ctx.readReg(rm, lockedMask);
	*p++ = PPC_OR(PPC_R12, hRm, hRm);                        // capture target pre-flush

	if (isBlx) {
		const u8 hLR = ctx.writeReg(14, true, lockedMask);
		emitLoadImm32(p, hLR, ctx.currentPC + 4);            // OP_BLX_REG: R14 = next_instruction
	}

	// Flush every dirty guest reg/flag NOW, while the two exit paths below still
	// share one code position: flushDirtyRegisters() clears the dirty bits, so
	// the per-path emitDynamicExit() calls won't each need to re-flush (only the
	// first would, silently dropping the writeback on the other path). Preceding
	// in-block instructions (e.g. `AND R0,R0,#x` before `BX lr`) leave dirty regs
	// that MUST be persisted here.
	ctx.flushDirtyFlags();
	ctx.flushDirtyRegisters();

	const u32 term = ctx.cpu.cyclesForArm(op);
	*p++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);          // R11 = bit0 (mode select)
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* toArm = p++;                                         // BEQ -> stay ARM

	// bit0 == 1: switch to THUMB (set CPSR.T so the C++ resume path uses 16-bit
	// fetch/pipeline math -- the ARM7 POP{pc} bug otherwise).
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);           // & ~1
	ctx.ensureFlagsLoaded();
	*p++ = PPC_LI(PPC_R10, 0x20);                             // CPSR.T (bit 5)
	*p++ = PPC_OR(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
	ctx.flagsDirty = true;
	ctx.flushDirtyFlags();
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/true);

	*toArm = PPC_BEQ((u32)((p - toArm) * 4));
	*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 29);           // & ~3 (CPSR.T already 0)
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, term, /*targetThumb=*/false);

	ctx.instrCount++;
	ctx.currentPC += 4;
	ctx.endBlock = true;
	ctx.blockTerminatedEarly = true;
}

// --------------------------------------------------------------- SWP/SWPB (B6)
// cond 00010 B 00 Rn Rd 0000 1001 Rm   -- an ordered load-then-store at [Rn]:
//   Rd = ROR(mem[Rn], 8*(Rn&3)) (word) / mem[Rn] (byte) ; mem[Rn] = Rm
// The word form rotates the loaded value exactly like OP_LDR / OP_SWP. Not a
// terminator. The SMC guard runs before either access, so a bail is a clean
// re-run (both accesses redo; RAM idempotent, I/O flagged untrusted).
void emitSwap(JitTraceCtx& ctx, u32 op)
{
	const bool B  = (op >> 22) & 1;
	const u8   rn = (op >> 16) & 0xF;
	const u8   rd = (op >> 12) & 0xF;
	const u8   rm = op & 0xF;
	const u32  size = B ? 1u : 4u;

	if (rn == 15 || rd == 15 || rm == 15) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRn = ctx.readReg(rn, lockedMask);
	const u8 hRm = ctx.readReg(rm, lockedMask);
	*p++ = PPC_STW(hRn, 1, 96);                              // EA
	*p++ = PPC_STW(hRm, 1, 100);                             // store value

	ctx.emitMemPrologue();
	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	ctx.emitSmcCheckAndBail(PPC_R12);

	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	ctx.emitSlowLoad(PPC_R10, PPC_R12, size, false);
	if (!B) {                                                // ROR(loaded, 8*(EA&3))
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31);      // EA & 3
		*p++ = PPC_LI(PPC_R11, 4);
		*p++ = PPC_SUBF(PPC_R12, PPC_R12, PPC_R11);          // 4 - (EA&3)
		*p++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28);      // ((4-x)&3) << 3
		*p++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
	}
	*p++ = PPC_STW(PPC_R10, 1, 104);                         // stash loaded value

	*p++ = PPC_LWZ(PPC_R12, 1, 96);
	*p++ = PPC_LWZ(PPC_R10, 1, 100);
	ctx.emitSlowStore(PPC_R12, PPC_R10, size);

	ctx.emitMemEpilogue();
	ctx.invalidateRegCache();
	*p++ = PPC_LWZ(PPC_R11, 1, 104);
	*p++ = PPC_STW(PPC_R11, 14, rd * 4);                     // Rd = loaded value (last)
}

// ------------------------------------------------------------- MRS / MSR (B6)
// MRS Rd, CPSR : cond 00010 0 00 1111 Rd 0000 0000 0000 -- Rd = whole CPSR word.
// PPC_REG_FLAGS holds exactly that (the non-flag bits are unchanged since block
// entry; only BX / MSR touch them and both are handled here). SPSR form -> interp.
void emitMrs(JitTraceCtx& ctx, u32 op)
{
	if ((op >> 22) & 1) { ctx.endBlock = true; return; }    // MRS SPSR -> interp
	const u8 rd = (op >> 12) & 0xF;
	if (rd == 15) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRd = ctx.writeReg(rd, true, lockedMask);
	ctx.ensureFlagsLoaded();
	*p++ = PPC_OR(hRd, PPC_REG_FLAGS, PPC_REG_FLAGS);
}

// MSR CPSR_f, <Rm | #imm> : only the flags byte (field mask == 0b1000, R == 0) is
// compilable -- any control/extension/status byte, or SPSR, ends the trace (mode
// switch + full CPSR reload). Writes bits 31..24 of the operand into the packed
// flags. The interpreter also calls changeCPSR() -> NDS_Reschedule() here; a
// flags-only write can't touch I/F/mode so there is no IRQ-unmask edge -- only a
// scheduler nudge the outer emulation loop performs regularly anyway.
void emitMsr(JitTraceCtx& ctx, u32 op, bool immForm)
{
	if ((op >> 22) & 1)            { ctx.endBlock = true; return; }   // SPSR
	if (((op >> 16) & 0xF) != 0x8) { ctx.endBlock = true; return; }   // not flags-only

	u8 rm = 0;
	if (!immForm) { rm = op & 0xF; if (rm == 15) { ctx.endBlock = true; return; } }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	ctx.ensureFlagsLoaded();
	if (immForm) {
		const u32 k = ror32(op & 0xFF, ((op >> 8) & 0xF) * 2);
		emitLoadImm32(p, PPC_R12, k);
		*p++ = PPC_RLWIMI(PPC_REG_FLAGS, PPC_R12, 0, 0, 7);
	} else {
		const u8 hRm = ctx.readReg(rm, lockedMask);
		*p++ = PPC_RLWIMI(PPC_REG_FLAGS, hRm, 0, 0, 7);
	}
	ctx.flagsDirty = true;
}

// ------------------------------------------------------------------ CLZ (B7)
// cond 0001 0110 1111 Rd 1111 0001 Rm -- Rd = count leading zeros of Rm. PPC
// cntlzw is an exact match, cntlzw(0) == 32 included (OP_CLZ's Rm == 0 case).
void emitClz(JitTraceCtx& ctx, u32 op)
{
	const u8 rd = (op >> 12) & 0xF;
	const u8 rm = op & 0xF;
	if (rd == 15 || rm == 15) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;
	const u8 hRm = ctx.readReg(rm, lockedMask);
	const u8 hRd = ctx.writeReg(rd, true, lockedMask);
	*p++ = PPC_CNTLZW(hRd, hRm);
}

// -------------------------------------------------------- BLX immediate (B7)
// 1111 101H imm24 -- unconditional call with a mandatory ARM->THUMB switch.
// DeSmuME runs it through OP_B (H == 0) / OP_BL (H == 1) with a cond == 0xF
// branch: R14 = currentPC + 4 ; CPSR.T = 1 ; PC = (currentPC + 8 +
// (signext24 << 2) + (H ? 2 : 0)) & ~1. Compile-time-constant THUMB target ->
// load it into a reg, set T, dynamic exit (emitStaticExit would bake in the ARM
// +8 pipeline; the C++ resume path derives the THUMB pipeline from CPSR.T, as
// for LDM{pc}). Block terminator.
void emitBlxImm(JitTraceCtx& ctx, u32 op)
{
	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const s32 sOff   = (s32)(op << 8) >> 6;                  // signext24 << 2
	const u32 h2     = ((op >> 24) & 1) ? 2u : 0u;
	const u32 target = (ctx.currentPC + 8 + (u32)sOff + h2) & ~1u;

	const u8 hLR = ctx.writeReg(14, true, lockedMask);
	emitLoadImm32(p, hLR, ctx.currentPC + 4);

	ctx.ensureFlagsLoaded();
	*p++ = PPC_LI(PPC_R10, 0x20);                            // CPSR.T (bit 5)
	*p++ = PPC_OR(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
	ctx.flagsDirty = true;

	emitLoadImm32(p, PPC_R12, target);
	ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, 3, /*targetThumb=*/true);   // OP_B / OP_BL return 3

	ctx.instrCount++;
	ctx.currentPC += 4;
	ctx.endBlock = true;
	ctx.blockTerminatedEarly = true;
}

// -------------------------------------------------------- LDRD / STRD (B7)
// cond 000 P U I W 0 Rn Rd xxxx 11 S 1 Rm/imm   (S: 0 LDRD / 1 STRD)
//   I : 1 imm8 (hi<<4 | lo) offset, 0 register offset (Rm, unshifted)
//   P : 1 pre/offset, 0 post (post always writes Rn = Rn +/- index)
//   U : add / subtract     W : writeback (pre-index only; !P && W -> interp)
// Two consecutive word accesses at addr / addr+4 into the pair {Rd, Rd+1}; no
// unaligned rotate (raw READ32/WRITE32 in OP_LDRD_STRD_*). Writeback committed
// after the access (SMC-bail = clean re-run), so bail on any Rn/Rd/Rd+1 aliasing
// that would make ordering observable. Odd Rd / Rd >= 14 / pc base -> interp.
void emitDoubleDataTransfer(JitTraceCtx& ctx, u32 op)
{
	const bool P     = (op >> 24) & 1;
	const bool U     = (op >> 23) & 1;
	const bool I     = (op >> 22) & 1;
	const bool W     = (op >> 21) & 1;
	const bool store = (op >> 5) & 1;
	const u8   rn    = (op >> 16) & 0xF;
	const u8   rd    = (op >> 12) & 0xF;
	const u8   rm    = op & 0xF;
	const bool writeback = (!P) || W;

	if (!P && W)                                  { ctx.endBlock = true; return; }
	if (rd & 1)                                   { ctx.endBlock = true; return; }  // Rd even
	if (rd >= 14)                                 { ctx.endBlock = true; return; }  // Rd+1 == pc / Rd == pc
	if (rn == 15)                                 { ctx.endBlock = true; return; }  // pc base -> interp
	if (!I && rm == 15)                           { ctx.endBlock = true; return; }
	if (writeback && (rn == rd || rn == rd + 1))  { ctx.endBlock = true; return; }
	if (!I && (rm == rd || rm == rd + 1))         { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRn = ctx.readReg(rn, lockedMask);
	u8 hRm = 0;
	if (!I) hRm = ctx.readReg(rm, lockedMask);
	const s32 soff = U ? (s32)(((op >> 4) & 0xF0) | (op & 0xF))
	                   : -(s32)(((op >> 4) & 0xF0) | (op & 0xF));

	// EA -> slot 96 ; writeback value (Rn +/- index) -> slot 104
	if (P) {
		if      (I) *p++ = PPC_ADDI(PPC_R11, hRn, soff);
		else if (U) *p++ = PPC_ADD (PPC_R11, hRn, hRm);
		else        *p++ = PPC_SUBF(PPC_R11, hRm, hRn);
		*p++ = PPC_STW(PPC_R11, 1, 96);
		if (writeback) *p++ = PPC_STW(PPC_R11, 1, 104);
	} else {
		*p++ = PPC_STW(hRn, 1, 96);                          // EA = Rn (post-index)
		if      (I) *p++ = PPC_ADDI(PPC_R11, hRn, soff);
		else if (U) *p++ = PPC_ADD (PPC_R11, hRn, hRm);
		else        *p++ = PPC_SUBF(PPC_R11, hRm, hRn);
		*p++ = PPC_STW(PPC_R11, 1, 104);
	}

	ctx.emitMemPrologue();

	if (store) {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		ctx.emitSmcCheckAndBail(PPC_R12);
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_LWZ(PPC_R10, 14, rd * 4);
		ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_ADDI(PPC_R12, PPC_R12, 4);
		*p++ = PPC_LWZ(PPC_R10, 14, (rd + 1) * 4);
		ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		if (writeback) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }
	} else {
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
		*p++ = PPC_STW(PPC_R10, 1, 100);                     // stash word 0
		*p++ = PPC_LWZ(PPC_R12, 1, 96);
		*p++ = PPC_ADDI(PPC_R12, PPC_R12, 4);
		ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);        // word 1 -> R10
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		*p++ = PPC_STW(PPC_R10, 14, (rd + 1) * 4);           // Rd+1
		*p++ = PPC_LWZ(PPC_R11, 1, 100);
		*p++ = PPC_STW(PPC_R11, 14, rd * 4);                 // Rd
		if (writeback) { *p++ = PPC_LWZ(PPC_R11, 1, 104); *p++ = PPC_STW(PPC_R11, 14, rn * 4); }
	}
}

// --------------------------------------------- Q sticky flag helper (B7b)
// CPSR.Q is conventional bit 27 (== IBM/rlwinm bit 4) of the packed CPSR word.
// It is sticky -- only ever set, never cleared here -- so `if (ovfReg bit31)
// Q = 1`. ovfReg holds the saturation/overflow condition as 0/1 in bit 31.
// Emitted as a forward-skip so Q is untouched when there was no overflow.
void emitStickyQ(JitTraceCtx& ctx, u8 ovfReg)
{
	u32*& p = ctx.emitPtr;
	ctx.ensureFlagsLoaded();
	*p++ = PPC_CMPWI(0, ovfReg, 0);
	u32* skip = p++;                                    // BEQ -> no set
	*p++ = PPC_LI(PPC_R8, 1);
	*p++ = PPC_RLWIMI(PPC_REG_FLAGS, PPC_R8, 27, 4, 4); // FLAGS bit 4 (IBM) = CPSR bit 27
	*skip = PPC_BEQ((u32)((p - skip) * 4));
	ctx.flagsDirty = true;
}

// Saturate hRes in place to 0x7FFFFFFF / 0x80000000 by the sign of the raw
// result: 0x80000000 + (hRes >> 31 arithmetically) == the interpreter's
// `0x80000000 - BIT31(res)`. Clobbers R8, R11.
void emitSaturate(u32*& p, u8 hRes)
{
	*p++ = PPC_SRAWI(PPC_R8, hRes, 31);                 // 0 or 0xFFFFFFFF
	*p++ = PPC_LIS(PPC_R11, 0x8000);                    // 0x80000000
	*p++ = PPC_ADD(hRes, PPC_R8, PPC_R11);              // 0x80000000 + (0 | -1)
}

// ----------------------------------------- QADD / QSUB / QDADD / QDSUB (B7b)
// cond 0001 0 D S 0 Rn Rd 0000 0101 Rm   (bits 22..21 = D:S select the four)
//   QADD  Rd = sat(Rn + Rm)         QSUB  Rd = sat(Rm - Rn)
//   QDADD Rd = sat(sat(Rn<<1) + Rm) QDSUB Rd = sat(Rm - sat(Rn<<1))
// Note the operand order: Rn = bits 19..16, Rm = bits 3..0, and QSUB/QDSUB
// compute Rm - (Rn term), not the other way. Q (CPSR bit 27) is set sticky on
// any saturation, including the QD* doubling step. PPC add/subf with OE give
// XER[OV] == the ARM signed overflow/underflow the interpreter's
// SIGNED_OVERFLOW / SIGNED_UNDERFLOW compute. cond == AL only; Rd == 15 (a
// non-interworking word-aligned PC write) -> interp.
void emitQArith(JitTraceCtx& ctx, u32 op)
{
	const bool doubled = (op >> 22) & 1;
	const bool isSub   = (op >> 21) & 1;
	const u8   rn = (op >> 16) & 0xF;
	const u8   rd = (op >> 12) & 0xF;
	const u8   rm = op & 0xF;
	if (rn == 15 || rd == 15 || rm == 15) { ctx.endBlock = true; return; }

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRn = ctx.readReg(rn, lockedMask);
	const u8 hRm = ctx.readReg(rm, lockedMask);
	const u8 hRd = ctx.writeReg(rd, true, lockedMask);
	ctx.ensureFlagsLoaded();

	u8 term = hRn;                                      // the Rn-side addend/subtrahend
	if (doubled) {
		// R12 = Rn << 1 ; saturate + Q if the sign changed
		*p++ = PPC_RLWINM(PPC_R12, hRn, 1, 0, 30);
		*p++ = PPC_XOR(PPC_R11, hRn, PPC_R12);
		*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 1, 31, 31); // sign-differs (0/1)
		*p++ = PPC_CMPWI(0, PPC_R11, 0);
		u32* noDbl = p++;                               // BEQ -> no doubling saturation
		emitSaturate(p, PPC_R12);
		*p++ = PPC_LI(PPC_R8, 1);
		*p++ = PPC_RLWIMI(PPC_REG_FLAGS, PPC_R8, 27, 4, 4);
		ctx.flagsDirty = true;
		*noDbl = PPC_BEQ((u32)((p - noDbl) * 4));
		term = PPC_R12;
	}

	if (isSub) *p++ = PPC_SUBFCO(hRd, term, hRm);       // Rm - term
	else       *p++ = PPC_ADDCO (hRd, hRm, term);       // Rm + term
	*p++ = PPC_MFXER(PPC_R8);
	*p++ = PPC_RLWINM(PPC_R11, PPC_R8, 2, 31, 31);      // XER[OV] -> 0/1

	// on overflow: replace hRd with the saturated value AND set Q
	*p++ = PPC_CMPWI(0, PPC_R11, 0);
	u32* noSat = p++;                                   // BEQ -> keep hRd, Q untouched
	emitSaturate(p, hRd);
	*p++ = PPC_LI(PPC_R8, 1);
	*p++ = PPC_RLWIMI(PPC_REG_FLAGS, PPC_R8, 27, 4, 4);
	*noSat = PPC_BEQ((u32)((p - noSat) * 4));
	ctx.flagsDirty = true;
}

// ---------------- sign-extended half of a register -> dst (B7b DSP muls) ----
inline void emitSHalf(u32*& p, u8 dst, u8 src, bool high)
{
	if (high) *p++ = PPC_SRAWI(dst, src, 16);           // HWORD: (s32)src >> 16
	else      *p++ = PPC_EXTSH(dst, src);               // LWORD: sign-extend low 16
}

// ------------------------ SMUL / SMLA / SMLAL / SMULW / SMLAW  <x><y>  (B7b)
// The ARMv5TE signed 16-bit-halfword multiply family. Encoding (bits 27..20):
//   0x10 SMLA<x><y>   Rd16 = half_x(Rm) * half_y(Rs) + Ra12          Q on ovf
//   0x12 SMLAW<y> / SMULW<y>  (bit5): Rd16 = (half_y(Rs) * Rm) >> 16 (+Ra12)
//   0x14 SMLAL<x><y>  {Rd16:Ra12} += sign_ext_64(half_x(Rm) * half_y(Rs))
//   0x16 SMUL<x><y>   Rd16 = half_x(Rm) * half_y(Rs)
//   x = bit5 (Rm half), y = bit6 (Rs half); 0 = low ("B"), 1 = high ("T").
// half*half fits in 32 bits so PPC mullw's low word is the exact product; the
// SM*W forms need the 48-bit product (mullw + mulhw) shifted right 16. The
// SMLAL accumulate matches OP_SMLAL_*'s exact (bug-compatible) arithmetic:
// RdLo_new = tmpLo + RdLo ; RdHi_new = RdHi + RdLo_new + (tmp<0 ? -1 : 0).
// cond == AL only; any pc operand / ARM-unpredictable RdHi==RdLo / Rm-aliases-
// Rd(Lo|Hi) -> interp.
void emitDspMul(JitTraceCtx& ctx, u32 op)
{
	const u8   kind = (u8)((op >> 21) & 3);   // 0 SMLA, 1 SMLAW/SMULW, 2 SMLAL, 3 SMUL
	const bool xHigh = (op >> 5) & 1;
	const bool yHigh = (op >> 6) & 1;
	const u8   rm = op & 0xF;                 // bits 3..0
	const u8   rs = (op >> 8) & 0xF;          // bits 11..8
	const u8   ra = (op >> 12) & 0xF;         // bits 15..12  (accumulator / RdLo)
	const u8   rd = (op >> 16) & 0xF;         // bits 19..16  (result / RdHi)
	const bool wide  = (kind == 1);           // SMLAW / SMULW : Rs half * full Rm >> 16
	const bool accW  = wide && !((op >> 5) & 1);   // SMLAW (bit5 == 0) vs SMULW (bit5 == 1)

	if (rm == 15 || rs == 15 || rd == 15) { ctx.endBlock = true; return; }
	const bool hasAcc = (kind == 0) || (kind == 2) || (wide && accW);
	if (hasAcc && ra == 15) { ctx.endBlock = true; return; }
	if (kind == 2) {                          // SMLAL<x><y>
		if (ra == rd || rm == ra || rm == rd) { ctx.endBlock = true; return; }
	}

	ctx.ensureArena();
	u32*& p = ctx.emitPtr;
	u32 lockedMask = 0;

	const u8 hRm = ctx.readReg(rm, lockedMask);
	const u8 hRs = ctx.readReg(rs, lockedMask);

	if (wide) {
		// tmp = (half_y(Rs) * (s32)Rm) >> 16   -> R11
		emitSHalf(p, PPC_R10, hRs, yHigh);
		*p++ = PPC_MULLW(PPC_R11, PPC_R10, hRm);        // lo 32
		*p++ = PPC_MULHW(PPC_R12, PPC_R10, hRm);        // hi 32 (signed)
		*p++ = PPC_RLWINM(PPC_R11, PPC_R11, 16, 16, 31); // lo >>u 16
		*p++ = PPC_RLWIMI(PPC_R11, PPC_R12, 16, 0, 15);  // | (hi << 16)  -> {hi:lo} >> 16
		if (accW) {                                     // SMLAW: + Ra, Q on signed ovf
			const u8 hRa = ctx.readReg(ra, lockedMask);
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*p++ = PPC_ADDCO(hRd, PPC_R11, hRa);
			*p++ = PPC_MFXER(PPC_R8);
			*p++ = PPC_RLWINM(PPC_R8, PPC_R8, 2, 31, 31);
			emitStickyQ(ctx, PPC_R8);
		} else {                                        // SMULW: just the shifted product
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*p++ = PPC_OR(hRd, PPC_R11, PPC_R11);
		}
		return;
	}

	// half_x(Rm) * half_y(Rs) -> R12  (fits in 32 bits)
	emitSHalf(p, PPC_R11, hRm, xHigh);
	emitSHalf(p, PPC_R12, hRs, yHigh);
	*p++ = PPC_MULLW(PPC_R12, PPC_R11, PPC_R12);        // tmp (32-bit)

	if (kind == 3) {                                    // SMUL<x><y>
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);
		*p++ = PPC_OR(hRd, PPC_R12, PPC_R12);
		return;
	}
	if (kind == 0) {                                    // SMLA<x><y> : + Ra, Q on ovf
		const u8 hRa = ctx.readReg(ra, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);
		*p++ = PPC_ADDCO(hRd, PPC_R12, hRa);
		*p++ = PPC_MFXER(PPC_R8);
		*p++ = PPC_RLWINM(PPC_R8, PPC_R8, 2, 31, 31);
		emitStickyQ(ctx, PPC_R8);
		return;
	}

	// kind == 2 : SMLAL<x><y> -- 64-bit accumulate, no flags
	//   RdLo(ra)_new = tmpLo + RdLo ;  RdHi(rd)_new = RdHi + RdLo_new + sign(tmp)
	*p++ = PPC_SRAWI(PPC_R11, PPC_R12, 31);             // R11 = (tmp < 0 ? -1 : 0)
	const u8 hLo = ctx.writeReg(ra, false, lockedMask); // needs old RdLo
	const u8 hHi = ctx.writeReg(rd, false, lockedMask); // needs old RdHi
	*p++ = PPC_ADD(PPC_R10, PPC_R12, hLo);              // R10 = RdLo_new = tmpLo + RdLo_old
	*p++ = PPC_ADD(hHi, hHi, PPC_R10);                  // RdHi += RdLo_new
	*p++ = PPC_ADD(hHi, hHi, PPC_R11);                  // RdHi += sign(tmp)
	*p++ = PPC_OR(hLo, PPC_R10, PPC_R10);               // RdLo = RdLo_new
}

} // namespace

void jitArmEmitOne(JitTraceCtx& ctx, u32 op)
{
	const u8 cond = (u8)(op >> 28);

	if (cond == COND_NV) {
		// ARMv5 unconditional-extension space. Only BLX imm actually executes
		// (DeSmuME: cond == 0xF passes TEST_COND only for CODE == 5 == the 101
		// branch group); PLD is a hint -> emit nothing, block keeps compiling;
		// CPS / SETEND / RFE / SRS / coprocessor-double -> interpreter.
		if ((op & 0x0E000000u) == 0x0A000000u) { emitBlxImm(ctx, op); return; }
		if ((op & 0x0D70F000u) == 0x0550F000u) return;                 // PLD (nop)
		ctx.endBlock = true;
		return;
	}

	// B / BL : bits 27..25 == 101
	if ((op & 0x0E000000u) == 0x0A000000u) { emitBranch(ctx, op, cond); return; }

	// Extra load/store (LDRH/STRH/LDRSB/LDRSH) + LDRD/STRD: bits 27..25 == 000,
	// bit7 & bit4 set, bits 6..5 != 00 (00 = multiply / SWP). LDRD/STRD is the
	// bit20 == 0 && bits6..5 >= 10 corner (B7); the rest is B3b.
	if ((op & 0x0E000000u) == 0 && (op & 0x90u) == 0x90u && (op & 0x60u) != 0) {
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		if (((op >> 20) & 1) == 0 && ((op >> 5) & 3) >= 2) emitDoubleDataTransfer(ctx, op);
		else                                               emitExtraDataTransfer(ctx, op);
		return;
	}

	// Miscellaneous (B6/B7/B7b): BX / BLX reg, SWP / SWPB, MRS, MSR, CLZ, the
	// QADD family and the SM* DSP multiplies. All cond == AL only (predicated ->
	// B-later). BKPT still falls through to emitDataProc's testOnly && !S guard.
	if ((op & 0x0FF000F0u) == 0x01600010u) {              // CLZ (B7)
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitClz(ctx, op);
		return;
	}
	if ((op & 0x0F900FF0u) == 0x01000050u) {              // QADD / QSUB / QDADD / QDSUB (B7b)
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitQArith(ctx, op);
		return;
	}
	if ((op & 0x0F900090u) == 0x01000080u) {              // SM{UL,LA,LAL,ULW,LAW}<x><y> (B7b)
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitDspMul(ctx, op);
		return;
	}
	if ((op & 0x0FFFFFD0u) == 0x012FFF10u) {              // BX (0x..1) / BLX (0x..3) reg
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitBranchExchange(ctx, op, (op & 0x20u) != 0);
		return;
	}
	if ((op & 0x0FB00FF0u) == 0x01000090u) {              // SWP / SWPB
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitSwap(ctx, op);
		return;
	}
	if ((op & 0x0FBF0FFFu) == 0x010F0000u) {              // MRS Rd, CPSR/SPSR
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitMrs(ctx, op);
		return;
	}
	if ((op & 0x0FB0FFF0u) == 0x0120F000u ||              // MSR CPSR/SPSR, register
	    (op & 0x0FB0F000u) == 0x0320F000u) {              // MSR CPSR/SPSR, immediate
		if (cond != COND_AL) { ctx.endBlock = true; return; }
		emitMsr(ctx, op, (op & 0x02000000u) != 0);
		return;
	}

	// Multiply / multiply-long : bits 27..23 == 0000x, bits 7..4 == 1001
	// (short MUL/MLA: 27..22 == 000000 ; long: 27..23 == 00001). SWP is 27..23
	// == 00010 and handled just above.
	if ((op & 0x0FC000F0u) == 0x00000090u || (op & 0x0F8000F0u) == 0x00800090u) {
		if (cond != COND_AL) { ctx.endBlock = true; return; }   // predicated -> B-later
		emitMultiply(ctx, op);
		return;
	}

	// Data-processing : bits 27..26 == 00 (bit 25 selects imm vs register op2)
	if ((op & 0x0C000000u) == 0x00000000u) { emitDataProc(ctx, op, cond); return; }

	// LDR / STR single data transfer : bits 27..26 == 01
	if ((op & 0x0C000000u) == 0x04000000u) {
		if (cond != COND_AL) { ctx.endBlock = true; return; }   // predicated -> B-later
		emitSingleDataTransfer(ctx, op);
		return;
	}

	// LDM / STM block data transfer : bits 27..25 == 100
	if ((op & 0x0E000000u) == 0x08000000u) {
		if (cond != COND_AL) { ctx.endBlock = true; return; }   // predicated -> B-later
		emitBlockDataTransfer(ctx, op);
		return;
	}

	ctx.endBlock = true;   // everything else -> interpreter (later B-groups)
}

#endif // DESMUME_JIT_ARM7
