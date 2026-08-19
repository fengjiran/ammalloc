#include "ammalloc/page_cache.h"

#include <limits>

namespace ammalloc {

uint64_t GetCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

Span* PageMap::GetSpan(size_t page_id) {
    // clang-format off
    // Acquire semantics ensure we see the initialized data of the root node
    // if it was just created by another thread.
    auto* curr = root_.load(std::memory_order_acquire);
    if (!curr) AM_UNLIKELY {
        return nullptr;
    }

    const size_t i0 = page_id >> (PageConfig::RADIX_NODE_BITS * 3);
    if (i0 >= PageConfig::RADIX_ROOT_SIZE) AM_UNLIKELY {
        return nullptr;
    }
    const size_t i1 = (page_id >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
    const size_t i2 = (page_id >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;
    const size_t i3 = page_id & PageConfig::RADIX_MASK;

    auto* p1 = static_cast<RadixNode*>(curr->children[i0].load(std::memory_order_acquire));
    if (!p1) AM_UNLIKELY {
        return nullptr;
    }

    auto* p2 = static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_acquire));
    if (!p2) AM_UNLIKELY {
        return nullptr;
    }

    auto* p3 = static_cast<RadixNode*>(p2->children[i2].load(std::memory_order_acquire));
    if (!p3) AM_UNLIKELY {
        return nullptr;
    }

    // The acquire leaf load observes Span metadata published before SetSpan's
    // release store on weak and strong memory models.
    return static_cast<Span*>(p3->children[i3].load(std::memory_order_acquire));
    // clang-format on
}

void PageMap::SetSpan(Span* span) {
    auto* curr = root_.load(std::memory_order_relaxed);
    if (!curr) {
        curr = &root_storage_;
        for (auto& child: curr->children) {
            child.store(nullptr, std::memory_order_relaxed);
        }
        AM_DCHECK((reinterpret_cast<uintptr_t>(curr) & (4096 - 1)) == 0);
        root_.store(curr, std::memory_order_release);
    }

    auto start = span->start_page_idx;
    const auto end = start + span->page_num;

    while (start < end) {
        // Lazily allocate radix nodes only on the PageMap write path.
        const size_t i0 = start >> (PageConfig::RADIX_NODE_BITS * 3);
        const size_t i1 = (start >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
        const size_t i2 = (start >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;
        const size_t i3 = start & PageConfig::RADIX_MASK;

        auto* p1 = static_cast<RadixNode*>(curr->children[i0].load(std::memory_order_relaxed));
        if (!p1) {
            p1 = radix_node_pool_.New();
            AM_DCHECK((reinterpret_cast<uintptr_t>(p1) & (4096 - 1)) == 0);
            curr->children[i0].store(p1, std::memory_order_release);
        }

        auto* p2 = static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_relaxed));
        if (!p2) {
            p2 = radix_node_pool_.New();
            AM_DCHECK((reinterpret_cast<uintptr_t>(p2) & (4096 - 1)) == 0);
            p1->children[i1].store(p2, std::memory_order_release);
        }

        auto* p3 = static_cast<RadixNode*>(p2->children[i2].load(std::memory_order_relaxed));
        if (!p3) {
            p3 = radix_node_pool_.New();
            AM_DCHECK((reinterpret_cast<uintptr_t>(p3) & (4096 - 1)) == 0);
            p2->children[i2].store(p3, std::memory_order_release);
        }

        size_t cnt = std::min(end - start, PageConfig::RADIX_NODE_SIZE - i3);
        for (size_t k = 0; k < cnt; ++k) {
            p3->children[i3 + k].store(span, std::memory_order_release);
        }
        start += cnt;
    }
}

void PageMap::ClearRange(size_t start_page_id, size_t page_num) {
    auto* curr = root_.load(std::memory_order_relaxed);
    if (!curr) {
        return;
    }

    auto cur_page_id = start_page_id;
    auto remaining_pages = page_num;

    while (remaining_pages > 0) {
        const size_t i0 = cur_page_id >> (PageConfig::RADIX_NODE_BITS * 3);
        auto* p1 = static_cast<RadixNode*>(curr->children[i0].load(std::memory_order_relaxed));
        if (!p1) {
            constexpr size_t l0_coverage = 1ULL << (PageConfig::RADIX_NODE_BITS * 3);
            size_t skip = l0_coverage - (cur_page_id & (l0_coverage - 1));
            size_t step = std::min(remaining_pages, skip);
            cur_page_id += step;
            remaining_pages -= step;
            continue;
        }

        const size_t i1 = (cur_page_id >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
        auto* p2 = static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_relaxed));
        if (!p2) {
            constexpr size_t l1_coverage = 1ULL << (PageConfig::RADIX_NODE_BITS * 2);
            size_t skip = l1_coverage - (cur_page_id & (l1_coverage - 1));
            size_t step = std::min(remaining_pages, skip);
            cur_page_id += step;
            remaining_pages -= step;
            continue;
        }

        const size_t i2 = (cur_page_id >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;
        auto* p3 = static_cast<RadixNode*>(p2->children[i2].load(std::memory_order_relaxed));
        if (!p3) {
            constexpr size_t l2_coverage = 1ULL << PageConfig::RADIX_NODE_BITS;
            size_t skip = l2_coverage - (cur_page_id & (l2_coverage - 1));
            size_t step = std::min(remaining_pages, skip);
            cur_page_id += step;
            remaining_pages -= step;
            continue;
        }

        const size_t i3 = cur_page_id & PageConfig::RADIX_MASK;
        size_t cnt = std::min(remaining_pages, PageConfig::RADIX_NODE_SIZE - i3);

        for (size_t k = 0; k < cnt; ++k) {
            p3->children[i3 + k].store(nullptr, std::memory_order_release);
        }
        cur_page_id += cnt;
        remaining_pages -= cnt;
    }
}

void PageMap::Reset() {
    root_.store(nullptr, std::memory_order_relaxed);
    radix_node_pool_.ReleaseMemory();
}

Span* PageCacheShard::AllocSpanLocked(size_t page_num) {
    // clang-format off
    if (page_num == 0 || page_num > std::numeric_limits<uint32_t>::max()) AM_UNLIKELY {
        return nullptr;
    }

    while (true) {
        // Oversized Spans bypass page-count buckets but retain pooled metadata.
        if (page_num > PageConfig::MAX_PAGE_NUM) AM_UNLIKELY {
            void* ptr = PageAllocator::SystemAlloc(page_num);
            if (!ptr) {
                return nullptr;
            }

            Span* span = nullptr;
            try {
                span = span_pool_.New(detail::PtrToPageId(ptr), static_cast<uint32_t>(page_num));
            } catch (const std::bad_alloc&) {
                PageAllocator::SystemFree(ptr, page_num);
                return nullptr;
            }
            span->SetUsed(true);
            span->SetCommitted(true);
            span->owner_shard_id = 0;

            PageMap::SetSpan(span);
            return span;
        }
        // clang-format on

        AM_DCHECK(page_num >= 1 && page_num <= PageConfig::MAX_PAGE_NUM);
        if (!span_lists_[page_num].empty()) {
            auto* span = span_lists_[page_num].pop_front();
            span->SetUsed(true);
            span->SetCommitted(true);
            return span;
        }

        // Split the first available larger bucket when no exact match exists.
        for (size_t i = page_num + 1; i <= PageConfig::MAX_PAGE_NUM; ++i) {
            if (span_lists_[i].empty()) {
                continue;
            }

            auto* big_span = span_lists_[i].pop_front();
            AM_DCHECK(big_span != nullptr);
            AM_DCHECK(big_span->page_num == i);
            AM_DCHECK(!big_span->IsUsed());
            Span* small_span = nullptr;
            try {
                small_span = span_pool_.New(big_span->start_page_idx, static_cast<uint32_t>(page_num));
            } catch (const std::bad_alloc&) {
                // Preserve the original free Span if metadata allocation fails.
                span_lists_[i].push_front(big_span);
                return nullptr;
            }
            small_span->SetUsed(true);
            small_span->SetCommitted(true);
            small_span->owner_shard_id = big_span->owner_shard_id;

            big_span->start_page_idx += page_num;
            big_span->page_num -= static_cast<uint32_t>(page_num);
            big_span->SetUsed(false);
            big_span->SetCommitted(true);
            big_span->last_used_time_ms = GetCurrentTimeMs();
            span_lists_[big_span->page_num].push_front(big_span);

            PageMap::SetSpan(small_span);
            PageMap::SetSpan(big_span);
            return small_span;
        }

        // Refill at the largest cacheable Span size to amortize system allocation.
        size_t alloc_page_nums = PageConfig::MAX_PAGE_NUM;
        void* ptr = PageAllocator::SystemAlloc(alloc_page_nums);
        if (!ptr) {
            return nullptr;
        }

        Span* span = nullptr;
        try {
            span = span_pool_.New(detail::PtrToPageId(ptr), static_cast<uint32_t>(alloc_page_nums));
        } catch (const std::bad_alloc&) {
            PageAllocator::SystemFree(ptr, alloc_page_nums);
            return nullptr;
        }
        span->SetUsed(false);
        span->SetCommitted(true);
        span->last_used_time_ms = GetCurrentTimeMs();
        span->owner_shard_id = 0;
        span_lists_[alloc_page_nums].push_front(span);
        PageMap::SetSpan(span);
    }
}

void PageCacheShard::ReleaseSpanLocked(Span* span) noexcept {
    AM_DCHECK(span != nullptr);
    // Oversized Spans are never retained in page-count buckets.
    // clang-format off
    if (span->page_num > PageConfig::MAX_PAGE_NUM) AM_UNLIKELY {
        auto* ptr = span->GetPageBaseAddr();
        PageMap::ClearRange(span->start_page_idx, span->page_num);
        PageAllocator::SystemFree(ptr, span->page_num);
        span_pool_.Delete(span);
        return;
    }
    // clang-format on

    // Coalescing never crosses an owner-shard boundary.
    while (true) {
        if (span->start_page_idx == 0) {
            break;
        }
        size_t left_id = span->start_page_idx - 1;
        auto* left_span = PageMap::GetSpan(left_id);
        if (!left_span || left_span->IsUsed() ||
            left_span->owner_shard_id != span->owner_shard_id ||
            span->page_num + left_span->page_num > PageConfig::MAX_PAGE_NUM) {
            break;
        }

        span_lists_[left_span->page_num].erase(left_span);
        span->start_page_idx = left_span->start_page_idx;
        span->page_num += left_span->page_num;
        span_pool_.Delete(left_span);
    }

    while (true) {
        size_t right_id = span->start_page_idx + span->page_num;
        auto* right_span = PageMap::GetSpan(right_id);
        if (!right_span || right_span->IsUsed() ||
            right_span->owner_shard_id != span->owner_shard_id ||
            span->page_num + right_span->page_num > PageConfig::MAX_PAGE_NUM) {
            break;
        }

        span_lists_[right_span->page_num].erase(right_span);
        span->page_num += right_span->page_num;
        span_pool_.Delete(right_span);
    }

    span->SetUsed(false);
    span->SetCommitted(true);
    span->aligned_obj_size = 0;
    span->use_count = 0;
    span->obj_offset = 0;
    span->capacity = 0;
    span->last_used_time_ms = GetCurrentTimeMs();

    span_lists_[span->page_num].push_front(span);
    // Rewrite every mapping because coalescing invalidated neighbor metadata.
    PageMap::SetSpan(span);
}

void PageCacheShard::ResetLocked() {
    for (auto& list: span_lists_) {
        while (!list.empty()) {
            auto* span = list.pop_front();
            AM_DCHECK(span != nullptr);
            PageAllocator::SystemFree(span->GetPageBaseAddr(), span->page_num);
            span_pool_.Delete(span);
        }
    }
    span_pool_.ReleaseMemory();
    PageMap::Reset();
}

#ifdef USE_PAGECACHE_SHARD
Span* PageCache::AllocSpan(size_t page_num) {
    const uint16_t shard_id = SelectShardForAlloc(page_num);
    auto& shard = GetShard(shard_id);
    std::lock_guard<std::mutex> lock(shard.GetMutex());
    auto* span = shard.AllocSpanLocked(page_num);
    if (span) {
        span->owner_shard_id = shard_id;
    }
    return span;
}

void PageCache::ReleaseSpan(Span* span) noexcept {
    AM_DCHECK(span != nullptr);
    auto& shard = OwnerShard(span);
    std::lock_guard<std::mutex> lock(shard.GetMutex());
    shard.ReleaseSpanLocked(span);
}

void PageCache::Reset() {
    for (uint16_t i = 0; i < active_shard_count_; ++i) {
        auto& shard = shards_[i];
        std::lock_guard<std::mutex> lock(shard.GetMutex());
        shard.ResetLocked();
    }
    PageMap::Reset();
}
#else

Span* PageCache::AllocSpanLocked(size_t page_num) {
    if (page_num > std::numeric_limits<uint32_t>::max()) {
        return nullptr;
    }

    while (true) {
        // Oversized Spans bypass page-count buckets but retain pooled metadata.
        // clang-format off
        if (page_num > PageConfig::MAX_PAGE_NUM) AM_UNLIKELY {
            void* ptr = PageAllocator::SystemAlloc(page_num);
            if (!ptr) {
                return nullptr;
            }

            Span* span = nullptr;
            try {
                span = span_pool_.New(detail::PtrToPageId(ptr), page_num);
            } catch (const std::bad_alloc&) {
                PageAllocator::SystemFree(ptr, page_num);
                return nullptr;
            }
            span->SetUsed(true);
            span->SetCommitted(true);
            span->owner_shard_id = 0;

            PageMap::SetSpan(span);
            return span;
        }
        // clang-format on

        AM_DCHECK(page_num >= 1 && page_num <= PageConfig::MAX_PAGE_NUM);
        if (!span_lists_[page_num].empty()) {
            auto* span = span_lists_[page_num].pop_front();
            span->SetUsed(true);
            span->SetCommitted(true);
            return span;
        }

        // Split the first available larger bucket when no exact match exists.
        for (size_t i = page_num + 1; i <= PageConfig::MAX_PAGE_NUM; ++i) {
            if (span_lists_[i].empty()) {
                continue;
            }

            auto* big_span = span_lists_[i].pop_front();
            Span* small_span = nullptr;
            try {
                small_span = span_pool_.New(big_span->start_page_idx, page_num);
            } catch (const std::bad_alloc&) {
                // Preserve the original free Span if metadata allocation fails.
                span_lists_[i].push_front(big_span);
                return nullptr;
            }
            small_span->SetUsed(true);
            small_span->SetCommitted(true);

            big_span->start_page_idx += page_num;
            big_span->page_num -= page_num;
            big_span->SetUsed(false);
            big_span->last_used_time_ms = GetCurrentTimeMs();
            span_lists_[big_span->page_num].push_front(big_span);

            PageMap::SetSpan(small_span);
            PageMap::SetSpan(big_span);
            return small_span;
        }

        // Refill at the largest cacheable Span size to amortize system allocation.
        size_t alloc_page_nums = PageConfig::MAX_PAGE_NUM;
        void* ptr = PageAllocator::SystemAlloc(alloc_page_nums);
        if (!ptr) {
            return nullptr;
        }

        Span* span = nullptr;
        try {
            span = span_pool_.New(reinterpret_cast<uintptr_t>(ptr) >> SystemConfig::PAGE_SHIFT,
                                  alloc_page_nums);
        } catch (const std::bad_alloc&) {
            PageAllocator::SystemFree(ptr, alloc_page_nums);
            return nullptr;
        }
        span->SetUsed(false);
        span->SetCommitted(true);
        span->last_used_time_ms = GetCurrentTimeMs();
        span_lists_[alloc_page_nums].push_front(span);
        PageMap::SetSpan(span);
    }
}

void PageCache::ReleaseSpan(Span* span) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    // Oversized Spans are never retained in page-count buckets.
    // clang-format off
    if (span->page_num > PageConfig::MAX_PAGE_NUM) AM_UNLIKELY {
        auto* ptr = span->GetPageBaseAddr();
        PageMap::ClearRange(span->start_page_idx, span->page_num);
        PageAllocator::SystemFree(ptr, span->page_num);
        span_pool_.Delete(span);
        return;
    }
    // clang-format on

