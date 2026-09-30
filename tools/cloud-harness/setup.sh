#!/usr/bin/env bash
# One-time setup of the headless test environment used by perf/, correct/ and ab/
# (a Claude Code cloud container: Ubuntu 24.04, Docker, no devkitPro package
# server access). Everything lands under /home/user/tools, which the scripts
# assume; nothing here touches the repo.
set -euo pipefail
T=/home/user/tools; mkdir -p $T

# 1. devkitPPC/devkitARM: the official image (pkg.devkitpro.org is blocked here).
pgrep dockerd >/dev/null || { (nohup dockerd >/tmp/dockerd.log 2>&1 &); sleep 8; }
docker pull devkitpro/devkitppc

# 2. Dolphin from source (Qt, OpenGL via Mesa llvmpipe under Xvfb).
apt-get update -qq || true
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  qt6-base-dev qt6-base-private-dev libqt6svg6-dev libxi-dev libxrandr-dev libudev-dev \
  libevdev-dev libpulse-dev libasound2-dev libbluetooth-dev libusb-1.0-0-dev \
  libgl1-mesa-dev libegl1-mesa-dev libx11-dev libxext-dev libxkbcommon-dev \
  libcurl4-openssl-dev libsystemd-dev libmbedtls-dev libhidapi-dev libpng-dev \
  liblzo2-dev libsfml-dev libfmt-dev libspng-dev libzstd-dev libbz2-dev liblzma-dev \
  zlib1g-dev libgl1-mesa-dri xauth pkg-config xvfb xdotool mtools dosfstools imagemagick
[ -d $T/dolphin ] || git clone --depth 1 https://github.com/dolphin-emu/dolphin.git $T/dolphin
( cd $T/dolphin && git submodule update --init --recursive --jobs 8 --depth 1 &&
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF \
    -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_VULKAN=OFF \
    -DENABLE_AUTOUPDATE=OFF -DENABLE_LTO=OFF -DUSE_MGBA=OFF -DENABLE_CLI_TOOL=OFF \
    -DENABLE_SDL=OFF -DUSE_UPNP=OFF && ninja -C build dolphin-emu )

# 3. Scripts + a Dolphin user dir whose SD card is folder-synced from
#    userdir/Load/WiiSDSync (Dolphin builds userdir/Load/WiiSD.raw from it).
mkdir -p $T/perf $T/correct $T/ab $T/roms $T/perf/userdir/Load/WiiSDSync/DS/{ROMS,SAVES,BIOS}
H=$(cd "$(dirname "$0")" && pwd)
cp $H/perf/* $T/perf/; cp -r $H/correct/* $T/correct/; cp $H/ab/* $T/ab/
cp -r $T/perf/userdir $T/correct/userdir 2>/dev/null || true
echo "Now put the test ROM at $T/roms/'AtomicAdventure3D 1_1.nds' and"
echo "$T/perf/userdir/Load/WiiSDSync/DS/ROMS/test.nds, then run correct/check.sh --update-baseline <tree>."
