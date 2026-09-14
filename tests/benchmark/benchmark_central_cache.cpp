// CentralCache component benchmarks. Each scenario establishes the required
// TransferCache/SpanList state outside the timed region, returns every object
// before the next scenario, and reports layered telemetry deltas with setup
// work subtracted via PrepGuard.
#include "ammalloc/ammalloc.h"
#include "ammalloc/central_cache.h"
#include "ammalloc/lock_instrumentation.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/page_heap_scavenger.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

namespace {

using namespace ammalloc;

// Five aligned size classes covering the linear (16/64), geometric (256),
// and large-object (4096/32768) regions. Batch and capacity values come
// from CalculateBatchSize so the sweep follows the production table exactly.
constexpr std::array<int, 5> kBenchSizes{16, 64, 256, 4096, 32768};

static_assert(SizeClass::CalculateBatchSize(16) == 512);
static_assert(SizeClass::CalculateBatchSize(64) == 512);
static_assert(SizeClass::CalculateBatchSize(256) == 128);
static_assert(SizeClass::CalculateBatchSize(4096) == 8);
static_assert(SizeClass::CalculateBatchSize(32768) == 2);

// CentralCache::kCapScale is private; keep the matching constant here so the
// benchmark can compute per-size transfer capacity without exposing internals.
constexpr size_t kCapScale = 8;
constexpr size_t kReleaseChainSize = 4;
constexpr size_t kSpscObjectSize = 64;
constexpr size_t kSpscBatchSize = 8;

constexpr size_t TransferCapacity(size_t aligned_size) {
    return kCapScale * SizeClass::CalculateBatchSize(aligned_size);
}

static_assert(kReleaseChainSize < TransferCapacity(32768));
static_assert(kSpscBatchSize <= SizeClass::kMaxBatchSize);

// ---------- Layered telemetry ----------
// Snapshot of every counter a CentralCache benchmark reports. Deltas are
// computed against a matching pre-loop snapshot; PrepGuard subtracts the
// contribution of paused setup work so the reported rate reflects only the
// timed region.
struct CentralBenchStats {
    size_t transfer_cache_drained_bytes;
    size_t spans_unpinned_by_direct_release;
    size_t fetch_transfer_hit_objects;
    size_t fetch_span_list_objects;
    size_t fetch_pagecache_spans;
    size_t release_transfer_overflow_objects;
    size_t release_spans_returned_to_pagecache;
    size_t page_allocator_normal_success;
    // SpanList attribution (P1).
    size_t spanlist_rotations;
    size_t spanlist_traversals;
    // RSS layer (Principle 5): only PageHeapScavenger's MADV_DONTNEED path
    // actually returns bytes to the OS, one layer below Span handoff.
    size_t scavenge_passes;
    size_t scavenged_spans;
    size_t scavenged_bytes;
    size_t madvise_success_count;
};

CentralBenchStats SnapshotStats() {
    const auto& cc = CentralCache::GetStats();
    const auto& pa = PageAllocator::GetStats();
    const auto& sc = PageHeapScavenger::GetStats();
    return CentralBenchStats{
            cc.transfer_cache_drained_bytes.load(std::memory_order_relaxed),
            cc.spans_unpinned_by_direct_release.load(std::memory_order_relaxed),
            cc.fetch_transfer_hit_objects.load(std::memory_order_relaxed),
            cc.fetch_span_list_objects.load(std::memory_order_relaxed),
            cc.fetch_pagecache_spans.load(std::memory_order_relaxed),
            cc.release_transfer_overflow_objects.load(std::memory_order_relaxed),
            cc.release_spans_returned_to_pagecache.load(std::memory_order_relaxed),
            pa.normal_alloc_success.load(std::memory_order_relaxed),
            cc.spanlist_rotations.load(std::memory_order_relaxed),
            cc.spanlist_traversals.load(std::memory_order_relaxed),
            sc.scavenge_passes.load(std::memory_order_relaxed),
            sc.scavenged_spans.load(std::memory_order_relaxed),
            sc.scavenged_bytes.load(std::memory_order_relaxed),
            sc.madvise_success_count.load(std::memory_order_relaxed),
    };
}

CentralBenchStats operator-(const CentralBenchStats& a, const CentralBenchStats& b) {
    return CentralBenchStats{
            a.transfer_cache_drained_bytes - b.transfer_cache_drained_bytes,
            a.spans_unpinned_by_direct_release - b.spans_unpinned_by_direct_release,
            a.fetch_transfer_hit_objects - b.fetch_transfer_hit_objects,
            a.fetch_span_list_objects - b.fetch_span_list_objects,
            a.fetch_pagecache_spans - b.fetch_pagecache_spans,
            a.release_transfer_overflow_objects - b.release_transfer_overflow_objects,
            a.release_spans_returned_to_pagecache - b.release_spans_returned_to_pagecache,
            a.page_allocator_normal_success - b.page_allocator_normal_success,
            a.spanlist_rotations - b.spanlist_rotations,
            a.spanlist_traversals - b.spanlist_traversals,
            a.scavenge_passes - b.scavenge_passes,
            a.scavenged_spans - b.scavenged_spans,
            a.scavenged_bytes - b.scavenged_bytes,
            a.madvise_success_count - b.madvise_success_count,
    };
}

CentralBenchStats operator+(const CentralBenchStats& a, const CentralBenchStats& b) {
    return CentralBenchStats{
            a.transfer_cache_drained_bytes + b.transfer_cache_drained_bytes,
            a.spans_unpinned_by_direct_release + b.spans_unpinned_by_direct_release,
            a.fetch_transfer_hit_objects + b.fetch_transfer_hit_objects,
            a.fetch_span_list_objects + b.fetch_span_list_objects,
            a.fetch_pagecache_spans + b.fetch_pagecache_spans,
            a.release_transfer_overflow_objects + b.release_transfer_overflow_objects,
            a.release_spans_returned_to_pagecache + b.release_spans_returned_to_pagecache,
            a.page_allocator_normal_success + b.page_allocator_normal_success,
            a.spanlist_rotations + b.spanlist_rotations,
            a.spanlist_traversals + b.spanlist_traversals,
            a.scavenge_passes + b.scavenge_passes,
            a.scavenged_spans + b.scavenged_spans,
            a.scavenged_bytes + b.scavenged_bytes,
            a.madvise_success_count + b.madvise_success_count,
    };
}

void ReportStats(benchmark::State& state, const CentralBenchStats& delta) {
    using benchmark::Counter;
    state.counters["tc_drain_Bps"] =
            Counter(static_cast<double>(delta.transfer_cache_drained_bytes),
                    Counter::kIsRate);
    state.counters["span_unpin_direct_rate"] =
            Counter(static_cast<double>(delta.spans_unpinned_by_direct_release),
                    Counter::kIsRate);
    state.counters["span_return_pc_rate"] =
            Counter(static_cast<double>(delta.release_spans_returned_to_pagecache),
                    Counter::kIsRate);
    state.counters["fetch_hit_rate"] =
            Counter(static_cast<double>(delta.fetch_transfer_hit_objects),
                    Counter::kIsRate);
    state.counters["fetch_span_rate"] =
            Counter(static_cast<double>(delta.fetch_span_list_objects),
                    Counter::kIsRate);
    state.counters["fetch_pc_rate"] =
            Counter(static_cast<double>(delta.fetch_pagecache_spans),
                    Counter::kIsRate);
    state.counters["rel_overflow_rate"] =
            Counter(static_cast<double>(delta.release_transfer_overflow_objects),
                    Counter::kIsRate);
    state.counters["pa_success_rate"] =
            Counter(static_cast<double>(delta.page_allocator_normal_success),
                    Counter::kIsRate);
    state.counters["span_rotations_rate"] =
            Counter(static_cast<double>(delta.spanlist_rotations), Counter::kIsRate);
    state.counters["span_traversals_rate"] =
            Counter(static_cast<double>(delta.spanlist_traversals), Counter::kIsRate);
    state.counters["scav_passes_rate"] =
            Counter(static_cast<double>(delta.scavenge_passes), Counter::kIsRate);
    state.counters["scav_spans_rate"] =
            Counter(static_cast<double>(delta.scavenged_spans), Counter::kIsRate);
    state.counters["scav_bytes_Bps"] =
            Counter(static_cast<double>(delta.scavenged_bytes), Counter::kIsRate);
    state.counters["madvise_ok_rate"] =
            Counter(static_cast<double>(delta.madvise_success_count), Counter::kIsRate);
}

// Lock wait/hold percentiles are only populated in the AMMALLOC_INSTRUMENT_LOCKS
// build; the default build reports zeros so filter output stays comparable.
// Called only from thread 0 of each benchmark to avoid double-counting.
void ReportLockHistogram(benchmark::State& state) {
    using benchmark::Counter;
    const auto spin = detail::GetLockHistogramSnapshot(detail::LockKind::kTransferSpin);
    const auto mutex = detail::GetLockHistogramSnapshot(detail::LockKind::kSpanListMutex);
    state.counters["spin_wait_p50_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(spin.wait_buckets, spin.wait_samples, 0.50)));
    state.counters["spin_wait_p99_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(spin.wait_buckets, spin.wait_samples, 0.99)));
    state.counters["spin_hold_p99_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(spin.hold_buckets, spin.hold_samples, 0.99)));
    state.counters["mutex_wait_p50_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(mutex.wait_buckets, mutex.wait_samples, 0.50)));
    state.counters["mutex_wait_p99_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(mutex.wait_buckets, mutex.wait_samples, 0.99)));
    state.counters["mutex_hold_p99_ns"] = Counter(static_cast<double>(
            detail::HistogramPercentileNs(mutex.hold_buckets, mutex.hold_samples, 0.99)));
}

// PrepGuard accumulates the telemetry contributed by paused setup work so
// ReportStats can subtract it from the whole-run delta. Typical usage:
//   PrepGuard prep;
//   ...setup outside timing...
//   const auto before = SnapshotStats();
//   for (auto _: state) {
//       timed work;
//       state.PauseTiming();
//       prep.Begin();
//       setup;
//       prep.End();
//       state.ResumeTiming();
//   }
//   ReportStats(state, prep.NetDelta(before));
class PrepGuard {
public:
    void Begin() { setup_start_ = SnapshotStats(); }

    void End() { absorbed_ = absorbed_ + (SnapshotStats() - setup_start_); }

    CentralBenchStats NetDelta(const CentralBenchStats& run_start) const {
        return (SnapshotStats() - run_start) - absorbed_;
    }

private:
    CentralBenchStats absorbed_{};
    CentralBenchStats setup_start_{};
};

CentralCache& GetCentralCache() {
    return CentralCache::GetInstance();
}

void ResetCentralBenchmarkState(const benchmark::State&) {
    // Google Benchmark invokes Setup after the preceding worker group has
    // joined. Purge the main worker's TLS before resetting global ownership.
    am_thread_cache_purge();
    GetCentralCache().Reset();
    PageCache::GetInstance().Reset();
    // Zero lock histograms so each scenario reports only its own contention.
    detail::ResetLockHistograms();
}

void TearDownCentralBenchmarkState(const benchmark::State&) {
    // CentralCache::Reset requires quiescence and no ThreadCache-owned objects.
    am_thread_cache_purge();
    GetCentralCache().Reset();
    PageCache::GetInstance().Reset();
}

void* DetachFreeList(FreeList& list) {
    void* head = nullptr;
    while (!list.empty()) {
        auto* object = static_cast<FreeBlock*>(list.Pop());
        object->next = static_cast<FreeBlock*>(head);
        head = object;
    }
    return head;
}

void ReleaseFreeList(FreeList& list, size_t size, CentralReleaseMode mode) {
    GetCentralCache().ReleaseListToSpans(DetachFreeList(list), SizeClass::Index(size), mode);
}

void ReleaseOneToBitmap(void* object, size_t size) {
    if (!object) {
        return;
    }
    static_cast<FreeBlock*>(object)->next = nullptr;
    GetCentralCache().ReleaseListToSpans(object, SizeClass::Index(size),
                                         CentralReleaseMode::kSpanBitmap);
}

void DrainAllTransferCaches() {
    static_cast<void>(GetCentralCache().DrainTransferCaches(
            std::numeric_limits<size_t>::max()));
}

bool FetchExact(FreeList& list, size_t count, size_t size) {
    if (GetCentralCache().FetchRange(list, count, size) == count) {
        return true;
    }
    // A short batch still transfers ownership of the returned prefix.  Restore
    // it before reporting a failed scenario so no later benchmark observes it.
    ReleaseFreeList(list, size, CentralReleaseMode::kSpanBitmap);
    DrainAllTransferCaches();
    return false;
}

bool PrepareHeldObjects(FreeList& list, size_t count, size_t size) {
    size_t remaining = count;
    while (remaining > 0) {
        const size_t request = std::min(remaining, SizeClass::kMaxBatchSize);
        if (!FetchExact(list, request, size)) {
            return false;
        }
        // Each direct fetch prefetches another request into TransferCache.  The
        // caller-held list pins the Span while this removes that prefetch.
        DrainAllTransferCaches();
        remaining -= request;
    }
    return list.size() == count;
}

bool PrepareTransferCache(size_t size, size_t count) {
    FreeList held;
    if (!PrepareHeldObjects(held, count, size)) {
        ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
        return false;
    }
    ReleaseFreeList(held, size, CentralReleaseMode::kTransferCache);
    return GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) == count;
}

bool PrepareFetchHit(size_t size, size_t request) {
    FreeList caller;
    if (!FetchExact(caller, request, size)) {
        return false;
    }
    // Keep only FetchRange's prefetched objects in TransferCache.
    ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
    return GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) == request;
}

bool PrepareSpanAnchor(size_t size, void** anchor) {
    FreeList caller;
    if (!FetchExact(caller, 1, size)) {
        return false;
    }
    *anchor = caller.Pop();
    // The anchor keeps the Span in CentralCache while the prefetch is returned.
    DrainAllTransferCaches();
    if (*anchor != nullptr &&
        GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) == 0) {
        return true;
    }
    ReleaseOneToBitmap(*anchor, size);
    *anchor = nullptr;
    return false;
}

bool SeedPartialTransferCache(size_t size, size_t partial) {
    FreeList caller;
    if (!FetchExact(caller, partial, size)) {
        return false;
    }
    ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
    return GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) == partial;
}

bool PreparePartialFetch(size_t size, size_t request, size_t partial, void** anchor) {
    if (partial == 0 || partial >= request || !PrepareSpanAnchor(size, anchor)) {
        return false;
    }
    if (SeedPartialTransferCache(size, partial)) {
        return true;
    }
    ReleaseOneToBitmap(*anchor, size);
    *anchor = nullptr;
    return false;
}

bool PrepareReleaseWithTransferOccupancy(void** chain, size_t size,
                                         size_t initial_transfer_count) {
    FreeList held;
    if (!PrepareHeldObjects(held, kReleaseChainSize, size) ||
        !PrepareTransferCache(size, initial_transfer_count)) {
        ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
        return false;
    }
    *chain = DetachFreeList(held);
    return true;
}

bool PrepareDetachedHeldObjects(void** chain, size_t count, size_t size) {
    FreeList held;
    if (!PrepareHeldObjects(held, count, size)) {
        ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
        return false;
    }
    *chain = DetachFreeList(held);
    return true;
}

void ReleaseChainToBitmap(void** chain, size_t size) {
    GetCentralCache().ReleaseListToSpans(*chain, SizeClass::Index(size),
                                         CentralReleaseMode::kSpanBitmap);
    *chain = nullptr;
}

bool PrepareFragmentedSpans(FreeList& first, FreeList& second, size_t size) {
    constexpr size_t kSpanCapacity = 16;
    if (!FetchExact(first, kSpanCapacity, size) ||
        !FetchExact(second, kSpanCapacity, size)) {
        ReleaseFreeList(first, size, CentralReleaseMode::kSpanBitmap);
        ReleaseFreeList(second, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }

    if (GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) != 0) {
        return false;
    }

    void* first_free = first.Pop();
    void* second_free = second.Pop();
    if (PageMap::GetSpan(first_free) == PageMap::GetSpan(second_free)) {
        ReleaseOneToBitmap(first_free, size);
        ReleaseOneToBitmap(second_free, size);
        return false;
    }

    ReleaseOneToBitmap(first_free, size);
    ReleaseOneToBitmap(second_free, size);
    return true;
}

// Fetches exactly `target_spans * span_capacity` objects for `size`, so the
// returned chain fully pins `target_spans` distinct Spans. Probes one object
// first to learn the per-Span capacity, then returns the probe and re-fetches
// the whole chain through PrepareHeldObjects for a deterministic layout.
bool PrepareMultiSpanChain(void** chain, size_t size, size_t target_spans,
                           size_t* total_count) {
    *chain = nullptr;
    *total_count = 0;
    FreeList probe_list;
    if (!FetchExact(probe_list, 1, size)) {
        return false;
    }
    void* probe = probe_list.Pop();
    Span* probe_span = PageMap::GetSpan(probe);
    if (!probe_span || probe_span->capacity == 0) {
        ReleaseOneToBitmap(probe, size);
        return false;
    }
    const size_t span_capacity = probe_span->capacity;
    ReleaseOneToBitmap(probe, size);
    DrainAllTransferCaches();

    const size_t needed = target_spans * span_capacity;
    FreeList held;
    if (!PrepareHeldObjects(held, needed, size)) {
        ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }
    *chain = DetachFreeList(held);
    *total_count = needed;
    return true;
}

void SetProcessed(benchmark::State& state, int64_t objects, size_t object_size) {
    state.SetItemsProcessed(objects);
    state.SetBytesProcessed(objects * static_cast<int64_t>(object_size));
}

void BM_CentralCache_Fetch_TransferHit(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    PrepGuard prep;
    if (!PrepareFetchHit(size, request)) {
        state.SkipWithError("failed to prepare a full TransferCache hit");
        return;
    }

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("TransferCache hit returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kTransferCache);
        prep.End();
        state.ResumeTiming();
    }
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Fetch_PartialTransferAndSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    const size_t partial = request / 2;
    void* anchor = nullptr;
    PrepGuard prep;
    if (!PreparePartialFetch(size, request, partial, &anchor)) {
        state.SkipWithError("failed to prepare a partial TransferCache batch");
        return;
    }

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("partial TransferCache fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        if (!SeedPartialTransferCache(size, partial)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore the partial TransferCache batch");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseOneToBitmap(anchor, size);
    DrainAllTransferCaches();
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Fetch_ExistingSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    void* anchor = nullptr;
    PrepGuard prep;
    if (!PrepareSpanAnchor(size, &anchor)) {
        state.SkipWithError("failed to retain an active SpanList candidate");
        return;
    }

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("existing SpanList fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        prep.End();
        state.ResumeTiming();
    }
    ReleaseOneToBitmap(anchor, size);
    DrainAllTransferCaches();
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Fetch_PageCacheReuse(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    PrepGuard prep;
    FreeList primed;
    if (!FetchExact(primed, request, size)) {
        state.SkipWithError("failed to prime PageCache for a CentralCache refill");
        return;
    }
    ReleaseFreeList(primed, size, CentralReleaseMode::kSpanBitmap);
    DrainAllTransferCaches();

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("PageCache reuse fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        // Direct release plus drain empties the Span, so the next timed fetch
        // must leave CentralCache and acquire a Span from PageCache again.
        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        prep.End();
        state.ResumeTiming();
    }
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Fetch_PageAllocatorFallback(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    PrepGuard prep;
    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("PageAllocator fallback fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        // All objects are back in PageCache before Reset removes its reusable
        // Span, making the next FetchRange exercise PageAllocator again.
        PageCache::GetInstance().Reset();
        prep.End();
        state.ResumeTiming();
    }
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Fetch_FragmentedSpans(benchmark::State& state) {
    // Fragmentation only bites when a single Span cannot hold the working set;
    // large-object classes are the interesting case, so this benchmark keeps
    // MAX_TC_SIZE and is not swept across the 5-size matrix.
    constexpr size_t size = SizeConfig::MAX_TC_SIZE;
    FreeList first;
    FreeList second;
    PrepGuard prep;
    if (!PrepareFragmentedSpans(first, second, size)) {
        ReleaseFreeList(first, size, CentralReleaseMode::kSpanBitmap);
        ReleaseFreeList(second, size, CentralReleaseMode::kSpanBitmap);
        state.SkipWithError("failed to prepare two fragmented SpanList candidates");
        return;
    }

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, 1, size)) {
            state.SkipWithError("fragmented SpanList fetch returned a short batch");
            break;
        }
        ++fetched_objects;

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        prep.End();
        state.ResumeTiming();
    }
    ReleaseFreeList(first, size, CentralReleaseMode::kSpanBitmap);
    ReleaseFreeList(second, size, CentralReleaseMode::kSpanBitmap);
    DrainAllTransferCaches();
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_TransferEmpty(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareDetachedHeldObjects(&chain, kReleaseChainSize, size)) {
        state.SkipWithError("failed to prepare a release chain with an empty TransferCache");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size));
        chain = nullptr;
        released_objects += static_cast<int64_t>(kReleaseChainSize);

        state.PauseTiming();
        prep.Begin();
        DrainAllTransferCaches();
        if (!PrepareDetachedHeldObjects(&chain, kReleaseChainSize, size)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore an empty-TransferCache release chain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_TransferPartial(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t capacity = TransferCapacity(size);
    const size_t initial_count = capacity - kReleaseChainSize / 2;
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareReleaseWithTransferOccupancy(&chain, size, initial_count)) {
        state.SkipWithError("failed to prepare a partially full TransferCache");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size));
        chain = nullptr;
        released_objects += static_cast<int64_t>(kReleaseChainSize);

        state.PauseTiming();
        prep.Begin();
        if (GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) !=
            capacity) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("partial TransferCache did not fill to capacity");
            break;
        }
        DrainAllTransferCaches();
        if (!PrepareReleaseWithTransferOccupancy(&chain, size, initial_count)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a partially full TransferCache");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_TransferFullToBitmap(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t capacity = TransferCapacity(size);
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareReleaseWithTransferOccupancy(&chain, size, capacity)) {
        state.SkipWithError("failed to prepare a full TransferCache");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size));
        chain = nullptr;
        released_objects += static_cast<int64_t>(kReleaseChainSize);

        state.PauseTiming();
        prep.Begin();
        if (GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) !=
            capacity) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("full TransferCache unexpectedly accepted an object");
            break;
        }
        DrainAllTransferCaches();
        if (!PrepareReleaseWithTransferOccupancy(&chain, size, capacity)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a full TransferCache");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_DirectBitmapRetainedSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    void* anchor = nullptr;
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareSpanAnchor(size, &anchor) ||
        !PrepareDetachedHeldObjects(&chain, kReleaseChainSize, size)) {
        ReleaseChainToBitmap(&chain, size);
        if (anchor) {
            ReleaseOneToBitmap(anchor, size);
        }
        state.SkipWithError("failed to prepare a direct-bitmap retained Span");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size),
                                             CentralReleaseMode::kSpanBitmap);
        chain = nullptr;
        released_objects += static_cast<int64_t>(kReleaseChainSize);

        state.PauseTiming();
        prep.Begin();
        if (!PrepareDetachedHeldObjects(&chain, kReleaseChainSize, size)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore direct-bitmap retained Span state");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    ReleaseOneToBitmap(anchor, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_DirectBitmapUnpinSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareDetachedHeldObjects(&chain, 1, size)) {
        state.SkipWithError("failed to prepare a single-span pin");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size),
                                             CentralReleaseMode::kSpanBitmap);
        chain = nullptr;
        ++released_objects;

        state.PauseTiming();
        prep.Begin();
        if (!PrepareDetachedHeldObjects(&chain, 1, size)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a single-span pin");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_DirectBitmapUnpinMultiSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    constexpr size_t kTargetSpans = 3;
    void* chain = nullptr;
    size_t chain_count = 0;
    PrepGuard prep;
    if (!PrepareMultiSpanChain(&chain, size, kTargetSpans, &chain_count)) {
        state.SkipWithError("failed to prepare a multi-span release chain");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size),
                                             CentralReleaseMode::kSpanBitmap);
        chain = nullptr;
        released_objects += static_cast<int64_t>(chain_count);

        state.PauseTiming();
        prep.Begin();
        DrainAllTransferCaches();
        if (!PrepareMultiSpanChain(&chain, size, kTargetSpans, &chain_count)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a multi-span release chain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Release_LongChain(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t chain_count = static_cast<size_t>(state.range(1));
    void* chain = nullptr;
    PrepGuard prep;
    if (!PrepareDetachedHeldObjects(&chain, chain_count, size)) {
        state.SkipWithError("failed to prepare a multi-batch release chain");
        return;
    }

    const auto before = SnapshotStats();
    int64_t released_objects = 0;
    for (auto _: state) {
        GetCentralCache().ReleaseListToSpans(chain, SizeClass::Index(size));
        chain = nullptr;
        released_objects += static_cast<int64_t>(chain_count);

        state.PauseTiming();
        prep.Begin();
        DrainAllTransferCaches();
        if (!PrepareDetachedHeldObjects(&chain, chain_count, size)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a multi-batch release chain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    ReleaseChainToBitmap(&chain, size);
    DrainAllTransferCaches();
    SetProcessed(state, released_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Drain_Empty(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    // Any budget <= size exercises the empty-cache path; use SIZE_MAX so the
    // full class sweep runs even when TransferCache is empty.
    constexpr size_t kBudget = std::numeric_limits<size_t>::max();
    PrepGuard prep;
    const auto before = SnapshotStats();
    int64_t drained_bytes = 0;
    for (auto _: state) {
        const size_t drained = GetCentralCache().DrainTransferCaches(kBudget);
        benchmark::DoNotOptimize(drained);
        if (drained != 0) {
            state.SkipWithError("empty TransferCache drain detached objects");
            break;
        }
    }
    static_cast<void>(size);// size only selects the Args row for reporting.
    state.SetBytesProcessed(drained_bytes);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Drain_Bounded(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const int budget_kind = static_cast<int>(state.range(1));
    const size_t batch = SizeClass::CalculateBatchSize(size);
    const size_t object_count = std::min(TransferCapacity(size), 4 * batch);
    size_t byte_budget = 0;
    switch (budget_kind) {
        case 0: byte_budget = 0; break;
        case 1: byte_budget = size - 1; break;
        case 2: byte_budget = size; break;
        case 3: byte_budget = batch * size; break;
        case 4: byte_budget = 2 * batch * size; break;
        default:
            state.SkipWithError("unknown budget_kind");
            return;
    }
    if (byte_budget > object_count * size) {
        byte_budget = object_count * size;
    }

    PrepGuard prep;
    if (!PrepareTransferCache(size, object_count)) {
        state.SkipWithError("failed to prepare a bounded drain");
        return;
    }

    const size_t expected = std::min(object_count, byte_budget / size) * size;
    const auto before = SnapshotStats();
    int64_t drained_bytes = 0;
    for (auto _: state) {
        const size_t drained = GetCentralCache().DrainTransferCaches(byte_budget);
        benchmark::DoNotOptimize(drained);
        if (drained != expected) {
            state.SkipWithError("bounded drain did not honor the exact byte budget");
            break;
        }
        drained_bytes += static_cast<int64_t>(drained);

        state.PauseTiming();
        prep.Begin();
        if (!PrepareTransferCache(size, object_count)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a bounded drain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    state.SetBytesProcessed(drained_bytes);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_Drain_MultipleBatches(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    // Force at least two internal kMaxBatchSize iterations. Only classes whose
    // transfer capacity exceeds kMaxBatchSize can host that many objects; the
    // registration skips sizes where this is impossible.
    const size_t object_count = 2 * SizeClass::kMaxBatchSize;
    PrepGuard prep;
    if (!PrepareTransferCache(size, object_count)) {
        state.SkipWithError("failed to prepare a multi-batch TransferCache drain");
        return;
    }

    const size_t expected = object_count * size;
    const auto before = SnapshotStats();
    int64_t drained_bytes = 0;
    for (auto _: state) {
        const size_t drained = GetCentralCache().DrainTransferCaches(expected);
        benchmark::DoNotOptimize(drained);
        if (drained != expected) {
            state.SkipWithError("multi-batch drain returned an unexpected byte count");
            break;
        }
        drained_bytes += static_cast<int64_t>(drained);

        state.PauseTiming();
        prep.Begin();
        if (!PrepareTransferCache(size, object_count)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a multi-batch TransferCache drain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    state.SetBytesProcessed(drained_bytes);
    ReportStats(state, prep.NetDelta(before));
}

bool PrepareMultiClassDrain(size_t small_size, size_t large_size,
                            size_t small_count, size_t large_count) {
    FreeList small;
    FreeList large;
    if (!FetchExact(small, small_count, small_size) ||
        !FetchExact(large, large_count, large_size)) {
        ReleaseFreeList(small, small_size, CentralReleaseMode::kSpanBitmap);
        ReleaseFreeList(large, large_size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }

    DrainAllTransferCaches();
    ReleaseFreeList(small, small_size, CentralReleaseMode::kTransferCache);
    ReleaseFreeList(large, large_size, CentralReleaseMode::kTransferCache);
    return GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(small_size)) ==
                   small_count &&
           GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(large_size)) ==
                   large_count;
}

void BM_CentralCache_Drain_UnboundedMultiClass(benchmark::State& state) {
    constexpr size_t kSmallSize = 64;
    constexpr size_t kLargeSize = SizeConfig::MAX_TC_SIZE;
    constexpr size_t kSmallCount = 8;
    constexpr size_t kLargeCount = 4;
    PrepGuard prep;
    if (!PrepareMultiClassDrain(kSmallSize, kLargeSize, kSmallCount, kLargeCount)) {
        state.SkipWithError("failed to prepare a multi-class TransferCache drain");
        return;
    }

    const size_t expected = kSmallCount * kSmallSize + kLargeCount * kLargeSize;
    const auto before = SnapshotStats();
    int64_t drained_bytes = 0;
    for (auto _: state) {
        const size_t drained = GetCentralCache().DrainTransferCaches(
                std::numeric_limits<size_t>::max());
        benchmark::DoNotOptimize(drained);
        if (drained != expected) {
            state.SkipWithError("unbounded multi-class drain returned an unexpected byte count");
            break;
        }
        drained_bytes += static_cast<int64_t>(drained);

        state.PauseTiming();
        prep.Begin();
        if (!PrepareMultiClassDrain(kSmallSize, kLargeSize, kSmallCount, kLargeCount)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a multi-class TransferCache drain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    state.SetBytesProcessed(drained_bytes);
    ReportStats(state, prep.NetDelta(before));
}

bool PrepareWrappedDrain(size_t size, size_t capacity) {
    const size_t half_capacity = capacity / 2;
    FreeList initial;
    FreeList appended;
    // PrepareHeldObjects chunks fetches at kMaxBatchSize and drains the
    // prefetch after each chunk, so the caller is the sole owner of every
    // object before we stage the TransferCache ring.
    if (!PrepareHeldObjects(initial, capacity, size)) {
        ReleaseFreeList(initial, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }
    if (!PrepareHeldObjects(appended, half_capacity, size)) {
        ReleaseFreeList(initial, size, CentralReleaseMode::kSpanBitmap);
        ReleaseFreeList(appended, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }
    // Fill TransferCache to full capacity, then drain the cold half so
    // `transfer_cache_begin` advances by half_capacity. The second release
    // then wraps around the ring tail.
    ReleaseFreeList(initial, size, CentralReleaseMode::kTransferCache);
    if (GetCentralCache().DrainTransferCaches(half_capacity * size) !=
        half_capacity * size) {
        ReleaseFreeList(appended, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }
    ReleaseFreeList(appended, size, CentralReleaseMode::kTransferCache);
    return GetCentralCache().GetTransferCacheCountForTest(SizeClass::Index(size)) ==
           capacity;
}

void BM_CentralCache_Drain_ColdEndWrap(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t capacity = TransferCapacity(size);
    PrepGuard prep;
    if (!PrepareWrappedDrain(size, capacity)) {
        state.SkipWithError("failed to prepare a wrapped TransferCache drain");
        return;
    }

    const size_t expected = capacity * size;
    const auto before = SnapshotStats();
    int64_t drained_bytes = 0;
    for (auto _: state) {
        const size_t drained = GetCentralCache().DrainTransferCaches(expected);
        benchmark::DoNotOptimize(drained);
        if (drained != expected) {
            state.SkipWithError("wrapped TransferCache drain returned an unexpected byte count");
            break;
        }
        drained_bytes += static_cast<int64_t>(drained);

        state.PauseTiming();
        prep.Begin();
        DrainAllTransferCaches();
        if (!PrepareWrappedDrain(size, capacity)) {
            prep.End();
            state.ResumeTiming();
            state.SkipWithError("failed to restore a wrapped TransferCache drain");
            break;
        }
        prep.End();
        state.ResumeTiming();
    }
    state.SetBytesProcessed(drained_bytes);
    ReportStats(state, prep.NetDelta(before));
}

template<bool MixedBuckets, bool WithDrainer, bool DirectBitmapRelease>
void BM_CentralCache_Contention(benchmark::State& state) {
    constexpr std::array<size_t, 4> kDistinctBuckets{16, 64, 256, 4096};
    constexpr size_t kSameBucketSize = 64;
    constexpr size_t kRequestPerIter = 8;
    const size_t size = MixedBuckets
                                ? kDistinctBuckets[static_cast<size_t>(state.thread_index()) %
                                                   kDistinctBuckets.size()]
                                : kSameBucketSize;
    // Thread 0 snapshots global counters around the whole group; KeepRunning's
    // internal end barrier keeps the after-snapshot within one iteration of
    // every worker finishing, which is close enough for aggregate attribution.
    CentralBenchStats before{};
    if (state.thread_index() == 0) {
        before = SnapshotStats();
    }
    int64_t processed_objects = 0;
    for (auto _: state) {
        if constexpr (WithDrainer) {
            if (state.thread_index() == 0) {
                const size_t drained =
                        GetCentralCache().DrainTransferCaches(8 * kSameBucketSize);
                benchmark::DoNotOptimize(drained);
                continue;
            }
        }

        FreeList caller;
        if (!FetchExact(caller, kRequestPerIter, size)) {
            state.SkipWithError("contention fetch returned a short batch");
            break;
        }
        const CentralReleaseMode release_mode =
                DirectBitmapRelease ? CentralReleaseMode::kSpanBitmap
                                    : CentralReleaseMode::kTransferCache;
        GetCentralCache().ReleaseListToSpans(DetachFreeList(caller), SizeClass::Index(size),
                                             release_mode);
        processed_objects += static_cast<int64_t>(kRequestPerIter);
    }
    SetProcessed(state, processed_objects, size);
    if (state.thread_index() == 0) {
        ReportStats(state, SnapshotStats() - before);
        ReportLockHistogram(state);
    }
}

struct SpscTransfer {
    void* head{nullptr};
    size_t count{0};
};

class SpscTransferQueue {
public:
    static constexpr size_t kCapacity = 8;

    void Reset() {
        producer_pos_.store(0, std::memory_order_relaxed);
        consumer_pos_.store(0, std::memory_order_relaxed);
    }

    void Push(SpscTransfer transfer) {
        size_t producer = producer_pos_.load(std::memory_order_relaxed);
        while (producer - consumer_pos_.load(std::memory_order_acquire) == kCapacity) {
            detail::CPUPause();
        }
        slots_[producer & (kCapacity - 1)] = transfer;
        producer_pos_.store(producer + 1, std::memory_order_release);
    }

    SpscTransfer Pop() {
        size_t consumer = consumer_pos_.load(std::memory_order_relaxed);
        while (consumer == producer_pos_.load(std::memory_order_acquire)) {
            detail::CPUPause();
        }
        SpscTransfer transfer = slots_[consumer & (kCapacity - 1)];
        consumer_pos_.store(consumer + 1, std::memory_order_release);
        return transfer;
    }

private:
    static_assert((kCapacity & (kCapacity - 1)) == 0);
    std::array<SpscTransfer, kCapacity> slots_{};
    alignas(SystemConfig::CACHE_LINE_SIZE) std::atomic<size_t> producer_pos_{0};
    alignas(SystemConfig::CACHE_LINE_SIZE) std::atomic<size_t> consumer_pos_{0};
};

SpscTransferQueue spsc_queue;

void SetupSpscTransfer(const benchmark::State& state) {
    ResetCentralBenchmarkState(state);
    spsc_queue.Reset();
}

void TearDownSpscTransfer(const benchmark::State& state) {
    TearDownCentralBenchmarkState(state);
}

void BM_CentralCache_CrossThread_SpscHandoff(benchmark::State& state) {
    int64_t transferred_objects = 0;
    for (auto _: state) {
        if (state.thread_index() == 0) {
            FreeList caller;
            const size_t fetched =
                    GetCentralCache().FetchRange(caller, kSpscBatchSize, kSpscObjectSize);
            if (fetched != kSpscBatchSize) {
                spsc_queue.Push(SpscTransfer{});
                state.SkipWithError("SPSC producer fetch returned a short batch");
                break;
            }
            spsc_queue.Push(SpscTransfer{DetachFreeList(caller), fetched});
            transferred_objects += static_cast<int64_t>(fetched);
        } else {
            const SpscTransfer transfer = spsc_queue.Pop();
            if (!transfer.head || transfer.count != kSpscBatchSize) {
                state.SkipWithError("SPSC producer failed to publish a complete batch");
                break;
            }
            GetCentralCache().ReleaseListToSpans(transfer.head,
                                                 SizeClass::Index(kSpscObjectSize));
        }
    }
    SetProcessed(state, transferred_objects, kSpscObjectSize);
}

// ---------- Multi-producer / multi-consumer handoff ----------
// Vyukov-style bounded MPMC ring. Producers block when full, consumers block
// when empty; both spin with a pause hint. Each slot carries one detached
// object chain so ownership transfers without touching the queue's atomics.
template<size_t Capacity>
class MpmcTransferQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    struct Slot {
        std::atomic<size_t> sequence{0};
        SpscTransfer data{};
    };

public:
    void Reset() {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        occupancy_peak_.store(0, std::memory_order_relaxed);
        overflow_events_.store(0, std::memory_order_relaxed);
    }

    void Push(SpscTransfer value) {
        size_t pos = head_.load(std::memory_order_relaxed);
        bool stalled = false;
        for (;;) {
            Slot& slot = slots_[pos & (Capacity - 1)];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.data = value;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    // Occupancy is a fuzzy peak indicator: by the time this
                    // producer samples tail, other consumers may have advanced
                    // it past this producer's slot, so skip the underflow case
                    // instead of recording a wrapped size_t.
                    const size_t tail = tail_.load(std::memory_order_relaxed);
                    if (pos + 1 >= tail) {
                        UpdatePeak(pos + 1 - tail);
                    }
                    if (stalled) {
                        overflow_events_.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
            } else if (diff < 0) {
                // Queue full: this push stalls until a consumer frees a slot.
                stalled = true;
                UpdatePeak(Capacity);
                detail::CPUPause();
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    SpscTransfer Pop() {
        size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & (Capacity - 1)];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    const SpscTransfer value = slot.data;
                    slot.sequence.store(pos + Capacity, std::memory_order_release);
                    return value;
                }
            } else if (diff < 0) {
                detail::CPUPause();
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    size_t occupancy_peak() const { return occupancy_peak_.load(std::memory_order_relaxed); }
    size_t overflow_events() const { return overflow_events_.load(std::memory_order_relaxed); }

private:
    // Best-effort peak: a single CAS avoids perturbing the measured path.
    void UpdatePeak(size_t occ) {
        size_t prev = occupancy_peak_.load(std::memory_order_relaxed);
        if (occ > prev) {
            occupancy_peak_.compare_exchange_strong(prev, occ, std::memory_order_relaxed);
        }
    }

    alignas(SystemConfig::CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(SystemConfig::CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    std::atomic<size_t> occupancy_peak_{0};
    std::atomic<size_t> overflow_events_{0};
    std::array<Slot, Capacity> slots_{};
};

template<size_t Capacity>
MpmcTransferQueue<Capacity>& GetMpmcQueue() {
    static MpmcTransferQueue<Capacity> instance;
    return instance;
}

enum class HandoffPattern { kBalanced,
                            kBursty,
                            kRateImbalance };

// Balancing rule: each producer pushes `consumers` batches per iteration and
// each consumer pops `producers` batches, so over the framework-guaranteed
// equal per-thread iteration count the group transfers
// `producers * consumers` batches per round with no residual drain.
template<HandoffPattern Pattern, size_t Capacity>
void RunHandoffWithQueue(benchmark::State& state, MpmcTransferQueue<Capacity>& queue,
                         int producers, int consumers) {
    const int tid = state.thread_index();
    const bool is_producer = tid < producers;
    const int batches_per_iter = is_producer ? consumers : producers;
    int64_t transferred_objects = 0;
    size_t iter = 0;
    for (auto _: state) {
        for (int b = 0; b < batches_per_iter; ++b) {
            if (is_producer) {
                FreeList caller;
                const size_t fetched =
                        GetCentralCache().FetchRange(caller, kSpscBatchSize, kSpscObjectSize);
                if (fetched != kSpscBatchSize) {
                    queue.Push(SpscTransfer{});
                    state.SkipWithError("handoff producer fetch returned a short batch");
                    break;
                }
                queue.Push(SpscTransfer{DetachFreeList(caller), fetched});
                transferred_objects += static_cast<int64_t>(fetched);
            } else {
                const SpscTransfer transfer = queue.Pop();
                if (!transfer.head || transfer.count != kSpscBatchSize) {
                    state.SkipWithError("handoff consumer received an incomplete batch");
                    break;
                }
                GetCentralCache().ReleaseListToSpans(transfer.head,
                                                     SizeClass::Index(kSpscObjectSize));
            }
        }
        if constexpr (Pattern == HandoffPattern::kBursty) {
            // Producers emit a burst then idle, so consumers see bursty arrival.
            if (is_producer && (++iter % 4) == 0) {
                for (int k = 0; k < 8; ++k) {
                    std::this_thread::yield();
                }
            }
        } else if constexpr (Pattern == HandoffPattern::kRateImbalance) {
            // Consumers lag producers, stressing queue occupancy and backpressure.
            if (!is_producer) {
                for (int k = 0; k < 16; ++k) {
                    detail::CPUPause();
                }
            }
        }
    }
    SetProcessed(state, transferred_objects, kSpscObjectSize);
    if (tid == 0) {
        state.counters["queue_occupancy_peak"] =
                benchmark::Counter(static_cast<double>(queue.occupancy_peak()));
        state.counters["queue_overflow_events"] =
                benchmark::Counter(static_cast<double>(queue.overflow_events()));
    }
}

// Producers and Consumers are compile-time template parameters so each
// registration can pin a single Threads(Producers+Consumers) value; google
// benchmark combines Args and Threads as a cartesian product, so the thread
// count cannot be derived from an Arg. Only the queue capacity Q is a runtime
// Arg (range 0), dispatched to a compile-time ring below.
template<int Producers, int Consumers, HandoffPattern Pattern>
void RunCrossThreadHandoff(benchmark::State& state) {
    const size_t q = static_cast<size_t>(state.range(0));
    if (q <= 8) {
        RunHandoffWithQueue<Pattern, 8>(state, GetMpmcQueue<8>(), Producers, Consumers);
    } else {
        RunHandoffWithQueue<Pattern, 64>(state, GetMpmcQueue<64>(), Producers, Consumers);
    }
}

void SetupCrossThread(const benchmark::State& state) {
    ResetCentralBenchmarkState(state);
    GetMpmcQueue<8>().Reset();
    GetMpmcQueue<64>().Reset();
}

template<int Producers, HandoffPattern Pattern = HandoffPattern::kBalanced>
void BM_CentralCache_CrossThread_Mpsc(benchmark::State& state) {
    RunCrossThreadHandoff<Producers, 1, Pattern>(state);
}

template<int Consumers, HandoffPattern Pattern = HandoffPattern::kBalanced>
void BM_CentralCache_CrossThread_Spmc(benchmark::State& state) {
    RunCrossThreadHandoff<1, Consumers, Pattern>(state);
}

template<int Producers, int Consumers, HandoffPattern Pattern>
void BM_CentralCache_CrossThread_Mpmc(benchmark::State& state) {
    RunCrossThreadHandoff<Producers, Consumers, Pattern>(state);
}

#ifdef AMMALLOC_TEST
void SetupSpanOnlyFetch(const benchmark::State& state) {
    ResetCentralBenchmarkState(state);
    // Existing unit-test injection verifies the graceful no-TransferCache
    // mode. It is intentionally unavailable in normal benchmark builds.
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    GetCentralCache().Reset();
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);
}

void BM_CentralCache_Fetch_SpanOnlyFallback(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = static_cast<size_t>(state.range(1));
    PrepGuard prep;
    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        const size_t fetched = GetCentralCache().FetchRange(caller, request, size);
        if (fetched != request) {
            state.SkipWithError("SpanList-only fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(fetched);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        prep.End();
        state.ResumeTiming();
    }
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}
#endif

// ---------- SpanList specialization ----------
// These scenarios pin exactly one Span in a bucket at a controlled free-slot
// count so the SpanList traversal, bitmap density, and rotation paths can be
// attributed via span_traversals_rate / span_rotations_rate. Every iteration
// releases the fetched batch and drains the prefetch, which restores the
// Span's free-slot count and keeps the layout stable across iterations.

// Probes one Span's capacity, then pins objects in `held` so the surviving
// Span has the requested free-slot count. A `target_free` of SIZE_MAX is a
// sentinel meaning "mostly free" (pin exactly one object). The Span stays in
// span_list because use_count > 0.
bool PrepareSpanWithFreeSlots(size_t size, size_t target_free, FreeList& held) {
    FreeList probe;
    if (!FetchExact(probe, 1, size)) {
        return false;
    }
    void* probe_obj = probe.Pop();
    Span* probe_span = PageMap::GetSpan(probe_obj);
    if (!probe_span || probe_span->capacity == 0) {
        ReleaseOneToBitmap(probe_obj, size);
        return false;
    }
    const size_t capacity = probe_span->capacity;
    ReleaseOneToBitmap(probe_obj, size);
    DrainAllTransferCaches();

    size_t hold = 0;
    if (target_free == SIZE_MAX) {
        hold = 1;// Mostly-free Span: pin a single object.
    } else {
        if (target_free > capacity) {
            return false;
        }
        hold = capacity - target_free;
    }
    if (hold > 0 && !PrepareHeldObjects(held, hold, size)) {
        ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        return false;
    }
    return held.size() == hold;
}

// Shared loop: fetch `request` from a single Span pinned at `target_free`,
// then restore the Span in the paused region. The restore path (release the
// caller batch to bitmap + drain the prefetch) returns exactly the extracted
// objects, so target_free is invariant across iterations.
void RunSpanListSingleSpan(benchmark::State& state, size_t size, size_t request,
                           size_t target_free) {
    FreeList held;
    PrepGuard prep;
    if (!PrepareSpanWithFreeSlots(size, target_free, held)) {
        state.SkipWithError("failed to pin a single Span at the target free count");
        return;
    }

    const auto before = SnapshotStats();
    int64_t fetched_objects = 0;
    for (auto _: state) {
        FreeList caller;
        if (!FetchExact(caller, request, size)) {
            state.SkipWithError("SpanList fetch returned a short batch");
            break;
        }
        fetched_objects += static_cast<int64_t>(request);

        state.PauseTiming();
        prep.Begin();
        ReleaseFreeList(caller, size, CentralReleaseMode::kSpanBitmap);
        DrainAllTransferCaches();
        prep.End();
        state.ResumeTiming();
    }
    ReleaseFreeList(held, size, CentralReleaseMode::kSpanBitmap);
    DrainAllTransferCaches();
    SetProcessed(state, fetched_objects, size);
    ReportStats(state, prep.NetDelta(before));
}

void BM_CentralCache_SpanList_DenseSingleSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = SizeClass::CalculateBatchSize(size);
    // Mostly-free Span (SIZE_MAX sentinel pins one object): extraction never
    // fills the Span, so traversals=1 and rotations=0.
    RunSpanListSingleSpan(state, size, request, /*target_free=*/SIZE_MAX);
}

void BM_CentralCache_SpanList_FillAtExtractionBoundary(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t request = SizeClass::CalculateBatchSize(size);
    // Free slots == extract_target (2 * request): the Span becomes full exactly
    // when extraction reaches its target, hitting the boundary rotation site
    // once per iteration (rotations ~= iterations).
    RunSpanListSingleSpan(state, size, request, /*target_free=*/2 * request);
}

void BM_CentralCache_SpanList_SparseSingleSpan(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t free_mult = static_cast<size_t>(state.range(1));
    const size_t request = SizeClass::CalculateBatchSize(size);
    // Free slots == free_mult * request with free_mult > 2 so the Span never
    // fills (no rotation) while the bitmap density varies: smaller free_mult
    // means a fuller Span and a longer AllocObject word scan.
    RunSpanListSingleSpan(state, size, request, /*target_free=*/free_mult * request);
}

// ---------- Args generators ----------
// Applying an Args table via ->Apply keeps the registration section compact
// and guarantees every benchmark in a family sees the same size/batch sweep.

void ApplyFetchArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        const auto s = static_cast<size_t>(size);
        const size_t batch = SizeClass::CalculateBatchSize(s);
        b->Args({size, 1});
        if (batch / 2 > 1) {
            b->Args({size, static_cast<int>(batch / 2)});
        }
        b->Args({size, static_cast<int>(batch)});
    }
}

// Same as ApplyFetchArgs but skips batch == 1 since PartialTransferAndSpan
// cannot construct a strict partial hit with a single-object request.
void ApplyPartialFetchArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        const auto s = static_cast<size_t>(size);
        const size_t batch = SizeClass::CalculateBatchSize(s);
        if (batch / 2 > 1) {
            b->Args({size, static_cast<int>(batch / 2)});
        }
        if (batch > 1) {
            b->Args({size, static_cast<int>(batch)});
        }
    }
}

void ApplySizeArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        b->Args({size});
    }
}

// Long-chain release: chain length sweeps the kMaxBatchSize boundary and
// multi-batch territory, capped per size so the working set stays < ~2 MiB.
void ApplyLongChainArgs(benchmark::internal::Benchmark* b) {
    struct Row {
        int size;
        std::array<int, 5> chains;
        int count;
    };
    constexpr std::array<Row, 5> kRows{{
            {16, {511, 512, 513, 1024, 4096}, 5},
            {64, {511, 512, 513, 1024, 4096}, 5},
            {256, {128, 512, 1024, 4096, 0}, 4},
            {4096, {8, 64, 512, 1024, 0}, 4},
            {32768, {2, 16, 64, 0, 0}, 3},
    }};
    for (const auto& row : kRows) {
        for (int i = 0; i < row.count; ++i) {
            b->Args({row.size, row.chains[static_cast<size_t>(i)]});
        }
    }
}

// Bounded-drain budgets: 0, S-1, S, B*S, 2*B*S. SIZE_MAX lives in the
// dedicated UnboundedMultiClass and ColdEndWrap cases.
void ApplyDrainBoundedArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        for (int kind = 0; kind <= 4; ++kind) {
            b->Args({size, kind});
        }
    }
}

// MultipleBatches needs transfer capacity > kMaxBatchSize so at least two
// internal iterations run; skip sizes where capacity is too small.
void ApplyDrainMultipleBatchArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        const auto s = static_cast<size_t>(size);
        if (TransferCapacity(s) > SizeClass::kMaxBatchSize) {
            b->Args({size});
        }
    }
}

void ApplyContentionThreads(benchmark::internal::Benchmark* b) {
    b->Threads(2)->Threads(4)->Threads(8)->Threads(16)->UseRealTime();
}

// SpanList density sweep: free slots = free_mult * batch. free_mult > 2 keeps
// the Span from filling (no rotation) so only bitmap density varies. The
// smallest multiplier is bounded by the 32768-byte class (capacity 16, batch
// 2), where free_mult <= 8; {3, 4, 6} is safe for every swept size.
void ApplySpanListSparseArgs(benchmark::internal::Benchmark* b) {
    for (const int size : kBenchSizes) {
        for (const int free_mult : {3, 4, 6}) {
            b->Args({size, free_mult});
        }
    }
}

// Cross-thread handoff: each registration pins one thread count (producers +
// consumers are template parameters), so only the queue capacity Q is swept as
// an Arg here. One Apply per distinct thread count keeps Args x Threads from
// forming an invalid cartesian product.
void ApplyHandoffQ(benchmark::internal::Benchmark* b, int threads) {
    b->Args({8})->Args({64})->Threads(threads)->UseRealTime();
}

void ApplyHandoffThreads3(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 3); }
void ApplyHandoffThreads4(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 4); }
void ApplyHandoffThreads5(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 5); }
void ApplyHandoffThreads6(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 6); }
void ApplyHandoffThreads8(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 8); }
void ApplyHandoffThreads9(benchmark::internal::Benchmark* b) { ApplyHandoffQ(b, 9); }

// ---------- Registration ----------
BENCHMARK(BM_CentralCache_Fetch_TransferHit)
        ->Apply(ApplyFetchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Fetch_PartialTransferAndSpan)
        ->Apply(ApplyPartialFetchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Fetch_ExistingSpan)
        ->Apply(ApplyFetchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Fetch_PageCacheReuse)
        ->Apply(ApplyFetchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Fetch_PageAllocatorFallback)
        ->Apply(ApplyFetchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Fetch_FragmentedSpans)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);

BENCHMARK(BM_CentralCache_Release_TransferEmpty)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_TransferPartial)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_TransferFullToBitmap)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_DirectBitmapRetainedSpan)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_DirectBitmapUnpinSpan)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_DirectBitmapUnpinMultiSpan)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Release_LongChain)
        ->Apply(ApplyLongChainArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);

