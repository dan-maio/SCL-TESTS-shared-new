# test-xip — modules provisioned in on-chip flash / XIP (see src/main.cpp for the full surface).
add_optimal_app(scl-fw-optimal-xip "XIP" ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
