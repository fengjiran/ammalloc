//
// Created by richard on 2/4/26.
//
#include "ammalloc/common.h"
#include "ammalloc/size_class.h"

#include <cstring>
#include <gtest/gtest.h>

namespace {
using namespace ammalloc;

// Helper to access private members for testing if needed,
// but SizeClass interface is mostly static public.

TEST(ConfigTest, ParseSize) {
    // 基础测试
    EXPECT_EQ(detail::ParseSize("100"), 100);
    EXPECT_EQ(detail::ParseSize("1024"), 1024);

    // 单位测试 (大小写不敏感)
    EXPECT_EQ(detail::ParseSize("1k"), 1024);
    EXPECT_EQ(detail::ParseSize("1K"), 1024);
    EXPECT_EQ(detail::ParseSize("1kb"), 1024);// 'b' 被忽略，只看首字母
    EXPECT_EQ(detail::ParseSize("1M"), 1024 * 1024);
    EXPECT_EQ(detail::ParseSize("2G"), 2ULL * 1024 * 1024 * 1024);

    // 空格测试
    EXPECT_EQ(detail::ParseSize("  64"), 64);
    EXPECT_EQ(detail::ParseSize("64 KB"), 64 * 1024);
    EXPECT_EQ(detail::ParseSize("  10  mb  "), 10 * 1024 * 1024);

    // 边界与异常测试
    EXPECT_EQ(detail::ParseSize(nullptr), 0);
    EXPECT_EQ(detail::ParseSize(""), 0);
    EXPECT_EQ(detail::ParseSize("abc"), 0); // 无效数字
    EXPECT_EQ(detail::ParseSize("10x"), 10);// 未知单位，当作 Bytes

    // 溢出测试 (64位系统下)
    // 1. 正常边界测试
    // 10000 TB = 10 PB，远未溢出
    EXPECT_EQ(detail::ParseSize("10000 TB"), 10000ULL * 1024 * 1024 * 1024 * 1024);

    // 2. 真正的溢出测试
    // 需要超过 16777216 TB (即 16 EB)
    // 这里使用 2000万 TB (20 EB)，肯定溢出
    EXPECT_EQ(detail::ParseSize("20000000 TB"), std::numeric_limits<size_t>::max());
}

TEST(ConfigUtilsTest, ParseBool) {
    // 1. 基础 Truthy 测试
    EXPECT_TRUE(detail::ParseBool("1"));
    EXPECT_TRUE(detail::ParseBool("true"));
    EXPECT_TRUE(detail::ParseBool("on"));
    EXPECT_TRUE(detail::ParseBool("yes"));

    // 2. 大小写混合测试
    EXPECT_TRUE(detail::ParseBool("True"));
    EXPECT_TRUE(detail::ParseBool("TRUE"));
    EXPECT_TRUE(detail::ParseBool("On"));
    EXPECT_TRUE(detail::ParseBool("Yes"));
    EXPECT_TRUE(detail::ParseBool("tRuE"));

    // 3. 空格测试
    EXPECT_TRUE(detail::ParseBool(" 1 "));
    EXPECT_TRUE(detail::ParseBool("  true"));
    EXPECT_TRUE(detail::ParseBool("on  "));

    // 4. Falsy 测试
    EXPECT_FALSE(detail::ParseBool("0"));
    EXPECT_FALSE(detail::ParseBool("false"));
    EXPECT_FALSE(detail::ParseBool("off"));
    EXPECT_FALSE(detail::ParseBool("no"));
    EXPECT_FALSE(detail::ParseBool("random_string"));
    EXPECT_FALSE(detail::ParseBool(""));
    EXPECT_FALSE(detail::ParseBool(nullptr));

    // 5. 边界干扰测试
    EXPECT_FALSE(detail::ParseBool("true_value"));// 前缀匹配不应算作 true
    EXPECT_FALSE(detail::ParseBool("10"));        // 包含1但不完全是1
}

TEST(ConfigTest, LegacyParserTests) {
    // Keep original parser tests to ensure no regression
    EXPECT_EQ(detail::ParseSize("100"), 100);
    EXPECT_EQ(detail::ParseSize("1k"), 1024);
    EXPECT_EQ(detail::ParseSize("1M"), 1024 * 1024);
    EXPECT_TRUE(detail::ParseBool("true"));
    EXPECT_FALSE(detail::ParseBool("false"));
}

TEST(CommonUtilsTest, AlignUp) {
    // 1. Power of two alignment (Fast path)
    EXPECT_EQ(detail::AlignUp(1, 8), 8);
    EXPECT_EQ(detail::AlignUp(7, 8), 8);
    EXPECT_EQ(detail::AlignUp(8, 8), 8);
    EXPECT_EQ(detail::AlignUp(9, 8), 16);

    EXPECT_EQ(detail::AlignUp(4095, 4096), 4096);
    EXPECT_EQ(detail::AlignUp(4096, 4096), 4096);
    EXPECT_EQ(detail::AlignUp(4097, 4096), 8192);

    // 2. Non-power of two alignment (Slow path fallback)
    EXPECT_EQ(detail::AlignUp(1, 7), 7);
    EXPECT_EQ(detail::AlignUp(6, 7), 7);
    EXPECT_EQ(detail::AlignUp(7, 7), 7);
    EXPECT_EQ(detail::AlignUp(8, 7), 14);

    // 3. Edge cases
    EXPECT_EQ(detail::AlignUp(0, 8), 8);// Special handling in impl
}

TEST(CommonUtilsTest, PtrToPageId) {
    if constexpr (SystemConfig::PAGE_SIZE == 4096) {
        void* ptr1 = reinterpret_cast<void*>(0x0);
        EXPECT_EQ(detail::PtrToPageId(ptr1), 0);

        void* ptr2 = reinterpret_cast<void*>(0xFFF);// 4095
        EXPECT_EQ(detail::PtrToPageId(ptr2), 0);

        void* ptr3 = reinterpret_cast<void*>(0x1000);// 4096
        EXPECT_EQ(detail::PtrToPageId(ptr3), 1);

        // Inverse check
        EXPECT_EQ(detail::PageIDToPtr(1), ptr3);
    }
}

TEST(SizeClassTest, SmallObjectMapping) {
    // Range [0, 128] -> 8-byte alignment
    // Index 0 -> 8
    // ...
    // Index 15 -> 128

    // Check 0 (Should map to 8)
    EXPECT_EQ(SizeClass::Index(0), 0);// Logic handles 0
    EXPECT_EQ(SizeClass::Index(1), 0);
    EXPECT_EQ(SizeClass::Index(8), 0);
    EXPECT_EQ(SizeClass::Size(0), 8);

    // Check 128 boundary
    EXPECT_EQ(SizeClass::Index(120), 14);
    EXPECT_EQ(SizeClass::Index(121), 15);
    EXPECT_EQ(SizeClass::Index(128), 15);
    EXPECT_EQ(SizeClass::Size(15), 128);
}

TEST(SizeClassTest, LargeObjectMapping) {
    // Range [129, ...]
    // The first large group is [129, 256].
    // It should have 4 steps (kStepsPerGroup = 4).
    // Interval size = 256 - 128 = 128.
    // Step size = 128 / 4 = 32.
    // So the buckets are:
    // Index 16: 128 + 32 = 160
    // Index 17: 160 + 32 = 192
    // Index 18: 192 + 32 = 224
    // Index 19: 224 + 32 = 256

    EXPECT_EQ(SizeClass::Index(129), 16);
    EXPECT_EQ(SizeClass::Index(160), 16);
    EXPECT_EQ(SizeClass::Size(16), 160);

    EXPECT_EQ(SizeClass::Index(161), 17);
    EXPECT_EQ(SizeClass::Index(192), 17);
    EXPECT_EQ(SizeClass::Size(17), 192);

    EXPECT_EQ(SizeClass::Index(256), 19);
    EXPECT_EQ(SizeClass::Size(19), 256);

    // Next group: [257, 512]
    // Interval = 256. Step = 64.
    // Index 20: 256 + 64 = 320
    EXPECT_EQ(SizeClass::Index(257), 20);
    EXPECT_EQ(SizeClass::Size(20), 320);
}

TEST(SizeClassTest, MaxSizeBoundary) {
    // MAX_TC_SIZE is 32KB = 32768
    size_t max_size = SizeConfig::MAX_TC_SIZE;
    size_t last_idx = SizeClass::Index(max_size);

    EXPECT_NE(last_idx, std::numeric_limits<size_t>::max());
    EXPECT_EQ(SizeClass::Size(last_idx), max_size);

    // Verify out of bound
    EXPECT_EQ(SizeClass::Index(max_size + 1), std::numeric_limits<size_t>::max());
}

TEST(SizeClassTest, RoundUp) {
    EXPECT_EQ(SizeClass::RoundUp(1), 8);
    EXPECT_EQ(SizeClass::RoundUp(8), 8);
    EXPECT_EQ(SizeClass::RoundUp(129), 160);
    EXPECT_EQ(SizeClass::RoundUp(SizeConfig::MAX_TC_SIZE), SizeConfig::MAX_TC_SIZE);
}

TEST(SizeClassTest, ComprehensiveRoundTrip) {
    // Verify Size(Index(s)) >= s for ALL sizes up to MAX_TC_SIZE
    // And ensure consistency: Index(Size(Index(s))) == Index(s)

    // We can iterate every single byte size since 32KB is small enough for a unit test
    for (size_t s = 1; s <= SizeConfig::MAX_TC_SIZE; ++s) {
        size_t idx = SizeClass::Index(s);

        // 1. Basic sanity
        EXPECT_LT(idx, SizeClass::kNumSizeClasses) << "Index out of bounds for size " << s;

        // 2. Size coverage
        size_t aligned_size = SizeClass::Size(idx);
        EXPECT_GE(aligned_size, s) << "Aligned size smaller than requested for size " << s;

        // 3. Mapping consistency
        // If we request the aligned size, we should get the same index
        EXPECT_EQ(SizeClass::Index(aligned_size), idx) << "Inconsistent mapping for size " << s;

        // 4. Check previous bucket (if not the first one)
        // Ensure that 's' couldn't fit in the previous bucket
        if (idx > 0) {
            size_t prev_aligned_size = SizeClass::Size(idx - 1);
            EXPECT_GT(s, prev_aligned_size) << "Size " << s << " should have fit in index " << (idx - 1);
        }
    }
}

TEST(SizeClassTest, BatchConfiguration) {
    // Validate Batch Logic
    // 1. Smallest object -> Max batch (512)
    EXPECT_EQ(SizeClass::CalculateBatchSize(8), 512);

    // 2. Largest object -> Min batch (2)
    EXPECT_EQ(SizeClass::CalculateBatchSize(SizeConfig::MAX_TC_SIZE), 2);

    // 3. Mid range check
    // e.g., 1024 bytes. 32KB / 1KB = 32 objects.
    EXPECT_EQ(SizeClass::CalculateBatchSize(1024), 32);
}

TEST(SizeClassTest, BatchStrategy) {
    // 小对象：批量数应为 512
    EXPECT_EQ(SizeClass::CalculateBatchSize(8), 512);
    // 大对象：批量数应为 2
    EXPECT_EQ(SizeClass::CalculateBatchSize(32 * 1024), 2);

    // 验证 GetMovePageNum 至少能支撑 8 次 Batch
    size_t size = 8;
    size_t batch = SizeClass::CalculateBatchSize(size);
    size_t pages = SizeClass::GetMovePageNum(size);
    size_t total_bytes = pages * SystemConfig::PAGE_SIZE;
    EXPECT_GE(total_bytes, batch * 8 * size);
}

TEST(SizeClassTest, MovePageConfiguration) {
    // Validate Page Allocation for CentralCache
    // Key requirement: (PageNum * PAGE_SIZE) >= (BatchSize * ObjSize)

    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t obj_size = SizeClass::Size(idx);
        size_t batch_num = SizeClass::CalculateBatchSize(obj_size);
        size_t page_num = SizeClass::GetMovePageNum(obj_size);

        size_t total_alloc_bytes = page_num * SystemConfig::PAGE_SIZE;
        size_t needed_bytes = batch_num * obj_size;

        EXPECT_GE(total_alloc_bytes, needed_bytes)
                << "Not enough pages allocated for batch! Index: " << idx << " Size: " << obj_size;

        // Also check upper bound (should not allocate excessively if not needed)
        // This is a heuristic check, just ensuring we don't return 0 or crazy numbers
        EXPECT_GE(page_num, 1);
        EXPECT_LE(page_num, PageConfig::MAX_PAGE_NUM);
    }
}

