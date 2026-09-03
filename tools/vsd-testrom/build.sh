#!/usr/bin/env bash
#
# Build the test ROMs used by the GX-merge / rendering harness.  Output -> ./out/ :
#
#   VSD (Volumetric Shadow Demo, upstream) - exercises the LEGACY readback path
#   (the demo uses framebuffer display + display capture, so the GX-merge sandbox
#   never arms; it is a fallback-safety regression test):
#     vsd_normal.nds  - upstream demo, rebuilt as-is (sanity baseline)
#     vsd_det.nds     - deterministic: no doll spin, no live counters, capture
#                       motion-blur off -> pixel-stable frames.  Still drivable.
#     vsd_frozen.nds  - vsd_det + stops reading input after warmup.
#
#   mergerom (purpose-built, ./mergerom/) - exercises the GX-merge 3-DRAW SANDWICH:
#   main engine in tiled BG mode, 3D on BG0, 2D BGs + sprites bucketed around it,
#   no capture / brightness / windows.
#     merge_normal.nds  - spinning, drivable
#     merge_det.nds     - spin frozen after warmup -> pixel-stable A/B
#     merge_frozen.nds  - merge_det + stops reading input after warmup
#     merge_fblend.nds  - merge_det + MT_FRONTBLEND: BG2 (front) alpha-blends
#                         50/50 against 3D/checker/backdrop.  Exercises the
#                         Phase-2 front-bucket "blend against beneath" path
#                         (GXMerge frontAlphaOver).  Pixel-stable A/B.
#     merge_mbright.nds - merge_det + MT_MASTERBRIGHT: whole-screen bright-down
#                         fade (factor 8).  Exercises the Phase-3 per-band GX
#                         brightness pass (GXMerge draw 4).  Pixel-stable A/B.
#     merge_mbsplit.nds - merge_mbright + MT_MB_SPLIT: factor rewritten at
#                         scanline 96 (HBlank IRQ) -> 2 brightness bands.
#
# Both are BlocksDS projects built in the official container so no host toolchain
# is needed (devkitARM will NOT produce a working ROM here - desmumewii needs the
# BlocksDS default ARM7 / NitroFS layout).
#
# Requires: docker.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$HERE/.work"
OUT="$HERE/out"
IMAGE="skylyrac/blocksds:slim-latest"
REPO="https://codeberg.org/SkyLyrac/volumetric_shadow_demo.git"
PIN="9ded33c86f4edcc031b5d1ff12de03c59906aa8d"   # pinned upstream commit

mkdir -p "$OUT"
docker pull -q "$IMAGE"

BDS='
	export BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core
	export BLOCKSDSEXT=/opt/wonderful/thirdparty/blocksds/external
	export WONDERFUL_TOOLCHAIN=/opt/wonderful
	set -e
	build() {  # <name> <defines>
		make clean >/dev/null 2>&1 || true
		make NAME="$1" DEFINES="$2" >/dev/null
		echo "  built $1.nds  [${2:-<none>}]"
	}
'

# --- VSD ---------------------------------------------------------------------
if [ ! -d "$WORK/.git" ]; then
	rm -rf "$WORK"
	git clone "$REPO" "$WORK"
fi
git -C "$WORK" fetch --all -q || true
git -C "$WORK" checkout -q "$PIN"
git -C "$WORK" reset -q --hard "$PIN"
# Prior runs' build artifacts are created root-owned inside the container; clear
# them with the same privilege so the host-side git clean below can't trip.
docker run --rm -v "$WORK":/proj -w /proj "$IMAGE" rm -rf build 2>/dev/null || true
git -C "$WORK" clean -qfdx
git -C "$WORK" apply "$HERE/deterministic.patch"
echo "applied deterministic.patch on top of $PIN"

docker run --rm -v "$WORK":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	build vsd_normal ""
	build vsd_det    "-DVSD_DETERMINISTIC"
	build vsd_frozen "-DVSD_DETERMINISTIC -DVSD_FREEZE"
'
cp "$WORK"/vsd_normal.nds "$WORK"/vsd_det.nds "$WORK"/vsd_frozen.nds "$OUT"/

# --- mergerom --------------------------------------------------------------
docker run --rm -v "$HERE/mergerom":/proj -w /proj "$IMAGE" rm -rf build 2>/dev/null || true
docker run --rm -v "$HERE/mergerom":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	build merge_normal  ""
	build merge_det     "-DMT_DETERMINISTIC"
	build merge_frozen  "-DMT_DETERMINISTIC -DMT_FREEZE"
	build merge_fblend  "-DMT_DETERMINISTIC -DMT_FRONTBLEND"
	build merge_mbright "-DMT_DETERMINISTIC -DMT_MASTERBRIGHT"
	build merge_mbsplit "-DMT_DETERMINISTIC -DMT_MASTERBRIGHT -DMT_MB_SPLIT"
'
cp "$HERE"/mergerom/merge_normal.nds "$HERE"/mergerom/merge_det.nds \
   "$HERE"/mergerom/merge_frozen.nds "$HERE"/mergerom/merge_fblend.nds \
   "$HERE"/mergerom/merge_mbright.nds "$HERE"/mergerom/merge_mbsplit.nds "$OUT"/

echo
echo "ROMs in $OUT :"
ls -la "$OUT"/*.nds
