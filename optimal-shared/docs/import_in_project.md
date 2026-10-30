# Importing optimal-shared into your own project

**Scenario.** You have a firmware project of your own:

```
my-project/
├── CMakeLists.txt          # builds your firmware
├── include/                # your headers
├── src/*.cpp               # your firmware sources (incl. src/main.cpp)
└── fixtures/               # libraries YOU want to load at runtime
    ├── CMakeLists.txt       # builds them
    ├── include/
    └── src/*.c , *.cpp
```

You want to **clone optimal-shared into it**, build the libraries in `fixtures/`
as **loadable shared libraries**, and call them from `src/main.cpp`
**directly, by name** — `my_lib_fn(1, 2)` — with the SCL loader/patcher pulled in
as **INTERFACE source libraries** (no prebuilt archives). This guide gives the two
sets of build options (one for your firmware, one for the fixtures) and the exact
CMake wiring.

Target board here: **WeAct F411CE (BlackPill)** (also QEMU / NUCLEO).

---

## 0. Clone optimal-shared (recursively)

optimal-shared uses `common-elf-reader` as a git submodule, so clone recursively.
As a submodule of your project:

```sh
cd my-project
git submodule add <url>/optimal-shared.git optimal-shared
git -C optimal-shared submodule update --init --recursive   # pulls common-elf-reader
git submodule update --init --recursive                      # (or, in one go, from a fresh clone)
```

or a plain recursive clone:

```sh
git clone --recursive <url>/optimal-shared.git optimal-shared
```

Your tree is now:

```
my-project/
├── CMakeLists.txt
├── include/  src/
├── fixtures/{CMakeLists.txt, include/, src/}
└── optimal-shared/              # ← the loader + reader (common-elf-reader submodule inside)
```

---

## 1. The two build recipes (the "right options")

Loading works only if **both** sides are built the SCL "no-pic fixed-address" way.

### 1a. Fixture libraries — `-fno-pic -mlong-calls`, fixed-address, split segments

Each library is a position-**dependent** ELF `ET_DYN` linked at a fixed address, so
your firmware can call its exports with a plain call (no GOT/PLT/r10):

| Option | Why |
|---|---|
| `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16` | match the MCU/FPU of your firmware exactly |
| `-fno-pic -mlong-calls` | fixed absolute addresses, no GOT/PLT/r10; long calls avoid BL range limits |
| `-O0 -ffunction-sections -fdata-sections` | (any `-O`; sections let `--gc-sections` trim) |
| `-shared -nostartfiles -Wl,-E` | a shared object exporting all globals (`-E` = export dynamic) |
| `-Wl,-z,max-page-size=0x100` | small pages → small `.so` (fits the on-chip store) |
| `-Wl,--unresolved-symbols=ignore-all` | imports from your firmware / other libs stay UND, resolved at load |
| `-T nopic-split.ld` (text VA + data VA filled in) | two `PT_LOAD` segments (R-X \| R-W) at the module's fixed windows |
| C++: add `-nostdlib -nodefaultlibs -fno-threadsafe-statics -fno-use-cxa-atexit` (and `-fno-exceptions -fno-rtti` unless the lib throws) | the C++ runtime is borrowed from your firmware |

The two fixed VAs are **not hand-picked**: they are packed automatically inside the
firmware's reserved SRAM windows (see §3). The ready-made engine that does the
3-pass measure→pack→relink is `optimal-shared/fixtures-src/build.sh`; point it at
your sources (§2).

### 1b. Your firmware — link so the loader can fill its GOT

Your firmware is built `-pie -Wl,-E` (its own globals land in its `.dynsym` so the
libraries can import them), links the fixtures by name, and lets the loader patch
its GOT/PLT at load:

