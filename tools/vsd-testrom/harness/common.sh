#!/usr/bin/env bash
# Shared config for the Dolphin-based vsd-testrom harness.
#
# The dolphin_launch / dolphin_kill / gecko_* / shot / ae_diff helpers now live
# in ONE place - tools/harness-control/common.sh - and this file is a thin shim
# that sets the vsd-specific defaults and sources it (harness-and-network plan
# §3.7). Override any of DOLPHIN_DATA / DOLPHIN_SD / DOL_DIR / GECKO_HOST /
# GECKO_PORT / WIN_NAME / SHOTS in the environment as before.
#
# Input is driven over Dolphin's USB Gecko "debug serial" (EXI Slot B = USB
# Gecko): the .dol builds carry gekko_utils/geckoinput.cpp, which reads command
# bytes off Gecko channel 1 and turns them into pad presses. DOLPHIN_SLOTB=7
# below is what makes dolphin_launch set that slot.

DOLPHIN_SLOTB="${DOLPHIN_SLOTB:-7}"   # EXI Slot B = USB Gecko (vsd input path)

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../harness-control" && pwd)/common.sh"
