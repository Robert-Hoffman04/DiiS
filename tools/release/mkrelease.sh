#!/bin/sh
# DiiS release zip: a clean default build (JIT + GxFast) of HEAD, packaged
# for the Homebrew Channel as the GitHub releases are:
#
#   DiiS-<short hash>.zip
#     apps/DiiS/boot.dol    the build
#     apps/DiiS/icon.png    res/hbc/icon.png
#     apps/DiiS/meta.xml    res/hbc/meta.xml, release_date set to today
#
# Written to build/release/. Refuses a working tree with uncommitted
# changes (the zip is named after HEAD) unless --dirty is given. Prints
# the matching `gh release create` command; it doesn't publish.
#
# Usage, from anywhere in the repo:  tools/release/mkrelease.sh [--dirty]
set -e

cd "$(git rev-parse --show-toplevel)"

if [ "$1" != "--dirty" ] && [ -n "$(git status --porcelain)" ]; then
	echo "mkrelease: uncommitted changes; commit them or pass --dirty" >&2
	exit 1
fi

HASH=$(git rev-parse --short HEAD)
NAME="DiiS-$HASH"

make clean >/dev/null
make -j"$(nproc)"

OUT=build/release
STAGE="$OUT/stage"
rm -rf "$STAGE" "$OUT/$NAME.zip"
mkdir -p "$STAGE/apps/DiiS"
cp diis.dol "$STAGE/apps/DiiS/boot.dol"
cp res/hbc/icon.png "$STAGE/apps/DiiS/icon.png"
sed "s#<release_date>[^<]*</release_date>#<release_date>$(date +%m/%d/%Y)</release_date>#" \
	res/hbc/meta.xml > "$STAGE/apps/DiiS/meta.xml"
(cd "$STAGE" && zip -q -r "../$NAME.zip" apps)
rm -rf "$STAGE"

echo
echo "$OUT/$NAME.zip"
unzip -l "$OUT/$NAME.zip"
echo
echo "To publish (Robert-Hoffman04/DiiS):"
echo "  gh release create v0.1-$HASH $OUT/$NAME.zip -R Robert-Hoffman04/DiiS \\"
echo "    --target $(git rev-parse HEAD) --title \"DiiS ($HASH)\" \\"
echo "    --notes \"JIT + GxFast build. Extract to the root of your SD card (apps/DiiS/).\""
