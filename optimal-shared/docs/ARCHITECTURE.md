# optimal-shared — architecture, usage, fixtures, main, and build characteristics

`optimal-shared` is a Cortex-M (ARM32) application that loads shared libraries at
runtime and calls their functions **directly, by name**, with no `dlopen`/`dlsym`,
no hand-written glue, and no per-library build files — the same way whether the
code executes from RAM or is provisioned in on-chip flash (XIP). It is the
"optimal" member of the SCL workspace: it keeps the readable calling model of the
static no-pic loader and mechanizes away its remaining manual steps.

This document explains the design, how to use it, how the fixtures are built, how
`main` is written, and the build characteristics that make it work.

---

## 1. The three requirements and how each is met

| Requirement | Mechanism |
|---|---|
| **Direct calls, by name** (`s_add(10, 5)`, a cast only to type a raw address) | libraries are `-fno-pic -mlong-calls`, linked at fixed addresses; `main` links them (`-l:*.so`) and `xip::nopic::patch_main()` fills `main`'s GOT/PLT at load |
| **Works on RAM and XIP** | the loader copies each module's R-X and R-W segments into reserved SRAM windows; XIP provisions the image into flash and the loader streams it in — the calling convention is identical |
| **No hand-made files / no manual code-gen** | one generic fixture recipe auto-packs all addresses **and derives the SRAM windows** (`windows.generated.ld`) from RAM geometry — no typed addresses anywhere; `gen-imports` emits the load list; `main` iterates the list in a loop |

The single design decision that unlocks all three is **fixed-address `-fno-pic`
modules**: because every intra-module reference is an absolute address baked in by
the linker, there is **no GOT, no PLT, no PIC base register (r10)** in the module,
and `main` can call an export with a plain call. The classic cost of fixed
addresses — hand-assigning them — is removed by auto-packing (§4).

---

## 2. Architecture

### 2.1 Components

```
          host build machine                         target (Cortex-M / QEMU)
┌───────────────────────────────────┐        ┌──────────────────────────────────┐
│ fixtures-src/build.sh              │        │ flash: flat-fs archive of *.so    │
│   one recipe, -fno-pic -mlong-calls│  .so   │   (provisioned at boot over       │
│   auto-pack addresses  ───────────────────► │    semihosting from the host)     │
│   emit fixtures/manifest.txt       │        │                                   │
│                                    │        │ SRAM window _sxip_text_* (R-X)    │
│ tools/gen-imports.py               │        │ SRAM window _sxip_data_* (R-W)    │
│   .so -> modules.generated.inc     │        │                                   │
│         imports.generated.h (opt.) │        │ main:  load loop → patch_main     │
│                                    │        │        s_add(10,5)  (direct call) │
│ CMake: scl::* + common-elf-reader  │        │                                   │
│   as INTERFACE source libraries    │        │ SCL loader (compiled into main)   │
└───────────────────────────────────┘        └──────────────────────────────────┘
```

### 2.2 Self-contained: root IS the SCL library

Like every other SCL-shared project, the **repository root is the SCL source
library** — everything it compiles is in-tree, no path outside the repo:

- `src/` and `include/` hold the SCL loader/patcher sources (plus our thin glue,
  `include/optimal/loader.hpp` + `src/loader.cpp`).
- `cmake/scl-lib.cmake` defines the `scl::*` INTERFACE targets.
- `common-elf-reader/` is a **git submodule at the project root**, beside
  `src/`/`include/`.

The loader and the ELF reader are **not** prebuilt archives — they are `INTERFACE`
targets whose sources propagate into each firmware and compile with its flags. The
root CMake wires them:

