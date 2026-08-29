#include "ammalloc/ammalloc.h"
#include "ammalloc/config.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/size_class.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

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

// am_free resolves the pointer's owning Span through PageMap and dispatches to
// its recorded size class. Hardened builds abort on interior/out-of-range
// frees instead of corrupting metadata. These death tests are active in Debug
// and in AM_HARDENED release builds; in a plain release build the checks are
// compiled out, so the bodies are empty.
TEST(AmMallocHardeningTest, LargeObjectInteriorPointerDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    void* p = am_malloc(SizeConfig::MAX_TC_SIZE + 1);
    ASSERT_NE(p, nullptr);
    EXPECT_DEATH(am_free(static_cast<char*>(p) + 8), "Check failed");
    am_free(p);
#endif
}

TEST(AmMallocHardeningTest, SmallObjectInteriorPointerDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    void* p = am_malloc(64);
    ASSERT_NE(p, nullptr);
    EXPECT_DEATH(am_free(static_cast<char*>(p) + 1), "Check failed");
    am_free(p);
#endif
}

// This models the state after a small Span's final object returned to its
// bitmap and CentralCache handed it back to PageCache. A second free must
// reject the retained free-Span mapping before examining recycled metadata.
TEST(AmMallocHardeningTest, ReleasedSmallSpanSecondFreeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    auto& cache = PageCache::GetInstance();
    auto* span = cache.AllocSpan(1);
    ASSERT_NE(span, nullptr);
    span->Init(64);

    void* p = span->AllocObject();
    ASSERT_NE(p, nullptr);
    span->FreeObject(p);
    cache.ReleaseSpan(span);

    EXPECT_DEATH(am_free(p), "Check failed");
#endif
}

// An uncarved PageCache Span follows am_free's large-object branch because its
// aligned_obj_size is zero. Retaining this cacheable range exercises the
// used-state check; direct-mapped large allocations clear their PageMap entry
// and remain unsupported rather than deterministically detectable on reuse.
TEST(AmMallocHardeningTest, ReleasedLargeSpanSecondFreeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    auto& cache = PageCache::GetInstance();
    auto* span = cache.AllocSpan(1);
    ASSERT_NE(span, nullptr);
    void* p = span->GetPageBaseAddr();

    am_free(p);
    EXPECT_DEATH(am_free(p), "Check failed");
#endif
}

// am_free cross-checks span->size_class_idx against span->aligned_obj_size
// before pushing into ThreadCache, so corrupted Span metadata aborts instead
// of feeding the object into a wrong-size class. The corruption happens in the
// parent before EXPECT_DEATH forks, so the field is restored before the final
// am_free of the (untouched) parent object.
TEST(AmMallocHardeningTest, CorruptedSizeClassIndexOutOfRangeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    void* p = am_malloc(64);
    ASSERT_NE(p, nullptr);
    auto* span = PageMap::GetSpan(p);
    ASSERT_NE(span, nullptr);

    const auto saved = span->size_class_idx;
    span->size_class_idx = static_cast<uint16_t>(SizeClass::kNumSizeClasses);
    EXPECT_DEATH(am_free(p), "Check failed");
    span->size_class_idx = saved;
    am_free(p);
#endif
}

// The freeing thread begins with no ThreadCache. The metadata check must run
// before am_free dispatches to am_free_slow_path, where a corrupted index would
// otherwise bypass the fast-path-only guard.
TEST(AmMallocHardeningTest, CorruptedSizeClassIndexOnThreadFirstFreeDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    void* p = am_malloc(64);
    ASSERT_NE(p, nullptr);
    auto* span = PageMap::GetSpan(p);
    ASSERT_NE(span, nullptr);

    const auto saved = span->size_class_idx;
    span->size_class_idx = static_cast<uint16_t>(SizeClass::kNumSizeClasses);
    EXPECT_DEATH(
            {
                std::thread freer([p] { am_free(p); });
                freer.join();
            },
            "Check failed");
    span->size_class_idx = saved;
    am_free(p);
#endif
}

TEST(AmMallocHardeningTest, CorruptedSizeClassMismatchDeath) {
#if defined(AM_HARDENED) || !defined(NDEBUG)
    void* p = am_malloc(64);
    ASSERT_NE(p, nullptr);
    auto* span = PageMap::GetSpan(p);
    ASSERT_NE(span, nullptr);

    const auto saved = span->size_class_idx;
    span->size_class_idx = static_cast<uint16_t>(SizeClass::Index(128));
    EXPECT_DEATH(am_free(p), "Check failed");
    span->size_class_idx = saved;
    am_free(p);
#endif
}
