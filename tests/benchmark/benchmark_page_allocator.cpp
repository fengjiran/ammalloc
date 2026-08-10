#include "ammalloc/config.h"
#include "ammalloc/page_allocator.h"

#include <array>
#include <benchmark/benchmark.h>

#define PAGE_ALLOCATOR_BENCHMARK

#ifdef PAGE_ALLOCATOR_BENCHMARK
namespace {

using namespace ammalloc;

constexpr size_t kHugePageNum = SystemConfig::HUGE_PAGE_SIZE / SystemConfig::PAGE_SIZE;
constexpr size_t kNearHugeThresholdPageNum = (SystemConfig::HUGE_PAGE_SIZE >> 1) / SystemConfig::PAGE_SIZE;

void BM_PageAllocator_NormalAllocFree_4K(benchmark::State& state) {
    constexpr size_t page_num = 1;
    for (auto _: state) {
        void* ptr = PageAllocator::SystemAlloc(page_num);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, page_num);
        }
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_AllocFree_ThresholdMinus1_NormalPath(benchmark::State& state) {
    constexpr size_t page_num = kNearHugeThresholdPageNum - 1;
    for (auto _: state) {
        void* ptr = PageAllocator::SystemAlloc(page_num);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, page_num);
        }
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_AllocFree_ThresholdAt1M_HugePathNoCache(benchmark::State& state) {
    constexpr size_t page_num = kNearHugeThresholdPageNum;
    for (auto _: state) {
        void* ptr = PageAllocator::SystemAlloc(page_num);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, page_num);
        }
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_AllocFree_2M_CacheSteadyState(benchmark::State& state) {
    PageAllocator::ReleaseHugePageCache();
    {
        void* warm = PageAllocator::SystemAlloc(kHugePageNum);
        if (warm != nullptr) {
            PageAllocator::SystemFree(warm, kHugePageNum);
        }
    }

    for (auto _: state) {
        void* ptr = PageAllocator::SystemAlloc(kHugePageNum);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, kHugePageNum);
        }
    }

    PageAllocator::ReleaseHugePageCache();
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_Alloc_2M_ColdMiss_AllocOnly(benchmark::State& state) {
    for (auto _: state) {
        state.PauseTiming();
        PageAllocator::ReleaseHugePageCache();
        state.ResumeTiming();

        void* ptr = PageAllocator::SystemAlloc(kHugePageNum);
        benchmark::DoNotOptimize(ptr);

        state.PauseTiming();
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, kHugePageNum);
        }
        PageAllocator::ReleaseHugePageCache();
        state.ResumeTiming();
    }

    PageAllocator::ReleaseHugePageCache();
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_AllocFree_2M_ColdMissRoundTrip(benchmark::State& state) {
    for (auto _: state) {
        state.PauseTiming();
        PageAllocator::ReleaseHugePageCache();
        state.ResumeTiming();

        void* ptr = PageAllocator::SystemAlloc(kHugePageNum);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, kHugePageNum);
        }

        state.PauseTiming();
        PageAllocator::ReleaseHugePageCache();
        state.ResumeTiming();
    }

    PageAllocator::ReleaseHugePageCache();
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_AllocFree_2M_MultiThreadContention(benchmark::State& state) {
    for (auto _: state) {
        void* ptr = PageAllocator::SystemAlloc(kHugePageNum);
        benchmark::DoNotOptimize(ptr);
        if (ptr != nullptr) {
            PageAllocator::SystemFree(ptr, kHugePageNum);
        }
    }
    state.SetItemsProcessed(state.iterations());
}

void BM_PageAllocator_Free_2M_CacheOverflow(benchmark::State& state) {
    constexpr size_t kBatch = 64;
    std::array<void*, kBatch> ptrs{};

    for (auto _: state) {
        state.PauseTiming();
        PageAllocator::ReleaseHugePageCache();
        for (size_t i = 0; i < kBatch; ++i) {
            ptrs[i] = PageAllocator::SystemAlloc(kHugePageNum);
        }
        state.ResumeTiming();

        for (size_t i = 0; i < kBatch; ++i) {
            if (ptrs[i] != nullptr) {
                PageAllocator::SystemFree(ptrs[i], kHugePageNum);
                ptrs[i] = nullptr;
            }
        }

        state.PauseTiming();
        PageAllocator::ReleaseHugePageCache();
        state.ResumeTiming();
    }

    PageAllocator::ReleaseHugePageCache();
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
}

BENCHMARK(BM_PageAllocator_NormalAllocFree_4K);
BENCHMARK(BM_PageAllocator_AllocFree_ThresholdMinus1_NormalPath);
BENCHMARK(BM_PageAllocator_AllocFree_ThresholdAt1M_HugePathNoCache);
BENCHMARK(BM_PageAllocator_AllocFree_2M_CacheSteadyState);
BENCHMARK(BM_PageAllocator_Alloc_2M_ColdMiss_AllocOnly);
BENCHMARK(BM_PageAllocator_AllocFree_2M_ColdMissRoundTrip);
BENCHMARK(BM_PageAllocator_Free_2M_CacheOverflow);

BENCHMARK(BM_PageAllocator_AllocFree_2M_MultiThreadContention)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

}// namespace

#endif
