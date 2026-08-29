#include "ammalloc/config.h"
#include "ammalloc/page_allocator.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace {
using namespace ammalloc;

struct PooledTestNode {
    PooledTestNode* next{nullptr};
};

// Page-sized, page-aligned node mirroring RadixNode's storage demands.
struct alignas(SystemConfig::PAGE_SIZE) PageAlignedTestNode {
    char data[SystemConfig::PAGE_SIZE];
};

// Reports whether two slots are adjacent in one chunk's bump region.
bool AreAdjacentSlots(const void* a, const void* b) noexcept {
    return reinterpret_cast<const char*>(b) ==
           reinterpret_cast<const char*>(a) + sizeof(PooledTestNode);
}

class PageAllocatorTest : public ::testing::Test {
public:
    void SetUp() override {
        PageAllocator::ResetStats();
        PageAllocator::ReleaseHugePageCache();
        g_mock_huge_alloc_fail.store(false, std::memory_order_relaxed);
        g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);
    }

    void TearDown() override {
        PageAllocator::ReleaseHugePageCache();
    }

    // Helper: validates pointer sanity.
    bool IsValidPtr(void* ptr) {
        return ptr != nullptr && ptr != MAP_FAILED;
    }

    // Helper: simulates huge-page allocation failure (test-only).
    static void MockHugePageAllocFail() {
        // The global flag forces AllocHugePage to return nullptr.
        g_mock_huge_alloc_fail.store(true, std::memory_order_relaxed);
    }

    static void ResetMock() {
        g_mock_huge_alloc_fail.store(false, std::memory_order_relaxed);
    }
};

// ========== Case 1: normal-page allocation/free (no cache) ==========
TEST_F(PageAllocatorTest, NormalPageAllocFree) {
    size_t page_num = 1;
    void* ptr = PageAllocator::SystemAlloc(page_num);
    // 1. Verify the pointer is non-null.
    EXPECT_TRUE(IsValidPtr(ptr));

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.normal_alloc_count.load(), 1);
    EXPECT_EQ(stats.normal_alloc_success.load(), 1);
    EXPECT_EQ(stats.normal_alloc_bytes.load(), SystemConfig::PAGE_SIZE * page_num);
    EXPECT_EQ(stats.huge_alloc_count.load(), 0);// No huge-page request.

    // 2. Verify read/write access (guards against a mapping without backing).
    // Write a pattern.
    int* int_ptr = static_cast<int*>(ptr);
    *int_ptr = 0xDEADBEEF;
    EXPECT_EQ(*int_ptr, 0xDEADBEEF);

    // Fill the whole page to catch any segfault.
    std::memset(ptr, 0xAB, page_num * SystemConfig::PAGE_SIZE);
    for (size_t i = 0; i < page_num * SystemConfig::PAGE_SIZE; ++i) {
        EXPECT_EQ(static_cast<unsigned char*>(ptr)[i], 0xAB);
    }

    PageAllocator::SystemFree(ptr, page_num);
    EXPECT_EQ(stats.free_count.load(), 1);
    EXPECT_EQ(stats.free_bytes.load(), SystemConfig::PAGE_SIZE * page_num);

    EXPECT_EQ(stats.huge_cache_hit_count.load(), 0);
    EXPECT_EQ(stats.huge_cache_miss_count.load(), 0);
}

// ========== Case 2: huge-page allocation/free (cache miss) ==========
TEST_F(PageAllocatorTest, HugePageAllocFree_MissCache) {
    size_t page_num = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
    void* ptr = PageAllocator::SystemAlloc(page_num);
    EXPECT_TRUE(IsValidPtr(ptr));

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.huge_alloc_count.load(), 1);
    EXPECT_EQ(stats.huge_alloc_success.load(), 1);
    EXPECT_EQ(stats.huge_alloc_bytes.load(), SystemConfig::HUGE_PAGE_SIZE);
    EXPECT_EQ(stats.huge_cache_miss_count.load(), 1);
    EXPECT_EQ(stats.huge_cache_hit_count.load(), 0);

    PageAllocator::SystemFree(ptr, page_num);
    EXPECT_EQ(stats.free_count.load(), 1);
    EXPECT_EQ(stats.free_bytes.load(), SystemConfig::HUGE_PAGE_SIZE);
}

