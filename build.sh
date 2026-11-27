#!/bin/sh
# Run inside the MSYS2 "MINGW32" shell (NOT MINGW64 / UCRT64: the game is 32-bit).
# One-time setup:  pacman -S --needed mingw-w64-i686-gcc
gcc -O2 -Wall -shared -o d3d8.dll d3d8_thps4plus.c -Wl,--kill-at -static-libgcc -luser32 -lkernel32 -lwinmm -ldxguid
echo "build finished - check above for the word error, and the d3d8.dll timestamp"
