#!/bin/sh
# Host-only build - no devkitPPC/Wii toolchain needed, see main.cpp header.
set -e
cd "$(dirname "$0")"
g++ -std=gnu++17 -Wall -I../../source main.cpp \
    ../../source/library/lib_rom.cpp ../../source/library/lib_png.cpp \
    ../../source/library/lib_ini.cpp ../../source/library/library.cpp \
    -lz -o library_test
./library_test "$@"
