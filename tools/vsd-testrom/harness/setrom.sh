#!/usr/bin/env bash
# setrom.sh <path-to.nds>
# Installs a ROM as sd:/DS/ROMS/test.nds inside the Dolphin FAT image that a
# -DDESMUME_FORCE_ROM build of desmumewii boots.
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

NDS="${1:?usage: setrom.sh <path-to.nds>}"
[ -f "$NDS" ] || { echo "no such file: $NDS" >&2; exit 1; }

if mcopy -o -i "$DOLPHIN_SD" ::/DS/ROMS/test.nds "$DOL_DIR/prev_test.nds" 2>/dev/null; then
	echo "backed up current test.nds -> $DOL_DIR/prev_test.nds"
fi
mcopy -o -i "$DOLPHIN_SD" "$NDS" ::/DS/ROMS/test.nds
echo "installed $(basename "$NDS") ($(stat -c%s "$NDS") bytes) as sd:/DS/ROMS/test.nds"
mdir -i "$DOLPHIN_SD" ::/DS/ROMS
