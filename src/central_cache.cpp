#include "ammalloc/central_cache.h"
#include "ammalloc/assert.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/spin_lock.h"

#include <spdlog/spdlog.h>

namespace ammalloc {

#ifdef AMMALLOC_TEST
std::atomic<size_t> g_mock_fetch_range_cap{0};
#endif

size_t CentralCache::FetchRange(FreeList& block_list, size_t batch_num, size_t aligned_size) {
    AM_DCHECK(batch_num <= SizeClass::kMaxBatchSize);
#ifdef AMMALLOC_TEST
    if (const size_t cap = g_mock_fetch_range_cap.load(std::memory_order_relaxed);
        cap > 0 && cap < batch_num) {
        batch_num = cap;
    }
#endif
    auto idx = SizeClass::Index(aligned_size);
    auto& bucket = buckets_[idx];

    void* local_ptrs[SizeClass::kMaxBatchSize];
    size_t fetched = 0;

    // Probe the O(1) TransferCache before taking the SpanList mutex.
    bucket.transfer_cache_lock.lock();
    size_t grab_count = std::min(batch_num, bucket.transfer_cache_count);
    for (size_t i = 0; i < grab_count; ++i) {
        local_ptrs[i] = bucket.transfer_cache[--bucket.transfer_cache_count];
    }
    bucket.transfer_cache_lock.unlock();

    fetched = grab_count;
    void* head = nullptr;
    void* tail = nullptr;
    for (size_t i = fetched; i > 0; --i) {
        void* obj = local_ptrs[i - 1];
        auto* node = static_cast<FreeBlock*>(obj);
        if (!head) {
            tail = obj;
        }
        node->next = static_cast<FreeBlock*>(head);
        head = node;
    }

    if (fetched < batch_num) {
        size_t need_for_thread = batch_num - fetched;
        // Prefetch one additional batch to amortize the SpanList lock for the
        // next requester.
        // TODO(ammalloc): Bound prefetching by the observed TransferCache capacity.
        size_t prefetch_target = batch_num;
        size_t total_to_extract = need_for_thread + prefetch_target;
        void* prefetch_ptrs[SizeClass::kMaxBatchSize];
        size_t actual_prefetched = 0;
        size_t total_extracted = 0;

        std::unique_lock<std::mutex> lock(bucket.span_list_lock);
        while (total_extracted < total_to_extract) {
            if (bucket.span_list.empty() ||
                bucket.span_list.begin()->use_count >= bucket.span_list.begin()->capacity) {
                if (!GetOneSpan(bucket, aligned_size, lock)) {
                    // GetOneSpan releases the lock before calling PageCache and
                    // leaves it unlocked on failure; reacquire so the unique_lock
                    // destructor/unlock below cannot throw on an unlocked mutex.
                    lock.lock();
                    break;
                }
            }

            auto* span = bucket.span_list.begin();
            while (total_extracted < total_to_extract) {
                void* obj = span->AllocObject();
                if (!obj) {
                    // Keep full Spans behind candidates that still have free bits.
                    bucket.span_list.erase(span);
                    bucket.span_list.push_back(span);
                    break;
                }

                if (total_extracted < need_for_thread) {
                    auto* node = static_cast<FreeBlock*>(obj);
                    if (!head) {
                        tail = obj;
                    }
                    node->next = static_cast<FreeBlock*>(head);
                    head = node;
                    ++fetched;
                } else {
                    prefetch_ptrs[actual_prefetched++] = obj;
                }
                ++total_extracted;
            }
        }
        // Publish prefetched pointers only after leaving the Span bitmap lock domain.
        lock.unlock();

        if (actual_prefetched > 0) {
            size_t successfully_pushed = 0;
            bucket.transfer_cache_lock.lock();
            while (successfully_pushed < actual_prefetched &&
                   bucket.transfer_cache_count < bucket.transfer_cache_capacity) {
                bucket.transfer_cache[bucket.transfer_cache_count++] = prefetch_ptrs[successfully_pushed++];
            }
            bucket.transfer_cache_lock.unlock();

            // Another thread may fill TransferCache while this thread scans
            // SpanList. Return any excess through the normal ownership path.
            if (successfully_pushed < actual_prefetched) {
                void* leftover_head = nullptr;

                for (size_t i = successfully_pushed; i < actual_prefetched; ++i) {
                    auto* node = static_cast<FreeBlock*>(prefetch_ptrs[i]);
                    node->next = static_cast<FreeBlock*>(leftover_head);
                    leftover_head = prefetch_ptrs[i];
                }

                ReleaseListToSpans(leftover_head, aligned_size);
            }
        }
    }

    if (fetched > 0) {
        // `fetched` is the node count of the head/tail chain built above;
        // push_range trusts `count` (debug-verified), so keep them in lockstep.
        block_list.push_range(FreeChain{head, tail, fetched});
    }
    return fetched;
}

void CentralCache::ReleaseListToSpans(void* start, size_t aligned_size) {
    auto idx = SizeClass::Index(aligned_size);
    auto& bucket = buckets_[idx];
    void* cur = start;

    while (cur) {
        void* local_ptrs[SizeClass::kMaxBatchSize];
        size_t local_count = 0;
        while (cur && local_count < SizeClass::kMaxBatchSize) {
            local_ptrs[local_count++] = cur;
            cur = static_cast<FreeBlock*>(cur)->next;
        }

        // Absorb the batch without touching Span metadata when capacity permits.
        size_t pushed = 0;
        bucket.transfer_cache_lock.lock();
        while (pushed < local_count && bucket.transfer_cache_count < bucket.transfer_cache_capacity) {
            bucket.transfer_cache[bucket.transfer_cache_count++] = local_ptrs[pushed++];
        }
        bucket.transfer_cache_lock.unlock();

        if (pushed < local_count) {
            std::unique_lock<std::mutex> lock(bucket.span_list_lock);
            for (size_t i = pushed; i < local_count; ++i) {
                void* obj = local_ptrs[i];
                auto* span = PageMap::GetSpan(obj);
                if (!span) {
                    continue;
                }

                span->FreeObject(obj);
                // Make a newly non-full Span the next allocation candidate.
                if (span->use_count == span->capacity - 1) {
                    bucket.span_list.erase(span);
                    bucket.span_list.push_front(span);
                }

                if (span->use_count == 0) {
                    bucket.span_list.erase(span);
                    // Never enter PageCache while holding a CentralCache bucket
                    // mutex; doing so would invert the allocator lock order.
                    lock.unlock();
                    PageCache::GetInstance().ReleaseSpan(span);
                    lock.lock();
                }
            }
        }
    }
}

void CentralCache::Reset() noexcept {
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        auto& bucket = buckets_[i];
        void* head = nullptr;
        bucket.transfer_cache_lock.lock();
        for (size_t j = 0; j < bucket.transfer_cache_count; ++j) {
            void* obj = bucket.transfer_cache[j];
            static_cast<FreeBlock*>(obj)->next = static_cast<FreeBlock*>(head);
            head = obj;
        }
        bucket.transfer_cache_count = 0;
        bucket.transfer_cache_lock.unlock();

        Span* span_list_head = nullptr;
        {
            std::lock_guard<std::mutex> lock(bucket.span_list_lock);

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
        size_t total_ptrs = 0;
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            size_t batch_num = SizeClass::CalculateBatchSize(SizeClass::Size(i));
            total_ptrs += kCapScale * batch_num;
        }
        size_t total_bytes = total_ptrs * sizeof(void*);
        size_t page_num = (total_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
        PageAllocator::SystemFree(buckets_[0].transfer_cache, page_num);

        // Invalidate every borrowed slice after releasing the shared mapping.
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            auto& bucket = buckets_[i];
            bucket.transfer_cache = nullptr;
            bucket.transfer_cache_capacity = 0;
            bucket.transfer_cache_count = 0;
        }
    }
}

