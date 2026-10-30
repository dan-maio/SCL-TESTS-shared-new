# SCL-TESTS-shared — how the four loader test trees compare

This monorepo holds four loader **test trees** that all consume the same core —
the **`SCL-shared` submodule** (they no longer each bundle a copy). Each one
demonstrates a **different way of loading code at runtime** on a bare-metal
Cortex-M, and each ships a **simple, complete test** that shows it off. This
document explains — in plain English — what each test does, how simple its main
program is, and how the fixtures (the little libraries that get loaded) are built
and used. Everything is **hard-float only** (`-mfloat-abi=hard -mfpu=fpv4-sp-d16`).

| Test tree | Loader model | What gets loaded | Call style from `main` |
|---|---|---|---|
| `dynamic-shared` | POSIX-style dynamic loading (`scl::dynamic-newlib` / `-nolib`) | shared libraries (`.so`, `ET_DYN`) | `dlopen` / `dlsym` / `dlclose` |
| `static-shared` | static patching (`scl::static` / `scl::xip-static`) | shared libraries (`.so`, `ET_DYN`) | plain call, **by name** |
| `optimal-shared` | XIP, no-`r10` fixed windows (`scl::optimal`) | shared libraries (`.so`, `ET_DYN`) | plain call, **by name** |
| `relocatable-shared` | module loading (`scl::reloc`) | relocatable objects (`.rel.o`, `ET_REL`) | plain function pointer |
| `overlay-shared` | linker overlays (`scl::overlay`) | own modules built into the firmware (no `.so`) | plain call, **by name** (link-bound) |

The first four *load a module at runtime*; **`overlay-shared` is different in
kind** — its modules are built with the firmware and bound at link time to a shared
SRAM window, and "loading" is a `memcpy`. It benchmarks the overlay technique
head-to-head against `dlopen` (same results, a fraction of the machinery).

---

## 1. The three loaders in one sentence each

- **dynamic** (`scl::dynamic-newlib` / `-nolib`): like Linux `dlopen` — you ask
  for a library *by file path*, get back a handle, look up symbols, call them,
  and `dlclose` when done.
- **static** (`scl::static`, `scl::xip-static`, and its no-pic `xip::nopic` flavour): the library is
  *pre-patched* at load so `main` can call its functions **by name** as if they
  were linked in — no lookup, no trampoline (and the no-pic variant even drops
  the r10 register dance).
- **relocatable** (`scl::reloc`): loads a plain **compiled-but-unlinked object**
  (`.rel.o`) into a RAM arena and applies its relocations — like a kernel
  module. No GOT/PLT at all; resolved functions are plain function pointers.

---

## 2. The tests side by side

| | `dynamic-shared` | `static-shared` | `relocatable-shared` |
|---|---|---|---|
| Test apps | `scl-fw-posix-dl`, `scl-fw-xip-nolib1`, `scl-fw-xip-newlib1` | `f411ce-sh`, `scl-fw-static-direct`, `scl-fw-drv`, 6 `scl-fw-xip-static*` apps | `scl-fw-reloc` |
| Main program | ~25 lines of `dlopen`/`dlsym`/call/`dlclose` | the `no-pic-simple` app is ~15 lines of **by-name** calls | ~20 lines of load + call |
| Fixtures loaded | 12 (POSIX) + 10 XIP-split | 10 PIC + XIP set | 9 `.rel.o` |
| What it proves | C, C++, RTTI, cross-module, heap, math, error path, XIP | C, C++/EH, GOT patching, by-name, no-r10, XIP, interrupts | C, C++, cross-module, **cyclic sets**, math, heap |
| Boards | weact / nucleo / qemu | weact / nucleo / qemu | weact / nucleo / qemu |

---

## 3. How simple is the main test? (the key idea)

The whole point of these repos is that the *test* is easy to read, not a fancy
test framework. Each main does load → call → print → unload.

### dynamic — `dlopen/dlsym/dlclose` (POSIX, like Linux)

```c
void* h = dlopen("/fixtures/mylib1.so", 0);         // load
auto fn = (unsigned (*)(unsigned))dlsym(h, "library_function"); // resolve
fn(42);                                              // call
dlclose(h);                                          // unload
```

The 4 POSIX functions are one-line wrappers over the `dl::` facade. The rest of
`main` is a short list of `open_so(...)` + `call1(...)` lines, one per fixture.

### static — call **by name** (the no-pic simple app)

```c
// main declares the exports as plain externs, links the fixtures, and the
// loader fills main's GOT at load (patch_main). Then:
s_add(10, 5);      // looks like an ordinary linked function — no dlopen at all
cpp3_method(7, 5);
```

This is the *simplest possible* calling convention: there is literally no loader
call in `main`, just declarations.

### relocatable — plain function pointer

```c
reloc::module_set s = load_set(pr, 2);              // link relprov+relmod together
unsigned r = ((unsigned (*)(unsigned, unsigned))
              reloc::sym(s, "prov_add"))(10, 20);    // resolve + call, no GOT/r10
```

---

## 4. Fixtures: building and usage

All three repos rebuild their fixtures from small C/C++ sources with the exact
flags their loader needs, then the firmware **reads them over semihosting** from
a folder on the host (no board filesystem dependency).

