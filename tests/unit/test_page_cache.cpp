#include "ammalloc/page_cache.h"

#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <random>

namespace {
using namespace ammalloc;

class PageCacheTest : public ::testing::Test {
protected:
    PageCache& cache_ = PageCache::GetInstance();

    void SetUp() override {
        cache_.Reset();
    }

    void TearDown() override {
        cache_.Reset();
    }

    AM_NODISCARD bool IsBucketEmpty(size_t page_num) const {
        return cache_.GetSpanList(page_num).empty();
    }

    AM_NODISCARD size_t GetBucketSize(size_t page_num) const {
        size_t cnt = 0;
        for (auto it = cache_.GetSpanList(page_num).begin();
             it != cache_.GetSpanList(page_num).end(); ++it) {
            ++cnt;
        }
        return cnt;
    }

    AM_NODISCARD Span* GetBucketFrontOrNull(size_t page_num) {
        auto it = cache_.GetSpanList(page_num).begin();
        if (it == cache_.GetSpanList(page_num).end()) {
            return nullptr;
        }
        return &*it;
    }
};

// Point 1: oversized allocation (> 128 pages).
// Oversized requests bypass the buckets: PageAllocator maps them directly and
// they return straight to the OS on release.
TEST_F(PageCacheTest, OversizedAllocation) {
    size_t huge_pages = PageConfig::MAX_PAGE_NUM + 10;
    auto* span = cache_.AllocSpan(huge_pages);
    EXPECT_TRUE(span != nullptr);
    EXPECT_EQ(span->page_num, huge_pages);
    EXPECT_TRUE(span->IsUsed());

    EXPECT_EQ(PageMap::GetSpan(span->GetPageBaseAddr()), span);
    void* last_page_ptr = static_cast<char*>(span->GetPageBaseAddr()) + (huge_pages - 1) * SystemConfig::PAGE_SIZE;
    EXPECT_EQ(PageMap::GetSpan(last_page_ptr), span);

    cache_.ReleaseSpan(span);
    EXPECT_TRUE(PageMap::GetSpan(last_page_ptr) == nullptr);
}

// Point 2: refill and split.
TEST_F(PageCacheTest, RefillAndSplit) {
    // 1. Allocate 1 page.
    // Expected: empty PageCache -> maps 128 pages -> splits off 1 page for the
    // caller -> the remaining 127 pages land in bucket[127].
    Span* span1 = cache_.AllocSpan(1);
    ASSERT_NE(span1, nullptr);
    EXPECT_EQ(span1->page_num, 1);

    // 2. Allocate 10 more pages.
    // Expected: take from bucket[127] -> split off 10 pages -> the remaining
    // 117 pages land in bucket[117].
    Span* span2 = cache_.AllocSpan(10);
    ASSERT_NE(span2, nullptr);
    EXPECT_EQ(span2->page_num, 10);

    // Verify: bucket 127 is empty and bucket 117 is populated.
    // Note: this assumes a cold-start singleton state; leftover state from a
    // previous run could satisfy the request straight from bucket[10].
    EXPECT_TRUE(IsBucketEmpty(127));
    EXPECT_FALSE(IsBucketEmpty(117));

    // Clean up.
    cache_.ReleaseSpan(span1);
    cache_.ReleaseSpan(span2);
}

// Point 3: coalescing.
// A complex scenario that verifies left and right merging.
TEST_F(PageCacheTest, MergeLogic) {
    // Strategy: allocate consecutive spans from one large block, then release
    // them selectively to simulate fragmentation.

    // 1. Allocate 64 pages (half of the largest block).
    Span* spanA = cache_.AllocSpan(64);
    ASSERT_NE(spanA, nullptr);

    // 2. Allocate 32 pages.
    Span* spanB = cache_.AllocSpan(32);
    ASSERT_NE(spanB, nullptr);

    // 3. Allocate another 32 pages.
    Span* spanC = cache_.AllocSpan(32);
    ASSERT_NE(spanC, nullptr);

    const size_t span_a_start = spanA->start_page_idx;
    const size_t span_b_start = spanB->start_page_idx;
    const size_t span_c_start = spanC->start_page_idx;

    // spanA, spanB, and spanC are expected to be physically contiguous,
    // because the split logic carves them off one large mapping in order.
    // Verify contiguity:
    bool is_continuous = (spanA->start_page_idx + spanA->page_num == spanB->start_page_idx) &&
                         (spanB->start_page_idx + spanB->page_num == spanC->start_page_idx);

    EXPECT_TRUE(is_continuous);
    // Non-contiguity (e.g. interleaved allocations from other threads) would
    // make the merge assertions below unverifiable. Single-threaded runs are
    // expected to be contiguous.
    if (!is_continuous) {
        // The order depends on the split implementation. AllocSpan uses head
        // split: the new span takes start_page_idx, so addresses grow. The
        // bucket scan goes upward from page_num + 1 and refills land in
        // bucket[128]: Alloc(64) splits 128 into 64(A) + 64(bucket[64]),
        // Alloc(32) splits 64 into 32(B) + 32(bucket[32]), and Alloc(32)
        // takes 32(C) directly. A -> B -> C is therefore contiguous.
    }

    // 4. Release A (64 pages) -> lands in bucket[64].
    cache_.ReleaseSpan(spanA);

    // 5. Release C (32 pages) -> lands in bucket[32].
    cache_.ReleaseSpan(spanC);

    // B sits between the two free neighbors, so A and C cannot merge yet.

    // 6. Release B (32 pages) -> triggers coalescing: A + B + C = 128 pages.
    cache_.ReleaseSpan(spanB);
    EXPECT_FALSE(IsBucketEmpty(128));

    // Coalescing must remap every absorbed range to the surviving descriptor
    // before that descriptor can be reused by a later PageCache allocation.
    EXPECT_EQ(PageMap::GetSpan(span_a_start), spanB);
    EXPECT_EQ(PageMap::GetSpan(span_b_start), spanB);
    EXPECT_EQ(PageMap::GetSpan(span_c_start), spanB);

    // Verify: bucket[128] should have gained one Span. Requesting 128 pages
    // succeeds iff the three releases coalesced back into the original block.

    Span* spanFull = cache_.AllocSpan(128);
    ASSERT_NE(spanFull, nullptr);

    // If coalescing succeeded, the merged block starts at A's original address.
    EXPECT_EQ(spanFull, spanB);
    EXPECT_EQ(spanFull->start_page_idx, span_a_start);

    cache_.ReleaseSpan(spanFull);
}

// Point 4: PageMap mapping consistency.
TEST_F(PageCacheTest, PageMapConsistency) {
    size_t pages = 4;
    Span* span = cache_.AllocSpan(pages);

    // Every page covered by the span must resolve back to that span.
    for (size_t i = 0; i < pages; ++i) {
        void* addr = static_cast<char*>(span->GetPageBaseAddr()) + i * SystemConfig::PAGE_SIZE;
        EXPECT_EQ(PageMap::GetSpan(addr), span);
    }

    cache_.ReleaseSpan(span);
    // After release, PageMap still points at the same descriptor, now sitting
    // in the free list with its used flag cleared. Verify the state change:
    Span* freed_span = PageMap::GetSpan(span->GetPageBaseAddr());
    EXPECT_FALSE(freed_span->IsUsed());
}

TEST_F(PageCacheTest, ResetClearsMappingsAndIsIdempotent) {
    Span* span = cache_.AllocSpan(4);
    ASSERT_NE(span, nullptr);

    void* addr = span->GetPageBaseAddr();
    EXPECT_EQ(PageMap::GetSpan(addr), span);

    cache_.Reset();
    EXPECT_EQ(PageMap::GetSpan(addr), nullptr);

    cache_.Reset();
    EXPECT_EQ(PageMap::GetSpan(addr), nullptr);
}

TEST_F(PageCacheTest, ExactBucketReuseWhenNeighborsInUse) {
    Span* span_a = cache_.AllocSpan(6);
    Span* span_b = cache_.AllocSpan(6);
    ASSERT_NE(span_a, nullptr);
    ASSERT_NE(span_b, nullptr);

    const size_t page_idx_a = span_a->start_page_idx;
    cache_.ReleaseSpan(span_a);

    Span* span_reuse = cache_.AllocSpan(6);
    ASSERT_NE(span_reuse, nullptr);
    EXPECT_EQ(span_reuse->start_page_idx, page_idx_a);
    EXPECT_TRUE(span_reuse->IsUsed());

    cache_.ReleaseSpan(span_reuse);
    cache_.ReleaseSpan(span_b);
}

TEST_F(PageCacheTest, SplitRemainderIsMappedInPageMap) {
    Span* span1 = cache_.AllocSpan(1);
    Span* span2 = cache_.AllocSpan(10);
    ASSERT_NE(span1, nullptr);
    ASSERT_NE(span2, nullptr);

    auto* remainder = GetBucketFrontOrNull(117);
    ASSERT_NE(remainder, nullptr);
    EXPECT_EQ(remainder->page_num, 117);
    EXPECT_FALSE(remainder->IsUsed());

    void* rem_start = remainder->GetPageBaseAddr();
    void* rem_end = static_cast<char*>(rem_start) + (remainder->page_num - 1) * SystemConfig::PAGE_SIZE;
    EXPECT_EQ(PageMap::GetSpan(rem_start), remainder);
    EXPECT_EQ(PageMap::GetSpan(rem_end), remainder);

    cache_.ReleaseSpan(span1);
    cache_.ReleaseSpan(span2);
}

TEST_F(PageCacheTest, ReleaseResetsSpanMetadataWithoutMerge) {
    Span* span_a = cache_.AllocSpan(8);
    Span* span_b = cache_.AllocSpan(8);
    Span* span_c = cache_.AllocSpan(8);
    ASSERT_NE(span_a, nullptr);
    ASSERT_NE(span_b, nullptr);
    ASSERT_NE(span_c, nullptr);

    cache_.ReleaseSpan(span_b);

    auto* free_span = GetBucketFrontOrNull(8);
    ASSERT_NE(free_span, nullptr);
    EXPECT_EQ(free_span->page_num, 8);
    EXPECT_FALSE(free_span->IsUsed());
    EXPECT_TRUE(free_span->IsCommitted());

    Span* reused = cache_.AllocSpan(8);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, free_span);
    EXPECT_TRUE(reused->IsUsed());

