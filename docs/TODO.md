# TODO (branch arm9-jit-infra)

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
8. [ ] Unified harness Step 11 - real-hardware cutover per plan §6. Needs
   explicit per-launch permission each time - do not do unprompted.

## Housekeeping
10. [ ] source/jit/jit_thumb.cpp:270 - stale inline TODO (clamp) on a
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
