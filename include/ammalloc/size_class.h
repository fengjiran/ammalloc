#ifndef AMMALLOC_SIZE_CLASS_H
#define AMMALLOC_SIZE_CLASS_H

/// @file
/// @brief Constant-time mapping between allocation sizes and fixed-size buckets.
///
/// Small sizes use compile-time lookup tables; larger ThreadCache sizes use a
/// fixed number of bit operations.

#include "ammalloc/assert.h"
#include "ammalloc/config.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace ammalloc {

namespace detail {

static constexpr size_t CalculateIndex(size_t original_size) noexcept {
    if (original_size == 0) {
        return 0;
    }

    // Linear range: 8-byte alignment in [1, 128] maps [1, 8] -> 0, ...,
    // [121, 128] -> 15. The same formula generates the lookup table that
    // Index() serves for sizes up to kSmallSizeThreshold.
    // clang-format off
    if (original_size <= 128) AM_LIKELY {
        return (original_size - 1) >> 3;
    }
    // clang-format on

    // Subdivide each power-of-two interval into `kStepsPerGroup` buckets.
    int msb = std::bit_width(original_size - 1) - 1;
    int group_idx = msb - 7;
    size_t base_idx = 16 + (group_idx << SizeConfig::kStepShift);
    int shift = msb - SizeConfig::kStepShift;
    size_t group_offset = ((original_size - 1) >> shift) & (SizeConfig::kStepsPerGroup - 1);

    return base_idx + group_offset;
}

static constexpr size_t CalculateSize(size_t idx) noexcept {
    // Right-inverse of CalculateIndex: indices 0..15 map directly to the
    // 8-byte ladder through 128 bytes.
    // clang-format off
    if (idx < 16) AM_LIKELY {
        return (idx + 1) << 3;
    }
    // clang-format on

    // Decode the group/step components of indices >= 16.
    size_t relative_idx = idx - 16;
    size_t group_idx = relative_idx >> SizeConfig::kStepShift;
    size_t step_idx = relative_idx & (SizeConfig::kStepsPerGroup - 1);
    size_t msb = group_idx + 7;
    size_t base_size = 1ULL << msb;
    size_t step_size = 1ULL << (msb - SizeConfig::kStepShift);
    return base_size + (step_idx + 1) * step_size;
}

}// namespace detail

// Validate small-object boundaries.
static_assert(detail::CalculateSize(0) == 8);
static_assert(detail::CalculateSize(15) == 128);

// Validate large-object group 0 (range 129-256). Step size = (256-128)/4 = 32.
static_assert(detail::CalculateSize(16) == 160);// 128 + 32
static_assert(detail::CalculateSize(17) == 192);// 160 + 32
static_assert(detail::CalculateSize(19) == 256);// Last bucket of group 0

// Validate large-object group 1 (range 257-512). Step size = (512-256)/4 = 64.
static_assert(detail::CalculateSize(20) == 320);// 256 + 64

// Validate the Index -> Size -> Index inverse property.
static_assert(detail::CalculateIndex(1) == 0);
static_assert(detail::CalculateIndex(8) == 0);
static_assert(detail::CalculateIndex(9) == 1);
static_assert(detail::CalculateIndex(128) == 15);
static_assert(detail::CalculateIndex(129) == 16);// Falls into 160-byte bucket
static_assert(detail::CalculateIndex(160) == 16);

