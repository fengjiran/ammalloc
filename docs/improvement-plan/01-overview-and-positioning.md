# 总体结论与产品定位

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [下一章](02-target-architecture.md)  
> **本章目标**：明确 ammalloc 的产品边界、运行模式和演进顺序。  
> **适用范围**：产品决策、部署模式与整体演进边界。  
> **核心 invariant**：正确性和生命周期安全先于极限性能与复杂并发优化。

## 1. 总体结论

ammalloc 现有的 `ThreadCache -> CentralCache -> PageCache -> PageAllocator` 分层架构是一个良好起点，但距离“可替换系统 malloc、可作为 aethermind 基础设施、综合对标 TCMalloc/jemalloc”还需要跨越三个阶段：

1. 达到工业级正确性、自举安全性和 ABI 完整性。
2. 解决并发扩展、内存碎片和 RSS 回收问题。
3. 建设 NUMA、采样分析、安全加固及推理引擎专用扩展能力。

实施过程中应首先保证正确性和生命周期安全，再推进无锁化与极限性能优化。当前 TLS 生命周期、OOM 处理、对齐、PageMap/Span 生命周期以及分配器递归等基础问题没有解决前，不宜扩大并发结构的复杂度。

## 2. 产品定位与运行模式

TCMalloc 采用 Frontend/Middle-end/Backend 分层，现代实现支持 per-CPU cache、动态缓存容量、hugepage-aware backend 和低开销采样。jemalloc 的主要优势包括多 arena、tcache、dirty/muzzy decay、后台 purge、extent hook、运行时控制和完整统计体系。

ammalloc 不必逐项复制两者，而应形成以下产品定位：

- **默认模式**：在低延迟与合理 RSS 之间保持均衡。
- **Latency 模式**：扩大前端缓存、降低 purge 频率，面向在线推理。
- **Memory 模式**：缩小缓存、积极 decay，面向多模型或多租户部署。
- **Hardened 模式**：启用抽样防护、指针校验和 quarantine。
- **Aethermind 扩展模式**：提供 NUMA、生命周期、热冷和 arena hint，但不污染标准 malloc ABI。

