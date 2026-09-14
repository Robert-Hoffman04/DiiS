#!/usr/bin/env bash
#
# bisect-arm9-jit.sh
#
# Walks the commits unique to the arm9-jit-infra branch, oldest first, and for
# each one: checks it out, builds a .dol with the ARM9 JIT compiled in and
# the software renderer forced, launches it in Dolphin, then waits for you to
# test it and press a key before moving to the next commit.
#
# This does NOT run in a container -- it needs a real devkitPPC toolchain and
# a real Dolphin install with a GUI, so run it on your own machine.
#
# ---------------------------------------------------------------------------
# REQUIRED SETUP (edit these, or export them before running)
# ---------------------------------------------------------------------------
REPO_DIR="${REPO_DIR:-/home/robert/Desktop/WII/desmumewii}"  # path to your clone (has the Makefile)
DOLPHIN_CMD=(flatpak run org.DolphinEmu.dolphin-emu)      # flatpak Dolphin; edit if your app id differs
START_COMMIT="${START_COMMIT:-5f03307}"                  # A0: first commit where the ARM9 JIT front end exists at all
END_REF="${END_REF:-657008d}"                             # stop at A3 (already confirmed bad) -- change if you want to go further
LOG_FILE="${LOG_FILE:-$REPO_DIR/bisect-log.csv}"
CLEAN_EACH_BUILD="${CLEAN_EACH_BUILD:-1}"                 # 1 = `make clean` before every build (safer, slower)
JITDEFS="${JITDEFS:--DDESMUME_JIT_ARM7 -DDESMUME_JIT_ARM9_ON}"
TESTDEFS="${TESTDEFS:--DDESMUME_FORCE_CORE=2 -DDESMUME_FORCE_ROM}"  # 2 = software raster; FORCE_ROM skips the file browser
#
# Notes:
# - DEVKITPPC must already be exported in your shell (as the Makefile requires).
# - DESMUME_FORCE_ROM boots straight into sd:/DS/ROMS/test.nds (or usb:/... if
#   you set DESMUME_FORCE_USB=1). Make sure that ROM exists on the SD image
#   Dolphin points at, or drop -DDESMUME_FORCE_ROM from TESTDEFS and pick the
#   ROM by hand each run.
# - DOLPHIN_CMD is an array so multi-word launchers (flatpak run ...) work.
#   Other examples if you switch setups later:
#     native Linux:  DOLPHIN_CMD=(dolphin-emu)
#     macOS:         DOLPHIN_CMD=(/Applications/Dolphin.app/Contents/MacOS/Dolphin)
#     Windows:       DOLPHIN_CMD=("/c/Program Files/Dolphin/Dolphin.exe")
#   Confirm your flatpak app id with: flatpak list --app | grep -i dolphin
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# On commits before 657008d, `jitArm9Enabled` is hardcoded `false` with no
# #ifdef gate -- -DDESMUME_JIT_ARM9_ON has no effect there, so the ARM9 JIT
# never actually runs no matter what JITDEFS says. This patches the literal
# `false` to `true` on those older commits so the front-end code that *does*
# exist gets genuinely exercised. On commits that already have the real gate,
# this is a no-op and JITDEFS handles it normally.
# ---------------------------------------------------------------------------
force_arm9_if_ungated() {
    local f="source/jit/jit_exec.cpp"
    [ -f "$f" ] || return 0
    if grep -q "DESMUME_JIT_ARM9_ON" "$f"; then
        return 0
    fi
    if grep -q "^bool jitArm9Enabled = false;" "$f"; then
        sed -i 's/^bool jitArm9Enabled = false;/bool jitArm9Enabled = true;  \/\/ forced by bisect script -- pre-A3 commit, no ifdef gate yet/' "$f"
        echo "  (this commit predates the DESMUME_JIT_ARM9_ON gate -- patched jitArm9Enabled to true so it's actually exercised)"
    fi
}

set -uo pipefail

if [ -z "${DEVKITPPC:-}" ]; then
    echo "ERROR: DEVKITPPC is not set. export DEVKITPPC=<path to devkitPPC> and re-run."
    exit 1
fi

if ! command -v "${DOLPHIN_CMD[0]}" >/dev/null 2>&1; then
    echo "ERROR: '${DOLPHIN_CMD[0]}' not found on PATH. Set DOLPHIN_CMD correctly."
    exit 1
fi
if [ "${DOLPHIN_CMD[0]}" = "flatpak" ] && ! flatpak info "${DOLPHIN_CMD[2]:-}" >/dev/null 2>&1; then
    echo "ERROR: flatpak app '${DOLPHIN_CMD[2]:-}' not installed. Check: flatpak list --app | grep -i dolphin"
    exit 1
fi

cd "$REPO_DIR" || { echo "ERROR: REPO_DIR '$REPO_DIR' not found."; exit 1; }

