#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# desmumewii renderer benchmark
#
# Builds the three renderer dols from a -DDESMUME_BENCH tree, runs every scene
# in scenes.conf through headless Dolphin, pulls the emulator's sd:/bench.log
# after each run, and hands the captures to analyze.py, which writes
# results.json + report.md and diffs against the previous run.
#
#   bench_sw     software rasterizer    DESMUME_FORCE_CORE=2
#   bench_gx     GX hardware 3D         DESMUME_FORCE_CORE=1
#   bench_merge  GXMerge sandwich       DESMUME_FORCE_CORE=1 + DESMUME_FORCE_GXCOMPOSITE
#   bench_jitoff ARM7 interpreter       DESMUME_FORCE_CORE=2, no JITDEFS (P5 A/B baseline)
#   bench_jiton  ARM7 JIT               DESMUME_FORCE_CORE=2 + DESMUME_JIT_ARM7
#   bench_jit9off ARM9 interpreter      DESMUME_FORCE_CORE=2, no JITDEFS (A3 baseline)
#   bench_jit9on  ARM9 JIT              DESMUME_FORCE_CORE=2 + DESMUME_JIT_ARM7
#                                       + DESMUME_JIT_ARM9_ON (jitArm9Enabled=true)
#   bench_jitfull full JIT + GXMerge    DESMUME_FORCE_CORE=1 + DESMUME_FORCE_GXCOMPOSITE
#                                       + DESMUME_JIT_ARM7 + DESMUME_JIT_ARM9_ON
#                                       (both cores JIT'd, real GXMerge renderer
#                                       instead of the sw core used by the A/B pairs
#                                       above - "how fast is it for real" number)
#
# Prerequisites:
#   - devkitPPC toolchain           (DEVKITPPC / DEVKITPRO in the environment)
#   - flatpak Dolphin               (org.DolphinEmu.dolphin-emu)
#   - mtools                        (mcopy / mdel)
#   - a Dolphin Wii SD image with   DS/ROMS/  and  DS/BIOS/{biosnds7.rom,
#     biosnds9.rom,firmware.bin}    (the same image the vsd-testrom harness uses;
#     $DOLPHIN_SD below)
#
# Usage:
#   tools/benchmark/benchmark.sh [options]
#     --no-build           reuse tools/benchmark/dols/*.dol
#     --clean              `make clean` before the first build
#     --duration N         seconds per run          (default 90; scenes.conf 'dur=' wins)
#     --modes "sw gx"      subset of renderers      (default "sw gx merge";
#                          also available: jitoff jiton jit9off jit9on jitfull
#                          profile [= jitfull + perf_zones frame-time breakdown])
#     --scenes "vsd ph"    subset of scene ids from scenes.conf
#     --no-compare         skip the diff against the previous run
#
# Env overrides: DOLPHIN_DATA, DOLPHIN_SD, DEVKITPRO, DEVKITPPC
#-----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC MTOOLS_SKIP_CHECK=1

TARGET=desmumewii
DOLDIR="$HERE/dols"
DUR_DEFAULT=90
MODES="sw gx merge"
SCENE_FILTER=""
DO_BUILD=1 DO_CLEAN=0 COMPARE_ARG=""

while [ $# -gt 0 ]; do
	case "$1" in
		--no-build)   DO_BUILD=0 ;;
		--clean)      DO_CLEAN=1 ;;
		--duration)   DUR_DEFAULT="$2"; shift ;;
		--modes)      MODES="$2"; shift ;;
		--scenes)     SCENE_FILTER="$2"; shift ;;
		--no-compare) COMPARE_ARG="--no-compare" ;;
		-h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
		*)            echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

