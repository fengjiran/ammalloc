# ammalloc 全面提升与演进方案

> 文档状态：规划草案  
> 适用范围：ammalloc 核心分配器、系统分配接口、测试基准及 aethermind 集成  
> 总体目标：将 ammalloc 建设为可安全接管进程内存、可作为后续项目底层基础设施，并在功能、性能、内存效率和可观测性方面对标 TCMalloc、jemalloc 的工业级用户态内存分配器。

本方案由 18 个专题文档组成。文件名前缀表示推荐阅读顺序，正文章节号用于架构主题引用；跨章节引用统一使用相对链接，不依赖目录站点或特定 Markdown renderer。

## 总体原则

ammalloc 现有的 `ThreadCache -> CentralCache -> PageCache -> PageAllocator` 分层架构是合理起点，但在成为进程级基础设施之前，必须依次解决：

1. correctness、ownership/lifetime、自举和 ABI；
2. 并发扩展、内存碎片、RSS 与可观测性；
3. NUMA、hugepage、安全和 大模型推理引擎专用能力；
4. 可复现测试、发布、灰度和回滚。

正确性门禁不得被性能目标绕过。所有性能结论必须基于机制分析和可复现实验，并守住项目当前基线：单线程 Fast Path 约 3.8 ns、随机大小约 26.0 ns、16 线程 64B 极高压竞争约 8.9 µs / 100+ GiB/s。

## 完整目录

| 序号 | 专题 | 主要风险 |
|---:|:---|---|
| 01 | [总体结论与产品定位](01-overview-and-positioning.md) | 架构、产品边界 |
| 02 | [目标架构](02-target-architecture.md) | 架构、兼容性 |
| 03 | [正确性、自举与 ABI](03-correctness-bootstrap-and-abi.md) | 正确性、内存、兼容性 |
| 04 | [PageMap 与 Span 生命周期](04-pagemap-and-span-lifecycle.md) | 正确性、并发、内存 |
| 05 | [Frontend 提升](05-frontend.md) | 性能、并发、生命周期 |
| 06 | [Middle-end 提升](06-middle-end.md) | 并发、性能、内存 |
| 07 | [Backend、PageCache 与大对象管理](07-backend-pagecache-large-object.md) | 正确性、并发、内存、性能 |
| 08 | [RSS、碎片与后台回收](08-rss-fragmentation-and-scavenging.md) | 内存、性能、运维 |
| 09 | [NUMA 与 aethermind 集成](09-numa-and-aethermind.md) | 架构、性能、兼容性 |
| 10 | [可观测性与 Profiling](10-observability-and-profiling.md) | 性能、自举、运维 |
| 11 | [安全加固](11-security-hardening.md) | 安全、正确性、性能 |
| 12 | [测试与验证体系](12-testing-and-validation.md) | 正确性、工程、性能测量 |
| 13 | [工程化与发布](13-engineering-and-release.md) | ABI、工程、运维 |
| 14 | [分阶段实施路线图](14-implementation-roadmap.md) | 依赖、交付、风险控制 |
| 15 | [优先级与风险矩阵](15-priority-and-risk-matrix.md) | 项目治理、风险 |
| 16 | [关键实施原则](16-implementation-principles.md) | 跨模块 invariant |
| 17 | [参考资料](17-references.md) | 事实与决策依据 |
| 18 | [最终建议](18-final-recommendations.md) | 里程碑、终态判断 |

## 推荐阅读路径

### 正确性与内存安全

[正确性、自举与 ABI](03-correctness-bootstrap-and-abi.md) → [PageMap 与 Span 生命周期](04-pagemap-and-span-lifecycle.md) → [Backend、PageCache 与大对象管理](07-backend-pagecache-large-object.md) → [安全加固](11-security-hardening.md) → [测试与验证体系](12-testing-and-validation.md)

### 热路径与并发扩展

[目标架构](02-target-architecture.md) → [Frontend](05-frontend.md) → [Middle-end](06-middle-end.md) → [Backend](07-backend-pagecache-large-object.md) → [RSS 与后台回收](08-rss-fragmentation-and-scavenging.md)

### 标准 malloc 替换与发布

[正确性、自举与 ABI](03-correctness-bootstrap-and-abi.md) → [可观测性](10-observability-and-profiling.md) → [测试与验证](12-testing-and-validation.md) → [工程化与发布](13-engineering-and-release.md)

### 大模型推理引擎 aethermind 集成

[目标架构](02-target-architecture.md) → [Backend](07-backend-pagecache-large-object.md) → [NUMA 与 aethermind](09-numa-and-aethermind.md) → [可观测性](10-observability-and-profiling.md) → [实施路线图](14-implementation-roadmap.md)

### 项目决策与执行

[总体结论与定位](01-overview-and-positioning.md) → [实施路线图](14-implementation-roadmap.md) → [优先级与风险矩阵](15-priority-and-risk-matrix.md) → [关键实施原则](16-implementation-principles.md) → [最终建议](18-final-recommendations.md)

## 文档维护规则

- 正文章节编号是跨评审、ADR 和 issue 引用的稳定标识。
- 章节之间使用具名相对链接，不使用模糊的“见上文”。
- 代码与文档冲突时，以经过测试验证的仓库事实为准，并同步更新相关章节。
- 当前实现、已批准设计和长期设想必须明确区分。
- 修改架构 invariant 时，应同时检查路线图、风险矩阵、测试计划和最终建议。
- 新增专题优先扩展现有职责边界；只有形成独立评审单元时才增加文件。
