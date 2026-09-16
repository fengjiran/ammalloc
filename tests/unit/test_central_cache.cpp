#include "ammalloc/central_cache.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/thread_cache.h"

#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using namespace ammalloc;

class CentralCacheTest : public ::testing::Test {
protected:
    CentralCache& central_cache_ = CentralCache::GetInstance();
    PageCache& page_cache_ = PageCache::GetInstance();

    void SetUp() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }

    void TearDown() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
};

void ReleaseOneToBitmap(CentralCache& cache, void* object, size_t size) {
    cache.ReleaseBatch(ObjectBatch::FromSingleObject(object, SizeClass::Index(size)),
                       CentralReleaseMode::kSpanBitmap);
}

void ReleaseFreeListToBitmap(CentralCache& cache, FreeList& list, size_t size) {
    cache.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(size)),
                       CentralReleaseMode::kSpanBitmap);
}

// Test helper: fetches up to `count` objects of `size` into `list` through the
// FetchBatch + PushBatch path and returns how many were delivered. Mirrors the
// retired fetch-into-list entry point so existing assertions on the
// returned count and the resulting list state stay valid, while every fetched
// batch is consumed by PushBatch (so no ObjectBatch leaks out to trip its
// destructor assert).
size_t FetchToList(CentralCache& cache, FreeList& list, size_t count, size_t size) {
    ObjectBatch batch = cache.FetchBatch(SizeClass::Index(size), count);
    const size_t fetched = batch.count();
    list.PushBatch(std::move(batch));
    return fetched;
}

// Point 1: basic FetchBatch.
TEST_F(CentralCacheTest, BasicFetchBatch) {
    FreeList list;
    size_t obj_size = 16;
    size_t batch_num = 10;

    size_t fetched = FetchToList(central_cache_, list, batch_num, obj_size);

    EXPECT_GT(fetched, 0);
    EXPECT_EQ(list.size(), fetched);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

// FetchBatch is the sole fetch boundary after phase 2: verify the returned
// ObjectBatch directly (count, class tag, canonical chain, consumption) instead
// of only observing a destination FreeList through FetchToList.
TEST_F(CentralCacheTest, FetchBatchYieldsCanonicalClassTaggedBatch) {
    const size_t idx = SizeClass::Index(64);
    ObjectBatch batch = central_cache_.FetchBatch(idx, 8);
    ASSERT_FALSE(batch.empty());
    EXPECT_EQ(batch.count(), 8u);
    EXPECT_EQ(batch.size_class_idx(), idx);
    EXPECT_NE(batch.head(), nullptr);
    EXPECT_NE(batch.tail(), nullptr);

    central_cache_.ReleaseBatch(std::move(batch), CentralReleaseMode::kSpanBitmap);
    EXPECT_TRUE(batch.empty());
}

TEST_F(CentralCacheTest, FetchBatchZeroCountYieldsInvalidClassBatch) {
    // preferred_count == 0 yields an empty batch with the invalid-class sentinel,
    // never a partial token, matching the FetchBatch return contract.
    ObjectBatch batch = central_cache_.FetchBatch(SizeClass::Index(64), 0);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(batch.count(), 0u);
    EXPECT_EQ(batch.size_class_idx(), SizeClass::kNumSizeClasses);
    central_cache_.ReleaseBatch(std::move(batch));
}

// Point 2: repeated FetchBatch calls.
TEST_F(CentralCacheTest, MultipleFetchBatch) {
    FreeList list;
    size_t obj_size = 32;

    for (int i = 0; i < 5; ++i) {
        size_t fetched = FetchToList(central_cache_, list, 20, obj_size);
        EXPECT_GT(fetched, 0);
    }

    EXPECT_GE(list.size(), 50);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

// Point 3: basic ReleaseBatch.
TEST_F(CentralCacheTest, BasicReleaseBatch) {
    FreeList list;
    size_t obj_size = 64;
    size_t batch_num = 10;

    // Fetch some objects first.
    size_t fetched = FetchToList(central_cache_, list, batch_num, obj_size);
    ASSERT_GT(fetched, 0);

    // Return them to CentralCache.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));

    // Verify: fetching again must succeed.
    size_t fetched2 = FetchToList(central_cache_, list, batch_num, obj_size);
    EXPECT_GT(fetched2, 0);

    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

// Point 4: allocation across size classes.
TEST_F(CentralCacheTest, DifferentSizeClasses) {
    std::vector<size_t> sizes = {16, 32, 64, 128, 160, 256, 512, 1024, 2048, 4096};

    for (size_t size: sizes) {
        FreeList list;
        size_t fetched = FetchToList(central_cache_, list, 5, size);
        EXPECT_GT(fetched, 0) << "Failed for size " << size;

        central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(size)));
    }
}

// Point 5: large-batch allocation.
TEST_F(CentralCacheTest, LargeBatchAllocation) {
    FreeList list;
    size_t obj_size = 128;
    size_t batch_num = 100;

    size_t fetched = FetchToList(central_cache_, list, batch_num, obj_size);
    EXPECT_GT(fetched, 0);

    // Verify every object is valid, collecting into a list so the batch can be
    // released through the move-only ObjectBatch path.
    FreeList verified;
    size_t count = 0;
    while (!list.empty()) {
        void* obj = list.Pop();
        EXPECT_NE(obj, nullptr);
        verified.Push(obj);
        ++count;
    }
    central_cache_.ReleaseBatch(verified.PopBatch(verified.size(), SizeClass::Index(obj_size)));
    EXPECT_EQ(count, fetched);
}

// Point 6: Reset.
TEST_F(CentralCacheTest, Reset) {
    FreeList list;
    size_t obj_size = 256;

    // Allocate some objects.
    FetchToList(central_cache_, list, 10, obj_size);

    // Drain the list.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));

    // Reset CentralCache.
    central_cache_.Reset();

    // Allocation must keep working afterwards.
    size_t fetched = FetchToList(central_cache_, list, 10, obj_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

TEST_F(CentralCacheTest, TransferCacheOomDegradesToSpanList) {
    // Release the existing backing before enabling the system-allocation hook;
    // Reset then exercises the same no-abort initialization path used by the
    // singleton constructor when backing allocation is unavailable.
    central_cache_.Reset();
    page_cache_.Reset();
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    EXPECT_NO_THROW(central_cache_.Reset());
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    FreeList list;
    ASSERT_GT(FetchToList(central_cache_, list, 1, 64), 0u);
    void* object = list.Pop();
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, SizeClass::Index(64)));
}

TEST_F(CentralCacheTest, DirectBitmapReleaseUnpinsSpanWithoutTransferCache) {
    constexpr size_t kSize = 64;
    const size_t idx = SizeClass::Index(kSize);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, kSize), 1u);
    void* object = list.Pop();
    auto* span = PageMap::GetSpan(object);
    ASSERT_NE(span, nullptr);
    ASSERT_GT(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    // FetchBatch prefetches one additional object. Drain it first, then return
    // the caller's object through the direct path so the Span can reach zero.
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize), kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 0u);
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);

    EXPECT_EQ(span->use_count, 0u);
    EXPECT_FALSE(span->IsUsed());
}

