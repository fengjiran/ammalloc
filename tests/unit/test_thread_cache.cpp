#include "ammalloc/ammalloc.h"
#include "ammalloc/central_cache.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/thread_cache.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <random>
#include <ranges>

namespace {
using namespace ammalloc;

class ThreadCacheTest : public ::testing::Test {
protected:
    PageCache& page_cache_ = PageCache::GetInstance();
    CentralCache& central_cache_ = CentralCache::GetInstance();

    void SetUp() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }

    void TearDown() override {
        central_cache_.Reset();
        page_cache_.Reset();
    }
};

TEST(ThreadCacheDeathTest, DestructionRequiresEmptyFreeLists) {
#ifndef NDEBUG
    EXPECT_DEATH(
            {
                ThreadCache cache;
                void* ptr = cache.Allocate(64);
                if (!ptr) {
                    std::_Exit(0);
                }
                cache.Deallocate(ptr, SizeClass::Index(64));
            },
            "Check failed");
#endif
}

// Counterpart of the negative guard above: draining via ReleaseAll() must
// satisfy the destructor precondition in every build (the debug DCHECK is
// compiled out under NDEBUG, so this case is meaningful in both modes).
TEST(ThreadCacheDeathTest, DestructionAfterReleaseAllExitsCleanly) {
    EXPECT_EXIT(
            {
                {
                    ThreadCache cache;
                    void* ptr = cache.Allocate(64);
                    if (!ptr) {
                        std::_Exit(0);
                    }
                    cache.Deallocate(ptr, SizeClass::Index(64));
                    cache.ReleaseAll();
                }// Destructor runs here and must not trip the guard.
                std::_Exit(0);
            },
            ::testing::ExitedWithCode(0),
            "");
}

// Test 1: basic Allocate.
TEST_F(ThreadCacheTest, BasicAllocate) {
    thread_local ThreadCache cache;

    void* ptr = cache.Allocate(16);
    EXPECT_TRUE(ptr != nullptr);

    cache.Deallocate(ptr, SizeClass::Index(16));
    cache.ReleaseAll();
}

// Test 2: Allocate(0) returns a valid pointer (16-byte block).
TEST_F(ThreadCacheTest, AllocateZero) {
    thread_local ThreadCache cache;

    void* ptr = cache.Allocate(0);
    EXPECT_TRUE(ptr != nullptr);

    cache.Deallocate(ptr, SizeClass::Index(0));
    cache.ReleaseAll();
}

// Test 3: basic Deallocate.
TEST_F(ThreadCacheTest, BasicDeallocate) {
    thread_local ThreadCache cache;

    void* ptr = cache.Allocate(32);
    EXPECT_TRUE(ptr != nullptr);

    cache.Deallocate(ptr, SizeClass::Index(32));

    // ReleaseAll cleanup.
    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, EdgeCases) {
    thread_local ThreadCache tc;

    // 1. size == 0 (promoted to the minimum bucket).
    void* ptr_zero = tc.Allocate(0);
    EXPECT_TRUE(ptr_zero != nullptr);
    tc.Deallocate(ptr_zero, SizeClass::Index(0));

    // 2. size == MAX_TC_SIZE (32KB).
    size_t max_size = SizeClass::RoundUp(SizeConfig::MAX_TC_SIZE);
    void* ptr_max = tc.Allocate(max_size);
    EXPECT_TRUE(ptr_max != nullptr);

    // Write to the first and last bytes to verify the range.
    char* char_ptr = static_cast<char*>(ptr_max);
    char_ptr[0] = 'A';
    char_ptr[max_size - 1] = 'Z';
    EXPECT_EQ(char_ptr[0], 'A');
    EXPECT_EQ(char_ptr[max_size - 1], 'Z');

    tc.Deallocate(ptr_max, SizeClass::Index(max_size));
    tc.ReleaseAll();
}

