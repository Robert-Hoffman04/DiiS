// Task ph-arm7: -DDIIS_ARM7PROF diagnostic (with -DDIIS_PERFZONES): what
// the ARM7 JIT spends its time on. Counts / timebase ticks per slow-memory
// address, per in-block interpreter fallback opcode, per dispatched entry PC.
// Emitted with each perf-zones block as "a7p ..." lines and reset.
#ifndef JIT_ARM7_PROF_H
#define JIT_ARM7_PROF_H
#include "../types.h"
#ifdef DIIS_ARM7PROF
void a7pMem(u32 addr, u32 size, bool write, u32 ticks);
void a7pFb(u32 pc, u32 op, bool thumb, u32 ticks);
void a7pRun(u32 pc, u32 cycles, u32 insns, u32 ticks, u32 exitPC);
extern "C" void a7pEmit(u32 frame);
#endif
#endif
