# floatapp — load a hard-float lib over semihosting, print to UART (WeAct F411CE)

A minimal example built per `the repo's docs/import_in_project.md`: the app
loads `fixtures/floatlib.so` **over semihosting**, calls its hard-float
`float_add` / `float_mul` **directly by name**, and prints the results to the
on-board **UART** (USART1, PA9 TX, 115200 8N1). The SCL loader is pulled in as an
INTERFACE source library; the SRAM window is **derived** by the fixtures build
(`fixtures/windows.generated.ld`) — no hand-typed addresses.

## Layout
```
CMakeLists.txt              main project (include scl-lib.cmake, WeAct build opts)
src/main.cpp               load floatlib.so via semihosting, call by name, print to UART
src/uart.c  include/uart.h USART1 driver (PA9/PA10, 115200)
src/app-windows.ld         device geometry + INCLUDE windows.generated.ld (no addresses)
fixtures/                  floatlib.h/.cpp (hard-float add/mul) + build.sh (derives window)
openocd-weact.cfg          WeAct on-board CMSIS-DAP (SWD)
run.sh                     build fixtures + firmware -> flash -> run
```

## Run
```sh
./run.sh                 # build + flash + run (Ctrl-C to stop)
RUN_MS=60000 ./run.sh    # auto-shutdown after 60 s
```
Open a serial terminal for the UART output:
```sh
picocom -b 115200 /dev/ttyUSB0     # or your PA9-TX adapter's port
```
Expected:
```
floatapp: 1 module(s) loaded + linked      (floatlib.so loaded over semihosting)
float_add(2.500, 3.500) = 6.000            (hard-float, by name)
float_mul(2.500, 3.500) = 8.750
floatapp: done
```
The results are printed to UART and mirrored to the semihosting console.

This example lives inside the optimal-shared repo (`examples/floatapp/`) and
consumes the loader from the repo root — no nested clone. From a fresh checkout:
```sh
git submodule update --init --recursive     # fetch common-elf-reader
examples/floatapp/run.sh
```
Prereqs: `arm-none-eabi-gcc`, `openocd` (xPacks auto-detected).
