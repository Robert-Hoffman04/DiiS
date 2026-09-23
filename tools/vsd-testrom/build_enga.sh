#!/usr/bin/env bash
#
# Engine A (main screen) fixtures for source/gx/gx_ds_enginea_render.cpp
# (gx-next-steps-log.md task 13). Called from build.sh; also runnable on its own.
#
#  (1) the Engine B fixtures re-targeted at the MAIN engine by mkenga.py (generated projects
#      under .gen/, gitignored): fxa_c* (windows/effects/MASTER_BRIGHT), cia_c* (CI4/CI8+TLUT,
#      extended BG/OBJ palettes via banks E/F), afa_c* (affine BG/OBJ, ext palettes). 3D is
#      disabled in all of them.
#  (2) a3drom: 3D layer / display-mode / capture fixtures a3_c0 .. a3_c24 (see its header).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/out"
IMAGE="skylyrac/blocksds:slim-latest"
mkdir -p "$OUT"

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

for d in fxrom_a cirom_a affinerom_a; do
	[ -d "$HERE/.gen/$d" ] && docker run --rm -v "$HERE/.gen/$d":/proj -w /proj "$IMAGE" rm -rf build 2>/dev/null || true
done
rm -rf "$HERE/.gen"
for r in fxrom cirom affinerom; do
	mkdir -p "$HERE/.gen/${r}_a/source"
	cp "$HERE/$r/Makefile" "$HERE/.gen/${r}_a/Makefile"
	python3 "$HERE/mkenga.py" "$HERE/$r/source/main.c" "$HERE/.gen/${r}_a/source/main.c"
done
docker run --rm -v "$HERE/.gen/fxrom_a":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	for c in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23; do build fxa_c$c "-DFX_CASE=$c"; done
'
docker run --rm -v "$HERE/.gen/cirom_a":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	for c in 0 1 2 3 4 5 6 7 8; do build cia_c$c "-DCI_CASE=$c"; done
'
docker run --rm -v "$HERE/.gen/affinerom_a":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	for c in 0 1 2 3 4 5 6 7 8 9 10; do build afa_c$c "-DAT_CASE=$c"; done
'
cp "$HERE"/.gen/fxrom_a/fxa_c*.nds "$HERE"/.gen/cirom_a/cia_c*.nds "$HERE"/.gen/affinerom_a/afa_c*.nds "$OUT"/

docker run --rm -v "$HERE/a3drom":/proj -w /proj "$IMAGE" rm -rf build 2>/dev/null || true
docker run --rm -v "$HERE/a3drom":/proj -w /proj "$IMAGE" bash -lc "$BDS"'
	for c in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28; do build a3_c$c "-DA3_CASE=$c"; done
'
for c in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28; do cp "$HERE"/a3drom/a3_c$c.nds "$OUT"/; done
echo "Engine A ROMs in $OUT : fxa_c* cia_c* afa_c* a3_c*"
