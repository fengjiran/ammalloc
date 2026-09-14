/// @file lock_instrumentation.cpp
/// @brief Histogram storage and percentile math for opt-in lock instrumentation.
///
/// The whole file is safe to compile in every build; only the recording
/// functions are gated behind `AMMALLOC_INSTRUMENT_LOCKS`. Snapshot and
/// percentile helpers remain available so benchmark code paths can call them
/// unconditionally and observe zeros in the default build.

#include "ammalloc/lock_instrumentation.h"

#include <cmath>

namespace ammalloc::detail {
namespace {

// One histogram per LockKind; the sentinel `kCount` slot is unused storage
// that keeps indexing branch-free (kind is cast directly to size_t).
alignas(64) LockHistogram g_histograms[static_cast<size_t>(LockKind::kCount)]{};

/// @brief Maps a nanosecond duration to its log2 bucket index.
///
/// Bucket 0 collects `ns == 0`. For `ns >= 1`, bucket `i` covers
/// `[2^(i-1), 2^i)`, computed as `bit_width(ns)`. Values that would exceed
/// the last bucket saturate into it.
inline size_t BucketIndexOf(uint64_t ns) noexcept {
    if (ns == 0) {
        return 0;
    }
    // bit_width(ns) = 64 - count_leading_zeros(ns); for ns >= 1 that maps to
    // the bucket whose upper bound is the smallest power of two > ns.
    const int leading_zeros = __builtin_clzll(ns);
    const size_t idx = static_cast<size_t>(64 - leading_zeros);
    return idx < LockHistogram::kBucketCount ? idx : LockHistogram::kBucketCount - 1;
}

}// namespace

#ifdef AMMALLOC_INSTRUMENT_LOCKS

void RecordLockWait(LockKind kind, uint64_t ns) noexcept {
    auto& hist = g_histograms[static_cast<size_t>(kind)];
    hist.wait_buckets[BucketIndexOf(ns)].fetch_add(1, std::memory_order_relaxed);
}

void RecordLockHold(LockKind kind, uint64_t ns) noexcept {
    auto& hist = g_histograms[static_cast<size_t>(kind)];
    hist.hold_buckets[BucketIndexOf(ns)].fetch_add(1, std::memory_order_relaxed);
}

#endif

LockHistogramSnapshot GetLockHistogramSnapshot(LockKind kind) noexcept {
    LockHistogramSnapshot snap{};
#ifdef AMMALLOC_INSTRUMENT_LOCKS
    const auto& hist = g_histograms[static_cast<size_t>(kind)];
    for (size_t i = 0; i < LockHistogram::kBucketCount; ++i) {
        const size_t w = hist.wait_buckets[i].load(std::memory_order_relaxed);
        const size_t h = hist.hold_buckets[i].load(std::memory_order_relaxed);
        snap.wait_buckets[i] = w;
        snap.hold_buckets[i] = h;
        snap.wait_samples += w;
        snap.hold_samples += h;
    }
#else
    static_cast<void>(kind);
#endif
    return snap;
}

void ResetLockHistograms() noexcept {
#ifdef AMMALLOC_INSTRUMENT_LOCKS
    for (auto& hist : g_histograms) {
        for (size_t i = 0; i < LockHistogram::kBucketCount; ++i) {
            hist.wait_buckets[i].store(0, std::memory_order_relaxed);
            hist.hold_buckets[i].store(0, std::memory_order_relaxed);
        }
    }
#endif
}

uint64_t HistogramPercentileNs(const std::array<size_t, LockHistogram::kBucketCount>& buckets,
                               size_t total, double fraction) noexcept {
    if (total == 0) {
        return 0;
    }
    const double clamped = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    // Rank is 1-based: the smallest rank r such that at least r samples lie
    // at or below the reported bucket. ceil() keeps p99 in the tail bucket
    // when only a handful of samples exist.
    const size_t target_rank = static_cast<size_t>(std::ceil(clamped * static_cast<double>(total)));
    const size_t rank = target_rank == 0 ? 1 : target_rank;

    size_t cumulative = 0;
    for (size_t i = 0; i < LockHistogram::kBucketCount; ++i) {
        cumulative += buckets[i];
        if (cumulative >= rank) {
            // Bucket i covers [2^(i-1), 2^i); report the upper bound 2^i so
            // the returned value is always an over-estimate (safe for SLOs).
            return i == 0 ? 0ULL : (1ULL << i);
        }
    }
    return 1ULL << (LockHistogram::kBucketCount - 1);
}

}// namespace ammalloc::detail
