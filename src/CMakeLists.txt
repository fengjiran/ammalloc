# Source collection: GLOB_RECURSE with CONFIGURE_DEPENDS picks up newly added
# sources automatically. Keep non-library .cpp/.cc files out of this directory;
# test/benchmark sources live under tests/.
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/*.cpp
        ${PROJECT_SOURCE_DIR}/src/*.cc
)

include(FetchContent)

add_library(${PROJECT_NAME} SHARED ${SRC_FILES})

# ammalloc headers and sources require C++20 (std::format, std::jthread,
# std::source_location); declare it on the target.
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Match the spdlog backend used by the sources (std::format). The option
# makes spdlog propagate SPDLOG_USE_STD_FORMAT publicly to its consumers.

if (USE_57BIT_VA)
    # PUBLIC: VA_BITS/PAGE_ID_BITS are constexpr in config.h also consumed by
    # test targets; keep the definition consistent across the ABI.
    target_compile_definitions(${PROJECT_NAME} PUBLIC AM_USE_57BIT_VA)
endif ()

if (BUILD_TESTS)
    # PUBLIC: page_cache.h/page_allocator.h expose test-only helpers under
    # AMMALLOC_TEST; test targets inherit it through linking.
    target_compile_definitions(${PROJECT_NAME} PUBLIC AMMALLOC_TEST)
endif ()

find_package(Threads REQUIRED)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(spdlog)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        Threads::Threads
        spdlog::spdlog
)
