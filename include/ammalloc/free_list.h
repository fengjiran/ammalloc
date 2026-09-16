#ifndef AMMALLOC_FREE_LIST_H
#define AMMALLOC_FREE_LIST_H

/// @file free_list.h
/// @brief Allocation-free intrusive LIFO object chain with per-class quota state.
/// @see docs/designs/08-free-list.md

#include "ammalloc/assert.h"
#include "ammalloc/attributes.h"
#include "ammalloc/config.h"
#include "ammalloc/size_class.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace ammalloc {

/// @brief Intrusive link stored in the body of a free object.
struct FreeBlock {
    /// Next free block in the LIFO chain; indeterminate when not in a FreeList.
    FreeBlock* next;
};

// The intrusive link must fit inside the smallest object slot; the size
// classes guarantee the minimum object is ALIGNMENT bytes (see size_class.h).
static_assert(sizeof(FreeBlock) <= SystemConfig::ALIGNMENT,
              "intrusive next pointer does not fit in the smallest object");

/// @brief A detached chain of free objects removed from a FreeList.
///
/// `head` and `tail` point to `FreeBlock` nodes. In a well-formed chain,
/// `tail->next == nullptr` and the chain contains exactly `count` nodes.
struct FreeChain {
    void* head = nullptr;///< First node in the chain (FreeBlock*).
    void* tail = nullptr;///< Last node in the chain (FreeBlock*); tail->next == nullptr.
    size_t count = 0;    ///< Number of nodes in the chain.
};

/// @brief Move-only ownership token for one detached batch of free objects.
///
/// An ObjectBatch carries a single intrusive chain (head/tail/count) plus the
/// size class that owns every object. Exactly one live instance is responsible
/// for delivering the batch to the next legitimate owner — a CentralCache
/// release (ReleaseBatch), a frontend fetch-accept (FreeList::PushBatch), or a
/// cross-thread transport detach (DetachChainForTransport). Moving transfers
/// that responsibility and empties the source; copying is forbidden so two
/// descriptors can never claim the same chain, and move assignment is deleted
/// so a batch can never be overwritten while still holding objects.
///
/// @note Ownership here means "who must keep delivering these objects", not
///       "free on destruct". The objects and their Span storage stay owned by
///       the allocator. The destructor therefore releases nothing; it only
///       asserts in debug builds that the batch was consumed, because an
///       unconsumed non-empty batch would silently leak its objects.
class ObjectBatch {
public:
    ObjectBatch() noexcept = default;

    ObjectBatch(const ObjectBatch&) = delete;
    ObjectBatch& operator=(const ObjectBatch&) = delete;
    ObjectBatch& operator=(ObjectBatch&&) = delete;

    ObjectBatch(ObjectBatch&& other) noexcept
        : head_(std::exchange(other.head_, nullptr)),
          tail_(std::exchange(other.tail_, nullptr)),
          count_(std::exchange(other.count_, 0)),
          size_class_idx_(std::exchange(other.size_class_idx_, kInvalidClass)) {}

    ~ObjectBatch() {
        // Never touch CentralCache here: hidden locks, singleton/TLS teardown
        // order, shutdown recursion and unpredictable latency all argue against
        // auto-release. Consumption is explicit; this only catches a forgotten
        // release in debug builds.
        AM_DCHECK(empty());
    }

    /// @brief Builds a single-object batch, terminating its intrusive link.
    /// @param object Non-null object whose first pointer-sized bytes may hold a
    ///        link; becomes both head and tail.
    /// @param idx Size class that owns `object`.
    /// @pre `object != nullptr` and `idx < SizeClass::kNumSizeClasses`.
    /// @note Validates only the descriptor shape and idx range; it does NOT
    ///       query PageMap to prove the object's real class, which would
    ///       duplicate slow-path work. Object-class correctness is a precondition.
    AM_NODISCARD static ObjectBatch FromSingleObject(void* object, size_t idx) noexcept {
        AM_DCHECK(object != nullptr);
        AM_DCHECK(idx < SizeClass::kNumSizeClasses);
        if (!object) {
            // Release-mode defense only; Debug aborts on the precondition above.
            // am_free rejects null before this factory is ever reached.
            return {};
        }
        static_cast<FreeBlock*>(object)->next = nullptr;
        return {object, object, 1, idx};
    }

