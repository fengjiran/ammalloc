# ammalloc Benchmarks

Single executable `ammalloc_benchmarks` collecting every `benchmark_*.cpp`
under this directory via `GLOB_RECURSE`. Use `--benchmark_filter=<regex>` to
run a subset.

## Build & Run

```bash
cmake --build build --target ammalloc_benchmarks -j
./build/tests/benchmark/ammalloc_benchmarks --benchmark_filter=BM_CentralCache_Fetch.*
```

## Environment Recording

Performance-sensitive PRs must attach a `bench-env-<date>.txt` capturing the
host so reviewers can normalize the numbers.

```bash
{
  echo "== date =="; date -Iseconds
  echo "== uname =="; uname -a
  echo "== lscpu =="; lscpu
  echo "== lscpu -p =="; lscpu -p=CPU,Core,Socket,Node
  echo "== numactl =="; numactl --hardware 2>/dev/null || echo "numactl not installed"
  echo "== governor =="; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "n/a"
  echo "== transparent_hugepage =="; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "n/a"
} > bench-env-$(date +%Y%m%d).txt
```

## Core Pinning

The Contention and PurgeStorm families scale to 16 worker threads and are
sensitive to scheduler migration. Pin the process to a fixed set of physical
cores on one NUMA node when collecting numbers for regression comparison:

```bash
# 16 logical CPUs on socket 0 (adjust to your topology from lscpu -p).
taskset -c 0-15 ./build/tests/benchmark/ammalloc_benchmarks \
    --benchmark_filter=BM_CentralCache_Contention.*
```

For single-threaded microbenchmarks (`Fetch_*`, `Release_*`, `Drain_*`) pin to
one core to isolate from frequency scaling on idle cores:

```bash
taskset -c 2 ./build/tests/benchmark/ammalloc_benchmarks \
    --benchmark_filter=BM_CentralCache_Fetch_TransferHit.*
```

Record the exact `taskset` line in the PR description so results can be
reproduced.

## Counter Conventions

The CentralCache benchmarks publish a shared counter set sourced from
`CentralCacheStats` and `PageAllocatorStats`. Deltas are computed against a
pre-loop snapshot; `PrepGuard` subtracts the telemetry contributed by paused
setup work so the reported rate reflects only the timed region.

| Counter | Unit | Meaning |
|---|---|---|
| `tc_drain_Bps` | bytes/s | Bytes removed from TransferCache by `DrainTransferCaches` |
| `span_unpin_direct_rate` | spans/s | Spans emptied via `kSpanBitmap` release mode |
| `span_return_pc_rate` | spans/s | Spans handed back to `PageCache::ReleaseSpan` (both modes) |
| `fetch_hit_rate` | objects/s | Objects popped directly from TransferCache by `FetchRange` |
| `fetch_span_rate` | objects/s | Objects carved from Span bitmaps by `FetchRange`, including prefetch |
| `fetch_pc_rate` | spans/s | Spans borrowed from PageCache by `GetOneSpan` |
| `rel_overflow_rate` | objects/s | `kTransferCache` releases that fell through to bitmap because TransferCache was full |
| `pa_success_rate` | calls/s | Successful `PageAllocator::SystemAlloc` calls (mmap-level) |
| `span_rotations_rate` | rotations/s | Full-Span `erase`+`push_back` rotations in the `FetchRange` SpanList loop |
| `span_traversals_rate` | visits/s | Span visits inside the `FetchRange` SpanList traversal loop |
| `scav_passes_rate` | passes/s | Completed `PageHeapScavenger::ScavengeOnePass` invocations |
| `scav_spans_rate` | spans/s | Idle spans detached for reclamation |
| `scav_bytes_Bps` | bytes/s | Bytes actually returned to the OS via `MADV_DONTNEED` |
| `madvise_ok_rate` | calls/s | Successful `madvise` calls |
| `queue_occupancy_peak` | batches | Peak in-flight batches observed in a CrossThread handoff ring |
| `queue_overflow_events` | events | Producer stalls because the handoff ring was full |
| `tc_trim_count_rate` | trims/s | `ThreadCache::Trim` invocations (explicit trims only, not overflow) |
| `tc_trimmed_bytes_rate` | bytes/s | Bytes evicted by `ThreadCache::Trim` |
| `refill_span_objects_rate` | objects/s | Objects carved from Spans to satisfy capped refills (ForcedRefill) |
| `overflow_objects_rate` | objects/s | Objects pushed out by `DeallocateSlowPath` overflow trims |
| `exit_burst_ns` | ns/iter | Wall time of a synchronized multi-thread exit `ReleaseAll` burst |

The following counters are populated **only** in the instrumented-lock build
(see below); they read as `0` in a default build:

