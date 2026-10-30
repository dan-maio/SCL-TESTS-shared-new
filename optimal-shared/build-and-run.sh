#!/usr/bin/env bash
# build-and-run.sh — self-contained one-shot for a chosen test.
#
#   ./build-and-run.sh [ram|xip] [weact|nucleo|qemu]     (default: xip qemu)
#
# Builds fixtures (generic recipe, auto-packed) -> gen-imports -> configures the
# whole project (vendored scl/ + common-elf-reader submodule) -> builds the test
# firmware -> runs it. No external dependencies beyond the ARM toolchain + QEMU.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCL="$HERE"
source "$SCL/tools-config.sh" 2>/dev/null || true
if [[ ! -x "$ARM_TOOLCHAIN_BIN/arm-none-eabi-gcc" ]]; then
    ARM_TOOLCHAIN_BIN=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
fi
export ARM_TOOLCHAIN_BIN
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"

TEST="${1:-xip}"; BOARD="${2:-qemu}"
case "$TEST" in ram) TARGET=scl-fw-optimal-ram ;; xip) TARGET=scl-fw-optimal-xip ;;
  *) echo "usage: $0 [ram|xip] [weact|nucleo|qemu]" >&2; exit 1 ;; esac
BUILD="$HERE/build"

# ensure the submodule is present (self-contained checkout)
if [[ ! -f "$HERE/common-elf-reader/CMakeLists.txt" ]]; then
    echo "=== fetching common-elf-reader submodule ==="
    git -C "$HERE" submodule update --init --recursive
fi

echo "=== 1/4 build fixtures (one generic recipe, auto-packed) ==="
bash "$HERE/fixtures-src/build.sh"

echo "=== 2/4 configure (gen-imports; vendored scl as INTERFACE) ==="
rm -rf "$BUILD"; mkdir -p "$BUILD"
if [[ "$BOARD" == "qemu" ]]; then
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$SCL/cmake/arm-none-eabi.cmake" \
          -DSCL_PLATFORM=qemu-f411re
else
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$SCL/cmake/arm-none-eabi.cmake" \
          -DSCL_PLATFORM=board -DSCL_BOARD="$BOARD"
fi

echo "=== 3/4 build $TARGET ==="
cmake --build "$BUILD" --target "$TARGET" -j

echo "=== 4/4 run $TARGET ($TEST on $BOARD) ==="
ELF="$(find "$BUILD" -type f -name "$TARGET" | head -1)"
if [[ "$BOARD" == "qemu" ]]; then
    exec "$SCL/test/tools/qemu-run.sh" --machine netduinoplus2 \
         --done "optimal-shared: done" "$ELF"
else
    killall -s 9 openocd 2>/dev/null || true
    exec "$SCL/test/tools/flash-run.sh" --timeout "${RUN_MS:-300000}" \
         --log "$BUILD/run.log" "$ELF.hex"
fi