| | Where built | Recipe | How the test uses them |
|---|---|---|---|
| **dynamic** | `test-dynamic/fixtures-src/build.sh` → `test-dynamic/fixtures/` | plain `-fPIC -shared -Wl,-E` | `dlopen` by path, `dlsym` the export, `dlclose` |
| **dynamic XIP** | `test-xip/fixtures-src/build-fixtures.sh` → `test-xip/fixtures/` | r10 single-pic-base + **split R-X\|RW** (`xip-split.ld`) so text runs from flash | `xip::*::dlopen` on the flash blob, `pic_call` (r10), unload |
| **static** | committed `.so` in `test/fixtures/` (+ `test-xip/` splits, no-pic set) | `-fPIC` / split / `-fno-pic -mlong-calls` | linked into `main` (`-l:*.so`), loaded from flat-fs, GOT patched |
| **relocatable** | `test-relocatable/fixtures-reloc/Makefile` → `.../build/*.rel.o` | `-mlong-calls -fno-pic -r` (partial link, stays relocatable) | read over semihosting, `reloc::load` / `load_set` into a RAM arena |

**Completeness of fixtures**: each repo exercises the *full* shared set —
`mylib1/2`, `libprov/libcons` (a provider + a consumer, i.e. cross-module calls),
`c1/c2/cpp3` (a three-way mutually-dependent cluster), plus C++ class
(`libcppsh`), RTTI/exceptions (`libcppeh`, `ehx_catch`) and hard-float math
(`cpp3`). `dynamic` adds 12 fixtures total and even reuses `libsh1/2` (the
static loader's own demo libs); `relocatable` adds `relprov`/`relmod`, a pair
purpose-built to show an ET_REL module calling another module.

---

## 5. Completeness — what the tests together cover

- **C and C++** libraries (classes, virtual calls, heap from `main`).
- **Cross-module calls** (lib A calls lib B): `libcons→libprov`, `ehx→libcppeh`,
  `c1↔c2↔cpp3` — including the **cyclic** cluster via `load_set` (relocatable)
  and the loader's pending re-resolution (dynamic).
- **RTTI** (`dynamic_cast`) and exceptions across the module boundary.
- **Constructors** (`.init_array`) running at load.
- **Execute-in-place**: text runs from flash, only data is copied to RAM
  (dynamic XIP and static XIP).
- **No-r10 direct calls** (static no-pic).
- **Math** (sinf/cosf/logf/sqrtf) and **heap** (malloc/new) borrowed from `main`.
- **Error paths**: missing file → clean `dlerror()`.
- **Interrupts** (static XIP interrupt apps).

Every project is verified under **QEMU** and on **WeAct** hardware, with the
same results.

---

## 6. Running everything

```sh
# dynamic
./dynamic-shared/test-dynamic/build-and-run.sh   <weact|nucleo|qemu> test1
./dynamic-shared/test-xip/build-and-run.sh       <weact|nucleo|qemu> nolib|newlib

# static (the simplest no-r10 by-name test)
./static-shared/test-xip/build-and-run.sh        <weact|nucleo|qemu> static-no-pic-simple

# relocatable
./relocatable-shared/test-relocatable/build-and-run.sh <weact|nucleo|qemu>
```

Each `build-and-run.sh` builds the fixtures first, configures + builds the
firmware, then flashes/runs it and streams the semihosting console.

---

## 7. Takeaway

Three loaders, three calling conventions, one philosophy: **a tiny, readable
`main`** that loads the **full fixture set** (rebuilt with exactly the right
flags) and exercises it end-to-end. Pick the loader whose calling style matches
your firmware: `dlopen` (dynamic), call-by-name (static), or kernel-module-style
objects (relocatable).

## Dual-core: the Pi-Zero RP2350B results

`pico2-pizero-shared/` in `SCL-TESTS-shared-new` runs these loaders on a Pi-Zero RP2350B
(2x Cortex-M33, 16 MB flash, 8 MB PSRAM) under the µOS++ SMP kernel. Every load and patch
happens on core 0 at boot, with core 1 still in the bootrom. Only then does SMP start,
and both cores just call the modules.

| Loader | Code can run from | Loader code at call time | Real C++ throws |
|---|---|---|---|
| `scl::reloc` | SRAM, PSRAM | none | not supported |
| `scl::static` | SRAM, PSRAM | none | not supported |
| `scl::optimal` | SRAM, PSRAM | none | not supported |
| `scl::overlay` | SRAM, PSRAM | none; swap only at boot | not tested |
| `scl::dynamic-newlib` | SRAM (heap) | none | not supported |
| `scl::dynamic-nolib` | SRAM, PSRAM | every call into main or another module | yes |
| `scl::xip-dynamic-newlib` | flash | every call into main or another module | yes |
| `scl::xip-dynamic-nolib` | flash | every call into main or another module | yes |
| `scl::xip-static` | flash | calls between modules only | yes |

So if no loader code may run once SMP has started, choose from the first five. The `r10`
models need a real recursive `scl_loader_lock`, because their call-time lookup takes it.

Rules these results depend on:

- **Load after static constructors,** from a wrapped `main()` for example. SCL's globals are
  C++ objects, so an earlier load is wiped when they are constructed.
- **Call `first_missing_import()` after loading the set.** A missing import does not fail
  the load, because a later module may provide it. Otherwise the first call faults.
- **Pass `flash_base` to `patch_main_imports()` or `bind_host_imports()`** when a flash
  module uses exceptions. Catch tables read the data segment's flash copy.
- **Hold a lock for exceptions.** This toolchain's `libstdc++` is `Thread model: single`,
  so one global exception state is shared. Throwing on both cores at once is untested.
- **Link the full `libsupc++`** when using `--specs=nano.specs`, whose `__cxa_throw` has no
  unwind tables, and keep `.ARM.exidx` as its own output section so `ld` sorts it.
- **Do not rely on `LDREX`/`STREX` across cores** on this chip. Import a lock from main.
- **An interrupt can enter a module** if it saves and restores `r10` around the call, on
  every model tested. Do not call a function that takes a blocking RTOS mutex from an ISR.
