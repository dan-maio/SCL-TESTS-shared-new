#!/usr/bin/env bash
# run.sh — build fixtures + firmware, flash to the WeAct F411CE, run with the
# semihosting console (the .so is loaded over semihosting; results go to UART).
#   ./run.sh                 # build + flash + run (Ctrl-C to stop)
#   RUN_MS=90000 ./run.sh    # auto-shutdown after 90 s
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCL="$HERE/SCL-shared"
PLATFORM="$HERE/platform"
# insert SCL from git (doc import_in_project_full §0): clone on first run
GIT_REMOTE="${GIT_REMOTE:-/home/dan/Downloads/GIT}"
# loader provided by the shared SCL-shared clone (symlinked into $SCL); no clone needed
TC="${ARM_TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
export ARM_TOOLCHAIN_BIN="$TC"; export PATH="$TC:$PATH"
OPENOCD="${OPENOCD:-$(command -v openocd || echo /home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin/openocd)}"
OPENOCD_SCRIPTS="${OPENOCD_SCRIPTS:-/home/dan/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts}"
BUILD="$HERE/build"

echo "== fixtures (loadable floatlib.so + derived windows) =="
bash "$HERE/fixtures/build.sh"
echo "== configure + build firmware =="
[ -d "$BUILD" ] || cmake -S "$HERE" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$PLATFORM/cmake/arm-none-eabi.cmake"
cmake --build "$BUILD" --target floatapp -j

echo "== flash + run (semihosting console; float results also on UART PA9 115200) =="
killall -s 9 openocd 2>/dev/null || true
CMD=( "$OPENOCD" -s "$OPENOCD_SCRIPTS" -f "$HERE/openocd-weact.cfg"
      -c "init" -c "reset halt" -c "program $BUILD/floatapp.hex"
      -c "arm semihosting enable" -c "reset run" )
if [[ -n "${RUN_MS:-}" ]]; then CMD+=( -c "sleep $RUN_MS" -c "shutdown" ); fi
exec "${CMD[@]}"
