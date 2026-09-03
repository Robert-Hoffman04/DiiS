/****************************************************************************
 * DeSmuMEWii ARM7 JIT
 *
 * jit_differential.h
 *
 * Lockstep interpreter-vs-JIT validation for live execution. Derived from
 * VBA-GX's JITDifferential.* (c) Daryl Borth, GPL v2+ -- see
 * jit/upstream/PROVENANCE.md.
 *
 * Compiled only when JIT_DIFFERENTIAL_TESTING is defined. In that build
 * jitRunArm7() routes through jitRunArm7Checked(): it runs `block->length`
 * interpreter steps from the current state, snapshots, restores, runs the JIT
 * block for real, and logs any divergence in R0..R14 / the NZCV nibble /
 * resume PC to sd:/jit.log. Guest memory writes made by the interpreter pass
 * are NOT reverted, so a block that writes a side-effectful IO register is
 * double-triggered under this build -- acceptable for a validation build,
 * where most ARM7 blocks touch only RAM.
 ***************************************************************************/

#ifndef DESMUME_JIT_DIFFERENTIAL_H
#define DESMUME_JIT_DIFFERENTIAL_H

#include "jit.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

struct armcpu_t;

u32 jitRunArm7Checked(armcpu_t* cpu, BasicBlock* block, u32 pc);

#endif

#endif // DESMUME_JIT_DIFFERENTIAL_H
