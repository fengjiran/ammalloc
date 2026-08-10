cmake_minimum_required(VERSION 3.28)
project(ammalloc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_TESTS "Adding test targets." ON)
option(BUILD_BENCHMARKS "Adding benchmark targets." ON)
option(USE_57BIT_VA "System use 57bits virtual address space." OFF)

add_subdirectory(src)

if (BUILD_TESTS)
    message(STATUS "Enable Testing")
    add_subdirectory(tests/unit)
endif ()

if (BUILD_BENCHMARKS)
    message(STATUS "Enable Benchmarks")
    add_subdirectory(tests/benchmark)
endif ()
