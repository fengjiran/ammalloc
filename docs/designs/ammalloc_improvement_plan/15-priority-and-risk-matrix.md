# 第 16 章：优先级与风险矩阵

> [总索引](README.md) · [上一章](14-implementation-roadmap.md) · [下一章](16-implementation-principles.md)  
> **本章目标**：统一优先级、风险类型、验收证据和动态调整规则。  
> **适用范围**：P0–P3 事项、风险登记、owner、门禁和重排规则。  
> **核心 invariant**：优先级由严重性、依赖和证据驱动，不由功能吸引力驱动。

优先级由“错误可导致什么”和“后续工作是否依赖它”共同决定。P0/P1 未关闭前，高阶性能项仅允许隔离研究，不进入默认实现。

## 16.1 定义

| 优先级 | 定义 |
|---|---|
| P0 | 可能导致 UAF、heap corruption、递归崩溃、错误 unmap、进程无法启动 |
| P1 | 阻碍安全部署、ABI 兼容、可靠 OOM、验证或关键性能护栏 |
| P2 | 扩展性、RSS、碎片和生产可观测性的主要缺口 |
| P3 | 高阶/平台特定优化，需数据证明 |

## 16.2 详细矩阵

| ID | 优先级 | 工作项 | 风险类型 | 依赖 | 主要验收 |
|---|---|---|---|---|---|
| R01 | P0 | PageMap reader 与 Span stable lifetime | 并发、内存 | 无 | stale reader/UAF 模型测试 |
| R02 | P0 | 多 writer 协议 | 并发 | R01 | multi-shard TSan/发布事务 |
| R03 | P0 | split/coalesce failure atomicity | 正确性、内存 | R01 | 每提交点 failpoint |
| R04 | P0 | TLS RAII/OOM/退出 | 正确性、内存 | 无 | 短线程/OOM/late TLS |
| R05 | P0 | Bootstrap/递归安全诊断 | 正确性、兼容性 | 无 | early/reentrant/preload |
| R06 | P0 | unknown/interior/double free 策略 | 安全、正确性 | R01 | ABI/hardening |
| R07 | P0 | Scavenger detached/stop/reset | 并发、正确性 | R01 | purge/reset/fork |
| R08 | P0 | owner 在首次发布前确定 | 正确性、并发 | R02 | non-zero shard |
| R09 | P0 | OS unmap 与 PageMap 顺序 | 内存、并发 | R01 | direct/large race |
| R10 | P1 | C/C++ ABI 完整性 | 兼容性、正确性 | R04-R06 | ABI exerciser |
| R11 | P1 | Central reset 测试假覆盖 | 测试、正确性 | 无 | Transfer hit diagnostics |
| R12 | P1 | 故障注入/状态模型 | 测试 | R01-R09 | deterministic reproduction |
| R13 | P1 | sanitizer/TSan/CI | 工程、正确性 | 基线 | 自动门禁 |
| R14 | P1 | 可复现性能基线 | 性能 | 无 | manifest/JSON/noise |
| R15 | P1 | core 移除 spdlog | 递归、工程 | R05 | symbol/dependency audit |
| R16 | P1 | stats/accounting schema | 内存、可观测性 | 生命周期 | 守恒/snapshot |
| R17 | P1 | OS syscall 移出 shard lock | 并发、性能 | R03/R08 | lock hold/p99 |
| R18 | P1 | release/install/visibility/ABI version | 兼容性、工程 | R10 | symbol/ABI/package |
| R19 | P2 | Frontend byte budget/GC | 内存、性能 | R16 | RSS/miss |
| R20 | P2 | Middle bulk bitmap/budget | 性能、内存 | R16 | cycles/lock |
| R21 | P2 | Incremental decay/pressure | 内存、性能 | R07/R16/R17 | burst RSS/refault |
| R22 | P2 | Central sharding | 并发、性能 | R02/R08 | same-class scaling |
| R23 | P2 | PageCache region/multi-shard | 并发、碎片 | R02/R08/R17 | region owner/scaling |
| R24 | P2 | LargeExtentSet | 内存、性能 | R03/R16/R23 | mmap/fragmentation |
| R25 | P2 | Sampling/control API | 可观测性、性能 | R16/R18 | overhead/profile |
| R26 | P2 | ReleaseChecked | 安全、性能 | R06/R16 | exploit checks/cost |
| R27 | P3 | per-CPU/rseq | 并发、性能 | R20/R22 | target workload |
| R28 | P3 | NUMA-local pipeline | 性能、兼容性 | R22-R24 | placement/remote |
| R29 | P3 | HugepageFiller/Region | 性能、内存 | R21/R23/R24 | THP/breakage |
| R30 | P3 | aethermind arenas | 正确性、性能 | R16/R18/R23 | lifecycle/E2E |
| R31 | P3 | Guarded sampling | 安全、内存 | R25/R26 | detection/overhead |

## 16.3 风险登记模板

每项记录：

- risk id/owner；
- affected invariant；
- trigger/workload；
- impact；
- likelihood；
- detection；
- mitigation；
- fallback；
- test/metric；
- status；
- target milestone。

## 16.4 优先级调整规则

- 任一真实 UAF/corruption 升 P0；
- benchmark 数字不可复现，相关性能结论降为未验证；
- 若功能阻断 preload/aethermind 灰度，至少 P1；
- 平台特定优化无真实 workload 收益保持 P3；
- 安全 profile 不得挤占默认生命周期 P0；
- P0 连续三次无法修复时重新评估架构，不以 workaround 掩盖。

