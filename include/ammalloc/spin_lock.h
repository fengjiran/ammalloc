#ifndef AMMALLOC_SPIN_LOCK_H
#define AMMALLOC_SPIN_LOCK_H

/// @file spin_lock.h
/// @brief Test-and-test-and-set spin lock for short allocator critical sections.

#include "ammalloc/attributes.h"
#include "ammalloc/common.h"

#include <atomic>
#include <thread>

namespace ammalloc {

/// @brief Provides TTAS mutual exclusion for short, non-blocking critical sections.
///
/// Contended waiters use relaxed reads and architecture pause hints before
/// attempting an acquire test-and-set, reducing cache-line bouncing. Prolonged
/// contention periodically offers the scheduler a chance to reschedule. The lock
/// is neither fair nor recursive and must not protect operations that sleep or block.
class SpinLock {
public:
    /// @brief Default-constructs an unlocked SpinLock.
    SpinLock() noexcept = default;
    /// @brief Non-copyable.
    SpinLock(const SpinLock&) = delete;
    /// @brief Non-copyable.
    SpinLock& operator=(const SpinLock&) = delete;

    /// @brief Blocks until the caller acquires the lock.
    void lock() noexcept {
        size_t spin_cnt = 0;
        while (true) {
            // Delay the cache-line-invalidating RMW until the lock appears free.
            if (!locked_.test(std::memory_order_relaxed)) {
                if (!locked_.test_and_set(std::memory_order_acquire)) {
                    return;
                }
            }

            detail::CPUPause();
            ++spin_cnt;

            // clang-format off
            if (spin_cnt > 2000) AM_UNLIKELY {
                std::this_thread::yield();
                spin_cnt = 0;
            }
            // clang-format on
        }
    }

    /// @brief Attempts to acquire the lock without waiting.
    /// @return True when the lock was acquired; false otherwise.
    bool try_lock() noexcept {
        return !locked_.test(std::memory_order_relaxed) &&
               !locked_.test_and_set(std::memory_order_acquire);
    }

    /// @brief Releases the lock with release ordering.
    /// @pre The calling thread owns the lock.
    void unlock() noexcept {
        locked_.clear(std::memory_order_release);
    }

private:
    // C++20 initializes the flag to clear and guarantees lock-free atomic operations.
    std::atomic_flag locked_{};
};

}// namespace ammalloc

#endif// AMMALLOC_SPIN_LOCK_H
