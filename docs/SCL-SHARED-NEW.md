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

every firmware in these suites links those objects. On bare-metal/QEMU the
hooks are no-ops; the code paths remain unchanged unless the application
overrides them.

## QEMU validation performed

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

All 18 tests pass on the WeAct F411CE with the W25Q64 fitted (2026-10-30),
using `run-all-weact.sh` (`RUN_MS=180000`):

| Group | Result |
|---|---|
| optimal XIP / RAM | PASS |
| relocatable | PASS |
| dynamic POSIX dlopen | PASS |
| dynamic XIP nolib / newlib | PASS |
| overlay | PASS |
| static / static-cpp / static-direct / static-drv | PASS |
| XIP semihosting / direct / no-PIC / no-PIC-simple / interrupts / SPI-flash | PASS |

Two runner/documentation fixes came out of the hardware run:

- PASS is decided by the completion marker, not the process exit code: OpenOCD
  returns 1 after a clean `shutdown` on the WCH-Link probe.
- `static` / `static-cpp` print `Second 1` (the semihosting argv makes
  `atoi(argv[1])` zero the blink loop), and `xip-spi-flash` ends with
  `========== DONE ==========`.
- The hardware tests take longer than the old 60 s window; the default is now
  180 s per firmware.

## Boot-time / no-tool paths

These suites do not yet exercise the new sidecar/semihosting symbol paths. To
test them:

1. Replace `scl_embed_symtab(target)` with a boot-time table load.
2. Read `firmware.elf` over semihosting and call
   `scl::symtab::collect_buffered()` + `ctx.set_symtab(...)`, or use the
   compile-time `SCL_REGISTER_SYM` table.
3. Remove the `.scl_symtab` placeholder/`objcopy` step.

`SCL-shared-new/docs/BOOT-SYMTAB.md` contains the code; the existing
`dl-impl.cpp` fallback is the reference implementation.

## Updating the loader

```sh
cd SCL-shared
git pull origin master
cd ..
git add SCL-shared
git commit -m "bump SCL-shared-new"
```

## Next steps

- NUCLEO-F411RE hardware validation (the suites support `SCL_BOARD=nucleo`).
- Add a slot in `dynamic-shared` that loads `main`'s table at boot (semihosting
  or sidecar) and runs with `gen-symtab` disabled.
- Add an RTOS/SMP smoke app that defines strong `scl_loader_lock/unlock` and
  calls `dl_set_lazy_binding(false)`.
