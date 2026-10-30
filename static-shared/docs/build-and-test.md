# Build & Test — static loader family

The one build/test recipe for the whole SCL. Run everything from the **repo
root**; the SCL content *is* the root, with the `common-elf-reader` parser
pulled in as the **git submodule** at `./common-elf-reader` (`elf32_read_parse.git`,
branch `static-shared`) — everything else is local. Three targets are supported:
**WEACT-F411CE**, **NUCLEO-F411RE**, and **QEMU** (emulated STM32F411RE). Only
board boot/init differs (`board=weact|nucleo|qemu`).

## The modules and where their tests live

| Module (target) | Code | Tested by | Hardware? |
|---|---|---|---|
| `scl::common` (core parser/resolver) | `src/`, `include/loader/` (+ submodule) | every firmware suite | yes |
| `scl::static` (ELF32 static patcher) | `src/static/` | `test-static/test-static` → `f411ce-sh` (§2) | board |
| `scl-xip-common` (XIP shared core) | `src/xip/` | XIP tests (§4) | board |
| `scl-xip-static` | `src/xip/static/adapter.cpp` | `test-xip/` → `static/*` (§4) | board |
| `xip::nopic` (no alias)† | `src/xip/static/nopic.cpp` | `test-xip/` → `static-no-pic*` (§4) | board |

> † Unlike `scl-xip-common`/`scl-xip-static` above, there is **no
> `scl-xip-nopic` CMake target**. The no-pic loader is a single source
> (`src/xip/static/nopic.cpp`) that the `static-no-pic*` apps compile in directly
> alongside `scl::xip-common`.

