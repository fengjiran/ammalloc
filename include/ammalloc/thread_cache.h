#ifndef AMMALLOC_THREAD_CACHE_H
#define AMMALLOC_THREAD_CACHE_H

/// @file thread_cache.h
/// @brief Thread-confined front-end cache for small-object allocation.
///
/// The hot path is lock-free because each ThreadCache and its FreeLists are
/// owned by one thread. Batched slow paths exchange objects with CentralCache,
/// while quota growth and decay limit persistent per-thread memory retention.
/// @see docs/designs/02-thread-cache.md

#include "ammalloc/assert.h"
#include "ammalloc/free_list.h"
#include "ammalloc/size_class.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ammalloc {

/// @brief Pure quota-adjustment rules for ThreadCache size-class caches.
/// State (max_size/overages) lives in FreeList; these helpers compute the next
/// values only, so the policy can be unit-tested without CentralCache I/O.
namespace quota_policy {

/// @brief Consecutive overflow trims that trigger quota decay.
inline constexpr size_t kMaxOverages = 3;

/// @brief Quota ceiling expressed in batch multiples.
///
/// Caps `max_size` growth at `kMaxQuotaBatches * batch` after refill and
/// derives the linear growth step (`batch / kMaxQuotaBatches`) above one batch.
inline constexpr size_t kMaxQuotaBatches = 8;

/// @brief Quota and overage counter after one overflow trim.
struct QuotaState {
    size_t max_size;
    size_t overages;
};

/// @brief Next quota after a successful central refill.
/// Two-stage growth: exponential warmup below one batch, then linear growth up
/// to kMaxQuotaBatches batches. Returns the unchanged quota at the ceiling.
AM_NODISCARD size_t NextAfterRefill(size_t cur_max_size, size_t batch) noexcept;

/// @brief Next quota state after a slow-path overflow release.
/// Repeated overflow without intervening refill decays the quota by one batch
/// (floor: one batch) and resets the counter; otherwise the counter advances.
/// At the floor, no decay state is retained.
AM_NODISCARD QuotaState NextAfterOverflow(size_t current, size_t batch,
                                          size_t overages) noexcept;

}// namespace quota_policy

/// @brief Selects whether a ThreadCache trim preserves shared reuse or returns
///        objects directly to their Span bitmaps.
///
/// `kReuse` retains a small warm working set and returns evicted objects through
/// TransferCache. `kRelease` bypasses TransferCache so an empty Span can return
/// to PageCache and eventually become reclaimable by the scavenger.
enum class ThreadCacheTrimMode : uint8_t {
    kReuse,
    kRelease
};

/// @brief Slow-path-only telemetry for ThreadCache retention control.
///
/// These counters deliberately do not track every push/pop: exact cached bytes
/// are computed only while trimming, so ThreadCache hit paths remain local and
/// branch-free. `cached_bytes_observed_at_trim` is an accumulated observation,
/// not a current-process RSS measurement.
struct ThreadCacheStats {
    // Aggregate `Σ(max_size * class_size)` across live ThreadCache instances.
    // It is quota capacity, not exact cached object bytes or RSS.
    std::atomic<size_t> reserved_quota_bytes{0};
    std::atomic<size_t> trim_count{0};
    std::atomic<size_t> cached_bytes_observed_at_trim{0};
    std::atomic<size_t> trimmed_bytes{0};
    std::atomic<size_t> budget_denied_growth{0};
};

/// @brief Returns process-wide slow-path ThreadCache retention telemetry.
AM_NODISCARD const ThreadCacheStats& GetThreadCacheStats() noexcept;

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
/// - A per-thread aggregate quota budget prevents independent classes from
///   each growing to their own maximum at once.
///
/// Lifetime model:
/// - Objects cached here remain owned by the allocator system.
/// - `ReleaseAll()` drains all thread-local state back to CentralCache.
class alignas(SystemConfig::CACHE_LINE_SIZE) ThreadCache {
public:
    /// @brief Default aggregate quota capacity for one ThreadCache.
    ///
    /// This caps capacity reservations, not exact cached bytes. It is updated
    /// only when slow paths change a class quota, so the normal push/pop path
    /// does not perform aggregate accounting.
    static constexpr size_t kDefaultCacheBudgetBytes = 2 * 1024 * 1024;
    /// @brief Target used by owner-thread soft trims and cooperative requests.
    static constexpr size_t kDefaultTrimTargetBytes = kDefaultCacheBudgetBytes / 2;

    /// @brief Constructs with the default aggregate quota budget.
    ThreadCache() noexcept;
    /// @brief Builds a cache with a caller-selected aggregate quota budget.
    /// @note Mainly useful for controlled tests and embedded callers. Values
    ///       below the one-object-per-class floor are raised to that floor.
    explicit ThreadCache(size_t cache_budget_bytes) noexcept;
    /// @brief Destroys an empty manually managed cache.
    /// @pre Every FreeList is empty; call `ReleaseAll()` before destruction.
    ~ThreadCache() noexcept;
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
        const auto idx = SizeClass::Index(original_size);

        // Hot path: satisfy the request entirely from TLS state.
        // clang-format off
        if (auto& list = free_lists_[idx]; !list.empty()) AM_LIKELY {
            return list.pop();
        }
        // clang-format on

