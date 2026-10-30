#!/usr/bin/env bash
# run-all-pizero.sh — run the SMP loader tests on the Pi-Zero RP2350B, one by one.
#
#   ./run-all-pizero.sh                  # all tests
#   ./run-all-pizero.sh psram            # only tests whose name matches "psram"
#   POWER_CYCLE=1 ./run-all-pizero.sh    # wait for a power cycle between tests
#
# Uses the port's OpenOCD sequence (test/boards/pico2-pizero/hw.sh) and saves the
# board's UART output, which holds the per-check results, beside each log.
#
# Tunables: UOS_PORT_DIR, BUILD, UART (default: try /dev/ttyACM*), SECS (default 240).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UOS_PORT_DIR="${UOS_PORT_DIR:-/home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-cortexm}"
BUILD="${BUILD:-$HERE/build}"
SECS="${SECS:-240}"
FILTER="${1:-}"
LOGDIR="$HERE/logs/pizero-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGDIR"
TESTS=(scl-smp-reloc-sram scl-smp-reloc-psram scl-smp-nolib-sram scl-smp-nolib-psram scl-smp-xip-flash)
pass=0; fail=0; first=1
for t in "${TESTS[@]}"; do
    [[ -n "$FILTER" && ! "$t" =~ $FILTER ]] && continue
    if [[ "${POWER_CYCLE:-0}" == "1" && $first == 0 ]]; then
        read -r -p "Power-cycle the board, then press Enter: " _
    fi
    first=0
    uart_pids=()
    for dev in ${UART:-/dev/ttyACM*}; do
        stty -F "$dev" 115200 raw -echo 2>/dev/null || continue
        cat "$dev" > "$LOGDIR/$t.uart.$(basename "$dev")" 2>/dev/null &
        uart_pids+=($!)
    done
    BUILD="$BUILD" "$UOS_PORT_DIR/test/boards/pico2-pizero/hw.sh" "$t" "$SECS" > "$LOGDIR/$t.log" 2>&1
    rc=$?
    sleep 1
    kill "${uart_pids[@]}" 2>/dev/null; wait "${uart_pids[@]}" 2>/dev/null
    if [[ $rc -eq 0 ]]; then echo "PASS  $t"; ((pass++)); else echo "FAIL  $t (rc=$rc)"; ((fail++)); fi
done
echo "PASS=$pass  FAIL=$fail"
echo "logs: $LOGDIR"
[[ $fail -eq 0 ]]
