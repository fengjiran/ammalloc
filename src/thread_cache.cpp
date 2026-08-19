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

void ThreadCache::ReleaseAll() {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        auto& list = free_lists_[i];
        if (list.empty()) {
            continue;
        }

        const auto size = SizeClass::Size(i);
        void* start = nullptr;
        const auto cnt = list.size();
        for (size_t j = 0; j < cnt; ++j) {
            void* ptr = list.pop();
            static_cast<FreeBlock*>(ptr)->next = static_cast<FreeBlock*>(start);
            start = ptr;
        }

        // Drain the entire per-class cache during teardown so ownership returns
        // to CentralCache/PageCache before the TLS object disappears.
        CentralCache::GetInstance().ReleaseListToSpans(start, size);
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

    // Two-stage growth policy:
    // - below one batch: exponential warmup reduces refill churn quickly,
    // - above one batch: linear growth keeps the high-water mark bounded.
    if (list.max_size() < batch_num) {
        const auto inc = std::max<size_t>(1, list.max_size());
        list.set_max_size(std::min(batch_num, list.max_size() + inc));
    } else if (list.max_size() < batch_num * 8) {
        size_t inc = std::max<size_t>(1, batch_num / 8);
        list.set_max_size(std::min(batch_num * 8, list.max_size() + inc));
    }

    // Fresh allocation demand cancels any prior decay trend for this class.
    list.set_overages(0);
    return list.pop();
}

void ThreadCache::DeallocateSlowPath(FreeList& list, size_t aligned_size) noexcept {
    const auto batch_num = SizeClass::CalculateBatchSize(aligned_size);
    void* start = nullptr;

    // Return at most one batch per overflow event. This bounds slow-path work
    // and avoids draining the local cache completely on every trim.
    for (size_t i = 0; i < batch_num && !list.empty(); ++i) {
        void* ptr = list.pop();
        static_cast<FreeBlock*>(ptr)->next = static_cast<FreeBlock*>(start);
        start = ptr;
    }

    if (start) {
        CentralCache::GetInstance().ReleaseListToSpans(start, aligned_size);
    }

    // Repeated overflow without intervening refill demand indicates that the
    // current high-water mark is larger than the thread's steady-state need.
    // After a few such events, decay the quota by one batch and reset the
    // counter. This keeps burst-era cache size from sticking forever.
    constexpr size_t kMaxOverages = 3;
    if (list.max_size() > batch_num) {
        list.set_overages(list.overages() + 1);
        if (list.overages() >= kMaxOverages) {
            list.set_max_size(std::max(list.max_size() - batch_num, batch_num));
            list.set_overages(0);
        }
    } else {
        // Once the quota is back at its batch-sized floor, no additional decay
        // state is needed.
        list.set_overages(0);
    }
}

}// namespace ammalloc