// ========== Case 3: huge-page allocation (cache hit) ==========
TEST_F(PageAllocatorTest, HugePageAlloc_HitCache) {
    size_t page_num = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
    void* ptr1 = PageAllocator::SystemAlloc(page_num);
    EXPECT_TRUE(IsValidPtr(ptr1));
    PageAllocator::SystemFree(ptr1, page_num);

    void* ptr2 = PageAllocator::SystemAlloc(page_num);
    EXPECT_TRUE(IsValidPtr(ptr2));

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.huge_cache_hit_count.load(), 1);
    EXPECT_EQ(stats.huge_cache_miss_count.load(), 1);// First access misses.
    EXPECT_EQ(stats.huge_alloc_count.load(), 1);     // A cache hit triggers no new allocation.

    PageAllocator::SystemFree(ptr2, page_num);
}

// ========== Case 4: huge-page failure -> normal-page fallback ==========
TEST_F(PageAllocatorTest, HugePageAllocFail_FallbackToNormal) {
    MockHugePageAllocFail();

    size_t page_num = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
    void* ptr = PageAllocator::SystemAlloc(page_num);
    EXPECT_TRUE(IsValidPtr(ptr));

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.huge_alloc_count.load(), 1);
    EXPECT_EQ(stats.huge_alloc_success.load(), 0);           // Huge-page allocation failed.
    EXPECT_EQ(stats.huge_fallback_to_normal_count.load(), 1);// Fallback count.
    EXPECT_EQ(stats.normal_alloc_count.load(), 1);
    EXPECT_EQ(stats.normal_alloc_success.load(), 1);

    ResetMock();
    PageAllocator::SystemFree(ptr, page_num);
}

// ========== Case 5: cache cleanup (global cache) ==========
TEST_F(PageAllocatorTest, HugeCacheCleanup) {
    size_t page_num = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
    std::vector<void*> ptrs;
    for (int i = 0; i < PageConfig::HUGE_PAGE_CACHE_SIZE; ++i) {
        void* p = PageAllocator::SystemAlloc(page_num);
        ptrs.push_back(p);
        PageAllocator::SystemFree(p, page_num);
    }

    void* p = PageAllocator::SystemAlloc(page_num);
    PageAllocator::SystemFree(p, page_num);
    PageAllocator::ReleaseHugePageCache();
}

// ========== Case 6: boundary conditions (page_num=0/null free) ==========
TEST_F(PageAllocatorTest, BoundaryConditions) {
    // 1. Allocate 0 pages.
    void* ptr1 = PageAllocator::SystemAlloc(0);
    EXPECT_EQ(ptr1, nullptr);

    // 2. Free a null pointer.
    PageAllocator::SystemFree(nullptr, 1);
    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.free_count.load(), 0);// Invalid frees are not counted.

    // 3. Free with page_num = 0.
    void* ptr2 = PageAllocator::SystemAlloc(1);
    PageAllocator::SystemFree(ptr2, 0);
    EXPECT_EQ(stats.free_count.load(), 0);// Invalid frees are not counted.

    // Clean up.
    PageAllocator::SystemFree(ptr2, 1);
}

TEST_F(PageAllocatorTest, ObjectPoolTryNewReturnsNullOnBackingOom) {
    ObjectPool<PooledTestNode> pool;
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);

    EXPECT_EQ(pool.TryNew(), nullptr);

    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);
    auto* node = pool.TryNew();
    ASSERT_NE(node, nullptr);
    pool.Delete(node);
}

TEST_F(PageAllocatorTest, ObjectPoolDeleteMakesStorageImmediatelyReusable) {
    ObjectPool<PooledTestNode> pool;
    auto* first = pool.TryNew();
    ASSERT_NE(first, nullptr);

    pool.Delete(first);
    auto* second = pool.TryNew();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first);

    pool.Delete(second);
}

// ========== ObjectPool pooling tests ==========

TEST_F(PageAllocatorTest, ObjectPoolGrowsNewChunkWhenExhausted) {
    ObjectPool<PooledTestNode, 4096> pool;
    std::vector<PooledTestNode*> nodes;
    bool new_chunk_seen = false;
    for (size_t i = 0; i < 4096; ++i) {
        PooledTestNode* node = pool.TryNew();
        ASSERT_NE(node, nullptr);
        nodes.push_back(node);
        // Within one chunk the bump pointer yields strictly adjacent slots;
        // a gap marks the first slot of a fresh chunk.
        if (i > 0 && !AreAdjacentSlots(nodes[i - 1], nodes[i])) {
            new_chunk_seen = true;
            break;
        }
    }
    EXPECT_TRUE(new_chunk_seen);

    // Every handed-out slot must be distinct.
    std::sort(nodes.begin(), nodes.end());
    EXPECT_EQ(std::adjacent_find(nodes.begin(), nodes.end()), nodes.end());

    for (PooledTestNode* node: nodes) {
        pool.Delete(node);
    }
}

