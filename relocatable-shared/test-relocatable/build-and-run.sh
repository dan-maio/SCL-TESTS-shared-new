#!/usr/bin/env bash
# build-and-run.sh — configure + build the relocatable (ET_REL) test firmware
# (fixtures are built automatically by the CMake target), then flash+run on a
# board or under QEMU.
#
#   test-relocatable/build-and-run.sh [<weact|nucleo|qemu>]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
ROOT="$HERE/.."
BUILD="$HERE/build"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    echo "Usage: $(basename "$0") [<weact|nucleo|qemu>]" >&2
    exit 1
fi
if [[ "${1:-}" == "weact" || "${1:-}" == "nucleo" || "${1:-}" == "qemu" ]]; then
    export SCL_BOARD="$1"
fi
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu" >&2; exit 1 ;; esac

echo "=== configure + build (CMake also builds the ET_REL fixtures) ==="
rm -rf "$BUILD"; mkdir -p "$BUILD"
if [[ "$SCL_BOARD" == "qemu" ]]; then
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
          -DSCL_PLATFORM=qemu-f411re
else
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
          -DSCL_PLATFORM=board -DSCL_BOARD="$SCL_BOARD"
fi
cmake --build "$BUILD" --target scl-fw-reloc -j

if [[ "$SCL_BOARD" == "qemu" ]]; then
    ELF="$BUILD/scl-fw-reloc"
    [[ -f "$ELF" ]] || { echo "error: $ELF not built" >&2; exit 1; }
    echo "=== run under QEMU ==="
    exec "$ROOT/test/tools/qemu-run.sh" --machine netduinoplus2 --done "RESULT: done" "$ELF"
else
    HEX="$BUILD/scl-fw-reloc.hex"
    echo "=== flash + run (semihosting console follows) ==="
    killall -s 9 openocd 2>/dev/null || true
    exec "$ROOT/test/tools/flash-run.sh" "$HEX"
fi
