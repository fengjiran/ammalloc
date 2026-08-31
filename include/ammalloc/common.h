#ifndef AMMALLOC_COMMON_H
#define AMMALLOC_COMMON_H

/// @file common.h
/// @brief Low-level address, alignment, CPU, and configuration parsing helpers.

#include "ammalloc/attributes.h"
#include "ammalloc/config.h"

#include <bit>
#include <cstdint>
#include <immintrin.h>

namespace ammalloc {
namespace detail {

/// @brief Rounds a size up to a requested alignment.
/// @param size Size in bytes. Zero maps to one alignment unit.
/// @param align Alignment in bytes.
/// @return Smallest multiple of `align` greater than or equal to `size`.
/// @pre `align` is greater than zero.
AM_NODISCARD constexpr size_t AlignUp(size_t size,
                                      size_t align = SystemConfig::ALIGNMENT) noexcept {
    // clang-format off
    if (size == 0) AM_UNLIKELY {
        return align;
    }
    // Power-of-two alignment is the allocator's common path.
    if (std::has_single_bit(align)) AM_LIKELY {
        return (size + align - 1) & ~(align - 1);
    }
    // clang-format on
    return (size + align - 1) / align * align;
}

/// @brief Maps an address to its global page index.
/// @param ptr Address to convert; null maps to page index zero.
/// @return Page index containing `ptr`.
/// @note This hot-path operation is O(1) and uses a shift for power-of-two pages.
AM_NODISCARD inline size_t PtrToPageId(void* ptr) noexcept {
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    if constexpr (std::has_single_bit(SystemConfig::PAGE_SIZE)) {
        constexpr size_t shift = std::countr_zero(SystemConfig::PAGE_SIZE);
        return addr >> shift;
    } else {
        return addr / SystemConfig::PAGE_SIZE;
    }
}

/// @brief Converts a global page index to its page-base address.
/// @param page_idx Page index to convert.
/// @return Address at the beginning of the page.
AM_NODISCARD inline void* PageIDToPtr(size_t page_idx) noexcept {
    if constexpr (std::has_single_bit(SystemConfig::PAGE_SIZE)) {
        constexpr size_t shift = std::countr_zero(SystemConfig::PAGE_SIZE);
        return reinterpret_cast<void*>(page_idx << shift);
    } else {
        return reinterpret_cast<void*>(page_idx * SystemConfig::PAGE_SIZE);
    }
}

/// @brief Issues an architecture-appropriate pause hint while spinning.
inline void CPUPause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#else
    // Preserve a compiler-visible wait point on unsupported architectures.
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/// @brief Parses a byte count with an optional binary unit suffix.
/// @param str Null-terminated value such as `1024`, `64KB`, or `16 M`.
/// @return Parsed byte count, zero for null or non-numeric input, or
///         `SIZE_MAX` when applying the suffix would overflow.
/// @note Unit matching is case-insensitive and recognizes B, K, M, G, and T.
size_t ParseSize(const char* str);

/// @brief Parses a case-insensitive Boolean configuration value.
/// @param str Null-terminated value to parse.
/// @return True for `1`, `true`, `on`, or `yes` after trimming whitespace;
///         false for null and all other values.
bool ParseBool(const char* str);

}// namespace detail

}// namespace ammalloc

#endif// AMMALLOC_COMMON_H