die() { echo "benchmark: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have mcopy   || die "mtools not found (mcopy)"
have flatpak || die "flatpak not found"
[ -f "$DOLPHIN_SD" ] || die "no Dolphin SD image at $DOLPHIN_SD (set DOLPHIN_SD)"
mdir -i "$DOLPHIN_SD" ::/DS/BIOS >/dev/null 2>&1 || die "SD image has no DS/BIOS/ - stage the DS bios files first"
[ -f "$HERE/scenes.conf" ] || die "missing $HERE/scenes.conf"

dolphin_kill() {
	pkill -9 -x dolphin-emu        2>/dev/null || true
	pkill -9 -f dolphin-emu-wrapper 2>/dev/null || true
}

defs_for() {
	local base="-DDESMUME_FORCE_ROM -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=200000"
	case "$1" in
		sw)     echo "$base -DDESMUME_FORCE_CORE=2" ;;
		gx)     echo "$base -DDESMUME_FORCE_CORE=1" ;;
		merge)  echo "$base -DDESMUME_FORCE_CORE=1 -DDESMUME_FORCE_GXCOMPOSITE" ;;
		# ARM7 JIT A/B (P5): same renderer (sw) as the "sw" baseline so any
		# delta is purely the ARM7 core, not a renderer swap.
		jitoff) echo "$base -DDESMUME_FORCE_CORE=2" ;;
		jiton)  echo "$base -DDESMUME_FORCE_CORE=2" ;;
		# ARM9 JIT A/B (A3): likewise sw renderer, delta is purely the ARM9 core.
		jit9off) echo "$base -DDESMUME_FORCE_CORE=2" ;;
		jit9on)  echo "$base -DDESMUME_FORCE_CORE=2" ;;
		# Full JIT: both cores JIT'd, GXMerge doing the actual compositing -
		# not an A/B baseline, this is the "real" fast-path configuration.
		jitfull) echo "$base -DDESMUME_FORCE_CORE=1 -DDESMUME_FORCE_GXCOMPOSITE" ;;
		# Same config as jitfull, plus the perf_zones frame-time accountant
		# (dumps sd:/perfzones.log). "where does the full-JIT frame go" mode.
		profile) echo "$base -DDESMUME_FORCE_CORE=1 -DDESMUME_FORCE_GXCOMPOSITE" ;;
		# profile + the Step 5.1a MAIN-text-BG-on-GX path armed at boot.
		profile2dbg) echo "$base -DDESMUME_FORCE_CORE=1 -DDESMUME_FORCE_GXCOMPOSITE -DDESMUME_FORCE_GX2DBG" ;;
		*)      die "unknown mode '$1'" ;;
	esac
}

# JITDEFS (separate Makefile var from TESTDEFS) for a mode; empty for every
# renderer-only mode.
jitdefs_for() {
	local base
	case "$1" in
		jiton)   base="-DDESMUME_JIT_ARM7" ;;
		jit9on)  base="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON" ;;
		jitfull) base="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON" ;;
		profile) base="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_PERFZONES" ;;
		profile2dbg) base="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_PERFZONES" ;;
		*)       base="" ;;
	esac
	# BENCH_EXTRA_JITDEFS: append experimental JIT flags to every JIT mode without
	# editing the mode table (e.g. BENCH_EXTRA_JITDEFS=-DSOME_NEW_JIT_FLAG for an
	# A/B of a gated codegen change). Empty by default -> no behaviour change.
	if [ -n "$base" ] && [ -n "${BENCH_EXTRA_JITDEFS:-}" ]; then
		base="$base ${BENCH_EXTRA_JITDEFS}"
	fi
	echo "$base"
}

LAST_JITDEFS="__unset__"   # forces a clean before the first build_mode call

build_mode() {
	local m="$1"
	local jd; jd="$(jitdefs_for "$m")"
	echo ">> build bench_$m.dol   (TESTDEFS: $(defs_for "$m")${jd:+   JITDEFS: $jd})"
	: > "$DOLDIR/build_$m.log"
	# JITDEFS touches every jit/*.cpp TU and depfiles don't track flag changes
	# (see the plan's BUILD GOTCHA) -- switching it needs a full `make clean`,
	# unlike the renderer-only TESTDEFS knobs which only main.o consumes.
	if [ "$jd" != "$LAST_JITDEFS" ]; then
		( cd "$ROOT" && make clean ) >>"$DOLDIR/build_$m.log" 2>&1
		LAST_JITDEFS="$jd"
	fi
	# only main.o consumes DESMUME_FORCE_*/DESMUME_BENCH, so once JITDEFS is
	# settled a forced rebuild of that one object + relink is enough.
	( cd "$ROOT" \
	  && rm -f build/main.o "$TARGET.elf" "$TARGET.dol" \
	  && make -j"$(nproc)" JITDEFS="$jd" TESTDEFS="$(defs_for "$m")" ) >>"$DOLDIR/build_$m.log" 2>&1 \
		|| { echo "   BUILD FAILED:"; tail -n 15 "$DOLDIR/build_$m.log"; exit 1; }
	cp "$ROOT/$TARGET.dol" "$DOLDIR/bench_$m.dol"
	echo "   -> $DOLDIR/bench_$m.dol"
}

#--- results dir -------------------------------------------------------------
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SHA="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)"
git -C "$ROOT" diff --quiet 2>/dev/null || SHA="${SHA}-dirty"
RUNDIR="$HERE/results/${STAMP}_${SHA}"
mkdir -p "$RUNDIR/raw" "$DOLDIR"

DOLPHIN_VER="$(timeout 20 flatpak run org.DolphinEmu.dolphin-emu --version 2>/dev/null | head -1)"
CPU="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
cat > "$RUNDIR/meta.json" <<JSON
{
  "timestamp": "$STAMP",
  "git": "$SHA",
  "host": "${CPU:-unknown}",
  "kernel": "$(uname -sr)",
  "dolphin": "${DOLPHIN_VER:-unknown}",
  "duration_default_s": $DUR_DEFAULT,
  "modes": "$MODES"
}
JSON

#--- build ------------------------------------------------------------------
if [ "$DO_BUILD" = 1 ]; then
	[ "$DO_CLEAN" = 1 ] && ( cd "$ROOT" && make clean >/dev/null 2>&1 || true )
	for m in $MODES; do build_mode "$m"; done
