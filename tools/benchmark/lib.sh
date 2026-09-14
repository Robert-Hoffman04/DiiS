#-----------------------------------------------------------------------------
# tools/benchmark/lib.sh - shared build/stage/capture helpers for run.sh,
# ab.sh and soak.sh. Sourced, not executed.
#
# Every benchmark script rides the same three primitives:
#   build_dol   <name> <testdefs...>  -DJITDEFS=...   -> dols/<name>.dol
#   stage_rom   <rom-path> [state-path]                -> sd:/DS/ROMS/test.nds
#   capture_run <dol-name> <out-dir> <capture.py args...>
#                                                        -> boots it, waits,
#                                                           kills it
#
# All session control (dolphin_launch/dolphin_kill) comes from the one
# shared copy in tools/harness-control/common.sh (plan §3.7) - this file
# does not reimplement it.
#-----------------------------------------------------------------------------
BENCH_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "$BENCH_HERE/../.." && pwd)"

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC MTOOLS_SKIP_CHECK=1

HARNESS_HOST="${HARNESS_HOST:-127.0.0.1}"
HARNESS_PORT="${HARNESS_PORT:-4300}"

DOLDIR="$BENCH_HERE/dols"
mkdir -p "$DOLDIR"

. "$BENCH_ROOT/tools/harness-control/common.sh"

