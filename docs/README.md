# ammalloc 文档索引

本文档是 ammalloc 项目文档的唯一入口。文档分类体系、命名与交叉引用规则、质量标准与维护流程见 [文档系统规范](guides/documentation-guide.md)；新建文档从 [文档模板](templates/module-design.md) 复制填充。

## 文档分类总览

| 类型 | 目录 | 状态字段 | 职责边界 |
|---|---|---|---|
| 架构总览 | [designs/ammalloc_design.md](designs/ammalloc_design.md) | Current | 全系统唯一权威总览：分层架构、硬性约束、跨层流程、性能基线 |
| 模块设计 | [designs/](designs/) | Current / Deprecated | 单模块深入设计：数据结构、并发模型、接口、边界、权衡 |
| 调研备忘 | [designs/research/](designs/research/) | 无状态，头部标注日期与可信度 | 未验证/未采纳的技术调研，不承诺实现 |
| 演进提案 | [improvement-plan/](improvement-plan/README.md) | Draft / In Progress / Implemented / Superseded | 未来架构演进提案与路线图（18 个编号专题） |
| 开发指南 | [guides/](guides/) | Current / Deprecated | "怎么做"的过程规范：编码、注释、测试、评审、文档 |
| API 参考 | [api/public-api.md](api/public-api.md) | Current | 公共 API 语义（与头文件 Doxygen 同源同步） |
| 决策记录 | [decisions/](decisions/) | Proposed / Accepted / Deprecated / Superseded | 已作出/被否决的架构决策及其理由（ADR） |
| 问题跟踪 | [issues.md](issues.md) | 每条目 `[x]`/`[ ]` | 已知缺陷与优化待办，短生命周期 |
| 变更记录 | [../CHANGELOG.md](../CHANGELOG.md) | 无 | 行为可见变更，按语义化版本 |

**职责边界判定**：描述"代码里现在是什么" → `designs/`；"将来要做什么" → `improvement-plan/`；"曾经怎么决策的" → `decisions/`；"怎么干活" → `guides/`；"API 怎么用" → `api/`；"已知缺陷/待办" → `issues.md`。

## 文档清单

### 架构与模块设计（docs/designs/）

| 文档 | 定位 | 状态 | 最后更新 |
|---|---|---|---|
| [ammalloc_design.md](designs/ammalloc_design.md) | 架构总览（00- 级，全系统权威） | Current | 2026-08-19 |
| [01-size-class.md](designs/01-size-class.md) | 尺寸类别映射：边界语义、对齐与查表 | Current | 2026-08-19 |
| [02-thread-cache.md](designs/02-thread-cache.md) | TLS 前端缓存：无锁快路径、慢启动与水位线 | Current | 2026-08-19 |
| [03-central-cache.md](designs/03-central-cache.md) | 中端缓存：TransferCache 与 SpanList、桶锁 | Current | 2026-08-19 |
| [04-page-cache.md](designs/04-page-cache.md) | 后端页缓存：分片、Span 切分与合并 | Current | 2026-08-19 |
| [05-page-allocator.md](designs/05-page-allocator.md) | OS 交互层：mmap/munmap/madvise、映射缓存 | Current | 2026-08-19 |
| [06-page-heap-scavenger.md](designs/06-page-heap-scavenger.md) | 后台回收线程：MADV_DONTNEED 与 RSS 治理 | Current | 2026-08-19 |
| [07-span-and-pagemap.md](designs/07-span-and-pagemap.md) | Span 元数据生命周期与 PageMap 基数树 | Current | 2026-08-19 |
| [08-free-list.md](designs/08-free-list.md) | 嵌入式 LIFO 空闲链表：单/批量流转与每类配额 | Current | 2026-08-21 |
| [research/allocator-background-thread.md](designs/research/allocator-background-thread.md) | 调研备忘：业界后台线程启动策略 | 调研 | 2026-03-05 |
| [research/thread-local-and-thread-cache.md](designs/research/thread-local-and-thread-cache.md) | 调研备忘：thread_local 在 ThreadCache 的语义、收益与成本 | 调研 | 2026-08-19 |

### 演进提案（docs/improvement-plan/）

[improvement-plan/README.md](improvement-plan/README.md) 是 18 个编号专题的入口与推荐阅读路径，覆盖正确性自举、并发扩展、RSS、NUMA、可观测性、安全、发布路线图等。专题状态见其目录表。

### 开发指南（docs/guides/）

| 文档 | 定位 | 状态 |
|---|---|---|
| [documentation-guide.md](guides/documentation-guide.md) | 文档系统规范：命名、交叉引用、质量、维护流程 | Current |
| [cpp_coding_style_guidelines.md](guides/cpp_coding_style_guidelines.md) | C++ 编码风格 | Current |
| [cpp_comment_guidelines.md](guides/cpp_comment_guidelines.md) | 注释与 Doxygen 规范 | Current |
| [test_writing_guidelines.md](guides/test_writing_guidelines.md) | GoogleTest 测试编写规范 | Current |
| [code_review_guide.md](guides/code_review_guide.md) | 代码审查方法（风险分级驱动） | Current |

### 架构决策记录（docs/decisions/）

| 文档 | 决策内容 | 状态 |
|---|---|---|
| [0001-scavenger-startup-strategy.md](decisions/0001-scavenger-startup-strategy.md) | PageHeapScavenger 启动时机策略 | Accepted |

### API 参考与问题跟踪

- [api/public-api.md](api/public-api.md)：公共 API（`am_malloc`/`am_free`）语义参考。
- [issues.md](issues.md)：已知缺陷与优化待办清单。

## 术语表

以下术语在全仓库文档中统一使用，含义以本表为准。

| 术语 | 定义 |
|---|---|
| ThreadCache | 线程局部前端缓存（TLS），快路径完全无锁 |
| CentralCache | 全局中端缓存，按尺寸类别分桶，桶锁保护 |
| PageCache | 后端页缓存，内部按分片（Shard）同步，负责 Span 切分与合并 |
| PageAllocator | OS 交互层，封装 mmap/munmap/madvise |
| PageHeapScavenger | 后台清理线程，对长期闲置 Span 执行 MADV_DONTNEED |
| Span | 连续页区间元数据，是 PageCache 与 PageMap 的基本管理单元 |
| PageMap | 4 层基数树，映射 PageID → 归属 Span，读路径无锁 |
| SizeClass | 尺寸类别：将任意请求大小映射到固定桶的编译期查表 |
| FreeList | 嵌入式空闲链表（LIFO），对象自身携带 next 指针 |
| TransferCache | CentralCache 桶内的指针数组，支持 O(1) 批量对象流转 |
| Shard | PageCache 的分片，每片独立互斥锁与空闲链表（owner-shard-local） |
| Slow-Start | ThreadCache 慢启动：仅在反复 refill 压力后才增长 FreeList 配额 |
| 水位线 | ThreadCache 的高低水位配额机制，超限批量归还、防抖动 |
| ObjectPool | 定长元数据池，核心路径零堆分配，规避分配器递归 |
| 自举（Bootstrap） | 分配器自身元数据分配不依赖系统 malloc 的约束 |
| MADV_DONTNEED | 释放物理页但保留虚拟映射的内核提示，RSS 治理手段 |
| Owner-shard-local | Span 的合并与回收仅发生在所属分片内 |
