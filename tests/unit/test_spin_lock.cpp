#include "ammalloc/spin_lock.h"

#include <array>
#include <barrier>
#include <cstddef>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {
using ammalloc::SpinLock;

TEST(SpinLockTest, TryLockAcquiresInitiallyAndAfterUnlock) {
    SpinLock lock;
    ASSERT_TRUE(lock.try_lock());
    lock.unlock();
    ASSERT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLockTest, FailedTryLockPreservesOwnership) {
    SpinLock lock;
    lock.lock();
    // Separate callers must both fail while the original owner holds the lock.
    // Joining makes the result visible without an extra atomic result variable.
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool acquired = false;
        std::thread contender([&] {
            acquired = lock.try_lock();
            if (acquired) {
                lock.unlock();
            }
        });
        contender.join();
        EXPECT_FALSE(acquired);
    }
    lock.unlock();
    ASSERT_TRUE(lock.try_lock());
    lock.unlock();
}

TEST(SpinLockTest, MixedAcquisitionProtectsSharedState) {
    constexpr size_t kThreadCount = 8;
    constexpr size_t kIterations = 10000;
    SpinLock lock;
    std::barrier start(static_cast<std::ptrdiff_t>(kThreadCount));
    size_t count = 0;
    size_t complement = ~count;
    size_t inconsistent_reads = 0;
    std::array<std::thread, kThreadCount> workers;
    for (size_t i = 0; i < kThreadCount; ++i) {
        workers[i] = std::thread([&, i] {
            start.arrive_and_wait();
            for (size_t j = 0; j < kIterations; ++j) {
                std::unique_lock<SpinLock> guard(lock, std::defer_lock);
                if (i % 2 == 0 || !guard.try_lock()) {
                    guard.lock();
                }
                // Plain shared fields rely exclusively on the lock for publication.
                if (complement != ~count) {
                    ++inconsistent_reads;
                }
                ++count;
                complement = ~count;
            }
        });
    }
    for (auto& worker: workers) {
        worker.join();
    }
    EXPECT_EQ(count, kThreadCount * kIterations);
    EXPECT_EQ(complement, ~count);
    EXPECT_EQ(inconsistent_reads, 0);
}

}// namespace
