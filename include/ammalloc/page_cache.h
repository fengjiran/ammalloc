#ifndef AMMALLOC_PAGE_CACHE_H
#define AMMALLOC_PAGE_CACHE_H

/// @file page_cache.h
/// @brief Sharded page cache and lock-free-read radix page map.
///
/// PageCache manages Span splitting, owner-shard-local coalescing, and fallback
/// to PageAllocator. PageMap publishes `PageID -> Span*` mappings through a
/// fixed-depth radix tree whose lookup path performs only atomic loads.
/// @see docs/designs/04-page-cache.md, docs/designs/07-span-and-pagemap.md

// Internal build knob: selects the sharded PageCache implementation.
// Defined by default; undefine to fall back to the single-mutex legacy variant.
// Downstream code must not depend on this symbol being defined or undefined.
#define USE_PAGECACHE_SHARD

#include "ammalloc/assert.h"
#include "ammalloc/attributes.h"
#include "ammalloc/noalloc_diagnostics.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/span.h"

#include <mutex>

namespace ammalloc {

/// @brief Returns a monotonic timestamp for Span idle-time accounting.
/// @return Milliseconds elapsed on `std::chrono::steady_clock`'s epoch.
uint64_t GetCurrentTimeMs() noexcept;

/// @brief Stores the wide root level of the PageMap radix tree.
/// @note Page alignment preserves the fixed page-sized node layout.
struct alignas(SystemConfig::PAGE_SIZE) RadixRootNode {
    // Published via release store in EnsurePathLocked; read via acquire load in GetSpan.
    std::array<std::atomic<void*>, PageConfig::RADIX_ROOT_SIZE> children;

    RadixRootNode() noexcept {
        for (auto& child: children) {
            child.store(nullptr, std::memory_order_relaxed);
        }
    }
};

/// @brief Stores either child-node or Span pointers in the PageMap radix tree.
/// @note Page alignment lets ObjectPool dedicate page-aligned slots to nodes.
struct alignas(SystemConfig::PAGE_SIZE) RadixNode {
    // Published via release store in EnsurePathLocked; read via acquire load in GetSpan.
    std::array<std::atomic<void*>, PageConfig::RADIX_NODE_SIZE> children;

    RadixNode() noexcept {
        for (auto& child: children) {
            child.store(nullptr, std::memory_order_relaxed);
        }
    }
};

/// @brief Maps page IDs to their owning Span through a radix tree.
///
/// `GetSpan` is a lock-free read path. Writers must hold the relevant PageCache
/// shard lock while changing mappings; radix nodes are never reclaimed during
/// normal concurrent operation. A returned Span pointer is unpinned: callers
/// must supply the object-lifetime pinning required to dereference it.
class PageMap {
public:
    /// @brief Looks up the Span managing a page without acquiring a lock.
    /// @param page_id Global page index.
    /// @return Unpinned borrowed Span pointer, or null when the page is not mapped.
    /// @note Acquire loads pair with release stores in `SetSpan`. The caller
    ///       may dereference the result only while holding the owning shard
    ///       lock, a live small-object/cache pin through `Span::use_count`, the
    ///       unique live large allocation, or allocator-wide quiescence.
    static Span* GetSpan(size_t page_id) noexcept;

    /// @brief Looks up the Span managing an address without acquiring a lock.
    /// @param ptr Address to locate.
    /// @return Unpinned borrowed Span pointer, or null when the address is not
    ///         mapped.
    static Span* GetSpan(void* ptr) noexcept {
        return GetSpan(reinterpret_cast<uintptr_t>(ptr) >> SystemConfig::PAGE_SHIFT);
    }

    /// @brief Ensures every radix path needed by a page range exists.
    /// @param start_page_id First page index in the range.
    /// @param page_num Number of consecutive pages.
    /// @return False when radix metadata allocation fails; no Span leaf is
    ///         published on failure. Already attached internal nodes remain
    ///         owned by the PageMap and may be reused later.
    /// @note GetSpan remains lock-free. Structural growth is serialized only
    ///       when a missing node is observed, so independent PageCache shards
    ///       cannot publish competing paths.
    static bool EnsureRange(size_t start_page_id, size_t page_num) noexcept;

    /// @brief Maps every page in a Span to that Span without allocating.
    /// @param span Span to publish.
    /// @pre `span` is non-null and the owning PageCache shard lock is held.
    /// @pre EnsureRange(span->start_page_idx, span->page_num) succeeded, or
    ///      the range was already mapped before a split/coalesce remap.
    static void SetSpan(Span* span) noexcept;

    /// @brief Clears mappings for a half-open page range.
    /// @param start_page_id First page index to clear.
    /// @param page_num Number of consecutive pages to clear.
    /// @pre The owning PageCache shard lock is held.
    /// @pre Before recycling a descriptor removed from this range, no
    ///      unprotected PageMap reader retains it; existing callers rely on
    ///      their object-ownership pinning.
    static void ClearRange(size_t start_page_id, size_t page_num) noexcept;

