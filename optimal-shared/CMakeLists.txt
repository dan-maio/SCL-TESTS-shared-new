# optimal-shared — self-contained Cortex-M SCL project: load shared libraries and
# call them BY NAME, with no hand-written glue. Structured like its sibling
# SCL-shared projects: this repo root IS the SCL source library.
#
#   src/ include/            the SCL loader/patcher sources (+ our optimal glue)
#   common-elf-reader/       the ELF32 reader — git submodule at the project root
#   cmake/ tools-config.sh   toolchain + the SCL library definitions (scl-lib.cmake)
#   test/                    shared firmware scaffolding (platforms, tools, fixtures)
#   test-flatfs/             on-chip flat-fs used by the XIP test
#   test-ram/  test-xip/     the two test apps (RAM load vs flash/XIP load)
#
# Consume the loader in your own app the same way: add this repo, link `optimal`.
cmake_minimum_required(VERSION 3.20)
project(optimal-shared LANGUAGES C CXX)

# ---- the SCL loader now comes from the shared SCL-shared submodule ------------
# (was: include(cmake/scl-lib.cmake) + inline scl-loader-nopic/optimal from ${_repo}/src)
# SCL-shared provides scl::common / scl::xip-common / scl::loader-nopic / scl::optimal.
if(NOT TARGET scl-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared ${CMAKE_BINARY_DIR}/_scl-shared)
endif()

set(_repo ${CMAKE_CURRENT_SOURCE_DIR})
set(_su ${_repo}/test/platforms/board-hosted/vendor)
set(_test3 ${_repo}/test-flatfs)
set(SCL_TEST_DIR ${_repo}/test)

# the optimal glue (src/loader.cpp + include/optimal) is scl::optimal in SCL-shared;
# expose it under the target name the test apps link.
add_library(optimal ALIAS scl-optimal)

