# SCL-TESTS-shared — Test Manual

**Version:** documents the current `SCL-TESTS-shared` tree: five test suites
(`dynamic-shared`, `static-shared`, `optimal-shared`, `overlay-shared`,
`relocatable-shared`) that exercise the seven SCL loader cases from the
`SCL-shared` submodule.

**Scope:** this manual is about **the tests themselves** — how each test app is
built, flashed and run, and the pieces that make that possible: the CMake
build system and its options, the toolchain files, the linker scripts, the
startup/boot files, the platform libraries, the fixture (`.so` / `.rel.o`)
build recipes, and the OpenOCD / QEMU runners. For the **loader API** itself
see `SCL-shared/docs/manual.md`.

---

## Table of contents

1. [The monorepo at a glance](#1-the-monorepo-at-a-glance)
2. [How the loader gets into the tests (submodule + symlinks)](#2-how-the-loader-gets-into-the-tests)
3. [Toolchains](#3-toolchains)
4. [Platforms — board / hosted / QEMU](#4-platforms--board--hosted--qemu)
5. [Startup & boot files](#5-startup--boot-files)
6. [Linker scripts](#6-linker-scripts)
7. [The CMake app helpers](#7-the-cmake-app-helpers)
8. [Runners — OpenOCD and QEMU](#8-runners--openocd-and-qemu)
9. [Fixture build recipes](#9-fixture-build-recipes)
10. [Test suite: dynamic-shared](#10-test-suite-dynamic-shared)
11. [Test suite: static-shared](#11-test-suite-static-shared)
12. [Test suite: optimal-shared](#12-test-suite-optimal-shared)
13. [Test suite: overlay-shared](#13-test-suite-overlay-shared)
14. [Test suite: relocatable-shared](#14-test-suite-relocatable-shared)
15. [CMake options reference](#15-cmake-options-reference)
16. [Glossary](#16-glossary)

---

## 1. The monorepo at a glance

```
SCL-TESTS-shared/
├── CMakeLists.txt          # root: just adds SCL-shared (scl::*) + enable_testing()
├── .gitmodules             # SCL-shared submodule
├── dynamic-shared/         # scl::dynamic-nolib / dynamic-newlib + XIP adapters
├── static-shared/          # scl::static / scl::xip-static / xip::nopic
├── optimal-shared/         # scl::optimal (no-pic XIP, by-name calls)
├── overlay-shared/         # scl::overlay (linker-overlay window manager)
├── relocatable-shared/     # scl::reloc (ET_REL modules)
├── SCL-shared/             # git submodule — the loader sources (loader repo)
└── docs/                   # this manual + loader-choice/import docs + make-pdfs.py
```

The **loader is not vendored**: each test tree's `include/` and `src/` are
symlinks into the sibling `SCL-shared` submodule, and each suite's
`CMakeLists.txt` is a tiny shim:

```cmake
# <tree>/CMakeLists.txt (dynamic-shared, static-shared, overlay-shared, relocatable-shared)
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared ${CMAKE_BINARY_DIR}/_scl-shared)
endif()
```

The root `CMakeLists.txt` exists so an IDE / top-level configure exposes every
`scl::*` target at once:

```cmake
cmake_minimum_required(VERSION 3.20)
project(scl-tests LANGUAGES C CXX)
add_subdirectory(SCL-shared scl)      # -> scl::common/reloc/static/xip-common/...
enable_testing()
```

`SCL-shared/CMakeLists.txt` defines all the INTERFACE loader targets
(`scl::common`, `scl::self-resolve`, `scl::reloc`, `scl::static`, `scl::xip-common`,
`scl::xip-static`, `scl::loader-nopic`, `scl::optimal`, `scl::overlay`,
`scl::dynamic-nolib`, `scl::dynamic-newlib`, `scl::xip-dynamic-nolib`,
`scl::xip-dynamic-newlib`). The `SCL_LOADER` cache option there merely aliases
`scl::loader`; the test apps link the concrete targets directly.

### The five suites and what they prove

| Suite | Loaders exercised | Module format | Runs code from | The headline question |
|---|---|---|---|---|
| `dynamic-shared` | `scl::dynamic-nolib`, `scl::dynamic-newlib`, `scl::xip-dynamic-*` | `-fPIC`/`-fpic` `.so` | RAM (arena/heap) and XIP | "does `dlopen` work for every fixture?" |
| `static-shared` | `scl::static`, `scl::xip-static`, `xip::nopic` | `-fPIC` `.so`, split XIP `.so`, `-fno-pic` fixed-address | RAM buffer and flash | "do direct `name(args)` calls work?" |
| `optimal-shared` | `scl::optimal` (`scl::loader-nopic`) | `-fno-pic -mlong-calls` auto-packed | RAM buffer and flash window | "by-name calls with zero glue?" |
| `overlay-shared` | `scl::overlay` (+ `scl::dynamic-nolib` for comparison) | linker overlays (`.ovN` sections) | shared SRAM window | "memcpy-load vs dlopen overhead?" |
| `relocatable-shared` | `scl::reloc` | `ET_REL` `.rel.o` | RAM arena | "can we beat a GOT/PLT with ABS32?" |

### The two boards + QEMU

Every suite targets the same three platforms (selected with `SCL_PLATFORM` /
`SCL_BOARD`):

| Platform | What it is | How it runs |
|---|---|---|
| `board` / `SCL_BOARD=nucleo` | STM32F411RE NUCLEO (ST-LINK) | OpenOCD `board/st_nucleo_f4.cfg` |
| `board` / `SCL_BOARD=weact` | STM32F411CE WeAct BlackPill (CMSIS-DAP) | OpenOCD `openocd-weact.cfg` |
| `qemu-f411re` | QEMU `netduinoplus2` (emulated F411RE) | `qemu-system-arm` + semihosting |

All firmware is **hard-float** (`-mfloat-abi=hard -mfpu=fpv4-sp-d16`) unless noted
(soft-float is used by some `scl::static` apps).

---

## 2. How the loader gets into the tests

- **`SCL-shared`** is a git submodule (`.gitmodules`:
  `url = /home/dan/Downloads/GIT/SCL-shared.git`). Check it out with
  `git submodule update --init`.
- **`include/`** and **`src/`** at the root of each suite are symlinks to
  `../SCL-shared/include` and `../SCL-shared/src`, so `#include "scl/..."`,
  `#include "dynamic-nolib/dl.hpp"` etc. resolve to the loader headers, and the
  `scl-common`/`scl-*` sources compile from `SCL-shared/src`.
- Each suite's CMake calls `add_subdirectory(../SCL-shared ...)` (guarded by
  `if(NOT TARGET scl-common)`) so a suite can be configured standalone or from
  the root.
- The loader is an **INTERFACE "source-code library"**: the `.cpp` files compile
  into *your* firmware with *your* flags. There are no `.a` archives.

**Submodule inside a submodule:** `SCL-shared` itself pulls
`common-elf-reader` as a nested submodule. `optimal-shared` also runs
`git submodule update --init --recursive` from its `build-and-run.sh`.

---

## 3. Toolchains

Two toolchain files live in each suite's `cmake/` (the copies are identical
across suites):

### 3.1 `cmake/arm-none-eabi.cmake` — freestanding

For the **non-PIC firmware apps** (dynamic/overlay/reloc/optimal test apps) that
link `-nostdlib`:

| Setting | Value |
|---|---|
| `CMAKE_SYSTEM_NAME` | `Generic` |
| `CMAKE_SYSTEM_PROCESSOR` | `arm` |
| compiler discovery | `find_program(arm-none-eabi-gcc/g++/objcopy/objdump/size)` on PATH, else xPack fallback `/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin` (override with `-DSCL_ARM_TOOLCHAIN_BIN=`) |
| `CMAKE_TRY_COMPILE_TARGET_TYPE` | `STATIC_LIBRARY` (a full-exe try-compile would fail: firmware links `-nostdlib`) |
| `SCL_MCU_FLAGS` | `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| `SCL_FW_FLAGS` | `SCL_MCU_FLAGS -fpic -O0 -g -ffunction-sections -fno-common -ffreestanding -fno-tree-loop-distribute-patterns` |
| C++ adds | `-fno-threadsafe-statics` |
| exceptions / RTTI | **kept ON** — main provides the C++ EH runtime to loaded libs |

Rationale (from the file): `-fpic` gives main a PC-relative GOT without `r10`;
`-fno-tree-loop-distribute-patterns` stops gcc turning the `libc-shim` loops back
into `memcpy`/`memset` calls (we link `-nostdlib`).

### 3.2 `cmake/arm-none-eabi-hosted.cmake` — hosted (newlib)

For the **hosted** apps (`scl::dynamic-newlib`, `scl::xip-dynamic-newlib`, the
`static-shared` firmware): non-PIC, newlib/nano, a heap available, and
`-DSCL_HOSTED=ON` (the `relocator` + `elf_image::collect_relocations` parts of
the loader need `std::vector`).

| Setting | Value |
|---|---|
| `SCL_MCU_FLAGS` | `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16` |
| C flags | `SCL_MCU_FLAGS -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3` |
| C++ flags | same + `-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -fno-move-loop-invariants` |

(The static-shared suites use their **own** per-app toolchain copies — see
§11 — which are soft-float `-mfloat-abi=soft`.)

### 3.3 `tools-config.sh` — tool auto-detection

Sourced by every `build-and-run.sh`; overridable by setting the variable before
sourcing. Defaults: ARM toolchain → xPack `15.2.1-1.1.1`, OpenOCD → xPack
`0.12.0-7.1`, OpenOCD scripts dir, `QEMU` → `qemu-system-arm`, `ARM_GDB`.
Exports `ARM_TOOLCHAIN_BIN OPENOCD OPENOCD_SCRIPTS QEMU ARM_GDB`.

---

## 4. Platforms — board / hosted / QEMU

Each suite owns a `test/platforms/` tree. Four platform definitions exist, each
as an INTERFACE CMake library (`platform-<name>-interface`, aliased
`platform::<name>`):

### 4.1 `platform::board` (freestanding, `test/platforms/board/`)

- Sources: globbed `board/src/*.c|*.cpp` + `common/src/*.cpp`:
  `startup.c`, `board.c`, `semihost.c`, `libc-shim.c`, `printf-shim.c`,
  `crt.cpp`, `dl-semihost.cpp`, `scl-log-semihost.cpp`, `semihost-file.cpp`.
- Include dirs: `board/include`, `fixtures-src/include`,
  `fixtures-src/libcppeh/include`, `../../../include/scl`.
- Board switch: `-DSCL_BOARD=nucleo` → `SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000`;
  `weact` → `SCL_BOARD_WEACT=1 HSE_VALUE=25000000`.
- **Firmware link recipe** (the platform contributes the whole link line):
  ```cmake
  -pie -nostdlib -nostartfiles
  LINKER:-Bsymbolic LINKER:--unresolved-symbols=ignore-all
  LINKER:-E LINKER:--gc-sections
  -L<platform>/ldscripts
  ```
  `-pie` gives the firmware a dynamic symbol table; `-Wl,-E` re-exports main's
  dynamic symbols so loaded libraries resolve against them; `-Bsymbolic` binds
  main's own references at link time (there is no dynamic linker on the target);
  `--gc-sections` strips unused code (loadable-library imports are kept alive by
  `-u` anchors, §7).
- Exports `PLATFORM_BOARD_LDSCRIPTS` and `PLATFORM_BOARD_OPENOCD_CFG`
  (`openocd-weact.cfg` / `openocd-nucleo.cfg`).

### 4.2 `platform::board-hosted` (hosted, `test/platforms/board-hosted/`)

Self-contained: the CMSIS + STM32F4 HAL + newlib platform is **vendored** under
`test/platforms/board-hosted/vendor/` (from the original static-unify project).
`STATIC_UNIFY_DIR` points at it. Sources include `symtab-blob.c` (the
`.scl_symtab` flash placeholder), `dl-load.cpp`, `scl-log-trace.cpp`,
`semihost-file.cpp`, `led.cpp`, `timer.cpp`, `initialize-hardware.c`,
`stm32f4xx_hal_msp.c`, the CMSIS/system/newlib/HAL tree.

Link recipe:
```cmake
-nostartfiles --specs=nano.specs LINKER:-u,_printf_float
-T<vendor>/common/ldscripts/mem-sh.ld
-T<vendor>/common/ldscripts/libs-sh.ld
-T<vendor>/firmware/ldscripts/sections-dyn.ld
LINKER:-E
target_link_libraries(... stdc++ m)
```
The hosted platform also defines `scl_embed_symtab(<target>)` — a POST_BUILD
step that runs the `gen-symtab-host` tool on the ELF and `objcopy --update-section
.scl_symtab=<blob>` to bake main's symbol table into the flash placeholder
(`SCL_SYMTAB_BLOB_KB`, default 88 KB). This is how hosted `dlopen` resolves
lib→main imports.

### 4.3 `platform::qemu-f411re` and `platform::qemu-f411re-hosted`

Same shape as the board platforms, but:
- `src/board.c` does **not** poll RCC ready bits (QEMU's STM32F4 model never sets
  them) and reports LED state over semihosting so the harness can verify it;
- the flat-fs "flash" region is mapped to **CCM SRAM** (`0x10000000`, 64K),
  because the emulated flash is read-only ROM;
- firmware is booted by `test/tools/qemu-run.sh` (`-M netduinoplus2`), not
  flashed.
- Exports `PLATFORM_QEMU_F411RE_RUNNER = test/tools/qemu-run.sh`,
  `PLATFORM_QEMU_F411RE_QEMU_MACHINE = netduinoplus2`.

### 4.4 optimal-shared's own `platform-optimal`

`optimal-shared` does **not** consume the four platform libraries; its top-level
`CMakeLists.txt` builds its own INTERFACE `platform-optimal` from an explicit
`PLATFORM_SOURCES` list over the `board-hosted/vendor` tree plus `test-flatfs`
and the QEMU-switchable `XIP_LED`/`XIP_INIT`/`XIP_FS_FILL` files selected by
`test/xip-cmake/qemu-xip.cmake`. Platform sources are compiled with
`-fno-pic;-fhosted` (`-fhosted` so newlib `syscalls.c` provides semihosting
`_read`/`_write`/`__initialize_args`). Defines include `HARD_FLOAT`, and C++
gets `-include abort_fix.h`.

---

## 5. Startup & boot files

There are two startup families: the small freestanding one and the µOS++/newlib
hosted one.

### 5.1 Freestanding `startup.c` (board / qemu platforms — dynamic, overlay, reloc)

`test/platforms/board/src/startup.c` (98 lines) is the whole boot path for the
`-pie -nostdlib` firmware:

| Piece | What it does |
|---|---|
| `g_vectors[16]` | Cortex-M4 vector table (`.isr_vector`): `_estack`, `Reset_Handler`, fault/exception handlers, `SysTick_Handler`. |
| `Reset_Handler` (naked) | 1. copies the **contiguous init-data image** `_sidata → [_sdata, _edata)` from flash to RAM in one linear pass — this one block contains `.data`, `.dynamic`, `.got`, `.got.plt`; 2. zeroes `.bss` (`_sbss.._ebss`); 3. enables the FPU (`CPACR |= 0xF<<20`); 4. `bl main`; 5. spin. |
| `Default_Handler` | catch-all: prints `!! FAULT (default)` then spins. |
| `HardFault_Handler` (weak) | prints `!! HARDFAULT` then spins. |
| weak aliases | `NMI/MemManage/BusFault/UsageFault/SVC/DebugMon/PendSV → Default_Handler`. |

**Why `.dynamic`/`.got` are inside the copied block:** `link_context::from_self()`
reads `_DYNAMIC` at its **RAM VMA** to resolve loaded-library imports against
main; the GOT is read PC-relatively and must be in RAM. Relocations are
**deliberately not applied** here — a `-pie` image at its link address has load
bias 0, so `R_ARM_RELATIVE` entries are already correct after the copy; the
loader applies relocations for the loaded *libraries* (nonzero base).

### 5.2 Hosted startup (board-hosted vendor — static, dynamic-newlib, optimal, XIP)

The µOS++-derived boot chain in the vendored hosted tree:

| File | Role |
|---|---|
| `system/src/cmsis/vectors_stm32f411xe.c` | The full STM32F411XE vector table (16 + 82 IRQs). Entry 1 = `Reset_Handler`. |
| `system/src/cortexm/exception-handlers.c` | `Reset_Handler` — in DEBUG copies `.got`/`.got.plt` from flash to RAM (`_sgot/_esgot/_sgotdata/_sgot_plt/_esgot_plt/_sgotpltdata`) then calls `_start()`; in release a naked `ldr r0,=_start; bx r0`. Also all the exception handlers + `dumpExceptionStack()`. |
| `system/src/newlib/startup.c` | The C `_start()` sequence: `__initialize_hardware_early` → data copy + BSS zero → `__initialize_hardware` → `__initialize_args` → `__run_init_array` → `main(argc,argv)` → `__run_fini_array` → `_exit`. |
| `system/src/cortexm/initialize-hardware.c` | Weak `__initialize_hardware_early()` (`SystemInit()`, VTOR, FPU) and `__initialize_hardware()` (`SystemCoreClockUpdate()`). |
| `system/src/cortexm/reset-hardware.c` | Weak `__reset_hardware()` → `NVIC_SystemReset()` (used by `_exit` in release). |
| `system/src/newlib/syscalls.c` | newlib/libnosys retarget: `__initialize_args` (parses `SYS_GET_CMDLINE` under `OS_USE_SEMIHOSTING`), `_exit`, `_kill`, `_swiopen/_swiread/_swiwrite/_swilseek/_swiclose`, `_stat`, `_gettimeofday`, etc. |
| `system/src/newlib/sbrk.c` | `_sbrk()` over `_Heap_Begin.._Heap_Limit`. |
| `system/src/newlib/cxx.cpp` | minimal `__verbose_terminate_handler` + `__cxa_pure_virtual` stubs. |
| `system/src/newlib/exit.c` / `assert.c` | weak `_exit`/`abort`/`__assert_func`. |

The hosted app also compiles `firmware/src/initialize-hardware.c` (the board
`__initialize_hardware()` override: `HAL_Init()` + `SystemClock_Config()` — HSE
16 MHz→84 MHz for NUCLEO, 25 MHz→100 MHz for WEACT) and
`firmware/src/timer.cpp` / `led.cpp`.

The **qemu-hosted** platform swaps in `qemu-f411re-hosted/src/initialize-hardware.c`
(no HSE/PLL ready polling; `SystemCoreClock = 16 MHz`) and `led.cpp` (traces over
semihosting).

### 5.3 static-shared app startup

The static-shared apps (`test-static`, `test-static-cpp`, `test-static-direct`,
`test-drv`) reuse the µOS++ system tree from `test-static/test-static/system/`
and add their own `src/`:

| File | Role |
|---|---|
| `src/main.cpp` | the test (loads `libsh1.so`/`libsh2.so`; runs the loader test battery). |
| `src/initialize-hardware.c` | board `__initialize_hardware()` (HAL + clock config). |
| `src/initialize-hardware-qemu.c` | QEMU variant (no PLL polling). |
| `src/write.c` | non-semihosting `_write()` retarget → `trace_write()` (guarded out under `OS_USE_SEMIHOSTING`). |
| `src/timer.cpp` | `timer::start/sleep/tick`; defines `SysTick_Handler` (`HAL_IncTick` + `timer::tick`). |
| `src/led.cpp` | `led` class over GPIO BSRR/ODR/IDR. |
| `src/stm32f4xx-hal-msp.c` | HAL MSP init hooks. |

**Reset vector chain:** `vectors_stm32f411xe.c` → `exception-handlers.c`
`Reset_Handler` (GOT copy → `_start`) → `startup.c` `_start` →
`__initialize_hardware*` → `main`.

### 5.4 relocatable-shared startup

`test-relocatable` compiles the **shared** freestanding platform files directly:
`startup.c`, `semihost.c`, `board.c`, `libc-shim.c` (from
`test/platforms/{board,qemu-f411re}/src`) plus `common/src/crt.cpp`. Its linker
script `platform/reloc.ld` is a plain non-PIC map (§6.5). The app overrides the
toolchain's `-fpic` with `-fno-pic -fno-pie`.

---

## 6. Linker scripts

### 6.1 `mem.ld` / `sections.ld` — plain dynamic-loader map (board platform)

```ld
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 0x20020000 */
_simage_arena_size = 64K;
```

`sections.ld`: `ENTRY(Reset_Handler)`, `INCLUDE mem.ld`. Sections:
`.isr_vector`/`.text`/`.rodata`/`.ARM.exidx`/`.rel.dyn` → FLASH; then the **one
contiguous RAM init image** (all `>RAM AT>FLASH`): `.data` (`_sidata/_sdata`),
`.dynamic`, `.got` (`_sgot`), `.got.plt` (`_sgot_plt`, `_edata`); `.bss`
(NOLOAD); `.image_arena` (NOLOAD, `_simage_arena_start/end`); `/DISCARD/`
`.ARM.attributes` `.comment`. A long comment explains why `.dynamic` must live
inside the copied block (§5.1).

### 6.2 `mem-overlay.ld` / `sections-overlay.ld` — overlay map

```ld
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 96K
  OVL   (rwx) : ORIGIN = 0x20018000, LENGTH = 16K   /* the shared overlay window */
}
_estack = 0x20020000;
_ovl_window = ORIGIN(OVL);  _ovl_window_size = LENGTH(OVL);
_simage_arena_size = 32K;
```

`sections-overlay.ld` is `sections.ld` plus the overlay block:

```ld
OVERLAY : NOCROSSREFS
{
  .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
  .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
  .ovcpp { KEEP(*(.ovcpp.text)) KEEP(*(.ovcpp.rodata)) KEEP(*(.ovcpp.data)) }
} >OVL AT>FLASH
```

followed by the load-image symbols `_ovN_lma = LOADADDR(.ovN)`,
`_ovN_size = SIZEOF(.ovN)`, `_ovl_vma = ADDR(.ov1)`, and one `ASSERT(... <=
_ovl_window_size)` per module. `NOCROSSREFS` fails the link if one overlay
references another (they are never co-resident).

### 6.3 `mem-flatfs.ld` / `sections-flatfs.ld` — flat-fs map (board)

```ld
MEMORY {
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K   /* sectors 0..5 */
  FLASH_FS (r)   : ORIGIN = 0x08040000, LENGTH = 256K   /* sectors 6..7, sector-aligned */
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
_fs_base = ORIGIN(FLASH_FS);  _fs_size = LENGTH(FLASH_FS);
```

`sections-flatfs.ld` adds a **`.ramfunc`** section inside the copied image — the
flash erase/program routines must run from RAM because instruction fetches from
flash stall during a flash op on the single-bank F411. Ends with
`ASSERT(_sidata + (_edata - _sdata) <= _fs_base, ...)`.

### 6.4 The no-pic XIP maps (`optimal-shared`, `static-shared` no-pic)

`test/common/ldscripts/sections-flatfs-hosted-nopic.ld` (board) /
`sections-flatfs-qemu-nopic.ld` (QEMU):

```ld
MEMORY {
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  FLASH_FS (r)   : ORIGIN = 0x08040000, LENGTH = 256K   /* qemu: rw @ 0x10000000, 64K CCM */
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
__Main_Stack_Size = 1024 * 3;
INCLUDE windows.generated.ld      /* derived by fixtures-src/build.sh — NOT hand-typed */
ENTRY(_start)
```

- The module windows are `_sxip_text_start/_sxip_text_end`,
  `_sxip_data_start/_sxip_data_end`, `_simage_arena_size` — all **generated** into
  `windows.generated.ld` by the fixture packer (found via `-L<fixtures>`).
- `.rel.dyn`/`.dynsym`/`.dynstr`/`.hash`/`.inits` are placed in FLASH with the
  `__rel_dyn_start/end`, `__dynsym_start/end`, `__dynstr_start/end` symbols
  (`xip::nopic::patch_main()` reads main's relocation/symbol tables directly from
  flash through these).
- `.data` (`>RAM AT>FLASH`) carries dummy GOT boundary symbols to bypass the
  vendor assembly copy loop, then the real `.got`/`.got.plt`.
- `.image_arena` (NOLOAD); `_Heap_Begin = _simage_arena_end`,
  `_Heap_Limit = _sxip_text_start - 4`.
- Two `ASSERT`s: `_simage_arena_end <= _sxip_text_start` (bss/arena don't smash
  the module windows) and code+init image fits below `_fs_base`.

### 6.5 `reloc.ld` — the plain non-PIC ET_REL firmware map

```ld
MEMORY {
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
```

Sections: `.isr_vector`, `.text` (incl. `.rodata`, `.glue_7`), `.ARM.exidx`,
`.init_array` (`__init_array_start/end`) → FLASH; `.data` (`>RAM AT>FLASH`,
`_sidata/_sdata/_edata`); `.bss` (NOLOAD); `/DISCARD/`. **Deliberately absent:**
`.dynsym/.dynstr/.hash/.rel.dyn`, GOT, `_DYNAMIC`, and `_simage_arena` — the
load arena is a static C array (`g_arena[16*1024]`) in `main.cpp`.

### 6.6 The `_reloc` table (`static-shared` `sections-sh.ld`, hosted/newlib maps)

For a **non-PIC** hosted firmware there is no `_DYNAMIC`. The linker script
emits a fixed-format table inside `.inits` that the newlib `link_context()`
reads:

```ld
_reloc = .;
LONG(LOADADDR(.rel.dyn));  LONG(SIZEOF(.rel.dyn));
LONG(LOADADDR(.dynsym));   LONG(SIZEOF(.dynsym));
LONG(LOADADDR(.dynstr));   LONG(SIZEOF(.dynstr));
LONG(LOADADDR(.hash));     LONG(SIZEOF(.hash));
LONG(LOADADDR(.inits));    LONG(SIZEOF(.inits));
```

The same block appears in `test-static/test-static/ldscripts/sections-sh.ld`
(`.inits`, µOS++ style, 560 lines — places `.isr_vector`, `.inits`, `.text`,
`.rodata`, `.ARM.exidx`, `.plt`, `.dynsym`, `.dynstr`, `.hash`, `.rel.dyn` in
FLASH, and `.data`/`.got`/`.got.plt` in RAM) and in the XIP flat-fs maps
(`sections-flatfs-qemu-newlib.ld`). `mem-sh.ld` defines
`RAM 128K @0x20000000`, `FLASH 512K @0x08000000` (µOS++ layout).

### 6.7 Fixture linker scripts

- **Plain PIC `.so` (`scl::static`, `scl::dynamic-newlib`)**: `gcc.ld` =
  `INCLUDE mem.ld` + `INCLUDE sections2.ld`, with the library's `mem.ld` being
  `RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35k` (15k for libsh2) — all sections
  are pure image-relative; the loader adds the base. `sections2.ld` places
  `.inits`, `.plt`, `.got` (`_lsgot/_elsgot`), `.got.plt`, `.rodata`, `.text`,
  `.ARM.extab`, `.ARM.exidx`, `.data`, `.bss`, `.noinit` all in `>RAM`.
- **XIP split `.so`** (`xip-split.ld`, byte-identical across trees): two
  `PT_LOAD` program headers —
  ```ld
  PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X: stays in flash */
    data PT_LOAD FLAGS(6);   /* RW: copied to RAM    */
  }
  SECTIONS {
    . = SIZEOF_HEADERS;      /* => text_vaddr == text_off (PLT PC-rel GOT works) */
    .hash .dynsym .dynstr .rel.dyn .text .rodata .plt .ARM.extab .ARM.exidx :text
    .preinit_array .init_array .fini_array .data.rel.ro .dynamic .got .data .bss :data
  }
  ```
- **No-pic fixed-address** (`nopic-split.ld`): same two-segment shape, but
  `. = __TEXT_BASE__;` / `. = __DATA_BASE__;` — the fixture is linked at its
  derived fixed VAs inside the firmware's reserved windows.
- **Overlay modules**: no per-module script; the application's
  `sections-overlay.ld` `OVERLAY : NOCROSSREFS` block gathers each module's
  `.ovN.text/.rodata/.data` (placed there by
  `__attribute__((section(...)))`).

---

## 7. The CMake app helpers

### 7.1 `scl_test_app(<name> ...)` — dynamic / overlay / static-xip apps

`test/cmake/scl-test-app.cmake`:

```cmake
scl_test_app(scl-fw-dyn1
    LIBS scl::dynamic-nolib        # SCL target(s) to link
    LDSCRIPT sections.ld           # from the platform ldscripts/ (default sections.ld)
    ANCHORS printf puts sinf ...   # extra -Wl,-u keep-alive anchors
    LINK_LIBM                     # append libm.a (hard-float battery)
    RUN_ARGS c1.so c2.so           # semihosting_cmdline args (QEMU only)
    DONE "battery: done"           # completion marker the runner waits for
    NEED_LED 1)                    # require >= 1 LED toggle (QEMU only)
```

What it does:
1. Selects the platform: `-DSCL_PLATFORM=<board|qemu-f411re>` →
   `platform::${SCL_PLATFORM}`; `SCL_BOARD` chooses the board/OpenOCD cfg.
2. Resolves `libsupc++.a` / `libgcc.a` / `libm.a` through the cross compiler
   (`-print-file-name` under the MCU flags — selects the hard-float multilib).
3. Globs the app's `src/*.c|*.cpp`, adds `include/`, links the platform +
   the requested `LIBS`.
4. Defines `DL_LIB_DIR=<test>/fixtures` (absolute — makes the launch directory
   irrelevant for semihosting file opens).
5. Applies the **C++ EH anchors**: `-Wl,-u,__cxa_throw/begin_catch/end_catch/
   end_cleanup/allocate_exception/gxx_personality_v0/dynamic_cast` + the
   `ANCHORS` — this keeps the EH runtime and any libc/libm symbol the fixtures
   import alive through `--gc-sections` (main never references them statically;
   loaded libraries resolve them via `-Wl,-E`).
6. Links `-T<platform_ldscripts>/<LDSCRIPT>` and libsupc++/libgcc.
7. POST_BUILD `objcopy` → `.hex` (ihex) and `.bin`. **(Flash the `.hex`, not
   the `.elf`**: a `-pie` ELF carries a PIE header page OpenOCD cannot place in a
   flash bank.)
8. Registers a ctest entry: QEMU platform → `qemu-run.sh --machine netduinoplus2
   [--done ...] [--arg ...] <elf>`; else → `flash-run.sh <name>.hex`.

### 7.2 `add_optimal_app(<target> <mode> <main-source>)` — optimal-shared

`test/common/optimal-app.cmake`:
- One `add_executable` from the app's own `main.cpp`.
- The platform sources (`PLATFORM_SOURCES` from the top-level CMake) are compiled
  with `-fno-pic;-fhosted`.
- Links `platform-optimal`, `optimal` (→ `scl::loader-nopic`), `libsupc++.a`,
  `libgcc.a`.
- Defines `DL_LIB_DIR=.../fixtures`, `OPTIMAL_MODE="RAM"|"XIP"`.
- Link: `-pie -nostartfiles --specs=nano.specs -Bsymbolic -E
  --unresolved-symbols=ignore-in-shared-libs -T${OPTIMAL_APP_LD} -Map
  -L<fixtures> ${OPTIMAL_LFLAGS} -lstdc++` — `OPTIMAL_LFLAGS` = `-l:*.so` for
  every module in `fixtures/link.txt` (all except the EH/RTTI pair), so main's
  references to fixture exports get GOT/PLT slots that `xip::nopic::patch_main()`
  fills at load.
- POST_BUILD `.hex`/`.bin`; QEMU test with `--done "optimal-shared: done"`.

### 7.3 Hand-rolled apps (test-dynamic/test1, test-static/*)

- `test-dynamic/test1/CMakeLists.txt` does **not** use `scl_test_app`; it builds
  `scl-fw-posix-dl` by hand: `src/main.cpp` + the vendored
  `dl-impl.cpp` (the `dl::` facade), links `platform::${SCL_PLATFORM}-hosted` +
  `scl::dynamic-newlib`, defines `SCL_SO_DIR=.../fixtures` and
  `FIRMWARE_ELF_PATH=...`, applies a long `-u` anchor list (printf/puts/strcat/
  malloc/free/`_Znwj`/`__cxa_*`/`__aeabi_*`/`_ZTVN10__cxxabiv*`), calls
  `scl_embed_symtab()`, and registers a QEMU/OpenOCD test.
- The `test-static/*` apps are fully self-contained (their own toolchain file,
  system tree and ldscripts) — see §11.

### 7.4 `embed-symtab.cmake` (pre-link variant)

`cmake/embed-symtab.cmake` provides `embed_symtab(target)`: a **pre-link** Python
step (`tools/gen-symtab-objs.py`) that walks the target's object files and
generates `symtab.c`, compiled and linked in. (The hosted platform uses the
**post-link** `scl_embed_symtab` instead.)

---

## 8. Runners — OpenOCD and QEMU

### 8.1 `test/tools/flash-run.sh` (OpenOCD)

- `flash-run.sh [--timeout <ms>] [--log <file>] <app-name|path/to/app.hex>`;
  accepts app names (finds the newest matching `.hex` under the SCL root) or an
  explicit `.hex` path.
- Picks the board cfg: `openocd-weact.cfg` (CMSIS-DAP, SWD, `target/stm32f4x.cfg`,
  adapter speed 2000) or `openocd-nucleo.cfg` (`board/st_nucleo_f4.cfg`,
  `reset_config connect_assert_srst`).
- **ABI guard** (hosted static apps): stages fixtures to `/tmp` and verifies with
  `arm-none-eabi-readelf -A` that `Tag_ABI_VFP_args` matches the firmware's float
  ABI — a mismatch would HardFault (NOCP) on the first float call.
- OpenOCD sequence: `init` → `reset halt` → `program <hex>` →
  `arm semihosting enable` (must be enabled **before** `reset run` — a `bkpt
  0xAB` with semihosting off double-faults) → `arm semihosting_cmdline -c <args>`
  (flatfs apps: the `.so` list) → `reset run`. No `verify` (its target-side CRC
  double-faults on this core).

### 8.2 `test/tools/qemu-run.sh` (QEMU)

- `qemu-run.sh [--machine <name>] [--arg <str> ...] [--done <marker> ...]
  [--allow <pattern> ...] [--timeout <secs>] [--need-led <n>] [--need-flash <n>]
  [--gdb] <firmware.elf>`.
- Runs `qemu-system-arm -M netduinoplus2 -display none -monitor none -serial none
  -semihosting-config enable=on,target=native,arg=<a>[,arg=...] -kernel <elf>`.
  Semihosting is `target=native`; `--arg` entries are comma-joined (QEMU 11
  single `-semihosting-config`). The firmware sees them via `SYS_GET_CMDLINE`.
- **Verdict logic:** fail-fast on `HARDFAULT`, `!! FAULT`, `(FAILURES) ==`,
  `SOME FAILED`, `-> FAILURE`, `FAIL — LED`; FAIL on an unexpected `FAIL/FAILED`
  token (unless `--allow`), on a missing `--done` marker, or unmet
  `--need-led`/`--need-flash`; prints the full log and `== QEMU TEST: PASS|FAIL`.
  Exit codes 0/1/2/124.
- `--gdb` delegates to `qemu-gdb.sh`.

### 8.3 Build-and-run dispatchers

Every suite has a top-level `build-and-run.sh` that does the whole
fixtures → configure → build → run chain. Examples:
- `dynamic-shared/test-dynamic/build-and-run.sh [<weact|nucleo|qemu>] test1`
- `dynamic-shared/test-xip/build-and-run.sh [<board>] <nolib|newlib>`
- `static-shared/test-static/build-and-run.sh [<board>] <static|static-cpp|static-direct|drv>`
- `static-shared/test-xip/build-and-run.sh [<board>] <static/semihosting|static-direct|...>`
- `optimal-shared/build-and-run.sh <ram|xip> <weact|nucleo|qemu>`
- `overlay-shared/test-overlay/build-and-run.sh [<board>] test1`
- `relocatable-shared/test-relocatable/build-and-run.sh [<board>]`

---

## 9. Fixture build recipes

The fixtures are the ARM `.so` / `.rel.o` files the firmware loads at run time.
Each model needs its modules built a specific way.

### 9.1 Plain PIC `.so` (`scl::dynamic-newlib`, `scl::static` RAM)

`dynamic-shared/test-dynamic/fixtures-src/build.sh`:
```sh
MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fPIC -shared -nostartfiles -Wl,-E"
OPT="-O0 -ffunction-sections -fdata-sections"
# C:        $CC  $MCU $PIC $OPT -o out.so src...
# C++ noEH: $CXX $MCU $PIC $OPT -std=c++20 -nostdlib -nodefaultlibs \
#           -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wl,-E
# C++ EH:   same minus -fno-exceptions -fno-rtti
```
Builds 12 fixtures into `test-dynamic/fixtures/`: `c1 c2 cpp3 libprov libcons
libcppsh libcppeh ehx_catch mylib1 mylib2` (from shared `test/fixtures-src`) +
`libsh1 libsh2` (tree-local). Note: **plain `-fPIC`** (PC-relative GOT, no `r10`)
— the dynamic loader's model.

### 9.2 XIP split `.so` (`scl::xip-static`, `scl::xip-dynamic-*`)

`dynamic-shared/test-xip/fixtures-src/build-fixtures.sh` / `build-xip-libs.sh`:
```sh
PIC="-fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative"
LDFLAGS="-Wl,-E -Wl,-Bsymbolic"
$CC $MCU $PIC $OPT -shared -nostartfiles $LDFLAGS -T ldscripts/xip-split.ld -o out.so src.c
```
`-Bsymbolic` binds intra-library calls locally (direct BL, no PLT), so only
genuine external calls go through the GOT. Builds the split R-X|RW images into
`test-xip/fixtures/`.

### 9.3 No-pic fixed-address fixtures (`scl::optimal` / `xip::nopic`)

`optimal-shared/fixtures-src/build.sh` — **one generic recipe, addresses
auto-packed**:
```sh
NOPIC="-fno-pic -mlong-calls"
LDFLAGS="-Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all"
# C++: -std=c++20 -nostdlib -nodefaultlibs -fno-exceptions -fno-rtti \
#       -fno-threadsafe-statics -fno-use-cxa-atexit
# link: $cc $MCU $NOPIC $OPT -shared -nostartfiles $LDFLAGS -Wl,--gc-sections \
#       -T <name>.ld -o fixtures/<name>.so sources...
```
The **3-pass packer** (the "no hand-picked addresses" trick):
1. link every module at placeholder VAs via a sed-generated `.ld` from the
   `nopic-split.ld` template (`__TEXT_BASE__`/`__DATA_BASE__`);
2. measure each module with `readelf` (R-X `memsz`, RW `memsz`);
3. pack **top-down below the stack**: `DATA_END = align_down(RAM_TOP - STACK, 8)`,
   `TEXT_START` down to fit, each module in `0x100`-aligned slots; re-link at the
   derived addresses and emit `fixtures/windows.generated.ld`:
   ```ld
   _simage_arena_size = 0x2000 ;
   _sxip_text_start   = 0x2001a100 ;
   _sxip_text_end     = 0x2001e900 ;
   _sxip_data_start   = 0x2001e900 ;
   _sxip_data_end     = 0x2001f400 ;
   ```
Also emits `manifest.txt` (11 modules, load order) and `link.txt` (9 modules —
all except the `libcppeh`/`ehx_catch` EH pair, which main reaches via
`xip::nopic::resolve_any()`).

The static-shared no-pic fixtures use the same recipe (with per-module `.ld` and
`sed`-packed `__TEXT_BASE__`/`__DATA_BASE__`).

### 9.4 ET_REL fixtures (`scl::reloc`)

`relocatable-shared/test-relocatable/fixtures-reloc/Makefile`:
```make
MCU    := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
RELOC  := -mlong-calls -fno-pic
COMMON := $(MCU) $(RELOC) -Os -ffreestanding -fno-common -Wall -Wextra
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=c++20 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit
# per module: compile then partial-link:
#   $CC $(CFLAGS) -c <src>.c -o build/<name>.o
#   $(LD) -r build/<name>.o -o build/<name>.rel.o
```
Builds 9 `.rel.o` into `fixtures-reloc/build/`: `relprov relmod mylib1 mylib2`
(tree-local) + `c1 c2 libprov libcons` (shared) + `cpp3` (C++).

### 9.5 `scl::static` fixture macros (static-shared)

The static apps build their `.so` with an `add_shlib` CMake function:
```cmake
set(LIB_CFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra -DSTM32F411RxE)
# C:   $CC $(LIB_CFLAGS) -c src.c -o x.o
#      $CC $(LIB_CFLAGS) x.o -o lib<name>.so -shared -nostartfiles
#          -T <dir>/ldscripts/gcc.ld -L <dir>/ldscripts
#          -Wl,--gc-sections -Wl,--no-warn-rwx-segments
# C++ (libcpp_features): + -fno-exceptions -fno-rtti -fno-threadsafe-statics
#      link: -shared -nostartfiles -nostdlib -Wl,--gc-sections -Wl,--no-warn-rwx-segments
#      (no -T: default script; -nostdlib keeps out libsupc++)
```
`test-drv` uses a smaller variant with `-T mem.ld` and (C++) `-Wl,-S` to strip
debug sections.

---

## 10. Test suite: dynamic-shared

### 10.1 What it proves

`dlopen`/`dlsym`/`dlclose`/`dlerror` over **every** fixture: plain C, C++ classes
(heap from main), RTTI/EH, cross-module imports (`libcons`→`libprov`,
`ehx`→`libcppeh`, the cyclic `c1`↔`c2`↔`cpp3` cluster), hard-float math,
constructors — plus XIP variants.

### 10.2 `test-dynamic/test1` — the POSIX app

- Build: `cmake -S test-dynamic -B test-dynamic/build
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake
  [-DSCL_PLATFORM=qemu-f411re | -DSCL_PLATFORM=board -DSCL_BOARD=weact]`
  (`SCL_HOSTED` is forced ON; refuses to configure with a non-cross toolchain).
- App `scl-fw-posix-dl`: `src/main.cpp` (only `dlopen/dlsym/dlclose/dlerror`) +
  the vendored `dl-impl.cpp` facade; links `platform::<plat>-hosted` +
  `scl::dynamic-newlib`; `SCL_SO_DIR=.../fixtures`; long `-u` anchor list;
  `scl_embed_symtab()`; `.hex`/`.bin`; QEMU/OpenOCD test (`--done "RESULT:"`).
- Expected output ends `RESULT: SUCCESS` (12 fixtures).
- Run: `./test-dynamic/build-and-run.sh [<weact|nucleo|qemu>] test1`.

### 10.3 `test-xip/test-xip-dynamic-nolib/test1` — freestanding XIP

- Built with the **freestanding** `arm-none-eabi.cmake`; links
  `scl::xip-dynamic-nolib`; `-pie -nostartfiles --specs=nano.specs -Bsymbolic -E`
  + the flat-fs linker script (`sections-flatfs-hosted.ld` on board,
  `sections-flatfs-qemu-nolib.ld` on QEMU).
- App `scl-fw-xip-nolib1`: `main.cpp` + `fs-archive.cpp` + `flash-prog.c`;
  provisions `mylib1.so`/`mylib2.so` into the on-chip flat-fs, then XIP-loads
  them via `xip::nolib::dlopen(fs_blob, ...)`; calls through `pic_call_*` with
  `get_got_base(h)`; helper macros `C0/C1/C2/RD/CSTR` in `test-helpers.hpp`.
- Run: `./test-xip/build-and-run.sh [<board>] nolib`; done marker
  `RESULT: done`.

### 10.4 `test-xip/test-xip-dynamic-newlib/test1` — hosted XIP

- Requires `SCL_HOSTED` + the **hosted** toolchain (skips otherwise); links
  `scl::xip-dynamic-newlib`; on QEMU the flat-fs is CCM SRAM
  (`sections-flatfs-qemu-newlib.ld`, which also carries the `_reloc` block).
- App `scl-fw-xip-newlib1`: `main.cpp` + `fs-archive.cpp` + `flash-prog.c` +
  `image-memory.cpp` + `symtab-blob.c`; uses the **embedded symbol table**
  (magic `'SCLS'`) via `gen-symtab-host` + `objcopy --update-section
  .scl_symtab`; provisions and XIP-loads with `xip::newlib::dlopen`.
- Run: `./test-xip/build-and-run.sh [<board>] newlib`; done marker
  `scl-fw-xip-newlib1: done`.

### 10.5 Fixture set

12 plain-PIC `.so` for the POSIX app; 10 split single-pic-base `.so` for the XIP
apps.

---

## 11. Test suite: static-shared

### 11.1 What it proves

The static patcher `scl::static`: load a plain `-fPIC` `.so` into a RAM buffer,
patch the library's GOT/PLT **and main's GOT**, call exports by name. Includes a
C++ fixture, a direct-call firmware, an SPI-flash driver app, and the full XIP
family (`scl::xip-static`, `xip::nopic`).

### 11.2 The apps (all self-contained: own toolchain file + system tree)

| App | Directory | Loader | What it does |
|---|---|---|---|
| `f411ce-sh` | `test-static/test-static` | `scl::static` | loads `libsh1.so`+`libsh2.so`; runs the loader test battery (`test_elf_reader`, `test_shlib_loader`, `test_dependency_scan`, `test_patch_counts`, `test_bounds_validation`, `test_symbol_table`, `test_structured_error_strings`); LED blink |
| `f411ce-sh` (C++) | `test-static/test-static-cpp` | `scl::static` | same + loads `libcpp_features.so` (C++ classes, `operator new/delete`, `__cxa_pure_virtual`) — the `.so` is **not** linked at build time |
| `scl-fw-static-direct` | `test-static/test-static-direct` | `scl::static` | the **direct-call** model: main links `-lsh1 -lsh2 -ltls_lib -lcpp_features` so the linker emits GOT/PLT slots; includes the emulated-TLS fixture `libtls_lib.so` |
| `scl-fw-drv` | `test-static/test-drv` | `scl::static` | an SPI-flash driver app: `libspi.so` drives a W25Q64 via bit-banged SPI; provisions `libuart.so`/`libled.so`/`libcpp_features.so` into a flat-fs and loads them (`Module::load_file`/`load_buf`) |

### 11.3 Build

Each app configures standalone, e.g.:
```sh
cmake -S test-static/test-static -B build \
      -DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake \
      [-DSCL_PLATFORM=qemu-f411re | -DSCL_BOARD=weact|nucleo]
cmake --build build
```
- The `test-static` toolchain file is **soft-float**:
  `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft`, `CMAKE_TRY_COMPILE_TARGET_TYPE
  STATIC_LIBRARY`.
- Firmware link: `-T mem-sh.ld -T libs-sh.ld -T sections-sh.ld -nostartfiles
  --gc-sections --specs=nano.specs -u _printf_float --no-dynamic-linker
  --no-warn-rwx-segments -L<lib dirs> -lsh1 -lsh2 [-lstdc++]`.
- `test-drv` **does not** pass `--no-dynamic-linker` (the direct-call model needs
  `.dynsym`/`.rel.dyn` generated) and links the `.so` files by **absolute path**
  in `target_link_libraries` (CMake for arm-none-eabi filters bare `-lspi`).

### 11.4 The XIP tests (`test-xip/`)

Configured from `test-xip/CMakeLists.txt` (`arm-none-eabi.cmake`); each app is a
hosted platform over the vendored tree with `-pie -nostartfiles --specs=nano.specs
-Bsymbolic -E -T<sections-flatfs-hosted.ld>`, and `scl::xip-static` (r10) or
`scl::loader-nopic` (no-pic). The six variants:

| App | Loader | Notes |
|---|---|---|
| `scl-fw-xip-static-semihosting` | `scl::xip-static` | the main XIP battery (c1/c2/cpp3, ehx/libcppeh/libcppsh/libprov, `walkthrough.cpp`, `walkthrough_complex.cpp`) |
| `scl-fw-xip-static-spi-flash` | `scl::xip-static` | WeAct-only; SPI-flash flat-fs backend (`fs-archive-spi.cpp`); OpenOCD erases `0x08040000..0x08080000` first |
| `scl-fw-xip-static-direct` | `scl::xip-static` | direct-by-name (links `-l:slib.so -l:mylib1.so ...`) |
| `scl-fw-xip-interrupt-static` / `-direct` | `scl::xip-static` | interrupt-in-loaded-module tests |
| `scl-fw-xip-static-no-pic` | `scl::loader-nopic` | nothing linked in; exports reached via `resolve()` + plain pointers; own `sections-flatfs-hosted-nopic.ld` with `_sxip_*` windows |
| `scl-fw-xip-static-no-pic-simple` | `scl::loader-nopic` | links `-l:slib.so -l:libprov.so ...` so `patch_main()` fills main's GOT → by-name calls |

---

## 12. Test suite: optimal-shared

### 12.1 What it proves

The "optimal" model: `-fno-pic -mlong-calls` modules auto-packed into derived
fixed windows; `main` calls every fixture export **by name** (`s_add(10,5)`), with
no `dlopen`, no `dlsym`, no `r10`, no hand-written load list (generated). Two apps
that differ only in where the module bytes come from.

### 12.2 The apps

- `scl-fw-optimal-ram` (`test-ram`): `OPTIMAL_MODE="RAM"`. `provide()` copies each
  `.so` from the host (semihost file) into a static **52 KB RAM arena**
  (`alignas(8) std::byte g_arena[52*1024]`) and returns a span into it.
- `scl-fw-optimal-xip` (`test-xip`): `OPTIMAL_MODE="XIP"`. `setup()` calls
  `fw::fs_format()`; `provide()` stages each `.so` into the `image_arena`,
  programs it into flash (`fw::fs_add`), frees the arena, and returns a span over
  the **flash-resident** copy (`fw::fs_blob`).

Both then run the identical flow: `g_blobs[i] = provide(kModules[i])` over the
generated list → `optimal::load_and_link(g_blobs, kNumModules, g_mods)` →
`[boot] N/11 modules loaded + linked, main GOT patched` → exercise every fixture
by name (slib → mylib1 → mylib2 → libprov/libcons → libcppsh → libcppeh/ehx →
cpp3 → c2/c1) → `optimal-shared: done`. The EH/RTTI pair (`libcppeh`,
`ehx_catch`) is reached via `xip::nopic::resolve_any(...)` + a cast.

### 12.3 Build

```sh
./build-and-run.sh <ram|xip> <weact|nucleo|qemu>
```
which runs `fixtures-src/build.sh` (auto-pack), then
`cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
[-DSCL_PLATFORM=qemu-f411re | -DSCL_PLATFORM=board -DSCL_BOARD=<board>]`
(configure-time `gen-imports` emits `modules.generated.inc` +
`imports.generated.h`), builds the target, and runs via `qemu-run.sh --done
"optimal-shared: done"` or `flash-run.sh`.

### 12.4 `examples/floatapp`

A standalone WeAct example that loads `floatlib.so` (hard-float math) into the
on-chip flat-fs and calls `float_add(2.5f,3.5f)`/`float_mul(2.5f,3.5f)` **by
name**, printing to **USART1** (PA9/PA10, 115200 8N1) via `uart.c` and to the
semihosting trace. It pulls the SCL via `cmake/scl-lib.cmake`, builds its own
`scl-loader-nopic`/`optimal` INTERFACE targets, uses `src/app-windows.ld` (which
`INCLUDE`s the derived `fixtures/windows.generated.ld`), and links
`-l:floatlib.so`.

---

## 13. Test suite: overlay-shared

### 13.1 What it proves

`test1` (`scl-fw-ovl1`) compares the **linker-overlay** technique
(`scl::overlay`: modules bound at link time to one shared SRAM window, "load" =
`memcpy` LMA→VMA + DSB/ISB) head-to-head against the **dynamic loader**
(`scl::dynamic-nolib`). Six tests, each run both ways.

### 13.2 Build

```cmake
set_source_files_properties(src/ov1.c src/ov2.c
    PROPERTIES COMPILE_OPTIONS "-fno-pic;-mlong-calls")
set_source_files_properties(src/ovcpp.cpp
    PROPERTIES COMPILE_OPTIONS
    "-fno-pic;-mlong-calls;-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit;-fno-unwind-tables")
scl_test_app(scl-fw-ovl1
    LIBS scl::dynamic-nolib scl::overlay
    LDSCRIPT sections-overlay.ld
    ANCHORS printf puts sinf cosf logf sqrtf trace_event trace_u32 fm_callback fm_var fm_info
    LINK_LIBM)
```

The overlay modules are `-fno-pic -mlong-calls` (they run in the SRAM window but
call into flash across more than Thumb-`BL` reach) and place their code/data in
`.ov1.*` / `.ov2.*` / `.ovcpp.*` sections via
`__attribute__((section(...)))`. `sections-overlay.ld` + `mem-overlay.ld` define
the shared 16K `OVL` window at `0x20018000` and the `OVERLAY : NOCROSSREFS` block
(§6.2). `main-overlay.cpp` builds a `scl_overlay_module[3]` table from the linker
symbols `_ovl_vma`/`_ovN_lma`/`_ovN_size` and drives `scl_overlay_init`/
`scl_overlay_load`. Entry points are declared `__attribute__((long_call))`.

### 13.3 The six comparisons (dynamic vs overlay)

1. module reads `fm_var`/`fm_info` + calls main (both must be 611);
2. main reads module `.data`/`.rodata` (`c2_data/c2_const` vs `ov2_data/ov2_const`);
3. C++ class method + exported const (`cpp3_magic` vs `ovcpp_magic`/`ovcpp_method`);
4. the headline: overlay entries **alias one window VMA**, dynamic libs get three
   distinct bases;
5. hard-float compute (libm via relocated PLT vs `long_call` BLX), results ±2;
6. heap (`cpp3_heap(8)` vs `ovcpp_heap(8)`, both 533 — one shared `crt.cpp`
   region_allocator heap).

Ends with `overlay: done` + PASS/FAIL tally + `semihost_exit(0)`.

Run: `./test-overlay/build-and-run.sh [<board>] test1` (QEMU `--done "overlay:
done"`).

---

## 14. Test suite: relocatable-shared

### 14.1 What it proves

`scl::reloc` loads **ET_REL** modules (`.rel.o`, `-mlong-calls -fno-pic -r`) into
a caller-supplied RAM arena, resolves imports against main (and across a loaded
set via `load_set`), runs constructors, and hands back plain function pointers.
No GOT, no PLT, no `r10`, no `_DYNAMIC`.

### 14.2 Build

```sh
cmake -S test-relocatable -B test-relocatable/build \
      -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake \
      [-DSCL_PLATFORM=qemu-f411re | -DSCL_PLATFORM=board -DSCL_BOARD=<board>]
cmake --build test-relocatable/build
```

The app `scl-fw-reloc` is an **ordinary non-PIC firmware**: it compiles the
freestanding platform files (`startup.c`, `semihost.c`, `board.c`, `libc-shim.c`,
`crt.cpp`), overrides the toolchain's `-fpic` with `-fno-pic -fno-pie -Os
-ffunction-sections -fdata-sections`, and links with
`-T platform/reloc.ld -nostdlib -nostartfiles --gc-sections` + libm/libgcc (by
multilib path). Fixtures are built by the `reloc-fixtures` custom target
(`make -C fixtures-reloc TOOLCHAIN_PATH=...`), and the app gets
`-DRELOC_LIB_DIR=<...>/fixtures-reloc/build`. Run via `qemu-run.sh --done
"RESULT: done"` or `flash-run.sh`.

### 14.3 The test groups (5)

1. `relprov` + `relmod` as a `load_set` (`relmod` calls `prov_add` in `relprov`);
2. `mylib1` (strings, struct-by-value, weak symbols);
3. `mylib2`;
4. `libprov` + `libcons` as a `load_set`;
5. the **cyclic cluster** `cpp3`+`c2`+`c1` as a `load_set`.

Exports are obtained with `reloc::sym(...)` and called as plain function pointers
(e.g. `prov_add(10,20)=8751`, `cpp3_method(7,5)=1242`, `c1_call_c2(7)=17469`).
Expected output: `RESULT: done`.

---

## 15. CMake options reference

| Option | Where | Values / default | Effect |
|---|---|---|---|
| `SCL_PLATFORM` | every suite | `board` (default) \| `qemu-f411re` | selects board or QEMU platform |
| `SCL_BOARD` | every suite | `weact` (default) \| `nucleo` (or `qemu` when `SCL_PLATFORM=qemu-f411re`) | board pin-out + OpenOCD cfg + `HSE_VALUE`/`SCL_BOARD_*` define |
| `SCL_HOSTED` | `SCL-shared` (forced ON by hosted suites) | `ON`/`OFF` | builds `relocator.cpp` + `elf_image::collect_relocations` (`std::vector` parts); required by `scl::dynamic-newlib` |
| `SCL_LOADER` | `SCL-shared` | `relocatable\|static-full-ram\|static\|optimal\|dynamic-nolib\|dynamic-newlib\|overlay` | aliases `scl::loader` to one case (the test apps link concrete targets instead) |
| `SCL_ARM_TOOLCHAIN_BIN` | toolchain files | path | override the arm-none-eabi bin directory |
| `SCL_SYMTAB_BLOB_KB` | hosted platform | 88 | flash (KB) reserved for the `.scl_symtab` loader symbol table |
| `DL_LIB_DIR` | app targets | absolute path to fixtures | semihosting directory the app reads `.so` from |
| `SCL_SO_DIR` | `test-dynamic/test1` | `.../fixtures` | hosted `.so` directory |
| `RELOC_LIB_DIR` | `test-relocatable` | `.../fixtures-reloc/build` | directory of `.rel.o` fixtures |
| `OPTIMAL_MODE` | optimal apps | `"RAM"` \| `"XIP"` | selects the `provide()` path |
| `RUN_MS` / timeout | `flash-run.sh` | ms | flash-and-run timeout |

Environment (from `tools-config.sh`, override before sourcing): `ARM_TOOLCHAIN_BIN`,
`OPENOCD`, `OPENOCD_SCRIPTS`, `QEMU`, `ARM_GDB`.

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **SCL** | Shared-Code Loader — the runtime-loading library family (repo `SCL-shared`). |
| **INTERFACE source library** | A CMake target with no `.a`; its `.cpp` files are compiled into each consumer with the consumer's flags. |
| **Suite / test tree** | One of `dynamic-shared`, `static-shared`, `optimal-shared`, `overlay-shared`, `relocatable-shared`. |
| **Fixture** | A prebuilt module the firmware loads at run time (`.so`, split `.so`, or `.rel.o`). |
| **Platform library** | The CMake INTERFACE target that supplies startup/board/semihosting sources + the firmware link recipe (`platform::board`, `platform::board-hosted`, `platform::qemu-f411re`, ...). |
| **`scl_test_app()`** | The CMake helper that turns `src/*` into a firmware executable with anchors, ldscript, `.hex`/`.bin`, and a ctest entry. |
| **`add_optimal_app()`** | optimal-shared's equivalent helper. |
| **Anchor (`-Wl,-u`)** | A keep-alive: forces a symbol into the link so `--gc-sections` cannot strip something only *loaded libraries* reference (a stripped symbol → runtime HardFault, not a link error). |
| **`-Wl,-E`** | Export main's dynamic symbols so loaded libraries can resolve imports against them. |
| **`-Bsymbolic`** | Bind main's own references at link time (no dynamic linker on the target). |
| **Load bias** | The runtime base address of a loaded image; `R_ARM_RELATIVE` fixups add it. |
| **`_DYNAMIC`** | The dynamic section array main emits under `-pie`; `link_context::from_self()` reads it. |
| **`_reloc` table** | A fixed `(address,size)[]` a *non-PIC* linker script emits so the newlib `link_context()` can find main's `.rel.dyn/.dynsym/.dynstr/.hash/.inits`. |
| **`.scl_symtab`** | The flash-resident symbol-table placeholder (`SCLS` magic) that `gen-symtab`/`scl_embed_symtab` fills for hosted `dlopen`. |
| **Flat-fs** | The on-chip flash archive (superblock + TOC + blobs) the XIP apps provision modules into (`fw::fs_format/fs_add/fs_blob`). |
| **`_simage_arena`** | The linker-reserved RAM region the dynamic loaders allocate image buffers from (`fw::image_arena()`). |
| **XIP** | eXecute-In-Place: code stays in flash, only RW data is copied to RAM. |
| **`r10`** | The PIC base register (`-msingle-pic-base -mpic-register=r10`) used by the single-pic-base fixtures. |
| **`_sxip_text/_sxip_data`** | The reserved fixed RAM windows for the no-pic XIP modules (`optimal`/`xip::nopic`); derived by the fixture packer, not hand-typed. |
| **ET_REL** | Relocatable object (`.o`) — the `scl::reloc` module format. |
| **`xip-split.ld`** | The two-`PT_LOAD` (`text` R-X / `data` RW) fixture linker script; `VA == file_offset` so flash PLT PC-relative GOT addressing works. |
| **`nopic-split.ld`** | The two-segment fixture script with fixed `__TEXT_BASE__`/`__DATA_BASE__` windows. |
| **Overlay** | Link-time time-multiplexing: modules bound to one shared SRAM window (`OVL`), stored back-to-back in flash; "load" is a `memcpy` + `DSB`/`ISB`. |
| **Semihosting** | ARM debug-probe I/O (`bkpt 0xAB`); the firmware prints trace and reads fixture files through it during tests. |
| **`qemu-run.sh`** | The QEMU test harness: boots the ELF with semihosting, watches for `--done`/fail markers, prints PASS/FAIL. |
| **`flash-run.sh`** | The OpenOCD test harness: `program` + `arm semihosting enable` + `reset run`, with an ABI guard for hosted fixtures. |

---

*End of SCL-TESTS-shared test manual. For the loader API, see
`SCL-shared/docs/manual.md`; for the loader-choice and import guides, see the
other `.md` files in this `docs/` directory.*