BENCHMARK(BM_CentralCache_Drain_Empty)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Drain_Bounded)
        ->Apply(ApplyDrainBoundedArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Drain_MultipleBatches)
        ->Apply(ApplyDrainMultipleBatchArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Drain_UnboundedMultiClass)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_Drain_ColdEndWrap)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);

BENCHMARK(BM_CentralCache_SpanList_DenseSingleSpan)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_SpanList_FillAtExtractionBoundary)
        ->Apply(ApplySizeArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK(BM_CentralCache_SpanList_SparseSingleSpan)
        ->Apply(ApplySpanListSparseArgs)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);

// Contention matrix: <MixedBuckets, WithDrainer, DirectBitmapRelease> x
// threads {2, 4, 8, 16}. Every combination exercises a distinct lock path
// (transfer_cache_lock only, span_list_mutex only, or both plus a drainer).
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, false, false, false)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, true, false, false)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, false, false, true)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, true, false, true)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, false, true, false)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, true, true, false)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, false, true, true)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_Contention, true, true, true)
        ->Apply(ApplyContentionThreads)
        ->Setup(ResetCentralBenchmarkState)
        ->Teardown(TearDownCentralBenchmarkState);

BENCHMARK(BM_CentralCache_CrossThread_SpscHandoff)
        ->Threads(2)
        ->UseRealTime()
        ->Setup(SetupSpscTransfer)
        ->Teardown(TearDownSpscTransfer);

