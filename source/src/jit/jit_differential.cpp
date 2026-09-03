/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_differential.cpp  -- see jit_differential.h
 *
 * Derived from VBA-GX's JITDifferential.cpp (c) Daryl Borth, GPL v2+.
 ***************************************************************************/

#include "jit_differential.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

#include "../armcpu.h"
#include <string.h>
#include <stdio.h>

namespace {

struct Snapshot {
	u32 r[16];
	u32 cpsr;
	u32 spsr;
};

void snap(const armcpu_t* c, Snapshot* s)
{
	memcpy(s->r, c->R, sizeof(s->r));
	s->cpsr = c->CPSR.val;
	s->spsr = c->SPSR.val;
}

int diffState(const Snapshot& jit, const Snapshot& cpp, char* out, size_t n)
{
	int mism = 0;
	size_t k = 0;
	for (int i = 0; i < 16 && k + 40 < n; i++) {
		if (jit.r[i] != cpp.r[i]) {
			k += snprintf(out + k, n - k, "  R%-2d jit=%08x cpp=%08x\n", i, jit.r[i], cpp.r[i]);
			mism++;
		}
	}
	if ((jit.cpsr & 0xF0000000u) != (cpp.cpsr & 0xF0000000u) && k + 40 < n) {
		snprintf(out + k, n - k, "  NZCV jit=%x cpp=%x\n", jit.cpsr >> 28, cpp.cpsr >> 28);
		mism++;
	}
	return mism;
}

} // namespace

bool jitRunDifferential(armcpu_t* cpu, BasicBlock* block)
{
	if (!cpu || !block || !block->execute) return false;

	Snapshot before;
	snap(cpu, &before);

	// TODO(P3): with the block-run dispatch path in place --
	//   1. run block->execute via ExecuteJITTrace, snapshot -> jitAfter
	//   2. restore `before`
	//   3. single-step the interpreter block->length times, snapshot -> cppAfter
	//   4. char buf[512]; if (diffState(jitAfter, cppAfter, buf, sizeof buf))
	//          { printf("[jit] DIFF @%08x\n%s", block->startPC, buf); return true; }
	(void)before;
	(void)&diffState;
	return false;
}

#endif // DESMUME_JIT_ARM7 && JIT_DIFFERENTIAL_TESTING
