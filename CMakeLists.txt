cmake_minimum_required(VERSION 3.14)
project(CourtDB VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# Build Configuration
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings — treat warnings seriously
add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
    -Wshadow
    -Wdouble-promotion
    -Wformat=2
    -Wnull-dereference
)

# Optimization for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
set(CMAKE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

# =============================================================================
# Library Target
# =============================================================================

add_library(courtdb STATIC
    src/storage/page.cpp
    src/disk/disk_manager.cpp
)

target_include_directories(courtdb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# =============================================================================
# Testing (GoogleTest via FetchContent)
# =============================================================================

option(COURTDB_BUILD_TESTS "Build unit tests" ON)

if(COURTDB_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    # Helper function to add test executables
    function(courtdb_add_test TEST_NAME TEST_SOURCE)
        add_executable(${TEST_NAME} ${TEST_SOURCE})
        target_link_libraries(${TEST_NAME} PRIVATE courtdb GTest::gtest_main)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endfunction()

    # Unit tests
    courtdb_add_test(page_test tests/storage/page_test.cpp)
    courtdb_add_test(disk_manager_test tests/disk/disk_manager_test.cpp)
endif()

# =============================================================================
# Benchmarks
# =============================================================================

option(COURTDB_BUILD_BENCHMARKS "Build benchmarks" OFF)

if(COURTDB_BUILD_BENCHMARKS)
    add_executable(page_benchmark benchmarks/storage/page_benchmark.cpp)
    target_link_libraries(page_benchmark PRIVATE courtdb)

    add_executable(disk_benchmark benchmarks/disk/disk_benchmark.cpp)
    target_link_libraries(disk_benchmark PRIVATE courtdb)
endif()
