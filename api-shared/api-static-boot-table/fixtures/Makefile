# fixtures/Makefile — float fixture as a PLAIN -fPIC .so (ET_DYN), copied WHOLE
# into RAM and run there by the static loader (scl::static). r10-free
# (PC-relative GOT); no single-pic-base, no split, no windows.
TC   ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
CXX  := $(TC)/arm-none-eabi-g++
READELF := $(TC)/arm-none-eabi-readelf
MCU  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
PIC  := -fPIC -fno-inline-functions -fno-common
INC  := -Iinclude
CXXF := $(MCU) $(PIC) -O0 -g3 -std=c++20 -ffreestanding -fno-exceptions -fno-rtti \
        -fno-threadsafe-statics -fno-use-cxa-atexit $(INC)
B    := build
SRCS := floatlib
OBJS := $(addprefix $(B)/,$(addsuffix .o,$(SRCS)))

all: $(B)/floatlib.so
	@echo "== fixture: floatlib.so (plain -fPIC ET_DYN, full-in-RAM) =="
	@$(READELF) -h $(B)/floatlib.so | grep Type | sed 's/^/  /'

$(B)/%.o: src/%.cpp | $(B)
	$(CXX) $(CXXF) -c $< -o $@
$(B)/floatlib.so: $(OBJS)
	$(CXX) $(CXXF) -shared -nostartfiles -nostdlib -Wl,-E -Wl,--gc-sections \
	    -Wl,--unresolved-symbols=ignore-all $(OBJS) -o $@
$(B):
	mkdir -p $(B)
clean:
	rm -rf $(B)
.PHONY: all clean
