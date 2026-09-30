#!/bin/bash
# GbaC0re PC build script — run inside "MSYS2 MinGW x64" from the pc/ directory.
# Usage: ./build.sh [clean]
set -e
cd "$(dirname "$0")"

if [ "$1" = "clean" ]; then
    make clean
    echo "cleaned."
    exit 0
fi

# Sanity checks.
command -v gcc >/dev/null || {
    echo "ERROR: gcc not found. Are you in 'MSYS2 MinGW x64'?"; exit 1; }
command -v sdl2-config >/dev/null || {
    echo "ERROR: sdl2-config not found. Run:"; 
    echo "  pacman -S --needed mingw-w64-x86_64-SDL2"; exit 1; }

make -j"$(nproc)"

# Stage SDL2.dll next to the exe so it runs by double-click.
DLL_SRC="$(sdl2-config --prefix)/bin/SDL2.dll"
if [ -f "$DLL_SRC" ]; then
    cp -u "$DLL_SRC" ./SDL2.dll
    echo "staged SDL2.dll"
else
    echo "WARNING: SDL2.dll not found at $DLL_SRC"
    echo "Copy it manually from C:\\msys64\\mingw64\\bin\\ next to gbac0re_pc.exe"
fi

echo ""
echo "Built: gbac0re_pc.exe"
echo "Run: ./gbac0re_pc.exe /c/path/to/game.gba"
