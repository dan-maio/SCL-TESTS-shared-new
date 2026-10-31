# luckfox-lyra-shared — SCL on the Luckfox Lyra B: RK3506, 3x Cortex-A7, under the µOS++ III SMP
# kernel. All code runs from DRAM: there is no execute-in-place flash here, and the board's SPI
# flash holds its boot firmware, which apps do not use. Storage is the SD card.
cmake_minimum_required(VERSION 3.20)

set(UOS_PORT_DIR "/home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-aarch32"
    CACHE PATH "micro-os-plus-iii-aarch32 working copy")
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${UOS_PORT_DIR}/../micro-os-plus-iii/cmake/toolchains/arm-none-eabi.cmake")
endif()

project(scl-luckfox-lyra LANGUAGES C CXX ASM)

set(BOARD "luckfox-lyra" CACHE STRING "" FORCE)
add_subdirectory("${UOS_PORT_DIR}" uos)

set(SCL_HOSTED ON CACHE BOOL "" FORCE)     # scl::xip-dynamic-newlib needs it
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared" scl)

set(BOARD_SUPPORT "${UOS_PORT_DIR}/test/luckfox-lyra/src/test-smp-boot.cpp")
set(BOARD_INC "${UOS_PORT_DIR}/test/luckfox-lyra/include")
set(BOARD_LD "${UOS_PORT_DIR}/test/boards/luckfox-lyra/linker.ld")