TEST_F(PageAllocatorTest, ObjectPoolReusesFreedSlotsAcrossChunksInLifoOrder) {
    ObjectPool<PooledTestNode, 4096> pool;
    std::vector<PooledTestNode*> nodes;
    size_t guard = 0;
    // Consume the first chunk; the loop ends with nodes.back() being the
    // first slot of the second chunk.
    while (nodes.size() < 2 ||
           AreAdjacentSlots(nodes[nodes.size() - 2], nodes.back())) {
        ASSERT_LT(guard++, 4096u) << "chunk boundary not reached";
        PooledTestNode* node = pool.TryNew();
        ASSERT_NE(node, nullptr);
        nodes.push_back(node);
    }

    PooledTestNode* chunk2_tail1 = pool.TryNew();
    PooledTestNode* chunk2_tail2 = pool.TryNew();
    ASSERT_NE(chunk2_tail1, nullptr);
    ASSERT_NE(chunk2_tail2, nullptr);

    PooledTestNode* first_of_chunk1 = nodes.front();
    pool.Delete(first_of_chunk1);
    pool.Delete(chunk2_tail2);

    PooledTestNode* reused1 = pool.TryNew();
    PooledTestNode* reused2 = pool.TryNew();
    ASSERT_NE(reused1, nullptr);
    ASSERT_NE(reused2, nullptr);
    EXPECT_EQ(reused1, chunk2_tail2);    // LIFO: last deleted returned first
    EXPECT_EQ(reused2, first_of_chunk1);// then the earlier deletion

    pool.Delete(reused1);
    pool.Delete(reused2);
}

TEST_F(PageAllocatorTest, ObjectPoolReleaseMemoryKeepsPoolReusable) {
    ObjectPool<PooledTestNode, 4096> pool;
    PooledTestNode* a = pool.TryNew();
    PooledTestNode* b = pool.TryNew();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    pool.Delete(b);// Leave one slot on the free list before releasing.

    pool.ReleaseMemory();
    pool.ReleaseMemory();// Releasing an empty pool must be a no-op.

    PooledTestNode* c = pool.TryNew();
    ASSERT_NE(c, nullptr);
    c->next = a;// The recycled slot must be writable.
    EXPECT_EQ(c->next, a);
    pool.Delete(c);
}

TEST_F(PageAllocatorTest, ObjectPoolDestructorReleasesAllChunks) {
    {
        ObjectPool<PooledTestNode, 4096> pool;
        for (int i = 0; i < 1500; ++i) {// 4 KiB chunk holds ~1022 slots; covers two chunks.
            ASSERT_NE(pool.TryNew(), nullptr);
        }
    }
    // Chunks are munmapped by the destructor: LeakSanitizer verifies no leak,
    // and a double-free during teardown would abort.
}

TEST_F(PageAllocatorTest, ObjectPoolKeepsPageAlignedSlotsForPageSizedType) {
    ObjectPool<PageAlignedTestNode> pool;
    std::vector<PageAlignedTestNode*> nodes;
    // 20 slots exceed one chunk's 17-slot capacity, covering the
    // cross-chunk allocation path for page-sized objects.
    for (size_t i = 0; i < 20; ++i) {
        PageAlignedTestNode* node = pool.TryNew();
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(node) & (SystemConfig::PAGE_SIZE - 1),
                  0u);
        nodes.push_back(node);
    }
    for (PageAlignedTestNode* node: nodes) {
        pool.Delete(node);
    }
}

// ========== Test suite: thread safety ==========
class PageAllocatorThreadSafeTest : public ::testing::Test {
protected:
    static constexpr int THREAD_NUM = 8;        // 8 threads.
    static constexpr int ALLOC_PER_THREAD = 100;// 100 allocations per thread.

    void SetUp() override {
        PageAllocator::ResetStats();
        PageAllocator::ReleaseHugePageCache();
    }

    void TearDown() override {
        PageAllocator::ReleaseHugePageCache();
    }

