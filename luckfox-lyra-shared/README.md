# luckfox-lyra-shared

SCL on the Luckfox Lyra B: RK3506, 3x Cortex-A7, under the µOS++ III SMP kernel. Like
`pico2-pizero-shared/`, it tests one rule: load and patch on core 0, at boot, and only then start SMP.

## How this board differs

| | Pi-Zero RP2350B | Luckfox Lyra B |
|---|---|---|
| CPU | 2x Cortex-M33, Thumb only | 3x Cortex-A7; main is ARM, the fixtures Thumb |
| Code runs from | Flash, SRAM, PSRAM | DRAM only |
| Caches | XIP cache | L1 instruction and data caches, which the loader's hooks must maintain |
| Storage | QSPI flash | SD card, in place of SPI flash |
| Interrupt in `scl-fix5` | A TIMER1 alarm | An SGI each core sends itself |
| Between tests | Power cycle, or as `hw.sh` allows | `hw.sh` resets the board itself |

The board boots from its internal SPI firmware, which apps do not use. So there is no flash to run
from or write to, and that has three effects:

- **`xip-*` tests run in place from DRAM:** the image is prelinked, then copied into a region used as
  read-only. This tests the split between code and data, not real flash.
- **`libsd` replaces `libspi`:** it writes a pattern and reads it back through imports from main.
- **The SD card is not reformatted:** everything happens in one file, `tests/sclsd.bin`, on its FAT volume.

## The tests

| Group | Fixtures | Tests | What it adds |
|---|---|---|---|
| `scl-fix-*` | `cpp3`, `c2`, `c1`, `mylib1` | 7 | C++, heap, float, a cycle of imports |
| `scl-fix2-*` | `libprov`, `libcons`, `mylib2` | 7 | One library importing from another |
| `scl-fix3-static` | as `fix2`, plus `mylib1` | 1 | The same on `scl::static`, which has no cyclic imports |
| `scl-fix-overlay` | `ov1`, `ov2`, `ovcpp` | 1 | Overlay swaps at boot |
| `scl-fix5-*` | `mylib1`, `slib` | 8 | Library calls from an interrupt on each core |
| `scl-fix6-*` | `libled`, `libuart`, `libsd` | 8 | The LED, the UART and the SD card |
| `scl-fix4-*` | `libcppeh`, `ehx_catch`, `libcppsh` | 8 | C++ exceptions, RTTI, virtual classes |
| `scl-neg-*` | as `fix5`, with `host_f` hidden | 8 | A missing import must be reported by name |

That is 48 tests. Not carried over from the Pi-Zero: the PSRAM tests, because the board has none, and
the 8 `scl-smp-*` tests, which the fixture tests largely overlap.

## What the port needed

- **`-mword-relocations`** on the `-fno-pic` fixtures. The Cortex-A7 compiler otherwise uses
  `movw`/`movt` for addresses, which the loaders and these links do not handle.
- **Thumb fixtures:** the ET_REL loader handles Thumb relocations, and `blx` switches from ARM main.
- **Cache hooks:** `scl_cache_clean()` and `scl_cache_invalidate()` clean with `DCCMVAU` and invalidate
  with `ICIALLUIS`, which also reaches the other cores.
- **Spinlocks** with `LDREX`/`STREX`, which work here because DRAM is Normal and shareable.
- **A loader fix:** the loaders forced the Thumb bit on code addresses, so calls went into ARM main in
  Thumb state and faulted. `include/scl/code-addr.h` keeps each address's own bit on a Cortex-A.
- **The heap directly, under `app_lock`:** on this port, leaving the kernel's scheduler lock turns
  interrupts on unconditionally, and `malloc` takes that lock. A thread could then be preempted while
  holding `app_lock`, which deadlocked `scl-fix-nolib`. The imported allocator calls therefore use the heap
  resource directly. The kernel is unchanged.
- **`SYS_WRITE` for test output:** OpenOCD reads a `SYS_WRITE0` string one byte at a time, about a second
  per line on this CPU. Everything still goes to the semihosting console.

## Run

```sh
./run-all-lyra.sh          # all 48, with a 3-second gap
./run-all-lyra.sh fix6     # only those whose name matches
```

The output is the semihosting console. The script does not open the UART, so it stays free for a
terminal, where the firmware prints the same results.

A run takes about 1 to 2 minutes per test, because semihosting is slow on this probe. The script sets
the SWD clock to 10 MHz (`KHZ`), which took one test from 133 s to 98 s. The probe is USB HID, whose
latency probably limits speed more than the clock does.

If a test hits its time limit (`SECS`), the log ends with `CMSIS-DAP command mismatch` and similar errors.
That is the shutdown cutting off a running test, not a probe fault.

## Status

The first full run of the 48 is not finished. So far, the 6 non-`newlib` `scl-fix-*` tests have passed.
`scl-fix-newlib` failed only on a wrong expected address in the test, since corrected, and has not been
re-run. Exceptions, interrupts and the other groups are not yet known.
