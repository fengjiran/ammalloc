#ifndef AMMALLOC_CENTRAL_CACHE_H
#define AMMALLOC_CENTRAL_CACHE_H

/// @file
/// @brief Shared object cache between per-thread caches and PageCache.
/// @see docs/designs/03-central-cache.md

#include "ammalloc/free_list.h"
#include "ammalloc/noalloc_diagnostics.h"
#include "ammalloc/size_class.h"
#include "ammalloc/span.h"
#include "ammalloc/spin_lock.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ammalloc {

/// @brief Chooses whether a returned object chain may enter TransferCache.
///
/// Direct bitmap release is used by owner-thread RSS trims and by bounded
/// TransferCache drains; ordinary overflow continues to prefer reuse.
enum class CentralReleaseMode : uint8_t {
    kTransferCache,
    kSpanBitmap,
};

/// @brief Slow-path-only CentralCache retention telemetry.
struct CentralCacheStats {
    std::atomic<size_t> transfer_cache_drained_bytes{0};
    std::atomic<size_t> spans_unpinned_by_direct_release{0};
};

#ifdef AMMALLOC_TEST
// Test-only hook: when set to 0 < cap < batch_num, FetchRange returns at most
// `cap` objects, forcing a partial refill at the ThreadCache layer.
extern std::atomic<size_t> g_mock_fetch_range_cap;
#else
#define g_mock_fetch_range_cap (0)
#endif

/// @brief Balances small objects between ThreadCache and PageCache.
///
/// Each size class has a two-tier bucket. A `SpinLock` protects the O(1)
/// TransferCache pointer array, while a separate `std::mutex` protects SpanList
/// traversal and bitmap operations. Calls into PageCache occur without holding
/// the bucket mutex to preserve the allocator lock order.
class CentralCache {
    /// @brief Stores the shared state for one size class.
    /// @note Cache-line alignment isolates locks used by adjacent buckets.
    struct alignas(SystemConfig::CACHE_LINE_SIZE) Bucket {
        /// Spin lock protecting `transfer_cache` and its count.
        SpinLock transfer_cache_lock;
        /// Number of valid pointers currently stored in `transfer_cache`.
        size_t transfer_cache_count{0};
        /// Physical slot of the cold end in the circular pointer array.
        /// Together with count this preserves LIFO reuse without moving a hot
        /// suffix while a drain holds the SpinLock.
        size_t transfer_cache_begin{0};
        /// Fixed capacity assigned during CentralCache initialization.
        size_t transfer_cache_capacity{0};
        /// Borrowed slice of the contiguous TransferCache backing allocation.
        void** transfer_cache{nullptr};

        // Starts on a fresh cache line: the TransferCache lock and this mutex
        // are held independently by different threads, and sharing a line would
        // ping-pong it between cores on hot size classes.
        /// Mutex protecting `span_list` and Span bitmap operations.
        alignas(SystemConfig::CACHE_LINE_SIZE) std::mutex span_list_lock;

        // The 64-aligned inline sentinel then starts on its own line.
        /// Allocation-free intrusive list of borrowed Span metadata.
        SpanList span_list;
    };

    // False-sharing guard: the two bucket locks are acquired independently by
    // different threads (AGENTS.md §4.2); the member alignment above must keep
    // them apart.
    static_assert(offsetof(Bucket, span_list_lock) >= SystemConfig::CACHE_LINE_SIZE,
                  "span_list_lock must not share a cache line with transfer_cache_lock");
    // `std::array` lays buckets back to back; a line-multiple size keeps each
    // bucket's cache lines disjoint from its neighbors'.
    static_assert(sizeof(Bucket) % SystemConfig::CACHE_LINE_SIZE == 0,
                  "Bucket size must be a multiple of the cache line size");

public:
    /// @brief Returns the process-wide CentralCache.
    /// @return Reference to the singleton instance.
    static CentralCache& GetInstance() {
        static CentralCache instance;
        return instance;
    }

    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    /// @brief Fetches a batch of objects to refill a ThreadCache.
    /// @param block_list Destination list that receives fetched objects.
    /// @param batch_num Maximum number of objects to fetch.
    /// @param aligned_size Size-class-aligned object size used to select the bucket.
    /// @return Number of fetched objects, which may be smaller on allocation failure.
    /// @pre `batch_num <= SizeClass::kMaxBatchSize`.
    /// @pre `aligned_size` is an exact size-class boundary
    ///      (`SizeClass::Size(SizeClass::Index(aligned_size)) == aligned_size`);
    ///      Span::Init aborts otherwise in every build.
    size_t FetchRange(FreeList& block_list, size_t batch_num, size_t aligned_size) noexcept;

