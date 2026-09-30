#!/usr/bin/env bash
# check.sh <source-tree> [options]  -- DiiS JIT correctness regression check (see README.txt)
#
#   (a) armwrestler / arm7wrestler / rockwrestler under the JIT  -> result log compared to baseline/
#   (b) JIT_DIFFERENTIAL_TESTING runs (3 wrestler ROMs + AtomicAdventure) -> mismatch count must be 0
#   (c) AtomicAdventure guest-state hash at frames 300/600/900 (RAM, regs, VRAM, screen, GE output)
#       compared to baseline/hash.txt (bit-exact, deterministic)
#
# options:
#   --update-baseline   record this tree's results as the baseline (also runs hash twice to prove
#                       determinism, and the NOJIT interpreter reference for information)
#   --interp            also run the NOJIT interpreter build of the same tree (wrestlers + hash) and show it
#   --only LIST         comma list of: wr,diff,hash   (default all)
#   --no-build          reuse cached dols (only valid if the tree is unchanged)
#   --label NAME        results dir label (default: basename of tree)
# exit status: 0 = everything PASS, 1 = at least one FAIL, 2 = harness error
set -u
C=/home/user/tools/correct
. "$C/lib/common.sh"

SRC=""; UPDATE=0; INTERP=0; ONLY="wr,diff,hash"; NOBUILD=0; LABEL=""
while [ $# -gt 0 ]; do case "$1" in
  --update-baseline) UPDATE=1; INTERP=1;; --interp) INTERP=1;; --only) ONLY=$2; shift;;
  --no-build) NOBUILD=1;; --label) LABEL=$2; shift;; -h|--help) sed -n 2,20p "$0"; exit 0;;
  *) SRC=$1;; esac; shift; done
[ -d "$SRC/source" ] || { echo "usage: $0 <source-tree> [--update-baseline] [--interp] [--only wr,diff,hash]"; exit 2; }
SRC=$(realpath "$SRC"); LABEL=${LABEL:-$(basename "$SRC")}
has() { case ",$ONLY," in *",$1,"*) return 0;; esac; return 1; }

