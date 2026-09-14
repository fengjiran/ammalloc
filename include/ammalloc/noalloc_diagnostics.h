#ifndef AMMALLOC_NOALLOC_DIAGNOSTICS_H
#define AMMALLOC_NOALLOC_DIAGNOSTICS_H

/// @file
/// @brief Allocation-free fatal diagnostics and no-throw mutex guards for allocator core paths.
///
/// The helpers in this header deliberately avoid the regular AM_CHECK path:
/// formatting and iostream output may allocate and therefore cannot be used
/// after allocator metadata allocation or synchronization has failed.

#include "ammalloc/attributes.h"

#include <mutex>
#include <type_traits>

namespace ammalloc::detail {

/// @brief Writes a fixed diagnostic to stderr and terminates without allocating.
/// @param message Null-terminated static diagnostic text.
/// @note Intended for unrecoverable allocator infrastructure failures only.
/// @note The attribute is required on this declaration: call sites live in
///       other translation units and cannot otherwise infer that control never
///       resumes after the call.
AM_NORETURN void FatalNoAlloc(const char* message) noexcept;

/// @brief Acquires a mutex or terminates with an allocation-free diagnostic.
/// @tparam Mutex Any BasicLockable type (std::mutex, InstrumentedMutex, ...).
/// @param mutex Mutex to acquire.
/// @note A mutex failure is not recoverable as ordinary allocation OOM: the
///       ownership of shared allocator metadata can no longer be trusted.
template<typename Mutex>
void LockOrFatal(Mutex& mutex) noexcept {
    try {
        mutex.lock();
    } catch (...) {
        FatalNoAlloc("mutex::lock failed");
    }
}

/// @brief Releases a mutex or terminates with an allocation-free diagnostic.
/// @tparam Mutex Any BasicLockable type whose `unlock()` may throw.
/// @param mutex Mutex to release.
template<typename Mutex>
void UnlockOrFatal(Mutex& mutex) noexcept {
    try {
        mutex.unlock();
    } catch (...) {
        FatalNoAlloc("mutex::unlock failed");
    }
}

/// @brief RAII guard whose lock and unlock operations cannot throw.
///
/// The name states the contract callers depend on, not the failure policy:
/// `std::mutex::lock` reports synchronization infrastructure failure by
/// throwing `std::system_error`, which would terminate silently at an
/// allocator-core `noexcept` boundary. These guards convert that throw into an
/// allocation-free fatal report, so no exception can escape.
///
/// The `Mutex` template parameter defaults to `std::mutex` so existing call
/// sites keep working via CTAD; instrumented builds substitute a wrapper.
template<typename Mutex = std::mutex>
class NoThrowLockGuard {
public:
    explicit NoThrowLockGuard(Mutex& mutex) noexcept : mutex_(mutex) {
        LockOrFatal(mutex_);
    }

    NoThrowLockGuard(const NoThrowLockGuard&) = delete;
    NoThrowLockGuard& operator=(const NoThrowLockGuard&) = delete;

    ~NoThrowLockGuard() noexcept {
        UnlockOrFatal(mutex_);
    }

private:
    Mutex& mutex_;
};

/// @brief Moveless unique lock whose lock and unlock operations cannot throw.
template<typename Mutex = std::mutex>
class NoThrowUniqueLock {
public:
    explicit NoThrowUniqueLock(Mutex& mutex) noexcept : mutex_(mutex), owns_(true) {
        LockOrFatal(mutex_);
    }

    NoThrowUniqueLock(const NoThrowUniqueLock&) = delete;
    NoThrowUniqueLock& operator=(const NoThrowUniqueLock&) = delete;

    ~NoThrowUniqueLock() noexcept {
        if (owns_) {
            UnlockOrFatal(mutex_);
        }
    }

    void lock() noexcept {
        if (owns_) {
            FatalNoAlloc("NoThrowUniqueLock double lock");
        }

        LockOrFatal(mutex_);
        owns_ = true;
    }

    void unlock() noexcept {
        if (!owns_) {
            FatalNoAlloc("NoThrowUniqueLock unlock without ownership");
        }

        UnlockOrFatal(mutex_);
        owns_ = false;
    }

private:
    Mutex& mutex_;
    bool owns_;
};

// The NoThrow prefix is only trustworthy if the compiler can prove it. A
// throwing lock path would silently reinstate the defect these guards exist to
// prevent, so the guarantee is asserted instead of merely documented.
static_assert(std::is_nothrow_constructible_v<NoThrowLockGuard<std::mutex>, std::mutex&>);
static_assert(std::is_nothrow_destructible_v<NoThrowLockGuard<std::mutex>>);
static_assert(std::is_nothrow_constructible_v<NoThrowUniqueLock<std::mutex>, std::mutex&>);
static_assert(std::is_nothrow_destructible_v<NoThrowUniqueLock<std::mutex>>);

}// namespace ammalloc::detail

#endif// AMMALLOC_NOALLOC_DIAGNOSTICS_H
