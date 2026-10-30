#!/usr/bin/env bash
# build-and-run.sh — build the XIP-split fixtures, configure + build the XIP
# dynamic test firmware, then flash+run on a board or under QEMU.
#
#   test-xip/build-and-run.sh [<weact|nucleo|qemu>] <nolib|newlib>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/../tools-config.sh"
ROOT="$HERE/.."
BUILD="$HERE/build"

if [[ $# -lt 1 || "${1:-}" == "-h" ]]; then
    echo "Usage: $(basename "$0") [<weact|nucleo|qemu>] <nolib|newlib>" >&2
    exit 1
fi
if [[ "$1" == "weact" || "$1" == "nucleo" || "$1" == "qemu" ]]; then
    export SCL_BOARD="$1"; shift
fi
VARIANT="${1:-nolib}"
case "$VARIANT" in
  nolib)  APP="scl-fw-xip-nolib1" ;;
  newlib) APP="scl-fw-xip-newlib1" ;;
  *) echo "variant must be 'nolib' or 'newlib'" >&2; exit 1 ;;
esac
SCL_BOARD="${SCL_BOARD:-weact}"
case "$SCL_BOARD" in nucleo|weact|qemu) ;; *) echo "SCL_BOARD must be nucleo, weact or qemu" >&2; exit 1 ;; esac

echo "=== build XIP-split fixtures ==="
"$HERE/fixtures-src/build-fixtures.sh"
"$HERE/fixtures-src/build-xip-libs.sh"

echo "=== configure + build ==="
rm -rf "$BUILD"; mkdir -p "$BUILD"
if [[ "$VARIANT" == "newlib" ]]; then
    # newlib XIP app: separate HOSTED build (needs a hosted C++ runtime).
    SRC="$HERE/test-xip-dynamic-newlib/test1"
    TOOLCHAIN="$ROOT/cmake/arm-none-eabi-hosted.cmake"
else
    SRC="$HERE"
    TOOLCHAIN="$ROOT/cmake/arm-none-eabi.cmake"
fi
if [[ "$SCL_BOARD" == "qemu" ]]; then
    cmake -S "$SRC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DSCL_PLATFORM=qemu-f411re
else
    cmake -S "$SRC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
          -DSCL_PLATFORM=board -DSCL_BOARD="$SCL_BOARD"
fi
cmake --build "$BUILD" --target "$APP" -j

if [[ "$SCL_BOARD" == "qemu" ]]; then
    ELF="$BUILD/$APP"
    [[ -f "$ELF" ]] || ELF="$(find "$BUILD" -name "$APP" -type f | head -1)"
    echo "=== run under QEMU ==="
    exec "$ROOT/test/tools/qemu-run.sh" --machine netduinoplus2 --done "RESULT: done" "$ELF"
else
    HEX="$(find "$BUILD" -name "$APP.hex" | head -1)"
    echo "=== flash + run (semihosting console follows) ==="
    killall -s 9 openocd 2>/dev/null || true
    exec "$ROOT/test/tools/flash-run.sh" "$HEX"
fi
