#!/usr/bin/env bash
# Build the §19 auto-run rockwrestler.nds. See PROVENANCE.md.
set -eu
cd "$(dirname "$0")"
export PATH="/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH"

mkdir -p out

# Unlike tools/armwrestler and tools/arm7wrestler, upstream RockWrestler
# needs no crt0/linker workaround at all: it's -nostartfiles, its own
# entry.s is a two-instruction stub, and linker7.ld/linker9.ld are trivial
# single-section scripts -- no libnds, no devkitPro-bundled crt0, so no
# version-mismatch surface to route around. Straight arm-none-eabi-g++,
# matching upstream's own build.sh, only reworked to land in out/.

# build timestamp, embedded via a generated .s (matches upstream's time.py)
date +'@ this file is generated automatically by build.sh
.text
.arm
.align 2
.global txt_buildtime
txt_buildtime: .asciz "BUILT %d/%m/%Y %H:%M:%S"' > src9/framework/time.s

echo doing arm9...
arm-none-eabi-g++ -march=armv5te -O2 -marm -o out/arm9.o \
  src9/entry.s src9/*.cpp src9/framework/*.cpp src9/framework/*.s src9/tests/*.cpp src9/tests/*.s common/*.cpp common/*.s \
  -nostartfiles -T linker9.ld -fno-exceptions -fno-rtti -Wno-narrowing -fno-delete-null-pointer-checks
arm-none-eabi-objcopy -O binary out/arm9.o out/arm9.bin --image-base=0x2000100

echo doing arm7...
arm-none-eabi-g++ -mcpu=arm7tdmi -O2 -marm -o out/arm7.o \
  src7/entry.s src7/*.cpp src7/framework/*.cpp src7/framework/*.s src7/tests/*.cpp common/*.cpp common/*.s \
  -nostartfiles -T linker7.ld -fno-exceptions -fno-rtti -Wno-narrowing -fno-delete-null-pointer-checks
arm-none-eabi-objcopy -O binary out/arm7.o out/arm7.bin --image-base=0x3800100

ndstool -c out/rockwrestler.nds -r9 0x2000100 -e9 0x2000100 -r7 0x3800100 -e7 0x3800100 \
  -9 out/arm9.bin -7 out/arm7.bin

echo "-> out/rockwrestler.nds"
