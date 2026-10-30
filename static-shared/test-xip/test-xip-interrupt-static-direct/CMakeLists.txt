cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-xip-interrupt-static-direct LANGUAGES C CXX)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

get_filename_component(_repo ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(_test3 ${_repo}/test-flatfs)
set(SCL_TEST_DIR ${_repo}/test)

# Board boot/init switch + HSE value (see test/platforms/board/include/board.h).
set(SCL_PLATFORM "board" CACHE STRING "Target platform (board for nucleo/weact, or qemu-f411re for QEMU)")
if(SCL_PLATFORM STREQUAL "qemu-f411re")
    set(SCL_BOARD "qemu")
    set(XIP_BOARD_DEFS "")
else()
    set(SCL_BOARD "weact" CACHE STRING "Target board (nucleo or weact)")
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
set(ST_XIP ${_repo}/test-xip/test-xip-static/semihosting)
set(ST_XIPD ${_repo}/test-xip/test-xip-static-direct)

# ---- hosted platform (own target name so it coexists with the nolib test) ---
set(PLATFORM_SOURCES
    ${_repo}/test-xip/common/src/semihost-file.cpp
    ${_su}/common/firmware/src/scl-log-trace.cpp
    ${XIP_LED}
    ${XIP_FS_FILL}
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

add_library(platform-board-hosted-xip-interrupt-static-direct INTERFACE)
target_sources(platform-board-hosted-xip-interrupt-static-direct INTERFACE ${PLATFORM_SOURCES})
set_source_files_properties(${PLATFORM_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")
target_include_directories(platform-board-hosted-xip-interrupt-static-direct INTERFACE
    ${_repo}/test/platforms/board/include
    ${_su}/common/firmware/include
    ${_su}/common/system/include/cmsis
    ${_su}/common/system/include/stm32f4-hal
    ${_su}/common/system/include/cortexm
    ${_su}/common/system/include/arm
    ${_su}/common/system/include
)
target_compile_definitions(platform-board-hosted-xip-interrupt-static-direct INTERFACE
    DEBUG USE_FULL_ASSERT OS_USE_SEMIHOSTING TRACE OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F411xE USE_HAL_DRIVER ${XIP_BOARD_DEFS}
    HARD_FLOAT
    OS_INCLUDE_STARTUP_GUARD_CHECKS=0
)
target_compile_options(platform-board-hosted-xip-interrupt-static-direct INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-include;${_repo}/test-xip/common/include/abort_fix.h>
)
target_link_libraries(platform-board-hosted-xip-interrupt-static-direct INTERFACE stdc++ m)

separate_arguments(SCL_MCU_FLAG_LIST UNIX_COMMAND "${SCL_MCU_FLAGS}")
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libsupc++.a
                OUTPUT_VARIABLE SCL_LIBSUPCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${SCL_MCU_FLAG_LIST} -print-file-name=libgcc.a
                OUTPUT_VARIABLE SCL_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

# The static XIP loader lives here (no dynamic loader); it links scl::xip-static
# (classify/relocate) and compiles pic-trampoline.c + image-memory.cpp directly.
# Uses direct calls (scl::Module + call_sym* helpers) instead of manual r10 setting.
add_executable(scl-fw-xip-interrupt-static-direct
    src/main.cpp
    ${ST_XIPD}/src/test_c1_c2_cpp3.cpp
    ${ST_XIPD}/src/test_ehx_cons_ppeh_ppsh_prov.cpp
    ${ST_XIPD}/src/walkthrough.cpp
    ${ST_XIPD}/src/walkthrough_complex.cpp
    ${_test3}/src/fs-archive.cpp
    ${_test3}/src/flash-prog.c
)
target_include_directories(scl-fw-xip-interrupt-static-direct PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ST_XIP}/include
    ${ST_XIP}/src
    ${_test3}/include
    ${SCL_ROOT}/include/scl
    ${_repo}/include
    ${_repo}/test-xip/common/include
)
target_link_libraries(scl-fw-xip-interrupt-static-direct PRIVATE
    platform-board-hosted-xip-interrupt-static-direct
    scl::xip-static
    ${SCL_LIBSUPCXX}
    ${SCL_LIBGCC}
)
target_compile_definitions(scl-fw-xip-interrupt-static-direct PRIVATE
    DL_LIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
)

# Copy standard PIC fixtures from test/fixtures to test-xip/fixtures so the
# provision step can read them via semihosting from the same fixtures dir.
# Generate XIP split-segment fixtures (R-X | RW, VA==offset). The prebuilt
# test/fixtures/*.so are fused RWE (RAM-load) which classify_segments rejects;
# build-xip-libs.sh rebuilds them as splits into ../fixtures at build time.
get_filename_component(SCL_TCBIN ${CMAKE_C_COMPILER} DIRECTORY)
add_custom_target(scl-fw-xip-interrupt-static-direct-fixtures
    COMMAND ${CMAKE_COMMAND} -E env TOOLCHAIN_BIN=${SCL_TCBIN}
            bash ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures-src/build-xip-libs.sh
    COMMENT "Building XIP split-segment fixtures (c1/c2/cpp3/... R-X | RW)" VERBATIM)
add_dependencies(scl-fw-xip-interrupt-static-direct scl-fw-xip-interrupt-static-direct-fixtures)

target_link_options(scl-fw-xip-interrupt-static-direct PRIVATE
    -pie
    -nostartfiles --specs=nano.specs
    LINKER:-Bsymbolic
    LINKER:-E
    LINKER:--unresolved-symbols=ignore-in-shared-libs
    -T${XIP_FLATFS_LD}
    LINKER:-Map=$<TARGET_FILE:scl-fw-xip-interrupt-static-direct>.map
    -L${CMAKE_CURRENT_SOURCE_DIR}/../fixtures
    -l:slib.so -l:mylib1.so -l:mylib2.so -l:c1.so -l:c2.so -l:cpp3.so -l:libprov.so -l:libcons.so -l:libcppsh.so -lstdc++
)

add_custom_command(TARGET scl-fw-xip-interrupt-static-direct POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:scl-fw-xip-interrupt-static-direct> $<TARGET_FILE:scl-fw-xip-interrupt-static-direct>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-xip-interrupt-static-direct> $<TARGET_FILE:scl-fw-xip-interrupt-static-direct>.bin
)

if(XIP_RUNNER)
    add_test(NAME scl-fw-xip-interrupt-static-direct
             COMMAND ${XIP_RUNNER} --machine ${XIP_MACHINE}
                     --done "regular tests done" $<TARGET_FILE:scl-fw-xip-interrupt-static-direct>
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
else()
    add_test(NAME scl-fw-xip-interrupt-static-direct
             COMMAND ${SCL_TEST_DIR}/tools/flash-run.sh $<TARGET_FILE:scl-fw-xip-interrupt-static-direct>.hex
             WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
endif()
