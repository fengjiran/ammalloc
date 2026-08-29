#ifndef AMMALLOC_PAGE_ALLOCATOR_H
#define AMMALLOC_PAGE_ALLOCATOR_H

/// @file
/// @brief Page-level OS allocation and fixed-size metadata pooling.
///
/// `PageAllocator` wraps `mmap`, `munmap`, and `madvise`, including an internal
/// lock-free cache for standard 2 MiB mappings. `ObjectPool` uses those pages to
/// recycle fixed-size metadata without recursively entering `am_malloc`.
/// @see docs/designs/05-page-allocator.md

#include "ammalloc/config.h"

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace ammalloc {

#ifdef AMMALLOC_TEST
extern std::atomic<bool> g_mock_huge_alloc_fail;
extern std::atomic<bool> g_mock_normal_alloc_fail;
#define PAGEALLOCATOR_FRIEND_TEST   \
    friend class PageAllocatorTest; \
    friend class PageAllocatorThreadSafeTest
#else
#define g_mock_huge_alloc_fail (false)
#define g_mock_normal_alloc_fail (false)
#define PAGEALLOCATOR_FRIEND_TEST
#endif

/// @brief Best-effort allocator telemetry stored in relaxed atomics.
/// @note Counters provide observation rather than a mutually consistent snapshot.
struct PageAllocatorStats {
    // Normal-page allocation counters.
    std::atomic<size_t> normal_alloc_count{0};
    std::atomic<size_t> normal_alloc_success{0};
    std::atomic<size_t> normal_alloc_bytes{0};

    // Huge-page allocation and cache counters.
    std::atomic<size_t> huge_alloc_count{0};
    std::atomic<size_t> huge_alloc_success{0};
    std::atomic<size_t> huge_alloc_bytes{0};
    std::atomic<size_t> huge_align_waste_bytes{0};
    std::atomic<size_t> huge_cache_hit_count{0};
    std::atomic<size_t> huge_cache_miss_count{0};

    std::atomic<size_t> free_count{0};
    std::atomic<size_t> free_bytes{0};

    std::atomic<size_t> normal_alloc_failed_count{0};
    std::atomic<size_t> huge_alloc_failed_count{0};
    std::atomic<size_t> alloc_failed_count{0};
    std::atomic<size_t> huge_fallback_to_normal_count{0};
    std::atomic<size_t> mmap_enomem_count{0};
    std::atomic<size_t> mmap_other_error_count{0};
    std::atomic<size_t> munmap_failed_count{0};
    std::atomic<size_t> madvise_failed_count{0};
};

/// @brief Provides thread-safe page mappings for PageCache and ObjectPool.
///
/// Allocation and release may block in kernel calls. Exact 2 MiB mappings can
/// be retained in a fixed-capacity, lock-free dual-stack cache; other mappings
/// return directly to the operating system.
class PageAllocator {
public:
    /// @brief Returns the live telemetry counters.
    /// @return Read-only reference to process-wide atomic counters.
    static const PageAllocatorStats& GetStats() {
        return stats_;
    }

    /// @brief Allocates a page-aligned mapping containing `page_num` pages.
    /// @param page_num Number of system pages to allocate.
    /// @return Page-aligned mapping, or null for invalid input or allocation failure.
    static void* SystemAlloc(size_t page_num) noexcept;

    /// @brief Releases or caches a mapping returned by `SystemAlloc`.
    /// @param ptr Mapping base address; null is accepted as a no-op.
    /// @param page_num Original mapping length in system pages; zero is a no-op.
    /// @note An aligned mapping of exactly `HUGE_PAGE_SIZE` bytes is eligible for
    ///       the internal cache after successful `MADV_DONTNEED`.
    static void SystemFree(void* ptr, size_t page_num) noexcept;

    /// @brief Resets all telemetry counters to zero.
    static void ResetStats() noexcept;

    /// @brief Unmaps all standard huge pages currently held by the cache.
    /// @note Intended for tests and controlled teardown after concurrent use stops.
    static void ReleaseHugePageCache() noexcept;

    /// @brief Records one `munmap` failure in telemetry.
    /// @note Used internally by HugePageCache during controlled cache release.
    static void RecordMunmapFailure() noexcept {
        stats_.munmap_failed_count.fetch_add(1, std::memory_order_relaxed);
    }

private:
    inline static PageAllocatorStats stats_;

    static void* AllocWithRetry(size_t size, int flags) noexcept;
    static void ApplyHugePageHint(void* ptr, size_t size) noexcept;
    static void* AllocNormalPage(size_t size) noexcept;
    static void* AllocHugePageWithTrim(size_t size) noexcept;
    static void* AllocHugePage(size_t size) noexcept;
    static bool SafeMunmap(void* ptr, size_t size) noexcept;

    PAGEALLOCATOR_FRIEND_TEST;
};

/// @brief Owns and recycles fixed-size objects in PageAllocator-backed chunks.
///
/// The pool owns all allocated chunks until `ReleaseMemory()` or destruction.
/// `TryNew()` constructs `T` in-place and `Delete()` destroys `T` then recycles storage.
///
/// @note Not thread-safe. Every member mutates pool state unconditionally, so the
///       caller must serialize all use of a given pool. Each pooled instance names
///       the lock that provides that serialization.
/// @tparam T Object type; its storage must hold an intrusive free-list pointer.
/// @tparam CHUNK_SIZE Target number of bytes requested for each new chunk.
template<typename T, size_t CHUNK_SIZE = 64 * 1024>
    requires(sizeof(T) >= sizeof(void*))
class ObjectPool {
public:
    ObjectPool() = default;

    static_assert(std::is_nothrow_destructible_v<T>,
                  "ObjectPool metadata destructors must not throw");

    /// @brief Constructs one object in pooled storage without throwing on OOM.
    /// @tparam Args Constructor argument types.
    /// @param args Arguments forwarded to the `T` constructor.
    /// @return Pointer to the constructed object owned by the caller until `Delete`,
    ///         or null if a backing page mapping cannot be acquired.
    /// @note `T` construction is constrained to no-throw so metadata OOM is
    ///       represented uniformly as a null result.
    template<typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    T* TryNew(Args&&... args) noexcept {
        if (free_list_) {
            void* obj = free_list_;
            free_list_ = free_list_->next;
            return new (obj) T(std::forward<Args>(args)...);
        }

        if (remain_bytes_ < sizeof(T)) {
            size_t num_objs = CHUNK_SIZE / sizeof(T);
            if (num_objs < 10) {
                num_objs = 10;
            }
            size_t needed_bytes = sizeof(ChunkHeader) + num_objs * sizeof(T) + alignof(T) - 1;
            size_t page_num = (needed_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
            void* ptr = PageAllocator::SystemAlloc(page_num);
            if (!ptr) {
                return nullptr;
            }

            auto* new_chunk = static_cast<ChunkHeader*>(ptr);
            new_chunk->next = chunk_list_;
            new_chunk->page_num = page_num;
            chunk_list_ = new_chunk;

            uintptr_t raw_data_start = reinterpret_cast<uintptr_t>(ptr) + sizeof(ChunkHeader);
            uintptr_t aligned_data_start = (raw_data_start + alignof(T) - 1) & (~(alignof(T) - 1));
            data_ = reinterpret_cast<char*>(aligned_data_start);

            size_t total_bytes = page_num << SystemConfig::PAGE_SHIFT;
            remain_bytes_ = total_bytes - (aligned_data_start - reinterpret_cast<uintptr_t>(ptr));
        }

        void* obj = data_;
        data_ += sizeof(T);
        remain_bytes_ -= sizeof(T);
        return new (obj) T(std::forward<Args>(args)...);
    }

    /// @brief Destroys an object and returns its storage to the pool.
    /// @param obj Live object previously returned by this pool.
    /// @pre `obj` is non-null and has not already been deleted.
    void Delete(T* obj) noexcept {
        obj->~T();
        auto* header = reinterpret_cast<FreeHeader*>(obj);
        header->next = free_list_;
        free_list_ = header;
    }

    /// @brief Releases every chunk owned by the pool.
    /// @pre No outstanding object is accessed during or after this call.
    void ReleaseMemory() noexcept {
        auto* cur = chunk_list_;
        while (cur) {
            auto* next = cur->next;
            PageAllocator::SystemFree(cur, cur->page_num);
            cur = next;
        }
        chunk_list_ = nullptr;
        free_list_ = nullptr;
        data_ = nullptr;
        remain_bytes_ = 0;
    }

    ~ObjectPool() {
        auto* cur = chunk_list_;
        while (cur) {
            auto* next = cur->next;
            PageAllocator::SystemFree(cur, cur->page_num);
            cur = next;
        }
    }

private:
    struct FreeHeader {
        FreeHeader* next;
    };

    struct ChunkHeader {
        ChunkHeader* next;
        size_t page_num;
    };

    char* data_{nullptr};
    size_t remain_bytes_{0};
    FreeHeader* free_list_{nullptr};
    ChunkHeader* chunk_list_{nullptr};
};

}// namespace ammalloc

#endif// AMMALLOC_PAGE_ALLOCATOR_H
