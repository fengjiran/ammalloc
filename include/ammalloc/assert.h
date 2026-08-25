#ifndef AMMALLOC_ASSERT_H
#define AMMALLOC_ASSERT_H

/// @file
/// @brief Self-contained assertion macros and abort helpers for ammalloc.
///
/// This header mirrors the relevant contract from `include/utils/logging.h`
/// without depending on the AetherMind include tree. Failed checks write the
/// stable `Check failed: ...` prefix used by death tests before aborting.

#include <cstdlib>
#include <format>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace ammalloc::detail {

/// @brief Reports a failed check at a captured source location and aborts.
/// @param condition Text of the failed condition.
/// @param loc Source location of the check expression.
/// @note Use `AM_CHECK` to capture the call-site location automatically.
inline void HandleCheckFailed(std::string_view condition, std::source_location loc) {
    std::cerr << std::format("Check failed: ({}) at {}:{}:{}\n",
                             condition, loc.file_name(), loc.line(), loc.column());
    std::abort();
}

/// @brief Reports a failed check with a formatted message and aborts.
/// @tparam Args Types of the format arguments.
/// @param condition Text of the failed condition.
/// @param loc Source location of the check expression.
/// @param fmt Format string passed to `std::format`.
/// @param args Arguments consumed by `fmt`.
/// @throws std::format_error if formatting fails before the process aborts.
template<typename... Args>
void HandleCheckFailed(std::string_view condition,
                       std::source_location loc,
                       std::format_string<Args...> fmt,
                       Args&&... args) {
    std::string message = std::format(fmt, std::forward<Args>(args)...);
    std::cerr << std::format("Check failed: ({}) at {}:{}:{} [{}]\nMessage: {}\n",
                             condition, loc.file_name(), loc.line(), loc.column(),
                             loc.function_name(), message);
    std::abort();
}

}// namespace ammalloc::detail

/// @brief Evaluates an invariant and aborts when it is false.
/// @param condition Expression evaluated exactly once.
/// @param ... Optional `std::format` string and arguments for the failure
///             message.
/// @note This macro is active in every build. The `Check failed` output prefix
///       is part of the death-test contract.
#define AM_CHECK(condition, ...)                                                             \
    do {                                                                                     \
        if (!(condition)) [[unlikely]] {                                                     \
            ::ammalloc::detail::HandleCheckFailed(                                           \
                    #condition, std::source_location::current() __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                                    \
    } while (false)

/// @brief Evaluates a debug-only invariant and aborts when it is false.
/// @param condition Expression evaluated once in debug builds.
/// @param ... Optional format string and arguments, evaluated only in debug
///             builds.
/// @note In `NDEBUG` builds, neither the condition nor trailing arguments are
///       evaluated. Do not place required side effects in either position.
/// @note The release expansion consumes its internal dangling `else` when the
///       invocation ends with a semicolon, preserving an enclosing `if`/`else`.
#ifdef NDEBUG
#define AM_DCHECK(condition, ...)               \
    while (false)                                     \
        if (static_cast<bool>(condition)) [[likely]]; \
        else
#else
#define AM_DCHECK(condition, ...) AM_CHECK(condition __VA_OPT__(,) __VA_ARGS__)
#endif

/// @brief Release-visible safety guard against caller-supplied bad pointers.
///
/// Unlike `AM_DCHECK` (a debug-only internal invariant), this check protects
/// against invalid caller inputs (double-free, interior pointer, out-of-range
/// free) that can corrupt bitmap/use_count/Span metadata. It is active in every
/// debug build and in release builds only when `AM_HARDENED` is defined; a
/// failure aborts with the same `Check failed` contract as `AM_CHECK` and
/// produces no code when disabled.
#if defined(AM_HARDENED) || !defined(NDEBUG)
#define AM_HCHECK(condition, ...) AM_CHECK(condition __VA_OPT__(,) __VA_ARGS__)
#else
#define AM_HCHECK(condition, ...) ((void)0)
#endif

#endif// AMMALLOC_ASSERT_H
