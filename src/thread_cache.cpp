/// @file thread_cache.cpp
/// @brief ThreadCache slow-path implementation: refill, trim, and quota policy.
///
/// The hot path is intentionally tiny: one TLS FreeList pop/push plus a quota
/// check. More expensive policy decisions are deferred to the cold refill/trim
/// helpers so the common case stays branch-light and lock-free.
#include "ammalloc/thread_cache.h"
#include "ammalloc/central_cache.h"

#include <algorithm>

namespace ammalloc {

namespace {

ThreadCacheStats g_thread_cache_stats;

AM_NODISCARD size_t TrimTargetForMode(ThreadCacheTrimMode mode) noexcept {
    return mode == ThreadCacheTrimMode::kRelease ? 0 : ThreadCache::kDefaultTrimTargetBytes;
}

}// namespace

const ThreadCacheStats& GetThreadCacheStats() noexcept {
    return g_thread_cache_stats;
}

namespace quota_policy {

size_t NextAfterRefill(size_t cur_max_size, size_t batch) noexcept {
    if (cur_max_size < batch) {
        // Exponential warmup until one batch.
        return std::min(batch, cur_max_size + std::max<size_t>(1, cur_max_size));
    }

    if (cur_max_size < batch * kMaxQuotaBatches) {
        // Linear growth above one batch, bounded at kMaxQuotaBatches batches.
        return std::min(batch * kMaxQuotaBatches,
                        cur_max_size + std::max<size_t>(1, batch / kMaxQuotaBatches));
    }
    return cur_max_size;
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

ThreadCache::ThreadCache() noexcept
    : ThreadCache(kDefaultCacheBudgetBytes) {}

ThreadCache::ThreadCache(size_t cache_budget_bytes) noexcept
    : reserved_quota_bytes_(InitialReservedQuotaBytes()),
      cache_budget_bytes_(std::max(cache_budget_bytes, InitialReservedQuotaBytes())),
      observed_trim_epoch_(trim_request_.load(std::memory_order_relaxed) >> 1) {
    g_thread_cache_stats.reserved_quota_bytes.fetch_add(reserved_quota_bytes_,
                                                        std::memory_order_relaxed);
}

ThreadCache::~ThreadCache() noexcept {
#ifndef NDEBUG
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        AM_DCHECK(free_lists_[i].empty(),
                  "ThreadCache destroyed with cached objects in class {}", i);
    }
#endif

    g_thread_cache_stats.reserved_quota_bytes.fetch_sub(reserved_quota_bytes_,
                                                        std::memory_order_relaxed);
}

size_t ThreadCache::CachedBytesSnapshot() const noexcept {
    size_t total = 0;
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        total += free_lists_[i].size() * SizeClass::Size(i);
    }
    return total;
}

bool ThreadCache::CanSetQuota(size_t idx, size_t next_max_size) const noexcept {
    // A shrink/no-op transition is always admissible: it releases
    // reservation and must never be blocked, or decay/Trim could not
    // hand budget back to the shared pool.
    const auto cur_max_size = free_lists_[idx].max_size();
    if (next_max_size <= cur_max_size) {
        return true;
    }

    // Growth consumes budget: approve only when the byte delta fits
    // the headroom (budget minus reserved quota).

    const auto delta = (next_max_size - cur_max_size) * SizeClass::Size(idx);
    return delta <= cache_budget_bytes_ - reserved_quota_bytes_;
}

void ThreadCache::SetQuota(size_t idx, size_t new_max_size) noexcept {
    auto& list = free_lists_[idx];
    const auto cur_max_size = list.max_size();
    if (cur_max_size == new_max_size) {
        return;
    }

    const auto class_size = SizeClass::Size(idx);
    if (new_max_size > cur_max_size) {
        const size_t delta = (new_max_size - cur_max_size) * class_size;
        reserved_quota_bytes_ += delta;
        g_thread_cache_stats.reserved_quota_bytes.fetch_add(delta,
                                                            std::memory_order_relaxed);
    } else {
        const size_t delta = (cur_max_size - new_max_size) * class_size;
        reserved_quota_bytes_ -= delta;
        g_thread_cache_stats.reserved_quota_bytes.fetch_sub(delta,
                                                            std::memory_order_relaxed);
    }
    list.set_max_size(new_max_size);
}

void ThreadCache::RequestGlobalTrim(ThreadCacheTrimMode mode) noexcept {
    uint64_t observed = trim_request_.load(std::memory_order_relaxed);
    while (true) {
        const uint64_t next_epoch = (observed >> 1) + 1;
        const uint64_t desired = (next_epoch << 1) | static_cast<uint8_t>(mode);
        if (trim_request_.compare_exchange_weak(observed, desired,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
            return;
        }
    }
}

void ThreadCache::ObserveGlobalTrimRequest() noexcept {
    const uint64_t request = trim_request_.load(std::memory_order_acquire);
    const uint64_t epoch = request >> 1;
    if (epoch == observed_trim_epoch_) {
        return;
    }

    const auto mode = static_cast<ThreadCacheTrimMode>(request & 1);
    Trim(mode, TrimTargetForMode(mode));
    observed_trim_epoch_ = epoch;
}

void ThreadCache::ReleaseAll() noexcept {
    for (size_t i = 0; i < SizeClass::kNumSizeClasses; ++i) {
        auto& list = free_lists_[i];
        if (!list.empty()) {
            const auto size = SizeClass::Size(i);
            const auto chain = list.pop_range(list.size());

            // Thread exit may occur in bursts. Preserve the ordinary bounded
            // reuse path instead of turning every TLS destructor into a hard
            // bitmap/PageCache purge; callers that require RSS reclamation use
            // the explicit owner-thread purge API before worker teardown.
            CentralCache::GetInstance().ReleaseListToSpans(chain.head, size);
        }
        SetQuota(i, 1);
        list.set_overages(0);
    }
}

void ThreadCache::Trim(ThreadCacheTrimMode mode, size_t target_bytes) noexcept {
    size_t cached_bytes = CachedBytesSnapshot();
    g_thread_cache_stats.trim_count.fetch_add(1, std::memory_order_relaxed);
    g_thread_cache_stats.cached_bytes_observed_at_trim.fetch_add(
            cached_bytes, std::memory_order_relaxed);

    for (size_t i = SizeClass::kNumSizeClasses; i > 0; --i) {
        const size_t idx = i - 1;
        auto& list = free_lists_[idx];
        const size_t class_size = SizeClass::Size(idx);
        const size_t batch_num = SizeClass::CalculateBatchSize(class_size);

        size_t evictable = list.size();
        if (mode == ThreadCacheTrimMode::kReuse) {
            // A soft trim preserves one existing batch at most. It never grows
            // a cold class merely to establish that warm floor.
            evictable -= std::min(evictable, batch_num);
        }

        size_t evict_count = 0;
        if (cached_bytes > target_bytes && evictable > 0) {
            const size_t excess = cached_bytes - target_bytes;
            const size_t requested = excess / class_size + (excess % class_size != 0);
            evict_count = std::min(evictable, requested);
        }

        if (evict_count > 0) {
            const auto chain = list.pop_range_tail(evict_count);
            const size_t evicted_bytes = chain.count * class_size;
            cached_bytes -= evicted_bytes;
            g_thread_cache_stats.trimmed_bytes.fetch_add(evicted_bytes,
                                                         std::memory_order_relaxed);
            CentralCache::GetInstance().ReleaseListToSpans(
                    chain.head, class_size,
                    mode == ThreadCacheTrimMode::kReuse
                            ? CentralReleaseMode::kTransferCache
                            : CentralReleaseMode::kSpanBitmap);
        }

        if (mode == ThreadCacheTrimMode::kRelease) {
            // A nonzero caller target may intentionally leave objects local;
            // trim is a contraction operation and must never bypass the
            // aggregate-budget gate by growing quota around a transient
            // max_size + 1 push. A caller that leaves such an overage relies on
            // the surrounding DeallocateSlowPath (or the next overflow) to
            // evict it.
            const size_t contracted = std::max<size_t>(1, list.size());
            SetQuota(idx, std::min(list.max_size(), contracted));
            list.set_overages(0);
        } else if (list.max_size() > batch_num) {
            // A soft trim retracts unused burst-era quota without introducing
            // a new per-class minimum above the current quota. In particular,
            // observing a trim request after Deallocate pushed max_size + 1
            // objects must not turn that transient overage into quota growth.
            const size_t contracted = std::max(batch_num, list.size());
            SetQuota(idx, std::min(list.max_size(), contracted));
            list.set_overages(0);
        }
    }
}

void* ThreadCache::FetchFromCentralCache(size_t idx) noexcept {
    ObserveGlobalTrimRequest();
    auto& list = free_lists_[idx];
    const auto aligned_size = SizeClass::Size(idx);
    const auto batch_num = SizeClass::CalculateBatchSize(aligned_size);

    // Refill only up to the current local quota. Slow-start intentionally keeps
    // early refills small so cold size classes do not immediately hoard a full
    // batch in every thread.
    const auto fetch_num = std::min(batch_num, list.max_size());
    const size_t fetched = CentralCache::GetInstance().FetchRange(list, fetch_num, aligned_size);
    if (fetched == 0) {
        return nullptr;// CentralCache exhausted for this size class.
    }

    // A partial refill signals memory pressure: hold the quota and the decay
    // signal so pressure does not feed back into larger subsequent requests.
    if (fetched < fetch_num) {
        return list.pop();
    }

    // Fresh allocation demand cancels any prior decay trend for this class.
    if (const auto next_max_size = quota_policy::NextAfterRefill(list.max_size(), batch_num);
        CanSetQuota(idx, next_max_size)) {
        SetQuota(idx, next_max_size);
    } else {
        g_thread_cache_stats.budget_denied_growth.fetch_add(1, std::memory_order_relaxed);
    }

    list.set_overages(0);
    return list.pop();
}

void ThreadCache::DeallocateSlowPath(size_t idx) noexcept {
    ObserveGlobalTrimRequest();
    auto& list = free_lists_[idx];
    const auto aligned_size = SizeClass::Size(idx);
    if (list.size() <= list.max_size()) {
        return;
    }

    const auto batch_num = SizeClass::CalculateBatchSize(aligned_size);

    // Return at most one batch per overflow event, evicting the oldest objects
    // first so recently freed ones stay local for reuse. This bounds slow-path
    // work and avoids draining the local cache completely on every trim.
    if (const auto chain = list.pop_range_tail(batch_num); chain.head) {
        CentralCache::GetInstance().ReleaseListToSpans(chain.head, aligned_size);
    }

    // Repeated overflow without intervening refill demand decays the quota
    // (see quota_policy::NextAfterOverflow), so burst-era caches do not stick.
    const auto next =
            quota_policy::NextAfterOverflow(list.max_size(), batch_num, list.overages());
    SetQuota(idx, next.max_size);
    list.set_overages(next.overages);
}

}// namespace ammalloc
