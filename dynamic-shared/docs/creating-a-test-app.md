# Creating a new test app in dynamic-shared

A plain-English recipe for adding a new firmware test app that loads shared
libraries through the dynamic loader. You write **one app folder** (`src/`,
`include/`, `CMakeLists.txt`) and the SCL — plus the vendored `common-elf-reader`
parser — is pulled in automatically; you then add one line to the runner script.

> The pattern shown is exactly what `test-dynamic/test1` (the POSIX battery) and
> `test-xip/test-xip-dynamic-{nolib,newlib}/test1` (the XIP apps) follow.

---

## 1. The app folder

Create a folder under a test tree, e.g. `test-dynamic/test2/`:

```
test-dynamic/test2/
├── CMakeLists.txt        # the whole recipe (below)
├── src/
│   └── main.cpp          # your firmware logic
└── include/              # app-local headers (optional)
    └── my-helpers.hpp
```

`src/main.cpp` uses **only the POSIX dynamic API** (see `test-dynamic/test1`):

```c
void* h = dlopen(SCL_SO_DIR "/mylib1.so", 0);        // load
auto fn = (unsigned (*)(unsigned))dlsym(h, "library_function"); // resolve
fn(42);                                                // call
dlclose(h);                                             // unload
dlerror();                                              // last error, if any
```

These four names are one-line wrappers over the `dl::` facade
(`test/platforms/board-hosted/vendor/firmware/src/dl-impl.cpp`).

---

## 2. `CMakeLists.txt` — the recipe, explained

```cmake
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)                 # standalone configure of just this app
    project(my-app LANGUAGES C CXX)
    set(SCL_HOSTED ON CACHE BOOL "" FORCE)  # hosted loader needs the hosted parts
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)       # brings the whole SCL library in
    enable_testing()
endif()

set(SCL_PLATFORM "board" CACHE STRING "board | qemu-f411re")   # board switch
include(${CMAKE_CURRENT_SOURCE_DIR}/../../test/platforms/${SCL_PLATFORM}-hosted/cmake/platform-library.cmake)

add_executable(my-app src/main.cpp
    ${STATIC_UNIFY_DIR}/firmware/src/dl-impl.cpp)   # the POSIX dl:: facade
target_link_libraries(my-app PRIVATE
    platform::${SCL_PLATFORM}-hosted                 # CMSIS/HAL/newlib platform
    scl::dynamic-newlib)                             # the loader (see below)
```

### What each piece does

- **`add_subdirectory(${SCL_ROOT} scl)`** adds the SCL **library package**:
  `scl::common`, `scl::dynamic-nolib`, `scl::dynamic-newlib` and, transitively,
  **`common-elf-reader`** (the ELF32 parser). Because `scl-common` links
  `common-elf-reader::elf-reader`, the parser's `src/elf-image.cpp` and its
  `loader/` headers compile into **your** app automatically — you never add the
  submodule to the app yourself.
- **`platform::${SCL_PLATFORM}-hosted`** is the hosted platform (newlib,
  CMSIS/HAL, semihosting, trace). `SCL_PLATFORM=qemu-f411re` selects the QEMU
  variant.
- **`scl::dynamic-newlib`** pulls in the hosted loader sources (dynamic-linker,
  patch-journal, image-memory, link-context, + the shared core). For a
  freestanding `dlopen_source` app you would link `scl::dynamic-nolib`
  instead.
- **`SCL_HOSTED=ON`** enables the `std::vector`-based hosted parts (the
  `relocator`, `elf_image::collect_relocations`) that `scl::dynamic-newlib`
  needs.

### Finishing touches (mirror `test-dynamic/test1/CMakeLists.txt`)

```cmake
# fixtures directory the app dlopen()s from (read over semihosting)
target_compile_definitions(my-app PRIVATE
    SCL_SO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
    FIRMWARE_ELF_PATH="$<TARGET_FILE:my-app>")

# -u anchors for the libc/libm/libsupc++ symbols the fixtures borrow
target_link_options(my-app PRIVATE LINKER:-u,printf ...)

scl_embed_symtab(my-app)                       # embedded main symbol table
# .hex/.bin conversion + a qemu/flash test registration
```

---

## 3. Fixtures — building and usage

Fixtures are the small `.so` libraries your app loads. They are **rebuilt by a
shell script** with the exact flags the loader expects:

- plain `-fPIC -shared -Wl,-E` for the hosted loader
  (`test-dynamic/fixtures-src/build.sh` → `test-dynamic/fixtures/`),
- r10 single-pic-base **split R-X|RW** for XIP
  (`test-xip/fixtures-src/build-fixtures.sh` → `test-xip/fixtures/`).

To add a fixture: drop a `.c` (or `.cpp`) into `test/fixtures-src/<name>/`, add
one `build_c` line in the build script, and the app loads it by
`dlopen(SCL_SO_DIR "/<name>.so", 0)`. No firmware relink — the `.so` is read over
semihosting at run time.

If your fixture imports symbols from `main` (`printf`, `AA`, `trace_event`, …),
define/export them in `main.cpp` and keep them alive with `-u` anchors (the
reference `test1/main.cpp` shows the full set).

---

## 4. Integrating into the run flow

Add one line to `test-dynamic/build-and-run.sh` (or `test-xip/build-and-run.sh`):

```bash
# target -> "cmake target : app subdirectory"
[my-test]="my-app:test-dynamic/test2"
```

The script already: builds the fixtures → configures with the right toolchain →
`cmake --build` → flashes (OpenOCD) or runs under QEMU (netduinoplus2) with the
semihosting console. Run with:

```sh
./test-dynamic/build-and-run.sh <weact|nucleo|qemu> my-test
```

The app should end with a marker line (e.g. `RESULT: done`) so the runner knows
it finished.

---

## 5. Summary

1. Make `test-dynamic/test2/{CMakeLists.txt,src/main.cpp,include/}`.
2. Write plain `dlopen`/`dlsym`/`dlclose` code.
3. Link `platform::*-hosted` + `scl::dynamic-newlib` (or `-nolib`).
4. `add_subdirectory(${SCL_ROOT} scl)` gives you SCL + `common-elf-reader` for
   free.
5. Add/build fixtures, point `SCL_SO_DIR` at them.
6. Add one target line to `build-and-run.sh`.
