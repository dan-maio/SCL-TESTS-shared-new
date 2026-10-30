# dynamic-shared — Cortex-M dynamic-loader SCL + simple POSIX tests

A self-contained subset of `shared-loaders-cortex-m-qemu` holding only the
**dynamic** shared-library loader family for Cortex-M (ARM32), following the same
approach as its sibling `static-shared`:

| Target | What it is |
|---|---|
| `scl::dynamic-nolib` | Freestanding `dlopen` — `dlopen_source`/`dlsym`/`dlclose` over any `byte_source`; lazy GOT/PLT binding, cross-library `r10` routing, EHABI exidx. No libc. |
| `scl::dynamic-nolib` | Freestanding `dlopen` — `dlopen_source`/`dlsym`/`dlclose` over any `byte_source`; lazy GOT/PLT binding, cross-library `r10` routing, EHABI exidx. No libc. |
| `scl::dynamic-newlib` | Hosted loader behind a **POSIX `dlopen`/`dlsym`/`dlclose`/`dlerror` facade** (link-time binding + journaled revert). |
| `scl::xip-dynamic-nolib` / `scl::xip-dynamic-newlib` | **Execute-in-place** adapters: split R-X\.so stays in flash, only RW copied to RAM (r10 = data base). |
| `common-elf-reader` | The shared ELF32 reader/parser the loaders build against (source, no `.a`). |

Everything is an INTERFACE "source code library" — no archives; consumers compile
the sources with their own flags.

## Layout

```
src/                   common core (dynamic-info, symbol-resolver, reloc-applier, …)
src/dynamic-nolib/     dl.cpp (the dlopen loader)
src/dynamic-newlib/    dynamic-linker.cpp, patch-journal.cpp, image-memory.cpp, link-context.cpp
src/port/              Cortex-M port layer (pic-trampoline, dl-cross-call, lazy-resolve, unwind-find-exidx)
include/loader/        common loader headers
include/scl/           firmware-level headers
include/dynamic-*/     loader-specific public headers
common-elf-reader/     the ELF32 parser
test/                  platforms (hosted + freestanding), tools, fixtures, cmake helpers
test-dynamic/          the simple POSIX dlopen/dlsym/dlclose test app + fixtures
tools/                 host symbol-table generator (embed_symtab)
```

## The test — standard POSIX API only

`test-dynamic/test1` is a hosted (newlib) firmware that uses **only** the POSIX
dynamic-loading API:

```c
void* h = dlopen("/path/lib.so", 0);   // load
auto fn = (int (*)(int))dlsym(h, "p1_add");  // resolve
fn(42);                                 // call
dlclose(h);                             // unload
dlerror();                              // last error, if any
```

No loader internals, no C++ tricks — just load / call / unload. The `dl::` facade
(`scl::dynamic-newlib`) is exposed under the plain POSIX names in `main.cpp`.

The test covers **all 12 available fixtures**, rebuilt from source as plain
`-fPIC` (the dynamic loader's model — the committed `.so` under `test/fixtures/`
are built for the r10 static/XIP loaders): `c1`, `c2`, `cpp3`, `libprov`,
`libcons`, `libcppsh`, `libcppeh`, `ehx_catch`, `mylib1`, `mylib2`, `libsh1`,
`libsh2` — each is `dlopen`ed, one or two exports exercised, then `dlclose`d.
That covers plain C, C++ classes (heap from main), RTTI/EH, cross-module imports
(`libcons`→`libprov`, `ehx`→`libcppeh`, the cyclic `c1`↔`c2`↔`cpp3` cluster),
hard-float math and constructors. Fixtures are built by
`test-dynamic/fixtures-src/build.sh` into `test-dynamic/fixtures/`.

### XIP dynamic tests (`test-xip/`)

The same loaders behind an **execute-in-place** adapter: split `R-X | RW` PIC
`.so` fixtures (built by `test-xip/fixtures-src/build-fixtures.sh`) have their
R-X segment execute straight from flash while only RW is copied to RAM
(`r10` = data base). Two simple apps (no test framework), one per loader:

- `test-xip/test-xip-dynamic-nolib/test1` — `scl::xip-dynamic-nolib`
  (freestanding loader, `-pie -Wl,-E` main).
- `test-xip/test-xip-dynamic-newlib/test1` — `scl::xip-dynamic-newlib`
  (hosted loader, embedded symtab; separate HOSTED build).

```sh
./test-xip/build-and-run.sh <weact|nucleo|qemu> nolib
./test-xip/build-and-run.sh <weact|nucleo|qemu> newlib
```

## Build + run

```sh
# one shot (build fixtures + firmware + flash/run), per board:
./test-dynamic/build-and-run.sh <weact|nucleo|qemu> test1

# or configure manually (hosted toolchain):
cmake -S test-dynamic -B test-dynamic/build \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi-hosted.cmake \
      -DSCL_PLATFORM=board -DSCL_BOARD=weact
cmake --build test-dynamic/build -j
```

Prerequisites: `arm-none-eabi-gcc` + `openocd` (xPack installs auto-detected by
`tools-config.sh`); `qemu-system-arm` for the qemu target.
