# SCL Static Build Reference Guide

Cortex-M Source Code Libraries (SCL) — Static loader family build system analysis and configuration reference.

---

## 1. Directory Structure (Library Only — Excluding test-*)

```
static-shared/
├── CMakeLists.txt                    # Root build file — defines all SCL library targets
├── cmake/
│   ├── arm-none-eabi-hosted.cmake    # Toolchain: hosted CMSIS/HAL/newlib firmware
│   └── embed-symtab.cmake            # Helper: generate & embed symbol table from objects
├── common-elf-reader/                # GIT SUBMODULE — ELF32 reader/parser
│   ├── CMakeLists.txt                # INTERFACE target: common-elf-reader::elf-reader
│   ├── include/loader/
│   │   ├── byte-source.hpp           # Abstract random-access byte reader interface
│   │   ├── detail/bytes.hpp          # Bounds-checked LE byte accessors
│   │   ├── elf-image.hpp             # Parsed ELF32 image view
│   │   ├── elf-types.hpp             # ARM ELF32-LE structures & constants
│   │   ├── error.hpp                 # Error codes + result<T> (C++20 expected-like)
│   │   └── sources/
│   │       ├── fd-byte-source.hpp    # POSIX/semihosting file descriptor backend
│   │       └── memory-byte-source.hpp # In-memory buffer backend
│   └── src/
│       └── elf-image.cpp             # ELF parser implementation
├── include/                          # PUBLIC HEADERS
│   ├── loader/                       # Common core (shared by all loaders)
│   │   ├── detail/elf-hash.hpp
│   │   ├── dynamic-info.hpp
│   │   ├── lazy-resolve.hpp
│   │   ├── region-allocator.hpp
│   │   ├── reloc-applier.hpp
│   │   └── symbol-resolver.hpp
│   ├── scl/                          # Firmware-level common headers
│   │   ├── barrier.h                 # DSB/ISB memory barrier (ARM/no-op off-target)
│   │   ├── image-memory.hpp          # region_allocator over linker arena
│   │   ├── link-context.hpp          # _DYNAMIC self-parsing
│   │   ├── log.h                     # Logging hook (weak no-op defaults)
│   │   └── pic-trampoline.h          # Enter library with r10 = GOT base
│   ├── static/                       # ELF32 static patcher API
│   │   ├── elf-reader.h
│   │   ├── logger.h
│   │   ├── module.h
│   │   ├── shlib-loader.h
│   │   └── tls.h
│   └── xip/                          # Execute-In-Place loader API
│       ├── compact.hpp
│       ├── module.hpp
│       ├── relocate.hpp
│       ├── segment-map.hpp
│       └── static/
│           ├── adapter.hpp
│           └── nopic.hpp
├── src/                              # IMPLEMENTATION
│   ├── dynamic-info.cpp
│   ├── image-memory.cpp
│   ├── lazy-resolve.cpp
│   ├── link-context.cpp
│   ├── log.c
│   ├── region-allocator.cpp
│   ├── reloc-applier.cpp
│   ├── scl-log-put-hex.c
│   ├── static/
│   │   ├── elf-reader.cpp
│   │   ├── logger.cpp
│   │   ├── module.cpp
│   │   ├── shlib-loader.cpp
│   │   └── tls.c
│   ├── symbol-resolver.cpp
│   └── xip/
│       ├── compact.cpp
│       ├── relocate.cpp
│       ├── segment-map.cpp
│       ├── static/
│       │   ├── adapter.cpp
│       │   └── nopic.cpp
│       └── xip-cross-call.c
└── tools/
    ├── build.sh                      # Compile gen-symtab host tool
    ├── gen-symtab                    # Compiled host binary
    ├── gen-symtab.cpp                # C++20 symbol table generator (host tool)
    ├── gen-symtab.py                 # Python symbol table generator (legacy)
    └── gen-symtab-objs.py            # Python symbol extractor from .o files
```