# Make sure we actually have the commits (in case of a shallow clone).
git rev-parse "$START_COMMIT" >/dev/null 2>&1 || { echo "ERROR: can't resolve START_COMMIT '$START_COMMIT' -- try 'git fetch --unshallow'."; exit 1; }
git rev-parse "$END_REF" >/dev/null 2>&1 || { echo "ERROR: can't resolve END_REF '$END_REF'."; exit 1; }

# Ordered, oldest-first, inclusive of START_COMMIT.
mapfile -t COMMITS < <(git log --oneline --reverse "${START_COMMIT}~1..${END_REF}")

if [ "${#COMMITS[@]}" -eq 0 ]; then
    echo "ERROR: no commits found in range ${START_COMMIT}~1..${END_REF}"
    exit 1
fi

echo "Found ${#COMMITS[@]} commits to walk, oldest first."
echo "Log file: $LOG_FILE"
[ -f "$LOG_FILE" ] || echo "index,commit,subject,build_result,verdict,notes" > "$LOG_FILE"

# Remember original ref so we can restore it at the end / on Ctrl-C.
ORIGINAL_REF="$(git symbolic-ref --short -q HEAD || git rev-parse HEAD)"

# flatpak's `flatpak run` wrapper process doesn't always propagate a kill into
# the sandboxed app, so use `flatpak kill` (by app id) as the reliable way to
# close it, falling back to killing the wrapper PID for non-flatpak launchers.
stop_dolphin() {
    if [ "${DOLPHIN_CMD[0]}" = "flatpak" ]; then
        flatpak kill "${DOLPHIN_CMD[2]}" 2>/dev/null
    fi
    if [ -n "${DOLPHIN_PID:-}" ] && kill -0 "$DOLPHIN_PID" 2>/dev/null; then
        kill "$DOLPHIN_PID" 2>/dev/null
        wait "$DOLPHIN_PID" 2>/dev/null
    fi
    unset DOLPHIN_PID
}

cleanup() {
    echo
    echo "Cleaning up..."
    stop_dolphin
    echo "Restoring original ref: $ORIGINAL_REF"
    git checkout --quiet "$ORIGINAL_REF"
}
trap cleanup EXIT INT TERM

idx=0
total="${#COMMITS[@]}"

for line in "${COMMITS[@]}"; do
    idx=$((idx + 1))
    hash="${line%% *}"
    subject="${line#* }"

    echo
    echo "=================================================================="
    echo "[$idx/$total] $hash  $subject"
    echo "=================================================================="

    git checkout --quiet --force "$hash" || {
        echo "  checkout FAILED, skipping."
        echo "$idx,$hash,\"$subject\",checkout-failed,skip," >> "$LOG_FILE"
        continue
    }

    if [ "$CLEAN_EACH_BUILD" = "1" ]; then
        make clean >/dev/null 2>&1
    fi

    force_arm9_if_ungated

    echo "  building (JITDEFS=$JITDEFS  TESTDEFS=$TESTDEFS)..."
    BUILD_LOG="$(mktemp)"
    if make JITDEFS="$JITDEFS" TESTDEFS="$TESTDEFS" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" > "$BUILD_LOG" 2>&1; then
        echo "  build OK."
    else
        echo "  BUILD FAILED. Last 30 lines:"
        tail -30 "$BUILD_LOG" | sed 's/^/    /'
        echo "$idx,$hash,\"$subject\",build-failed,skip,\"see $BUILD_LOG\"" >> "$LOG_FILE"
        echo "  Press [Enter] to skip to the next commit (build log kept at $BUILD_LOG)..."
        read -r -n1 _
        continue
    fi
    rm -f "$BUILD_LOG"

    DOL_NAME="$(basename "$REPO_DIR").dol"
    if [ ! -f "$DOL_NAME" ]; then
        echo "  ERROR: expected output '$DOL_NAME' not found after build, skipping."
        echo "$idx,$hash,\"$subject\",no-dol,skip," >> "$LOG_FILE"
        continue
    fi

    echo "  launching Dolphin with $DOL_NAME ..."
    "${DOLPHIN_CMD[@]}" -e "$REPO_DIR/$DOL_NAME" >/dev/null 2>&1 &
    DOLPHIN_PID=$!

    echo
    echo "  Test this build now. When you're done:"
    echo "    [g] mark GOOD (bug not yet present) and continue"
    echo "    [b] mark BAD  (bug present) and continue"
    echo "    [s] skip / unsure, no verdict, continue"
    echo "    [q] quit the whole run"
    read -r -n1 verdict_key
    echo

    case "$verdict_key" in
        g|G) verdict="good" ;;
        b|B) verdict="bad" ;;
        q|Q)
            verdict="quit"
            echo "$idx,$hash,\"$subject\",ok,$verdict," >> "$LOG_FILE"
            echo "Stopping at your request."
            exit 0
            ;;
        *) verdict="skip" ;;
    esac

    stop_dolphin

    echo "$idx,$hash,\"$subject\",ok,$verdict," >> "$LOG_FILE"
done

echo
echo "Done. Results in $LOG_FILE"