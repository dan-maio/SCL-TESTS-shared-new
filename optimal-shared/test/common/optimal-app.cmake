# optimal-app.cmake — build one optimal-shared test firmware.
#
#   add_optimal_app(<target> <mode-string> <main-source>)
#
# Each test app has its OWN self-contained <main-source> (test-ram/src/main.cpp,
# test-xip/src/main.cpp) holding the full by-name exercise of every fixture plus
# that test's blob source; they differ only in provide()/setup(). The firmware
# LINKS the fixtures (-l:*.so) so main's references get GOT/PLT slots that
# xip::nopic::patch_main() fills at load.
function(add_optimal_app TARGET MODE MAIN_SRC)
    add_executable(${TARGET} ${MAIN_SRC})
    # The vendored platform sources need -fhosted (so syscalls.c defines the
    # semihosting _read/_write/__initialize_args under __STDC_HOSTED__==1). Source
    # properties are directory-scoped, and these files compile HERE (INTERFACE
    # propagation into the executable's dir), so set it in this scope.
    set_source_files_properties(${PLATFORM_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-fno-pic;-fhosted")
    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/test/common)
    target_link_libraries(${TARGET} PRIVATE
        platform-optimal
        optimal                       # -> scl::loader-nopic -> scl::* + elf-reader
        ${SCL_LIBSUPCXX} ${SCL_LIBGCC})
    target_compile_definitions(${TARGET} PRIVATE
        DL_LIB_DIR="${CMAKE_SOURCE_DIR}/fixtures"
        OPTIMAL_MODE="${MODE}")
    target_link_options(${TARGET} PRIVATE
        -pie -nostartfiles --specs=nano.specs
        LINKER:-Bsymbolic LINKER:-E LINKER:--unresolved-symbols=ignore-in-shared-libs
        -T${OPTIMAL_APP_LD}
        LINKER:-Map=$<TARGET_FILE:${TARGET}>.map
        -L${CMAKE_SOURCE_DIR}/fixtures
        ${OPTIMAL_LFLAGS} -lstdc++)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET}> $<TARGET_FILE:${TARGET}>.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> $<TARGET_FILE:${TARGET}>.bin)
    if(XIP_RUNNER)
        add_test(NAME ${TARGET}
                 COMMAND ${XIP_RUNNER} --machine ${XIP_MACHINE}
                         --done "optimal-shared: done" $<TARGET_FILE:${TARGET}>
                 WORKING_DIRECTORY ${SCL_TEST_DIR}/..)
    endif()
endfunction()
