# Cortex-M Static Loader (SCL) Codebase Analysis

This document provides a comprehensive codebase and architectural analysis of the **Cortex-M Source Code Libraries (SCL)** static loader family, designed for loading shared libraries on ARM Cortex-M microcontrollers (with a reference target of the WeAct BlackPill STM32F411CE board).

---

## 1. Architecture & Core Design

The SCL project acts as a bare-metal, micro-scale equivalent of Linux's dynamic linker (`ld.so`/`dlopen`). It is built as a set of **INTERFACE source libraries** using CMake. Rather than compiling into static `.a` archives or dynamic `.so` libraries, SCL propagates its C/C++ source files directly to the consuming firmware target. This ensures the firmware compiles the loader code with its own compiler flags (optimizations, CPU flags, floating-point ABI) exactly once.

```text
        ┌───────────────────────────────────────────────┐
        │        Consuming Application Firmware         │
        └───────────────────────┬───────────────────────┘
                                │
                                │ (links)
                                ▼
                        ┌───────────────┐
                        │ scl::static   │
                        └───────┬───────┘
                                │
                                │ (depends)
                                ▼
        ┌───────────────────────────────────────────────┐
        │                  scl::common                  │
        │  (Portable C++20 parsing & resolution core)   │
        │ ┌───────────────────────────────────────────┐ │
        │ │  byte_source       ──  Storage Abstraction │ │
        │ │  elf_image         ──  ELF Parser          │ │
        │ │  symbol_resolver   ──  SysV Hash Lookup    │ │
        │ │  reloc_applier     ──  Eager relocator     │ │
        │ │  LazyResolve      ──  Lazy GOT Binder     │ │
        │ │  region_allocator  ──  RAM Arena Allocator │ │
        │ └───────────────────────────────────────────┘ │
        └───────────────────────────────────────────────┘

        Also depending on scl::common:
        scl::static, scl-xip-common

        XIP adapter targets:
        scl-xip-static         → scl-xip-common + scl::static
```

### 1.1 The Library Blocks

The static loader family has **four** library targets: two core loaders, one shared XIP core, and one XIP adapter target. See `README.md` §2.

1. **`scl::common`**: The shared, portable core. Written in portable C++20, it contains the ELF parsing, symbol resolution, basic relocation mapping, and memory management classes. It compiles and runs both on the host (for unit testing) and on the Cortex-M target.
2. **`scl::static`**: Standalone ELF32 static patcher. Loads PIC `.so`, patches GOT, handles DT_NEEDED dependencies.
3. **`scl-xip-common`**: Shared XIP core (segment-map, relocate, compact, cross-call trampoline).
4. **`scl-xip-static`**: XIP adapter for static patcher.

---

## 2. Shared Core Components (`scl::common`)

The common core defines the interface contract for memory representation and storage.

* **`byte_source`** ([byte-source.hpp](../common-elf-reader/include/loader/byte-source.hpp)): Hides the storage medium. Relies on `read_at(offset, span)` to stream bytes. Example backends:
  * `semihost_file`: Opens a file on the host machine via the ST-Link GDB server interface.
  * `Flashbyte_source`: Maps an address offset to an on-chip, memory-mapped flash location.
  * `Memorybyte_source`: Direct wrapper around a host memory buffer (for testing).
* **`elf_image`** ([elf-image.cpp](../common-elf-reader/src/elf-image.cpp)): Parses 32-bit ELF headers (`elf::Ehdr`) and Program Headers (`elf::Phdr`). Computes memory boundaries using segment virtual addresses:
  $$\text{Image Size} = \max(\text{Segment } v\_addr + \text{Segment } memsz)$$
* **`symbol_resolver`** ([symbol-resolver.cpp](../src/symbol-resolver.cpp)): Resolves symbol names using the SysV ELF hash table (`.hash` section).
* **`reloc_applier`** ([reloc-applier.cpp](../src/reloc-applier.cpp)): Applies relocation entries from `.rel.dyn` and `.rel.plt` directly onto memory spans without heap allocation.
* **`region_allocator`** ([region-allocator.cpp](../src/region-allocator.cpp)): Manages fixed-size RAM slots in a reserved arena for loaded binary segments.

---

## 3. Summary of Test Infrastructure

The project features a highly complete test suite designed for both host validation and target execution:

1. **Host Unit Tests** (6 suites, `SCL_BUILD_TESTS=ON`):
   * Run on the build machine without hardware. Validate the ELF parser, symbol resolver, and relocation applier.

2. **Static Patcher Tests** (`test-static/`):
   * `f411ce-sh`, `static-cpp`, `static-direct`, `drv`: Standalone ELF32 static patcher with various scenarios.

3. **XIP Tests** (`test-xip/`, 5 static targets):
   * `scl-fw-xip-static-semihosting`: Static loader + XIP.
   * `scl-fw-xip-static-direct`: Direct API calls + XIP.
   * `scl-fw-xip-interrupt-static`: XIP + TIM2 interrupt + static.
   * `scl-fw-xip-interrupt-static-direct`: Direct calls + interrupts.
   * `scl-fw-xip-static-spi-flash`: SPI flash → internal flash → XIP.

---

## 4. Integrating SCL into a New Project

To add SCL to an application, link with the namespaced CMake targets:

```cmake
add_subdirectory(cortexm-shared-loader SCL)
target_link_libraries(my_firmware PRIVATE scl::static) # or scl::xip-static
```

### 4.1 Application Requirements
1. **Startup & Platform**: The application must configure CPU clocks, initialize RAM/Flash, and define startup entry points.
2. **Logging Back-end**: Implement the weak logger hooks in your code to capture SCL diagnostics:
   ```c
   #include "scl/log.h"
   void scl_log_puts(const char* s) { my_uart_puts(s); }
   void scl_log_putc(char c)        { my_uart_putc(c); }
   ```

---