```cmake
include(cmake/scl-lib.cmake)                   # scl::* + common-elf-reader::elf-reader

add_library(scl-loader-nopic INTERFACE)        # the no-pic loader + two port sources
target_sources(scl-loader-nopic INTERFACE
    src/xip/static/nopic.cpp                   # loader + patch_main
    src/image-memory.cpp                       # provision staging arena
    src/link-context.cpp)                      # main's own .dynsym (import resolution)
target_link_libraries(scl-loader-nopic INTERFACE scl::xip-common)  # → scl::common → elf-reader
add_library(scl::loader-nopic ALIAS scl-loader-nopic)

add_library(optimal INTERFACE)                 # our reusable glue (include/optimal + src)
target_sources(optimal INTERFACE src/loader.cpp)
target_link_libraries(optimal INTERFACE scl::loader-nopic)
```

`common-elf-reader::elf-reader` (reached transitively through `scl::xip-common →
scl::common`) is the ELF32 parser the loader uses to read the `.so` blobs — headers,
program headers, `.dynsym`/`.dynstr`, and the relocation tables.

### 2.3 Two self-contained apps (RAM / XIP)

Each test has its **own complete `main.cpp`** — `test-ram/src/main.cpp` and
`test-xip/src/main.cpp` — so the full by-name exercise is visible right there in
the app. The two files are identical apart from their blob source
(`provide()`/`setup()`):

| | test-ram | test-xip |
|---|---|---|
| where the `.so` bytes live | a static RAM arena filled over semihosting | the on-chip flat-fs (flash) |
| `provide()` returns | a span into that RAM buffer | a span into the flash-resident blob |
| everything after | `optimal::load_and_link` + the by-name exercise — **identical** | |

### 2.4 Load sequence (in the app)

1. **provision** — copy every `.so` from the host into the on-chip flat-fs
   (this is the "flash" side of XIP; under QEMU the flash region is emulated SRAM).
2. **load loop** — for each module, `xip::nopic::load()`:
   - copies the R-X segment into `_sxip_text_*` and the R-W segment into
     `_sxip_data_*` at the module's link-time (fixed) address — a 0-bias copy;
   - zeroes `.bss`;
   - applies `R_ARM_ABS32` for undefined imports (`value = S`, where `S` is the
     resolved address of a symbol exported by another module or by `main`);
   - runs `.init_array` (C++ constructors, `s_a` 10→15, etc.).
3. **`resolve_all()`** — re-resolves imports across the whole set so **cyclic**
   module↔module dependencies (`c1↔c2↔cpp3`) are satisfied once everyone is placed.
4. **`patch_main()` loop** — walk `main`'s own `_DYNAMIC` (`.rel.dyn` GLOB_DAT +
   `.rel.plt` JUMP_SLOT) and, for each slot whose symbol a module exports, write
   the module's fixed address into `main`'s GOT. From here, `s_add` in `main`
   resolves through `main`'s own GOT to the loaded module — a direct call.

Because the windows are RAM, all patching happens at **load time**; there is no
provision-time flash binding and no call-time trampoline.

---

## 3. `main` — how the application is written

