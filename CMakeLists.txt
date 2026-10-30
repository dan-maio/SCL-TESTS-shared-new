# SCL-TESTS-shared — tests / platforms / fixtures / examples for the SCL loaders.
# The loader itself is NOT vendored here: it comes from the SCL-shared submodule,
# consumed as INTERFACE source libraries. This root just re-exports SCL-shared's
# scl::* targets so each standalone test suite (which does
# `if(NOT TARGET scl-common) add_subdirectory(<SCL_ROOT> scl)`) picks them up.
cmake_minimum_required(VERSION 3.20)
project(scl-tests LANGUAGES C CXX)
add_subdirectory(SCL-shared scl)      # -> scl::common/reloc/static/xip-common/xip-static/loader-nopic/optimal
enable_testing()
