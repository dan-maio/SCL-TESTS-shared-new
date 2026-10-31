# pico2-pizero-shared

Small examples of the same loaders on this board are in `SCL-EXAMPLES-shared-new/pizero/`.

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
| `scl-fix-xipstatic-*`, `scl-fix2-xipstatic-*` | `scl::xip-static` | groups 1 and 2, `r10` split | flash, data in SRAM or PSRAM |
| `scl-fix-xipnolib-*`, `scl-fix2-xipnolib-*` | `scl::xip-dynamic-nolib` | groups 1 and 2, `r10` split | flash, data in SRAM or PSRAM |

`scl::static` skips the `c1`/`c2`/`cpp3` cycle, as the WeAct static tests do. Overlays are
swapped only at boot: `ovcpp` stays resident and both cores call it.

The `scl-fix4-*` tests load `libcppeh`, `ehx_catch` and `libcppsh`: C++ exceptions, RTTI
and virtual classes.

| Tests | Loader | Throws for real |
|---|---|---|
| `scl-fix4-nolib-sram`, `scl-fix4-nolib-psram` | `scl::dynamic-nolib` | yes |
| `scl-fix4-newlib-sram` | `scl::dynamic-newlib` | yes |
| `scl-fix4-static-sram` | `scl::static` | yes |
| `scl-fix4-reloc-sram` | `scl::reloc` | yes |
| `scl-fix4-optimal-sram` | `scl::optimal` | yes |
| `scl-fix4-xip-flash` | `scl::xip-dynamic-newlib` | yes |
| `scl-fix4-xipstatic-flash` | `scl::xip-static` | yes |
| `scl-fix4-xipnolib-flash` | `scl::xip-dynamic-nolib` | yes |

The throwing tests throw and catch inside `libcppeh`, throw in `libcppeh` and catch in
`ehx_catch` through the cross-call trampoline, and throw in `libcppeh` and catch in main.

The `scl-fix5-*` tests load `mylib1` and `slib`, and add a `TIMER1` alarm interrupt on each
core, every 200 us, that enters the modules with its own `r10` while threads run inside
them. The ISR makes pure calls, and calls `my_systick_handler` under main's lock. They run on
`reloc`, `dynamic-nolib` and `static` (SRAM and PSRAM), `dynamic-newlib` and `optimal` (SRAM),
and the three XIP adapters (flash). The board's vector table has no `TIMER1` entries, so
each core points `VTOR` at a patched RAM copy, as the WeAct test does.

Every fixture test also calls its loader's `first_missing_import()` at boot, and fails if
any import has no provider. The eight `scl-neg-*` tests hide `host_f` from main's table: it
must be reported by name, and `scl::reloc` must refuse the load.

The `scl-fix6-*` tests load this suite's `fixtures/rp2350/` versions of `libled`, `libuart` and
`libspi`, which drive the real peripherals. This board has no W25Q64, so `libspi` uses SPI0's
internal loopback, which needs no wiring. It tests the SPI peripheral, not a flash chip.

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

One full run of all 77 tests passed on the board (2026-10-31), with the loader at `86c83d2`. That loader
has not yet run on the WeAct board.
As a negative control, removing the module's lock made `scl-smp-reloc-sram` fail with
`shared=79163 of 80000`.

The port's `hw.sh` advises one test per power cycle. These runs did not power-cycle
between tests.

## What the board showed

- **A PLT in a flash module reads a GOT in flash,** which the loader cannot patch: the load
  succeeded, and the first import call hung. Either prelink the image before programming it,
  as the `scl-fix*` XIP tests do, or build with `-mlong-calls`, as `smpmod` does, so imports
  go through the RAM GOT via `r10`.
- **`scl::static` ignored `.rel.plt` when `.rel.dyn` was missing.** The load reported
  success with every import unpatched, and the first import call jumped to the PLT
  stub's offset. Libraries from the default linker script have only `.rel.plt`. This is
  also what the `missing relocation/dynsym/dynstr data` lines in the WeAct `static-drv`
  logs were; `static-drv` no longer logs them, and passes at `9f1320f`.