    /// @brief Clears the radix root and releases pooled internal nodes.
    /// @pre All allocator users, including PageMap borrowers, are quiescent and
    ///      the caller serializes this reset.
    static void Reset() noexcept;

#ifdef AMMALLOC_TEST
    /// @brief Fails RadixNode allocation after the requested number of successes.
    /// @param successful_allocations Number of node allocations to permit, or
    ///        `SIZE_MAX` to disable fault injection.
    /// @pre Allocator users are quiescent.
    static void SetRadixAllocationFailureForTest(size_t successful_allocations) noexcept {
        radix_allocations_before_failure_.store(successful_allocations,
                                                std::memory_order_relaxed);
    }
#endif

private:
    static bool HasPath(size_t page_id) noexcept;
    static bool EnsurePathLocked(size_t page_id) noexcept;
    static RadixNode* TryNewRadixNode() noexcept;

    // Published with release semantics once initialized.
    inline static std::atomic<RadixRootNode*> root_ = nullptr;
    // Static backing storage for root_; avoids heap allocation during initialization.
    inline static RadixRootNode root_storage_{};
    inline static ObjectPool<RadixNode> radix_node_pool_{};
    /// Protects root initialization, monotonically growing node links, and every
    /// mutation of the process-wide `radix_node_pool_`. The pool itself does not
    /// lock, and it is shared by all shards, so this mutex is the only thing
    /// serializing its free list and bump pointer.
    inline static std::mutex structure_mutex_{};
#ifdef AMMALLOC_TEST
    inline static std::atomic<size_t> radix_allocations_before_failure_{
            std::numeric_limits<size_t>::max()};
#endif
};

#ifdef AMMALLOC_TEST
#define PAGE_CACHE_FRIENDS_TEST \
    friend class PageCacheTest;
#else
#define PAGE_CACHE_FRIENDS_TEST
#endif

/// @brief Owns one independently synchronized partition of PageCache state.
///
/// Each shard has one mutex, one page-count-indexed free-list array, and one
/// Span metadata pool. Allocation records the owner shard in the returned Span;
/// release and coalescing remain within that shard.
class alignas(SystemConfig::CACHE_LINE_SIZE) PageCacheShard {
public:
    PageCacheShard() = default;
    ~PageCacheShard() = default;

    PageCacheShard(const PageCacheShard&) = delete;
    PageCacheShard& operator=(const PageCacheShard&) = delete;

    /// @brief Reports whether a page-count bucket is empty.
    /// @param bucket_idx Page-count bucket index.
    /// @return True when the bucket contains no free spans.
    /// @pre `bucket_idx < span_lists_.size()`.
    AM_NODISCARD bool IsBucketEmpty(size_t bucket_idx) const noexcept {
        AM_DCHECK(bucket_idx < span_lists_.size());
        return span_lists_[bucket_idx].empty();
    }

    /// @brief Returns the mutex protecting this shard.
    /// @return Mutable reference to the shard mutex.
    AM_NODISCARD std::mutex& GetMutex() noexcept {
        return mutex_;
    }

    /// @brief Returns the mutex protecting this shard.
    /// @return Const reference to the shard mutex.
    AM_NODISCARD const std::mutex& GetMutex() const noexcept {
        return mutex_;
    }

private:
    /// Protects `span_lists_`, `span_pool_`, and PageMap writes by this shard.
    std::mutex mutex_;
    /// Free spans indexed by page count; index zero is unused.
    std::array<SpanList, PageConfig::MAX_PAGE_NUM + 1> span_lists_{};
    // Protected by mutex_.
    ObjectPool<Span> span_pool_{};

    /// @brief Allocates a Span while the shard mutex is held.
    /// @param page_num Requested number of pages.
    /// @return Owned-shard Span borrowed by the caller, or null on failure.
    /// @pre `mutex_` is held by the calling thread.
    Span* AllocSpanLocked(size_t page_num) noexcept;

    /// @brief Releases and coalesces a Span while the shard mutex is held.
    /// @param span Span owned by this shard.
    /// @pre `mutex_` is held and `span->owner_shard_id` identifies this shard.
    /// @pre A small-object Span has `use_count == 0` after every user and cache
    ///      object returned to its bitmap. A large Span is released once by its
    ///      unique live allocation owner.
    void ReleaseSpanLocked(Span* span) noexcept;

    /// @brief Releases all free spans and pooled metadata owned by this shard.
    /// @pre `mutex_` is held and allocator users, including PageMap borrowers,
    ///      are quiescent.
    void ResetLocked() noexcept;

