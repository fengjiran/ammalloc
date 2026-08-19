#include "ammalloc/config.h"
#include "ammalloc/size_class.h"

#include <gtest/gtest.h>

namespace {
using namespace ammalloc;

// Linear-region bucket count: [1, 128] with one bucket per ALIGNMENT byte.
constexpr size_t kLinear = 128 / SystemConfig::ALIGNMENT;

TEST(SizeClassTest, SmallObjectMapping) {
    // Size 0 is policy-mapped to the minimum class (one ALIGNMENT unit).
    EXPECT_EQ(SizeClass::Index(0), 0);
    EXPECT_EQ(SizeClass::Index(1), 0);
    EXPECT_EQ(SizeClass::Index(SystemConfig::ALIGNMENT), 0);
    EXPECT_EQ(SizeClass::Size(0), SystemConfig::ALIGNMENT);

    // Boundary between the last two linear buckets and the 128-byte top.
    EXPECT_EQ(SizeClass::Index(128 - SystemConfig::ALIGNMENT), kLinear - 2);
    EXPECT_EQ(SizeClass::Index(128 - SystemConfig::ALIGNMENT + 1), kLinear - 1);
    EXPECT_EQ(SizeClass::Index(128), kLinear - 1);
    EXPECT_EQ(SizeClass::Size(kLinear - 1), 128);
}

TEST(SizeClassTest, LargeObjectMapping) {
    // Group [129, 256]: step = (256 - 128) / 4 = 32, so buckets are
    // 160/192/224/256; the next group [257, 512] uses step 64 (bucket 320).
    EXPECT_EQ(SizeClass::Index(129), kLinear);
    EXPECT_EQ(SizeClass::Index(160), kLinear);
    EXPECT_EQ(SizeClass::Size(kLinear), 160);

    EXPECT_EQ(SizeClass::Index(161), kLinear + 1);
    EXPECT_EQ(SizeClass::Index(192), kLinear + 1);
    EXPECT_EQ(SizeClass::Size(kLinear + 1), 192);

    EXPECT_EQ(SizeClass::Index(256), kLinear + 3);
    EXPECT_EQ(SizeClass::Size(kLinear + 3), 256);

    EXPECT_EQ(SizeClass::Index(257), kLinear + 4);
    EXPECT_EQ(SizeClass::Size(kLinear + 4), 320);
}

TEST(SizeClassTest, MaxSizeBoundary) {
    size_t max_size = SizeConfig::MAX_TC_SIZE;
    size_t last_idx = SizeClass::Index(max_size);

    EXPECT_NE(last_idx, std::numeric_limits<size_t>::max());
    EXPECT_EQ(SizeClass::Size(last_idx), max_size);

    EXPECT_EQ(SizeClass::Index(max_size + 1), std::numeric_limits<size_t>::max());
}

TEST(SizeClassTest, RoundUp) {
    EXPECT_EQ(SizeClass::RoundUp(1), SystemConfig::ALIGNMENT);
    EXPECT_EQ(SizeClass::RoundUp(SystemConfig::ALIGNMENT), SystemConfig::ALIGNMENT);
    EXPECT_EQ(SizeClass::RoundUp(129), 160);
    EXPECT_EQ(SizeClass::RoundUp(SizeConfig::MAX_TC_SIZE), SizeConfig::MAX_TC_SIZE);
}

TEST(SizeClassTest, AllClassesAlignedToSystemAlignment) {
    // Every class size must be a multiple of ALIGNMENT so that objects
    // carved from an ALIGNMENT-aligned span base stay ALIGNMENT-aligned.
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        EXPECT_EQ(SizeClass::Size(idx) % SystemConfig::ALIGNMENT, 0)
                << "Size class " << idx << " is not a multiple of ALIGNMENT";
    }
}

TEST(SizeClassTest, ComprehensiveRoundTrip) {
    // Exhaustive round-trip over every byte size up to MAX_TC_SIZE:
    // Size(idx) must cover s, remapping must be stable, and s must not
    // fit into the previous, smaller bucket.
    for (size_t s = 1; s <= SizeConfig::MAX_TC_SIZE; ++s) {
        size_t idx = SizeClass::Index(s);

        EXPECT_LT(idx, SizeClass::kNumSizeClasses) << "Index out of bounds for size " << s;

        size_t aligned_size = SizeClass::Size(idx);
        EXPECT_GE(aligned_size, s) << "Aligned size smaller than requested for size " << s;
        EXPECT_EQ(SizeClass::Index(aligned_size), idx) << "Inconsistent mapping for size " << s;

        if (idx > 0) {
            size_t prev_aligned_size = SizeClass::Size(idx - 1);
            EXPECT_GT(s, prev_aligned_size) << "Size " << s << " should have fit in index " << (idx - 1);
        }
    }
}

TEST(SizeClassTest, BatchConfiguration) {
    // Smallest class is clamped to the 512 cap.
    EXPECT_EQ(SizeClass::CalculateBatchSize(8), 512);

    // Largest class is clamped to the 2 floor.
    EXPECT_EQ(SizeClass::CalculateBatchSize(SizeConfig::MAX_TC_SIZE), 2);

    // Mid range: 32 KiB / 1 KiB = 32 objects per batch.
    EXPECT_EQ(SizeClass::CalculateBatchSize(1024), 32);
}