// MPSC: P producers, 1 consumer, Threads(P+1); Q swept as the only Arg.
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpsc, 2)
        ->Apply(ApplyHandoffThreads3)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpsc, 4)
        ->Apply(ApplyHandoffThreads5)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpsc, 8)
        ->Apply(ApplyHandoffThreads9)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);

// SPMC: 1 producer, C consumers, Threads(C+1).
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Spmc, 2)
        ->Apply(ApplyHandoffThreads3)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Spmc, 4)
        ->Apply(ApplyHandoffThreads5)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Spmc, 8)
        ->Apply(ApplyHandoffThreads9)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);

// MPMC: P producers, C consumers, Threads(P+C), swept over all three patterns.
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 2, HandoffPattern::kBalanced)
        ->Apply(ApplyHandoffThreads4)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 4, HandoffPattern::kBalanced)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 2, HandoffPattern::kBalanced)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 4, HandoffPattern::kBalanced)
        ->Apply(ApplyHandoffThreads8)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 2, HandoffPattern::kBursty)
        ->Apply(ApplyHandoffThreads4)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 4, HandoffPattern::kBursty)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 2, HandoffPattern::kBursty)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 4, HandoffPattern::kBursty)
        ->Apply(ApplyHandoffThreads8)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 2, HandoffPattern::kRateImbalance)
        ->Apply(ApplyHandoffThreads4)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 2, 4, HandoffPattern::kRateImbalance)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 2, HandoffPattern::kRateImbalance)
        ->Apply(ApplyHandoffThreads6)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);
BENCHMARK_TEMPLATE(BM_CentralCache_CrossThread_Mpmc, 4, 4, HandoffPattern::kRateImbalance)
        ->Apply(ApplyHandoffThreads8)
        ->Setup(SetupCrossThread)
        ->Teardown(TearDownCentralBenchmarkState);

#ifdef AMMALLOC_TEST
BENCHMARK(BM_CentralCache_Fetch_SpanOnlyFallback)
        ->Apply(ApplyFetchArgs)
        ->Setup(SetupSpanOnlyFetch)
        ->Teardown(TearDownCentralBenchmarkState);
#endif

}// namespace
