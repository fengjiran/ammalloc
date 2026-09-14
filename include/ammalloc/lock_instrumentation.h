#ifndef AMMALLOC_LOCK_INSTRUMENTATION_H
#define AMMALLOC_LOCK_INSTRUMENTATION_H

/// @file lock_instrumentation.h
/// @brief Opt-in wait/hold histogram for allocator lock sites.
///
/// All instrumentation is gated behind `AMMALLOC_INSTRUMENT_LOCKS`. When the
/// macro is undefined (default), the header exposes only inert typedefs and
/// empty inline functions, so production builds pay zero cost. The
/// instrumented build is intended for offline contention analysis via
/// `cmake -DAMMALLOC_BENCH_INSTRUMENT_LOCKS=ON` and never for absolute
/// throughput comparison against the default build.

#include "ammalloc/attributes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ammalloc::detail {

/// @brief Identifies a lock site whose wait/hold distribution is tracked.
enum class LockKind : uint8_t {
    /// CentralCache::Bucket::transfer_cache_lock (SpinLock).
    kTransferSpin,
    /// CentralCache::Bucket::span_list_lock (std::mutex or wrapper).
    kSpanListMutex,
    /// Sentinel; must remain last.
    kCount
};

/// @brief Log2-bucketed sample counts for one LockKind.
///
/// Bucket `i` covers `[2^(i-1), 2^i)` nanoseconds for `i >= 1`; bucket 0
/// collects zero-duration samples. 32 buckets span 0 to ~2 seconds, plenty
/// for allocator lock timings.
struct LockHistogram {
    static constexpr size_t kBucketCount = 32;
    std::array<std::atomic<size_t>, kBucketCount> wait_buckets{};
    std::array<std::atomic<size_t>, kBucketCount> hold_buckets{};
};

/// @brief Immutable copy of one LockKind's histogram for reporting.
struct LockHistogramSnapshot {
    std::array<size_t, LockHistogram::kBucketCount> wait_buckets{};
    std::array<size_t, LockHistogram::kBucketCount> hold_buckets{};
    size_t wait_samples{0};
    size_t hold_samples{0};
};

#ifdef AMMALLOC_INSTRUMENT_LOCKS

/// @brief Returns monotonic nanoseconds for lock timing.
/// @note Uses steady_clock; the vDSO clock_gettime on Linux keeps this at
///       ~20-30 ns per call, which is the accepted cost of instrumentation.
AM_ALWAYS_INLINE uint64_t ClockNs() noexcept {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

/// @brief Records one wait-duration sample (time from lock() entry to acquire).
void RecordLockWait(LockKind kind, uint64_t ns) noexcept;

/// @brief Records one hold-duration sample (time from acquire to unlock()).
void RecordLockHold(LockKind kind, uint64_t ns) noexcept;

#else

AM_ALWAYS_INLINE constexpr uint64_t ClockNs() noexcept { return 0; }
AM_ALWAYS_INLINE void RecordLockWait(LockKind, uint64_t) noexcept {}
AM_ALWAYS_INLINE void RecordLockHold(LockKind, uint64_t) noexcept {}

#endif

/// @brief Returns a snapshot of `kind`'s histogram; zeros when instrumentation is off.
LockHistogramSnapshot GetLockHistogramSnapshot(LockKind kind) noexcept;

/// @brief Zeros every histogram; used between benchmark scenarios.
void ResetLockHistograms() noexcept;

/// @brief Returns the upper-bound nanosecond value of the bucket containing
///        the given percentile fraction of `total` samples.
/// @param buckets Log2-bucketed sample counts (index i covers `[2^(i-1), 2^i)`).
/// @param total Sum of `buckets`; zero yields zero.
/// @param fraction Percentile in `[0.0, 1.0]`; values >= 1.0 return the highest
///        non-empty bucket's upper bound.
/// @return Approximate percentile in nanoseconds (bucket upper bound).
uint64_t HistogramPercentileNs(const std::array<size_t, LockHistogram::kBucketCount>& buckets,
                               size_t total, double fraction) noexcept;

#ifdef AMMALLOC_INSTRUMENT_LOCKS

/// @brief std::mutex wrapper that records wait and hold durations.
///
/// API-compatible with std::mutex so `NoThrowUniqueLock<InstrumentedMutex>`
/// and standard RAII adapters work unchanged. Only used for the CentralCache
/// span-list mutex; the SpinLock is instrumented in-place.
class InstrumentedMutex {
public:
    InstrumentedMutex() = default;
    InstrumentedMutex(const InstrumentedMutex&) = delete;
    InstrumentedMutex& operator=(const InstrumentedMutex&) = delete;

    void lock() {
        const uint64_t t0 = ClockNs();
        inner_.lock();
        const uint64_t t1 = ClockNs();
        RecordLockWait(LockKind::kSpanListMutex, t1 - t0);
        hold_start_ns_ = t1;
    }

    bool try_lock() {
        // try_lock has no wait phase by definition; only record the acquire
        // timestamp so a subsequent unlock() reports a valid hold duration.
        const bool acquired = inner_.try_lock();
        if (acquired) {
            hold_start_ns_ = ClockNs();
        }
        return acquired;
    }

    void unlock() {
        RecordLockHold(LockKind::kSpanListMutex, ClockNs() - hold_start_ns_);
        inner_.unlock();
    }

private:
    std::mutex inner_;
    uint64_t hold_start_ns_{0};
};

using MaybeInstrumentedMutex = InstrumentedMutex;

#else

using MaybeInstrumentedMutex = std::mutex;

#endif

}// namespace ammalloc::detail

#endif// AMMALLOC_LOCK_INSTRUMENTATION_H
