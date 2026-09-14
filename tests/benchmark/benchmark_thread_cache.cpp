// ThreadCache retention benchmarks: tail traversal and owner-thread returns
// through CentralCache, with quota-full caches prepared outside timing.
#include "ammalloc/ammalloc.h"
#include "ammalloc/central_cache.h"
#include "ammalloc/free_list.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/size_class.h"
#include "ammalloc/thread_cache.h"

#include <atomic>
#include <barrier>
#include <benchmark/benchmark.h>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace ammalloc;

// 16-byte class geometry: batch == 512, quota ceiling == 8 * 512 == 4096.
constexpr size_t kObjSize = 16;
constexpr size_t kBatch = 512;
constexpr size_t kQuotaCap = 4096;

// Google Benchmark invokes Setup/Teardown once around each complete thread
// group. Per-round barriers keep one thread's warmup out of another's trim.
std::unique_ptr<std::barrier<>> purge_barrier;
std::atomic<bool> purge_failed{false};

void SetupPurgeStorm(const benchmark::State& state) {
    purge_barrier = std::make_unique<std::barrier<>>(state.threads());
    purge_failed.store(false, std::memory_order_relaxed);
}

void TeardownPurgeStorm(const benchmark::State&) {
    purge_barrier.reset();
}

bool PurgeGroupSucceeded() {
    purge_barrier->arrive_and_wait();
    const bool succeeded = !purge_failed.load(std::memory_order_relaxed);
    // Every participant must sample this phase before the next may fail.
    purge_barrier->arrive_and_wait();
    return succeeded;
}

// Worst-case PopRangeTail traversal: a quota-full chain evicts one batch
// from the tail and immediately re-inserts it at the head, so every
// iteration repeats the same ~3584-step cut walk plus ~512-step suffix walk.
void BM_FreeList_TrimTailWorst16B(benchmark::State& state) {
    std::vector<FreeBlock> blocks(kQuotaCap);
    for (size_t i = 0; i < kQuotaCap; ++i) {
        blocks[i].next = (i + 1 < kQuotaCap) ? &blocks[i + 1] : nullptr;
    }
    FreeList list;
    // One full chain; each iteration trims the tail batch and pushes it back.
    list.PushRange(FreeChain{&blocks[0], &blocks[kQuotaCap - 1], kQuotaCap});
    for (auto _: state) {
        auto evicted = list.PopRangeTail(kBatch);
        benchmark::DoNotOptimize(evicted);
        list.PushRange(evicted);
    }
}
BENCHMARK(BM_FreeList_TrimTailWorst16B)->ThreadRange(1, 16);

enum class ReturnOperation { kHardTrim,
                             kSoftTrim,
                             kReleaseAll,
                             kPublicPurge };

