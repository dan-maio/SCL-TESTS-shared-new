# The SCL Libraries, Explained Simply

A plain-English companion to [README.md](../README.md). It answers three questions for each
of the SCL libraries: **why does it exist**, **how does it work**, and
**how does a real `main()` use it** — with the exact lifecycle of every test
application broken into *start → configure/activate → use → end/close*.

---

## 1. The problem the static libraries solve

A normal microcontroller firmware is **one frozen block**: every function it
will ever call is glued in at compile time. If you want to run code that is
*not* inside that block — a plug-in, a downloaded module, an alternative
algorithm — you need machinery that answers three questions at run time:

1. **Where do the bytes of the extra code come from?** (a file on the host, a
   region of flash, somewhere in the firmware image itself)
2. **Where does that code run from?** (a piece of RAM chosen at run time, or a
   fixed address chosen by the linker)
3. **How do names get connected?** The extra code calls `printf` or reads
   `fm_var` from the firmware — someone must patch the right addresses in, and
   the firmware must be able to find `library_function` inside the extra code.

The static SCL libraries are different answers to those questions, with
different trade-offs. `scl::common` is the toolbox they all share.

---

## 2. `scl::common` — the shared toolbox

**Why.** Most of the loaders need the same low-level chores: read an ELF
file, find a symbol by name, apply relocations. Writing that once is the whole
point of the SCL (the original repo had four diverging copies).

**What is inside, in plain words:**

| Piece | Plain-English job |
|---|---|
| `byte_source` | "give me N bytes at offset X" — hides WHERE a library's bytes live (host file, flash, RAM buffer) |
| `elf_image` | understands the `.so` file format: headers, sections, sizes |
| `symbol_resolver` | phone book: name → address, using the ELF hash table |
| `reloc_applier` / `relocator` | the patcher: walks the relocation list and writes correct addresses into the loaded image |
| `region_allocator` | hands out RAM slots for loaded libraries from a reserved arena |
| `LazyResolve` | figure out, from a patched call slot, *which* symbol it stands for (used for on-first-call binding) |
| `result<T>` / `Error` | C++20 "value or error code" return type used everywhere |
| `scl_log` hook | all diagnostics go through two tiny functions the app may override; silent by default |

**Example.** Every loader begins the same way:

```cpp
Somebyte_source src(...);                    // app decides where bytes come from
auto img = loader::elf_image::parse(src);    // common: is this a valid ARM .so?
// -> from here the loaders diverge.
```

---

## 3. `scl::static` — the standalone ELF32 patcher

**Why.** A self-contained loader (imported from the former `shared-static-rework`
firmware) that takes a PIC `.so`, drops it in a RAM arena, and **patches its GOT**
in place so the bare-metal app can call in — like `dynamic-newlib`, but shipped as
its own demo firmware with its own µOS++ startup and the `_reloc`-table model.

**How.** Its `elf_reader` is a thin adapter over the shared `loader::elf_image`
parser; `shlib_loader` does the segment load, `patch_shlib`/`patch_main` GOT
patching, `R_ARM_*` fixups, weak symbols, constructors, and `DT_NEEDED`. Lives in
`src/static/` + `include/static/`; the demo app is `test-static/test-static/`.

---

## 4. One table to compare them

| | `scl::static` | `scl::xip-*` | `scl::xip-nopic` |
|---|---|---|---|
| Bytes come from | any `byte_source` | any `byte_source` | any `byte_source` |
| Runs from | caller-provided buffer | flash (R-X) + RAM (RW) | fixed RAM windows (text + data) |
| Names connected | at load time, GOT patch | at load time, split-base | at load time, R_ARM_ABS32 (or `patch_main` by name) |
| Calling in | `pic_call_*` | `pic_call_*` (r10) | plain / **by name**, no r10 |
| Undo on close | no unload | free RAM arena | re-copy on next load |
| Best for | standalone patcher, no -fpic fw | RAM-constrained, flash-resident code | no-r10 direct calls, all boards |

*(Other families — `scl::dynamic-nolib`, flat-fs, `scl::overlay`,
`scl::dynamic-newlib`, `scl::reloc` — exist in the full project.)*

*Start-to-close in one sentence each:* **static** = copy to heap → patch
(writing everything down) → call directly → replay the notes backwards;
**xip** = classify segments → copy RW to RAM → patch split-base → execute from flash.
