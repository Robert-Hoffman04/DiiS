# common.sh - shared helpers for check.sh (sourced).
C=${C:-/home/user/tools/correct}
DOLPHIN=${DOLPHIN:-/home/user/tools/dolphin/build/Binaries/dolphin-emu}
DISP=${CHECK_DISPLAY:-:70}
UDIR=$C/userdir
SDSYNC=$UDIR/Load/WiiSDSync
IMG=$UDIR/Load/WiiSD.raw
export MTOOLS_SKIP_CHECK=1
log() { echo "[check $(date +%H:%M:%S)] $*" >&2; }

ensure_xvfb() {
  pgrep -f "Xvfb $DISP " >/dev/null || { Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 2; }
}
ensure_docker() {
  docker info >/dev/null 2>&1 || { pgrep dockerd >/dev/null || (nohup dockerd >/tmp/dockerd.log 2>&1 &); for i in $(seq 30); do docker info >/dev/null 2>&1 && break; sleep 1; done; }
}
# pad_dol in out : Dolphin wants dol size % 32 == 0
pad_dol() { python3 - "$1" "$2" <<'P'
import sys; d=open(sys.argv[1],'rb').read(); d+=b'\0'*(-len(d)%32); open(sys.argv[2],'wb').write(d)
P
}
# img_get <name-on-card> <local-out> : read a file out of the LIVE card image (Dolphin only
# syncs the folder back on clean exit and we kill it). Card offset differs by image layout.
img_get() {
  local off
  for off in 0 524288 1048576 65536 32256; do
    mcopy -o -i "$IMG@@$off" "::/$1" "$2" >/dev/null 2>&1 && return 0
    [ "$off" = 0 ] && { mcopy -o -i "$IMG" "::/$1" "$2" >/dev/null 2>&1 && return 0; }
  done
  return 1
}
kill_dolphin() {
  [ -n "${DPID:-}" ] && { kill -9 -- -$DPID 2>/dev/null; kill -9 $DPID 2>/dev/null; }
  # only OUR dolphin (our userdir); never touch another agent's
  pkill -9 -f "dolphin-emu -u $UDIR " 2>/dev/null; DPID=; sleep 1
}
# launch_dolphin <dol> <rom> <timeout_s> <logfile>   (fresh SD image is generated from folder at boot)
launch_dolphin() {
  local dol=$1 rom=$2 tmo=$3 out=$4
  ensure_xvfb; kill_dolphin
  find "$SDSYNC" -type f ! -path "*/DS/BIOS/*" -delete 2>/dev/null
  mkdir -p "$SDSYNC/DS/ROMS" "$SDSYNC/DS/SAVES" "$SDSYNC/DS/BIOS"
  cp "$rom" "$SDSYNC/DS/ROMS/test.nds"
  rm -f "$IMG"
  DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.5 MESA_GLSL_VERSION_OVERRIDE=450 \
    timeout -s KILL "$tmo" "$DOLPHIN" -u "$UDIR" -b -e "$dol" \
      -C Dolphin.Core.WiiSDCard=True -C Dolphin.Core.WiiSDCardEnableFolderSync=True \
      -C Dolphin.DSP.Volume=0 -C Dolphin.Core.GFXBackend=OGL >"$out" 2>&1 &
  DPID=$!; disown $DPID 2>/dev/null
}
shot() { DISPLAY=$DISP import -window root "$1" 2>/dev/null; }

# build_dol <tree-name> <dol-out-name> <testdefs> <jitdefs> <makevars> [jobs]
# Persistent tree at $C/cache/<tree-name> (Makefile names the .dol after the dir). A full
# rebuild (make clean) only happens when JITDEFS/makevars differ from the tree's last build;
# a TESTDEFS change only recompiles main.o + relinks (~40s, LTO link dominates).
build_dol() {
  local tree=$1 name=$2 testdefs=$3 jitdefs=$4 mv=$5 jobs=${6:-4} T=$C/cache/$1
  local stamp="$jitdefs|$mv" logf=$C/cache/logs/build-$tree-$name.log
  mkdir -p "$C/cache/logs" "$C/cache/dols"
  local pre=""
  [ "$(cat "$T/.flagstamp" 2>/dev/null)" = "$stamp" ] || pre="make clean >/dev/null 2>&1;"
  rm -f "$T/build/main.o" "$T/build/rtc.o" "$T/$tree.elf" "$T/$tree.dol"
  docker run --rm -v "$T":/work/$tree -w /work/$tree devkitpro/devkitppc bash -c \
    "export DEVKITPPC=/opt/devkitpro/devkitPPC; $pre make -j$jobs TESTDEFS='$testdefs' JITDEFS='$jitdefs' $mv" >"$logf" 2>&1
  [ -f "$T/$tree.dol" ] || { log "BUILD FAILED $tree/$name (see $logf)"; tail -15 "$logf" >&2; return 1; }
  echo "$stamp" > "$T/.flagstamp"
  pad_dol "$T/$tree.dol" "$C/cache/dols/$name.dol"
}
sync_tree() { python3 "$C/cache/sync.py" "$1" "$C/cache/$2" >&2; }

# wait_card_file <card-name> <local-out> <grep-regex> <timeout_s> : poll the live card image until the
# file exists, matches the regex and is byte-identical on two consecutive reads (rewrite-every-30-frames
# sinks make this safe). Returns 0 on success, 1 on timeout / emulator died without a result.
wait_card_file() {
  local name=$1 out=$2 re=$3 tmo=$4 t0=$(date +%s) prev="" cur
  while [ $(( $(date +%s) - t0 )) -lt "$tmo" ]; do
    sleep 4
    if img_get "$name" "$out.tmp" && grep -q -E "$re" "$out.tmp" 2>/dev/null; then
      cur=$(md5sum < "$out.tmp")
      [ "$cur" = "$prev" ] && { mv "$out.tmp" "$out"; return 0; }
      prev=$cur
    fi
    pgrep -f "dolphin-emu -u $UDIR " >/dev/null || { [ -f "$out.tmp" ] && grep -q -E "$re" "$out.tmp" && { mv "$out.tmp" "$out"; return 0; }; return 1; }
  done
  return 1
}
