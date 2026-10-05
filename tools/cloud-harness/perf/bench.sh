#!/usr/bin/env bash
# usage: bench.sh <source-tree-dir> <label>     (env: FRAMES=900 TMO=1500 DISP=:60)
# Builds the tree (persistent incremental copy in trees/<label>), runs it headless in Dolphin
# (GxFast forced at compile time), writes results/<label>.csv and prints a summary.
set -e
SRC=$(realpath "$1"); L=$2; [ -n "$SRC" ] && [ -n "$L" ] || { echo "usage: $0 <src> <label>"; exit 1; }
P=/home/user/tools/perf; T=/home/user/tools
FRAMES=${FRAMES:-1800}; TMO=${TMO:-1500}; DISP=${DISP:-:60}
DEFS="-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=1 -DDESMUME_PERFZONES -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=$FRAMES -DDESMUME_FORCE_RENDERMODE=2 $EXTRA_DEFS"
TREE=$P/trees/$L; mkdir -p $TREE $P/results
pgrep dockerd >/dev/null || { (nohup dockerd >/tmp/dockerd.log 2>&1 &); sleep 8; }
python3 $P/sync.py "$SRC" $TREE
# rebuild if defs changed (make does not track TESTDEFS)
if [ "$(cat $TREE/.defs 2>/dev/null)" != "$DEFS" ]; then rm -rf $TREE/build; echo "$DEFS" > $TREE/.defs; fi
echo "[build] $L ($DEFS)"
docker run --rm -v $TREE:/work/$L -w /work/$L devkitpro/devkitppc bash -c \
  "export DEVKITPPC=/opt/devkitpro/devkitPPC; make -j4 TESTDEFS=\"$DEFS\" 2>&1 | grep -E 'error|Error|\.dol' ; test -f $L.dol"
python3 - $TREE/$L.dol $P/$L.dol <<'P'
import sys; d=open(sys.argv[1],'rb').read(); d+=b'\0'*(-len(d)%32); open(sys.argv[2],'wb').write(d)
P
U=$P/userdir-${LABEL_UD:-$(echo ${DISP:-:60} | tr -d :)}; [ -d $U ] || cp -r $P/userdir $U; I=$U/Load/WiiSD.raw; export MTOOLS_SKIP_CHECK=1
OFF=; for o in 0 524288 1048576 65536; do mdir -i $I@@$o ::/ >/dev/null 2>&1 && { OFF=$o; break; }; done
[ -n "$OFF" ] || { echo "cannot find FAT partition in $I"; exit 1; }
mdel -i $I@@$OFF ::/perfzones.log 2>/dev/null || true     # drop stale log from a previous run
mkdir -p $U/Load/WiiSDSync/DS/ROMS; cmp -s "$T/roms/AtomicAdventure3D 1_1.nds" $U/Load/WiiSDSync/DS/ROMS/test.nds || cp "$T/roms/AtomicAdventure3D 1_1.nds" $U/Load/WiiSDSync/DS/ROMS/test.nds
rm -f $U/Load/WiiSDSync/perfzones.log
echo "[run] dolphin on $DISP, timeout ${TMO}s"
Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XP=$!; export DISPLAY=$DISP; sleep 2
S=$(date +%s)
LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450 \
 timeout -s KILL $TMO $T/dolphin/build/Binaries/dolphin-emu -u $U -b -e $P/$L.dol \
 -C Dolphin.Core.WiiSDCard=True -C Dolphin.Core.WiiSDCardEnableFolderSync=True \
 -C Dolphin.DSP.Volume=0 -C Dolphin.Core.GFXBackend=OGL >/tmp/dol-$L.out 2>&1 &
DP=$!
# poll: stop as soon as the last expected row shows up in the live image
while kill -0 $DP 2>/dev/null; do
  sleep 20
  mcopy -o -i $I@@$OFF ::/perfzones.log $P/results/$L.raw 2>/dev/null && \
    grep -q "^$((FRAMES-60))," $P/results/$L.raw && { sleep 6; break; }
done
pkill -KILL -f "dolphin-emu -u $U" 2>/dev/null || true; kill $XP 2>/dev/null || true
echo "[run] host time $(( $(date +%s) - S ))s"
mcopy -o -i $I@@$OFF ::/perfzones.log $P/results/$L.raw
python3 - $P/results/$L.raw $P/results/$L.csv <<'P'
import sys; L=open(sys.argv[1],'rb').read().decode('latin1').split('\n')
n=next(l for l in L if l.startswith('frame,')).count(',')   # header; a -DDESMUME_PERFZONES_PMC log has a '# pmc sets' line first
keep=[l for l in L if l.startswith('#') or l.startswith('frame') or (l and l.count(',')==n)]
open(sys.argv[2],'w').write('\n'.join(keep)+'\n')
P
rm -f $P/results/$L.raw
python3 $P/summary.py $P/results/$L.csv
