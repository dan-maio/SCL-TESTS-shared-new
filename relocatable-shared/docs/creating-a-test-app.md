# Creating a new test app in relocatable-shared

A plain-English recipe for adding a firmware test app that loads **relocatable
objects** (`ET_REL` `.rel.o`) through `scl::reloc`. You write one app folder
(`src/`, `CMakeLists.txt`) plus an `reloc.ld`, and the SCL — including the
vendored `common-elf-reader` parser — is pulled in automatically.

> The pattern is exactly what `test-relocatable` (the `scl-fw-reloc` app)
> follows.

---

## 1. The app folder

```
test-relocatable/
├── CMakeLists.txt        # the recipe (below)
├── src/
│   └── main.cpp          # your firmware logic
├── platform/
│   └── reloc.ld          # plain NON-PIC firmware linker script
├── fixtures-reloc/       # ET_REL fixtures (sources + Makefile)
│   ├── Makefile
│   └── src/<name>.c
└── build-and-run.sh
```

`src/main.cpp` loads modules, resolves exports, and calls them as **plain
function pointers**:

```c
reloc::module_set s = load_set(pr, 2);          // link relprov+relmod together
unsigned r = ((unsigned (*)(unsigned, unsigned))
              reloc::sym(s, "prov_add"))(10, 20); // resolve + call — no GOT, no r10
```

`main` also provides a `main_resolve` callback: it maps the symbol names the
modules import (`trace_event`, `AA`, `printf`, …) to addresses in `main`. That
is the only "extra" work the reloc loader asks of you.

---

## 2. `CMakeLists.txt` — the recipe, explained

```cmake
project(scl-reloc-tests LANGUAGES C CXX)

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)      # repo root
add_subdirectory(${SCL_ROOT} scl)                  # the SCL library package

# freestanding platform sources (no libc)
set(PLAT ${SCL_ROOT}/test/platforms/${SCL_PLATFORM})   # board | qemu-f411re
add_executable(scl-fw-reloc
    src/main.cpp
    ${PLAT}/src/startup.c
    ${PLAT}/src/semihost.c
    ${PLAT}/src/board.c
    ${SCL_ROOT}/test/platforms/common/src/crt.cpp       # new/delete + malloc
    ${PLAT}/src/libc-shim.c)

target_link_libraries(scl-fw-reloc PRIVATE scl::reloc)  # loader + parser
target_compile_options(scl-fw-reloc PRIVATE
    -fno-pic -fno-pie -Os ...)                          # ordinary non-PIC main
target_link_options(scl-fw-reloc PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld     # no _DYNAMIC/GOT
    -nostdlib -nostartfiles -Wl,--gc-sections)
target_link_libraries(scl-fw-reloc PRIVATE ${_libm} ${_libgcc})  # libm/libgcc
```

### What each piece does

- **`add_subdirectory(${SCL_ROOT} scl)`** adds `scl::common` + `scl::reloc`, and
  transitively **`common-elf-reader`** — `scl-common` links
  `common-elf-reader::elf-reader`, so the parser sources compile into your app
  automatically (no submodule wiring by hand).
- **`scl::reloc`** is the ET_REL loader (`src/reloc/reloc-loader.cpp` +
  `reloc/reloc-loader.hpp`).
- The **freestanding platform** (`startup.c`/`semihost.c`/`board.c`/
  `libc-shim.c` + `crt.cpp`) gives you a bare-metal `main` with semihosting
  console and a static heap.
- **`-fno-pic -fno-pie` + `reloc.ld`**: this firmware is deliberately NOT
  position-independent — there is no `.dynsym`, no GOT-in-RAM, no `_DYNAMIC`
  (compare the dynamic/static apps, which link `-pie -Wl,-E`).

Finishing touches (mirror `test-relocatable/CMakeLists.txt`): build the `.rel.o`
fixtures with a `make` custom target, point the app at them with
`RELOC_LIB_DIR`, and register a qemu/flash test.

---

## 3. Fixtures — building and usage

Relocatable fixtures are compiled then **partial-linked** (`ld -r`) so they stay
`ET_REL` (unresolved symbols and relocations are left for the loader):

```
compile:  arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfloat-abi=hard \
          -mlong-calls -fno-pic -Os -ffreestanding -fno-common -c <name>.c
link:     arm-none-eabi-ld -r <name>.o -o build/<name>.rel.o
```

`-mlong-calls -fno-pic` is the whole point: every external call becomes an
`R_ARM_ABS32` literal the loader patches with a runtime address — **no GOT, no
PLT, no r10**. `test-relocatable/fixtures-reloc/Makefile` already does this for
9 modules (`relprov`, `relmod`, `mylib1/2`, `c1/c2/cpp3`, `libprov/libcons`);
add a module by dropping a source in `fixtures-reloc/src/` and listing it in
`C_FIXTURES`.

The app reads each `.rel.o` over semihosting from `RELOC_LIB_DIR`, loads it with
`reloc::load` (single) or `reloc::load_set` (a group with cross-module/cyclic
dependencies) into a RAM arena, resolves exports via `reloc::sym`, and calls
them directly.

---

## 4. Integrating into the run flow

`test-relocatable/build-and-run.sh` already does the whole cycle: build the
fixtures (via the CMake `reloc-fixtures` target) → configure with
`cmake/arm-none-eabi.cmake` → build → flash (OpenOCD) or run under QEMU. Run
with:

```sh
./test-relocatable/build-and-run.sh <weact|nucleo|qemu>
```

The app ends with `RESULT: done` (and `semihost_exit(0)` on the board) so the
runner knows it finished.

---

## 5. Summary

1. Make `test-relocatable/{CMakeLists.txt,src/main.cpp,platform/reloc.ld}`.
2. Write plain load/`sym`/call code + a small `main_resolve` symbol table.
3. Link the freestanding platform + `scl::reloc`.
4. `add_subdirectory(${SCL_ROOT} scl)` gives you SCL + `common-elf-reader` for
   free.
5. Add ET_REL fixtures (compile `-mlong-calls -fno-pic`, `ld -r`), point
   `RELOC_LIB_DIR` at them.
6. `build-and-run.sh` does the rest.
