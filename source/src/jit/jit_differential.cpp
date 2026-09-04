/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_differential.cpp  -- see jit_differential.h
 *
 * Derived from VBA-GX's JITDifferential.cpp (c) Daryl Borth, GPL v2+.
 ***************************************************************************/

#include "jit_differential.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

#include "jit_trace.h"
#include "../armcpu.h"
#include "../MMU.h"
#include <string.h>
#include <stdio.h>

// ===========================================================================
// A1: guest-memory journal
// ---------------------------------------------------------------------------
// The interpreter "reference" run in jitRunArm7Checked() executes the REAL
// armcpu_exec<ARMCPU_ARM7>() against the actual shared guest memory. Before A1
// that pass was not sandboxed: a store in the block performed a real,
// permanent write, and the JIT re-run afterward then read memory the reference
// pass had already mutated. Any code that reads back what it just wrote (a
// counter, a PRNG seed, anything self-referential) got its update
// double-applied, showing up as a deterministic register mismatch that looked
// like a JIT bug but was harness corruption. The old mitigation was to simply
// *not compare* any block that contained a store opcode -- which on the game
// core (ARM9, A2+) would blind the harness to most real code.
//
// A1's fix: the three MMU write choke points (_MMU_write08/16/32, MMU.h) call
// jitDiffJournalNote() *before* each store while the journal is armed. Each
// touched RAM byte's pre-write value is recorded; after the reference run the
// journal is replayed in reverse to restore memory exactly, then the JIT runs
// against pristine state and the comparison is trusted.
//
// Bound (not fixed): writes outside restorable RAM (0x02 main RAM, 0x03 shared
// WRAM / ARM7 WRAM) -- i.e. I/O registers, IPC FIFO, VRAM -- cannot be rolled
// back and often have read side effects, so any block whose reference run
// touches one sets s_journalUnrestorable and is not trusted for that dispatch.
// This is strictly less conservative than the old opcode scan: a block that
// only stores to RAM is now fully compared.
// ===========================================================================

struct DiffWrite { u8* cell; u8 old; };
static const u32 JIT_DIFF_JOURNAL_MAX = 8192;   // bytes; ~512 word stores
static DiffWrite s_journal[JIT_DIFF_JOURNAL_MAX];
static u32  s_journalN            = 0;
static bool s_journalArmed        = false;
static bool s_journalOverflow     = false;   // > MAX bytes touched -> don't trust
static bool s_journalUnrestorable = false;   // wrote outside restorable RAM
static int  s_journalProc         = ARMCPU_ARM7;   // core whose reference run is armed

// Byte cell backing a restorable ARM7 address, or nullptr. Mirrors the bank
// decode in _MMU_read08 / _MMU_ARM7_write08: main RAM is a flat mirror, bank
// 0x03 goes through the WRAMCNT-aware MMU_MEM table (identical byte the
// interpreter store will hit).
static u8* diffJournalCellArm7(u32 adr)
{
	if ((adr & 0x0F000000) == 0x02000000)
		return &MMU.MAIN_MEM[adr & _MMU_MAIN_MEM_MASK];
	if ((adr >> 24) == 0x03)
		return &MMU.MMU_MEM[ARMCPU_ARM7][adr >> 20][adr & MMU.MMU_MASK[ARMCPU_ARM7][adr >> 20]];
	return nullptr;
}

// ARM9 restorable RAM: DTCM (relocatable, overlays main RAM -- check first),
// main RAM (shared, flat mirror), ITCM (32 KB window), shared WRAM (bank 0x03,
// -> SWIRAM via the ARM9 MMU_MEM table). Mirrors the decode order in MMU.h's
// _MMU_write* template + _MMU_ARM9_write*. Everything else (VRAM, palette, OAM,
// I/O, GBA slot) is unrestorable.
static u8* diffJournalCellArm9(u32 adr)
{
	if ((adr & ~0x3FFFu) == MMU.DTCMRegion)
		return &MMU.ARM9_DTCM[adr & 0x3FFF];
	if ((adr & 0x0F000000) == 0x02000000)
		return &MMU.MAIN_MEM[adr & _MMU_MAIN_MEM_MASK];
	if (adr < 0x02000000)
		return &MMU.ARM9_ITCM[adr & 0x7FFF];
	if ((adr >> 24) == 0x03)
		return &MMU.MMU_MEM[ARMCPU_ARM9][adr >> 20][adr & MMU.MMU_MASK[ARMCPU_ARM9][adr >> 20]];
	return nullptr;
}

