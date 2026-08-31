/// @file page_allocator.cpp
/// @brief System page allocation with optimistic huge-page alignment and a lock-free 2 MiB cache.
///
/// Tries an exact mapping first, then over-allocates and trims on misalignment.
/// Released 2 MiB mappings may remain in the internal lock-free cache for reuse.

#include "ammalloc/page_allocator.h"
#include "ammalloc/assert.h"
#include "ammalloc/config.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <sys/mman.h>
#include <thread>

namespace {

bool IsPageAligned(const void* ptr) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) & (ammalloc::SystemConfig::PAGE_SIZE - 1)) == 0;
}

bool IsHugePageAligned(const void* ptr) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) & (ammalloc::SystemConfig::HUGE_PAGE_SIZE - 1)) == 0;
}

// Zero-allocation lock-free dual-stack cache for 2MB huge pages.
//
// This cache eliminates the std::mutex bottleneck under high concurrency while
// strictly adhering to ammalloc's zero-allocation bootstrapping constraints
// (no new/delete, no std::vector).
//
// It maintains two lock-free stacks (free_head_ and used_head_) over a fixed
// array of Slots. ABA problem is prevented by packing a 16-bit index and a
// 48-bit tag into a single 64-bit atomic head.
class HugePageCache {
public:
    static HugePageCache& GetInstance() {
        // Static storage avoids both allocator recursion and destruction-order hazards.
        alignas(alignof(HugePageCache)) static char storage[sizeof(HugePageCache)];
        static auto* instance = new (storage) HugePageCache();
        return *instance;
    }

    // Pops a cached 2MB huge page from the used stack.
    // Returns nullptr if the cache is empty.
    void* Get() noexcept {
        uint16_t index = kInvalid;
        if (!Pop(used_head_, index)) {
            return nullptr;
        }

        void* ptr = slots_[index].ptr;
        Push(free_head_, index);
        return ptr;
    }

    // Pushes a 2MB huge page into the cache.
    // Returns false if the cache is full, in which case the caller must munmap.
    bool Put(void* ptr) noexcept {
        uint16_t index = kInvalid;
        if (!Pop(free_head_, index)) {
            return false;
        }

        slots_[index].ptr = ptr;
        Push(used_head_, index);
        return true;
    }

    void ReleaseAllForTesting() noexcept {
        while (void* ptr = Get()) {
            if (munmap(ptr, ammalloc::SystemConfig::HUGE_PAGE_SIZE) != 0) {
                ammalloc::PageAllocator::RecordMunmapFailure();
            }
        }
    }

private:
    struct Slot {
        void* ptr{nullptr};
        std::atomic<uint16_t> next{kInvalid};
    };

    static constexpr uint16_t kInvalid = 0xFFFF;
    static constexpr size_t kCapacity = ammalloc::PageConfig::HUGE_PAGE_CACHE_SIZE;
    static_assert(kCapacity > 0 && kCapacity < kInvalid);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    static constexpr uint64_t kIndexMask = 0xFFFFull;
    static constexpr uint64_t kTagMask = (1ull << 48) - 1;

    alignas(ammalloc::SystemConfig::CACHE_LINE_SIZE) std::atomic<uint64_t> free_head_{0};
    alignas(ammalloc::SystemConfig::CACHE_LINE_SIZE) std::atomic<uint64_t> used_head_{0};
    Slot slots_[kCapacity]{};

    static constexpr uint64_t Pack(uint16_t index, uint64_t tag) noexcept {
        return ((tag & kTagMask) << 16) | index;
    }

    static constexpr uint16_t Index(uint64_t head) noexcept {
        return static_cast<uint16_t>(head & kIndexMask);
    }

    static constexpr uint64_t Tag(uint64_t head) noexcept {
        return head >> 16;
    }

    HugePageCache() noexcept {
        for (uint16_t i = 0; i < kCapacity - 1; ++i) {
            slots_[i].next.store(i + 1, std::memory_order_relaxed);
        }
        slots_[kCapacity - 1].next.store(kInvalid, std::memory_order_relaxed);
        free_head_.store(Pack(0, 0), std::memory_order_relaxed);
        used_head_.store(Pack(kInvalid, 0), std::memory_order_relaxed);
    }