TEST_F(CentralCacheTest, TransferCacheDrainHonorsByteBudgetAndPreservesHealth) {
    constexpr size_t kSize = 64;
    const size_t idx = SizeClass::Index(kSize);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, kSize), 1u);
    void* object = list.Pop();

    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 1u);
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize - 1), 0u);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 1u);
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize), kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);

    FreeList verify;
    EXPECT_GT(FetchToList(central_cache_, verify, 1, kSize), 0u);
    object = verify.Pop();
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());
}

TEST_F(CentralCacheTest, TransferCacheDrainPreservesLifoAcrossColdEndWrap) {
    constexpr size_t kSize = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(kSize);
    FreeList first;
    ASSERT_EQ(FetchToList(central_cache_, first, 8, kSize), 8u);

    central_cache_.ReleaseBatch(first.PopBatch(first.size(), idx));
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 16u);

    EXPECT_EQ(central_cache_.DrainTransferCaches(8 * kSize), 8 * kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 8u);

    FreeList second;
    ASSERT_EQ(FetchToList(central_cache_, second, 4, kSize), 4u);
    FreeList third;
    ASSERT_EQ(FetchToList(central_cache_, third, 8, kSize), 8u);
    // The second request leaves a cold-end offset; the third request needs a
    // Span refill and appends prefetched pointers across the circular boundary.
    ASSERT_GT(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    while (!third.empty()) {
        second.Push(third.Pop());
    }
    central_cache_.ReleaseBatch(second.PopBatch(second.size(), idx),
                                CentralReleaseMode::kSpanBitmap);
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());

    FreeList verify;
    EXPECT_GT(FetchToList(central_cache_, verify, 1, kSize), 0u);
    void* object = verify.Pop();
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());
}

TEST_F(CentralCacheTest, ConcurrentTransferDrainDoesNotNestBucketLocks) {
    constexpr size_t kSize = 64;
    constexpr size_t kIterations = 200;
    std::atomic<bool> producer_ready{false};

    std::thread producer([&] {
        producer_ready.store(true, std::memory_order_release);
        for (size_t i = 0; i < kIterations; ++i) {
            FreeList list;
            const size_t fetched = FetchToList(CentralCache::GetInstance(), list, 8, kSize);
            if (fetched > 0) {
                CentralCache::GetInstance().ReleaseBatch(
                        list.PopBatch(list.size(), SizeClass::Index(kSize)));
            }
        }
    });

    std::thread drainer([&] {
        while (!producer_ready.load(std::memory_order_acquire)) {
        }
        for (size_t i = 0; i < kIterations; ++i) {
            CentralCache::GetInstance().DrainTransferCaches(8 * kSize);
        }
    });

    producer.join();
    drainer.join();
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());
}

