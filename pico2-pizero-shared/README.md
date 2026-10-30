# pico2-pizero-shared

SCL on the Pi-Zero RP2350B (2x Cortex-M33, 16 MB flash, 8 MB QSPI PSRAM) under the
µOS++ III SMP kernel. It tests one rule:

> Every load and patch happens on core 0, at boot. Only after the module is loaded
> does the SMP system start, and from then on both cores only call the module.

## Sequence

| Step | Where | What runs |
|---|---|---|
| 1 | constructor 101 (`rtos-glue.cpp`), then the other static constructors | clocks, UART, heap, SCL's globals |
| 2 | `__wrap_main()` (`boot_load()`) | PSRAM init, module load and patch, on core 0 with core 1 in the bootrom |
| 3 | same | `scl_publish_release()`, then `g_published = 1` |
| 4 | kernel `main()`, then `os_main()` | checks the load and creates the threads |
| 5 | `os_main()` | `multicore::launch_core1()`; SMP is running |
| 6 | workers | two pinned to a core and two free to migrate, calling the module |

## Tests

| Test | Loader | Module text runs from | Module data | Iterations per worker |
|---|---|---|---|---|
| `scl-smp-reloc-sram` | `scl::reloc` | SRAM | SRAM | 20000 |
| `scl-smp-reloc-psram` | `scl::reloc` | PSRAM | PSRAM | 5000 |
| `scl-smp-nolib-sram` | `scl::dynamic-nolib`, eager | SRAM | SRAM | 20000 |
| `scl-smp-nolib-psram` | `scl::dynamic-nolib`, eager | PSRAM | PSRAM | 5000 |
| `scl-smp-xip-flash` | `scl::xip-dynamic-newlib` | flash (XIP) | SRAM | 20000 |
| `scl-smp-xip-flash-psram` | `scl::xip-dynamic-newlib` | flash (XIP) | PSRAM | 5000 |
| `scl-smp-reloc-psram-big` | `scl::reloc` | PSRAM | PSRAM | 200 |
| `scl-smp-nolib-psram-big` | `scl::dynamic-nolib`, eager | PSRAM | PSRAM | 200 |

The `-big` modules hold a 64 KB read-only table and 64 KB of writable data, more than
the RP2350's 16 KB XIP cache. Each worker walks the table and writes and verifies its
own 16 KB slot, so accesses miss and dirty lines are evicted while both cores run.

Each test checks that:

- the load ran on core 0, before the kernel, and took the loader lock;
- module text is in the expected memory;
- no loader lock was taken after publication or off core 0;
- `mod_compute` and `mod_fmix` (FPU) return correct values on both cores;
- no increment of the module's shared counter was lost, and both cores called it;
- pinned workers stayed on their core, and the loader made its cache calls.

In the tests with PSRAM `scl_cache_clean` and `scl_cache_invalidate` do real RP2350 XIP
cache maintenance.

## Tests with the WeAct fixtures

These run the fixtures the WeAct suites use: `cpp3`, `c2` and `c1`, which import from
each other, plus `mylib1`. As on the WeAct board they are read from the host over
semihosting at boot; for XIP they are patched and written to flash at the 8 MB offset
through the bootrom, then loaded from there.

| Test | Loader | Code runs from | Data | Iterations per worker |
|---|---|---|---|---|
| `scl-fix-reloc-sram` | `scl::reloc`, `load_set` | SRAM | SRAM | 2000 |
| `scl-fix-reloc-psram` | `scl::reloc`, `load_set` | PSRAM | PSRAM | 500 |
| `scl-fix-nolib-sram` | `scl::dynamic-nolib`, eager, cross-call | SRAM | SRAM | 2000 |
| `scl-fix-nolib-psram` | `scl::dynamic-nolib`, eager, cross-call | PSRAM | PSRAM | 500 |
| `scl-fix-xip-flash` | `scl::xip-dynamic-newlib`, cross-call | flash (XIP) | SRAM | 2000 |
| `scl-fix-xip-flash-psram` | `scl::xip-dynamic-newlib`, cross-call | flash (XIP) | PSRAM | 500 |

