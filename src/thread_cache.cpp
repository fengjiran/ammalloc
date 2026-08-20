// Copyright 2026 The AetherMind Authors
// SPDX-License-Identifier: Apache-2.0
//
// ThreadCache implementation.
//
// The hot path is intentionally tiny: one TLS FreeList pop/push plus a quota
// check. More expensive policy decisions are deferred to the cold refill/trim
// helpers below so the common case stays branch-light and lock-free.
#include "ammalloc/thread_cache.h"

#include "ammalloc/central_cache.h"

namespace ammalloc {

namespace quota_policy {

size_t NextAfterRefill(size_t current, size_t batch) noexcept {
    if (current < batch) {
        // Exponential warmup until one batch.
        return std::min(batch, current + std::max<size_t>(1, current));
    }
    if (current < batch * 8) {
        // Linear growth above one batch, bounded at 8 batches.
        return std::min(batch * 8, current + std::max<size_t>(1, batch / 8));
    }
    return current;
}

QuotaState NextAfterOverflow(size_t current, size_t batch, size_t overages) noexcept {
    if (current <= batch) {
        // At the floor: no decay state needed.
        return {current, 0};
    }
    if (overages + 1 >= kMaxOverages) {
        // Enough consecutive overflows: decay by one batch, floor at one batch.
        return {std::max(current - batch, batch), 0};
    }
    return {current, overages + 1};
}

}// namespace quota_policy

void ThreadCache::ReleaseAll() {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        auto& list = free_lists_[i];
        if (list.empty()) {
            continue;
        }

        const auto size = SizeClass::Size(i);
        const auto chain = list.pop_range(list.size());

        // Drain the entire per-class cache during teardown so ownership returns
        // to CentralCache/PageCache before the TLS object disappears.
        CentralCache::GetInstance().ReleaseListToSpans(chain.head, size);
        list.set_max_size(1);
        list.set_overages(0);
    }
}

void* ThreadCache::FetchFromCentralCache(FreeList& list, size_t aligned_size) noexcept {
    const auto batch_num = SizeClass::CalculateBatchSize(aligned_size);

    // Refill only up to the current local quota. Slow-start intentionally keeps
    // early refills small so cold size classes do not immediately hoard a full
    // batch in every thread.
    if (const auto fetch_num = std::min(batch_num, list.max_size());
        CentralCache::GetInstance().FetchRange(list, fetch_num, aligned_size) == 0) {
        return nullptr;// Out of memory
    }

    // Fresh allocation demand cancels any prior decay trend for this class.
    list.set_max_size(quota_policy::NextAfterRefill(list.max_size(), batch_num));
    list.set_overages(0);
    return list.pop();
}

void ThreadCache::DeallocateSlowPath(FreeList& list, size_t aligned_size) noexcept {
    const auto batch_num = SizeClass::CalculateBatchSize(aligned_size);

    // Return at most one batch per overflow event. This bounds slow-path work
    // and avoids draining the local cache completely on every trim.
    const auto chain = list.pop_range(batch_num);
    if (chain.head) {
        CentralCache::GetInstance().ReleaseListToSpans(chain.head, aligned_size);
    }

    // Repeated overflow without intervening refill demand decays the quota
    // (see quota_policy::NextAfterOverflow), so burst-era caches do not stick.
    const auto next =
            quota_policy::NextAfterOverflow(list.max_size(), batch_num, list.overages());
    list.set_max_size(next.max_size);
    list.set_overages(next.overages);
}

}// namespace ammalloc
