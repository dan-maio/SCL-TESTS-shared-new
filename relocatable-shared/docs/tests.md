# relocatable-shared — the tests in detail

`relocatable-shared` is the RELOCATABLE-loader subset of the SCL. It ships one
firmware test app:

| App | Loader | Theme |
|---|---|---|
| `scl-fw-reloc` | `scl::reloc` | ELF32 **relocatable** (ET_REL) modules — no GOT, no PLT, no r10 |

The test (`test-relocatable`) is a **self-contained non-PIC firmware** (its own
`platform/reloc.ld`, freestanding platform: `startup.c`, `semihost.c`, `board.c`,
`crt.cpp`, `libc-shim.c`). Simple C-style main, no test framework.

---

## The loader — `scl::reloc`

`reloc::load(image, arena, resolve)` and `reloc::load_set(images, arena, resolve)`
load ELF32 **relocatable objects** (`.rel.o`, built `-mlong-calls -fno-pic -r`,
like a Linux kernel module):

- **No GOT, no PLT, no `_DYNAMIC`.** Section headers + `.symtab`/`.strtab`.
- `-fno-pic` ⇒ the module runs at fixed absolute addresses; every external
  reference is an `R_ARM_ABS32` literal (`ldr rN,=target ; blx rN`) — no r10,
  no trampoline, no >BL-range problem (`-mlong-calls`).
- The loader places every `SHF_ALLOC` section into a caller-supplied RAM arena,
  resolves `.symtab` (defined ⇒ `section_base + st_value`; UND ⇒ the resolver),
  applies `R_ARM_ABS32` / `R_ARM_TARGET1` / `R_ARM_REL32` / `R_ARM_THM_CALL`,
  then runs `.init_array`.
- `load_set` links a group in two phases (place all → resolve all → run ctors),
  so **cyclic** cross-module dependencies work.

A resolved export is a **plain function pointer** — call it directly.

## Fixtures — all 9

Built ET_REL by `test-relocatable/fixtures-reloc/Makefile` into
`fixtures-reloc/build/*.rel.o`: `relprov`, `relmod` (purpose-built), `mylib1`,
`mylib2`, `c1`, `c2`, `cpp3` (shared fixtures reused), `libprov`, `libcons`.

## How it tests

`main` provides the symbol surface the modules import (`trace_event/trace_u32`,
`fm_var/fm_info/fm_callback`, `AA/BB/s1/f/w`, a minimal `printf` (%u/%s),
`strcat`/`strlen`, `log`, `sinf/cosf/logf/sqrtf` from libm, `malloc/free` +
`operator new/delete` from `crt.cpp`, `__errno`, `__aeabi_d2f/f2d`) and a
`main_resolve` resolver. Each group is read over semihosting, loaded into a
reusable RAM arena, its exports resolved via `reloc::sym`, called, and the arena
reused for the next group.

### Group 1 — `relprov` + `relmod` (one `load_set`)

| Call | Expected | Proves |
|---|---|---|
| `prov_add(10,20)` | 8751 | exported fn + its own `.data` (ctor bumped `prov_data`) |
| `prov_state()` | 1 | `.bss` counter, constructor ran |
| `rel_compute(7)` | 13935 | **cross-module** call → `relprov.prov_add`, main `fm_callback`, local THM_CALL helper, `.rodata` const |

### Group 2 — `mylib1` (single `load`)

`library_function(42)=65`, `retstr()="Hello mylib1!"` — both constructors ran
(bb=40), `printf`/`strcat`/`strlen`/`log` imported from main.

### Group 3 — `mylib2` (single `load`)

`lf(7)=30` — constructor ran (bbb=50), imports from main (`w`, `printf`).

### Group 4 — `libprov` + `libcons` (one `load_set`)

`cons_call(5)=106` — **cross-module** import → `libprov.prov_value`.

### Group 5 — `cpp3` + `c2` + `c1` (one `load_set`, **cyclic**)

| Call | Expected | Proves |
|---|---|---|
| `cpp3_method(7,5)` | 1242 | C++ class, hard-float math (sinf/cosf/logf/sqrtf from main) |
| `c1_call_c2(7)` | 17469 | **cross-module** c1 → c2 (`c2_add`) |
| `c2_add(10,20)` | 8770 | c2's own `.data`/`.bss` |

All three constructors ran (`Banner.ctor`, `c2_ctor`, `c1_ctor`) — proving the
`load_set` two-phase resolution handled the mutual c1↔c2↔cpp3 cycle.

End of run: `RESULT: done` (the app exits cleanly via `semihost_exit` on the
board; under QEMU the runner stops at the marker).

---

## Running it

```sh
./test-relocatable/build-and-run.sh <weact|nucleo|qemu>
```

Verified: QEMU PASS and WeAct hardware executes all groups correctly (the
WCH-Link probe can drop the semihosting console right at the end, but every value
matches QEMU).