    while (true) {
        if (span->start_page_idx == 0) {
            break;
        }
        size_t left_id = span->start_page_idx - 1;
        auto* left_span = PageMap::GetSpan(left_id);
        if (!left_span || left_span->IsUsed() ||
            span->page_num + left_span->page_num > PageConfig::MAX_PAGE_NUM) {
            break;
        }

        span_lists_[left_span->page_num].erase(left_span);
        span->start_page_idx = left_span->start_page_idx;
        span->page_num += left_span->page_num;
        // Poison metadata before recycling it so debug checks catch stale use.
        left_span->start_page_idx = std::numeric_limits<size_t>::max();
        left_span->page_num = 0;
        left_span->SetUsed(true);
        span_pool_.Delete(left_span);
    }

    while (true) {
        size_t right_id = span->start_page_idx + span->page_num;
        auto* right_span = PageMap::GetSpan(right_id);
        if (!right_span || right_span->IsUsed() ||
            span->page_num + right_span->page_num > PageConfig::MAX_PAGE_NUM) {
            break;
        }

        span_lists_[right_span->page_num].erase(right_span);
        span->page_num += right_span->page_num;
        // Poison metadata before recycling it so debug checks catch stale use.
        right_span->start_page_idx = std::numeric_limits<size_t>::max();
        right_span->page_num = 0;
        right_span->SetUsed(true);
        span_pool_.Delete(right_span);
    }

    span->SetUsed(false);
    span->obj_size = 0;
    span->last_used_time_ms = GetCurrentTimeMs();
    span->SetCommitted(true);
    span_lists_[span->page_num].push_front(span);
    // Rewrite every mapping because coalescing invalidated neighbor metadata.
    PageMap::SetSpan(span);
}

void PageCache::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& list: span_lists_) {
        while (!list.empty()) {
            auto* span = list.pop_front();
            PageAllocator::SystemFree(span->GetPageBaseAddr(), span->page_num);
            span_pool_.Delete(span);
        }
    }

    span_pool_.ReleaseMemory();
    PageMap::Reset();
}


#endif


}// namespace ammalloc