// Test 4: repeated allocate and deallocate.
TEST_F(ThreadCacheTest, MultipleAllocateDeallocate) {
    thread_local ThreadCache cache;
    constexpr int num_allocs = 100;
    std::vector<void*> ptrs;
    constexpr size_t size = SizeClass::RoundUp(64);

    for ([[maybe_unused]] int i : std::views::iota(0, num_allocs)) {
        void* ptr = cache.Allocate(size);
        EXPECT_TRUE(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    std::ranges::for_each(ptrs, [&](void* ptr) {
        cache.Deallocate(ptr, SizeClass::Index(size));
    });

    cache.ReleaseAll();
}

// Test 5: allocation across different size classes.
TEST_F(ThreadCacheTest, DifferentSizeClasses) {
    thread_local ThreadCache cache;
    std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

    for (size_t orig_size: sizes) {
        size_t aligned_size = SizeClass::RoundUp(orig_size);
        void* ptr = cache.Allocate(aligned_size);
        EXPECT_TRUE(ptr != nullptr) << "Failed for size " << orig_size;
        cache.Deallocate(ptr, SizeClass::Index(aligned_size));
    }

    cache.ReleaseAll();
}

// Test 6: ReleaseAll behavior.
TEST_F(ThreadCacheTest, ReleaseAll) {
    thread_local ThreadCache cache;
    constexpr size_t size = SizeClass::RoundUp(128);

    // Allocate some objects without freeing them.
    for ([[maybe_unused]] int i : std::views::iota(0, 50)) {
        void* ptr = cache.Allocate(size);
        EXPECT_TRUE(ptr != nullptr);
        // Intentionally not freed; ReleaseAll below reclaims them.
    }

    cache.ReleaseAll();

    // Allocation after ReleaseAll must still work.
    void* ptr = cache.Allocate(size);
    EXPECT_TRUE(ptr != nullptr);
    cache.Deallocate(ptr, SizeClass::Index(size));
    cache.ReleaseAll();
}

// Test 7: slow-start policy.
TEST_F(ThreadCacheTest, SlowStartAndScavenge) {
    thread_local ThreadCache tc;
    size_t size = SizeClass::RoundUp(8);// smallest object; batch_num is usually 512

    std::vector<void*> ptrs;

    // 1. Allocate continuously to trigger slow-start growth.
    // Allocating 1500 objects forces multiple FetchFromCentralCache calls.
    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), size_t(1500))) {
        void* ptr = tc.Allocate(size);
        EXPECT_TRUE(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    // Verify all allocated pointers are distinct (duplicates are adjacent
    // after sorting).
    std::ranges::sort(ptrs);
    EXPECT_TRUE(std::ranges::adjacent_find(ptrs) == ptrs.end())
            << "Duplicate pointers allocated!";

    // 2. Deallocate continuously to trigger the release-too-long path.
    // Once the freed count exceeds the limit (1024), objects are returned in bulk.
    std::ranges::for_each(ptrs, [&](void* ptr) {
        tc.Deallocate(ptr, SizeClass::Index(size));
    });

    // 3. Clean up leftovers.
    tc.ReleaseAll();
}

// Test 8: trigger the release-too-long path.
TEST_F(ThreadCacheTest, TriggerReleaseTooLongList) {
    thread_local ThreadCache cache;
    size_t size = SizeClass::RoundUp(512);
    size_t batch_size = SizeClass::CalculateBatchSize(size);

    // Allocate enough objects.
    std::vector<void*> ptrs;
    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), batch_size * 4)) {
        void* ptr = cache.Allocate(size);
        EXPECT_TRUE(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    // Deallocate all objects to trigger the release-too-long path.
    std::ranges::for_each(ptrs, [&](void* ptr) {
        cache.Deallocate(ptr, SizeClass::Index(size));
    });

    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, SlowStartGrowthThenOveragesShrinkMaxSize) {
    thread_local ThreadCache cache;
    const size_t size = SizeClass::RoundUp(4096);
    const size_t idx = SizeClass::Index(size);
    const size_t batch_num = SizeClass::CalculateBatchSize(size);

    ASSERT_EQ(cache.GetMaxSizeForTest(idx), 1u);
    ASSERT_EQ(cache.GetOveragesForTest(idx), 0u);

    std::vector<void*> ptrs;
    ptrs.reserve(512);

    size_t peak_max_size = cache.GetMaxSizeForTest(idx);
    size_t growth_allocations = 0;
    const size_t target_peak_max_size = batch_num * 3;
    for (; growth_allocations < 4096 && peak_max_size < target_peak_max_size; ++growth_allocations) {
        void* ptr = cache.Allocate(size);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
        peak_max_size = cache.GetMaxSizeForTest(idx);
    }

    ASSERT_GE(peak_max_size, target_peak_max_size)
            << "max_size failed to grow far enough beyond batch size";

    const size_t extra_allocations = batch_num * 8;
    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), extra_allocations)) {
        void* ptr = cache.Allocate(size);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
        peak_max_size = std::max(peak_max_size, cache.GetMaxSizeForTest(idx));
    }

    size_t max_observed_overages = 0;
    size_t shrunk_max_size = peak_max_size;
    bool observed_shrink = false;

    for (void* ptr: ptrs) {
        cache.Deallocate(ptr, SizeClass::Index(size));
        max_observed_overages = std::max(max_observed_overages, cache.GetOveragesForTest(idx));

        const size_t current_max_size = cache.GetMaxSizeForTest(idx);
        if (current_max_size < peak_max_size) {
            shrunk_max_size = current_max_size;
            observed_shrink = true;
            break;
        }
    }

    EXPECT_GT(max_observed_overages, 0u) << "overages never accumulated during overflow deallocation";
    EXPECT_TRUE(observed_shrink) << "max_size never shrank after repeated overflow deallocations";
    EXPECT_LT(shrunk_max_size, peak_max_size);
    EXPECT_GE(shrunk_max_size, batch_num);

    cache.ReleaseAll();
}