    friend class PageCache;
    PAGE_CACHE_FRIENDS_TEST;
    friend class PageHeapScavenger;
};

#ifdef USE_PAGECACHE_SHARD

/// @brief Routes page allocation and release through owner-tracked shards.
///
/// Public allocation and release select or recover a shard and acquire only its
/// mutex. Span metadata remains owned by that shard's ObjectPool. PageMap reads
/// remain lock-free; its writes occur under the selected shard lock.
class PageCache {
public:
    /// @brief Returns the process-wide PageCache.
    /// @return Reference to the singleton stored without allocator recursion.
    static PageCache& GetInstance() {
        // Static storage prevents recursive allocation during singleton creation.
        alignas(alignof(PageCache)) static char storage[sizeof(PageCache)];
        static auto* instance = new (storage) PageCache();
        return *instance;
    }

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    /// @brief Allocates a Span containing `page_num` pages.
    /// @param page_num Required page count.
    /// @return Borrowed Span metadata owned by its selected shard, or null on failure.
    Span* AllocSpan(size_t page_num) noexcept;

    /// @brief Returns a Span to its recorded owner shard for coalescing.
    /// @param span Span previously returned by `AllocSpan`.
    /// @pre `span` is non-null and not already released.
    /// @pre Small-object Spans have no user, ThreadCache, or TransferCache
    ///      object remaining; large Spans are released by their unique live
    ///      allocation owner.
    void ReleaseSpan(Span* span) noexcept;

    /// @brief Releases free state from all active shards and resets PageMap.
    /// @pre All allocator users and PageMap borrowers are quiescent; intended
    ///      for test isolation.
    void Reset() noexcept;

    /// @brief Reports whether one shard's page-count bucket is empty.
    /// @param shard_id Active shard index.
    /// @param bucket_idx Page-count bucket index.
    /// @return True when the selected bucket has no free spans.
    AM_NODISCARD bool IsBucketEmpty(size_t shard_id,
                                    size_t bucket_idx) const noexcept {
        AM_DCHECK(shard_id < active_shard_count_);
        return GetShard(shard_id).IsBucketEmpty(bucket_idx);
    }

    /// @brief Returns the number of shards currently used for allocation.
    /// @return Active shard count.
    AM_NODISCARD uint16_t GetActiveShardCount() const noexcept {
        return active_shard_count_;
    }

    /// @brief Returns a bucket from shard zero for legacy scavenger access.
    /// @param i Page-count bucket index.
    /// @return Mutable reference to the selected SpanList.
    /// @pre Exactly one shard is active and its mutex protects mutation.
    AM_NODISCARD SpanList& GetSpanList(size_t i) noexcept {
        return GetShard(0).span_lists_[i];
    }

    /// @brief Returns a bucket from shard zero for legacy scavenger access.
    /// @param i Page-count bucket index.
    /// @return Const reference to the selected SpanList.
    /// @pre Exactly one shard is active and its mutex protects concurrent mutation.
    AM_NODISCARD const SpanList& GetSpanList(size_t i) const noexcept {
        return GetShard(0).span_lists_[i];
    }

    /// @brief Returns shard zero's mutex for legacy scavenger access.
    /// @return Mutable reference to the mutex.
    /// @pre Exactly one shard is active.
    AM_NODISCARD std::mutex& GetMutex() noexcept {
        AM_DCHECK(active_shard_count_ == 1);
        return GetShard(0).GetMutex();
    }

    /// @brief Returns shard zero's mutex for legacy scavenger access.
    /// @return Const reference to the mutex.
    /// @pre Exactly one shard is active.
    AM_NODISCARD const std::mutex& GetMutex() const noexcept {
        AM_DCHECK(active_shard_count_ == 1);
        return GetShard(0).GetMutex();
    }

    /// @brief Returns the mutex protecting an active shard.
    /// @param shard_id Active shard index.
    /// @return Mutable reference to the selected shard mutex.
    AM_NODISCARD std::mutex& GetShardMutex(uint16_t shard_id) noexcept {
        return GetShard(shard_id).GetMutex();
    }

    /// @brief Returns the mutex protecting an active shard.
    /// @param shard_id Active shard index.
    /// @return Const reference to the selected shard mutex.
    AM_NODISCARD const std::mutex& GetShardMutex(uint16_t shard_id) const noexcept {
        return GetShard(shard_id).GetMutex();
    }

#ifdef AMMALLOC_TEST
    /// @brief Changes the number of active shards for a test.
    /// @param shard_count New count in `[1, kMaxShardCount]`.
    /// @pre Allocator users are quiescent and no concurrent use has begun.
    void SetActiveShardCountForTest(uint16_t shard_count) noexcept {
        AM_DCHECK(shard_count >= 1);
        AM_DCHECK(shard_count <= kMaxShardCount);
        active_shard_count_ = shard_count;
    }
#endif

private:
    PageCache() = default;

