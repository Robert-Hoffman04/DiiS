#!/usr/bin/env bash
# Build the §18 auto-run arm7wrestler.nds. See PROVENANCE.md.
set -eu
cd "$(dirname "$0")"
export PATH="/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH"

mkdir -p out
AS=arm-none-eabi-as
LD=arm-none-eabi-ld
OBJCOPY=arm-none-eabi-objcopy

# ARM7: the real test content. -march=armv5te is needed purely so the
# assembler can *encode* the ARMv5-only opcodes (CLZ, LDRD, QADD/QSUB,
# SMLAxy) this ROM deliberately executes to see what real ARM7TDMI hardware
# does with them (undefined-instruction exception, or a no-op) -- see
# UPSTREAM-README.md. The target CPU is still armv4t (ARM7TDMI); we are not
# claiming the hardware supports these encodings, only asking the assembler
# to produce their bit patterns.
"$AS" -march=armv5te -mfpu=softvfp -EL -o out/awr7.o armwrestler-ds.asm
"$AS" -march=armv5te -mfpu=softvfp -mthumb -EL -o out/twr7.o thumbwrestler-ds.asm
"$AS" -mcpu=arm7tdmi -mfpu=softvfp -EL -o out/ds_arm7_crt0.o ds_arm7_crt0.S
"$LD" -T ds_arm7.ld -EL -e _start -o out/a7.out out/ds_arm7_crt0.o out/awr7.o out/twr7.o
"$OBJCOPY" -O binary out/a7.out out/arm7wrestler.ds.arm7.bin

# ARM9: upstream's own trivial stub (armwrestler-arm9.s) -- no crt0 needed,
# it's straight-line code with no subroutine calls.
"$AS" -mcpu=arm9tdmi -mfpu=softvfp -EL -o out/awr9.o armwrestler-arm9.asm
"$LD" -Ttext 0x02004000 -EL -e main -o out/a9.out out/awr9.o
"$OBJCOPY" -O binary out/a9.out out/arm7wrestler.ds.arm9.bin

ndstool -c out/arm7wrestler.nds -h 0x4000 -r9 0x2004000 -e9 0x2004000 -r7 0x2380000 -e7 0x2380000 \
  -9 out/arm7wrestler.ds.arm9.bin -7 out/arm7wrestler.ds.arm7.bin

echo "-> out/arm7wrestler.nds"
