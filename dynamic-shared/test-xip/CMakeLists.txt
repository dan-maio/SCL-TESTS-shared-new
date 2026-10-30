# test-xip — XIP dynamic-loader test apps.
#
# Configure with the ARM toolchain:
#   cmake -S test-xip -B test-xip/build \
#         -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-none-eabi.cmake [-DSCL_PLATFORM=qemu-f411re]
cmake_minimum_required(VERSION 3.20)
project(scl-tests-xip-dynamic LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-xip/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
add_subdirectory(${SCL_ROOT} scl)

enable_testing()
include(${SCL_ROOT}/test/cmake/scl-test-app.cmake)

add_subdirectory(test-xip-dynamic-nolib/test1)
# newlib XIP app is a separate HOSTED build (see build-and-run.sh)
