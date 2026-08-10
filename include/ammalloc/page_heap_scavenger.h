#ifndef AMMALLOC_PAGE_HEAP_SCAVENGER_H
#define AMMALLOC_PAGE_HEAP_SCAVENGER_H

/// @file
/// @brief Background reclamation of physical pages from idle PageCache spans.

#include <condition_variable>
#include <stop_token>
#include <thread>

namespace ammalloc {

/// @brief Runs periodic `MADV_DONTNEED` reclamation for idle free spans.
///
/// The singleton owns one `std::jthread`. Each pass temporarily removes an idle
/// span under its PageCache shard lock, performs `madvise` without that lock,
/// and then returns the span to its owner shard.
class PageHeapScavenger {
public:
    /// @brief Returns the process-wide scavenger instance.
    /// @return Reference to the singleton stored without allocator recursion.
    static PageHeapScavenger& GetInstance() {
        alignas(alignof(PageHeapScavenger)) static char storage[sizeof(PageHeapScavenger)];
        static PageHeapScavenger* instance = new (storage) PageHeapScavenger();
        return *instance;
    }

    PageHeapScavenger(const PageHeapScavenger&) = delete;
    PageHeapScavenger& operator=(const PageHeapScavenger&) = delete;

    /// @brief Starts the background thread if it is not already running.
    /// @throws std::system_error if the operating system cannot create the thread.
    /// @note Calls to `Start` and `Stop` must be externally serialized.
    void Start();

    /// @brief Requests shutdown and joins the background thread.
    /// @note Calls to `Start` and `Stop` must be externally serialized.
    void Stop();

private:
    PageHeapScavenger() = default;

    void ScavengeLoop(std::stop_token stoken);
    static void ScavengeOnePass();

    std::jthread scavenge_thread_;
    std::condition_variable_any cv_;
    std::mutex mutex_;

    /// Delay between scavenging passes.
    static constexpr uint64_t kScavengeIntervalMs = 1000;
    /// Minimum idle time before a committed free span is reclaimed.
    static constexpr uint64_t kIdleThresholdMs = 10000;
};

}// namespace ammalloc

#endif// AMMALLOC_PAGE_HEAP_SCAVENGER_H
