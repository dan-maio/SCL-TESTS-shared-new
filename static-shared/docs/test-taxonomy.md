# SCL Test Taxonomy — how the tests are organised and named

The workspace has one test tree per **loading technique**, and everything else is
a variation inside it. This document defines that structure, the rules for naming
at each level, classifies the existing tests, and records the migration still
outstanding.

It exists because the naming had drifted: one test was named for its transport
(`spi`) and for a calling style it did not use (`direct`), which made it look like
a technique nobody else covered when it was in fact a second scenario of one that
already existed.

---

## 1. Two levels, two different rules

### Level 1 — the technique. Fixed set, strict names.

Every test lives under a top-level directory beginning `test-`, one per loading
technique:

| Directory | Technique | Library |
|---|---|---|
| `test-static/` | resolve once at load, patch the GOT, no runtime service | `scl::static` |
| `test-xip/` | R-X executes in place from flash; only RW copied to RAM (or fixed RAM windows for `nopic`) | `xip::stat` + `xip::nopic` adapters |

This list is closed. A new top-level `test-*` directory means a genuinely new way
of getting code into the target — not a new peripheral, storage medium, or module
set.

### Level 2 — everything else. Relaxed names.

Inside a technique directory, name each test after whatever combination it
actually exercises. These names are **not** restricted to SCL concepts — the
peripheral, the storage, the module set, the calling style all qualify:

```
test-static/test-static-direct
test-static/test-static-interrupt
test-xip/test-xip-spi-uart
test-xip/test-xip-static/spi-flash
```

Nest a further level when two tests share a configuration and differ only in
scenario.

### Why the levels differ

Level 1 is a **claim about mechanism**: put a word there and you assert a distinct
way of loading code. Level 2 only distinguishes siblings, so it can say anything
useful.

That is the whole lesson of the rename. `spi` was wrong at level 1 because there
is no SPI loading technique — XIP always executes from internal flash, whatever
the module travelled over. The same word is right at level 2, where it states
where the modules came from. Same word, different level, and only one of them was
a lie.

Conversely, stripping the word entirely is its own mistake: the first correction
renamed the test to `test2`, which asserted nothing false but told the next reader
nothing either.

> **If a word would change when you change where the file came from, it belongs at
> level 2, never at level 1.**

---

## 2. Vocabulary for level 2

Not mandatory, but these carry established meaning — reuse them rather than
inventing synonyms:

| Word | Means |
|---|---|
| **direct** | the app calls module functions and reads its globals by name: `library_function(7)`, `s_info.tag` |
| *(absent)* | the app names them in a string: `call_sym0(m, "uart_init")`, `m.sym("f")`, `resolve(m, "s_info")` |
| **interrupt** | module code runs from an ISR, so `r10` must be established on handler entry |
| **nolib** / **newlib** | freestanding no-heap, versus hosted newlib with a real heap |

### On "direct"

It describes **the call site**, not how the binding happens. Two mechanisms
produce it and the name does not distinguish them:

- **link time** — the `.so` is *additionally* named on main's link line
  (`-l:slib.so …`), so the linker binds the reference at build time.
  `test-xip-static-direct`, `test-xip-interrupt-static-direct`, `test-xip-static-no-pic-simple`.
- **load time** — the loader walks main's own `_DYNAMIC` and points its GOT slots
  at the module's exports: `shlib_loader::patch_main()`, or
  `xip::stat::load(..., patch_host_got=true)`. `test-static-direct`, `test-drv`,
  `test-static/test-static`, `test-static/test-static-cpp`.

`scl::Module` does **not** make a test direct. It removes boilerplate around the
resolved form, but the function is still named in a string.

Do not confuse the link line with what gets loaded: *every* static test loads
`.so` modules at run time — that is the exercise. Naming them on the link line is
an extra build step only the link-time-direct tests take.

---

## 3. The tree

```
test/                      shared infrastructure — no tests
    platforms/  fixtures/  fixtures-src/  tools/  common/  cmake/

test-static/               scl::static
    test-static            two .so over semihosting
    test-static-cpp        same, C++ fixtures
    test-static-direct     direct API calls
    test-drv               real drivers (spi/uart/led), SPI flash + flat-fs

test-xip/                  xip::stat + xip::nopic + the xip adapters
    test-xip-static        semihosting/ and spi-flash/
    test-xip-static-direct
    test-xip-static-no-pic
    test-xip-static-no-pic-simple
    test-xip-interrupt-static
    test-xip-interrupt-static-direct
```

`test/` is deliberately not a technique tree and holds no tests. It is the shared
infrastructure every technique depends on: board platforms, built fixtures and
their sources, the flash-run tooling, common headers and cmake helpers.

### Calling style, by test

| direct — `f()` at the call site | resolved — `m.sym("f")` / `call_sym*` |
|---|---|
| `test-static/test-static` (load) | `test-xip-static/semihosting` |
| `test-static/test-static-cpp` (load) | `test-xip-static/spi-flash` |
| `test-static/test-static-direct` (load) | `test-xip-interrupt-static` |
| `test-static/test-drv` (load) | |
| `test-xip-static-direct` (link) | |
| `test-xip-interrupt-static-direct` (link) | |

Every XIP test executes from **internal** flash. The flat-fs is at
`_fs_base = ORIGIN(FLASH_FS)` = `0x08040000` (`sections-flatfs-hosted.ld`), and
`xip::stat::load()` allocates RAM only for `data_memsz` — text is never copied.

### Running a test

Each technique tree has a `build-and-run.sh` whose target names are the
subdirectory names with the `test-` prefix dropped, so the option *is* the path:

```
test-xip/build-and-run.sh      static/spi-flash     # test-xip/test-xip-static/spi-flash
test-static/build-and-run.sh   drv                  # test-static/test-drv
```

Run one with no arguments to list its targets and the directory each maps to.
The tables are explicit rather than globbed, because the cmake target name does
not always follow the directory name, but each entry is checked against the
filesystem before use.

### One wart worth knowing

`test-xip/test-xip-static/spi-flash` builds its four modules from
`test-static/test-drv/fixtures/src/`, so an XIP test depends on a *static* test's
directory existing. The driver sources are really shared fixtures and belong under
`test/fixtures-src/`; until they move, do not delete or relocate `test-drv`
without checking that reference.

---

## 4. Naming a new test

(For *building* one — the six decisions and their consequences — see
`doc/creating-a-test.md`. This section is only about the name.)

1. Pick the technique. That fixes the top-level directory, from the closed list in
   §1. If none fits, you are adding a loading technique, which is a much larger
   claim than a new test.
2. Inside it, name the test after what it actually exercises. Reuse the §2
   vocabulary where it applies; add whatever else is useful.
3. If it matches an existing test on everything that matters, it is a scenario,
   not a test — nest it beside that one and name it after what varies
   (`semihosting`, `spi-flash`), not by number.
