#include "ammalloc/ammalloc.h"
#include "ammalloc/config.h"
#include "ammalloc/page_allocator.h"

#include <gtest/gtest.h>

namespace {

using namespace ammalloc;

// Allocate `count` objects of `size` at once, verify every returned address
// is ALIGNMENT-aligned, then release them all. Holding many objects at the
// same time is essential: allocating and immediately freeing would keep
// returning the first slot, which is aligned even when odd slots
// (data_start + odd * size) are not.
void ExpectAlignedAllocations(size_t size, size_t count) {
    std::vector<void*> held;
    held.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        void* p = am_malloc(size);
        ASSERT_NE(p, nullptr) << "am_malloc(" << size << ") failed at slot " << i;
        held.push_back(p);
    }

    for (void* p: held) {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % SystemConfig::ALIGNMENT, 0)
                << "misaligned address for request size " << size;
    }

    for (void* p: held) {
        am_free(p);
    }
}

}// namespace

TEST(AmMallocAlignmentTest, SmallSizesReturnAlignedAddresses) {
    // Sweep 0..512: the linear region [1, 128] (one class per ALIGNMENT
    // bytes) plus the first geometric groups. 64 simultaneous objects per
    // size exercise many slots of the owning span, including slot 0 which is
    // the span data start, and cross the ThreadCache -> CentralCache batch
    // refill boundary.
    for (size_t req = 0; req <= 512; ++req) {
        ExpectAlignedAllocations(req, 64);
    }
}

TEST(AmMallocAlignmentTest, BoundarySizesReturnAlignedAddresses) {
    // MAX_TC_SIZE stays on the thread-cache path; MAX_TC_SIZE + 1 takes the
    // direct page-allocation path (whole pages, no slot carving), which must
    // honor the same alignment contract.
    ExpectAlignedAllocations(SizeConfig::MAX_TC_SIZE, 64);
    ExpectAlignedAllocations(SizeConfig::MAX_TC_SIZE + 1, 64);
}

// CreateThreadCache allocates the ThreadCache metadata page via
// PageAllocator::SystemAlloc; when that fails the API must degrade to the OOM
// contract (null/central-cache fallback) instead of crashing. The mock injects
// failure into AllocNormalPage, the path used for the single 4KiB page. A fresh
// thread has no TLS ThreadCache, so the slow path really reaches SystemAlloc.
TEST(AmMallocOomTest, MallocReturnsNullWhenThreadCacheInitFails) {
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);

    std::thread worker([] {
        EXPECT_EQ(am_malloc(64), nullptr);
    });
    worker.join();

    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);
}

TEST(AmMallocOomTest, FreeFallsBackToCentralCacheWhenThreadCacheInitFails) {
    // Allocate from the main thread so the object lives in a real Span.
    void* p = am_malloc(64);
    ASSERT_NE(p, nullptr);

    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    std::thread worker([p] {
        // CreateThreadCache fails inside am_free_slow_path, which must degrade
        // to CentralCache::ReleaseListToSpans rather than crash.
        am_free(p);

        // Prove the worker never built a ThreadCache: a subsequent allocation
        // still fails, so the free really took the slow path.
        EXPECT_EQ(am_malloc(64), nullptr);
    });

    worker.join();
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    // The allocator stays healthy once the failure injection is lifted.
    std::thread verifier([] {
        void* q = am_malloc(64);
        EXPECT_NE(q, nullptr);
        am_free(q);
    });
    verifier.join();
}

// Thread exit with a live cache runs ThreadCacheCleaner, which must drain every
// FreeList back to CentralCache (ReleaseAll) and free the metadata page
// (ReleaseThreadCache) without crashing or leaking. The OOM tests above only
// cover the injected-failure branch, not this teardown happy path.
TEST(AmMallocThreadExitTest, ThreadExitDrainsCacheToCentralCache) {
    std::thread worker([] {
        constexpr size_t kSize = 64;
        constexpr int kAllocs = 2000;
        std::vector<void*> held;
        held.reserve(kAllocs);
        for (int i = 0; i < kAllocs; ++i) {
            void* p = am_malloc(kSize);
            EXPECT_NE(p, nullptr);
            if (p) held.push_back(p);
        }
        for (void* p : held) am_free(p);
    });
    worker.join();
    ExpectAlignedAllocations(64, 64);
}

// Cross-thread free: an object allocated on one thread is freed on another, so
// am_free re-reads span->size_class_idx and pushes the object into the freeing
// thread's own FreeList (cache-ownership migration). The freer starts with no
// ThreadCache, exercising am_free_slow_path -> CreateThreadCache.
TEST(AmMallocCrossThreadFreeTest, FreeOnDifferentThread) {
    constexpr size_t kSize = 64;
    constexpr int kCount = 100;
    std::vector<void*> held;
    held.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        void* p = am_malloc(kSize);
        ASSERT_NE(p, nullptr);
        static_cast<char*>(p)[0] = 'a';
        held.push_back(p);
    }
    std::thread freer([&held] {
        for (void* p : held) am_free(p);
    });
    freer.join();
    void* q = am_malloc(kSize);
    ASSERT_NE(q, nullptr);
    am_free(q);
}
