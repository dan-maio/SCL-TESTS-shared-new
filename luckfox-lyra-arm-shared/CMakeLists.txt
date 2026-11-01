# luckfox-lyra-arm-shared — ARM-state (A32) loader tests on the Luckfox Lyra B.
#
# The existing luckfox-lyra-shared suite builds every fixture with -mthumb.  This
# suite builds the same fixtures with -marm and runs them through each loader, so
# the A32 relocation paths (R_ARM_CALL, R_ARM_JUMP24, R_ARM_MOVW_ABS_NC,
# R_ARM_MOVT_ABS, ...) are exercised on the only board here with a full ARM core.
#
# Fixtures: libprov, libcons, mylib2 (group 2) and mylib1 (group 3, static).
# Tests: reloc (group 2), static (group 3), optimal, dynamic-nolib,
# dynamic-newlib, xip-static, xip-dynamic-nolib, xip-dynamic-newlib, overlay.
# Run with the board's hw.sh, like luckfox-lyra-shared.
cmake_minimum_required(VERSION 3.20)

set(UOS_PORT_DIR "/home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-aarch32"
    CACHE PATH "micro-os-plus-iii-aarch32 working copy")
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${UOS_PORT_DIR}/../micro-os-plus-iii/cmake/toolchains/arm-none-eabi.cmake")
endif()

project(scl-luckfox-lyra-arm LANGUAGES C CXX ASM)

set(BOARD "luckfox-lyra" CACHE STRING "" FORCE)
add_subdirectory("${UOS_PORT_DIR}" uos)

set(SCL_HOSTED ON CACHE BOOL "" FORCE)     # dynamic-newlib and xip-dynamic-newlib
set(SCL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared" CACHE PATH "SCL-shared checkout")
add_subdirectory("${SCL_DIR}" scl)

set(BOARD_SUPPORT "${UOS_PORT_DIR}/test/luckfox-lyra/src/test-smp-boot.cpp")
set(BOARD_INC "${UOS_PORT_DIR}/test/luckfox-lyra/include")
set(BOARD_LD "${UOS_PORT_DIR}/test/boards/luckfox-lyra/linker.ld")

# The point of this suite: every fixture is ARM state.
set(MCU -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4)
set(FIX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../relocatable-shared/test/fixtures-src")
set(FIX_INC -I${FIX_SRC}/include -I${FIX_SRC}/libprov -I${FIX_SRC}/libcons
            -I${FIX_SRC}/mylib1 -I${FIX_SRC}/mylib2)
set(FIX_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures-arm")
file(MAKE_DIRECTORY "${FIX_DIR}")
set(FIX_CXX -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit)
set(LD_SPLIT "${CMAKE_CURRENT_SOURCE_DIR}/../dynamic-shared/test-xip/fixtures-src/ldscripts/xip-split.ld")
set(LD_NOPIC "${CMAKE_CURRENT_SOURCE_DIR}/../optimal-shared/fixtures-src/ldscripts/nopic-split.ld")
set(WIN_TEXT 0x07A00000)
set(WIN_DATA 0x07A80000)
set(FIX_FILES "")

# name slot lang-opts sources: the four forms the loaders take, built -marm.
function(fixture_arm name slot opts)
    set(srcs ${ARGN})
    # ET_REL: -O0 makes GCC emit R_ARM_MOVW_ABS_NC/MOVT_ABS (not just ABS32).
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.rel.o"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -mlong-calls -mword-relocations -fno-pic -O0 -r -nostdlib
                ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.rel.o"
        DEPENDS ${srcs} VERBATIM)
    # r10 .so, split into an R-X and a RW segment.
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fpic -msingle-pic-base -mpic-register=r10
                -mno-pic-data-is-text-relative -O0 -shared -nostartfiles -nostdlib -nodefaultlibs
                -Wl,-E -Wl,-Bsymbolic -T "${LD_SPLIT}" ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.so"
        DEPENDS ${srcs} "${LD_SPLIT}" VERBATIM)
    # Plain -fPIC .so for scl::static and scl::xip-static.
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.pic.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fPIC -O0 -shared -nostartfiles -nostdlib -nodefaultlibs
                -Wl,-E ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.pic.so"
        DEPENDS ${srcs} VERBATIM)
    # No-pic fixed-window image.  -Os keeps the ARM build linkable as a shared
    # object (at -O0 the linker rejects text MOVW/MOVT against imports); the
    # R_ARM_MOVW/MOVT paths are already exercised by the ET_REL test above.
    math(EXPR tb "${WIN_TEXT} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR db "${WIN_DATA} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.opt.so"
        COMMAND sed -e "s/__TEXT_BASE__/${tb}/g" -e "s/__DATA_BASE__/${db}/g" "${LD_NOPIC}" > "${FIX_DIR}/${name}.opt.ld"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fno-pic -mword-relocations -mlong-calls -Os -shared
                -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-z,max-page-size=0x100
                -Wl,--unresolved-symbols=ignore-all -T "${FIX_DIR}/${name}.opt.ld"
                ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.opt.so"
        DEPENDS ${srcs} "${LD_NOPIC}")
    set(FIX_FILES ${FIX_FILES} "${FIX_DIR}/${name}.rel.o" "${FIX_DIR}/${name}.so"
                  "${FIX_DIR}/${name}.pic.so" "${FIX_DIR}/${name}.opt.so" PARENT_SCOPE)
