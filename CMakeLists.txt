# Builds either as a plain add_subdirectory(ammalloc) child of the root
# AetherMind project (reusing the root's spdlog target) or fully standalone
# from its own source directory:
#   cmake -S ammalloc -B build && cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(ammalloc LANGUAGES CXX)
endif()

include(FetchContent)

file(GLOB_RECURSE AMMALLOC_SRC_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_library(ammalloc STATIC ${AMMALLOC_SRC_FILES})

# ammalloc headers and sources require C++20 (std::format, std::jthread,
# std::source_location); declare it on the target so standalone builds do
# not depend on the root project's CMAKE_CXX_STANDARD.
target_compile_features(ammalloc PUBLIC cxx_std_20)

option(USE_57BIT_VA "System use 57bits virtual address space." OFF)

target_include_directories(ammalloc
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Fetch spdlog locally only when building standalone; as a subdirectory the
# root project has already made it available via FetchContent.
if(NOT TARGET spdlog::spdlog)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.17.0
    )
    FetchContent_MakeAvailable(spdlog)
endif()

find_package(Threads REQUIRED)

target_link_libraries(ammalloc
        PRIVATE
        Threads::Threads
        spdlog::spdlog
)

# Match the root project's spdlog backend (std::format) when built standalone.
target_compile_definitions(ammalloc PRIVATE SPDLOG_USE_STD_FORMAT)

if (BUILD_TESTS)
    target_compile_definitions(ammalloc PUBLIC AMMALLOC_TEST)
endif()

if (USE_57BIT_VA)
    target_compile_definitions(ammalloc PUBLIC AM_USE_57BIT_VA)
endif ()