**What needs hardware:** every suite is a firmware pass/fail
gate. §2 is a **compile check** without a board; the actual on-target runs are
§3 (and `test-static/test-static`'s own run scripts in §2).

## 1. Setup

```sh
# ARM toolchain on PATH (the vendored fixture Makefiles call objdump directly)
export PATH=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin:$PATH

# optional: full clean (no cleaner.sh here — remove the build trees + fixture output)
rm -rf test-xip/build test-static/*/build \
       test-xip/test-xip-static-no-pic/fixtures \
       test-xip/test-xip-static-no-pic-simple/fixtures
```

> **`CMake Warning: … CMAKE_TOOLCHAIN_FILE … not used`?** The toolchain file is
> read once, on the **first** configure of a build dir, and baked into its cache.
> Re-running `cmake … -B <existing-tree>` ignores the flag (that warning); if the
> tree was first configured without it, the cache is locked to the **host**
> compiler (`/usr/bin/c++`) — the ARM app then builds with the wrong compiler, or
> hosted apps self-skip and produce nothing ("firmware .hex not found" later).
> Fix: **delete that `-B` directory** and configure again. Verify with
> `grep CMAKE_CXX_COMPILER: <tree>/CMakeCache.txt` → expect `arm-none-eabi-g++`.

## 2. The static patcher (`scl::static`) — `test-static/test-static`

A **self-contained** firmware app (its own µOS++ `system/`, soft-float
`ldscripts/`, and `libsh1/2` demo `.so`) that links `scl::static` — the loader +
the `common-elf-reader` submodule parser compile straight into `f411ce-sh.elf`.
It is **not** part of the `test/` CMake tree; configure it on its own.

**Build (CMake, INTERFACE style):**
```sh
cmake -S test-static/test-static -B test/build-static \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/test-static/test-static/cmake/arm-none-eabi.cmake
cmake --build test/build-static -j          # -> test/build-static/f411ce-sh.elf (+ libsh1/2.so)
```

**Build with CMake + run on the board — one shot** (WeAct BlackPill STM32F411CE attached).
Run **from the app dir** (`test-static/test-static`), since the script uses `-S .`,
`openocd.cfg`, and relative `libsh1/2.so` paths:
```sh
cd test-static/test-static
./build-and-run-cmake.sh
```
`build-and-run-cmake.sh` does the whole cycle:
1. `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` then
   `cmake --build build` → `build/f411ce-sh.elf`;
2. starts OpenOCD (`openocd.cfg`), and on exit tears it down;
3. runs GDB `-batch`: `load`s the ELF and `continue`s with semihosting enabled
   and the command line `myprogram libsh1/libsh1.so libsh2/libsh2.so`, so the
   patcher opens both demo `.so` over semihosting and the run streams back to the
   console.

`./build-and-run-openocd-only.sh` just starts OpenOCD for a manual GDB session.

Single soft-float image (no `SCL_HOSTED`/ABI switch). Loads `libsh1/libsh1.so` +
`libsh2/libsh2.so` over semihosting, patches their GOTs, runs constructors, and
calls across the boundary. Details in `test-static/test-static/doc.md`.

## 3. Run the static apps on the board — NUCLEO-F411RE or WEACT-F411CE

Both boards share this one codebase; only board boot/init and the OpenOCD flash
config differ (NUCLEO = ST-LINK, WEACT = CMSIS-DAP). Pick the board with
`board=` (default `weact`), pick the app with `test=`.

`build-and-run.sh` prints the full test-name list with no arguments. The SPI
apps (`drv`, `xip-static/spi-flash`) are WEACT-only (the NUCLEO has no SPI
wiring) and the script refuses `board=nucleo` for them.

Run one at a time; output comes back over the semihosting console.

### Expected on-target output

The apps report over semihosting and end in a `PASS` summary. Verified on a
WeAct BlackPill STM32F411CE:

- **`f411ce-sh`** (`scl::static`) — segment dumps, constructor prints, a GOT
  walkthrough, then the demo library calls (see `test-static/test-static/doc.md`).

## 4. XIP (Execute-In-Place) tests — `test-xip/`

The XIP test tree exercises the static XIP adapter variants. Code executes
directly from flash; only RW data is copied to RAM.

```sh
# build all XIP targets (individual targets via --target)
cmake -S test-xip -B test-xip/build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build test-xip/build --target scl-fw-xip-static-semihosting -j
```

Or use the runner script:
```sh
./test-xip/build-and-run.sh static/semihosting          # static XIP (semihosting)
./test-xip/build-and-run.sh static-direct               # direct API calls XIP
./test-xip/build-and-run.sh static-no-pic               # fixed-address no-r10 XIP
./test-xip/build-and-run.sh static-no-pic-simple        # no-pic, all calls BY NAME
./test-xip/build-and-run.sh interrupt-static            # XIP + TIM2 interrupt + static
./test-xip/build-and-run.sh interrupt-static-direct     # direct calls + interrupts
./test-xip/build-and-run.sh static/spi-flash            # SPI flash storage + XIP
```

| Target | App | Loader | Description |
|--------|-----|--------|-------------|
| `static/semihosting` | `scl-fw-xip-static-semihosting` | `scl-xip-static` | Static loader + XIP |
| `static-direct` | `scl-fw-xip-static-direct` | `scl-xip-common` | Direct API calls + XIP |
| `static-no-pic` | `scl-fw-xip-static-no-pic` | `scl-xip-nopic` | Fixed-address `-fno-pic` XIP, no r10 |
| `static-no-pic-simple` | `scl-fw-xip-static-no-pic-simple` | `scl-xip-nopic` | No-pic, calls **by name** (patch_main) |
| `interrupt-static` | `scl-fw-xip-interrupt-static` | `scl-xip-static` | XIP + TIM2 interrupt + static loader |
| `interrupt-static-direct` | `scl-fw-xip-interrupt-static-direct` | `scl-xip-common` | XIP + direct calls + interrupts |
| `static/spi-flash` | `scl-fw-xip-static-spi-flash` | `scl-xip-static` | SPI flash → internal flash → XIP |

## Tips

- After the first configure, just rerun the matching `cmake --build … -j`.
- Build one app instead of all: `cmake --build test/build-static --target f411ce-sh`.
- Everything (except the parser submodule) is self-contained; no sibling folder
  is needed.

This is the exact sequence that produces the `scl::static` `f411ce-sh.elf` and
the static **XIP** test targets. Each module and app is explained in `README.md`
(§2, §4) and [scl-guide.md](scl-guide.md).