// Point 7: reallocate after returning objects.
TEST_F(CentralCacheTest, ReallocateAfterRelease) {
    FreeList list;
    size_t obj_size = 512;

    // First allocation.
    size_t fetched1 = FetchToList(central_cache_, list, 20, obj_size);
    ASSERT_GT(fetched1, 0);

    // Release.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));

    // Second allocation.
    size_t fetched2 = FetchToList(central_cache_, list, 20, obj_size);
    ASSERT_GT(fetched2, 0);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

// Point 8: stress test.
TEST_F(CentralCacheTest, StressTest) {
    std::vector<std::pair<void*, size_t>> allocated;
    std::mt19937 g(42);
    std::uniform_int_distribution<> size_dis(8, 1024);
    std::uniform_int_distribution<> batch_dis(1, 50);

    // Allocate randomly.
    for (int i = 0; i < 100; ++i) {
        size_t obj_size = size_dis(g);
        obj_size = SizeClass::RoundUp(obj_size);

        FreeList list;
        size_t batch_num = batch_dis(g);
        size_t fetched = FetchToList(central_cache_, list, batch_num, obj_size);

        while (!list.empty()) {
            void* obj = list.Pop();
            allocated.emplace_back(obj, obj_size);
        }
    }

    // Release in random order.
    std::shuffle(allocated.begin(), allocated.end(), g);

    // Group releases by size class, then hand each group over as one batch.
    std::map<size_t, FreeList> release_lists;
    for (auto& [obj, size]: allocated) {
        release_lists[size].Push(obj);
    }

    for (auto& [size, list]: release_lists) {
        central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(size)));
    }
}

// Point 9: multi-threaded allocation.
TEST_F(CentralCacheTest, MultiThreadedAllocation) {
    constexpr int num_threads = 4;
    constexpr int allocations_per_thread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&success_count]() {
            for (int i = 0; i < allocations_per_thread; ++i) {
                FreeList list;
                size_t obj_size = 64;
                size_t fetched = FetchToList(CentralCache::GetInstance(), list, 10, obj_size);
                if (fetched > 0) {
                    success_count.fetch_add(fetched);
                }
                // Clean up.
                CentralCache::GetInstance().ReleaseBatch(
                        list.PopBatch(list.size(), SizeClass::Index(obj_size)));
            }
        });
    }

    for (auto& t: threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * allocations_per_thread * 10);
}

// Point 10: basic FreeList operations.
TEST_F(CentralCacheTest, FreeListOperations) {
    FreeList list;

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);

    // Exercise the list with real allocated memory.
    size_t obj_size = 64;
    FreeList source;
    FetchToList(central_cache_, source, 5, obj_size);

    // Push
    void* a = source.Pop();
    void* b = source.Pop();
    void* c = source.Pop();

    list.Push(a);
    list.Push(b);
    list.Push(c);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.size(), 3);

    // Pop (LIFO)
    EXPECT_EQ(list.Pop(), c);
    EXPECT_EQ(list.Pop(), b);
    EXPECT_EQ(list.Pop(), a);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Point 11: FreeList PushRange and PopRange.
TEST_F(CentralCacheTest, FreeListPushRange) {
    FreeList list;
    size_t obj_size = 64;

    // Fetch objects from CentralCache.
    FreeList source;
    FetchToList(central_cache_, source, 5, obj_size);

    void* a = source.Pop();
    void* b = source.Pop();
    void* c = source.Pop();

    // Build the chain: a -> b -> c.
    auto* head = static_cast<FreeBlock*>(a);
    auto* node2 = static_cast<FreeBlock*>(b);
    auto* tail = static_cast<FreeBlock*>(c);

    head->next = node2;
    node2->next = tail;
    tail->next = nullptr;

    list.PushRange(FreeChain{head, tail, 3});

    EXPECT_EQ(list.size(), 3);
    EXPECT_EQ(list.Pop(), head);
    EXPECT_EQ(list.Pop(), node2);
    EXPECT_EQ(list.Pop(), tail);
    EXPECT_TRUE(list.empty());
}

// Point 12: FreeList max_size.
TEST_F(CentralCacheTest, FreeListMaxSize) {
    FreeList list;

    EXPECT_EQ(list.max_size(), 1);

    list.set_max_size(100);
    EXPECT_EQ(list.max_size(), 100);

    list.set_max_size(1000);
    EXPECT_EQ(list.max_size(), 1000);
}

// Point 13: small-object allocation (an 8-byte request maps to the smallest
// 16-byte class).
TEST_F(CentralCacheTest, SmallObjectAllocation) {
    FreeList list;
    size_t obj_size = SizeClass::RoundUp(8);// 8 maps to the 16-byte class; the Span is carved at that class size.

    size_t fetched = FetchToList(central_cache_, list, 50, obj_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(obj_size)));
}

// Point 14: boundary-size allocation.
TEST_F(CentralCacheTest, BoundarySizeAllocation) {
    // Test the largest ThreadCache size.
    size_t max_size = SizeConfig::MAX_TC_SIZE;

    FreeList list;
    size_t fetched = FetchToList(central_cache_, list, 10, max_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(max_size)));
}

