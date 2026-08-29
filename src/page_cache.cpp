#include "ammalloc/page_cache.h"

#include <limits>

namespace ammalloc {

uint64_t GetCurrentTimeMs() noexcept {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

Span* PageMap::GetSpan(size_t page_id) noexcept {
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

bool PageMap::HasPath(size_t page_id) noexcept {
    auto* root = root_.load(std::memory_order_acquire);
    if (!root) {
        return false;
    }

    const size_t i0 = page_id >> (PageConfig::RADIX_NODE_BITS * 3);
    if (i0 >= PageConfig::RADIX_ROOT_SIZE) {
        return false;
    }
    const size_t i1 = (page_id >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
    const size_t i2 = (page_id >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;
    auto* p1 = static_cast<RadixNode*>(root->children[i0].load(std::memory_order_acquire));
    if (!p1) {
        return false;
    }
    auto* p2 = static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_acquire));
    if (!p2) {
        return false;
    }
    return p2->children[i2].load(std::memory_order_acquire) != nullptr;
}

RadixNode* PageMap::TryNewRadixNode() noexcept {
#ifdef AMMALLOC_TEST
    size_t remaining = radix_allocations_before_failure_.load(std::memory_order_relaxed);
    while (remaining != std::numeric_limits<size_t>::max()) {
        if (remaining == 0) {
            return nullptr;
        }
        if (radix_allocations_before_failure_.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
            break;
        }
    }
#endif
    return radix_node_pool_.TryNew();
}

bool PageMap::EnsurePathLocked(size_t page_id) noexcept {
    const size_t i0 = page_id >> (PageConfig::RADIX_NODE_BITS * 3);
    if (i0 >= PageConfig::RADIX_ROOT_SIZE) {
        return false;
    }
    const size_t i1 = (page_id >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
    const size_t i2 = (page_id >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;

    auto* root = root_.load(std::memory_order_relaxed);
    if (!root) {
        root = &root_storage_;
        for (auto& child: root->children) {
            child.store(nullptr, std::memory_order_relaxed);
        }
        root_.store(root, std::memory_order_release);
    }

    auto* p1 = static_cast<RadixNode*>(root->children[i0].load(std::memory_order_relaxed));
    if (!p1) {
        p1 = TryNewRadixNode();
        if (!p1) {
            return false;
        }
        root->children[i0].store(p1, std::memory_order_release);
    }

    auto* p2 = static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_relaxed));
    if (!p2) {
        p2 = TryNewRadixNode();
        if (!p2) {
            return false;
        }
        p1->children[i1].store(p2, std::memory_order_release);
    }

    auto* p3 = static_cast<RadixNode*>(p2->children[i2].load(std::memory_order_relaxed));
    if (!p3) {
        p3 = TryNewRadixNode();
        if (!p3) {
            return false;
        }
        p2->children[i2].store(p3, std::memory_order_release);
    }
    return true;
}

bool PageMap::EnsureRange(size_t start_page_id, size_t page_num) noexcept {
    if (page_num == 0) {
        return true;
    }
    if (page_num > 1 &&
        start_page_id > std::numeric_limits<size_t>::max() - (page_num - 1)) {
        return false;
    }

    size_t current = start_page_id;
    size_t remaining = page_num;
    while (remaining > 0) {
        const size_t i0 = current >> (PageConfig::RADIX_NODE_BITS * 3);
        if (i0 >= PageConfig::RADIX_ROOT_SIZE) {
            return false;
        }
        const size_t i3 = current & PageConfig::RADIX_MASK;
        const size_t step = std::min(remaining, PageConfig::RADIX_NODE_SIZE - i3);
        if (!HasPath(current)) {
            detail::NoThrowLockGuard lock(structure_mutex_);
            if (!EnsurePathLocked(current)) {
                return false;
            }
        }
        current += step;
        remaining -= step;
    }
    return true;
}

void PageMap::SetSpan(Span* span) noexcept {
    if (!span || span->page_num == 0) {
        detail::FatalNoAlloc("PageMap::SetSpan received invalid Span");
    }

    auto* root = root_.load(std::memory_order_acquire);
    if (!root) {
        detail::FatalNoAlloc("PageMap::SetSpan path was not prepared");
    }

    size_t current = span->start_page_idx;
    size_t remaining = span->page_num;
    while (remaining > 0) {
        const size_t i0 = current >> (PageConfig::RADIX_NODE_BITS * 3);
        if (i0 >= PageConfig::RADIX_ROOT_SIZE) {
            detail::FatalNoAlloc("PageMap::SetSpan page ID is out of range");
        }
        const size_t i1 = (current >> (PageConfig::RADIX_NODE_BITS * 2)) & PageConfig::RADIX_MASK;
        const size_t i2 = (current >> PageConfig::RADIX_NODE_BITS) & PageConfig::RADIX_MASK;
        const size_t i3 = current & PageConfig::RADIX_MASK;
        auto* p1 = static_cast<RadixNode*>(root->children[i0].load(std::memory_order_acquire));
        auto* p2 = p1 ? static_cast<RadixNode*>(p1->children[i1].load(std::memory_order_acquire))
                      : nullptr;
        auto* p3 = p2 ? static_cast<RadixNode*>(p2->children[i2].load(std::memory_order_acquire))
                      : nullptr;
        if (!p3) {
            detail::FatalNoAlloc("PageMap::SetSpan path was not prepared");
        }

        const size_t count = std::min(remaining, PageConfig::RADIX_NODE_SIZE - i3);
        for (size_t i = 0; i < count; ++i) {
            p3->children[i3 + i].store(span, std::memory_order_release);
        }
        current += count;
        remaining -= count;
    }
}

void PageMap::ClearRange(size_t start_page_id, size_t page_num) noexcept {
    auto* curr = root_.load(std::memory_order_relaxed);
    if (!curr) {
        return;
    }

    auto cur_page_id = start_page_id;
    auto remaining_pages = page_num;

    while (remaining_pages > 0) {
        const size_t i0 = cur_page_id >> (PageConfig::RADIX_NODE_BITS * 3);
        if (i0 >= PageConfig::RADIX_ROOT_SIZE) {
            return;
        }
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

void PageMap::Reset() noexcept {
    detail::NoThrowLockGuard lock(structure_mutex_);
    root_.store(nullptr, std::memory_order_relaxed);
    radix_node_pool_.ReleaseMemory();
#ifdef AMMALLOC_TEST
    radix_allocations_before_failure_.store(std::numeric_limits<size_t>::max(),
                                            std::memory_order_relaxed);
#endif
}

Span* PageCacheShard::AllocSpanLocked(size_t page_num) noexcept {
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

            Span* span = span_pool_.TryNew(detail::PtrToPageId(ptr), static_cast<uint32_t>(page_num));
            if (!span) {
                PageAllocator::SystemFree(ptr, page_num);
                return nullptr;
            }
            if (!PageMap::EnsureRange(span->start_page_idx, span->page_num)) {
                span_pool_.Delete(span);
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
            Span* small_span =
                    span_pool_.TryNew(big_span->start_page_idx, static_cast<uint32_t>(page_num));
            if (!small_span) {
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

        Span* span = span_pool_.TryNew(detail::PtrToPageId(ptr),
                                       static_cast<uint32_t>(alloc_page_nums));
        if (!span) {
            PageAllocator::SystemFree(ptr, alloc_page_nums);
            return nullptr;
        }
        if (!PageMap::EnsureRange(span->start_page_idx, span->page_num)) {
            span_pool_.Delete(span);
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

    // Coalescing never crosses an owner-shard boundary. Keep absorbed
    // descriptors alive until their PageMap leaves point at the survivor.
    Span* retired_spans = nullptr;
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
        left_span->next = retired_spans;
        retired_spans = left_span;
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
        right_span->next = retired_spans;
        retired_spans = right_span;
    }

    span->SetUsed(false);
    span->SetCommitted(true);
    span->aligned_obj_size = 0;
    span->use_count = 0;
    span->obj_offset = 0;
    span->capacity = 0;
    span->last_used_time_ms = GetCurrentTimeMs();

    // Rewrite every mapping because coalescing invalidated neighbor metadata.
    PageMap::SetSpan(span);
    while (retired_spans) {
        auto* next_span = retired_spans->next;
        span_pool_.Delete(retired_spans);
        retired_spans = next_span;
    }
    span_lists_[span->page_num].push_front(span);
}

void PageCacheShard::ResetLocked() noexcept {
    for (auto& list: span_lists_) {
        while (!list.empty()) {
            auto* span = list.pop_front();
            AM_DCHECK(span != nullptr);
            PageAllocator::SystemFree(span->GetPageBaseAddr(), span->page_num);
            span_pool_.Delete(span);
        }
    }
    span_pool_.ReleaseMemory();
}

#ifdef USE_PAGECACHE_SHARD
Span* PageCache::AllocSpan(size_t page_num) noexcept {
    const uint16_t shard_id = SelectShardForAlloc(page_num);
    auto& shard = GetShard(shard_id);
    detail::NoThrowLockGuard lock(shard.GetMutex());
    auto* span = shard.AllocSpanLocked(page_num);
    if (span) {
        span->owner_shard_id = shard_id;
    }
    return span;
}

void PageCache::ReleaseSpan(Span* span) noexcept {
    AM_DCHECK(span != nullptr);
    auto& shard = OwnerShard(span);
    detail::NoThrowLockGuard lock(shard.GetMutex());
    shard.ReleaseSpanLocked(span);
}

void PageCache::Reset() noexcept {
    for (uint16_t i = 0; i < active_shard_count_; ++i) {
        auto& shard = shards_[i];
        detail::NoThrowLockGuard lock(shard.GetMutex());
        shard.ResetLocked();
    }
    PageMap::Reset();
}
#else

Span* PageCache::AllocSpanLocked(size_t page_num) noexcept {
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

            Span* span = span_pool_.TryNew(detail::PtrToPageId(ptr),
                                           static_cast<uint32_t>(page_num));
            if (!span) {
                PageAllocator::SystemFree(ptr, page_num);
                return nullptr;
            }
            if (!PageMap::EnsureRange(span->start_page_idx, span->page_num)) {
                span_pool_.Delete(span);
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
            Span* small_span = span_pool_.TryNew(big_span->start_page_idx,
                                                 static_cast<uint32_t>(page_num));
            if (!small_span) {
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

        Span* span = span_pool_.TryNew(detail::PtrToPageId(ptr),
                                       static_cast<uint32_t>(alloc_page_nums));
        if (!span) {
            PageAllocator::SystemFree(ptr, alloc_page_nums);
            return nullptr;
        }
        if (!PageMap::EnsureRange(span->start_page_idx, span->page_num)) {
            span_pool_.Delete(span);
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
    detail::NoThrowLockGuard lock(mutex_);

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

    // Keep absorbed descriptors alive until their PageMap leaves point at the
    // survivor, matching the sharded implementation above.
    Span* retired_spans = nullptr;
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
        left_span->next = retired_spans;
        retired_spans = left_span;
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
        right_span->next = retired_spans;
        retired_spans = right_span;
    }

    span->SetUsed(false);
    span->aligned_obj_size = 0;
    span->use_count = 0;
    span->obj_offset = 0;
    span->capacity = 0;
    span->last_used_time_ms = GetCurrentTimeMs();
    span->SetCommitted(true);
    // Rewrite every mapping because coalescing invalidated neighbor metadata.
    PageMap::SetSpan(span);
    while (retired_spans) {
        auto* next_span = retired_spans->next;
        span_pool_.Delete(retired_spans);
        retired_spans = next_span;
    }
    span_lists_[span->page_num].push_front(span);
}

void PageCache::Reset() noexcept {
    detail::NoThrowLockGuard lock(mutex_);
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
