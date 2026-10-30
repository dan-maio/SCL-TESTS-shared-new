# api-optimal-boot-table

`scl::optimal` with a table built at boot: `build_blob()`, `view`, `source()` and
`xip::nopic::set_main_symbols()`, plus counting lock and cache hooks.

main exports a decoy `app_gain()` returning 99, and the table maps that name to
`gain_from_table()` returning 1.5. `float_scale(2, 3)` must return 9, which shows
the table replaced `.dynsym`. The run also checks that the lock nests.

```sh
./run.sh
```

Expected last line: `RESULT: SUCCESS`.
