/// @file central_cache.cpp
/// @brief Middle-end object cache with per-size-class TransferCache and SpanList management.

#include "ammalloc/central_cache.h"
#include "ammalloc/assert.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/spin_lock.h"

#include <algorithm>
#include <array>
#include <limits>

namespace ammalloc {

#ifdef AMMALLOC_TEST
std::atomic<size_t> g_mock_fetch_range_cap{0};
#endif

size_t CentralCache::FetchRange(FreeList& free_list, size_t fetch_num,
                                size_t aligned_size) noexcept {
    AM_DCHECK(fetch_num <= SizeClass::kMaxBatchSize);
#ifdef AMMALLOC_TEST
    if (const size_t cap = g_mock_fetch_range_cap.load(std::memory_order_relaxed);
        cap > 0 && cap < fetch_num) {
        fetch_num = cap;
    }
#endif
    const auto idx = SizeClass::Index(aligned_size);
    auto& bucket = buckets_[idx];

    // Only [0, grab_count) or [0, actual_prefetched) is read after being
    // written. Reuse one uninitialized buffer for the non-overlapping
    // TransferCache and SpanList phases to avoid clearing and reserving two
    // 4 KiB pointer arrays on every call.
    std::array<void*, SizeClass::kMaxBatchSize> scratch_ptrs;// NOLINT

    // Probe the O(1) TransferCache before taking the SpanList mutex.
    bucket.transfer_cache_lock.lock();
    size_t grab_count = std::min(fetch_num, bucket.transfer_cache_size);
    for (size_t i = 0; i < grab_count; ++i) {
        const size_t logical_index = --bucket.transfer_cache_size;
        scratch_ptrs[i] = bucket.transfer_cache[TransferIndex(bucket, logical_index)];
    }
    bucket.transfer_cache_lock.unlock();
    if (grab_count > 0) {
        stats_.fetch_transfer_hit_objects.fetch_add(grab_count,
                                                    std::memory_order_relaxed);
    }

    size_t fetched = grab_count;
    void* head = nullptr;
    void* tail = nullptr;
    for (size_t i = fetched; i > 0; --i) {
        void* obj = scratch_ptrs[i - 1];
        auto* node = static_cast<FreeBlock*>(obj);
        if (!head) {
            tail = obj;
        }
        node->next = static_cast<FreeBlock*>(head);
        head = node;
    }

    if (fetched < fetch_num) {
        const size_t need_for_thread = fetch_num - fetched;
        // Prefetch one additional batch to amortize the SpanList lock for the
        // next requester.
        // TODO(owner): Bound prefetching by the observed TransferCache capacity.
        const size_t extract_target = need_for_thread + fetch_num;
        size_t actual_prefetched = 0;
        size_t extracted = 0;

        SpanListUniqueLock lock(bucket.span_list_lock);
        auto& cur_span_list = bucket.span_list;
        auto begin = cur_span_list.begin();
        size_t rotations = 0;
        size_t traversals = 0;
        while (extracted < extract_target) {
            if (cur_span_list.empty() || begin->IsFull()) {
                if (!GetOneSpan(bucket, aligned_size, lock)) {
                    break;
                }
                // GetOneSpan pushed a fresh Span to the front; refresh the
                // head iterator before allocating from it.
                begin = cur_span_list.begin();
            }
            ++traversals;

            while (extracted < extract_target) {
                void* obj = begin->AllocObject();
                if (!obj) {
                    // Keep full Spans behind candidates that still have free bits.
                    Span* full_span = &*begin;
                    cur_span_list.erase(full_span);
                    cur_span_list.push_back(full_span);
                    ++rotations;
                    begin = cur_span_list.begin();
                    break;
                }

                if (extracted < need_for_thread) {
                    auto* node = static_cast<FreeBlock*>(obj);
                    if (!head) {
                        tail = obj;
                    }

                    node->next = static_cast<FreeBlock*>(head);
                    head = node;
                    ++fetched;
                } else {
                    scratch_ptrs[actual_prefetched++] = obj;
                }
                ++extracted;
            }

            if (begin->IsFull()) {
                Span* full_span = &*begin;
                cur_span_list.erase(full_span);
                cur_span_list.push_back(full_span);
                ++rotations;
                begin = cur_span_list.begin();
            }
        }
        // Publish prefetched pointers only after leaving the Span bitmap lock domain.
        if (extracted > 0) {
            stats_.fetch_span_list_objects.fetch_add(extracted,
                                                     std::memory_order_relaxed);
        }
        if (traversals > 0) {
            stats_.spanlist_traversals.fetch_add(traversals,
                                                 std::memory_order_relaxed);
        }
        if (rotations > 0) {
            stats_.spanlist_rotations.fetch_add(rotations,
                                                std::memory_order_relaxed);
        }
        lock.unlock();

        if (actual_prefetched > 0) {
            size_t successfully_pushed = 0;
            bucket.transfer_cache_lock.lock();
            while (successfully_pushed < actual_prefetched &&
                   bucket.transfer_cache_size < bucket.transfer_cache_capacity) {
                bucket.transfer_cache[TransferIndex(bucket, bucket.transfer_cache_size++)] =
                        scratch_ptrs[successfully_pushed++];
            }
            bucket.transfer_cache_lock.unlock();

            // Another thread may fill TransferCache while this thread scans
            // SpanList. Return any excess through the normal ownership path.
            if (successfully_pushed < actual_prefetched) {
                void* leftover_head = nullptr;

                for (size_t i = successfully_pushed; i < actual_prefetched; ++i) {
                    auto* node = static_cast<FreeBlock*>(scratch_ptrs[i]);
                    node->next = static_cast<FreeBlock*>(leftover_head);
                    leftover_head = scratch_ptrs[i];
                }

                ReleaseListToSpans(leftover_head, idx);
            }
        }
    }

    if (fetched > 0) {
        // `fetched` is the node count of the head/tail chain built above;
        // PushRange trusts `count` (debug-verified), so keep them in lockstep.
        free_list.PushRange(FreeChain{head, tail, fetched});
    }
    return fetched;
}

const CentralCacheStats& CentralCache::GetStats() noexcept {
    return stats_;
}

size_t CentralCache::GetTransferCacheCountForTest(size_t idx) noexcept {
    AM_DCHECK(idx < SizeClass::kNumSizeClasses);
    auto& bucket = buckets_[idx];
    bucket.transfer_cache_lock.lock();
    const size_t count = bucket.transfer_cache_size;
    bucket.transfer_cache_lock.unlock();
    return count;
}

void CentralCache::ReleaseListToSpans(void* start, size_t idx,
                                      CentralReleaseMode mode) noexcept {
    AM_DCHECK(idx < SizeClass::kNumSizeClasses);
    auto& bucket = buckets_[idx];
    void* cur = start;

    while (cur) {
        // Only [0, local_count) is read after being written; keep the buffer
        // uninitialized to avoid clearing 4 KiB on every batch.
        std::array<void*, SizeClass::kMaxBatchSize> local_ptrs;// NOLINT
        size_t local_count = 0;
        while (cur && local_count < SizeClass::kMaxBatchSize) {
            local_ptrs[local_count++] = cur;
            cur = static_cast<FreeBlock*>(cur)->next;
        }

        size_t pushed = 0;
        if (mode == CentralReleaseMode::kTransferCache) {
            // Absorb the batch without touching Span metadata when capacity
            // permits. RSS trims select kSpanBitmap and intentionally bypass
            // this tier so no empty Span remains pinned by CentralCache.
            bucket.transfer_cache_lock.lock();
            while (pushed < local_count &&
                   bucket.transfer_cache_size < bucket.transfer_cache_capacity) {
                bucket.transfer_cache[TransferIndex(bucket, bucket.transfer_cache_size++)] =
                        local_ptrs[pushed++];
            }
            bucket.transfer_cache_lock.unlock();
        }

        if (pushed < local_count) {
            // Objects that could not be absorbed by TransferCache fall through
            // to the Span bitmap path. Track the overflow so benchmarks can
            // attribute release pressure between the two tiers.
            if (mode == CentralReleaseMode::kTransferCache) {
                stats_.release_transfer_overflow_objects.fetch_add(
                        local_count - pushed, std::memory_order_relaxed);
            }
            // Spans that reach use_count == 0 are collected into an intrusive
            // list and released to PageCache only after dropping the bucket
            // lock: one unlock per batch instead of one per empty Span, and no
            // PageCache entry while holding a CentralCache mutex (which would
            // invert the allocator lock order).
            Span* empty_span_head = nullptr;
            SpanListUniqueLock lock(bucket.span_list_lock);
            for (size_t i = pushed; i < local_count; ++i) {
                void* obj = local_ptrs[i];
                auto* span = PageMap::GetSpan(obj);
                if (!span) {
                    continue;
                }

                span->FreeObject(obj);
                // Empty and newly-non-full are mutually exclusive except for
                // capacity-1 spans where both would match; an empty span leaves
                // the bucket entirely, so only non-empty candidates need front
                // placement.
                if (span->use_count == 0) {
                    bucket.span_list.erase(span);
                    // Reuse the intrusive link for the release collection so
                    // batching cannot recurse through an STL allocation.
                    span->next = empty_span_head;
                    empty_span_head = span;
                    if (mode == CentralReleaseMode::kSpanBitmap) {
                        stats_.spans_unpinned_by_direct_release.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                } else if (span->use_count == span->capacity - 1) {
                    // Make a newly non-full Span the next allocation candidate.
                    bucket.span_list.erase(span);
                    bucket.span_list.push_front(span);
                }
            }
            lock.unlock();

            while (empty_span_head) {
                auto* next_span = empty_span_head->next;
                stats_.release_spans_returned_to_pagecache.fetch_add(
                        1, std::memory_order_relaxed);
                PageCache::GetInstance().ReleaseSpan(empty_span_head);
                empty_span_head = next_span;
            }
        }
    }
}

size_t CentralCache::DrainTransferCaches(size_t max_bytes) noexcept {
    if (max_bytes == 0) {
        return 0;
    }

    const bool unbounded = max_bytes == std::numeric_limits<size_t>::max();
    size_t drained_bytes = 0;

    // Large classes release the most retained bytes per bounded pointer batch.
    for (size_t i = SizeClass::kNumSizeClasses; i > 0; --i) {
        const size_t idx = i - 1;
        const size_t aligned_size = SizeClass::Size(idx);
        auto& bucket = buckets_[idx];

        if (!unbounded && max_bytes < aligned_size) {
            continue;
        }

        // Snapshot a bounded amount of this bucket. A concurrent producer may
        // append after the snapshot, but cannot make a drain with SIZE_MAX run
        // forever; a later pressure request handles newly retained objects.
        bucket.transfer_cache_lock.lock();
        const size_t snapshot_count =
                unbounded
                        ? bucket.transfer_cache_size
                        : std::min(bucket.transfer_cache_size, max_bytes / aligned_size);
        bucket.transfer_cache_lock.unlock();

        size_t remaining_from_snapshot = snapshot_count;
        while (remaining_from_snapshot > 0) {
            const size_t batch_limit = std::min(SizeClass::kMaxBatchSize,
                                                remaining_from_snapshot);

            // Only [0, detached) is read after being written; keep the buffer
            // uninitialized to avoid clearing 4 KiB on every batch.
            std::array<void*, SizeClass::kMaxBatchSize> local_ptrs;// NOLINT
            bucket.transfer_cache_lock.lock();
            const size_t detached = std::min(batch_limit,
                                             bucket.transfer_cache_size);
            if (detached == 0) {
                bucket.transfer_cache_lock.unlock();
                break;
            }

            // `transfer_cache_begin` is the cold end; normal FetchRange pops
            // from the logical end. A circular array avoids O(capacity) shifting
            // under this SpinLock while preserving the remaining LIFO order.
            for (size_t j = 0; j < detached; ++j) {
                local_ptrs[j] = bucket.transfer_cache[TransferIndex(bucket, j)];
            }
            bucket.transfer_cache_begin = detached == bucket.transfer_cache_capacity
                                                  ? 0
                                                  : TransferIndex(bucket, detached);
            bucket.transfer_cache_size -= detached;
            bucket.transfer_cache_lock.unlock();

            void* head = nullptr;
            for (size_t j = 0; j < detached; ++j) {
                auto* node = static_cast<FreeBlock*>(local_ptrs[j]);
                node->next = static_cast<FreeBlock*>(head);
                head = node;
            }

            // The transfer lock is intentionally not held across PageMap,
            // bitmap, or PageCache work. ReleaseListToSpans drops the bucket
            // mutex before it can enter PageCache as well.
            ReleaseListToSpans(head, idx, CentralReleaseMode::kSpanBitmap);

            const size_t batch_bytes = detached * aligned_size;
            drained_bytes += batch_bytes;
            remaining_from_snapshot -= detached;
            if (!unbounded) {
                max_bytes -= batch_bytes;
            }
        }
    }

    stats_.transfer_cache_drained_bytes.fetch_add(drained_bytes,
                                                  std::memory_order_relaxed);
    return drained_bytes;
}

void CentralCache::Reset() noexcept {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        auto& bucket = buckets_[i];
        void* head = nullptr;
        bucket.transfer_cache_lock.lock();
        for (size_t j = 0; j < bucket.transfer_cache_size; ++j) {
            void* obj = bucket.transfer_cache[TransferIndex(bucket, j)];
            static_cast<FreeBlock*>(obj)->next = static_cast<FreeBlock*>(head);
            head = obj;
        }
        bucket.transfer_cache_size = 0;
        bucket.transfer_cache_begin = 0;
        bucket.transfer_cache_lock.unlock();

        Span* span_list_head = nullptr;
        {
            SpanListLockGuard lock(bucket.span_list_lock);

            // Restore bitmap ownership for every object detached from TransferCache.
            void* cur = head;
            while (cur) {
                void* next = static_cast<FreeBlock*>(cur)->next;
                if (auto* span = PageMap::GetSpan(cur)) {
                    span->FreeObject(cur);
                }
                cur = next;
            }

            while (!bucket.span_list.empty()) {
                auto* span = bucket.span_list.pop_front();
                // Reuse intrusive links so reset cannot recurse through an STL allocation.
                span->next = span_list_head;
                span_list_head = span;
            }
        }

        // Release bucket locking before entering PageCache.
        while (span_list_head) {
            auto* next_span = span_list_head->next;
            PageCache::GetInstance().ReleaseSpan(span_list_head);
            span_list_head = next_span;
        }
    }

    // Bucket zero retains the base of the one contiguous TransferCache mapping.
    if (buckets_[0].transfer_cache) {
        // Release the same total the allocation used: sum the stored bucket
        // capacities instead of recomputing the batch policy.
        size_t total_ptrs = 0;
        for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
            total_ptrs += buckets_[i].transfer_cache_capacity;
        }
        size_t total_bytes = total_ptrs * sizeof(void*);
        size_t page_num = (total_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
        PageAllocator::SystemFree(buckets_[0].transfer_cache, page_num);

        // Invalidate every borrowed slice after releasing the shared mapping.
        for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
            auto& bucket = buckets_[i];
            bucket.transfer_cache = nullptr;
            bucket.transfer_cache_capacity = 0;
            bucket.transfer_cache_size = 0;
            bucket.transfer_cache_begin = 0;
        }
    }