// Test 9: stress test.
TEST_F(ThreadCacheTest, StressTest) {
    thread_local ThreadCache cache;
    std::vector<std::pair<void*, size_t>> allocated;
    std::mt19937 g(42);
    std::uniform_int_distribution<> size_dis(8, 1024);

    // Random allocation.
    for (int i = 0; i < 100; ++i) {
        size_t size = size_dis(g);
        size = SizeClass::RoundUp(size);
        void* ptr = cache.Allocate(size);
        if (ptr) {
            allocated.emplace_back(ptr, size);
        }
    }

    // Random deallocation.
    std::ranges::shuffle(allocated, g);
    std::ranges::for_each(allocated, [&](std::pair<void*, size_t> entry) {
        cache.Deallocate(entry.first, SizeClass::Index(entry.second));
    });

    cache.ReleaseAll();
}

// Simulates single-threaded random allocate/free behavior.
void ThreadRoutine(int thread_id, size_t iterations) {
    thread_local ThreadCache tc;// one ThreadCache instance per thread
    std::vector<void*> allocated_ptrs;
    allocated_ptrs.reserve(1000);

    std::mt19937 gen(thread_id);
    // Random size: 1 byte to 32KB.
    std::uniform_int_distribution<size_t> size_dist(1, 32 * 1024);
    // Random action: 70% allocate, 30% free (simulating a memory-growth phase).
    std::uniform_int_distribution<int> action_dist(1, 100);

    for (size_t i = 0; i < iterations; ++i) {
        if (allocated_ptrs.empty() || action_dist(gen) <= 70) {
            // Allocate.
            size_t size = size_dist(gen);
            size_t aligned_size = SizeClass::RoundUp(size);
            void* ptr = tc.Allocate(aligned_size);
            if (ptr) {
                // Simple write to verify the slot.
                *static_cast<size_t*>(ptr) = aligned_size;
                allocated_ptrs.push_back(ptr);
            }
        } else {
            // Deallocate a randomly chosen pointer.
            size_t idx = gen() % allocated_ptrs.size();
            void* ptr = allocated_ptrs[idx];
            size_t aligned_size = *static_cast<size_t*>(ptr);// read back the size

            tc.Deallocate(ptr, SizeClass::Index(aligned_size));

            // Remove by replacing with the last element.
            allocated_ptrs[idx] = allocated_ptrs.back();
            allocated_ptrs.pop_back();
        }
    }

    // Free all remaining memory before the thread exits.
    std::ranges::for_each(allocated_ptrs, [&](void* ptr) {
        size_t aligned_size = *static_cast<size_t*>(ptr);
        tc.Deallocate(ptr, SizeClass::Index(aligned_size));
    });

    // Return the ThreadCache objects to CentralCache.
    tc.ReleaseAll();
}

