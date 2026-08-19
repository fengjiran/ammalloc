#ifndef AMMALLOC_FREE_LIST_H
#define AMMALLOC_FREE_LIST_H

/// @file
/// @brief Allocation-free intrusive LIFO object chain with per-class quota state.
/// @see docs/designs/02-thread-cache.md

#include "ammalloc/attributes.h"

#include <cstddef>
#include <cstdint>

namespace ammalloc {

/// @brief Intrusive link stored in the body of a free object.
struct FreeBlock {
    FreeBlock* next;
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

    /// @brief Removes all objects without modifying their embedded links.
    void clear() noexcept {
        head_ = nullptr;
        size_ = 0;
    }

    /// @brief Pushes one object onto the front of the list.
    /// @param ptr Object whose first pointer-sized bytes may store an intrusive link;
    ///        null is ignored.
    void push(void* ptr) noexcept {
        if (!ptr) AM_UNLIKELY return;

        auto* block = static_cast<FreeBlock*>(ptr);
        block->next = head_;
        head_ = block;
        ++size_;
    }

    /// @brief Prepends an existing intrusive chain to the list.
    /// @param begin First object in the chain.
    /// @param end Last object in the chain.
    /// @param count Number of objects in the chain.
    /// @pre A non-empty chain is well formed and `end` is reachable from `begin`.
    void push_range(void* begin, void* end, size_t count) noexcept {
        if (!begin || !end || count == 0) {
            return;
        }

        static_cast<FreeBlock*>(end)->next = head_;
        head_ = static_cast<FreeBlock*>(begin);
        size_ += count;
    }

    /// @brief Removes the most recently pushed object.
    /// @return Removed object, or null when the list is empty.
    AM_NODISCARD void* pop() noexcept {
        if (empty()) AM_UNLIKELY return nullptr;

        auto* block = head_;
        if (block->next) AM_LIKELY AM_BUILTIN_PREFETCH(block->next, 0, 3);

        head_ = head_->next;
        --size_;
        return block;
    }

    /// @brief Returns the current ThreadCache high-water limit.
    /// @return Configured maximum local object count.
    AM_NODISCARD size_t max_size() const noexcept {
        return max_size_;
    }

    /// @brief Replaces the ThreadCache high-water limit.
    /// @param n New object-count limit.
    void set_max_size(size_t n) noexcept {
        max_size_ = n;
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
    // Head of the intrusive LIFO chain.
    FreeBlock* head_;

    // Number of cached objects currently held in this list.
    uint32_t size_;

    // ThreadCache high-water mark for this size class.
    //
    // This starts at 1, grows under refill pressure, and decays in batch-sized
    // steps after repeated overflow trims. CentralCache-owned temporary lists do
    // not interpret this field as a policy knob.
    uint32_t max_size_;

    // Counts consecutive overflow-trim events without intervening refill demand.
    //
    // ThreadCache uses this as a cheap decay signal: sustained free pressure
    // means the current high-water mark is likely above steady-state demand.
    uint32_t overages_;
};

}// namespace ammalloc

#endif// AMMALLOC_FREE_LIST_H