    static bool IsValidPtr(void* ptr) {
        return ptr != nullptr && ptr != MAP_FAILED;
    }

    static void ThreadFunc(std::atomic<int>& counter) {
        size_t page_num = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
        for (int i = 0; i < ALLOC_PER_THREAD; ++i) {
            void* ptr = PageAllocator::SystemAlloc(page_num);
            if (IsValidPtr(ptr)) {
                PageAllocator::SystemFree(ptr, page_num);
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};

// ========== Case 7: concurrent multi-threaded allocation/free ==========
TEST_F(PageAllocatorThreadSafeTest, ConcurrentAllocFree) {
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    // Start 8 threads.
    for (int i = 0; i < THREAD_NUM; ++i) {
        threads.emplace_back(ThreadFunc, std::ref(success_count));
    }

    // Wait for all threads to finish.
    for (auto& t: threads) {
        t.join();
    }

    // ========== Core check: thread-safety baseline (must hold) ==========
    // 1. Successes == threads * allocations per thread (no allocation/free
    //    failures).
    const int total_expected = THREAD_NUM * ALLOC_PER_THREAD;
    EXPECT_EQ(success_count.load(std::memory_order_relaxed), total_expected);

    // 2. Frees == successes (each allocation has a matching free).
    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.free_count.load(std::memory_order_relaxed), total_expected);

    // ========== Cache-aware statistics checks ==========
    // Key relation: total huge-page requests = cache hits + cache misses.
    // (Every successful allocation is a huge-page request: the thread body
    // only allocates huge pages.)
    size_t total_huge_request = stats.huge_cache_hit_count.load() + stats.huge_cache_miss_count.load();
    EXPECT_EQ(total_huge_request, total_expected);

    // Key relation: cache misses = real huge-page allocations
    // (huge_alloc_count). AllocHugePage runs only on a miss, incrementing
    // huge_alloc_count.
    EXPECT_EQ(stats.huge_cache_miss_count.load(), stats.huge_alloc_count.load());

    // Extra check: no normal-page allocations (the thread body only asks for
    // huge pages).
    EXPECT_EQ(stats.normal_alloc_count.load(), 0);

    // The run completes without crashes or data races.
    SUCCEED();
}

TEST_F(PageAllocatorTest, AllocHugeAlignment) {
    // 1. Compute the threshold that triggers huge-page handling:
    //    size >= HUGE_PAGE_SIZE / 2.
    size_t huge_size = SystemConfig::HUGE_PAGE_SIZE;
    size_t page_num = huge_size >> SystemConfig::PAGE_SHIFT;// Request 2MB (usually 512 pages).
    void* ptr = PageAllocator::SystemAlloc(page_num);
    EXPECT_TRUE(ptr != nullptr);

    // 2. Verify alignment (core check).
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t alignment = SystemConfig::HUGE_PAGE_SIZE;

    // The address must be divisible by 2MB.
    EXPECT_EQ(addr % alignment, 0)
            << "Pointer " << ptr << " is NOT aligned to " << alignment;

    // 3. Verify first/last-byte access (the trim logic must not cut needed
    //    memory).
    char* char_ptr = static_cast<char*>(ptr);
    size_t total_bytes = page_num * SystemConfig::PAGE_SIZE;
    // Write the head.
    char_ptr[0] = 'A';
    // Write the tail (last byte).
    char_ptr[total_bytes - 1] = 'Z';

    EXPECT_EQ(char_ptr[0], 'A');
    EXPECT_EQ(char_ptr[total_bytes - 1], 'Z');

    PageAllocator::SystemFree(ptr, page_num);
}

TEST_F(PageAllocatorTest, MultipleAllocations) {
    std::vector<std::pair<void*, size_t>> allocations;

    // Mixed allocation: 1, 10, 512 (huge) pages.
    std::vector<size_t> sizes = {1, 10, 128, 512, 600};

    for (size_t pages: sizes) {
        void* ptr = PageAllocator::SystemAlloc(pages);
        EXPECT_TRUE(ptr != nullptr);

        // Simple write check.
        static_cast<char*>(ptr)[0] = 0xFF;

        // For large mappings, also verify alignment.
        size_t bytes = pages << SystemConfig::PAGE_SHIFT;
        if (bytes >= (SystemConfig::HUGE_PAGE_SIZE >> 1)) {
            EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % SystemConfig::HUGE_PAGE_SIZE, 0);
        }

        allocations.emplace_back(ptr, pages);
    }

    // Release everything.
    for (auto& [fst, snd]: allocations) {
        PageAllocator::SystemFree(fst, snd);
    }
}

TEST_F(PageAllocatorTest, InvalidArgs) {
    // 1. Allocate 0 pages. The implementation rejects a zero page count
    //    before it reaches mmap, so the result is null.
    void* ptr = PageAllocator::SystemAlloc(0);
    EXPECT_TRUE(ptr == nullptr);

    // 2. Free a null pointer (must not crash).
    PageAllocator::SystemFree(nullptr, 100);

    // 3. Free with page_num = 0 (must not crash).
    char dummy;
    PageAllocator::SystemFree(&dummy, 0);
}

TEST_F(PageAllocatorTest, OverflowGuard_SystemAlloc) {
    constexpr size_t kTooManyPages = (std::numeric_limits<size_t>::max() >> SystemConfig::PAGE_SHIFT) + 1;
    void* ptr = PageAllocator::SystemAlloc(kTooManyPages);
    EXPECT_EQ(ptr, nullptr);

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.normal_alloc_count.load(), 0);
    EXPECT_EQ(stats.huge_alloc_count.load(), 0);
    EXPECT_EQ(stats.alloc_failed_count.load(), 0);
}