TEST(SizeClassTest, BatchStrategy) {
    EXPECT_EQ(SizeClass::CalculateBatchSize(8), 512);
    EXPECT_EQ(SizeClass::CalculateBatchSize(32 * 1024), 2);

    // A span must cover at least 8 batch transfers.
    size_t size = 8;
    size_t batch = SizeClass::CalculateBatchSize(size);
    size_t pages = SizeClass::GetMovePageNum(size);
    size_t total_bytes = pages * SystemConfig::PAGE_SIZE;
    EXPECT_GE(total_bytes, batch * 8 * size);
}

TEST(SizeClassTest, MovePageConfiguration) {
    // Invariant: one span must hold at least one full batch of objects.
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t obj_size = SizeClass::Size(idx);
        size_t batch_num = SizeClass::CalculateBatchSize(obj_size);
        size_t page_num = SizeClass::GetMovePageNum(obj_size);

        size_t total_alloc_bytes = page_num * SystemConfig::PAGE_SIZE;
        size_t needed_bytes = batch_num * obj_size;

        EXPECT_GE(total_alloc_bytes, needed_bytes)
                << "Not enough pages allocated for batch! Index: " << idx << " Size: " << obj_size;

        // Heuristic bounds: never zero pages, never more than the largest
        // retained span.
        EXPECT_GE(page_num, 1);
        EXPECT_LE(page_num, PageConfig::MAX_PAGE_NUM);
    }
}

TEST(SizeClassTest, BatchMonotonicNonIncreasing) {
    // Invariant: batch size may only shrink as the class grows (larger
    // objects are costlier to hoard). Policy-independent, unlike the
    // concrete-value checks in BatchConfiguration.
    for (size_t idx = 1; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t prev_batch = SizeClass::CalculateBatchSize(SizeClass::Size(idx - 1));
        size_t curr_batch = SizeClass::CalculateBatchSize(SizeClass::Size(idx));
        EXPECT_LE(curr_batch, prev_batch) << "Batch grew at index " << idx;
    }
}

TEST(SizeClassTest, MovePageMinFloor32KiB) {
    // Invariant: every class reserves at least one 32 KiB span so a single
    // span serves repeated refills. Policy-independent, unlike exact page counts.
    constexpr size_t kMinSpanBytes = 32 * 1024;
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t pages = SizeClass::GetMovePageNum(SizeClass::Size(idx));
        EXPECT_GE(pages * SystemConfig::PAGE_SIZE, kMinSpanBytes)
                << "Span below 32 KiB floor at index " << idx;
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
    EXPECT_LT(avg_fragmentation, 0.125);// Average fragmentation stays below 12.5%.
}

TEST(SizeClassTest, SafeSizeBounds) {
    EXPECT_EQ(SizeClass::SafeSize(0), SystemConfig::ALIGNMENT);
    EXPECT_EQ(SizeClass::SafeSize(kLinear - 1), 128);
    EXPECT_EQ(SizeClass::SafeSize(kLinear), 160);
    EXPECT_EQ(SizeClass::SafeSize(SizeClass::kNumSizeClasses - 1),
              SizeConfig::MAX_TC_SIZE);
}

TEST(SizeClassTest, SafeSizeOutOfRange_Death) {
    // SafeSize uses AM_CHECK, which is active in every build (not
    // debug-only), so no #ifndef NDEBUG guard is needed here.
    EXPECT_DEATH(SizeClass::SafeSize(SizeClass::kNumSizeClasses), "Check failed");
}

// Invalid-input contract tests: lock down boundary behavior to prevent
// future regressions.

TEST(SizeClassInvalidInput, RoundUpOverMaxTcSize) {
    // Oversize requests pass through unaligned.
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::RoundUp(over_max), over_max);

    EXPECT_EQ(SizeClass::RoundUp(over_max + 1000), over_max + 1000);
}

TEST(SizeClassInvalidInput, CalculateBatchSizeWithZero) {
    // Size 0 is invalid input for the batch policy.
    EXPECT_EQ(SizeClass::CalculateBatchSize(0), 0);
}

TEST(SizeClassInvalidInput, GetMovePageNumWithZero) {
    EXPECT_EQ(SizeClass::GetMovePageNum(0), 0);
}

TEST(SizeClassInvalidInput, IndexReturnsMaxForInvalid) {
    // Oversize requests map to the max() sentinel.
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::Index(over_max), std::numeric_limits<size_t>::max());

    EXPECT_EQ(SizeClass::Index(over_max * 2), std::numeric_limits<size_t>::max());
}

TEST(SizeClassInvalidInput, CalculateBatchSizeOverMaxTcSize) {
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::CalculateBatchSize(over_max), 0);
}

TEST(SizeClassInvalidInput, GetMovePageNumOverMaxTcSize) {
    size_t over_max = SizeConfig::MAX_TC_SIZE + 1;
    EXPECT_EQ(SizeClass::GetMovePageNum(over_max), 0);
}

}// namespace