// Warm the quota before returning objects: allocated objects held by the
// caller are not part of ThreadCache's FreeList. The bounded surplus is
// returned directly to bitmaps outside timing, leaving exactly one quota of
// cached objects for the measured owner-thread return. Hard trim measures Trim only;
// the public purge API additionally drains CentralCache's retained objects.
//
// Span telemetry is snapshotted around the whole run rather than per iteration
// to keep atomic loads out of the timed region; the reported rate therefore
// mixes setup and timed contributions and is intended for relative regression
// detection across the four ReturnOperation variants.
template<ReturnOperation Operation>
void BenchmarkThreadCacheReturn(benchmark::State& state) {
    ThreadCache tc;
    const size_t idx = SizeClass::Index(kObjSize);
    constexpr size_t kWarmupLimit = 16 * kQuotaCap;
    constexpr size_t kRetained = Operation == ReturnOperation::kSoftTrim ? kBatch : 0;
    std::vector<void*> held;
    held.reserve(kWarmupLimit);
    const auto& cc_stats = CentralCache::GetStats();
    const auto& tc_stats = GetThreadCacheStats();
    const size_t unpin_before =
            cc_stats.spans_unpinned_by_direct_release.load(std::memory_order_relaxed);
    const size_t return_before =
            cc_stats.release_spans_returned_to_pagecache.load(std::memory_order_relaxed);
    const size_t trim_count_before = tc_stats.trim_count.load(std::memory_order_relaxed);
    const size_t trimmed_bytes_before = tc_stats.trimmed_bytes.load(std::memory_order_relaxed);
    int64_t released_objects = 0;
    for (auto _: state) {
        state.PauseTiming();
        while (held.size() < kWarmupLimit &&
               (tc.GetMaxSizeForTest(idx) < kQuotaCap ||
                held.size() < kQuotaCap || tc.CachedBytesSnapshot() != 0)) {
            void* p = tc.Allocate(kObjSize);
            if (!p) {
                break;
            }
            held.push_back(p);
        }
        const bool ready = tc.GetMaxSizeForTest(idx) == kQuotaCap &&
                           held.size() >= kQuotaCap && tc.CachedBytesSnapshot() == 0;
        void* surplus = nullptr;
        const size_t cached_count = ready ? kQuotaCap : 0;
        for (size_t i = cached_count; i < held.size(); ++i) {
            auto* object = static_cast<FreeBlock*>(held[i]);
            object->next = static_cast<FreeBlock*>(surplus);
            surplus = object;
        }
        CentralCache::GetInstance().ReleaseListToSpans(
                surplus, idx, CentralReleaseMode::kSpanBitmap);
        for (size_t i = 0; i < cached_count; ++i) {
            tc.Deallocate(held[i], idx);
        }
        held.clear();
        if (!ready || tc.CachedBytesSnapshot() != kQuotaCap * kObjSize) {
            purge_failed.store(true, std::memory_order_relaxed);
        }
        if (!PurgeGroupSucceeded()) {
            tc.ReleaseAll();
            state.ResumeTiming();
            state.SkipWithError("failed to prepare a quota-full ThreadCache");
            break;
        }
        state.ResumeTiming();
        if constexpr (Operation == ReturnOperation::kReleaseAll) {
            tc.ReleaseAll();
        } else if constexpr (Operation == ReturnOperation::kPublicPurge) {
            // am_thread_cache_purge() operates on the calling thread's TLS
            // cache; this benchmark drives a local `tc`, so reproduce the same
            // two-step sequence (owner-thread hard trim + full CentralCache
            // TransferCache drain) against the local instance.
            tc.Trim(ThreadCacheTrimMode::kRelease, 0);
            CentralCache::GetInstance().DrainTransferCaches(
                    std::numeric_limits<size_t>::max());
        } else {
            constexpr auto mode = Operation == ReturnOperation::kSoftTrim
                                          ? ThreadCacheTrimMode::kReuse
                                          : ThreadCacheTrimMode::kRelease;
            tc.Trim(mode, 0);
        }
        state.PauseTiming();
        const bool expected_retention = tc.CachedBytesSnapshot() == kRetained * kObjSize;
        released_objects += static_cast<int64_t>(kQuotaCap - kRetained);
        if (!expected_retention) {
            purge_failed.store(true, std::memory_order_relaxed);
        }
        const bool group_succeeded = PurgeGroupSucceeded();
        tc.ReleaseAll();
        state.ResumeTiming();
        if (!group_succeeded) {
            state.SkipWithError("unexpected retained object count after ThreadCache return");
            break;
        }
    }
    tc.ReleaseAll();
    state.SetItemsProcessed(released_objects);
    state.SetBytesProcessed(released_objects * static_cast<int64_t>(kObjSize));
    // Only thread 0 publishes the aggregate so google benchmark's counter
    // summation across threads does not multiply the global delta by N.
    if (state.thread_index() == 0) {
        const size_t unpin_after =
                cc_stats.spans_unpinned_by_direct_release.load(std::memory_order_relaxed);
        const size_t return_after =
                cc_stats.release_spans_returned_to_pagecache.load(std::memory_order_relaxed);
        const size_t trim_count_after = tc_stats.trim_count.load(std::memory_order_relaxed);
        const size_t trimmed_bytes_after = tc_stats.trimmed_bytes.load(std::memory_order_relaxed);
        state.counters["span_unpin_direct_rate"] = benchmark::Counter(
                static_cast<double>(unpin_after - unpin_before),
                benchmark::Counter::kIsRate);
        state.counters["span_return_pc_rate"] = benchmark::Counter(
                static_cast<double>(return_after - return_before),
                benchmark::Counter::kIsRate);
        state.counters["tc_trim_count_rate"] = benchmark::Counter(
                static_cast<double>(trim_count_after - trim_count_before),
                benchmark::Counter::kIsRate);
        state.counters["tc_trimmed_bytes_rate"] = benchmark::Counter(
                static_cast<double>(trimmed_bytes_after - trimmed_bytes_before),
                benchmark::Counter::kIsRate);
    }
}

void BM_ThreadCache_CentralInteraction_TrimRelease16B(benchmark::State& state) {
    BenchmarkThreadCacheReturn<ReturnOperation::kHardTrim>(state);
}

