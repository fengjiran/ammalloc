#ifndef AMMALLOC_SPAN_H
#define AMMALLOC_SPAN_H

/// @file
/// @brief Span metadata, bitmap allocation, and intrusive Span lists.
/// @see docs/designs/07-span-and-pagemap.md

#include "ammalloc/assert.h"
#include "ammalloc/common.h"
#include "ammalloc/config.h"

namespace ammalloc {

/// @brief Describes one contiguous page range and its object-allocation state.
///
/// PageCache owns Span metadata and uses it for page-level splitting and
/// coalescing. CentralCache borrows active Spans and uses the in-page bitmap for
/// object allocation. The structure occupies one cache line. Fields are not
/// atomic; callers must hold the owning PageCache shard lock or CentralCache
/// bucket lock required by the current state.
struct alignas(SystemConfig::CACHE_LINE_SIZE) Span {
    // Intrusive linked list pointers for PageCache/SpanList.
    Span* next{nullptr};
    Span* prev{nullptr};

    // Page-level addressing info.
    uint64_t start_page_idx{0};
    uint32_t page_num{0};

    // Packed status flags. Use IsUsed/SetUsed/IsCommitted/SetCommitted.
    uint16_t flags{0};
    uint16_t size_class_idx{0};// Index into the CentralCache bucket array.

    // Object allocation metadata (valid when used by CentralCache).
    uint32_t aligned_obj_size{0};
    uint32_t capacity{0};   // Maximum objects stored in this Span.
    uint32_t use_count{0};  // Objects currently allocated from this Span.
    uint32_t scan_cursor{0};// First bitmap word that may contain a free bit.

    // Calculated data offset (avoids storing full pointer).
    uint32_t obj_offset{0};    // Offset from the page base to the first object.
    uint32_t owner_shard_id{0};// PageCache shard that owns this metadata.

    // Cold data: used by background scavenger thread.
    uint64_t last_used_time_ms{0};

    enum FlagBit : uint16_t {
        kUsedMask = 1u << 0,
        kCommittedMask = 1u << 1
    };

    Span() = default;
    Span(uint64_t start_page_idx_, uint32_t page_num_) noexcept
        : start_page_idx(start_page_idx_), page_num(page_num_) {}

    /// @brief Reports whether the Span is assigned to an active allocation path.
    /// @return True when the used flag is set.
    AM_NODISCARD AM_ALWAYS_INLINE bool IsUsed() const noexcept {
        return flags & kUsedMask;
    }

    /// @brief Reports whether the virtual range has committed physical backing.
    /// @return True when the committed flag is set.
    AM_NODISCARD AM_ALWAYS_INLINE bool IsCommitted() const noexcept {
        return flags & kCommittedMask;
    }

    /// @brief Updates the used flag.
    /// @param used New flag value.
    AM_ALWAYS_INLINE void SetUsed(bool used) noexcept {
        flags = (flags & ~kUsedMask) | (used ? kUsedMask : 0);
    }

    /// @brief Updates the committed flag.
    /// @param committed New flag value.
    AM_ALWAYS_INLINE void SetCommitted(bool committed) noexcept {
        flags = (flags & ~kCommittedMask) | (committed ? kCommittedMask : 0);
    }

    // Derive bitmap and data addresses to keep Span within one cache line.
    /// @brief Returns the base address of the represented page range.
    /// @return Page-aligned base address.
    AM_NODISCARD AM_ALWAYS_INLINE void* GetPageBaseAddr() const noexcept {
        return reinterpret_cast<void*>(start_page_idx << SystemConfig::PAGE_SHIFT);
    }

    /// @brief Returns the bitmap stored at the beginning of the page range.
    /// @return Pointer to the first bitmap word.
    AM_NODISCARD AM_ALWAYS_INLINE uint64_t* GetBitmap() const noexcept {
        return static_cast<uint64_t*>(GetPageBaseAddr());
    }

    /// @brief Returns the number of bitmap words required by `capacity` objects.
    /// @return Bitmap length in 64-bit words.
    AM_NODISCARD AM_ALWAYS_INLINE size_t GetBitmapNum() const noexcept {
        return (capacity + 63) >> 6;
    }

