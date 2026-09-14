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
5. [ ] Longer ARM7 JIT block chains - instrument why ARM7 chains break
   (bailedOut / dynamic-exit / IRQ-check / quota, split by core) to see if
   some exits are staticly chainable.
6. [ ] Shrink the ARM7 uncompilable-PC set - capture dontJIT top: PCs
   (likely predicated LDR/STR) and widen the emitter for the top offenders;
   2289 marker re-hits/frame is the largest single ARM7 cost.

## Known bugs / unfinished features tracked in memory
7. [ ] GX2DBG SUB-screen affine-BG bug - ghosting/grey-grid artifacts on the
   SUB screen GX-offload path (MAIN is visually verified OK).
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
