#include "ammalloc/central_cache.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/thread_cache.h"

#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <random>
#include <thread>
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

// Point 1: basic FetchRange.
TEST_F(CentralCacheTest, BasicFetchRange) {
    FreeList list;
    size_t obj_size = 16;
    size_t batch_num = 10;

    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);

    EXPECT_GT(fetched, 0);
    EXPECT_EQ(list.size(), fetched);

    // Clean up.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// Point 2: repeated FetchRange calls.
TEST_F(CentralCacheTest, MultipleFetchRange) {
    FreeList list;
    size_t obj_size = 32;

    for (int i = 0; i < 5; ++i) {
        size_t fetched = central_cache_.FetchRange(list, 20, obj_size);
        EXPECT_GT(fetched, 0);
    }

    EXPECT_GE(list.size(), 50);

    // Clean up.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// Point 3: basic ReleaseListToSpans.
TEST_F(CentralCacheTest, BasicReleaseListToSpans) {
    FreeList list;
    size_t obj_size = 64;
    size_t batch_num = 10;

    // Fetch some objects first.
    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);
    ASSERT_GT(fetched, 0);

    // Build the release chain.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }

    // Return them to CentralCache.
    central_cache_.ReleaseListToSpans(head, obj_size);

    // Verify: fetching again must succeed.
    size_t fetched2 = central_cache_.FetchRange(list, batch_num, obj_size);
    EXPECT_GT(fetched2, 0);

    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// Point 4: allocation across size classes.
TEST_F(CentralCacheTest, DifferentSizeClasses) {
    std::vector<size_t> sizes = {16, 32, 64, 128, 160, 256, 512, 1024, 2048, 4096};

    for (size_t size: sizes) {
        FreeList list;
        size_t fetched = central_cache_.FetchRange(list, 5, size);
        EXPECT_GT(fetched, 0) << "Failed for size " << size;

        void* head = nullptr;
        while (!list.empty()) {
            void* obj = list.pop();
            auto* block = static_cast<FreeBlock*>(obj);
            block->next = static_cast<FreeBlock*>(head);
            head = obj;
        }
        central_cache_.ReleaseListToSpans(head, size);
    }
}

// Point 5: large-batch allocation.
TEST_F(CentralCacheTest, LargeBatchAllocation) {
    FreeList list;
    size_t obj_size = 128;
    size_t batch_num = 100;

    size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);
    EXPECT_GT(fetched, 0);

    // Verify every object is valid.
    void* head = nullptr;
    size_t count = 0;
    while (!list.empty()) {
        void* obj = list.pop();
        EXPECT_NE(obj, nullptr);
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
        ++count;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
    EXPECT_EQ(count, fetched);
}

// Point 6: Reset.
TEST_F(CentralCacheTest, Reset) {
    FreeList list;
    size_t obj_size = 256;

    // Allocate some objects.
    central_cache_.FetchRange(list, 10, obj_size);

    // Drain the list.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);

    // Reset CentralCache.
    central_cache_.Reset();

    // Allocation must keep working afterwards.
    size_t fetched = central_cache_.FetchRange(list, 10, obj_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
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
    ASSERT_GT(central_cache_.FetchRange(list, 1, 64), 0u);
    void* object = list.pop();
    static_cast<FreeBlock*>(object)->next = nullptr;
    central_cache_.ReleaseListToSpans(object, 64);
}

TEST_F(CentralCacheTest, DirectBitmapReleaseUnpinsSpanWithoutTransferCache) {
    constexpr size_t kSize = 64;
    const size_t idx = SizeClass::Index(kSize);
    FreeList list;
    ASSERT_EQ(central_cache_.FetchRange(list, 1, kSize), 1u);
    void* object = list.pop();
    auto* span = PageMap::GetSpan(object);
    ASSERT_NE(span, nullptr);
    ASSERT_GT(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    // FetchRange prefetches one additional object. Drain it first, then return
    // the caller's object through the direct path so the Span can reach zero.
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize), kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 0u);
    static_cast<FreeBlock*>(object)->next = nullptr;
    central_cache_.ReleaseListToSpans(object, kSize, CentralReleaseMode::kSpanBitmap);

    EXPECT_EQ(span->use_count, 0u);
    EXPECT_FALSE(span->IsUsed());
}

TEST_F(CentralCacheTest, TransferCacheDrainHonorsByteBudgetAndPreservesHealth) {
    constexpr size_t kSize = 64;
    const size_t idx = SizeClass::Index(kSize);
    FreeList list;
    ASSERT_EQ(central_cache_.FetchRange(list, 1, kSize), 1u);
    void* object = list.pop();

    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 1u);
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize - 1), 0u);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 1u);
    EXPECT_EQ(central_cache_.DrainTransferCaches(kSize), kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    static_cast<FreeBlock*>(object)->next = nullptr;
    central_cache_.ReleaseListToSpans(object, kSize, CentralReleaseMode::kSpanBitmap);

    FreeList verify;
    EXPECT_GT(central_cache_.FetchRange(verify, 1, kSize), 0u);
    object = verify.pop();
    static_cast<FreeBlock*>(object)->next = nullptr;
    central_cache_.ReleaseListToSpans(object, kSize, CentralReleaseMode::kSpanBitmap);
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());
}

