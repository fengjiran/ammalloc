# 第 15 章：分阶段实施路线图

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](13-engineering-and-release.md) · [下一章](15-priority-and-risk-matrix.md)  
> **本章目标**：按依赖关系和风险消减顺序安排实施阶段与退出门。  
> **适用范围**：Phase 0–7 的工作包、依赖、退出条件和 Definition of Done。  
> **核心 invariant**：后续阶段不得绕过前置 correctness、lifetime、ABI 和 measurement 门禁。

路线图以依赖关系和风险消减排序，不按功能吸引力排序。per-CPU、NUMA、HugepageFiller 等高阶优化不得绕过 ABI、自举、生命周期和基准基础。

## 15.1 总体依赖图

~~~text
Baseline & test truth
  -> ABI correctness / bootstrap
  -> PageMap + Span stable lifetime
  -> Frontend/Central lifecycle
  -> PageCache transactional correctness
  -> stats/accounting/fault injection
  -> preload-ready explicit allocator
  -> cache budgets + decay + OS-out-of-lock
  -> Central/PageCache sharding + region
  -> NUMA / per-CPU / hugepage experiments
  -> aethermind arenas + production default evaluation
~~~

旁路依赖：

- 安全基线贯穿所有阶段；
- 工程发布从第一阶段开始；
- sampling 在统计 schema 稳定后开始；
- Hardened/guarded 在默认生命周期正确后开始；
- competitive benchmark 在可复现环境建立后持续运行。

## 15.2 Phase 0：基线冻结与事实修正

目标：让后续结果可相信。

工作包：

1. 修复 CentralCache Reset 导致 TransferCache 测试假覆盖；
2. 区分 drain/reset/destroy；
3. 整理测试 fixture 和 singleton 生命周期；
4. 建立 Debug/Release/ASan/UBSan/TSan；
5. 建立 benchmark manifest/JSON/perf；
6. 校准 3.8 ns、26 ns、8.9 us/100+ GiB/s；
7. 标记 docs current/future；
8. 建立 failpoint 基础和 accounting snapshot；
9. 清理构建 target/宏不一致；
10. 冻结当前 public API 行为。

退出门：

- 测试实际覆盖声称路径；
- baseline 多轮可复现；
- 工作区/构建说明一致；
- 所有已知阻塞性审核问题有 owner 和方案。

风险：工程、性能测量、正确性。

## 15.3 Phase 1：显式分配器正确性基线

目标：`am_malloc/am_free` 可安全作为显式 allocator 使用。

工作包：

1. TLS RAII、创建 OOM、线程退出；
2. PageMap writer lock；
3. stable Span metadata；
4. split/coalesce 失败原子；
5. public pointer/large base validation；
6. Scavenger explicit detached state、stop/reset；
7. bootstrap-safe core diagnostics；
8. ThreadCache/Central/PageCache byte accounting；
9. multi-shard 保持关闭但修正 owner 输入；
10. fuzz/state-model 初版。

退出门：

- 无已知 reachable UAF、double allocation、mapping stale；
- OOM/failpoint 守恒；
- ASan/UBSan/TSan；
- 短线程/fork/shutdown；
- fast path 与随机大小护栏；
- 显式 API 长时间 stress。

交付：v0.x explicit-preview。

## 15.4 Phase 2：完整 ABI 与自举

目标：形成可测试的 malloc replacement，但不默认生产启用。

工作包：

1. C malloc family；
2. C++ new/delete family；
3. errno/zero/alignment/realloc；
4. BootstrapAllocator/recursion state；
5. sized free；
6. symbol visibility/version script；
7. preload target；
8. constructor/destructor/fork；
9. core 移除 spdlog；
10. ABI exerciser/real-program smoke。

退出门：

- ABI 测试全通过；
- early/late/reentrant allocation；
- preload 主流样本；
- domain mismatch 明确；
- public symbols准确；
- 可通过进程启动配置回退系统 allocator；
- 生产仍 opt-in。

交付：v0.x preload-preview。

## 15.5 Phase 3：内存效率与控制面

目标：在不改变架构并发复杂度的前提下治理 RSS 和缓存。

工作包：