// FetchBatch returns a partial batch when supply is short: one Span with a
// single free slot left and PageCache unable to refill (OOM) serve 15 of 16
// requested objects. This is the partial-refill fact the ThreadCache quota
// policy is built on (a partial refill must not grow the quota).
TEST_F(CentralCacheTest, FetchBatchReturnsPartialOnShortSupply) {
    // MAX_TC_SIZE class: batch == 2, span capacity 16, and the Span is exactly
    // the 128-page refill unit (no spare Span left in PageCache). Its mapping
    // (< 1 MiB) goes through AllocNormalPage, so the mock blocks new Span
    // acquisition.
    const size_t size = SizeConfig::MAX_TC_SIZE;
    ASSERT_EQ(SizeClass::CalculateBatchSize(size), 2u);

    // Take one object: the only Span has `capacity - 1` free slots left.
    FreeList one;
    ASSERT_EQ(FetchToList(central_cache_, one, 1, size), 1u);
    void* first = one.Pop();
    auto* span = PageMap::GetSpan(first);
    ASSERT_NE(span, nullptr);
    const size_t capacity = span->capacity;
    one.Push(first);

    // Request `capacity`: only `capacity - 1` are available before the Span is
    // full and the mocked PageCache cannot supply a new one.
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    FreeList list;
    const size_t fetched = FetchToList(central_cache_, list, capacity, size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(fetched, capacity - 1);
    EXPECT_EQ(list.size(), capacity - 1);

    // Clean up.
    central_cache_.ReleaseBatch(one.PopBatch(one.size(), SizeClass::Index(size)));
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), SizeClass::Index(size)));
}

TEST_F(CentralCacheTest, FetchBatchUsesNextPartialSpanBeforePageCacheRefill) {
    constexpr size_t kSpanCapacity = 16;
    const size_t size = SizeConfig::MAX_TC_SIZE;

    // The first fetch fills one Span for the caller and one for prefetch; the
    // second fetch drains that prefetch, leaving two full Spans and an empty
    // TransferCache.
    FreeList first_span_objects;
    FreeList second_span_objects;
    ASSERT_EQ(FetchToList(central_cache_, first_span_objects, kSpanCapacity, size),
              kSpanCapacity);
    ASSERT_EQ(FetchToList(central_cache_, second_span_objects, kSpanCapacity, size),
              kSpanCapacity);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(SizeClass::Index(size)), 0u);

    void* first_free = first_span_objects.Pop();
    void* second_free = second_span_objects.Pop();
    ASSERT_NE(PageMap::GetSpan(first_free), PageMap::GetSpan(second_free));

    // The last release becomes the head candidate. Each Span has exactly one
    // free bitmap slot, so satisfying caller + prefetch requires rotating the
    // first full Span and continuing with the next partial Span.
    ReleaseOneToBitmap(central_cache_, first_free, size);
    ReleaseOneToBitmap(central_cache_, second_free, size);

    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    FreeList fetched_objects;
    const size_t fetched = FetchToList(central_cache_, fetched_objects, 1, size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(fetched, 1u);
    EXPECT_EQ(fetched_objects.size(), 1u);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(SizeClass::Index(size)), 1u);

    EXPECT_EQ(central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max()), size);
    ReleaseFreeListToBitmap(central_cache_, first_span_objects, size);
    ReleaseFreeListToBitmap(central_cache_, second_span_objects, size);
    ReleaseFreeListToBitmap(central_cache_, fetched_objects, size);
}

TEST_F(CentralCacheTest, FetchBatchRotatesSpanFilledAtExtractionBoundary) {
    constexpr size_t kSpanCapacity = 16;
    const size_t size = SizeConfig::MAX_TC_SIZE;

    FreeList first_span_objects;
    FreeList second_span_objects;
    ASSERT_EQ(FetchToList(central_cache_, first_span_objects, kSpanCapacity, size),
              kSpanCapacity);
    ASSERT_EQ(FetchToList(central_cache_, second_span_objects, kSpanCapacity, size),
              kSpanCapacity);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(SizeClass::Index(size)), 0u);

    void* first_free = first_span_objects.Pop();
    void* second_free_a = second_span_objects.Pop();
    void* second_free_b = second_span_objects.Pop();
    ASSERT_NE(PageMap::GetSpan(first_free), PageMap::GetSpan(second_free_a));
    ASSERT_EQ(PageMap::GetSpan(second_free_a), PageMap::GetSpan(second_free_b));

    // Put the Span with two free slots at the front. FetchBatch(1) extracts
    // exactly two objects (caller + prefetch), so this Span becomes full at the
    // extraction boundary without a subsequent failed AllocObject probe.
    ReleaseOneToBitmap(central_cache_, first_free, size);
    ReleaseOneToBitmap(central_cache_, second_free_a, size);
    ReleaseOneToBitmap(central_cache_, second_free_b, size);

    FreeList boundary_objects;
    ASSERT_EQ(FetchToList(central_cache_, boundary_objects, 1, size), 1u);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(SizeClass::Index(size)), 1u);

    // Consume the prefetched object without touching SpanList. The next slow
    // fetch must see the remaining partial Span before attempting PageCache.
    FreeList prefetched_objects;
    ASSERT_EQ(FetchToList(central_cache_, prefetched_objects, 1, size), 1u);

    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    FreeList remaining_candidate;
    const size_t fetched = FetchToList(central_cache_, remaining_candidate, 1, size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(fetched, 1u);
    EXPECT_EQ(remaining_candidate.size(), 1u);

    ReleaseFreeListToBitmap(central_cache_, first_span_objects, size);
    ReleaseFreeListToBitmap(central_cache_, second_span_objects, size);
    ReleaseFreeListToBitmap(central_cache_, boundary_objects, size);
    ReleaseFreeListToBitmap(central_cache_, prefetched_objects, size);
    ReleaseFreeListToBitmap(central_cache_, remaining_candidate, size);
}

