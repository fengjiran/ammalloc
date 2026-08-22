# ammalloc Benchmark Framework 性能实验规范

- **状态**: Current
- **版本**: 2.0
- **日期**: 2026-08-22
- **适用范围**: ammalloc Comparative Benchmark、Diagnostic Benchmark、结果分析与性能回归
- **关联实现**: [现有 Google Benchmark 用例](../../tests/benchmark/)（部分能力已实现）；本文定义目标框架与正式结果准入规则
- **相关文档**: [架构总览](../designs/ammalloc_design.md) · [测试与验证演进方案](../improvement-plan/12-testing-and-validation.md) · [文档系统规范](documentation-guide.md)

## 1. 文档定位

本文档定义 ammalloc 性能实验的规范性契约，并指导 Benchmark Framework 的长期建设。它不是 benchmark case 清单，而是回答以下问题：

1. 什么结果可以称为正式性能结果；
2. 如何保证不同 allocator 接收等价 workload；
3. 如何隔离被测 allocator、framework 和前序实验状态；
4. 如何定义 throughput、latency、RSS、fragmentation 和硬件事件；
5. 如何判断 improvement、regression 或证据不足；
6. 如何用 ammalloc 内部指标解释外部变化，而不污染正式排名。

本文使用以下规范词：

- **必须**：违反后结果无效，不能进入正式比较或回归门禁；
- **应该**：默认遵守，偏离时必须记录理由和影响；
- **可以**：可选能力，不影响基础有效性。

当前实现尚未具备本文全部能力。未通过对应 readiness gate 的功能只能产生探索性结果，不得以目标设计冒充已验证实现。

## 2. 目标、非目标与核心原则

### 2.1 目标

Framework 最终支持：

- glibc malloc；
- jemalloc；
- Google TCMalloc；
- ammalloc；
- 具有明确 capability 和配置描述的其他 allocator。

正式实验应具备：

- 可复现的 workload；
- 进程级隔离；
- 明确的操作与指标语义；
- 可验证的环境和 allocator 配置；
- 结构化原始结果；
- 统计稳定性与不确定性表达；
- 可追溯的比较和回归结论。

### 2.2 非目标

Framework 不保证：

- 单个 microbenchmark 能代表真实应用；
- 默认配置比较等价于 allocator 的最佳可达性能；
- 开启 Diagnostic instrumentation 后的结果可用于正式排名；
- 一次运行或单个均值足以证明性能变化；
- 跨硬件、跨内核或跨编译器的绝对数值可直接回归比较。

### 2.3 核心原则

1. **相同实验语义**：不同 allocator 使用同一规范化 case、随机种子和操作生成规则。
2. **进程级隔离**：正式结果的最小执行单元是一个全新进程。
3. **测量模式分离**：throughput、latency、perf profiling 和 Diagnostic instrumentation 默认分开运行。
4. **同一时刻配对**：RSS amplification 的分子与分母必须来自同一采样时刻。
5. **先判有效，再比较数值**：环境或样本无效时不得输出 PASS。
6. **外部指标判定，内部指标归因**：ammalloc 内部统计不参与跨 allocator 排名。
7. **结论受证据约束**：结果只能支持其 workload、配置、硬件和统计区间内的结论。

## 3. 结果等级与有效性状态

### 3.1 结果等级

| 等级 | 用途 | 是否可排名 | 是否可阻断 CI |
|---|---|---:|---:|
| Exploratory | 本地调试、同进程 adapter、参数探索 | 否 | 否 |
| Controlled | 独立进程、环境有效、重复运行 | 是 | 视统计策略而定 |
| Release | 固定主机、完整元数据、基线兼容、统计门禁 | 是 | 是 |
| Diagnostic | ammalloc 内部归因或 profiling | 否 | 只能作为解释证据 |

### 3.2 运行状态

每次运行必须输出以下状态之一：

```text
VALID
INVALID_CONFIG
INVALID_ENVIRONMENT
UNSUPPORTED_CAPABILITY
EXECUTION_ERROR
INSUFFICIENT_SAMPLES
UNSTABLE_MEASUREMENT
```

