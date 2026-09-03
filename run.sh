#!/usr/bin/env bash
# run.sh - launch the .dol/.elf that `make` just built, in Dolphin.
#
# `make` drops its output as <repo-dir-name>.dol / .elf directly in the repo
# root (see Makefile: TARGET := $(notdir $(CURDIR))), so this just needs to
# find that file next to itself and hand it to Dolphin. Any extra arguments
# are passed straight through to `flatpak run` (e.g. `./run.sh -C
# Dolphin.Core.WiiSDCard=False` to disable the SD card, or a config override).
#
# Usage: ./run.sh [extra flatpak/dolphin args...]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# Prefer the .dol that matches this directory's own name (what the Makefile
# actually produces); fall back to whatever .dol/.elf sits in the root in
# case the checkout was renamed.
NAME="$(basename "$ROOT")"
BUILT=""
for candidate in "$NAME.dol" "$NAME.elf" *.dol *.elf; do
	if [ -f "$candidate" ]; then
		BUILT="$candidate"
		break
	fi
done

if [ -z "$BUILT" ]; then
	echo "run.sh: no .dol/.elf found in $ROOT - run 'make' first." >&2
	exit 1
fi

echo "run.sh: launching $BUILT"
exec flatpak run org.DolphinEmu.dolphin-emu -e "$ROOT/$BUILT" \
	-C Dolphin.Core.WiiSDCard=True \
	"$@"