# tunables (env)
HF=${HASH_FRAMES:-300,600,900}          # state-hash checkpoints
HT=${HASH_TOTAL:-$(( ${HF##*,} + 60 ))}  # frame the bench run quits at
DF=${DIFF_FRAMES:-300}                  # AtomicAdventure frames in the differential run
RM=${RENDERMODE:-2}                     # 0 Software, 1 GxAccurate, 2 GxFast (compile-time forced)
AAROM=${AAROM:-/home/user/tools/roms/AtomicAdventure3D 1_1.nds}

exec 9>"$C/.lock"; flock -n 9 || { echo "another check.sh is running"; exit 2; }
STAMP=$(date +%Y%m%d-%H%M%S); RES=$C/results/${STAMP}_$LABEL; mkdir -p "$RES" "$C/baseline"
trap 'kill_dolphin; ' EXIT
T0=$(date +%s); el() { echo $(( $(date +%s) - T0 )); }
ensure_docker; ensure_xvfb
FAILS=0; SUMMARY=$RES/summary.txt
verdict() { # PASS|FAIL|INFO  text
  echo "$1  $2" | tee -a "$SUMMARY"; [ "$1" = FAIL ] && FAILS=$((FAILS+1)); return 0; }

# ---------------------------------------------------------------- 0. trees + ROMs
log "sync trees from $SRC"
for t in tree-n tree-d tree-i; do
  { [ "$t" = tree-i ] && [ $INTERP = 0 ]; } && continue
  { [ "$t" = tree-d ] && ! has diff; } && continue
  mkdir -p "$C/cache/$t"; sync_tree "$SRC" $t
  # state-hash instrumentation is a tiny -DDESMUME_STATE_HASH-gated patch to main.cpp (lib/state_hash.patch);
  # a tree that already carries it (e.g. current DiiS worktree) is used as-is.
  if ! grep -q DESMUME_STATE_HASH "$C/cache/$t/source/main.cpp"; then
    patch -s -p1 -d "$C/cache/$t" < "$C/lib/state_hash.patch" || { echo "state_hash.patch does not apply to $SRC"; exit 2; }
  fi
done
PSUM=$(sha1sum < "$C/lib/state_hash.patch" | cut -c1-8)

# wrestler ROMs are built (devkitARM in docker; ndstool is a static host build in cache/nds) from the tree's tools/
for w in armwrestler arm7wrestler rockwrestler; do
  d=$C/cache/wr-$w; sh=$(cat $(find "$SRC/tools/$w" -type f ! -path '*/out/*' | sort) | sha1sum | cut -c1-12)
  if [ "$(cat $d/.src 2>/dev/null)" != "$sh" ] || [ ! -f $d/out/$w.nds ]; then
    log "build ROM $w"; rm -rf $d; cp -r "$SRC/tools/$w" $d; rm -rf $d/out
    docker run --rm -v "$C/cache":/cache devkitpro/devkitppc bash -c "export PATH=/cache/nds/bin:\$PATH; cd /cache/wr-$w && ./build.sh" >"$C/cache/logs/rom-$w.log" 2>&1 \
      && echo "$sh" > $d/.src || { echo "ROM build failed: $w (see cache/logs/rom-$w.log)"; exit 2; }
  fi
done

# ---------------------------------------------------------------- 1. builds
FD="-DDESMUME_FORCE_ROM -DDESMUME_FORCE_CORE=1 -DDESMUME_DETERMINISTIC_RTC"
HASHD="$FD -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=$HT -DDESMUME_STATE_HASH -DDESMUME_STATE_HASH_FRAMES=$HF -DDESMUME_FORCE_RENDERMODE=$RM"
DIFFD="$FD -DDESMUME_BENCH -DDESMUME_BENCH_FRAMES=$((DF+30)) -DDESMUME_STATE_HASH -DDESMUME_STATE_HASH_FRAMES=$DF -DDESMUME_FORCE_RENDERMODE=$RM"
bd() { # tree name testdefs jitdefs makevars jobs
  local st="$(cat "$C/cache/$1/.synchash")|$PSUM|$3|$4|$5"
  [ -f "$C/cache/dols/$2.dol" ] && [ "$(cat "$C/cache/dols/$2.stamp" 2>/dev/null)" = "$st" ] && { echo "cached $2" >&2; return 0; }
  [ $NOBUILD = 1 ] && [ -f "$C/cache/dols/$2.dol" ] && return 0
  log "build $2"; build_dol "$1" "$2" "$3" "$4" "$5" "$6" && echo "$st" > "$C/cache/dols/$2.stamp"
}
mkdir -p "$C/cache/dols" "$C/cache/logs"
JOBS=2
( { has wr || has hash; } || exit 0
  has wr && for w in armwrestler arm7wrestler rockwrestler; do bd tree-n $w "$FD -DDESMUME_$(echo $w | tr a-z A-Z)_PROBE" "" "" $JOBS || exit 1; done
  has hash && { bd tree-n aa-hash "$HASHD" "" "" $JOBS || exit 1; }; exit 0 ) & PN=$!
( has diff || exit 0
  for w in armwrestler arm7wrestler rockwrestler; do bd tree-d $w-diff "$FD -DDESMUME_$(echo $w | tr a-z A-Z)_PROBE" "-DJIT_DIFFERENTIAL_TESTING" "" $JOBS || exit 1; done
  bd tree-d aa-diff "$DIFFD" "-DJIT_DIFFERENTIAL_TESTING" "" $JOBS || exit 1; exit 0 ) & PD=$!
PI=""
if [ $INTERP = 1 ]; then
  ( for w in armwrestler arm7wrestler rockwrestler; do bd tree-i $w-interp "$FD -DDESMUME_$(echo $w | tr a-z A-Z)_PROBE" "" "NOJIT=1" $JOBS || exit 1; done
    bd tree-i aa-hash-interp "$HASHD" "" "NOJIT=1" $JOBS || exit 1; exit 0 ) & PI=$!
fi
wait $PN || { echo "BUILD FAILED (tree-n)"; exit 2; }
wait $PD || { echo "BUILD FAILED (tree-d)"; exit 2; }
[ -n "$PI" ] && { wait $PI || { echo "BUILD FAILED (tree-i)"; exit 2; }; }
log "builds done at +$(el)s"

# ---------------------------------------------------------------- run helpers
# run_wrestler <dolname> <wrestler> <outfile>  : boots the probe ROM, returns its sd:/<wrestler>.log
run_wrestler() {
  launch_dolphin "$C/cache/dols/$1.dol" "$C/cache/wr-$2/out/$2.nds" 240 "$RES/dolphin-$1.out"
  wait_card_file "$2.log" "$3" "\\[$2\\]" 120; local rc=$?; kill_dolphin; return $rc
}
# diff_summary <jit.log> : "<mismatches> <blocks9> <untr9> <blocks7> <untr7>"
diff_summary() { python3 - "$1" <<'P'
import re,sys
t=open(sys.argv[1],'rb').read().decode('latin1')
nd=len(re.findall(r'\[jit\] diff9? (?:CHAIN-)?DIFF @',t)); tot={};
for m in re.finditer(r'\[jit\] (diff9?) alive: (\d+) blocks, (\d+) insns, (\d+) mismatches \((\d+) logged\); untrusted=(\d+)',t):
    tot[m.group(1)]=(int(m.group(2)),int(m.group(4)),int(m.group(6)))
mm=max([nd]+[v[1] for v in tot.values()])
b9=tot.get('diff9',(0,0,0)); b7=tot.get('diff',(0,0,0))
print(mm,b9[0],b9[2],b7[0],b7[2])
P
}

# ---------------------------------------------------------------- 2a. wrestlers under the JIT
if has wr; then
  for w in armwrestler arm7wrestler rockwrestler; do
    out=$RES/$w.log; run_wrestler $w $w $out; rc=$?
    if [ $rc != 0 ]; then verdict FAIL "$w (JIT): no result log within timeout (crash/hang?)"; continue; fi
    [ $UPDATE = 1 ] && cp $out "$C/baseline/$w.txt"
    if [ ! -f "$C/baseline/$w.txt" ]; then verdict FAIL "$w (JIT): no baseline";
    elif cmp -s "$out" "$C/baseline/$w.txt"; then verdict PASS "$w (JIT): $(head -1 $out | sed 's/^\[[a-z0-9]*\] //') [== baseline]"
    else verdict FAIL "$w (JIT): result differs from baseline"; diff "$C/baseline/$w.txt" "$out" | head -12 | sed 's/^/      /'; fi
    if [ $INTERP = 1 ]; then
      run_wrestler $w-interp $w $RES/$w.interp.log && {
        [ $UPDATE = 1 ] && cp $RES/$w.interp.log "$C/baseline/$w.interp.txt"
        if cmp -s $out $RES/$w.interp.log; then verdict INFO "$w: interpreter (NOJIT) result identical to JIT"
        else verdict INFO "$w: interpreter (NOJIT) result differs from JIT: $(head -1 $RES/$w.interp.log)"; fi; }
    fi
  done
fi

# ---------------------------------------------------------------- 2b. differential testing
if has diff; then
  : > "$RES/diff-table.txt"
  for w in armwrestler arm7wrestler rockwrestler; do
    launch_dolphin "$C/cache/dols/$w-diff.dol" "$C/cache/wr-$w/out/$w.nds" 240 "$RES/dolphin-$w-diff.out"
    if wait_card_file "$w.log" "$RES/$w-diff.result" "\\[$w\\]" 120; then
      sleep 12; img_get jit.log "$RES/$w-diff.jit.log"     # let it run on past the result for more coverage
    fi
    kill_dolphin; [ -f "$RES/$w-diff.jit.log" ] || { verdict FAIL "diff:$w: no jit.log (build/run failure)"; continue; }
    echo "$w $(diff_summary "$RES/$w-diff.jit.log")" >> "$RES/diff-table.txt"
  done
  launch_dolphin "$C/cache/dols/aa-diff.dol" "$AAROM" 400 "$RES/dolphin-aa-diff.out"
  if wait_card_file statehash.log "$RES/aa-diff.hash" "frame=$DF " 300; then img_get jit.log "$RES/aa-diff.jit.log"; fi
  kill_dolphin
  if [ -f "$RES/aa-diff.jit.log" ]; then echo "AtomicAdventure${DF}f $(diff_summary "$RES/aa-diff.jit.log")" >> "$RES/diff-table.txt"
  else verdict FAIL "diff:AtomicAdventure: run did not reach frame $DF (crash/hang/too slow)"; fi
  [ $UPDATE = 1 ] && cp "$RES/diff-table.txt" "$C/baseline/diff.txt"
  while read -r n mm b9 u9 b7 u7; do
    base=$(awk -v n="$n" '$1==n{print $2}' "$C/baseline/diff.txt" 2>/dev/null); base=${base:-0}
    cov="arm9 blocks=$b9 untrusted=$u9 | arm7 blocks=$b7 untrusted=$u7"
    if [ "$mm" -gt "$base" ]; then verdict FAIL "diff:$n: $mm interp-vs-JIT mismatches (baseline $base)  [see $RES/$n-diff.jit.log or aa-diff.jit.log]  $cov"
    else verdict PASS "diff:$n: $mm mismatches (baseline $base)  $cov"; fi
  done < "$RES/diff-table.txt"
fi

# ---------------------------------------------------------------- 2c. state hash
run_hash() { # dolname outfile
  launch_dolphin "$C/cache/dols/$1.dol" "$AAROM" 400 "$RES/dolphin-$1.out"
  wait_card_file statehash.log "$2" "frame=${HF##*,} " 300; local rc=$?; kill_dolphin; return $rc; }
if has hash; then
  if run_hash aa-hash "$RES/hash.txt"; then
    if [ $UPDATE = 1 ]; then
      run_hash aa-hash "$RES/hash2.txt" && cmp -s "$RES/hash.txt" "$RES/hash2.txt" \
        && verdict INFO "hash determinism: two runs of the same build are bit-identical" \
        || verdict FAIL "hash NOT deterministic between two runs of the same build (baseline unusable)"
      cp "$RES/hash.txt" "$C/baseline/hash.txt"; echo "$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null) rm=$RM frames=$HF" > "$C/baseline/META"
    fi
    if cmp -s "$RES/hash.txt" "$C/baseline/hash.txt"; then verdict PASS "state-hash AtomicAdventure @ $HF: identical to baseline"
    else
      verdict FAIL "state-hash AtomicAdventure: differs from baseline (fields that differ per checkpoint below)"
      python3 - "$C/baseline/hash.txt" "$RES/hash.txt" <<'P' | tee -a "$SUMMARY"
import sys
a=[dict(x.split('=') for x in l.split()) for l in open(sys.argv[1]) if l.strip()]
b=[dict(x.split('=') for x in l.split()) for l in open(sys.argv[2]) if l.strip()]
for x,y in zip(a,b):
    d=[k for k in x if x[k]!=y.get(k)]
    print("      frame %s: %s" % (x['frame'], ','.join(d) if d else 'same'))
if len(a)!=len(b): print("      checkpoint count differs: %d vs %d"%(len(a),len(b)))
P
    fi
    if [ $INTERP = 1 ]; then
      run_hash aa-hash-interp "$RES/hash.interp.txt" && {
        python3 - "$RES/hash.txt" "$RES/hash.interp.txt" <<'P' | tee -a "$SUMMARY"
import sys
a=[dict(x.split('=') for x in l.split()) for l in open(sys.argv[1]) if l.strip()]
b=[dict(x.split('=') for x in l.split()) for l in open(sys.argv[2]) if l.strip()]
for x,y in zip(a,b):
    d=[k for k in x if x[k]!=y.get(k)]
    print("INFO  hash JIT-vs-interpreter frame %s: %s" % (x['frame'], ','.join(d) if d else 'identical'))
P
        [ $UPDATE = 1 ] && cp "$RES/hash.interp.txt" "$C/baseline/hash.interp.txt"; }
    fi
  else verdict FAIL "state-hash: AtomicAdventure run did not produce frame ${HF##*,} (crash/hang)"; fi
fi

echo "----"; echo "check.sh: $( [ $FAILS = 0 ] && echo PASS || echo "FAIL ($FAILS)" )  tree=$SRC  host time $(el)s  results=$RES" | tee -a "$SUMMARY"
[ $FAILS = 0 ]
