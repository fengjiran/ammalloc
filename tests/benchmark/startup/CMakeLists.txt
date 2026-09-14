# Startup / degradation / stress benchmarks run in an isolated process so that
# cold-init timing and fault-injection modes never pollute the steady-state
# google-benchmark executable. This target uses a self-written main() with
# std::chrono instead of benchmark::benchmark_main.
set(STARTUP_TARGET ammalloc_bench_startup)

find_package(Threads REQUIRED)

file(GLOB_RECURSE STARTUP_SRC_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(${STARTUP_TARGET} ${STARTUP_SRC_FILES})

target_link_libraries(${STARTUP_TARGET} PRIVATE
        ${PROJECT_NAME}
        Threads::Threads
)

# The degradation modes drive the g_mock_* fault-injection hooks, which only
# exist under AMMALLOC_TEST.
target_compile_definitions(${STARTUP_TARGET} PRIVATE AMMALLOC_TEST)
