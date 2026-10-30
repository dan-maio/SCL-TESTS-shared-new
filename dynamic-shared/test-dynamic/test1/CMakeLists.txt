# test1 — the simplest POSIX dynamic-loading firmware app.
#
# Standard POSIX dlopen/dlsym/dlclose/dlerror over scl::dynamic-newlib (hosted,
# newlib). Fixtures are built by test-dynamic/fixtures-src/build.sh into
# test-dynamic/fixtures/ and read over semihosting from SCL_SO_DIR.
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)   # standalone configure of just this test app
    project(scl-fw-posix-dl LANGUAGES C CXX)
    set(SCL_HOSTED ON CACHE BOOL "" FORCE)
    get_filename_component(SCL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
    add_subdirectory(${SCL_ROOT} scl)
    enable_testing()
endif()

if(NOT SCL_HOSTED OR NOT TARGET scl-dynamic-newlib)
    message(STATUS "test1 skipped: needs SCL_HOSTED (hosted toolchain)")
    return()
endif()

set(SCL_PLATFORM "board" CACHE STRING
    "Target platform (board for nucleo/weact, or qemu-f411re for qemu-system-arm)")
include(${CMAKE_CURRENT_SOURCE_DIR}/../../test/platforms/${SCL_PLATFORM}-hosted/cmake/platform-library.cmake)

add_executable(scl-fw-posix-dl
    src/main.cpp
    ${STATIC_UNIFY_DIR}/firmware/src/dl-impl.cpp   # the dl:: facade (vendored)
)
target_link_libraries(scl-fw-posix-dl PRIVATE
    platform::${SCL_PLATFORM}-hosted
    scl::dynamic-newlib)
target_compile_definitions(scl-fw-posix-dl PRIVATE
    SCL_SO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
    FIRMWARE_ELF_PATH="$<TARGET_FILE:scl-fw-posix-dl>")

# Keep-alive anchors: the fixtures import these from main (newlib, libm,
# libsupc++/libstdc++). main itself never references most of them, so without
# -u they would be garbage-collected and never reach main's symtab.
foreach(_a
    printf puts strcat strcpy strlen log logf cosf sinf sqrtf malloc free
    _Znwj _ZdlPvj _ZnwjRKSt9nothrow_t _ZSt7nothrow
    __cxa_throw __cxa_begin_catch __cxa_end_catch __cxa_allocate_exception
    __cxa_free_exception __cxa_end_cleanup __gxx_personality_v0 __dynamic_cast
    __aeabi_unwind_cpp_pr0 __aeabi_unwind_cpp_pr1
    _ZTVN10__cxxabiv117__class_type_infoE _ZTVN10__cxxabiv120__si_class_type_infoE
    __aeabi_fadd __aeabi_fmul __aeabi_ui2f __aeabi_f2uiz __aeabi_fcmplt
)
    list(APPEND _posixdl_uflags "LINKER:-u,${_a}")
endforeach()
target_link_options(scl-fw-posix-dl PRIVATE
    ${_posixdl_uflags}
    LINKER:-Map=$<TARGET_FILE:scl-fw-posix-dl>.map)

scl_embed_symtab(scl-fw-posix-dl)

add_custom_command(TARGET scl-fw-posix-dl POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:scl-fw-posix-dl> $<TARGET_FILE:scl-fw-posix-dl>.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:scl-fw-posix-dl> $<TARGET_FILE:scl-fw-posix-dl>.bin)

string(TOUPPER "${SCL_PLATFORM}" _SCL_PLATFORM_UPPER)
string(REPLACE "-" "_" _SCL_PLATFORM_VAR "${_SCL_PLATFORM_UPPER}")
set(_runner "${PLATFORM_${_SCL_PLATFORM_VAR}_HOSTED_RUNNER}")
if(_runner)
    add_test(NAME scl-fw-posix-dl
             COMMAND ${_runner} --machine ${PLATFORM_${_SCL_PLATFORM_VAR}_HOSTED_QEMU_MACHINE}
                     --done "RESULT:" $<TARGET_FILE:scl-fw-posix-dl>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
else()
    add_test(NAME scl-fw-posix-dl
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../test/tools/flash-run.sh
                     $<TARGET_FILE:scl-fw-posix-dl>.hex
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()
