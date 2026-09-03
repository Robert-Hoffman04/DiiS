/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_thumb.cpp
 *
 * THUMB (ARMv4T) emitter table. Runs on top of jit_trace.cpp's scanner /
 * register allocator / packed-flag helpers. Derived instruction-by-instruction
 * from VBA-GX's JITCompiler.cpp (c) Daryl Borth, GPL v2+ --
 * see jit/upstream/PROVENANCE.md.
 *
 * P2 coverage (plan groups 1, 2, 6):
 *   Format 1  LSL/LSR/ASR #imm            0x0000-0x17FF
 *   Format 2  ADD/SUB reg|imm3            0x1800-0x1FFF
 *   Format 3  MOV/CMP/ADD/SUB #imm8       0x2000-0x3FFF
 *   Format 4  ALU reg-reg (16 ops)        0x4000-0x43FF
 *   Format 16 conditional branch          0xD000-0xDDFF
 *   Format 18 unconditional branch        0xE000-0xE7FF
 * Everything else ends the trace (interpreter continues).
 ***************************************************************************/

#include "jit_trace.h"

#if defined(DESMUME_JIT_ARM7)

#include "jit_ppc_emitter.h"

void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode)
{
	u32*&     emitPtr    = ctx.emitPtr;
	JITCache& cache      = ctx.cache;
	const u32 currentPC  = ctx.currentPC;
	u32       lockedMask = 0;

	switch (opcode >> 11) {

	// --------------------------------------------------------------------
	// Format 1: LSL / LSR / ASR by immediate
	// --------------------------------------------------------------------
	case 0: case 1: case 2: {
		ctx.ensureArena();
		const u8 rd  = opcode & 0x07;
		const u8 rs  = (opcode >> 3) & 0x07;
		const u8 off = (opcode >> 6) & 0x1F;
		const u8 op  = (opcode >> 11) & 0x03;   // 0=LSL 1=LSR 2=ASR

		const u8 hRs = ctx.readReg(rs, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);

		if (op == 0) {                          // LSL
			if (off == 0) {
				*emitPtr++ = PPC_OR(hRd, hRs, hRs);
			} else {
				ctx.emitFlagBit(JITF_C, hRs, off);
				*emitPtr++ = PPC_RLWINM(hRd, hRs, off, 0, 31 - off);
			}
		} else if (op == 1) {                   // LSR  (#0 means #32)
			if (off == 0) {
				ctx.emitFlagBit(JITF_C, hRs, 1);
				*emitPtr++ = PPC_LI(hRd, 0);
			} else {
				ctx.emitFlagBit(JITF_C, hRs, (33 - off) & 31);
				*emitPtr++ = PPC_SRWI(hRd, hRs, off);
			}
		} else {                               // ASR  (#0 means #32)
			if (off == 0) {
				ctx.emitFlagBit(JITF_C, hRs, 1);
				*emitPtr++ = PPC_MFXER(PPC_R10);
				*emitPtr++ = PPC_SRAWI(hRd, hRs, 31);
				*emitPtr++ = PPC_MTXER(PPC_R10);
			} else {
				ctx.emitFlagBit(JITF_C, hRs, (33 - off) & 31);
				*emitPtr++ = PPC_MFXER(PPC_R10);
				*emitPtr++ = PPC_SRAWI(hRd, hRs, off);
				*emitPtr++ = PPC_MTXER(PPC_R10);
			}
		}
		ctx.emitNZ(hRd);
		break;
	}

	// --------------------------------------------------------------------
	// Format 2: ADD / SUB (register or 3-bit immediate)
	// --------------------------------------------------------------------
	case 3: {
		ctx.ensureArena();
		const u8 rd     = opcode & 0x07;
		const u8 rs     = (opcode >> 3) & 0x07;
		const u8 type   = (opcode >> 9) & 0x03;   // 0=ADDreg 1=SUBreg 2=ADDimm 3=SUBimm
		const u8 rn_imm = (opcode >> 6) & 0x07;

		const u8 hRs = ctx.readReg(rs, lockedMask);
		u8 hRn = 0;
		if (type < 2) hRn = ctx.readReg(rn_imm, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);

		if (type < 2) *emitPtr++ = PPC_OR(PPC_R12, hRn, hRn);
		else          *emitPtr++ = PPC_LI(PPC_R12, rn_imm);

		if (type == 0 || type == 2) *emitPtr++ = PPC_ADDCO(hRd, hRs, PPC_R12);
		else                        *emitPtr++ = PPC_SUBFCO(hRd, PPC_R12, hRs); // Rs - R12

		ctx.emitCVfromXER(PPC_R11);
		ctx.emitNZ(hRd);
		break;
	}

	// --------------------------------------------------------------------
	// Format 3: MOV / CMP / ADD / SUB  Rd, #imm8
	// --------------------------------------------------------------------
	case 4: case 5: case 6: case 7: {
		ctx.ensureArena();
		const u8 op  = (opcode >> 11) & 0x03;    // 0=MOV 1=CMP 2=ADD 3=SUB
		const u8 rd  = (opcode >> 8) & 0x07;
		const u8 imm = opcode & 0xFF;

		if (op == 0) {                           // MOV
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_LI(hRd, imm);
			ctx.emitFlagConst(JITF_N, false);
			ctx.emitFlagConst(JITF_Z, imm == 0);
		} else {
			u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_LI(PPC_R12, imm);
			if (op == 1) {                       // CMP -> R11 = Rd - imm
				*emitPtr++ = PPC_SUBFCO(PPC_R11, PPC_R12, hRd);
			} else if (op == 2) {                // ADD
				hRd = ctx.writeReg(rd, false, lockedMask);
				*emitPtr++ = PPC_ADDCO(hRd, hRd, PPC_R12);
			} else {                             // SUB
				hRd = ctx.writeReg(rd, false, lockedMask);
				*emitPtr++ = PPC_SUBFCO(hRd, PPC_R12, hRd);
			}
			ctx.emitCVfromXER(PPC_R10);
			ctx.emitNZ((op == 1) ? PPC_R11 : hRd);
		}
		break;
	}

	// --------------------------------------------------------------------
	// 0x4000-0x47FF : Format 4 (ALU) here; Format 5 / BX deferred to P2b
	// --------------------------------------------------------------------
	case 8: {
		if (opcode & 0x0400) { ctx.endBlock = true; break; }  // Format 5 / BX: later

		const u8 op = (opcode >> 6) & 0x0F;
		const u8 rs = (opcode >> 3) & 0x07;
		const u8 rd = opcode & 0x07;
		ctx.ensureArena();

		if (op == 0 || op == 1 || op == 12 || op == 14) {     // AND EOR ORR BIC
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			if (op == 0)  *emitPtr++ = PPC_AND (hRd, hRd, hRs);
			if (op == 1)  *emitPtr++ = PPC_XOR (hRd, hRd, hRs);
			if (op == 12) *emitPtr++ = PPC_OR  (hRd, hRd, hRs);
			if (op == 14) *emitPtr++ = PPC_ANDC(hRd, hRd, hRs);
			ctx.emitNZ(hRd);
		}
		else if (op == 8) {                                   // TST
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_AND(PPC_R12, hRd, hRs);
			ctx.emitNZ(PPC_R12);
		}
		else if (op == 15) {                                  // MVN
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_NOR(hRd, hRs, hRs);
			ctx.emitNZ(hRd);
		}
		else if (op == 10) {                                  // CMP -> R12 = Rd - Rs
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_SUBFCO(PPC_R12, hRs, hRd);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(PPC_R12);
		}
		else if (op == 11) {                                  // CMN -> R12 = Rd + Rs
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_ADDCO(PPC_R12, hRd, hRs);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(PPC_R12);
		}
		else if (op == 9) {                                   // NEG -> Rd = 0 - Rs
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_LI(PPC_R12, 0);
			*emitPtr++ = PPC_SUBFCO(hRd, hRs, PPC_R12);       // R12 - Rs
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(hRd);
		}
		else if (op == 5 || op == 6) {                        // ADC / SBC
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			const u8 fC  = ctx.readFlag(JITF_C, PPC_R12);
			// push guest C into host XER CA: 0+(-1)->CA=0 ; 1+(-1)->CA=1
			*emitPtr++ = PPC_ADDIC(PPC_R12, fC, -1);
			if (op == 5) *emitPtr++ = PPC_ADDEO (hRd, hRd, hRs);          // Rd + Rs + C
			else         *emitPtr++ = PPC_SUBFEO(hRd, hRs, hRd);         // Rd + ~Rs + C
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(hRd);
		}
		else if (op == 13) {                                  // MUL (N,Z only)
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			*emitPtr++ = PPC_MULLW(hRd, hRd, hRs);
			ctx.emitNZ(hRd);
		}
		else if (op == 2 || op == 3 || op == 4 || op == 7) {  // LSL/LSR/ASR/ROR by reg
			// The shift-out carry idioms below are only exact for shift amounts
			// 1..31. Amount 0 (Rd/C unchanged, N/Z from Rd) and >=32 (per-op
			// special cases in the interpreter) are handled by:
			//   - a compile-time-cheap runtime bail for >= 32
			//   - the amount==0 skip branch
			const u8 hRs = ctx.readReg(rs, lockedMask);
			*emitPtr++ = PPC_RLWINM(PPC_R12, hRs, 0, 24, 31);  // R12 = Rs & 0xFF

			ctx.emitEagerFlush();                              // state safe before the guard
			*emitPtr++ = PPC_CMPWI(0, PPC_R12, 32);
			u32* guardHi = emitPtr++;
			ctx.registerBailout(guardHi, JIT_COND_BGE);        // amount >= 32 -> interpreter

			const u8 hRd = ctx.writeReg(rd, false, lockedMask);

			*emitPtr++ = PPC_CMPWI(0, PPC_R12, 0);
			u32* skipZero = emitPtr++;                         // amount == 0 -> only N/Z

			if (op == 2) {                                     // LSL: out bit = 32-amt
				*emitPtr++ = PPC_LI(PPC_R11, 32);
				*emitPtr++ = PPC_SUBF(PPC_R11, PPC_R12, PPC_R11);
				*emitPtr++ = PPC_SRW(PPC_R10, hRd, PPC_R11);
			} else {                                           // LSR/ASR/ROR: out bit = amt-1
				*emitPtr++ = PPC_ADDI(PPC_R11, PPC_R12, -1);
				*emitPtr++ = PPC_SRW(PPC_R10, hRd, PPC_R11);
			}
			ctx.emitFlagBit(JITF_C, PPC_R10, 0);

			if (op == 2)      *emitPtr++ = PPC_SLW (hRd, hRd, PPC_R12);
			else if (op == 3) *emitPtr++ = PPC_SRW (hRd, hRd, PPC_R12);
			else if (op == 4) *emitPtr++ = PPC_SRAW(hRd, hRd, PPC_R12);
			else {                                             // ROR by (amount & 31), amount 1..31
				*emitPtr++ = PPC_LI(PPC_R11, 32);
				*emitPtr++ = PPC_SUBF(PPC_R11, PPC_R12, PPC_R11);
				*emitPtr++ = PPC_RLWNM(hRd, hRd, PPC_R11, 0, 31);
			}

			*skipZero = PPC_BEQ((u32)((emitPtr - skipZero) * 4));
			ctx.emitNZ(hRd);   // N/Z reflect the (possibly unchanged) result
		}
		else {
			ctx.endBlock = true;   // unreachable for 4-bit op, defensive
		}
		break;
	}

	// --------------------------------------------------------------------
	// Format 16: conditional branch  (0xD000-0xDDFF; 0xDF00 = SWI)
	// --------------------------------------------------------------------
	case 26: case 27: {
		if ((opcode & 0x0F00) == 0x0F00) { ctx.endBlock = true; break; }  // SWI

		const u8 cond    = (opcode >> 8) & 0x0F;
		const s8 off      = (s8)(opcode & 0xFF);
		const u32 targetPC = currentPC + 4 + (off << 1);

		ctx.ensureArena();

		bool composite = false, branchIfZero = false;
		u32  flagReg = 0;
		bool guardIsBEQ = false;

		switch (cond) {
			case 0x0: flagReg = ctx.readFlag(JITF_Z, PPC_R12); guardIsBEQ = true;  break; // EQ
			case 0x1: flagReg = ctx.readFlag(JITF_Z, PPC_R12); guardIsBEQ = false; break; // NE
			case 0x2: flagReg = ctx.readFlag(JITF_C, PPC_R12); guardIsBEQ = true;  break; // CS
			case 0x3: flagReg = ctx.readFlag(JITF_C, PPC_R12); guardIsBEQ = false; break; // CC
			case 0x4: flagReg = ctx.readFlag(JITF_N, PPC_R12); guardIsBEQ = true;  break; // MI
			case 0x5: flagReg = ctx.readFlag(JITF_N, PPC_R12); guardIsBEQ = false; break; // PL
			case 0x6: flagReg = ctx.readFlag(JITF_V, PPC_R12); guardIsBEQ = true;  break; // VS
			case 0x7: flagReg = ctx.readFlag(JITF_V, PPC_R12); guardIsBEQ = false; break; // VC
			case 0x8: composite = true; branchIfZero = false; break;  // HI  C & ~Z
			case 0x9: composite = true; branchIfZero = true;  break;  // LS  ~(C & ~Z)
			case 0xA: composite = true; branchIfZero = true;  break;  // GE  ~(N ^ V)
			case 0xB: composite = true; branchIfZero = false; break;  // LT  N ^ V
			case 0xC: composite = true; branchIfZero = true;  break;  // GT  ~(Z | (N^V))
			case 0xD: composite = true; branchIfZero = false; break;  // LE  Z | (N^V)
			default:  ctx.endBlock = true; break;                     // AL/NV: not here
		}
		if (ctx.endBlock) break;

		u32* guard = nullptr;
		if (!composite) {
			*emitPtr++ = PPC_CMPWI(0, flagReg, 0);
			guard = emitPtr++;                       // patched below
		} else {
			if (cond == 0x8 || cond == 0x9) {        // HI / LS : R11 = C & ~Z
				u8 fC = ctx.readFlag(JITF_C, PPC_R10);
				u8 fZ = ctx.readFlag(JITF_Z, PPC_R12);
				*emitPtr++ = PPC_ANDC(PPC_R11, fC, fZ);
			} else if (cond == 0xA || cond == 0xB) { // GE / LT : R11 = N ^ V
				u8 fN = ctx.readFlag(JITF_N, PPC_R10);
				u8 fV = ctx.readFlag(JITF_V, PPC_R12);
				*emitPtr++ = PPC_XOR(PPC_R11, fN, fV);
			} else {                                 // GT / LE : R11 = (N ^ V) | Z
				u8 fN = ctx.readFlag(JITF_N, PPC_R10);
				u8 fV = ctx.readFlag(JITF_V, PPC_R12);
				*emitPtr++ = PPC_XOR(PPC_R11, fN, fV);
				u8 fZ = ctx.readFlag(JITF_Z, PPC_R10);
				*emitPtr++ = PPC_OR(PPC_R11, PPC_R11, fZ);
			}
			*emitPtr++ = PPC_CMPWI(0, PPC_R11, 0);
			guard = emitPtr++;
			guardIsBEQ = !branchIfZero;
		}

		// ---- TAKEN path (branch): exit the block ----
		ctx.emitAddCycles(ctx.cyclesAccum + ctx.cpu.cyclesForThumb(opcode));
		ctx.emitDirtyFlagFlush();
		ctx.emitDirtyRegisterFlush();
		ctx.emitResultMetadata(ctx.instrCount + 1, 0);

		*emitPtr++ = PPC_LIS(PPC_R29, (targetPC + 4) >> 16);
		*emitPtr++ = PPC_ORI(PPC_R29, PPC_R29, (targetPC + 4) & 0xFFFF);
		*emitPtr++ = PPC_LIS(PPC_R4, targetPC >> 16);
		*emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
		{
			s32 stubOff = (s32)((u8*)cache.linkerStubAddress - (u8*)emitPtr);
			*emitPtr++ = PPC_BL(stubOff);
			s32 retOff  = (s32)((u8*)cache.linkerReturnAddress - (u8*)emitPtr);
			*emitPtr++ = PPC_B(retOff);
		}

		// patch the guard to skip the taken stub when the condition is not met
		{
			u32 skip = (u32)((emitPtr - guard) * 4);
			*guard = guardIsBEQ ? PPC_BEQ(skip) : PPC_BNE(skip);
		}
		// fall through: keep compiling the not-taken continuation
		break;
	}

	// --------------------------------------------------------------------
	// Format 18: unconditional branch (0xE000-0xE7FF)
	// --------------------------------------------------------------------
	case 28: {
		ctx.ensureArena();
		s32 sOff = (s32)((opcode & 0x07FF) << 21);
		sOff >>= 20;                                   // sign-extend, *2
		const u32 targetPC = currentPC + 4 + sOff;

		ctx.emitAddCycles(ctx.cyclesAccum + ctx.cpu.cyclesForThumb(opcode));
		ctx.flushDirtyFlags();
		ctx.flushDirtyRegisters();
		ctx.emitResultMetadata(ctx.instrCount + 1, 0);

		*emitPtr++ = PPC_LIS(PPC_R29, (targetPC + 4) >> 16);
		*emitPtr++ = PPC_ORI(PPC_R29, PPC_R29, (targetPC + 4) & 0xFFFF);
		*emitPtr++ = PPC_LIS(PPC_R4, targetPC >> 16);
		*emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
		{
			s32 stubOff = (s32)((u8*)cache.linkerStubAddress - (u8*)emitPtr);
			*emitPtr++ = PPC_BL(stubOff);
			s32 retOff  = (s32)((u8*)cache.linkerReturnAddress - (u8*)emitPtr);
			*emitPtr++ = PPC_B(retOff);
		}

		// account for this branch ourselves -- the scanner skips its post-loop
		// bump once endBlock is set
		ctx.instrCount++;
		ctx.currentPC += 2;
		ctx.endBlock = true;
		ctx.blockTerminatedEarly = true;
		break;
	}

	default:
		ctx.endBlock = true;
		break;
	}
}

#endif // DESMUME_JIT_ARM7
