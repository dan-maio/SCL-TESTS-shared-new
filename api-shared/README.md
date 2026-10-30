# api-shared

On-target tests of the APIs added by SCL-shared-new. Each test is a small app that
links its board support from an existing suite (`platform` is a symlink) and the
loader from the root `SCL-shared` submodule.

| Test | Loader | Platform | New APIs |
|---|---|---|---|
| `api-static-boot-table` | `scl::static` | `static-shared` | `SCL_REGISTER_SYM`, `registered()`, `source_registered()`, `add_entries()`, `scl_publish_release()`, lock and cache hooks |
| `api-optimal-boot-table` | `scl::optimal` | `optimal-shared` | `build_blob()`, `view`, `source()`, `set_main_symbols()`, lock and cache hooks |
| `api-dynamic-xip-semihost-table` | `scl::xip-dynamic-newlib` | `dynamic-shared` | `collect_buffered()`, `collect()`, `for_each()`, `set_symtab(const scl_sym_entry*, ...)` |
| `api-nolib-eager` | `scl::dynamic-nolib` | `dynamic-shared` | `dl_set_symtab()`, `dl_set_lazy_binding(false)`, lock and cache hooks |

In each test the module imports `app_gain()` from main, so `float_scale(2, 3)` only
returns 9 if the new table supplied it. The optimal and nolib tests also export a
decoy `app_gain()` returning 99, reachable only through `.dynsym`.

```sh
../run-all-weact.sh '^api-'         # from this directory
cd api-nolib-eager && RUN_MS=20000 ./run.sh
```

Each test ends with `RESULT: SUCCESS` or `RESULT: FAILURE`.
