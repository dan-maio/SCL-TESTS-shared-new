# fixtures/Makefile — floatlib.so as an r10-based split .so for the XIP dynamic
# loader (scl::xip-dynamic-newlib): -fpic -msingle-pic-base -mpic-register=r10
# -mno-pic-data-is-text-relative, linked into two segments (R-X | RW) by
# xip-split.ld with VA == file_offset, so the R-X text can execute from flash.
TC   ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
CXX  := $(TC)/arm-none-eabi-g++
READELF := $(TC)/arm-none-eabi-readelf
STRIP := $(TC)/arm-none-eabi-strip
LD   := xip-split.ld
MCU  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
PIC  := -fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative
OPT  := -O0 -ffunction-sections -fdata-sections -std=c++20 -fno-exceptions -fno-rtti \
        -fno-threadsafe-statics -fno-use-cxa-atexit
LDF  := -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-Bsymbolic -Wl,--gc-sections

all: build/floatlib.so
build/floatlib.so: src/floatlib.cpp include/floatlib.h $(LD)
	@mkdir -p build
	$(CXX) $(MCU) $(PIC) $(OPT) -Iinclude $(LDF) -T $(LD) -o $@ src/floatlib.cpp
	$(STRIP) --strip-debug $@
	@$(READELF) -l $@ | awk '/LOAD/{print "    "$$0}'   # must show R E + RW
clean:; rm -rf build
.PHONY: all clean