比较结果必须输出以下状态之一：

```text
PASS
REGRESSION
IMPROVEMENT
INCONCLUSIVE
INCOMPATIBLE_BASELINE
```

`INVALID_*`、`UNSUPPORTED_*`、`EXECUTION_ERROR`、`INSUFFICIENT_SAMPLES` 和 `UNSTABLE_MEASUREMENT` 不得转换为 PASS。

## 4. 总体架构

```text
                         Benchmark Config
                                |
                                v
                      Benchmark Controller
                validate / expand / schedule / exec
                                |
             one fresh process per case and repetition
                                |
                                v
                        Benchmark Worker
          +---------------------+---------------------+
          |                     |                     |
   Allocator Binding      Workload Runtime      Metric Collector
          |                     |                     |
          +---------------------+---------------------+
                                |
                         Raw Result JSON
                                |
                                v
                         Result Analyzer
             validate / aggregate / compare / regress
                                |
                  JSON / CSV / Markdown / Console
```

### 4.1 Controller

Controller 必须负责：

- schema 校验与参数矩阵展开；
- environment preflight；
- case ID 和顺序生成；
- allocator、baseline/current 的交错或随机调度；
- 为每个 repetition 启动全新 Worker；
- timeout、退出状态和输出完整性校验；
- 保存原始结果，不在采集阶段覆盖数据。

Controller 不进入被测进程的 measurement path。

### 4.2 Worker

Worker 每次只执行：

```text
one allocator
x one normalized allocator configuration
x one workload case
x one measurement mode
x one repetition
```

Worker 必须在进程启动时完成 allocator binding，执行一个 case 后退出。正式 Comparative Benchmark 禁止在同一 Worker 内切换 allocator 或连续执行多个会共享 allocator 状态的 case。

### 4.3 Analyzer

Analyzer 必须先检查 schema、环境兼容性和运行有效性，再进行聚合。Analyzer 不得从自然语言日志推断关键字段。

## 5. Allocator Binding 与能力模型

### 5.1 两种绑定模式

#### Explicit Adapter

```text
Workload -> Allocator API -> concrete allocator
```

适用于：

- 开发阶段快速比较；
- ammalloc component benchmark；
- allocator 不具备 malloc replacement ABI 时的受限比较。

纳秒级 Primitive Benchmark 不得通过 virtual call 生成正式排名；应使用 direct symbol call、函数指针的可校准方案或 compile-time binding。

#### Native Replacement

```text
Workload -> malloc/free/... -> process-start allocator binding
```

适用于完整 C/C++ ABI、真实应用和正式 Comparative Benchmark。ammalloc 只有在 malloc replacement ABI、自举、fork/teardown 和兼容性验证通过后才能进入此模式。

### 5.2 Capability Model

每个 allocator backend 必须声明 capability：

```text
allocate
deallocate
calloc
realloc
aligned_alloc
posix_memalign
usable_size
native_replacement
stats
purge_or_release_control
```

规则：

- case 缺少所需 capability 时输出 `UNSUPPORTED_CAPABILITY`；
- emulation 必须显式标记，且不能与 native implementation 放入同一排名；
- zero-size、alignment、OOM、realloc preservation 和 usable-size 语义必须先通过 conformance test；
- allocator 返回失败时不得继续解引用或释放无效指针。

### 5.3 Allocator Configuration Policy

正式报告必须区分：

| 策略 | 含义 |
|---|---|
| Default | allocator 官方默认配置 |
| Matched | 尽可能对齐 background reclaim、NUMA、THP 等政策 |
| Tuned | 针对指定 workload 调优后的配置 |

不同策略不得混入同一排名。Result 必须保存 allocator 版本、构建选项、动态链接信息、环境变量和规范化运行配置。

## 6. Workload Model

### 6.1 职责分离

```text
Workload Generator
    -> deterministic operation and ownership schedule

Execution Runtime
    -> workers / barriers / affinity / stop protocol / handoff

Allocator Backend
    -> allocation operations only
```

