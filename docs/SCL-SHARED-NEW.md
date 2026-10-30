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

All 18 tests have a passing log on the WeAct F411CE with the W25Q64 fitted
(2026-10-30), using `run-all-weact.sh` (`RUN_MS=180000`). The logs come from 15
partial sessions, many before `1fb1680`, not one full run; the root `README.md`
describes the limits of this evidence.

| Group | Result |
|---|---|
| optimal XIP / RAM | PASS |
| relocatable | PASS |
| dynamic POSIX dlopen | PASS |
| dynamic XIP nolib / newlib | PASS |
| overlay | PASS |
| static / static-cpp / static-direct / static-drv | PASS |
| XIP semihosting / direct / no-PIC / no-PIC-simple / interrupts / SPI-flash | PASS |

Three runner/documentation fixes came out of the hardware run:

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

`SCL-shared/docs/BOOT-SYMTAB.md` contains the code; the existing fallback in
`dynamic-shared/test/platforms/board-hosted/vendor/firmware/src/dl-impl.cpp` is
the reference implementation. The §1 snippet in `BOOT-SYMTAB.md` also needs
`#include "loader/sources/memory-byte-source.hpp"` and
`#include "dynamic-nolib/dl.hpp"` to compile.

## Known issues in SCL-shared-new (`1fb1680`)

Found by reviewing the code against its docs. They are recorded here so test work
can target them; none are fixed in the pinned commit.

| Issue | Effect |
|---|---|
| `scl::static` does not link `symtab-runtime.cpp` | `scl::symtab::registered()` gives an undefined reference in static-only firmware, and `collect()` lives in the same file. `add_entries()` itself is available. |
| The M7 example in `RTOS-MULTICORE.md` §3 overrides only `scl_cache_clean` and `scl_cache_invalidate` | RAM loaders call `scl_cache_clean_invalidate()`, whose default is only a barrier, so following the example performs no cache maintenance for them. |
| RAM `dynamic-newlib` has no lock calls | Only its XIP adapter is guarded, although the docs describe lock guards on every loader. |
| Constructors run under the lock | `dlopen_source`, reloc `load` and `load_set` hold the guard during constructors; the docs say the hooks are not held while module code runs. |
| `scl_overlay_load` | The `resident` check is outside the lock, and there is no cache maintenance after the copy into the window. |
| `tests/run.sh` in a clean clone | Fails at `open ELF`: the default `411ce-100Mhz.elf` is gitignored. Pass another ARM ELF with `ELF=...`. |
| `sidecar-elf-example.ld` | The `SIDECAR` region ends at `0x080A0000`, past the end of the F411's 512K flash at `0x08080000`. |
| `SCL-shared/docs/manual.md` | Unchanged from the upstream repository, so it describes the submodule layout and omits the new APIs. |

## Updating the loader

```sh
cd SCL-shared
git pull origin master
cd ..
git add SCL-shared
git commit -m "bump SCL-shared-new"
```

## Next steps

- One complete `run-all-weact.sh` run against the pinned commit.
- Fix the loader issues above in SCL-shared-new, then bump the submodule.
- NUCLEO-F411RE hardware validation (the suites support `SCL_BOARD=nucleo`).
- Add a slot in `dynamic-shared` that loads `main`'s table at boot (semihosting
  or sidecar) and runs with `gen-symtab` disabled.
- Add an RTOS/SMP smoke app that defines strong `scl_loader_lock/unlock` and
  calls `dl_set_lazy_binding(false)`.
