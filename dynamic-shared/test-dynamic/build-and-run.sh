#!/usr/bin/env bash
# build-and-run.sh — build the simple POSIX-dl fixtures, configure + build the
# test firmware, then flash+run it on a board or under QEMU.
#
#   test-dynamic/build-and-run.sh [<board>] <target>
#
#   board   weact | nucleo | qemu   (default weact)
#   target  test1
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
ROOT="$HERE/.."
BUILD="$HERE/build"
LOG="$BUILD/run.log"

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $(basename "$0") [<weact|nucleo|qemu>] <test1>" >&2
    exit 1
fi
if [[ "$1" == "weact" || "$1" == "nucleo" || "$1" == "qemu" ]]; then
    export SCL_BOARD="$1"; shift
fi
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu" >&2; exit 1 ;; esac

APP="${1:-test1}"

echo "=== build simple POSIX-dl fixtures ==="
"$HERE/fixtures-src/build.sh"

echo "=== configure (hosted toolchain) ==="
rm -rf "$BUILD"; mkdir -p "$BUILD"
if [[ "$SCL_BOARD" == "qemu" ]]; then
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi-hosted.cmake" \
          -DSCL_PLATFORM=qemu-f411re
else
    cmake -S "$HERE" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi-hosted.cmake" \
          -DSCL_PLATFORM=board -DSCL_BOARD="$SCL_BOARD"
fi

echo "=== build firmware ==="
cmake --build "$BUILD" --target scl-fw-posix-dl -j

ELF="$BUILD/test1/scl-fw-posix-dl"
[[ -f "$ELF" ]] || { echo "error: $ELF not built" >&2; exit 1; }

if [[ "$SCL_BOARD" == "qemu" ]]; then
    echo "=== run $APP under QEMU ==="
    exec "$ROOT/test/tools/qemu-run.sh" --machine netduinoplus2 --done "RESULT:" "$ELF"
else
    HEX="$BUILD/test1/scl-fw-posix-dl.hex"
    echo "=== flash + run $APP (semihosting console -> $LOG) ==="
    killall -s 9 openocd 2>/dev/null || true
    exec "$ROOT/test/tools/flash-run.sh" --log "$LOG" "$HEX"
fi