| Link option | Why |
|---|---|
| `-pie -Wl,-E` | your firmware exports its symbols to the libraries; enables the GOT slots |
| `-Wl,-Bsymbolic` | bind your own intra-firmware refs locally |
| `-Wl,--unresolved-symbols=ignore-in-shared-libs` | the `.so`s' UND imports are satisfied at load, not at your link |
| `-nostartfiles --specs=nano.specs` | freestanding startup + newlib-nano |
| `-T <app>.ld` | your linker script that **reserves** the `_sxip_text_*` / `_sxip_data_*` windows (§3) |
| `-L fixtures -l:mylib.so …` | reference each fixture by name so the linker emits a GOT/PLT slot for its exports |
| platform sources compiled `-fno-pic -fhosted` | firmware is r10-free; `-fhosted` so newlib syscalls compile |

At load, `xip::nopic::patch_main()` walks your firmware's `_DYNAMIC` and writes each
library's export address into your GOT/PLT — after which `my_lib_fn(1,2)` is a
direct call.

---

## 2. Build YOUR fixtures as loadable `.so`s

`optimal-shared/fixtures-src/build.sh` is the recipe engine. It:
- reads the fixed-window bounds from **your firmware's linker script** (single
  source of truth),
- builds every module once, measures the two segment sizes, **packs** them
  contiguously inside the windows, re-links at the packed addresses,
- writes `fixtures/manifest.txt` (load order) and `fixtures/link.txt` (the subset
  to `-l:`-link).

Point it at your libraries by editing the one `MODULES` table (name + kind + sources)
near the top of a copy of `build.sh`, and set `FIXTURES_SRC` / `APP_LD`:

```sh
# fixtures/build-fixtures.sh  (a thin wrapper you keep in your project)
export FIXTURES_SRC="$PWD/fixtures/src"                     # your lib sources
export APP_LD="$PWD/src/app-windows.ld"                     # your firmware linker script
bash optimal-shared/fixtures-src/build.sh                   # -> fixtures/*.so + manifest + link.txt
```

Then drive it from `fixtures/CMakeLists.txt` so it runs as part of the build:

```cmake
# fixtures/CMakeLists.txt — build the loadable libraries with the no-pic recipe.
add_custom_target(fixtures ALL
  COMMAND ${CMAKE_COMMAND} -E env
          ARM_TOOLCHAIN_BIN=${ARM_TOOLCHAIN_BIN}
          FIXTURES_SRC=${CMAKE_CURRENT_SOURCE_DIR}/src
          APP_LD=${CMAKE_SOURCE_DIR}/src/app-windows.ld
          bash ${CMAKE_SOURCE_DIR}/optimal-shared/fixtures-src/build.sh
  BYPRODUCTS ${CMAKE_SOURCE_DIR}/fixtures/manifest.txt
  COMMENT "Building loadable fixtures (-fno-pic, auto-packed)")
```

> Adding a library = one line in the `MODULES` table. No per-library linker script,
> no hand-picked addresses.

Generate the load list (and optional zero-header declarations) with `gen-imports`
(the C++ tool `optimal-shared/tools/gen-imports.cpp`, or `gen-imports.py`):

```sh
optimal-shared/tools/gen-imports  build/imports.generated.h  build/modules.generated.inc  fixtures/*.so
```

---

## 3. The SRAM windows are DERIVED — you do not type any address

The loaded modules' code/data live in two SRAM windows, but you never write their
addresses. `fixtures-src/build.sh` reads only your chip's **RAM geometry** (the
`MEMORY { RAM … }` line and `__Main_Stack_Size`) from your linker script, measures
the built modules, packs them **top-down just below the stack**, and emits
`fixtures/windows.generated.ld`:

```ld
/* AUTO-GENERATED by build.sh — DO NOT EDIT */
_simage_arena_size = 0x2000 ;
_sxip_text_start   = 0x2001a100 ;   _sxip_text_end = 0x2001e900 ;
_sxip_data_start   = 0x2001e900 ;   _sxip_data_end = 0x2001f400 ;
```

