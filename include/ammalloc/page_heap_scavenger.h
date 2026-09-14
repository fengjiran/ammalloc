#ifndef AMMALLOC_PAGE_HEAP_SCAVENGER_H
#define AMMALLOC_PAGE_HEAP_SCAVENGER_H

/// @file page_heap_scavenger.h
/// @brief Background reclamation of physical pages from idle PageCache spans.
/// @see docs/designs/06-page-heap-scavenger.md, docs/decisions/0001-scavenger-startup-strategy.md

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <stop_token>
#include <thread>

namespace ammalloc {

/// @brief Best-effort scavenger telemetry stored in relaxed atomics.
///
/// `scavenged_bytes` measures pages that successfully returned to the OS via
/// `MADV_DONTNEED`; it is the only counter in the ammalloc stack that
/// reflects actual RSS reduction, one layer below the Span handoff counters
/// in `CentralCacheStats`.
struct ScavengerStats {
    /// Number of completed `ScavengeOnePass()` invocations.
    std::atomic<size_t> scavenge_passes{0};
    /// Spans detached from PageCache free lists for reclamation.
    std::atomic<size_t> scavenged_spans{0};
    /// Bytes successfully released via `MADV_DONTNEED`.
    std::atomic<size_t> scavenged_bytes{0};
    /// Successful `madvise` calls (one per reclaimed Span).
    std::atomic<size_t> madvise_success_count{0};
    /// Failed `madvise` calls; the Span stays committed and retries next pass.
    std::atomic<size_t> madvise_failed_count{0};
};

/// @brief Runs periodic `MADV_DONTNEED` reclamation for idle free spans.
///
/// The singleton owns one `std::jthread`. Each pass temporarily removes an idle
/// span under its PageCache shard lock, performs `madvise` without that lock,
/// and then returns the span to its owner shard.
class PageHeapScavenger {
public:
    /// @brief Returns the process-wide scavenger instance.
    /// @return Reference to the singleton stored without allocator recursion.
    static PageHeapScavenger& GetInstance() {
        alignas(alignof(PageHeapScavenger)) static char storage[sizeof(PageHeapScavenger)];
        static auto* instance = new (storage) PageHeapScavenger();
        return *instance;
    }

    PageHeapScavenger(const PageHeapScavenger&) = delete;
    PageHeapScavenger& operator=(const PageHeapScavenger&) = delete;

    /// @brief Starts the background thread if it is not already running.
    /// @throws std::system_error if the operating system cannot create the thread.
    /// @note Calls to `Start` and `Stop` must be externally serialized.
    void Start();

    /// @brief Requests shutdown and joins the background thread.
    /// @note Calls to `Start` and `Stop` must be externally serialized.
    void Stop();

    /// @brief Returns the live scavenger telemetry counters.
    /// @return Read-only reference to process-wide atomic counters.
    static const ScavengerStats& GetStats() noexcept { return stats_; }

    /// @brief Zeros every telemetry counter.
    /// @note Intended for benchmark and test isolation; safe to call while the
    ///       scavenger thread is running because each counter uses relaxed
    ///       `store(0)` and observation is best-effort.
    static void ResetStats() noexcept {
        stats_.scavenge_passes.store(0, std::memory_order_relaxed);
        stats_.scavenged_spans.store(0, std::memory_order_relaxed);
        stats_.scavenged_bytes.store(0, std::memory_order_relaxed);
        stats_.madvise_success_count.store(0, std::memory_order_relaxed);
        stats_.madvise_failed_count.store(0, std::memory_order_relaxed);
    }

private:
    PageHeapScavenger() = default;

    void ScavengeLoop(std::stop_token stoken);
    static void ScavengeOnePass();

    std::jthread scavenge_thread_;
    std::condition_variable_any cv_;
    std::mutex mutex_;

    inline static ScavengerStats stats_{};

    /// Delay between scavenging passes.
    static constexpr uint64_t kScavengeIntervalMs = 1000;
    /// Minimum idle time before a committed free span is reclaimed.
    static constexpr uint64_t kIdleThresholdMs = 10000;
};

}// namespace ammalloc

#endif// AMMALLOC_PAGE_HEAP_SCAVENGER_H