Workload 不得依赖 allocator 身份或内部统计。相同 case 在不同 allocator 下必须由同一 seed 和同一生成算法得到等价的 size、lifetime、ownership 和阶段序列。随机数消费顺序不得依赖分配成功、线程调度或被测 allocator 返回值；失败后的停止或继续策略必须预先写入 case。

### 6.2 WorkloadConfig

规范化配置至少包括：

```cpp
struct WorkloadConfig {
    RunMode run_mode;
    uint64_t operations_per_thread;
    uint64_t duration_ns;
    uint32_t thread_count;
    uint64_t random_seed;
    uint64_t target_live_bytes;
    SizeDistribution size_distribution;
    LifetimeDistribution lifetime_distribution;
    OwnershipModel ownership_model;
    TouchPolicy touch_policy;
    double alloc_ratio;
    double free_ratio;
};
```

`Operations` 模式只能设置 `operations_per_thread`；`Duration` 模式只能设置 `duration_ns`。配置同时设置或全部缺失时输出 `INVALID_CONFIG`。

### 6.3 Size、Lifetime 与 Ownership

SizeDistribution 可以支持：

```text
Fixed / Uniform / LogUniform / Discrete / PowerLaw / TraceDriven
```

LifetimeDistribution 可以支持：

```text
Immediate / FIFO / LIFO / Random / ShortLongMixed / TraceDriven
```

OwnershipModel 可以支持：

```text
Local / ProducerConsumer / RandomRemote / RoundRobinRemote
```

概率分布必须归一化并在运行前校验。TraceDriven 输入必须记录 trace hash、版本、过滤规则和 replay scale。

每个 case 还必须声明是否以及如何触碰分配内存，包括 touch bytes、read/write pattern 和 first-touch owner。未触页的虚拟分配不能用于解释 committed RSS，编译器也不得消除 allocator call、内存访问或必要的数据依赖。

### 6.4 Cross-thread 控制实验

CrossThreadFree 和 ProducerConsumer 必须定义：

- producer、consumer 和 object 的所有权转移点；
- handoff queue 容量、同步机制和 backpressure；
- queue operation 是否计入 transaction latency；
- 停止时未消费对象的回收责任；
- no-allocation control case，用于估算 framework handoff 成本。

如果 queue 开销主导总时间，结果只能解释完整 workload，不能单独声称 allocator remote-free 性能。

## 7. Operation Accounting

每个 workload 必须在规范中声明 operation unit。通用计数至少包括：

```text
attempted_allocations
successful_allocations
failed_allocations
deallocations
reallocations
transactions
requested_bytes
successfully_allocated_requested_bytes
peak_live_allocations
peak_live_requested_bytes
```

规则：

- `alloc + free` 可以定义为一个 transaction，但同时必须报告两个 allocator calls；
- throughput 的 denominator 必须使用实际完成的 operation unit；
- OOM 不得静默计入成功吞吐；
- allocator failure 导致实际 live set 偏离规范化 schedule 时，除非 case 专门测量内存压力或失败行为，否则该运行不得进入横向性能排名；
- bytes 指标必须标记 requested、usable 或 touched；
- Duration 模式停止后必须完成已承诺的 ownership handoff 和资源清理，但清理不得计入 steady-state throughput；
- cleanup failure 必须进入结果状态。

## 8. Thread Runtime 与执行生命周期

### 8.1 生命周期

```text
Config validation
  -> Worker process start and allocator binding
  -> framework metadata preallocation
  -> worker creation
  -> per-worker CPU/NUMA binding
  -> first-touch policy
  -> readiness barrier
  -> warmup
  -> reset measurement-local counters
  -> synchronized measurement start
  -> steady-state workload
  -> synchronized measurement stop
  -> cooldown / retention observation
  -> collect and serialize
  -> controlled cleanup
  -> process exit
```

所有 worker 必须通过 readiness barrier 后才开始计时。barrier、thread creation、affinity 设置和结果序列化默认不计入 allocator steady-state throughput。

### 8.2 Warmup

Warmup 是一级概念，但不能使用固定 `3~5 s` 冒充普适标准。case 必须选择：

- duration-based warmup；
- operation-based warmup；
- state-based warmup，例如 live set 和 allocator cache 达到目标状态。

