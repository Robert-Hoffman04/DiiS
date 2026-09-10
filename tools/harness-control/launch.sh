#!/usr/bin/env bash
#-----------------------------------------------------------------------------
# tools/harness-control/launch.sh - one entry point for getting a .dol running,
# against Dolphin or a real Wii (plan §3.7 / §0 / §6).
#
#   launch.sh dolphin <dol> [logfile]
#       headless Dolphin with Wii network passthrough on; the .dol's
#       HARNESS_TRANSPORT_NET connects back to wii_control.py exactly the way
#       real hardware will. This is the default dev path.
#
#   launch.sh wiiload <dol> [companion-file ...]
#       real wiiload transfer to WIILOAD=tcp:<host> (devkitPro wiiload, :4299).
#       Target is a physical Wii or Dolphin's emulated one - only the IP differs.
#       *** Sending/launching on a physical Wii needs explicit per-run
#       permission every time (plan §6.3). This script only does the transfer
#       you asked for; it does not decide that for you. ***
#
# Env: WIILOAD (tcp:<host>), plus everything common.sh honours.
#-----------------------------------------------------------------------------
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/common.sh"

mode="${1:-}"; shift || true

case "$mode" in
	dolphin)
		dol="${1:?usage: launch.sh dolphin <dol> [logfile]}"
		# Network passthrough must be enabled in this Dolphin's config
		# (Config -> Wii -> "BBA network settings" / Wii network); this is a
		# one-time settings check, not something the script can force via -C
		# reliably across Dolphin versions. See plan §0.
		dolphin_kill; sleep 2
		dolphin_launch "$dol" "${2:-}"
		echo "launched (dolphin): $dol"
		;;
	wiiload)
		dol="${1:?usage: launch.sh wiiload <dol> [companion-file ...]}"; shift
		: "${WIILOAD:?set WIILOAD=tcp:<host> (Dolphin's or the Wii's IP)}"
		echo "wiiload -> $WIILOAD : $dol ${*:+(+ $*)}"
		wiiload "$dol" "$@"
		;;
	*)
		sed -n '2,30p' "$0"
		exit 2
		;;
esac
