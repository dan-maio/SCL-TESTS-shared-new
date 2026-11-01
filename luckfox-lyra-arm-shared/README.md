# luckfox-lyra-arm-shared — ARM-state (A32) loader tests

The Luckfox Lyra B is the only board here with a full ARMv7-A core, so it is the
only place where ARM-state (A32) modules can be loaded and run.  The other Lyra
suite, `luckfox-lyra-shared/`, builds every fixture with `-mthumb`; this suite
builds the same fixtures with **`-marm`** and drives them through every SCL
loader.

| Test | Loader | Fixtures | Group |
|---|---|---|---|
| `arm-reloc` | `scl::reloc` | libprov, libcons, mylib2 | 2 |
| `arm-static` | `scl::static` | libprov, libcons, mylib1, mylib2 | 3 |
| `arm-optimal` | `scl::optimal` | libprov, libcons, mylib2 | 2 |
| `arm-dynamic-nolib` | `scl::dynamic-nolib` | libprov, libcons, mylib2 | 2 |
| `arm-dynamic-newlib` | `scl::dynamic-newlib` | libprov, libcons, mylib2 | 2 |
| `arm-xip-static` | `scl::xip-static` | libprov, libcons, mylib2 | 2 |
| `arm-xip-dynamic-nolib` | `scl::xip-dynamic-nolib` | libprov, libcons, mylib2 | 2 |
| `arm-xip-dynamic-newlib` | `scl::xip-dynamic-newlib` | libprov, libcons, mylib2 | 2 |
| `arm-overlay` | `scl::overlay` | ov1, ov2, ovcpp | overlay |

All 9 pass on the Lyra B (2026-11-01) at `SCL-shared` `eb2ae55`, whose
`common-elf-reader` submodule is `elf32_read_parse-new.git` `f650889`.

## The A32 paths this exercises

The fixtures are built `-mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4`.
The ET_REL form is `-O0 -mlong-calls -fno-pic -r`, which emits:

* `R_ARM_CALL` / `R_ARM_JUMP24` for calls;
* `R_ARM_ABS32` / `R_ARM_TARGET1` for pointers and constructors;
* `R_ARM_MOVW_ABS_NC` / `R_ARM_MOVT_ABS` for 32-bit addresses;
* `R_ARM_PREL31` in `.ARM.exidx`.

The `.pic.so` forms exercise `R_ARM_RELATIVE`, `R_ARM_GLOB_DAT`,
`R_ARM_JUMP_SLOT` and the ARM PLT/GOT.  The `.opt.so` form is linked at the
`0x07A00000` / `0x07A80000` windows and uses `R_ARM_ABS32`/`R_ARM_JUMP_SLOT`.

The loader must preserve each symbol's own bit 0: an ARM function has an even
`st_value`, a Thumb function an odd one.  Forcing Thumb (the old
`addr_of() | 1`) breaks the first ARM call.

## Source

* `CMakeLists.txt` — ARM fixture matrix and the 9 test binaries.
* `../luckfox-lyra-shared/src/fixtures-main.cpp` — the existing group 1–6 test
  body, reused with `TEST_GROUP=2` or `3` and `FIXTURE_DIR` pointing at the ARM
  fixtures.
* `../luckfox-lyra-shared/src/overlay-main.cpp` — the overlay test.
* `../luckfox-lyra-shared/src/embed-arena.S` — the loader arena.

## Build and run

The suite uses the `SCL-shared` submodule of `SCL-TESTS-shared-new`, which pins
the loader and its `common-elf-reader` submodule:

```sh
cd luckfox-lyra-arm-shared
cmake -S . -B build
cmake --build build --target arm-tests -j8

BUILD=$PWD/build \
OPENOCD=$PWD/../../SCL-EXAMPLES-shared-new/luckfox-lyra/openocd-linebuf.sh \
  /home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-aarch32/test/boards/luckfox-lyra/hw.sh arm-reloc 180
```

Or run all nine:

```sh
for t in arm-reloc arm-static arm-optimal arm-dynamic-nolib arm-dynamic-newlib \
         arm-xip-static arm-xip-dynamic-nolib arm-xip-dynamic-newlib arm-overlay; do
  BUILD=$PWD/build OPENOCD=... \
    /home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-aarch32/test/boards/luckfox-lyra/hw.sh "$t" 180
done
```

`SCL_DIR` may be overridden (`-DSCL_DIR=/path/to/SCL-shared-new`) for a loader
working tree; the committed default is the `../SCL-shared` submodule.
