// Copyright 2026 The AetherMind Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef AMMALLOC_THREAD_CACHE_H
#define AMMALLOC_THREAD_CACHE_H

/// @file
/// @brief Thread-confined front-end cache for small-object allocation.
///
/// The hot path is lock-free because each ThreadCache and its FreeLists are
/// owned by one thread. Batched slow paths exchange objects with CentralCache,
/// while quota growth and decay limit persistent per-thread memory retention.

#include "ammalloc/assert.h"
#include "ammalloc/central_cache.h"

namespace ammalloc {

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

    /// @brief Allocates one object from the FreeList for `aligned_size`.
    ///
    /// @param aligned_size Size-class-aligned object size. Callers must pass the
    ///        internal aligned size, not the original user request.
    /// @return Pointer to an object slot, or nullptr if the slow path cannot
    ///         refill from CentralCache.
    ///
    /// @pre `aligned_size > 0`.
    /// @pre `aligned_size <= SizeConfig::MAX_TC_SIZE`
    /// @pre `aligned_size == SizeClass::RoundUp(aligned_size)`.
    /// @note The fast path is a single FreeList pop with no locking.
    AM_NODISCARD AM_ALWAYS_INLINE void* Allocate(size_t aligned_size) noexcept {
        AMMALLOC_DCHECK(aligned_size > 0);
        AMMALLOC_DCHECK(aligned_size <= SizeConfig::MAX_TC_SIZE);
        AMMALLOC_DCHECK(aligned_size == SizeClass::RoundUp(aligned_size));
        size_t idx = SizeClass::Index(aligned_size);
        auto& list = free_lists_[idx];

        // Hot path: satisfy the request entirely from TLS state.
        // clang-format off
        if (!list.empty()) AM_LIKELY {
            return list.pop();
        }
        // clang-format on

        // Refill from CentralCache only after local capacity is exhausted.
        return FetchFromCentralCache(list, aligned_size);
    }

    /// @brief Returns one object to its size-class FreeList.
    ///
    /// @param ptr Object pointer being freed.
    /// @param aligned_size Span-recorded aligned object size.
    ///
    /// @pre `ptr != nullptr`
    /// @pre `aligned_size > 0`.
    /// @pre `aligned_size <= SizeConfig::MAX_TC_SIZE`
    /// @note The fast path is a single FreeList push. Slow path is entered only
    ///       when local occupancy reaches the current per-class limit.
    void AM_ALWAYS_INLINE Deallocate(void* ptr, size_t aligned_size) {
        AMMALLOC_DCHECK(ptr != nullptr);
        AMMALLOC_DCHECK(aligned_size <= SizeConfig::MAX_TC_SIZE);

        size_t idx = SizeClass::Index(aligned_size);
        auto& list = free_lists_[idx];

        // Hot path: keep recently freed objects local to preserve locality.
        list.push(ptr);

        // Crossing the local quota triggers batched trim back to CentralCache.
        // clang-format off
        if (list.size() > list.max_size()) AM_UNLIKELY {
            DeallocateSlowPath(list, aligned_size);
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
        AMMALLOC_DCHECK(idx < free_lists_.size());
        return free_lists_[idx].max_size();
    }

    /// @brief Returns a FreeList overage counter for testing.
    /// @param idx Size-class index.
    /// @return Consecutive overflow-trim count.
    /// @pre `idx < SizeClass::kNumSizeClasses`.
    AM_NODISCARD size_t GetOveragesForTest(size_t idx) const noexcept {
        AMMALLOC_DCHECK(idx < free_lists_.size());
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
