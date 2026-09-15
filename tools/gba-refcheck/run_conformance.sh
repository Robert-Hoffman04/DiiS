#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# tools/gba-refcheck/run_conformance.sh - GBA conformance suite driver
# (PLAN.md §4.3 item 9 / §5.3).
#
# Builds a dedicated .dol per gbaconf.conf row (one -DDESMUME_GBA_*_SOAK
# probe + its companion synthetic ROM, same pattern as the individual
# item-2/3/4/6/7 soaks this session), runs it headless under Dolphin over
# the unified test harness's network transport, captures the probe's
# deterministic PKT_PROFILE checkpoint lines, and diffs them byte-for-byte
# against a committed golden/<id>.txt.
#
# Reuses tools/benchmark/lib.sh's build_dol/stage_rom/capture_run/
# bench_sd_snapshot primitives (same Dolphin session-control plumbing as
# run.sh/ab.sh/soak.sh - not reimplemented here) and capture.py's existing
# --until-frame stop condition (it already recognises both a perf-zones CSV
# frame row and any "frame=N" text, which these probes' own
# harness_profile_emitf lines carry).
#
# Usage:
#   tools/gba-refcheck/run_conformance.sh                # run + diff (default)
#   tools/gba-refcheck/run_conformance.sh --record        # (re)write golden/
#   tools/gba-refcheck/run_conformance.sh --tests "irqsoak apusoak"
#   tools/gba-refcheck/run_conformance.sh --no-build       # reuse dols/*.dol
#   tools/gba-refcheck/run_conformance.sh --timeout 120    # per-test default
#
# Exit code: 0 if every ran test PASSes, 1 otherwise (build failure, device
# never connected, missing golden, or a diff). Kills Dolphin the instant
# each test's capture ends, and again on exit via the same trap run.sh uses.
#-----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/../benchmark"
# shellcheck source=../benchmark/lib.sh
. "$BENCH/lib.sh"

CONF="$HERE/gbaconf.conf"
GOLDEN_DIR="$HERE/golden"
DO_BUILD=1 DO_RECORD=0 FILTER="" TIMEOUT_DEFAULT=""

