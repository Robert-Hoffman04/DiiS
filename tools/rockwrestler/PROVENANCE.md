# Vendored + patched: RockWrestler

All files under `src7/`, `src9/`, `common/`, `linker7.ld`, `linker9.ld` are
derived from:

* Author: RockPolish (Discord: Emudev Discord server).
* Upstream: https://github.com/RockPolish/rockwrestler -- "WIP NDS test ROM
  for emulators". No formal license file upstream, same "freely
  redistributed by the DS emulator dev community for exactly this purpose"
  situation as `tools/armwrestler`/`tools/arm7wrestler` (see their
  PROVENANCE.md) -- this is the ROM the project's own plan doc (§8.4/§16/§19)
  names as the intended primary DS homebrew conformance/regression target,
  covering ground armwrestler/arm7wrestler don't: ARMv5 `LDM`/`STM` edge
  cases, `BLX`/interworking, IPCSYNC/IPCFIFO (+ their IRQs), WRAMCNT/VRAMCNT,
  DTCM/ITCM, and the DS math coprocessor (division, square root).
* See `UPSTREAM-README.md` (kept alongside these files unmodified) for the
  full test catalog and what each category checks.

Unlike `tools/arm7wrestler`, this needed **no crt0/linker workaround**:
upstream's own build is already `-nostartfiles` with a two-instruction
`entry.s` per CPU and trivial single-section `linker7.ld`/`linker9.ld` --
no libnds, so no devkitPro-bundled-crt0-vs-installed-libnds version
mismatch to route around. `build.sh` is upstream's own, reworked only to
land output under `out/` and generate the build-timestamp `.s` inline
(upstream's `time.py` did the same thing, just as a separate step).

## §19 patch: auto-run driver + slot-2 result I/O

All real per-instruction CPU test content here runs on the **ARM9**
(`src9/`) -- per `UPSTREAM-README.md`, even the ARMv4-general tests
"behave the same on both NDS CPUs but are run on the ARM9 for simplicity".
The ARM7 side (`src7/`) only cooperates as a remote-memory-access/IPC
peer via its own `arm7_waitloop` (`src7/framework/waitloop.cpp`), which was
already fully non-interactive (a `REG_IPCSYNC`-driven dispatch loop, no
button input anywhere in `src7/`) -- so only `src9/framework/menu.cpp` and
`src9/framework/menu.s` needed patching, mirroring §17/§18's approach:

1. `src9/framework/menu.cpp`: `cpp_menu()`'s interactive A/B/up/down loop
   is replaced with a flat walk (`autorun_find()`) over every real test
   (`MenuEntry.type == 0`) across the five test-category submenus
   (ARMv4, ARMv5, IPC, DS MATH, MEMORY), skipping "INITIAL STATE"
   (`type == 2`: register dumps only, no pass/fail). The `Menu`/
   `MenuEntry` structs and all `menu_*_entries` arrays -- including
   `menu_initialstate` -- are left exactly as upstream wrote them; they're
   the ground truth for names/order/grouping, just walked programmatically
   instead of via key input.
2. `cpp_fail_test()` (called by `fail_test`/`timeout_test`/`timeout_rw`,
   `menu.s`) additionally tallies into **slot-2 expansion RAM** (guest
   `0x09000000`+, desmumewii's `ExpMemory` addon -- the same guest range
   this ROM's own `enable_trace()`/`disable_trace()` debug hooks already
   poke at `0x08004400`/`0x08005500`, `common/common.cpp`): running/failed
   counts, and a capped 32-entry fail log storing each failure's
   `MenuEntry.text` pointer (a live ARM9 address into this ROM's own
   rodata) plus the sub-case number `fail_test`/`timeout_test` reported
   (`-1` for `timeout_rw`, which doesn't have one). See the layout comment
   at the top of the autorun block in `menu.cpp` for exact byte offsets.
3. `menu.s`: `fail_test`/`timeout_test`/`timeout_rw` no longer wait for the
   B button before returning to `main_post_init` -- headless has no
   button, and by the time control reaches here `cpp_fail_test()` has
   already recorded the result.
4. A completion sentinel (`'RKW1'`) is written to `0x09000000` last, after
   every real test slot has run out (`autorun_find()` returns null) --
   same "counts land before the sentinel, so a single per-frame sentinel
   check on the host side can't observe a half-written header" discipline
   as `tools/armwrestler`/`tools/arm7wrestler`.

No interworking hazard here of the kind `tools/arm7wrestler` hit: this
ROM's driver-level test calls (`call_fncptr`, ARM `bx r0`) and the ARM9
CPU-state tests (`run_tests_armv5_blx`, `run_tests_armv5_ldrpopr15`) all
run on ARM9 (ARMv5), where `LDM`/`POP` *does* interwork on the popped
address's bit 0 -- the naive `mov lr,pc; bx target+1` / `pop {..,pc}`
idiom that broke on ARM7 (ARMv4T) is exactly correct here.

Both `jitArm7Enabled` and `jitArm9Enabled` are left alone by the probe
(`-DDESMUME_ROCKWRESTLER_PROBE`, `source/src/main.cpp`) -- unlike
armwrestler/arm7wrestler, which each isolate one CPU's JIT because the
other side is a stub, RockWrestler's IPC/WRAMCNT/VRAMCNT/TCM tests
genuinely exercise both CPUs together, which is the point of adopting it
as the primary conformance target (plan doc §8.4).

## Building

```
tools/rockwrestler/build.sh
```

Needs `devkitARM` (`arm-none-eabi-g++`/`-objcopy`) and `ndstool` on `PATH`
-- `/opt/devkitpro/devkitARM/bin` and `/opt/devkitpro/tools/bin`. Produces
`out/rockwrestler.nds`.

## Running against desmumewii

Build desmumewii with `-DDESMUME_ROCKWRESTLER_PROBE` (`source/src/main.cpp`):
selects the `ExpMemory` slot-2 addon and polls it once per frame; on
completion it writes `sd:/rockwrestler.log` (total run/failed, plus each
failing test's name and detail number) and quits. Stage
`out/rockwrestler.nds` as `sd:/DS/ROMS/test.nds` and boot with
`-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=2` for a fully headless run (add
`-DDESMUME_JIT_ARM7` to `JITDEFS` for the JIT path; omit for the interpreter
baseline). Predicated Bcc/BLcc compile unconditionally now -- the old
`-DJIT_ARM_PRED_BRANCH` gate was removed once this gate cleared.
