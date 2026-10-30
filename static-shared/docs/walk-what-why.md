# Walk Through: What, Why, How — Build Characteristics of the Static Loader Tests

A detailed walkthrough of every test suite in the static loader family of the SCL repository: what it tests, how it's compiled, what linker scripts it uses, and **why** each choice was made.

---

## 1. The Toolchain

### 1.1 Static loader tests (`test-static/*/cmake/arm-none-eabi.cmake`)

```
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft
```

**Why:**
- `-mfloat-abi=soft` — The static loader's `.so` libraries use soft-float ABI. This is because the loader must work with any library regardless of whether it was compiled with hard or soft float. Soft-float is the lowest common denominator.
- No `-fpic` — The static loader patches GOT entries directly, so the main firmware doesn't need PIC.
- No `-ffreestanding` — These tests use `--specs=nano.specs` (newlib-nano), which provides a hosted-like environment with `printf`, `malloc`, etc.

---

## 2. test-static — Static ELF Patcher/Loader

### 2.1 test-static/test-static

**What:** The original static shared-library loader. Loads PIC `.so` files at runtime, resolves symbols, patches GOT entries, runs constructors.

**SCL target:** `scl::static`

**Compile flags:**
```
-O0 -g3 -Wall -Wextra -std=c++20
-fno-rtti -fno-exceptions
-DDEBUG -DUSE_FULL_ASSERT -DOS_USE_SEMIHOSTING -DTRACE
-DSTM32F411xE -DUSE_HAL_DRIVER -DHSE_VALUE=25000000
```

**Why:**
- `-O0 -g3` — Debug build, no optimization, full debug info for stepping through loader logic
- `-fno-rtti -fno-exceptions` — Saves ~20 KB of FLASH; the loader doesn't use `dynamic_cast` or `throw`
- `OS_USE_SEMIHOSTING` — Output goes through BKPT 0xAB to OpenOCD console (no UART needed)
- `HSE_VALUE=25000000` — WeAct BlackPill has 25 MHz crystal (not 8 MHz like Nucleo)

**Linker scripts:** `mem-sh.ld` + `libs-sh.ld` + `sections-sh.ld`

**Link flags:**
```
-nostartfiles --specs=nano.specs -u _printf_float
-Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments
-lsh1 -lsh2 -lstdc++
```

