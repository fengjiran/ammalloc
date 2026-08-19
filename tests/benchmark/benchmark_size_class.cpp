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

// Size-ladder boundaries where branch direction or bit arithmetic changes:
// zero special case, linear/geometric edge, table/arithmetic threshold, and
// the MAX_TC_SIZE edges (out-of-range returns max / passthrough).
constexpr size_t kBoundarySizes[] = {
        0, 1, 128, 129, SizeConfig::kSmallSizeThreshold, SizeConfig::kSmallSizeThreshold + 1,
        SizeConfig::MAX_TC_SIZE, SizeConfig::MAX_TC_SIZE + 1};

// Benchmark taxonomy: Small = [1, 128] (linear lookup-table region),
// Mid = [129, kSmallSizeThreshold] (geometric region, still table-served),
// Large = (kSmallSizeThreshold, MAX_TC_SIZE] (pure arithmetic formula).

void BM_SizeClass_Index_Small(benchmark::State& state) {
    uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
    for (auto _: state) {
        const auto size = static_cast<size_t>((NextRand(rng_state) & 127ULL) + 1ULL);
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Small);

void BM_SizeClass_Index_Mid(benchmark::State& state) {
    uint64_t rng_state = 0x8f4f7d1c3b2a9e05ULL;
    const size_t range = SizeConfig::kSmallSizeThreshold - 128;
    for (auto _: state) {
        const auto size = static_cast<size_t>((NextRand(rng_state) % range) + 129ULL);
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Mid);

void BM_SizeClass_Index_Large(benchmark::State& state) {
    uint64_t rng_state = 0x243f6a8885a308d3ULL;
    const size_t range = SizeConfig::MAX_TC_SIZE - SizeConfig::kSmallSizeThreshold;
    for (auto _: state) {
        const auto size =
                static_cast<size_t>((NextRand(rng_state) % range) + SizeConfig::kSmallSizeThreshold + 1ULL);
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Large);

void BM_SizeClass_Index_Mixed(benchmark::State& state) {
    // Arg = percentage of table-path requests; measures threshold-branch
    // predictability, the dominant mapping cost under realistic workloads.
    const auto small_pct = static_cast<uint64_t>(state.range(0));
    constexpr size_t small_range = SizeConfig::kSmallSizeThreshold;
    constexpr size_t large_range = SizeConfig::MAX_TC_SIZE - SizeConfig::kSmallSizeThreshold;
    uint64_t rng_state = 0x4d595df4d0f33173ULL;
    for (auto _: state) {
        size_t size;
        if (NextRand(rng_state) % 100 < small_pct) {
            size = NextRand(rng_state) % small_range + 1;
        } else {
            size = NextRand(rng_state) % large_range + SizeConfig::kSmallSizeThreshold + 1;
        }
        benchmark::DoNotOptimize(SizeClass::Index(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Index_Mixed)->Args({90})->Args({50});

void BM_SizeClass_Index_Boundaries(benchmark::State& state) {
    size_t i = 0;
    for (auto _: state) {
        const size_t size = kBoundarySizes[i++ % std::size(kBoundarySizes)];
        benchmark::DoNotOptimize(SizeClass::Index(size));
    }
}
BENCHMARK(BM_SizeClass_Index_Boundaries);

void BM_SizeClass_Size(benchmark::State& state) {
    uint64_t rng_state = 0xbf58476d1ce4e5b9ULL;
    for (auto _: state) {
        const auto idx = static_cast<size_t>(NextRand(rng_state) % SizeClass::kNumSizeClasses);
        benchmark::DoNotOptimize(SizeClass::Size(idx));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_Size);

void BM_SizeClass_RoundUp_Small(benchmark::State& state) {
    uint64_t rng_state = 0x94d049bb133111ebULL;
    for (auto _: state) {
        const auto size = static_cast<size_t>((NextRand(rng_state) & 127ULL) + 1ULL);
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Small);

void BM_SizeClass_RoundUp_Mid(benchmark::State& state) {
    uint64_t rng_state = 0xa4b1c2d3e4f5a607ULL;
    constexpr size_t range = SizeConfig::kSmallSizeThreshold - 128;
    for (auto _: state) {
        const auto size = static_cast<size_t>((NextRand(rng_state) % range) + 129ULL);
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Mid);

void BM_SizeClass_RoundUp_Large(benchmark::State& state) {
    uint64_t rng_state = 0xd6e8feb86659fd93ULL;
    constexpr size_t range = SizeConfig::MAX_TC_SIZE - SizeConfig::kSmallSizeThreshold;
    for (auto _: state) {
        const auto size =
                static_cast<size_t>((NextRand(rng_state) % range) + SizeConfig::kSmallSizeThreshold + 1ULL);
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Large);

void BM_SizeClass_RoundUp_Mixed(benchmark::State& state) {
    const auto small_pct = static_cast<uint64_t>(state.range(0));
    constexpr size_t small_range = SizeConfig::kSmallSizeThreshold;
    constexpr size_t large_range = SizeConfig::MAX_TC_SIZE - SizeConfig::kSmallSizeThreshold;
    uint64_t rng_state = 0x715a2b3c4d5e6f07ULL;
    for (auto _: state) {
        size_t size;
        if (NextRand(rng_state) % 100 < small_pct) {
            size = NextRand(rng_state) % small_range + 1;
        } else {
            size = NextRand(rng_state) % large_range + SizeConfig::kSmallSizeThreshold + 1;
        }
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Mixed)->Args({90})->Args({50});

void BM_SizeClass_RoundUp_Boundaries(benchmark::State& state) {
    size_t i = 0;
    for (auto _: state) {
        const size_t size = kBoundarySizes[i++ % std::size(kBoundarySizes)];
        benchmark::DoNotOptimize(SizeClass::RoundUp(size));
    }
}
BENCHMARK(BM_SizeClass_RoundUp_Boundaries);

void BM_SizeClass_CalculateBatchSize(benchmark::State& state) {
    uint64_t rng_state = 0xa0761d6478bd642fULL;
    for (auto _: state) {
        const auto idx = NextRand(rng_state) % SizeClass::kNumSizeClasses;
        const size_t size = SizeClass::Size(idx);
        benchmark::DoNotOptimize(SizeClass::CalculateBatchSize(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_CalculateBatchSize);

void BM_SizeClass_GetMovePageNum(benchmark::State& state) {
    uint64_t rng_state = 0xe7037ed1a0b428dbULL;
    for (auto _: state) {
        const auto idx = NextRand(rng_state) % SizeClass::kNumSizeClasses;
        const size_t size = SizeClass::Size(idx);
        benchmark::DoNotOptimize(SizeClass::GetMovePageNum(size));
        benchmark::DoNotOptimize(rng_state);
    }
}
BENCHMARK(BM_SizeClass_GetMovePageNum);

}// namespace
