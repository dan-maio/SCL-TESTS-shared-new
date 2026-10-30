# Build & Link Guide (static family) — linker scripts, C/C++ compile options, and link flags

A cross-project reference for the three sibling repositories:

| Repo | Role | Produces |
|---|---|---|
| **shared-static-rework** | STM32F411CE firmware + in-house ELF32 shared-library loader | `f411ce-sh.elf`, `libsh1/2.so` |
| **cortexm-shared-loader** (SCL) | reusable Cortex-M loader library + on-target test apps | `.so` fixtures |
| **common-elf-reader** | standalone ELF32 reader/parser shared by both loaders | header/source library only |

> **Note (shared-lib-loaders branch):** these three projects now live together in this repo — the SCL is the root, `common-elf-reader` is a git submodule at `./common-elf-reader`, and the former standalone `shared-static-rework` is imported as the `scl::static` module (`src/static` + `include/static`) with its demo app under `test-static/test-static/`. The per-repo framing below is kept for the *why*; paths are relative to this repo.

It explains **why** each linker script, compiler option and link flag is present —
not just what it does — because on a bare-metal Cortex-M target with **no OS, no
MMU, and a hand-written dynamic loader**, most of these choices are load-bearing:
change one and the image either fails to link or HardFaults at boot.

All three build with the same cross toolchain:

```
arm-none-eabi-gcc / g++  (xPack GNU Arm Embedded 15.2.1)
```

---

## 0. Why linker scripts at all?

On a hosted system the OS loader maps segments, resolves the dynamic table, and
sets up the stack; the default linker script is invisible. Here there is none of
that. The linker script is the **only** thing that decides:

- where code and constants live (**FLASH**, `0x08000000`) vs where mutable state
  lives (**RAM**, `0x20000000`);
- which data must be **copied FLASH→RAM at boot** (initialised `.data`, the GOT)
  and which is just **zeroed** (`.bss`);
- where the startup code can **find the metadata** it needs to relocate itself
  and to load `.so` files at runtime (the `_reloc` table, `.dynsym`, `.hash`).

Because these firmwares implement their **own** dynamic linker, the scripts also
have to keep sections that a normal bare-metal image would discard — `.dynsym`,
`.dynstr`, `.hash`, `.rel.dyn`, `.got` — and place the writable ones (`.got`) in
RAM so the loader can patch them.

Two distinct runtime models appear across the repos:

