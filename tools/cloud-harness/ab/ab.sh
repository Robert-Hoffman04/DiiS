#!/usr/bin/env bash
# usage: ab.sh <label> <run-id> ; tree in /home/user/tools/ab/src/<label>; no perfzones. Private userdir, DISP=:66
set -e
L=$1; R=$2; A=/home/user/tools/ab; T=/home/user/tools; DISP=:66; TMO=${TMO:-1500}
DEFS="-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=1 -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=1800 -DDESMUME_FORCE_RENDERMODE=2"
TREE=$A/src/$L
pgrep dockerd >/dev/null || { (nohup dockerd >/tmp/dockerd.log 2>&1 &); sleep 8; }
if [ ! -f $TREE/$L.dol ] || [ "$REBUILD" = 1 ]; then
docker run --rm -v $TREE:/work/$L -w /work/$L devkitpro/devkitppc bash -c \
  "export DEVKITPPC=/opt/devkitpro/devkitPPC; make -j4 TESTDEFS=\"$DEFS\" 2>&1 | grep -E 'error|Error|\.dol' ; test -f $L.dol"
fi
python3 - $TREE/$L.dol $A/$L.dol <<'P'
import sys; d=open(sys.argv[1],'rb').read(); d+=b'\0'*(-len(d)%32); open(sys.argv[2],'wb').write(d)
P
U=$A/userdir; I=$U/Load/WiiSD.raw; export MTOOLS_SKIP_CHECK=1
OFF=; for o in 0 524288 1048576 65536; do mdir -i $I@@$o ::/ >/dev/null 2>&1 && { OFF=$o; break; }; done
[ -n "$OFF" ] || { echo "no FAT"; exit 1; }
mdel -i $I@@$OFF ::/bench.log 2>/dev/null || true
mkdir -p $U/Load/WiiSDSync/DS/ROMS; cmp -s "$T/roms/AtomicAdventure3D 1_1.nds" $U/Load/WiiSDSync/DS/ROMS/test.nds || cp "$T/roms/AtomicAdventure3D 1_1.nds" $U/Load/WiiSDSync/DS/ROMS/test.nds
rm -f $U/Load/WiiSDSync/bench.log
Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XP=$!; export DISPLAY=$DISP; sleep 2
LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450 \
 timeout -s KILL $TMO $T/dolphin/build/Binaries/dolphin-emu -u $U -b -e $A/$L.dol \
 -C Dolphin.Core.WiiSDCard=True -C Dolphin.Core.WiiSDCardEnableFolderSync=True \
 -C Dolphin.DSP.Volume=0 -C Dolphin.Core.GFXBackend=OGL >/tmp/dol-ab-$L.out 2>&1 &
DP=$!
while kill -0 $DP 2>/dev/null; do
  sleep 20
  mcopy -o -i $I@@$OFF ::/bench.log $A/results/$L-$R.raw 2>/dev/null && grep -q "^1740," $A/results/$L-$R.raw && { sleep 3; break; }
done
pkill -KILL -f "dolphin-emu -u $U" 2>/dev/null || true; kill $XP 2>/dev/null || true
mcopy -o -i $I@@$OFF ::/bench.log $A/results/$L-$R.raw
python3 - $A/results/$L-$R.raw $A/results/$L-$R.csv <<'P'
import sys; L=open(sys.argv[1],'rb').read().decode('latin1').split('\n')
keep=[l for l in L if l.startswith('#') or l.startswith('frame') or (l and l.count(',')==4 and l.split(',')[0].isdigit())]
open(sys.argv[2],'w').write('\n'.join(keep)+'\n')
P
head -3 $A/results/$L-$R.csv; wc -l $A/results/$L-$R.csv