    /// @brief Returns the first object slot after bitmap and alignment padding.
    /// @return Pointer to the object-data region.
    AM_NODISCARD AM_ALWAYS_INLINE void* GetDataBasePtr() const noexcept {
        return static_cast<char*>(GetPageBaseAddr()) + obj_offset;
    }

    /// @brief Initializes bitmap and size-class metadata for object allocation.
    /// @param aligned_object_size Valid size-class-aligned object size.
    /// @pre The Span is exclusively owned by the calling CentralCache bucket.
    void Init(size_t aligned_object_size);

    /// @brief Allocates one object by clearing a free bitmap bit.
    /// @return Object pointer, or null when the Span is full.
    /// @pre The corresponding CentralCache bucket lock is held.
    void* AllocObject();

    /// @brief Returns one object to the Span bitmap.
    /// @param ptr Object slot previously allocated from this Span.
    /// @pre The corresponding CentralCache bucket lock is held.
    void FreeObject(void* ptr);
};

static_assert(sizeof(Span) == SystemConfig::CACHE_LINE_SIZE, "Span must be exactly 64 bytes");
static_assert(alignof(Span) == SystemConfig::CACHE_LINE_SIZE, "Span alignment mismatch");

/// @brief Maintains an allocation-free doubly linked list with a circular sentinel.
///
/// SpanList never owns or deletes Span metadata. PageCache owns each node, and
/// callers provide the shard or bucket lock appropriate to the list. The inline
/// sentinel removes null branches from insertion and removal.
class alignas(SystemConfig::CACHE_LINE_SIZE) SpanList {
public:
    /// @brief Constructs an empty circular list.
    SpanList() noexcept {
        head_.next = &head_;
        head_.prev = &head_;
    }

    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    /// @brief Returns the first node.
    /// @return First Span, or `end()` when empty.
    AM_NODISCARD Span* begin() const noexcept {
        return head_.next;
    }

    /// @brief Returns the circular sentinel.
    /// @return End sentinel; it is not an allocatable Span.
    AM_NODISCARD Span* end() noexcept {
        return &head_;
    }

    /// @brief Reports whether the list has no nodes.
    /// @return True when `begin() == end()`.
    AM_NODISCARD bool empty() const noexcept {
        return head_.next == &head_;
    }

    /// @brief Inserts a Span immediately before a list position.
    /// @param pos Existing node or sentinel that follows the insertion point.
    /// @param span Unlinked Span to insert.
    /// @pre The lock protecting this list is held.
    /// @pre `pos` and `span` are non-null, and `span` is not already linked.
    static void insert(Span* pos, Span* span) noexcept {
        AM_DCHECK(pos != nullptr && span != nullptr);
        span->next = pos;
        span->prev = pos->prev;
        span->prev->next = span;
        pos->prev = span;
    }

    /// @brief Inserts a Span at the front of the list.
    /// @param span Unlinked Span to insert.
    void push_front(Span* span) const noexcept {
        insert(begin(), span);
    }

    /// @brief Inserts a Span at the back of the list.
    /// @param span Unlinked Span to insert.
    void push_back(Span* span) noexcept {
        insert(end(), span);
    }

    /// @brief Unlinks a Span without destroying its metadata.
    /// @param span Linked Span to remove.
    /// @return Node following the removed Span, possibly `end()`.
    /// @pre The lock protecting this list is held.
    Span* erase(Span* span) const noexcept {
        AM_DCHECK(span != nullptr && span != &head_);
        auto* prev = span->prev;
        auto* next = span->next;
        prev->next = next;
        next->prev = prev;
        span->prev = nullptr;
        span->next = nullptr;
        return next;
    }

    /// @brief Removes the first Span without destroying its metadata.
    /// @return Removed Span, or null when the list is empty.
    AM_NODISCARD Span* pop_front() const noexcept {
        // clang-format off
        if (empty()) AM_UNLIKELY {
            return nullptr;
        }
        auto* span = head_.next;
        erase(span);
        return span;
        // clang-format on
    }

private:
    /// Inline sentinel that also anchors the circular links.
    Span head_;
};

}// namespace ammalloc

#endif// AMMALLOC_SPAN_H
