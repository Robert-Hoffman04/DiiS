#!/bin/sh
# Host-only build - no devkitPPC/Wii toolchain needed, see main.cpp header.
set -e
cd "$(dirname "$0")"
g++ -std=gnu++17 -Wall -I../../source main.cpp \
    ../../source/gx/gx_swizzle.cpp ../../source/gx/gx_texconv.cpp \
    -o gx_texconv_test
./gx_texconv_test
