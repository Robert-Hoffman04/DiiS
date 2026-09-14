#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# desmumewii benchmark - soak.sh
#
# JIT_DIFFERENTIAL_TESTING soak: build a dol that runs the interpreter and
# both JITs side by side, comparing guest state after every instruction, and
# run it against a ROM for a fixed duration. A correctness gate, not a
# benchmark - "pass" is zero mismatch/canary/overrun lines, not an fps
# number. This journal (sd:/jit.log) is a separate, older mechanism from the
# perf-zones/PKT_PROFILE stream the rest of this directory uses - it isn't
# wired onto the network harness, so this still pulls the SD file directly.
#-----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/lib.sh"

DUR=210
ROM="$BENCH_ROOT/testdata/Super Mario 64 DS (USA, Australia) (Rev 1).nds"
JITDEFS="-DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON -DJIT_DIFFERENTIAL_TESTING"

usage() {
	cat <<'EOF'
Usage: tools/benchmark/soak.sh [--duration N] [--rom PATH] [--jitdefs "..."]
  --duration N   seconds to run (default 210)
  --rom PATH     ROM to boot (default: SM64DS)
  --jitdefs "..." override JITDEFS (default enables both JITs + differential testing)
EOF
}
while [ $# -gt 0 ]; do
	case "$1" in
		--duration) DUR="$2"; shift ;;
		--rom)      ROM="$2"; shift ;;
		--jitdefs)  JITDEFS="$2"; shift ;;
		-h|--help)  usage; exit 0 ;;
		*)          echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

bench_preflight
[ -f "$ROM" ] || die "rom not found: $ROM"

RESDIR="$HERE/results/_soak_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RESDIR"
OUT="$RESDIR/jit.log"

build_dol "soak" "-DDESMUME_FORCE_ROM -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=200000 -DDESMUME_FORCE_CORE=2" "$JITDEFS"

bench_sd_snapshot "$RESDIR"
trap bench_sd_restore EXIT

dolphin_kill; sleep 2
mdel -i "$DOLPHIN_SD" ::/jit.log 2>/dev/null || true
stage_rom "$ROM"

echo ">> run ${DUR}s"
dolphin_launch "$DOLDIR/soak.dol" "$RESDIR/dolphin.log"
sleep "$DUR"
dolphin_kill; sleep 3

bench_sd_restore; trap - EXIT

if mcopy -i "$DOLPHIN_SD" ::/jit.log "$OUT" 2>/dev/null; then
	echo ">> captured -> $OUT"
	echo "---- mismatch / DIFF / CHAIN-DIFF / CANARY / OVERRUN lines (want NONE) ----"
	grep -nE "DIFF|MISMATCH|mismatch|CANARY|OVERRUN|CHAIN-DIFF|bad resume|!!!" "$OUT" || echo "  (none)"
	echo "---- last few diff report lines ----"
	grep -E "diff9?|selftest|predBcc|cycDrift" "$OUT" | tail -10
else
	echo "!! no jit.log produced"
	exit 1
fi