1. Frontend total byte budget/GC；
2. Middle-end budget/adaptive prefetch；
3. bulk bitmap；
4. incremental Scavenger/decay；
5. retained/purged state；
6. stats/control schema；
7. cgroup/pressure；
8. large metadata/aligned/realloc；
9. OS syscall 移出 shard lock；
10. sampling 初版。

退出门：

- burst RSS 回落；
- refault/latency 可控；
- stats accounting 可信；
- pressure 无振荡；
- slow-path p99 改善或不退化；
- sampling 默认开销达标。

交付：v1.0 explicit-stable 候选；preload 仍灰度。

## 15.6 Phase 4：并发分片与 LargeExtent

目标：移除热点 Middle/Backend 单锁上限并降低中大型 mmap churn。

工作包：

1. Central size-class shard；
2. ObjectBatch/稳定 route；
3. refill single-flight；
4. PageCache multi-shard owner 发布；
5. PageMap multi-writer；
6. region ownership；
7. SmallRun non-empty bitmap；
8. intrusive LargeExtent size/address index；
9. fit/fragmentation；
10. multi-shard Scavenger。

退出门：

- 2～4 shard correctness/TSan；
- same-class/Backend contention 缩放；
- 无跨 shard coalesce；
- mmap/munmap 和外部碎片下降；
- metadata/retained budget；
- 16/32 thread 综合护栏。

交付：v1.x scalable-backend。

## 15.7 Phase 5：NUMA、per-CPU 与 Hugepage 实验

目标：在稳定架构上进行可归因的高阶性能实验。

并行但独立 feature flags：

- per-CPU/rseq Frontend；
- NUMA-local Central/PageCache region；
- remote free queue；
- HugepageFiller；
- HugeRegion；
- THP/hugetlb policy；
- descriptor TransferCache；
- logical page size profile。

每个实验：

1. 独立设计/不变量；
2. 独立基准；
3. 默认关闭；
4. 与现有模式 A/B；
5. 失败/unsupported 回退；
6. 不同时合并多个不可归因变化。

退出门：

- 真实 workload 综合收益；
- 尾延迟、RSS、metadata 不显著恶化；
- 平台兼容；
- 一键回退。

## 15.8 Phase 6：aethermind 内存基础设施

目标：按推理生命周期而不是通用 malloc 猜测来优化。

工作包：

1. versioned arena/domain；
2. request segment arena；
3. model arena；
4. KV/HugeRegion block pool；
5. worker/node route与 first touch；
6. pressure priority；
7. model/request sampling；
8. device/pinned domain wrapper；
9. aethermind exporter；
10. unload/reset/runbook。

退出门：

- tokens/s、TTFT/TPOT、p99、peak/steady RSS 综合收益；
- domain/reset 无 UAF；
- multi-model；
- cgroup pressure；
- canary 和 rollback；
- 运维可诊断。

交付：aethermind opt-in -> selected default。

## 15.9 Phase 7：安全与生产成熟

工作包：

- ReleaseChecked；
- pointer encoding；
- guarded sampling；
- quarantine；
- metadata protection；
- fuzz/sanitizer farm；
- ABI stability；
- package/sign/SBOM；
- LTS/backport；
- production SLO；
- incident response。

退出门：

- 默认与 Hardened profile 都有稳定 SLO；
- 漏洞/崩溃可定位；
- release qualification 自动化；
- allocator 可作为批准范围内的默认基础设施。

## 15.10 工作流与 WIP 限制

- 同一时间最多一个 P0 生命周期重构处于未稳定状态；
- 性能 feature 不与 ABI/metadata layout 大改混合；
- 每个 phase 保持可构建、可测试；
- feature flag 不是永久技术债，决策后删除无用分支；
- milestone 结束做 docs/current-state 更新；
- benchmark 数据与 commit 绑定；
- 阻塞问题不以新增优化绕过。

## 15.11 每阶段统一 DoD

- 设计不变量；
- 实现方案获批；
- 单元/并发/故障测试；
- sanitizer；
- before/after benchmark；
- RSS/metadata；
- ABI/config/docs；
- observability；
- rollout/rollback；
- known limitations；
- reviewer sign-off。