---

## 2. CMake Build Targets

### 2.1 Library Targets (INTERFACE Source Libraries)

All SCL libraries are **INTERFACE source-code libraries** — CMake never produces `.a` archives. Sources propagate to each consuming target, which compiles them with its own flags.

| Target | Alias | Description | ARM-Only |
|--------|-------|-------------|----------|
| `scl-common` | `scl::common` | Shared core: dynamic_info, symbol_resolver, reloc_applier, lazy_resolve, region_allocator, log | No |
| `scl-static` | `scl::static` | Standalone ELF32 static patcher/loader | Yes |
| `scl-xip-common` | `scl::xip-common` | XIP shared core (segment-map, relocate, compact) | No |
| `scl-xip-static` | `scl::xip-static` | XIP adapter for static loader | Yes |

### 2.2 Dependency Graph

```
scl::common
├── common-elf-reader::elf-reader (submodule)
└── [SCL_HOSTED] → relocator.cpp

scl::static → common-elf-reader::elf-reader  (links elf-reader directly, NOT scl::common)

scl::xip-common → scl::common
scl::xip-static → scl::xip-common + scl::static
```

---

## 3. CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `SCL_BUILD_TESTS` | `ON` if top-level project | Enable CTest registration for firmware test suites |
| `SCL_HOSTED` | `ON` on host, `OFF` when cross-compiling | Build hosted parts requiring `std::vector` (relocator) |
| `COMMON_ELF_READER_DIR` | `./common-elf-reader` | Path to the ELF32 reader/parser submodule |
| `SCL_HF` | `OFF` | Hard-float ABI (hosted toolchain only) |
| `SCL_ARM_TOOLCHAIN_BIN` | xPack GCC 15.2.1 path | Directory containing arm-none-eabi-gcc/g++/objcopy |

---

## 4. Toolchain Files

### 4.1 `cmake/arm-none-eabi-hosted.cmake` — Hosted Firmware

**Purpose:** Cortex-M4 hosted firmware (CMSIS/HAL/newlib — static loader apps)

| Setting | Value |
|---------|-------|
| `CMAKE_SYSTEM_NAME` | Generic |
| `CMAKE_SYSTEM_PROCESSOR` | arm |
| `CMAKE_TRY_COMPILE_TARGET_TYPE` | STATIC_LIBRARY |

**Compiler Flags:**

```
MCU (soft): -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
MCU (hard): -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
C:      ${MCU} -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3
CXX:    ${C} -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -fno-move-loop-invariants
```

**Key Characteristics:**
- Soft-float by default (`-DSCL_HF=ON` for hard float)
- Non-PIC
- Exceptions/RTTI OFF
- nano-spec friendly

### 4.2 Toolchain Location Override

```bash
# Option 1: CMake variable
cmake -DSCL_ARM_TOOLCHAIN_BIN=/path/to/bin ...

# Option 2: PATH
export PATH=/path/to/arm-none-eabi-gcc/bin:$PATH
```

Default: `/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin`

---

## 5. Compile-Time Constants (#define)

### 5.1 SCL-Defined Constants

| Constant | Defined By | Guard/Condition | Purpose |
|----------|------------|-----------------|---------|
| `SCL_HOSTED=1` | CMake (`scl-common`) | `SCL_HOSTED` option ON | Enables `std::vector` parts: `relocator`, `elf_image::collect_relocations()` |

### 5.2 Application-Defined Constants

| Constant | Typical Definition | Purpose |
|----------|-------------------|---------|
| `SCL_SO_DIR` | `"/absolute/path/to/vendor/libs/build"` | Directory of static-app fixture `.so` files |
| `HARD_FLOAT` | `1` | Enables hard-float test set |
| `DEBUG` | `1` | CMSIS/HAL debug mode |
| `USE_FULL_ASSERT` | `1` | CMSIS/HAL assertion support |
| `OS_USE_SEMIHOSTING` | `1` | Enable semihosting output |
| `TRACE` | `1` | Enable trace output |
| `OS_USE_TRACE_SEMIHOSTING_DEBUG` | `1` | Semihosting trace channel |
| `STM32F411xE` | `1` | STM32 device variant |
| `USE_HAL_DRIVER` | `1` | Use ST HAL library |
| `HSE_VALUE` | `8000000` | External oscillator frequency (Hz) |