// A release chain may exceed kMaxBatchSize: ThreadCache::ReleaseAll hands over a
// whole free list (up to kMaxQuotaBatches batches). The batching loop must
// return every object, not just the first batch.
TEST_F(CentralCacheTest, ReleaseBatchHandlesChainsLongerThanOneBatch) {
    constexpr size_t kObjSize = 16;
    const size_t idx = SizeClass::Index(kObjSize);
    constexpr size_t kBatch = SizeClass::kMaxBatchSize;

    FreeList first;
    FreeList second;
    ASSERT_EQ(FetchToList(central_cache_, first, kBatch, kObjSize), kBatch);
    ASSERT_EQ(FetchToList(central_cache_, second, kBatch, kObjSize), kBatch);

    // One chain twice as long as a single release batch.
    while (!second.empty()) {
        first.Push(second.Pop());
    }
    central_cache_.ReleaseBatch(first.PopBatch(first.size(), idx));

    // A single-batch implementation would stop after kBatch objects and drop
    // the rest; every object must be absorbed instead.
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 2 * kBatch);
}

TEST_F(CentralCacheTest, ReleaseBatchOverflowsToBitmapWhenTransferCacheIsFull) {
    const size_t obj_size = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(obj_size);
    // This class has batch == 2, so TransferCache holds kCapScale * 2 == 16.
    constexpr size_t kTransferCapacity = 16;
    constexpr size_t kChainLength = kTransferCapacity + 4;

    FreeList first;
    FreeList second;
    constexpr size_t kHalf = kChainLength / 2;
    ASSERT_EQ(FetchToList(central_cache_, first, kHalf, obj_size), kHalf);
    ASSERT_EQ(FetchToList(central_cache_, second, kHalf, obj_size), kHalf);

    while (!second.empty()) {
        first.Push(second.Pop());
    }
    central_cache_.ReleaseBatch(first.PopBatch(first.size(), idx));

    // The first 16 objects saturate TransferCache; the remaining 4 must reach
    // the Span bitmaps directly.
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), kTransferCapacity);
    EXPECT_EQ(central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max()),
              kTransferCapacity * obj_size);
}

// TransferCache drain telemetry is the only externally observable record of
// drain work, so its accounting must match the returned byte count.
TEST_F(CentralCacheTest, TransferCacheDrainReportsBytesInStats) {
    constexpr size_t kObjSize = 64;
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, kObjSize), 1u);

    const size_t before =
            CentralCache::GetStats().transfer_cache_drained_bytes.load(std::memory_order_relaxed);
    const size_t drained =
            central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());
    const size_t after =
            CentralCache::GetStats().transfer_cache_drained_bytes.load(std::memory_order_relaxed);

    EXPECT_EQ(drained, kObjSize);
    EXPECT_EQ(after - before, drained);

    ReleaseFreeListToBitmap(central_cache_, list, kObjSize);
}

// Emptying a Span through the direct release path must be counted once.
TEST_F(CentralCacheTest, DirectBitmapReleaseCountsUnpinnedSpansInStats) {
    constexpr size_t kObjSize = 64;
    const size_t idx = SizeClass::Index(kObjSize);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, kObjSize), 1u);
    void* object = list.Pop();

    // Consume the prefetched sibling so the caller's object is the last pin.
    ASSERT_EQ(central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max()), kObjSize);

    const size_t before =
            CentralCache::GetStats().spans_unpinned_by_direct_release.load(std::memory_order_relaxed);
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);
    const size_t after =
            CentralCache::GetStats().spans_unpinned_by_direct_release.load(std::memory_order_relaxed);

    EXPECT_EQ(after - before, 1u);
}