Each iteration runs `cpp3_method` (libm through main), `c2_param`, `c1_use_fm`,
`library_function` and `cpp3_heap` (main's heap) concurrently. It then runs `c2_add`,
`cpp3_float_cross`, `cpp3_recurse` and `c1_call_c2` under main's lock, because those
bump fixture counters that have no lock of their own, and checks the counts at the end.

Under the `r10` models, calls between modules and into main go through a trampoline
whose lookup is loader code, run at call time on both cores: 264000 lock acquisitions
in the 2000-iteration tests. Those tests therefore expect call-time locking, and need
a real recursive loader lock, which `fixtures-main.cpp` provides. `scl::reloc` takes
no lock after publication.

The `scl-fix2-*` tests run the same loaders and memories on `libprov`, `libcons` and
`mylib2` (5000 or 1000 iterations for reloc and nolib; 2000 or 500 for XIP).

More loaders on the same fixtures, all read over semihosting at boot:

| Tests | Loader | Fixtures | Code runs from |
|---|---|---|---|
| `scl-fix-newlib-sram`, `scl-fix2-newlib-sram` | `scl::dynamic-newlib` | groups 1 and 2, plain `-fPIC` | SRAM (heap) |
| `scl-fix3-static-sram`, `scl-fix3-static-psram` | `scl::static` | `libprov`, `libcons`, `mylib1`, `mylib2`, plain `-fPIC` | SRAM, PSRAM |
| `scl-fix-optimal-*`, `scl-fix2-optimal-*` | `scl::optimal` | groups 1 and 2, `-fno-pic` at fixed addresses | SRAM, PSRAM windows |
| `scl-fix-overlay-sram`, `scl-fix-overlay-psram` | `scl::overlay` | `ov1`, `ov2`, `ovcpp`, linked with the firmware | SRAM, PSRAM window |

`scl::static` skips the `c1`/`c2`/`cpp3` cycle, as the WeAct static tests do. Overlays are
swapped only at boot: `ovcpp` stays resident and both cores call it.

Each fixture test first prints every fixture call with its traces, on core 0 and then
on core 1, like the WeAct apps. Traces are off for the stress loop that follows.

## Run

```sh
cmake -S . -B build
cmake --build build --target scl-smp-all scl-fix-all
./run-all-pizero.sh                 # build, then all tests, live, 3 s apart
./run-all-pizero.sh psram           # a subset
GAP=5 ./run-all-pizero.sh           # a different gap
POWER_CYCLE=1 ./run-all-pizero.sh   # wait for a power cycle between tests
```

Board support comes from the port's working copy; set `-DUOS_PORT_DIR=...` if it
moves. The tests use its linker script and its `test/boards/pico2-pizero/hw.sh`.
Per-check results arrive over semihosting and are in `logs/*/<test>.log`. The runner
never opens the UART, so it stays free for a terminal; the firmware prints the same
results there.

## Status

One full run of all 30 tests passed on the board (2026-10-30), against SCL-shared-new
`20c30b3` plus the `elf-reader.cpp` fix below, committed as `9696b85`. Both free workers ran on both cores in
almost every test; `scl-fix3-static-psram` once kept one on core 0.
As a negative control, removing the module's lock made `scl-smp-reloc-sram` fail with
`shared=79163 of 80000`.

The port's `hw.sh` advises one test per power cycle. These runs did not power-cycle
between tests.

## What the board showed

- **A flash module's imports must not use a PLT.** A PLT reads a GOT in flash, which
  the loader cannot patch: the load succeeded, and the first import call hung. Build
  the PIC module with `-mlong-calls`, so imports go through the RAM GOT via `r10`.
- **`scl::static` ignored `.rel.plt` when `.rel.dyn` was missing.** The load reported
  success with every import unpatched, and the first import call jumped to the PLT
  stub's offset. Libraries from the default linker script have only `.rel.plt`. This is
  also what the `missing relocation/dynsym/dynstr data` lines in the WeAct `static-drv`
  logs were, so that test's behaviour changes with the fix and needs a re-run.
- **Load after static constructors.** SCL's globals (`g_main_ctx`, `g_handles`, XIP's
  `g_ctx`) are C++ objects. A load from an earlier constructor is wiped when they are
  constructed, and the failure only shows at the first call-time lookup. These tests
  load from `__wrap_main()`.
- **Eager binding broke cross-call.** `dl_set_lazy_binding(false)` also skipped recording
  the `.rel.dyn` table the cross-call router reads, so those slots stayed empty. Fixed
  in `20c30b3`.
- **A missing import is not reported under cross-call.** A symbol absent from main's
  table (`puts`, from a `printf` GCC rewrote) loaded without error and faulted at
  `PC=0` when called. This is not fixed.
- **Writing flash slows it.** After the bootrom write, flash stays in serial read mode
  until reconfigured: `scl-fix-xip-flash` takes about 80 s.
- **`dl_set_symtab()` did not link on non-PIE firmware**, because `dl.cpp` still
  references `from_self()`. `_DYNAMIC` is now weak (SCL-shared-new `36ac5e6`).
- **RP2350 has no global exclusive monitor**, so `LDREX`/`STREX` in a module do not
  protect data shared across cores. This module uses an SIO spinlock in main.

## Not tested

- Loading or unloading after SMP has started, which this design excludes.
- `scl::xip-static` and `scl::xip-dynamic-nolib`.
- The C++ exception fixtures (`ehx_catch`, `libcppeh`, `libcppsh`), and the STM32 driver
  fixtures (`libspi`, `libled`, `libuart`).
- Fixture functions that write their own state without a lock, on two cores at once.
- Module code that blocks or creates threads, or runs in an ISR.
- Long soak runs: each run lasts seconds.
