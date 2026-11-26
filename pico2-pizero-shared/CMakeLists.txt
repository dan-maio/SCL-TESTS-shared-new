# pico2-pizero-shared — SCL on the Pi-Zero RP2350B (2x Cortex-M33, 16 MB flash, PSRAM)
# under the µOS++ III SMP kernel. Board support comes from the port repository.
#
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<kernel>/cmake/toolchains/arm-none-eabi.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)

set(UOS_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../uos/micro-os-plus-iii-cortexm"
    CACHE PATH "micro-os-plus-iii-cortexm working copy")
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${UOS_PORT_DIR}/../micro-os-plus-iii/cmake/toolchains/arm-none-eabi.cmake")
endif()

project(scl-pico2-pizero LANGUAGES C CXX ASM)

set(BOARD "pico2-pizero" CACHE STRING "" FORCE)
add_subdirectory("${UOS_PORT_DIR}" uos)

set(SCL_HOSTED ON CACHE BOOL "" FORCE)     # scl::xip-dynamic-newlib needs it
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared" scl)

# The port sets these in its own scope, so name the board's glue here.
set(UOS_PICO2 "${UOS_PORT_DIR}/test/boards/pico2")
set(UOS_BOARD_UART "${UOS_PICO2}/glue/uart.cpp")
set(UOS_BOARD_MULTICORE "${UOS_PICO2}/glue/multicore.cpp")
set(UOS_BOARD_PSRAM "${UOS_PICO2}/glue/psram.cpp")
set(UOS_BOARD_RTOS_GLUE "${UOS_PICO2}/glue/rtos-glue.cpp" "${UOS_PICO2}/glue/trace-uart.cpp"
    "${UOS_PORT_DIR}/test/boards/shared/syscalls.c")
set(UOS_BOARD_NCPU 2)

