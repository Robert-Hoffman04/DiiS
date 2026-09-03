#!/usr/bin/env bash
# abtoggle.sh [dol=ds_merge] [warmup=30]
# Boots ONCE, then screenshots merge-ON / merge-OFF / merge-ON / merge-OFF by
# flipping the runtime GXMerge toggle (GC D-pad Down) in place.  Because it is a
# single session the window geometry is identical for every shot, so the diff
# isolates the merge compositor from the underlying GXRender output.
#
# Needs a deterministic ROM staged as test.nds (e.g. out/merge_fblend.nds).
# Input goes over Dolphin's USB Gecko serial - no GCPad/pipe setup needed.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NAME="${1:-ds_merge}"; WARM="${2:-30}"

dolphin_kill; sleep 3
rm -f "$SHOTS"/abt_*.png "$SHOTS"/abtdiff_*.png
dolphin_launch "$NAME"
echo "launched $NAME, warmup ${WARM}s"
sleep "$WARM"
WID="$(dolphin_wid)"; echo "window=$WID"
gecko_open || { echo "no gecko - cannot toggle"; dolphin_kill; exit 1; }

shot "$WID" "$SHOTS/abt_1_mergeON.png";  echo "  shot 1 mergeON"
merge_toggle; shot "$WID" "$SHOTS/abt_2_mergeOFF.png"; echo "  shot 2 mergeOFF"
merge_toggle; shot "$WID" "$SHOTS/abt_3_mergeON.png";  echo "  shot 3 mergeON"
merge_toggle; shot "$WID" "$SHOTS/abt_4_mergeOFF.png"; echo "  shot 4 mergeOFF"
dolphin_kill

echo "--- diffs (single session; geometry constant) ---"
d() { echo "  $1 vs $2 : $(ae_diff "$SHOTS/abt_$1.png" "$SHOTS/abt_$2.png" "$SHOTS/abtdiff_$1_$2.png") px"; }
d 1_mergeON  3_mergeON    # ON  vs ON   -> expect 0 (merge path stable)
d 2_mergeOFF 4_mergeOFF   # OFF vs OFF  -> expect 0 (legacy path stable)
d 1_mergeON  2_mergeOFF   # merge compositor vs legacy readback+composite
