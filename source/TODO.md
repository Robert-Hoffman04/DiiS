# TODO (branch arm9-jit-infra)

## Immediate - finish current work in flight
1. [x] Validate the uncommitted repeated-bail demotion fix (source/src/jit/jit_exec.cpp).
   Implements NOTES.md Step 5 item 3 ("demote on N consecutive zero-progress
   hits"). Built an A/B harness (tools/benchmark/corecost-ab.sh: baseline =
   fix stashed out, after = fix in, both -DJIT_CORE_COST_HISTO over the
   unified harness PKT_PROFILE sink) and ran a 30s SM64DS smoke soak.
   Confirmed: ARM7 bail0/spin_hits fell from ~586/frame to ~93/frame (-84%,
   5 PCs demoted after the 4-hit threshold), noBlock/marker_hits rose by the
   same amount, and total calls/ran/wasted counts were byte-identical
   baseline vs after (same guest trace, purely cheaper handling of the same
   events - no behavior change). arm7_jit zone time was flat-to-slightly-
   better within the noise of a 30s run, consistent with NOTES.md's own
   ~0.2-0.4 ms/f estimate out of a ~5.9 ms/f zone. Committed.
2. [x] Double-check the BailTrack direct-mapped table ((pc>>2)&63) for
   correctness under PC aliasing. trackRepeatedBail() is only called from
   the r.instructions==0 (bail0) path, so a hash collision with a different
   PC just resets the counter (delays demotion) - it can never fire for a
   PC that isn't itself bailing, so no spurious demotion of a good block.
   Confirmed by inspection and by the A/B run above (demote1 count unchanged
   baseline vs after: the length-1 path is untouched by this change).

## Highest-leverage next steps (NOTES.md's own priority order)
3. [x] Better ARM9 block-table hash - tried mulhwu (top 32 bits of full
   product) instead of mullw, A/B'd with -DJIT_HASH_HISTO. REJECTED: total
   collapse to 1/65536 touched buckets (100% of evictions in one bucket,
   worse than the original shift-xor hash). Root cause: mulhwu's high-word
   result scales ~linearly with pc with no modular wraparound, so it barely
   moves across SM64DS's ~4MB clustered ARM9 working set. mullw (committed)
   stays the hash; mulhwu kept in-tree as an off-by-default, documented
   -DJIT_HASH_HISTO negative result (see jit/NOTES.md). Committed.
4. [x] 2-way set associativity on getBlock() - DONE. BLOCK_TABLE_WAYS=2 in
   jit_cache.h; every set-index->slot-address site updated (getBlock,
   registerBlock, both hand-emitted PPC hash stubs, profCacheEvict,
   blockTable/installFrame allocation). Validated: -DJIT_DIFFERENTIAL_TESTING
   clean over ~450s combined soak (vs an identical pre-existing sd:/jit.log
   FAT-capture artifact confirmed present on the baseline too, so not a
   regression); -DJIT_HASH_HISTO shows ARM9 collision evictions fell from
   ~43,775/150s (touching 82 buckets, worst bucket 10231 evictions) to
   8/150s (touching 8 buckets, worst bucket 1 eviction, and all 8 are
   same-PC ISA-mode-flip re-registrations, not hash weakness) - essentially
   total elimination of the structural hot-bucket thrash Step 5's follow-up
   identified. See jit/NOTES.md and tools/benchmark/assoc-ab.sh. Committed.
5. [x] Longer ARM7 JIT block chains - instrumented why chains break (added
   the ARM9-side DESMUME_JIT_TRACE_FIRST "why didn't this chain?" slot
   classification to jitRunArm7() too, split by core). 180s in-game SM64DS
   soak result: hash-collision/cache-pressure slot eviction is now a
   non-factor on both cores (item 4 confirmed from the chain angle too).
   When a chain DOES end cleanly ("edge": 64% of ARM7 dispatches, 17% of
   ARM9), the next PC is essentially always (~99.9%) already a dontJIT
   marker - not staticly chainable without item 6 first. Bigger finding:
   most round-trips are NOT clean edges - ~33% of ARM7 and ~83% of ARM9
   dispatches return via a mid-chain deferred-bailout guard trip (a
   predicated instruction resolving opposite to its compiled fast-path
   assumption), previously uninstrumented. See jit/NOTES.md Step 6. Real
   fix (compile both predicate outcomes) is a new, larger, separately-scoped
   project - see item 12. Also fixed a pre-existing missing
   `#include <stdio.h>` in jit_trace.cpp (DESMUME_JIT_TRACE_FIRST alone
   failed to build without it). Committed.
