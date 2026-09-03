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

#if defined(DESMUME_JIT_ARM7)

#include "jit_ppc_emitter.h"

// --- exit helpers --------------------------------------------------------
// Static-target exit: chain through the linker stub (self-patches on a hit).
static void emitStaticExit(JitTraceCtx& ctx, u32 targetPC, u32 metaCount, u32 termCycles)
{
	u32*& p = ctx.emitPtr;
	JITCache& cache = ctx.cache;
	ctx.emitAddCycles(ctx.cyclesAccum + termCycles);
	ctx.flushDirtyFlags();
	ctx.flushDirtyRegisters();
	ctx.emitResultMetadata(metaCount, 0);
	*p++ = PPC_LIS(PPC_R29, (targetPC + 4) >> 16);
	*p++ = PPC_ORI(PPC_R29, PPC_R29, (targetPC + 4) & 0xFFFF);
	*p++ = PPC_LIS(PPC_R4, targetPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
#if JIT_ENABLE_CHAINING
	{ s32 o = (s32)((u8*)cache.linkerStubAddress - (u8*)p); *p++ = PPC_BL(o); }
#endif
	{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)p); *p++ = PPC_B(o); }
}

// Dynamic-target exit: `pcReg` holds the runtime thumb PC (already & ~1).
// Returns to the C++ dispatcher (no chaining -- target unknown at compile time).
// pcReg must survive the register flush (use a scratch: r10..r12).
static void emitDynamicExit(JitTraceCtx& ctx, u8 pcReg, u32 metaCount, u32 termCycles)
{
	u32*& p = ctx.emitPtr;
	JITCache& cache = ctx.cache;
	ctx.emitAddCycles(ctx.cyclesAccum + termCycles);
	ctx.flushDirtyFlags();
	ctx.flushDirtyRegisters();
	ctx.emitResultMetadata(metaCount, 0);
	*p++ = PPC_OR(PPC_R29, pcReg, pcReg);
	*p++ = PPC_OR(PPC_R4, pcReg, pcReg);
	s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)p);
	*p++ = PPC_B(retOff);
}

