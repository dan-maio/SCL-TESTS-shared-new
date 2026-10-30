# static-shared — Cortex-M SCL, static-loader-only subset.
#
# Source Code Libraries for loading shared libraries on Cortex-M (ARM32), trimmed
# to the static-loader family:
#
#   scl::static      — ELF32 static patcher: load a -fPIC .so, patch GOT/PLT, run
#                      constructors, byte-exact unload, TLS, refcount, scopes...
#   scl::xip-static  — execute-in-place flavour (code from flash, data in RAM).
#                      Also pulls scl::xip-common (segment-map/relocate/compact).
#
# Dependencies: common-elf-reader (vendored beside this CMakeLists.txt).
# Consume with:  add_subdirectory(<this dir> scl)
#                target_link_libraries(app PRIVATE scl::static)   # or scl::xip-static
#
# All targets are INTERFACE "source code libraries": no .a, sources propagate to
# the consumer and are compiled with the consumer's own flags.

option(SCL_BUILD_TESTS "Enable CTest registration for the firmware suites" ${PROJECT_IS_TOP_LEVEL})

# The ELF32 reader/parser (elf_image, byte_source, elf-types, result<T>) is
# vendored here (was a git submodule). It is consumed as source, same INTERFACE
# pattern as the SCL itself.
# SCL_ROOT = this file's repo (cmake/..), so scl-lib.cmake can be include()d
# from ANY project, not only optimal-shared's own top-level CMakeLists.
get_filename_component(SCL_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(COMMON_ELF_READER_DIR "${SCL_ROOT}/common-elf-reader"
    CACHE PATH "Path to the common-elf-reader repo")
if(NOT EXISTS "${COMMON_ELF_READER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "common-elf-reader not found at ${COMMON_ELF_READER_DIR}. "
        "Vendor it beside this repo or set -DCOMMON_ELF_READER_DIR=<path>.")
endif()

# The common core is portable C++20; the static loader and the XIP port layer are
# Cortex-M only.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)" OR CMAKE_CROSSCOMPILING)
    set(SCL_TARGET_ARM ON)
else()
    set(SCL_TARGET_ARM OFF)
endif()

# SCL_HOSTED: parts needing a hosted C++ runtime (std::vector): the relocator and
# elf_image::collect_relocations. The static loader itself needs a hosted runtime,
# so hosted apps set this ON.
if(SCL_TARGET_ARM)
    option(SCL_HOSTED "Build hosted-only parts (std::vector): relocator" OFF)
else()
    option(SCL_HOSTED "Build hosted-only parts (std::vector): relocator" ON)
endif()

add_subdirectory(${COMMON_ELF_READER_DIR} common-elf-reader)

# ---- scl-common: code shared by the loaders ---------------------------------
add_library(scl-common INTERFACE EXCLUDE_FROM_ALL)
target_sources(scl-common INTERFACE
    ${SCL_ROOT}/src/dynamic-info.cpp
    ${SCL_ROOT}/src/lazy-resolve.cpp
    ${SCL_ROOT}/src/region-allocator.cpp
    ${SCL_ROOT}/src/reloc-applier.cpp
    ${SCL_ROOT}/src/symbol-resolver.cpp
    ${SCL_ROOT}/src/log.c
    ${SCL_ROOT}/src/scl-log-put-hex.c
)
target_include_directories(scl-common INTERFACE ${SCL_ROOT}/include)
target_link_libraries(scl-common INTERFACE common-elf-reader::elf-reader)
target_compile_features(scl-common INTERFACE cxx_std_20)

if(SCL_TARGET_ARM)
    # ---- scl-static: standalone ELF32 static patcher -------------------------
    # Load a -fPIC .so into a caller buffer, patch its GOT + main's GOT, run
    # constructors, resolve symbols; byte-exact unload via patch journal;
    # single/multi-thread + global-dynamic TLS; RTLD_LOCAL/GLOBAL; refcount.
    add_library(scl-static INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-static INTERFACE
        ${SCL_ROOT}/src/static/elf-reader.cpp
        ${SCL_ROOT}/src/static/shlib-loader.cpp
        ${SCL_ROOT}/src/static/logger.cpp
        ${SCL_ROOT}/src/static/module.cpp
        ${SCL_ROOT}/src/static/tls.c
    )
    target_include_directories(scl-static INTERFACE
        ${SCL_ROOT}/include
        ${SCL_ROOT}/include/static
    )
    target_link_libraries(scl-static INTERFACE common-elf-reader::elf-reader)

    # ---- scl-xip-common: execute-in-place primitives -------------------------
    add_library(scl-xip-common INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-xip-common INTERFACE
        ${SCL_ROOT}/src/xip/segment-map.cpp
        ${SCL_ROOT}/src/xip/relocate.cpp
        ${SCL_ROOT}/src/xip/compact.cpp
    )
    target_include_directories(scl-xip-common INTERFACE ${SCL_ROOT}/include)
    target_link_libraries(scl-xip-common INTERFACE scl-common)

    # ---- scl-xip-static: XIP adapter over the static loader ------------------
    add_library(scl-xip-static INTERFACE EXCLUDE_FROM_ALL)
    target_sources(scl-xip-static INTERFACE
        ${SCL_ROOT}/src/xip/static/adapter.cpp
        ${SCL_ROOT}/src/xip/xip-cross-call.c
    )
    target_include_directories(scl-xip-static INTERFACE ${SCL_ROOT}/include/xip)
    target_link_libraries(scl-xip-static INTERFACE scl-xip-common scl-static)
endif()

# ---- namespaced aliases ------------------------------------------------------
add_library(scl::common ALIAS scl-common)
if(TARGET scl-static)
    add_library(scl::static ALIAS scl-static)
endif()
if(TARGET scl-xip-common)
    add_library(scl::xip-common ALIAS scl-xip-common)
endif()
if(TARGET scl-xip-static)
    add_library(scl::xip-static ALIAS scl-xip-static)
endif()

# ---- tests ------------------------------------------------------------------
# The test applications are Cortex-M4 firmware, built as separate cross projects
# (test-static/ and test-xip/), flashed on weact/nucleo or run under qemu.
if(SCL_BUILD_TESTS AND NOT SCL_TARGET_ARM)
    enable_testing()
endif()