TEST_F(ThreadCacheTest, MultiThreadStress) {
    const int num_threads = std::thread::hardware_concurrency();
    const size_t iterations_per_thread = 50000;// 50k operations per thread

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(ThreadRoutine, i, iterations_per_thread);
    }

    for (auto& t: threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    size_t total_ops = num_threads * iterations_per_thread;
    std::cout << " " << num_threads << " threads executed "
              << total_ops << " ops in " << diff.count() << " seconds.\n";
    std::cout << " " << (total_ops / diff.count() / 1000000.0)
              << " Million Ops/sec\n";
}

// Test 10: multi-threaded allocation (each thread has its own ThreadCache).
TEST_F(ThreadCacheTest, MultiThreadedAllocation) {
    constexpr int num_threads = 4;
    constexpr int allocations_per_thread = 100;
    constexpr size_t size = SizeClass::RoundUp(64);

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for ([[maybe_unused]] int t : std::views::iota(0, num_threads)) {
        threads.emplace_back([&success_count]() {
            thread_local ThreadCache cache;
            for (int i = 0; i < allocations_per_thread; ++i) {
                void* ptr = cache.Allocate(size);
                if (ptr) {
                    success_count.fetch_add(1);
                    cache.Deallocate(ptr, SizeClass::Index(size));
                }
            }
            cache.ReleaseAll();
        });
    }

    for (auto& t: threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * allocations_per_thread);
}

// Test 11: multi-threaded allocation of different sizes.
TEST_F(ThreadCacheTest, MultiThreadedDifferentSizes) {
    constexpr int num_threads = 4;
    constexpr int allocations_per_thread = 50;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&success_count, t]() {
            thread_local ThreadCache cache;
            std::vector<size_t> orig_sizes = {8, 16, 32, 64, 128, 256, 512, 1024};
            size_t aligned_size = SizeClass::RoundUp(orig_sizes[t % orig_sizes.size()]);

            for (int i = 0; i < allocations_per_thread; ++i) {
                void* ptr = cache.Allocate(aligned_size);
                if (ptr) {
                    success_count.fetch_add(1);
                    cache.Deallocate(ptr, SizeClass::Index(aligned_size));
                }
            }
            cache.ReleaseAll();
        });
    }

    for (auto& t: threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * allocations_per_thread);
}

// Test 12: minimum-class allocation (RoundUp(8) -> 16-byte class).
TEST_F(ThreadCacheTest, SmallObjectAllocation) {
    thread_local ThreadCache cache;
    constexpr size_t size = SizeClass::RoundUp(8);

    for ([[maybe_unused]] int i : std::views::iota(0, 100)) {
        void* ptr = cache.Allocate(size);
        EXPECT_NE(ptr, nullptr);
        cache.Deallocate(ptr, SizeClass::Index(size));
    }

    cache.ReleaseAll();
}

// Test 13: boundary-size allocation.
TEST_F(ThreadCacheTest, BoundarySizeAllocation) {
    thread_local ThreadCache cache;
    size_t max_size = SizeClass::RoundUp(SizeConfig::MAX_TC_SIZE);

    void* ptr = cache.Allocate(max_size);
    EXPECT_NE(ptr, nullptr);
    cache.Deallocate(ptr, SizeClass::Index(max_size));

    cache.ReleaseAll();
}

// Test 14: repeated allocate/deallocate of the same size.
TEST_F(ThreadCacheTest, RepeatedAllocateDeallocate) {
    thread_local ThreadCache cache;
    size_t size = SizeClass::RoundUp(128);

    for ([[maybe_unused]] int round : std::views::iota(0, 10)) {
        std::vector<void*> ptrs;
        for ([[maybe_unused]] int i : std::views::iota(0, 20)) {
            void* ptr = cache.Allocate(size);
            EXPECT_NE(ptr, nullptr);
            ptrs.push_back(ptr);
        }
        std::ranges::for_each(ptrs, [&](void* ptr) {
            cache.Deallocate(ptr, SizeClass::Index(size));
        });
    }

    cache.ReleaseAll();
}

