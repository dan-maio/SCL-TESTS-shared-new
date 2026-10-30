# optimal-shared — load shared libraries and call them **by name**, no glue

A fourth loader project for the SCL workspace (beside `dynamic-shared`,
`static-shared`, `relocatable-shared`). It is the **simplest complete** way to
load shared libraries at runtime on a bare-metal Cortex-M (ARM32) and use them
from `main`, built to three hard rules:

1. **Direct calls, by name.** `main` calls a library function as
   `s_add(10, 5)` — an ordinary C call. No `dlopen`/`dlsym`, no handle, no
   function-pointer table, no macro. (A cast is used only where you turn a raw
   address into a typed pointer — never for a normal call.)
2. **RAM and XIP.** The same firmware loads libraries whether their code runs
   from RAM or is provisioned in on-chip flash.
3. **No hand-made files, no manual code-gen step.** One generic recipe builds
   every fixture (no per-library linker script, no hand-picked addresses); a host
   tool auto-emits the module load list; adding a library is one line.

It reuses the **SCL loader/patcher** and the **common-elf-reader** as CMake
`INTERFACE` ("source code") libraries — no prebuilt archives — exactly as an
external application would consume them.

```
$ ./build-and-run.sh xip qemu
...
optimal-shared [XIP]: load-in-a-loop, exercise EVERY fixture (no r10, no dlsym)
[boot] 11/11 modules loaded + linked, main GOT patched
-- slib --   ctor: s_a=15  s_add(10,5)=30  s_text()="static-xip slib says hi"
-- mylib1 --  ctor: aa=20  library_function(39)=62  retstr()="Hello mylib1!"  weak foo()=20
-- mylib2 --  ctor: aaa=20  lf(39)=62  xxx()=126  weak mm()=0
-- libprov / libcons --  prov_value(5)=105  cons_call(5)=106
-- libcppsh --  cppsh_make(7)=0x20005600  cppsh_compute(w,5)=92  instances=1001
-- libcppeh / ehx --  eh_dyncast(0)=2 (RTTI dynamic_cast)  ehx_catch_cross(0)=0
-- cpp3 --  cpp3_method(7,5)=1242  cpp3_orchestrate(5)=35190 (hard-float)
-- c2 / c1 --  c2_add(10,20)=8774  c1_call_c2(7)=17479 (cyclic cluster)
optimal-shared: done
== QEMU TEST: PASS
```

## Why this is the "optimal" of the four

| | dynamic (`dlopen`) | static PIC | static no-pic | relocatable | **optimal** |
|---|---|---|---|---|---|
| call style in `main` | `dlsym`+cast | `patch_main` | by name | fn-pointer | **by name, direct** |
| RAM | ✅ | ✅ | ✅ (RAM window) | ✅ | ✅ |
| XIP (code from flash) | ✅ | ✅ | ✅ | ❌ | ✅ |
| per-library linker script | no | no | generated | no | **no** |
| hand-picked addresses | no | no | auto-packed | no | **auto-packed** |
| hand-written load list | yes | yes | yes | yes | **generated** |

`optimal-shared` takes the **by-name, no-r10** calling model of the no-pic static
loader (the only one that reads like ordinary linked code) and removes its last
two manual steps: the import surface and the load list are generated, and the
firmware iterates the libraries in a loop instead of naming them one by one.

## How it works in one picture

```
fixtures-src/build.sh ──► fixtures/*.so   (one recipe, -fno-pic -mlong-calls,
     │                    fixtures/manifest.txt   addresses auto-packed)
     │
tools/gen-imports.py ──► test/common/modules.generated.inc  (the load list app loops over)
     │                   test/common/imports.generated.h    (optional zero-header imports)
     │
CMake: add_subdirectory(scl) + common-elf-reader submodule ─► scl::* (INTERFACE)
     │  link optimal (scl::loader-nopic)  +  -l:*.so   (so main gets a GOT slot per export)
     ▼
main.cpp: provide (RAM buf | flash) → optimal::load_and_link (load+resolve+patch)
      then:  s_add(10, 5);  cons_call(5);  cpp3_method(7, 5);  ...
```

The loader copies each module's read-exec and read-write segments into the
firmware's reserved SRAM windows, applies the one relocation the linker could not
pre-bake (absolute imports from `main` and from other modules), runs the
constructors, then `patch_main()` writes each module's export addresses into
`main`'s own GOT/PLT slots — after which naming the function *is* calling it.

## Self-contained

Everything needed to build is in the repository — nothing outside it is
referenced:

```sh
git clone <this repo> && cd optimal-shared
git submodule update --init          # fetches common-elf-reader
./build-and-run.sh xip qemu
```

Like every other SCL-shared project, **the repository root is the SCL source
library**: `src/` and `include/` hold the loader/patcher code, and
`common-elf-reader/` is a git submodule at the root beside them. No path outside
the repo is referenced.

Prerequisites: `arm-none-eabi-gcc` and, for the qemu target, `qemu-system-arm`.

## Two tests: RAM and XIP

```sh
./build-and-run.sh ram  qemu     # modules loaded from a RAM buffer
./build-and-run.sh xip  qemu     # modules provisioned in on-chip flash (XIP)
./build-and-run.sh xip  weact    # …or on WeAct F411CE / nucleo via OpenOCD
```

Each test has its **own** complete `main.cpp` (`test-ram/src/main.cpp`,
`test-xip/src/main.cpp`) holding the full by-name exercise; they differ only in
where each module's bytes come from — a RAM buffer or the flash flat-fs. That one
`provide()` function is the entire RAM-vs-XIP difference.

## Layout

```
CMakeLists.txt           the SCL library root (defines scl::* + optimal + the tests)
src/  include/           SCL loader/patcher sources + our glue (include/optimal, src/loader.cpp)
common-elf-reader/       ELF32 reader — git submodule (project root)
cmake/  tools-config.sh  toolchain files + scl-lib.cmake (the scl::* definitions)
tools/gen-imports.py     host tool: .so -> module load list (+ optional import header)
fixtures-src/build.sh    ONE generic recipe for every fixture (auto-packs addresses)
test/                    shared firmware scaffolding (platforms, tools, fixtures, xip infra)
test-ram/src/main.cpp    RAM app: full by-name exercise + RAM-buffer provide()
test-xip/src/main.cpp    XIP app: full by-name exercise + flat-fs provide()
test/common/ldscripts/   the reserved-window app linker scripts (single source of truth)
test-flatfs/             on-chip flat-fs used by the XIP test
test-ram/                RAM test app: provide-ram.cpp (blob from a RAM buffer)
test-xip/                XIP test app: provide-xip.cpp (blob from on-chip flash)
build-and-run.sh         one-shot fixtures → gen-imports → build → run [ram|xip] [board]
docs/ARCHITECTURE.md     full design write-up
```

See `docs/ARCHITECTURE.md` (and the rendered PDF) for the complete write-up.
