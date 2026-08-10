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

# Benchmarks are collected by GLOB_RECURSE with CONFIGURE_DEPENDS so newly
# added benchmarks are picked up automatically. Keep non-benchmark .cpp
# files out of benchmark/, they would become standalone executables.
file(GLOB_RECURSE BENCH_SRC_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

# All benchmarks are linked into a single executable; run it directly to
# execute every benchmark (--benchmark_filter selects a subset).
add_executable(${BENCH_TARGET} ${BENCH_SRC_FILES})
target_link_libraries(${BENCH_TARGET} PRIVATE
        ${PROJECT_NAME}
        benchmark::benchmark_main)
