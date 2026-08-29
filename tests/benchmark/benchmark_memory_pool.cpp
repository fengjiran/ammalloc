//
// Performance Benchmark Tests for ammalloc using Google Benchmark
//

#include "ammalloc/ammalloc.h"
#include "ammalloc/config.h"

#include <benchmark/benchmark.h>
#include <array>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace ammalloc;

using alloc_func_type = void* (*) (size_t);
using free_func_type = void (*)(void*);

template<size_t AllocSize, size_t WindowSize, alloc_func_type alloc_func, free_func_type free_func>
void BM_Malloc_Churn(benchmark::State& state) {
    static_assert((WindowSize & (WindowSize - 1)) == 0, "WindowSize must be a power of 2");

    std::array<void*, WindowSize> window{};
    size_t i = 0;
    for (auto _: state) {
        size_t idx = i & WindowSize - 1;
        void* old_ptr = window[idx];

        window[idx] = alloc_func(AllocSize);
        benchmark::DoNotOptimize(window[idx]);
        if (old_ptr) {
            free_func(old_ptr);
        }
        ++i;
    }

    for (void* ptr: window) {
        if (ptr) {
            free_func(ptr);
        }
    }
}

template<size_t AllocSize, size_t BatchSize, alloc_func_type alloc_func, free_func_type free_func>
void BM_Malloc_Deep_Churn(benchmark::State& state) {
    std::vector<void*> ptrs;
    ptrs.reserve(BatchSize);
    for (auto _: state) {
        // Allocate
        for (size_t i = 0; i < BatchSize; ++i) {
            void* p = alloc_func(AllocSize);
            benchmark::DoNotOptimize(p);
            ptrs.push_back(p);
        }

        // Free
        for (size_t i = 0; i < BatchSize; ++i) {
            free_func(ptrs[i]);
        }

        ptrs.clear();
    }
}

void BM_am_malloc_free_pair_random_size(benchmark::State& state) {
    constexpr size_t num_sizes = 8192;
    std::vector<size_t> sizes(num_sizes);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(1, SizeConfig::MAX_TC_SIZE);
    for (size_t i = 0; i < num_sizes; ++i) {
        sizes[i] = dist(rng);
    }

    constexpr size_t window_size = 1024;
    std::array<void*, window_size> window{};
    size_t i = 0;

    for (auto _: state) {
        size_t w_idx = i & (window_size - 1);
        size_t s_idx = i & (num_sizes - 1);

        if (window[w_idx] != nullptr) {
            am_free(window[w_idx]);
        }

        window[w_idx] = am_malloc(sizes[s_idx]);
        benchmark::DoNotOptimize(window[w_idx]);
        ++i;
    }

    for (void* p: window) {
        if (p != nullptr) {
            am_free(p);
        }
    }
}

void BM_std_malloc_free_pair_random_size(benchmark::State& state) {
    constexpr size_t num_sizes = 8192;
    std::vector<size_t> sizes(num_sizes);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(1, SizeConfig::MAX_TC_SIZE);
    for (size_t i = 0; i < num_sizes; ++i) {
        sizes[i] = dist(rng);
    }

    constexpr size_t window_size = 1024;
    std::array<void*, window_size> window{};
    size_t i = 0;

    for (auto _: state) {
        size_t w_idx = i & (window_size - 1);
        size_t s_idx = i & (num_sizes - 1);

        if (window[w_idx] != nullptr) {
            std::free(window[w_idx]);
        }

        window[w_idx] = std::malloc(sizes[s_idx]);
        benchmark::DoNotOptimize(window[w_idx]);
        ++i;
    }

    for (void* p: window) {
        if (p != nullptr) {
            std::free(p);
        }
    }
}

// 1. Extreme fast path (Window = 1).
// Each loop iteration is malloc -> free -> malloc -> free, measuring the pure
// malloc+free overhead.
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 1, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 1, std::malloc, std::free);

// 2. ThreadCache steady-state throughput (Window = 256).
// Objects cycle inside ThreadCache without touching CentralCache.
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 256, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 256, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 256, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 256, std::malloc, std::free);