| Counter | Unit | Meaning |
|---|---|---|
| `spin_wait_p50_ns` / `spin_wait_p99_ns` | ns | TransferCache `SpinLock` acquire-wait percentiles |
| `spin_hold_p99_ns` | ns | TransferCache `SpinLock` hold-time p99 |
| `mutex_wait_p50_ns` / `mutex_wait_p99_ns` | ns | SpanList mutex acquire-wait percentiles |
| `mutex_hold_p99_ns` | ns | SpanList mutex hold-time p99 |

Contention benchmarks are multithreaded; only `thread_index() == 0` publishes
counters so google benchmark's cross-thread aggregation does not multiply the
global delta by the worker count.

`span_return_pc_rate` measures Span handoff to PageCache and does **not**
imply RSS was returned to the OS: PageCache may keep the empty Span cached for
reuse. The actual RSS layer is `scav_bytes_Bps`, sourced from the
`PageHeapScavenger` `MADV_DONTNEED` path. Because the scavenger only reclaims
spans idle for `kIdleThresholdMs` (10 s), a balanced steady-state workload
reuses spans before they age out and reports `scav_bytes_Bps = 0`; observable
reclamation needs an idle-aging workload such as `steady_stress` below.

## Reclamation Layers

The three reporting layers answer different questions and must not be conflated:

| Layer | Counter | Question answered |
|---|---|---|
| TransferCache drain | `tc_drain_Bps` | How many cached bytes left the middle-end transfer tier? |
| Span handoff | `span_return_pc_rate` | How many empty Spans went back to PageCache (still resident)? |
| RSS reclamation | `scav_bytes_Bps` | How many bytes actually returned to the OS via `madvise`? |

## Instrumented Lock Build

A separate build records per-lock wait/hold histograms so contention can be
attributed to the TransferCache `SpinLock` versus the SpanList mutex. It adds
two `steady_clock` reads per lock operation and therefore **must not** be used
for absolute throughput numbers; use it only to compare against a default-build
baseline run of the same filter.

```bash
cmake -S . -B build-instrumented -DAMMALLOC_BENCH_INSTRUMENT_LOCKS=ON
cmake --build build-instrumented --target ammalloc_benchmarks -j
./build-instrumented/tests/benchmark/ammalloc_benchmarks \
    --benchmark_filter='BM_CentralCache_Contention' --benchmark_min_time=0.05s
```

The `spin_*` and `mutex_*` percentile counters are non-zero only in this build;
a default build reports `0` for them. Histograms are reset in each scenario's
`Setup`, so the percentiles reflect that scenario alone.

## Startup Target

Startup, degradation, and long-run stress scenarios live in a separate
executable (`ammalloc_bench_startup`) with its own `main()`. Each `--mode` runs
in a fresh process so cold-init timing is clean and the process-wide
`g_mock_*` fault injection cannot leak into other scenarios.

```bash
cmake --build build --target ammalloc_bench_startup -j
BIN=./build/tests/benchmark/startup/ammalloc_bench_startup
$BIN --mode=cold_init                 # process-start to first-allocation latency
$BIN --mode=reset_latency --iters=2000  # CentralCache+PageCache Reset min/p50/p99/max
$BIN --mode=degraded_no_tc --iters=20000          # SpanList-only (no TransferCache)
$BIN --mode=degraded_partial_fetch --iters=20000 --cap=1   # capped refills
$BIN --mode=degraded_zero_return --iters=20000    # am_malloc returns nullptr path
$BIN --mode=steady_stress --seconds=30            # RSS peak + scavenger telemetry
```

`scripts/run_startup_bench.sh` spawns every mode `N` times and writes a CSV for
cross-run comparison. The degraded modes require the `AMMALLOC_TEST` compile
definition, which the startup target sets privately.

## Size Class Sweep

Fetch/Release/Drain families sweep five aligned size classes covering the
linear, geometric, and large-object regions:

| Size (B) | Batch B | Transfer capacity 8B |
|---:|---:|---:|
| 16 | 512 | 4096 |
| 64 | 512 | 4096 |
| 256 | 128 | 1024 |
| 4096 | 8 | 64 |
| 32768 | 2 | 16 |

Batch values are compile-time asserted in `benchmark_central_cache.cpp` so a
future `SizeClass` table change surfaces as a build error rather than silent
benchmark drift.

## Reporting Template

Attach to performance-affecting PRs:

```
Host: <paste bench-env summary>
Pinning: <taskset line>
Build: <Debug|Release>, commit <sha>
Filter: <--benchmark_filter value>

| Benchmark | Args | ns/op | items/s | fetch_hit_rate | span_return_pc_rate |
|---|---|---:|---:|---:|---:|
| ... | ... | ... | ... | ... | ... |
```
