# Vendored + patched: armwrestler / thumbwrestler

`armwrestler-ds.asm`, `thumbwrestler-ds.asm`, `armwrestler-arm7.asm`,
`ds_arm9.ld`, `ds_arm9_crt0.S`, `font8x8.pat` are derived from:

* Original author: mic (micol972@gmail.com), 2004-2006 -- `armwrestler`, a
  hand-written ARM9/THUMB instruction-set correctness tester for DS
  emulators. No formal license file upstream; freely redistributed by the DS
  emulator dev community for exactly this purpose (see e.g. desmume,
  melonDS, no$gba threads referencing it as a standard conformance ROM).
* Fork used as the base here: https://github.com/Atem2069/armwrestler-fixed
  (TCM init fix, a DMA2CNT_L write fix, and the `E7FFDEFF` firmware-boot
  magic that makes it boot under an emulated firmware -- see its
  `UPSTREAM-README.md`, kept alongside these files unmodified).
* `armwrestler-arm7.asm` is upstream's own trivial ARM7 stub
  (`arm7_main: b arm7_main` -- ARM7 is not exercised by this ROM at all).

## §17 patch: auto-run driver + slot-2 result I/O

The stock ROM is entirely menu/button driven (`CheckKeys` polls
`KEYINPUT`/0x4000130) and reports pass/fail only by drawing to the DS
screen -- unusable headless. `armwrestler-ds.asm` and `thumbwrestler-ds.asm`
are patched (search for "§17" in each file) to:

1. Replace the interactive `forever`/`_forever` menu loops with a
   straight-line walk over every ARM test group (`RunTest` 0..5) and every
   THUMB test group (`_runtest` 0..1; `_test2` is header-only, no real
   content) -- no input needed, and no other behaviour of `RunTest` /
   `_runtest` / `Test0..Test5` / `_test0`/`_test1` / `DrawResult` /
   `_drawresult` is touched.
2. Tally every `DrawResult` / `_drawresult` call (the single existing
   pass/fail choke point each test already goes through to draw OK/BAD) into
   **slot-2 expansion RAM** (guest `0x09000000`+, backed by desmumewii's
   `NDS_ADDON_EXPMEMORY` addon -- flat host RAM, no flash/SRAM protocol to
   drive) instead of only the screen: counts, plus up to 32 {name-string
   pointer, raw bitmask} failure records. See the header comment on
   `AW_RecordResult` (ARM) / `_AW_Record` (THUMB) in each file for the exact
   layout.
3. Write a completion sentinel (`"AWR1"`) last, once every counter write has
   landed, so a host-side poll can never observe a half-written result.

This "write results into slot-2 instead of the screen" trick generalises:
any DS emulator that emulates the GBA-slot address space (desmumewii's
`ExpMemory` addon; melonDS's own GBA-slot SRAM/flash emulation is the same
idea) can read the same bytes back with no input-injection or
framebuffer-capture harness needed.

## Building

```
tools/armwrestler/build.sh
```

Needs `devkitARM` (`arm-none-eabi-as`/`-ld`/`-objcopy`) and `ndstool` on
`PATH` -- `/opt/devkitpro/devkitARM/bin` and `/opt/devkitpro/tools/bin`.
Produces `out/armwrestler.nds`.

## Running against desmumewii

Build desmumewii with `-DDESMUME_ARMWRESTLER_PROBE` (`source/main.cpp`):
selects the `ExpMemory` slot-2 addon, disables the ARM7 JIT (armwrestler's
ARM7 side is a no-op stub, nothing to gain there), and polls slot-2 for the
sentinel once per frame; on completion it writes `sd:/armwrestler.log` (ARM
+ THUMB pass/fail counts, plus each failing test's name and raw bitmask) and
quits. Stage `out/armwrestler.nds` as `sd:/DS/ROMS/test.nds` and boot with
`-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=2` for a fully headless run.