Result 必须记录 warmup 配置和实际执行量。是否保留 warmup live objects 必须由 workload 明确定义。

### 8.3 CPU 与 NUMA

CPU policy 支持 `Compact`、`Spread`、`Explicit`；NUMA policy 支持 `Local`、`Interleave`、`Remote`、`Explicit`。正式结果必须保存最终生效的 CPU mask、线程到 CPU 映射、memory policy 和 topology，而不只保存请求配置。

## 9. Measurement Mode

一次 Worker 默认只启用一种主要 measurement mode：

```text
Throughput
Latency
Memory
Perf
Diagnostic
SyscallProfile
```

多个 collector 只有在证明相互扰动可忽略时才可合并。正式报告必须标记采集组合；不同组合的结果不得直接回归比较。

## 10. Throughput 与时间语义

统一输出：

```text
elapsed_wall_ns
completed_operations
operations_per_second
wall_ns_per_op
```

公式：

```text
operations_per_second = completed_operations / elapsed_wall_seconds
wall_ns_per_op = elapsed_wall_ns / completed_operations
```

`wall_ns_per_op` 是吞吐量的倒数，不得命名为 multi-thread average latency。

Scaling efficiency 定义为：

```text
scaling_efficiency(N)
    = throughput(N) / (N * throughput(1))
```

报告必须说明 N 表示 physical cores 还是 logical CPUs，并分别处理跨 NUMA node 和 SMT 区间。

## 11. Latency Measurement

### 11.1 指标类型

必须区分：

| 类型 | 定义 |
|---|---|
| Service latency | 单次 allocator API call 的实际耗时 |
| Transaction latency | 完整 alloc/use/free 或 ownership-transfer 事务耗时 |
| Wall ns/op | 总 wall time 除以总操作数，不是 latency distribution |

### 11.2 采样规则

fast path 不得逐次调用高开销 clock。sampled latency 必须：

- 使用固定 seed 的伪随机采样，避免与周期性 refill/batch 混叠；
- 每线程使用预分配、无锁的 histogram 或 sample buffer；
- 分开记录 API type、size bucket 和必要的线程维度；
- 保存 sample count、sampling probability、clock source 和 estimated clock overhead；
- 不足以支持目标 percentile 时输出 `INSUFFICIENT_SAMPLES`；
- throughput run 与 latency run 默认分离。

可以输出 P50、P90、P95、P99、P99.9 和 max，但 max 必须附样本数，不能视为稳定统计量。

## 12. Memory Measurement

### 12.1 采集项

统一采集：

```text
RSS / peak RSS
private anonymous RSS
PSS (optional)
VmSize
live requested bytes
live allocations
page faults
post-free RSS
```

Linux 数据源可以包括 `/proc/self/status`、`/proc/self/smaps_rollup` 和 `getrusage()`。必须记录数据源、采样周期和读取失败。

### 12.2 同时刻放大率

```text
memory_amplification(t)
    = rss_basis(t) / live_requested_bytes(t)
```

`rss_basis` 必须明确选择并记录为 whole-process RSS、private-anonymous RSS 或相对 process baseline 的 RSS delta；同一比较组必须使用相同 basis。分子与分母必须来自同一采样点。禁止使用 `peak RSS / peak live bytes`，因为两个峰值可能不在同一时刻。

当 `live_requested_bytes(t) == 0` 或低于 case 定义的有效阈值时，不计算 amplification。

### 12.3 基线与 retention

Result 必须区分：

- process baseline；
- steady-state snapshot；
- peak process RSS；
- post-free RSS at fixed `delta_t`；
- mapped、committed、requested 和 touched bytes。

framework metadata、线程栈和动态库无法可靠逐项扣除时，应报告 whole-process RSS 与基线差值，而不是声称得到 allocator 独占 RSS。

## 13. Perf 与系统调用 Profiling

### 13.1 PerfCollector

首批事件可以包括：

```text
cycles / instructions
cache references / misses
LLC load misses
dTLB load misses
branch misses
page faults / context switches
```

硬件事件通常不能同时无损采集，因此应拆为多个 profiling pass。每个事件必须保存：

