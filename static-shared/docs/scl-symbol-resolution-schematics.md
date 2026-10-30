# SCL Symbol Resolution Pipeline — Call Stack Schematics

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Relocation Types Reference](#2-relocation-types-reference)
3. [GOT / PLT / .got.plt Architecture](#3-got--plt--gotplt-architecture)
4. [Function Call Flows](#4-function-call-flows)
5. [Data Symbol Access](#5-data-symbol-access)
6. [Source File Index](#6-source-file-index)
7. [Test Files — Where Each Resolution Happens](#7-test-files--where-each-resolution-happens)

---

## 1. Architecture Overview

SCL's **static loader pipeline**:

```
┌─────────────────────────────────────────────────────────────────┐
│                      SCL Loader Pipelines                       │
├─────────────────────────────────────────────────────────────────┤
│ Static (hosted)                                                 │
├─────────────────────────────────────────────────────────────────┤
│ shlib-loader.cpp                                                │
│ Module::build()                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Relocation Types Reference

### ET_DYN (Shared Objects)

| Type | Value | GOT Write Formula | Used For |
|------|-------|-------------------|----------|
| `R_ARM_NONE` | 0 | (no write) | Registration-only entries |
| `R_ARM_ABS32` | 2 | `*slot += sym_target()` | Absolute data pointers |
| `R_ARM_REL32` | 3 | `*slot += sym_target() - (base + r_offset)` | PC-relative references |
| `R_ARM_COPY` | 20 | `memcpy(slot, src, st_size)` | Data in main from lib |
| `R_ARM_GLOB_DAT` | 21 | `*slot = sym_target()` | Data GOT entries |
| `R_ARM_JUMP_SLOT` | 22 | `*slot = sym_target()` | Function PLT entries |
| `R_ARM_RELATIVE` | 23 | `*slot += base` | Local data pointers |

---

## 3. GOT / PLT / .got.plt Architecture

### Memory Layout (per loaded library)

```
┌─────────────────────────────────────────────────────────┐
│                    Loaded Library Image                  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .text (code)                                      │  │
│  │   PLT stub 0:  add ip,pc,#off; ldr pc,[ip]       │  │
│  │   PLT stub 1:  add ip,pc,#off; ldr pc,[ip]       │  │
│  │   PLT stub N:  ...                                │  │
│  │   Library code:  bl PLT_stub_N                    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .got (Global Offset Table)                        │  │
│  │   GOT[0]: data symbol A address  (GLOB_DAT)      │  │
│  │   GOT[1]: data symbol B address  (GLOB_DAT)      │  │
│  │   GOT[2]: __gxx_personality_v0   (GLOB_DAT)      │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .got.plt (PLT GOT)                                │  │
│  │   GOT.plt[0]: &_GLOBAL_OFFSET_TABLE_ (reserved)  │  │
│  │   GOT.plt[1]: link_map (reserved)                │  │
│  │   GOT.plt[2]: dl_runtime_resolve (reserved)      │  │
│  │   GOT.plt[3]: func X target  (JUMP_SLOT)        │  │
│  │   GOT.plt[4]: func Y target  (JUMP_SLOT)        │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .rel.dyn / .rel.plt (Relocation Tables)           │  │
│  │   {r_offset, r_info(type, sym)} for each reloc    │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │ .dynsym / .dynstr / .hash (Symbol Table)          │  │
│  │   Symbol resolution via ELF hash chain walk       │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### PLT Stub Anatomy (ARM Thumb)

```
PLT_stub_N:                         GOT.plt[N]:
┌──────────────────────┐            ┌──────────────────────┐
│ add ip, pc, #page_hi │            │                      │
│ add ip, ip, #page_lo │───────►    │ target_func_addr     │
│ ldr pc, [ip]         │            │ (or dl_runtime_resolve│
└──────────────────────┘            │  before first call)  │
                                    └──────────────────────┘
```

---

## 4. Function Call Flows

### 4a. Main → Library (Eager Binding)

```
┌─────────────────────────────────────────────────────────────────┐
│ CALLER: main (compiled -fpic, no r10 usage)                     │
│                                                                 │
│   // C code: lib_func(42);                                      │
│   mov r0, #42                                                   │
│   bl  <PLT_stub_for_lib_func>     ◄── linker-generated PLT     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT STUB (in main's .plt)                                       │
│                                                                 │
│   add ip, pc, #page_offset       ; ip = &main_GOT.plt[N]       │
│   ldr pc, [ip]                   ; jump to *GOT.plt[N]          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ GOT.plt[N] (after reloc_applier applied JUMP_SLOT)              │
│                                                                 │
│   contains: absolute address of lib_func                        │
│   i.e.: library_base + lib_func_offset                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY: lib_func                                               │
│                                                                 │
│   ; Function body uses r10-relative GOT access                  │
│   ldr r3, [r10, #offset]    ; r3 = GOT[sym] = &some_global     │
│   ldr r0, [r3]              ; r0 = some_global value            │
│   ...                                                           │
│   bx lr                     ; return to main                    │
└─────────────────────────────────────────────────────────────────┘
```

### 4b. Library → Main (Callback)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY: event_handler()                                        │
│                                                                 │
│   bl  <PLT_stub_for_main_callback>                              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ PLT stub → GOT.plt[N] = &main_callback                          │
│                                                                 │
│   (resolved via combined_resolver: other libs → main)           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ MAIN: main_callback()                                           │
│                                                                 │
│   ; main is -fpic (PC-relative GOT), does NOT use r10           │
│   ; r10 value is irrelevant for main                            │
│   bx lr                                                         │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. Data Symbol Access

### 5a. Library → Own Global (GLOB_DAT, Eager)

```
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY CODE (compiled -msingle-pic-base -mpic-register=r10)    │
│                                                                 │
│   // C code: int x = my_global;                                 │
│   ldr r3, [r10, #GOT_offset_for_my_global]   ; r3 = &my_global │
│   ldr r0, [r3]                               ; r0 = my_global   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ GOT ENTRY (patched by reloc_applier, GLOB_DAT)                  │
│                                                                 │
│   GOT[k] = library_base + my_global_offset                      │
│                                                                 │
│   Relocation: R_ARM_GLOB_DAT                                    │
│   Formula:   *slot = sym_target()                               │
│   Source:    reloc-applier.cpp:46-48                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ DATA SECTION                                                    │
│                                                                 │
│   my_global: .word 0x42          ; actual data                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5b. Main → Library Data (R_ARM_COPY in Static Loader)

```
┌─────────────────────────────────────────────────────────────────┐
│ MAIN CODE                                                       │
│                                                                 │
│   // extern int lib_data;  x = lib_data;                        │
│   ldr r3, [pc, #GOT_offset_for_lib_data]   ; r3 = &lib_data    │
│   ldr r0, [r3]                              ; r0 = lib_data     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ MAIN's GOT ENTRY (patched by patch_main, R_ARM_COPY)            │
│                                                                 │
│   GOT[k] = library_base + lib_data_offset_in_lib                │
│                                                                 │
│   Static loader: shlib-loader.cpp:490-516                       │
│   Writes library symbol's runtime address into main's slot      │
│   (NOT standard memcpy semantics — see comment at line 491)     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ LIBRARY's DATA SECTION                                          │
│                                                                 │
│   lib_data: .word 42         ; actual storage in library        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. Source File Index

### Core Relocation Pipeline

| File | Purpose |
|------|---------|
| `common-elf-reader/include/loader/elf-types.hpp` | ELF structs, `rel_type` enum |
| `common-elf-reader/include/loader/detail/bytes.hpp` | Bounds-checked memory access |
| `include/loader/detail/elf-hash.hpp` | SysV ELF hash function |
| `src/relocator.cpp` | Relocation classification + plan |
| `src/reloc-applier.cpp` | Applies relocations to image |
| `src/symbol-resolver.cpp` | Hash-chain symbol lookup |
| `src/lazy-resolve.cpp` | JUMP_SLOT lazy resolution |
| `src/dynamic-info.cpp` | Parse _DYNAMIC array |
| `src/link-context.cpp` | Main's symbol surface |

### Static Loader

| File | Purpose |
|------|---------|
| `include/static/shlib-loader.h` | shlib_loader class API |
| `src/static/shlib-loader.cpp` | Two-phase GOT patching |
| `src/static/elf-reader.cpp` | ELF parsing, hash lookup |
| `src/static/module.cpp` | Module::build() orchestrator |

---

## 7. Test Files — Where Each Resolution Happens

### 7a. Fixture Libraries Source Map

Each `.so` fixture exercises specific relocation types:

| Fixture | Source | Exports | Imports | Relocations Exercised |
|---------|--------|---------|---------|----------------------|
| `mylib1.so` | `test/fixtures-src/mylib1/mylib1.c` | `library_function`, `foo`, `boo`, `lkj` (weak), `aa`, `bb` (data), `str` (RELATIVE) | `AA`, `BB`, `s1` (GLOB_DAT), `f`, `w` (JUMP_SLOT), `printf` | GLOB_DAT, JUMP_SLOT, RELATIVE, weak interposition |
| `mylib2.so` | `test/fixtures-src/mylib2/mylib2.c` | `lf`, `mm` (weak), `aaa`, `bbb` (data) | `AA`, `BB`, `w`, `printf` | GLOB_DAT, JUMP_SLOT, RELATIVE, weak interposition |
| `c1.so` | `test/fixtures-src/c1/c1.c` | `c1_call_c2`, `c1_into_fm`, `c1_data`, `c1_bss` | `c2_add` (cross-lib), `fm_var` (GLOB_DAT), `trace_event` (JUMP_SLOT) | Cross-call r10 swap, GLOB_DAT, JUMP_SLOT, RELATIVE |
| `c2.so` | `test/fixtures-src/c2/c2.c` | `c2_add`, `c2_chain`, `c2_relay`, `c2_scale` | `c1_into_fm` (cross-lib), `cpp3_step` (cross-lib), `fm_callback` | Cross-call r10 swap, hard-float s0 preservation |
| `cpp3.so` | `test/fixtures-src/cpp3/cpp3.cpp` | `cpp3_method`, `cpp3_orchestrate`, `cpp3_recurse`, `cpp3_heap` | `sinf`, `cosf` (libm), `c2_relay` (cross-lib), `malloc`, `_Znwj` (heap) | Cross-call r10 swap, hard-float, C++ heap, deep recursion |
| `libprov.so` | `test/fixtures-src/libprov/libprov.c` | `prov_value` | (none) | Pure export (no relocations) |
| `libcons.so` | `test/fixtures-src/libcons/libcons.c` | `cons_call` | `prov_value` (cross-lib) | Cross-library JUMP_SLOT |
| `libcppsh.so` | `test/fixtures-src/libcppsh/` | `cppsh_make`, `cppsh_compute` | `_Znwj`, `_ZdlPv` (heap) | C++ vtable, operator new/delete JUMP_SLOT |
| `libcppeh.so` | `test/fixtures-src/libcppeh/` | `eh_selftest`, `eh_throw`, `eh_dyncast` | `__cxa_*`, `__gxx_personality_v0` | EH runtime GLOB_DAT/JUMP_SLOT, .ARM.exidx |
| `ehx_catch.so` | `test/fixtures-src/ehx/ehx-catch.cpp` | `ehx_catch_cross` | `eh_throw` (cross-lib), `__cxa_*` | Cross-lib EH unwind through dl_cross_call |
| `libcpp_features.so` | `test-static/test-static-cpp/new-lib/cpp_features.cpp` | `cpp_features_run`, `cpp_features_summary` | `malloc`, `free` | C++ strategy pattern, GOT/PLT |

### 7b. Test Files — Mechanism to Source Mapping

#### GOT (R_ARM_GLOB_DAT) — Data Symbol Resolution

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-static/test-static/src/main.cpp` | `AA`, `BB`, `s1` | Static loader patch_shlib/patch_main |
| `test-static/test-static-direct/src/main.cpp` | `aa`, `bb` | Direct-link scl::Module |

#### PLT/.got.plt (R_ARM_JUMP_SLOT) — Function Call Resolution

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-static/test-static/src/main.cpp` | `library_function`, `f`, `w`, `printf` | Static loader GOT/PLT patching |
| `test-static/test-static-cpp/src/main.cpp` | `malloc`, `free`, `cpp_features_run` | C++ PLT entries |

#### R_ARM_RELATIVE — Local Pointer Rebasing

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-static/test-static/src/main.cpp` | String pointers in libsh1/libsh2 | Static loader RELATIVE patching |

#### Weak Symbol Interposition

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-static/test-static/src/main.cpp` | `foo`, `boo`, `lkj`, `mm` | Static loader symbol_table: first-loaded-wins for global, weak appended |
| `test-static/test-static-direct/src/main.cpp` | `foo`, `boo` | Direct-link weak resolution |

#### XIP (Execute from Flash)

| Test File | Symbols | Description |
|-----------|---------|-------------|
| `test-xip/test-xip-static/semihosting/` | Same as static | Static loader, XIP from flash |
| `test-xip/test-xip-static-direct/` | Same as static-direct | Direct-link, XIP |
| `test-xip/test-xip-interrupt-static/` | Same + ISR | XIP with interrupt handling |

### 7c. Cross-Reference: Mechanism → Test Files

| Mechanism | Test Files |
|-----------|------------|
| **GOT (GLOB_DAT)** | test-static/* |
| **PLT (JUMP_SLOT)** | test-static/* |
| **RELATIVE** | test-static/* |
| **Weak interposition** | test-static/* |
| **XIP** | test-xip/* |

---

## Quick Reference Card

```
┌─────────────────────────────────────────────────────────────────┐
│                    SCL SYMBOL RESOLUTION                        │
│                    Quick Reference                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  FUNCTION CALL (ET_DYN):                                        │
│    caller → bl PLT_stub → ldr pc,[GOT.plt] → target            │
│                                                                 │
│  DATA ACCESS (ET_DYN):                                          │
│    ldr r3,[r10,#GOT_off] → ldr r0,[r3] → data value            │
└─────────────────────────────────────────────────────────────────┘
```