1. **`_reloc`-table model** (shared-static-rework, and SCL's *hosted* platform):
   the linker emits a small fixed table pointing at `.dynsym/.dynstr/.hash/
   .rel.dyn`; startup reads it to self-relocate and to serve symbol lookups.
2. **`_DYNAMIC` model** (SCL's *freestanding* platform): the image is a real
   `-pie` object and the firmware parses its own `_DYNAMIC` array at boot, the
   way a real ELF interpreter would.

---

## 1. shared-static-rework

### 1.1 Memory map — `ldscripts/mem-sh.ld`

Split into three scripts (`-T mem-sh.ld -T libs-sh.ld -T sections-sh.ld`) so the
**board memory map**, the **extra libraries list**, and the **section placement**
can vary independently (the µOS++ convention).

```
RAM   (rw) : ORIGIN = 0x20000000, LENGTH = 128K
FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
```

`libs-sh.ld` is an empty `GROUP()` placeholder — a documented seam for adding
`-l` archives without touching the section script.

### 1.2 Section placement — `ldscripts/sections-sh.ld`

The important, non-boilerplate parts:

**The `_reloc` table (in `.inits`, → FLASH).** This is the linchpin of the whole
loader. The linker computes and emits, as raw `LONG()` words, the load address
and size of every section the runtime linker needs:

```ld
_reloc = .;
LONG(LOADADDR(.rel.dyn));  LONG(SIZEOF(.rel.dyn));
LONG(LOADADDR(.dynsym));   LONG(SIZEOF(.dynsym));
LONG(LOADADDR(.dynstr));   LONG(SIZEOF(.dynstr));
LONG(LOADADDR(.hash));     LONG(SIZEOF(.hash));
LONG(LOADADDR(.inits));    LONG(SIZEOF(.inits));
```

*Why:* the firmware has no ELF program headers to walk at runtime, so the linker
bakes the "where is my dynamic table" answer into a constant the C code reads
(`_reloc` in `shlib-loader`/`elf-reader`). This is what lets main resolve symbols
that loaded `.so` files import from it.

**`.dynsym / .dynstr / .hash / .rel.dyn` kept in FLASH.** A normal bare-metal
link would strip these. They are explicitly placed so the loader can do SysV-hash
symbol lookup against main.

**`.got` and `.got.plt` in RAM, `AT>FLASH`.** The GOT is *writable* — the loader
patches it when a `.so` is loaded — so its run address (VMA) is in RAM, while its
initial values are stored in FLASH (LMA) and copied at startup:

```ld
.got : { *(.got*) } >RAM AT>FLASH        /* _sgotdata = LOADADDR(.got) */
.got.plt : { *(.got.plt*) } >RAM AT>FLASH
```

*Why:* code in FLASH is read-only; a GOT that must be rewritten at runtime cannot
live there. The `_sgot*`/`_esgot*` symbols let startup copy and the loader bound
its writes.

**`.data …>RAM AT>FLASH`, `.bss (NOLOAD) …>RAM`.** Standard split: initialised
data has a FLASH master copy (`_sidata = LOADADDR(.data)`) copied to RAM at boot;
`.bss` is `NOLOAD` (occupies no file bytes) and is zeroed by startup.

### 1.3 The shared libraries — `libsh1/2/ldscripts/{gcc,mem,sections2}.ld`

`gcc.ld` just `INCLUDE`s `mem.ld` + `sections2.ld`. The library memory map is:

```
RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35K
```

*Why origin 0:* the `.so` is **position-independent** and gets copied to an
arbitrary RAM arena at load time; linking it at 0 makes every address a pure
offset the loader adds `base` to. `rwx` because on an MMU-less part code + GOT +
data necessarily share one region. `sections2.ld` additionally splits `.got` /
`.got.plt` out with `_lsgot*` markers and gives `.rodata` its own block, so the
loader can find and patch the library's GOT.

### 1.4 C compile options (`LIB_CFLAGS`, `APP_CFLAGS`)

Shared CPU flags: `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft` — Cortex-M4, Thumb-2
instruction set, **soft float** (this firmware does not use the FPU ABI; contrast
SCL's freestanding platform which is hard-float).

| Flag | Where | Why |
|---|---|---|
| `-fPIC` | libs | position-independent code — mandatory, the `.so` is relocated to an arbitrary base. |
| `-fno-common` | libs | put tentative definitions in `.bss` (real symbols), not the COMMON pseudo-section — the loader needs them as normal symbols. |
| `-fno-inline-functions` | libs | keep exported functions as real, callable symbols for the cross-module demo. |
| `-ffunction-sections -fdata-sections` | app | one section per function/datum so `--gc-sections` can drop the unused. |
| `-fno-move-loop-invariants` | app | µOS++-verified codegen setting for the startup copy loops. |
| `-O0 -g3` | both | unoptimised + full debug: the loader walkthroughs (GOT dumps) rely on predictable symbols/layout. |
| `-DSTM32F411xE -DUSE_HAL_DRIVER -DHSE_VALUE=8000000` | app | ST HAL configuration. |
| `-DOS_USE_SEMIHOSTING -DTRACE -DOS_USE_TRACE_SEMIHOSTING_DEBUG` | app | route `printf`/trace to the semihosting console. |

### 1.5 C++ compile options (`APP_CXXFLAGS`)

`APP_CFLAGS` **plus**:

| Flag | Why |
|---|---|
| `-std=c++20` | the parser/loader use `std::span`, `std::optional`, `std::string_view`, concepts-free `result<T>`. |
| `-fno-rtti` | no `typeid`/`dynamic_cast` → no RTTI tables, smaller image. |
| `-fno-exceptions` | no unwind tables/`__cxa_throw` in *this* firmware (its demo doesn't throw); saves flash and avoids pulling the EH runtime. |

*(Contrast: SCL's freestanding apps keep exceptions/RTTI **on**, because that
firmware deliberately runs C++ exceptions across the load boundary.)*

### 1.6 Link options (`APP_LDFLAGS`)

```
-T ldscripts/mem-sh.ld -T ldscripts/libs-sh.ld -T ldscripts/sections-sh.ld
-nostartfiles -Wl,--gc-sections --specs=nano.specs -u _printf_float
-Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
-Llibsh1 -Llibsh2 -lsh1 -lsh2 -lstdc++
```

| Flag | Why |
|---|---|
| `-T … ×3` | supply the custom scripts; order matters (mem → libs → sections). |
| `-nostartfiles` | use the project's own `startup.c`/vectors, not the toolchain crt0. |
| `-Wl,--gc-sections` | drop unreferenced sections (pairs with `-ffunction/-fdata-sections`). |
| `--specs=nano.specs` | link newlib-nano (small `printf`, no wide/locale bloat). |
| `-u _printf_float` | force-pull the float `printf` variant nano would otherwise omit. |
| `-Wl,--no-dynamic-linker` | this is a static executable image; do not emit a `PT_INTERP`/dynamic-linker request. |
| `-Wl,--no-warn-rwx-segments` | silence the (expected) RWX-segment warning — MMU-less, code+data share a region by design. |
| `-lsh1 -lsh2` | link the two demo libraries for their import surface. |
| `-lstdc++` | the C++ support library for the loader code. |

### 1.7 CMake mirror

`CMakeLists.txt` reproduces the Makefile exactly. Two Cortex-M-specific tricks:

- **`set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)`** (in
  `cmake/arm-none-eabi.cmake`) — a normal executable try-compile would fail (no
  hosted runtime, custom scripts), so CMake probes the compiler by building a
  static lib instead.
- **`"SHELL:-T ${script}"`** link fragments — CMake de-duplicates repeated plain
  `-T` flags and would feed a script as a bare input file ("did you forget
  `-T`?"). The `SHELL:` prefix keeps each `-T <script>` pair intact.

---

## 2. cortexm-shared-loader (SCL) — hosted toolchain & `.so` fixtures

The SCL ships its loaders as xpack-style INTERFACE "platform library" targets.
The static family builds against the **hosted** platform library:

| `platform::board-hosted` |
|---|
| Runtime | full CMSIS + ST HAL + newlib-nano |
| Code model | non-PIC, `_reloc` table (like §1.2) |
| Float ABI | soft by default, `-DSCL_HF=ON` for hard |
| Toolchain file | `cmake/arm-none-eabi-hosted.cmake` |

### 2.1 Hosted flags (`cmake/arm-none-eabi-hosted.cmake`)

Non-PIC, newlib-nano, heap available — the environment SCL's `SCL_HOSTED` parts
(the `std::vector` relocator, `scl::dynamic-newlib`) target. Float ABI switchable:

```
SCL_HF=OFF → -mfloat-abi=soft
SCL_HF=ON  → -mfloat-abi=hard -mfpu=fpv4-sp-d16
+ -O0 -ffunction-sections -fdata-sections -fno-inline-functions -g3
C++: + -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics -fno-move-loop-invariants
```

The two ABIs build into **separate trees** (`build-dynamic-newlib`, `build-dynamic-newlib-hf`)
with matching fixture sets (`libs/build` vs `libs/build_hf`); the binaries keep
the same name because the float ABI is a per-configuration global — each ABI
needs its own tree regardless (so renaming would add nothing).

### 2.2 The `.so` fixtures

Fixture libraries are built (per the recipe recorded in their headers):

```
C:   -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
     -fpic -msingle-pic-base -mno-pic-data-is-text-relative
     link: -shared -nostartfiles -Wl,-nmagic -Wl,-E
           -Xlinker --unresolved-symbols=ignore-all -T ldscripts/gcc.ld
C++: -std=c++23 -fPIC -shared -fno-threadsafe-statics -fno-use-cxa-atexit -nostdlib -Os
     (libcppeh/ehx: exceptions + RTTI ON)
```

- `-msingle-pic-base -mno-pic-data-is-text-relative` — the library uses a single
  PIC base held in `r10` (set by the loader's trampoline on entry), and does
  **not** assume data sits at a fixed offset from text (it's relocated).
- `-Wl,-nmagic` — no page alignment/padding; keep the `.so` compact for RAM.
- `-shared -nostartfiles` linked at RAM origin 0 (`gcc.ld` → `mem.ld` = 35K @ 0),
  same PIC rationale as §1.3.

---

## 3. common-elf-reader

**No linker scripts, no firmware, no link step of its own** — it is a pure
source-code library (the reader/parser: `elf_image`, `byte_source`, ELF32 types,
`result<T>`, and the bounded byte accessors in `detail/bytes.hpp`).

- **CMake:** exposed as an **INTERFACE** target
  (`common-elf-reader::elf-reader`). Its one source (`src/elf-image.cpp`) and its
  `include/` propagate to whatever links it, and are **compiled from source into
  each consumer with that consumer's own flags** — never archived to a `.a`.
- **Only compile setting it imposes:** `target_compile_features(... cxx_std_20)`.
- **`SCL_HOSTED`** (INTERFACE compile definition, opt-in) — gates
  `elf_image::collect_relocations()` (which needs `std::vector`); left OFF for
  freestanding firmware, ON for host tools/tests.

Because it is header + one TU compiled *in situ*, it automatically inherits each
project's `-mcpu`/`-mfloat-abi`/`-std`/optimisation, so the same parser is valid
in soft-float shared-static-rework, hard-float freestanding SCL, and plain host
g++ unit tests — with no per-ABI archive to manage.

---

## 4. Cross-cutting comparison (static family)

| Concern | shared-static-rework | SCL hosted | common-elf-reader |
|---|---|---|---|
| Float ABI | soft | soft / `-DSCL_HF` hard | inherits consumer |
| PIC model | app non-PIC + `_reloc` table | non-PIC + `_reloc` | n/a |
| C++ EH | off (`-fno-exceptions -fno-rtti`) | off | inherits consumer |
| Startup | own `startup.c` (`-nostartfiles`) | newlib-nano crt | n/a |
| libc | newlib-nano (`nano.specs`) | newlib-nano | n/a |
| GOT | RAM `AT>FLASH`, loader-patched | RAM, loader-patched | n/a |
| `.so` link | `-shared -nostartfiles`, RAM@0 | same fixtures | n/a |

---

## 5. Flag glossary (quick reference)

| Flag | Meaning |
|---|---|
| `-mcpu=cortex-m4 -mthumb` | Cortex-M4 core, Thumb-2 encoding. |
| `-mfloat-abi=soft` / `hard -mfpu=fpv4-sp-d16` | float args in integer regs / in VFP single-precision regs. |
| `-fPIC` / `-fpic` | position-independent code (large / small GOT model). |
| `-msingle-pic-base -mno-pic-data-is-text-relative` | one `r10` PIC base; data not at a fixed text offset (relocated). |
| `-ffunction-sections -fdata-sections` + `--gc-sections` | one section each, then garbage-collect the unused. |
| `-fno-common` | tentative defs → real `.bss` symbols. |
| `-nostartfiles` / `-nostdlib` | no toolchain crt0 / no default libraries + crt. |
| `--specs=nano.specs` + `-u _printf_float` | newlib-nano, force float `printf`. |
| `--unresolved-symbols=ignore-all` | defer unresolved symbols to the runtime loader. |
| `-Wl,--no-dynamic-linker` | static image, no `PT_INTERP`. |
| `-Wl,--no-warn-rwx-segments` | expected RWX (no MMU); silence the warning. |
| `AT>FLASH` (ld) | section runs at a RAM VMA but is stored at a FLASH LMA (copied at boot). |
| `NOLOAD` (ld) | occupies address space but no file bytes (`.bss`). |
