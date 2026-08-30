#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ -d "$SCRIPT_DIR/tools/bin" ]; then
    export PATH="$SCRIPT_DIR/tools/bin:$PATH"
fi

echo "=== Configuring Moonlight-XP build ==="
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-xp-i686.cmake

echo "=== Building Moonlight-XP ==="
ninja -C build

echo "=== Stripping executable ==="
mkdir -p dist
if command -v llvm-strip &> /dev/null; then
    llvm-strip -o dist/MoonlightXP.exe build/MoonlightXP.exe
elif command -v i686-w64-mingw32-strip &> /dev/null; then
    i686-w64-mingw32-strip -o dist/MoonlightXP.exe build/MoonlightXP.exe
else
    cp build/MoonlightXP.exe dist/MoonlightXP.exe
fi

echo "=== Success! Output binary: dist/MoonlightXP.exe ==="
ls -lh dist/MoonlightXP.exe