static u8* diffJournalCell(u32 adr)
{
	return s_journalProc == ARMCPU_ARM9 ? diffJournalCellArm9(adr)
	                                    : diffJournalCellArm7(adr);
}

void jitDiffJournalNote(int procnum, u32 addr, u32 size)
{
	if (!s_journalArmed) return;
	if (procnum != s_journalProc) return;        // a different core's write

	for (u32 i = 0; i < size; i++) {
		u8* cell = diffJournalCell(addr + i);
		if (!cell) { s_journalUnrestorable = true; continue; }
		if (s_journalN >= JIT_DIFF_JOURNAL_MAX) { s_journalOverflow = true; return; }
		s_journal[s_journalN].cell = cell;
		s_journal[s_journalN].old  = *cell;
		s_journalN++;
	}
}

void jitDiffJournalNoteRead(int procnum, int at, u32 addr)
{
	if (!s_journalArmed) return;
	if (procnum != s_journalProc) return;
	if (at == MMU_AT_CODE) return;                 // instruction fetch, no side effect
	// I/O register bank: IPC FIFO recv (0x04100000) pops on read, and various
	// other registers have read side effects. Conservatively distrust any block
	// whose reference run reads there. VRAM/palette/OAM (0x05/0x06/0x07) reads
	// are pure and stay trusted.
	if ((addr & 0xFF000000u) == 0x04000000u)
		s_journalUnrestorable = true;
}

static void journalArm(int proc)
{
	s_journalN = 0;
	s_journalOverflow = false;
	s_journalUnrestorable = false;
	s_journalProc = proc;
	s_journalArmed = true;
}

static void journalRollback()   // also disarms
{
	s_journalArmed = false;
	// reverse order so repeated / overlapping writes unwind to the original
	for (u32 i = s_journalN; i-- > 0; )
		*s_journal[i].cell = s_journal[i].old;
	s_journalN = 0;
}

// Boot-time self-test: arm the journal, drive writes through the same MMU
// choke points the interpreter reference run uses, roll back, and prove memory
// is byte-identical again -- plus that an I/O write trips s_journalUnrestorable
// and an oversized run trips s_journalOverflow. Deterministic; does not need
// any THUMB block to execute.
bool jitDiffJournalSelfTest()
{
	const u32 aMain = 0x02300000;   // main RAM  (bank 0x02)
	const u32 aSwi  = 0x03000010;   // shared WRAM
	const u32 aEram = 0x03800020;   // ARM7 WRAM
	u8* cMain = diffJournalCell(aMain);
	u8* cSwi  = diffJournalCell(aSwi);
	u8* cEram = diffJournalCell(aEram);
	bool ok = (cMain && cSwi && cEram);

	// snapshot originals (whole words around each target)
	const u32 oMain = cMain ? _MMU_read32<ARMCPU_ARM7>(aMain) : 0;
	const u32 oSwi  = cSwi  ? _MMU_read32<ARMCPU_ARM7>(aSwi)  : 0;
	const u32 oEram = cEram ? _MMU_read32<ARMCPU_ARM7>(aEram) : 0;

	journalArm(ARMCPU_ARM7);
	_MMU_write32<ARMCPU_ARM7>(aMain, 0xDEADBEEF);
	_MMU_write16<ARMCPU_ARM7>(aMain, 0x1234);        // overlap the first write
	_MMU_write08<ARMCPU_ARM7>(aSwi + 1, 0x5A);
	_MMU_write32<ARMCPU_ARM7>(aEram, 0xA5A5A5A5);
	jitDiffJournalNote(ARMCPU_ARM7, 0x06000000, 4);  // VRAM -> unrestorable
	const bool sawWrites =
		(_MMU_read16<ARMCPU_ARM7>(aMain) == 0x1234) &&
		(_MMU_read32<ARMCPU_ARM7>(aEram) == 0xA5A5A5A5);
	const bool unrestorableTripped = s_journalUnrestorable;
	journalRollback();

	const bool restored =
		(_MMU_read32<ARMCPU_ARM7>(aMain) == oMain) &&
		(_MMU_read32<ARMCPU_ARM7>(aSwi)  == oSwi)  &&
		(_MMU_read32<ARMCPU_ARM7>(aEram) == oEram);

	// overflow path: more single-byte notes than the journal can hold, all to
	// one restorable cell, then prove rollback still returns it to original.
	const u32 oOvf = _MMU_read08<ARMCPU_ARM7>(aMain);
	journalArm(ARMCPU_ARM7);
	for (u32 i = 0; i < JIT_DIFF_JOURNAL_MAX + 64; i++)
		jitDiffJournalNote(ARMCPU_ARM7, aMain, 1);
	*cMain = (u8)~oOvf;                              // mutate after the cap
	const bool overflowTripped = s_journalOverflow;
	journalRollback();
	const bool overflowRestored = (_MMU_read08<ARMCPU_ARM7>(aMain) == oOvf);

	ok = ok && sawWrites && unrestorableTripped && restored
	        && overflowTripped && overflowRestored;

	FILE* f = fopen("sd:/jit.log", "a");
	if (f) {
		fprintf(f, "[jit] journal selftest %s: writes=%d unrestorable=%d restored=%d "
		           "ovfFlag=%d ovfRestored=%d\n",
		        ok ? "PASS" : "FAIL", sawWrites, unrestorableTripped, restored,
		        overflowTripped, overflowRestored);
		fclose(f);
	}
	return ok;
}

