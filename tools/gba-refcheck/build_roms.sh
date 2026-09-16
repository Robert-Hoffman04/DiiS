#!/bin/bash
# Build the two hand-written synthetic GBA test ROMs (gradient / checkerboard)
# referenced in docs/PLAN.md §4.2/§4.3 item 8. Local-only test aids, same
# handling as before: sources are committed (tools/gba-refcheck/*.s), the
# assembled .gba outputs are not (see .gitignore).
set -euo pipefail
cd "$(dirname "$0")"

: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
AS="$DEVKITARM/bin/arm-none-eabi-as"
LD="$DEVKITARM/bin/arm-none-eabi-ld"
OBJCOPY="$DEVKITARM/bin/arm-none-eabi-objcopy"

for f in "$AS" "$LD" "$OBJCOPY"; do
    [ -x "$f" ] || { echo "missing tool: $f" >&2; exit 1; }
done

build_one() {
    local name="$1" title="$2" code="$3"
    "$AS" -mcpu=arm7tdmi -o "${name}.o" "${name}.s"
    "$LD" -Ttext=0x080000C0 -e_start -o "${name}.elf" "${name}.o"
    "$OBJCOPY" -O binary "${name}.elf" "${name}.raw"
    python3 make_header.py "${name}.raw" "${name}.gba" "$title" "$code"
}

build_one gradient      "GRADIENT"   "AGRE"
build_one checkerboard  "CHECKER"    "ACHE"
build_one affinecheck   "AFFCHECK"   "AAFF"
build_one objcheck      "OBJCHECK"   "AOBJ"
build_one irqsoak       "IRQSOAK"    "AIRQ"
build_one dsound        "DSOUND"     "ADSN"
build_one waitcnt       "WAITCNT"    "AWAI"
build_one dmavcap       "DMAVCAP"    "ADVC"
build_one windowcheck   "WINCHECK"   "AWIN"

echo "built: $(pwd)/gradient.gba $(pwd)/checkerboard.gba $(pwd)/affinecheck.gba $(pwd)/irqsoak.gba $(pwd)/dsound.gba $(pwd)/waitcnt.gba $(pwd)/dmavcap.gba $(pwd)/windowcheck.gba"
