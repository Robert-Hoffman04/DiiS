#!/usr/bin/env bash
#
# Deferred-CPU-compositing transition fixtures (queue item 13j): lzrom/ -> out/lz_c0 .. lz_c24.nds
# (see lzrom/source/main.c for what each case does). Requires docker + the BlocksDS image, like build_enga.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/out"
IMAGE="skylyrac/blocksds:slim-latest"
mkdir -p "$OUT"
docker run --rm -v "$HERE/lzrom":/proj -w /proj "$IMAGE" rm -rf build 2>/dev/null || true
docker run --rm -v "$HERE/lzrom":/proj -w /proj "$IMAGE" bash -lc '
	export BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core
	export BLOCKSDSEXT=/opt/wonderful/thirdparty/blocksds/external
	export WONDERFUL_TOOLCHAIN=/opt/wonderful
	set -e
	for c in $(seq 0 24); do
		make clean >/dev/null 2>&1 || true
		make NAME=lz_c$c DEFINES="-DLZ_CASE=$c" >/dev/null
		echo "  built lz_c$c.nds"
	done
'
for c in $(seq 0 24); do cp "$HERE"/lzrom/lz_c$c.nds "$OUT"/; done
echo "lz ROMs in $OUT"
