/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_thumb_test.cpp
 *
 * Synthetic differential test for the THUMB emitter (jit_thumb.cpp). For each
 * test vector it runs the same short instruction sequence twice from an
 * identical ARM7 state -- once through the C++ interpreter one step at a time,
 * once through a freshly compiled JIT block -- and compares R0..R14, the NZCV
 * nibble and the resume PC. Results go to sd:/jit.log.
 *
 * This needs no scheduler integration (P3): it drives armcpu_exec<ARM7>()
 * directly and calls the block through ExecuteJITTrace() with a jit_cpu_state
 * pointing straight at NDS_ARM7. Compiled only when DESMUME_JIT_SELFTEST is set.
 ***************************************************************************/

#include "jit.h"

#if defined(DESMUME_JIT_ARM7) && defined(DESMUME_JIT_SELFTEST)

#include "jit_trace.h"
#include "../armcpu.h"
#include "../MMU.h"
#include <stdio.h>
#include <string.h>

#define JITT_SCRATCH 0x03801000u   // ARM7 WRAM: code
#define JITT_DATA    0x03805000u   // ARM7 WRAM: seeded data
#define JITT_DATA2   0x03805100u   // ARM7 WRAM: STM/LDM target
#define JITT_STACK   0x03806000u   // ARM7 WRAM: PUSH/POP stack

struct ThumbVec {
	const char* name;
	u16 code[8];
	u8  nreal;       // real instructions (padding SWI follows)
	u32 r[16];
	u32 cpsr;        // full word; NZCV live in bits 31..28
};

