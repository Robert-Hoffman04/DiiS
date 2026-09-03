/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_differential.h
 *
 * Lockstep interpreter-vs-JIT validation. Derived from VBA-GX's
 * JITDifferential.* (c) Daryl Borth, GPL v2+ -- see jit/upstream/PROVENANCE.md.
 *
 * P1: the DeSmuME-shaped interface + CPU-state snapshot only. The catch-up
 * interpreter re-run that produces the "should have happened" reference is
 * wired up once the block-run dispatch path exists (P3); it needs the same
 * single-step hooks that path introduces.
 *
 * Compiled only when JIT_DIFFERENTIAL_TESTING is defined.
 ***************************************************************************/

#ifndef DESMUME_JIT_DIFFERENTIAL_H
#define DESMUME_JIT_DIFFERENTIAL_H

#include "jit.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

struct armcpu_t;

// Run `block` from cpu's current state under the JIT, restore, re-run the same
// guest instructions through the interpreter, and log any divergence in the
// 16 GPRs / the NZCV nibble / cycle count. Returns true if a mismatch was
// reported.
bool jitRunDifferential(armcpu_t* cpu, BasicBlock* block);

#endif

#endif // DESMUME_JIT_DIFFERENTIAL_H
