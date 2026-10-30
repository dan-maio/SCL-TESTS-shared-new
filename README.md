# SCL-TESTS-shared-new

Test benches for **SCL-shared-new** (the proposals fork with boot-time symbol
tables, RTOS/multi-core hooks, cache hooks and eager binding).

This is a copy of `SCL-TESTS-shared` with one structural change: the
`SCL-shared/` submodule now points at the local bare remote
`/home/dan/Downloads/GIT/SCL-shared-new.git`, not the upstream
`SCL-shared.git`.

```text
SCL-TESTS-shared-new/
  SCL-shared/          submodule -> /home/dan/Downloads/GIT/SCL-shared-new.git
  dynamic-shared/      POSIX dlopen (newlib), nolib XIP, fixtures
  static-shared/       scl::static + XIP + no-PIC + SPI-flash
  relocatable-shared/  ET_REL loader, load_set, cyclic cluster
  optimal-shared/      fixed-window XIP, generated import surface
  overlay-shared/      linker overlay manager
  docs/                cross-cutting guides and PDFs
```

Each suite's `include` and `src` symlinks resolve into `SCL-shared/`, so the
suites compile the new loader sources with their own flags.

## QEMU status (verified)

| Suite | Command | Result |
|---|---|---|
| `optimal-shared` (XIP) | `./build-and-run.sh xip qemu` | **PASS** |
| `optimal-shared` (RAM) | `./build-and-run.sh ram qemu` | **PASS** |
| `relocatable-shared` | `./test-relocatable/build-and-run.sh qemu` | **PASS** |
| `dynamic-shared` (newlib POSIX `dlopen`) | `./test-dynamic/build-and-run.sh qemu test1` | **PASS** |

The dynamic test still uses the retained build-time `gen-symtab` flow, which
confirms the host tools and the new loader sources work together.

## WeAct F411CE status (verified)

All tests pass on the WeAct F411CE with the W25Q64 fitted (2026-10-30),
using `RUN_MS=180000`:

| Group | Tests | Result |
|---|---|---|
| optimal | `optimal-xip`, `optimal-ram` | PASS |
| relocatable | `relocatable` | PASS |
| dynamic | `dynamic-dlopen`, `dynamic-xip-nolib`, `dynamic-xip-newlib` | PASS |
| overlay | `overlay` | PASS |
| static | `static`, `static-cpp`, `static-direct`, `static-drv` | PASS |
| XIP static | `xip-static-semihosting`, `xip-static-direct`, `xip-static-no-pic`, `xip-static-no-pic-simple`, `xip-interrupt-static`, `xip-interrupt-static-direct`, `xip-spi-flash` | PASS |

The runner treats a test as PASS when its completion marker appears in the log;
OpenOCD returns 1 after a clean `shutdown` on this probe. `xip-spi-flash` and
`static-drv` exercise the W25Q64.

## Run

```sh
# all WeAct tests, sequentially, with a 3-second gap
./run-all-weact.sh

# only a subset (regex on the test name)
./run-all-weact.sh static
./run-all-weact.sh 'xip-'

# skip the two tests that need the W25Q64 SPI flash
SKIP_SPI=1 ./run-all-weact.sh

# print commands without flashing
DRY_RUN=1 ./run-all-weact.sh
```

`run-all-weact.sh` streams the OpenOCD semihosting console live, logs each test
under `logs/weact-<timestamp>/`, continues after failures, and prints a
PASS/FAIL summary at the end.

Tunables: `RUN_MS` (default 180000 ms per firmware), `WALL_MS` (default
600000 ms hard cap per test including build), `GAP` (default 3 s), `SKIP_SPI`,
`LOGDIR`. PASS is decided by the test's completion marker, because OpenOCD
returns 1 after a clean `shutdown` on this probe.

Individual suites:

```sh
# optimal: XIP or RAM
cd optimal-shared && ./build-and-run.sh xip weact

# relocatable
cd relocatable-shared && ./test-relocatable/build-and-run.sh weact

# dynamic POSIX dlopen (hosted toolchain)
cd dynamic-shared && ./test-dynamic/build-and-run.sh weact test1
```

Prerequisites: `arm-none-eabi-gcc/g++` (xPack is auto-detected by
`tools-config.sh`) and `qemu-system-arm`.

## Updating the loader

The submodule is a local path; after changing `SCL-shared-new`:

```sh
cd SCL-shared && git pull origin master
cd .. && git add SCL-shared && git commit -m "bump SCL-shared-new"
```

## What is exercised

- `scl::common` now includes `symtab-runtime.cpp`, `port/os-hooks.c` and
  `port/cache.c`; all suites link them.
- Reloc/optimal/static/XIP load paths now take the lock hooks and cache calls
  (weak no-ops on bare metal).
- The dynamic-newlib test exercises the retained `gen-symtab` + `.scl_symtab`
  path; the boot-time/semihosting alternatives are documented in
  `SCL-shared/docs/BOOT-SYMTAB.md`.
- `scl::static` now has `symbol_table::add_entries()` and cache maintenance
  before constructors.
- `dynamic-nolib` has eager-binding (`dl_set_lazy_binding`) and external-table
  (`dl_set_symtab`) APIs; the existing tests run with defaults unchanged.

## Notes

- All suites above have now been executed on the WeAct board. The NUCLEO board
  has not been validated.
- `scl::static` standalone targets now link `src/port/os-hooks.c` and
  `src/port/cache.c` (SCL-shared-new commit `1fb1680`); without that, static-only
  firmware failed to link `scl_loader_lock/unlock`.
- The SPI tests (`static-drv`, `xip-spi-flash`) require the W25Q64; both passed
  with it fitted.