TEST_F(CentralCacheTest, TransferCacheDrainPreservesLifoAcrossColdEndWrap) {
    constexpr size_t kSize = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(kSize);
    FreeList first;
    ASSERT_EQ(central_cache_.FetchRange(first, 8, kSize), 8u);

    void* head = nullptr;
    while (!first.empty()) {
        auto* object = static_cast<FreeBlock*>(first.pop());
        object->next = static_cast<FreeBlock*>(head);
        head = object;
    }
    central_cache_.ReleaseListToSpans(head, kSize);
    ASSERT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 16u);

    EXPECT_EQ(central_cache_.DrainTransferCaches(8 * kSize), 8 * kSize);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 8u);

    FreeList second;
    ASSERT_EQ(central_cache_.FetchRange(second, 4, kSize), 4u);
    FreeList third;
    ASSERT_EQ(central_cache_.FetchRange(third, 8, kSize), 8u);
    // The second request leaves a cold-end offset; the third request needs a
    // Span refill and appends prefetched pointers across the circular boundary.
    ASSERT_GT(central_cache_.GetTransferCacheCountForTest(idx), 0u);

    head = nullptr;
    while (!second.empty()) {
        auto* object = static_cast<FreeBlock*>(second.pop());
        object->next = static_cast<FreeBlock*>(head);
        head = object;
    }
    while (!third.empty()) {
        auto* object = static_cast<FreeBlock*>(third.pop());
        object->next = static_cast<FreeBlock*>(head);
        head = object;
    }
    central_cache_.ReleaseListToSpans(head, kSize, CentralReleaseMode::kSpanBitmap);
    central_cache_.DrainTransferCaches(std::numeric_limits<size_t>::max());

    FreeList verify;
    EXPECT_GT(central_cache_.FetchRange(verify, 1, kSize), 0u);
    auto* object = static_cast<FreeBlock*>(verify.pop());
    object->next = nullptr;
    central_cache_.ReleaseListToSpans(object, kSize, CentralReleaseMode::kSpanBitmap);
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
            const size_t fetched = CentralCache::GetInstance().FetchRange(list, 8, kSize);
            void* head = nullptr;
            while (!list.empty()) {
                auto* object = static_cast<FreeBlock*>(list.pop());
                object->next = static_cast<FreeBlock*>(head);
                head = object;
            }
            if (fetched > 0) {
                CentralCache::GetInstance().ReleaseListToSpans(head, kSize);
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
    size_t fetched1 = central_cache_.FetchRange(list, 20, obj_size);
    ASSERT_GT(fetched1, 0);

    // Build the release chain.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }

    // Release.
    central_cache_.ReleaseListToSpans(head, obj_size);

    // Second allocation.
    size_t fetched2 = central_cache_.FetchRange(list, 20, obj_size);
    ASSERT_GT(fetched2, 0);

    // Clean up.
    head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
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
        size_t fetched = central_cache_.FetchRange(list, batch_num, obj_size);

        while (!list.empty()) {
            void* obj = list.pop();
            allocated.emplace_back(obj, obj_size);
        }
    }

    // Release in random order.
    std::shuffle(allocated.begin(), allocated.end(), g);

    // Group releases by size.
    std::map<size_t, void*> release_lists;
    std::map<size_t, void*> release_tails;
    std::map<size_t, size_t> release_counts;

    for (auto& [obj, size]: allocated) {
        auto* block = static_cast<FreeBlock*>(obj);
        if (release_lists.find(size) == release_lists.end()) {
            release_lists[size] = obj;
            release_tails[size] = obj;
            block->next = nullptr;
        } else {
            block->next = static_cast<FreeBlock*>(release_lists[size]);
            release_lists[size] = obj;
        }
        ++release_counts[size];
    }

    for (auto& [size, head]: release_lists) {
        central_cache_.ReleaseListToSpans(head, size);
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
                size_t fetched = CentralCache::GetInstance().FetchRange(list, 10, obj_size);
                if (fetched > 0) {
                    success_count.fetch_add(fetched);
                }
                // Clean up.
                void* head = nullptr;
                while (!list.empty()) {
                    void* obj = list.pop();
                    auto* block = static_cast<FreeBlock*>(obj);
                    block->next = static_cast<FreeBlock*>(head);
                    head = obj;
                }
                CentralCache::GetInstance().ReleaseListToSpans(head, obj_size);
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
    central_cache_.FetchRange(source, 5, obj_size);

    // Push
    void* a = source.pop();
    void* b = source.pop();
    void* c = source.pop();

    list.push(a);
    list.push(b);
    list.push(c);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.size(), 3);

    // Pop (LIFO)
    EXPECT_EQ(list.pop(), c);
    EXPECT_EQ(list.pop(), b);
    EXPECT_EQ(list.pop(), a);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0);
}