usage() {
	cat <<'EOF'
Usage: tools/gba-refcheck/run_conformance.sh [options]
  --record          (re)write golden/<id>.txt from this run's actual output
                     instead of diffing against it
  --tests "id id"   subset of ids from gbaconf.conf (default: all)
  --no-build        reuse tools/gba-refcheck/dols/*.dol instead of rebuilding
  --timeout N       override every row's timeout column with N seconds
  -h, --help        this text
Env overrides (see tools/benchmark/lib.sh): DOLPHIN_DATA, DOLPHIN_SD,
DEVKITPRO, DEVKITPPC, HARNESS_HOST, HARNESS_PORT
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--record)   DO_RECORD=1 ;;
		--no-build) DO_BUILD=0 ;;
		--tests)    FILTER="$2"; shift ;;
		--timeout)  TIMEOUT_DEFAULT="$2"; shift ;;
		-h|--help)  usage; exit 0 ;;
		*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

bench_preflight
[ -f "$CONF" ] || die "missing $CONF"
mkdir -p "$GOLDEN_DIR"

# This suite builds its own .dol per row directly under tools/gba-refcheck/
# (not tools/benchmark/dols/) so it never collides with, or gets swept by,
# the fps-matrix/wrestler dols living there.
DOLDIR="$HERE/dols"
mkdir -p "$DOLDIR"

filter_ok() { local list="$1" v="$2"; [ -z "$list" ] && return 0; case " $list " in *" $v "*) return 0 ;; *) return 1 ;; esac; }

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTROOT="$HERE/results/$STAMP"
mkdir -p "$OUTROOT"

bench_sd_snapshot "$OUTROOT"
trap bench_sd_restore EXIT

RAN=0 PASS=0 FAIL=0
FAILED_IDS=""

while IFS='|' read -r id rom probeflag untilframe timeout jitdefs label; do
	id="$(trim "${id:-}")"
	[ -z "$id" ] && continue
	case "$id" in \#*) continue ;; esac
	rom="$(trim "${rom:-}")"; probeflag="$(trim "${probeflag:-}")"
	untilframe="$(trim "${untilframe:-}")"
	timeout="$(trim "${timeout:-}")"; jitdefs="$(trim "${jitdefs:-}")"
	label="$(trim "${label:-}")"
	filter_ok "$FILTER" "$id" || continue

	# "-" means explicitly empty JITDEFS (pure interpreter); blank means the
	# suite default (both JITs on).
	case "$jitdefs" in
		-) jitdefs="" ;;
		"") jitdefs="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON" ;;
	esac

	romfile="$HERE/$rom"
	if [ ! -f "$romfile" ]; then
		echo ">> skip $id: rom not found ($romfile) - run tools/gba-refcheck/build_roms.sh first"
		continue
	fi

	echo ">> gba-conformance $id ($label)"
	if [ "$DO_BUILD" = 1 ]; then
		BENCH_HERE="$HERE" DOLDIR="$DOLDIR" build_dol "gbaconf_$id" \
			"-DDESMUME_FORCE_ROM -DDESMUME_HARNESS -DHARNESS_TRANSPORT_NET -DDESMUME_PERFZONES -D$probeflag -DDESMUME_FORCE_CORE=2" \
			"$jitdefs"
	elif [ ! -f "$DOLDIR/gbaconf_$id.dol" ]; then
		die "--no-build but $DOLDIR/gbaconf_$id.dol is missing"
	fi

	outdir="$OUTROOT/$id"
	mkdir -p "$outdir"
	dolphin_kill; sleep 2
	stage_rom "$romfile"

	eff_timeout="${TIMEOUT_DEFAULT:-${timeout:-120}}"
	DOLDIR="$DOLDIR" capture_run "gbaconf_$id" "$outdir" --until-frame "$untilframe" --timeout "$eff_timeout"
	rc=$?
	RAN=$((RAN + 1))

	if [ "$rc" = 2 ]; then
		echo "   FAIL $id: device never connected (check Dolphin network passthrough, PLAN §7.1)"
		FAIL=$((FAIL + 1)); FAILED_IDS="$FAILED_IDS $id"
		continue
	fi

	# Keep only this suite's own marker lines - drop the DESMUME_PERFZONES
	# CSV rows (wall_us is never expected to be byte-identical run to run,
	# only used here to drive --until-frame) and anything else on the wire.
	actual="$outdir/profile.filtered.log"
	grep -E "^(irqsoak|apusoak|waitcntsoak|dmasoak|sstest) " "$outdir/profile.log" > "$actual" 2>/dev/null || true

	if [ ! -s "$actual" ]; then
		echo "   FAIL $id: no probe output captured (check $outdir/profile.log / dolphin.log)"
		FAIL=$((FAIL + 1)); FAILED_IDS="$FAILED_IDS $id"
		continue
	fi

	golden="$GOLDEN_DIR/$id.txt"
	if [ "$DO_RECORD" = 1 ]; then
		cp "$actual" "$golden"
		echo "   RECORDED $id -> $golden"
		PASS=$((PASS + 1))
		continue
	fi

	if [ ! -f "$golden" ]; then
		echo "   FAIL $id: no golden file at $golden (run with --record first)"
		FAIL=$((FAIL + 1)); FAILED_IDS="$FAILED_IDS $id"
		continue
	fi

	if diff -u "$golden" "$actual" > "$outdir/diff.txt"; then
		echo "   PASS $id"
		PASS=$((PASS + 1))
	else
		echo "   FAIL $id: profile output differs from golden/$id.txt (see $outdir/diff.txt)"
		FAIL=$((FAIL + 1)); FAILED_IDS="$FAILED_IDS $id"
	fi
done < "$CONF"

bench_sd_restore; trap - EXIT

echo
echo "gba-conformance: $PASS/$RAN PASS"
[ -n "$FAILED_IDS" ] && echo "FAILED:$FAILED_IDS"
echo "results: $OUTROOT"

[ "$RAN" = 0 ] && die "nothing ran (check $CONF and --tests filter)"
[ "$FAIL" = 0 ]