**Why:**
- `-nostartfiles` — The platform provides its own `Reset_Handler` and `_start` (newlib's would conflict)
- `--specs=nano.specs` — Link against newlib-nano (small `printf`, `malloc`)
- `-u _printf_float` — Force linking floating-point printf support (otherwise stripped by nano)
- `-Wl,--no-dynamic-linker` — Don't emit `PT_DYNAMIC` — this is a static executable
- `-Wl,--no-warn-rwx-segments` — Suppress the "RWX segment" warning (the loader intentionally creates RWX memory regions)

**Shared library build flags:**
```
-mcpu=cortex-m4 -mthumb -mfloat-abi=soft
-fPIC -fno-inline-functions -fno-common -O0 -g3
-shared -nostartfiles
```

**Why:**
- `-mfloat-abi=soft` — The `.so` files must be ABI-compatible with the soft-float main firmware
- `-fPIC` — Position-independent: all code/data addresses are PC-relative or go through the GOT
- `-shared -nostartfiles` — Produce a shared object, not an executable
- `-fno-inline-functions` — Prevent GCC from inlining across translation units (would break PIC assumptions)

**.so linker scripts:** `mem.ld` (RAM at 0x00000000, 35K) + `sections2.ld`

**Why RAM at address 0:**
- The `.so` files are linked at address 0 (a placeholder). At runtime, the loader copies them to a real RAM address and patches all relocations. The address 0 is never actually used — it's just a convention that "this library can be loaded anywhere."

### 2.2 test-static/test-static-cpp

**What:** Same as test-static but with a C++ shared library (`libcpp_features.so`).

**Extra build flags for the .so:**
```
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
-shared -nostartfiles -nostdlib
```

**Why:**
- `-nostdlib` — The `.so` doesn't link against libstdc++ or libc. It resolves `operator new`, `printf`, etc. from the main firmware at load time via GOT patching.
- `-fno-use-cxa-atexit` — Don't register destructors via `__cxa_atexit` (no libc available in the `.so`)

### 2.3 test-static/test-static-direct

**What:** Like test-static but the `.so` libraries are linked directly at build time (not loaded at runtime). Used to test that the same libraries work both ways.

**Extra link flags:**
```
-L${APP_DIR}/new-lib -lcpp_features
```

### 2.4 test-static/test-drv

**What:** Driver-style test — loads multiple small `.so` files (libspi.so, libuart.so, libled.so) as if they were hardware drivers.

**Different toolchain flags:**
```
-mfloat-abi=soft   (not hard)
```

**Why:** The driver `.so` files are compiled with soft-float to match the widest ABI compatibility. Some libraries might be compiled without FPU support.

---

## 3. test-xip — Execute-In-Place

### 3.1 The XIP Concept

XIP leaves the code segment in flash (executed in place) and copies only the data segment to RAM. This is a **two-base model**:
- `text_base` = flash address (code runs directly from flash)
- `data_base` = RAM address (GOT, .data, .bss live here)

### 3.2 Linker Script: `xip-split.ld`

```ld
PHDRS {
  text PT_LOAD FLAGS(5);   /* R-X */
  data PT_LOAD FLAGS(6);   /* RW- */
}
```

**Why PHDRS:**
- Explicitly assigns sections to segments with specific permissions
- `.text`, `.rodata`, `.hash`, `.dynsym`, `.dynstr`, `.rel.dyn` → `:text` (R-X, stays in flash)
- `.data`, `.got`, `.dynamic`, `.bss` → `:data` (RW, copied to RAM)

### 3.3 Linker Script: `sections-flatfs-hosted.ld`

**Memory layout:**
```
FLASH    (rx)  : 0x08000000, 256K    ← firmware code
FLASH_FS (r)   : 0x08040000, 256K    ← filesystem with .so files
RAM      (rwx) : 0x20000000, 128K
```

**Key feature:** `.image_arena` in RAM (32K or 64K) — this is where loaded `.so` files' data segments are copied.

**ASSERT:**
```ld
_sidata + (_edata - _sdata) <= _fs_base
```
**Why:** Ensures the firmware's initialized data load image doesn't overflow into the filesystem region. If this fails, the firmware would corrupt the `.so` files stored in flash.

### 3.4 XIP Test Variants

| Test | Loader | What |
|------|--------|------|
| test-xip-static | `scl::xip-static` | Static loader + XIP: loads .so data segments, leaves code in flash |
| test-xip-static-direct | `scl::xip-common` | Same but libraries are linked at build time |
| test-xip-static-no-pic | `scl::xip-nopic` | `-fno-pic` fixed-address XIP, no r10 (resolve + pointers) |
| test-xip-static-no-pic-simple | `scl::xip-nopic` | Same loader, main calls **by name** (`patch_main`) |
| test-xip-interrupt-static | `scl::xip-common` | XIP interrupt with static loader |
| test-xip-interrupt-static-direct | `scl::xip-common` | XIP interrupt with direct linking |

**Common compile flags (all XIP tests):**
```
-fno-pic -fhosted
CXX: -include abort_fix.h
```

**Why `-fno-pic`:**
- The main firmware is NOT PIC — it runs at its link address (0x08000000). Only the loaded `.so` files are PIC.

**Common link flags:**
```
-pie -nostartfiles --specs=nano.specs
-Bsymbolic -E
-T sections-flatfs-hosted.ld
--gc-sections
```

**Why these:**
- `-pie` — Produce a position-independent executable (the loader needs to patch the main firmware's GOT too)
- `-Bsymbolic` — Bind references to the main firmware's own symbols directly (don't go through the GOT for self-references)
- `-E` — Export all symbols (so loaded `.so` files can resolve against main)
- `--specs=nano.specs` — Use newlib-nano for small printf/malloc

---

## 4. Linker Script Patterns Explained

### 4.1 `.inits` section with `_reloc` table

Found in `sections-sh.ld` and hosted `sections-flatfs-hosted.ld`:

```ld
.inits : ALIGN(4) {
    _reloc = .;
    LONG(LOADADDR(.rel.dyn)); LONG(SIZEOF(.rel.dyn));
    LONG(LOADADDR(.dynsym)); LONG(SIZEOF(.dynsym));
    LONG(LOADADDR(.dynstr)); LONG(SIZEOF(.dynstr));
    LONG(LOADADDR(.hash));   LONG(SIZEOF(.hash));
    ...
} >FLASH
```

**Why:** The static loader reads this table at runtime to find the relocation sections without parsing the ELF file. It's a "cheat sheet" embedded in the firmware binary. The loader's `link_context::from_self()` reads `_DYNAMIC` at its RAM VMA.

### 4.2 `.image_arena` (NOLOAD)

```ld
.image_arena (NOLOAD) : ALIGN(8) {
    _simage_arena_start = .;
    . = . + _simage_arena_size;
    _simage_arena_end = .;
} >RAM
```

**Why NOLOAD:** This region is reserved for loaded `.so` files at runtime. It's not initialized from FLASH — the loader fills it. `NOLOAD` prevents the linker from creating a load image for it.

---

## 5. Summary Table

| Test Suite | Float ABI | PIC | libc | SCL Target | Linker Script | Key Feature |
|-----------|-----------|-----|------|-----------|---------------|-------------|
| test-static | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Runtime GOT patching |
| test-static-cpp | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | C++ in .so files |
| test-static-direct | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Direct linking |
| test-drv | soft | -fPIC (.so) | nano.specs | scl::static | mem-sh + sections-sh | Driver-style .so |
| test-xip-* | soft/hard | -fno-pic (main) | nano.specs | scl::xip-* | sections-flatfs-hosted.ld | Execute-in-place |
| common-elf-reader/test | host | N/A | host libc | N/A (test only) | N/A | Host-side parser test |
| common-elf-reader/test-arm | hard | none | none (semihosting) | N/A (test only) | weact sections.ld | Bare-metal parser test |

---

## 6. Key Design Decisions

### Why soft-float for the static loader?

The **static loader** (`test-static`) uses `-mfloat-abi=soft` because:
1. The `.so` files are compiled separately and must be ABI-compatible with the main firmware
2. Using soft-float ensures any library works regardless of whether it uses FPU
3. The static loader doesn't need FPU performance — it's a patcher, not a compute workload

### Why `-fno-pic` for XIP?

**XIP** (`-fno-pic` for main): The main firmware runs at a fixed flash address. Only the loaded `.so` files are PIC (they have their own GOT). The main firmware's GOT is patched once at boot.

### Why `.so` files linked at address 0?

```ld
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 35K }
```

The `.so` files are linked at address 0 as a convention meaning "relocatable, no preferred address." At runtime, the loader places them at the actual allocated address and applies relocations. The address 0 is never used — it's a placeholder that makes it clear the library doesn't assume a fixed load address.

### Why `_reloc` table in `.inits`?

The static loader needs to find `.rel.dyn`, `.dynsym`, `.dynstr`, `.hash` in the firmware binary. Instead of parsing the ELF file (which would require the ELF parser to run on the firmware itself), the linker script embeds a simple table of `{load_address, size}` pairs. The loader reads this table directly — one memory read, no parsing.
