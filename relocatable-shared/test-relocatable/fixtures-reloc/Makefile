# fixtures-reloc — the ELF32 *relocatable* (ET_REL) module fixtures.
#
# Built the "relocatable module" way for scl::reloc:
#   compile: -mlong-calls -fno-pic   (calls become ldr rN,=target ; blx rN —
#                                     an R_ARM_ABS32 literal the loader patches)
#   link:    -r                      (partial link: keep ET_REL, leave symbols
#                                     UND and relocations unresolved)
# Result: each module carries only R_ARM_ABS32 (+ R_ARM_TARGET1 == ABS32)
# relocations against .symtab. No PLT, no GOT, no r10, no Thumb-BL patching.
#
#   make            # -> build/<name>.rel.o for every fixture
#   make clean
TOOLCHAIN_PATH ?= $(shell dirname $$(which arm-none-eabi-gcc))
CC   := $(TOOLCHAIN_PATH)/arm-none-eabi-gcc
CXX  := $(TOOLCHAIN_PATH)/arm-none-eabi-g++
LD   := $(TOOLCHAIN_PATH)/arm-none-eabi-ld
READELF := $(TOOLCHAIN_PATH)/arm-none-eabi-readelf

# Purpose-built reloc sources live here (src/); the main ABI header
# (trace_event/fm_callback/...) and the shared fixtures come from test/fixtures-src.
SRC := src
INC := -I../../test/fixtures-src/include

# Hard-float M4 to match the freestanding platform. -mlong-calls -fno-pic is the
# whole point; -ffreestanding since these never see a libc.
MCU    := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
RELOC  := -mlong-calls -fno-pic
COMMON := $(MCU) $(RELOC) -Os -ffreestanding -fno-common -Wall -Wextra
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=c++20 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit

BUILD := build

# Demo modules (src/<name>.c) + the shared fixture set reused as ET_REL modules.
C_FIXTURES  := relprov relmod mylib1 mylib2
SHARED_C    := c1 c2 libprov libcons
SHARED_CXX  := cpp3
ALL_NAMES   := $(C_FIXTURES) $(SHARED_C) $(SHARED_CXX)
ALL_OBJS    := $(addprefix $(BUILD)/,$(addsuffix .rel.o,$(ALL_NAMES)))

SHARED_SRC := ../../test/fixtures-src

all: $(ALL_OBJS)
	@echo "== fixtures-reloc: $(words $(ALL_NAMES)) ET_REL modules in $(BUILD)/ =="

# Rule template: compile then partial-link (-r) to ET_REL.
# $(1)=name $(2)=compiler $(3)=flags $(4)=ext $(5)=source path
define RELOC_RULE
$(BUILD)/$(1).rel.o: $(5) | $(BUILD)
	$(2) $(3) $(INC) -c $$< -o $(BUILD)/$(1).o
	$(LD) -r $(BUILD)/$(1).o -o $$@
	@$(READELF) -h $$@ | grep -q "REL (Relocatable" && echo "  [ET_REL] $$@"
endef

$(foreach n,$(C_FIXTURES),  $(eval $(call RELOC_RULE,$(n),$(CC), $(CFLAGS),  c,  $(SRC)/$(n).c)))
$(foreach n,$(SHARED_C),    $(eval $(call RELOC_RULE,$(n),$(CC), $(CFLAGS),  c,  $(SHARED_SRC)/$(n)/$(n).c)))
$(foreach n,$(SHARED_CXX),  $(eval $(call RELOC_RULE,$(n),$(CXX),$(CXXFLAGS),cpp,$(SHARED_SRC)/$(n)/$(n).cpp)))

$(BUILD):
	mkdir -p $(BUILD)

# Show the relocation types each module carries (ABS32 / TARGET1 / THM_CALL only).
check: all
	@for m in $(ALL_NAMES); do \
	  echo "== $$m =="; \
	  $(READELF) -r $(BUILD)/$$m.rel.o | grep -oE "R_ARM_[A-Z0-9_]+" | sort | uniq -c; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