Your `src/app-windows.ld` therefore contains **no window addresses** — just the
device geometry and one line pulling in the generated file (found via `-Lfixtures`):

```ld
MEMORY { FLASH (rx):ORIGIN=0x08000000,LENGTH=256K   RAM (rwx):ORIGIN=0x20000000,LENGTH=128K … }
_estack = ORIGIN(RAM) + LENGTH(RAM);
__Main_Stack_Size = 1024 * 3 ;
INCLUDE windows.generated.ld          /* _sxip_* + _simage_arena_size, derived */
```

Copy `optimal-shared/test/common/ldscripts/sections-flatfs-*-nopic.ld` as your
template — it already does exactly this (heap fills the space below the windows;
an `ASSERT` fails the link if your firmware's `.bss` would collide with them, in
which case you shrink your firmware or the fixture set, never edit an address).

---

## 4. Wire your main `CMakeLists.txt` — SCL as INTERFACE

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_fw C CXX)

set(SCL ${CMAKE_SOURCE_DIR}/optimal-shared)

# --- pull in the SCL loader/patcher + ELF reader as INTERFACE source libraries ---
set(COMMON_ELF_READER_DIR ${SCL}/common-elf-reader)       # submodule, project root of SCL
include(${SCL}/cmake/scl-lib.cmake)                        # scl::common / static / xip-* + elf-reader

add_library(scl-loader-nopic INTERFACE)                   # the no-pic loader + two port sources
target_sources(scl-loader-nopic INTERFACE
    ${SCL}/src/xip/static/nopic.cpp
    ${SCL}/src/image-memory.cpp
    ${SCL}/src/link-context.cpp)
target_include_directories(scl-loader-nopic INTERFACE
    ${SCL}/include ${SCL}/include/scl ${SCL}/include/xip)
target_link_libraries(scl-loader-nopic INTERFACE scl::xip-common)   # -> scl::common -> elf-reader

add_library(optimal INTERFACE)                            # the small load_and_link helper
target_sources(optimal INTERFACE ${SCL}/src/loader.cpp)
target_include_directories(optimal INTERFACE ${SCL}/include)
target_link_libraries(optimal INTERFACE scl-loader-nopic)

# --- your firmware ---
add_executable(my_fw src/main.cpp ${MY_SOURCES} ${PLATFORM_SOURCES})
add_dependencies(my_fw fixtures)                          # build the .so's first (§2)
target_link_libraries(my_fw PRIVATE optimal)              # <- the loader, as INTERFACE
target_include_directories(my_fw PRIVATE include ${CMAKE_BINARY_DIR})  # for *.generated.*
target_compile_definitions(my_fw PRIVATE DL_LIB_DIR="${CMAKE_SOURCE_DIR}/fixtures")

# platform/newlib translation units must be r10-free + hosted
set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")

# read the fixtures to -l:-link (all but any EH/RTTI libs, which you call via a pointer)
file(STRINGS ${CMAKE_SOURCE_DIR}/fixtures/link.txt _libs)
set(_lflags "")
foreach(m ${_libs})
    list(APPEND _lflags "-l:${m}")
endforeach()

target_link_options(my_fw PRIVATE
    -pie -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic LINKER:-E LINKER:--unresolved-symbols=ignore-in-shared-libs
    -T${CMAKE_SOURCE_DIR}/src/app-windows.ld
    -L${CMAKE_SOURCE_DIR}/fixtures ${_lflags} -lstdc++)
```

`scl-lib.cmake` brings `scl::common`, `scl::static`, `scl::xip-common`,
`scl::xip-static` and `common-elf-reader::elf-reader` — **all INTERFACE**: their
sources compile into `my_fw` with your flags, no `.a`.

---

## 5. Call the libraries by name in `src/main.cpp`

```cpp
#include "optimal/loader.hpp"

