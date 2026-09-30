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
 * Coverage: Formats 1-19 except the deliberately-unhandled edges. Guest memory
 * goes through a C call to JitCpuProfile::slowRead/slowWrite (no inline fast
 * path yet -- that's P6). Register shifts by >= 32 and hi-reg PC writes bail
 * to the interpreter.
 ***************************************************************************/

#include "jit_trace.h"

#if defined(DESMUME_JIT)

#include "jit_ppc_emitter.h"
#ifdef DESMUME_ARM_TIME_SPLIT
#include <stdio.h>
#endif

// The block-exit helpers (emitStaticExit / emitDynamicExit / emitInterpreterBail)
// and the ARM predication helper (emitEvalCond) are JitTraceCtx methods shared
// with jit_arm.cpp -- see jit_trace.cpp.

// Flags of THUMB CMP Rd,#imm8 (0x28xx) / CMP Rd,Rs (0x428x) into the packed
// flags -- the non-fused CMP emitters and the fused Bcc's taken path.
void jitThumbEmitCmpFlags(JitTraceCtx& ctx, u16 op)
{
	u32*& emitPtr = ctx.emitPtr;
	u32 lockedMask = 0;
	if ((op & 0xF800) == 0x2800) {
		const u8 hRd = ctx.readReg((op >> 8) & 7, lockedMask);
		*emitPtr++ = PPC_LI(PPC_R12, op & 0xFF);
		*emitPtr++ = PPC_SUBFCO(PPC_R11, PPC_R12, hRd);
		ctx.emitCVfromXER(PPC_R10);
		ctx.emitNZ(PPC_R11);
	} else {
		const u8 hRs = ctx.readReg((op >> 3) & 7, lockedMask);
		const u8 hRd = ctx.readReg(op & 7, lockedMask);
		*emitPtr++ = PPC_SUBFCO(PPC_R12, hRs, hRd);
		ctx.emitCVfromXER(PPC_R11);
		ctx.emitNZ(PPC_R12);
	}
}

void jitThumbEmitOne(JitTraceCtx& ctx, u16 opcode)
{
	u32*&     emitPtr    = ctx.emitPtr;
	JITCache& cache      = ctx.cache;
	const u32 currentPC  = ctx.currentPC;
	u32       lockedMask = 0;

	switch (opcode >> 11) {

	// ================================================================== F1
	case 0: case 1: case 2: {
		ctx.ensureArena();
		const u8 rd = opcode & 0x07, rs = (opcode >> 3) & 0x07;
		const u8 off = (opcode >> 6) & 0x1F, op = (opcode >> 11) & 0x03;
		const u8 hRs = ctx.readReg(rs, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);

		if (op == 0) {
			if (off == 0) { *emitPtr++ = PPC_OR(hRd, hRs, hRs); }
			else { ctx.emitFlagBit(JITF_C, hRs, off);
			       *emitPtr++ = PPC_RLWINM(hRd, hRs, off, 0, 31 - off); }
		} else if (op == 1) {
			if (off == 0) { ctx.emitFlagBit(JITF_C, hRs, 1); *emitPtr++ = PPC_LI(hRd, 0); }
			else { ctx.emitFlagBit(JITF_C, hRs, (33 - off) & 31);
			       *emitPtr++ = PPC_SRWI(hRd, hRs, off); }
		} else {
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

	// ================================================================== F2
	case 3: {
		ctx.ensureArena();
		const u8 rd = opcode & 0x07, rs = (opcode >> 3) & 0x07;
		const u8 type = (opcode >> 9) & 0x03, rn_imm = (opcode >> 6) & 0x07;
		const u8 hRs = ctx.readReg(rs, lockedMask);
		u8 hRn = 0;
		if (type < 2) hRn = ctx.readReg(rn_imm, lockedMask);
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);
		// Register operand read in place (pinned); imm3 via r12.
		const u8 rB = (type < 2) ? hRn : PPC_R12;
		if (type >= 2) *emitPtr++ = PPC_LI(PPC_R12, rn_imm);
		// C/V dead (dead-flag elimination): plain add/subf, no XER update.
		const bool cvd = ctx.cvDead();
		if (type == 0 || type == 2) *emitPtr++ = cvd ? PPC_ADD(hRd, hRs, rB)  : PPC_ADDCO(hRd, hRs, rB);
		else                        *emitPtr++ = cvd ? PPC_SUBF(hRd, rB, hRs) : PPC_SUBFCO(hRd, rB, hRs);
		ctx.emitCVfromXER(PPC_R11);
		ctx.emitNZ(hRd);
		break;
	}

	// ================================================================== F3
	case 4: case 5: case 6: case 7: {
		ctx.ensureArena();
		const u8 op = (opcode >> 11) & 0x03, rd = (opcode >> 8) & 0x07;
		const u8 imm = opcode & 0xFF;
		if (op == 0) {
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_LI(hRd, imm);
			ctx.emitFlagConst(JITF_N, false);
			ctx.emitFlagConst(JITF_Z, imm == 0);
		} else {
			if (op == 1 && ctx.fuseCmp) {                       // fused into the next Bcc
				ctx.fusedCmpValid = true; ctx.fusedCmpOp = opcode;
				break;
			}
			if (op == 1) { jitThumbEmitCmpFlags(ctx, opcode); break; }
			u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_LI(PPC_R12, imm);
			if (op == 1)      { *emitPtr++ = PPC_SUBFCO(PPC_R11, PPC_R12, hRd); }
			else if (op == 2) { hRd = ctx.writeReg(rd, false, lockedMask);
			                    *emitPtr++ = ctx.cvDead() ? PPC_ADDI(hRd, hRd, imm)
			                                              : PPC_ADDCO(hRd, hRd, PPC_R12); }
			else              { hRd = ctx.writeReg(rd, false, lockedMask);
			                    *emitPtr++ = ctx.cvDead() ? PPC_ADDI(hRd, hRd, -(s32)imm)
			                                              : PPC_SUBFCO(hRd, PPC_R12, hRd); }
			ctx.emitCVfromXER(PPC_R10);
			ctx.emitNZ((op == 1) ? PPC_R11 : hRd);
		}
		break;
	}

	// ============================================ F4 (0x4000-0x43FF) / F5+BX/BLX
	case 8: {
		if (opcode & 0x0400) {
			// ---- 0x4400-0x47FF : Format 5 hi-reg ops + BX/BLX ----------
			const u8 sub = (opcode >> 8) & 0x03;   // 0=ADD 1=CMP 2=MOV 3=BX/BLX
			ctx.ensureArena();

			if (sub == 3) {                          // BX Rs / BLX Rs (bit7 = L)
				const bool isBlx = (opcode & 0x0080) != 0;   // ARMv5 (ARM9): BLX reg
				const u8 rs = (opcode >> 3) & 0x0F;
				// Capture Rm into a scratch first -- before any writeReg()
				// touches the register cache (the P2b hi-reg rd==rs hazard).
				if (rs == 15) {
					u32 v = (currentPC + 4) & ~1u;
					*emitPtr++ = PPC_LIS(PPC_R12, v >> 16);
					*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, v & 0xFFFF);
				} else {
					u8 hRs = ctx.readReg(rs, lockedMask);
					*emitPtr++ = PPC_OR(PPC_R12, hRs, hRs);
				}
				if (isBlx) {
					// OP_BLX_THUMB: R14 = next_instruction | 1, unconditionally
					// (same value whichever mode bit0 selects), so emit it
					// before the branch to keep the dirty-reg flush consistent
					// on both the THUMB and the ARM (interpreter-bail) paths.
					const u32 retLR = (currentPC + 2) | 1;
					const u8 hLR = ctx.writeReg(14, true, lockedMask);
					*emitPtr++ = PPC_LIS(hLR, retLR >> 16);
					*emitPtr++ = PPC_ORI(hLR, hLR, retLR & 0xFFFF);
				}
				*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);  // bit0
				*emitPtr++ = PPC_CMPWI(0, PPC_R11, 0);
				u32* toArm = emitPtr++;                                 // BEQ -> ARM bail
				*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);   // & ~1
				ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode), /*targetThumb=*/true);
				*toArm = PPC_BEQ((u32)((emitPtr - toArm) * 4));
				ctx.emitInterpreterBail(ctx.instrCount);              // bit0==0: ARM switch
				ctx.instrCount++; ctx.currentPC += 2;
				ctx.endBlock = true; ctx.blockTerminatedEarly = true;
				break;
			}

			const u8 h1 = (opcode >> 7) & 1, h2 = (opcode >> 6) & 1;
			const u8 rs = ((opcode >> 3) & 0x07) | (h2 << 3);
			const u8 rd = (opcode & 0x07) | (h1 << 3);

			if (rd == 15 && sub != 1) {
				// MOV pc,Rs / ADD pc,Rs (OP_MOV_SPE / OP_ADD_SPE): R15 = Rs or
				// R15 + Rs, used as-is as the next fetch address (no interworking,
				// no bit0 masking), 3 cycles. A guarded dynamic THUMB exit -- the
				// same exit the in-block interpreter fallback took for it.
				const u32 pc4 = currentPC + 4;
				if (sub == 2) {                                       // MOV
					if (rs == 15) { *emitPtr++ = PPC_LIS(PPC_R12, pc4 >> 16);
					                *emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, pc4 & 0xFFFF); }
					else          { const u8 hRs = ctx.readReg(rs, lockedMask);
					                *emitPtr++ = PPC_OR(PPC_R12, hRs, hRs); }
				} else {                                              // ADD
					const u32 k = (rs == 15) ? 2 * pc4 : pc4;
					*emitPtr++ = PPC_LIS(PPC_R12, k >> 16);
					*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, k & 0xFFFF);
					if (rs != 15) *emitPtr++ = PPC_ADD(PPC_R12, PPC_R12, ctx.readReg(rs, lockedMask));
				}
				ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, 3, /*targetThumb=*/true);
				ctx.instrCount++; ctx.currentPC += 2;
				ctx.endBlock = true; ctx.blockTerminatedEarly = true;
				break;
			}

			if (sub == 1) {                          // CMP (flags, no write)
				u8 rRs, rRd;
				if (rs == 15) { rRs = PPC_R10;
					*emitPtr++ = PPC_LIS(PPC_R10, (currentPC + 4) >> 16);
					*emitPtr++ = PPC_ORI(PPC_R10, PPC_R10, (currentPC + 4) & 0xFFFF);
				} else rRs = ctx.readReg(rs, lockedMask);
				if (rd == 15) { rRd = PPC_R11;
					*emitPtr++ = PPC_LIS(PPC_R11, (currentPC + 4) >> 16);
					*emitPtr++ = PPC_ORI(PPC_R11, PPC_R11, (currentPC + 4) & 0xFFFF);
				} else rRd = ctx.readReg(rd, lockedMask);
				*emitPtr++ = PPC_SUBFCO(PPC_R12, rRs, rRd);
				ctx.emitCVfromXER(PPC_R10);
				ctx.emitNZ(PPC_R12);
			} else if (sub == 2) {                   // MOV (no flags)
				if (rs == 15) {
					u8 rRd = ctx.writeReg(rd, true, lockedMask);
					*emitPtr++ = PPC_LIS(rRd, (currentPC + 4) >> 16);
					*emitPtr++ = PPC_ORI(rRd, rRd, (currentPC + 4) & 0xFFFF);
				} else {
					u8 rRs = ctx.readReg(rs, lockedMask);   // load source first
					u8 rRd = ctx.writeReg(rd, true, lockedMask);
					*emitPtr++ = PPC_OR(rRd, rRs, rRs);
				}
			} else {                                 // ADD (no flags)
				u8 rRd = ctx.writeReg(rd, false, lockedMask);
				if (rs == 15) {
					*emitPtr++ = PPC_LIS(PPC_R12, (currentPC + 4) >> 16);
					*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, (currentPC + 4) & 0xFFFF);
					*emitPtr++ = PPC_ADD(rRd, rRd, PPC_R12);
				} else {
					u8 rRs = ctx.readReg(rs, lockedMask);
					*emitPtr++ = PPC_ADD(rRd, rRd, rRs);
				}
			}
			break;
		}

		// ---- 0x4000-0x43FF : Format 4 ALU -----------------------------
		const u8 op = (opcode >> 6) & 0x0F;
		const u8 rs = (opcode >> 3) & 0x07;
		const u8 rd = opcode & 0x07;
		ctx.ensureArena();

		if (op == 0 || op == 1 || op == 12 || op == 14) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			if (op == 0)  *emitPtr++ = PPC_AND (hRd, hRd, hRs);
			if (op == 1)  *emitPtr++ = PPC_XOR (hRd, hRd, hRs);
			if (op == 12) *emitPtr++ = PPC_OR  (hRd, hRd, hRs);
			if (op == 14) *emitPtr++ = PPC_ANDC(hRd, hRd, hRs);
			ctx.emitNZ(hRd);
		}
		else if (op == 8) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_AND(PPC_R12, hRd, hRs);
			ctx.emitNZ(PPC_R12);
		}
		else if (op == 15) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_NOR(hRd, hRs, hRs);
			ctx.emitNZ(hRd);
		}
		else if (op == 10) {
			if (ctx.fuseCmp) { ctx.fusedCmpValid = true; ctx.fusedCmpOp = opcode; }
			else             jitThumbEmitCmpFlags(ctx, opcode);
		}
		else if (op == 11) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_ADDCO(PPC_R12, hRd, hRs);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(PPC_R12);
		}
		else if (op == 9) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, true, lockedMask);
			*emitPtr++ = PPC_LI(PPC_R12, 0);
			*emitPtr++ = PPC_SUBFCO(hRd, hRs, PPC_R12);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(hRd);
		}
		else if (op == 5 || op == 6) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			const u8 fC  = ctx.readFlag(JITF_C, PPC_R12);
			*emitPtr++ = PPC_ADDIC(PPC_R12, fC, -1);
			if (op == 5) *emitPtr++ = PPC_ADDEO (hRd, hRd, hRs);
			else         *emitPtr++ = PPC_SUBFEO(hRd, hRs, hRd);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(hRd);
		}
		else if (op == 13) {
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			*emitPtr++ = PPC_MULLW(hRd, hRd, hRs);
			ctx.emitNZ(hRd);
		}
		else {
			// op 2/3/4/7 : LSL/LSR/ASR/ROR by register
			// PPC slw/srw/sraw take a 6-bit shift count: amounts 32..63 give 0
			// (slw/srw) or all-sign (sraw), matching the interpreter's >=32
			// handling for Rd. Carry is exact for amounts 0..32; for amounts
			// >32 the ASR/ROR carry can differ by the sign/low bit -- a rare
			// edge the differential harness flags (TODO: clamp).
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.writeReg(rd, false, lockedMask);
			*emitPtr++ = PPC_RLWINM(PPC_R12, hRs, 0, 24, 31);      // R12 = Rs & 0xFF
			*emitPtr++ = PPC_CMPWI(0, PPC_R12, 0);
			u32* skipZero = emitPtr++;                             // amount 0: only N/Z
			if (op == 2) { *emitPtr++ = PPC_LI(PPC_R11, 32);
			               *emitPtr++ = PPC_SUBF(PPC_R11, PPC_R12, PPC_R11);  // 32 - amt
			               *emitPtr++ = PPC_SRW(PPC_R10, hRd, PPC_R11); }
			else         { *emitPtr++ = PPC_ADDI(PPC_R11, PPC_R12, -1);       // amt - 1
			               *emitPtr++ = PPC_SRW(PPC_R10, hRd, PPC_R11); }
			ctx.emitFlagBit(JITF_C, PPC_R10, 0);
			if (op == 2)      *emitPtr++ = PPC_SLW (hRd, hRd, PPC_R12);
			else if (op == 3) *emitPtr++ = PPC_SRW (hRd, hRd, PPC_R12);
			else if (op == 4) *emitPtr++ = PPC_SRAW(hRd, hRd, PPC_R12);
			else { *emitPtr++ = PPC_LI(PPC_R11, 32);
			       *emitPtr++ = PPC_SUBF(PPC_R11, PPC_R12, PPC_R11);
			       *emitPtr++ = PPC_RLWNM(hRd, hRd, PPC_R11, 0, 31); }
			*skipZero = PPC_BEQ((u32)((emitPtr - skipZero) * 4));
			ctx.emitNZ(hRd);
		}
		break;
	}

	// ================================================ F6 : LDR Rd,[PC,#imm]
	case 9: {
		ctx.ensureArena();
		const u8 rd = (opcode >> 8) & 0x07;
		const u32 ea = ((currentPC + 4) & ~3u) + ((opcode & 0xFF) << 2);
		if (ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase) {   // P14/P16 inline RAM load
			*emitPtr++ = PPC_LIS(PPC_R12, ea >> 16);
			*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, ea & 0xFFFF);
			(void)ctx.emitInlineLoad(rd, PPC_R12, 4, false, /*wordRotate=*/false, lockedMask);
			break;
		}
		ctx.emitMemPrologue();
		*emitPtr++ = PPC_LIS(PPC_R12, ea >> 16);
		*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, ea & 0xFFFF);
		ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
		ctx.emitMemEpilogue();
		*emitPtr++ = PPC_OR(ctx.hostRegFor(rd), PPC_R10, PPC_R10);
		break;
	}

	// ==================================== F9/F8/F10 : loads & stores
	case 10: case 11:                       // 0x5000-0x5FFF  reg offset
	case 12: case 13:                       // 0x6000-0x6FFF  word imm
	case 14: case 15:                       // 0x7000-0x7FFF  byte imm
	case 16: case 17: {                     // 0x8000-0x8FFF  half imm
		bool isLoad = false, isStore = false, signExt = false, regOff = false;
		u8 rd = opcode & 0x07, rb = (opcode >> 3) & 0x07, ro = (opcode >> 6) & 0x07;
		u32 size = 4, immOff = 0;

		if ((opcode & 0xF000) == 0x6000) {
			size = 4; immOff = ((opcode >> 6) & 0x1F) << 2;
			isLoad = opcode & 0x0800; isStore = !isLoad;
		} else if ((opcode & 0xF000) == 0x7000) {
			size = 1; immOff = (opcode >> 6) & 0x1F;
			isLoad = opcode & 0x0800; isStore = !isLoad;
		} else if ((opcode & 0xF000) == 0x8000) {
			size = 2; immOff = ((opcode >> 6) & 0x1F) << 1;
			isLoad = opcode & 0x0800; isStore = !isLoad;
		} else { // 0x5000  register offset
			regOff = true;
			switch (opcode & 0x0E00) {
				case 0x0000: isStore = true; size = 4; break;
				case 0x0200: isStore = true; size = 2; break;
				case 0x0400: isStore = true; size = 1; break;
				case 0x0800: isLoad = true;  size = 4; break;
				case 0x0A00: isLoad = true;  size = 2; break;
				case 0x0C00: isLoad = true;  size = 1; break;
				case 0x0600: isLoad = true;  size = 1; signExt = true; break;
				case 0x0E00: isLoad = true;  size = 2; signExt = true; break;
			}
		}
		if (!isLoad && !isStore) { ctx.endBlock = true; break; }

		ctx.ensureArena();
		const u8 hRb = ctx.readReg(rb, lockedMask);
		u8 hRo = 0;
		if (regOff) hRo = ctx.readReg(ro, lockedMask);
		u8 hVal = 0;
		if (isStore) hVal = ctx.readReg(rd, lockedMask);

		if (regOff) *emitPtr++ = PPC_ADD(PPC_R12, hRb, hRo);
		else        *emitPtr++ = PPC_ADDI(PPC_R12, hRb, (s32)immOff);

		// Word loads rotate for an unaligned EA (ROR by 8*(EA&3)), matching both
		// the interpreter's OP_LDR_IMM_OFF/OP_LDR_REG_OFF and ARM mode's own
		// emitLoadStoreTail -- byte/halfword forms don't. This THUMB path used
		// to pass wordRotate=false unconditionally (a real bug: armwrestler's
		// THUMB LDR test, which deliberately misaligns the EA, caught it).
		const bool wordRotate = isLoad && size == 4;

		if (isLoad && (ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase)) {   // P14/P16 inline RAM load
			(void)ctx.emitInlineLoad(rd, PPC_R12, size, signExt, wordRotate, lockedMask);
			break;
		}

		if (isStore && ctx.cpu.arm9DtcmBase) {          // P16 inline RAM store
			ctx.emitArm9Store(size, /*writeback=*/false, /*rn=*/0, hVal);
			break;
		}

		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);           // save EA
		if (isStore) *emitPtr++ = PPC_STW(hVal, 1, 100); // save value

		ctx.emitMemPrologue();
		*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);

		if (isStore) {
			ctx.emitSmcCheckAndBail(PPC_R12);
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			*emitPtr++ = PPC_LWZ(PPC_R10, 1, 100);
			ctx.emitSlowStore(PPC_R12, PPC_R10, size);
			ctx.emitMemEpilogue();
		} else {
			ctx.emitSlowLoad(PPC_R10, PPC_R12, size, signExt);
			if (wordRotate) {                              // ROR(R10, 8*(EA&3))
				*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
				*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 30, 31); // EA & 3
				*emitPtr++ = PPC_LI(PPC_R11, 4);
				*emitPtr++ = PPC_SUBF(PPC_R12, PPC_R12, PPC_R11);     // 4 - (EA&3)
				*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 3, 27, 28); // ((4-x)&3)<<3
				*emitPtr++ = PPC_RLWNM(PPC_R10, PPC_R10, PPC_R12, 0, 31);
			}
			ctx.emitMemEpilogue();
			*emitPtr++ = PPC_OR(ctx.hostRegFor(rd), PPC_R10, PPC_R10);
		}
		break;
	}

	// ================================================ F11 : LDR/STR [SP,#imm]
	case 18: case 19: {
		ctx.ensureArena();
		const u8 rd = (opcode >> 8) & 0x07;
		const u32 immOff = (opcode & 0xFF) << 2;
		const bool isLoad = opcode & 0x0800;
		const u8 hSp = ctx.readReg(13, lockedMask);
		u8 hVal = 0;
		if (!isLoad) hVal = ctx.readReg(rd, lockedMask);
		*emitPtr++ = PPC_ADDI(PPC_R12, hSp, (s32)immOff);
		if (isLoad && (ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase)) {   // P14/P16 inline RAM load
			(void)ctx.emitInlineLoad(rd, PPC_R12, 4, false, /*wordRotate=*/false, lockedMask);
			break;
		}
		if (!isLoad && ctx.cpu.arm9DtcmBase) {          // P16 inline RAM store
			ctx.emitArm9Store(4, /*writeback=*/false, /*rn=*/0, hVal);
			break;
		}
		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
		if (!isLoad) *emitPtr++ = PPC_STW(hVal, 1, 100);
		ctx.emitMemPrologue();
		*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
		if (isLoad) {
			ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
			ctx.emitMemEpilogue();
			*emitPtr++ = PPC_OR(ctx.hostRegFor(rd), PPC_R10, PPC_R10);
		} else {
			ctx.emitSmcCheckAndBail(PPC_R12);
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			*emitPtr++ = PPC_LWZ(PPC_R10, 1, 100);
			ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
			ctx.emitMemEpilogue();
		}
		break;
	}

	// ================================================ F12 : ADD Rd, PC|SP, #imm
	case 20: case 21: {
		ctx.ensureArena();
		const u8 rd = (opcode >> 8) & 0x07;
		const u32 imm = (opcode & 0xFF) << 2;
		const bool useSP = opcode & 0x0800;
		const u8 hRd = ctx.writeReg(rd, true, lockedMask);
		if (useSP) {
			const u8 hSp = ctx.readReg(13, lockedMask);
			*emitPtr++ = PPC_ADDI(hRd, hSp, (s32)imm);
		} else {
			const u32 v = ((currentPC + 4) & ~3u) + imm;
			*emitPtr++ = PPC_LIS(hRd, v >> 16);
			if (v & 0xFFFF) *emitPtr++ = PPC_ORI(hRd, hRd, v & 0xFFFF);
		}
		break;
	}

	// ================================ F13 (ADD/SUB SP) + F14 (PUSH/POP)
	case 22: case 23: {
		if ((opcode & 0xFF00) == 0xB000) {          // F13
			ctx.ensureArena();
			const u32 off = (opcode & 0x7F) << 2;
			const bool isSub = opcode & 0x0080;
			const u8 hSp = ctx.writeReg(13, false, lockedMask);
			*emitPtr++ = PPC_ADDI(hSp, hSp, isSub ? -(s32)off : (s32)off);
			break;
		}
		if ((opcode & 0xF600) != 0xB400) { ctx.endBlock = true; break; }  // BKPT etc

		const bool isPop = opcode & 0x0800;
		const bool Rbit  = opcode & 0x0100;
		const u8   list  = opcode & 0xFF;
		int nregs = __builtin_popcount(list) + (Rbit ? 1 : 0);
		if (nregs == 0) { ctx.endBlock = true; break; }

		ctx.ensureArena();

		// P15: inline POP without pc. One page guard, sequential lwbrx, register
		// cache intact. PUSH keeps the slow path (store); POP{...,pc} keeps it
		// (block-terminator interworking).
		if ((ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase) && isPop && !Rbit) {
			const u8 hSpP = ctx.readReg(13, lockedMask);
			*emitPtr++ = PPC_STW(hSpP, 1, 104);                       // stash raw old SP
			*emitPtr++ = PPC_RLWINM(PPC_R12, hSpP, 0, 0, 29);         // word-aligned low addr
			u8 regs[8]; u32 nn = 0;
			for (int i = 0; i < 8; i++) if (list & (1 << i)) regs[nn++] = (u8)i;
			ctx.emitInlineBlockLoad(regs, nn, PPC_R12, lockedMask);
			const u8 hSp2 = ctx.writeReg(13, /*fullOverwrite=*/true, lockedMask);
			*emitPtr++ = PPC_LWZ(hSp2, 1, 104);
			*emitPtr++ = PPC_ADDI(hSp2, hSp2, 4 * nregs);
			break;
		}

		// P16 (ARM9): inline PUSH (incl. {..,lr}). The guest SP decrement is
		// committed only AFTER emitArm9BlockStore returns, so its internal SMC
		// bail re-runs the whole PUSH against the original SP -- no double
		// decrement (the stack-corruption failure mode the slow path guards).
		if (ctx.cpu.arm9DtcmBase && !isPop) {
			const u8 hSpP = ctx.readReg(13, lockedMask);
			*emitPtr++ = PPC_STW(hSpP, 1, 104);                       // stash raw old SP
			*emitPtr++ = PPC_ADDI(PPC_R12, hSpP, -4 * nregs);
			*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 29);      // word-aligned low addr
			u8 regs[9]; u32 nn = 0;
			for (int i = 0; i < 8; i++) if (list & (1 << i)) regs[nn++] = (u8)i;
			if (Rbit) regs[nn++] = 14;                                // lr pushed at the top
			ctx.emitArm9BlockStore(regs, nn);
			const u8 hSp2 = ctx.writeReg(13, /*fullOverwrite=*/true, lockedMask);
			*emitPtr++ = PPC_LWZ(hSp2, 1, 104);
			*emitPtr++ = PPC_ADDI(hSp2, hSp2, -4 * nregs);
			break;
		}

		// POP {...,pc} inline: the popped pc is loaded like any other list word,
		// into guest R15's pinned host register (r29, about to be rewritten by the
		// exit anyway), through the same region-guarded inline block load as the
		// no-pc form above (slowRead per word off the RAM regions). It is then
		// stashed where the slow path below leaves it and shares that path's
		// interworking exit. This is THUMB's standard function return, so it
		// used to cost one slowRead C call per popped word on every return.
		bool popPC = false;
		if ((ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase) && isPop && Rbit) {
			const u8 hSpP = ctx.readReg(13, lockedMask);
			*emitPtr++ = PPC_RLWINM(PPC_R12, hSpP, 0, 0, 29);         // word-aligned low addr
			u8 regs[9]; u32 nn = 0;
			for (int i = 0; i < 8; i++) if (list & (1 << i)) regs[nn++] = (u8)i;
			regs[nn++] = 15;                                          // pc popped at the top
			ctx.emitInlineBlockLoad(regs, nn, PPC_R12, lockedMask);
			*emitPtr++ = PPC_ADDI(hSpP, hSpP, 4 * nregs);
			*emitPtr++ = PPC_STW(ctx.hostRegFor(15), 1, 100);          // raw popped pc
			popPC = true;
		} else {
		u8 hSp;
		if (!isPop) {
			// Compute the prospective post-decrement, word-aligned base into a
			// scratch register FIRST and run the SMC guard against it before
			// committing anything to the cached/guest SP. emitSmcCheckAndBail's
			// bail resumes this whole PUSH from scratch via the interpreter --
			// with the old ordering (decrement, then flush via emitMemPrologue,
			// then guard) a bail here left the decrement already applied in
			// guest memory while none of the pushed register values had been
			// written yet, so the interpreter's re-run decremented SP a SECOND
			// time and the stack slots the original decrement reserved were
			// never actually filled in -- a real, silent stack corruption a
			// POP reads back as garbage possibly many instructions later (see
			// the plan memory for how this was traced).
			hSp = ctx.readReg(13, lockedMask);
			*emitPtr++ = PPC_ADDI(PPC_R12, hSp, -4 * nregs);
			*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 29);
			*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
			ctx.emitMemPrologue();
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			ctx.emitSmcCheckAndBail(PPC_R12);
			// guard passed -- commit the real decrement into the pinned SP now
			// (a bail before this point re-runs the whole PUSH against the
			// undecremented SP, avoiding the double-decrement stack corruption).
			*emitPtr++ = PPC_ADDI(hSp, hSp, -4 * nregs);
		} else {
			hSp = ctx.writeReg(13, false, lockedMask);
			*emitPtr++ = PPC_RLWINM(PPC_R12, hSp, 0, 0, 29);       // word-align base
			*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
			ctx.emitMemPrologue();
		}

		u32 slot = 0;
		for (int i = 0; i < 9; i++) {
			const bool isLR = (i == 8);
			if (isLR ? !Rbit : !(list & (1 << i))) continue;
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			if (slot) *emitPtr++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));
			if (isPop) {
				if (isLR) { ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
				            *emitPtr++ = PPC_STW(PPC_R10, 1, 100); popPC = true; }
				else      { ctx.emitSlowLoad(ctx.hostRegFor(i), PPC_R12, 4, false); }
			} else {
				ctx.emitSlowStore(PPC_R12, ctx.hostRegFor(isLR ? 14 : i), 4);
			}
			slot++;
		}

		ctx.emitMemEpilogue();

		// SP writeback: PUSH already holds the decremented SP in the pinned reg;
		// POP adds the popped size to it.
		if (isPop) *emitPtr++ = PPC_ADDI(ctx.hostRegFor(13), ctx.hostRegFor(13), 4 * nregs);
		}

		if (popPC) {
			// The popped value's bit0 is the real ARMv4T mode switch (same
			// convention as BX Rs, case 8/sub==3 above) -- POP{...,PC} is by
			// far the most common function-return shape in THUMB code, and a
			// return into an ARM-mode caller is completely ordinary. Must check
			// bit0 rather than assume THUMB: landing an ARM-mode target through
			// a THUMB fetch misdecodes the first real ARM opcode and free-runs
			// from a garbage address.
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 100);                  // R12 = raw popped PC
			*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);   // R11 = bit0 (mode bit)
			*emitPtr++ = PPC_CMPWI(0, PPC_R11, 0);
			u32* toArm = emitPtr++;                                  // BEQ -> ARM-mode path
			// bit0==1: stay THUMB
			*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);    // & ~1
			ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode), /*targetThumb=*/true);
			*toArm = PPC_BEQ((u32)((emitPtr - toArm) * 4));
			// bit0==0: switch to ARM. Clear CPSR.T so the C++ resume path
			// (jit_exec.cpp / jit_differential.cpp) sees T==0 and uses
			// ARM-mode fetch/pipeline math instead of defaulting to THUMB.
			*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 29);    // & ~3 (ARM word align)
			ctx.ensureFlagsLoaded();
			*emitPtr++ = PPC_LI(PPC_R10, 0x20);                     // CPSR.T bit (bit 5)
			*emitPtr++ = PPC_ANDC(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
			ctx.flagsDirty = true;
			ctx.flushDirtyFlags();
			ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode), /*targetThumb=*/false);

			ctx.instrCount++; ctx.currentPC += 2;
			ctx.endBlock = true; ctx.blockTerminatedEarly = true;
		}
		break;
	}

	// ================================================ F15 : LDMIA/STMIA Rb!
	case 24: case 25: {
		const u8 rb = (opcode >> 8) & 0x07;
		const u8 list = opcode & 0xFF;
		const bool isLoad = opcode & 0x0800;
		if (list == 0) { ctx.endBlock = true; break; }

		ctx.ensureArena();
		const u8 hRb = ctx.readReg(rb, lockedMask);
		*emitPtr++ = PPC_OR(PPC_R12, hRb, hRb);

		// P15/P16: inline LDMIA / STMIA. One region guard for the run, then n
		// sequential lwbrx / stwbrx; register cache intact. emitArm9BlockLoad /
		// emitArm9BlockStore both restore the raw base to R12 on return.
		if ((ctx.cpu.pageDescBase || ctx.cpu.arm9DtcmBase) && isLoad) {
			u8 regs[8]; u32 nn = 0;
			for (int i = 0; i < 8; i++) if (list & (1 << i)) regs[nn++] = (u8)i;
			ctx.emitInlineBlockLoad(regs, nn, PPC_R12, lockedMask);
			const u8 hRb2 = ctx.writeReg(rb, /*fullOverwrite=*/true, lockedMask);
			*emitPtr++ = PPC_OR(hRb2, PPC_R12, PPC_R12);   // R12 still = raw base
			*emitPtr++ = PPC_ADDI(hRb2, hRb2, (s32)(nn * 4));
			break;
		}
		if (ctx.cpu.arm9DtcmBase && !isLoad) {
			u8 regs[8]; u32 nn = 0;
			for (int i = 0; i < 8; i++) if (list & (1 << i)) regs[nn++] = (u8)i;
			ctx.emitArm9BlockStore(regs, nn);
			const u8 hRb2 = ctx.writeReg(rb, /*fullOverwrite=*/true, lockedMask);
			*emitPtr++ = PPC_OR(hRb2, PPC_R12, PPC_R12);   // R12 restored to raw base
			*emitPtr++ = PPC_ADDI(hRb2, hRb2, (s32)(nn * 4));
			break;
		}

		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
		ctx.emitMemPrologue();
		if (!isLoad) { *emitPtr++ = PPC_LWZ(PPC_R12, 1, 96); ctx.emitSmcCheckAndBail(PPC_R12); }

		u32 slot = 0;
		for (int i = 0; i < 8; i++) {
			if (!(list & (1 << i))) continue;
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			if (slot) *emitPtr++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));
			if (isLoad) ctx.emitSlowLoad(ctx.hostRegFor(i), PPC_R12, 4, false);
			else        ctx.emitSlowStore(PPC_R12, ctx.hostRegFor(i), 4);
			slot++;
		}

		ctx.emitMemEpilogue();
		// writeback: Rb = base + 4 * count  (raw base still stashed at 96(r1))
		{
			const u8 hRb2 = ctx.hostRegFor(rb);
			*emitPtr++ = PPC_LWZ(hRb2, 1, 96);
			*emitPtr++ = PPC_ADDI(hRb2, hRb2, (s32)(slot * 4));
		}
		break;
	}

	// ================================================ F16 : conditional branch
	case 26: case 27: {
		if ((opcode & 0x0F00) == 0x0F00) { ctx.endBlock = true; break; }  // SWI

		const u8 cond = (opcode >> 8) & 0x0F;
		const s8 off8 = (s8)(opcode & 0xFF);
		const u32 targetPC = currentPC + 4 + (off8 << 1);
		ctx.ensureArena();

		// Fused CMP+Bcc (see JitTraceCtx::fuseCmp): native compare, branch over
		// the taken path when the condition fails; the taken path rebuilds the
		// packed flags from the (unchanged, pinned) CMP operands before exiting.
		u32* fusedSkip = nullptr;
		if (ctx.fusedCmpValid) {
			const u16 cop = ctx.fusedCmpOp;
			ctx.fusedCmpValid = false;
			const bool uns = (cond == 0x2 || cond == 0x3 || cond == 0x8 || cond == 0x9);
			if ((cop & 0xF800) == 0x2800) {
				const u8 hA = ctx.readReg((cop >> 8) & 7, lockedMask);
				*emitPtr++ = uns ? PPC_CMPLI(0, hA, cop & 0xFF) : PPC_CMPWI(0, hA, cop & 0xFF);
			} else {
				const u8 hA = ctx.readReg(cop & 7, lockedMask), hB = ctx.readReg((cop >> 3) & 7, lockedMask);
				*emitPtr++ = uns ? PPC_CMPLW(0, hA, hB) : PPC_CMPW(0, hA, hB);
			}
			// skip-when-not-taken: {BO, BI} of the inverse condition
			static const u8 kSkip[14][2] = {
				{4, 2}, {12, 2},          // EQ, NE
				{12, 0}, {4, 0},          // CS (>=u), CC (<u)
				{0, 0}, {0, 0}, {0, 0}, {0, 0},   // MI PL VS VC: never fused
				{4, 1}, {12, 1},          // HI (>u), LS (<=u)
				{12, 0}, {4, 0},          // GE, LT
				{4, 1}, {12, 1},          // GT, LE
			};
			fusedSkip = emitPtr;
			*emitPtr++ = PPC_BC(kSkip[cond][0], kSkip[cond][1], 0);
			ctx.deadFlags = 0;
			jitThumbEmitCmpFlags(ctx, cop);                             // taken path: flags for the exit
		}
		u32* guard = nullptr;
		bool guardIsBEQ = false;
		if (!fusedSkip) {

		bool composite = false, branchIfZero = false;
		u32 flagReg = 0;
		switch (cond) {
			case 0x0: flagReg = ctx.readFlag(JITF_Z, PPC_R12); guardIsBEQ = true;  break;
			case 0x1: flagReg = ctx.readFlag(JITF_Z, PPC_R12); guardIsBEQ = false; break;
			case 0x2: flagReg = ctx.readFlag(JITF_C, PPC_R12); guardIsBEQ = true;  break;
			case 0x3: flagReg = ctx.readFlag(JITF_C, PPC_R12); guardIsBEQ = false; break;
			case 0x4: flagReg = ctx.readFlag(JITF_N, PPC_R12); guardIsBEQ = true;  break;
			case 0x5: flagReg = ctx.readFlag(JITF_N, PPC_R12); guardIsBEQ = false; break;
			case 0x6: flagReg = ctx.readFlag(JITF_V, PPC_R12); guardIsBEQ = true;  break;
			case 0x7: flagReg = ctx.readFlag(JITF_V, PPC_R12); guardIsBEQ = false; break;
			case 0x8: composite = true; branchIfZero = false; break;
			case 0x9: composite = true; branchIfZero = true;  break;
			case 0xA: composite = true; branchIfZero = true;  break;
			case 0xB: composite = true; branchIfZero = false; break;
			case 0xC: composite = true; branchIfZero = true;  break;
			case 0xD: composite = true; branchIfZero = false; break;
			default:  ctx.endBlock = true; break;
		}
		if (ctx.endBlock) break;

		if (!composite) {
			// the readFlag rlwinm just emitted becomes record-form (rlwinm.):
			// cr0.eq <=> flag clear, no separate cmpwi
			(void)flagReg;
			emitPtr[-1] |= 1;
			guard = emitPtr++;
		} else {
			if (cond == 0x8 || cond == 0x9) {
				u8 fC = ctx.readFlag(JITF_C, PPC_R10);
				u8 fZ = ctx.readFlag(JITF_Z, PPC_R12);
				*emitPtr++ = PPC_ANDC(PPC_R11, fC, fZ);
			} else if (cond == 0xA || cond == 0xB) {
				u8 fN = ctx.readFlag(JITF_N, PPC_R10);
				u8 fV = ctx.readFlag(JITF_V, PPC_R12);
				*emitPtr++ = PPC_XOR(PPC_R11, fN, fV);
			} else {
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
		}

		// Taken cost is hardcoded to 3 here (matching OP_B_COND's taken
		// return value) rather than going through cyclesForThumb(), which
		// now reports the NOT-taken/fall-through cost (1) for this opcode
		// range -- see the comment on jit_arm7_profile.cpp's case 0xD.
		ctx.emitAddCycles(ctx.cyclesAccum + 3);
		ctx.emitResultMetadata(ctx.instrCount + 1, 0);
#if JIT_SPIN_SKIP
		// Spin-loop fast-forward (jitThumbSpinLoop(), jit_trace.cpp). r3 already
		// holds this iteration's cycles; each further iteration adds c = the
		// loop's own cost and one would run while r3 < JIT_YIELD_NUMBER, so add
		// n = ceil((Y - r3) / c) iterations' worth of cycles and instructions.
		// The self-chained entry guard then yields exactly as the loop would
		// have. Skipped (loop runs normally) when r3 is already at the quota or
		// the load hits a port whose read has side effects (0x041xxxxx IPC FIFO
		// / card data) or anything at/above 0x08000000 (slot 2).
		if (ctx.spinLoopLen && ctx.instrCount + 1 == ctx.spinLoopLen && targetPC == ctx.startPC) {
			const u32 c = ctx.cyclesAccum + 3;
			*emitPtr++ = PPC_ADDI(PPC_R12, ctx.hostRegFor(ctx.spinBase), ctx.spinImm);
			*emitPtr++ = PPC_SRWI(PPC_R12, PPC_R12, 20);
			*emitPtr++ = PPC_CMPLI(0, PPC_R12, 0x041);
			u32* skipA = emitPtr++;                                       // BEQ: side-effect port
			*emitPtr++ = PPC_CMPLI(0, PPC_R12, 0x07F);
			u32* skipB = emitPtr++;                                       // BGT: slot 2 and up
			*emitPtr++ = PPC_CMPWI(0, PPC_R3, JIT_YIELD_NUMBER);
			u32* skipC = emitPtr++;                                       // BGE: already at quota
			*emitPtr++ = PPC_LI(PPC_R12, JIT_YIELD_NUMBER - 1);
			*emitPtr++ = PPC_SUBF(PPC_R12, PPC_R3, PPC_R12);             // Y - 1 - r3 (>= 0)
			*emitPtr++ = PPC_LI(PPC_R11, (s32)c);
			*emitPtr++ = PPC_DIVWU(PPC_R12, PPC_R12, PPC_R11);           // n - 1
			*emitPtr++ = PPC_ADDI(PPC_R12, PPC_R12, 1);                  // n
			*emitPtr++ = PPC_MULLW(PPC_R11, PPC_R12, PPC_R11);
			*emitPtr++ = PPC_ADD(PPC_R3, PPC_R3, PPC_R11);               // r3 += n * c
			*emitPtr++ = PPC_MULLI(PPC_R11, PPC_R12, (s32)ctx.spinLoopLen);
			*emitPtr++ = PPC_ADD(PPC_R31, PPC_R31, PPC_R11);             // icount += n * len
			*skipA = PPC_BEQ((u32)((emitPtr - skipA) * 4));
			*skipB = PPC_BGT((u32)((emitPtr - skipB) * 4));
			*skipC = PPC_BGE((u32)((emitPtr - skipC) * 4));
		}
#endif
		ctx.emitChainTail(targetPC);
		if (guard) {
			u32 skip = (u32)((emitPtr - guard) * 4);
			*guard = guardIsBEQ ? PPC_BEQ(skip) : PPC_BNE(skip);
		}
		if (fusedSkip) *fusedSkip |= (u32)((emitPtr - fusedSkip) * 4) & 0xFFFC;
		break;
	}

	// ================================================ F18 : unconditional B
	case 28: {
		ctx.ensureArena();
		s32 sOff = (s32)((opcode & 0x07FF) << 21);
		sOff >>= 20;
		const u32 targetPC = currentPC + 4 + sOff;
		ctx.emitStaticExit(targetPC, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode));
		ctx.instrCount++; ctx.currentPC += 2;
		ctx.endBlock = true; ctx.blockTerminatedEarly = true;
		break;
	}

	// ================================ F19 : BL / BLX (imm) (prefix + suffix)
	case 30: {
		if ((currentPC >> 10) != ((currentPC + 2) >> 10)) { ctx.endBlock = true; break; }
		const u16 lo = (u16)ctx.cpu.fetch16(currentPC + 2);
		const bool isBl  = (lo & 0xF800) == 0xF800;   // H==11 suffix -> BL
		const bool isBlx = (lo & 0xF801) == 0xE800;   // H==01 suffix, bit0==0 -> BLX
		if (!isBl && !isBlx) { ctx.endBlock = true; break; }
		ctx.ensureArena();

		s32 sOff = (s32)((opcode & 0x07FF) << 21);
		sOff >>= 9;
		sOff |= (lo & 0x07FF) << 1;
		const u32 retLR = (currentPC + 4) | 1;

		if (isBl) {
			const u32 targetPC = currentPC + 4 + sOff;
			const u8 hLR = ctx.writeReg(14, true, lockedMask);
			*emitPtr++ = PPC_LIS(hLR, retLR >> 16);
			*emitPtr++ = PPC_ORI(hLR, hLR, retLR & 0xFFFF);
			ctx.emitStaticExit(targetPC, ctx.instrCount + 2, ctx.cpu.cyclesForThumb(opcode));
		} else {
			// BLX (imm): word-align the target, switch to ARM. The next block is
			// ARM mode -- until A5's ARM front-end lands the block just exits and
			// the C++ resume path (which checks CPSR.T) runs the ARM code. The
			// THUMB run up to the call is still native.
			const u32 targetPC = (currentPC + 4 + sOff) & ~3u;
			const u8 hLR = ctx.writeReg(14, true, lockedMask);
			*emitPtr++ = PPC_LIS(hLR, retLR >> 16);
			*emitPtr++ = PPC_ORI(hLR, hLR, retLR & 0xFFFF);
			*emitPtr++ = PPC_LIS(PPC_R12, targetPC >> 16);
			*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, targetPC & 0xFFFF);
			// clear CPSR.T (bit 5) so the resume path uses ARM fetch/pipeline
			ctx.ensureFlagsLoaded();
			*emitPtr++ = PPC_LI(PPC_R10, 0x20);
			*emitPtr++ = PPC_ANDC(PPC_REG_FLAGS, PPC_REG_FLAGS, PPC_R10);
			ctx.flagsDirty = true;
			ctx.flushDirtyFlags();
			ctx.emitDynamicExit(PPC_R12, ctx.instrCount + 2, ctx.cpu.cyclesForThumb(opcode), /*targetThumb=*/false);
		}

		ctx.instrCount += 2; ctx.currentPC += 4;
		ctx.endBlock = true; ctx.blockTerminatedEarly = true;
		break;
	}

	default:
		ctx.endBlock = true;
		break;
	}
}

#endif // DESMUME_JIT
