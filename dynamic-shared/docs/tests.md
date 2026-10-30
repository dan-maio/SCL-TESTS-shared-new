# dynamic-shared — the tests in detail

`dynamic-shared` is the DYNAMIC shared-library loader subset of the SCL. It
ships **three firmware test apps** across **three loaders**:

| App | Loader | Theme |
|---|---|---|
| `scl-fw-posix-dl` | `scl::dynamic-newlib` (hosted) | pure POSIX `dlopen`/`dlsym`/`dlclose`/`dlerror` |
| `scl-fw-xip-nolib1` | `scl::xip-dynamic-nolib` (freestanding) | execute-in-place, R-X from flash |
| `scl-fw-xip-newlib1` | `scl::xip-dynamic-newlib` (hosted) | execute-in-place, embedded symtab |

All three are **hosted** (newlib + CMSIS/HAL) firmware, runnable on WeAct,
Nucleo or QEMU. Every test uses a **simple main** — no test framework, just
load / call / unload.

---

## 1. `scl-fw-posix-dl` — `test-dynamic/test1`

The reference POSIX dynamic-loading demo: `main.cpp` exposes the `dl::` facade
under the **standard POSIX names**, so the test reads like ordinary C:

```c
void* dlopen(const char* path, int /*mode*/) { return dl::dlopen(path); }
void* dlsym(void* h, const char* n)           { return dl::dlsym((dl::handle)h, n); }
int   dlclose(void* h)                        { return dl::dlclose((dl::handle)h); }
const char* dlerror(void)                     { return dl::dlerror(); }
```

### Fixtures — all 12 available

Every fixture is rebuilt from source as a plain `-fPIC -shared -Wl,-E` ET_DYN
(the committed `.so` under `test/fixtures/` are built for the r10 static/XIP
loaders, which the hosted loader cannot run — it does not set r10). The build is
`test-dynamic/fixtures-src/build.sh` → `test-dynamic/fixtures/`:

`c1`, `c2`, `cpp3`, `libprov`, `libcons`, `libcppsh`, `libcppeh`, `ehx_catch`,
`mylib1`, `mylib2`, `libsh1`, `libsh2`.

### How it tests

For each fixture it does `dlopen` → `dlsym` → call an export → `dlclose`.
Cross-module pairs are loaded **together** (the provider stays resident until the
consumer is exercised), and every fixture is unloaded right after its own test so
the heap never holds all twelve images at once.

| Fixture | Export called | Expected | Proves |
|---|---|---|---|
| `libprov` | `prov_value(5)` | 105 | plain C load/call |
| `libcons` | `cons_call(5)` | 106 | **cross-module** import → `libprov.prov_value` |
| `cpp3` | `cpp3_heap(8)` | 533 | C++ class, `malloc`/`new` from main's heap |
| `mylib1` | `library_function(42)` | 65 | constructor ran (bb=40), `printf` import |
| `mylib2` | `lf(7)` | 30 | constructor ran |
| `libsh1` | `library_function(7)` | 30 | the scl::static demo lib as a dynamic fixture |
| `libsh2` | `lf(7)` | 30 | same |
| `libcppsh` | `cppsh_compute(w,5)` | 92 | C++ virtual class, `operator new` from main |
| `libcppeh` | `eh_dyncast(0)` | 2 | RTTI / `__dynamic_cast` resolved from main |
| `ehx_catch` | `ehx_catch_cross(0)` | 0 | **cross-module** import → `libcppeh.eh_throw` |
| `c2` + `c1` | `c1_call_c2(7)` | 17469 | **cross-module** c1 → c2 |

It also checks the **error path**: `dlopen("does_not_exist.so")` returns `nullptr`
and `dlerror()` reports `"FileNotFound"`.

End of run: `RESULT: SUCCESS` if every `dlopen`/`dlsym` succeeded.

### Main-side surface the fixtures import

`main` defines `AA/BB/s1/f/w`, `fm_var/fm_info`, `trace_event/trace_u32`,
`fm_callback`, and **anchors** the borrowed libc/libm/libsupc++ symbols
(`printf, puts, strcat, strlen, log, sinf/cosf/logf/sqrtf, malloc/free,
_Znwj/_ZdlPvj/_ZnwjRKSt9nothrow_t, __cxa_*, __gxx_personality_v0,
__dynamic_cast, __aeabi_*`). The `gen-symtab` embedded-symbol-table whitelist
was extended so those internal symbols reach main's symtab and the loader can
resolve them.

---

## 2. `scl-fw-xip-nolib1` — `test-xip/test-xip-dynamic-nolib/test1`

**Execute-in-place** with the freestanding loader (`scl::xip-dynamic-nolib`,
which adapts `scl::dynamic-nolib`).

The fixtures are **split `R-X | RW`** PIC `.so` built with the r10
single-pic-base model and `xip-split.ld` (`VA == file_offset`), so the **R-X
segment executes in place straight from flash** while only the RW segment
(GOT/.data/.bss) is copied to RAM; `r10` = the data base.

### Flow

1. `fw::fs_format()` — erase + init the on-chip flat-fs.
2. `provision(name)`: read the `.so` over semihosting; if it is XIP-eligible
   (`xip::classify_segments` split **and** `xip::text_is_immutable`), run
   `xip::nolib::patch_main_imports()` to pre-bind the flash-resident `.got.plt`
   slots (required — a flash PLT stub reads its GOT slot PC-relative, so the
   slot must already hold the target before the image is flashed); then
   `fw::fs_add` into the flat-fs.
3. `xip::nolib::dlopen(blob, name)` → `dlsym` → `get_got_base(h)` → call via
   `pic_call_u32(fn, (void*)got, arg)` (the r10 trampoline) → `dlclose`.

### Results

```
mylib1 XIP: aa=20 bb=40                 (ctor ran: aa doubled, bb = aa+20)
  library_function(39)=62  library_function2(10)=32  retstr()="Hello mylib1!"
mylib2 XIP: lf(7)=30
RESULT: done
```

`is_xip(h)` confirms the module actually executes its text from flash, not RAM.

---

## 3. `scl-fw-xip-newlib1` — `test-xip/test-xip-dynamic-newlib/test1`

Same XIP behaviour, but through the **hosted** loader
(`scl::xip-dynamic-newlib`). The difference: main's exported symbols come from
the **embedded symbol table** (`scl_embed_symtab` — 313 symbols written into a
flash blob post-link and read at boot into `xip::newlib::set_symtab`). It is a
separate build with the **hosted toolchain** (`arm-none-eabi-hosted.cmake`),
because the hosted parts need a real C++ runtime.

Identical fixtures and expected results to the nolib variant (`mylib1 XIP
aa=20 bb=40`, functions, `retstr`, `mylib2 lf(7)=30`, `RESULT: done`).

---

## Running them

```sh
# POSIX battery (hosted toolchain):
./test-dynamic/build-and-run.sh <weact|nucleo|qemu> test1

# XIP variants (fixtures are XIP-split by the script):
./test-xip/build-and-run.sh <weact|nucleo|qemu> nolib
./test-xip/build-and-run.sh <weact|nucleo|qemu> newlib
```

Verified: QEMU PASS and WeAct hardware PASS for all three.