// Point 11: FreeList push_range and pop_range.
TEST_F(CentralCacheTest, FreeListPushRange) {
    FreeList list;
    size_t obj_size = 64;

    // Fetch objects from CentralCache.
    FreeList source;
    central_cache_.FetchRange(source, 5, obj_size);

    void* a = source.pop();
    void* b = source.pop();
    void* c = source.pop();

    // Build the chain: a -> b -> c.
    auto* head = static_cast<FreeBlock*>(a);
    auto* node2 = static_cast<FreeBlock*>(b);
    auto* tail = static_cast<FreeBlock*>(c);

    head->next = node2;
    node2->next = tail;
    tail->next = nullptr;

    list.push_range(FreeChain{head, tail, 3});

    EXPECT_EQ(list.size(), 3);
    EXPECT_EQ(list.pop(), head);
    EXPECT_EQ(list.pop(), node2);
    EXPECT_EQ(list.pop(), tail);
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

    size_t fetched = central_cache_.FetchRange(list, 50, obj_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, obj_size);
}

// Point 14: boundary-size allocation.
TEST_F(CentralCacheTest, BoundarySizeAllocation) {
    // Test the largest ThreadCache size.
    size_t max_size = SizeConfig::MAX_TC_SIZE;

    FreeList list;
    size_t fetched = central_cache_.FetchRange(list, 10, max_size);
    EXPECT_GT(fetched, 0);

    // Clean up.
    void* head = nullptr;
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, max_size);
}

// FetchRange returns a partial batch when supply is short: one Span with a
// single free slot left and PageCache unable to refill (OOM) serve 15 of 16
// requested objects. This is the partial-refill fact the ThreadCache quota
// policy is built on (a partial refill must not grow the quota).
TEST_F(CentralCacheTest, FetchRangeReturnsPartialOnShortSupply) {
    // MAX_TC_SIZE class: batch == 2, span capacity 16, and the Span is exactly
    // the 128-page refill unit (no spare Span left in PageCache). Its mapping
    // (< 1 MiB) goes through AllocNormalPage, so the mock blocks new Span
    // acquisition.
    const size_t size = SizeConfig::MAX_TC_SIZE;
    ASSERT_EQ(SizeClass::CalculateBatchSize(size), 2u);

    // Take one object: the only Span has `capacity - 1` free slots left.
    FreeList one;
    ASSERT_EQ(central_cache_.FetchRange(one, 1, size), 1u);
    void* first = one.pop();
    auto* span = PageMap::GetSpan(first);
    ASSERT_NE(span, nullptr);
    const size_t capacity = span->capacity;
    one.push(first);

    // Request `capacity`: only `capacity - 1` are available before the Span is
    // full and the mocked PageCache cannot supply a new one.
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    FreeList list;
    const size_t fetched = central_cache_.FetchRange(list, capacity, size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(fetched, capacity - 1);
    EXPECT_EQ(list.size(), capacity - 1);

    // Clean up.
    void* head = nullptr;
    while (!one.empty()) {
        void* obj = one.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    while (!list.empty()) {
        void* obj = list.pop();
        auto* block = static_cast<FreeBlock*>(obj);
        block->next = static_cast<FreeBlock*>(head);
        head = obj;
    }
    central_cache_.ReleaseListToSpans(head, size);
}

}// namespace
