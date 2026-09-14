#!/usr/bin/env bash
# item6-ab.sh - A/B the two TODO item 6 chain-length fixes (jit_arm.cpp:
# BXcc lr widened to ARM7, and the ADD/SUB{cc} pc,pc,Rm register-form
# jump-table widening) against the tree as it stood right after item 4
# (2-way associativity, commit f92719a), using -DJIT_CORE_COST_HISTO
# -DDESMUME_PERFZONES over the unified harness's PKT_PROFILE sink.
#
# Isolates jit_arm.cpp specifically (checked out from f92719a for the
# baseline leg) rather than git-stashing, since the BXcc-lr half of the
# fix is already committed and stashing can't reach "before" it.
#
# Requires: Dolphin's Wii network passthrough already enabled (plan §0).
#
# Usage: tools/benchmark/item6-ab.sh [seconds]   (default 150)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DUR="${1:-150}"
JITARM="$ROOT/source/src/jit/jit_arm.cpp"
BASELINE_REV="f92719a"   # right after item 4, before item 5/6

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC MTOOLS_SKIP_CHECK=1
. "$ROOT/tools/harness-control/common.sh"

ROM="$ROOT/Super Mario 64 DS (USA, Australia) (Rev 1).nds"
STATE="$HERE/states/sm64.ds0"
RESDIR="$HERE/results/_item6_$(date -u +%Y%m%dT%H%M%SZ)"
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

cp "$JITARM" "$RESDIR/_jit_arm_after.cpp"   # save working-tree (after) version

echo "== BASELINE (jit_arm.cpp @ $BASELINE_REV, right after item 4) =="
git -C "$ROOT" show "$BASELINE_REV:source/src/jit/jit_arm.cpp" > "$JITARM"
build_and_run baseline
cp "$RESDIR/_jit_arm_after.cpp" "$JITARM"

echo "== AFTER (BXcc-lr + jump-table widening, working tree as-is) =="
build_and_run after

echo
echo "==== per-core jitcorecost summary ===="
for tag in baseline after; do
	f="$RESDIR/$tag/profile.log"
	[ -f "$f" ] || { echo "$tag: no profile.log"; continue; }
	echo "-- $tag --"
	for core in arm7 arm9; do
		echo "  [$core]"
		grep "jitcorecost frame=" "$f" | grep "core=$core" | tail -2
		grep "jitcorecost2 frame=" "$f" | grep "core=$core" | tail -2
	done
done
echo
echo "==== perf_zones frame-time summary (arm7_jit / arm9_jit) ===="
for tag in baseline after; do
	f="$RESDIR/$tag/profile.log"
	[ -f "$f" ] || continue
	echo "-- $tag --"
	grep -i "zone.*arm7_jit\|zone.*arm9_jit\|pz_arm7\|pz_arm9" "$f" | tail -6
done
echo
echo "Results dir: $RESDIR"
