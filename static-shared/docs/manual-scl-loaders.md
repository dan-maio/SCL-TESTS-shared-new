# SCL Loaders Family Reference Manual
## Architecture, API, and Integration Guide for the Static Loader Family

This manual documents the design, implementation files, internal mechanisms, APIs, and firmware integrations for the static loader family within the **Cortex-M Source Code Libraries (SCL)** codebase.

---

## 1. Loader Taxonomy Comparison

The static loader family is designed for bare-metal ARM Cortex-M microcontrollers. Each handles code relocation, indirection, and memory allocation differently:

| Loader | Compilation Model | RAM Allocation | Relocation & Indirection | Unloading Strategy |
| :--- | :--- | :--- | :--- | :--- |
| **`scl::static`** | Non-PIC | Static/Fixed Flash | Pre-linked compiler offsets | None (Permanent) |
| **`scl-xip-*`** | PIC (split R-X/RW) | Flash (text) + RAM (data) | Split-base relocation | Free RAM arena |
| **`scl-xip-nopic`** | Non-PIC (`-fno-pic -mlong-calls`) | Fixed RAM windows (text + data) | Fixed VAs; R_ARM_ABS32 patched at load | none (re-copy on next load) |

> **Note:** The `scl::dynamic-nolib`, `scl::dynamic-newlib`, `scl::overlay`, and `scl::reloc` loader families are out of scope for this subset manual.

---

## 2. scl::static (Static Unify/Loader)

Acts as a link-time dynamic connector when used with pre-linked `.so` libraries
(§3.1), or as a runtime `.so` loader for standalone PIC modules (§3.3). In
either mode it parses ELF tables and binds symbol addresses into the target
memory buffer.

### 2.1. Key Implementation Files
- **`src/static/shlib-loader.cpp`** ([shlib-loader.cpp](../src/static/shlib-loader.cpp)): Links and patches compiled libraries to static host code.
- **`test-drv/src/main.cpp`** ([main.cpp](../test-drv/src/main.cpp)): Demonstrates runtime-only loading without build-time linkage (see §3.3).

### 2.2. Mechanism
1. **ELF Tables Parsing**: Reads section relocation tables during build time.
2. **Absolute Patching**: Modifies target offsets in the flash binary block directly, bypassing the need for runtime GOT tables or register configurations.

---

## 3. Test Index

All test firmware images are built with `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake`.

### 3.1. Static Unify Tests (`test-static/`)

| Target | Linker | Run |
|--------|--------|-----|
| `f411ce-sh` | ELF32 static patcher (loads `libsh1/2.so`) | `cd test-static/test-static && ./build-and-run-cmake.sh` |
| `f411ce-sh` (cpp) | static patcher + C++ EH/RTTI | `cd test-static/test-static-cpp && ./build-and-run-*.sh` |

### 3.1b. No-pic XIP tests (`test-xip/`, `scl-xip-nopic`)

| Target | App | Run |
|--------|-----|-----|
| `static-no-pic` | `scl-fw-xip-static-no-pic` (resolve + plain pointers) | `./test-xip/build-and-run.sh <weact\|nucleo\|qemu> static-no-pic` |
| `static-no-pic-simple` | `scl-fw-xip-static-no-pic-simple` (all calls **by name**) | `./test-xip/build-and-run.sh <weact\|nucleo\|qemu> static-no-pic-simple` |

### 3.2. SPI Flash Detail

See [SPI.md](test/SPI.md) for the full W25Q64 driver comparison (bitbang vs HAL), clock tree, filesystem format, and the complete problem/solution log.

### 3.3. Mixed Static-Loader + FlatFS Test (`test-drv/`)

**What it does**

`test-drv` proves the `scl::static` loader can load a PIC `.so` from a host
filesystem (semihosting) into RAM, then use its exported SPI-flash functions
to provision two more `.so` files onto a W25Q64. Those `.so` files are later
read back from the SPI flash, loaded into a separate RAM buffer via the
identical in-memory ELF parser (`load_from_buf`), and their exported
functions called — all without any compile-time linkage between the main
firmware and the loaded modules.

| Step | Action | Loader |
|------|--------|--------|
| 1 | `spi.so` loaded from semihosting → `spi_init()`, `spi_format()` init the W25Q64 | `scl::static` (`load_so_into`) |
| 2 | `uart.so`, `led.so` loaded from semihosting → raw bytes saved to flatfs on W25Q64 via `spi_write()` | Semihosting `read()` only (no patching) |
| 3 | `uart.so` raw bytes read from flatfs → `load_from_buf()` copies LOAD segments to `g_exec` → symbols resolved → `uart_init()`, `uart_puts()`, `uart_discard()` called | Custom in-memory ELF parser (`load_from_buf`) |
| 4 | Same for `led.so` → `led_init()`, `led_blink()`, `led_discard()` | Same as 3 |

**What it teaches us**

1. **No build-time linkage needed.** The `scl::static` loader works fine without
   `-lspi -luart -lled` at link time. Symbols are resolved by walking `.dynsym`
   in the loaded RAM buffer — no `.rel.dyn` or `_reloc` block required on the
   main side (for zero-reloc PIC objects).

2. **Separate RAM buffers are essential.** A single shared buffer (`g_img`)
   caused dangling function pointers when a new `.so` overwrote the previous
   one's code. The fix uses a permanent buffer (`g_spi_img`) for the SPI
   driver and separate temp/exec buffers (`g_img`, `g_exec`) for provision
   and runtime loading.

3. **Zero-reloc PIC `.so` files don't need patching.** `readelf -r spi.so`
   shows zero relocations. Functions that use only hardcoded peripheral
   register addresses compile to position-independent code without any
   GOT/PLT — `patch_shlib()` returns 0 patched symbols, which is correct.

4. **In-memory ELF parsing is self-contained.** `load_from_buf()` parses
   ELF headers, program headers, section headers, copies LOAD segments,
   applies `R_ARM_RELATIVE` if present, and resolves exported symbols —
   all from a raw buffer, without any file I/O or external loader.

5. **`patch_shlib()` returns `uint32_t` (count), not `bool`.** A return
   value of 0 means "nothing to patch", not "failure". Using `!patch_shlib()`
   as a boolean check caused an early return that skipped symbol resolution.

6. **`elf_image` has a dangling-pointer hazard after move.** The `elf_image`
   class stores a raw `byte_source*` into the `elf_reader`'s heap-allocated
   source. When the `elf_reader` is move-constructed into `shlib_loader`,
   the pointer remains valid (the `unique_ptr` points to the same heap
   object), but any code that reads from `elf_image` after the move reads
   through the same source — which is correct only if the source is still
   alive. The safe pattern is to extract section metadata *before* the move,
   as done in `load_so_into()`.

| Target | Storage | Driver | Run |
|--------|---------|--------|-----|
| `scl-fw-drv` | Semihosting → flatfs on W25Q64 | Bitbang GPIO | `./test-drv/build-and-run.sh` |

> **Note:** `test-drv/` is not under `test/` because it exercises `scl::static`
> as a runtime `.so` loader for `.so` files that are not pre-linked with the
> main firmware — a different scenario from the `test-static/test-static` tests,
> which link `.so` files at build time via `-l`.