/// @brief Maps request sizes to bucket geometry and batch-transfer policies.
///
/// The size ladder follows a TCMalloc-style stepped strategy:
/// - [1, 128] bytes: 8-byte alignment.
/// - [129, `MAX_TC_SIZE`] bytes: four buckets per power-of-two interval.
///
/// Batch and page-transfer results are also indexed by size class so requests
/// in the same bucket share one policy. The class holds no mutable runtime
/// state: every lookup table is a compile-time constant, so all members are
/// safe for concurrent use.
class SizeClass {
public:
    /// @brief Maps a requested size to its size-class index.
    ///
    /// This function implements a hybrid mapping strategy to balance memory overhead
    /// and lookup speed:
    /// 1. Linear Mapping [1, 128] bytes: Precise 8-byte alignment for the most frequent
    ///    small allocations.
    /// 2. Logarithmic Stepped Mapping (128B+): Uses a geometric progression (groups)
    ///    to maintain a constant relative fragmentation (~12.5% to 25% depending on
    ///    kStepShift) while significantly reducing the number of FreeLists in ThreadCache.
    ///
    /// @note Special case for size=0:
    /// - `Index(0)` returns 0 (maps to the minimum 8-byte size class).
    /// - This design lets mapping interfaces (Index, RoundUp) handle size=0
    ///   gracefully, while strategy interfaces (CalculateBatchSize, GetMovePageNum)
    ///   treat 0 as invalid input.
    /// - ammalloc intentionally serves `am_malloc(0)` from its minimum size class;
    ///   this is a project policy rather than a requirement that every allocator
    ///   return non-null for a zero-size request.
    ///
    /// @param original_size The requested allocation size in bytes.
    /// @return The zero-based index of the size class, or std::numeric_limits<size_t>::max()
    ///         if the size exceeds MAX_TC_SIZE.
    ///
    /// @note This implementation is branch-prediction friendly and utilizes C++20
    ///       bit-manipulation (std::bit_width) for O(1) performance. Index() uses
    ///       a precomputed lookup table for sizes in [0, kSmallSizeThreshold], and
    ///       falls back to the arithmetic mapping formula for larger in-range sizes.
    AM_ALWAYS_INLINE static constexpr size_t Index(size_t original_size) noexcept {
        // clang-format off
        if (original_size > SizeConfig::MAX_TC_SIZE) AM_UNLIKELY {
            return std::numeric_limits<size_t>::max();
        }

        // Fast path: O(1) table lookup for sizes in [0, kSmallSizeThreshold].
        if (original_size <= SizeConfig::kSmallSizeThreshold) AM_LIKELY {
            return small_index_table_[original_size];
        }
        // clang-format on

        // Slow path: arithmetic formula for larger in-range sizes.
        return detail::CalculateIndex(original_size);
    }

    /// @brief Returns the object size represented by a size-class index.
    ///
    /// This function decodes the logical index back into the actual byte size
    /// of the memory block. It satisfies:
    /// - `Index(Size(idx)) == idx` for all valid indices
    /// - `Size(Index(s)) >= s` for all valid sizes (not strict equality)
    ///
    /// Size is therefore a right-inverse of Index, but not a strict bijection:
    /// Index maps multiple sizes to the same class (e.g., 129→16, 160→16).
    ///
    /// @param idx The size class index to be decoded.
    /// @return The maximum byte size of the objects stored in this size class's FreeList.
    /// @pre `idx < kNumSizeClasses`.
    AM_ALWAYS_INLINE static constexpr size_t Size(size_t idx) noexcept {
        return size_table_[idx];
    }

    /// @brief Returns a bucket size after enforcing index bounds.
    ///
    /// Debugging/contract-checking interface: an out-of-range `idx` triggers
    /// `AMMALLOC_CHECK(false)` and aborts the process. The returned 0 is
    /// unreachable in practice.
    ///
    /// @param idx The size class index.
    /// @return The maximum byte size of the class, or 0 when `idx` is out of range.
    AM_ALWAYS_INLINE static size_t SafeSize(size_t idx) noexcept {
        // clang-format off
        if (idx >= kNumSizeClasses) AM_UNLIKELY {
            AMMALLOC_CHECK(false, "SizeClass::Size index {} out of range [0, {})", idx, kNumSizeClasses);
            return 0;
        }
        // clang-format on
        return size_table_[idx];
    }

    /// @brief Rounds a request up to its size-class boundary.
    ///
    /// Behavior:
    /// - For `size <= MAX_TC_SIZE`: Returns the smallest size class >= size.
    /// - For `size > MAX_TC_SIZE`: Returns `size` unchanged (passthrough).
    ///
    /// The passthrough behavior for oversize values means this function does NOT
    /// guarantee alignment for large allocations outside ThreadCache's scope.
    /// Callers must handle oversize allocations separately (e.g., direct page allocation).
    ///
    /// @param original_size User requested size.
    /// @return Aligned size class, or original size if exceeds MAX_TC_SIZE.
    AM_ALWAYS_INLINE static constexpr size_t RoundUp(size_t original_size) noexcept {
        size_t idx = Index(original_size);
        // clang-format off
        if (idx == std::numeric_limits<size_t>::max()) AM_UNLIKELY {
            return original_size;
        }
        // clang-format on

        return size_table_[idx];
    }

