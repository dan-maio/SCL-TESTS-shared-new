# pico2-pizero-shared — SCL on the Pi-Zero RP2350B (2x Cortex-M33, 16 MB flash, PSRAM)
# under the µOS++ III SMP kernel. Board support comes from the port repository.
#
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<kernel>/cmake/toolchains/arm-none-eabi.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)

set(UOS_PORT_DIR "/home/dan/Desktop/micro-os-plu-iii-smp-pr-github/micro-os-plus-iii-cortexm"
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