    cache_.ReleaseSpan(span_a);
    cache_.ReleaseSpan(reused);
    cache_.ReleaseSpan(span_c);
}

TEST_F(PageCacheTest, UnknownAddressReturnsNullFromPageMap) {
    int stack_value = 42;
    EXPECT_EQ(PageMap::GetSpan(&stack_value), nullptr);
}

TEST_F(PageCacheTest, OversizedOverflowRequestReturnsNullAndStateRemainsUsable) {
    constexpr size_t too_many_pages = (std::numeric_limits<size_t>::max() >> SystemConfig::PAGE_SHIFT) + 1;
    Span* failed = cache_.AllocSpan(too_many_pages);
    EXPECT_EQ(failed, nullptr);

    Span* ok = cache_.AllocSpan(2);
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok->page_num, 2);
    EXPECT_EQ(PageMap::GetSpan(ok->GetPageBaseAddr()), ok);
    cache_.ReleaseSpan(ok);
}

TEST_F(PageCacheTest, ClearRangeOnEmptyPageMapKeepsLookupNull) {
    cache_.Reset();

    PageMap::ClearRange(123456, 64);
    EXPECT_EQ(PageMap::GetSpan(static_cast<size_t>(123456)), nullptr);
    EXPECT_EQ(PageMap::GetSpan(static_cast<size_t>(123456 + 63)), nullptr);
}