### 5.3 Compile-Time Constants in Headers

| Constant | File | Value | Purpose |
|----------|------|-------|---------|
| `kMaxBlocks` | `loader/region-allocator.hpp` | `16` | Max live spans in region allocator |
| `kAlign` | `loader/region-allocator.hpp` | `8` | Allocation granularity (bytes) |

---

## 6. Linker Scripts

### 6.1 Application Linker Script Requirements

| Loader Type | Required Linker Script Elements |
|-------------|--------------------------------|
| `scl::static` | `_reloc` table (address/size pairs for .rel.dyn, .dynsym, .dynstr, .hash, .inits); mem-sh.ld + libs-sh.ld + sections-dyn.ld |
| `scl::xip-*` | sections-flatfs-hosted.ld; xip-split.ld |

### 6.2 Embedded Symbol Table Generation

**`cmake/embed-symtab.cmake`**

Generates `symtab.c` from object files before linking, avoiding circular dependencies.

```cmake
include(embed-symtab)
embed_symtab(target_name)
```

**Tools:**
- `tools/gen-symtab.cpp` — C++20 host tool (recommended): extracts symbols from linked firmware ELF, generates binary table for `.scl_symtab` section. Uses `common-elf-reader` for parsing. Filters compiler-internal symbols via whitelist to reduce table from ~87 KB to ~7.5 KB.
- `tools/gen-symtab.py` — Legacy Python: extracts symbols from ELF via `nm`, generates C table
- `tools/gen-symtab-objs.py` — Python: extracts symbols from .o files, generates C table

The C++20 host tool is compiled automatically by the build system (see `test/cmake/scl-test-app.cmake`). It runs as a POST_BUILD step, parsing the linked ELF and injecting the binary table via `objcopy --update-section .scl_symtab`.

Both Python tools generate a C file with `{name, address}` pairs and a `symtab_lookup()` function.

---

## 7. Build Recipes

### 7.1 Consuming SCL in a New Application

```cmake
add_subdirectory(path/to/static-shared scl)
target_link_libraries(my-fw PRIVATE scl::static)
```

### 7.2 Application Link-Time Contracts

#### `scl::static` (Hosted)

```cmake
# Link flags
target_link_options(my-fw PRIVATE
    --specs=nano.specs
    -u _printf_float
)

# Link libraries
target_link_libraries(my-fw PRIVATE
    stdc++ m
)

# Linker script must emit: _reloc table
```

### 7.3 Shared Object (.so) Build Flags

```bash
arm-none-eabi-g++ -fpic -msingle-pic-base -mpic-register=r10 -nostdlib -shared \
    -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
    -T gcc.ld -o mylib.so mylib.cpp
```

---

## 8. Source File → Library Mapping

### 8.1 `scl-common` Sources

| File | Description |
|------|-------------|
| `src/dynamic-info.cpp` | _DYNAMIC segment parsing |
| `src/lazy-resolve.cpp` | Lazy GOT.PLT slot resolution |
| `src/region-allocator.cpp` | Fixed-arena slot allocator |
| `src/reloc-applier.cpp` | Allocation-free relocation application |
| `src/symbol-resolver.cpp` | SysV hash table lookup |
| `src/log.c` | Weak no-op logging defaults |
| `src/scl-log-put-hex.c` | Hex value printing helper |
| `src/relocator.cpp` | [SCL_HOSTED] std::vector relocation plan builder |

### 8.2 `scl-static` Sources

