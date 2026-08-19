#ifndef AMMALLOC_CENTRAL_CACHE_H
#define AMMALLOC_CENTRAL_CACHE_H

/// @file
/// @brief Shared object cache between per-thread caches and PageCache.
/// @see docs/designs/03-central-cache.md

#include "ammalloc/free_list.h"
#include "ammalloc/size_class.h"
#include "ammalloc/span.h"
#include "ammalloc/spin_lock.h"

#include <mutex>

namespace ammalloc {

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
        /// Fixed capacity assigned during CentralCache initialization.
        size_t transfer_cache_capacity{0};
        /// Borrowed slice of the contiguous TransferCache backing allocation.
        void** transfer_cache{nullptr};

        /// Mutex protecting `span_list` and Span bitmap operations.
        std::mutex span_list_lock;
        /// Allocation-free intrusive list of borrowed Span metadata.
        SpanList span_list;
    };

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
    /// @pre `aligned_size` identifies a valid ThreadCache size class.
    size_t FetchRange(FreeList& block_list, size_t batch_num, size_t aligned_size);

    /// @brief Returns an intrusive object chain to the matching shared bucket.
    /// @param start Head of a non-empty chain of objects from one size class.
    /// @param aligned_size Size-class-aligned object size shared by every object.
    /// @pre Each object belongs to an ammalloc Span for `aligned_size`.
    void ReleaseListToSpans(void* start, size_t aligned_size);

    /// @brief Releases cached objects, spans, and TransferCache backing storage.
    /// @note Intended for tests or controlled teardown after concurrent use stops.
    void Reset() noexcept;

private:
    CentralCache() {
        InitTransferCache();
    }

    /// @brief Allocates and partitions contiguous backing storage for all TransferCaches.
    /// @note Uses PageAllocator directly to prevent recursive `am_malloc` entry.
    void InitTransferCache();

    /// @brief Obtains and initializes a Span for one bucket.
    /// @param bucket Bucket that receives the Span.
    /// @param aligned_size Object size used to initialize Span metadata.
    /// @param lock Held lock for `bucket.span_list_lock`; temporarily released
    ///        while entering PageCache and reacquired before insertion.
    /// @return Borrowed Span owned by PageCache, or null on allocation failure.
    /// @pre `lock` owns `bucket.span_list_lock`.
    static Span* GetOneSpan(Bucket& bucket, size_t aligned_size, std::unique_lock<std::mutex>& lock);

    constexpr static size_t kNumSizeClasses = SizeClass::Index(SizeConfig::MAX_TC_SIZE) + 1;
    constexpr static size_t kCapScale = 8;
    /// One independently synchronized bucket per size class.
    std::array<Bucket, kNumSizeClasses> buckets_{};
};


}// namespace ammalloc

#endif// AMMALLOC_CENTRAL_CACHE_H