// Bail to the interpreter at ctx.currentPC (this instruction re-run there).
static void emitInterpreterBail(JitTraceCtx& ctx, u32 metaCount)
{
	u32*& p = ctx.emitPtr;
	JITCache& cache = ctx.cache;
	ctx.flushDirtyFlags();
	ctx.flushDirtyRegisters();
	ctx.emitAddCycles(ctx.cyclesAccum);
	ctx.emitResultMetadata(metaCount, 1);
	*p++ = PPC_LIS(PPC_R4, ctx.currentPC >> 16);
	*p++ = PPC_ORI(PPC_R4, PPC_R4, ctx.currentPC & 0xFFFF);
	s32 retOff = (s32)((u8*)cache.linkerReturnAddress - (u8*)p);
	*p++ = PPC_B(retOff);
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
		if (type < 2) *emitPtr++ = PPC_OR(PPC_R12, hRn, hRn);
		else          *emitPtr++ = PPC_LI(PPC_R12, rn_imm);
		if (type == 0 || type == 2) *emitPtr++ = PPC_ADDCO(hRd, hRs, PPC_R12);
		else                        *emitPtr++ = PPC_SUBFCO(hRd, PPC_R12, hRs);
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
			u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_LI(PPC_R12, imm);
			if (op == 1)      { *emitPtr++ = PPC_SUBFCO(PPC_R11, PPC_R12, hRd); }
			else if (op == 2) { hRd = ctx.writeReg(rd, false, lockedMask);
			                    *emitPtr++ = PPC_ADDCO(hRd, hRd, PPC_R12); }
			else              { hRd = ctx.writeReg(rd, false, lockedMask);
			                    *emitPtr++ = PPC_SUBFCO(hRd, PPC_R12, hRd); }
			ctx.emitCVfromXER(PPC_R10);
			ctx.emitNZ((op == 1) ? PPC_R11 : hRd);
		}
		break;
	}

	// ============================================ F4 (0x4000-0x43FF) / F5+BX
	case 8: {
		if (opcode & 0x0400) {
			// ---- 0x4400-0x47FF : Format 5 hi-reg ops + BX --------------
			const u8 sub = (opcode >> 8) & 0x03;   // 0=ADD 1=CMP 2=MOV 3=BX
			ctx.ensureArena();

			if (sub == 3) {                          // BX Rs
				const u8 rs = (opcode >> 3) & 0x0F;
				if (rs == 15) {
					u32 v = (currentPC + 4) & ~1u;
					*emitPtr++ = PPC_LIS(PPC_R12, v >> 16);
					*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, v & 0xFFFF);
				} else {
					u8 hRs = ctx.readReg(rs, lockedMask);
					*emitPtr++ = PPC_OR(PPC_R12, hRs, hRs);
				}
				*emitPtr++ = PPC_RLWINM(PPC_R11, PPC_R12, 0, 31, 31);  // bit0
				*emitPtr++ = PPC_CMPWI(0, PPC_R11, 0);
				u32* toArm = emitPtr++;                                 // BEQ -> ARM bail
				*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);   // & ~1
				emitDynamicExit(ctx, PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode));
				*toArm = PPC_BEQ((u32)((emitPtr - toArm) * 4));
				emitInterpreterBail(ctx, ctx.instrCount);              // bit0==0: ARM switch
				ctx.instrCount++; ctx.currentPC += 2;
				ctx.endBlock = true; ctx.blockTerminatedEarly = true;
				break;
			}

			const u8 h1 = (opcode >> 7) & 1, h2 = (opcode >> 6) & 1;
			const u8 rs = ((opcode >> 3) & 0x07) | (h2 << 3);
			const u8 rd = (opcode & 0x07) | (h1 << 3);

			if (rd == 15 && sub != 1) { ctx.endBlock = true; break; }  // PC write = branch

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
			const u8 hRs = ctx.readReg(rs, lockedMask);
			const u8 hRd = ctx.readReg(rd, lockedMask);
			*emitPtr++ = PPC_SUBFCO(PPC_R12, hRs, hRd);
			ctx.emitCVfromXER(PPC_R11);
			ctx.emitNZ(PPC_R12);
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
		else {   // op 2/3/4/7 : LSL/LSR/ASR/ROR by register
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
		ctx.emitMemPrologue();
		*emitPtr++ = PPC_LIS(PPC_R12, ea >> 16);
		*emitPtr++ = PPC_ORI(PPC_R12, PPC_R12, ea & 0xFFFF);
		ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		*emitPtr++ = PPC_STW(PPC_R10, 14, rd * 4);
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
			ctx.invalidateRegCache();
		} else {
			ctx.emitSlowLoad(PPC_R10, PPC_R12, size, signExt);
			ctx.emitMemEpilogue();
			ctx.invalidateRegCache();
			*emitPtr++ = PPC_STW(PPC_R10, 14, rd * 4);
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
		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
		if (!isLoad) *emitPtr++ = PPC_STW(hVal, 1, 100);
		ctx.emitMemPrologue();
		*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
		if (isLoad) {
			ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
			ctx.emitMemEpilogue();
			ctx.invalidateRegCache();
			*emitPtr++ = PPC_STW(PPC_R10, 14, rd * 4);
		} else {
			ctx.emitSmcCheckAndBail(PPC_R12);
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			*emitPtr++ = PPC_LWZ(PPC_R10, 1, 100);
			ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
			ctx.emitMemEpilogue();
			ctx.invalidateRegCache();
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
		const u8 hSp = ctx.writeReg(13, false, lockedMask);
		if (!isPop) *emitPtr++ = PPC_ADDI(hSp, hSp, -4 * nregs);   // pre-decrement
		*emitPtr++ = PPC_RLWINM(PPC_R12, hSp, 0, 0, 29);           // word-align base
		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
		ctx.emitMemPrologue();
		if (!isPop) { *emitPtr++ = PPC_LWZ(PPC_R12, 1, 96); ctx.emitSmcCheckAndBail(PPC_R12); }

		bool popPC = false;
		u32 slot = 0;
		for (int i = 0; i < 9; i++) {
			const bool isLR = (i == 8);
			if (isLR ? !Rbit : !(list & (1 << i))) continue;
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			if (slot) *emitPtr++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));
			if (isPop) {
				ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
				if (isLR) { *emitPtr++ = PPC_STW(PPC_R10, 1, 100); popPC = true; }
				else      { *emitPtr++ = PPC_STW(PPC_R10, 14, i * 4); }
			} else {
				*emitPtr++ = PPC_LWZ(PPC_R10, 14, (isLR ? 14 : i) * 4);
				ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
			}
			slot++;
		}

		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();

		// SP writeback (PUSH: decremented value already in gpr[13]; POP: += size)
		{
			const u8 hSp2 = ctx.writeReg(13, true, lockedMask);
			*emitPtr++ = PPC_LWZ(hSp2, 14, 13 * 4);
			if (isPop) *emitPtr++ = PPC_ADDI(hSp2, hSp2, 4 * nregs);
		}

		if (popPC) {
			ctx.flushDirtyRegisters();
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 100);
			*emitPtr++ = PPC_RLWINM(PPC_R12, PPC_R12, 0, 0, 30);   // & ~1 (ARMv4T)
			emitDynamicExit(ctx, PPC_R12, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode));
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
		*emitPtr++ = PPC_STW(PPC_R12, 1, 96);
		ctx.emitMemPrologue();
		if (!isLoad) { *emitPtr++ = PPC_LWZ(PPC_R12, 1, 96); ctx.emitSmcCheckAndBail(PPC_R12); }

		u32 slot = 0;
		for (int i = 0; i < 8; i++) {
			if (!(list & (1 << i))) continue;
			*emitPtr++ = PPC_LWZ(PPC_R12, 1, 96);
			if (slot) *emitPtr++ = PPC_ADDI(PPC_R12, PPC_R12, (s32)(slot * 4));
			if (isLoad) {
				ctx.emitSlowLoad(PPC_R10, PPC_R12, 4, false);
				*emitPtr++ = PPC_STW(PPC_R10, 14, i * 4);
			} else {
				*emitPtr++ = PPC_LWZ(PPC_R10, 14, i * 4);
				ctx.emitSlowStore(PPC_R12, PPC_R10, 4);
			}
			slot++;
		}

		ctx.emitMemEpilogue();
		ctx.invalidateRegCache();
		// writeback: Rb += 4 * count
		{
			const u8 hRb2 = ctx.writeReg(rb, true, lockedMask);
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

		bool composite = false, branchIfZero = false, guardIsBEQ = false;
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

		u32* guard;
		if (!composite) {
			*emitPtr++ = PPC_CMPWI(0, flagReg, 0);
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

		ctx.emitAddCycles(ctx.cyclesAccum + ctx.cpu.cyclesForThumb(opcode));
		ctx.emitDirtyFlagFlush();
		ctx.emitDirtyRegisterFlush();
		ctx.emitResultMetadata(ctx.instrCount + 1, 0);
		*emitPtr++ = PPC_LIS(PPC_R29, (targetPC + 4) >> 16);
		*emitPtr++ = PPC_ORI(PPC_R29, PPC_R29, (targetPC + 4) & 0xFFFF);
		*emitPtr++ = PPC_LIS(PPC_R4, targetPC >> 16);
		*emitPtr++ = PPC_ORI(PPC_R4, PPC_R4, targetPC & 0xFFFF);
#if JIT_ENABLE_CHAINING
		{ s32 o = (s32)((u8*)cache.linkerStubAddress - (u8*)emitPtr); *emitPtr++ = PPC_BL(o); }
#endif
		{ s32 o = (s32)((u8*)cache.linkerReturnAddress - (u8*)emitPtr); *emitPtr++ = PPC_B(o); }
		{
			u32 skip = (u32)((emitPtr - guard) * 4);
			*guard = guardIsBEQ ? PPC_BEQ(skip) : PPC_BNE(skip);
		}
		break;
	}

	// ================================================ F18 : unconditional B
	case 28: {
		ctx.ensureArena();
		s32 sOff = (s32)((opcode & 0x07FF) << 21);
		sOff >>= 20;
		const u32 targetPC = currentPC + 4 + sOff;
		emitStaticExit(ctx, targetPC, ctx.instrCount + 1, ctx.cpu.cyclesForThumb(opcode));
		ctx.instrCount++; ctx.currentPC += 2;
		ctx.endBlock = true; ctx.blockTerminatedEarly = true;
		break;
	}

	// ================================================ F19 : BL (prefix+suffix)
	case 30: {
		if ((currentPC >> 10) != ((currentPC + 2) >> 10)) { ctx.endBlock = true; break; }
		const u16 lo = (u16)ctx.cpu.fetch16(currentPC + 2);
		if ((lo & 0xF800) != 0xF800) { ctx.endBlock = true; break; }
		ctx.ensureArena();

		s32 sOff = (s32)((opcode & 0x07FF) << 21);
		sOff >>= 9;
		sOff |= (lo & 0x07FF) << 1;
		const u32 targetPC = currentPC + 4 + sOff;
		const u32 retLR = (currentPC + 4) | 1;

		const u8 hLR = ctx.writeReg(14, true, lockedMask);
		*emitPtr++ = PPC_LIS(hLR, retLR >> 16);
		*emitPtr++ = PPC_ORI(hLR, hLR, retLR & 0xFFFF);

		emitStaticExit(ctx, targetPC, ctx.instrCount + 2, ctx.cpu.cyclesForThumb(opcode));
		ctx.instrCount += 2; ctx.currentPC += 4;
		ctx.endBlock = true; ctx.blockTerminatedEarly = true;
		break;
	}

	default:
		ctx.endBlock = true;
		break;
	}
}

#endif // DESMUME_JIT_ARM7