void BM_ThreadCache_CentralInteraction_TrimReuse16B(benchmark::State& state) {
    BenchmarkThreadCacheReturn<ReturnOperation::kSoftTrim>(state);
}

void BM_ThreadCache_CentralInteraction_ReleaseAll16B(benchmark::State& state) {
    BenchmarkThreadCacheReturn<ReturnOperation::kReleaseAll>(state);
}

void BM_ThreadCache_CentralInteraction_PublicPurge16B(benchmark::State& state) {
    BenchmarkThreadCacheReturn<ReturnOperation::kPublicPurge>(state);
}

BENCHMARK(BM_ThreadCache_CentralInteraction_TrimRelease16B)
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Setup(SetupPurgeStorm)
        ->Teardown(TeardownPurgeStorm);
BENCHMARK(BM_ThreadCache_CentralInteraction_TrimReuse16B)
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Setup(SetupPurgeStorm)
        ->Teardown(TeardownPurgeStorm);
BENCHMARK(BM_ThreadCache_CentralInteraction_ReleaseAll16B)
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Setup(SetupPurgeStorm)
        ->Teardown(TeardownPurgeStorm);
BENCHMARK(BM_ThreadCache_CentralInteraction_PublicPurge16B)
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Setup(SetupPurgeStorm)
        ->Teardown(TeardownPurgeStorm);

#ifdef AMMALLOC_TEST
// Forced partial refill: g_mock_fetch_range_cap caps every CentralCache
// FetchRange so the owner thread's FreeList empties repeatedly and each refill
// returns only `cap` objects. This isolates the partial-refill slow path that
// a degraded or contended middle-end would impose on the front end.
void TeardownForcedRefill(const benchmark::State&) {
    g_mock_fetch_range_cap.store(0, std::memory_order_relaxed);
    am_thread_cache_purge();
    CentralCache::GetInstance().Reset();
    PageCache::GetInstance().Reset();
}

void BM_ThreadCache_CentralInteraction_ForcedRefill(benchmark::State& state) {
    // The cap is process-wide; set it from the benchmark body (Setup cannot
    // rely on range access) and clear it in Teardown.
    g_mock_fetch_range_cap.store(static_cast<size_t>(state.range(0)),
                                 std::memory_order_relaxed);
    ThreadCache tc;
    const size_t idx = SizeClass::Index(kObjSize);
    const auto& cc = CentralCache::GetStats();
    const size_t span_before = cc.fetch_span_list_objects.load(std::memory_order_relaxed);
    // Working set far exceeds any cap so each iteration forces many refills.
    constexpr size_t kWorkSet = 4 * kBatch;
    std::vector<void*> objs(kWorkSet, nullptr);
    int64_t allocs = 0;
    for (auto _: state) {
        for (size_t i = 0; i < kWorkSet; ++i) {
            objs[i] = tc.Allocate(kObjSize);
        }
        allocs += static_cast<int64_t>(kWorkSet);

        state.PauseTiming();
        for (size_t i = 0; i < kWorkSet; ++i) {
            if (objs[i]) {
                tc.Deallocate(objs[i], idx);
            }
        }
        tc.ReleaseAll();
        state.ResumeTiming();
    }
    tc.ReleaseAll();
    state.SetItemsProcessed(allocs);
    state.SetBytesProcessed(allocs * static_cast<int64_t>(kObjSize));
    if (state.thread_index() == 0) {
        const size_t span_after = cc.fetch_span_list_objects.load(std::memory_order_relaxed);
        // Refill volume: objects carved from Span bitmaps to satisfy the
        // capped refills. Higher when cap is smaller (more refill rounds).
        state.counters["refill_span_objects_rate"] = benchmark::Counter(
                static_cast<double>(span_after - span_before), benchmark::Counter::kIsRate);
    }
}

BENCHMARK(BM_ThreadCache_CentralInteraction_ForcedRefill)
        ->Args({1})
        ->Args({128})
        ->Args({256})
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Teardown(TeardownForcedRefill);
#endif

