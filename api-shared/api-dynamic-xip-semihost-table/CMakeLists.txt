# api-dynamic-xip-semihost-table — scl::xip-dynamic-newlib with main's table read
# at boot over semihosting by collect_buffered(), with no gen-symtab step.
cmake_minimum_required(VERSION 3.20)
project(api-dynamic-xip-semihost-table LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=platform/cmake/arm-none-eabi-hosted.cmake")
endif()

set(SCL_HOSTED ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/SCL-shared scl)

set(_repo ${CMAKE_SOURCE_DIR}/platform)
set(_su   ${_repo}/test/platforms/board-hosted/vendor)
set(_test3 ${_repo}/test-flatfs)

set(SCL_PLATFORM "board" CACHE STRING "board (weact/nucleo) or qemu-f411re")
set(SCL_BOARD "weact" CACHE STRING "weact or nucleo")
if(SCL_BOARD STREQUAL "nucleo")
    set(XIP_BOARD_DEFS SCL_BOARD_NUCLEO=1 HSE_VALUE=16000000)
else()
    set(XIP_BOARD_DEFS SCL_BOARD_WEACT=1 HSE_VALUE=25000000)
endif()
include(${_repo}/test-xip/cmake/qemu-xip.cmake)         # sets XIP_INIT/XIP_LED/XIP_FS_FILL/XIP_FLATFS_LD
set(XIP_FLATFS_LD ${CMAKE_SOURCE_DIR}/src/sections-flatfs-hosted.ld)   # hardware flatfs script

# ---- board-support platform (non-PIC/hosted startup + HAL + flat-fs) --------
set(PLATFORM_SOURCES
    ${_repo}/test-xip/common/src/semihost-file.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    ${XIP_LED} ${XIP_INIT}
    ${_su}/common/firmware/src/timer.cpp
    ${_su}/common/firmware/src/stm32f4xx_hal_msp.c
    ${_su}/common/system/src/cmsis/system_stm32f4xx.c
    ${_su}/common/system/src/cmsis/vectors_stm32f411xe.c
    ${_su}/common/system/src/cortexm/exception-handlers.c
    ${_su}/common/system/src/cortexm/initialize-hardware.c
    ${_su}/common/system/src/cortexm/reset-hardware.c
    ${_su}/common/system/src/diag/trace.c
    ${_su}/common/system/src/diag/trace-impl.c
    ${_su}/common/system/src/newlib/assert.c ${_su}/common/system/src/newlib/exit.c
    ${_su}/common/system/src/newlib/sbrk.c ${_su}/common/system/src/newlib/startup.c
    ${_su}/common/system/src/newlib/syscalls.c ${_su}/common/system/src/newlib/cxx.cpp
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_flash_ramfunc.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${_su}/common/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c)
add_library(platform-xip INTERFACE)
target_sources(platform-xip INTERFACE ${PLATFORM_SOURCES})
set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")
target_include_directories(platform-xip INTERFACE
    ${_repo}/test/platforms/board/include
    ${_su}/common/firmware/include ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm ${_su}/common/system/include)
target_compile_definitions(platform-xip INTERFACE
    DEBUG OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER ${XIP_BOARD_DEFS} HARD_FLOAT OS_INCLUDE_STARTUP_GUARD_CHECKS=0)
target_link_libraries(platform-xip INTERFACE stdc++ m)

separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

# ---- the app ----------------------------------------------------------------
add_executable(api-dynamic-xip-semihost-table
    src/main.cpp
    src/uart.c
    ${_test3}/src/fs-archive.cpp ${_test3}/src/flash-prog.c
    ${CMAKE_SOURCE_DIR}/SCL-shared/src/image-memory.cpp)
target_include_directories(api-dynamic-xip-semihost-table PRIVATE
    src ${_repo}/test-xip/common/include ${_test3}/include ${CMAKE_SOURCE_DIR}/SCL-shared/include)
target_link_libraries(api-dynamic-xip-semihost-table PRIVATE
    platform-xip scl::xip-dynamic-newlib ${SCL_LIBSUPCXX} ${SCL_LIBGCC})
target_compile_definitions(api-dynamic-xip-semihost-table PRIVATE
    SCL_SO_DIR="${CMAKE_SOURCE_DIR}/fixtures/build" SYMBUF_KB=1 STRBUF_KB=1 FIRMWARE_ELF="$<TARGET_FILE:api-dynamic-xip-semihost-table>.sym.elf")
target_link_options(api-dynamic-xip-semihost-table PRIVATE
    -pie -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic LINKER:-E -T${XIP_FLATFS_LD}
    LINKER:--gc-sections LINKER:-Map=$<TARGET_FILE:api-dynamic-xip-semihost-table>.map)

# No gen-symtab step: main reads a symbol ELF over semihosting at boot. The full
# ELF's .strtab is about 90 KB, too big to buffer on a 128 KB part, so keep only
# the symbols modules import from main. The firmware image is not changed.
add_custom_command(TARGET api-dynamic-xip-semihost-table POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --strip-all --keep-symbol=app_gain
            $<TARGET_FILE:api-dynamic-xip-semihost-table> $<TARGET_FILE:api-dynamic-xip-semihost-table>.sym.elf
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:api-dynamic-xip-semihost-table> $<TARGET_FILE:api-dynamic-xip-semihost-table>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:api-dynamic-xip-semihost-table> $<TARGET_FILE:api-dynamic-xip-semihost-table>.bin
    VERBATIM)
