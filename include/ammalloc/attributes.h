#ifndef AMMALLOC_ATTRIBUTES_H
#define AMMALLOC_ATTRIBUTES_H

/// @file attributes.h
/// @brief Portable compiler attributes and builtin wrappers used by ammalloc.
///
/// This self-contained counterpart of `include/macros.h` keeps ammalloc's
/// public headers independent of the AetherMind include tree for standalone
/// builds and distribution.

/// @brief Tests whether the compiler provides a named builtin.
#ifdef __has_builtin
#define AM_HAS_BUILTIN(...) __has_builtin(__VA_ARGS__)
#else
#define AM_HAS_BUILTIN(...) 0
#endif

/// @brief Tests whether the compiler provides a named C++ attribute.
#ifdef __has_cpp_attribute
#define AM_HAS_CPP_ATTRIBUTE(...) __has_cpp_attribute(__VA_ARGS__)
#else
#define AM_HAS_CPP_ATTRIBUTE(...) 0
#endif

/// @brief Emits a compiler prefetch hint when supported.
/// @param addr Address to prefetch into cache.
/// @param ... Forwarded locality and read/write hints to `__builtin_prefetch`.
/// @note Expands to nothing when the compiler builtin is unavailable.
#if AM_HAS_BUILTIN(__builtin_prefetch)
#define AM_BUILTIN_PREFETCH(...) __builtin_prefetch(__VA_ARGS__)
#else
#define AM_BUILTIN_PREFETCH(...)
#endif

/// @brief Marks a return value as requiring caller consideration when supported.
#if AM_HAS_CPP_ATTRIBUTE(nodiscard)
#define AM_NODISCARD [[nodiscard]]
#else
#define AM_NODISCARD
#endif

/// @brief Marks a function as never returning to its caller when supported.
///
/// Without this, a call site in another translation unit cannot prove the
/// callee aborts, so it must keep the post-call path alive and spill state
/// across the call even though control never resumes there.
#if AM_HAS_CPP_ATTRIBUTE(noreturn)
#define AM_NORETURN [[noreturn]]
#else
#define AM_NORETURN
#endif

/// @brief Marks a control-flow path as likely when supported.
#if AM_HAS_CPP_ATTRIBUTE(likely)
#define AM_LIKELY [[likely]]
#else
#define AM_LIKELY
#endif

/// @brief Marks a control-flow path as unlikely when supported.
#if AM_HAS_CPP_ATTRIBUTE(unlikely)
#define AM_UNLIKELY [[unlikely]]
#else
#define AM_UNLIKELY
#endif

/// @brief Prevents the compiler from inlining the annotated function.
/// @note Uses `__attribute__((noinline))` on GCC/Clang, `__declspec(noinline)` on MSVC.
#if defined(__GNUC__) || defined(__clang__)
#define AM_NOINLINE __attribute__((noinline))
/// @brief Requests the compiler to always inline the annotated function.
/// @note Uses `__attribute__((always_inline)) inline` on GCC/Clang, `__forceinline` on MSVC.
#define AM_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define AM_NOINLINE __declspec(noinline)
#define AM_ALWAYS_INLINE __forceinline
#else
#define AM_NOINLINE
#define AM_ALWAYS_INLINE inline
#endif

#endif // AMMALLOC_ATTRIBUTES_H
