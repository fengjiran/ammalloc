set(BENCH_TARGET ammalloc_benchmarks)

include(FetchContent)

# Google benchmark build options; must be set before MakeAvailable.
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
)
FetchContent_MakeAvailable(benchmark)

# Steady-state benchmarks are collected by GLOB (non-recursive) with
# CONFIGURE_DEPENDS so newly added top-level benchmarks are picked up
# automatically. The recursion is intentionally NOT used: the startup/
# subdirectory holds an isolated-process target that must not be linked into
# this executable. Keep non-benchmark .cpp files out of benchmark/.
file(GLOB BENCH_SRC_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

# All benchmarks are linked into a single executable; run it directly to
# execute every benchmark (--benchmark_filter selects a subset).
add_executable(${BENCH_TARGET} ${BENCH_SRC_FILES})
target_link_libraries(${BENCH_TARGET} PRIVATE
        ${PROJECT_NAME}
        benchmark::benchmark_main)

# Isolated startup / degradation / stress target with its own main().
add_subdirectory(startup)
