#!/usr/bin/env bash
# dettest.sh <dol> [warmup=30] [gap=5] [nshots=4]
# Boots <dol> (test.nds already staged - use a deterministic ROM), grabs nshots
# screenshots <gap>s apart after <warmup>s, and reports the pixel delta between
# consecutive frames.  AE == 0 everywhere == the ROM is rendering a stable frame.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:?usage: dettest.sh <dol> [warmup] [gap] [nshots]}"
WARM="${2:-30}"; GAP="${3:-5}"; N="${4:-4}"

dolphin_kill; sleep 3
rm -f "$SHOTS"/det_${NAME}_*.png
dolphin_launch "$NAME"
echo "launched $NAME, warmup ${WARM}s"
sleep "$WARM"
WID="$(dolphin_wid)"; echo "window=$WID"
for i in $(seq 1 "$N"); do
	shot "$WID" "$SHOTS/det_${NAME}_$i.png" && echo "  shot $i" || echo "  shot $i FAILED"
	[ "$i" -lt "$N" ] && sleep "$GAP"
done
dolphin_kill

echo "--- consecutive-frame differences ---"
for i in $(seq 2 "$N"); do
	p=$((i-1))
	[ -f "$SHOTS/det_${NAME}_$p.png" ] && [ -f "$SHOTS/det_${NAME}_$i.png" ] || continue
	ae="$(ae_diff "$SHOTS/det_${NAME}_$p.png" "$SHOTS/det_${NAME}_$i.png")"
	echo "  shot $p -> $i : AE=$ae px"
done