// Test 15: verify FetchFromCentralCache is triggered.
TEST_F(ThreadCacheTest, FetchFromCentralCacheTrigger) {
    thread_local ThreadCache cache;
    size_t size = SizeClass::RoundUp(256);

    // Allocate more than batch_size objects to trigger FetchFromCentralCache repeatedly.
    size_t batch_size = SizeClass::CalculateBatchSize(size);
    std::vector<void*> ptrs;

    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), batch_size * 3)) {
        void* ptr = cache.Allocate(size);
        EXPECT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    std::ranges::for_each(ptrs, [&](void* ptr) {
        cache.Deallocate(ptr, SizeClass::Index(size));
    });

    cache.ReleaseAll();
}

// A fully failed refill (OOM) must return null and leave the quota untouched:
// max_size stays at the slow-start floor, so no larger refill is requested.
TEST_F(ThreadCacheTest, RefillFailureKeepsQuotaAndReturnsNull) {
    thread_local ThreadCache tc;
    const size_t size = SizeClass::RoundUp(64);
    const size_t idx = SizeClass::Index(size);

    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    void* p = tc.Allocate(size);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(tc.GetMaxSizeForTest(idx), 1u);
    EXPECT_EQ(tc.GetOveragesForTest(idx), 0u);

    tc.ReleaseAll();
}

// A simple comparison test to get an intuitive feel for ThreadCache's performance.
TEST_F(ThreadCacheTest, BenchmarkVsStdMalloc) {
    const size_t iterations = 1000000;               // 1 million iterations
    const size_t alloc_size = SizeClass::RoundUp(32);// 32-byte small object

    // 1. Test std::malloc.
    auto start_std = std::chrono::high_resolution_clock::now();
    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), iterations)) {
        void* p = std::malloc(alloc_size);
        std::free(p);
    }

    auto end_std = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_std = end_std - start_std;

    // 2. Test ThreadCache.
    thread_local ThreadCache tc;
    auto start_tc = std::chrono::high_resolution_clock::now();
    for ([[maybe_unused]] size_t i : std::views::iota(size_t(0), iterations)) {
        void* p = tc.Allocate(alloc_size);
        tc.Deallocate(p, SizeClass::Index(alloc_size));
    }
    tc.ReleaseAll();
    auto end_tc = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_tc = end_tc - start_tc;

    std::cout << " Time: " << diff_std.count() << " s\n";
    std::cout << " Time: " << diff_tc.count() << " s\n";
}

// The per-class quota stays bounded under sustained mixed pressure: never zero
// (which would make refill fail permanently) and never above
// quota_policy::kMaxQuotaBatches * batch.
TEST_F(ThreadCacheTest, MaxSizeStaysBoundedUnderSustainedLoad) {
    thread_local ThreadCache tc;
    const size_t size = SizeClass::RoundUp(1024);
    const size_t idx = SizeClass::Index(size);
    const size_t batch = SizeClass::CalculateBatchSize(size);

    std::vector<void*> held;
    for ([[maybe_unused]] int round : std::views::iota(0, 200)) {
        for ([[maybe_unused]] int i : std::views::iota(0, 64)) {
            void* p = tc.Allocate(size);
            ASSERT_NE(p, nullptr);
            held.push_back(p);
        }
        for ([[maybe_unused]] int i : std::views::iota(0, 32)) {
            tc.Deallocate(held.back(), idx);
            held.pop_back();
        }

        const size_t ms = tc.GetMaxSizeForTest(idx);
        EXPECT_GE(ms, 1u);
        EXPECT_LE(ms, batch * quota_policy::kMaxQuotaBatches);
    }

    std::ranges::for_each(held, [&](void* p) { tc.Deallocate(p, idx); });
    tc.ReleaseAll();
}

// A partial refill (0 < fetched < fetch_num) signals memory pressure and must
// NOT grow the quota or reset the decay signal (the M3 fix). Forcing the partial
// result by capping CentralCache::FetchRange at one object exercises this branch.
TEST_F(ThreadCacheTest, PartialRefillHoldsQuotaAndOverage) {
    thread_local ThreadCache tc;
    const size_t size = SizeClass::RoundUp(64);
    const size_t idx = SizeClass::Index(size);

    // First refill grows max_size 1 -> 2, so the next fetch_num is 2, making a
    // partial result (1) observably smaller than the requested amount.
    void* p1 = tc.Allocate(size);
    ASSERT_NE(p1, nullptr);
    ASSERT_EQ(tc.GetMaxSizeForTest(idx), 2u);

    g_mock_fetch_range_cap.store(1, std::memory_order_relaxed);
    void* p2 = tc.Allocate(size);
    g_mock_fetch_range_cap.store(0, std::memory_order_relaxed);

    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(tc.GetMaxSizeForTest(idx), 2u); // quota unchanged
    EXPECT_EQ(tc.GetOveragesForTest(idx), 0u);// decay signal preserved

    tc.Deallocate(p1, idx);
    tc.Deallocate(p2, idx);
    tc.ReleaseAll();
}