else
	for m in $MODES; do
		[ -f "$DOLDIR/bench_$m.dol" ] || die "--no-build but $DOLDIR/bench_$m.dol is missing"
	done
fi

#--- stage / restore the SD test rom --------------------------------------
HAVE_PREV=0
if mcopy -i "$DOLPHIN_SD" ::/DS/ROMS/test.nds "$RUNDIR/_prev_test.nds" 2>/dev/null; then
	HAVE_PREV=1
fi
restore_rom() {
	dolphin_kill
	if [ "$HAVE_PREV" = 1 ]; then
		mcopy -o -i "$DOLPHIN_SD" "$RUNDIR/_prev_test.nds" ::/DS/ROMS/test.nds 2>/dev/null \
			&& rm -f "$RUNDIR/_prev_test.nds"
	fi
	mdel -i "$DOLPHIN_SD" ::/bench.log 2>/dev/null || true
	mdel -i "$DOLPHIN_SD" ::/perfzones.log 2>/dev/null || true
}
trap restore_rom EXIT

#--- run one scene/mode ---------------------------------------------------
run_one() {
	local scene="$1" mode="$2" rom="$3" dur="$4"
	dolphin_kill; sleep 3
	mdel -i "$DOLPHIN_SD" ::/bench.log 2>/dev/null || true
	mdel -i "$DOLPHIN_SD" ::/perfzones.log 2>/dev/null || true
	mcopy -o -i "$DOLPHIN_SD" "$rom" ::/DS/ROMS/test.nds \
		|| { echo "   mcopy of $rom failed"; return 1; }
	echo ">> $scene / $mode   $(basename "$rom")   ${dur}s"
	setsid flatpak run org.DolphinEmu.dolphin-emu -b -e "$DOLDIR/bench_$mode.dol" \
		-C Dolphin.Core.WiiSDCard=True -C Dolphin.DSP.Volume=0 \
		> "$RUNDIR/raw/dolphin_${scene}_${mode}.log" 2>&1 &
	disown 2>/dev/null || true   # we reap it with pkill; don't want the job-control "Killed" line
	sleep "$dur"
	dolphin_kill; sleep 3
	if mcopy -i "$DOLPHIN_SD" ::/bench.log "$RUNDIR/raw/${scene}_${mode}.log" 2>/dev/null; then
		echo "   captured $(grep -c ',' "$RUNDIR/raw/${scene}_${mode}.log") rows"
	else
		echo "   !! no bench.log - see raw/dolphin_${scene}_${mode}.log"
	fi
	# perf_zones frame-time breakdown (only a -DDESMUME_PERFZONES build writes it)
	if mcopy -i "$DOLPHIN_SD" ::/perfzones.log "$RUNDIR/raw/${scene}_${mode}.perfzones.log" 2>/dev/null; then
		echo "   captured $(grep -c ',' "$RUNDIR/raw/${scene}_${mode}.perfzones.log") perfzone rows"
		mdel -i "$DOLPHIN_SD" ::/perfzones.log 2>/dev/null || true
	fi
}

#--- matrix --------------------------------------------------------------
in_filter() { case " $1 " in *" $2 "*) return 0 ;; *) return 1 ;; esac; }

RAN=0
while IFS='|' read -r id rom window label dur; do
	id="$(echo "${id:-}" | xargs)"
	[ -z "$id" ] && continue
	case "$id" in \#*) continue ;; esac
	rom="$(echo "${rom:-}" | xargs)"; window="$(echo "${window:-}" | xargs)"
	dur="$(echo "${dur:-}" | xargs | sed 's/[^0-9]//g')"
	[ -n "$SCENE_FILTER" ] && ! in_filter "$SCENE_FILTER" "$id" && continue

	rom="${rom/#\~/$HOME}"
	case "$rom" in /*) : ;; *) rom="$ROOT/$rom" ;; esac
	if [ ! -f "$rom" ]; then echo ">> skip $id: rom not found ($rom)"; continue; fi

	for m in $MODES; do
		run_one "$id" "$m" "$rom" "${dur:-$DUR_DEFAULT}"
		RAN=$((RAN + 1))
	done
done < "$HERE/scenes.conf"

restore_rom; trap - EXIT

[ "$RAN" = 0 ] && die "no scenes ran (check scenes.conf / --scenes filter)"

#--- analyse ------------------------------------------------------------
echo
python3 "$HERE/analyze.py" "$RUNDIR" $COMPARE_ARG

# perf_zones frame-time breakdown, if a `profile` (-DDESMUME_PERFZONES) run
# left any *.perfzones.log captures.
if ls "$RUNDIR"/raw/*.perfzones.log >/dev/null 2>&1; then
	echo
	python3 "$HERE/perfzones.py" "$RUNDIR" --json
fi
echo
echo "results: $RUNDIR"