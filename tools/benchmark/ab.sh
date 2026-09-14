#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# desmumewii benchmark - ab.sh
#
# Generic two-config A/B probe: build two dols (a "before" and an "after"
# JITDEFS/TESTDEFS pair, optionally with the working tree's uncommitted
# changes stashed out for the "before" leg), run each through the same
# ROM/state/window over the network harness, and show the matching
# profile.log lines from both side by side.
#
# Replaces writing a new one-off *-ab.sh script per investigation - what
# used to be a bespoke script is now one command line. See README.md for
# worked examples (the old assoc-ab.sh/corecost-ab.sh/hashhisto-ab.sh/
# item6-ab.sh investigations, reproduced as ab.sh invocations).
#-----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/lib.sh"

LABEL_A=before LABEL_B=after
STASH=0
TESTDEFS="-DDESMUME_FORCE_ROM -DDESMUME_AUTOLOADSTATE -DDESMUME_HARNESS -DHARNESS_TRANSPORT_NET -DDESMUME_PERFZONES -DDESMUME_FORCE_CORE=1"
JITDEFS_A="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON"
JITDEFS_B="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON"
ROM="$BENCH_ROOT/Super Mario 64 DS (USA, Australia) (Rev 1).nds"
STATE="$HERE/states/sm64.ds0"
TIMEOUT=150
GREP=""

usage() {
	cat <<'EOF'
Usage: tools/benchmark/ab.sh [options]
  --label-a NAME       name for the "before" leg (default: before)
  --label-b NAME       name for the "after" leg (default: after)
  --stash              git stash uncommitted changes for the "before" leg,
                        pop them back for the "after" leg (so "before" is
                        HEAD and "after" is the working tree)
  --testdefs "..."     TESTDEFS shared by both legs
  --jitdefs-a "..."    JITDEFS for the "before" leg
  --jitdefs-b "..."    JITDEFS for the "after" leg
  --rom PATH           ROM to boot (default: SM64DS)
  --state PATH         savestate to autoload (default: tools/benchmark/states/sm64.ds0;
                        pass "" for none)
  --timeout N          capture.py --timeout seconds per leg (default 150)
  --grep REGEX         only show profile.log lines matching REGEX (default: all)
Env overrides: same as run.sh (DOLPHIN_DATA, DOLPHIN_SD, HARNESS_HOST, HARNESS_PORT, ...)
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--label-a)   LABEL_A="$2"; shift ;;
		--label-b)   LABEL_B="$2"; shift ;;
		--stash)     STASH=1 ;;
		--testdefs)  TESTDEFS="$2"; shift ;;
		--jitdefs-a) JITDEFS_A="$2"; shift ;;
		--jitdefs-b) JITDEFS_B="$2"; shift ;;
		--rom)       ROM="$2"; shift ;;
		--state)     STATE="$2"; shift ;;
		--timeout)   TIMEOUT="$2"; shift ;;
		--grep)      GREP="$2"; shift ;;
		-h|--help)   usage; exit 0 ;;
		*)           echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

bench_preflight
[ -f "$ROM" ] || die "rom not found: $ROM"
[ -n "$STATE" ] && { [ -f "$STATE" ] || die "state not found: $STATE"; }

RESDIR="$HERE/results/_ab_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RESDIR"
bench_sd_snapshot "$RESDIR"

POPPED=0
restore_all() {
	bench_sd_restore
	if [ "$STASH" = 1 ] && [ "$POPPED" = 0 ]; then
		git -C "$BENCH_ROOT" stash pop -q 2>/dev/null || true
	fi
}
trap restore_all EXIT

run_leg() {
	local name="$1" jitdefs="$2"
	build_dol "ab_$name" "$TESTDEFS" "$jitdefs"
	dolphin_kill; sleep 2
	stage_rom "$ROM" "$STATE"
	capture_run "ab_$name" "$RESDIR/$name" --timeout "$TIMEOUT"
}

if [ "$STASH" = 1 ]; then
	echo ">> git stash (before leg = HEAD)"
	git -C "$BENCH_ROOT" stash push -q -m "ab.sh $LABEL_A/$LABEL_B" \
		|| die "git stash failed (clean tree? nothing to stash)"
fi

echo ">> leg A: $LABEL_A"
run_leg "$LABEL_A" "$JITDEFS_A"

if [ "$STASH" = 1 ]; then
	echo ">> git stash pop (after leg = working tree)"
	git -C "$BENCH_ROOT" stash pop -q
	POPPED=1
fi

echo ">> leg B: $LABEL_B"
run_leg "$LABEL_B" "$JITDEFS_B"

restore_all; trap - EXIT

echo
for name in "$LABEL_A" "$LABEL_B"; do
	echo "=== $name ==="
	python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print('  frame=%d asserts=%d/%d crashes=%d'%(d['frame'],d['asserts_pass'],d['asserts_pass']+d['asserts_fail'],d['crashes']))" \
		"$RESDIR/$name/summary.json" 2>/dev/null || echo "  (no summary.json)"
	if [ -n "$GREP" ]; then
		grep -E "$GREP" "$RESDIR/$name/profile.log" 2>/dev/null || echo "  (no matching profile.log lines)"
	fi
	echo
done

echo "raw captures: $RESDIR/$LABEL_A/  $RESDIR/$LABEL_B/"