TEST(SizeClassTest, FragmentationAnalysis) {
    double total_waste = 0;
    size_t total_alloc = 0;

    for (size_t sz = 1; sz <= 32768; ++sz) {
        size_t bucket = SizeClass::RoundUp(sz);
        total_waste += (bucket - sz);
        total_alloc += bucket;
    }

    double avg_fragmentation = total_waste / total_alloc;
    EXPECT_LT(avg_fragmentation, 0.125);// < 12.5%
}

TEST(SizeClassTest, SafeSizeBounds) {
    EXPECT_EQ(SizeClass::SafeSize(0), 8);
    EXPECT_EQ(SizeClass::SafeSize(15), 128);
    EXPECT_EQ(SizeClass::SafeSize(16), 160);
    EXPECT_EQ(SizeClass::SafeSize(SizeClass::kNumSizeClasses - 1),
              SizeConfig::MAX_TC_SIZE);
}

// ============================================================================
// Invalid Input Contract Tests
// Purpose: Lock down boundary behavior to prevent future regressions
// See: docs/reviews/code_review/20260311_size_class_code_review.md (P2)
// ============================================================================

TEST(SizeClassInvalidInput, RoundUpOverMaxTcSize) {
    // RoundUp returns original size when size > MAX_TC_SIZE (no alignment needed)
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::RoundUp(over_max), over_max);

    // Even larger values should return as-is
    EXPECT_EQ(SizeClass::RoundUp(over_max + 1000), over_max + 1000);
}

TEST(SizeClassInvalidInput, CalculateBatchSizeWithZero) {
    // size == 0 returns 0 batch size (invalid, no allocation possible)
    EXPECT_EQ(SizeClass::CalculateBatchSize(0), 0);
}

TEST(SizeClassInvalidInput, GetMovePageNumWithZero) {
    EXPECT_EQ(SizeClass::GetMovePageNum(0), 0);
}

TEST(SizeClassInvalidInput, IndexReturnsMaxForInvalid) {
    // size > MAX_TC_SIZE returns max() sentinel value
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::Index(over_max), std::numeric_limits<size_t>::max());

    // Much larger values also return max()
    EXPECT_EQ(SizeClass::Index(over_max * 2), std::numeric_limits<size_t>::max());
}

}// namespace
