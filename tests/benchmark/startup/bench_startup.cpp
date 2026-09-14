/// @file bench_startup.cpp
/// @brief Isolated-process startup, degradation, and stress benchmarks.
///
/// These scenarios cannot live in the steady-state google-benchmark executable:
/// cold-init timing must run in a fresh process, and the fault-injection modes
/// flip process-wide `g_mock_*` hooks that would corrupt any benchmark sharing
/// the address space. Each `--mode` is meant to be spawned as its own process
/// (see scripts/run_startup_bench.sh).

#include "ammalloc/ammalloc.h"
#include "ammalloc/central_cache.h"
#include "ammalloc/page_allocator.h"
#include "ammalloc/page_cache.h"
#include "ammalloc/page_heap_scavenger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ammalloc;
using Clock = std::chrono::steady_clock;

// Captured during this TU's dynamic initialization, before main() runs, so the
// cold_init mode can measure process-start-to-first-allocation latency.
const Clock::time_point kProcessEpoch = Clock::now();

constexpr size_t kObjSize = 64;

double UsSince(const Clock::time_point& start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// Parses VmRSS (in kB) from /proc/self/status; returns 0 when unavailable.
size_t ReadVmRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            const char* p = line.c_str() + 6;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            return static_cast<size_t>(std::strtoull(p, nullptr, 10));
        }
    }
    return 0;
}

void ReportPercentiles(const char* label, std::vector<double>& samples_us) {
    if (samples_us.empty()) {
        std::printf("mode=%s no_samples\n", label);
        return;
    }
    std::sort(samples_us.begin(), samples_us.end());
    const auto pick = [&](double frac) {
        const size_t idx = static_cast<size_t>(frac * static_cast<double>(samples_us.size() - 1));
        return samples_us[idx];
    };
    std::printf("mode=%s n=%zu min_us=%.3f p50_us=%.3f p99_us=%.3f max_us=%.3f\n",
                label, samples_us.size(), samples_us.front(), pick(0.50), pick(0.99),
                samples_us.back());
}

// ---------- Mode: cold_init ----------
// Measures latency from process start (static-init epoch) to the first
// successful allocation, which lazily constructs the CentralCache/PageCache
// singletons and their TransferCache backing.
int ModeColdInit() {
    const auto t0 = Clock::now();
    void* p = am_malloc(kObjSize);
    const double first_alloc_us = UsSince(t0);
    const double since_process_us =
            std::chrono::duration<double, std::micro>(Clock::now() - kProcessEpoch).count();
    const bool ok = (p != nullptr);
    am_free(p);
    std::printf("mode=cold_init ok=%d first_alloc_us=%.3f since_static_init_us=%.3f\n",
                ok ? 1 : 0, first_alloc_us, since_process_us);
    return ok ? 0 : 1;
}

// ---------- Mode: reset_latency ----------
// Populates the allocator, then times CentralCache::Reset + PageCache::Reset
// across many rounds to characterize controlled-teardown latency.
int ModeResetLatency(size_t iters) {
    std::vector<double> samples;
    samples.reserve(iters);
    std::vector<void*> warm(4096, nullptr);
    for (size_t i = 0; i < iters; ++i) {
        for (auto& p : warm) {
            p = am_malloc(kObjSize);
        }
        for (auto* p : warm) {
            am_free(p);
        }
        am_thread_cache_purge();

        const auto t0 = Clock::now();
        CentralCache::GetInstance().Reset();
        PageCache::GetInstance().Reset();
        samples.push_back(UsSince(t0));
    }
    ReportPercentiles("reset_latency", samples);
    return 0;
}

// ---------- Mode: degraded_no_tc ----------
// Forces the TransferCache backing allocation to fail during Reset so the
// CentralCache degrades to the SpanList-only slow path, then measures
// steady-state alloc/free throughput without the O(1) transfer tier.
int ModeDegradedNoTransferCache(size_t iters) {
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    CentralCache::GetInstance().Reset();
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);

    std::vector<void*> objs(256, nullptr);
    const auto t0 = Clock::now();
    size_t success = 0;
    for (size_t i = 0; i < iters; ++i) {
        for (auto& p : objs) {
            p = am_malloc(kObjSize);
            if (p) {
                ++success;
            }
        }
        for (auto* p : objs) {
            am_free(p);
        }
    }
    const double elapsed_us = UsSince(t0);
    const double total_ops = static_cast<double>(iters * objs.size());
    std::printf("mode=degraded_no_tc iters=%zu success=%zu ns_per_op=%.3f\n",
                iters, success, (elapsed_us * 1000.0) / total_ops);
    return 0;
}

// ---------- Mode: degraded_partial_fetch ----------
// Caps every CentralCache FetchRange at `cap` objects so the front end refills
// in small slices, then measures steady-state alloc/free throughput.
int ModeDegradedPartialFetch(size_t iters, size_t cap) {
    g_mock_fetch_range_cap.store(cap, std::memory_order_relaxed);

    std::vector<void*> objs(256, nullptr);
    const auto t0 = Clock::now();
    size_t success = 0;
    for (size_t i = 0; i < iters; ++i) {
        for (auto& p : objs) {
            p = am_malloc(kObjSize);
            if (p) {
                ++success;
            }
        }
        for (auto* p : objs) {
            am_free(p);
        }
    }
    const double elapsed_us = UsSince(t0);
    g_mock_fetch_range_cap.store(0, std::memory_order_relaxed);
    const double total_ops = static_cast<double>(iters * objs.size());
    std::printf("mode=degraded_partial_fetch cap=%zu iters=%zu success=%zu ns_per_op=%.3f\n",
                cap, iters, success, (elapsed_us * 1000.0) / total_ops);
    return 0;
}

