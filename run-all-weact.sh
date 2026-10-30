#!/usr/bin/env bash
# run-all-weact.sh — run every QEMU-origin suite on a WeAct F411CE, one by one.
#
#   ./run-all-weact.sh                 # all tests
#   ./run-all-weact.sh static          # only tests whose name matches "static"
#   DRY_RUN=1 ./run-all-weact.sh       # print commands only
#
# Each test is flashed, run with the semihosting console streamed live, and
# logged. A 3-second gap is inserted between tests. Failures do not stop the run;
# the summary at the end reports PASS/FAIL and the log paths.
#
# Tunables:
#   RUN_MS=60000   time each firmware is allowed to run (default 60 s)
#   GAP=3          seconds between tests
#   SKIP_SPI=1     skip the two tests that need the W25Q64 SPI flash
#   LOGDIR=...     log directory (default logs/weact-<timestamp>)
#   SCL_BOARD fixed to weact
set -uo pipefail
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/tools-config.sh" 2>/dev/null || true
if [[ ! -x "${ARM_TOOLCHAIN_BIN:-}/arm-none-eabi-gcc" ]]; then
    ARM_TOOLCHAIN_BIN=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
fi
export ARM_TOOLCHAIN_BIN
export PATH="$ARM_TOOLCHAIN_BIN:$PATH"
export SCL_BOARD=weact
export RUN_MS="${RUN_MS:-60000}"
GAP="${GAP:-3}"
SKIP_SPI="${SKIP_SPI:-0}"
FILTER="${1:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOGDIR="${LOGDIR:-$ROOT/logs/weact-$STAMP}"

if [[ "${DRY_RUN:-0}" != "1" ]]; then
    mkdir -p "$LOGDIR"
fi

# name | directory | command | completion marker
TESTS=(
  "optimal-xip|optimal-shared|./build-and-run.sh xip weact|optimal-shared: done"
  "optimal-ram|optimal-shared|./build-and-run.sh ram weact|optimal-shared: done"
  "relocatable|relocatable-shared|./test-relocatable/build-and-run.sh weact|RESULT: done"
  "dynamic-dlopen|dynamic-shared|./test-dynamic/build-and-run.sh weact test1|RESULT: SUCCESS"
  "dynamic-xip-nolib|dynamic-shared|./test-xip/build-and-run.sh weact nolib|RESULT: done"
  "dynamic-xip-newlib|dynamic-shared|./test-xip/build-and-run.sh weact newlib|RESULT: done"
  "overlay|overlay-shared|./test-overlay/build-and-run.sh weact test1|overlay: done"
  "static|static-shared|./test-static/build-and-run.sh weact static|Second 5"
  "static-cpp|static-shared|./test-static/build-and-run.sh weact static-cpp|Second 5"
  "static-direct|static-shared|./test-static/build-and-run.sh weact static-direct|All direct-link static loader tests completed successfully."
  "static-drv|static-shared|./test-static/build-and-run.sh weact drv|test-drv done"
  "xip-static-semihosting|static-shared|./test-xip/build-and-run.sh weact static/semihosting|scl-fw-xip-static: done"
  "xip-static-direct|static-shared|./test-xip/build-and-run.sh weact static-direct|scl-fw-xip-static-direct: done"
  "xip-static-no-pic|static-shared|./test-xip/build-and-run.sh weact static-no-pic|scl-fw-xip-static-no-pic: done"
  "xip-static-no-pic-simple|static-shared|./test-xip/build-and-run.sh weact static-no-pic-simple|scl-fw-xip-static-no-pic-simple: done"
  "xip-interrupt-static|static-shared|./test-xip/build-and-run.sh weact interrupt-static|regular tests done"
  "xip-interrupt-static-direct|static-shared|./test-xip/build-and-run.sh weact interrupt-static-direct|regular tests done"
  "xip-spi-flash|static-shared|./test-xip/build-and-run.sh weact static/spi-flash|scl-fw-xip-static-spi-flash: done"
)

SPI_TESTS=" static-drv xip-spi-flash "

declare -a RESULTS=()
pass=0; fail=0; skip=0

echo "SCL-TESTS-shared-new — WeAct F411CE run-all"
echo "logs: $LOGDIR"
echo "RUN_MS=${RUN_MS}ms  GAP=${GAP}s  SKIP_SPI=$SKIP_SPI"
[[ -n "$FILTER" ]] && echo "filter: $FILTER"
echo

for entry in "${TESTS[@]}"; do
    IFS='|' read -r name dir cmd marker <<<"$entry"

    if [[ -n "$FILTER" && ! "$name" =~ $FILTER ]]; then
        continue
    fi
    if [[ "$SKIP_SPI" == "1" && "$SPI_TESTS" == *" $name "* ]]; then
        echo "--- SKIP $name (needs W25Q64; SKIP_SPI=1)"
        RESULTS+=("SKIP|$name|")
        ((skip++))
        continue
    fi

    log="$LOGDIR/$name.log"
    echo "================================================================"
    echo "--- RUN  $name"
    echo "    cd $dir && SCL_BOARD=weact RUN_MS=$RUN_MS $cmd"
    echo "    log: $log"
    echo "================================================================"

    if [[ "${DRY_RUN:-0}" == "1" ]]; then
        RESULTS+=("DRY|$name|")
        continue
    fi

    killall -s 9 openocd >/dev/null 2>&1 || true
    ( cd "$ROOT/$dir" && SCL_BOARD=weact RUN_MS="$RUN_MS" bash -c "$cmd" ) 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}

    if [[ $rc -eq 0 ]] && grep -qF -- "$marker" "$log"; then
        echo "--- PASS $name"
        RESULTS+=("PASS|$name|$log")
        ((pass++))
    else
        echo "--- FAIL $name (rc=$rc, marker '${marker}' not found)"
        echo "    last lines:"
        tail -12 "$log" | sed 's/^/    | /'
        RESULTS+=("FAIL|$name|$log")
        ((fail++))
    fi

    killall -s 9 openocd >/dev/null 2>&1 || true
    echo "--- gap ${GAP}s"
    sleep "$GAP"
done

echo
echo "================ SUMMARY ================"
for r in "${RESULTS[@]}"; do
    IFS='|' read -r st name log <<<"$r"
    printf '%-5s %-28s %s\n' "$st" "$name" "$log"
done
echo "-----------------------------------------"
echo "PASS=$pass  FAIL=$fail  SKIP=$skip"
echo "logs: $LOGDIR"
[[ $fail -eq 0 ]]