die()  { echo "benchmark: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# trim <string> - leading/trailing whitespace, pure bash (no xargs: a whole
# comment line can contain an unmatched quote, which xargs warns about).
trim() {
	local s="$1"
	s="${s#"${s%%[![:space:]]*}"}"
	s="${s%"${s##*[![:space:]]}"}"
	printf '%s' "$s"
}

bench_preflight() {
	have mcopy   || die "mtools not found (mcopy)"
	have flatpak || die "flatpak not found"
	have python3 || die "python3 not found"
	[ -f "$DOLPHIN_SD" ] || die "no Dolphin SD image at $DOLPHIN_SD (set DOLPHIN_SD)"
	mdir -i "$DOLPHIN_SD" ::/DS/BIOS >/dev/null 2>&1 || die "SD image has no DS/BIOS/ - stage the DS bios files first"
}

# build_dol <name> <testdefs (one string)> <jitdefs (one string)>
# Only rebuilds main.o + relinks when testdefs changes; forces a full `make
# clean` when jitdefs changes (JITDEFS touches every jit/*.cpp TU and
# depfiles don't track flag changes). Result: $DOLDIR/<name>.dol
_BENCH_LAST_JITDEFS="__unset__"
build_dol() {
	local name="$1" testdefs="$2" jitdefs="$3"
	echo ">> build $name.dol   (TESTDEFS: $testdefs${jitdefs:+   JITDEFS: $jitdefs})"
	local log="$DOLDIR/build_$name.log"; : > "$log"
	if [ "$jitdefs" != "$_BENCH_LAST_JITDEFS" ]; then
		( cd "$BENCH_ROOT" && make clean ) >>"$log" 2>&1
		_BENCH_LAST_JITDEFS="$jitdefs"
	fi
	( cd "$BENCH_ROOT" \
	  && rm -f build/main.o desmumewii.elf desmumewii.dol \
	  && make -j"$(nproc)" JITDEFS="$jitdefs" TESTDEFS="$testdefs" ) >>"$log" 2>&1 \
		|| { echo "   BUILD FAILED:"; tail -n 20 "$log"; exit 1; }
	cp "$BENCH_ROOT/desmumewii.dol" "$DOLDIR/$name.dol"
	echo "   -> $DOLDIR/$name.dol"
}

# build_wrestler <tool-dir under tools/> -> path to the built .nds
build_wrestler() {
	local dir="$BENCH_ROOT/tools/$1"
	( cd "$dir" && ./build.sh >build.log 2>&1 ) \
		|| { echo "   wrestler build failed ($1):"; tail -n 20 "$dir/build.log"; exit 1; }
	echo "$dir/out/$2"
}

#--- SD staging / restore ----------------------------------------------------
# Saves whatever is currently at sd:/DS/ROMS/test.nds and sd:/DS/SAVES/test.ds0
# once per script run, and restores it (plus clears stray log files) on exit.
_BENCH_HAVE_PREV_ROM=0
_BENCH_HAVE_PREV_STATE=0
bench_sd_snapshot() {
	local savedir="$1"
	mkdir -p "$savedir"
	mcopy -i "$DOLPHIN_SD" ::/DS/ROMS/test.nds "$savedir/_prev_test.nds" 2>/dev/null \
		&& _BENCH_HAVE_PREV_ROM=1
	mcopy -i "$DOLPHIN_SD" ::/DS/SAVES/test.ds0 "$savedir/_prev_test.ds0" 2>/dev/null \
		&& _BENCH_HAVE_PREV_STATE=1
	_BENCH_SAVEDIR="$savedir"
}
bench_sd_restore() {
	dolphin_kill
	if [ "$_BENCH_HAVE_PREV_ROM" = 1 ]; then
		mcopy -o -i "$DOLPHIN_SD" "$_BENCH_SAVEDIR/_prev_test.nds" ::/DS/ROMS/test.nds 2>/dev/null
	fi
	if [ "$_BENCH_HAVE_PREV_STATE" = 1 ]; then
		mcopy -o -i "$DOLPHIN_SD" "$_BENCH_SAVEDIR/_prev_test.ds0" ::/DS/SAVES/test.ds0 2>/dev/null
	else
		mdel -i "$DOLPHIN_SD" ::/DS/SAVES/test.ds0 2>/dev/null || true
	fi
	rm -f "$_BENCH_SAVEDIR"/_prev_test.* 2>/dev/null || true
}

# stage_rom <rom-path> [state-path]
stage_rom() {
	local rom="$1" state="${2:-}"
	mcopy -o -i "$DOLPHIN_SD" "$rom" ::/DS/ROMS/test.nds \
		|| die "mcopy of $rom failed"
	if [ -n "$state" ]; then
		mcopy -o -i "$DOLPHIN_SD" "$state" ::/DS/SAVES/test.ds0 \
			|| die "mcopy of state $state failed"
	else
		mdel -i "$DOLPHIN_SD" ::/DS/SAVES/test.ds0 2>/dev/null || true
	fi
}

# capture_run <dol-name> <out-dir> [capture.py args...]
# Starts capture.py listening in the background, launches Dolphin against
# the given dol, waits for capture.py to reach its own stop condition (frame
# budget / captured frame / timeout / crash - see capture.py --help), then
# kills Dolphin. Returns capture.py's exit code (0 pass, 1 assert-fail/crash,
# 2 device never connected).
capture_run() {
	local dolname="$1" outdir="$2"; shift 2
	mkdir -p "$outdir"
	python3 "$BENCH_HERE/capture.py" --host "$HARNESS_HOST" --port "$HARNESS_PORT" \
		--out "$outdir" --map "$BENCH_ROOT/desmumewii.elf.map" "$@" \
		>"$outdir/capture.stdout.log" 2>&1 &
	local cap_pid=$!
	sleep 1   # let capture.py bind its listen socket before Dolphin tries to connect
	dolphin_launch "$DOLDIR/$dolname.dol" "$outdir/dolphin.log"
	wait "$cap_pid"; local rc=$?
	dolphin_kill; sleep 2
	return $rc
}

#--- run metadata -------------------------------------------------------------
bench_write_meta() {
	local rundir="$1" duration_default="$2" modes="$3"
	local sha; sha="$(git -C "$BENCH_ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)"
	git -C "$BENCH_ROOT" diff --quiet 2>/dev/null || sha="${sha}-dirty"
	local dolphin_ver; dolphin_ver="$(timeout 20 flatpak run org.DolphinEmu.dolphin-emu --version 2>/dev/null | head -1)"
	local cpu; cpu="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
	cat > "$rundir/meta.json" <<JSON
{
  "timestamp": "$(date -u +%Y%m%dT%H%M%SZ)",
  "git": "$sha",
  "host": "${cpu:-unknown}",
  "kernel": "$(uname -sr)",
  "dolphin": "${dolphin_ver:-unknown}",
  "timeout_default_s": $duration_default,
  "modes": "$modes"
}
JSON
}