#ifndef NDEBUG
// AM_DCHECK is a debug-only contract: an out-of-range bucket index aborts
// before any bucket access could corrupt unrelated size classes.
TEST_F(CentralCacheTest, FromSingleObjectRejectsOutOfRangeIndex) {
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, 64), 1u);
    void* object = list.Pop();

    // The factory guards the size-class range before any bucket access.
    EXPECT_DEATH(
            ObjectBatch::FromSingleObject(object, SizeClass::kNumSizeClasses),
            "Check failed");

    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, SizeClass::Index(64)),
                                CentralReleaseMode::kSpanBitmap);
}
#endif// NDEBUG

// The drain walks classes from the largest down, so a budget that fits one
// object must be spent on the largest class first.
TEST_F(CentralCacheTest, DrainTransferCachesSpendsBudgetOnLargestClassFirst) {
    constexpr size_t kSmallSize = 64;
    const size_t large_size = SizeConfig::MAX_TC_SIZE;
    const size_t small_idx = SizeClass::Index(kSmallSize);
    const size_t large_idx = SizeClass::Index(large_size);

    FreeList small_list;
    FreeList large_list;
    ASSERT_EQ(FetchToList(central_cache_, small_list, 1, kSmallSize), 1u);
    ASSERT_EQ(FetchToList(central_cache_, large_list, 1, large_size), 1u);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(small_idx), 1u);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(large_idx), 1u);

    // Budget for exactly one large object; smaller classes must stay untouched.
    EXPECT_EQ(central_cache_.DrainTransferCaches(large_size), large_size);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(large_idx), 0u);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(small_idx), 1u);

    ReleaseFreeListToBitmap(central_cache_, small_list, kSmallSize);
    ReleaseFreeListToBitmap(central_cache_, large_list, large_size);
}

TEST_F(CentralCacheTest, DrainTransferCachesWithZeroBudgetIsNoOp) {
    constexpr size_t kObjSize = 64;
    const size_t idx = SizeClass::Index(kObjSize);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, kObjSize), 1u);

    EXPECT_EQ(central_cache_.DrainTransferCaches(0), 0u);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 1u);

    ReleaseFreeListToBitmap(central_cache_, list, kObjSize);
}

// Reset must leave the singleton functional when invoked repeatedly, since
// test teardown and controlled shutdown paths may call it back to back.
TEST_F(CentralCacheTest, ResetIsIdempotent) {
    FreeList list;
    ASSERT_GT(FetchToList(central_cache_, list, 8, 64), 0u);
    ReleaseFreeListToBitmap(central_cache_, list, 64);

    central_cache_.Reset();
    central_cache_.Reset();

    FreeList after;
    EXPECT_GT(FetchToList(central_cache_, after, 8, 64), 0u);
    ReleaseFreeListToBitmap(central_cache_, after, 64);
}

// ObjectBatch is the move-only ownership token handed to CentralCache release.
// These tests drive it through its public creation paths (FromSingleObject,
// AdoptChain, and FreeList::PopBatch/PopBatchTail); every non-empty batch must
// be consumed before it destructs, or the debug invariant aborts.
class ObjectBatchTest : public ::testing::Test {
protected:
    CentralCache& central_cache_ = CentralCache::GetInstance();
    PageCache& page_cache_ = PageCache::GetInstance();

    void SetUp() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
    void TearDown() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
};

TEST(ObjectBatchTraits, IsMoveOnlyAndSmall) {
    EXPECT_FALSE(std::is_copy_constructible_v<ObjectBatch>);
    EXPECT_FALSE(std::is_copy_assignable_v<ObjectBatch>);
    EXPECT_FALSE(std::is_move_assignable_v<ObjectBatch>);
    EXPECT_TRUE(std::is_nothrow_move_constructible_v<ObjectBatch>);
    EXPECT_LE(sizeof(ObjectBatch), 4 * sizeof(void*));
}

TEST_F(ObjectBatchTest, FromSingleObjectYieldsConsumableBatch) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, 64), 1u);
    void* object = list.Pop();

    ObjectBatch batch = ObjectBatch::FromSingleObject(object, idx);
    EXPECT_FALSE(batch.empty());
    EXPECT_EQ(batch.count(), 1u);
    EXPECT_EQ(batch.head(), object);
    EXPECT_EQ(batch.tail(), object);
    EXPECT_EQ(batch.size_class_idx(), idx);

    central_cache_.ReleaseBatch(std::move(batch), CentralReleaseMode::kSpanBitmap);
    EXPECT_TRUE(batch.empty());
}

