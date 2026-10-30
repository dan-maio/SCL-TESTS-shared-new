# fixtures/CMakeLists.txt — build floatlib.so with the SCL no-pic recipe.
add_custom_target(fixtures ALL
  COMMAND ${CMAKE_COMMAND} -E env ARM_TOOLCHAIN_BIN=${ARM_TOOLCHAIN_BIN}
          bash ${CMAKE_CURRENT_SOURCE_DIR}/build.sh
  BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/floatlib.so
             ${CMAKE_CURRENT_SOURCE_DIR}/link.txt
  COMMENT "Building fixtures/floatlib.so (-fno-pic -mlong-calls, hard-float)")