    /// @brief Returns an intrusive object chain to the matching shared bucket.
    /// @param start Head of a non-empty chain of objects from one size class.
    /// @param aligned_size Size-class-aligned object size shared by every object.
    /// @pre Each object belongs to an ammalloc Span for `aligned_size`.
    void ReleaseListToSpans(void* start, size_t aligned_size,
                            CentralReleaseMode mode = CentralReleaseMode::kTransferCache) noexcept;

    /// @brief Returns bounded TransferCache contents directly to Span bitmaps.
    /// @param max_bytes Maximum logical object bytes to drain; SIZE_MAX removes
    ///        the full per-bucket snapshot observed by this call.
    /// @return Logical object bytes detached from TransferCache.
    ///
    /// Each transfer lock is held only while moving one fixed stack batch. The
    /// lock is released before PageMap, Span bitmap, or PageCache operations.
    size_t DrainTransferCaches(size_t max_bytes) noexcept;

    /// @brief Returns slow-path retention telemetry.
    AM_NODISCARD static const CentralCacheStats& GetStats() noexcept;

    /// @brief Returns one bucket's TransferCache occupancy for tests.
    AM_NODISCARD size_t GetTransferCacheCountForTest(size_t idx) noexcept;

    /// @brief Releases cached objects and spans, then rebuilds the TransferCache
    ///        backing so the singleton keeps its O(1) fast path afterwards.
    /// @note Intended for tests or controlled teardown after concurrent use stops
    ///       and every ThreadCache has drained via `ReleaseAll`.
    void Reset() noexcept;

private:
    CentralCache() noexcept {
        // A failed backing allocation leaves all capacities at zero. Fetch and
        // release then use the SpanList path without retrying on every request.
        static_cast<void>(TryInitTransferCache());
    }

    /// @brief (Re)allocates TransferCache backing without aborting.
    /// @return True when the backing is ready; on failure every bucket slice
    ///         stays zeroed and callers degrade to the SpanList slow path.
    /// @note Used at construction and Reset; failure is a permanent SpanList
    ///       fallback until a controlled Reset retries initialization.
    bool TryInitTransferCache() noexcept;

    /// @brief Returns the total pointer capacity of all TransferCache buckets.
    /// @note Single source of truth for the contiguous backing size.
    static size_t CalculateTotalTransferPtrs() noexcept;

    /// @brief Maps a logical cold-to-hot offset to a circular backing slot.
    /// @pre `offset < bucket.transfer_cache_capacity`.
    AM_NODISCARD static size_t TransferIndex(const Bucket& bucket,
                                             size_t offset) noexcept {
        AM_DCHECK(offset < bucket.transfer_cache_capacity);
        const size_t index = bucket.transfer_cache_begin + offset;
        return index < bucket.transfer_cache_capacity
                       ? index
                       : index - bucket.transfer_cache_capacity;
    }

    /// @brief Obtains and initializes a Span for one bucket.
    /// @param bucket Bucket that receives the Span.
    /// @param aligned_size Object size used to initialize Span metadata.
    /// @param lock Held lock for `bucket.span_list_lock`; temporarily released
    ///        while entering PageCache, reacquired before returning on both
    ///        success and failure.
    /// @return Borrowed Span owned by PageCache, or null on allocation failure.
    /// @pre `lock` owns `bucket.span_list_lock`.
    static Span* GetOneSpan(Bucket& bucket, size_t aligned_size,
                            detail::NoThrowUniqueLock& lock) noexcept;

    /// TransferCache capacity per bucket, expressed as a multiple of the
    /// class batch size.
    constexpr static size_t kCapScale = 8;
    /// One independently synchronized bucket per size class.
    std::array<Bucket, SizeClass::kNumSizeClasses> buckets_{};
    inline static CentralCacheStats stats_{};
};


}// namespace ammalloc

#endif// AMMALLOC_CENTRAL_CACHE_H