TEST_F(PageAllocatorTest, OverflowGuard_SystemFree) {
    constexpr size_t kTooManyPages = (std::numeric_limits<size_t>::max() >> SystemConfig::PAGE_SHIFT) + 1;
    char dummy = 0;

    PageAllocator::SystemFree(&dummy, kTooManyPages);

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.free_count.load(), 0);
    EXPECT_EQ(stats.free_bytes.load(), 0);
    EXPECT_EQ(stats.munmap_failed_count.load(), 0);
}

TEST_F(PageAllocatorTest, NonHugeSizedFreeDoesNotPopulateHugeCache) {
    constexpr size_t kNonHugePages =
            (SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE) + 88;
    constexpr size_t kHugePages = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;

    void* non_huge_ptr = PageAllocator::SystemAlloc(kNonHugePages);
    ASSERT_TRUE(IsValidPtr(non_huge_ptr));
    PageAllocator::SystemFree(non_huge_ptr, kNonHugePages);

    void* huge_ptr = PageAllocator::SystemAlloc(kHugePages);
    ASSERT_TRUE(IsValidPtr(huge_ptr));

    const auto& stats = PageAllocator::GetStats();
    EXPECT_EQ(stats.huge_cache_hit_count.load(), 0);
    EXPECT_EQ(stats.huge_cache_miss_count.load(), 1);

    PageAllocator::SystemFree(huge_ptr, kHugePages);
}

TEST_F(PageAllocatorTest, AdjacentHugeBoundaryDoesNotPopulateHugeCache) {
    constexpr size_t kHugePages = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
    const size_t kAdjacentPages[2] = {kHugePages - 1, kHugePages + 1};

    for (size_t pages: kAdjacentPages) {
        PageAllocator::ResetStats();
        PageAllocator::ReleaseHugePageCache();

        void* non_huge_ptr = PageAllocator::SystemAlloc(pages);
        ASSERT_TRUE(IsValidPtr(non_huge_ptr));
        PageAllocator::SystemFree(non_huge_ptr, pages);

        void* huge_ptr = PageAllocator::SystemAlloc(kHugePages);
        ASSERT_TRUE(IsValidPtr(huge_ptr));

        const auto& stats = PageAllocator::GetStats();
        EXPECT_EQ(stats.huge_cache_hit_count.load(), 0);
        EXPECT_EQ(stats.huge_cache_miss_count.load(), 1);

        PageAllocator::SystemFree(huge_ptr, kHugePages);
    }
}

TEST_F(PageAllocatorTest, AllocWithPopulateConfig) {
    // setenv is not thread-safe; this test runs alone, so it is fine here.
    setenv("AM_USE_MAP_POPULATE", "1", 1);

    // RuntimeConfig reads the environment at first use. This test assumes it
    // is the first use in the process (the singleton does not support reset).
    void* ptr = PageAllocator::SystemAlloc(10);// Should trigger MAP_POPULATE.
    EXPECT_TRUE(ptr != nullptr);

    PageAllocator::SystemFree(ptr, 10);

    // Clean up the environment.
    unsetenv("AM_USE_MAP_POPULATE");
}

}// namespace
