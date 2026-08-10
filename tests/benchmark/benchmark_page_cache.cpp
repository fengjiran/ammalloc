#include "ammalloc/page_cache.h"

#include <benchmark/benchmark.h>

namespace {

using namespace ammalloc;

constexpr size_t kWarmSpanPages = PageConfig::MAX_PAGE_NUM;

void PrimeBucket(size_t page_num) {
    auto& cache = PageCache::GetInstance();

    Span* warm = cache.AllocSpan(page_num);
    if (warm != nullptr) {
        cache.ReleaseSpan(warm);
    }
}

void BM_PageCache_Alloc_ExactBucketHit(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    const size_t page_num = static_cast<size_t>(state.range(0));

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        state.PauseTiming();
        PrimeBucket(page_num);
        state.ResumeTiming();

        Span* span = cache.AllocSpan(page_num);
        benchmark::DoNotOptimize(span);

        state.PauseTiming();
        if (span != nullptr) {
            cache.ReleaseSpan(span);
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageCache_Alloc_SplitFromLarger(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    const size_t page_num = static_cast<size_t>(state.range(0));

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        state.PauseTiming();
        PrimeBucket(kWarmSpanPages);
        state.ResumeTiming();

        Span* span = cache.AllocSpan(page_num);
        benchmark::DoNotOptimize(span);

        state.PauseTiming();
        if (span != nullptr) {
            cache.ReleaseSpan(span);
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageCache_Release_NoMerge(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    const size_t page_num = static_cast<size_t>(state.range(0));

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        state.PauseTiming();
        Span* span_a = cache.AllocSpan(page_num);
        Span* span_b = cache.AllocSpan(page_num);
        Span* span_c = cache.AllocSpan(page_num);
        state.ResumeTiming();

        benchmark::DoNotOptimize(span_b);
        if (span_b != nullptr) {
            cache.ReleaseSpan(span_b);
        }

        state.PauseTiming();
        if (span_a != nullptr) {
            cache.ReleaseSpan(span_a);
        }
        if (span_c != nullptr) {
            cache.ReleaseSpan(span_c);
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageCache_Release_MergeLeftRight(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    const size_t page_num = static_cast<size_t>(state.range(0));

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        state.PauseTiming();
        Span* span_a = cache.AllocSpan(page_num);
        Span* span_b = cache.AllocSpan(page_num);
        Span* span_c = cache.AllocSpan(page_num);
        if (span_a != nullptr) {
            cache.ReleaseSpan(span_a);
        }
        if (span_c != nullptr) {
            cache.ReleaseSpan(span_c);
        }
        state.ResumeTiming();

        benchmark::DoNotOptimize(span_b);
        if (span_b != nullptr) {
            cache.ReleaseSpan(span_b);
        }
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageMap_GetSpan_Hit(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    cache.Reset();
    Span* span = cache.AllocSpan(8);
    if (span == nullptr) {
        state.SkipWithError("failed to allocate span for hit benchmark");
        return;
    }
    const size_t hit_page_id = span->start_page_idx + 3;

    for (auto _: state) {
        Span* mapped = PageMap::GetSpan(hit_page_id);
        benchmark::DoNotOptimize(mapped);
    }

    cache.ReleaseSpan(span);
    state.SetItemsProcessed(state.iterations());
}

void BM_PageMap_GetSpan_Miss(benchmark::State& state) {
    constexpr size_t miss_page_id = 0;

    for (auto _: state) {
        Span* mapped = PageMap::GetSpan(miss_page_id);
        benchmark::DoNotOptimize(mapped);
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageMap_GetSpan_Mixed90_10(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    cache.Reset();
    Span* span = cache.AllocSpan(8);
    if (span == nullptr) {
        state.SkipWithError("failed to allocate span for mixed benchmark");
        return;
    }
    const size_t hit_page_id = span->start_page_idx + 1;
    constexpr size_t miss_page_id = 0;

    size_t i = 0;
    for (auto _: state) {
        size_t page_id = ((i % 10) == 0) ? miss_page_id : hit_page_id;
        Span* mapped = PageMap::GetSpan(page_id);
        benchmark::DoNotOptimize(mapped);
        ++i;
    }

    cache.ReleaseSpan(span);
    state.SetItemsProcessed(state.iterations());
}

void BM_PageCache_AllocRelease_SameBucket_Contention(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    constexpr size_t page_num = 8;

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        Span* span = cache.AllocSpan(page_num);
        benchmark::DoNotOptimize(span);
        if (span != nullptr) {
            cache.ReleaseSpan(span);
        }
    }

    state.SetItemsProcessed(state.iterations());
}

void BM_PageCache_AllocRelease_MixedBuckets_Contention(benchmark::State& state) {
    auto& cache = PageCache::GetInstance();
    size_t i = static_cast<size_t>(state.thread_index());

    if (state.thread_index() == 0) {
        cache.Reset();
    }

    for (auto _: state) {
        const size_t page_num = (i % 32) + 1;
        Span* span = cache.AllocSpan(page_num);
        benchmark::DoNotOptimize(span);
        if (span != nullptr) {
            cache.ReleaseSpan(span);
        }
        ++i;
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_PageCache_Alloc_ExactBucketHit)->Arg(1)->Arg(8)->Arg(32)->Arg(128);
BENCHMARK(BM_PageCache_Alloc_SplitFromLarger)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_PageCache_Release_NoMerge)->Arg(1)->Arg(8)->Arg(32);
BENCHMARK(BM_PageCache_Release_MergeLeftRight)->Arg(1)->Arg(8)->Arg(32);

BENCHMARK(BM_PageMap_GetSpan_Hit);
BENCHMARK(BM_PageMap_GetSpan_Miss);
BENCHMARK(BM_PageMap_GetSpan_Mixed90_10);

BENCHMARK(BM_PageCache_AllocRelease_SameBucket_Contention)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

BENCHMARK(BM_PageCache_AllocRelease_MixedBuckets_Contention)
        ->Threads(1)
        ->Threads(2)
        ->Threads(4)
        ->Threads(8)
        ->Threads(16)
        ->UseRealTime();

}// namespace
