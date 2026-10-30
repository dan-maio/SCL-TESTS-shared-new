# SCL Cortex-M Loaders — Complete Reference Manual

**Full API, Internal Functions, Execution Paths, and Template Applications**

*Version 1.0 — Generated from source tree `shared-lib-loaders/`*

---

## Table of Contents

1. [Overview & Architecture](#1-overview--architecture)
2. [scl::common — Full Shared Core API](#2-sclcommon--full-shared-core-api)
3. [scl::static — Full API & Internals](#3-sclstatic--full-api--internals)
   - 3.3 [Direct Calls via GOT/PLT (Build Options)](#33-direct-calls-via-gotplt-build-options)
4. [scl::xip — Full API & Internals](#4-sclxip--full-api--internals)
5. [common-elf-reader — Full ELF Parsing API](#5-common-elf-reader--full-elf-parsing-api)
6. [Template Application](#6-template-application)
7. [Execution Path / Call Stack Analysis](#7-execution-path--call-stack-analysis)
8. [Text Schemas & Diagrams](#8-text-schemas--diagrams)
9. [Build System](#9-build-system--firmware-tests-and-fixtures)
10. [Advanced Interrupt Handling and Context Switching](#10-advanced-interrupt-handling-and-context-switching)

---

## 1. Overview & Architecture

### 1.1 The Problem

A standard Cortex-M firmware is one frozen binary. To run external code at runtime — plug-ins, downloaded modules, A/B algorithms — you need answers to:

1. **Where do the bytes come from?** (semihosting file, on-chip flash archive, RAM buffer)
2. **Where does the code run?** (RAM arena slot, heap buffer, flash in-place)
3. **How are names connected?** (GOT/PLT, absolute patching, link-time binding, lazy resolution)

The SCL provides a **static-loader family** that implements this answer, plus a shared ELF toolbox (`scl::common`).

### 1.2 The Static-Loader Family

The static-loader family implements the load-time **static patching** approach: it opens a PIC `.so` as a byte stream, parses its ELF headers, copies its load segments to a RAM buffer, and patches every GOT entry with a resolved absolute address — both in the library and in the main firmware. The base loader is augmented by an XIP adapter target that wraps it with execute-in-place support:

| Library | Loads | Code Model | Linking | Memory | Float ABI | Needs Heap? |
|---------|-------|-----------|---------|--------|-----------|-------------|
| **`scl::static`** | ET_DYN `.so` (PIC) | `-fpic -shared` | GOT patching + main patch | Static/fixed | Soft/Hard | Yes |
| **`scl::xip-*`** | ET_DYN `.so` | Split R-X/RW | Split-base relocate | Flash (text) + RAM (data) | Varies | Varies |
| **`scl::xip-nopic`** | ET_DYN `.so` | `-fno-pic -mlong-calls` fixed VAs | R_ARM_ABS32 at load; `patch_main` for by-name calls | Fixed RAM windows | Hard | No |

**XIP adapter targets (wrapping base loaders):**

| Target | Wraps | Description |
|--------|-------|-------------|
| `scl-xip-common` | — | Shared XIP core: segment-map, relocate, compact, cross-call trampoline |
| `scl-xip-static` | `scl::static` | XIP adapter for static patcher (r10, flash R-X) |
| `scl-xip-nopic` | — | Fixed-address `-fno-pic` adapter (no r10); see `docs/xip-static-no-pic.md` |

### 1.3 Source Layout (paths relative to `../`)

```
shared-lib-loaders/
  include/loader/         -- common core headers (dynamic_info, symbol_resolver, etc.)
  include/scl/            -- firmware-level headers (log, trampoline, arena, link-context)
  include/static/         -- static patcher API (shlib-loader.h, elf-reader.h, module.h, tls.h, logger.h)
  include/xip/            -- XIP API (segment-map.hpp, relocate.hpp, compact.hpp)
                           -- XIP adapter: static/adapter.hpp
  src/                    -- common core implementations
  src/port/               -- Cortex-M assembly port layer (pic-trampoline.c)
  src/static/             -- shlib-loader, elf-reader, logger, module, tls
  src/xip/                -- segment-map, relocate, compact, static/{adapter,nopic}.cpp
                           -- xip-cross-call.c (shared cross-library trampoline)
  common-elf-reader/      -- vendored ELF parser: elf_image, byte_source, error/result
  doc/                    -- documentation (this file lives here)
```

### 1.4 File Path Convention

Throughout this manual, file paths are given **relative to `./doc`** (the directory containing this file). For example, `../src/static/shlib-loader.cpp` means `<repo-root>/src/static/shlib-loader.cpp`.

---

## 2. scl::common — Full Shared Core API

> **Location:** `../include/loader/` (headers), `../src/` (implementations), `../common-elf-reader/` (vendored)

scl::common is the **shared toolbox** that every loader links. Fully portable C++20, host-testable.

### 2.0 Architectural Theory: ELF Loading on Cortex-M

**The ELF standard** defines two relevant object types: `ET_REL` (relocatable `.o` files with unresolved symbol references) and `ET_DYN` (dynamic shared objects, `.so`, designed for load-time relocation). On a desktop OS the kernel ELF loader and the dynamic linker (`ld.so`) collaborate to map segments, resolve symbols, and apply relocations. On Cortex-M **there is no OS, no MMU, and no `ld.so`** — the firmware is the kernel, the loader, and the application all in one.

**Why this matters on Cortex-M:**
- **No MMU:** All addresses are physical. Loaded code must be placed at a real RAM address with no virtual-to-physical translation layer. Position-independent code (PIC) uses PC-relative addressing so the same bytes work at any base address — the loader just adds the runtime base to every absolute reference.
- **Thumb-2 only:** Cortex-M cores execute only Thumb-2 (16/32-bit mixed) instructions. All function addresses must have bit 0 set (Thumb bit). The loader must OR-in bit 0 on every resolved function pointer.
- **Hard-float ABI (optional):** Some Cortex-M4F/M7 cores have a single-precision FPU. The calling convention may pass float arguments in VFP registers (`s0`-`s15`). The loader's trampolines must save/restore these registers around cross-module calls.
- **No ELF loader in hardware:** The CPU does not understand ELF headers, program headers, or section headers. Every byte of parsing — finding `PT_LOAD` segments, iterating the `_DYNAMIC` array, walking the SysV hash chains — is done in software by the SCL common core.

**The Cortex-M memory model** used by all SCL loaders:

```
FLASH (0x08000000, read-only, execute-in-place):
  .text, .rodata, .init_array, .ARM.exidx  (firmware code and read-only data)

RAM (0x20000000, read-write):
  .data, .bss, stack, heap (mutables state)

ARENA (0x20010000, reserved region in RAM):
  Loaded library image buffers
```

The SCL common core provides the purely computational pieces: parse an ELF from bytes, look up a name in a hash table, apply a relocation list to a writable buffer. Every loader then assembles these pieces into a complete load pipeline suited to its trade-offs.

### 2.1 ELF32 Reader/Parser (vendored)

#### `loader::byte_source` — Storage Abstraction
- **File:** `../common-elf-reader/include/loader/byte-source.hpp:1`
- Abstract random-access byte reader. The core never assumes where object bytes live.
- **Methods:**
  - `virtual bool read_at(uint64_t offset, std::span<std::byte> dest) noexcept = 0` — fill `dest` from `offset`
- **Provided implementations:**
  - `loader::memory_byte_source` — over an in-memory `std::span<const std::byte>` (`../common-elf-reader/include/loader/sources/memory-byte-source.hpp:1`)

#### `loader::elf_image` — ELF File Parser
- **File:** `../common-elf-reader/include/loader/elf-image.hpp:1`, `../common-elf-reader/src/elf-image.cpp:1`
- Validated view over an ELF `.so`: headers, program headers, sections.
- **Static factory:**
  - `static result<elf_image> parse(byte_source& src) noexcept` — parses and validates ELF header
- **Methods:**
  - `const elf::Ehdr& header() const noexcept` — ELF file header
  - `result<void> program_header(half i, elf::Phdr& out) const noexcept` — read i-th program header
  - `result<void> section_header(half i, elf::Shdr& out) const noexcept` — read i-th section header
  - `result<const elf::Shdr*> find_section(string_view name) const noexcept` — find section by name
  - `result<uint32_t> image_size() const noexcept` — total PT_LOAD VA range
  - `result<uint32_t> image_base() const noexcept` — lowest PT_LOAD vaddr
  - *(SCL_HOSTED only)* `result<vector<reloc_entry>> collect_relocations() const noexcept`

#### `loader::elf::` Types — ELF32 Structures
- **File:** `../common-elf-reader/include/loader/elf-types.hpp:1`
- `Ehdr`, `Phdr`, `Shdr`, `Sym`, `Rel`, `Dyn`, `rel_type` enum, `r_type()`, `r_sym()`
- Full ARM ELF32-LE constant definitions

#### `loader::error` / `loader::result<T>` — Error Handling
- **File:** `../common-elf-reader/include/loader/error.hpp:1`
- `enum class error { OK, NOT_ELF, NOT_ARM32, NOT_SHARED_OBJECT, BAD_SECTION, OUT_OF_MEMORY, SHORT_READ, UNSUPPORTED_RELOC, SYMBOL_NOT_FOUND, FILE_NOT_FOUND }`
- `template<typename T> class result<T>` — C++20 expected-like (value or error). Use `loader::unexpected(e)` to construct the failure alternative.

#### `loader::detail::bytes` — Bounded LE Accessors
- **File:** `../common-elf-reader/include/loader/detail/bytes.hpp:1`
- Header-only, bounds-checked little-endian access:
  - `uint32_t load_word(span<const byte>, size_t offset) noexcept`
  - `uint8_t load_byte(span<const byte>, size_t offset) noexcept`
  - `void store_word(span<byte>, size_t offset, uint32_t value) noexcept`
  - `string_view name_at(span<const byte> strtab, uint32_t offset) noexcept` — NUL-terminated string read (never scans past the table)

### 2.2 Dynamic Segment Parser

#### `loader::dynamic_info`
- **File:** `../include/loader/dynamic-info.hpp:1`, `../src/dynamic-info.cpp:1`
- Parses `_DYNAMIC` segment array (DT_HASH, DT_SYMTAB, DT_STRTAB, DT_STRSZ, DT_JMPREL, DT_INIT_ARRAY, ...)
- **Fields:**
  - `uint32_t hash` — DT_HASH value (`.hash` address)
  - `uint32_t symtab` — DT_SYMTAB value (`.dynsym` address)
  - `uint32_t strtab` — DT_STRTAB value (`.dynstr` address)
  - `uint32_t strsz` — DT_STRSZ value
- **Static factory:**
  - `static result<dynamic_info> parse(span<const byte> bytes) noexcept` — iterate DYN entries until DT_NULL
- **Error:** `error::BAD_SECTION` if hash/symtab/strtab all missing

### 2.3 Symbol Resolution

#### `loader::elf_hash`
- **File:** `../include/loader/symbol-resolver.hpp:1`, `../src/symbol-resolver.cpp:1`
- **Signature:** `[[nodiscard]] uint32_t elf_hash(string_view name) noexcept`
- Classic SysV ELF hash of a symbol name

#### `loader::symbol_resolver`
- **File:** `../include/loader/symbol-resolver.hpp:1`, `../src/symbol-resolver.cpp:1`
- Lookup symbols in a (`.dynsym`, `.dynstr`, `.hash`) triple
- **Constructor:**
  - `symbol_resolver(span<const byte> dynsym, span<const byte> dynstr, span<const byte> hash) noexcept`
- **Methods:**
  - `result<uint32_t> address_of(string_view name) const noexcept` — returns **image-relative** symbol value (0 if undefined)

### 2.4 Relocation

#### `loader::reloc_applier`
- **File:** `../include/loader/reloc-applier.hpp:1`, `../src/reloc-applier.cpp:1`
- Allocation-free relocation streaming — writes into loaded image directly (freestanding-safe)
- **Struct: `loader::symbol_source`** — function pointer + context for external resolution
  - `result<uint32_t> (*fn)(void* ctx, string_view name)` — callback
  - `result<uint32_t> operator()(string_view name) noexcept` — invokes callback
- **Static method:**
  ```cpp
  static result<void> apply_rel(
      span<byte> image,         // writable loaded image (vaddr 0 = image[0])
      uint32_t base,            // runtime address of image[0]
      span<const byte> reldyn,  // .rel.dyn table (elf::Rel array)
      span<const byte> dynsym,  // .dynsym (elf::Sym array)
      span<const byte> dynstr,  // .dynstr
      symbol_source resolve     // external symbol resolver
  ) noexcept;
  ```
- **Handles:** `R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`, `R_ARM_ABS32`, `R_ARM_REL32`, `R_ARM_COPY`

### 2.5 Lazy Binding

#### `loader::lazy_tables`
- **File:** `../include/loader/lazy-resolve.hpp:1`
- **Fields:**
  - `span<const byte> jmprel` — JMPREL (JUMP_SLOT relocs, elf::Rel array)
  - `span<const byte> dynsym` — `.dynsym`
  - `span<const byte> dynstr` — `.dynstr`

#### `loader::lazy_resolve_slot`
- **File:** `../include/loader/lazy-resolve.hpp:1`, `../src/lazy-resolve.cpp:1`
- **Signature:**
  ```cpp
  [[nodiscard]] result<uint32_t> lazy_resolve_slot(
      uint32_t got_off,         // offset of GOT slot within image
      const lazy_tables& t,     // JMPREL + dynsym + dynstr tables
      symbol_source resolve     // resolver for external names
  ) noexcept;
  ```
- Scans JMPREL for entry with `r_offset == got_off`, reads symbol name, resolves, returns absolute target with Thumb bit set.

### 2.6 Memory Management

#### `loader::region_allocator`
- **File:** `../include/loader/region-allocator.hpp:1`, `../src/region-allocator.cpp:1`
- Fixed-capacity first-fit free-list allocator over an external region. 8-byte aligned, coalesces adjacent free blocks.
- **Constants:** `kMaxBlocks = 16`, `kAlign = 8`
- **Constructor:** `region_allocator(void* base, size_t size) noexcept`
- **Methods:**
  - `result<void*> alloc(size_t n) noexcept` — allocate (rounded up to kAlign)
  - `void free(void* p) noexcept` — free (no-op for nullptr)
- **Internal:**
  - `struct Block { uint32_t off, len; bool used, live; }`
  - `int find_free_slot() noexcept` — find reusable slot index
  - `void coalesce() noexcept` — merge adjacent free spans

### 2.7 Firmware-Level Helpers

#### `fw::image_arena()`
- **File:** `../include/scl/image-memory.hpp:1`, `../src/image-memory.cpp:1`
- **Signature:** `loader::region_allocator& fw::image_arena() noexcept`
- Returns singleton allocator over linker-defined `_simage_arena_start` / `_simage_arena_end`
- Used by `scl::xip-static` to allocate image buffers from the reserved RAM arena

#### `fw::link_context`
- **File:** `../include/scl/link-context.hpp:1`, `../src/link-context.cpp:1`
- Provides main firmware's exported symbol table (parsed from firmware's own `_DYNAMIC`)
- **Static factory:**
  - `static result<link_context> from_self() noexcept` — reads `_DYNAMIC` at a fixed link-time address
- **Methods:**
  - `symbol_source source() const noexcept` — get resolver callback
  - `result<uint32_t> resolve(string_view name) const noexcept` — absolute address in main

#### `scl_log` Hook
- **File:** `../include/scl/log.h:1`, `../src/log.c:1`
- **Signatures:**
  - `void scl_log_puts(const char* s)` — emit NUL-terminated string (weak no-op default)
  - `void scl_log_putc(char c)` — emit single character (weak no-op default)
- All SCL diagnostics go through this. Override with strong definitions for tracing.

#### `pic_call_void`, `pic_call_u32`, `pic_call_u32_2`
- **File:** `../include/scl/pic-trampoline.h:1`
- Declared extern "C" — implementations in `../src/port/pic-trampoline.c:1`
- Enter a loaded library with `r10 = got` (the GOT base register)
- **Signatures:**
  - `void pic_call_void(void* fn, void* got)`
  - `uint32_t pic_call_u32(void* fn, void* got, uint32_t a0)`
  - `uint32_t pic_call_u32_2(void* fn, void* got, uint32_t a0, uint32_t a1)`

---
## 3. scl::static — Full API & Internals

> **Purpose:** Standalone ELF32 static patcher. Loads PIC `.so`, GOT-patches for direct calling, handles DT_NEEDED dependencies.
> **Files:** `../include/static/`, `../src/static/`

### 3.0 Architectural Theory: Static Patching Without an OS

**The problem:** The `dynamic-nolib` and `dynamic-newlib` loaders both assume the firmware is compiled `-fpic` (position-independent) so that loaded libraries can resolve symbols against the firmware's `.dynsym`. But what if the firmware is **not** PIC? Or what if you want a loader that can be bolted onto an existing non-PIC firmware project without recompiling it with `-fpic -Wl,-E`?

**The `scl::static` approach:** Instead of the firmware exporting symbols through a dynamic symbol table, the **loader** is given explicit pointers to the sections it needs to patch (`_reloc` table). It opens the `.so` file as a byte stream (over semihosting), parses its ELF headers, copies its load segments to a RAM buffer, then walks the **library's** `.rel.dyn` and patches every GOT entry with the resolved address. It also walks the **firmware's** `.rel.dyn` and patches the firmware's own references to library symbols.

**Two-stage patching:**

1. **`patch_shlib`** iterates the library's `.rel.dyn`:
   - `R_ARM_RELATIVE`: adds the load bias (`base + A`, where A is the addend)
   - `R_ARM_GLOB_DAT` / `R_ARM_JUMP_SLOT`: looks up the symbol name via SysV hash in the library's own `.dynsym`/`.dynstr`, then resolves against the firmware's `.dynsym` (or a caller-supplied `symbol_table`). The resolved absolute address is written into the library's GOT.
   - `R_ARM_ABS32`: resolves the symbol and adds the addend, writes into the library's section.

 2. **`patch_main`** iterates the firmware's `.rel.dyn`:
    - `R_ARM_COPY`: the library exports a global variable that the firmware has a COPY relocation for. The loader `memcpy`s the library's initialised value into the firmware's `.bss` allocation for that variable.
    - `R_ARM_GLOB_DAT`: the firmware's GOT entry for a library-exported symbol is patched with the absolute address in the library's RAM buffer.
    - **Registration in `global_syms`:** every successfully resolved GOT slot is also registered in the caller-provided `symbol_table` (`global_syms->add_global`). This makes the resolved address visible to **subsequently loaded modules** — for example, `uart_puts` resolved from the first `.so` load becomes available when a second `.so` (e.g. a C++ fixture) needs it.
    - Weak symbols: resolved later via `resolve_weak_function()`, which lazily looks up the name when first called.

**DT_NEEDED dependency resolution:**

The `.so` file may list dependencies via `DT_NEEDED` entries. The loader's `resolve_dependencies()` method calls a caller-supplied callback (`load_dep(sOName, out_loader)`) which loads each dependency recursively. Each loaded dependency produces its own `shlib_loader` instance, stored in `loaded_deps_` so they outlive the resolution phase.

**COPY relocations on Cortex-M:**

A `R_ARM_COPY` relocation is special: the compiler determines that a global variable imported from a shared library should live in the **main executable's `.bss`** (because the main executable's link-time size for that symbol is authoritative). At load time, the dynamic linker copies the initialised value from the library's `.data` into the main's `.bss` slot. The library's GOT entry for that symbol points to main's copy. This is the standard ELF COPY relocation model, and it works the same on Cortex-M — except there is no OS to ensure the copy happens. The SCL `patch_main` handles it explicitly.

**Why no trampoline:** After `patch_shlib` and `patch_main`, every GOT entry in the loaded library contains a real absolute address. The library's code uses `ldr r3, [r10, #offset]` to read its GOT, so r10 must still be set. However, the calling firmware (which is non-PIC and does not use r10) calls the library through the trampoline `pic_call_u32` — exactly as in `scl::dynamic-nolib`. The difference is that the **resolution** happened eagerly at load time, not lazily at first call.

### 3.1 Public API

#### `class elf_loader::shlib_loader`
- **File:** `../include/static/shlib-loader.h:26`, `../src/static/shlib-loader.cpp:1`

| Method | File:Line | Signature | Description |
|--------|-----------|-----------|-------------|
| `load_bin` | cpp:53 | `loader_error load_bin(uint8_t* buffer, uint32_t bufsize)` | Copy PT_LOAD segments into RAM buffer. Scans DT_NEEDED. |
| `read_elf` | cpp:44 | `loader_error read_elf(string_view file)` | Open and parse ELF file |
| `populate_helper_sections` | cpp:69 | `loader_error populate_helper_sections()` | Locate `.inits`, `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` in both lib and main |
| `get_ram_shlib` | cpp:162 | `uint8_t* get_ram_shlib()` | Returns loaded library's RAM base |
| `get_ram_main` | cpp:166 | `uint8_t* get_ram_main()` | Returns main's RAM base |
| `find_shlib_symbols_to_patch` | cpp:183 | `loader_error find_shlib_symbols_to_patch(span<symbol_patch>)` | Build patch array for library symbols |
| `find_main_symbols_to_patch` | cpp:200 | `loader_error find_main_symbols_to_patch(span<symbol_patch>)` | Build patch array for main symbols |
| `patch_shlib` | cpp:270 | `uint32_t patch_shlib(symbol_table* global_syms)` | Fix up library GOT: ABS32, RELATIVE, GLOB_DAT, JUMP_SLOT |
| `patch_main` | cpp:381 | `uint32_t patch_main(symbol_table* global_syms)` | Fix up main's references to library symbols: COPY, GLOBAL, WEAK |
| `execute_constructors` | cpp:496 | `loader_error execute_constructors()` | Run `.init_array` constructor functions |
| `resolve_dependencies` | cpp:489 | `loader_error resolve_dependencies(symbol_table*, load_dep_callback)` | Auto-load DT_NEEDED deps |
| `resolve_weak_function` | h:62 | `unsigned int resolve_weak_function(funcm f)` | Lazy weak-function dispatch |

#### `class elf_loader::elf_reader`
- **File:** `../include/static/elf-reader.h:1`, `../src/static/elf-reader.cpp:1`
- Thin adapter over shared `loader::elf_image`
- **Methods:** `find_symbol_address(name)`, `pass_args()`, helper section maps

#### Cache-coherence barrier — `scl_memory_barrier()`
- **File:** `../include/scl/barrier.h:4`
- DSB 0xf + ISB 0xf on ARM; no-op otherwise
- The static loader calls this after patching shlib and main symbols (`shlib-loader.cpp:394`, `:500`). It replaced the former per-file `sync_after_patching()` helper, which the refactor removed in favour of the shared `scl/barrier.h` macro. *(Note: a stale `void sync_after_patching();` declaration still lingers in `../include/static/shlib-loader.h:117` — dead, unused.)*

### 3.2 Internal Data Structures

```cpp
// ../src/static/shlib-loader.cpp:41
struct init_lds {
    uint32_t reldyn, reldyn_size;     // .rel.dyn address + size
    uint32_t dynsym, dynsym_size;     // .dynsym address + size
    uint32_t dynstr, dynstr_size;     // .dynstr address + size
    uint32_t hash, hash_size;         // .hash address + size
    uint32_t inits, inits_size;       // .init_array address + size
};
```

- `section_map m_shlib{}`, `section_map m_main{}` — cached section locations (each is `helper_section[6]`, since `helper_section_idx::COUNT = 6`)
- `weak_sym_cache_` — vector of `weak_main_sym` for late weak-function resolution
- `loaded_deps_` — vector of unique_ptr to auto-loaded DT_NEEDED dependencies

### 3.3 Direct Calls via GOT/PLT (Build Options)

The firmware can call loaded `.so` functions **directly** — `spi_init()`,
`uart_puts("hello")` — without `dlsym` or `Module::sym()`. The linker
generates PLT stubs and GOT slots; the runtime loader (`patch_main()`)
patches the GOT with the resolved addresses.

#### Required Build Options

| Setting | Why |
|---------|-----|
| **`extern "C"` in headers** | C++ name mangling (`_Z8spi_initv`) won't match the `.so`'s C symbols (`spi_init`). All headers shared between firmware and `.so` must wrap declarations in `extern "C" { }` |
| **`target_link_libraries` with `.so` paths** | The linker creates `.dynsym`, `.rel.dyn`, `.plt`, `.got` sections. CMake bare-metal filters `-l` flags; use absolute `.so` paths in `target_link_libraries` instead |
| **`patch_main()` in loader pipeline** | `shlib_loader::patch_main()` writes each resolved symbol's address into the main firmware's GOT slot. `Module::build()` calls this automatically |
| **GOT copy at startup** | The `Reset_Handler` in `exception-handlers.c` copies `.got` / `.got.plt` from flash to RAM before `main()` runs. The linker script provides `_sgotdata`/`_sgot`/`_esgot` symbols |
| **Linker script with `.got`, `.got.plt`, `.plt`** | `sections-sh.ld` places `.got` / `.got.plt` in RAM (`> RAM AT> FLASH`) and `.plt` in FLASH |

#### What the Linker Produces

```
.dynsym    → 13 entries (spi_init, uart_puts, led_blink, …)
.rel.dyn   → 12 R_ARM_JUMP_SLOT entries (one per function)
.plt       → Thumb PLT stubs in FLASH (0x0800e274)
.got       → GOT slots in RAM (0x20000124), initialised with PLT addresses
.got.plt   → PLT GOT in RAM (0x20000160)
.dynamic   → DT_NEEDED entries for libspi.so, libuart.so, libled.so
```

`patch_main()` iterates `.rel.dyn`, looks up each symbol in the loaded
`.so`'s `.dynsym`, and writes the runtime address into the GOT slot.
After `Module::load_*()` returns, every PLT call goes straight to the
correct function in RAM.

#### CMake Snippet

```cmake
add_executable(scl-fw-drv …)
target_link_libraries(scl-fw-drv PRIVATE scl::static
    ${DRV_DIR}/fixtures/libspi.so
    ${DRV_DIR}/fixtures/libuart.so
    ${DRV_DIR}/fixtures/libled.so
    ${DRV_DIR}/fixtures/libcpp_features.so)   # C++ fixture
# No -fPIC, no --no-dynamic-linker, no -rdynamic needed.
# The .so files must be named lib*.so for the linker to find them.

# C++ fixture build recipe:
set(LIB_CXXFLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -fPIC -fno-inline-functions -fno-common -O0 -g3 -Wall -Wextra
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
add_custom_target(build_cpp_features
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS}
            -c cpp_features.cpp -o cpp_features.o
    COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} cpp_features.o
            -o libcpp_features.so -shared -nostartfiles -nostdlib
            -Wl,--gc-sections,-S   # -S strips debug sections at link time
    COMMENT "Building libcpp_features.so")
```

**Note:** `-Wl,-S` strips debug sections (`.debug_*`) from the `.so`, reducing a C++ fixture from ~87 KB to ~14 KB — essential when loading from a small on-flash filesystem buffer.

#### main.cpp Usage

```cpp
#include "spi.h"    // extern "C" void spi_init(void);
#include "uart.h"   // extern "C" void uart_puts(const char*);
#include "led.h"    // extern "C" void led_blink(uint32_t);
#include "cpp_features.h"

// Local new/delete so the linker does NOT resolve _Znaj/_ZdlPv from
// libcpp_features.so's PLT — these operators must be callable before
// the .so is loaded (init_syms → symbol_table → new[]).
void* operator new(size_t size)                  { return malloc(size); }
void* operator new[](size_t size)                { return malloc(size); }
void  operator delete(void* p) noexcept           { if (p) free(p); }
void  operator delete[](void* p) noexcept         { if (p) free(p); }
void  operator delete(void* p, size_t) noexcept   { if (p) free(p); }
void  operator delete[](void* p, size_t) noexcept { if (p) free(p); }

static elf_loader::symbol_table g_syms;
static void init_syms() {
    g_syms.add_global("printf",     (uint32_t)printf);
    g_syms.add_global("puts",       (uint32_t)puts);
    g_syms.add_global("malloc",     (uint32_t)malloc);
    g_syms.add_global("free",       (uint32_t)free);
    // …
}

// --- C tests ---
scl::Module mod;
mod.load_file("libspi.so", buf, sz, &g_syms);
spi_init(); spi_format();

// Load uart.so from flash, use it, discard
spi_read(uart_off, g_img, uart_sz);
scl::Module mu;
mu.load_buf(g_img, uart_sz, g_exec, sizeof(g_exec), &g_syms);
uart_init();
uart_puts("UART from flash");

// --- C++ fixture test ---
// Load uart.so again (fresh copy, hardware re-inited)
spi_read(uart_off, g_img, uart_sz);
scl::Module mu2;
mu2.load_buf(g_img, uart_sz, g_exec, sizeof(g_exec), &g_syms);
uart_init();  // re-init after discard, fresh module has initialized=0

// Load C++ fixture at offset 16384 (after uart's code)
spi_read(cpp_off, g_img, cpp_sz);
scl::Module mc;
mc.load_buf(g_img, cpp_sz, g_exec + 16384, sizeof(g_exec) - 16384, &g_syms);

// Direct calls through GOT/PLT (patched by patch_main)
uint32_t r0 = cpp_features_run(0, 10);
uint32_t s  = cpp_features_summary();
cpp_features_print();   // outputs both via printf (semihost) and uart_puts (UART)
```

**Key pattern:** `patch_main()` registers every resolved symbol (e.g. `uart_puts`) in `g_syms`. When a later module (e.g. the C++ fixture) imports `uart_puts`, `patch_shlib()` finds it in `g_syms` and patches the GOT slot correctly — even though `uart_puts` was never in `init_syms()`.

#### Limitations

- PLT stubs are **Thumb code** (Cortex-M4). The linker may emit ARM-mode
  stubs for non-Thumb targets — verify with `objdump -d -j .plt`.
- **CMake for `arm-none-eabi` filters `-l` flags.** Use absolute `.so`
  paths in `target_link_libraries`, not `-lspi`. For non-CMake builds
  (Eclipse, Makefile), `-lsh1 -lsh2` works directly.
- **One GOT slot per symbol, per loaded module.** If the firmware calls
  `spi_init()` but `spi.so` hasn't been loaded yet, the GOT slot
  contains the PLT resolver address (undefined → crash). Always load
  the `.so` before calling its functions.
- **C++ only.** C firmware must use the `Module::sym()` API since C has
  no `extern "C"` mangling distinction.
- **C++ fixture needs local new/delete.** When a C++ `.so` defines
  `operator new[]` / `operator delete[]`, the linker resolves the
  firmware's own `new[]`/`delete[]` to the `.so`'s PLT. This creates a
  bootstrap problem: the symbol table itself uses `new[]` in
  `init_syms()` — before the `.so` is loaded. Define the operators
  locally in the firmware (wrapping `malloc`/`free`) to break the cycle.
- **`.init_array` not generated with `-nostdlib`.** The
  `__attribute__((constructor))` directive does not produce
  `.init_array` entries when the `.so` is compiled with `-nostartfiles
  -nostdlib`. Use explicit exports and call constructors by name, or
  provide a custom `.inits` section via the linker script.

---
## 4. scl::xip — Full API & Internals

> **Purpose:** Execute-In-Place loaders. Keep R-X segments in flash, copy only RW to RAM.
> **Files:** `../include/xip/`, `../src/xip/`

### 4.0 Architectural Theory: Execute-In-Place on Cortex-M

**The problem with RAM-only loading:** Every dynamic loader above copies the **entire** library into RAM — both the code (R-X, read-execute) and the data (RW, read-write). On a Cortex-M with 128 KB of RAM this is expensive: a 48 KB library consumes 37% of RAM just for code that never changes. Can we execute code **directly from flash** and only copy the mutable data to RAM?

**XIP theory (Harvard vs von Neumann):** Desktop CPUs use a von Neumann architecture with a unified address space but separate caches for instructions and data. Cortex-M uses a **modified Harvard** architecture: flash is on a separate bus from RAM, but both are accessible from a single 32-bit address space (flash at `0x08000000`, RAM at `0x20000000`). The CPU can fetch instructions from either bus — it does not need code to be in RAM. This makes true XIP (execute from flash) feasible without an MMU.

**What prevents a loaded `.so` from executing directly from flash:**

1. **Segment fusion:** Many toolchains emit a single fused `PT_LOAD` segment containing both `.text` (R-X) and `.data`/`.got` (RW) with `RWX` permissions. A single fused segment cannot XIP because the writable part (GOT, data) is interleaved with read-only code — you would have to make the entire flash region writable, which violates the flash hardware protection.

   **Solution:** The `xip::classify_segments()` function requires exactly **two** `PT_LOAD` segments: one with `PF_X & !PF_W` (R-X, the flash candidate) and one with `PF_W` (RW, always copied to RAM). This is achieved by linking the `.so` with a **split linker script** (`xip-split.ld`) that forces code and data into separate segments.

2. **Relocations targeting the text segment:** Even with cleanly split segments, some relocations may write into the R-X range (e.g., the compiler emitted a relocation in `.text` that the loader must patch). Flash is read-only at runtime — any relocation targeting the R-X segment disqualifies XIP.

   **Solution:** `xip::text_is_immutable()` scans every `SHT_REL` section and verifies that every `r_offset` points to a writable section (`SHF_WRITE`). If even one relocation targets a non-writable section (the R-X range), the module cannot XIP and must be copied whole to RAM.

3. **PC-relative GOT addressing:** The PLT stubs in flash compute their GOT slot address PC-relative: `add r12, pc, #offset`. This works only if the GOT's virtual address equals its file offset — i.e., `data_vaddr == data_off`. The split linker script must honour this constraint.

   **Solution:** `xip::classify_segments()` additionally checks `VA == file_offset` for both segments. Without this, the PC-relative calculation in flash would compute a wrong RAM address for the GOT slot.

**The XIP memory model:**

```
FLASH (memory-mapped, read-only):
  ┌──────────────────────────────┐
  │ R-X segment (.text, .rodata, │
  │ .plt, .got)                  │  ← executed here, in place
  │ text_base = flash_addr       │
  │ text_vaddr == text_off       │
  └──────────────────────────────┘

RAM (allocated by loader):
  ┌──────────────────────────────┐
  │ RW segment (.data, .bss,     │  ← only mutable part copied
  │ .got.plt, .data.rel.ro)      │     to RAM
  │ data_base = ram_addr         │
  │ data_vaddr == data_off       │
  │                              │
  │ got_base = data_base +       │
  │   (.got_vaddr - data_vaddr)  │  ← r10 points here
  └──────────────────────────────┘
```

**Split-base relocation (`xip::apply_rel_xip`):**

With one load bias (standard PIC loading), `R_ARM_RELATIVE` adds a single base: `*P = A + base`. With XIP there are **two bases**: `text_base` (flash address of the R-X segment) and `data_base` (RAM address of the RW segment). The relocation applier must decide per-addend:

```
if in_text(A) => *P = A + (text_base - text_vaddr)
if in_data(A) => *P = A + (data_base - data_vaddr)
```

The `SegmentMap::in_text()` and `SegmentMap::in_data()` methods provide this classification. Externally defined symbols (imported from the firmware) are routed to the appropriate base based on their `st_value` falling inside the text or data VA range.

**XIP cross-library calls:**

Because the GOT in flash is **read-only** (the PLT reads it PC-relative), the loader cannot pre-patch JUMP_SLOT entries with resolved addresses — the flash GOT is immutable at runtime. Instead, every external JUMP_SLOT is pointed at `xip_cross_call` (a trampoline). At call time, the trampoline:

1. Maps the GOT slot address back to a symbol name (using the `.rel.dyn`/`.rel.plt` tables in flash)
2. Resolves the symbol (other loaded libs first, then main)
3. Returns `{target|Thumb-bit, callee_got}`
4. Swaps r10 to the callee's `got_base` and calls the target

The GOT lookup reads from the **flash-resident** GOT copy (at `flash_data_base`) rather than the RAM copy. The `xip_cross_lookup()` function in `../src/xip/static/adapter.cpp` handles this distinction.

**Impact on RAM and flash:**

| Metric | Copy-whole (no XIP) | XIP (split) | Saving |
|--------|--------------------|-------------|--------|
| RAM per library | 48 KB (code + data) | 4 KB (data only) | **44 KB (92%)** |
| Flash per library | 48 KB (one copy in flash archive) | 48 KB (already in flash, used in-place) | 0 (flash is the same) |
| Load time | 48 KB memcpy | 4 KB memcpy | **92% faster** |
| Cross-call overhead | `dl_cross_call` r10 swap | `xip_cross_call` r10 swap (same) | none |

### 4.1 Core XIP API

#### `struct xip::SegmentMap`
- **File:** `../include/xip/segment-map.hpp:16`
- **Fields:**
  - `uintptr_t text_vaddr, text_off, text_size, text_align` — R-X segment
  - `uintptr_t data_vaddr, data_off, data_filesz, data_memsz` — RW segment
  - `bool split` — false = fused RWX blob, cannot XIP
- **Methods:**
  - `bool in_data(uintptr_t vaddr) const noexcept` — true if vaddr in RW range
  - `bool in_text(uintptr_t vaddr) const noexcept` — true if vaddr in R-X range (and not in data)

#### `xip::classify_segments`
- **File:** `../src/xip/segment-map.cpp:16`
- **Signature:** `bool classify_segments(span<const byte> image, SegmentMap& out) noexcept`
- Parse program headers: find exactly one R-X (PF_X & !PF_W) and one RW (PF_W) PT_LOAD
- Requires `VA == file_offset` for both (PLT PC-relative GOT computation)
- Returns false for fused RWX or multi-segment images

#### `xip::text_is_immutable`
- **File:** `../include/xip/relocate.hpp:23`, `../src/xip/relocate.cpp:1`
- **Signature:** `bool text_is_immutable(span<const byte> image, const SegmentMap& seg) noexcept`
- Scans all SHT_REL sections: verifies every relocation targets a writable section (SHF_WRITE)
- If any reloc targets the R-X range → false (module disqualifies XIP)

#### `xip::apply_rel_xip`
- **File:** `../include/xip/relocate.hpp:30`, `../src/xip/relocate.cpp:1`
- **Signature:**
  ```cpp
  loader::result<void> apply_rel_xip(
      const SegmentMap& seg,     // segment classification
      uintptr_t text_base,       // flash address of text_vaddr
      uintptr_t data_base,       // RAM address of data_vaddr
      span<byte> ram_data,       // RW data in RAM
      span<const byte> reldyn,   // .rel table
      span<const byte> dynsym,   // .dynsym
      span<const byte> dynstr,   // .dynstr
      loader::symbol_source resolve  // external resolver
  ) noexcept;
  ```
- Split-base relocation: R_ARM_RELATIVE addend routed to `text_base` or `data_base` per `in_text()/in_data()`
- Only RW words written (r_offset must be in data segment)

#### `xip::compact_for_xip`
- **File:** `../include/xip/compact.hpp:30`, `../src/xip/compact.cpp:11`
- **Signature:** `bool compact_for_xip(span<const byte> so_image, Module& out) noexcept`
- Checks `classify_segments` (clean split) AND `text_is_immutable` (no writable-target relocs)
- If both pass: `out.text = R-X subspan` (flash), `out.xip = true`
- Otherwise: `out.xip = false` (caller must copy whole to RAM)

### 4.2 XIP Adapter

The static loader's XIP adapter lives in `../src/xip/static/adapter.cpp` (complete 407-line implementation with `bind_host_imports()`, `load()`, `resolve()`, `unload()`, `xip_cross_lookup()`), with its public header at `../include/xip/static/adapter.hpp`.

**Shared components:**
- `../src/xip/xip-cross-call.c` — shared cross-library call trampoline (parameterized by lookup function name via `XIP_CROSS_LOOKUP` / `XIP_CROSS_FN` macros)

The static adapter follows the shared XIP pattern: classify, `apply_rel_xip` (or fallback copy-whole), run ctors with split bases.

### 4.3 XIP Linker Script Architecture & Offset Cancellation (`xip-split.ld`)

To achieve true execute-in-place without copying code to RAM, the shared library must be linked with a specialized linker script (`xip-split.ld`). Standard toolchains merge code and data into a single `PT_LOAD` segment or emit arbitrary file offsets, which breaks XIP.

#### The `PHDRS` Block

The linker script explicitly defines two distinct program headers:

```ld
PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X: PF_R | PF_X (4 + 1 = 5) — stays in flash */
  data PT_LOAD FLAGS(6);   /* RW:  PF_R | PF_W (4 + 2 = 6) — copied to RAM */
}
```

- **`text PT_LOAD FLAGS(5)`**: `FLAGS(5)` specifies `PF_R (4) | PF_X (1) = 5` (Read-Execute). This segment contains `.text`, `.rodata`, `.plt`, `.ARM.extab`, `.ARM.exidx`, `.hash`, `.dynsym`, `.dynstr`, and `.rel.dyn`. It remains in flash memory (`0x08xxxxxx` range) and is executed in place (`(*F*)`).
- **`data PT_LOAD FLAGS(6)`**: `FLAGS(6)` specifies `PF_R (4) | PF_W (2) = 6` (Read-Write). This segment contains `.data`, `.bss`, `.got`, `.data.rel.ro`, `.init_array`, and `.dynamic`. The loader allocates RAM (`0x20xxxxxx` range) and copies **only** this segment (`(*R*)`).

#### `SIZEOF_HEADERS` and Virtual Address Offset Cancellation

A critical constraint for XIP on ARM Cortex-M is that PLT stubs in flash compute their GOT slot addresses using PC-relative arithmetic: `add r12, pc, #offset`. This requires that the virtual address relative to file offset is identical for both code and data (`text_vaddr - text_off == data_vaddr - data_off`).

In `xip-split.ld`, this is guaranteed by setting the section origin to `SIZEOF_HEADERS`:

```ld
SECTIONS
{
  . = SIZEOF_HEADERS;

  /* ===== R-X Segment (stays in Flash) ===== */
  .hash           : { *(.hash) } :text
  .dynsym         : { *(.dynsym) } :text
  .dynstr         : { *(.dynstr) } :text
  .rel.dyn        : { *(.rel*) } :text
  .text           : { *(.text .text.*) } :text
  .rodata         : { *(.rodata .rodata.*) } :text
  .plt            : { *(.plt) } :text
}
```

Setting `. = SIZEOF_HEADERS` forces `text_vaddr == text_off` (cancelling the offset: `text_vaddr - text_off = 0`). Consequently:

$$\text{data\_vaddr} - \text{data\_off} = \text{text\_vaddr} - \text{text\_off} = 0$$

Because the offset difference cancels to zero, PC-relative calculations executed from flash evaluate directly to the exact physical GOT slot location in RAM without requiring runtime offset translation!

#### Quantitative RAM Footprint Reduction

| Load Mode | Flash Usage | RAM Usage | RAM Reduction |
|-----------|-------------|-----------|---------------|
| **Copy-Whole (standard PIC)** | 48 KB (flash archive) | 48 KB (full code + data copy) | 0% (baseline) |
| **XIP Split Model (`xip-split.ld`)** | 48 KB (executed in flash) | **1.2 KB – 4 KB** (RW data & GOT only) | **>92% RAM Savings** |

### 4.4 Didactic XIP Complex Walkthrough Results & Symbol Resolution Evolution

The complex XIP walkthrough suite (`walkthrough_complex.cpp`) executes a multi-module load of `c2.so` and `cpp3.so`, verifying symbol resolution and tracking slot addresses step-by-step from GOT/PLT initialization to execution.

#### Memory Placement & Address Region Markers

To visually trace memory regions, diagnostic logs use explicit region markers:
- **`(*F*)`**: Addresses residing in **Flash** memory (`0x08xxxxxx` range, read-only / in-place code).
- **`(*R*)`**: Addresses residing in **SRAM / RAM** (`0x20xxxxxx` range, mutable data & relocated GOT entries).

When `c2.so` and `cpp3.so` are loaded:
```
[1] Segment Mapping:
    c2.so:
      .text/.rodata/PLT  0x0800a200 (*F*)  4120 B
      .got/.data/.bss    0x20001000 (*R*)  480 B
    cpp3.so:
      .text/.rodata/PLT  0x0800c000 (*F*)  8450 B
      .got/.data/.bss    0x20001200 (*R*)  610 B
```

#### Step-by-Step Symbol Resolution & Patching Evolution

##### Stage 0 — Initial GOT State (BEFORE Relocation in Flash)
In the raw compiled `.so` file in flash, GOT entries contain initial un-rebased file offsets or placeholder pointers:
```
  Initial GOT values BEFORE relocation:
    [ 0] Slot VMA=0x00001000: 0x00000f00 (*F*)  (reserved)
    [ 1] Slot VMA=0x00001004: 0x00000000        (reserved)
    [ 2] Slot VMA=0x00001008: 0x00000000        (reserved)
    [ 3] Slot VMA=0x0000100c: 0x00000214 (*F*)  (RELATIVE -> rebase)
    [ 4] Slot VMA=0x00001010: 0x00000000        (to be patched: cpp3_method)
```

##### Stage 1 — Relocated GOT State in RAM (AFTER Loader Relocation)
The loader allocates RAM for the RW segment (`0x20001000 (*R*)`), copies `.got`/`.data`, and applies split-base relocations (`xip::apply_rel_xip`):
- `R_ARM_RELATIVE` slots are rebased by adding `data_base` (`0x20001000 (*R*)`).
- `R_ARM_GLOB_DAT` / `R_ARM_ABS32` slots are resolved to host or peer symbol addresses:
```
  GOT values AFTER relocation (current state in RAM):
    [ 0] RAM=0x20001000 (*R*): 0x00000f00 (*F*) -> 0x0800a200 (*F*)  (reserved by dynamic linker)
    [ 1] RAM=0x20001004 (*R*): 0x00000000        -> 0x20001000 (*R*)  (callee got_base)
    [ 2] RAM=0x20001008 (*R*): 0x00000000        -> 0x08001421 (*F*)  (resolver trampoline)
    [ 3] RAM=0x2000100c (*R*): 0x00000214 (*F*) -> 0x20001214 (*R*)  <- rebased (c2_var)
    [ 4] RAM=0x20001010 (*R*): 0x00000000        -> 0x0800c145 (*F*)  <- resolved (cpp3_method)
```

##### Stage 2 — PLT / Cross-Library JUMP_SLOT Routing
For function calls crossing library boundaries (e.g. `c2` calling `cpp3_method` or math/libc functions), `R_ARM_JUMP_SLOT` relocations in the RAM GOT (`(*R*)`) point to the `xip_cross_call` resolver or directly resolved code entry points (`(*F*)`):
```
  [PLT] function slots reached via the PLT (GOT JUMP_SLOTs):
    .rel.plt GOT[0x0010] RAM=0x20001010 (*R*): 0x00000000 -> 0x0800c145 (*F*)  cpp3_method
    .rel.plt GOT[0x0014] RAM=0x20001014 (*R*): 0x00000000 -> 0x080021a9 (*F*)  sinf
```

##### Stage 3 — Execution & Parameter Passing
During execution (`cpp3_method(seed, x)`):
1. **`r10` Register Set**: `r10` is loaded with `cpp3.so`'s `got_base` (`0x20001200 (*R*)`).
2. **Instruction Fetch from Flash**: The Cortex-M CPU fetches instructions directly from Flash (`0x0800c145 (*F*)`).
3. **Data Access in RAM**: Global/static variables (`cpp3_magic`) are read/written in RAM (`0x20001210 (*R*)`).
4. **Result Verification**: `cpp3_method(seed, x)` returns the exact expected parameter calculation, confirming that cross-library calls, PLT routing, and split-base relocation operate seamlessly directly out of Flash.


---
## 5. common-elf-reader — Full ELF Parsing API

> **Location:** `../common-elf-reader/` (vendored source)
> **Files:** `include/loader/{byte-source.hpp,elf-image.hpp,elf-types.hpp,error.hpp}`, `include/loader/detail/bytes.hpp`, `include/loader/sources/memory-byte-source.hpp`, `src/elf-image.cpp`
> **CMake target:** `common-elf-reader::elf-reader` (INTERFACE source library)

### 5.0 Overview: What common-elf-reader Provides

The `common-elf-reader` is a standalone ELF32 reader/parser extracted so both the SCL and the former `shared-static-rework` project share one parser API. It provides four layers:

| Layer | File | What it does |
|-------|------|-------------|
| **I/O abstraction** | `byte-source.hpp` | Abstract random-access byte reader — the parser never assumes where bytes live |
| **Error handling** | `error.hpp` | `error` enum (10 codes) + `result<T>` (C++20 expected-like) |
| **ELF structures** | `elf-types.hpp` | ARM ELF32-LE on-disk structs (`Ehdr`, `Phdr`, `Shdr`, `Sym`, `Rel`, `Dyn`) + constants (`DT_*`, `PT_LOAD`, `R_ARM_*`, `SHF_*`) + accessors (`r_sym`, `r_type`, `st_bind`, `st_type`) |
| **Bounded byte access** | `detail/bytes.hpp` | Header-only LE load/store with bounds checks — used by ALL loaders |
| **ELF parser** | `elf-image.hpp` + `elf-image.cpp` | Validated ELF view: parse, program/section headers, find section by name, image size/base |

The reader has **no dependencies** beyond C++20. It is compiled from source into each consumer (INTERFACE target, no `.a` archive). Only `collect_relocations()` needs `SCL_HOSTED` (for `std::vector`); freestanding firmware uses `reloc_applier` instead.

### 5.1 API Reference

#### `loader::byte_source` — I/O Abstraction
- **File:** `../common-elf-reader/include/loader/byte-source.hpp:17`
- Abstract base class for random-access byte reading. The entire parser stack goes through this interface — swap the backend to read from semihosting, flash, RAM, SD card, etc.
- **Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `size` | `virtual size_t size() const noexcept = 0` | Total bytes in the object |
| `read_at` | `virtual result<void> read_at(size_t offset, span<byte> out) noexcept = 0` | Fill `out` from `offset`. Returns `error::SHORT_READ` if range exceeds object |

#### `loader::memory_byte_source` — In-Memory Backend
- **File:** `../common-elf-reader/include/loader/sources/memory-byte-source.hpp:16`
- Concrete `byte_source` over a `span<const byte>`. Non-owning — the caller keeps the buffer alive.
- **Constructor:** `explicit memory_byte_source(span<const byte> data) noexcept`
- Implements `size()` and `read_at()` via `memcpy`

#### `loader::error` — Error Codes
- **File:** `../common-elf-reader/include/loader/error.hpp:12`
- **Enum values:** `OK`, `FILE_NOT_FOUND`, `SHORT_READ`, `NOT_ELF`, `NOT_ARM32`, `NOT_SHARED_OBJECT`, `BAD_SECTION`, `SYMBOL_NOT_FOUND`, `UNSUPPORTED_RELOC`, `OUT_OF_MEMORY`
- Used throughout the entire SCL as the failure type

#### `loader::result<T>` — Value-or-Error
- **File:** `../common-elf-reader/include/loader/error.hpp:40`
- C++20 replacement for `std::expected<T, error>`. Exposes:
  - `has_value()` / `operator bool` — check for success
  - `operator*` / `operator->` — access the value
  - `err()` — get the error (when `!has_value()`)
  - `value_or(fallback)` — value or default
- **Partial specialisation** `result<void>` (line 74-87): carries success/error with no value
- **Builder:** `loader::unexpected(error e)` constructs the failure alternative

#### `loader::elf::` Types — ARM ELF32 Structures
- **File:** `../common-elf-reader/include/loader/elf-types.hpp:13`
- All structs are `memcpy`-compatible with on-disk layout (requires little-endian host, enforced by `static_assert` at line 17).

| Struct | Bytes | Fields | Purpose |
|--------|-------|--------|---------|
| `Ehdr` | 52 | `e_ident[16], e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx` | ELF file header |
| `Phdr` | 32 | `p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align` | Program header (segment) |
| `Shdr` | 40 | `sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize` | Section header |
| `Sym` | 16 | `st_name, st_value, st_size, st_info, st_other, st_shndx` | Symbol table entry |
| `Rel` | 8 | `r_offset, r_info` | REL relocation entry (ARM uses REL, not RELA) |
| `Dyn` | 8 | `d_tag, d_val` | Dynamic section entry |

- **Constants:** `DT_NULL` (0), `DT_HASH` (4), `DT_STRTAB` (5), `DT_SYMTAB` (6), `DT_STRSZ` (10), `DT_INIT_ARRAY` (25), `DT_FINI_ARRAY` (26), `PT_LOAD` (1), `SHT_REL` (9), `SHF_ALLOC` (2), `ET_DYN` (3), `EM_ARM` (40)
- **Relocation types** (`rel_type` enum, line 122): `ABS32` (2), `REL32` (3), `COPY` (20), `GLOB_DAT` (21), `JUMP_SLOT` (22), `RELATIVE` (23)
- **Decoders:**
  - `elf::r_sym(word info)` — extract symbol index from `r_info` (high 24 bits)
  - `elf::r_type(word info)` — extract relocation type from `r_info` (low 8 bits)
  - `elf::st_bind(uint8_t info)` — extract binding from `st_info` (high nibble)
  - `elf::st_type(uint8_t info)` — extract symbol type from `st_info` (low nibble)

#### `loader::detail::bytes` — Bounded LE Accessors
- **File:** `../common-elf-reader/include/loader/detail/bytes.hpp:22`
- Header-only, bounds-checked. Used by `reloc_applier`, `symbol_resolver`, `lazy_resolve`, and all loaders.
- **Functions:**

| Function | Signature | Description |
|----------|-----------|-------------|
| `load_word` | `uint32_t load_word(span<const byte> s, size_t off) noexcept` | LE 32-bit read; out-of-range → 0 |
| `load_byte` | `uint8_t load_byte(span<const byte> s, size_t off) noexcept` | Single byte read; out-of-range → 0 |
| `store_word` | `void store_word(span<byte> s, size_t off, uint32_t v) noexcept` | LE 32-bit write; out-of-range → no-op |
| `name_at` | `string_view name_at(span<const byte> tab, uint32_t off) noexcept` | NUL-terminated string bounded by span; hand-rolled scan (no `strnlen` for freestanding) |

#### `loader::elf_image` — ELF Parser
- **File:** `../common-elf-reader/include/loader/elf-image.hpp:18`, `../common-elf-reader/src/elf-image.cpp:26`
- Validated view over an ARM ELF32 shared object. The `byte_source` must outlive the `elf_image`.
- **Static factory:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `parse` | `static result<elf_image> parse(byte_source& src) noexcept` | Validate magic/class/data/machine/type, read Ehdr. Errors: `NOT_ELF`, `NOT_ARM32`, `NOT_SHARED_OBJECT` |

- **Accessors:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `header` | `const elf::Ehdr& header() const noexcept` | Cached ELF file header |
| `program_header` | `result<void> program_header(half index, elf::Phdr& out) const noexcept` | Read program header `i` into `out`. Error: `BAD_SECTION` if index ≥ e_phnum |
| `section_header` | `result<void> section_header(half index, elf::Shdr& out) const noexcept` | Read section header `i` into `out`. Error: `BAD_SECTION` if index ≥ e_shnum |
| `find_section` | `result<elf::Shdr> find_section(string_view name) const noexcept` | Find section by name. Reads `.shstrtab`, iterates all section headers comparing names. Error: `BAD_SECTION` if not found |
| `image_size` | `result<uint32_t> image_size() const noexcept` | Highest `p_vaddr + p_memsz` across all PT_LOAD segments (the total VA range). 32-bit safe |
| `image_base` | `result<uint32_t> image_base() const noexcept` | Lowest `p_vaddr` across all PT_LOAD segments |
| `source` | `byte_source& source() const noexcept` | Access the underlying byte source |

- **Hosted-only:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `collect_relocations` | `result<vector<byte>> collect_relocations() const noexcept` | Concatenate all allocated `SHT_REL` sections (`.rel.dyn`, `.rel.plt`, ...) into one buffer. Requires `SCL_HOSTED` for `std::vector` |

### 5.2 Internal Functions (`../common-elf-reader/src/elf-image.cpp`)

| Function | Line | Scope | Description |
|----------|------|-------|-------------|
| `read_pod<T>` | 14 | anonymous ns | `result<T> read_pod(byte_source& src, size_t offset)` — read a trivially-copyable POD at offset. Used to read Ehdr, Phdr, Shdr from the byte source. Template ensures trivially-copyable via `static_assert`. |

### 5.3 Architecture: How the Parser Works on Cortex-M

**Parsing flow for every `dlopen`:**

```
byte_source (semihosting file / flash blob / RAM)
  │
  └─► elf_image::parse(bytes)
       │
       ├─► read_pod<Ehdr>(src, 0)   ──► 52 bytes at offset 0
       │    └─► validate: e_ident[0:3] == "\x7fELF"
       │    └─► validate: EI_CLASS == 32-bit
       │    └─► validate: EI_DATA == little-endian
       │    └─► validate: e_machine == EM_ARM (40)
       │    └─► validate: e_type == ET_DYN (3)
       │    └─► cache Ehdr in elf_image
       │
       ├─► for i in 0..e_phnum:        (used by PT_LOAD copy loop)
       │    └─► read_pod<Phdr>(src, e_phoff + i * sizeof(Phdr))
       │    └─► returns Phdr.p_type, p_offset, p_vaddr, p_filesz, p_memsz
       │
       ├─► for i in 0..e_shnum:        (used by find_section)
       │    └─► read_pod<Shdr>(src, e_shoff + i * sizeof(Shdr))
       │    └─► returns Shdr.sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size
       │
       ├─► image_size():
       │    └─► Iterate PT_LOAD segments, compute max(p_vaddr + p_memsz)
       │    └─► This is the total RAM footprint for arena allocation
       │
       └─► image_base():
            └─► Iterate PT_LOAD segments, find min(p_vaddr)
```

**Why bounds-checked byte access matters on Cortex-M:**

The `detail/bytes.hpp` helpers (`load_word`, `store_word`, `name_at`) are used by every loader component when walking in-memory ELF tables (`.dynsym`, `.dynstr`, `.hash`, `.rel.*`). On Cortex-M there is no MMU to trap out-of-bounds access — a malformed section offset would silently read arbitrary memory and potentially jump to it (HardFault). The bounds checks ensure that:
- `load_word` returns 0 for out-of-range offsets (instead of reading garbage)
- `store_word` is a no-op for out-of-range offsets (instead of corrupting adjacent memory)
- `name_at` never scans past the string table span (no unbounded `strlen` into unmapped memory)

**The `detail/` namespace convention:**

Access to raw ELF bytes goes through `loader::detail::load_word` / `store_word` / `name_at` rather than direct pointer dereferencing or `memcpy`. This single chokepoint makes it possible to add memory access instrumentation, endian conversion (for big-endian targets), or fault injection testing without touching every loader.

### 5.4 Dependencies and Integration

**How the SCL consumes common-elf-reader (`../CMakeLists.txt:47`):**

```cmake
add_subdirectory(${COMMON_ELF_READER_DIR} common-elf-reader)
# → defines common-elf-reader::elf-reader (INTERFACE)

target_link_libraries(scl-common INTERFACE common-elf-reader::elf-reader)
# → every loader transitively gets the parser
```

All loader targets transitively link `common-elf-reader::elf-reader` — through `scl-common`, and `scl-static` directly (via `target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)`). The parser's single `.cpp` file (`src/elf-image.cpp`) is compiled exactly once per executable (CMake de-duplicates INTERFACE sources), with the consuming application's compiler flags — ensuring the correct `-mcpu`, `-mfloat-abi`, and optimisation level regardless of which loader flavour is in use.

---

## 6. Template Application

A unified firmware `main()` that exercises the **static-loader family** APIs.

> **Note:** This template combines the `scl::common`, `scl::static`, and `scl::xip` APIs in one file for illustration. It demonstrates every call pattern — adapt the platform init to your actual board.

```cpp
// ============================================================
// TEMPLATE: all SCL APIs in one file
// ============================================================
#include <cstdint>
#include <cstring>
#include <span>

// ---- scl::common core ----
#include "loader/elf-image.hpp"
#include "loader/byte-source.hpp"
#include "loader/sources/memory-byte-source.hpp"
#include "loader/error.hpp"
#include "loader/dynamic-info.hpp"
#include "loader/symbol-resolver.hpp"
#include "loader/reloc-applier.hpp"
#include "loader/lazy-resolve.hpp"
#include "loader/region-allocator.hpp"
#include "loader/detail/bytes.hpp"
#include "loader/elf-types.hpp"

// ---- scl::firmware helpers ----
#include "scl/log.h"
#include "scl/image-memory.hpp"
#include "scl/link-context.hpp"
#include "scl/pic-trampoline.h"

// ---- scl::static (legacy patcher) ----
#include "static/elf-reader.h"
#include "static/shlib-loader.h"

// ---- scl::xip ----
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include "xip/compact.hpp"
#include "xip/module.hpp"

// ============================================================
// SECTION 1: scl::common — low-level toolbox usage
// ============================================================
void demo_common_toolbox() {
    // ---- byte_source: storage abstraction ----
    std::byte raw_elf[4096];                     // pretend .so bytes
    loader::memory_byte_source src(raw_elf);     // wrap RAM buffer as a byte_source

    // ---- error/result ----
    loader::result<int> ok_result = 42;
    loader::result<int> err_result = loader::unexpected(loader::error::NOT_ELF);
    if (!err_result) {
        loader::error e = err_result.err();      // e == loader::error::NOT_ELF
    }

    // ---- elf_image: parse ELF ----
    auto parsed = loader::elf_image::parse(src);
    if (parsed) {
        auto& hdr = parsed->header();           // Ehdr: e_phnum, e_shnum, e_entry...
        auto sz   = parsed->image_size();       // total PT_LOAD VA range
        auto base = parsed->image_base();       // lowest PT_LOAD vaddr

        // Program headers
        loader::elf::Phdr ph;
        parsed->program_header(0, ph);           // ph.p_vaddr, p_offset, p_filesz, p_memsz

        // Section headers
        auto sh = parsed->find_section(".dynsym"); // Shdr: sh_addr, sh_offset, sh_size
        auto sh_by_idx = loader::elf::Shdr{};
        parsed->section_header(3, sh_by_idx);
    }

    // ---- detail/bytes: bounded LE accessors ----
    std::span<const std::byte> buf(raw_elf, 100);
    uint32_t w = loader::detail::load_word(buf, 16);   // LE read at offset
    uint8_t  b = loader::detail::load_byte(buf, 0);     // single byte
    loader::detail::store_word({raw_elf, 100}, 20, 0x12345678);  // LE write
    auto str = loader::detail::name_at(buf, 4);          // NUL-terminated string read

    // ---- dynamic_info: parse _DYNAMIC ----
    auto di = loader::dynamic_info::parse(buf);  // extracts DT_HASH, DT_SYMTAB, DT_STRTAB

    // ---- symbol_resolver: SysV hash lookup ----
    auto resolver = loader::symbol_resolver(buf, buf, buf);
    auto addr = resolver.address_of("printf");   // image-relative .dynsym value

    // ---- reloc_applier: allocation-free relocation streaming ----
    loader::reloc_applier::apply_rel(
        std::span<std::byte>(raw_elf, 4096),     // writable image
        0x20000000,                              // base address
        buf, buf, buf,                           // .rel.dyn, .dynsym, .dynstr
        loader::symbol_source{                   // external resolver callback
            +[](void*, std::string_view n) -> loader::result<uint32_t> {
                if (n == "printf") return 0x08001234u;
                return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
            },
            nullptr
        }
    );

    // ---- LazyResolve: map GOT slot to JUMP_SLOT symbol ----
    loader::lazy_tables lt{buf, buf, buf};       // JMPREL + dynsym + dynstr
    auto target = loader::lazy_resolve_slot(      // resolve GOT[5]->symbol
        0x80, lt,                                // got_off = 0x80
        loader::symbol_source{nullptr, nullptr}
    );

    // ---- region_allocator: fixed-arena allocator ----
    std::byte arena_buf[16384];
    loader::region_allocator arena(arena_buf, sizeof(arena_buf));
    auto* block = arena.alloc(4096);             // allocate (8-byte aligned)
    arena.free(block);                           // free with coalesce

    // ---- elf_hash ----
    uint32_t hash = loader::elf_hash("mylib_function");  // SysV hash value
}

// ============================================================
// SECTION 2: scl::static — legacy ELF32 patcher
// ============================================================
void demo_static_loader() {
    elf_loader::shlib_loader loader;

    // ---- read ELF file ----
    loader.read_elf("libsh1.so");

    // ---- load binary into RAM buffer ----
    uint8_t buffer[8192];
    loader.load_bin(buffer, sizeof(buffer));

    // ---- populate helper sections ----
    loader.populate_helper_sections();

    // ---- patch counts ----
    uint32_t sh_cnt = loader.get_shlib_patch_count();
    uint32_t m_cnt  = loader.get_main_patch_count();

    // ---- allocate patch arrays ----
    elf_loader::symbol_patch sh_patches[64];
    elf_loader::symbol_patch m_patches[64];
    loader.allocate_patch_arrays(sh_patches, m_patches);

    // ---- find symbols ----
    loader.find_shlib_symbols_to_patch(sh_patches);
    loader.find_main_symbols_to_patch(m_patches);

    // ---- patch GOT (library and main) ----
    elf_loader::symbol_table syms;
    uint32_t n_sh = loader.patch_shlib(&syms);
    uint32_t n_m  = loader.patch_main(&syms);

    // ---- resolve DT_NEEDED dependencies ----
    loader.resolve_dependencies(&syms, nullptr);

    // ---- run constructors ----
    loader.execute_constructors();

    // ---- call library functions directly (no trampoline) ----
    auto fn = reinterpret_cast<uint32_t (*)(uint32_t)>(
        reinterpret_cast<uintptr_t>(loader.get_ram_shlib()) + 0x100);
    // (addresses are known after patching)

    // ---- sync barriers ----
    elf_loader::sync_after_patching();
}

// ============================================================
// SECTION 3: scl::xip — Execute-In-Place
// ============================================================
void demo_xip() {
    // ---- core XIP analysis (pure computation) ----
    std::byte so_bytes[4096];       // .so file bytes (in flash)
    xip::SegmentMap seg;
    bool can_xip = xip::classify_segments(so_bytes, seg);
    if (can_xip) {
        bool immutable = xip::text_is_immutable(so_bytes, seg);
        if (immutable) {
            // Text can stay in flash, only RW goes to RAM
            xip::Module xmod;
            bool ok = xip::compact_for_xip(so_bytes, xmod);
            if (ok) {
                // xmod.text -> flash span, xmod.data -> empty (loader fills)
                // xmod.text_base = flash address of .text
                // xmod.xip = true
            }
        }
    }

    // ---- XIP split-base relocation (low-level) ----
    std::byte ram_buf[2048];        // RAM for RW data
    loader::symbol_resolver resolver({}, {}, {});
    xip::apply_rel_xip(
        seg,                        // segment map
        0x08000000,                 // text_base (flash address)
        0x20000000,                 // data_base (RAM address)
        ram_buf,                    // RW data in RAM
        {}, {}, {},                 // .rel.dyn, .dynsym, .dynstr
        loader::symbol_source{nullptr, nullptr}
    );
}

// ============================================================
// SECTION 4: Variable and data cross-reference
// ============================================================
// Global variables that loaders expect from firmware:
extern uint32_t _simage_arena_start;    // Arena base (linker script)
extern uint32_t _simage_arena_end;      // Arena end
// GOT boundaries (for scl::static):
extern uint32_t _sgot, _esgot, _sgot_plt, _esgot_plt;

// ============================================================
// SECTION 5: Logging hook
// ============================================================
// Override weak no-ops in scl/log.h:
extern "C" void scl_log_puts(const char* s) {
    // send to semihosting trace, UART, etc.
}
extern "C" void scl_log_putc(char c) {
    // send single char to trace
}
```

---
## 7. Execution Path / Call Stack Analysis

### 7.1 Path D: Static Patcher (scl::static)

```
shlib_loader::read_elf("lib.so")                  [../src/static/elf-reader.cpp]
  └─► elf_reader::open → elf_image::parse

shlib_loader::load_bin(buf, size)                 [../src/static/shlib-loader.cpp:55]
  └─► copy PT_LOAD segments into buf at p_vaddr offsets
  └─► scan DT_NEEDED → record dependency list

shlib_loader::populate_helper_sections()           [../src/static/shlib-loader.cpp:105]
  └─► locate .inits, .rel.dyn, .dynsym, .dynstr, .hash in both lib and main
  └─► main sections come from _reloc init block (linker-exported table)

shlib_loader::patch_shlib(&syms)                  [../src/static/shlib-loader.cpp:266]
  └─► for each .rel.dyn entry:
       └─► R_ARM_ABS32:     resolve sym → patch lib GOT
       └─► R_ARM_RELATIVE:  add load bias
       └─► R_ARM_GLOB_DAT:  resolve sym → patch lib GOT
       └─► R_ARM_JUMP_SLOT: resolve sym → patch lib GOT

shlib_loader::patch_main(&syms)                   [../src/static/shlib-loader.cpp:403]
  └─► for each main's .rel.dyn:
       └─► R_ARM_COPY:      memcpy lib export → main's .bss
       └─► R_ARM_GLOBAL:    patch main GOT with lib's exports
       └─► R_ARM_WEAK:      weak symbol resolution

shlib_loader::execute_constructors()               [../src/static/shlib-loader.cpp:509]
  └─► iterate .inits / .init_array → call each ctor

  │
  ▼
DIRECT CALL (no trampoline — addresses already patched)
```

### 7.2 Path H: XIP Load

```
xip::compact_for_xip(so_image, out)                [../src/xip/compact.cpp:11]
  │
  ├─► xip::classify_segments(so_image, seg)       [../src/xip/segment-map.cpp:16]
  │    └─► find one R-X (exec, !write) PT_LOAD + one RW(!W) PT_LOAD
  │    └─► require VA == file_offset for both (PLT GOT computation)
  │    └─► set seg.split = false if fused RWX or mismatch
  │
  ├─► if seg.split:
  │    └─► xip::text_is_immutable(so_image, seg)   [../src/xip/relocate.cpp]
  │         └─► scan all SHT_REL sections
  │         └─► verify every r_offset → SHF_WRITE section
  │         └─► false if any reloc targets R-X range
  │
  └─► if both pass:
       out.text = so_image.subspan(text_off, text_size)  // in-place flash view
       out.text_base = flash_addr of text
       out.xip = true

  │
  ▼
XIP LOADER (xip::static adapter)                  [../src/xip/static/adapter.cpp]
  │
  ├─► if can_xip:
  │    ├─► alloc RAM for RW segment (data_memsz)
  │    ├─► copy RW initializer bytes from flash to RAM
  │    ├─► memset 0 for .bss
  │    ├─► compute got_base = data_base + (.got_vaddr - data_vaddr)
  │    ├─► xip::apply_rel_xip(seg, text_base, data_base, ...)
  │    │    └─► RELATIVE: *P = A + (in_text(A) ? text_base - text_vaddr : data_base - data_vaddr)
  │    │    └─► GLOB_DAT/JUMP_SLOT: *P = sym_value(routed to text or data base)
  │    │    └─► Only RW words written (r_offset must be in data segment)
  │    ├─► Register exidx (text_base + .ARM.exidx offset — in flash)
  │    ├─► Route external JUMP_SLOTs → xip_cross_call
  │    └─► Run ctors via pic_call_void with got_base
  │
  └─► else (fallback to copy-whole):
       ├─► alloc RAM for entire image
       ├─► copy all LOAD segments to RAM
       ├─► apply all relocations manually (with split-base awareness)
       ├─► Route JUMP_SLOTs → xip_cross_call
       ├─► Register exidx
       └─► Run ctors
```

---

## 8. Text Schemas & Diagrams

### 8.1 Memory Layout: XIP Split

```
FLASH (memory-mapped)
┌──────────────────────────────┐
│ R-X segment (stays in flash):│
│ .text  .rodata  .plt  .got   │
│ (PC-relative, position-indep)│
│ text_vaddr == text_off       │
│ text_base = flash_addr       │
│                              │
│ (NO relocations target here) │
├──────────────────────────────┤
│ RW segment copy (initializer │
│ bytes; memcpy'd to RAM at    │
│ load time)                   │
└──────────────────────────────┘

RAM (copied from flash RW bytes)
┌──────────────────────────────┐
│ RW segment:                  │
│ .data  .got  .bss            │
│ data_vaddr == data_off       │
│ data_base = ram_addr         │
│                              │
│ GOT is at:                   │
│ got_base = data_base +       │
│   (.got_vaddr - data_vaddr)  │
│ (r10 = got_base for pic_call)│
└──────────────────────────────┘
```

### 8.2 ELF Parsing Flow

```
Byte Source (semihost file / flash blob / RAM buffer)
│
└─► elf_image::parse()
     │
     ├─► validate ELF header (magic, class=32, endian=LE, machine=ARM,
     │                        version=1, phoff!=0, shoff!=0)
     │
     ├─► Ehdr fields: e_phnum (program header count)
     │                e_shnum (section header count)
     │                e_entry (entry point)
     │                e_shstrndx (.shstrtab section index)
     │
     ├─► [optional] read program headers:
     │    └─► Phdr: p_type=PT_LOAD, p_vaddr, p_offset, p_filesz, p_memsz, p_flags
     │    └─► Phdr: p_type=PT_DYNAMIC, p_vaddr (→ _DYNAMIC array)
     │    └─► Phdr: p_type=PT_ARM_EXIDX, p_vaddr, p_filesz
     │
     ├─► [optional] read section headers:
     │    └─► Shdr: sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size
     │    └─► Section names read from .shstrtab
     │
     ├─► find_section(name):
     │    └─► read Shdr at index matching name
     │
     └─► image_size(): max(PT_LOAD vaddr+memsz) - min(PT_LOAD vaddr)
         image_base(): min(PT_LOAD vaddr)

_DYNAMIC parsing (via dynamic_info):
  └─► iterate Dyn array until DT_NULL:
       └─► DT_HASH:    address of .hash
       └─► DT_SYMTAB:  address of .dynsym
       └─► DT_STRTAB:  address of .dynstr
       └─► DT_STRSZ:   size of .dynstr
       └─► DT_JMPREL:  address of .rel.plt
       └─► DT_INIT:    address of .init
       └─► DT_INIT_ARRAY: address of .init_array
       └─► DT_FINI:       address of .fini
       └─► DT_NEEDED:     dependency name (string table offset)

Symbol resolution (via symbol_resolver):
  └─► .hash: [nbucket(4), nchain(4), bucket[nbucket](4), chain[nchain](4)]
  └─► elf_hash(name) % nbucket → bucket[i] → walk chain[]:
       └─► compare name with .dynstr + .dynsym[sym_index].st_name
       └─► match → return .dynsym[sym_index].st_value (image-relative)

Relocation apply (via reloc_applier):
  └─► .rel.dyn / .rel.plt: array of elf::Rel { r_offset, r_info }
       └─► r_type = r_info & 0xff
       └─► r_sym  = r_info >> 8
       └─► for each:
            RELATIVE:  *base[r_offset] = base + A (load bias)
            GLOB_DAT:  *base[r_offset] = resolve(sym_name)
            JUMP_SLOT: *base[r_offset] = resolve(sym_name)
            ABS32:     *base[r_offset] = S + A
            REL32:     *base[r_offset] = S + A - P
            COPY:      memcpy(size)
```

### 8.3 Firmware Export Table: _reloc

```
Static loader (scl::static):
  └─► firmware's linker script emits a _reloc table:
       _reloc = .
       LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
       LONG(ADDR(.dynsym));      LONG(SIZEOF(.dynsym));
       LONG(ADDR(.dynstr));      LONG(SIZEOF(.dynstr));
       LONG(ADDR(.hash));        LONG(SIZEOF(.hash));
       LONG(ADDR(.inits));       LONG(SIZEOF(.inits));
  └─► link_context reads this fixed-layout table at boot
```

---
## 9. Build System — Firmware, Tests, and Fixtures

This chapter documents the complete build pipeline: CMake targets, toolchain files, platform library, test app helper, fixture build, flash/run scripts, and the integration recipe for adding a new app.

### 9.1 CMake Architecture Overview

The SCL uses a **two-level CMake** structure:

```
Level 1: repo root (CMakeLists.txt)
  └─► scl-common           (INTERFACE, portable C++20)
  └─► scl-static           (INTERFACE, ARM only)
  └─► scl-xip-common       (INTERFACE, depends on scl-common)
  └─► scl-xip-static       (INTERFACE, depends on scl-static + scl-xip-common)

Level 2: test/ (shared assets: cmake helpers + platforms)
  └─► includes ../ (SCL_ROOT) → gets all scl::* targets
  └─► includes cmake/scl-test-app.cmake
  └─► includes platforms/ (board, board-hosted, qemu-f411re, qemu-f411re-hosted)

Standalone test trees:
  test-static/test-static/               (self-contained, scl::static only)
  test-static/test-static-cpp/           (same, C++ fixtures)
  test-static/test-static-direct/        (direct API calls, main's GOT patched)
  test-static/test-drv/                  (spi/uart/led drivers, SPI flash + flat-fs)
  test-xip/                              (scl::xip-static apps)
```

**Key pattern:** All SCL targets are `INTERFACE EXCLUDE_FROM_ALL` (never produce `.a` archives). Sources propagate to the consuming app, which compiles them with its own flags. Only what an app links gets compiled, exactly once per executable (transitively shared sources de-duplicated).

### 9.2 Toolchain Files (`../cmake/`)

Two toolchain files control the cross-build:

#### `../cmake/arm-none-eabi.cmake:1` — Freestanding (hard-float)

```
Target:   Cortex-M4F, hard-float (fpv4-sp-d16)
Flags:    -fpic -O0 -g -ffunction-sections -fno-common
          -ffreestanding -fno-tree-loop-distribute-patterns
C++ add:  -fno-threadsafe-statics
Exceptions/RTTI: ON (main provides EH runtime to loaded libs)
Link:     -pie -nostdlib -nostartfiles
          -Wl,-Bsymbolic -Wl,--unresolved-symbols=ignore-all
          -Wl,-E -Wl,--gc-sections
```

**Used by:** XIP firmware apps (`test-xip/`).

#### `../cmake/arm-none-eabi-hosted.cmake:1` — Hosted (newlib, soft/hard float)

```
Target:   Cortex-M4, soft-float by default (-DSCL_HF=ON for hard)
Flags:    -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3
C++ add:  -fno-exceptions -fno-rtti -fno-use-cxa-atexit
          -fno-threadsafe-statics -fno-move-loop-invariants
Link:     --specs=nano.specs -u _printf_float
```

**Used by:** hosted static test apps (apps that link against newlib startup code).

### 9.3 Platform Library (`../test/platforms/board/`)

The **`platform::board`** INTERFACE target (`../test/platforms/board/cmake/platform-library.cmake:1`) provides a unified freestanding platform serving both the NUCLEO-F411RE and the WEACT-F411CE (selected with `-DSCL_BOARD=nucleo|weact`):

- **Sources:** `startup.c` (reset vector, .data copy, .bss zero, FPU enable, `main` call), `board.c` (clock init, SysTick), `semihost.c`/`semihost-file.cpp` (ARM semihosting calls), `libc-shim.c` (memcpy/memset/strcmp for `-nostdlib`), `crt.cpp` (operator new/delete), `printf-shim.c`, `scl-log-semihost.cpp`
- **Link recipe:** `-pie -nostdlib -nostartfiles -Wl,-Bsymbolic --unresolved-symbols=ignore-all -Wl,-E --gc-sections -L<ldscripts>`
- **Linker scripts:** `sections.ld` (main), `sections-flatfs.ld` (extended flash sectors), `mem.ld` (FLASH@0x08000000/512K, RAM@0x20000000/128K, ARENA@0x20010000/28K)

The hosted platform lives at `../test/platforms/board-hosted/vendor/` (vendored CMSIS, HAL, newlib, plus its own `mem-sh.ld`/`sections-sh.ld`/`libs-sh.ld`).

### 9.4 Test App Helper (`../test/cmake/scl-test-app.cmake:1`)

The `scl_test_app()` function (`../test/cmake/scl-test-app.cmake:33`) creates one firmware executable. Signature:

```cmake
scl_test_app(<target-name>
    LIBS scl::static                # SCL libraries to link
    LDSCRIPT sections.ld            # linker script from platform ldscripts/
    ANCHORS sym1 sym2 ...           # -Wl,-u keep-alive anchors
    LINK_LIBM                       # append libm.a for hard-float math
    SOURCES extra1.cpp extra2.cpp   # extra app sources
    INCLUDE_DIRS path1 path2        # extra include dirs
)
```

**What it does automatically:**
1. Globs `src/*.c` and `src/*.cpp` from the app directory
2. Adds the platform library (`platform::board`)
3. Links the specified SCL libraries
4. Defines `DL_LIB_DIR=<test/fixtures>` (absolute path to committed `.so` files)
5. Adds EH anchors (`__cxa_throw`, `__cxa_begin_catch`, `__gxx_personality_v0`, `__dynamic_cast`, ...)
6. Resolves `libsupc++.a`, `libgcc.a`, `libm.a` via the cross-compiler's `-print-file-name`
7. Links with the chosen linker script
8. POST_BUILD: `objcopy -O ihex` → `.hex` and `-O binary` → `.bin`
9. Registers a CTest test that runs `flash-run.sh <app>.hex`

### 9.5 Building Each Firmware

#### Static Patcher App (scl::static)

```sh
# From test-static/test-static/:
cd test-static/test-static
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j                          # → build/f411ce-sh.elf
```

Self-contained project with its own `system/`, `ldscripts/`, `libsh1/2/` fixtures. Links `scl::static`.

#### XIP Apps (scl::xip-static)

```sh
# From the repo root:
cmake -S test-xip -B test-xip/build-fw -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build test-xip/build-fw -j
```

Builds the static XIP apps: `test-xip-static` (semihosting), `test-xip-static-direct`,
`test-xip-static-no-pic`, `test-xip-static-no-pic-simple`, `test-xip-interrupt-static`,
`test-xip-interrupt-static-direct`. The `static/spi-flash` app
builds standalone (it removes `-ffreestanding` for `scl::static`); run its own
`build-and-run.sh`, or `test-xip/build-and-run.sh spi-flash`.

### 9.6 Fixture Build System

Fixtures are the prebuilt ARM `.so` files loaded at runtime by the test apps.

#### Shared Fixtures (`../test/fixtures/`, `../test/fixtures-src/`)

- **Committed prebuilt `.so` files** live in `test/fixtures/` (mylib1/2, libcppsh, libcppeh, c1/c2/cpp3, libprov/cons, ehx_catch, libsh1/2)
- **Sources** in `test/fixtures-src/` per library (C: `mylib1.c`, `c1.c`; C++: `libcppsh.cpp`, `cpp3.cpp`)
- Each has its own Makefile or CMake target. Build recipe (from `build-linking-guide.md`):

```makefile
# C fixture:
CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
          -fpic -msingle-pic-base -mno-pic-data-is-text-relative -O0 -g -fno-common
LDFLAGS = -shared -nostartfiles -Wl,-nmagic -Wl,-E \
          -Xlinker --unresolved-symbols=ignore-all -T ldscripts/gcc.ld
```

```makefile
# C++ fixture:
CXXFLAGS = -std=c++23 -fPIC -shared -fno-threadsafe-statics \
           -fno-use-cxa-atexit -nostdlib -Os
# libcppeh/ehx additionally: exceptions + RTTI ON
```

- **Key linker script** (`../test/fixtures-src/ldscripts/mem.ld`): RAM origin 0, length 35K (all offsets are pure image-relative, loader adds base). `gcc.ld` wraps `mem.ld` + `sections.ld`.
- **`-msingle-pic-base -mno-pic-data-is-text-relative`:** library uses r10 as single PIC base; does not assume data sits at fixed text offset (it's relocated at load time).

#### test-drv Fixtures (C and C++, CMake `add_shlib` / `add_shlib_cpp`)

The `test-static/test-static/test-drv` project uses CMake custom targets to build both C and C++ fixtures:

```cmake
# C fixture (existing):
function(add_shlib name)
    add_custom_target(build_${name}
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS}
                -c ${name}.c -o ${name}.o
        COMMAND ${CMAKE_C_COMPILER} ${LIB_CFLAGS} ${name}.o
                -o lib${name}.so -shared -nostartfiles
                -T ldscripts/mem.ld -Wl,--gc-sections
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib(spi) add_shlib(uart) add_shlib(led)

# C++ fixture (new):
set(LIB_CXXFLAGS ${LIB_CFLAGS} -fno-exceptions -fno-rtti -fno-threadsafe-statics)
function(add_shlib_cpp name)
    add_custom_target(build_cpp_${name}
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS}
                -c ${name}.cpp -o ${name}.o
        COMMAND ${CMAKE_CXX_COMPILER} ${LIB_CXXFLAGS} ${name}.o
                -o lib${name}.so -shared -nostartfiles -nostdlib
                -T ldscripts/mem.ld -Wl,--gc-sections,-S
        COMMENT "Building lib${name}.so")
endfunction()
add_shlib_cpp(cpp_features)
```

Key differences from the C version:
- Uses `-nostdlib` (no libc — the C++ fixture provides its own `new`/`delete` wrapping `malloc`/`free`)
- Linker flag `-Wl,-S` strips debug sections at link time (reduces `.so` from ~87 KB to ~14 KB)
- Compiled with `-fno-exceptions -fno-rtti` (no unwind tables, no typeinfo)
- The fixture is passed as a **link-time input** to the firmware (`target_link_libraries(... libcpp_features.so)`) so the linker creates PLT/GOT entries for direct calls like `cpp_features_run()`

### 9.7 Running on Target

#### Flash Scripts (`../test/tools/flash-run.sh`)

The main script `../test/tools/flash-run.sh:1`:

1. Accepts an **app name** (e.g. `f411ce-sh`) or an explicit `.hex` path
2. For app names: searches build trees under the SCL root for the newest matching `.hex`
3. **ABI guard:** verifies the staged fixtures' float ABI matches the firmware (via `readelf -A` → `Tag_ABI_VFP_args`). Mismatch aborts with a clear message.
4. Launches OpenOCD: `init` → `reset halt` → `program <hex>` → `arm semihosting enable` → `reset run`
5. Semihosting console streams back to the terminal; Ctrl-C stops

**Via CTest:**

```sh
ctest --test-dir test-static/test-static/build --output-on-failure  # static patcher demo
ctest --test-dir test-xip/build --output-on-failure                # XIP static apps
```

### 9.8 Adding a New Test App

Minimal template — create `test-static/test-<name>/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)       # allow standalone configure
    project(scl-fw-<name> LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/scl-test-app.cmake)
endif()

scl_test_app(scl-fw-<name>
    LIBS scl::static                 # SCL target(s)
    LDSCRIPT sections.ld             # from platform ldscripts/
    ANCHORS printf puts              # extra -Wl,-u anchors
    LINK_LIBM)                       # if hard-float math needed
```

Place `src/main.cpp` and optional `include/` next to the CMakeLists. The app sources are globbed automatically. Fixtures go in `test/fixtures/` (prebuilt `.so`) or the hosted vendor tree.

### 9.9 Full Build Matrix

| Build Dir | Configure Command | Apps | Toolchain |
|-----------|-----------------|------|-----------|
| `test/build-static/` | `cmake -S test-static/test-static -B test/build-static -DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake` | f411ce-sh | arm-none-eabi (hosted/soft) |
| `test-xip/build/` | `cmake -S test-xip -B test-xip/build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | scl-fw-xip-static-semihosting, scl-fw-xip-static-direct, scl-fw-xip-interrupt-static, scl-fw-xip-interrupt-static-direct | arm-none-eabi (freestanding/hard) |

### 9.10 Compile-Time Constants Reference

| Constant | Defined by | Consumed by | Meaning |
|----------|-----------|-------------|---------|
| `DL_LIB_DIR` | `scl_test_app()` → `target_compile_definitions` | app `main.cpp` | Absolute path to `.so` fixtures for semihosting |
| `SCL_HOSTED` | SCL CMake when option ON | `elf-image.hpp`, SCL CMake | Enables `std::vector` parts (`scl::static`, `elf_image::collect_relocations`) |

### 9.11 Requirements Reference: Tools and Default Paths

| Tool | Default Location | Used by | Override |
|------|-----------------|---------|----------|
| `arm-none-eabi-gcc/g++/objcopy/size/gdb/nm` | `~/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin` | All cross builds | `-DSCL_ARM_TOOLCHAIN_BIN=<dir>` (CMake), or `PATH` |
| `openocd` | `~/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/bin` | `test/tools/flash-run*.sh` | `OPENOCD=` env var |
| OpenOCD scripts | `~/.local/xPacks/@xpack-dev-tools/openocd/0.12.0-7.1/.content/openocd/scripts` | `test/tools/flash-run*.sh` | `OPENOCD_SCRIPTS=` env var |
| CMake ≥ 3.20 | system | All builds | system |
| Python 3 | system | `make-pdfs.py`, `embed-blob.py`, `pack-flatfs.py` | system |
| `make` | system | Vendored fixture Makefiles (called by CMake) | system |

### 9.12 Complete CMakeLists.txt Reference

All CMakeLists.txt files in the static-loader subset, organised hierarchically from repository root to leaf executable targets.

---

#### Level 0 — Repository Root

##### `CMakeLists.txt` (root)
**Path:** `../CMakeLists.txt`
**Project:** `static-shared-scl`
**Role:** SCL library package. Defines **INTERFACE library targets** (no `.a` archives): `scl-common`, `scl-static`, plus the XIP core `scl-xip-common` and the XIP adapter `scl-xip-static`.
**Key points:**
- Pulls in `common-elf-reader/` (vendored) via `add_subdirectory`
- All targets are `INTERFACE EXCLUDE_FROM_ALL` — sources propagate to consumers
- Creates namespaced aliases: `scl::common`, `scl::static`, `scl::xip-common`, `scl::xip-static`
**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `-DCOMMON_ELF_READER_DIR=<path>` | `./common-elf-reader` | Path to the vendored ELF reader |
| `-DSCL_BUILD_TESTS=ON\|OFF` | `ON` (top-level) | Enable CTest registration |
| `-DSCL_HOSTED=ON\|OFF` | `OFF` (ARM), `ON` (host) | Build `std::vector`-dependent parts |
| `-DCMAKE_TOOLCHAIN_FILE=<path>` | — | Not needed for host build |

---

#### Level 1 — Vendored ELF Reader

##### `common-elf-reader/CMakeLists.txt`
**Path:** `../common-elf-reader/CMakeLists.txt`
**Target:** `common-elf-reader::elf-reader` (INTERFACE)
**Role:** Shared ELF32 parser: `elf_image`, `byte_source`, `error`/`result<T>`, ELF types, bounded byte accessors.
**Sources:** `src/elf-image.cpp`
**Options:**

| Option | Inherited | Description |
|--------|-----------|-------------|
| `-DSCL_HOSTED=ON\|OFF` | from root CMake | Gates `elf_image::collect_relocations()` |

---

#### Standalone — Static Patcher Test Project

##### `test-static/test-static/CMakeLists.txt`
**Path:** `../test-static/test-static/CMakeLists.txt`
**Target:** `f411ce-sh`
**Role:** Static patcher demo firmware. Self-contained: own linker scripts (`mem-sh.ld` + `libs-sh.ld` + `sections-sh.ld`), own `system/`, own `libsh1/2` fixtures. Defines `add_shlib()` macro for custom `-fPIC -shared -nostartfiles` targets.
**Config:** Links `scl::static`, three custom ldscripts, `-lsh1 -lsh2`, `-nostartfiles`, `--gc-sections`, `--specs=nano.specs`. Generates `.bin`, `.lst`, runs `size` post-build.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=test-static/test-static/cmake/arm-none-eabi.cmake` | self-contained ARM soft-float |
| `-DCMAKE_BUILD_TYPE=Debug\|Release` | build configuration |

---

#### Standalone — XIP Test Project

##### `test-xip/CMakeLists.txt`
**Path:** `../test-xip/CMakeLists.txt`
**Project:** `scl-xip-tests`
**Role:** Top-level XIP firmware test aggregator. Reuses the sibling `../test` tree's app helper (`cmake/scl-test-app.cmake`) and platform sources. Adds the static XIP apps: `test-xip-static` (semihosting), `test-xip-static-direct`, `test-xip-interrupt-static`, `test-xip-interrupt-static-direct`. Requires ARM cross-compiler.
**Options:**

| Option | Description |
|--------|-------------|
| `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` | **required** (freestanding ARM) |

---

#### Summary Table

| # | Depth | File | Options | What It Does |
|---|-------|------|---------|-------------|
| 1 | root | `../CMakeLists.txt` | `COMMON_ELF_READER_DIR`, `SCL_BUILD_TESTS`, `SCL_HOSTED` | SCL library package: scl-common, scl-static, scl-xip-common, scl-xip-static |
| 2 | vendored | `../common-elf-reader/CMakeLists.txt` | (inherits `SCL_HOSTED`) | elf_image/byte_source parser INTERFACE target |
| 3 | test-static/ | `../test-static/test-static/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | Static patcher demo (own ldscripts) |
| 4 | test-xip/ | `../test-xip/CMakeLists.txt` | `CMAKE_TOOLCHAIN_FILE` | XIP firmware aggregator (static apps) |

---
## 10. Advanced Interrupt Handling and Context Switching

When developing bare-metal or real-time embedded systems that support dynamic code loading (such as ELF shared libraries or relocatable objects), handling hardware interrupts (IRQs) within loaded modules introduces significant architectural challenges.

This section details the fundamental registers of Cortex-M, the design patterns to handle interrupts, preemptive Hard-FPU context switching, and handling timer interrupts during semihosting-based debugging.

---

### 10.1 The Fundamental Architecture Obstacle: The `r10` GOT Base

In Cortex-M microcontrollers, dynamically loaded shared libraries (`.so` / `ET_DYN`) are compiled with Position-Independent Code flags:
* `-fPIC` (or `-fpic`)
* `-msingle-pic-base` (which enforces a single Global Offset Table per module)

In this compile model, access to all global variables, static variables, and external functions is resolved relative to a designated **base register** (specifically `r10` / `sl` in the GNU ARM toolchain). 

#### The Interrupt Entry Conflict
When a peripheral triggers an interrupt, the Cortex-M core performs the following hardware sequence:
1. **Context Stacking**: The CPU automatically pushes caller-saved registers (`r0`-`r3`, `r12`, `lr`, `pc`, `xPSR`) to the active stack pointer (`MSP` or `PSP`).
2. **Execution Switch**: The core switches to **Handler Mode** using the Main Stack Pointer (`MSP`).
3. **ISR Vector Branch**: The CPU loads the target address from the vector table (pointed to by the `VTOR` register) and branches directly to it.

**Crucially, the hardware does not modify or load `r10`.** The `r10` register retains whatever value was present at the exact clock cycle the interrupt occurred. This could be:
* The main firmware's GOT base address.
* The GOT base of a completely different dynamic module.
* Uninitialized garbage.

If the hardware branches directly to an ISR inside the loaded library, the C/C++ function will attempt to read or write its global variables using `r10`. Because `r10` points to the wrong GOT (or invalid memory), this instantly triggers a **UsageFault**, **BusFault**, or **HardFault**.

---

### 10.2 Design Patterns for Interrupt Handling

Depending on your loader flavor (static patcher, XIP-static, or plain firmware), choose one of the following implementation patterns.

#### Pattern A: Main Firmware Trampoline (Recommended for `scl::static`)
The hardware vector table resides permanently in the main firmware. The main firmware exposes a static ISR that acts as an execution trampoline, manually configuring the dynamic library's register context before executing the handler.

```c
/* Main Firmware Source */

// Pointers resolved during dynamic module loading
extern void* g_fixture_got_base;         // The library's got_base (r10 target)
extern void (*g_fixture_isr_handler)(void); // The target library function

/**
 * @brief Hardware ISR wrapper defined in the main vector table
 */
void USART1_IRQHandler(void) {
    if (g_fixture_isr_handler && g_fixture_got_base) {
        // Save current r10 in r4 (callee-saved), set new r10, branch, restore r10
        __asm__ volatile (
            "mov r4, r10\n"           // Save current r10 to r4
            "mov r10, %0\n"           // Load fixture GOT base into r10
            "blx %1\n"                // Call the fixture ISR handler
            "mov r10, r4\n"           // Restore original r10
            :
            : "r"(g_fixture_got_base), "r"(g_fixture_isr_handler)
            : "r4", "lr", "memory"
        );
    }
}
```

#### Pattern B: Self-Contained Assembly Wrapper (Module-Side Trampoline)
If the module is a shared library (`.so`) but needs to register itself directly into a RAM-based vector table, the registered pointer **cannot** point to the C-coded ISR. It must point to a naked assembly wrapper inside the module.

This wrapper is responsible for determining the module's GOT base address, configuring `r10`, and executing the handler.

```assembly
/* Module Assembly Wrapper (handler.S) */
.syntax unified
.cpu cortex-m4
.thumb

.section .text.isr_wrapper, "ax", %progbits
.global my_module_isr_wrapper
.type my_module_isr_wrapper, %function

my_module_isr_wrapper:
    push {r4, lr}
    
    // Load the GOT base address relative to the PC
    ldr r4, =_GLOBAL_OFFSET_TABLE_
    mov r10, r4
    
    // Call the actual C handler
    bl my_module_isr_c_handler
    
    pop {r4, pc}
    .size my_module_isr_wrapper, .-my_module_isr_wrapper
```

#### Pattern C: Dynamic RAM Vector Table Patching with Trampoline (Recommended for `scl::xip-static`)
If the dynamic module is compiled as a position-independent shared library (`-fPIC` which requires `r10` for local static/global access), but the system design requires dynamically registering the interrupt vector at runtime rather than hardcoding it in the ROM/Flash vector table, we can combine RAM vector table patching with a firmware-side trampoline.

##### How it works
1. **RAM Vector Table Initialization**: At boot, the main firmware copies the hardware vector table from Flash to RAM, aligning it to a 512-byte boundary (Cortex-M4 alignment requirement for a 128-word table), and points `SCB->VTOR` to the RAM copy.
2. **Dynamic Registration on Load**: When the static loader loads the module, we resolve the target handler and the library's `got_base`. We then dynamically patch the timer's vector slot in the RAM table with the address of a firmware-side trampoline function (`my_dynamic_tim2_handler`).
3. **Context-Saving Trampoline**: The trampoline preserves the caller's registers, loads the dynamic module's `got_base` into `r10`, calls the C handler, and restores `r10`.
4. **Dynamic Restoration on Unload**: When the module is unloaded, the vector slot in the RAM table is restored to `nullptr`.

##### Implementation Example
```c
/* Main Firmware Source */

// Align to 512 bytes for Cortex-M4 vector table requirements (16 exceptions + 86 interrupts)
__attribute__((aligned(512))) static uint32_t g_ram_vector_table[128];

void init_ram_vector_table(void) {
    uint32_t* flash_vectors = (uint32_t*)SCB->VTOR;
    for (int i = 0; i < 128; i++) {
        g_ram_vector_table[i] = flash_vectors[i];
    }
    __disable_irq();
    SCB->VTOR = (uint32_t)g_ram_vector_table;
    __DSB(); __ISB();
    __enable_irq();
}

void register_tim2_isr(void (*isr)(void)) {
    __disable_irq();
    g_ram_vector_table[TIM2_IRQn + 16] = (uint32_t)isr;
    __DSB(); __ISB();
    __enable_irq();
}

// Trampoline wrapper in main firmware
extern "C" void my_dynamic_tim2_handler(void) {
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;

        blink_leds[0].turn_on();
        delay_ms(100);
        blink_leds[0].turn_off();

        // Swap r10 with got_base and execute resolved handler
        if (g_fixture_systick_handler && g_fixture_got_base) {
            unsigned int toggle = 0;
            __asm__ volatile (
                "mov r4, r10\n"
                "mov r10, %1\n"
                "mov r0, #1\n"
                "blx %2\n"
                "mov %0, r0\n"
                "mov r10, r4\n"
                : "=r"(toggle)
                : "r"(g_fixture_got_base), "r"(g_fixture_systick_handler)
                : "r0", "r4", "lr", "memory"
            );
        }
    }
}
```

---

### 10.3 Preemptive Context Switching with Hard-FPU

When integrating a multitasking scheduler (such as an RTOS) in a Cortex-M4 environment with a hardware Floating-Point Unit (FPU), the context switcher must handle FPU register states and maintain strict alignment constraints.

#### 10.3.1 FPU Register Stacking and `EXC_RETURN`
Upon exception entry, if the FPU was active (i.e. `CONTROL.FPCA == 1`), the Cortex-M4 hardware automatically stacks registers `s0-s15` and `FPSCR` in addition to the standard core registers. 
The software context switcher (`PendSV_Handler`) is responsible for saving and restoring the remaining callee-saved FPU registers (`s16-s31`) conditionally based on the `EXC_RETURN` value.

* **FPU Frame Detection**: Bit 4 of the Link Register (`lr` / `EXC_RETURN`) determines whether an FPU frame was stacked by the hardware:
  * `0`: FPU context active; FPU frame present on stack.
  * `1`: Standard context; FPU frame not present.
* **Conditional Stacking Assembly**:
  ```assembly
  // Check if FPU context was active (bit 4 of lr is 0)
  tst lr, #0x10
  it eq
  vstmdbeq r0!, {s16-s31}   @ Save FPU registers s16-s31
  ```

#### 10.3.2 Enforcing Strict 8-byte Stack Alignment
Under the ARM Architecture Procedure Call Standard (AAPCS), the stack pointer must be aligned to an **8-byte boundary** at all external function calls. 
* **The Problem**: Pushing the core registers `r4-r11, lr` (9 registers = 36 bytes) shifts the stack pointer by a value that is not a multiple of 8, breaking alignment.
* **The Solution**: We include a padding register (e.g. `r3`) in the software stacking list, expanding the list to 10 registers (`r3-r11, lr` = 40 bytes), which preserves 8-byte alignment.
  ```assembly
  stmdb r0!, {r3-r11, lr}   @ 10 registers (40 bytes), maintains 8-byte alignment
  ```

#### 10.3.3 Dynamic Stack-Selection Context Switcher (MSP / PSP Isolation)
In a hybrid multitasking system, we can configure Task 0 (the main firmware thread) to execute on the Main Stack Pointer (`MSP`) to support debuggers and standard libraries, while Task 1 (e.g., a background LED task) executes on an isolated Process Stack Pointer (`PSP`).

To do this, the context switcher must dynamically load/store from the active stack based on bit 2 of `EXC_RETURN` (which indicates whether `MSP` or `PSP` was active):
* `0`: Interrupted context was using `MSP`.
* `1`: Interrupted context was using `PSP`.

```assembly
/* Dynamic Stack-Selection Context Switcher */
extern "C" __attribute__((naked)) void PendSV_Handler(void) {
    __asm__ volatile (
        "cpsid i\n"

        // 1. Check active stack pointer (bit 2 of EXC_RETURN in lr)
        "tst lr, #4\n"
        "ite ne\n"
        "mrsne r0, psp\n"               // If PSP was active, load into r0
        "mrseq r0, msp\n"               // If MSP was active, load into r0

        // 2. Conditionally stack FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vstmdbeq r0!, {s16-s31}\n"

        // 3. Stack core registers + EXC_RETURN (including r3 for 8-byte alignment)
        "stmdb r0!, {r3-r11, lr}\n"

        // 4. Save SP back to current TCB
        "ldr r1, =g_current_task\n"
        "ldr r1, [r1]\n"
        "ldr r2, =g_tasks\n"
        "lsl r1, r1, #3\n"
        "str r0, [r2, r1]\n"

        // 5. Select next task
        "bl select_next_task\n"

        // Set g_current_task = r0
        "ldr r1, =g_current_task\n"
        "str r0, [r1]\n"

        // 6. Load new task SP
        "ldr r2, =g_tasks\n"
        "lsl r0, r0, #3\n"
        "ldr r0, [r2, r0]\n"

        // 7. Pop core registers
        "ldmia r0!, {r3-r11, lr}\n"

        // 8. Conditionally pop FPU registers
        "tst lr, #0x10\n"
        "it eq\n"
        "vldmiaeq r0!, {s16-s31}\n"

        // 9. Restore stack pointer to the correct register (MSP or PSP) based on bit 2 of new lr
        "tst lr, #4\n"
        "ite ne\n"
        "msrne psp, r0\n"
        "msreq msp, r0\n"

        "cpsie i\n"
        "bx lr\n"
        : : : "memory"
    );
}
```

---

### 10.4 Debugging & Timer Interrupts under Semihosting

If the application uses **semihosting** (such as printing log output via `trace_printf` over SWD/JTAG), debugging interrupts becomes a challenge because of how the CPU core behaves during debugger halts.

#### 10.4.1 The SysTick Debug Freeze
The SysTick timer is physically integrated directly inside the ARM Cortex-M processor core. Because of this integration, **whenever the CPU core is halted by a debugger, the SysTick timer freezes instantly and stops counting.**

This behavior causes a severe conflict when the application utilizes **semihosting** (such as printing test logs to the host PC via `trace_printf()`):

1. **How Semihosting Halts the Core**:
   Every time the firmware calls `trace_printf()`, it executes a `BKPT` (breakpoint) instruction. The processor immediately halts. The hardware debugger (e.g., OpenOCD/GDB via JTAG/SWD) detects the halt, reads the log text out of the microcontroller's RAM, prints it to your PC's command line, and then instructs the CPU to resume execution.
   
2. **The Time Inflation Discrepancy**:
   Because sending characters to a PC terminal over a debug probe is slow, each print statement pauses the CPU for **100 to 200 milliseconds** of real-world desk time. However, the CPU active execution time *between* these print statements (the actual test math) is extremely fast, taking only **a few microseconds**.

3. **Why the Interrupt Never Fires**:
   * **Real-world time elapsed on your desk**: 10 seconds (due to 50 print statements pausing the core for 200ms each).
   * **Active CPU time counted by SysTick**: 0.0005 seconds (50 active test blocks of 10 microseconds each, as SysTick was frozen during all print pauses).
   
   Since SysTick only registers 0.0005 seconds of elapsed time during the entire test suite run, the timer is nowhere near its 1.0-second limit. Therefore, the SysTick interrupt never fires, and the LED remains completely dark during the tests.
   
   Only when all prints finish and the CPU enters the final continuous loop (`for (;;) { wfi; }`) does SysTick finally accumulate enough active cycles to reach 1.0 second and begin blinking the LED.

#### 10.4.2 The APB Peripheral Timer Advantage (`TIM2`/`TIM3`)
To make the LED blink during semihosting execution, we must use a **Hardware Timer** (like `TIM2`) instead of SysTick.
* **Continuous Bus Clocking**: Hardware timers are peripheral modules running on the APB bus. By default, they continue to count even when the CPU core is halted by a debugger.
* **Instant Interrupt Execution**: While the core is halted, `TIM2` continues counting. Once the debugger resumes the CPU core, the timer immediately asserts its interrupt request, triggering `TIM2_IRQHandler` to execute and blink the LED during active test windows.

```cpp
/* Configure TIM2 to overflow every 1.0 second */
void timer_start(void) {
    // 1. Enable TIM2 clock on APB1
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    __asm__ volatile ("dsb\n isb\n");

    // 2. Fetch APB1 clock frequency and calculate timer clock frequency
    std::uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    std::uint32_t tim_clk = pclk1;
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0) {
        tim_clk = 2 * pclk1;
    }

    // 3. Configure Prescaler to tick at 10 kHz, and Auto-reload for 1Hz overflow
    TIM2->PSC = (tim_clk / 10000) - 1;
    TIM2->ARR = 10000 - 1;

    // 4. Clear flag, enable interrupt, enable NVIC, and start timer
    TIM2->CNT = 0;
    TIM2->SR = ~TIM_SR_UIF;
    TIM2->DIER |= TIM_DIER_UIE;
    NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 |= TIM_CR1_CEN;
}
```

#### 10.4.3 The 24-bit SysTick Reload Register Limit
When using SysTick, the reload register is only **24 bits wide**, allowing a maximum value of `16,777,215`.
* **The Failure**: Attempting to load `SystemCoreClock` (e.g., `100,000,000` or `168,000,000` for a 1-second interval) overflows the register, causing `SysTick_Config` to immediately fail and abort configuration. The timer then falls back to the default high-frequency 1000 Hz tick rate configured by the HAL.
* **The Solution**: If SysTick is required for low-frequency intervals, configure it to run at a high frequency (like **1000 Hz**), and use volatile tick counters inside the ISR to implement low-frequency actions:
  ```cpp
  extern "C" void SysTick_Handler(void) {
      g_systick_counter = g_systick_counter + 1;
      if (g_systick_counter >= 1000) { // 1000ms = 1.0 second
          g_systick_counter = 0;
          blink_leds[0].turn_on();
          g_led_off_counter = 100;    // LED on for 100ms
      }
      
      if (g_led_off_counter > 0) {
          g_led_off_counter = g_led_off_counter - 1;
          if (g_led_off_counter == 0) {
              blink_leds[0].turn_off();
          }
      }
  }
  ```

#### 10.4.4 Early Boot Starvation Guard
At boot, the HAL initializes SysTick to fire at 1000 Hz. If the custom `SysTick_Handler` uses a busy-wait delay (like `delay_ms(100)`) inside the ISR *before* the SysTick timer is reconfigured to a lower frequency, the handler will take 100ms to complete and will immediately re-trigger, completely starving the main thread.
* **Resolution**: Keep a `volatile bool g_blink_enabled = false` status flag. Guard the ISR with this flag, and only set it to `true` after reconfiguring the timer to a safe frequency.

#### 10.4.5 SysTick-Dependent Delay Lockup inside High-Priority ISRs
A common pitfall in embedded systems is calling a platform delay helper (like `delay_ms(100)`) from inside a hardware timer interrupt handler (such as a `TIM2` ISR).
* **The Failure**: Typical platform delay loops (e.g. in `board.c`) spin on a global millisecond counter (`s_millis`) that is updated by the **SysTick exception handler**:
  ```c
  void delay_ms(uint32_t ms) {
      uint32_t start = s_millis;
      while ((s_millis - start) < ms) { /* spin */ }
  }
  ```
  Since the processor is in Handler Mode executing the high-priority `TIM2_IRQHandler`, the `SysTick` exception (which usually runs at equal or lower priority) is blocked from executing. As a result, `s_millis` never increments, and the loop spins forever, locking up the CPU with the LED permanently on.
* **The Solution**: Avoid calling any timer-tick-dependent delay functions inside an ISR context. Instead, implement a freestanding instruction-based busy loop (`local_delay_ms`) that burns CPU cycles directly without relying on peripheral interrupts:
  ```cpp
  inline void local_delay_ms(std::uint32_t ms) {
      // 16 MHz HSI clock: 5333 loop cycles per millisecond
      std::uint32_t count = 5333 * ms;
      __asm__ volatile (
          "1:\n"
          "subs %0, %0, #1\n"
          "bne 1b\n"
          : "+r"(count)
          :
          : "cc"
      );
  }
  ```

---

### 10.5 Summary of Architectural Design Choices

| Metric | Main Firmware Trampoline | Assembly Module Wrapper | TIM2 Hardware Timer (Host-Debug) |
| :--- | :--- | :--- | :--- |
| **Interrupt Latency** | Low (Minimal trampoline overhead) | Low (Wrapper call overhead) | Low (ISR entrance overhead) |
| **GOT Base (`r10`)** | Handled by Firmware Trampoline | Handled by Module Assembly | Handled by ISR wrapper if PIC |
| **Debugger Stability** | Frozen during BKPT halts | Frozen during BKPT halts | **Active during BKPT halts** |
| **C/C++ Code Eligibility** | Yes (Standard PIC function) | Yes (Wrapped via ASM entry) | Yes (Direct handler routing) |
| **Implementation Risk** | Low (Centralized in main firmware) | High (Custom assembly needed) | Low (Standard timer module) |

*End of Full-Manual-SCL.md — covers the `scl::static` and `scl::xip-static` loader family, the shared `scl::common` core, the vendored `common-elf-reader`, the static and XIP execution paths, the build/test/run pipeline, and advanced interrupt/context-switching patterns.*
