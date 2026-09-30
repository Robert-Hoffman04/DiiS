# tools/cloud-harness

Headless performance and correctness harness used to optimise DiiS from a
Claude Code cloud container (Ubuntu, Docker, no display, no Wii). It runs a
bench build of DiiS in a from-source Dolphin under Xvfb + Mesa llvmpipe.
`setup.sh` builds the environment under `/home/user/tools`, and the scripts
assume those paths.

The key property is that Dolphin's Wii timebase is emulated time. The
perf-zone microseconds are therefore deterministic run to run (noise
< 0.001 ms/frame) and independent of how slow the host is. They are an
instruction-cost measure: the real Wii is slower, and more so for code that
misses its caches, so treat them as a guide, not as hardware timings.

| Path | What it does |
|---|---|
| `setup.sh` | One-time setup: devkitPPC docker image, Dolphin from source, packages, script install. |
| `perf/bench.sh <tree> <label>` | Builds a `-DDESMUME_PERFZONES -DDESMUME_BENCH` dol (GxFast via `-DDESMUME_FORCE_RENDERMODE=2`, forced ROM) in docker and runs 1800 frames. It pulls `sd:/perfzones.log` off the live SD image and prints mean ms/frame per zone for frames >= 180. Use a distinct `DISP` per concurrent run: each display gets its own Dolphin user dir. |
| `perf/compare.py a.csv b.csv` | Per-zone deltas between two runs. |
| `correct/check.sh <tree>` | Correctness gate (about 9 min). It runs armwrestler, arm7wrestler and rockwrestler under the JIT, compared with `baseline/`. It runs `JIT_DIFFERENTIAL_TESTING` on the three wrestlers plus the game, which must show 0 mismatches. It also hashes guest state (main RAM, TCMs, WRAM, VRAM, palette/OAM, both CPUs' registers, timers, screens, GE output) at frames 300, 600 and 900 and compares the hashes with the baseline. The state hash comes from `lib/state_hash.patch`, applied to a copy of the tree under `-DDESMUME_STATE_HASH -DDESMUME_DETERMINISTIC_RTC`. Run `--update-baseline` once on a known-good tree. |
| `ab/ab.sh` | End-to-end A/B of two commits without perf zones, from `sd:/bench.log`. |

**Notes**
- **Correctness gate:** a behaviour-preserving optimisation must leave the state hash bit-identical. An injected JIT flag bug was caught by the hash, not by the wrestlers.
- **SD image location:** Dolphin reads the SD card from `<userdir>/Load/WiiSD.raw`, built from `<userdir>/Load/WiiSDSync/`. A killed Dolphin never syncs back, so read results from the live image with mtools. The FAT may be at offset 0.
- **DOL padding:** current Dolphin rejects a dol whose last section's 32-byte-padded size runs past the end of the file ("Failed to init core"). The scripts zero-pad the dol to a multiple of 32.
- **ndstool:** `check.sh` builds the wrestler ROMs with an `ndstool` it expects in `correct/cache/nds/`. The docker image lacks it, so build it from source once.
