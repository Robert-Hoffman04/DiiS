#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# tools/harness-control/common.sh
#
# The single copy of the Dolphin session-control helpers. Both
# tools/vsd-testrom/harness/*.sh and tools/benchmark/*.sh source THIS file
# instead of keeping their own dolphin_launch/dolphin_kill/gecko_* copies
# (harness-and-network plan §3.7).
#
# This is *live session control* only. Offline analysis of already-collected
# results (tools/benchmark/analyze.py, perfzones.py, scenes.conf) stays where
# it is - a deliberately separate concern.
#
# Everything is overridable from the environment:
#   DOLPHIN_DATA    Dolphin flatpak data dir (SD images, configs)
#   DOLPHIN_SD      FAT image desmumewii reads sd:/ from
#   DOL_DIR         directory holding the .dol builds (basename form of
#                   dolphin_launch resolves against this)
#   DOLPHIN_SLOTB   Dolphin EXI Slot B device index ("" = leave default,
#                   7 = USB Gecko - set this for the gecko_* input path)
#   GECKO_HOST/PORT Dolphin's emulated USB Gecko TCP server (127.0.0.1:55020)
#   WIN_NAME        xdotool --name match for the running Dolphin window
#   SHOTS           directory screenshots are written to
#-----------------------------------------------------------------------------

DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DOL_DIR="${DOL_DIR:-$DOLPHIN_DATA/mergetest}"
DOLPHIN_SLOTB="${DOLPHIN_SLOTB:-}"
GECKO_HOST="${GECKO_HOST:-127.0.0.1}"
GECKO_PORT="${GECKO_PORT:-55020}"          # Dolphin EXI_DeviceGecko SERVER_PORT (0xd6ec)
WIN_NAME="${WIN_NAME:-Dolphin 2606a | JIT64}"
SHOTS="${SHOTS:-$DOL_DIR}"

export MTOOLS_SKIP_CHECK=1

#--- Dolphin process lifecycle ----------------------------------------------

dolphin_kill() {
	pkill -9 -x  dolphin-emu          2>/dev/null || true
	pkill -9 -f  dolphin-emu-wrapper  2>/dev/null || true
	gecko_close
}

# dolphin_launch <dol>  [logfile]
#   <dol>     either a path (contains '/' or ends in .dol) or a bare basename
#             resolved as $DOL_DIR/<name>.dol
#   [logfile] where stdout/stderr go (default $DOL_DIR/run_<name>.log)
# Launches headless (-b) in its own session, detached, in the background.
dolphin_launch() {
	local arg="$1" dol name log
	case "$arg" in
		*/*|*.dol) dol="$arg";              name="$(basename "${arg%.dol}")" ;;
		*)         dol="$DOL_DIR/$arg.dol"; name="$arg" ;;
	esac
	log="${2:-$DOL_DIR/run_$name.log}"

	local slotb=()
	[ -n "$DOLPHIN_SLOTB" ] && slotb=(-C "Dolphin.Core.SlotB=$DOLPHIN_SLOTB")

	setsid flatpak run org.DolphinEmu.dolphin-emu -b -e "$dol" \
		-C Dolphin.Core.WiiSDCard=True -C Dolphin.DSP.Volume=0 \
		"${slotb[@]}" \
		>"$log" 2>&1 &
	disown 2>/dev/null || true   # reaped with pkill; suppress the job-control "Killed" line
}

dolphin_wid() { xdotool search --name "$WIN_NAME" 2>/dev/null | head -1; }

#--- screenshots / image diff ---------------------------------------------
# On this GNOME/Wayland box Dolphin renders through Xwayland; `import -window`
# needs the X11 path (QT_QPA_PLATFORM=xcb for Dolphin itself, DISPLAY for import).
shot() { DISPLAY="${DISPLAY:-:0}" import -window "$1" "$2" 2>/dev/null; }

# compare(1) exits non-zero whenever the images differ; never let that abort a script.
ae_diff() { compare -metric AE "$1" "$2" "${3:-null:}" 2>&1 || true; }

#--- USB Gecko serial input ---------------------------------------------------
# Hold one connection open for the whole session so Dolphin's gecko client stays
# attached (usb_isgeckoalive() on the guest tracks that socket).
GECKO_FD=""
gecko_open() { # retries: Dolphin opens the listener a beat after the core starts
	local i
	for i in $(seq 1 30); do
		if exec {GECKO_FD}<>"/dev/tcp/$GECKO_HOST/$GECKO_PORT" 2>/dev/null; then
			echo "gecko: connected $GECKO_HOST:$GECKO_PORT"
			return 0
		fi
		sleep 1
	done
	echo "gecko: FAILED to connect $GECKO_HOST:$GECKO_PORT" >&2
	return 1
}
gecko_close() { [ -n "${GECKO_FD:-}" ] && { exec {GECKO_FD}>&- ; } 2>/dev/null; GECKO_FD=""; }
gecko_send()  { [ -n "${GECKO_FD:-}" ] || return 1; printf '%s' "$1" >&"$GECKO_FD"; }

# tap a button: send the byte, wait (button auto-releases after GECKO_TAP_FRAMES)
gecko_tap() { gecko_send "$1"; sleep "${2:-0.2}"; }

# NOTE: there used to be a merge_toggle() here (GC D-pad Down -> runtime
# GXMerge_SetEnabled toggle in main.cpp DSExec). GXMerge/GX2DBG compositing
# is now mandatory whenever the GX core runs - main.cpp has no runtime
# toggle left to send 'd' to, so this helper (and tools/vsd-testrom/
# harness/abtoggle.sh, which only existed to drive it) were removed rather
# than kept as dead code that would silently produce false-positive
# ON/ON diffs.