// 3. System-wide churn (Window = 1024).
// Forces ThreadCache overflow, exercising CentralCache bucket locks and batch
// transfers.
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1024, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1024, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 4096, 1024, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 4096, 1024, std::malloc, std::free);

BENCHMARK(BM_am_malloc_free_pair_random_size);
BENCHMARK(BM_std_malloc_free_pair_random_size);

// Registered test: BatchSize 2000 far exceeds ThreadCache's max_size (512),
// so this always hits the CentralCache slow path.
BENCHMARK_TEMPLATE(BM_Malloc_Deep_Churn, 8, 2000, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Deep_Churn, 8, 2000, std::malloc, std::free);

// Template parameters: Size (allocation size), BatchSize (per-loop allocation
// count).
template<size_t Size, size_t BatchSize>
void BM_am_malloc_multithread(benchmark::State& state) {
    // Google Benchmark runs this body concurrently in N threads; each thread
    // gets its own state and local variables.
    // Note: BatchSize must stay small enough to fit on the thread stack (e.g.
    // > 100,000 would overflow it). BatchSize = 1000 is an 8 KB array, safe.
    std::array<void*, BatchSize> local_ptrs{};

    // Core benchmark loop.
    for (auto _: state) {
        // 1. Batch allocation (simulates tidal concurrency).
        for (size_t i = 0; i < BatchSize; ++i) {
            local_ptrs[i] = am_malloc(Size);
            benchmark::DoNotOptimize(local_ptrs[i]);// Prevents the compiler from optimizing the result away.
        }

        // 2. Batch release.
        for (void* p: local_ptrs) {
            am_free(p);
        }
    }

    // Report bytes processed so the framework can print throughput (MB/s).
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(BatchSize) * static_cast<int64_t>(Size));
}

template<size_t Size, size_t BatchSize>
void BM_std_malloc_multithread(benchmark::State& state) {
    std::array<void*, BatchSize> local_ptrs{};

    for (auto _: state) {
        for (size_t i = 0; i < BatchSize; ++i) {
            local_ptrs[i] = std::malloc(Size);
            benchmark::DoNotOptimize(local_ptrs[i]);
        }
        for (size_t i = 0; i < BatchSize; ++i) {
            std::free(local_ptrs[i]);
        }
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(BatchSize) * static_cast<int64_t>(Size));
}

// Real-world simulation: multiple threads + random sizes.
template<size_t BatchSize>
void BM_am_malloc_multithread_random(benchmark::State& state) {
    constexpr size_t kNumSizes = 8192;
    std::array<size_t, kNumSizes> sizes;
    std::mt19937 rng(state.thread_index());             // Distinct seed per thread.
    std::uniform_int_distribution<size_t> dist(1, 1024);// Random 1B..1KB requests.
    for (size_t i = 0; i < kNumSizes; ++i) sizes[i] = dist(rng);

    std::array<void*, BatchSize> local_ptrs{};
    size_t s_idx = 0;

    for (auto _: state) {
        for (size_t i = 0; i < BatchSize; ++i) {
            local_ptrs[i] = am_malloc(sizes[(s_idx++) & (kNumSizes - 1)]);
            benchmark::DoNotOptimize(local_ptrs[i]);
        }
        for (size_t i = 0; i < BatchSize; ++i) {
            am_free(local_ptrs[i]);
        }
    }
}

// ============================================================================
// Registered test cases (UseRealTime reports true concurrent wall time).
// ============================================================================

// Test 8 bytes, allocating 1000 per loop.
// ->Threads(N) runs the function in N concurrent threads.
// ->UseRealTime() measures wall time instead of per-core CPU time.
BENCHMARK_TEMPLATE(BM_am_malloc_multithread, 8, 1000)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

BENCHMARK_TEMPLATE(BM_std_malloc_multithread, 8, 1000)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

// Test 64 bytes.
BENCHMARK_TEMPLATE(BM_am_malloc_multithread, 64, 1000)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

BENCHMARK_TEMPLATE(BM_std_malloc_multithread, 64, 1000)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

BENCHMARK_TEMPLATE(BM_am_malloc_multithread_random, 1000)->Threads(16)->UseRealTime();

}// namespace
