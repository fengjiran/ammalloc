#include "ammalloc/page_heap_scavenger.h"
#include "ammalloc/page_cache.h"

#include <spdlog/spdlog.h>
#include <sys/mman.h>

namespace ammalloc {

void PageHeapScavenger::Start() {
    if (!scavenge_thread_.joinable()) {
        scavenge_thread_ = std::jthread([this](std::stop_token stoken) {
            ScavengeLoop(stoken);
        });
        spdlog::debug("PageHeapScavenger thread started.");
    }
}

void PageHeapScavenger::Stop() {
    if (scavenge_thread_.joinable()) {
        scavenge_thread_.request_stop();
        // Join here so Stop establishes a clear quiescent-state boundary.
        scavenge_thread_.join();
        spdlog::debug("PageHeapScavenger thread stopped.");
    }
}

void PageHeapScavenger::ScavengeLoop(std::stop_token stoken) {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stoken.stop_requested()) {
        // The stop-aware wait avoids delaying shutdown for a full scan interval.
        bool stop_requested = cv_.wait_for(lock, stoken,
                                           std::chrono::milliseconds(kScavengeIntervalMs),
                                           [&stoken] { return stoken.stop_requested(); });
        if (stop_requested) {
            break;
        }

        // Do not hold the wait mutex during a potentially long scan.
        lock.unlock();
        ScavengeOnePass();

        lock.lock();
    }
}

void PageHeapScavenger::ScavengeOnePass() {
    auto now = GetCurrentTimeMs();
    auto& page_cache = PageCache::GetInstance();
    size_t release_bytes = 0;

    for (size_t i = PageConfig::MAX_PAGE_NUM; i > 0; --i) {
        Span* head = nullptr;
        Span* tail = nullptr;
        auto& span_list = page_cache.GetSpanList(i);
        {
            std::lock_guard<std::mutex> lock(page_cache.GetMutex());
            auto* cur = span_list.begin();
            while (cur != span_list.end()) {
                auto* next = cur->next;
                if (cur->IsUsed()) {
                    spdlog::error("Scavenger: used span {} in free list.",
                                  static_cast<void*>(cur));
                    cur = next;
                    continue;
                }

                if (!cur->IsCommitted()) {
                    cur = next;
                    continue;
                }

                if (now - cur->last_used_time_ms >= kIdleThresholdMs) {
                    span_list.erase(cur);
                    // Reserve the detached Span so release/coalescing cannot claim it.
                    cur->SetUsed(true);

                    if (!head) {
                        head = cur;
                        tail = cur;
                    } else {
                        tail->next = cur;// NOLINT
                        tail = cur;
                        tail->next = nullptr;
                    }
                }
                cur = next;
            }
        }

        // Keep the system call outside the PageCache lock.
        auto* cur = head;
        while (cur) {
            void* start_ptr = cur->GetPageBaseAddr();
            size_t size = cur->page_num << SystemConfig::PAGE_SHIFT;
            if (madvise(start_ptr, size, MADV_DONTNEED) == 0) {
                cur->SetCommitted(false);
                release_bytes += size;
            } else {
                spdlog::warn("madvise MADV_DONTNEED failed for span {}",
                             static_cast<void*>(cur));
            }
            cur = cur->next;
        }

        if (head) {
            std::lock_guard<std::mutex> lock(page_cache.GetMutex());
            cur = head;
            while (cur) {
                auto* next = cur->next;
                cur->SetUsed(false);
                cur->last_used_time_ms = GetCurrentTimeMs();
                span_list.push_back(cur);
                cur = next;
            }
        }
    }

    if (release_bytes > 0) {
        spdlog::debug("Scavenger released {} MB physical memory.", release_bytes >> 20);
    }
}

}// namespace ammalloc
