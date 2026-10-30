# SCL vs Test App — Responsibility Split After Loading & Parsing

The previous document (`TEST-MODULE-LOADING.md`) covered the **reading** side:
how the test app's `byte_source` subclasses deliver bytes. This document covers
what happens **after** those bytes are loaded and parsed: for every loading
case, **who is responsible for what** — the **SCL library** (the loader engine
under `src/`, `include/`, and the `common-elf-reader` parser) versus the
**test application** (the `test-*` trees, the platform layer, and each `main`).

The rule that holds everywhere:

- **SCL** owns the *loader engine*: parsing, image placement, relocation,
  constructor execution, symbol lookup, handle lifetime — it is **storage- and
  board-agnostic**.
- **The test app** owns everything *around* the engine: where the bytes come
  from (the reader), how they were provisioned, the board, the console, the
  fixtures, and the test logic that calls the engine and reports results.

File references are given as `path:line`. All line numbers are for this
workspace.

---

## 1. Case: `scl::static` (standalone static patcher)

**Apps:** `f411ce-sh`, `scl-fw-static-cpp`, `scl-fw-static-direct`, `scl-fw-drv`.

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Parse | `src/static/elf-reader.cpp` (`elf_reader` adapter over `common-elf-reader`) | finds `.dynsym`/`.dynstr`, reads section headers |
| Load + patch **both** GOTs | `src/static/shlib-loader.cpp` (`shlib_loader::load_bin`, `populate_helper_sections`, `patch_shlib`, `patch_main`) | patches the library's GOT **and** main's GOT so direct calls work both ways |
| Constructors | `src/static/module.cpp` `Module::build` (line ~18 path) | runs `.init_array` after patching |
| Symbol lookup | `src/static/module.cpp` `Module::sym` | scans `.dynsym` for a name → returns runtime address |
| Front-end | `src/static/module.cpp:11` `Module::load_file`, `:18` `Module::load_buf` | from a path (semihosting) or a RAM buffer |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Firmware `.dynsym` | `test-static/test-static/CMakeLists.txt` links `libsh1/libsh2.so` with `-l:…` | so the firmware ELF carries `.dynsym`/`.rel.dyn`/`.plt`/`.got` for the loader to patch against |
| Reader | app opens `libsh1.so` over semihosting, or passes a buffer | supplies the bytes |
| Driver fixtures | `test-static/test-drv/CMakeLists.txt` `add_shlib(spi/uart/led)` | builds `libspi/uart/led.so` (WEACT pins) |
| Test logic | `test-static/*/src/main.cpp` | calls `m.sym(...)`, casts to function pointers, verifies results |

**Boundary:** the **SCL** makes a `.so` callable and patches both sides; the
**app** makes sure the `.so` is linked into the firmware image and that the
firmware exports the symbols the libraries import.

---

## 2. Case: XIP loaders (`scl::xip-*`)

**Apps:** `scl-fw-xip-static*`, `scl-fw-xip-interrupt*`, `scl-fw-xip-static-spi-flash`, plus `scl-fw-xip-static-no-pic*` (`scl::xip-nopic` — fixed-address, no r10, by-name via `patch_main`).

### What the SCL does

| Step | Where | What it does |
|---|---|---|
| Classify R-X \| RW | `include/xip/segment-map.hpp` `struct SegmentMap`; `src/xip/segment-map.cpp` | decides which segment stays in flash (R-X) and which is copied to RAM (RW); `split == false` ⇒ copy-whole |
| Relocate for two bases | `src/xip/relocate.cpp` | resolves addends against text_base (flash) vs data_base (RAM) |
| Compact/derive data-base | `src/xip/compact.cpp` | derives the RAM data base / got base |
| Pre-bind host imports | `src/xip/static/adapter.cpp:127` `xip::stat::bind_host_imports` | pre-links function JUMP_SLOTs to the cross-call trampoline |
| Load (text in flash, data in RAM) | `src/static/module.cpp` `Module::load_buf` + `shlib_loader` | patches the RW copy's GOT; leaves the R-X text untouched in flash |
| Cross-library trampoline | `src/xip/xip-cross-call.c` (`xip_cross_call`), `src/xip/static/adapter.cpp` (`xip_cross_lookup`) | r10 swap so a flash PLT stub can call another module |
| Symbol lookup | `Module::sym` | returns either a **flash** (code) or **RAM** (data) address |

