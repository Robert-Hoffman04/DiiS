#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# desmumewii benchmark - run.sh
#
# One pass over everything: the renderer/JIT fps matrix (scenes.conf) and the
# correctness probe ROMs (wrestlers.conf), all captured live over the unified
# test harness's network transport (no SD-file polling, no blind sleeps -
# see tools/harness-control/README.md for the transport itself). Writes one
# results/<stamp>_<sha>/ directory and hands it to analyze.py, which prints a
# summary and diffs against the most recent previous run.
#
# See README.md for usage. Run with --help for the option list.
#-----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/lib.sh"

MODES="sw gxa gxf"
SCENE_FILTER="" WRESTLER_FILTER=""
DO_BUILD=1 DO_CLEAN=0 COMPARE_ARG="" DO_PERF=1 DO_WRESTLERS=1
PROBE_CPUMODE=""
TIMEOUT_DEFAULT=180

usage() {
	cat <<'EOF'
Usage: tools/benchmark/run.sh [options]
  --modes "sw gxa gxf" render modes to sweep (default all three, all with
                       the JIT on). sw = RenderMode::Software, gxa =
                       GxAccurate, gxf = GxFast (gx = alias for gxa). Also
                       available (CPU A/B, software render): interp (both
                       cores interpreted) and jit (both cores JITted; same
                       run as sw). ALL modes share ONE dol; the mode is
                       switched at connect via PKT_CTRL "rendermode ..." +
                       "cpumode interp|jit"
  --scenes "vsd ph"    subset of scene ids from scenes.conf
  --wrestlers "id id"  subset of ids from wrestlers.conf
  --no-build           reuse tools/benchmark/dols/*.dol
  --clean              `make clean` before the first build
  --timeout N          per-run safety-net seconds (default 180;
                        scenes.conf 'timeout=' column wins per scene)
  --no-perf            skip the fps matrix, run correctness probes only
  --no-wrestlers        skip the correctness probes, run the fps matrix only
  --no-compare         skip the diff against the previous run
  --probe-cpumode interp
                       run the correctness probes in Interpreter mode (sends
                       "cpumode interp" at connect). Default: leave the
                       probes' own JIT setup alone (JIT on, with the
                       armwrestler/arm7wrestler single-core overrides)
Env overrides: DOLPHIN_DATA, DOLPHIN_SD, DEVKITPRO, DEVKITPPC, HARNESS_HOST, HARNESS_PORT
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--no-build)    DO_BUILD=0 ;;
		--clean)       DO_CLEAN=1 ;;
		--timeout)     TIMEOUT_DEFAULT="$2"; shift ;;
		--modes)       MODES="$2"; shift ;;
		--scenes)      SCENE_FILTER="$2"; shift ;;
		--wrestlers)   WRESTLER_FILTER="$2"; shift ;;
		--no-perf)     DO_PERF=0 ;;
		--no-wrestlers) DO_WRESTLERS=0 ;;
		--no-compare)  COMPARE_ARG="--no-compare" ;;
		--probe-cpumode) PROBE_CPUMODE="$2"; shift ;;
		-h|--help)     usage; exit 0 ;;
		*)             echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

bench_preflight
[ -f "$HERE/scenes.conf" ]    || die "missing $HERE/scenes.conf"
[ -f "$HERE/wrestlers.conf" ] || die "missing $HERE/wrestlers.conf"

# TESTDEFS common to every perf mode: harness net transport + always-on
# perf-zones breakdown (cheap - see perf_zones.cpp) + fixed test ROM slot.
perf_defs_for() {
	local base="-DDESMUME_FORCE_ROM -DDESMUME_AUTOLOADSTATE -DDESMUME_HARNESS -DHARNESS_TRANSPORT_NET -DDESMUME_PERFZONES -DDESMUME_FORCE_CORE=1"
	case "$1" in
		sw|gx|gxa|gxf|interp|jit) echo "$base" ;;
		*) die "unknown mode '$1' (want sw gx gxa gxf interp jit)" ;;
	esac
}
# Every mode differs only in runtime state (render mode, CPU mode), so they
# all share one dol (build once). The JIT is always compiled in (plain make);
# JITDEFS stays empty - it only carries debug defines now (see ab.sh).
dol_for_mode() { echo perf; }
# PKT_CTRL commands to send after connect, one per line. The device applies
# them before its first frame (main.cpp harness_poll_host), so an interp run
# never emulates a single frame on the JIT. Render modes always say "cpumode
# jit" explicitly: they are the renderer comparison at the emulator's actual
# fastest CPU config, not an interpreter-only baseline.
ctrl_for_mode() {
	case "$1" in
		sw|jit)  echo "rendermode software"; echo "cpumode jit" ;;
		interp)  echo "rendermode software"; echo "cpumode interp" ;;
		gx|gxa)  echo "rendermode accurate"; echo "cpumode jit" ;;
		gxf)     echo "rendermode fast";     echo "cpumode jit" ;;
	esac
}

in_filter() { local list="$1" v="$2"; [ -z "$list" ] && return 0; case " $list " in *" $v "*) return 0 ;; *) return 1 ;; esac; }

#--- results dir --------------------------------------------------------------
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SHA="$(git -C "$BENCH_ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)"
git -C "$BENCH_ROOT" diff --quiet 2>/dev/null || SHA="${SHA}-dirty"
RUNDIR="$HERE/results/${STAMP}_${SHA}"
mkdir -p "$RUNDIR/raw"
bench_write_meta "$RUNDIR" "$TIMEOUT_DEFAULT" "$MODES"

bench_sd_snapshot "$RUNDIR"
trap bench_sd_restore EXIT

RAN=0

#--- perf matrix ---------------------------------------------------------
if [ "$DO_PERF" = 1 ]; then
	[ "$DO_CLEAN" = 1 ] && ( cd "$BENCH_ROOT" && make clean >/dev/null 2>&1 || true )
	BUILT=" "
	for m in $MODES; do
		d="$(dol_for_mode "$m")"
		case "$BUILT" in *" $d "*) continue ;; esac
		BUILT="$BUILT$d "
		if [ "$DO_BUILD" = 1 ]; then
			build_dol "$d" "$(perf_defs_for "$m")" ""
		else
			[ -f "$DOLDIR/$d.dol" ] || die "--no-build but $DOLDIR/$d.dol is missing"
		fi
	done

	while IFS='|' read -r id rom window label timeout state; do
		id="$(trim "${id:-}")"
		[ -z "$id" ] && continue
		case "$id" in \#*) continue ;; esac
		rom="$(trim "${rom:-}")"; window="$(trim "${window:-}")"
		timeout="$(trim "${timeout:-}" | sed 's/[^0-9]//g')"
		state="$(trim "${state:-}")"
		in_filter "$SCENE_FILTER" "$id" || continue

		rom="${rom/#\~/$HOME}"; case "$rom" in /*) : ;; *) rom="$BENCH_ROOT/$rom" ;; esac
		if [ ! -f "$rom" ]; then echo ">> skip $id: rom not found ($rom)"; continue; fi
		if [ -n "$state" ]; then
			state="${state/#\~/$HOME}"; case "$state" in /*) : ;; *) state="$BENCH_ROOT/$state" ;; esac
			[ -f "$state" ] || { echo ">> skip $id: state not found ($state)"; continue; }
		fi

		hi="${window##*-}"; [ "$hi" = "$window" ] && hi=""   # no '-' -> auto window, no explicit stop frame
		stop_args=()
		[ -n "$hi" ] && stop_args=(--until-frame "$((hi + 30))")

		for m in $MODES; do
			outdir="$RUNDIR/raw/perf_${id}_${m}"
			echo ">> perf $id / $m   $(basename "$rom")"
			dolphin_kill; sleep 2
			stage_rom "$rom" "$state"
			ctrl_args=()
			while IFS= read -r c; do
				[ -n "$c" ] && ctrl_args+=(--ctrl-cmd "$c")
			done < <(ctrl_for_mode "$m")
			capture_run "$(dol_for_mode "$m")" "$outdir" \
				"${ctrl_args[@]}" \
				"${stop_args[@]}" \
				--timeout "${timeout:-$TIMEOUT_DEFAULT}"
			rc=$?
			[ "$rc" = 2 ] && echo "   !! device never connected - check Dolphin network passthrough (plan §0)"
			RAN=$((RAN + 1))
		done
	done < "$HERE/scenes.conf"
fi

#--- correctness probes ---------------------------------------------------
if [ "$DO_WRESTLERS" = 1 ]; then
	while IFS='|' read -r id tooldir outname probeflag settle label; do
		id="$(trim "${id:-}")"
		[ -z "$id" ] && continue
		case "$id" in \#*) continue ;; esac
		tooldir="$(trim "${tooldir:-}")"; outname="$(trim "${outname:-}")"
		probeflag="$(trim "${probeflag:-}")"; settle="$(trim "${settle:-}")"
		in_filter "$WRESTLER_FILTER" "$id" || continue

		echo ">> wrestler $id"
		rom="$(build_wrestler "$tooldir" "$outname")"
		[ -f "$rom" ] || { echo "   !! build produced no $rom"; continue; }

		if [ "$DO_BUILD" = 1 ]; then
			build_dol "wrestler_$id" \
				"-DDESMUME_FORCE_ROM -DDESMUME_HARNESS -DHARNESS_TRANSPORT_NET -DDESMUME_PERFZONES -D$probeflag -DDESMUME_FORCE_CORE=1" \
				""
		fi

		outdir="$RUNDIR/raw/wrestler_${id}"
		dolphin_kill; sleep 2
		stage_rom "$rom"
		probe_args=()
		[ -n "$PROBE_CPUMODE" ] && probe_args=(--ctrl-cmd "cpumode $PROBE_CPUMODE")
		capture_run "wrestler_$id" "$outdir" \
			"${probe_args[@]}" \
			--settle-frame "${settle:-300}" --capture-frame "$id" \
			--timeout "$TIMEOUT_DEFAULT"
		rc=$?
		[ "$rc" = 2 ] && echo "   !! device never connected - check Dolphin network passthrough (plan §0)"
		RAN=$((RAN + 1))
	done < "$HERE/wrestlers.conf"
fi

bench_sd_restore; trap - EXIT

[ "$RAN" = 0 ] && die "nothing ran (check scenes.conf/wrestlers.conf and --scenes/--wrestlers filters)"

#--- analyse ---------------------------------------------------------------
echo
python3 "$HERE/analyze.py" "$RUNDIR" $COMPARE_ARG
echo
echo "results: $RUNDIR"
