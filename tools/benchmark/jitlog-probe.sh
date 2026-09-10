#!/usr/bin/env bash
# jitlog-probe.sh - build a DESMUME_JIT_TRACE_FIRST dol, run SM64DS headless
# through Dolphin for a fixed window, and pull sd:/jit.log. One-off telemetry
# capture (chain length / ins-per-entry / bail rates) - not part of the
# regular benchmark matrix.
#
# Usage: tools/benchmark/jitlog-probe.sh [seconds]   (default 90)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DUR="${1:-90}"

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC MTOOLS_SKIP_CHECK=1
. "$ROOT/tools/harness-control/common.sh"   # dolphin_launch / dolphin_kill (plan §3.7)

ROM="$ROOT/Super Mario 64 DS (USA, Australia) (Rev 1).nds"
OUT="$HERE/results/_jitlog_$(date -u +%Y%m%dT%H%M%SZ).log"
TESTDEFS="-DDESMUME_FORCE_ROM -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=200000 -DDESMUME_FORCE_CORE=2"
JITDEFS="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_JIT_TRACE_FIRST"

echo ">> build (JITDEFS: $JITDEFS)"
( cd "$ROOT" && make clean >/dev/null 2>&1 && make -j"$(nproc)" JITDEFS="$JITDEFS" TESTDEFS="$TESTDEFS" ) >"$HERE/results/_jitlog_build.log" 2>&1 \
	|| { echo "BUILD FAILED"; tail -20 "$HERE/results/_jitlog_build.log"; exit 1; }

mcopy -i "$DOLPHIN_SD" ::/DS/ROMS/test.nds "$HERE/results/_jitlog_prev.nds" 2>/dev/null && HAVE_PREV=1 || HAVE_PREV=0
restore() {
	dolphin_kill
	[ "$HAVE_PREV" = 1 ] && mcopy -o -i "$DOLPHIN_SD" "$HERE/results/_jitlog_prev.nds" ::/DS/ROMS/test.nds 2>/dev/null && rm -f "$HERE/results/_jitlog_prev.nds"
	mdel -i "$DOLPHIN_SD" ::/jit.log 2>/dev/null||true
	mdel -i "$DOLPHIN_SD" ::/bench.log 2>/dev/null||true
}
trap restore EXIT

dolphin_kill; sleep 2
mdel -i "$DOLPHIN_SD" ::/jit.log 2>/dev/null||true
mcopy -o -i "$DOLPHIN_SD" "$ROM" ::/DS/ROMS/test.nds || { echo "mcopy rom failed"; exit 1; }

echo ">> run ${DUR}s"
dolphin_launch "$ROOT/desmumewii.dol" /dev/null
sleep "$DUR"
dolphin_kill; sleep 3

if mcopy -i "$DOLPHIN_SD" ::/jit.log "$OUT" 2>/dev/null; then
	echo ">> captured -> $OUT"
	echo "---- last a9 tally / alive lines ----"
	grep -E "a9 tally|alive:|arm9 mix" "$OUT" | tail -12
else
	echo "!! no jit.log produced"
fi
