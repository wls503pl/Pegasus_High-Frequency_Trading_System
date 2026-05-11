cmake_minimum_required(VERSION 3.16)
project(Pegasus VERSION 0.1.0)

# ──────────────────────────────────────────────
# Compilation Standards
# ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ──────────────────────────────────────────────
# Compilation Options
# ──────────────────────────────────────────────
# Release default optimization; can be toggled using -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    # Release: Radical Optimization
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    # Debug: No optimizations + Full debug information
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
)

# ──────────────────────────────────────────────
# AddressSanitizer (automatically enabled in Debug mode)
# ──────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ──────────────────────────────────────────────
# Header-only library: include/
# ──────────────────────────────────────────────
add_library(pegasus_headers INTERFACE)
target_include_directories(pegasus_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)

# ──────────────────────────────────────────────
# Phase 1 Test Objectives
# ──────────────────────────────────────────────

# --- DataTypes ---
add_executable(test_phase1_datatypes
    tests/phase1/test_phase1_datatypes.cpp
)
target_link_libraries(test_phase1_datatypes PRIVATE pegasus_headers)

# --- IPC ---
add_executable(test_phase1_ipc
    tests/phase1/test_phase1_ipc.cpp
)
target_link_libraries(test_phase1_ipc PRIVATE pegasus_headers)

# --- RDTSC Timer ---
add_executable(test_phase1_timer
    tests/phase1/test_phase1_timer.cpp
)
target_link_libraries(test_phase1_timer PRIVATE pegasus_headers)

# ──────────────────────────────────────────────
# A custom target that runs all tests with one click.
# make run_phase1
# ──────────────────────────────────────────────
add_custom_target(run_phase1
    COMMAND echo "\\n======== DataTypes ========"
    COMMAND ./test_phase1_datatypes
    COMMAND echo "\\n======== IPC ========"
    COMMAND ./test_phase1_ipc
    COMMAND echo "\\n======== RDTSC Timer ========"
    COMMAND ./test_phase1_timer
    DEPENDS test_phase1_datatypes test_phase1_ipc test_phase1_timer
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all Phase 1 tests"
)