TEST_F(ThreadCacheTest, AggregateQuotaBudgetBoundsAllSizeClasses) {
    ThreadCache baseline;
    const size_t base_reservation = baseline.GetReservedQuotaBytesForTest();
    const size_t budget = base_reservation + SystemConfig::PAGE_SIZE;
    ThreadCache cache(budget);
    const auto denied_before = GetThreadCacheStats().budget_denied_growth.load(
            std::memory_order_relaxed);

    std::vector<std::pair<void*, size_t>> held;
    held.reserve(SizeClass::kNumSizeClasses * 2);
    for (size_t idx : std::views::iota(size_t(0), SizeClass::kNumSizeClasses)) {
        const size_t size = SizeClass::Size(idx);
        // Two cold refills attempt to grow this class twice. The aggregate
        // capacity budget must hold regardless of the order or class size.
        for ([[maybe_unused]] size_t n : std::views::iota(size_t(0), size_t(2))) {
            void* p = cache.Allocate(size);
            ASSERT_NE(p, nullptr);
            held.emplace_back(p, size);
            EXPECT_LE(cache.GetReservedQuotaBytesForTest(), budget);
        }
    }

    EXPECT_GT(GetThreadCacheStats().budget_denied_growth.load(std::memory_order_relaxed),
              denied_before);

    std::ranges::for_each(held, [&](std::pair<void*, size_t> entry) {
        cache.Deallocate(entry.first, SizeClass::Index(entry.second));
    });
    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, QuotaReservationTracksGrowthAndHardTrim) {
    ThreadCache baseline;
    const size_t base_reservation = baseline.GetReservedQuotaBytesForTest();
    const size_t size = SizeClass::RoundUp(16);
    const size_t idx = SizeClass::Index(size);
    ThreadCache cache(base_reservation + size);

    void* small = cache.Allocate(size);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(cache.GetMaxSizeForTest(idx), 2u);
    EXPECT_EQ(cache.GetReservedQuotaBytesForTest(), base_reservation + size);

    const size_t large_size = SizeConfig::MAX_TC_SIZE;
    const size_t large_idx = SizeClass::Index(large_size);
    void* large = cache.Allocate(large_size);
    ASSERT_NE(large, nullptr);
    EXPECT_EQ(cache.GetMaxSizeForTest(large_idx), 1u);
    EXPECT_EQ(cache.GetReservedQuotaBytesForTest(), base_reservation + size);

    cache.Deallocate(small, idx);
    cache.Deallocate(large, large_idx);
    cache.Trim(ThreadCacheTrimMode::kRelease, 0);
    EXPECT_EQ(cache.CachedBytesSnapshot(), 0u);
    EXPECT_EQ(cache.GetReservedQuotaBytesForTest(), base_reservation);
}

