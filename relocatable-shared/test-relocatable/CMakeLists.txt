# scl::reloc test tree — a SEPARATE, self-contained Cortex-M4 firmware project.
# Unlike the ET_DYN test apps, this main is an ordinary NON-PIC firmware: no
# -pie, no -Wl,-E, no .dynsym/GOT/_DYNAMIC. It reuses the plain freestanding
# platform sources (startup/semihost/board/crt/libc-shim) and its OWN simple
# linker script (platform/reloc.ld).
#
#   cmake -S test-relocatable -B test-relocatable/build \
#         -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake \
#         [-DSCL_PLATFORM=qemu-f411re]
#   cmake --build test-relocatable/build        # -> scl-fw-reloc.hex (+ fixtures)
cmake_minimum_required(VERSION 3.20)
project(scl-reloc-tests LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-relocatable builds a Cortex-M4 firmware app: configure with "
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)
enable_testing()

# Platform selection: board (nucleo/weact hardware) or qemu-f411re (emulated).
set(SCL_PLATFORM "board" CACHE STRING "Target platform (board for nucleo/weact, or qemu-f411re for QEMU)")
set(PLAT ${SCL_ROOT}/test/platforms/${SCL_PLATFORM})
set(PLAT_COMMON ${SCL_ROOT}/test/platforms/common)

if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(BOARD_DEFS "")
elseif(SCL_PLATFORM STREQUAL "board")
    set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
    if(SCL_BOARD STREQUAL "nucleo")
        set(BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
    elseif(SCL_BOARD STREQUAL "weact")
        set(BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
    else()
        message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
    endif()
else()
    message(FATAL_ERROR "SCL_PLATFORM must be 'board' or 'qemu-f411re' (got '${SCL_PLATFORM}')")
endif()

add_executable(scl-fw-reloc
    src/main.cpp
    ${PLAT}/src/startup.c      # vectors + reset (copy .data, zero .bss, FPU, main)
    ${PLAT}/src/semihost.c     # trace_puts/putc + semihost_open/flen/read/close
    ${PLAT}/src/board.c        # board_init / SysTick
    ${PLAT_COMMON}/src/crt.cpp        # operator new/delete + malloc/free (static heap)
    ${PLAT}/src/libc-shim.c)   # memcpy/memset/... (freestanding, -nostdlib)

target_include_directories(scl-fw-reloc PRIVATE ${PLAT}/include)
target_compile_definitions(scl-fw-reloc PRIVATE ${BOARD_DEFS})
target_link_libraries(scl-fw-reloc PRIVATE scl::reloc)   # loader + parser; no dlopen

# Ordinary NON-PIC firmware: override the toolchain's -fpic; no -pie at link.
target_compile_options(scl-fw-reloc PRIVATE
    -fno-pic -fno-pie -Os -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

# libm (cpp3's sinf/cosf/logf/sqrtf) + libgcc (compiler helpers), by multilib path.
separate_arguments(_mcu UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_C_COMPILER} ${_mcu} -print-file-name=libm.a
                OUTPUT_VARIABLE _libm OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_C_COMPILER} ${_mcu} -print-file-name=libgcc.a
                OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)

target_link_options(scl-fw-reloc PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/platform/reloc.ld
    -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments)
target_link_libraries(scl-fw-reloc PRIVATE ${_libm} ${_libgcc})

add_custom_command(TARGET scl-fw-reloc POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-reloc> $<TARGET_FILE:scl-fw-reloc>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-reloc> $<TARGET_FILE:scl-fw-reloc>.bin)

# Build the ET_REL fixtures and point the app at them (opened over semihosting).
set(RELOC_FIX ${CMAKE_CURRENT_SOURCE_DIR}/fixtures-reloc)
get_filename_component(SCL_TCBIN ${CMAKE_C_COMPILER} DIRECTORY)
add_custom_target(reloc-fixtures
    COMMAND make -C ${RELOC_FIX} TOOLCHAIN_PATH=${SCL_TCBIN}
    COMMENT "Building ET_REL fixtures (relprov/relmod/mylib1/2/c1/c2/cpp3 .rel.o)" VERBATIM)
add_dependencies(scl-fw-reloc reloc-fixtures)
target_compile_definitions(scl-fw-reloc PRIVATE RELOC_LIB_DIR="${RELOC_FIX}/build")

# On-target run: reuse the shared flash-run.sh (OpenOCD program + semihosting),
# or run under QEMU.
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    add_test(NAME scl-fw-reloc
             COMMAND ${SCL_ROOT}/test/tools/qemu-run.sh --machine netduinoplus2
                     --done "RESULT: done" $<TARGET_FILE:scl-fw-reloc>
             WORKING_DIRECTORY ${SCL_ROOT})
else()
    add_test(NAME scl-fw-reloc
             COMMAND ${SCL_ROOT}/test/tools/flash-run.sh $<TARGET_FILE:scl-fw-reloc>.hex
             WORKING_DIRECTORY ${SCL_ROOT})
endif()
