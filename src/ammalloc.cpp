#include "ammalloc/ammalloc.h"
#include "ammalloc/central_cache.h"
#include "ammalloc/config.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/page_heap_scavenger.h"
#include "ammalloc/thread_cache.h"

#include <atomic>
#include <exception>

namespace {

using namespace ammalloc;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((tls_model("initial-exec")))
#endif
thread_local ThreadCache* pTLSThreadCache = nullptr;
thread_local bool g_ThreadCacheAlreadyDestructed = false;

ThreadCache* CreateThreadCache() noexcept {
    if (g_ThreadCacheAlreadyDestructed) {
        return nullptr;
    }

    // All guarded state is thread-local and SystemAlloc is thread-safe, so no
    // cross-thread serialization is needed here; a global mutex would only add
    // a startup contention point.
    if (pTLSThreadCache) {
        return pTLSThreadCache;
    }

    constexpr auto tc_size = sizeof(ThreadCache);
    constexpr auto page_num = (tc_size + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
    void* ptr = PageAllocator::SystemAlloc(page_num);
    if (!ptr) {
        return nullptr;
    }
    return new (ptr) ThreadCache;
}

void ReleaseThreadCache(ThreadCache* tc) noexcept {
    if (!tc) {
        return;
    }

    tc->~ThreadCache();
    constexpr auto tc_size = sizeof(ThreadCache);
    constexpr auto page_num = (tc_size + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
    PageAllocator::SystemFree(tc, page_num);
}

struct ThreadCacheCleaner {
    ~ThreadCacheCleaner() {
        g_ThreadCacheAlreadyDestructed = true;
        if (pTLSThreadCache) {
            pTLSThreadCache->ReleaseAll();
            ReleaseThreadCache(pTLSThreadCache);
            pTLSThreadCache = nullptr;
        }
    }
};

thread_local ThreadCacheCleaner tc_cleaner;

// Keep scavenger startup off the allocation fast path after the first attempt.
void EnsureScavengerStarted() noexcept {
    if (!RuntimeConfig::GetInstance().EnableScavenger()) {
        return;
    }

    static std::atomic<bool> started{false};

    // clang-format off
    if (!started.load(std::memory_order_acquire)) AM_UNLIKELY {
        bool expected = false;
        if (started.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
            try {
                PageHeapScavenger::GetInstance().Start();
            } catch (const std::exception&) {
                // Allocation remains functional without proactive RSS reclamation.
                // Keep `started` set so failure cannot create retry storms. No
                // generic logger is safe on this allocator slow path.
            } catch (...) {
                // Background reclamation is optional. Unknown failures must not
                // escape this noexcept allocation boundary either.
            }
        }
    }
    // clang-format on
}

AM_NOINLINE void* am_malloc_slow_path(size_t original_size) noexcept {
    // Defer scavenger thread creation until allocation is already on a slow path,
    // avoiding process-startup cost when ammalloc is never used.
    EnsureScavengerStarted();

    if (original_size > SizeConfig::MAX_TC_SIZE) {
        const auto aligned_size = detail::AlignUp(original_size, SystemConfig::PAGE_SIZE);
        const size_t page_num = aligned_size >> SystemConfig::PAGE_SHIFT;
        const auto* span = PageCache::GetInstance().AllocSpan(page_num);
        if (!span) {
            return nullptr;
        }

        return span->GetPageBaseAddr();
    }

    if (!pTLSThreadCache) {
        pTLSThreadCache = CreateThreadCache();
        if (!pTLSThreadCache) {
            return nullptr;
        }
    }

    return pTLSThreadCache->Allocate(original_size);
}

AM_NOINLINE void am_free_slow_path(void* ptr, Span* span, size_t aligned_size,
                                   size_t idx) noexcept {
    // Large-object Spans do not carry an object size and return directly to PageCache.
    if (aligned_size == 0) {
        AM_HCHECK(ptr == span->GetPageBaseAddr(), "Invalid large-object pointer.");
        PageCache::GetInstance().ReleaseSpan(span);
        return;
    }

    // clang-format off
    if (!pTLSThreadCache) AM_UNLIKELY {
        pTLSThreadCache = CreateThreadCache();
        if (!pTLSThreadCache) {
            static_cast<FreeBlock*>(ptr)->next = nullptr;
            CentralCache::GetInstance().ReleaseListToSpans(ptr, aligned_size);
            return;
        }
    }
    // clang-format on

    pTLSThreadCache->Deallocate(ptr, idx);
}

}// namespace

namespace ammalloc {

void* am_malloc(size_t original_size) {
    // Read TLS once so the hot path performs a single TLS lookup.
    auto* tc = pTLSThreadCache;
    // clang-format off
    if (original_size > SizeConfig::MAX_TC_SIZE || tc == nullptr) AM_UNLIKELY {
        return am_malloc_slow_path(original_size);
    }
    // clang-format on

    return tc->Allocate(original_size);
}

void am_free(void* ptr) {
    // clang-format off
    if (!ptr) AM_UNLIKELY {
        return;
    }

    auto* span = PageMap::GetSpan(ptr);
    if (!span) AM_UNLIKELY {
        return;
    }

    const auto aligned_size = span->aligned_obj_size;
    const size_t idx = span->size_class_idx;
    AM_HCHECK(aligned_size == 0 ||
                      (idx < SizeClass::kNumSizeClasses &&
                       SizeClass::Size(idx) == aligned_size),
              "Corrupted Span size-class metadata.");

    auto* tc = pTLSThreadCache;
    if (aligned_size == 0 || tc == nullptr) AM_UNLIKELY {
        am_free_slow_path(ptr, span, aligned_size, idx);
        return;
    }
    // clang-format on

    AM_HCHECK(span->ObjectSlotOf(ptr) != std::numeric_limits<size_t>::max(),
              "Invalid small-object pointer.");

    tc->Deallocate(ptr, idx);
}

}// namespace ammalloc