// LSL r1,r1,#1 ; ADD r0,r1,r2 ; SUB r3,r0,#5 ; MOVS r4,#0 ; ...
static const ThumbVec kVecs[] = {
	{ "lsl_imm",   {0x0049},                 1, {0,0x40000001,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "lsr_imm",   {0x08C9},                 1, {0,0x00000003,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "asr_imm",   {0x1109},                 1, {0,0x80000000,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "add_reg",   {0x1888},                 1, {5,7,9,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "sub_reg",   {0x1A88},                 1, {5,7,9,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "add_imm3",  {0x1DC8},                 1, {0,0x10,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "mov_imm8",  {0x2000},                 1, {0xdeadbeef,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "cmp_imm8",  {0x2805},                 1, {5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "add_imm8",  {0x30FF},                 1, {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "sub_imm8",  {0x3803},                 1, {2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "and_reg",   {0x4008},                 1, {0xff00ff00,0x0ff00ff0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "eor_reg",   {0x4048},                 1, {0xffffffff,0x0f0f0f0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "orr_reg",   {0x4308},                 1, {0xf0f0f0f0,0x0f0f0f0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "bic_reg",   {0x4388},                 1, {0xffffffff,0x0000ffff,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "mvn_reg",   {0x43C8},                 1, {0,0x0000ffff,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "tst_reg",   {0x4208},                 1, {0xff00,0x00ff,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "cmp_alu",   {0x4288},                 1, {9,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "cmn_alu",   {0x42C8},                 1, {0x7fffffff,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "neg_alu",   {0x4248},                 1, {0,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "adc_set",   {0x4148},                 1, {10,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0x20000000 }, // C=1
	{ "adc_clr",   {0x4148},                 1, {10,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "sbc_set",   {0x4188},                 1, {10,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0x20000000 },
	{ "sbc_clr",   {0x4188},                 1, {10,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "mul_alu",   {0x4348},                 1, {7,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "lsl_reg",   {0x4088},                 1, {0x00000001,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "lsr_reg",   {0x40C8},                 1, {0x80000000,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "asr_reg",   {0x4108},                 1, {0x80000000,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "ror_reg",   {0x41C8},                 1, {0x000000ff,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "lsl_reg_big",{0x4088},                1, {0x12345678,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 }, // >=32 -> bail
	// straight-line multi
	{ "seq3",      {0x0049,0x1888,0x1F03},   3, {0,0x40000001,0x100,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	// branches
	{ "b_uncond",  {0x2005,0xE003},           2, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 }, // mov r0,#5 ; b +6
	{ "beq_taken", {0x2800,0xD002},          2, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 }, // cmp r0,#0 -> Z ; beq
	{ "beq_nottak",{0x2801,0xD002},          2, {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "bne_taken", {0x2801,0xD102},          2, {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },
	{ "bcs_taken", {0x2800,0xD202},          2, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0x20000000 },
	{ "bgt_taken", {0x2900,0xDC02},          2, {0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 }, // cmp r1,#0 -> gt ; bgt
	{ "ble_taken", {0x2900,0xDD02},          2, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, 0 },

	// --- P2b: hi-reg, memory, PUSH/POP, LDM/STM, BX, BL --------------------
	// data scratch at JITT_DATA (pre-seeded 0x11111111,0x22222222,0x33333333,0x44444444)
	{ "hi_mov",    {0x4640},                 1, {0,0,0,0,0,0,0,0,0xcafef00d,0,0,0,0,0,0,0}, 0 }, // mov r0,r8
	{ "hi_add",    {0x4480},                 1, {7,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0}, 0 },         // add r0,r8
	{ "hi_cmp",    {0x4590},                 1, {0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0}, 0 },          // cmp r0,r10 ; r0=0,r10=? r10=0
	{ "str_ldr_w", {0x6013,0x681C},          2, {0,0xabcd1234,JITT_DATA,0,0,0,0,0,0,0,0,0,0,0,0,0} }, // str r3,[r2] ; ldr r4,[r3]?? -> use r2
	{ "ldr_w",     {0x6810},                 1, {0,JITT_DATA,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },       // ldr r0,[r2,#0]
	{ "ldrb",      {0x7810},                 1, {0,JITT_DATA,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },       // ldrb r0,[r2,#0]
	{ "ldrh",      {0x8810},                 1, {0,JITT_DATA,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },       // ldrh r0,[r2,#0]
	{ "str_w",     {0x6050,0x6810},          2, {0,JITT_DATA,0,0xdeadbeef,0,0,0,0,0,0,0,0,0,0,0,0} }, // str r0? 0x6050: str r0,[r2,#4]; then ldr r0,[r2,#4]
	{ "ldr_regoff",{0x5888},                 1, {0,JITT_DATA,4,0,0,0,0,0,0,0,0,0,0,0,0,0} },      // ldr r0,[r1,r2]
	{ "ldrsb",     {0x5688},                 1, {0,JITT_DATA,3,0,0,0,0,0,0,0,0,0,0,0,0,0} },      // ldrsb r0,[r1,r2]  (byte 0x44 -> +0x44)
	{ "ldr_pcrel", {0x4801},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },             // ldr r0,[pc,#4]
	{ "ldr_sprel", {0x9802},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,JITT_DATA,0,0} },     // ldr r0,[sp,#8]
	{ "str_sprel", {0x9200,0x9A00},          2, {0,0,0xfeedface,0,0,0,0,0,0,0,0,0,0,JITT_DATA2,0,0} }, // str r2,[sp,#0]; ldr r2,[sp,#0]
	{ "add_pc",    {0xA004},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },             // add r0,pc,#16
	{ "add_sp",    {0xA810},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0x2000,0,0} },        // add r0,sp,#64
	{ "adj_sp_p",  {0xB004},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0x1000,0,0} },        // add sp,#16
	{ "adj_sp_m",  {0xB084},                 1, {0,0,0,0,0,0,0,0,0,0,0,0,0,0x1000,0,0} },        // sub sp,#16
	{ "push_pop",  {0xB407,0x2000,0xBC07},   3, {0xa,0xb,0xc,0,0,0,0,0,0,0,0,0,0,JITT_STACK,0,0} }, // push{r0-r2}; mov r0,#0; pop{r0-r2}
	{ "push_lr_pop_pc",{0xB500,0x46C0,0xBD00},3,{0,0,0,0,0,0,0,0,0,0,0,0,0,JITT_STACK,JITT_SCRATCH+9,0} }, // push{lr}; nop; pop{pc}
	{ "stmia_ldmia",{0xC10C,0xCC60},         2, {0,JITT_DATA2,0xdddd,0xeeee,JITT_DATA2,0,0,0,0,0,0,0,0,0,0,0} }, // stmia r1!,{r2,r3} ; ldmia r4!,{r5,r6}
	{ "bx_r3",     {0x4718},                 1, {0,0,0,JITT_SCRATCH+0x21,0,0,0,0,0,0,0,0,0,0,0,0} }, // bx r3 (thumb target)
	{ "bl_call",   {0xF000,0xF803},          2, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },             // bl +6
};

static void primeThumb(u32 pc)
{
	NDS_ARM7.instruction      = _MMU_read16<ARMCPU_ARM7, MMU_AT_CODE>(pc & ~1u);
	NDS_ARM7.instruct_adr     = pc;
	NDS_ARM7.next_instruction = pc + 2;
	NDS_ARM7.R[15]            = pc + 4;
}

static void loadState(const ThumbVec& v)
{
	for (int i = 0; i < 16; i++) NDS_ARM7.R[i] = v.r[i];
	NDS_ARM7.CPSR.val = v.cpsr;
	NDS_ARM7.CPSR.bits.T = 1;
	NDS_ARM7.CPSR.bits.mode = SYS;
}

int jitThumbSelfTest()
{
	if (!jitProfile[JIT_ARM7]) { printf("[jit] thumb selftest: no profile\n"); return -1; }

	FILE* f = fopen("sd:/jit.log", "a");
	int pass = 0, fail = 0, skip = 0;

	for (const ThumbVec& v : kVecs) {
		// stage code + a SWI terminator past the real instructions
		for (int j = 0; j < v.nreal; j++)
			_MMU_write16<ARMCPU_ARM7>(JITT_SCRATCH + j * 2, v.code[j]);
		_MMU_write16<ARMCPU_ARM7>(JITT_SCRATCH + v.nreal * 2, 0xDF00); // SWI = trace terminator

		// (re)seed data scratch so both runs start from the same memory
		for (int k = 0; k < 8; k++) {
			_MMU_write32<ARMCPU_ARM7>(JITT_DATA  + k * 4, 0x11111111u * (k + 1));
			_MMU_write32<ARMCPU_ARM7>(JITT_DATA2 + k * 4, 0xA0000000u | k);
		}

		// ---- JIT ----
		jitCacheArm7.flushCache();
		BasicBlock* b = jitCompileTrace(JITT_SCRATCH, jitCacheArm7, *jitProfile[JIT_ARM7], /*thumb=*/true);
		if (!b || !b->execute) {
			if (f) fprintf(f, "[jit] thumb %-12s SKIP (not compiled)\n", v.name);
			skip++;
			continue;
		}
		loadState(v);
		NDS_ARM7.R[15] = JITT_SCRATCH + 4;
		jit_cpu_state st = { &NDS_ARM7.R[0], &NDS_ARM7.CPSR.val, nullptr };
		JITResult r; memset(&r, 0, sizeof r);
		ExecuteJITTrace(b->execute, &r, &st);
		u32 jR[16]; memcpy(jR, NDS_ARM7.R, sizeof jR);
		u32 jCPSR = NDS_ARM7.CPSR.val;

		// ---- interpreter reference: same number of steps the JIT actually ran ----
		const int steps = (int)((r.bailedOut && r.instructions < v.nreal) ? r.instructions : v.nreal);
		loadState(v);
		primeThumb(JITT_SCRATCH);
		for (int k = 0; k < steps; k++) armcpu_exec<ARMCPU_ARM7>();
		u32 iR[16];   memcpy(iR, NDS_ARM7.R, sizeof iR);
		u32 iCPSR   = NDS_ARM7.CPSR.val;
		// after armcpu_exec the *next* instruction to run sits in instruct_adr
		// (next_instruction is one further along, already prefetched)
		u32 iNext   = NDS_ARM7.instruct_adr;

		// ---- compare ----
		char detail[192]; detail[0] = 0; size_t dl = 0;
		bool ok = true;
		for (int i = 0; i < 15; i++) {
			if (jR[i] != iR[i]) {
				ok = false;
				dl += snprintf(detail + dl, sizeof detail - dl, " R%d j=%08x i=%08x", i, jR[i], iR[i]);
			}
		}
		if ((jCPSR & 0xF0000000u) != (iCPSR & 0xF0000000u)) {
			ok = false;
			dl += snprintf(detail + dl, sizeof detail - dl, " NZCV j=%x i=%x", jCPSR >> 28, iCPSR >> 28);
		}
		// resume PC (interpreter ran the same number of steps the JIT did)
		if (r.nextPC != iNext) {
			ok = false;
			dl += snprintf(detail + dl, sizeof detail - dl, " PC j=%08x i=%08x", r.nextPC, iNext);
		}

		if (ok) pass++; else fail++;
		if (f) fprintf(f, "[jit] thumb %-12s %-4s ins=%u/%u bail=%u%s\n",
		               v.name, ok ? "PASS" : "FAIL",
		               (unsigned)r.instructions, (unsigned)v.nreal, (unsigned)r.bailedOut, detail);
	}

	if (f) {
		fprintf(f, "[jit] thumb selftest: %d pass, %d fail, %d skip\n", pass, fail, skip);
		fclose(f);
	}
	printf("[jit] thumb selftest: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

#endif // DESMUME_JIT_ARM7 && DESMUME_JIT_SELFTEST