// ===========================================================================
// Shared checked-run harness (A2)
// ---------------------------------------------------------------------------
// A1 built this for the ARM7 only. A2 generalises it: the same journal + reverse
// rollback + register/flag/PC compare + per-block cycle compare now serves the
// ARM9 THUMB front-end too (jitRunArm9Checked), with its own counter set and
// report line so the two cores' drift is not conflated. armcpu_exec<> is a
// template on a compile-time PROCNUM, so the core is threaded through as a
// function pointer to the right instantiation.
// ===========================================================================

struct DiffCounters {
	const char* tag;            // "diff" (ARM7) / "diff9" (ARM9)
	int   mismatches;           // capped -- gates the per-mismatch SD write
	u64   mismatchesTotal;      // true count, for the periodic report
	u64   notTrusted;           // comparison skipped: journal overflow / unrestorable
	u64   countDiverge;         // JIT insn count != interp step count (not a bail)
	u64   cycleDriftBlk;        // trusted blocks whose cycle counts differed
	u64   cycleDriftAbs;        // sum |JIT - interp| cycles over those
	u32   cycleDriftMax;        // worst single-block drift
	u64   blocksRun;
	u64   insnsRun;
	u64   lastReport;
	u32   lastDumpPC;
};
static const int JIT_DIFF_MAX_LOGS = 200;
static DiffCounters s_c7 = { "diff",  0,0,0,0,0,0,0,0,0,0, 0xFFFFFFFFu };
static DiffCounters s_c9 = { "diff9", 0,0,0,0,0,0,0,0,0,0, 0xFFFFFFFFu };

extern u64 g_jitBlocksRun, g_jitInsnsRun;

