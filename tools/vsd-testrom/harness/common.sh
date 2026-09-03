#!/usr/bin/env bash
# Shared config for the Dolphin-based test harness.
#
# Input is driven over Dolphin's USB Gecko / EXI "debug serial" (EXI Slot B),
# not a GCPad pipe: the .dol builds carry gekko_utils/geckoinput.cpp, which reads
# command bytes off Gecko channel 1 and turns them into pad presses (see that
# file's header for the byte protocol). dolphin_launch sets Slot B = USB Gecko;
# Dolphin then runs a TCP server on $GECKO_PORT that gecko_open connects to.
#
# Override any of these in the environment:
#   DOLPHIN_DATA   Dolphin's data dir (holds the SD images, etc.)
#   DOLPHIN_SD     FAT image that desmumewii reads sd:/DS/ROMS/test.nds from
#   DOL_DIR        directory containing the desmumewii .dol builds to test
#   GECKO_HOST/PORT  Dolphin's emulated USB Gecko TCP server (127.0.0.1:55020)
#   WIN_NAME       xdotool window-name match for the running Dolphin window
DOLPHIN_DATA="${DOLPHIN_DATA:-$HOME/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu}"
DOLPHIN_SD="${DOLPHIN_SD:-$DOLPHIN_DATA/desmume-sd.raw}"
DOL_DIR="${DOL_DIR:-$DOLPHIN_DATA/mergetest}"
GECKO_HOST="${GECKO_HOST:-127.0.0.1}"
GECKO_PORT="${GECKO_PORT:-55020}"          # Dolphin EXI_DeviceGecko SERVER_PORT (0xd6ec)
WIN_NAME="${WIN_NAME:-Dolphin 2606a | JIT64}"
SHOTS="${SHOTS:-$DOL_DIR}"

export MTOOLS_SKIP_CHECK=1

dolphin_kill() { pkill -9 -x dolphin-emu 2>/dev/null || true; pkill -9 -f dolphin-emu-wrapper 2>/dev/null || true; gecko_close; }

# compare(1) exits non-zero whenever the images differ; never let that abort a script.
ae_diff() { compare -metric AE "$1" "$2" "${3:-null:}" 2>&1 || true; }

dolphin_launch() { # <dol-basename>  -> launches in background, logs to $DOL_DIR/run_<name>.log
	setsid flatpak run org.DolphinEmu.dolphin-emu -b -e "$DOL_DIR/$1.dol" \
		-C Dolphin.Core.WiiSDCard=True -C Dolphin.DSP.Volume=0 \
		-C Dolphin.Core.SlotB=7 \
		>"$DOL_DIR/run_$1.log" 2>&1 &
}

dolphin_wid() { xdotool search --name "$WIN_NAME" 2>/dev/null | head -1; }

shot() { DISPLAY="${DISPLAY:-:0}" import -window "$1" "$2" 2>/dev/null; }

# --- USB Gecko serial input -------------------------------------------------
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
gecko_send() { [ -n "${GECKO_FD:-}" ] || return 1; printf '%s' "$1" >&"$GECKO_FD"; }

# tap a button: send the byte, wait, (button auto-releases after GECKO_TAP_FRAMES)
gecko_tap() { gecko_send "$1"; sleep "${2:-0.2}"; }

# GC D-pad Down (byte 'd') is the desmumewii runtime GXMerge on/off toggle
# (main.cpp DSExec:  pad & PAD_BUTTON_DOWN  ->  GXMerge_SetEnabled toggle).
merge_toggle() { gecko_send d; sleep 2; }
