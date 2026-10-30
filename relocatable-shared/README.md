# relocatable-shared — Cortex-M relocatable-loader SCL + simple test

A self-contained subset of `shared-loaders-cortex-m-qemu` holding only the
**relocatable** (ET_REL) loader family for Cortex-M (ARM32), alongside its
siblings in this workspace (`dynamic-shared`, `static-shared`):

| Target | What it is |
|---|---|
| `scl::reloc` | Loads ELF32 **relocatable** modules (`.rel.o`, built `-mlong-calls -fno-pic -r`) into a caller-supplied RAM arena — like a Linux kernel module. **No GOT, no PLT, no r10, no `_DYNAMIC`**; eager `R_ARM_ABS32`/`TARGET1`/`REL32`/`THM_CALL` relocation; `load_set()` for cyclic module groups. |
| `common-elf-reader` | The shared ELF32 reader/parser the loader builds against (source, no `.a`). |

Everything is an INTERFACE "source code library" — no archives; consumers compile
the sources with their own flags.

## Layout

```
src/reloc/reloc-loader.cpp   the ET_REL loader
src/                       shared core (reloc-applier, symbol-resolver, log, …)
include/loader/, include/scl/   common headers
include/reloc/reloc-loader.hpp  public API
common-elf-reader/          the ELF32 parser (git submodule, branch static-shared)
test/                       freestanding platforms (board, qemu-f411re, common),
                            tools (flash-run.sh, qemu-run.sh), fixtures-src
test-relocatable/           the ET_REL test project
cmake/ , tools-config.sh
```

## The test — plain C, no fancy C++

`test-relocatable/src/main.cpp` loads ET_REL modules, resolves their imports
against main (and across a loaded set), runs their constructors, and calls
exported functions as plain function pointers:

```cpp
reloc::module_set s = load_set(pr, 2);            // relprov + relmod (one link unit)
uint32_t r = ((uint32_t (*)(uint32_t, uint32_t))   // reloc::sym() -> address
              reloc::sym(s, "prov_add"))(10, 20);  // call, no GOT/PLT/r10
```

It exercises all 9 fixtures rebuilt as ET_REL (`relprov`, `relmod`, `mylib1`,
`mylib2`, `c1`, `c2`, `cpp3`, `libprov`, `libcons`) — including the cyclic
`c1`/`c2`/`cpp3` cluster via `load_set`, and cross-module calls (`relmod` →
`relprov`, `libcons` → `libprov`).

## Build + run

```sh
# one shot (build fixtures + firmware + flash/run):
./test-relocatable/build-and-run.sh <weact|nucleo|qemu>
```

Prerequisites: `arm-none-eabi-gcc` + `openocd` (xPack installs auto-detected by
`tools-config.sh`); `qemu-system-arm` for the qemu target.