static u32 jitRunChecked(int jitIdx, int proc, JITCache& jcache, u32 (*execOne)(),
                         armcpu_t* cpu, BasicBlock* block, u32 pc, DiffCounters& C)
{
	JitCpuProfile* prof = jitProfile[jitIdx];
	const armcpu_t save = *cpu;
	// A block's ISA == the CPU mode when it was dispatched. jitRunChecked serves
	// both the THUMB and the ARM front-ends now, so every mode-specific step here
	// (fetch width, pipeline offset, PC stride, the T-vs-!T loop guard) branches
	// on this.
	const bool thumb = (save.CPSR.bits.T != 0);
	const u32  step  = thumb ? 2u : 4u;

	// ---- interpreter reference: up to block->insnCount() steps, journalled ----
	journalArm(proc);
	cpu->R[15] = pc + (thumb ? 4u : 8u);
	cpu->instruct_adr     = pc;
	cpu->instruction      = thumb ? prof->fetch16(pc & ~1u) : prof->fetch32(pc & ~3u);
	cpu->next_instruction = pc + step;

	u32 istep = 0;
	u32 iCycles = 0;
	while (istep < block->insnCount() &&
	       ((bool)cpu->CPSR.bits.T == thumb) && !cpu->waitIRQ) {
		u32 curPC = cpu->instruct_adr;
		iCycles += execOne();
		istep++;
		// a taken branch / mode switch ends the comparable run
		if (cpu->instruct_adr != curPC + step) break;
	}
	u32 iR[16];
	memcpy(iR, cpu->R, sizeof iR);
	const u32 iCPSR = cpu->CPSR.val;
	const u32 iPC   = cpu->instruct_adr;

	// ---- undo every guest-RAM write the reference run made, then run the
	//      JIT block for real against pristine state ----
	journalRollback();
	*cpu = save;
	cpu->R[15] = pc + (thumb ? 4u : 8u);
	jit_cpu_state st = { &cpu->R[0], &cpu->CPSR.val, nullptr };
	JITResult r;
	memset(&r, 0, sizeof r);

#ifdef JIT_DIFF_DUMP_PC
	if (pc == (u32)JIT_DIFF_DUMP_PC) {
		static bool s_dumped = false;
		if (!s_dumped) {
			s_dumped = true;
			FILE* f = fopen("sd:/jit.log", "a");
			if (f) {
				const u32* code = (const u32*)block->execute;
				fprintf(f, "[jit] DUMP @%08x %s ins=%u r7=%08x r13=%08x:\n",
				        pc, thumb ? "T" : "A", (unsigned)block->insnCount(),
				        (unsigned)save.R[7], (unsigned)save.R[13]);
				for (u32 i = 0; i < 140; i++)
					fprintf(f, "  %3u %08x\n", i, (unsigned)code[i]);
				fclose(f);
			}
		}
	}
#endif

	ExecuteJITTrace(block->execute, &r, &st);
	if (r.smcHit) jcache.invalidateSMCTarget(r.smcAddress);

	if (r.instructions == 0) {
		// JIT made no progress -- restore and let the interpreter step once
		*cpu = save;
		cpu->R[15] = pc + (thumb ? 4u : 8u);
		u32 c = execOne();
		return c ? c : 1;
	}

	// See jit_exec.cpp's identical fix: POP{...,PC} / BLX can switch to ARM
	// mode and clear CPSR.T before returning here -- check it rather than
	// assuming THUMB.
	const u32 npc = r.nextPC;
	cpu->instruct_adr = npc;
	if (cpu->CPSR.bits.T) {
		cpu->instruction      = prof->fetch16(npc & ~1u);
		cpu->next_instruction = npc + 2;
		cpu->R[15]            = npc + 4;
	} else {
		cpu->instruction      = prof->fetch32(npc & ~3u);
		cpu->next_instruction = npc + 4;
		cpu->R[15]            = npc + 8;
	}

	// ---- classify the run before comparing ----
	const bool trustable = !s_journalOverflow && !s_journalUnrestorable;
	if (!trustable) C.notTrusted++;

	// Instruction-count blind spot, surfaced instead of silent: when the JIT
	// and interpreter step counts disagree without a clean JIT bail the
	// register/flag compare below is skipped (needs equal counts to mean
	// anything). Counting it lets ARM9 sign-off see whether it is exercised.
	if (r.instructions != istep && !r.bailedOut)
		C.countDiverge++;

	if (r.instructions == istep && !r.bailedOut && trustable) {
		char d[320]; size_t k = 0; d[0] = 0;
		for (int i = 0; i < 15; i++)
			if (cpu->R[i] != iR[i] && k + 32 < sizeof d)
				k += snprintf(d + k, sizeof d - k, " R%d j=%08x i=%08x", i, cpu->R[i], iR[i]);
		if ((cpu->CPSR.val & 0xF0000000u) != (iCPSR & 0xF0000000u) && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " NZCV j=%x i=%x", cpu->CPSR.val >> 28, iCPSR >> 28);
		if (npc != iPC && k + 24 < sizeof d)
			k += snprintf(d + k, sizeof d - k, " PC j=%08x i=%08x", npc, iPC);

		// Per-block cycle-count comparison. Advisory on ARM7, but the ARM9 JIT
		// feeds VCount/DMA/IRQ pacing off these counts, so the drift must be
		// measured. Tracked apart from correctness -- a cycle delta is model
		// coarseness, not a miscompile.
		const u32 jc = r.cycles ? r.cycles : 1;
		if (jc != iCycles) {
			u32 drift = jc > iCycles ? jc - iCycles : iCycles - jc;
			C.cycleDriftBlk++;
			C.cycleDriftAbs += drift;
			if (drift > C.cycleDriftMax) C.cycleDriftMax = drift;
			if (k + 28 < sizeof d)
				k += snprintf(d + k, sizeof d - k, " CYC j=%u i=%u", (unsigned)jc, (unsigned)iCycles);
		}

		bool realMismatch = false;
		for (int i = 0; i < 15 && !realMismatch; i++) realMismatch = (cpu->R[i] != iR[i]);
		if ((cpu->CPSR.val & 0xF0000000u) != (iCPSR & 0xF0000000u)) realMismatch = true;
		if (npc != iPC) realMismatch = true;

		if (realMismatch && d[0]) {
			C.mismatchesTotal++;
			if (C.mismatches < JIT_DIFF_MAX_LOGS) {
				C.mismatches++;
				FILE* f = fopen("sd:/jit.log", "a");
				if (f) {
					fprintf(f, "[jit] %s DIFF @%08x %s len=%u ins=%u:%s\n",
					        C.tag, pc, thumb ? "T" : "A",
					        (unsigned)block->insnCount(), (unsigned)r.instructions, d);
					if (pc != C.lastDumpPC) {
						C.lastDumpPC = pc;
						fprintf(f, "[jit]   opcodes:");
						for (u32 i = 0; i < block->insnCount(); i++)
							fprintf(f, thumb ? " %04x" : " %08x",
							        (unsigned)(thumb ? prof->fetch16(pc + i * 2)
							                         : prof->fetch32(pc + i * 4)));
						fprintf(f, "\n");
					}
					fclose(f);
				}
			}
		}
	}

	C.blocksRun++;
	C.insnsRun += r.instructions;
	g_jitBlocksRun++;
	g_jitInsnsRun += r.instructions;
	if (C.blocksRun - C.lastReport >= 100000) {
		C.lastReport = C.blocksRun;
		FILE* f = fopen("sd:/jit.log", "a");
		if (f) {
			fprintf(f, "[jit] %s alive: %llu blocks, %llu insns, %llu mismatches (%d logged); "
			           "untrusted=%llu countDiv=%llu cycDrift=%llu blk (sum=%llu max=%u)\n",
			        C.tag, (unsigned long long)C.blocksRun, (unsigned long long)C.insnsRun,
			        (unsigned long long)C.mismatchesTotal, C.mismatches,
			        (unsigned long long)C.notTrusted, (unsigned long long)C.countDiverge,
			        (unsigned long long)C.cycleDriftBlk, (unsigned long long)C.cycleDriftAbs,
			        (unsigned)C.cycleDriftMax);
			fclose(f);
		}
	}

	return r.cycles ? r.cycles : 1;
}

u32 jitRunArm7Checked(armcpu_t* cpu, BasicBlock* block, u32 pc)
{
	return jitRunChecked(JIT_ARM7, ARMCPU_ARM7, jitCacheArm7,
	                     &armcpu_exec<ARMCPU_ARM7>, cpu, block, pc, s_c7);
}

u32 jitRunArm9Checked(armcpu_t* cpu, BasicBlock* block, u32 pc)
{
	return jitRunChecked(JIT_ARM9, ARMCPU_ARM9, jitCacheArm9,
	                     &armcpu_exec<ARMCPU_ARM9>, cpu, block, pc, s_c9);
}

#endif // DESMUME_JIT_ARM7 && JIT_DIFFERENTIAL_TESTING