TEST_F(ThreadCacheTest, SoftTrimKeepsOneBatchAndRetractsBurstQuota) {
    ThreadCache cache;
    const size_t size = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(size);
    const size_t batch = SizeClass::CalculateBatchSize(size);

    std::vector<void*> held;
    while (cache.GetMaxSizeForTest(idx) < batch + 2) {
        void* p = cache.Allocate(size);
        ASSERT_NE(p, nullptr);
        held.push_back(p);
    }

    const size_t cached_before_free = cache.CachedBytesSnapshot();
    const size_t max_size = cache.GetMaxSizeForTest(idx);
    ASSERT_LE(cached_before_free, max_size * size);
    const size_t free_to_capacity = max_size - cached_before_free / size;
    ASSERT_LE(free_to_capacity, held.size());
    std::ranges::for_each(held | std::views::take(free_to_capacity), [&](void* p) {
        cache.Deallocate(p, idx);
    });

    ASSERT_GT(cache.CachedBytesSnapshot(), batch * size);
    const size_t reservation_before = cache.GetReservedQuotaBytesForTest();
    cache.Trim(ThreadCacheTrimMode::kReuse, batch * size);
    EXPECT_LE(cache.CachedBytesSnapshot(), batch * size);
    EXPECT_LE(cache.GetReservedQuotaBytesForTest(), reservation_before);
    EXPECT_LE(cache.GetMaxSizeForTest(idx), batch);

    std::ranges::for_each(held | std::views::drop(free_to_capacity), [&](void* p) {
        cache.Deallocate(p, idx);
    });
    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, CooperativeSoftTrimCannotGrowQuotaPastBudget) {
    ThreadCache baseline;
    const size_t base_reservation = baseline.GetReservedQuotaBytesForTest();
    const size_t size = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(size);
    constexpr size_t kQuota = 4;
    const size_t budget = base_reservation + (kQuota - 1) * size;
    ThreadCache cache(budget);

    std::vector<void*> held;
    while (cache.GetMaxSizeForTest(idx) < kQuota || held.size() < kQuota + 1) {
        void* p = cache.Allocate(size);
        ASSERT_NE(p, nullptr);
        held.push_back(p);
    }
    ASSERT_EQ(cache.GetMaxSizeForTest(idx), kQuota);
    ASSERT_EQ(cache.GetReservedQuotaBytesForTest(), budget);

    std::ranges::for_each(held | std::views::take(kQuota), [&](void* p) {
        cache.Deallocate(p, idx);
    });
    ASSERT_EQ(cache.CachedBytesSnapshot(), kQuota * size);

    ThreadCache::RequestGlobalTrim(ThreadCacheTrimMode::kReuse);
    cache.Deallocate(held[kQuota], idx);

    EXPECT_LE(cache.GetMaxSizeForTest(idx), kQuota);
    EXPECT_LE(cache.GetReservedQuotaBytesForTest(), budget);
    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, CooperativeHardTrimIsObservedOnlyAtSlowPath) {
    ThreadCache cache;
    const size_t size = SizeConfig::MAX_TC_SIZE;
    const size_t idx = SizeClass::Index(size);
    const size_t batch = SizeClass::CalculateBatchSize(size);

    std::vector<void*> held;
    while (cache.GetMaxSizeForTest(idx) < batch + 2) {
        void* p = cache.Allocate(size);
        ASSERT_NE(p, nullptr);
        held.push_back(p);
    }

    const size_t max_size = cache.GetMaxSizeForTest(idx);
    const size_t fill_count = max_size - cache.CachedBytesSnapshot() / size;
    ASSERT_LT(fill_count, held.size());
    std::ranges::for_each(held | std::views::take(fill_count), [&](void* p) {
        cache.Deallocate(p, idx);
    });
    ASSERT_EQ(cache.CachedBytesSnapshot(), max_size * size);

    ThreadCache::RequestGlobalTrim(ThreadCacheTrimMode::kRelease);
    // A normal hit does not observe a shared epoch. Pushing one more object
    // crosses the quota, enters the existing slow path, and performs the trim.
    cache.Deallocate(held[fill_count], idx);
    EXPECT_EQ(cache.CachedBytesSnapshot(), 0u);

    std::ranges::for_each(held | std::views::drop(fill_count + 1), [&](void* p) {
        cache.Deallocate(p, idx);
    });
    cache.ReleaseAll();
}

TEST_F(ThreadCacheTest, ExplicitOwnerPurgeDrainsCurrentThreadAndTransferCache) {
    constexpr size_t size = 64;
    std::thread worker([] {
        void* p = am_malloc(size);
        ASSERT_NE(p, nullptr);
        am_free(p);
        am_thread_cache_purge();

        void* reused = am_malloc(size);
        ASSERT_NE(reused, nullptr);
        am_free(reused);
        am_thread_cache_purge();
    });
    worker.join();

    const size_t idx = SizeClass::Index(size);
    EXPECT_EQ(central_cache_.GetTransferCacheCountForTest(idx), 0u);
}
}// namespace