### What the test app does

| Step | Where | What it does |
|---|---|---|
| Provisioning pipeline | `test-xip/test-xip-static/spi-flash/src/main.cpp` | semihosting → **SPI** flash flatfs (`fw_spi::fs_add`) → copy to **internal** flash (`fw_int::fs_add`) |
| Internal-flash flatfs | `test-xip/test-xip-static/spi-flash/src/fs-archive-internal.cpp` | `fs_find`/`fs_blob` return the memory-mapped blob |
| Call helpers | `test-xip/*/include/test-helpers.hpp` `call_sym0..3` | resolve + `pic_call` into the module |
| Interrupt vector patch | `test-xip/test-xip-interrupt*/src/main.cpp` | app patches the vector table (SysTick/TIM2) to the loaded handler |
| XIP fixtures | `test-xip/test-xip-static/semihosting/fixtures-src/build.sh`, `test-xip/fixtures-src/build-xip-libs.sh` | build `-msingle-pic-base -mpic-register=r10` split `R-X|RW` `.so` |
| Walkthroughs + reporting | `test-xip/*/src/main.cpp` | execution proofs (flash vs RAM addresses), `PASS/FAIL` |

**Boundary:** the **SCL** decides *what stays in flash and what moves to RAM*
and makes cross-module calls work; the **app** decides *what to provision
where* (semihosting → SPI → internal), patches the interrupt vectors, and runs
the tests.

---

## 3. Linker scripts: firmware and fixtures

Linker scripts are another place where the boundary is strict: **the SCL never
owns a linker script**.

The SCL instead defines a **contract** — a set of symbols, sections and
properties the *application's* scripts must provide so the loaders work. Both
sides of that contract are listed below.

### 3.1 What the SCL requires from the FIRMWARE linker script

The freestanding loaders read a fixed set of symbols/sections out of the
firmware image. The application's `sections.ld` must provide:

| Required by the SCL | Purpose | Provided in (app-owned) |
|---|---|---|
| `_simage_arena_start` / `_simage_arena_end` | boundaries of the 64 KB load arena handed out by `image_arena()` / `region_allocator` | `test/platforms/board/ldscripts/mem.ld:12` (`_simage_arena_size = 64K`) + `sections.ld:84-89` (`.image_arena` `NOLOAD` block) |
| `_estack` | initial SP (loaded from vector[0]) | `mem.ld:9` (`ORIGIN(RAM)+LENGTH(RAM)` = `0x20020000`) |
| `.dynamic` / `_DYNAMIC` at a **RAM VMA** | `link_context::from_self()` parses it to resolve library imports against main | `sections.ld:56` (inside the contiguous `.data/.dynamic/.got/.got.plt` init block copied to RAM) |
| `.got` / `.got.plt` in RAM | the loader relocates these | `sections.ld:61,67` |
| `.rel.dyn` kept | for the lazy/PLT fixup tables | `sections.ld:34` |
| `-Wl,-E` exports | every symbol a loaded library might import must be in `.dynsym` | the app's link flags (`scl-test-app.cmake`) |
| `.ARM.exidx` | C++ EH unwinding across the library boundary | `sections.ld:27` |

For **XIP**, the app's `sections-flatfs-hosted.ld` plus the
`-Wl,--defsym=_fs_base=0x08040000`/`_fs_size` options carve out the internal
flash region the flatfs provisioning programs.

### 3.2 The FIXTURE linker scripts (app-owned, per test)

The `.so` fixtures are built with their own small scripts, all under the test
trees — never under the SCL:

