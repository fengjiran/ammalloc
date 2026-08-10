//
// Created by AetherMind Team on 3/8/26.
//

#include "ammalloc/size_class.h"

#include <benchmark/benchmark.h>

namespace {

using namespace ammalloc;

AM_ALWAYS_INLINE uint64_t NextRand(uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}

void BM_SizeClass_Index_Small(benchmark::State& state) {
    uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
    for (auto _: state) {
        const size_t size = static_cast<size_t>((NextRand(rng_state) & 127ULL) + 1ULL);
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Small);

void BM_SizeClass_Index_Large(benchmark::State& state) {
    uint64_t rng_state = 0x243f6a8885a308d3ULL;
    const size_t range = SizeConfig::MAX_TC_SIZE - 128;
    for (auto _: state) {
        const size_t size = static_cast<size_t>((NextRand(rng_state) % range) + 129ULL);
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Large);

void BM_SizeClass_Size(benchmark::State& state) {
    uint64_t rng_state = 0xbf58476d1ce4e5b9ULL;
    for (auto _: state) {
        const size_t idx = static_cast<size_t>(NextRand(rng_state) % SizeClass::kNumSizeClasses);
        benchmark::DoNotOptimize(SizeClass::Size(idx));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Size);

void BM_SizeClass_RoundUp_Small(benchmark::State& state) {
    uint64_t rng_state = 0x94d049bb133111ebULL;
    for (auto _: state) {
        const size_t size = static_cast<size_t>((NextRand(rng_state) & 127ULL) + 1ULL);
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Small);

void BM_SizeClass_RoundUp_Large(benchmark::State& state) {
    uint64_t rng_state = 0xd6e8feb86659fd93ULL;
    const size_t range = SizeConfig::MAX_TC_SIZE - 128;
    for (auto _: state) {
        const size_t size = static_cast<size_t>((NextRand(rng_state) % range) + 129ULL);
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Large);

void BM_SizeClass_CalculateBatchSize(benchmark::State& state) {
    uint64_t rng_state = 0xa0761d6478bd642fULL;
    for (auto _: state) {
        const size_t idx = static_cast<size_t>(NextRand(rng_state) % SizeClass::kNumSizeClasses);
        const size_t size = SizeClass::Size(idx);
        benchmark::DoNotOptimize(SizeClass::CalculateBatchSize(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_CalculateBatchSize);

void BM_SizeClass_GetMovePageNum(benchmark::State& state) {
    uint64_t rng_state = 0xe7037ed1a0b428dbULL;
    for (auto _: state) {
        const size_t idx = static_cast<size_t>(NextRand(rng_state) % SizeClass::kNumSizeClasses);
        const size_t size = SizeClass::Size(idx);
        benchmark::DoNotOptimize(SizeClass::GetMovePageNum(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_GetMovePageNum);

}// namespace