- **C++ exceptions needed five separate fixes on this board.**
  - `--specs=nano.specs` links `libstdc++_nano`, whose `__cxa_throw` has no unwind tables,
    so a throw stops at its first frame. These tests link the full `libsupc++`.
  - The board's linker script folds `.ARM.exidx` into `.text`, where `ld` does not sort it.
    These tests use a derived script.
  - The XIP adapters passed `dl_exidx_register()` its arguments in the wrong order, with a
    count of 0, so their unwind tables were never registered. `xip::stat` had none at all.
  - The extab personality pointer needs its Thumb bit set. In flash that must be done
    before programming, so provisioning now does it.
  - A catch table in flash reads the typeinfo's flash copy, which was never relocated.
    Pass `flash_base` to `patch_main_imports()` or `bind_host_imports()` to prelink it.
- **Exceptions are not safe on two cores at once.** libstdc++ here is `Thread model: single`,
  with one global exception state, so these tests throw under main's lock.
- **`scl::static` had three more bugs.** `R_ARM_ABS32` dropped the addend, so a typeinfo's
  vptr was 8 bytes off (applied once only since `b05616a`, because `patch_shlib()` can run twice). `R_ARM_REL32` was overwritten with an absolute address. Exports
  with no relocation in their own library were never published.
- **`scl::reloc` rejected exception-enabled objects.** It now accepts `R_ARM_PREL31`,
  `R_ARM_TARGET2` and `R_ARM_NONE`, and builds one sorted unwind table at load, because a
  partial link leaves many unsorted ones.
- **`R_ARM_REL32` was wrong in `scl::static`, `scl::dynamic-newlib` and `scl::optimal`.** It
  needs `S + A - P`. These loaders now also register unwind tables.
- **`scl::optimal` added a word's value to `R_ARM_JUMP_SLOT`,** which holds a PLT stub address.
- **`scl::xip-static` and `scl::xip-dynamic-nolib` needed a PIE main.** Both now take
  `set_symtab()`. `xip::stat` binds main's imports into the image at provision time, so
  only calls between modules do a lookup: 48000 lock acquisitions against 264000 for
  `xip::nolib` in the same test.
- **Load after static constructors.** SCL's globals (`g_main_ctx`, `g_handles`, XIP's
  `g_ctx`) are C++ objects. A load from an earlier constructor is wiped when they are
  constructed, and the failure only shows at the first call-time lookup. These tests
  load from `__wrap_main()`.
- **Eager binding broke cross-call.** `dl_set_lazy_binding(false)` also skipped recording
  the `.rel.dyn` table the cross-call router reads, so those slots stayed empty. Fixed
  in `20c30b3`.
- **A missing import used to load silently and fault when called** (`puts`, from a `printf`
  GCC rewrote). Each loader now has `first_missing_import()`, to call after loading the
  set, and call-time lookups log the name. Loading itself still succeeds, because a later
  module may provide the symbol, so callers must make the check.
- **Writing flash slows it.** After the bootrom write, flash stays in serial read mode
  until reconfigured: `scl-fix-xip-flash` takes about 80 s.
- **`dl_set_symtab()` did not link on non-PIE firmware**, because `dl.cpp` still
  references `from_self()`. `_DYNAMIC` is now weak (SCL-shared-new `36ac5e6`).
- **RP2350 has no global exclusive monitor**, so `LDREX`/`STREX` in a module do not
  protect data shared across cores. This module uses an SIO spinlock in main.

## Not tested

- Loading or unloading after SMP has started, which this design excludes.
- The original STM32 driver fixtures, and any external SPI device.
- Real throws from PSRAM under `scl::dynamic-newlib`, `scl::static`, `scl::reloc` and `scl::optimal`.
- Exceptions thrown on both cores at the same moment.
- Fixture functions that write their own state without a lock, on two cores at once.
- Module code that blocks or creates threads, or runs in an ISR.
- Long soak runs: each run lasts seconds.
