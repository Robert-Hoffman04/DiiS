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
 * interpreter steps from the current state, snapshots CPU registers, runs the
 * JIT block for real, and logs any divergence in R0..R14 / the NZCV nibble /
 * resume PC to sd:/jit.log.
 *
 * A1: guest-RAM writes made by the interpreter reference pass are now
 * journalled (jitDiffJournalNote(), wired into the MMU write choke points) and
 * rolled back byte-for-byte before the JIT re-run, so store-containing blocks
 * are actually compared -- "0 mismatches" now means something for blocks that
 * touch RAM. Writes outside restorable RAM (I/O, IPC FIFO, VRAM) still can't
 * be reverted: such a block sets s_journalUnrestorable and is not trusted for
 * that dispatch (counted in the periodic report as `untrusted=`). The report
 * also carries per-block cycle drift and instruction-count divergence, both
 * needed to sign off the ARM9 front-end (A2+), whose timing feeds pacing.
 *
 * A2: the same harness now serves the ARM9 THUMB front-end. jitRunArm9Checked()
 * shares jitRunChecked() with the ARM7 path; the journal decodes ARM9 RAM
 * (DTCM / ITCM / main / shared WRAM) and ARM9 stats report under the "diff9"
 * tag. In a JIT_DIFFERENTIAL_TESTING build the ARM9 JIT runs regardless of
 * jitArm9Enabled so a full boot+gameplay capture exercises it.
 ***************************************************************************/

#ifndef DESMUME_JIT_DIFFERENTIAL_H
#define DESMUME_JIT_DIFFERENTIAL_H

#include "jit.h"

#if defined(DESMUME_JIT_ARM7) && defined(JIT_DIFFERENTIAL_TESTING)

struct armcpu_t;

u32 jitRunArm7Checked(armcpu_t* cpu, BasicBlock* block, u32 pc);
u32 jitRunArm9Checked(armcpu_t* cpu, BasicBlock* block, u32 pc);

#endif

#endif // DESMUME_JIT_DIFFERENTIAL_H
