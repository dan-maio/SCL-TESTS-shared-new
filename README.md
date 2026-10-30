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
  api-shared/          on-target tests of the new APIs
  pico2-pizero-shared/ SMP loader tests on the Pi-Zero RP2350B (µOS++ SMP)
  dynamic-shared/      POSIX dlopen (newlib), nolib XIP, fixtures
  static-shared/       scl::static + XIP + no-PIC + SPI-flash
  relocatable-shared/  ET_REL loader, load_set, cyclic cluster
  optimal-shared/      fixed-window XIP, generated import surface
  overlay-shared/      linker overlay manager
  docs/                cross-cutting guides and PDFs
```

Each suite's `include` and `src` symlinks resolve into `SCL-shared/`, so the
suites compile the new loader sources with their own flags.

## QEMU status (recorded)

Results recorded when the suites were wired to SCL-shared-new, and not re-run on
the pinned commit. Only these four
commands have QEMU results; `overlay-shared` and `static-shared` have none.

| Suite | Command | Result |
|---|---|---|
| `optimal-shared` (XIP) | `./build-and-run.sh xip qemu` | **PASS** |
| `optimal-shared` (RAM) | `./build-and-run.sh ram qemu` | **PASS** |
| `relocatable-shared` | `./test-relocatable/build-and-run.sh qemu` | **PASS** |
| `dynamic-shared` (newlib POSIX `dlopen`) | `./test-dynamic/build-and-run.sh qemu test1` | **PASS** |

The dynamic test still uses the retained build-time `gen-symtab` flow, which
confirms the host tools and the new loader sources work together.

## Status by board

| Board | Suite | Tests | Result (2026-10-30) | Loader |
|---|---|---|---|---|
| WeAct F411CE + W25Q64 | suites + `api-shared/` | 22 | all pass | `380161e`, 5 re-run at `b05616a` |
| Pi-Zero RP2350B + PSRAM | `pico2-pizero-shared/` | 77 | all pass, in one run | `9f1320f` |

The WeAct board has not run since `9f1320f`, which changed `scl::static`, `scl::reloc`,
`scl::optimal` and `scl::dynamic-newlib`. The Pi-Zero's 77 tests pass at that commit.
The NUCLEO board has not been validated.

## WeAct F411CE status (verified)

One `./run-all-weact.sh` run from clean builds, with `RUN_MS=180000`, at `380161e`: 21 of 22
passed. `static-cpp` failed because `patch_shlib()` runs twice in that test and an addend
was applied twice. After the fix (`b05616a`), `static`, `static-cpp`, `static-direct`,
`static-drv` and `api-static-boot-table` were re-run and passed.

| Group | Tests | Result |
|---|---|---|
| optimal | `optimal-xip`, `optimal-ram` | PASS |
| relocatable | `relocatable` | PASS |
| dynamic | `dynamic-dlopen`, `dynamic-xip-nolib`, `dynamic-xip-newlib` | PASS |
| overlay | `overlay` | PASS |
| static | `static`, `static-cpp`, `static-direct`, `static-drv` | PASS |
| XIP static | `xip-static-semihosting`, `xip-static-direct`, `xip-static-no-pic`, `xip-static-no-pic-simple`, `xip-interrupt-static`, `xip-interrupt-static-direct`, `xip-spi-flash` | PASS |
| new APIs | `api-static-boot-table`, `api-optimal-boot-table`, `api-dynamic-xip-semihost-table`, `api-nolib-eager` | PASS |

A test passes when its completion marker appears and no failure pattern
(`RESULT: FAILURE`, `FAIL:`, `HardFault`, `LOAD FAILED`) does. OpenOCD's exit code is
ignored, because it returns 1 after a clean `shutdown` on this probe.
`xip-spi-flash` and `static-drv` exercise the W25Q64.

A marker shows the firmware reached its end; only the `api-*` tests check all their own
values. The `load_bin ... buffer has` error in `static`, `static-cpp` and `static-direct`
comes from a bounds test that expects it. `static-drv` no longer logs
`missing relocation/dynsym/dynstr data`: its libraries had only `.rel.plt`, which the loader
was skipping, leaving their imports unpatched.

## Pi-Zero RP2350B status (verified)

All 77 SMP loader tests, 69 of them using the shared fixtures, passed on the board in one
full run. They cover every loader, in SRAM, PSRAM and flash as each allows, C++
exceptions, per-core interrupts and missing-import reporting. See
`pico2-pizero-shared/README.md`, and `docs/LOADER-CHOICE.md` for what it means when choosing.

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

Prerequisites: `arm-none-eabi-gcc/g++` and `openocd` (each suite's
`tools-config.sh` falls back to the xPack paths under
`/home/dan/.local/xPacks`), plus `qemu-system-arm` for the QEMU commands.
`run-all-weact.sh` has its own toolchain fallback, because the repository root
has no `tools-config.sh`.

## Cloning

```sh
git clone /home/dan/Downloads/GIT/SCL-TESTS-shared-new.git
cd SCL-TESTS-shared-new && git submodule update --init
```

The tree is not self-contained, so it only builds as-is on this machine:

- Seven tracked symlinks point at absolute paths under
  `/home/dan/Documents/shared-loaders-weact_agy`
  (`*/test/fixtures/fixtures`, the `libsh1`, `libsh2` and `new-lib` links under
  `static-shared/test-static`, and
  `test-xip-static-direct/src/sections-flatfs-hosted.ld`).
- `.gitmodules`, the toolchain files and the debug scripts use absolute paths
  under `/home/dan`.
- `*/test/cmake/toolchains/arm-none-eabi-gcc.cmake` is a broken symlink in the
  `dynamic`, `optimal`, `overlay` and `static` suites.

## Updating the loader

The submodule is a local path; after changing `SCL-shared-new`:

```sh
cd SCL-shared && git pull origin master
cd .. && git add SCL-shared && git commit -m "bump SCL-shared-new"
```

## What is exercised

- `scl::common` now includes `symtab-runtime.cpp`, `port/os-hooks.c` and
  `port/cache.c`, so suites linking it build them. `scl::static` on its own
  links those three too.
- The original tests run through the new lock and cache calls with their weak
  defaults: the lock hooks are no-ops and the cache hooks are barriers only. Every
  loader now makes both calls.
- The dynamic-newlib test exercises the retained `gen-symtab` + `.scl_symtab`
  path; the boot-time/semihosting alternatives are documented in
  `SCL-shared/docs/BOOT-SYMTAB.md`.

## New APIs

`api-shared/` calls the new APIs on the board: the symbol-table functions,
`SCL_REGISTER_SYM`, `add_entries`, `dl_set_symtab`, `dl_set_lazy_binding`,
`set_main_symbols`, `scl_publish_release`, and the lock and cache hooks. See
`api-shared/README.md`.

## What is not exercised

- On the WeAct board the hook tests only count calls, on one core with no data
  cache. `pico2-pizero-shared/` covers two cores, an RTOS and XIP cache maintenance.
- `scl_publish_release()` is only called once, with nothing observing it.
- Loading a sidecar ELF from flash is untested; the tests read over semihosting.

## Notes

- `scl::common` and `scl::static` changed behaviour since `SCL-shared`: see
  `SCL-shared/docs/IMPLEMENTED-PROPOSALS.md`.
- `scl::static` standalone targets now link `src/port/os-hooks.c`,
  `src/port/cache.c` and `src/symtab-runtime.cpp`.
- The SPI tests (`static-drv`, `xip-spi-flash`) require the W25Q64; both passed
  with it fitted.
