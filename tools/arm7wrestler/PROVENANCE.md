# Vendored + patched: arm7wrestler

`armwrestler-ds.asm`, `thumbwrestler-ds.asm`, `armwrestler-arm9.asm`,
`font8x8.pat` are derived from:

* Original author: mic (micol972@gmail.com), 2004-2006 -- the ARM7 test
  content here is the same author's `armwrestler`/`thumbwrestler` sources
  (see `tools/armwrestler/PROVENANCE.md`), reworked to actually run on the
  ARM7 instead of being a no-op stub there.
* Fork used as the base here: https://github.com/Arisotura/arm7wrestler
  ("ARM7 counterpart of the DS CPU tester ARMWrestler") -- adds an
  `ExceptionHandler` install (ARM7 has no CP15; several ARMv5-only opcodes
  raise an undefined-instruction exception on real ARM7TDMI hardware instead
  of executing, and this ROM tests that) and moves the real ARM/THUMB test
  content onto `arm7/`, leaving `arm9/` as a trivial idle/vram-copy stub. No
  formal license file upstream, same "freely redistributed by the DS
  emulator dev community for exactly this purpose" situation as
  `tools/armwrestler` (see its PROVENANCE.md) -- this is the ROM the
  project's own plan doc (§8.3/§16/§18) calls out by name as the intended
  ARM7 CPU-correctness gate.
* See `UPSTREAM-README.md` (kept alongside these files unmodified) for the
  documented ARM7-vs-ARM9 behavioral differences this ROM specifically
  targets (LDM writeback-with-base-in-list, CLZ/QADD/QSUB/QDADD/QDSUB/MRC-p15
  raising undefined-instruction, LDRD/SMUL/SMLA being silent no-ops).

## §18 patch: auto-run driver + slot-2 result I/O + ARM7 crt0/linker

Same technique as `tools/armwrestler`'s §17 patch:

1. `armwrestler-ds.asm`'s `main:` replaces the stock interactive menu/keys
   loop with a straight-line walk over ARM `Test0..Test5` (via `RunTest`)
   and THUMB `_test0`/`_test1`/`_test2` (via `_runtest`, bypassing
   `_tmbmain`'s own interactive loop) -- no input needed. `Test6..Test9` are
   NOT called: they're dead aliases that fall straight into `TestTmb`
   (see `jumptable` at the bottom of the file), not real tests. Every
   `DrawResult`/`_drawresult` call is otherwise untouched.
2. `AW_RecordResult` (ARM, hooked into `DrawResult`) and `_TW_Record`
   (THUMB, hooked into `_drawresult`) tally results into **slot-2 expansion
   RAM** (guest `0x09000000`+, desmumewii's `ExpMemory` addon), using the
   *same* header layout `tools/armwrestler` established: ARM total/fail at
   `+0x04`/`+0x08`, THUMB total/fail at `+0x0C`/`+0x10`, a shared 32-entry
   fail log at `+0x14`/`+0x18..`. Both ARM and THUMB tests run on the same
   CPU here (ARM7), so this needed no per-CPU layout change.
3. A completion sentinel (`"AWR1"`) is written last, same as `tools/armwrestler`.

The stock interactive loop body (`handle_menu`/`not_start`/`not_select`/...)
is deleted rather than kept as dead code, matching `tools/armwrestler`'s own
§17 patch.

### Why this needed its own crt0/linker script (`ds_arm7_crt0.S`/`ds_arm7.ld`)

Unlike `tools/armwrestler` (whose real content is on ARM9 and links with a
minimal bare-metal ARM9 crt0), the real content here is on ARM7, and this
devkitPro installation's bundled `ds_arm7_crt0.o` (from devkitARM's own
`ds_arm7.specs`) references `__dsimode`/`__libnds_exit`, symbols this
installation's `libnds7.a` doesn't provide (a toolchain/libnds version
mismatch, not something worth chasing here). `ds_arm7_crt0.S`/`ds_arm7.ld`
are a from-scratch minimal ARM7 crt0 + linker script, adapted directly from
`tools/armwrestler/ds_arm9_crt0.S`/`ds_arm9.ld` (the classic Jeff Frohwein
GBA/DS template, public-domain per its own header) with:

* no CP15 ITCM setup (ARM7TDMI has no coprocessor 15 at all -- this is one
  of the exact behaviors the ROM tests, see `ExceptionHandler` above);
* no secure-area `E7FFDEFF` padding (an ARM9-boot-specific convention;
  upstream's own trivial ARM7 stub, `tools/armwrestler/armwrestler-arm7.asm`,
  doesn't use it either);
* code/rodata/data linked directly in main RAM at `0x02380000` (the same
  address devkitARM's own `ds_arm7.ld` uses as its ARM7 main-RAM origin),
  with no LMA/VMA WRAM-relocation step -- real hardware/commercial ARM7 code
  is more often relocated into shared WRAM, but straight main-RAM residency
  is simpler, still a real hardware-valid placement, and is what the
  ARM9-side crt0 this is adapted from already does;
* the stack (`__sp_usr`/`__sp_irq`) placed in ARM7's private 64KB WRAM
  (`0x03800000`-`0x0380FFFF`), growing down and away from the `0x380FFDC`
  exception-vector slot `ExceptionHandler`'s install writes to.

`-march=armv5te` is passed to the assembler for both `.s` files -- not
because the target CPU is ARMv5 (it's ARMv4T, ARM7TDMI), but purely so the
assembler can *encode* the ARMv5-only opcodes (`CLZ`, `LDRD`, `QADD`/`QSUB`,
`SMLAxy`) this ROM deliberately executes to observe real ARM7TDMI behavior.

## Building

```
tools/arm7wrestler/build.sh
```

Needs `devkitARM` (`arm-none-eabi-as`/`-ld`/`-objcopy`) and `ndstool` on
`PATH` -- `/opt/devkitpro/devkitARM/bin` and `/opt/devkitpro/tools/bin`.
Produces `out/arm7wrestler.nds`.

## Running against desmumewii

Build desmumewii with `-DDESMUME_ARM7WRESTLER_PROBE` (`source/src/main.cpp`):
selects the `ExpMemory` slot-2 addon, disables the ARM9 JIT (this ROM's ARM9
side is a trivial idle/vram-copy stub -- nothing to gain there; **leaves the
ARM7 JIT setting alone**, unlike `-DDESMUME_ARMWRESTLER_PROBE` which forces
it off, since exercising the ARM7 JIT is the entire point here), and polls
slot-2 for the sentinel once per frame; on completion it writes
`sd:/arm7wrestler.log` (ARM + THUMB pass/fail counts, plus each failing
test's name and raw bitmask) and quits. Stage `out/arm7wrestler.nds` as
`sd:/DS/ROMS/test.nds` and boot with `-DDESMUME_FORCE_ROM
-DDESMUME_FORCE_CORE=2` for a fully headless run.
