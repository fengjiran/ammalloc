#ifndef AMMALLOC_THREAD_CACHE_H
#define AMMALLOC_THREAD_CACHE_H

/// @file
/// @brief Thread-confined front-end cache for small-object allocation.
///
/// The hot path is lock-free because each ThreadCache and its FreeLists are
/// owned by one thread. Batched slow paths exchange objects with CentralCache,
/// while quota growth and decay limit persistent per-thread memory retention.
/// @see docs/designs/02-thread-cache.md

#include "ammalloc/assert.h"
#include "ammalloc/free_list.h"
#include "ammalloc/size_class.h"

namespace ammalloc {

/// @brief Pure quota-adjustment rules for ThreadCache size-class caches.
/// State (max_size/overages) lives in FreeList; these helpers compute the next
/// values only, so the policy can be unit-tested without CentralCache I/O.
namespace quota_policy {

/// @brief Consecutive overflow trims that trigger quota decay.
inline constexpr size_t kMaxOverages = 3;

/// @brief Next quota after a successful central refill.
/// Two-stage growth: exponential warmup below one batch, then linear growth up
/// to 8 batches. Returns the unchanged quota at the ceiling.
AM_NODISCARD size_t NextAfterRefill(size_t current, size_t batch) noexcept;

/// @brief Quota and overage counter after one overflow trim.
struct QuotaState {
    size_t max_size;
    size_t overages;
};

/// @brief Next quota state after a slow-path overflow release.
/// Repeated overflow without intervening refill decays the quota by one batch
/// (floor: one batch) and resets the counter; otherwise the counter advances.
/// At the floor, no decay state is retained.
AM_NODISCARD QuotaState NextAfterOverflow(size_t current, size_t batch,
                                          size_t overages) noexcept;

}// namespace quota_policy

/// @brief Caches thread-cacheable objects for one owning thread.
///
/// ThreadCache provides the allocator's lowest-latency path: a TLS-owned array
/// of LIFO FreeLists indexed by size class. The common case never takes a lock
/// and never touches global metadata.
///
/// Key design properties:
/// - Fast path stays entirely thread-local.
/// - Slow-start grows each FreeList only after repeated refill pressure.
/// - Overflow deallocation trims one batch back to CentralCache.
/// - Repeated overflow events decay `max_size` to avoid sticky high-water marks
///   after transient bursts.
///
/// Lifetime model:
/// - Objects cached here remain owned by the allocator system.
/// - `ReleaseAll()` drains all thread-local state back to CentralCache.
class alignas(SystemConfig::CACHE_LINE_SIZE) ThreadCache {
public:
    ThreadCache() noexcept = default;
    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    /// @brief Allocates one object from the FreeList for `original_size`.
    ///
    /// @param original_size Raw user request size, not yet rounded to a class
    ///        boundary. Zero is served from the minimum size class.
    /// @return Pointer to an object slot, or nullptr if the slow path cannot
    ///         refill from CentralCache.
    ///
    /// @pre `original_size <= SizeConfig::MAX_TC_SIZE`
    /// @note The fast path is a single FreeList pop with no locking.
    AM_NODISCARD AM_ALWAYS_INLINE void* Allocate(size_t original_size) noexcept {
        AM_DCHECK(original_size <= SizeConfig::MAX_TC_SIZE);
        // One Index call serves both paths; RoundUp(size) == Size(Index(size))
        // by construction, so the class size is derived only when needed.
        size_t idx = SizeClass::Index(original_size);
        auto& list = free_lists_[idx];

        // Hot path: satisfy the request entirely from TLS state.
        // clang-format off
        if (!list.empty()) AM_LIKELY {
            return list.pop();
        }
        // clang-format on

        // Refill from CentralCache only after local capacity is exhausted.
        return FetchFromCentralCache(list, SizeClass::Size(idx));
    }

    /// @brief Returns one object to its size-class FreeList.
    ///
    /// @param ptr Object pointer being freed.
    /// @param idx Size-class index recorded in the owning Span
    ///        (`span->size_class_idx`), avoiding a re-mapping of the object size.
    ///
    /// @pre `ptr != nullptr`
    /// @pre `idx < SizeClass::kNumSizeClasses`
    /// @note The fast path is a single FreeList push. Slow path is entered only
    ///       when local occupancy reaches the current per-class limit.
    AM_ALWAYS_INLINE void Deallocate(void* ptr, size_t idx) {
        AM_DCHECK(ptr != nullptr);
        AM_DCHECK(idx < SizeClass::kNumSizeClasses);

        auto& list = free_lists_[idx];

        // Hot path: keep recently freed objects local to preserve locality.
        list.push(ptr);

        // Crossing the local quota triggers batched trim back to CentralCache.
        // The class size is needed only on this slow path.
        // clang-format off
        if (list.size() > list.max_size()) AM_UNLIKELY {
            DeallocateSlowPath(list, SizeClass::Size(idx));
        }
        // clang-format on
    }

    /// @brief Drains every size-class FreeList back to CentralCache.
    ///
    /// Used during TLS teardown and tests to avoid keeping thread-local state
    /// alive longer than the owning thread.
    void ReleaseAll();

    /// @brief Returns a FreeList high-water limit for testing.
    /// @param idx Size-class index.
    /// @return Current maximum local object count.
    /// @pre `idx < SizeClass::kNumSizeClasses`.
    AM_NODISCARD size_t GetMaxSizeForTest(size_t idx) const noexcept {
        AM_DCHECK(idx < free_lists_.size());
        return free_lists_[idx].max_size();
    }

    /// @brief Returns a FreeList overage counter for testing.
    /// @param idx Size-class index.
    /// @return Consecutive overflow-trim count.
    /// @pre `idx < SizeClass::kNumSizeClasses`.
    AM_NODISCARD size_t GetOveragesForTest(size_t idx) const noexcept {
        AM_DCHECK(idx < free_lists_.size());
        return free_lists_[idx].overages();
    }

private:
    // One TLS-owned LIFO cache per size class. No synchronization is required
    // because the owning thread is the only mutator.
    std::array<FreeList, SizeClass::kNumSizeClasses> free_lists_{};

    /// @brief Refills an empty FreeList from CentralCache and updates its quota.
    ///
    /// The quota follows a two-stage policy:
    /// - exponential warmup until one batch,
    /// - linear growth up to a bounded multiple of the batch size.
    AM_NOINLINE static void* FetchFromCentralCache(FreeList& list, size_t aligned_size) noexcept;

    /// @brief Trims one batch to CentralCache and applies quota decay.
    ///
    /// Repeated overflow trims without intervening refill demand reduce
    /// `max_size`, preventing long-lived threads from pinning burst-era quotas.
    AM_NOINLINE static void DeallocateSlowPath(FreeList& list, size_t aligned_size) noexcept;
};
}// namespace ammalloc

#endif// AMMALLOC_THREAD_CACHE_H