    // Lock-free pop from the specified stack head.
    // Returns true and sets `out` to the popped index, or false if empty.
    bool Pop(std::atomic<uint64_t>& head, uint16_t& out) noexcept {
        uint64_t old_val = head.load(std::memory_order_acquire);
        while (true) {
            const uint16_t index = Index(old_val);
            if (index == kInvalid) {
                return false;
            }

            const uint16_t next = slots_[index].next.load(std::memory_order_relaxed);
            const uint64_t new_val = Pack(next, Tag(old_val) + 1);
            if (head.compare_exchange_weak(old_val, new_val,
                                           std::memory_order_acquire,
                                           std::memory_order_acquire)) {
                // Acquire on CAS failure ensures we re-read the updated slot
                // `.next` value published by the concurrent Push's release
                // store on retry.
                out = index;
                return true;
            }
        }
    }

    // Lock-free push of an index onto the specified stack head.
    void Push(std::atomic<uint64_t>& head, uint16_t index) noexcept {
        uint64_t old_val = head.load(std::memory_order_relaxed);
        while (true) {
            slots_[index].next.store(Index(old_val), std::memory_order_relaxed);
            const uint64_t new_val = Pack(index, Tag(old_val) + 1);
            if (head.compare_exchange_weak(old_val, new_val,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                // Release on CAS success publishes the slot's `.next` pointer
                // written above so that a subsequent Pop's acquire load
                // observes a consistent stack link.
                return;
            }
        }
    }
};

}// namespace