# ---- platform selection (weact / nucleo / qemu) ------------------------------
set(SCL_PLATFORM "board" CACHE STRING "board (nucleo/weact) or qemu-f411re")
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(XIP_BOARD_DEFS "")
else()
    set(SCL_BOARD "weact" CACHE STRING "nucleo or weact")
    if(SCL_BOARD STREQUAL "nucleo")
        set(XIP_BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
    else()
        set(XIP_BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
    endif()
endif()
include(${_repo}/test/xip-cmake/qemu-xip.cmake)

# ---- gen-imports: emit test/common/modules.generated.inc from the fixtures ----
file(STRINGS ${_repo}/fixtures/manifest.txt _manifest_names)
set(_sos "")
foreach(_m ${_manifest_names})
    list(APPEND _sos ${_repo}/fixtures/${_m})
endforeach()
get_filename_component(_tcbin ${CMAKE_C_COMPILER} DIRECTORY)
# Build the C++ host tool with CMake (a HOST binary — the ARM toolchain here is for
# the firmware, so we compile it with a native compiler, like common-elf-reader's
# elf2json). Done at configure time because the generated files are needed before
# the firmware compiles. Falls back to the Python twin if no host compiler is found.
set(GEN_IMPORTS ${CMAKE_BINARY_DIR}/gen-imports)
find_program(HOST_CXX NAMES c++ g++ clang++)
set(_gen_ok FALSE)
if(HOST_CXX)
    execute_process(
        COMMAND ${HOST_CXX} -std=c++20 -O2 -o ${GEN_IMPORTS} ${_repo}/tools/gen-imports.cpp
        RESULT_VARIABLE _bi)
    if(_bi EQUAL 0)
        message(STATUS "built host tool: ${GEN_IMPORTS}")
        execute_process(
            COMMAND ${GEN_IMPORTS}
                    ${_repo}/test/common/imports.generated.h
                    ${_repo}/test/common/modules.generated.inc ${_sos}
            RESULT_VARIABLE _gi)
        set(_gen_ok TRUE)
    endif()
endif()
if(NOT _gen_ok)
    execute_process(
        COMMAND python3 ${_repo}/tools/gen-imports.py
                ${_repo}/test/common/imports.generated.h
                ${_repo}/test/common/modules.generated.inc
                ${_tcbin}/arm-none-eabi-readelf ${_sos}
        RESULT_VARIABLE _gi)
endif()
if(NOT _gi EQUAL 0)
    message(FATAL_ERROR "gen-imports failed (${_gi}); run fixtures-src/build.sh first")
endif()

# Also expose a rebuildable host target: `cmake --build build --target gen-imports`.
add_custom_command(OUTPUT ${GEN_IMPORTS}
    COMMAND ${HOST_CXX} -std=c++20 -O2 -o ${GEN_IMPORTS} ${_repo}/tools/gen-imports.cpp
    DEPENDS ${_repo}/tools/gen-imports.cpp
    COMMENT "Building host tool gen-imports" VERBATIM)
add_custom_target(gen-imports DEPENDS ${GEN_IMPORTS})

# ---- shared firmware platform (vendored nucleo-f411re tree) -------------------
set(PLATFORM_SOURCES
    ${_repo}/test/xip-common/src/semihost-file.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    ${XIP_LED} ${XIP_FS_FILL}
    ${_su}/common/firmware/src/timer.cpp
    ${XIP_INIT}
    ${_su}/common/firmware/src/stm32f4xx_hal_msp.c
    ${_su}/common/system/src/cmsis/system_stm32f4xx.c
    ${_su}/common/system/src/cmsis/vectors_stm32f411xe.c
    ${_su}/common/system/src/cortexm/exception-handlers.c
    ${_su}/common/system/src/cortexm/initialize-hardware.c
    ${_su}/common/system/src/cortexm/reset-hardware.c
    ${_su}/common/system/src/diag/trace.c
    ${_su}/common/system/src/diag/trace-impl.c
    ${_su}/common/system/src/newlib/assert.c
    ${_su}/common/system/src/newlib/exit.c
    ${_su}/common/system/src/newlib/sbrk.c
    ${_su}/common/system/src/newlib/startup.c
    ${_su}/common/system/src/newlib/syscalls.c
    ${_su}/common/system/src/newlib/cxx.cpp
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
    ${_test3}/src/fs-archive.cpp
    ${_test3}/src/flash-prog.c
)
add_library(platform-optimal INTERFACE)
target_sources(platform-optimal INTERFACE ${PLATFORM_SOURCES})
target_include_directories(platform-optimal INTERFACE
    ${_repo}/test/platforms/board/include
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include
    ${_test3}/include
    ${_repo}/test/xip-common/include)
target_compile_definitions(platform-optimal INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER ${XIP_BOARD_DEFS} HARD_FLOAT
    OS_INCLUDE_STARTUP_GUARD_CHECKS=0)
target_compile_options(platform-optimal INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-include;${_repo}/test/xip-common/include/abort_fix.h>)
target_link_libraries(platform-optimal INTERFACE stdc++ m)

separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

# the shared app linker script (defines the reserved SRAM windows; single source
# of truth that fixtures-src/build.sh reads to auto-pack the fixtures)
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(OPTIMAL_APP_LD ${_repo}/test/common/ldscripts/sections-flatfs-qemu-nopic.ld)
else()
    set(OPTIMAL_APP_LD ${_repo}/test/common/ldscripts/sections-flatfs-hosted-nopic.ld)
endif()

# the by-name-linked subset (all fixtures except the EH/RTTI pair)
file(STRINGS ${_repo}/fixtures/link.txt _linkset)
set(OPTIMAL_LFLAGS "")
foreach(_m ${_linkset})
    list(APPEND OPTIMAL_LFLAGS "-l:${_m}")
endforeach()

enable_testing()
include(${_repo}/test/common/optimal-app.cmake)   # add_optimal_app(target mode provide-src)
add_subdirectory(test-ram)
add_subdirectory(test-xip)
