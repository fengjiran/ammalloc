#include "ammalloc/common.h"

#include <gtest/gtest.h>

namespace {
using namespace ammalloc;

TEST(ConfigTest, ParseSize) {
    EXPECT_EQ(detail::ParseSize("100"), 100);
    EXPECT_EQ(detail::ParseSize("1024"), 1024);

    // Case-insensitive unit suffixes; the trailing 'b' is ignored.
    EXPECT_EQ(detail::ParseSize("1k"), 1024);
    EXPECT_EQ(detail::ParseSize("1K"), 1024);
    EXPECT_EQ(detail::ParseSize("1kb"), 1024);
    EXPECT_EQ(detail::ParseSize("1M"), 1024 * 1024);
    EXPECT_EQ(detail::ParseSize("2G"), 2ULL * 1024 * 1024 * 1024);

    EXPECT_EQ(detail::ParseSize("  64"), 64);
    EXPECT_EQ(detail::ParseSize("64 KB"), 64 * 1024);
    EXPECT_EQ(detail::ParseSize("  10  mb  "), 10 * 1024 * 1024);

    EXPECT_EQ(detail::ParseSize(nullptr), 0);
    EXPECT_EQ(detail::ParseSize(""), 0);
    EXPECT_EQ(detail::ParseSize("abc"), 0); // Invalid number.
    EXPECT_EQ(detail::ParseSize("10x"), 10);// Unknown suffix 'x' counts as plain bytes.

    // Overflow must be reported as SIZE_MAX rather than wrapping.
    EXPECT_EQ(detail::ParseSize("10000 TB"), 10000ULL * 1024 * 1024 * 1024 * 1024);
    EXPECT_EQ(detail::ParseSize("20000000 TB"), std::numeric_limits<size_t>::max());
}

TEST(ConfigUtilsTest, ParseBool) {
    EXPECT_TRUE(detail::ParseBool("1"));
    EXPECT_TRUE(detail::ParseBool("true"));
    EXPECT_TRUE(detail::ParseBool("on"));
    EXPECT_TRUE(detail::ParseBool("yes"));

    EXPECT_TRUE(detail::ParseBool("True"));
    EXPECT_TRUE(detail::ParseBool("TRUE"));
    EXPECT_TRUE(detail::ParseBool("On"));
    EXPECT_TRUE(detail::ParseBool("Yes"));
    EXPECT_TRUE(detail::ParseBool("tRuE"));

    EXPECT_TRUE(detail::ParseBool(" 1 "));
    EXPECT_TRUE(detail::ParseBool("  true"));
    EXPECT_TRUE(detail::ParseBool("on  "));

    EXPECT_FALSE(detail::ParseBool("0"));
    EXPECT_FALSE(detail::ParseBool("false"));
    EXPECT_FALSE(detail::ParseBool("off"));
    EXPECT_FALSE(detail::ParseBool("no"));
    EXPECT_FALSE(detail::ParseBool("random_string"));
    EXPECT_FALSE(detail::ParseBool(""));
    EXPECT_FALSE(detail::ParseBool(nullptr));

    // Only exact tokens count: prefix matches and partial digits are false.
    EXPECT_FALSE(detail::ParseBool("true_value"));
    EXPECT_FALSE(detail::ParseBool("10"));
}

TEST(ConfigTest, LegacyParserTests) {
    // Keep the original parser assertions to guard against regressions.
    EXPECT_EQ(detail::ParseSize("100"), 100);
    EXPECT_EQ(detail::ParseSize("1k"), 1024);
    EXPECT_EQ(detail::ParseSize("1M"), 1024 * 1024);
    EXPECT_TRUE(detail::ParseBool("true"));
    EXPECT_FALSE(detail::ParseBool("false"));
}

TEST(CommonUtilsTest, AlignUp) {
    // Power-of-two alignment (fast path).
    EXPECT_EQ(detail::AlignUp(1, 8), 8);
    EXPECT_EQ(detail::AlignUp(7, 8), 8);
    EXPECT_EQ(detail::AlignUp(8, 8), 8);
    EXPECT_EQ(detail::AlignUp(9, 8), 16);

    EXPECT_EQ(detail::AlignUp(4095, 4096), 4096);
    EXPECT_EQ(detail::AlignUp(4096, 4096), 4096);
    EXPECT_EQ(detail::AlignUp(4097, 4096), 8192);

    // Non-power-of-two alignment (slow-path fallback).
    EXPECT_EQ(detail::AlignUp(1, 7), 7);
    EXPECT_EQ(detail::AlignUp(6, 7), 7);
    EXPECT_EQ(detail::AlignUp(7, 7), 7);
    EXPECT_EQ(detail::AlignUp(8, 7), 14);

    // Zero maps to one alignment unit.
    EXPECT_EQ(detail::AlignUp(0, 8), 8);
}

TEST(CommonUtilsTest, PtrToPageId) {
    if constexpr (SystemConfig::PAGE_SIZE == 4096) {
        void* ptr1 = nullptr;
        EXPECT_EQ(detail::PtrToPageId(ptr1), 0);

        void* ptr2 = reinterpret_cast<void*>(0xFFF);// 4095, last byte of page 0
        EXPECT_EQ(detail::PtrToPageId(ptr2), 0);

        void* ptr3 = reinterpret_cast<void*>(0x1000);// 4096, first byte of page 1
        EXPECT_EQ(detail::PtrToPageId(ptr3), 1);

        // Round-trip: PageIDToPtr must invert PtrToPageId.
        EXPECT_EQ(detail::PageIDToPtr(1), ptr3);
    }
}

}// namespace