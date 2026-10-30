# api-static-boot-table — scl::static with a compile-time table:
# SCL_REGISTER_SYM + registered() + add_entries, plus counting lock/cache hooks.
cmake_minimum_required(VERSION 3.20)
project(api-static-boot-table LANGUAGES C CXX)
# the static-loader firmware is NON-pic with its own GOT linker scripts, and its
# sources use <string_view> (hosted). Strip the toolchain's -fpic/-ffreestanding.
string(REPLACE "-fpic " "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-fpic " "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-ffreestanding " "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REPLACE "-ffreestanding " "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=platform/cmake/arm-none-eabi.cmake")
endif()

get_filename_component(SCL ${CMAKE_SOURCE_DIR}/platform ABSOLUTE)
set(SCLSH ${CMAKE_SOURCE_DIR}/SCL-shared)
add_subdirectory(${SCLSH} scl)                    # scl::static
set(ST ${SCL}/test-static/test-static)          # the static-loader firmware platform
set(FIX ${CMAKE_SOURCE_DIR}/fixtures/build)

set(SCL_BOARD "weact")
set(BOARD_DEFS SCL_BOARD_WEACT=1)

# build the fixture (.so) before the firmware links against it
get_filename_component(TCBIN ${CMAKE_C_COMPILER} DIRECTORY)
add_custom_target(floatlib-fixture
    COMMAND make -C ${CMAKE_SOURCE_DIR}/fixtures TC=${TCBIN}
    COMMENT "Building floatlib.so (plain -fPIC ET_DYN)")

add_executable(api-static-boot-table
    src/main.cpp src/uart.c
    ${ST}/system/src/cmsis/system_stm32f4xx.c
    ${ST}/system/src/cmsis/vectors_stm32f411xe.c
    ${ST}/system/src/cortexm/exception-handlers.c
    ${ST}/system/src/cortexm/initialize-hardware.c
    ${ST}/system/src/cortexm/reset-hardware.c
    ${ST}/system/src/diag/trace-impl.c ${ST}/system/src/diag/trace.c
    ${ST}/system/src/newlib/assert.c ${ST}/system/src/newlib/exit.c
    ${ST}/system/src/newlib/sbrk.c ${ST}/system/src/newlib/startup.c
    ${ST}/system/src/newlib/syscalls.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_flash_ex.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${ST}/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
    ${ST}/src/initialize-hardware.c)
add_dependencies(api-static-boot-table floatlib-fixture)

target_include_directories(api-static-boot-table PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SCLSH}/include ${SCLSH}/include/static
    ${ST}/include ${ST}/system/include ${ST}/system/include/cmsis
    ${ST}/system/include/stm32f4-hal ${ST}/system/include/cortexm ${ST}/system/include/diag)
target_compile_definitions(api-static-boot-table PRIVATE
    STM32F411xE USE_HAL_DRIVER HSE_VALUE=25000000 ${BOARD_DEFS}
    OS_USE_SEMIHOSTING DEBUG TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    DL_LIB_DIR="${FIX}")
target_compile_options(api-static-boot-table PRIVATE
    -O0 -g3 -Wall -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20 -fno-rtti -fno-exceptions>)
target_link_libraries(api-static-boot-table PRIVATE scl::static ${FIX}/floatlib.so)
target_link_options(api-static-boot-table PRIVATE
    -T${ST}/ldscripts/mem-sh.ld -T${ST}/ldscripts/libs-sh.ld -T${ST}/ldscripts/sections-sh.ld
    -nostartfiles -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs
    -Wl,--no-warn-rwx-segments -Wl,-Map=$<TARGET_FILE:api-static-boot-table>.map)
add_custom_command(TARGET api-static-boot-table POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:api-static-boot-table> $<TARGET_FILE:api-static-boot-table>.hex)
