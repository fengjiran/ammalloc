//
// Created by AetherMind Team on 2/16/26.
// Performance Benchmark Tests for ammalloc using Google Benchmark
//

#include "ammalloc/ammalloc.h"
#include "ammalloc/config.h"

#include <benchmark/benchmark.h>
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

// 1. 测试极致 Fast Path (Window = 1)
// 每次循环都是 malloc -> free -> malloc -> free
// 测量的是纯 malloc+free 开销
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 1, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 1, std::malloc, std::free);

// 2. 测试 ThreadCache 稳态吞吐 (Window = 256)
// 对象在 ThreadCache 内部循环，不触发 CentralCache
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 256, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 256, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 256, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 64, 256, std::malloc, std::free);

// 3. 测试系统综合搅动率 (Window = 1024)
// 强制触发 ThreadCache 溢出，测试 CentralCache 桶锁和批量搬运性能
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1024, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 8, 1024, std::malloc, std::free);

BENCHMARK_TEMPLATE(BM_Malloc_Churn, 4096, 1024, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Churn, 4096, 1024, std::malloc, std::free);

BENCHMARK(BM_am_malloc_free_pair_random_size);
BENCHMARK(BM_std_malloc_free_pair_random_size);

// 注册测试：BatchSize 设置为 2000，远超 ThreadCache 的 max_size (512)
// 这将绝对触发 CentralCache 的慢速路径！
BENCHMARK_TEMPLATE(BM_Malloc_Deep_Churn, 8, 2000, am_malloc, am_free);
BENCHMARK_TEMPLATE(BM_Malloc_Deep_Churn, 8, 2000, std::malloc, std::free);

// 模板参数：Size (分配大小), BatchSize (单线程一次循环分配的数量)
template<size_t Size, size_t BatchSize>
void BM_am_malloc_multithread(benchmark::State& state) {
    // 这里的代码，Google Benchmark 会自动在 N 个线程中并发执行！
    // 每个线程都有自己独立的 state 和局部变量。
    // 注意：BatchSize 不能太大（例如超过 100,000），否则可能导致线程栈溢出 (Stack Overflow)。
    // 对于 BatchSize = 1000，数组大小为 8KB，在栈上绝对安全。
    std::array<void*, BatchSize> local_ptrs{};

    // 核心测试循环
    for (auto _: state) {
        // 1. 批量分配 (模拟潮汐并发)
        for (size_t i = 0; i < BatchSize; ++i) {
            local_ptrs[i] = am_malloc(Size);
            benchmark::DoNotOptimize(local_ptrs[i]);// 防止被编译器优化掉
        }

        // 2. 批量释放
        for (void* p: local_ptrs) {
            am_free(p);
        }
    }

    // 告诉框架我们实际处理了多少字节，方便输出吞吐量 (MB/s)
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

// 真实世界模拟：多线程 + 随机大小
template<size_t BatchSize>
void BM_am_malloc_multithread_random(benchmark::State& state) {
    constexpr size_t kNumSizes = 8192;
    std::array<size_t, kNumSizes> sizes;
    std::mt19937 rng(state.thread_index());             // 每个线程不同种子
    std::uniform_int_distribution<size_t> dist(1, 1024);// 1B ~ 1KB 随机
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
// 注册测试用例 (使用 UseRealTime 获取真实的并发挂钟时间)
// ============================================================================

// 测试 8 字节，每次循环分配 1000 个
// ->Threads(N) 表示启动 N 个线程同时执行这个函数
// ->UseRealTime() 告诉框架使用挂钟时间(Wall Time)而不是 CPU 时间(会把多核时间累加)
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

// 测试 64 字节
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