endfunction()

set(KIND_C -std=gnu11 -ffreestanding -fno-common ${FIX_INC} -x c)
set(KIND_CPP ${FIX_CXX} -ffreestanding -fno-common ${FIX_INC} -x c++)
fixture_arm(libprov 5 KIND_C "${FIX_SRC}/libprov/libprov.c")
fixture_arm(libcons 6 KIND_C "${FIX_SRC}/libcons/libcons.c")
fixture_arm(mylib2 4 KIND_C "${FIX_SRC}/mylib2/mylib2.c")
fixture_arm(mylib1 3 KIND_C "${FIX_SRC}/../../test-relocatable/fixtures-reloc/src/mylib1.c")
add_custom_target(arm-fixtures DEPENDS ${FIX_FILES})

function(arm_test name loader target ext group)
    set(srcs ../luckfox-lyra-shared/src/fixtures-main.cpp
             ../luckfox-lyra-shared/src/embed-arena.S ${BOARD_SUPPORT})
    # The fixture text address check: heap-loaded (dynamic-newlib) and the
    # fixed 0x07Axxxxx optimal window are above 0x01000000; the others are not.
    set(top 0x00)
    if(target STREQUAL "scl::optimal" OR target STREQUAL "scl::dynamic-newlib")
        set(top 0x07)
    endif()
    set(defs TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST HW_BUILD
             TEST_${loader} TEST_NAME="${name}" TEST_EXPECT_TEXT_TOP=${top} TEST_ITERS=2000
             TEST_GROUP=${group} FIXTURE_DIR="${FIX_DIR}" FIXTURE_EXT="${ext}")
    if(target STREQUAL "scl::xip-static")
        list(APPEND defs TEST_XIP_STATIC)
    elseif(target STREQUAL "scl::xip-dynamic-nolib")
        list(APPEND defs TEST_XIP_NOLIB)
    endif()
    if(loader STREQUAL "LOADER_NOLIB" OR loader STREQUAL "LOADER_XIP")
        list(APPEND defs TEST_CROSS_CALL_LOOKUP)
    endif()
    if(target STREQUAL "scl::xip-dynamic-newlib")
        list(APPEND srcs "${SCL_DIR}/src/image-memory.cpp")
    endif()
    uos_add_app("${name}-hwd"
        SOURCES ${srcs}
        NCPU 3
        LINKER_SCRIPT "${BOARD_LD}"
        DEFINES ${defs}
        INCLUDES "${BOARD_INC}" "${FIX_SRC}/libcppeh/include"
        LIBRARIES micro-os-plus::aarch32 ${target})
    add_dependencies("${name}-hwd" arm-fixtures)
    target_compile_definitions("${name}-hwd" PRIVATE $<$<COMPILE_LANGUAGE:ASM>:SCL_ARENA_BYTES=98304>)
    if(loader STREQUAL "LOADER_OPTIMAL")
        target_compile_definitions("${name}-hwd" PRIVATE
            $<$<COMPILE_LANGUAGE:ASM>:SCL_WINDOW_TEXT=${WIN_TEXT} SCL_WINDOW_DATA=${WIN_DATA}>)
    endif()
    set_target_properties("${name}-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    list(APPEND ARM_TESTS "${name}-hwd")
    set(ARM_TESTS "${ARM_TESTS}" PARENT_SCOPE)
endfunction()

# Group 2: libprov + libcons + mylib2.  Group 3: adds mylib1 for scl::static.
arm_test(arm-reloc            LOADER_RELOC   scl::reloc                  .rel.o    2)
arm_test(arm-optimal          LOADER_OPTIMAL scl::optimal                .opt.so   2)
arm_test(arm-dynamic-nolib    LOADER_NOLIB   scl::dynamic-nolib          .so       2)
arm_test(arm-dynamic-newlib   LOADER_NEWLIB  scl::dynamic-newlib         .pic.so   2)
arm_test(arm-xip-static       LOADER_XIP     scl::xip-static             .so       2)
arm_test(arm-xip-dynamic-nolib LOADER_XIP    scl::xip-dynamic-nolib      .so       2)
arm_test(arm-xip-dynamic-newlib LOADER_XIP   scl::xip-dynamic-newlib     .so       2)
arm_test(arm-static           LOADER_STATIC  scl::static                 .pic.so   3)

# Overlay: the fixtures are linked with the ARM firmware and swapped at boot.
set(OV_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../overlay-shared/test-overlay/test1/src")
set_source_files_properties("${OV_SRC}/ov1.c" "${OV_SRC}/ov2.c" PROPERTIES COMPILE_OPTIONS "-fno-pic;-mlong-calls")
set_source_files_properties("${OV_SRC}/ovcpp.cpp" PROPERTIES COMPILE_OPTIONS
    "-fno-pic;-mlong-calls;-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit;-fno-unwind-tables;-fno-asynchronous-unwind-tables")
file(READ "${BOARD_LD}" OV_LD)
string(REGEX REPLACE "(RAM \\(rwx\\)[^\n]*\n)" "\\1   OVL (rwx) : ORIGIN = 0x07A00000, LENGTH = 16K\n" OV_LD "${OV_LD}")
string(REGEX REPLACE "(    \\.bss \\(NOLOAD\\))"
"    OVERLAY : NOCROSSREFS
    {
        .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
        .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
        .ovcpp { KEEP(*(.ovcpp.text)) KEEP(*(.ovcpp.rodata)) KEEP(*(.ovcpp.data)) }
    } >OVL AT>RAM
    _ov1_lma = LOADADDR(.ov1);     _ov1_size = SIZEOF(.ov1);
    _ov2_lma = LOADADDR(.ov2);     _ov2_size = SIZEOF(.ov2);
    _ovcpp_lma = LOADADDR(.ovcpp); _ovcpp_size = SIZEOF(.ovcpp);
    _ovl_vma = ADDR(.ov1);

\\1" OV_LD "${OV_LD}")
file(WRITE "${CMAKE_BINARY_DIR}/overlay.ld" "${OV_LD}")
uos_add_app("arm-overlay-hwd"
    SOURCES ../luckfox-lyra-shared/src/overlay-main.cpp
            "${OV_SRC}/ov1.c" "${OV_SRC}/ov2.c" "${OV_SRC}/ovcpp.cpp" ${BOARD_SUPPORT}
    NCPU 3
    LINKER_SCRIPT "${CMAKE_BINARY_DIR}/overlay.ld"
    DEFINES TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST HW_BUILD
            TEST_NAME="arm-overlay" TEST_ITERS=2000
    INCLUDES "${BOARD_INC}" "${FIX_SRC}/include"
    LIBRARIES micro-os-plus::aarch32 scl::overlay)
set_target_properties("arm-overlay-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
list(APPEND ARM_TESTS "arm-overlay-hwd")

add_custom_target(arm-tests DEPENDS ${ARM_TESTS})
