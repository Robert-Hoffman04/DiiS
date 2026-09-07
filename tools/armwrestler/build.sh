#!/usr/bin/env bash
# Build the §17 auto-run armwrestler.nds. See PROVENANCE.md.
set -eu
cd "$(dirname "$0")"
export PATH="/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH"

mkdir -p out
AS=arm-none-eabi-as
LD=arm-none-eabi-ld
OBJCOPY=arm-none-eabi-objcopy

"$AS" -march=armv5te -mno-fpu -EL -o out/awr9.o armwrestler-ds.asm
"$AS" -mcpu=arm9tdmi -mno-fpu -mthumb -EL -o out/twr9.o thumbwrestler-ds.asm
"$AS" -mcpu=arm9tdmi -mno-fpu -EL -o out/ds_arm9_crt0.o ds_arm9_crt0.S
"$LD" -T ds_arm9.ld -EL -e _start -o out/a9.out out/ds_arm9_crt0.o out/awr9.o out/twr9.o
"$OBJCOPY" -O binary out/a9.out out/armwrestler.ds.arm9.bin

"$AS" -mcpu=arm7tdmi -mno-fpu -EL -o out/awr7.o armwrestler-arm7.asm
"$LD" -Ttext 0x3800000 -EL -e arm7_main -o out/a7.out out/awr7.o
"$OBJCOPY" -O binary out/a7.out out/armwrestler.ds.arm7.bin

ndstool -c out/armwrestler.nds -h 0x4000 -r9 0x2004000 -e9 0x2004800 -r7 0x3800000 -e7 0x3800000 \
  -9 out/armwrestler.ds.arm9.bin -7 out/armwrestler.ds.arm7.bin

echo "-> out/armwrestler.nds"