TEST_F(ObjectBatchTest, MoveConstructionTransfersOwnershipAndEmptiesSource) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, 64), 1u);
    void* object = list.Pop();

    ObjectBatch source = ObjectBatch::FromSingleObject(object, idx);
    ObjectBatch dest(std::move(source));

    EXPECT_TRUE(source.empty());
    EXPECT_EQ(source.count(), 0u);
    EXPECT_EQ(source.head(), nullptr);
    EXPECT_FALSE(dest.empty());
    EXPECT_EQ(dest.head(), object);
    EXPECT_EQ(dest.size_class_idx(), idx);

    central_cache_.ReleaseBatch(std::move(dest), CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, PopBatchTagsWholeListWithClass) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 8, 64), 8u);

    ObjectBatch batch = list.PopBatch(list.size(), idx);
    EXPECT_EQ(batch.count(), 8u);
    EXPECT_EQ(batch.size_class_idx(), idx);
    EXPECT_TRUE(list.empty());

    central_cache_.ReleaseBatch(std::move(batch), CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, PopBatchTailEvictsSuffixOnly) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 8, 64), 8u);

    ObjectBatch batch = list.PopBatchTail(3, idx);
    EXPECT_EQ(batch.count(), 3u);
    EXPECT_EQ(list.size(), 5u);

    central_cache_.ReleaseBatch(std::move(batch), CentralReleaseMode::kSpanBitmap);
    central_cache_.ReleaseBatch(list.PopBatch(list.size(), idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, EmptyListPopBatchYieldsEmptyBatch) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ObjectBatch batch = list.PopBatch(4, idx);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(batch.count(), 0u);
    EXPECT_EQ(batch.head(), nullptr);
    // An empty batch carries the invalid-class sentinel, not the requested idx.
    EXPECT_EQ(batch.size_class_idx(), SizeClass::kNumSizeClasses);
    // Releasing an empty batch is a safe no-op.
    central_cache_.ReleaseBatch(std::move(batch));
}

TEST_F(ObjectBatchTest, AdoptChainWrapsWellFormedChain) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 4, 64), 4u);
    // PopRange yields the canonical transport representation AdoptChain expects.
    const FreeChain chain = list.PopRange(list.size());
    ASSERT_EQ(chain.count, 4u);

    ObjectBatch batch = ObjectBatch::AdoptChain(chain, idx);
    EXPECT_FALSE(batch.empty());
    EXPECT_EQ(batch.count(), 4u);
    EXPECT_EQ(batch.head(), chain.head);
    EXPECT_EQ(batch.tail(), chain.tail);
    EXPECT_EQ(batch.size_class_idx(), idx);

    central_cache_.ReleaseBatch(std::move(batch), CentralReleaseMode::kSpanBitmap);
    EXPECT_TRUE(batch.empty());
}

TEST_F(ObjectBatchTest, AdoptChainEmptyYieldsInvalidClassBatch) {
    // An empty transport chain adopts to an empty batch carrying the invalid-class
    // sentinel, never the requested idx, so canonical form stays consistent.
    ObjectBatch batch = ObjectBatch::AdoptChain(FreeChain{}, SizeClass::Index(64));
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(batch.count(), 0u);
    EXPECT_EQ(batch.size_class_idx(), SizeClass::kNumSizeClasses);
    central_cache_.ReleaseBatch(std::move(batch));
}

TEST_F(ObjectBatchTest, PushBatchPrependsChainAndEmptiesSource) {
    const size_t idx = SizeClass::Index(64);
    FreeList source;
    ASSERT_EQ(FetchToList(central_cache_, source, 4, 64), 4u);
    ObjectBatch batch = source.PopBatch(source.size(), idx);
    ASSERT_EQ(batch.count(), 4u);
    void* const expected_head = batch.head();

    FreeList dest;
    dest.PushBatch(std::move(batch));

    // PushBatch consumes the token and prepends the whole chain LIFO-first.
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(dest.size(), 4u);
    EXPECT_EQ(dest.Pop(), expected_head);

    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(expected_head, idx),
                                CentralReleaseMode::kSpanBitmap);
    central_cache_.ReleaseBatch(dest.PopBatch(dest.size(), idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, PushBatchOntoNonEmptyListAccumulatesSize) {
    const size_t idx = SizeClass::Index(64);
    FreeList dest;
    ASSERT_EQ(FetchToList(central_cache_, dest, 3, 64), 3u);

    FreeList source;
    ASSERT_EQ(FetchToList(central_cache_, source, 4, 64), 4u);
    ObjectBatch batch = source.PopBatch(source.size(), idx);
    void* const expected_head = batch.head();

    dest.PushBatch(std::move(batch));

    // The fetched chain lands in front of the objects dest already held.
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(dest.size(), 7u);
    EXPECT_EQ(dest.Pop(), expected_head);

    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(expected_head, idx),
                                CentralReleaseMode::kSpanBitmap);
    central_cache_.ReleaseBatch(dest.PopBatch(dest.size(), idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, PushBatchEmptyIsNoOp) {
    const size_t idx = SizeClass::Index(64);
    FreeList dest;
    ASSERT_EQ(FetchToList(central_cache_, dest, 2, 64), 2u);

    ObjectBatch empty_batch;
    dest.PushBatch(std::move(empty_batch));

    EXPECT_TRUE(empty_batch.empty());
    EXPECT_EQ(dest.size(), 2u);

    central_cache_.ReleaseBatch(dest.PopBatch(dest.size(), idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, DetachChainForTransportEmptyYieldsEmptyChain) {
    ObjectBatch batch;
    const FreeChain chain = std::move(batch).DetachChainForTransport();
    EXPECT_EQ(chain.count, 0u);
    EXPECT_EQ(chain.head, nullptr);
    EXPECT_EQ(chain.tail, nullptr);
    EXPECT_TRUE(batch.empty());
}

TEST_F(ObjectBatchTest, DetachChainForTransportPreservesChainAndEmptiesSource) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 4, 64), 4u);
    ObjectBatch batch = list.PopBatch(list.size(), idx);
    void* const expected_head = batch.head();
    void* const expected_tail = batch.tail();

    const FreeChain chain = std::move(batch).DetachChainForTransport();

    // The token degrades to a copyable chain and the source is left empty, so it
    // can never be consumed a second time.
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(chain.count, 4u);
    EXPECT_EQ(chain.head, expected_head);
    EXPECT_EQ(chain.tail, expected_tail);

    central_cache_.ReleaseBatch(ObjectBatch::AdoptChain(chain, idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, DetachAdoptRoundTripPreservesChain) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 6, 64), 6u);
    ObjectBatch original = list.PopBatch(list.size(), idx);
    void* const head = original.head();
    void* const tail = original.tail();
    const size_t count = original.count();

    // token -> transport -> token must be lossless across the queue boundary.
    const FreeChain chain = std::move(original).DetachChainForTransport();
    ObjectBatch round_tripped = ObjectBatch::AdoptChain(chain, idx);

    EXPECT_TRUE(original.empty());
    EXPECT_EQ(round_tripped.count(), count);
    EXPECT_EQ(round_tripped.head(), head);
    EXPECT_EQ(round_tripped.tail(), tail);
    EXPECT_EQ(round_tripped.size_class_idx(), idx);

    central_cache_.ReleaseBatch(std::move(round_tripped),
                                CentralReleaseMode::kSpanBitmap);
}

