# 第 3 章：目标架构

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](01-overview-and-positioning.md) · [下一章](03-correctness-bootstrap-and-abi.md)  
> **本章目标**：定义 Frontend、Middle-end、Backend 与横向基础设施的目标形态。  
> **适用范围**：全分配链路及统计、自举、压力反馈等横向能力。  
> **核心 invariant**：分层职责清晰，标准 ABI 与扩展 API 汇入同一受验证核心。

```text
标准 C/C++ ABI                    ammalloc 扩展 API
malloc/free/new/delete            arena/NUMA/alignment/lifetime hint
          |                                  |
          +---------------+------------------+
                          v
                       Frontend
                 +--------+--------+
                 |                 |
          Per-thread Cache   Per-CPU Cache
            第一阶段          Linux/rseq 可选
                 |                 |
                 +--------+--------+
                          v
               NUMA-local Middle-end
          +---------------+----------------+
          |                                |
  Sharded TransferCache       Central Free Lists
                                     |
                                     v
                          PageHeap / Extent Manager
                 +-------------------+------------------+
                 |                   |                  |
          Small-run buckets   Large extent tree   Hugepage regions
                 +-------------------+------------------+
                                     |
                         mmap/madvise/munmap

横向基础设施：
- PageMap + 稳定的 Span 生命周期
- 统计、采样、Profiling 和内存压力反馈
- 无分配的自举、错误处理与配置系统
```

