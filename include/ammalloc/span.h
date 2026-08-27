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
    uint32_t use_count{0};  // Objects currently absent from the Span bitmap (incl. user/caches).
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
        return (capacity + SystemConfig::BITMAP_MASK) >> SystemConfig::BITMAP_SHIFT;
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

    /// @brief Resolves a pointer to its object-slot index without touching the
    ///        bitmap. Uses uintptr_t arithmetic so the comparison stays defined
    ///        for arbitrary caller pointers.
    /// @param ptr Address alleged to reference one object slot in this Span.
    /// @return Zero-based slot index, or `std::numeric_limits<size_t>::max()`
    ///         when `ptr` is out of range or misaligned. Large-object Spans
    ///         (`capacity == 0`) always yield that sentinel; callers validate
    ///         those against `GetPageBaseAddr()`.
    AM_NODISCARD size_t ObjectSlotOf(void* ptr) const noexcept;
};

// The message stays literal: static_assert requires a string literal, so it
// describes the invariant instead of hardcoding CACHE_LINE_SIZE's value.
static_assert(sizeof(Span) == SystemConfig::CACHE_LINE_SIZE,
              "Span must fit exactly one cache line");
static_assert(alignof(Span) == SystemConfig::CACHE_LINE_SIZE,
              "Span alignment mismatch");

/// @brief Maintains an allocation-free doubly linked list with a circular sentinel.
///
/// SpanList never owns or deletes Span metadata. PageCache owns each node, and
/// callers provide the shard or bucket lock appropriate to the list. The inline
/// sentinel removes null branches from insertion and removal.
class alignas(SystemConfig::CACHE_LINE_SIZE) SpanList {
public:
    /// @brief Forward iterator following the intrusive `next` links.
    ///        Invalidated when its node is erased from the list.
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Span;
        using difference_type = std::ptrdiff_t;
        using pointer = Span*;
        using reference = Span&;

        Iterator() = default;
        explicit Iterator(Span* node) noexcept : node_(node) {}

        reference operator*() const noexcept {
            return *node_;
        }

        pointer operator->() const noexcept {
            return node_;
        }

        Iterator& operator++() noexcept {
            node_ = node_->next;
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
            return a.node_ == b.node_;
        }

        friend bool operator!=(const Iterator& a, const Iterator& b) noexcept {
            return a.node_ != b.node_;
        }

    private:
        Span* node_{nullptr};
    };

    /// @brief Read-only forward iterator following the intrusive `next` links.
    ///        Invalidated when its node is erased from the list.
    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const Span;
        using difference_type = std::ptrdiff_t;
        using pointer = const Span*;
        using reference = const Span&;

        ConstIterator() = default;
        explicit ConstIterator(const Span* node) noexcept : node_(node) {}

        reference operator*() const noexcept {
            return *node_;
        }

        pointer operator->() const noexcept {
            return node_;
        }

        ConstIterator& operator++() noexcept {
            node_ = node_->next;
            return *this;
        }

        ConstIterator operator++(int) noexcept {
            ConstIterator tmp = *this;
            ++*this;
            return tmp;
        }

        friend bool operator==(const ConstIterator& a, const ConstIterator& b) noexcept {
            return a.node_ == b.node_;
        }
        
        friend bool operator!=(const ConstIterator& a, const ConstIterator& b) noexcept {
            return a.node_ != b.node_;
        }

    private:
        const Span* node_{nullptr};
    };

    /// @brief Constructs an empty circular list.
    SpanList() noexcept {
        head_.next = &head_;
        head_.prev = &head_;
    }

    SpanList(const SpanList&) = delete;
    SpanList& operator=(const SpanList&) = delete;

    /// @brief Returns an iterator to the first node.
    /// @return Iterator to the first Span, or `end()` when empty.
    AM_NODISCARD Iterator begin() noexcept {
        return Iterator(head_.next);
    }

    /// @brief Returns a read-only iterator to the first node.
    /// @return ConstIterator to the first Span, or `end()` when empty.
    AM_NODISCARD ConstIterator begin() const noexcept {
        return ConstIterator(head_.next);
    }

    /// @brief Returns the circular sentinel iterator.
    /// @return Iterator to the sentinel; it is not an allocatable Span.
    AM_NODISCARD Iterator end() noexcept {
        return Iterator(&head_);
    }

    /// @brief Returns the circular sentinel iterator for read-only traversal.
    /// @return ConstIterator to the sentinel; it is not an allocatable Span.
    AM_NODISCARD ConstIterator end() const noexcept {
        return ConstIterator(&head_);
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
    void push_front(Span* span) noexcept {
        insert(&*begin(), span);
    }

    /// @brief Inserts a Span at the back of the list.
    /// @param span Unlinked Span to insert.
    void push_back(Span* span) noexcept {
        insert(&*end(), span);
    }

    /// @brief Unlinks a Span without destroying its metadata.
    /// @param span Linked Span to remove.
    /// @return Node following the removed Span, possibly `end()`.
    /// @pre The lock protecting this list is held.
    Span* erase(Span* span) noexcept {
        AM_DCHECK(span != nullptr && span != &head_);
        auto* prev = span->prev;
        auto* next = span->next;
        prev->next = next;
        next->prev = prev;
        span->prev = nullptr;
        span->next = nullptr;
        return next;
    }

    /// @brief Unlinks the Span addressed by an iterator without destroying its
    ///        metadata, then advances to the next node.
    /// @param pos Iterator to the Span to remove.
    /// @return Iterator to the node following the removed Span, possibly
    ///         `end()`. Invalidates only the erased position.
    /// @pre The lock protecting this list is held.
    /// @pre `pos` dereferences to a linked Span (not default-constructed).
    Iterator erase(Iterator pos) noexcept {
        return Iterator(erase(&*pos));
    }

    /// @brief Removes the first Span without destroying its metadata.
    /// @return Removed Span, or null when the list is empty.
    AM_NODISCARD Span* pop_front() noexcept {
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
