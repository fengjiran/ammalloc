#ifndef AMMALLOC_FREE_LIST_H
#define AMMALLOC_FREE_LIST_H

/// @file
/// @brief Allocation-free intrusive LIFO object chain with per-class quota state.
/// @see docs/designs/02-thread-cache.md

#include "ammalloc/assert.h"
#include "ammalloc/attributes.h"
#include "ammalloc/config.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ammalloc {

/// @brief Intrusive link stored in the body of a free object.
struct FreeBlock {
    FreeBlock* next;
};

// The intrusive link must fit inside the smallest object slot; the size
// classes guarantee the minimum object is ALIGNMENT bytes (see size_class.h).
static_assert(sizeof(FreeBlock) <= SystemConfig::ALIGNMENT,
              "intrusive next pointer does not fit in the smallest object");

/// @brief A detached chain of free objects removed from a FreeList.
struct FreeChain {
    void* head = nullptr;
    void* tail = nullptr;
    size_t count = 0;
};

/// @brief Stores free objects in an allocation-free intrusive LIFO chain.
///
/// FreeList stores reclaimed objects by linking through the freed object body
/// itself, so push/pop do not allocate metadata and remain constant-time.
/// Besides the object chain, the list also carries per-class quota state used
/// by ThreadCache slow-start and overages-based decay.
///
/// @note FreeList is not thread-safe. ThreadCache instances are thread-confined;
///       shared uses require external synchronization.
class FreeList {
public:
    /// @brief Constructs an empty list with an initial quota of one object.
    constexpr FreeList() noexcept
        : head_(nullptr), size_(0), max_size_(1), overages_(0) {}

    FreeList(const FreeList&) = delete;
    FreeList& operator=(const FreeList&) = delete;

    /// @brief Reports whether the list contains no objects.
    /// @return True when the intrusive chain is empty.
    AM_NODISCARD bool empty() const noexcept {
        return head_ == nullptr;
    }

    /// @brief Returns the number of objects in the list.
    /// @return Current object count.
    AM_NODISCARD size_t size() const noexcept {
        return size_;
    }

    /// @brief Pushes one object onto the front of the list.
    /// @param ptr Object whose first pointer-sized bytes may store an intrusive link;
    ///        null is ignored.
    void push(void* ptr) noexcept {
        // clang-format off
        if (!ptr) AM_UNLIKELY {
            return;
        }
        // clang-format on

        auto* block = static_cast<FreeBlock*>(ptr);
        block->next = head_;
        head_ = block;
        ++size_;
    }

    /// @brief Prepends an existing chain to the list.
    /// @param chain Detached chain (head/tail/count); the head/tail pair must be
    ///        a well-formed chain of `count` objects.
    void push_range(const FreeChain& chain) noexcept {
        if (!chain.head || !chain.tail || chain.count == 0) {
            return;
        }

        // Debug-only invariant: `count` must match the chain length and `tail`
        // must be the chain's last node. A mismatch would desync size_ from head_.
        AM_DCHECK(CountChain(static_cast<FreeBlock*>(chain.head),
                             static_cast<FreeBlock*>(chain.tail)) == chain.count);

        static_cast<FreeBlock*>(chain.tail)->next = head_;
        head_ = static_cast<FreeBlock*>(chain.head);
        size_ += chain.count;
    }

    /// @brief Removes up to `n` objects from the front, preserving chain order.
    /// @param n Maximum number of objects to remove.
    /// @return The detached chain (head/tail/count); count is smaller than `n`
    ///         only when the list holds fewer than `n` objects. The returned
    ///         chain is terminated: `tail->next == nullptr`.
    AM_NODISCARD FreeChain pop_range(size_t n) noexcept {
        FreeChain out;
        FreeBlock* cur = head_;
        for (size_t i = 0; i < n && cur; ++i) {
            out.tail = cur;
            cur = cur->next;
            ++out.count;
        }

        if (out.count > 0) {
            out.head = head_;
            head_ = cur;
            AM_DCHECK(out.count <= size_);
            size_ -= out.count;
            // Terminate the detached chain so walkers cannot reach the
            // objects that remain in this list.
            static_cast<FreeBlock*>(out.tail)->next = nullptr;
            // Invariant: head_ is null exactly when size_ is zero.
            AM_DCHECK((head_ == nullptr) == (size_ == 0));
        }
        return out;
    }

    /// @brief Removes the most recently pushed object.
    /// @return Removed object, or null when the list is empty.
    AM_NODISCARD void* pop() noexcept {
        // clang-format off
        if (empty()) AM_UNLIKELY {
            return nullptr;
        }

        auto* block = head_;
        if (block->next) AM_LIKELY {
            AM_BUILTIN_PREFETCH(block->next, 0, 3);
        }
        // clang-format on

        // Debug-only invariants: catch a head_/size_ desync before it
        // propagates as a wrapped counter or a stale size residue.
        AM_DCHECK(size_ > 0);

        head_ = head_->next;
        --size_;
        // Invariant: head_ is null exactly when size_ is zero.
        AM_DCHECK((head_ == nullptr) == (size_ == 0));
        return block;
    }

    /// @brief Returns the current ThreadCache high-water limit.
    /// @return Configured maximum local object count.
    AM_NODISCARD size_t max_size() const noexcept {
        return max_size_;
    }

    /// @brief Replaces the ThreadCache high-water limit.
    /// @param n New object-count limit; clamped to at least 1 because a zero
    ///        quota would make every refill fail permanently.
    void set_max_size(size_t n) noexcept {
        // Debug-only: flag callers that pass zero instead of silently clamping.
        AM_DCHECK(n >= 1);
        max_size_ = std::max(n, size_t{1});
    }

    /// @brief Returns the consecutive overflow-trim count.
    /// @return Number of overage events since the last reset.
    AM_NODISCARD size_t overages() const noexcept {
        return overages_;
    }

    /// @brief Replaces the consecutive overflow-trim count.
    /// @param n New overage count.
    void set_overages(size_t n) noexcept {
        overages_ = n;
    }

private:
    // Debug helper: walks the chain from `head` to `end`; returns SIZE_MAX
    // when `end` is unreachable.
    static size_t CountChain(const FreeBlock* head, const FreeBlock* end) noexcept {
        size_t n = 0;
        while (head && head != end) {
            head = head->next;
            ++n;
        }
        return head ? n + 1 : std::numeric_limits<size_t>::max();
    }

    // Head of the intrusive LIFO chain.
    FreeBlock* head_;

    // Number of cached objects currently held in this list.
    size_t size_;

    // ThreadCache high-water mark for this size class.
    //
    // This starts at 1, grows under refill pressure, and decays in batch-sized
    // steps after repeated overflow trims.
    size_t max_size_;

    // Counts consecutive overflow-trim events without intervening refill demand.
    //
    // ThreadCache uses this as a cheap decay signal: sustained free pressure
    // means the current high-water mark is likely above steady-state demand.
    size_t overages_;
};

}// namespace ammalloc

#endif// AMMALLOC_FREE_LIST_H