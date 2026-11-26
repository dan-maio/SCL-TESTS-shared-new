#!/usr/bin/env bash
# run-all-lyra.sh — build and run every test on the Luckfox Lyra B, one by one, showing the
# semihosting console live, with a gap between tests.
#
#   ./run-all-lyra.sh                 # all tests
#   ./run-all-lyra.sh fix6            # only tests whose name matches "fix6"
#
# Uses the port's OpenOCD sequence (test/boards/luckfox-lyra/hw.sh), which resets the board before
# each test, so no power cycle is needed. The UART is not opened, so it stays free for a terminal.
#
# Tunables: GAP (seconds, default 3), SECS (per test, default 300), KHZ (SWD clock, default 10000),
# UOS_PORT_DIR, BUILD.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UOS_PORT_DIR="${UOS_PORT_DIR:-$HERE/../uos/micro-os-plus-iii-aarch32}"
BUILD="${BUILD:-$HERE/build}"
SECS="${SECS:-300}"
GAP="${GAP:-3}"
KHZ="${KHZ:-10000}"
FILTER="${1:-}"
LOGDIR="$HERE/logs/lyra-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
TESTS=()
for g in "" 2; do
    for l in reloc nolib newlib optimal xip xipstatic xipnolib; do TESTS+=("scl-fix$g-$l"); done
done
TESTS+=(scl-fix3-static scl-fix-overlay)
for g in 5 6 4; do
    for l in reloc nolib newlib static optimal xip xipstatic xipnolib; do TESTS+=("scl-fix$g-$l"); done
done
for l in reloc nolib newlib static optimal xip xipstatic xipnolib; do TESTS+=("scl-neg-$l"); done
pass=0; fail=0
declare -a RESULTS=()
export PATH="${ARM_TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}:$PATH"
cmake -S "$HERE" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target scl-all -j16 >"$LOGDIR/build.log" 2>&1 || { echo "build failed: $LOGDIR/build.log"; exit 1; }
for t in "${TESTS[@]}"; do
    [[ -n "$FILTER" && ! "$t" =~ $FILTER ]] && continue
    echo
    echo "================================================================"
    echo "--- RUN  $t"
    echo "================================================================"
    # OpenOCD block-buffers a pipe; line-buffer it so the console is live.
    BUILD="$BUILD" OPENOCD="$HERE/openocd-linebuf.sh" UOS_HW_ADAPTER_KHZ="$KHZ" \
        "$UOS_PORT_DIR/test/boards/luckfox-lyra/hw.sh" "$t" "$SECS" 2>&1 | tee "$LOGDIR/$t.log"
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