    /// @brief Calculates the batch size for ThreadCache/CentralCache transfers.
    ///
    /// This strategy balances lock contention and memory usage.
    /// The policy is defined per size class, not per raw request size: requests
    /// that map to the same class always get the same batch size.
    ///
    /// - Small objects: Move more objects (up to 512) to amortize the cost of locking CentralCache.
    /// - Large objects: Move fewer objects (down to 2) to prevent ThreadCache from hoarding memory.
    ///
    /// @param aligned_size Requested or already-rounded object size.
    /// @return Number of objects to move in one batch, or 0 for invalid input.
    static constexpr size_t CalculateBatchSize(size_t aligned_size) noexcept {
        // clang-format off
        if (aligned_size == 0 || aligned_size > SizeConfig::MAX_TC_SIZE) AM_UNLIKELY {
            return 0;
        }
        // clang-format on

        return BatchByIndex(Index(aligned_size));
    }

    /// @brief Calculates the Span page count used to refill a size class.
    ///
    /// This strategy determines the size of the Span (in pages) allocated by CentralCache.
    /// The policy is defined per size class, not per raw request size: requests
    /// that map to the same class always get the same span size.
    ///
    /// It ensures that a single Span can satisfy multiple batch requests from ThreadCache,
    /// reducing the frequency of accessing the global PageCache lock.
    ///
    /// @param aligned_size Requested or already-rounded object size.
    /// @return Number of pages to allocate, or 0 for invalid input.
    AM_ALWAYS_INLINE static constexpr size_t GetMovePageNum(size_t aligned_size) noexcept {
        if (aligned_size == 0 || aligned_size > SizeConfig::MAX_TC_SIZE) return 0;
        return MovePagesByIndex(Index(aligned_size));
    }

    SizeClass() = delete;

    /// Number of size classes used to size ThreadCache and CentralCache arrays.
    static constexpr size_t kNumSizeClasses = detail::CalculateIndex(SizeConfig::MAX_TC_SIZE) + 1;

    static constexpr size_t kMaxBatchSize = 512;

private:
    AM_ALWAYS_INLINE static uint16_t BatchByIndex(size_t idx) noexcept {
        return batch_table_[idx];
    }

    AM_ALWAYS_INLINE static uint16_t MovePagesByIndex(size_t idx) noexcept {
        return move_page_table_[idx];
    }

    // Small-size lookup table avoids bit arithmetic on the hottest size range.
    static constexpr auto small_index_table_ = []() consteval {
        std::array<uint8_t, SizeConfig::kSmallSizeThreshold + 1> small_index_table{};
        for (size_t sz = 0; sz <= SizeConfig::kSmallSizeThreshold; ++sz) {
            small_index_table[sz] = static_cast<uint8_t>(detail::CalculateIndex(sz));
        }
        return small_index_table;
    }();

    // Full inverse table keeps every Index-to-Size lookup constant-time.
    static constexpr auto size_table_ = []() consteval {
        std::array<uint32_t, kNumSizeClasses> size_table{};
        for (size_t idx = 0; idx < kNumSizeClasses; ++idx) {
            size_table[idx] = static_cast<uint32_t>(detail::CalculateSize(idx));
        }
        return size_table;
    }();

    // Cap per-class cached bytes at MAX_TC_SIZE, clamped to [2, kMaxBatchSize].
    static constexpr auto batch_table_ = []() consteval {
        std::array<uint16_t, kNumSizeClasses> t{};
        for (size_t idx = 0; idx < kNumSizeClasses; ++idx) {
            size_t norm = size_table_[idx];
            size_t batch = SizeConfig::MAX_TC_SIZE / norm;
            if (batch < 2) {
                batch = 2;
            }
            if (batch > kMaxBatchSize) {
                batch = kMaxBatchSize;
            }
            t[idx] = static_cast<uint16_t>(batch);
        }
        return t;
    }();

    // Size each span for about eight batch requests, with a 32 KiB floor, so
    // one span serves repeated refills; clamp to the largest retained span.
    static constexpr auto move_page_table_ = []() consteval {
        std::array<uint16_t, kNumSizeClasses> t{};
        for (size_t idx = 0; idx < kNumSizeClasses; ++idx) {
            size_t norm = size_table_[idx];
            size_t batch = batch_table_[idx];
            size_t total_objs = batch << 3;
            size_t total_bytes = total_objs * norm;
            if (total_bytes < 32 * 1024) {
                total_bytes = 32 * 1024;
            }

            size_t pages = (total_bytes + SystemConfig::PAGE_SIZE - 1) >> SystemConfig::PAGE_SHIFT;
            if (pages > PageConfig::MAX_PAGE_NUM) {
                pages = PageConfig::MAX_PAGE_NUM;
            }

            t[idx] = static_cast<uint16_t>(pages);
        }
        return t;
    }();
};

