# 第 19 章：最终建议

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](17-references.md)  
> **本章目标**：汇总推荐终态、近期里程碑和对标判定标准。  
> **适用范围**：架构终态、最近里程碑、延后事项、竞品对标与组织执行。  
> **核心 invariant**：先可信、再兼容、再可治理与扩展，最后评估生产默认化。

## 19.1 核心结论

ammalloc 当前已经具备有价值的高性能骨架：ThreadCache LIFO、TransferCache、Span bitmap、PageMap、PageCache split/coalesce、PageAllocator 和初步 Scavenger。但它仍更接近“高性能显式 allocator 原型”，尚未达到可安全替换系统 malloc、也未达到与 TCMalloc/jemalloc 全面比较的工程成熟度。

最先决定项目成败的不是再增加一个无锁结构，而是：

1. PageMap/Span descriptor 生命周期；
2. 自举和完整 C/C++ ABI；
3. TLS/Reset/Shutdown/Fork；
4. owner 发布、锁外 OS 事务；
5. 测试真实覆盖、故障注入和统计守恒；
6. 可复现的 latency/throughput/RSS 基线。

## 19.2 推荐架构终态

~~~text
Standard malloc/new ABI        aethermind explicit arena API
          |                              |
          +--------------+---------------+
                         v
        Frontend: per-thread baseline / per-CPU optional
                         |
        Middle-end: ObjectBatch + Transfer + Central shards
                         |
        Backend: node-local region owner shards
          | SmallRun | LargeExtent | DirectMapped |
          | HugeFiller/Region optional             |
                         |
        PageAllocator: map/purge/unmap/NUMA policy

Cross-cutting:
  stable PageMap lifetime
  bootstrap-safe metadata
  accounting/control/sampling
  pressure/decay
  hardening
  test/benchmark/release
~~~

## 19.3 最近三个里程碑

### 里程碑 1：可信显式 allocator

- 修复测试假覆盖；
- TLS/OOM；
- PageMap stable metadata/writer；
- split/coalesce transaction；
- Scavenger lifecycle；
- accounting/failpoint；
- baseline。

成功定义：显式 API 在 sanitizer、并发、OOM、长时间 churn 下可靠，且现有性能护栏不退化。

### 里程碑 2：Preload-ready

- 完整 ABI；
- bootstrap；
- core 无高层依赖；
- symbols/visibility；
- fork/teardown；
- real-program compatibility；
- rollback。

成功定义：可在批准测试程序安全 preload，但仍不是全局默认。

### 里程碑 3：可扩展与可治理

- cache budgets；
- incremental decay；
- OS-out-of-lock；
- Central/PageCache shards；
- LargeExtent/region；
- stats/control/sampling；
- aethermind explicit arenas。

成功定义：吞吐、尾延迟、RSS 和诊断能力达到可与主流 allocator 公平比较。

## 19.4 暂不默认投入的项目

在前三个里程碑完成前，以下只做隔离研究：

- per-CPU/rseq；
- lock-free bitmap；
- epoch descriptor reuse；
- HugepageFiller/HugeRegion；
- explicit hugetlb 默认路径；
- 复杂 remote-owner queue；
- 每请求完整 arena；
- 运行中 allocator domain 切换。

原因不是这些方向无价值，而是其正确性、验证和收益依赖尚未建立。

## 19.5 与主流分配器的对标方法

“对标”定义为：

- API/ABI 兼容范围明确；
- 同硬件、工具链、workload、THP/NUMA；
- 相同 sampling/background 配置；
- latency p50/p99/p999；
- throughput；
- peak/steady RSS；
- internal/external/realized fragmentation；
- metadata；
- syscalls/faults/locks/TLB；
- aethermind tokens/s、TTFT/TPOT；
- 完整原始工件。

不要求每个微基准都第一；目标是针对 aethermind 和通用服务 workload 的综合 Pareto 改善。

## 19.6 组织与执行建议

- 为 P0 lifecycle/ABI 指定明确 owner；
- 性能、并发、ABI、安全评审角色分离；
- 每个阶段只引入可归因复杂度；
- 维护风险登记和 ADR；
- 建立稳定 benchmark host；
- 真实 workload trace 由 aethermind 团队共同维护；
- 发布与 on-call 在 preload 前介入；
- 文档每 milestone 更新 current state；
- 所有大改先设计评审再写代码，遵守 AGENTS.md 预审批。

## 19.7 最终判定

最合理的路线不是从当前实现直接跳到“默认替换 malloc + per-CPU + NUMA + hugepage”，而是：

~~~text
先可信
  -> 再兼容
  -> 再可观测和可治理
  -> 再扩展
  -> 最后针对 aethermind 做高阶优化和默认化
~~~

只有当正确性、ABI、自举、失败处理、生命周期、测试和发布体系共同成立，3.8 ns 级快路径才真正具有生产价值；只有当延迟、吞吐、RSS 和真实推理指标同时被测量，ammalloc 才能有意义地对标 TCMalloc、jemalloc，并成为 aethermind 的长期内存基础设施。
