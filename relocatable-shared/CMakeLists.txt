# Shim: this project dir is the TEST TREE; the loader now comes from the shared
# SCL-shared submodule (sibling of the project). Suites do add_subdirectory(${SCL_ROOT} scl)
# and reference ${SCL_ROOT}/include — both resolve here (include is a symlink into SCL-shared).
cmake_minimum_required(VERSION 3.20)
if(NOT TARGET scl-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../SCL-shared ${CMAKE_BINARY_DIR}/_scl-shared)
endif()