// declare each firmware symbol the libraries import (kept in your .dynsym by -Wl,-E):
extern "C" unsigned host_add(unsigned, unsigned) { /* ... */ }

// declare the library exports you call (their real prototypes — your libs' API):
extern "C" unsigned my_lib_fn(unsigned, unsigned);
extern "C" unsigned my_lib_var;

static const char* kModules[] = {
#include "modules.generated.inc"     // generated load list (or write it by hand)
};

int main() {
    optimal::Module mods[std::size(kModules)];
    std::span<const std::byte> blobs[std::size(kModules)];
    for (std::size_t i = 0; i < std::size(kModules); ++i)
        blobs[i] = provide(kModules[i]);            // read each .so (RAM buffer or flash)
    optimal::load_and_link(blobs, std::size(kModules), mods);   // load + resolve + patch_main

    unsigned r = my_lib_fn(1, 2);                   // ← direct, by name
    unsigned v = my_lib_var;                        // ← library global, by name
}
```

`provide()` is your ~15-line reader — a RAM buffer or your on-chip flash store; see
`optimal-shared/test-ram/src/main.cpp` and `test-xip/src/main.cpp` for both.

---

## 6. Build & run

```sh
# 1) fixtures (loadable .so's)          2) firmware                 3) run
cmake --build build --target fixtures
cmake --build build --target my_fw
#   WeAct (CMSIS-DAP over SWD):
optimal-shared/test/tools/flash-run.sh build/my_fw.hex
#   or QEMU:
optimal-shared/test/tools/qemu-run.sh --machine netduinoplus2 --done "done" build/my_fw
```

---

## 7. Reference: optimal-shared's own build proves the recipe

optimal-shared itself is exactly this shape — its `CMakeLists.txt`,
`fixtures-src/build.sh`, `test/common/ldscripts/*.ld` and
`test-{ram,xip}/src/main.cpp` are the authoritative, working copy of every option
above. Both its tests pass on **QEMU** and on the **WeAct F411CE** board:

```sh
optimal-shared/build-and-run.sh xip weact    # flash + run on the board (OpenOCD, semihosting)
optimal-shared/build-and-run.sh ram weact
optimal-shared/build-and-run.sh xip qemu     # no hardware needed
```

**Verified on WeAct F411CE hardware** (CMSIS-DAP/SWD), identical to QEMU — **both
the XIP and the RAM test** loaded all 11 fixtures and exercised every export by name:

```
optimal-shared [XIP]: load-in-a-loop, exercise EVERY fixture (no r10, no dlsym)
[boot] 11/11 modules loaded + linked, main GOT patched
-- slib --      s_add(10,5)=30   s_get_a()=15   s_print(): [slib] from flash: s_a=15 s_b=30
-- mylib1 --    ptrfunc(g,5)=70  ret_struct(): a="15_in_12345bb" b=15  weak boo2()=60
-- libcppsh --  cppsh_compute(w,5)=92 (virtual)  instances=1001
-- libcppeh --  eh_dyncast(0)=2 (RTTI dynamic_cast)
-- cpp3 --      cpp3_method(7,5)=1242 (hard-float)  cpp3_heap(8)=533 (heap from main)
-- c2 / c1 --   c2_step_down(4)=10  c2_scale(1.5f)=8705  c1_call_c2(7)=17482 (cyclic)
optimal-shared: done

optimal-shared [RAM]: load-in-a-loop, exercise EVERY fixture (no r10, no dlsym)
[boot] 11/11 modules loaded + linked, main GOT patched
   … identical results (s_add(10,5)=30, ptrfunc(g,5)=70, cppsh_compute=92,
     eh_dyncast=2, cpp3_method(7,5)=1242, c2_step_down(4)=10, c1_call_c2(7)=17482) …
optimal-shared: done
```

Both `./build-and-run.sh xip weact` and `./build-and-run.sh ram weact` were flashed
and run on the board with matching output.

When in doubt about a flag, copy it from there.