```text
raw_count
time_enabled
time_running
scaled_count
multiplex_ratio
availability_status
```

严重 multiplex、事件不支持或权限不足时，不得把数据进入 regression gate。

### 13.2 SystemCallCollector

`strace` 等侵入式工具只能用于 `SyscallProfile`，不得默认与 throughput 共同运行。可以归一化输出 mmap、munmap、madvise、brk per million operations。

## 14. Diagnostic Instrumentation

ammalloc-specific 指标用于解释 ThreadCache、CentralCache、PageCache、PageAllocator 和 mapping cache 行为。

硬性约束：

- Comparative build 默认关闭内部 instrumentation；
- Diagnostic build 与 Comparative build 分开标记；
- 优先 per-thread、cache-line-aligned counter，结束后聚合；
- 禁止在 fast path 为统计引入共享热点 atomic；
- counter snapshot 若非一致快照，必须在 schema 中声明；
- instrumented 与 uninstrumented 外部结果不得直接比较。

内部指标可以包括 cache hit/miss、refill/flush、slow-path、contention、page allocation、mapping cache、mmap/munmap/madvise 和 retained bytes，但字段必须在实现存在并验证后才能标记为 available。

## 15. Environment Validity Policy

### 15.1 必须保存的环境信息

```text
Git commit and dirty state
allocator and dependency versions
compiler and flags
kernel and libc
CPU model / microcode / topology
online CPUs and affinity
NUMA topology and memory policy
RAM
system page size and THP state
CPU governor / frequency / boost state
SMT state
container or VM identity
benchmark config and seed
timestamp
```

### 15.2 必须控制或验证的条件

Release 级实验应该使用固定主机，并验证：

- CPU governor、boost、SMT 与基线一致；
- 没有超出阈值的 CPU steal、迁移和 context-switch 干扰；
- affinity 实际生效；
- NUMA first-touch 和 memory policy 实际生效；
- THP、allocator background thread 和 release policy 已固定；
- thermal throttling 或系统负载未使运行失效。

仅记录差异不能使不兼容环境变得可比较。超出 policy 时输出 `INVALID_ENVIRONMENT` 或 `INCOMPATIBLE_BASELINE`。

## 16. 配置与 Case Identity

配置格式可以选择 JSON 或 YAML，但必须具有版本化 schema、严格字段校验和明确单位。禁止静默忽略未知字段。

示例：

```yaml
schema_version: 1
benchmark: mixed-size-scalability
allocator_policy: default
allocators: [glibc, jemalloc, tcmalloc, ammalloc]
workload:
  type: mixed-size
  run:
    mode: duration
    duration_seconds: 10
  target_live_bytes: 1073741824
  seed: 42
threads:
  values: [1, 2, 4, 8, 16]
measurement:
  mode: throughput
  warmup_seconds: 3
repetitions: 7
```

Case ID 必须由规范化配置计算，至少覆盖：

```text
schema version
workload and parameters
allocator policy
thread/CPU/NUMA policy
measurement mode
build and instrumentation mode
```

random seed 和 repetition ID 单独保存，使同一 case 可执行确定性重放和独立重复。

## 17. Result Schema

Result schema 从框架第一阶段开始版本化。最小结构：

```json
{
  "schema_version": 1,
  "case_id": "...",
  "run_id": "...",
  "validity": "VALID",
  "result_level": "Controlled",
  "allocator": {},
  "environment": {},
  "workload": {},
  "execution": {},
  "accounting": {},
  "metrics": {},
  "diagnostics": null,
  "warnings": []
}
```

要求：

- 单位写入字段名或 schema；
- missing、unsupported 和 zero 必须区分；
- 保存 raw repetition，聚合结果不得覆盖原始结果；
- schema 破坏性变化必须递增 major version；
- Console、CSV 和 Markdown 都由结构化结果派生。

## 18. 重复运行与统计判定

### 18.1 调度

baseline/current 或多个 allocator 应交错、配对或随机排序，避免温度、频率和系统状态随时间漂移造成固定顺序偏差。每个 repetition 使用全新 Worker。

### 18.2 描述统计

