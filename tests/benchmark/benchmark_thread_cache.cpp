// ThreadCache retention benchmarks: worst-case tail traversal and the
// cooperative hard-purge storm measured on the owner-thread Trim path.
#include "ammalloc/free_list.h"
#include "ammalloc/size_class.h"
#include "ammalloc/thread_cache.h"

#include <benchmark/benchmark.h>
#include <vector>

namespace {
using namespace ammalloc;

// 16-byte class geometry: batch == 512, quota ceiling == 8 * 512 == 4096.
constexpr size_t kObjSize = 16;
constexpr size_t kBatch = 512;
constexpr size_t kQuotaCap = 4096;

// Worst-case PopRangeTail traversal: a quota-full chain evicts one batch
// from the tail and immediately re-inserts it at the head, so every
// iteration repeats the same ~3584-step cut walk plus ~512-step suffix walk.
void BM_FreeList_TrimTailWorst16B(benchmark::State& state) {
    std::vector<FreeBlock> blocks(kQuotaCap);
    for (size_t i = 0; i < kQuotaCap; ++i) {
        blocks[i].next = (i + 1 < kQuotaCap) ? &blocks[i + 1] : nullptr;
    }
    FreeList list;
    // One full chain; each iteration trims the tail batch and pushes it back.
    list.PushRange(FreeChain{&blocks[0], &blocks[kQuotaCap - 1], kQuotaCap});
    for (auto _: state) {
        auto evicted = list.PopRangeTail(kBatch);
        benchmark::DoNotOptimize(evicted);
        list.PushRange(evicted);
    }
}
BENCHMARK(BM_FreeList_TrimTailWorst16B)->ThreadRange(1, 16);

// Full-stack hard-purge storm: each owner thread fills its 16-byte class to
// its quota ceiling (paused), then times Trim(kRelease, 0): one 4096-step
// tail traversal plus per-object bitmap return under the CentralCache bucket
// lock, contended across all running threads.
void BM_ThreadCache_PurgeStormRelease16B(benchmark::State& state) {
    thread_local ThreadCache tc;
    std::vector<void*> held;
    held.reserve(kQuotaCap);
    for (auto _: state) {
        state.PauseTiming();
        while (held.size() < kQuotaCap) {
            void* p = tc.Allocate(kObjSize);
            if (!p) {
                break;
            }
            held.push_back(p);
        }
        state.ResumeTiming();
        tc.Trim(ThreadCacheTrimMode::kRelease, 0);
        state.PauseTiming();
        held.clear();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_ThreadCache_PurgeStormRelease16B)->ThreadRange(1, 16);

} // namespace
