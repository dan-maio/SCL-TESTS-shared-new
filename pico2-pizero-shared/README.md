# pico2-pizero-shared

SCL on the Pi-Zero RP2350B (2x Cortex-M33, 16 MB flash, 8 MB QSPI PSRAM) under the
µOS++ III SMP kernel. It tests one rule:

> Every load and patch happens on core 0, at boot. Only after the module is loaded
> does the SMP system start, and from then on both cores only call the module.

## Sequence

| Step | Where | What runs |
|---|---|---|
| 1 | constructor 101 (`rtos-glue.cpp`) | clocks, UART, heap |
| 2 | constructor 150 (`boot_load()`) | PSRAM init, module load and patch, on core 0 with core 1 in the bootrom |
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

## Run

```sh
cmake -S . -B build
cmake --build build --target scl-smp-all
./run-all-pizero.sh                 # all tests
./run-all-pizero.sh psram           # a subset
POWER_CYCLE=1 ./run-all-pizero.sh   # wait for a power cycle between tests
```

Board support comes from the port's working copy; set `-DUOS_PORT_DIR=...` if it
moves. The tests use its linker script and its `test/boards/pico2-pizero/hw.sh`.
Per-check results arrive over semihosting and are in `logs/*/<test>.log`. The runner
never opens the UART, so it stays free for a terminal; the firmware prints the same
results there.

## Status

Three consecutive runs of all eight tests passed on the board (2026-10-30), against
SCL-shared-new `e60801e`. Both free workers ran on both cores in the runs inspected.
As a negative control, removing the module's lock made `scl-smp-reloc-sram` fail with
`shared=79163 of 80000`.

The port's `hw.sh` advises one test per power cycle. These runs did not power-cycle
between tests.

## What the board showed

- **A flash module's imports must not use a PLT.** A PLT reads a GOT in flash, which
  the loader cannot patch: the load succeeded, and the first import call hung. Build
  the PIC module with `-mlong-calls`, so imports go through the RAM GOT via `r10`.
- **`dl_set_symtab()` did not link on non-PIE firmware**, because `dl.cpp` still
  references `from_self()`. `_DYNAMIC` is now weak (SCL-shared-new `36ac5e6`).
- **RP2350 has no global exclusive monitor**, so `LDREX`/`STREX` in a module do not
  protect data shared across cores. This module uses an SIO spinlock in main.

## Not tested

- Loading or unloading after SMP has started, which this design excludes.
- `scl::static`, `scl::optimal`, `scl::overlay`, and `xip::static`.
- Module code that blocks or creates threads, or runs in an ISR.
- Long soak runs: each run lasts seconds.
