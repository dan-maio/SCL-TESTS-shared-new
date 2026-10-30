# SCL-shared-new integration

`SCL-TESTS-shared-new` consumes the proposals fork through a git submodule:

```text
path: SCL-shared
url : /home/dan/Downloads/GIT/SCL-shared-new.git
```

This mirrors the upstream layout: the top-level `CMakeLists.txt` re-exports
`scl::*`, and every suite does:

```cmake
if(NOT TARGET scl-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared ...)
endif()
```

The suite-level `include -> ../SCL-shared/include` and
`src -> ../SCL-shared/src` symlinks therefore resolve into the new loader
without per-suite edits.

## What the suites compile

Because `scl::common` in SCL-shared-new now includes:

- `src/symtab-runtime.cpp` (boot-time table builder/view),
- `src/port/os-hooks.c` (weak loader lock hooks),
- `src/port/cache.c` (weak cache/publication hooks),

firmware linking `scl::common` builds those objects. `scl::static` does not link
`scl::common`: since `1fb1680` it adds only the two `port/` files, so standalone
static firmware does not get `symtab-runtime.cpp`.

On bare-metal/QEMU the lock hooks are no-ops and the cache hooks are barriers
only (DSB/ISB), unless the application overrides them.

## QEMU validation performed

Only these four commands have recorded results; `overlay-shared` and
`static-shared` have none.

| Command (from the suite directory) | Result |
|---|---|
| `optimal-shared ./build-and-run.sh xip qemu` | PASS |
| `optimal-shared ./build-and-run.sh ram qemu` | PASS |
| `relocatable-shared ./test-relocatable/build-and-run.sh qemu` | PASS |
| `dynamic-shared ./test-dynamic/build-and-run.sh qemu test1` | PASS |

The dynamic test still uses the retained build-time `gen-symtab` +
`objcopy --update-section` flow, so it also verifies that the host tools remain
compatible with SCL-shared-new.

## WeAct F411CE validation

One `run-all-weact.sh` run from clean builds (`RUN_MS=180000`) at `9f1320f`: all 22 passed.
An earlier run at `380161e` found a double-applied addend in `static-cpp`, fixed in `b05616a`.

| Group | Result |
|---|---|
| optimal XIP / RAM | PASS |
| relocatable | PASS |
| dynamic POSIX dlopen | PASS |
| dynamic XIP nolib / newlib | PASS |
| overlay | PASS |
| static / static-cpp / static-direct / static-drv | PASS |
| XIP semihosting / direct / no-PIC / no-PIC-simple / interrupts / SPI-flash | PASS |
| new APIs (`api-shared/`, four tests) | PASS |

Three runner/documentation fixes came out of the hardware run:

- PASS is decided by the completion marker, not the process exit code: OpenOCD
  returns 1 after a clean `shutdown` on the WCH-Link probe.
- `static` / `static-cpp` print `Second 1` (the semihosting argv makes
  `atoi(argv[1])` zero the blink loop), and `xip-spi-flash` ends with
  `========== DONE ==========`.
- The hardware tests take longer than the old 60 s window; the default is now
  180 s per firmware.

## Boot-time / no-tool paths

`api-shared/` exercises them. `api-dynamic-xip-semihost-table` reads its table over
semihosting with no `gen-symtab` step, from an ELF copy reduced to the imported
symbols by `objcopy`, because the full ELF's `.strtab` (90 KB) does not fit in RAM.
`api-static-boot-table` uses the compile-time `SCL_REGISTER_SYM` table. The other
suites still use `scl_embed_symtab()`.

## Updating the loader

```sh
cd SCL-shared
git pull origin master
cd ..
git add SCL-shared
git commit -m "bump SCL-shared-new"
```

## Pi-Zero RP2350B validation

`pico2-pizero-shared/` runs 77 tests under the µOS++ SMP kernel, all passing in one run at
`9f1320f`: every loader, on the shared fixtures, with C++ exceptions and an interrupt on each
core. It found most of the loader fixes since `ab7737d`. The suite's README lists them.
`SCL-EXAMPLES-shared-new/pizero/` has the WeAct examples and these fixtures on every loader on
this board, and writing flash with both cores running; all 76 firmwares pass at `9f1320f`.

## Next steps

- NUCLEO-F411RE hardware validation (the suites support `SCL_BOARD=nucleo`).
- Cache overrides on a Cortex-M7. `pico2-pizero-shared/` covers SMP and the RP2350's XIP
  cache.