至少输出：

```text
sample count
mean
median
standard deviation
coefficient of variation
min / max
confidence interval
```

重复次数由噪声和 minimum detectable effect 决定；`5~10` 只能作为初始值，不能作为普适充分条件。

### 18.3 统一回归方向

先把指标转换为 performance loss：

```text
HigherIsBetter:
loss = (baseline - current) / baseline

LowerIsBetter:
loss = (current - baseline) / baseline
```

只有同时满足以下条件才能判定 `REGRESSION` 或 `IMPROVEMENT`：

1. environment 和 case 兼容；
2. 样本量满足要求；
3. 结果稳定性满足 policy；
4. `abs(loss)` 超过工程阈值；
5. 统计区间或预先选定的检验支持差异；
6. multiple comparisons policy 未使结论失效。

满足上述条件且 `loss > 0` 时判定 `REGRESSION`，`loss < 0` 时判定 `IMPROVEMENT`。差异未超过工程阈值且证据质量充分时输出 `PASS`；证据不足、噪声过大或统计区间跨越判定边界时输出 `INCONCLUSIVE`。不能把证据不足解释为无回归。

## 19. Benchmark Suite

### 19.1 Comparative Benchmark

用于 allocator 横向比较，关注外部可观测指标：

- Primitive：fixed-size allocate/deallocate、API语义明确的基础操作；
- Workload：MixedSize、RandomLifetime、CrossThreadFree、Burst、LongLived、Fragmentation；
- Application：真实程序、trace replay、RPC/KV/storage 等。

每个 case 必须声明操作单位、live-set、size、lifetime、ownership、failure 和 cleanup 语义。

### 19.2 Diagnostic Benchmark

用于 ammalloc 内部组件归因：

- SizeClass；
- ThreadCache；
- CentralCache；
- PageCache/PageMap；
- PageAllocator/mapping cache；
- scavenger 和内存回收机制。

Diagnostic Benchmark 不要求与其他 allocator 内部组件一一对应。

### 19.3 Google Benchmark 定位

Google Benchmark 继续承担低开销 Primitive 和 Component Benchmark。复杂 live-set、lifetime、cross-thread、RSS 和多阶段 workload 由独立 Workload Engine 承担。

## 20. Preset 与矩阵控制

| Preset | 用途 | 要求 |
|---|---|---|
| Quick | 开发反馈 | 少量 case；Exploratory 或 Controlled |
| Regression | 固定主机 CI | 固定 case、环境 policy、统计门禁 |
| Full | milestone/release | 完整矩阵与正式报告 |

矩阵不能仅做 allocator × size × thread × workload 的无界笛卡尔积。每个 preset 必须有显式 case manifest，避免运行量膨胀和多重比较失控。

核心维度可以覆盖：

```text
size: 32B / 64B / 256B / 1KiB / 4KiB / 64KiB / 1MiB / mixed
threads: 1 / physical-core ladder / optional SMT ladder
ownership: local / cross-thread / producer-consumer
lifetime: immediate / random / short-long mixed
run mode: operations / duration
```

具体线程数必须由目标主机 topology 派生，而不是无条件执行到 64 threads。

## 21. 仓库布局与现有 Benchmark 边界

目标布局：

```text
benchmark/
  controller/
  worker/
  core/
  allocators/
  workloads/
  metrics/
  result/
  analysis/
  configs/

tests/benchmark/
  existing Google Benchmark primitive/component cases
```

约束：

- `tests/benchmark/` 保留 Google Benchmark 单可执行目标和 component 归因职责；
- `benchmark/` 是未来独立实验基础设施，不得被现有 GLOB 意外并入 `ammalloc_benchmarks`；
- 迁移用例前先比较 operation semantics，不因重构改变 workload；
- framework 自身不得进入 allocator 核心库，也不得给分配热路径引入依赖。

## 22. 实施阶段与 Readiness Gate

### Phase 0：规范冻结

- versioned config/result schema；
- case identity；
- operation accounting；
- validity status；
- environment policy。

成功条件：相同输入可生成相同 case，invalid result 不会进入比较。

### Phase 1：可信 Vertical Slice

