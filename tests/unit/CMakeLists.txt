set(TEST_TARGET ammalloc_unit_tests)
include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

# Unit tests are collected by GLOB_RECURSE with CONFIGURE_DEPENDS so newly
# added tests are picked up automatically. Keep non-test .cpp files out of
# unit/, they would be turned into standalone test targets.
file(GLOB_RECURSE TEST_SRC_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

# All unit tests are linked into a single executable; run it directly to
# execute every TEST case (--gtest_filter selects a subset).
add_executable(${TEST_TARGET} ${TEST_SRC_FILES})
target_link_libraries(${TEST_TARGET} PRIVATE
        ${PROJECT_NAME}
        GTest::gtest_main)