    /// @brief Adopts exclusive delivery responsibility for a detached chain.
    /// @param chain Transport representation (head/tail/count) of a well-formed
    ///        detached chain, e.g. from `FreeList::PopRange` or a lock-free queue.
    /// @param idx Size class owning every object in `chain`.
    /// @pre `idx < SizeClass::kNumSizeClasses`. For a non-empty chain: head/tail
    ///      are non-null, `count > 0`, tail is reachable in `count - 1` links,
    ///      `tail->next == nullptr`, and every object belongs to `idx`.
    /// @note This is the single audited edge where an externally-owned raw chain
    ///       (lock-free queue, remote-free queue, backend completion) becomes a
    ///       move-only token. It does NOT query PageMap; class ownership stays a
    ///       caller precondition, checked on the normal release path as applicable.
    AM_NODISCARD static ObjectBatch AdoptChain(FreeChain chain, size_t idx) noexcept {
        AM_DCHECK(idx < SizeClass::kNumSizeClasses);
        if (chain.count == 0) {
            AM_DCHECK(chain.head == nullptr);
            AM_DCHECK(chain.tail == nullptr);
            return {};
        }
        return {chain.head, chain.tail, chain.count, idx};
    }

    /// @brief Reports whether the batch holds no objects.
    AM_NODISCARD bool empty() const noexcept {
        return count_ == 0;
    }
    /// @brief Hottest node, the next one a consumer should take.
    AM_NODISCARD void* head() const noexcept {
        return head_;
    }
    /// @brief Coldest node; `tail()->next == nullptr` in a well-formed batch.
    AM_NODISCARD void* tail() const noexcept {
        return tail_;
    }
    /// @brief Number of objects in the chain.
    AM_NODISCARD size_t count() const noexcept {
        return count_;
    }
    /// @brief Size class owning every object; `kInvalidClass` when empty.
    AM_NODISCARD size_t size_class_idx() const noexcept {
        return size_class_idx_;
    }

    /// @brief Converts this token back into a copyable transport chain.
    /// @return The detached `FreeChain` (head/tail/count); empty for an empty
    ///         batch. The size class is deliberately NOT carried: the caller
    ///         binds it into the transport record so class identity travels with
    ///         the chain across the queue and is re-adopted on the far side.
    /// @note The reverse of `AdoptChain` — the single audited edge where a
    ///       move-only token degrades to a copyable representation for a
    ///       lock-free ring. `&&`-qualified and empties the source via
    ///       MarkProcessed, so one token can never feed two queues. O(1): it
    ///       copies three scalars and never walks the chain.
    AM_NODISCARD FreeChain DetachChainForTransport() && noexcept {
        if (empty()) {
            return {};
        }
        FreeChain out{head_, tail_, count_};
        MarkProcessed();
        return out;
    }

private:
    friend class FreeList;
    friend class CentralCache;

    ObjectBatch(void* head, void* tail, size_t count, size_t idx) noexcept
        : head_(head), tail_(tail), count_(count), size_class_idx_(idx) {
        AM_DCHECK(IsCanonical());
    }

    /// @brief Records that this token's delivery responsibility has been handed
    ///        to the next legitimate owner.
    /// @note Means "this batch has been processed", NOT "every object was
    ///       released or returned to a bitmap" and NOT "each object's delivery
    ///       succeeded". It is the shared exit for every consumer: a
    ///       CentralCache release, a FreeList fetch-accept, or a transport
    ///       detach. Precondition violations (for example a PageMap miss) are
    ///       defensively skipped yet still land here.
    void MarkProcessed() noexcept {
        head_ = nullptr;
        tail_ = nullptr;
        count_ = 0;
        size_class_idx_ = kInvalidClass;
    }

    /// @brief Debug-only shape check; the chain walk is O(count).
    AM_NODISCARD bool IsCanonical() const noexcept {
        if (count_ == 0) {
            return head_ == nullptr && tail_ == nullptr && size_class_idx_ == kInvalidClass;
        }

        if (head_ == nullptr || tail_ == nullptr || size_class_idx_ >= SizeClass::kNumSizeClasses) {
            return false;
        }

        auto* cur = static_cast<FreeBlock*>(head_);
        for (size_t i = 1; i < count_; ++i) {
            if (cur == nullptr) {
                return false;
            }
            cur = cur->next;
        }
        return cur == static_cast<FreeBlock*>(tail_) && cur->next == nullptr;
    }

    /// Sentinel for "no class"; a valid idx is always `< kNumSizeClasses`.
    static constexpr size_t kInvalidClass = SizeClass::kNumSizeClasses;

    void* head_{nullptr};
    void* tail_{nullptr};
    size_t count_{0};
    size_t size_class_idx_{kInvalidClass};
};

static_assert(!std::is_copy_constructible_v<ObjectBatch>);
static_assert(!std::is_copy_assignable_v<ObjectBatch>);
static_assert(!std::is_move_assignable_v<ObjectBatch>);
static_assert(std::is_nothrow_move_constructible_v<ObjectBatch>);
static_assert(sizeof(ObjectBatch) <= 4 * sizeof(void*));