- Controller -> fresh Worker；
- Explicit Adapter；
- FixedSize 和 MixedSize；
- 分别执行 Throughput mode 和受控 steady-state Memory mode；
- glibc 与 ammalloc。

成功条件：重复运行稳定，原始结果可重放，操作与内存计数守恒。

### Phase 2：Allocator 与 Workload 扩展

- jemalloc、TCMalloc；
- capability/conformance；
- RandomLifetime、CrossThreadFree、Burst；
- allocator configuration policy。

成功条件：unsupported capability 被正确隔离，cross-thread control case 可解释。

### Phase 3：独立测量模式

- sampled latency；
- perf passes；
- syscall profiling；
- retention/fragmentation。

成功条件：collector overhead 已校准，不同 measurement mode 不被误比较。

### Phase 4：ammalloc Diagnostic

- 非侵入统计；
- external regression 到 internal root cause 的关联；
- instrumented/uninstrumented 明确隔离。

成功条件：instrumentation 开销被量化，内部统计可解释而不参与排名。

### Phase 5：Regression 与 Release

- 固定 benchmark host；
- baseline compatibility；
- paired/randomized scheduling；
- confidence/noise/multiple-comparison policy；
- milestone report。

成功条件：CI 能区分 regression、pass、inconclusive 和 invalid environment。

### Phase 6：Native 与 Application Benchmark

- malloc replacement ABI；
- preload/link-time binding；
- trace 和真实应用；
- fork/teardown/background-thread compatibility。

成功条件：通过 ABI 和真实程序兼容性 gate 后才发布 Native Comparative 结果。

## 23. 正式结果发布清单

发布任何“更快/更省内存/无回归”结论前必须确认：

- [ ] case、allocator policy 和 measurement mode 完全明确；
- [ ] 每个 repetition 来自全新 Worker；
- [ ] environment validity 为 VALID；
- [ ] allocator capability 和 API 语义已验证；
- [ ] operation accounting 守恒；
- [ ] latency 与 wall ns/op 未混淆；
- [ ] memory amplification 使用同时刻分子和分母；
- [ ] collector 组合和 instrumentation mode 一致；
- [ ] 原始 repetitions 已保存；
- [ ] 样本、噪声和统计区间支持结论；
- [ ] baseline 环境、配置、schema 和 case ID 兼容；
- [ ] 结论没有外推到未测 workload 或硬件。

## 24. 风险与权衡

| 风险 | 影响 | 控制措施 |
|---|---|---|
| Framework 过重 | 开发成本高 | 先做可信 vertical slice，再扩矩阵 |
| Instrumentation 污染 | fast path 失真 | measurement mode 和 build 隔离 |
| RSS 归因不准 | 内存排名误导 | fresh process、同刻采样、基线差值 |
| Queue 主导 cross-thread | 错归因 allocator | no-allocation control case |
| PMU multiplex | counter 失真 | 分 pass、保存 running/enabled time |
| 固定阈值误报 | CI 不稳定 | 统计区间、噪声门禁、inconclusive |
| 配置不公平 | 排名失真 | Default/Matched/Tuned 分组 |
| 矩阵爆炸 | 成本和假阳性上升 | case manifest、预先选定核心 KPI |

## 25. 结论

ammalloc Benchmark Framework 的核心不是收集更多 case，而是建立可证伪、可复现、可解释的实验协议：

```text
规范化 Case
  -> 全新进程隔离
  -> 明确 Workload 和 Operation 语义
  -> 单一 Measurement Mode
  -> 结构化 Raw Result
  -> 有效性校验
  -> 统计比较
  -> 受证据边界约束的结论
```

只有通过 capability、environment、accounting、measurement 和 statistics gate 的结果，才可以进入 allocator 排名、性能回归或发布结论。Diagnostic 数据用于解释变化，不用于替代外部性能证据。

## 26. 版本历史

| 版本 | 日期 | 变更 |
|---|---|---|
| 2.0 | 2026-08-22 | 升级为科学可信的性能实验规范：增加进程隔离、计量语义、环境与配置公平性、有效性状态及统计门禁 |