namespace ammalloc {

#ifdef AMMALLOC_TEST
std::atomic<bool> g_mock_huge_alloc_fail{false};
std::atomic<bool> g_mock_normal_alloc_fail{false};
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 0
#endif


void PageAllocator::ResetStats() noexcept {
    stats_.normal_alloc_count.store(0, std::memory_order_relaxed);
    stats_.normal_alloc_success.store(0, std::memory_order_relaxed);
    stats_.normal_alloc_bytes.store(0, std::memory_order_relaxed);
    stats_.huge_alloc_count.store(0, std::memory_order_relaxed);
    stats_.huge_alloc_success.store(0, std::memory_order_relaxed);
    stats_.huge_alloc_bytes.store(0, std::memory_order_relaxed);
    stats_.huge_align_waste_bytes.store(0, std::memory_order_relaxed);
    stats_.huge_cache_hit_count.store(0, std::memory_order_relaxed);
    stats_.huge_cache_miss_count.store(0, std::memory_order_relaxed);
    stats_.free_count.store(0, std::memory_order_relaxed);
    stats_.free_bytes.store(0, std::memory_order_relaxed);
    stats_.alloc_failed_count.store(0, std::memory_order_relaxed);
    stats_.huge_fallback_to_normal_count.store(0, std::memory_order_relaxed);
    stats_.normal_alloc_failed_count.store(0, std::memory_order_relaxed);
    stats_.huge_alloc_failed_count.store(0, std::memory_order_relaxed);
    stats_.munmap_failed_count.store(0, std::memory_order_relaxed);
    stats_.madvise_failed_count.store(0, std::memory_order_relaxed);
    stats_.mmap_enomem_count.store(0, std::memory_order_relaxed);
    stats_.mmap_other_error_count.store(0, std::memory_order_relaxed);
}

bool PageAllocator::SafeMunmap(void* ptr, size_t size) noexcept {
    if (!ptr || size == 0) {
        return true;
    }

    AM_DCHECK(IsPageAligned(ptr));
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    // clang-format off
    if (munmap(ptr, size) == 0) AM_LIKELY {
        return true;
    }
    // clang-format on

    stats_.munmap_failed_count.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void* PageAllocator::AllocWithRetry(size_t size, int flags) noexcept {
    AM_DCHECK(size > 0);
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    for (size_t i = 0; i < PageConfig::MAX_ALLOC_RETRIES; ++i) {
        if (void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
            ptr != MAP_FAILED) {
            AM_DCHECK(IsPageAligned(ptr));
            return ptr;
        }

        if (const int err = errno; err == ENOMEM) {
            stats_.mmap_enomem_count.fetch_add(1, std::memory_order_relaxed);
            // ENOMEM may be transient under memory pressure. Keep retry/backoff
            // modest to avoid turning allocator failure paths into latency spikes.
            if (i + 1 < PageConfig::MAX_ALLOC_RETRIES) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } else {
            stats_.mmap_other_error_count.fetch_add(1, std::memory_order_relaxed);
            // Fatal/non-transient errors are not worth retrying. Telemetry is
            // intentionally allocation-free because this is allocator core.
            return nullptr;
        }
    }

    return nullptr;
}

void PageAllocator::ApplyHugePageHint(void* ptr, size_t size) noexcept {
    AM_DCHECK(ptr != nullptr);
    AM_DCHECK(IsPageAligned(ptr));
    AM_DCHECK(size > 0);
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    // Best-effort THP hint. Failure is expected on systems/configurations
    // without THP support, so keep it quiet.
    if (madvise(ptr, size, MADV_HUGEPAGE) != 0) {
        stats_.madvise_failed_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Prefetch/populate strategy is configurable and independent of THP hint.
    if (RuntimeConfig::GetInstance().UseMapPopulate()) {
        if (madvise(ptr, size, MADV_WILLNEED) != 0) {
            stats_.madvise_failed_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void* PageAllocator::AllocNormalPage(size_t size) noexcept {
    AM_DCHECK(size > 0);
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    stats_.normal_alloc_count.fetch_add(1, std::memory_order_relaxed);
#ifdef AMMALLOC_TEST
    if (g_mock_normal_alloc_fail.load(std::memory_order_relaxed)) {
        stats_.normal_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
#endif
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (RuntimeConfig::GetInstance().UseMapPopulate()) {
        flags |= MAP_POPULATE;
    }

    void* ptr = AllocWithRetry(size, flags);
    if (!ptr) {
        stats_.normal_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    stats_.normal_alloc_success.fetch_add(1, std::memory_order_relaxed);
    stats_.normal_alloc_bytes.fetch_add(size, std::memory_order_relaxed);

    return ptr;
}

void* PageAllocator::AllocHugePageWithTrim(size_t size) noexcept {
    AM_DCHECK(size > 0);
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    // Reject sizes whose alignment over-allocation would wrap.
    // clang-format off
    if (size > (std::numeric_limits<size_t>::max() - SystemConfig::HUGE_PAGE_SIZE)) AM_UNLIKELY {
        stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // clang-format on

    const size_t alloc_size = size + SystemConfig::HUGE_PAGE_SIZE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* raw_ptr = AllocWithRetry(alloc_size, flags);
    if (!raw_ptr) {
        stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    const auto raw_addr = reinterpret_cast<uintptr_t>(raw_ptr);
    const uintptr_t aligned_addr = (raw_addr + SystemConfig::HUGE_PAGE_SIZE - 1) &
                                   ~(SystemConfig::HUGE_PAGE_SIZE - 1);
    AM_DCHECK((aligned_addr & (SystemConfig::HUGE_PAGE_SIZE - 1)) == 0);
    AM_DCHECK(aligned_addr >= raw_addr);
    AM_DCHECK(aligned_addr + size <= raw_addr + alloc_size);

    const size_t head_gap = aligned_addr - raw_addr;
    const size_t tail_gap = alloc_size - head_gap - size;

    void* head_ptr = raw_ptr;
    void* body_ptr = reinterpret_cast<void*>(aligned_addr);
    void* tail_ptr = reinterpret_cast<void*>(aligned_addr + size);

    bool head_mapped = head_gap > 0;
    bool body_mapped = true;
    bool tail_mapped = tail_gap > 0;

    auto cleanup_remaining_mappings = [&](bool skip_head, bool skip_tail) {
        if (body_mapped && SafeMunmap(body_ptr, size)) {
            body_mapped = false;
        }

        if (!skip_tail && tail_mapped && SafeMunmap(tail_ptr, tail_gap)) {
            tail_mapped = false;
        }

        if (!skip_head && head_mapped && SafeMunmap(head_ptr, head_gap)) {
            head_mapped = false;
        }

        // Failed munmaps are already counted by SafeMunmap. Do not log here:
        // logging may allocate while this failure path owns allocator mappings.
    };

    if (head_gap > 0) {
        if (!SafeMunmap(head_ptr, head_gap)) {
            // Head trim already failed once; avoid immediate duplicate munmap on the same
            // segment and reclaim the remaining still-mapped subranges first.
            const bool skip_head = true;
            const bool skip_tail = false;
            cleanup_remaining_mappings(skip_head, skip_tail);

            stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        head_mapped = false;
    }

    if (tail_gap > 0) {
        if (!SafeMunmap(tail_ptr, tail_gap)) {
            const bool skip_head = false;
            const bool skip_tail = true;
            cleanup_remaining_mappings(skip_head, skip_tail);

            stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        tail_mapped = false;
    }

    AM_DCHECK(!head_mapped);
    AM_DCHECK(body_mapped);
    AM_DCHECK(!tail_mapped);

    stats_.huge_align_waste_bytes.fetch_add(head_gap + tail_gap, std::memory_order_relaxed);
    ApplyHugePageHint(body_ptr, size);
    stats_.huge_alloc_success.fetch_add(1, std::memory_order_relaxed);
    stats_.huge_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    return body_ptr;
}

// Optimistic huge-page allocation strategy:
// 1) First try exact-size mmap and accept it if already huge-page aligned.
// 2) If misaligned, unmap it and retry with over-allocation + trimming.
// This avoids extra VMA operations on the fast-success path.
void* PageAllocator::AllocHugePage(size_t size) noexcept {
    AM_DCHECK(size > 0);
    AM_DCHECK((size & (SystemConfig::PAGE_SIZE - 1)) == 0);

    stats_.huge_alloc_count.fetch_add(1, std::memory_order_relaxed);

#ifdef AMMALLOC_TEST
    if (g_mock_huge_alloc_fail.load(std::memory_order_relaxed)) {
        stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
#endif

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* ptr = AllocWithRetry(size, flags);
    if (!ptr) {
        stats_.huge_alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if ((addr & (SystemConfig::HUGE_PAGE_SIZE - 1)) == 0) {
        ApplyHugePageHint(ptr, size);
        stats_.huge_alloc_success.fetch_add(1, std::memory_order_relaxed);
        stats_.huge_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
        return ptr;
    }

    SafeMunmap(ptr, size);
    return AllocHugePageWithTrim(size);
}

void* PageAllocator::SystemAlloc(size_t page_num) noexcept {
    // clang-format off
    if (page_num == 0) AM_UNLIKELY {
        return nullptr;
    }

    // Reject page counts whose byte-size conversion would wrap.
    if (page_num > (std::numeric_limits<size_t>::max() >> SystemConfig::PAGE_SHIFT)) AM_UNLIKELY {
        return nullptr;
    }

    const size_t size = page_num << SystemConfig::PAGE_SHIFT;
    // Requests below 1MB do not benefit enough from huge-page machinery.
    if (size < (SystemConfig::HUGE_PAGE_SIZE >> 1)) AM_LIKELY {
        void* ptr = AllocNormalPage(size);
        if (!ptr) {
            stats_.alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        }
        return ptr;
    }
    // clang-format on

    // Cache only exact-size huge pages (2MB). Variable-sized large mappings are
    // returned directly to the OS on free.
    if (size == SystemConfig::HUGE_PAGE_SIZE) {
        void* ptr = HugePageCache::GetInstance().Get();
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        if (ptr && (addr & (SystemConfig::HUGE_PAGE_SIZE - 1)) == 0) {
            stats_.huge_cache_hit_count.fetch_add(1, std::memory_order_relaxed);
            return ptr;
        }
        stats_.huge_cache_miss_count.fetch_add(1, std::memory_order_relaxed);
    }

    auto* ptr = AllocHugePage(size);
    // Keep availability-first semantics: fall back to normal pages when huge-page
    // allocation fails.
    if (!ptr) {
        stats_.huge_fallback_to_normal_count.fetch_add(1, std::memory_order_relaxed);
        ptr = AllocNormalPage(size);
        if (!ptr) {
            stats_.alloc_failed_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return ptr;
}

void PageAllocator::SystemFree(void* ptr, size_t page_num) noexcept {
    if (!ptr || page_num == 0) {
        return;
    }

    // Reject page counts whose byte-size conversion would wrap.
    // clang-format off
    if (page_num > (std::numeric_limits<size_t>::max() >> SystemConfig::PAGE_SHIFT)) AM_UNLIKELY {
        return;
    }
    // clang-format on

    const size_t size = page_num << SystemConfig::PAGE_SHIFT;
    stats_.free_count.fetch_add(1, std::memory_order_relaxed);
    stats_.free_bytes.fetch_add(size, std::memory_order_relaxed);
    // For exact-size huge pages, prefer caching the VMA and dropping physical
    // backing (`MADV_DONTNEED`) to reduce future mmap/munmap overhead.
    if (size == SystemConfig::HUGE_PAGE_SIZE && IsHugePageAligned(ptr)) {
        if (madvise(ptr, size, MADV_DONTNEED) != 0) {
            stats_.madvise_failed_count.fetch_add(1, std::memory_order_relaxed);
            // Do not cache a mapping whose physical backing could not be discarded.
            SafeMunmap(ptr, size);
            return;
        }

        if (HugePageCache::GetInstance().Put(ptr)) {
            return;
        }
    }
    SafeMunmap(ptr, size);
}

void PageAllocator::ReleaseHugePageCache() noexcept {
    HugePageCache::GetInstance().ReleaseAllForTesting();
}

}// namespace ammalloc