/// @brief Stores free objects in an allocation-free intrusive LIFO chain.
///
/// FreeList stores reclaimed objects by linking through the freed object body
/// itself, so Push()/Pop() do not allocate metadata and remain constant-time.
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
    AM_ALWAYS_INLINE void Push(void* ptr) noexcept {
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
    void PushRange(const FreeChain& chain) noexcept {
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

    /// @brief Consumes a fetched batch, prepending its chain to this list.
    /// @param batch Ownership token produced by `CentralCache::FetchBatch`; taken
    ///        by value so this list becomes the sole owner. Always marked
    ///        processed before returning, so an empty or fully accepted batch
    ///        never trips the destructor's consumed assertion.
    /// @note Frontend counterpart of `CentralCache::ReleaseBatch`: fetch moves a
    ///       batch in, release moves one out. Class-agnostic by design — routing
    ///       the batch to the right `free_lists_[idx]` is the ThreadCache's job,
    ///       not this container's. The chain is already canonical (debug-verified
    ///       when the batch was built), so no re-walk is needed here.
    void PushBatch(ObjectBatch batch) noexcept {
        if (batch.empty()) {
            batch.MarkProcessed();
            return;
        }

        static_cast<FreeBlock*>(batch.tail())->next = head_;
        head_ = static_cast<FreeBlock*>(batch.head());
        size_ += batch.count();
        batch.MarkProcessed();
    }

    /// @brief Removes up to `n` objects from the front, preserving chain order.
    /// @param n Maximum number of objects to remove.
    /// @return The detached chain (head/tail/count); count is smaller than `n`
    ///         only when the list holds fewer than `n` objects. The returned
    ///         chain is terminated: `tail->next == nullptr`.
    AM_NODISCARD FreeChain PopRange(size_t n) noexcept {
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

    /// @brief Removes up to `n` objects from the back of the list, keeping the
    ///        front (most recently pushed) objects local for reuse.
    /// @param n Maximum number of objects to remove.
    /// @return The detached chain (head/tail/count); the head is the newest
    ///         removed object and the tail the oldest. O(list size) traversal,
    ///         intended for slow paths only.
    AM_NODISCARD FreeChain PopRangeTail(size_t n) noexcept {
        FreeChain out;
        const size_t pop_count = n < size_ ? n : size_;
        if (pop_count == 0) {
            return out;
        }

        // Nodes [0, keep) stay in the list; nodes [keep, size_) are evicted.
        // Walk to the last retained node (index keep - 1).
        const size_t keep = size_ - pop_count;
        FreeBlock* keep_tail = head_;
        for (size_t i = 0; i + 1 < keep; ++i) {
            keep_tail = keep_tail->next;
        }

        if (keep == 0) {
            // The whole list is evicted.
            out.head = head_;
            head_ = nullptr;
        } else {
            out.head = keep_tail->next;
            keep_tail->next = nullptr;
        }

        // Walk to the end of the detached chain; it is a suffix of the list.
        auto* tail = static_cast<FreeBlock*>(out.head);
        while (tail->next) {
            tail = tail->next;
        }
        out.tail = tail;
        out.count = pop_count;
        size_ = keep;
        // Invariant: head_ is null exactly when size_ is zero.
        AM_DCHECK((head_ == nullptr) == (size_ == 0));
        return out;
    }

    /// @brief Detaches up to `count` front objects as a move-only owned batch.
    /// @param count Maximum number of objects to remove.
    /// @param idx Size class owning every object in this list.
    /// @return Batch tagging the PopRange chain with `idx`; empty when the list
    ///         holds nothing (an empty batch must carry `kInvalidClass`).
    AM_NODISCARD ObjectBatch PopBatch(size_t count, size_t idx) noexcept {
        const FreeChain chain = PopRange(count);
        if (chain.count == 0) {
            return {};
        }
        return {chain.head, chain.tail, chain.count, idx};
    }

    /// @brief Detaches up to `count` back objects as a move-only owned batch.
    /// @param count Maximum number of objects to remove.
    /// @param idx Size class owning every object in this list.
    /// @return Batch tagging the PopRangeTail chain with `idx`; empty when
    ///         nothing is evictable.
    AM_NODISCARD ObjectBatch PopBatchTail(size_t count, size_t idx) noexcept {
        const FreeChain chain = PopRangeTail(count);
        if (chain.count == 0) {
            return {};
        }
        return {chain.head, chain.tail, chain.count, idx};
    }

    /// @brief Removes the most recently pushed object.
    /// @return Removed object, or null when the list is empty.
    AM_NODISCARD AM_ALWAYS_INLINE void* Pop() noexcept {
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

    FreeBlock* head_;

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