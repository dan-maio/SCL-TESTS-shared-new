# test-dynamic — the POSIX dynamic-loading (dlopen/dlsym/dlclose) test tree.
#
# Configure with the HOSTED toolchain:
#   cmake -S test-dynamic -B test-dynamic/build \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-hosted.cmake -DSCL_PLATFORM=qemu-f411re
#   cmake --build test-dynamic/build
cmake_minimum_required(VERSION 3.20)
project(scl-tests-dynamic LANGUAGES C CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "test-dynamic/ builds Cortex-M4 firmware apps: configure with "
                        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/arm-none-eabi-hosted.cmake")
endif()

set(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(SCL_ROOT ${SCL_ROOT} ABSOLUTE)
set(SCL_HOSTED ON CACHE BOOL "" FORCE)   # the hosted dynamic loader needs SCL_HOSTED
add_subdirectory(${SCL_ROOT} scl)

enable_testing()
include(${SCL_ROOT}/test/cmake/scl-test-app.cmake)

add_subdirectory(test1)