#ifndef NDEBUG
// A forgotten release must be caught: the destructor asserts the batch is empty,
// so letting a non-empty batch leave scope aborts in debug builds.
TEST_F(ObjectBatchTest, UnconsumedNonEmptyBatchAbortsOnDestruction) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 1, 64), 1u);
    void* object = list.Pop();

    EXPECT_DEATH(
            {
                ObjectBatch batch = ObjectBatch::FromSingleObject(object, idx);
                static_cast<void>(batch.count());
                // `batch` leaves scope still holding an object.
            },
            "Check failed");

    // The forked death subprocess does not consume the parent's object.
    central_cache_.ReleaseBatch(ObjectBatch::FromSingleObject(object, idx),
                                CentralReleaseMode::kSpanBitmap);
}

// The single-object factory treats null as a precondition violation.
TEST_F(ObjectBatchTest, FromSingleObjectRejectsNull) {
    EXPECT_DEATH(ObjectBatch::FromSingleObject(nullptr, SizeClass::Index(64)),
                 "Check failed");
}

// AdoptChain is the only public raw-chain edge, so its canonical invariants must
// be enforced: idx range, count/tail agreement, tail reachability, termination.
TEST_F(ObjectBatchTest, AdoptChainRejectsOutOfRangeIndex) {
    // The idx guard fires before any chain inspection.
    EXPECT_DEATH(ObjectBatch::AdoptChain(FreeChain{}, SizeClass::kNumSizeClasses),
                 "Check failed");
}

TEST_F(ObjectBatchTest, AdoptChainRejectsCountMismatch) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 2, 64), 2u);
    const FreeChain chain = list.PopRange(list.size());
    // Overstate count so the canonical walk runs off the terminated chain end.
    EXPECT_DEATH(ObjectBatch::AdoptChain(FreeChain{chain.head, chain.tail, 5}, idx),
                 "Check failed");
    central_cache_.ReleaseBatch(ObjectBatch::AdoptChain(chain, idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, AdoptChainRejectsUnreachableTail) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 2, 64), 2u);
    const FreeChain chain = list.PopRange(list.size());
    // A null tail with a non-zero count is unreachable.
    EXPECT_DEATH(ObjectBatch::AdoptChain(FreeChain{chain.head, nullptr, 2}, idx),
                 "Check failed");
    central_cache_.ReleaseBatch(ObjectBatch::AdoptChain(chain, idx),
                                CentralReleaseMode::kSpanBitmap);
}

TEST_F(ObjectBatchTest, AdoptChainRejectsUnterminatedTail) {
    const size_t idx = SizeClass::Index(64);
    FreeList list;
    ASSERT_EQ(FetchToList(central_cache_, list, 2, 64), 2u);
    const FreeChain chain = list.PopRange(list.size());
    // Claim a 1-object chain ending at head, but head->next is still linked.
    EXPECT_DEATH(ObjectBatch::AdoptChain(FreeChain{chain.head, chain.head, 1}, idx),
                 "Check failed");
    central_cache_.ReleaseBatch(ObjectBatch::AdoptChain(chain, idx),
                                CentralReleaseMode::kSpanBitmap);
}
#endif// NDEBUG

}// namespace