void CentralCache::InitTransferCache() {
    size_t total_ptrs = 0;
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        size_t batch_num = SizeClass::CalculateBatchSize(SizeClass::Size(i));
        total_ptrs += kCapScale * batch_num;
    }

    // One PageAllocator mapping avoids recursive am_malloc entry and per-bucket VMAs.
    size_t total_bytes = total_ptrs * sizeof(void*);
    size_t page_num = (total_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
    void* p = PageAllocator::SystemAlloc(page_num);
    if (!p) {
        spdlog::critical("CentralCache failed to allocate memory for TransferCaches!");
        std::abort();
    }

    auto** cur_ptr = static_cast<void**>(p);
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        size_t batch_num = SizeClass::CalculateBatchSize(SizeClass::Size(i));
        buckets_[i].transfer_cache_capacity = batch_num * kCapScale;
        buckets_[i].transfer_cache = cur_ptr;
        cur_ptr += buckets_[i].transfer_cache_capacity;
    }
}

Span* CentralCache::GetOneSpan(Bucket& bucket, size_t aligned_size, std::unique_lock<std::mutex>& lock) {
    lock.unlock();
    auto page_num = SizeClass::GetMovePageNum(aligned_size);
    auto* span = PageCache::GetInstance().AllocSpan(page_num);
    if (!span) {
        return nullptr;
    }

    span->Init(aligned_size);
    lock.lock();
    bucket.span_list.push_front(span);
    return span;
}

}// namespace ammalloc