// ---------- Mode: degraded_zero_return ----------
// Fails every page allocation so am_malloc cannot obtain a Span and returns
// nullptr; measures the cost of the fully-degraded rejection path.
int ModeDegradedZeroReturn(size_t iters) {
    g_mock_normal_alloc_fail.store(true, std::memory_order_relaxed);
    g_mock_huge_alloc_fail.store(true, std::memory_order_relaxed);

    const auto t0 = Clock::now();
    size_t non_null = 0;
    for (size_t i = 0; i < iters; ++i) {
        void* p = am_malloc(kObjSize);
        if (p) {
            ++non_null;
            am_free(p);
        }
    }
    const double elapsed_us = UsSince(t0);
    g_mock_normal_alloc_fail.store(false, std::memory_order_relaxed);
    g_mock_huge_alloc_fail.store(false, std::memory_order_relaxed);
    std::printf("mode=degraded_zero_return iters=%zu non_null=%zu ns_per_call=%.3f\n",
                iters, non_null, (elapsed_us * 1000.0) / static_cast<double>(iters));
    // A healthy rejection path returns nullptr every time.
    return non_null == 0 ? 0 : 1;
}

// ---------- Mode: steady_stress ----------
// Runs a long-lived alloc/free workload while a sampler thread records VmRSS
// and the scavenger telemetry, producing the actual-RSS-reclamation signal that
// the in-process benchmarks cannot observe.
int ModeSteadyStress(size_t seconds) {
    PageHeapScavenger::ResetStats();
    PageHeapScavenger::GetInstance().Start();

    std::atomic<bool> stop{false};
    std::atomic<size_t> peak_rss_kb{0};
    std::thread sampler([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const size_t rss = ReadVmRssKb();
            size_t prev = peak_rss_kb.load(std::memory_order_relaxed);
            while (rss > prev &&
                   !peak_rss_kb.compare_exchange_weak(prev, rss, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::vector<void*> objs(4096, nullptr);
    int64_t ops = 0;
    const auto start = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - start).count() <
           static_cast<double>(seconds)) {
        for (auto& p : objs) {
            p = am_malloc(kObjSize);
        }
        for (auto* p : objs) {
            am_free(p);
        }
        ops += static_cast<int64_t>(objs.size());
    }

    stop.store(true, std::memory_order_relaxed);
    sampler.join();
    PageHeapScavenger::GetInstance().Stop();

    const auto& scav = PageHeapScavenger::GetStats();
    const double elapsed_s =
            std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("mode=steady_stress seconds=%.1f ops=%lld ops_per_s=%.0f "
                "peak_rss_kb=%zu scav_passes=%zu scav_spans=%zu scav_bytes=%zu "
                "madvise_ok=%zu\n",
                elapsed_s, static_cast<long long>(ops),
                static_cast<double>(ops) / (elapsed_s > 0 ? elapsed_s : 1.0),
                peak_rss_kb.load(std::memory_order_relaxed),
                scav.scavenge_passes.load(std::memory_order_relaxed),
                scav.scavenged_spans.load(std::memory_order_relaxed),
                scav.scavenged_bytes.load(std::memory_order_relaxed),
                scav.madvise_success_count.load(std::memory_order_relaxed));
    return 0;
}

void PrintUsage() {
    std::printf(
            "usage: ammalloc_bench_startup --mode=<name> [--iters=N] [--seconds=N] [--cap=K]\n"
            "  modes: cold_init | reset_latency | degraded_no_tc | degraded_partial_fetch\n"
            "         | degraded_zero_return | steady_stress\n");
}

}// namespace

int main(int argc, char** argv) {
    std::string mode;
    size_t iters = 20000;
    size_t seconds = 30;
    size_t cap = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--mode=", 0) == 0) {
            mode = arg.substr(7);
        } else if (arg.rfind("--iters=", 0) == 0) {
            iters = std::strtoull(arg.c_str() + 8, nullptr, 10);
        } else if (arg.rfind("--seconds=", 0) == 0) {
            seconds = std::strtoull(arg.c_str() + 10, nullptr, 10);
        } else if (arg.rfind("--cap=", 0) == 0) {
            cap = std::strtoull(arg.c_str() + 6, nullptr, 10);
        }
    }

    // Dispatch without touching ammalloc before the selected mode's own first
    // allocation, so cold_init keeps a clean lazy-init path.
    if (mode == "cold_init") {
        return ModeColdInit();
    }
    if (mode == "reset_latency") {
        return ModeResetLatency(iters);
    }
    if (mode == "degraded_no_tc") {
        return ModeDegradedNoTransferCache(iters);
    }
    if (mode == "degraded_partial_fetch") {
        return ModeDegradedPartialFetch(iters, cap);
    }
    if (mode == "degraded_zero_return") {
        return ModeDegradedZeroReturn(iters);
    }
    if (mode == "steady_stress") {
        return ModeSteadyStress(seconds);
    }

    PrintUsage();
    return 2;
}
