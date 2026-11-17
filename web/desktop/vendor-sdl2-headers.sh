#!/bin/sh
# Copies the build machine's SDL2 headers into web/desktop/sdl2-include for the aarch64
# cross build of wadamesh-desktop (SDL2 itself is loaded at run time, see sdl2_dlshim.c,
# so only the headers are needed). Needs libsdl2-dev (apt) on a 64-bit Linux host.
set -e
cd "$(dirname "$0")"
rm -rf sdl2-include && mkdir sdl2-include
cp /usr/include/SDL2/*.h sdl2-include/
cp /usr/include/*-linux-gnu/SDL2/*.h sdl2-include/ 2>/dev/null || true
# one directory, and no host-CPU intrinsics in the config
sed -i 's|#include <SDL2/_real_SDL_config.h>|#include "_real_SDL_config.h"|' sdl2-include/SDL_config.h
sed -i '/#define HAVE_IMMINTRIN_H/d' sdl2-include/_real_SDL_config.h 2>/dev/null || true
echo "SDL2 headers in $(pwd)/sdl2-include"