    // Rebuild the backing so the singleton keeps its O(1) fast path after
    // Reset; an OOM here degrades gracefully to the SpanList slow path.
    static_cast<void>(TryInitTransferCache());
}

size_t CentralCache::FillTransferCapacities(
        std::array<size_t, SizeClass::kNumSizeClasses>& out) noexcept {
    size_t total_ptrs = 0;
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        out[i] = kCapScale * SizeClass::CalculateBatchSize(SizeClass::Size(i));
        total_ptrs += out[i];
    }
    return total_ptrs;
}

bool CentralCache::TryInitTransferCache() noexcept {
    std::array<size_t, SizeClass::kNumSizeClasses> capacities{};
    const auto total_ptrs = FillTransferCapacities(capacities);

    // One PageAllocator mapping avoids recursive am_malloc entry and per-bucket VMAs.
    size_t total_bytes = total_ptrs * sizeof(void*);
    size_t page_num = (total_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
    void* p = PageAllocator::SystemAlloc(page_num);
    if (!p) {
        return false;
    }

    auto** cur_ptr = static_cast<void**>(p);
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        buckets_[i].transfer_cache_capacity = capacities[i];
        buckets_[i].transfer_cache = cur_ptr;
        buckets_[i].transfer_cache_begin = 0;
        cur_ptr += capacities[i];
    }
    return true;
}

Span* CentralCache::GetOneSpan(Bucket& bucket, size_t aligned_size,
                               SpanListUniqueLock& lock) noexcept {
    lock.unlock();
    auto page_num = SizeClass::GetMovePageNum(aligned_size);
    auto* span = PageCache::GetInstance().AllocSpan(page_num);
    if (!span) {
        // Restore the lock before returning: the caller relies on holding it
        // on both success and failure.
        lock.lock();
        return nullptr;
    }

    span->Init(aligned_size);
    lock.lock();
    stats_.fetch_pagecache_spans.fetch_add(1, std::memory_order_relaxed);
    bucket.span_list.push_front(span);
    return span;
}

}// namespace ammalloc
