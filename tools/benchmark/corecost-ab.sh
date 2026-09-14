#!/usr/bin/env bash
# corecost-ab.sh - A/B the repeated-bail demotion fix (jit_exec.cpp,
# BailTrack/trackRepeatedBail) against the pre-fix tree, using
# -DJIT_CORE_COST_HISTO over the unified harness's PKT_PROFILE sink.
#
# Builds two dols (baseline = HEAD, i.e. the fix stashed out; after = working
# tree, i.e. the fix in), runs each through SM64DS headless Dolphin for a
# fixed window, captures jitcorecost/jitcorecost2 lines via wii_control.py,
# and prints the ARM7 bail0/spin_hits delta between the two.
#
# Requires: Dolphin's Wii network passthrough already enabled (plan §0) -
# this is a one-time Dolphin config setting, not something this script sets.
#
# Usage: tools/benchmark/corecost-ab.sh [seconds]   (default 150)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DUR="${1:-150}"

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC MTOOLS_SKIP_CHECK=1
. "$ROOT/tools/harness-control/common.sh"   # dolphin_launch / dolphin_kill

ROM="$ROOT/Super Mario 64 DS (USA, Australia) (Rev 1).nds"
STATE="$HERE/states/sm64.ds0"
RESDIR="$HERE/results/_corecost_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RESDIR"
TESTDEFS="-DDESMUME_FORCE_ROM -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=200000 -DDESMUME_FORCE_CORE=1 -DDESMUME_AUTOLOADSTATE"
JITDEFS="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DDESMUME_HARNESS -DHARNESS_TRANSPORT_NET -DDESMUME_PERFZONES -DJIT_CORE_COST_HISTO"

mcopy -i "$DOLPHIN_SD" ::/DS/ROMS/test.nds "$RESDIR/_prev.nds" 2>/dev/null && HAVE_PREV=1 || HAVE_PREV=0
mcopy -i "$DOLPHIN_SD" ::/DS/SAVES/test.ds0 "$RESDIR/_prev.ds0" 2>/dev/null && HAVE_PREV_STATE=1 || HAVE_PREV_STATE=0
SERVER_PID=""
restore() {
	dolphin_kill
	[ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
	[ "$HAVE_PREV" = 1 ] && mcopy -o -i "$DOLPHIN_SD" "$RESDIR/_prev.nds" ::/DS/ROMS/test.nds 2>/dev/null && rm -f "$RESDIR/_prev.nds"
	if [ "$HAVE_PREV_STATE" = 1 ]; then
		mcopy -o -i "$DOLPHIN_SD" "$RESDIR/_prev.ds0" ::/DS/SAVES/test.ds0 2>/dev/null && rm -f "$RESDIR/_prev.ds0"
	else
		mdel -i "$DOLPHIN_SD" ::/DS/SAVES/test.ds0 2>/dev/null || true
	fi
}
trap restore EXIT

build_and_run() {
	local tag="$1"
	echo ">> [$tag] build (JITDEFS: $JITDEFS)"
	( cd "$ROOT" && make clean >/dev/null 2>&1 && make -j"$(nproc)" JITDEFS="$JITDEFS" TESTDEFS="$TESTDEFS" ) >"$RESDIR/_build_$tag.log" 2>&1 \
		|| { echo "BUILD FAILED ($tag)"; tail -30 "$RESDIR/_build_$tag.log"; exit 1; }

	echo ">> [$tag] server up"
	"$ROOT/tools/harness-control/wii_control.py" --host 127.0.0.1 --port 4300 \
		--out "$RESDIR/$tag" --map "$ROOT/desmumewii.elf.map" >"$RESDIR/_server_$tag.log" 2>&1 &
	SERVER_PID=$!
	sleep 1

	dolphin_kill; sleep 2
	mcopy -o -i "$DOLPHIN_SD" "$ROM" ::/DS/ROMS/test.nds || { echo "mcopy rom failed"; exit 1; }
	mcopy -o -i "$DOLPHIN_SD" "$STATE" ::/DS/SAVES/test.ds0 || { echo "mcopy state failed"; exit 1; }
	echo ">> [$tag] run ${DUR}s"
	dolphin_launch "$ROOT/desmumewii.dol" /dev/null
	sleep "$DUR"
	dolphin_kill; sleep 3
	kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null
	SERVER_PID=""
}

echo "== BASELINE (fix stashed out) =="
git -C "$ROOT" stash push -q -m corecost-ab-baseline -- source/src/jit/jit_exec.cpp
build_and_run baseline
git -C "$ROOT" stash pop -q

echo "== AFTER (fix in, working tree as-is) =="
build_and_run after

echo
echo "==== ARM7 jitcorecost summary ===="
for tag in baseline after; do
	f="$RESDIR/$tag/profile.log"
	[ -f "$f" ] || { echo "$tag: no profile.log"; continue; }
	echo "-- $tag --"
	grep "jitcorecost frame=" "$f" | grep "core=arm7" | tail -3
	grep "jitcorecost2 frame=" "$f" | grep "core=arm7" | tail -3
done
echo
echo "Results dir: $RESDIR"