// Overflow trim: deallocate a burst larger than the quota ceiling so the
// owner-thread FreeList climbs past max_size and DeallocateSlowPath trims a
// batch back to CentralCache. This exercises the implicit overflow path,
// distinct from the explicit Trim() measured above.
void BM_ThreadCache_CentralInteraction_OverflowTrim(benchmark::State& state) {
    ThreadCache tc;
    const size_t idx = SizeClass::Index(kObjSize);
    // Burst exceeds the 4096-object quota ceiling, guaranteeing overflow.
    const size_t burst = kQuotaCap + kBatch;
    std::vector<void*> objs;
    objs.reserve(burst);
    int64_t deallocs = 0;
    int64_t overflowed_objects = 0;
    for (auto _: state) {
        state.PauseTiming();
        tc.ReleaseAll();
        objs.clear();
        for (size_t i = 0; i < burst; ++i) {
            if (void* p = tc.Allocate(kObjSize)) {
                objs.push_back(p);
            }
        }
        // Empty the local FreeList so the timed deallocations start from zero
        // and must climb back over max_size to trigger overflow trims. The
        // objects stay live (held in `objs`); ReleaseAll only drains the cache.
        tc.ReleaseAll();
        state.ResumeTiming();

        for (void* p : objs) {
            tc.Deallocate(p, idx);
        }
        deallocs += static_cast<int64_t>(objs.size());

        state.PauseTiming();
        // DeallocateSlowPath evicts the surplus without touching trim_count, so
        // measure overflow as (returned - still cached): whatever did not stay
        // local was pushed back to CentralCache by an overflow trim.
        const size_t cached = tc.CachedBytesSnapshot() / kObjSize;
        overflowed_objects +=
                static_cast<int64_t>(objs.size()) - static_cast<int64_t>(cached);
        tc.ReleaseAll();
        state.ResumeTiming();
    }
    tc.ReleaseAll();
    state.SetItemsProcessed(deallocs);
    state.SetBytesProcessed(deallocs * static_cast<int64_t>(kObjSize));
    state.counters["overflow_objects_rate"] = benchmark::Counter(
            static_cast<double>(overflowed_objects), benchmark::Counter::kIsRate);
}

BENCHMARK(BM_ThreadCache_CentralInteraction_OverflowTrim)
        ->ThreadRange(1, 16)
        ->UseRealTime()
        ->Setup(SetupPurgeStorm)
        ->Teardown(TeardownPurgeStorm);

// Thread-exit burst: N worker threads each fill their TLS ThreadCache via the
// public am_malloc/am_free API, rendezvous at a barrier, then exit together so
// every ThreadCacheCleaner runs ReleaseAll against CentralCache simultaneously.
// The timed region covers barrier release through full join, capturing the
// concurrent-exit return storm rather than thread spawn cost.
void BM_ThreadCache_CentralInteraction_ThreadExitBurst(benchmark::State& state) {
    const int n_threads = static_cast<int>(state.range(0));
    const int per_thread = static_cast<int>(state.range(1));
    int64_t total_objects = 0;
    int64_t burst_ns = 0;
    for (auto _: state) {
        state.PauseTiming();
        // Barrier lives on the stack for this iteration; every worker is joined
        // before it goes out of scope, so a reference capture is safe.
        std::barrier<> sync(n_threads + 1);
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(n_threads));
        for (int i = 0; i < n_threads; ++i) {
            threads.emplace_back([&sync, per_thread]() {
                std::vector<void*> objs(static_cast<size_t>(per_thread), nullptr);
                for (auto& p : objs) {
                    p = am_malloc(kObjSize);
                }
                for (auto* p : objs) {
                    am_free(p);
                }
                // Caches are full; rendezvous so all threads exit together and
                // their ThreadCacheCleaner ReleaseAll calls hit CentralCache at
                // the same instant.
                sync.arrive_and_wait();
            });
        }
        state.ResumeTiming();

        const auto t0 = std::chrono::steady_clock::now();
        sync.arrive_and_wait();
        for (auto& t : threads) {
            t.join();
        }
        const auto t1 = std::chrono::steady_clock::now();
        burst_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        total_objects += static_cast<int64_t>(n_threads) * per_thread;

        state.PauseTiming();
        am_thread_cache_purge();
        CentralCache::GetInstance().Reset();
        PageCache::GetInstance().Reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(total_objects);
    state.SetBytesProcessed(total_objects * static_cast<int64_t>(kObjSize));
    state.counters["exit_burst_ns"] = benchmark::Counter(
            static_cast<double>(burst_ns), benchmark::Counter::kAvgIterations);
}

BENCHMARK(BM_ThreadCache_CentralInteraction_ThreadExitBurst)
        ->Args({4, 1024})
        ->Args({4, 16384})
        ->Args({16, 1024})
        ->Args({16, 16384});

}// namespace