6. [~] Shrink the ARM7+ARM9 uncompilable-PC set - captured the dontJIT top:
   opcode dump (lowered its report threshold, jit_trace.cpp, to get a hit
   within a short soak). Top offenders collapse into a few instruction
   shapes: BXcc lr (conditional return), MSR CPSR_c (mode switch), ADD{cc}
   pc,pc,Rm,LSL#2 (jump table), SUBS pc,lr,#4 (IRQ return), LDMcc{...,pc}
   (conditional epilogue), predicated STM. Fixed two: BXcc lr now compiles
   on ARM7 too (was gated ARM9/v5-only for no functional reason - predicated
   execution is base ARMv4T, and the existing ARM9 guarded-exit mechanism
   has nothing v5-specific in it), and the ADD/SUB{cc} pc,pc,Rm register-form
   jump-table dispatch now compiles on both cores (emitDataProcToPc's header
   comment already claimed to cover this but only the immediate-operand2
   form was actually special-cased for rn==15 - materializes currentPC+8
   into a scratch reg instead of trying to read a live PC register; still a
   dynamic exit since Rm is a runtime table index, no static-target risk).
   Both validated via differential-testing soak, no mismatches. Perf A/B
   (tools/benchmark/item6-ab.sh, jit/NOTES.md Step 9): arm7_jit perf_zones
   time down ~10% on average across 7 matched windows (range -3.7% to
   -15.7%, never a regression), arm9_jit flat within noise as expected
   (BXcc-lr was already ARM9-only-fixed). Same guest work (cycles/insns
   retired per frame, flat) now costs ~14% fewer jitRunArm7() dispatcher
   round-trips - the intended "longer chains" effect. Committed. Remaining
   shapes (MSR CPSR_c / SUBS pc,lr,#4 / predicated LDM{...,pc}) touch real
   CPU-mode/exception semantics and need their own scope discussion first -
   do not start unprompted. See jit/NOTES.md Steps 7-9.

## Known bugs / unfinished features tracked in memory
7. [skip] GX2DBG SUB-screen affine-BG bug - moved to docs/PLAN.md ("Known
   bugs and residual items"). Turned out to already be fixed (sprite
   front-to-back EFB-depth compositing, commit b73d12c) - both screens
   visually verified now. Given that plus the -61%/+45fps win, GX2DBG is
   promoted from bench-only opt-in to the default execution path for the GX
   core on this branch (source/src/main.cpp) - see docs/PLAN.md for
   residual, non-blocking polish items.
8. [ ] Unified harness Step 11 - real-hardware cutover per plan §6. Needs
   explicit per-launch permission each time - do not do unprompted.

## Housekeeping
9. [x] source/TODO was empty - now tracked here. Renamed to source/TODO.md
   so it opens with a Markdown viewer.
10. [ ] source/src/jit/jit_thumb.cpp:270 - stale inline TODO (clamp) on a
    differential-harness flag comment; resolve or promote to a tracked item.
11. [ ] After ARM9 hash-collision work lands, scope the next investigation:
    ARM9 JIT-exec (~4.25 ms/f) and the 2D compositor (~5 ms/f) become the
    real remaining frame-time levers.
12. [ ] New from item 5's investigation: real conditional-execution support
    in the JIT - compile a path for both predicate outcomes (or a compiled,
    not interpreted, fallback for a predicate miss) instead of always
    bailing to the interpreter on a deferred-bailout guard trip. This is
    what actually lengthens chains (~33% of ARM7 / ~83% of ARM9 dispatches
    currently end this way) - substantially bigger and riskier than items
    1-4 (touches the condition-check emitter for every predicated opcode on
    both fronts, ARM and THUMB). Needs its own scope discussion before
    starting - do not start unprompted.