# ---- the module, in the two forms the loaders take ----------------------------
set(MOD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/smpmod.cpp")
set(MOD_LD "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/xip-split.ld")
set(MCU -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16)
set(MOD_CXX -O1 -std=c++20 -ffreestanding -fno-exceptions -fno-rtti
            -fno-threadsafe-statics -fno-use-cxa-atexit)
set(MOD_REL "${CMAKE_CURRENT_BINARY_DIR}/smpmod.rel.o")
set(MOD_SO "${CMAKE_CURRENT_BINARY_DIR}/smpmod.so")
add_custom_command(OUTPUT "${MOD_REL}"
    COMMAND ${CMAKE_CXX_COMPILER} ${MCU} ${MOD_CXX} -mlong-calls -fno-pic -r -nostdlib
            "${MOD_SRC}" -o "${MOD_REL}"
    DEPENDS "${MOD_SRC}" VERBATIM)
# -mlong-calls: imports go through the RAM GOT (r10), not a PLT reading a flash GOT.
add_custom_command(OUTPUT "${MOD_SO}"
    COMMAND ${CMAKE_CXX_COMPILER} ${MCU} ${MOD_CXX} -mlong-calls -fpic -msingle-pic-base -mpic-register=r10
            -mno-pic-data-is-text-relative -shared -nostartfiles -nostdlib -nodefaultlibs
            -Wl,-E -Wl,-Bsymbolic -T "${MOD_LD}" "${MOD_SRC}" -o "${MOD_SO}"
    DEPENDS "${MOD_SRC}" "${MOD_LD}" VERBATIM)

set(MOD_REL_BIG "${CMAKE_CURRENT_BINARY_DIR}/smpmod-big.rel.o")
set(MOD_SO_BIG "${CMAKE_CURRENT_BINARY_DIR}/smpmod-big.so")
add_custom_command(OUTPUT "${MOD_REL_BIG}"
    COMMAND ${CMAKE_CXX_COMPILER} ${MCU} ${MOD_CXX} -DSMP_BIG -mlong-calls -fno-pic -r -nostdlib
            "${MOD_SRC}" -o "${MOD_REL_BIG}"
    DEPENDS "${MOD_SRC}" VERBATIM)
add_custom_command(OUTPUT "${MOD_SO_BIG}"
    COMMAND ${CMAKE_CXX_COMPILER} ${MCU} ${MOD_CXX} -DSMP_BIG -mlong-calls -fpic -msingle-pic-base -mpic-register=r10
            -mno-pic-data-is-text-relative -shared -nostartfiles -nostdlib -nodefaultlibs
            -Wl,-E -Wl,-Bsymbolic -T "${MOD_LD}" "${MOD_SRC}" -o "${MOD_SO_BIG}"
    DEPENDS "${MOD_SRC}" "${MOD_LD}" VERBATIM)
set(MOD_ALL "${MOD_REL}" "${MOD_SO}" "${MOD_REL_BIG}" "${MOD_SO_BIG}")
add_custom_target(scl-smp-modules DEPENDS ${MOD_ALL})
set_source_files_properties(src/embed.S PROPERTIES OBJECT_DEPENDS "${MOD_ALL}")

# ---- one image per loader and memory ------------------------------------------
# name  loader-define  scl-target  module  arena-section  expected text MSB  PSRAM?
function(scl_smp_test name loader target module section top psram iters)
    set(defs TRACE SEMIHOST HW_BUILD TEST_${loader} TEST_NAME="${name}"
        TEST_EXPECT_TEXT_TOP=${top} TEST_ITERS=${iters})
    set(srcs src/main.cpp src/embed.S ${UOS_BOARD_UART} ${UOS_BOARD_RTOS_GLUE} ${UOS_BOARD_MULTICORE})
    # psram: ON = module loaded into PSRAM from an SRAM stage; DATA = data only.
    if(psram)
        list(APPEND defs TEST_PSRAM PICO2_PSRAM_COPY=1 PICO2_PSRAM_CS_PIN=47)
        list(APPEND srcs ${UOS_BOARD_PSRAM})
    endif()
    if(psram STREQUAL "ON")
        list(APPEND defs TEST_STAGE)
    endif()
    if(psram STREQUAL "DATA")
        list(APPEND defs TEST_EXPECT_DATA_TOP=0x11)
    endif()
    set(arena 32768)
    if("BIG" IN_LIST ARGN)
        list(APPEND defs TEST_BIG)
        set(arena 262144)
    endif()
    if(target STREQUAL "scl::xip-dynamic-newlib")
        list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared/src/image-memory.cpp")
    endif()
    uos_add_app("${name}-hwd"
        SOURCES ${srcs}
        NCPU ${UOS_BOARD_NCPU}
        LINKER_SCRIPT "${UOS_PORT_DIR}/test/pico2-pizero/psram-mat-test-250/psram-sram-boot.ld"
        DEFINES ${defs}
        INCLUDES "${UOS_PORT_DIR}/test/boards/shared"
        LIBRARIES micro-os-plus::cortexm ${target})
    add_dependencies("${name}-hwd" scl-smp-modules)
    target_link_options("${name}-hwd" PRIVATE -Wl,--wrap=main)
    target_compile_definitions("${name}-hwd" PRIVATE
        $<$<COMPILE_LANGUAGE:ASM>:SMPMOD_PATH="${module}" SCL_ARENA_SECTION=${section} SCL_ARENA_BYTES=${arena}>)
    set_target_properties("${name}-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    list(APPEND SCL_SMP_TESTS "${name}-hwd")
    set(SCL_SMP_TESTS "${SCL_SMP_TESTS}" PARENT_SCOPE)
endfunction()

scl_smp_test(scl-smp-reloc-sram   LOADER_RELOC scl::reloc               "${MOD_REL}" .bss.scl_arena   0x20 OFF 20000)
scl_smp_test(scl-smp-reloc-psram  LOADER_RELOC scl::reloc               "${MOD_REL}" .psram.scl_arena 0x11 ON  5000)
scl_smp_test(scl-smp-nolib-sram   LOADER_NOLIB scl::dynamic-nolib       "${MOD_SO}"  .bss.scl_arena   0x20 OFF 20000)
scl_smp_test(scl-smp-nolib-psram  LOADER_NOLIB scl::dynamic-nolib       "${MOD_SO}"  .psram.scl_arena 0x11 ON  5000)
scl_smp_test(scl-smp-xip-flash    LOADER_XIP   scl::xip-dynamic-newlib  "${MOD_SO}"  .bss.scl_arena   0x10 OFF 20000)
scl_smp_test(scl-smp-xip-flash-psram  LOADER_XIP   scl::xip-dynamic-newlib "${MOD_SO}"      .psram.scl_arena 0x10 DATA 5000)
scl_smp_test(scl-smp-reloc-psram-big  LOADER_RELOC scl::reloc              "${MOD_REL_BIG}" .psram.scl_arena 0x11 ON   200 BIG)
scl_smp_test(scl-smp-nolib-psram-big  LOADER_NOLIB scl::dynamic-nolib      "${MOD_SO_BIG}"  .psram.scl_arena 0x11 ON   200 BIG)

add_custom_target(scl-smp-all DEPENDS ${SCL_SMP_TESTS})

# ---- the WeAct fixtures, read over semihosting at boot --------------------------
set(FIX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../relocatable-shared/test/fixtures-src")
set(FIX_INC -I${FIX_SRC}/include -I${FIX_SRC}/c1 -I${FIX_SRC}/c2 -I${FIX_SRC}/cpp3 -I${FIX_SRC}/mylib1
            -I${FIX_SRC}/mylib2 -I${FIX_SRC}/libprov -I${FIX_SRC}/libcons)
set(FIX_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIX_DIR}")
set(FIX_CXX -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit)
set(FIX_FILES "")
set(WIN_sram_TEXT 0x20070000)
set(WIN_sram_DATA 0x20078000)
set(WIN_psram_TEXT 0x11100000)
set(WIN_psram_DATA 0x11180000)
set(slot 0)
foreach(pair "cpp3|cpp3/cpp3.cpp" "c2|c2/c2.c" "c1|c1/c1.c" "mylib1|../../test-relocatable/fixtures-reloc/src/mylib1.c"
             "mylib2|mylib2/mylib2.c" "libprov|libprov/libprov.c" "libcons|libcons/libcons.c"
             "slib|../../../static-shared/test-xip/test-xip-interrupt-static/fixtures-src/slib.c")
    string(REPLACE "|" ";" p "${pair}")
    list(GET p 0 n)
    list(GET p 1 s)
    set(src "${FIX_SRC}/${s}")
    if(s MATCHES "cpp$")
        set(lang ${FIX_CXX})
    else()
        set(lang -std=gnu11)
    endif()
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.rel.o"
        COMMAND ${CMAKE_CXX_COMPILER} -x $<IF:$<STREQUAL:${n},cpp3>,c++,c> ${MCU} -mlong-calls -fno-pic -Os -ffreestanding
                -fno-common ${lang} ${FIX_INC} -c "${src}" -o "${FIX_DIR}/${n}.tmp.o"
        COMMAND ${CMAKE_LINKER} -r "${FIX_DIR}/${n}.tmp.o" -o "${FIX_DIR}/${n}.rel.o"
        DEPENDS "${src}" VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.so"
        COMMAND ${CMAKE_CXX_COMPILER} -x $<IF:$<STREQUAL:${n},cpp3>,c++,c> ${MCU} -fpic -msingle-pic-base -mpic-register=r10
                -mno-pic-data-is-text-relative -O0 -ffunction-sections -fdata-sections
                ${lang} ${FIX_INC} -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-Bsymbolic
                -T "${CMAKE_CURRENT_SOURCE_DIR}/../dynamic-shared/test-xip/fixtures-src/ldscripts/xip-split.ld"
                "${src}" -o "${FIX_DIR}/${n}.so"
        DEPENDS "${src}" VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.pic.so"
        COMMAND ${CMAKE_CXX_COMPILER} -x $<IF:$<STREQUAL:${n},cpp3>,c++,c> ${MCU} -fPIC -O0
                -ffunction-sections -fdata-sections ${lang} ${FIX_INC} -shared -nostartfiles
                -nostdlib -nodefaultlibs -Wl,-E "${src}" -o "${FIX_DIR}/${n}.pic.so"
        DEPENDS "${src}" VERBATIM)
    list(APPEND FIX_FILES "${FIX_DIR}/${n}.rel.o" "${FIX_DIR}/${n}.so" "${FIX_DIR}/${n}.pic.so")
    # scl::optimal: -fno-pic, linked at this fixture's own slot in each window.
    foreach(mem IN ITEMS sram psram)
        math(EXPR tb "${WIN_${mem}_TEXT} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR db "${WIN_${mem}_DATA} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        set(out "${FIX_DIR}/${n}.opt-${mem}.so")
        add_custom_command(OUTPUT "${out}"
            COMMAND sed -e "s/__TEXT_BASE__/${tb}/g" -e "s/__DATA_BASE__/${db}/g"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../optimal-shared/fixtures-src/ldscripts/nopic-split.ld"
                    > "${out}.ld"
            COMMAND ${CMAKE_CXX_COMPILER} -x $<IF:$<STREQUAL:${n},cpp3>,c++,c> ${MCU} -fno-pic -mlong-calls -O0
                    -ffunction-sections -fdata-sections ${lang} ${FIX_INC} -shared -nostartfiles -nostdlib
                    -nodefaultlibs -Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all
                    -T "${out}.ld" "${src}" -o "${out}"
            DEPENDS "${src}")
        list(APPEND FIX_FILES "${out}")
    endforeach()
    math(EXPR slot "${slot} + 1")
endforeach()
# The C++ exception and virtual-class fixtures, built with exceptions and RTTI on.
set(EH_INC -I${FIX_SRC}/libcppeh/include -I${FIX_SRC}/libcppsh/include)
set(EH_CXX -std=c++20 -fno-threadsafe-statics)
set(slot 0)
foreach(pair "libcppeh|libcppeh/src/cppeh.cpp" "ehx_catch|ehx/ehx-catch.cpp"
             "libcppsh|libcppsh/src/cppsh-api.cpp;libcppsh/src/widget.cpp")
    string(FIND "${pair}" "|" bar)
    string(SUBSTRING "${pair}" 0 ${bar} n)
    math(EXPR bar "${bar} + 1")
    string(SUBSTRING "${pair}" ${bar} -1 rel)
    set(srcs "")
    foreach(r IN LISTS rel)
        list(APPEND srcs "${FIX_SRC}/${r}")
    endforeach()
    set(objs "")
    foreach(s IN LISTS srcs)
        get_filename_component(b "${s}" NAME_WE)
        set(o "${FIX_DIR}/eh-${n}-${b}.o")
        add_custom_command(OUTPUT "${o}"
            COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -mlong-calls -fno-pic -Os ${EH_CXX} ${EH_INC} -c "${s}" -o "${o}"
            DEPENDS "${s}" VERBATIM)
        list(APPEND objs "${o}")
    endforeach()
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.rel.o"
        COMMAND ${CMAKE_LINKER} -r ${objs} -o "${FIX_DIR}/${n}.rel.o"
        DEPENDS ${objs} VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fpic -msingle-pic-base -mpic-register=r10
                -mno-pic-data-is-text-relative -O0 ${EH_CXX} ${EH_INC} -shared -nostartfiles -nostdlib
                -nodefaultlibs -Wl,-E -Wl,-Bsymbolic
                -T "${CMAKE_CURRENT_SOURCE_DIR}/../dynamic-shared/test-xip/fixtures-src/ldscripts/xip-split.ld"
                ${srcs} -o "${FIX_DIR}/${n}.so"
        DEPENDS ${srcs} VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.pic.so"
        COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fPIC -O0 ${EH_CXX} ${EH_INC} -shared -nostartfiles -nostdlib
                -nodefaultlibs -Wl,-E ${srcs} -o "${FIX_DIR}/${n}.pic.so"
        DEPENDS ${srcs} VERBATIM)
    list(APPEND FIX_FILES "${FIX_DIR}/${n}.rel.o" "${FIX_DIR}/${n}.so" "${FIX_DIR}/${n}.pic.so")
    foreach(mem IN ITEMS sram psram)
        math(EXPR tb "${WIN_${mem}_TEXT} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR db "${WIN_${mem}_DATA} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        set(out "${FIX_DIR}/${n}.opt-${mem}.so")
        add_custom_command(OUTPUT "${out}"
            COMMAND sed -e "s/__TEXT_BASE__/${tb}/g" -e "s/__DATA_BASE__/${db}/g"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../optimal-shared/fixtures-src/ldscripts/nopic-split.ld"
                    > "${out}.ld"
            COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -fno-pic -mlong-calls -O0 ${EH_CXX} ${EH_INC} -shared
                    -nostartfiles -nostdlib -nodefaultlibs -Wl,-E -Wl,-z,max-page-size=0x100
                    -Wl,--unresolved-symbols=ignore-all -T "${out}.ld" ${srcs} -o "${out}"
            DEPENDS ${srcs})
        list(APPEND FIX_FILES "${out}")
    endforeach()
    math(EXPR slot "${slot} + 1")
endforeach()
# The RP2350 driver fixtures: the same forms, from this suite's own sources.
set(slot 0)
foreach(pair "libled|led" "libuart|uart" "libspi|spi")
    string(REPLACE "|" ";" p "${pair}")
    list(GET p 0 n)
    list(GET p 1 s)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rp2350/${s}.c")
    set(C_OPTS -std=gnu11 -ffreestanding -fno-common)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.rel.o"
        COMMAND ${CMAKE_C_COMPILER} ${MCU} -mlong-calls -fno-pic -Os ${C_OPTS} -c "${src}" -o "${FIX_DIR}/${n}.tmp.o"
        COMMAND ${CMAKE_LINKER} -r "${FIX_DIR}/${n}.tmp.o" -o "${FIX_DIR}/${n}.rel.o"
        DEPENDS "${src}" VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.so"
        COMMAND ${CMAKE_C_COMPILER} ${MCU} -fpic -msingle-pic-base -mpic-register=r10
                -mno-pic-data-is-text-relative -O0 ${C_OPTS} -shared -nostartfiles -nostdlib -nodefaultlibs
                -Wl,-E -Wl,-Bsymbolic
                -T "${CMAKE_CURRENT_SOURCE_DIR}/../dynamic-shared/test-xip/fixtures-src/ldscripts/xip-split.ld"
                "${src}" -o "${FIX_DIR}/${n}.so"
        DEPENDS "${src}" VERBATIM)
    add_custom_command(OUTPUT "${FIX_DIR}/${n}.pic.so"
        COMMAND ${CMAKE_C_COMPILER} ${MCU} -fPIC -O0 ${C_OPTS} -shared -nostartfiles -nostdlib -nodefaultlibs
                -Wl,-E "${src}" -o "${FIX_DIR}/${n}.pic.so"
        DEPENDS "${src}" VERBATIM)
    list(APPEND FIX_FILES "${FIX_DIR}/${n}.rel.o" "${FIX_DIR}/${n}.so" "${FIX_DIR}/${n}.pic.so")
    foreach(mem IN ITEMS sram psram)
        math(EXPR tb "${WIN_${mem}_TEXT} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        math(EXPR db "${WIN_${mem}_DATA} + ${slot} * 0x1000" OUTPUT_FORMAT HEXADECIMAL)
        set(out "${FIX_DIR}/${n}.opt-${mem}.so")
        add_custom_command(OUTPUT "${out}"
            COMMAND sed -e "s/__TEXT_BASE__/${tb}/g" -e "s/__DATA_BASE__/${db}/g"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../optimal-shared/fixtures-src/ldscripts/nopic-split.ld"
                    > "${out}.ld"
            COMMAND ${CMAKE_C_COMPILER} ${MCU} -fno-pic -mlong-calls -O0 ${C_OPTS} -shared -nostartfiles -nostdlib
                    -nodefaultlibs -Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all
                    -T "${out}.ld" "${src}" -o "${out}"
            DEPENDS "${src}")
        list(APPEND FIX_FILES "${out}")
    endforeach()
    math(EXPR slot "${slot} + 1")
endforeach()
add_custom_target(scl-fix-modules DEPENDS ${FIX_FILES})

# Exceptions need main's unwind table sorted, and ld only sorts it as its own output
# section. The board's script folds it into .text, so these tests get a derived script.
file(READ "${UOS_PORT_DIR}/test/pico2-pizero/psram-mat-test-250/psram-sram-boot.ld" BOOT_LD)
set(EH_LD "${BOOT_LD}")
string(REGEX REPLACE "[ \t]*__exidx_start = \\.;[^\n]*\n" "" EH_LD "${EH_LD}")
string(REGEX REPLACE "[ \t]*\\*\\(\\.ARM\\.exidx[^\n]*\n" "" EH_LD "${EH_LD}")
string(REGEX REPLACE "[ \t]*__exidx_end = \\.;[^\n]*\n" "" EH_LD "${EH_LD}")
string(REGEX REPLACE "([ \t]*\\.preinit_array : ALIGN)"
"    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

\\1" EH_LD "${EH_LD}")
file(WRITE "${CMAKE_BINARY_DIR}/eh-boot.ld" "${EH_LD}")

# Only the test's own source catches exceptions; the kernel and board stay as built.
set_source_files_properties(src/fixtures-main.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions;-frtti")

# name  loader-define  scl-target  extension  arena-section  text MSB  psram  iterations
function(scl_fix_test name loader target ext section top psram iters)
    if(target STREQUAL "scl::xip-static")
        set(xip_variant TEST_XIP_STATIC)
    elseif(target STREQUAL "scl::xip-dynamic-nolib")
        set(xip_variant TEST_XIP_NOLIB)
    else()
        set(xip_variant "")
    endif()
    set(defs ${xip_variant} TRACE SEMIHOST HW_BUILD TEST_${loader} TEST_NAME="${name}" TEST_EXPECT_TEXT_TOP=${top}
        TEST_ITERS=${iters} FIXTURE_DIR="${FIX_DIR}" FIXTURE_EXT="${ext}")
    set(group 1)
    if("GROUP2" IN_LIST ARGN)
        set(group 2)
    endif()
    if("GROUP3" IN_LIST ARGN)
        set(group 3)
    endif()
    if("GROUP5" IN_LIST ARGN)
        set(group 5)
    endif()
    if("GROUP6" IN_LIST ARGN)
        set(group 6)
    endif()
    if("HIDE_HOST_F" IN_LIST ARGN)
        list(APPEND defs TEST_HIDE_HOST_F TEST_EXPECT_MISSING="host_f")
    endif()
    if("GROUP4" IN_LIST ARGN)
        set(group 4)
        if("THROW" IN_LIST ARGN)
            list(APPEND defs TEST_EH_THROW)
        endif()
    endif()
    list(APPEND defs TEST_GROUP=${group})
    set(srcs src/fixtures-main.cpp src/embed-arena.S ${UOS_BOARD_UART} ${UOS_BOARD_RTOS_GLUE} ${UOS_BOARD_MULTICORE})
    if(psram)
        list(APPEND defs TEST_PSRAM PICO2_PSRAM_COPY=1 PICO2_PSRAM_CS_PIN=47)
        list(APPEND srcs ${UOS_BOARD_PSRAM})
    endif()
    if(psram STREQUAL "DATA")
        list(APPEND defs TEST_EXPECT_DATA_TOP=0x11)
    endif()
    if(loader STREQUAL "LOADER_NOLIB" OR loader STREQUAL "LOADER_XIP")
        list(APPEND defs TEST_CROSS_CALL_LOOKUP)
    endif()
    if(target STREQUAL "scl::xip-dynamic-newlib")
        list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared/src/image-memory.cpp")
    endif()
    set(ld "${UOS_PORT_DIR}/test/pico2-pizero/psram-mat-test-250/psram-sram-boot.ld")
    if(group EQUAL 4)
        set(ld "${CMAKE_BINARY_DIR}/eh-boot.ld")
    endif()
    uos_add_app("${name}-hwd"
        SOURCES ${srcs}
        NCPU ${UOS_BOARD_NCPU}
        LINKER_SCRIPT "${ld}"
        DEFINES ${defs}
        INCLUDES "${UOS_PORT_DIR}/test/boards/shared"
        LIBRARIES micro-os-plus::cortexm ${target})
    add_dependencies("${name}-hwd" scl-fix-modules)
    if(group EQUAL 4)
        # nano.specs selects libstdc++_nano, whose __cxa_throw has no unwind tables, so
        # a throw stops at its first frame. The full libsupc++ has them.
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${MCU} -print-file-name=libsupc++.a
                        OUTPUT_VARIABLE libsupcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
        target_link_libraries("${name}-hwd" PRIVATE "${libsupcxx}")
        target_include_directories("${name}-hwd" PRIVATE "${FIX_SRC}/libcppeh/include")
        target_compile_definitions("${name}-hwd" PRIVATE $<$<COMPILE_LANGUAGE:ASM>:TEST_EH_UNWIND>)
    endif()
    if(loader STREQUAL "LOADER_OPTIMAL")
        if(psram)
            set(wt ${WIN_psram_TEXT})
            set(wd ${WIN_psram_DATA})
        else()
            set(wt ${WIN_sram_TEXT})
            set(wd ${WIN_sram_DATA})
        endif()
        target_compile_definitions("${name}-hwd" PRIVATE
            $<$<COMPILE_LANGUAGE:ASM>:SCL_WINDOW_TEXT=${wt} SCL_WINDOW_DATA=${wd}>)
    endif()
    target_link_options("${name}-hwd" PRIVATE -Wl,--wrap=main)
    target_compile_definitions("${name}-hwd" PRIVATE
        $<$<COMPILE_LANGUAGE:ASM>:SCL_ARENA_SECTION=${section} SCL_ARENA_BYTES=98304>)
    set_target_properties("${name}-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    list(APPEND SCL_FIX_TESTS "${name}-hwd")
    set(SCL_FIX_TESTS "${SCL_FIX_TESTS}" PARENT_SCOPE)
endfunction()

scl_fix_test(scl-fix-reloc-sram      LOADER_RELOC scl::reloc              .rel.o .bss.scl_arena   0x20 OFF  2000)
scl_fix_test(scl-fix-reloc-psram     LOADER_RELOC scl::reloc              .rel.o .psram.scl_arena 0x11 ON   500)
scl_fix_test(scl-fix-nolib-sram      LOADER_NOLIB scl::dynamic-nolib      .so    .bss.scl_arena   0x20 OFF  2000)
scl_fix_test(scl-fix-nolib-psram     LOADER_NOLIB scl::dynamic-nolib      .so    .psram.scl_arena 0x11 ON   500)
scl_fix_test(scl-fix-xip-flash       LOADER_XIP   scl::xip-dynamic-newlib .so    .bss.scl_arena   0x10 OFF  2000)
scl_fix_test(scl-fix-xip-flash-psram LOADER_XIP   scl::xip-dynamic-newlib .so    .psram.scl_arena 0x10 DATA 500)
scl_fix_test(scl-fix2-reloc-sram      LOADER_RELOC scl::reloc              .rel.o .bss.scl_arena   0x20 OFF  5000 GROUP2)
scl_fix_test(scl-fix2-reloc-psram     LOADER_RELOC scl::reloc              .rel.o .psram.scl_arena 0x11 ON   1000 GROUP2)
scl_fix_test(scl-fix2-nolib-sram      LOADER_NOLIB scl::dynamic-nolib      .so    .bss.scl_arena   0x20 OFF  5000 GROUP2)
scl_fix_test(scl-fix2-nolib-psram     LOADER_NOLIB scl::dynamic-nolib      .so    .psram.scl_arena 0x11 ON   1000 GROUP2)
scl_fix_test(scl-fix2-xip-flash       LOADER_XIP   scl::xip-dynamic-newlib .so    .bss.scl_arena   0x10 OFF  2000 GROUP2)
scl_fix_test(scl-fix2-xip-flash-psram LOADER_XIP   scl::xip-dynamic-newlib .so    .psram.scl_arena 0x10 DATA 500  GROUP2)
scl_fix_test(scl-fix-newlib-sram      LOADER_NEWLIB scl::dynamic-newlib     .pic.so .bss.scl_arena  0x20 OFF 2000)
scl_fix_test(scl-fix2-newlib-sram     LOADER_NEWLIB scl::dynamic-newlib     .pic.so .bss.scl_arena  0x20 OFF 5000 GROUP2)
scl_fix_test(scl-fix3-static-sram     LOADER_STATIC scl::static             .pic.so .bss.scl_arena   0x20 OFF 5000 GROUP3)
scl_fix_test(scl-fix3-static-psram    LOADER_STATIC scl::static             .pic.so .psram.scl_arena 0x11 ON  1000 GROUP3)
scl_fix_test(scl-fix-optimal-sram     LOADER_OPTIMAL scl::optimal .opt-sram.so  .bss.scl_arena 0x20 OFF 2000)
scl_fix_test(scl-fix-optimal-psram    LOADER_OPTIMAL scl::optimal .opt-psram.so .bss.scl_arena 0x11 ON  500)
scl_fix_test(scl-fix2-optimal-sram    LOADER_OPTIMAL scl::optimal .opt-sram.so  .bss.scl_arena 0x20 OFF 5000 GROUP2)
scl_fix_test(scl-fix2-optimal-psram   LOADER_OPTIMAL scl::optimal .opt-psram.so .bss.scl_arena 0x11 ON  1000 GROUP2)
scl_fix_test(scl-fix-xipstatic-flash       LOADER_XIP scl::xip-static         .so .bss.scl_arena   0x10 OFF  2000)
scl_fix_test(scl-fix-xipstatic-flash-psram LOADER_XIP scl::xip-static         .so .psram.scl_arena 0x10 DATA 500)
scl_fix_test(scl-fix2-xipstatic-flash       LOADER_XIP scl::xip-static        .so .bss.scl_arena   0x10 OFF  2000 GROUP2)
scl_fix_test(scl-fix2-xipstatic-flash-psram LOADER_XIP scl::xip-static        .so .psram.scl_arena 0x10 DATA 500  GROUP2)
scl_fix_test(scl-fix-xipnolib-flash        LOADER_XIP scl::xip-dynamic-nolib  .so .bss.scl_arena   0x10 OFF  2000)
scl_fix_test(scl-fix-xipnolib-flash-psram  LOADER_XIP scl::xip-dynamic-nolib  .so .psram.scl_arena 0x10 DATA 500)
scl_fix_test(scl-fix2-xipnolib-flash        LOADER_XIP scl::xip-dynamic-nolib .so .bss.scl_arena   0x10 OFF  2000 GROUP2)
scl_fix_test(scl-fix2-xipnolib-flash-psram  LOADER_XIP scl::xip-dynamic-nolib .so .psram.scl_arena 0x10 DATA 500  GROUP2)
scl_fix_test(scl-fix4-nolib-sram      LOADER_NOLIB scl::dynamic-nolib       .so .bss.scl_arena   0x20 OFF  500 GROUP4 THROW)
scl_fix_test(scl-fix4-nolib-psram     LOADER_NOLIB scl::dynamic-nolib       .so .psram.scl_arena 0x11 ON   100 GROUP4 THROW)
scl_fix_test(scl-fix4-xip-flash       LOADER_XIP   scl::xip-dynamic-newlib  .so .bss.scl_arena   0x10 OFF  200 GROUP4 THROW)
scl_fix_test(scl-fix4-xipstatic-flash LOADER_XIP   scl::xip-static          .so .bss.scl_arena   0x10 OFF  200 GROUP4 THROW)
scl_fix_test(scl-fix4-xipnolib-flash  LOADER_XIP   scl::xip-dynamic-nolib   .so .bss.scl_arena   0x10 OFF  200 GROUP4 THROW)
scl_fix_test(scl-fix4-newlib-sram     LOADER_NEWLIB scl::dynamic-newlib     .pic.so .bss.scl_arena 0x20 OFF 500 GROUP4 THROW)
scl_fix_test(scl-fix4-static-sram     LOADER_STATIC scl::static             .pic.so .bss.scl_arena 0x20 OFF 500 GROUP4 THROW)
scl_fix_test(scl-fix4-reloc-sram      LOADER_RELOC scl::reloc               .rel.o .bss.scl_arena  0x20 OFF 500 GROUP4 THROW)
scl_fix_test(scl-fix4-optimal-sram    LOADER_OPTIMAL scl::optimal           .opt-sram.so .bss.scl_arena 0x20 OFF 500 GROUP4 THROW)
scl_fix_test(scl-fix5-reloc-sram      LOADER_RELOC scl::reloc               .rel.o .bss.scl_arena    0x20 OFF 30000 GROUP5)
scl_fix_test(scl-fix5-reloc-psram     LOADER_RELOC scl::reloc               .rel.o .psram.scl_arena  0x11 ON  10000 GROUP5)
scl_fix_test(scl-fix5-nolib-sram      LOADER_NOLIB scl::dynamic-nolib       .so .bss.scl_arena       0x20 OFF 20000 GROUP5)
scl_fix_test(scl-fix5-nolib-psram     LOADER_NOLIB scl::dynamic-nolib       .so .psram.scl_arena     0x11 ON  5000  GROUP5)
scl_fix_test(scl-fix5-newlib-sram     LOADER_NEWLIB scl::dynamic-newlib     .pic.so .bss.scl_arena   0x20 OFF 30000 GROUP5)
scl_fix_test(scl-fix5-static-sram     LOADER_STATIC scl::static             .pic.so .bss.scl_arena   0x20 OFF 30000 GROUP5)
scl_fix_test(scl-fix5-static-psram    LOADER_STATIC scl::static             .pic.so .psram.scl_arena 0x11 ON  10000 GROUP5)
scl_fix_test(scl-fix5-optimal-sram    LOADER_OPTIMAL scl::optimal           .opt-sram.so .bss.scl_arena 0x20 OFF 30000 GROUP5)
scl_fix_test(scl-fix5-xip-flash       LOADER_XIP   scl::xip-dynamic-newlib  .so .bss.scl_arena       0x10 OFF 5000  GROUP5)
scl_fix_test(scl-fix5-xipstatic-flash LOADER_XIP   scl::xip-static          .so .bss.scl_arena       0x10 OFF 5000  GROUP5)
scl_fix_test(scl-fix5-xipnolib-flash  LOADER_XIP   scl::xip-dynamic-nolib   .so .bss.scl_arena       0x10 OFF 5000  GROUP5)
scl_fix_test(scl-fix6-reloc-sram      LOADER_RELOC scl::reloc               .rel.o .bss.scl_arena    0x20 OFF 2000 GROUP6)
scl_fix_test(scl-fix6-reloc-psram     LOADER_RELOC scl::reloc               .rel.o .psram.scl_arena  0x11 ON  500  GROUP6)
scl_fix_test(scl-fix6-nolib-sram      LOADER_NOLIB scl::dynamic-nolib       .so .bss.scl_arena       0x20 OFF 2000 GROUP6)
scl_fix_test(scl-fix6-nolib-psram     LOADER_NOLIB scl::dynamic-nolib       .so .psram.scl_arena     0x11 ON  500  GROUP6)
scl_fix_test(scl-fix6-newlib-sram     LOADER_NEWLIB scl::dynamic-newlib     .pic.so .bss.scl_arena   0x20 OFF 2000 GROUP6)
scl_fix_test(scl-fix6-static-sram     LOADER_STATIC scl::static             .pic.so .bss.scl_arena   0x20 OFF 2000 GROUP6)
scl_fix_test(scl-fix6-static-psram    LOADER_STATIC scl::static             .pic.so .psram.scl_arena 0x11 ON  500  GROUP6)
scl_fix_test(scl-fix6-optimal-sram    LOADER_OPTIMAL scl::optimal           .opt-sram.so .bss.scl_arena 0x20 OFF 2000 GROUP6)
scl_fix_test(scl-fix6-xip-flash       LOADER_XIP   scl::xip-dynamic-newlib  .so .bss.scl_arena       0x10 OFF 500  GROUP6)
scl_fix_test(scl-fix6-xipstatic-flash LOADER_XIP   scl::xip-static          .so .bss.scl_arena       0x10 OFF 500  GROUP6)
scl_fix_test(scl-fix6-xipnolib-flash  LOADER_XIP   scl::xip-dynamic-nolib   .so .bss.scl_arena       0x10 OFF 500  GROUP6)

# Negative tests: main's table hides host_f, and it must be reported by name.
scl_fix_test(scl-neg-reloc     LOADER_RELOC scl::reloc              .rel.o .bss.scl_arena 0x20 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-nolib     LOADER_NOLIB scl::dynamic-nolib      .so .bss.scl_arena    0x20 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-newlib    LOADER_NEWLIB scl::dynamic-newlib    .pic.so .bss.scl_arena 0x20 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-static    LOADER_STATIC scl::static            .pic.so .bss.scl_arena 0x20 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-optimal   LOADER_OPTIMAL scl::optimal          .opt-sram.so .bss.scl_arena 0x20 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-xip       LOADER_XIP scl::xip-dynamic-newlib   .so .bss.scl_arena    0x10 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-xipstatic LOADER_XIP scl::xip-static           .so .bss.scl_arena    0x10 OFF 1 GROUP5 HIDE_HOST_F)
scl_fix_test(scl-neg-xipnolib  LOADER_XIP scl::xip-dynamic-nolib    .so .bss.scl_arena    0x10 OFF 1 GROUP5 HIDE_HOST_F)

# ---- the WeAct overlay fixtures: linked with the firmware, one shared window -------
set(OV_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../overlay-shared/test-overlay/test1/src")
set_source_files_properties("${OV_SRC}/ov1.c" "${OV_SRC}/ov2.c" PROPERTIES COMPILE_OPTIONS "-fno-pic;-mlong-calls")
set_source_files_properties("${OV_SRC}/ovcpp.cpp" PROPERTIES COMPILE_OPTIONS
    "-fno-pic;-mlong-calls;-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit;-fno-unwind-tables;-fno-asynchronous-unwind-tables")
function(scl_overlay_test name origin psram iters)
    set(ld "${BOOT_LD}")
    string(REGEX REPLACE "(PSRAM \\(rwx\\)[^\n]*\n)" "\\1    OVL   (rwx) : ORIGIN = ${origin}, LENGTH = 16K\n" ld "${ld}")
    string(REGEX REPLACE "(    \\.psram \\(NOLOAD\\))"
"    OVERLAY : NOCROSSREFS
    {
        .ov1   { KEEP(*(.ov1.text))   KEEP(*(.ov1.rodata))   KEEP(*(.ov1.data)) }
        .ov2   { KEEP(*(.ov2.text))   KEEP(*(.ov2.rodata))   KEEP(*(.ov2.data)) }
        .ovcpp { KEEP(*(.ovcpp.text)) KEEP(*(.ovcpp.rodata)) KEEP(*(.ovcpp.data)) }
    } >OVL AT>FLASH
    _ov1_lma = LOADADDR(.ov1);     _ov1_size = SIZEOF(.ov1);
    _ov2_lma = LOADADDR(.ov2);     _ov2_size = SIZEOF(.ov2);
    _ovcpp_lma = LOADADDR(.ovcpp); _ovcpp_size = SIZEOF(.ovcpp);
    _ovl_vma = ADDR(.ov1);

\\1" ld "${ld}")
    set(ld_file "${CMAKE_BINARY_DIR}/${name}.ld")
    file(WRITE "${ld_file}" "${ld}")
    set(defs TRACE SEMIHOST HW_BUILD TEST_NAME="${name}" TEST_ITERS=${iters})
    set(srcs src/overlay-main.cpp "${OV_SRC}/ov1.c" "${OV_SRC}/ov2.c" "${OV_SRC}/ovcpp.cpp"
        ${UOS_BOARD_UART} ${UOS_BOARD_RTOS_GLUE} ${UOS_BOARD_MULTICORE})
    if(psram)
        list(APPEND defs TEST_PSRAM PICO2_PSRAM_COPY=1 PICO2_PSRAM_CS_PIN=47)
        list(APPEND srcs ${UOS_BOARD_PSRAM})
    endif()
    uos_add_app("${name}-hwd"
        SOURCES ${srcs}
        NCPU ${UOS_BOARD_NCPU}
        LINKER_SCRIPT "${ld_file}"
        DEFINES ${defs}
        INCLUDES "${UOS_PORT_DIR}/test/boards/shared" "${FIX_SRC}/include"
        LIBRARIES micro-os-plus::cortexm scl::overlay)
    target_link_options("${name}-hwd" PRIVATE -Wl,--wrap=main)
    set_target_properties("${name}-hwd" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    list(APPEND SCL_FIX_TESTS "${name}-hwd")
    set(SCL_FIX_TESTS "${SCL_FIX_TESTS}" PARENT_SCOPE)
endfunction()
scl_overlay_test(scl-fix-overlay-sram  0x20070000 OFF 5000)
scl_overlay_test(scl-fix-overlay-psram 0x11100000 ON  1000)

add_custom_target(scl-fix-all DEPENDS ${SCL_FIX_TESTS})