        // Refill from CentralCache only after local capacity is exhausted.
        return FetchFromCentralCache(idx);
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
    AM_ALWAYS_INLINE void Deallocate(void* ptr, size_t idx) noexcept {
        AM_DCHECK(ptr != nullptr);
        AM_HCHECK(idx < SizeClass::kNumSizeClasses, "class index out of range");

        auto& list = free_lists_[idx];

        // Hot path: keep recently freed objects local to preserve locality.
        list.push(ptr);

        // Crossing the local quota triggers batched trim back to CentralCache.
        // The class size is needed only on this slow path.
        // clang-format off
        if (list.size() > list.max_size()) AM_UNLIKELY {
            DeallocateSlowPath(idx);
        }
        // clang-format on
    }

    /// @brief Drains every size-class FreeList back to CentralCache.
    ///
    /// Used during TLS teardown and tests to avoid keeping thread-local state
    /// alive longer than the owning thread.
    void ReleaseAll() noexcept;

    /// @brief Trims this owner thread's cached objects toward `target_bytes`.
    ///
    /// The fixed-size class scan runs from large classes to small classes. In
    /// `kReuse` mode it retains up to one current batch per class, so the target
    /// is best-effort; `kRelease` may evict every object and can therefore meet
    /// a zero-byte target. This function is owner-thread-only and must never be
    /// called by a background thread for another ThreadCache.
    void Trim(ThreadCacheTrimMode mode, size_t target_bytes) noexcept;

    /// @brief Publishes a cooperative trim request for all ThreadCache owners.
    ///
    /// Owners observe the generation only at allocation/deallocation slow
    /// paths or an explicit owner-thread safepoint. Publishing never derefers
    /// another thread's TLS cache.
    static void RequestGlobalTrim(ThreadCacheTrimMode mode) noexcept;

    /// @brief Applies the newest cooperative request at an owner-thread
    /// safepoint, if one exists.
    void ObserveGlobalTrimRequest() noexcept;

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

    /// @brief Returns this cache's aggregate quota-capacity reservation.
    /// @note It is intentionally not the current cached-object byte count.
    AM_NODISCARD size_t GetReservedQuotaBytesForTest() const noexcept {
        return reserved_quota_bytes_;
    }

    /// @brief Returns the configured aggregate quota capacity.
    AM_NODISCARD size_t GetCacheBudgetBytesForTest() const noexcept {
        return cache_budget_bytes_;
    }

    /// @brief Computes current cached-object bytes with a fixed class scan.
    /// @note Owner-thread-only diagnostic. Production trim paths call it to
    ///       measure retention; it is not a hot-path metric.
    AM_NODISCARD size_t CachedBytesSnapshot() const noexcept;

private:
    // One TLS-owned LIFO cache per size class. No synchronization is required
    // because the owning thread is the only mutator.
    std::array<FreeList, SizeClass::kNumSizeClasses> free_lists_{};

    /// Sum of `max_size * class_size` across all classes. This is capacity
    /// accounting only and changes exclusively on refill/overflow/trim paths.
    size_t reserved_quota_bytes_{0};
    /// Aggregate quota-capacity ceiling for this owner thread.
    const size_t cache_budget_bytes_{kDefaultCacheBudgetBytes};
    /// Last cooperative trim generation observed by this owner thread.
    uint64_t observed_trim_epoch_{0};

    // Packed as `(epoch << 1) | mode`. One atomic publication keeps concurrent
    // pressure requesters from pairing one request's epoch with another
    // request's mode; this state is never read on a normal ThreadCache hit.
    inline static std::atomic<uint64_t> trim_request_{0};

    /// @brief Refills an empty FreeList from CentralCache and updates its quota.
    ///
    /// The quota follows a two-stage policy:
    /// - exponential warmup until one batch,
    /// - linear growth up to a bounded multiple of the batch size.
    /// @param idx Size-class index identifying the FreeList to refill; the
    ///        object size is derived as `SizeClass::Size(idx)`.
    AM_NOINLINE void* FetchFromCentralCache(size_t idx) noexcept;

    /// @brief Trims one batch to CentralCache and applies quota decay.
    ///
    /// Repeated overflow trims without intervening refill demand reduce
    /// `max_size`, preventing long-lived threads from pinning burst-era quotas.
    /// @param idx Size-class index identifying the FreeList to trim; the
    ///        object size is derived as `SizeClass::Size(idx)`.
    AM_NOINLINE void DeallocateSlowPath(size_t idx) noexcept;

    /// @brief Returns the fixed one-object-per-class quota reservation.
    AM_NODISCARD static constexpr size_t InitialReservedQuotaBytes() noexcept {
        size_t total = 0;
        for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
            total += SizeClass::Size(i);
        }
        return total;
    }

    /// @brief True if applying the quota transition for a class is admissible.
    ///
    /// Shrink and no-op transitions always pass (they release reservation);
    /// growth must fit inside the remaining per-thread byte budget.
    AM_NODISCARD bool CanSetQuota(size_t idx, size_t next_max_size) const noexcept;

    /// @brief Updates one class quota and its aggregate byte reservation.
    void SetQuota(size_t idx, size_t new_max_size) noexcept;
};

// CreateThreadCache reserves exactly one SystemAlloc page for this object.
// Lock the layout assumption so future members cannot silently outgrow it.
static_assert(sizeof(ThreadCache) <= SystemConfig::PAGE_SIZE,
              "ThreadCache must fit in one SystemAlloc page");
}// namespace ammalloc

#endif// AMMALLOC_THREAD_CACHE_H