    static constexpr uint16_t kMaxShardCount = 4;

    /// Number of shards currently enabled; the production default is one.
    uint16_t active_shard_count_{1};

    /// Fixed-capacity shard storage avoids runtime metadata allocation.
    std::array<PageCacheShard, kMaxShardCount> shards_{};

    /// @brief Selects an owner shard for a new allocation.
    /// @param page_num Requested page count.
    /// @return Active shard index.
    /// @note Currently returns shard 0.
    AM_NODISCARD uint16_t SelectShardForAlloc(size_t page_num) noexcept {
        return 0;
    }

    // Requires `shard_id < active_shard_count_`. Debug-only check.
    AM_NODISCARD PageCacheShard& GetShard(uint16_t shard_id) noexcept {
        AM_DCHECK(shard_id < active_shard_count_);
        return shards_[shard_id];
    }

    AM_NODISCARD const PageCacheShard& GetShard(uint16_t shard_id) const noexcept {
        AM_DCHECK(shard_id < active_shard_count_);
        return shards_[shard_id];
    }

    // Extracts owner from `span->owner_shard_id`; requires non-null span with valid shard ID.
    AM_NODISCARD PageCacheShard& OwnerShard(Span* span) noexcept {
        AM_DCHECK(span != nullptr);
        AM_DCHECK(span->owner_shard_id < active_shard_count_);
        return shards_[span->owner_shard_id];
    }

    AM_NODISCARD const PageCacheShard& OwnerShard(const Span* span) const noexcept {
        AM_DCHECK(span != nullptr);
        AM_DCHECK(span->owner_shard_id < active_shard_count_);
        return shards_[span->owner_shard_id];
    }
};

#else
/// @brief Legacy PageCache implementation protected by one global mutex.
/// @note The sharded implementation is selected when `USE_PAGECACHE_SHARD` is defined.
class PageCache {
public:
    /// @brief Returns the process-wide PageCache.
    /// @return Reference to the singleton stored without allocator recursion.
    static PageCache& GetInstance() {
        // Static storage prevents recursive allocation during singleton creation.
        alignas(alignof(PageCache)) static char storage[sizeof(PageCache)];
        static auto* instance = new (storage) PageCache();
        return *instance;
    }

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    /// @brief Allocates a Span containing `page_num` pages.
    /// @param page_num Required page count.
    /// @return The allocated span, or nullptr if system allocation fails.
    Span* AllocSpan(size_t page_num) noexcept {
        detail::NoThrowLockGuard lock(mutex_);
        return AllocSpanLocked(page_num);
    }

    /// @brief Releases and coalesces a Span under the global mutex.
    /// @param span Span previously returned by `AllocSpan`.
    /// @pre `span` is non-null and not already released.
    /// @pre Small-object Spans have no user or cached object remaining; large
    ///      Spans are released by their unique live allocation owner.
    void ReleaseSpan(Span* span) noexcept;

    /// @brief Clears all free spans and resets PageMap for test isolation.
    /// @pre All allocator users and PageMap borrowers are quiescent.
    void Reset() noexcept;

    /// @brief Reports whether a page-count bucket is empty.
    /// @param bucket_idx Page-count bucket index.
    /// @return True when the bucket contains no free spans.
    /// @pre `bucket_idx < span_lists_.size()`.
    AM_NODISCARD bool IsBucketEmpty(size_t bucket_idx) const noexcept {
        AM_DCHECK(bucket_idx < span_lists_.size());
        return span_lists_[bucket_idx].empty();
    }

    /// @brief Returns the global mutex protecting this legacy PageCache.
    /// @return Mutable reference to the mutex.
    AM_NODISCARD std::mutex& GetMutex() noexcept {
        return mutex_;
    }

    /// @brief Returns a page-count bucket from the legacy PageCache.
    /// @param i Page-count bucket index.
    /// @return Mutable reference to the selected SpanList.
    AM_NODISCARD SpanList& GetSpanList(size_t i) noexcept {
        return span_lists_[i];
    }

private:
    /// Protects free lists, Span metadata, and PageMap writes.
    std::mutex mutex_;
    /// Free spans indexed by page count; index zero is unused.
    std::array<SpanList, PageConfig::MAX_PAGE_NUM + 1> span_lists_{};
    // Protected by mutex_.
    ObjectPool<Span> span_pool_{};

    PageCache() = default;
    ~PageCache() = default;

    /// @brief Allocates a Span while `mutex_` is held.
    /// @param page_num Required page count.
    /// @return Allocated Span, or null on failure.
    /// @pre `mutex_` is held by the caller.
    Span* AllocSpanLocked(size_t page_num) noexcept;

    PAGE_CACHE_FRIENDS_TEST;
    friend class PageHeapScavenger;
};

#endif


}// namespace ammalloc

#endif// AMMALLOC_PAGE_CACHE_H