TEST_F(PageCacheTest, ClearRangeAfterUnmapMakesLookupNull) {
    Span* span = cache_.AllocSpan(5);
    ASSERT_NE(span, nullptr);

    const size_t start = span->start_page_idx;
    EXPECT_EQ(PageMap::GetSpan(start), span);
    EXPECT_EQ(PageMap::GetSpan(start + 4), span);

    PageMap::ClearRange(start, 5);
    EXPECT_EQ(PageMap::GetSpan(start), nullptr);
    EXPECT_EQ(PageMap::GetSpan(start + 4), nullptr);

    ASSERT_TRUE(PageMap::EnsureRange(span->start_page_idx, span->page_num));
    PageMap::SetSpan(span);
    EXPECT_EQ(PageMap::GetSpan(start), span);
    cache_.ReleaseSpan(span);
}

TEST_F(PageCacheTest, EnsureRangeFailureLeavesSpanLeafUnpublished) {
    constexpr size_t kPageId = 123456;
    for (size_t successful_allocations = 0; successful_allocations < 3;
         ++successful_allocations) {
        cache_.Reset();
        PageMap::SetRadixAllocationFailureForTest(successful_allocations);

        EXPECT_FALSE(PageMap::EnsureRange(kPageId, 1));
        EXPECT_EQ(PageMap::GetSpan(kPageId), nullptr);
    }

    PageMap::SetRadixAllocationFailureForTest(std::numeric_limits<size_t>::max());
    ASSERT_TRUE(PageMap::EnsureRange(kPageId, 1));
    Span span{kPageId, 1};
    PageMap::SetSpan(&span);
    EXPECT_EQ(PageMap::GetSpan(kPageId), &span);
    PageMap::ClearRange(kPageId, 1);
}

TEST_F(PageCacheTest, FreshSpanRollsBackWhenPageMapMetadataOom) {
    PageAllocator::ResetStats();
    PageMap::SetRadixAllocationFailureForTest(0);

    EXPECT_EQ(cache_.AllocSpan(1), nullptr);
    const auto& stats = PageAllocator::GetStats();
    // One mapping backs Span metadata and remains pool-owned for reuse; the
    // second is the fresh 128-page Span mapping and must be rolled back.
    EXPECT_EQ(stats.normal_alloc_success.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(stats.free_count.load(std::memory_order_relaxed), 1u);

    PageMap::SetRadixAllocationFailureForTest(std::numeric_limits<size_t>::max());
    Span* span = cache_.AllocSpan(1);
    ASSERT_NE(span, nullptr);
    EXPECT_EQ(PageMap::GetSpan(span->GetPageBaseAddr()), span);
    cache_.ReleaseSpan(span);
}

// Point 5: stress test with random allocation and release.
TEST_F(PageCacheTest, RandomStress) {
    std::vector<Span*> spans;
    std::mt19937 g(42);
    std::uniform_int_distribution<> dis(1, 20);

    for (int i = 0; i < 1000; ++i) {
        // Randomly allocate 1..20 pages.
        size_t k = dis(g);
        Span* s = cache_.AllocSpan(k);
        spans.push_back(s);
    }

    // Release in random order.
    std::shuffle(spans.begin(), spans.end(), g);
    for (auto* s: spans) {
        cache_.ReleaseSpan(s);
    }

    // Final check: everything is released and nothing leaks (verify under ASan).
}

}// namespace
