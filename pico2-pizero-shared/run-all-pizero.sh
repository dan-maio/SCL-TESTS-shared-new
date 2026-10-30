#!/usr/bin/env bash
# run-all-pizero.sh — build and run every test on the Pi-Zero RP2350B, one by one,
# showing the semihosting console live, with a gap between tests.
#
#   ./run-all-pizero.sh                  # all tests
#   ./run-all-pizero.sh fix              # only tests whose name matches "fix"
#   POWER_CYCLE=1 ./run-all-pizero.sh    # wait for a power cycle between tests
#
# Uses the port's OpenOCD sequence (test/boards/pico2-pizero/hw.sh). Each test's output
# is also saved under logs/. The UART is not opened, so it stays free for a terminal;
# the firmware prints the same results there.
#
# Tunables: GAP (seconds, default 3), SECS (per test, default 240), UOS_PORT_DIR, BUILD.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UOS_PORT_DIR="${UOS_PORT_DIR:-/home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-cortexm}"
BUILD="${BUILD:-$HERE/build}"
SECS="${SECS:-240}"
GAP="${GAP:-3}"
FILTER="${1:-}"
LOGDIR="$HERE/logs/pizero-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
TESTS=(scl-smp-reloc-sram scl-smp-reloc-psram scl-smp-nolib-sram scl-smp-nolib-psram scl-smp-xip-flash
       scl-smp-xip-flash-psram scl-smp-reloc-psram-big scl-smp-nolib-psram-big
       scl-fix-reloc-sram scl-fix-reloc-psram scl-fix-nolib-sram scl-fix-nolib-psram
       scl-fix-xip-flash scl-fix-xip-flash-psram
       scl-fix2-reloc-sram scl-fix2-reloc-psram scl-fix2-nolib-sram scl-fix2-nolib-psram
       scl-fix2-xip-flash scl-fix2-xip-flash-psram
       scl-fix-newlib-sram scl-fix2-newlib-sram scl-fix3-static-sram scl-fix3-static-psram
       scl-fix-optimal-sram scl-fix-optimal-psram scl-fix2-optimal-sram scl-fix2-optimal-psram
       scl-fix-overlay-sram scl-fix-overlay-psram
       scl-fix-xipstatic-flash scl-fix-xipstatic-flash-psram scl-fix2-xipstatic-flash scl-fix2-xipstatic-flash-psram
       scl-fix-xipnolib-flash scl-fix-xipnolib-flash-psram scl-fix2-xipnolib-flash scl-fix2-xipnolib-flash-psram
       scl-fix4-nolib-sram scl-fix4-nolib-psram scl-fix4-xip-flash scl-fix4-xipstatic-flash scl-fix4-xipnolib-flash
       scl-fix4-newlib-sram scl-fix4-static-sram scl-fix4-reloc-sram scl-fix4-optimal-sram
       scl-fix5-reloc-sram scl-fix5-reloc-psram scl-fix5-nolib-sram scl-fix5-nolib-psram
       scl-fix5-newlib-sram scl-fix5-static-sram scl-fix5-static-psram scl-fix5-optimal-sram
       scl-fix5-xip-flash scl-fix5-xipstatic-flash scl-fix5-xipnolib-flash
       scl-fix6-reloc-sram scl-fix6-reloc-psram scl-fix6-nolib-sram scl-fix6-nolib-psram
       scl-fix6-newlib-sram scl-fix6-static-sram scl-fix6-static-psram scl-fix6-optimal-sram
       scl-fix6-xip-flash scl-fix6-xipstatic-flash scl-fix6-xipnolib-flash
       scl-neg-reloc scl-neg-nolib scl-neg-newlib scl-neg-static
       scl-neg-optimal scl-neg-xip scl-neg-xipstatic scl-neg-xipnolib)
pass=0; fail=0; first=1
declare -a RESULTS=()
export PATH="${ARM_TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}:$PATH"
cmake -S "$HERE" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target scl-smp-all scl-fix-all -j8 >"$LOGDIR/build.log" 2>&1 || { echo "build failed: $LOGDIR/build.log"; exit 1; }
for t in "${TESTS[@]}"; do
    [[ -n "$FILTER" && ! "$t" =~ $FILTER ]] && continue
    if [[ "${POWER_CYCLE:-0}" == "1" && $first == 0 ]]; then
        read -r -p "Power-cycle the board, then press Enter: " _
    fi
    first=0
    echo
    echo "================================================================"
    echo "--- RUN  $t"
    echo "================================================================"
    # OpenOCD block-buffers a pipe; line-buffer it so the console is live.
    BUILD="$BUILD" OPENOCD="$HERE/openocd-linebuf.sh" \
        "$UOS_PORT_DIR/test/boards/pico2-pizero/hw.sh" "$t" "$SECS" 2>&1 | tee "$LOGDIR/$t.log"
    rc=${PIPESTATUS[0]}
    if [[ $rc -eq 0 ]]; then
        echo "--- PASS $t"; RESULTS+=("PASS  $t"); ((pass++))
    else
        echo "--- FAIL $t (rc=$rc)"; RESULTS+=("FAIL  $t"); ((fail++))
    fi
    echo "--- gap ${GAP}s"
    sleep "$GAP"
done
echo
echo "================ SUMMARY ================"
printf '%s\n' "${RESULTS[@]}"
echo "PASS=$pass  FAIL=$fail"
echo "logs: $LOGDIR"
[[ $fail -eq 0 ]]