| File | Description |
|------|-------------|
| `src/static/elf-reader.cpp` | Thin adapter over common-elf-reader |
| `src/static/shlib-loader.cpp` | Segment load + GOT patching |
| `src/static/logger.cpp` | Loader-side logging |
| `src/static/module.cpp` | Module management |
| `src/static/tls.c` | Single/multi-thread TLS area + global-dynamic `__tls_get_addr` |

### 8.3 `scl-xip-*` Sources

| File | Target | Description |
|------|--------|-------------|
| `src/xip/segment-map.cpp` | xip-common | Segment classification (R-X vs RW) |
| `src/xip/relocate.cpp` | xip-common | XIP split-base relocation + R-X immutability scan |
| `src/xip/compact.cpp` | xip-common | Flash-compaction decision |
| `src/xip/xip-cross-call.c` | (shared) | Shared cross-library call trampoline (parameterized by XIP_CROSS_LOOKUP/XIP_CROSS_FN macros) |
| `src/xip/static/adapter.cpp` | xip-static | Static loader XIP adapter (r10, flash R-X) |
| `src/xip/static/nopic.cpp` | xip-nopic | Fixed-address `-fno-pic` adapter (no r10) |
| `src/xip/xip-cross-call.c` | xip-static | Shared cross-call trampoline (compiled with static defaults) |

---

## 9. Required Tools

| Tool | Version | Purpose | Override |
|------|---------|---------|----------|
| `arm-none-eabi-gcc` | 15.2.1 (xPack) | C compiler | `SCL_ARM_TOOLCHAIN_BIN` or PATH |
| `arm-none-eabi-g++` | 15.2.1 (xPack) | C++ compiler | `SCL_ARM_TOOLCHAIN_BIN` or PATH |
| `arm-none-eabi-objcopy` | 15.2.1 (xPack) | Binary conversion | `SCL_ARM_TOOLCHAIN_BIN` or PATH |
| `arm-none-eabi-nm` | 15.2.1 (xPack) | Symbol extraction | PATH |
| `cmake` | ≥ 3.20 | Build system | — |
| `python3` | ≥ 3.x | Symbol table generation | — |
| `openocd` | 0.12.0 | Flashing/debugging | `OPENOCD` env var |

---

## 10. Error Codes

Defined in `common-elf-reader/include/loader/error.hpp`:

| Code | Value | Description |
|------|-------|-------------|
| `OK` | 0 | No error |
| `FILE_NOT_FOUND` | 1 | Byte source could not be opened |
| `SHORT_READ` | 2 | Fewer bytes than requested returned |
| `NOT_ELF` | 3 | Magic number is not 0x7f 'E' 'L' 'F' |
| `NOT_ARM32` | 4 | Not little-endian 32-bit ARM |
| `NOT_SHARED_OBJECT` | 5 | e_type is not ET_DYN |
| `BAD_SECTION` | 6 | Required section missing or malformed |
| `SYMBOL_NOT_FOUND` | 7 | Hash lookup failed |
| `UNSUPPORTED_RELOC` | 8 | Relocation type not implemented |
| `OUT_OF_MEMORY` | 9 | Fixed-capacity container overflowed |

---

## 11. Language Requirements

| Component | Standard | Notes |
|-----------|----------|-------|
| SCL common core | C++20 | Uses `std::span`, `std::string_view`, `std::optional`, `std::bit` |
| SCL hosted parts | C++20 | Uses `std::vector`, `std::unique_ptr` |
| Log helpers | C11 | `log.c`, `scl-log-put-hex.c` |

---

## 12. Quick Reference

### Configure & Build (Library Only)

```bash
# The SCL is consumed as a subdirectory — not built standalone
# In your application's CMakeLists.txt:
add_subdirectory(path/to/static-shared)
target_link_libraries(my-app PRIVATE scl::static)
```

---

*Generated from SCL codebase analysis — `/home/dan/Documents/static-shared`*