`main` is ordinary C. It declares the exports it uses with their **real
prototypes** (the fixtures' API — the same thing a library header would give you)
and calls them directly:

```cpp
extern "C" {
unsigned int s_add(unsigned int, unsigned int);
unsigned int cons_call(unsigned int);
unsigned int cpp3_method(unsigned int, unsigned int);
// ...
}

int main() {
    for (const char* m : kModules) provision(m);       // kModules is GENERATED
    for (int i = 0; i < kNumModules; ++i) xload(kModules[i], g_mods[i]);
    xip::nopic::resolve_all();
    for (int i = 0; i < kNumModules; ++i) xip::nopic::patch_main(g_mods[i].get());

    s_add(10, 5);                 // ← direct, by name. no dlsym, no r10, no macro.
    cons_call(5);                 // cross-module (libcons → libprov)
    cpp3_method(7, 5);            // C++ class + sinf/cosf/logf/sqrtf borrowed from main
}
```

- **The load list is generated.** `kModules` is `#include
  "modules.generated.inc"`, emitted by `gen-imports` from the built `.so`s. Adding
  a library never edits `main`.
- **The prototypes are the library's API.** They are not "glue" — they are the
  same declarations any consumer writes to use a C library. You add a prototype
  only for a function you actually call. (If a fixture ships a header, include it
  instead.) For the zero-header case, `gen-imports` also emits
  `imports.generated.h` with one placeholder declaration per export; you then call
  by name with a cast to the prototype (argument types are not in an ELF symbol
  table). `main` here uses explicit prototypes because they read better.
- **No fancy C++.** No templates at the call site, no macros, no
  `reinterpret_cast` for a normal call.

---

## 4. Fixtures — one generic recipe, no hand-made files

Every fixture is built by a single `fixtures-src/build.sh` with **one recipe**:

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16    # the MCU
-fno-pic -mlong-calls                                          # fixed-address, no r10
-O0 -ffunction-sections -fdata-sections
-shared -nostartfiles -Wl,-E -Wl,-z,max-page-size=0x100
-T <one split-segment linker template>                        # sed-filled, not hand-written
```

**No address is hand-picked — not even the windows.** `build.sh` runs three passes:

1. **build at a placeholder base** and measure each module's two `PT_LOAD` sizes
   with `readelf`;
2. **derive the windows** from the chip's RAM geometry (the `MEMORY { RAM … }`
   line and `__Main_Stack_Size`, the only things read from the app script) and the
   measured totals: the modules are packed **top-down, just below the stack**, and
   the derived bounds are written to `fixtures/windows.generated.ld`;
3. **re-link** each module at its packed address.

The app linker script contains **no window addresses** — it just
`INCLUDE`s `windows.generated.ld` (found via `-Lfixtures`). Because the placement
is anchored to the top of RAM it does not depend on firmware size, so no probe
link is needed. Overlap is impossible by construction; an `ASSERT` fails the link
if the firmware's own `.bss`/heap would reach the windows. The per-module `.ld`
files and `windows.generated.ld` under `fixtures/` are **generated artifacts**,
never hand-authored.

**Adding a library** is one line in the `MODULES` table:

```bash
"c1  c  $FSRC/c1/c1.c  -I$FSRC/c1 $FMINC"
```

`build.sh` then also emits `fixtures/manifest.txt` (the load order), which both
`gen-imports` and the firmware consume.

### The fixture set (all 11 fixtures — every export exercised)

Fixture **sources live in this project**, under `fixtures-src/src/` (override with
`FIXTURES_SRC=`), so the build is entirely from-here. Each test's `main.cpp`
exercises **every export of every fixture** — functions, `.data`/`.bss`/`.rodata`
variables, file-scope statics, constructors, struct-by-value, string literals,
function pointers, weak symbols, C++ objects/virtual methods, RTTI, hard-float, and
cross-module heap — and both the RAM and XIP tests run that same body.

| Fixture | Exercises |
|---|---|
| `slib` | plain C, globals, **constructor** (`s_a` 10→15), struct global `s_info`, imports from `main` (`host_f`, `HOST_K`) |
| `mylib1` | exported globals (`aa` 10→20, `bb`), **struct-by-value** return, **string literals** (`retstr`), main-resolved externals (`AA/BB/f/w`), **weak** symbols (`foo/boo`), mutating call |
| `mylib2` | the two-library load path, its own globals (`aaa/bbb/xx`, `str2`), **weak** `mm` across the main/lib boundary |
| `libprov` / `libcons` | **cross-module** call (`libcons` → `libprov`) |
| `libcppsh` | **C++ class** behind a C ABI, instance counting, heap from `main` |
| `libcppeh` | **C++ exceptions + RTTI** (`dynamic_cast`), runtime resolved from `main` |
| `ehx_catch` | **cross-library** throw→catch wiring (non-throwing path exercised) |
| `cpp3` | C++ + **hard-float** math (`sinf/cosf/logf/sqrtf`) borrowed from `main`, recursion |
| `c1` / `c2` | the **cyclic** cluster `c1↔c2↔cpp3`, cross-module, globals/bss, float |

For each fixture `main` reads its **constructor-initialised state and globals**,
then makes **direct by-name calls** — the "call, construction init, variable
usage" surface, end to end.

### Two manifests: load everything, link most by name

`build.sh` emits two lists:

- **`manifest.txt`** — every module, in load order. The firmware loads all of them.
- **`link.txt`** — the subset linked into `main` with `-l:` for by-name calls.

`libcppeh` and `ehx_catch` are in `manifest.txt` but **not** `link.txt`: linking an
EH/RTTI DSO into `main` under `-Wl,-E` drags in libgcc's *hidden*
`__aeabi_unwind_cpp_pr*`, which the link rejects. They are still loaded like every
other module and called through a **typed function pointer** from
`xip::nopic::resolve_any()` — a plain direct call, just not a linked name. This is
the one documented exception to pure by-name calling, and it is data-driven (the
`NOLINK` set in `build.sh`), not a code change.

---

## 5. `gen-imports` — what it's for, and how to do without it

### 5.1 Why it exists

`gen-imports` mechanizes the two pieces that would otherwise be **hand-maintained
lists that drift out of sync with the built `.so`s**:

1. **`modules.generated.inc`** — the ordered module basenames the app loads in a
   loop. Because this is generated from the fixtures actually built, *adding a
   library is one line in `build.sh` and never touches `main`*: the loop picks up
   the new name automatically.
2. **`imports.generated.h`** — one `extern "C"` declaration per exported symbol
   (functions as `void name(void)` placeholders, globals as `unsigned char name`),
   for the **zero-header** case: a library that ships no API header can still be
   called by name (`name` + a cast to the real prototype at the call site).

Why placeholders and not real prototypes? Argument/return types are **not** in an
ELF symbol table — only the library's own header carries them. `gen-imports` gives
you the *name* (a real, linkable symbol whose GOT slot `patch_main` fills); you
supply the type.

### 5.2 Why a C++ version (`gen-imports.cpp`)

There are two interchangeable implementations that emit **byte-identical** files:

- `tools/gen-imports.py <out_h> <out_inc> <readelf> <lib...>` — shells out to
  `readelf`. Needs Python **and** a `readelf` on the host.
- `tools/gen-imports.cpp` — a host C++ app that **parses the ELF32
  `.dynsym`/`.dynstr` itself**, so it depends on **neither Python nor `readelf`**:
  `gen-imports <out_h> <out_inc> <lib...>`.

The C++ tool exists so the project is **self-contained at build time** — the only
host requirement is the C++ compiler you already have. **CMake builds it** at
configure time (a *host* binary, like `common-elf-reader`'s `elf2json`; the ARM
toolchain here is only for the firmware) and runs it; `tools/build.sh` builds it
standalone, and `cmake --build build --target gen-imports` rebuilds it on demand.
CMake falls back to the Python twin only if no host C++ compiler is found.

### 5.3 The escape hatch — how to NOT depend on the tool

Nothing forces you to use `gen-imports`; both generated files have a manual
equivalent, and `main` here already takes the manual route for declarations:

| Generated file | Manual escape |
|---|---|
| `imports.generated.h` | **Declare the exports yourself** with their real prototypes (what `test-*/src/main.cpp` does — that block *is* the fixtures' API), or `#include` the library's own header. Then you never include the generated header. |
| `modules.generated.inc` | **Write the module list by hand** — a small `const char* kModules[] = { "slib.so", ... };` in `main`. You then don't run `gen-imports` at all. |

So `gen-imports` is a convenience that keeps the load list and the zero-header
declarations in lock-step with the built fixtures; drop in the two hand-written
lists above and the tool — and its Python/`readelf` needs — disappears entirely.
It runs at CMake configure time, so re-running is automatic when you reconfigure
after adding a library.

---

## 6. Build characteristics

| Aspect | Value |
|---|---|
| Target | Cortex-M4, ARM32 Thumb, hard-float (`fpv4-sp-d16`) |
| Boards | `weact` (F411CE), `nucleo` (F411RE), `qemu` (netduinoplus2) |
| Module ABI | `-fno-pic -mlong-calls`, ELF32 `ET_DYN`, split R-X \| R-W segments |
| Module code | fixed absolute addresses, **no GOT / PLT / r10** in the module |
| App link | `-pie -Wl,-E -Wl,-Bsymbolic -Wl,--unresolved-symbols=ignore-in-shared-libs`, `-l:*.so` |
| Relocation applied at load | `R_ARM_ABS32` (imports `S`, internals pre-baked) + `.init_array` |
| Placement | 0-bias copy into reserved SRAM windows (RAM); flat-fs image in flash (XIP) |
| Loader footprint | `nopic.cpp` + `image-memory.cpp` + `link-context.cpp` + `scl::common` + ELF reader, all compiled from source into the firmware |
| SCL dependency | consumed as CMake `INTERFACE` (`scl::loader-nopic`, `scl::xip-common`), no `.a` |
| ELF reading | `common-elf-reader::elf-reader`, INTERFACE, transitive |
| Verified | QEMU `netduinoplus2` — `== QEMU TEST: PASS` |

### RAM vs XIP

The **calling convention and the loader code are identical** in both modes; what
differs is where the module image lives before it is placed:

- **RAM**: the image is copied into the SRAM windows and executed there.
- **XIP**: the image is provisioned into on-chip flash (the flat-fs archive); the
  loader streams the two segments into the SRAM windows at boot. Because the
  no-pic model bakes absolute addresses, a module cannot execute *directly* from
  arbitrary flash without being copied to its linked address — so the "flash" role
  here is **persistent storage** of the `.so`, and execution is from the fixed RAM
  window. (For execute-*directly*-from-flash with the code never copied, the SCL
  repo also provides the PIC `scl::xip-static` backend, which keeps R-X in flash
  and uses an r10 data base; it trades the no-r10 simplicity for true in-place
  execution. `optimal-shared` chooses the no-r10 model because it yields the
  cleanest by-name `main`.)

---

## 7. Running

```sh
./build-and-run.sh xip qemu     # XIP test under QEMU (fixtures → gen-imports → build → run)
./build-and-run.sh ram qemu     # RAM test under QEMU
./build-and-run.sh xip weact    # flash + run on WeAct F411CE (OpenOCD, semihosting console)
./build-and-run.sh xip nucleo   # flash + run on NUCLEO-F411RE
```

Expected tail:

```
optimal-shared: load-in-a-loop, call BY NAME (no r10, no dlsym)
[boot] flat-fs files = 7
[boot] 7 modules loaded, main GOT patched
-- slib --   s_add(10,5)=15  s_scale(3)=6  s_via_helper(5)=25  s_apply(3)=13
-- libprov / libcons --   prov_value(5)=6  cons_call(5)=7
-- libcppsh --   cppsh_make(7)=0x200053f8  cppsh_compute(w,5)=92  instances=1001
-- cpp3 / c2 / c1 --   ... cross-module, cyclic, hard-float ...
optimal-shared: done
== QEMU TEST: PASS
```

---

## 8. Takeaway

`optimal-shared` is the shortest path from "a `.so` on the host" to
"`s_add(10, 5)` in `main`" on bare metal: one generic fixture recipe with
auto-packed addresses, an auto-generated load list, the SCL loader pulled in as an
INTERFACE source library, and a `main` that loads everything in a loop and then
calls libraries by name like any linked code — no `dlopen`, no r10, no macros, no
hand-made per-library files.