| Fixture set | Script | What it produces |
|---|---|---|
| Driver `.so` (`libspi/uart/led`) | `test-static/test-drv/fixtures/ldscripts/mem.ld` (RAM at `0x00000000`, 128 K) + `sections.ld` | PIC `.so` linked at image base 0 |
| XIP `.so` | `test-xip/fixtures-src/ldscripts/xip-split.ld` | the **split `R-X | RW`** two-`PT_LOAD` layout (`PHDRS`: `text` flags 5, `data` flags 6; `data_vaddr == data_off`) that makes `xip::SegmentMap::split == true` so XIP is possible |

### 3.3 Responsibility split for linker scripts

- **The SCL** provides the *requirements* list (§3.1). It never ships a complete
  firmware or fixture script, and it never knows the memory map or board.
- **The test app / platform** owns and writes every real linker script —
  firmware (`mem.ld`/`sections.ld`) and fixtures (`xip-split.ld`, `mem.ld`, …) —
  and must satisfy the SCL contract: reserve the arena, put `.dynamic`/GOT in
  RAM, export symbols, and (for XIP) produce a split `R-X | RW` image.

If the firmware script forgets the arena or `_DYNAMIC`, `image_arena()` or
`link_context::from_self()` fails at runtime — the loader did nothing wrong;
the script (app side) violated the contract. If the XIP fixture is linked with
a fused `RWX` segment, `SegmentMap::split` reports false and the module is
copy-whole-loaded instead — again the fixture script's responsibility.

---

## 4. What is shared / cross-cutting

Some responsibilities are not "SCL" or "app" alone but a **contract**:

- **The symbol contract.** The SCL resolves imports against the app's exports.
  The app must export them (`-Wl,-E`, `.dynsym`, `-l:libsh.so`). If the app
  exports nothing, every load fails with unresolved symbols — the loader did
  its job; the export list is the app's.
- **The reader contract.** `loader::byte_source` (`common-elf-reader/
  include/loader/byte-source.hpp`) is a pure virtual interface (no work).
  The **library** consumes it; the **app** implements it.
- **The arena contract.** The SCL places images in memory the app reserved
  (caller `buf` for static/XIP).
- **The linker-script contract.** See §3: the SCL lists what the app's scripts
  must provide (`_simage_arena_start/end`, `_DYNAMIC` in RAM, `.got`, exports,
  split `R-X|RW` fixtures); the app writes the scripts.

---

## 5. Summary — the responsibility boundary after loading & parsing

| Concern | Owned by SCL | Owned by the test app |
|---|---|---|
| Parse ELF / ET_DYN | ✔ `elf_image::parse`, `elf_reader` | |
| Image placement (arena slot / provided buffer) | ✔ (uses what the app supplies) | supplies the arena / buf |
| Relocation / GOT / PLT patching | ✔ `reloc_applier`/`shlib_loader` | |
| Running constructors / destructors | ✔ | |
| Symbol lookup / resolution | ✔ `Module::sym` | must **export** the symbols |
| Cross-library routing / EH / lazy resolve | ✔ trampolines + port layer | enables them |
| Getting the bytes (reader) | | ✔ `byte_source` subclasses |
| Choosing the storage / provisioning | | ✔ flatfs, SPI→internal pipeline |
| Board init, clock, LED, console | | ✔ platform + `board.c` |
| Linker scripts (firmware + fixtures) | only requirements | ✔ owns and writes them (`mem.ld`/`sections.ld`; fixture `xip-split.ld`/…) |
| Fixture `.so` building | | ✔ CMake / Makefiles / scripts |
| Test battery, checks, reporting | | ✔ each `main.cpp` |

**Takeaway:** after the bytes are read, the **SCL does all the *loader* work**
— parse, place, relocate, construct, resolve — against a storage/board-neutral
contract; the **test app does all the *situational* work** — where the bytes
came from, what got provisioned, how the board is set up, and what the test
asserts. Neither side can do the other's job without breaking the boundary:
the SCL has no idea what a WEACT/NUCLEO is, and the app never re-implements a
single relocation or constructor walk.
