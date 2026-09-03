/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_trace.h
 *
 * The front-end-agnostic half of the compiler: block-local scan/allocate/
 * bailout state that both jit_thumb.cpp (P2) and jit_arm.cpp (P7) build on
 * top of. In VBA-GX these structures were file-static inside JITCompiler.cpp;
 * splitting them out is what lets one trace scanner drive two emitter tables.
 ***************************************************************************/

#ifndef DESMUME_JIT_TRACE_H
#define DESMUME_JIT_TRACE_H

#include "jit.h"

#if defined(DESMUME_JIT_ARM7)

// Deferred-bailout branch conditions (see JITCompiler.cpp's two-pass scheme).
enum BailoutCond { COND_BEQ, COND_BNE, COND_BGE, COND_BLT, COND_BLE };

struct DeferredBailout {
	u32* branchPtr;
	BailoutCond cond;
	u32 pc;
	u32 cycles;
	u32 instructions;
};

struct SMCBailoutPatch {
	u32* branchLocation;
	u32  pc;
	u32  eaReg;
	u32  instructions;
	u32  cycles;
};

// Lazy guest-register cache slot (guest R0..R14 -> host R15..R28).
struct RegisterState {
	bool allocated;
	bool dirty;
	u8   hostReg;
	u32  age;   // monotonic, for LRU eviction
};

// Compile a trace starting at startPC using cpu's front-end.
//
// P1: no emitter front-end exists yet -- this always registers a length-1
// "don't JIT this" fallback block (execute == nullptr), which the dispatch
// path (P3) resolves straight to the interpreter without re-attempting
// compilation on every fetch.
BasicBlock* jitCompileTrace(u32 startPC, JITCache& cache, const JitCpuProfile& cpu);

#endif // DESMUME_JIT_ARM7

#endif // DESMUME_JIT_TRACE_H
