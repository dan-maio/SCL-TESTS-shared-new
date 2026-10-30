# test-xip-dynamic-nolib/test1 — simple XIP dynamic (freestanding dlopen) demo.
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    project(scl-fw-xip-nolib1 LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

get_filename_component(_repo ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(_test3 ${_repo}/test-flatfs)          # flatfs (fs-archive/flash-prog)
set(SCL_TEST_DIR ${_repo}/test)

set(SCL_PLATFORM "board" CACHE STRING "board | qemu-f411re")
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(XIP_BOARD_DEFS "")
else()
    set(SCL_BOARD "weact" CACHE STRING "weact or nucleo")
    if(SCL_BOARD STREQUAL "nucleo")
        set(XIP_BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
    elseif(SCL_BOARD STREQUAL "weact")
        set(XIP_BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
    else()
        message(FATAL_ERROR "SCL_BOARD must be 'nucleo' or 'weact' (got '${SCL_BOARD}')")
    endif()
endif()

set(_su ${_repo}/test/platforms/board-hosted/vendor)
include(${_repo}/test-xip/cmake/qemu-xip.cmake)

set(PLATFORM_SOURCES
    ${_repo}/test-xip/common/src/semihost-file.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    ${XIP_LED}
    ${XIP_FS_FILL}
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
)

add_library(platform-board-hosted-xip-nolib INTERFACE)
target_sources(platform-board-hosted-xip-nolib INTERFACE ${PLATFORM_SOURCES})
set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")
target_include_directories(platform-board-hosted-xip-nolib INTERFACE
    ${_repo}/test/platforms/board/include
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include
)
target_compile_definitions(platform-board-hosted-xip-nolib INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER ${XIP_BOARD_DEFS}
    HARD_FLOAT
    OS_INCLUDE_STARTUP_GUARD_CHECKS=0
)
target_compile_options(platform-board-hosted-xip-nolib INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-include;${_repo}/test-xip/common/include/abort_fix.h>
)
target_link_libraries(platform-board-hosted-xip-nolib INTERFACE stdc++ m)

separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

add_executable(scl-fw-xip-nolib1
    src/main.cpp
    ${_test3}/src/fs-archive.cpp
    ${_test3}/src/flash-prog.c
)
target_include_directories(scl-fw-xip-nolib1 PRIVATE
    ${_repo}/test-xip/common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${_test3}/include
    ${SCL_ROOT}/include/scl
    ${_repo}/include
)
target_link_libraries(scl-fw-xip-nolib1 PRIVATE
    platform-board-hosted-xip-nolib
    scl::xip-dynamic-nolib
    ${SCL_LIBSUPCXX}
    ${SCL_LIBGCC}
)
target_compile_definitions(scl-fw-xip-nolib1 PRIVATE
    DL_LIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures"
)

# XIP-split fixtures (test-xip/fixtures) are built by the fixtures-src scripts;
# the app reads them over semihosting from DL_LIB_DIR. No file(COPY) needed.

target_link_options(scl-fw-xip-nolib1 PRIVATE
    -pie
    -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic
    LINKER:-E
    -T${XIP_FLATFS_LD}
    LINKER:--gc-sections
    LINKER:-Map=$<TARGET_FILE:scl-fw-xip-nolib1>.map
)

add_custom_command(TARGET scl-fw-xip-nolib1 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-xip-nolib1> $<TARGET_FILE:scl-fw-xip-nolib1>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-xip-nolib1> $<TARGET_FILE:scl-fw-xip-nolib1>.bin
)

if(XIP_RUNNER)
    add_test(NAME scl-fw-xip-nolib1
             COMMAND ${XIP_RUNNER} --machine ${XIP_MACHINE}
                     --done "RESULT: done" $<TARGET_FILE:scl-fw-xip-nolib1>
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
else()
    add_test(NAME scl-fw-xip-nolib1
             COMMAND ${SCL_TEST_DIR}/tools/flash-run.sh $<TARGET_FILE:scl-fw-xip-nolib1>.hex
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
endif()