static_assert(SizeClass::Size(0) == 8);
static_assert(SizeClass::Size(15) == 128);
static_assert(SizeClass::Size(16) == 160);
static_assert(SizeClass::Size(19) == 256);
static_assert(SizeClass::Size(20) == 320);
static_assert(SizeClass::Index(SizeClass::Size(20)) == 20);
static_assert(SizeClass::Index(129) == 16);
static_assert(SizeClass::Index(150) == 16);

static_assert(SizeClass::Index(0) == 0);
static_assert(SizeClass::RoundUp(0) == 8);
static_assert(SizeClass::kNumSizeClasses <= std::numeric_limits<uint8_t>::max());

// MAX_TC_SIZE must be a power of two to land exactly on a size class boundary.
// With kStepsPerGroup=4, each power-of-2 interval is evenly divided,
// so 2^n (n>=7) is always the upper bound of some size class.
static_assert(std::has_single_bit(SizeConfig::MAX_TC_SIZE),
              "MAX_TC_SIZE must be a power of two to ensure it lands on a size class boundary");
static_assert(SizeClass::Size(SizeClass::kNumSizeClasses - 1) == SizeConfig::MAX_TC_SIZE);

// Sample class boundaries because validating all 32K sizes exceeds common
// compiler constexpr step limits.

namespace detail {

consteval bool ValidateIndexInRangeSampled() {
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t class_size = SizeClass::Size(idx);
        if (idx > 0) {
            size_t prev_class = SizeClass::Size(idx - 1);
            if (SizeClass::Index(prev_class) != idx - 1) {
                return false;
            }

            if (SizeClass::Index(prev_class + 1) != idx) {
                return false;
            }
        }

        if (SizeClass::Index(class_size) != idx) {
            return false;
        }
    }
    return true;
}

consteval bool ValidateSizeNotLessThanInputSampled() {
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t class_size = SizeClass::Size(idx);
        if (SizeClass::Size(SizeClass::Index(class_size)) != class_size) {
            return false;
        }

        if (idx > 0) {
            size_t prev_class = SizeClass::Size(idx - 1);
            // Interior sample: mid must not round down into the previous class.
            if (size_t mid = (prev_class + class_size) / 2; SizeClass::Size(SizeClass::Index(mid)) < mid) {
                return false;
            }
        }
    }
    return true;
}

consteval bool ValidateIndexIdempotentSampled() {
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t class_size = SizeClass::Size(idx);
        if (SizeClass::Index(SizeClass::Size(SizeClass::Index(class_size))) != SizeClass::Index(class_size)) {
            return false;
        }
    }
    return true;
}

consteval bool ValidateSizeMonotonic() {
    for (size_t idx = 1; idx < SizeClass::kNumSizeClasses; ++idx) {
        if (SizeClass::Size(idx) <= SizeClass::Size(idx - 1)) {
            return false;
        }
    }
    return true;
}

consteval bool ValidateRoundUpMonotonicSampled() {
    size_t prev = SizeClass::RoundUp(1);
    for (size_t idx = 0; idx < SizeClass::kNumSizeClasses; ++idx) {
        size_t class_size = SizeClass::Size(idx);
        size_t curr = SizeClass::RoundUp(class_size);
        if (curr < prev) {
            return false;
        }
        prev = curr;
    }
    return true;
}

}// namespace detail

static_assert(detail::ValidateIndexInRangeSampled(), "Index(s) must map to valid class at boundaries");
static_assert(detail::ValidateSizeNotLessThanInputSampled(), "Size(Index(s)) must be >= s at class boundaries");
static_assert(detail::ValidateIndexIdempotentSampled(), "Index(Size(Index(s))) must equal Index(s) at boundaries");
static_assert(detail::ValidateSizeMonotonic(), "Size(idx) must be strictly increasing");
static_assert(detail::ValidateRoundUpMonotonicSampled(), "RoundUp(s) must be non-decreasing at class boundaries");
}// namespace ammalloc

#endif// AMMALLOC_SIZE_CLASS_H