# Fixtures build as Thumb, as on the Cortex-M boards: the loaders' ET_REL relocations are Thumb.
set(MCU -mcpu=cortex-a7 -mthumb -mfloat-abi=hard -mfpu=neon-vfpv4)
set(FIX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../relocatable-shared/test/fixtures-src")
set(FIX_INC -I${FIX_SRC}/include -I${FIX_SRC}/c1 -I${FIX_SRC}/c2 -I${FIX_SRC}/cpp3 -I${FIX_SRC}/mylib1
            -I${FIX_SRC}/mylib2 -I${FIX_SRC}/libprov -I${FIX_SRC}/libcons)
set(FIX_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIX_DIR}")
set(FIX_CXX -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit)
set(LD_SPLIT "${CMAKE_CURRENT_SOURCE_DIR}/../dynamic-shared/test-xip/fixtures-src/ldscripts/xip-split.ld")
set(LD_NOPIC "${CMAKE_CURRENT_SOURCE_DIR}/../optimal-shared/fixtures-src/ldscripts/nopic-split.ld")
set(WIN_TEXT 0x07A00000)
set(WIN_DATA 0x07A80000)
set(FIX_FILES "")
# name slot lang-opts sources: the four forms the loaders take.
function(fixture name slot opts)
    set(srcs ${ARGN})
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.rel.o"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -mlong-calls -mword-relocations -fno-pic -Os -r -nostdlib ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.rel.o"
        DEPENDS ${srcs} VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fpic -msingle-pic-base -mpic-register=r10 -mno-pic-data-is-text-relative -O0
                -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-Bsymbolic -T "${LD_SPLIT}" ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.so"
        DEPENDS ${srcs} VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.pic.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fPIC -O0 -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.pic.so"
        DEPENDS ${srcs} VERBATIM)
    math(EXPR tb "${WIN_TEXT} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
    math(EXPR db "${WIN_DATA} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
    add_custom_command(OUTPUT "${FIX_DIR}/${name}.opt.so"
        COMMAND sed -e "s/__TEXT_BASE__/${tb}/g" -e "s/__DATA_BASE__/${db}/g" "${LD_NOPIC}" > "${FIX_DIR}/${name}.opt.ld"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fno-pic -mword-relocations -mlong-calls -O0 -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E
                -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all -T "${FIX_DIR}/${name}.opt.ld" ${${opts}} ${srcs} -o "${FIX_DIR}/${name}.opt.so"
        DEPENDS ${srcs})
    set(FIX_FILES ${FIX_FILES} "${FIX_DIR}/${name}.rel.o" "${FIX_DIR}/${name}.so" "${FIX_DIR}/${name}.pic.so" "${FIX_DIR}/${name}.opt.so" PARENT_SCOPE)
endfunction()
set(KIND_C -std=gnu11 -ffreestanding -fno-common ${FIX_INC} -x c)
set(KIND_CPP ${FIX_CXX} -ffreestanding -fno-common ${FIX_INC} -x c++)
fixture(cpp3 0 KIND_CPP "${FIX_SRC}/cpp3/cpp3.cpp")
fixture(c2 1 KIND_C "${FIX_SRC}/c2/c2.c")
fixture(c1 2 KIND_C "${FIX_SRC}/c1/c1.c")
fixture(mylib1 3 KIND_C "${FIX_SRC}/../../test-relocatable/fixtures-reloc/src/mylib1.c")
fixture(mylib2 4 KIND_C "${FIX_SRC}/mylib2/mylib2.c")
fixture(libprov 5 KIND_C "${FIX_SRC}/libprov/libprov.c")
fixture(libcons 6 KIND_C "${FIX_SRC}/libcons/libcons.c")
fixture(slib 7 KIND_C "${CMAKE_CURRENT_SOURCE_DIR}/../static-shared/test-xip/test-xip-interrupt-static/fixtures-src/slib.c")
set(KIND_EH -std=c++20 -fno-threadsafe-statics -I${FIX_SRC}/libcppeh/include -I${FIX_SRC}/libcppsh/include -x c++)
fixture(libcppeh 0 KIND_EH "${FIX_SRC}/libcppeh/src/cppeh.cpp")
fixture(ehx_catch 1 KIND_EH "${FIX_SRC}/ehx/ehx-catch.cpp")
fixture(libcppsh 2 KIND_EH "${FIX_SRC}/libcppsh/src/cppsh-api.cpp" "${FIX_SRC}/libcppsh/src/widget.cpp")
set(KIND_RK -std=gnu11 -ffreestanding -fno-common -x c)
fixture(libled 0 KIND_RK "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rk3506/led.c")
fixture(libuart 1 KIND_RK "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rk3506/uart.c")
fixture(libsd 2 KIND_RK "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rk3506/sd.c")
add_custom_target(scl-fix-modules DEPENDS ${FIX_FILES})

function(scl_fix_test name loader target ext top iters group)
    set(srcs src/fixtures-main.cpp src/embed-arena.S ${BOARD_SUPPORT})
    set(defs TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST HW_BUILD TEST_${loader} TEST_NAME="${name}" TEST_EXPECT_TEXT_TOP=${top} TEST_ITERS=${iters}
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
        list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared/src/image-memory.cpp")
    endif()
    set(libs micro-os-plus::aarch32 ${target})
    if(group EQUAL 6)
        list(APPEND libs micro-os-plus::devices-rk3506)
    endif()
    if("HIDE_HOST_F" IN_LIST ARGN)
        list(APPEND defs TEST_HIDE_HOST_F TEST_EXPECT_MISSING="host_f")
    endif()
    if("THROW" IN_LIST ARGN)
        list(APPEND defs TEST_EH_THROW)
    endif()
    uos_add_app("${name}-hwd"
        SOURCES ${srcs}
        NCPU 3
        LINKER_SCRIPT "${BOARD_LD}"
        DEFINES ${defs}
        INCLUDES "${BOARD_INC}" "${FIX_SRC}/libcppeh/include"
        LIBRARIES ${libs})
    list(APPEND SCL_TESTS "${name}-hwd")
    set(SCL_TESTS "${SCL_TESTS}" PARENT_SCOPE)
    add_dependencies("${name}-hwd" scl-fix-modules)
    if(group EQUAL 4)
        # nano.specs selects libstdc++_nano, whose __cxa_throw has no unwind tables.
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -mcpu=cortex-a7 -marm -mfloat-abi=hard -mfpu=neon-vfpv4 -print-file-name=libsupc++.a
                        OUTPUT_VARIABLE libsupcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
        target_link_libraries("${name}-hwd" PRIVATE "${libsupcxx}")
        target_compile_options("${name}-hwd" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexceptions> $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
    endif()
    target_compile_definitions("${name}-hwd" PRIVATE $<$<COMPILE_LANGUAGE:ASM>:SCL_ARENA_BYTES=98304>)
    if(loader STREQUAL "LOADER_OPTIMAL")
        target_compile_definitions("${name}-hwd" PRIVATE $<$<COMPILE_LANGUAGE:ASM>:SCL_WINDOW_TEXT=${WIN_TEXT} SCL_WINDOW_DATA=${WIN_DATA}>)
    endif()
    set_target_properties("${name}-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
endfunction()
# name loader target extension text-MSB iterations group
foreach(g 1 2 5 6 4)
    set(n ${g})
    if(g EQUAL 1)
        set(n "")
    endif()
    set(extra "")
    set(it 2000)
    if(g EQUAL 4)
        set(extra THROW)
        set(it 200)
    elseif(g EQUAL 5)
        set(it 5000)
    elseif(g EQUAL 6)
        set(it 20)
    endif()
    scl_fix_test(scl-fix${n}-reloc LOADER_RELOC scl::reloc .rel.o 0x00 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-nolib LOADER_NOLIB scl::dynamic-nolib .so 0x00 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-newlib LOADER_NEWLIB scl::dynamic-newlib .pic.so 0x06 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-optimal LOADER_OPTIMAL scl::optimal .opt.so 0x07 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-xip LOADER_XIP scl::xip-dynamic-newlib .so 0x00 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-xipstatic LOADER_XIP scl::xip-static .so 0x00 ${it} ${g} ${extra})
    scl_fix_test(scl-fix${n}-xipnolib LOADER_XIP scl::xip-dynamic-nolib .so 0x00 ${it} ${g} ${extra})
    if(NOT g EQUAL 1 AND NOT g EQUAL 2)   # scl::static has no cyclic imports; group 3 replaces 1 and 2
        scl_fix_test(scl-fix${n}-static LOADER_STATIC scl::static .pic.so 0x00 ${it} ${g} ${extra})
    endif()
endforeach()
scl_fix_test(scl-fix3-static LOADER_STATIC scl::static .pic.so 0x00 2000 3)
# Negative tests: main's table hides host_f, and it must be reported by name.
scl_fix_test(scl-neg-reloc LOADER_RELOC scl::reloc .rel.o 0x00 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-nolib LOADER_NOLIB scl::dynamic-nolib .so 0x00 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-newlib LOADER_NEWLIB scl::dynamic-newlib .pic.so 0x06 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-static LOADER_STATIC scl::static .pic.so 0x00 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-optimal LOADER_OPTIMAL scl::optimal .opt.so 0x07 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-xip LOADER_XIP scl::xip-dynamic-newlib .so 0x00 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-xipstatic LOADER_XIP scl::xip-static .so 0x00 1 5 HIDE_HOST_F)
scl_fix_test(scl-neg-xipnolib LOADER_XIP scl::xip-dynamic-nolib .so 0x00 1 5 HIDE_HOST_F)
# The overlay test: linked with the firmware, one shared window, stored elsewhere in DRAM.
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
uos_add_app("scl-fix-overlay-hwd"
    SOURCES src/overlay-main.cpp "${OV_SRC}/ov1.c" "${OV_SRC}/ov2.c" "${OV_SRC}/ovcpp.cpp" ${BOARD_SUPPORT}
    NCPU 3
    LINKER_SCRIPT "${CMAKE_BINARY_DIR}/overlay.ld"
    DEFINES TRACE __ARM_EABI__ __ARM_ARCH_7A__ SEMIHOST HW_BUILD TEST_NAME="scl-fix-overlay" TEST_ITERS=2000
    INCLUDES "${BOARD_INC}" "${FIX_SRC}/include"
    LIBRARIES micro-os-plus::aarch32 scl::overlay)
set_target_properties("scl-fix-overlay-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
list(APPEND SCL_TESTS "scl-fix-overlay-hwd")

add_custom_target(scl-all DEPENDS ${SCL_TESTS})
