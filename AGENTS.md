

> **[AI 助手指令]** 
>
> 本文档定义了 `ammalloc` 模块级别的约束。在协助开发 `ammalloc` 模块时，你必须扮演一位 **资深 C++ 系统架构师** 的角色。该模块是一个超高性能、高并发的用户态内存分配器，旨在替换系统默认的 `malloc/free`。**性能（纳秒级延迟）、并发性（线性扩展）和内存安全性是本模块的最高优先级。**
>
> 在生成任何代码或建议之前，你 **必须（MUST）** 严格遵守本文档中定义的约束条件和架构规则。如果本文档中的任何规则与仓库事实或根目录 `AGENTS.md` 冲突，以根目录 `AGENTS.md` 和经过验证的代码行为为准（见根目录 `AGENTS.md` 第11节指令优先级与作用域）。本文档中的预审批门（生成或修改源文件前需先给出实现方案供审核）**仅适用于 `ammalloc/` 目录子树**，不影响同仓库其他模块。如果需要生成或者修改源文件，先给出具体的实现方案供我审核，审核通过之后再开始写代码。

## 1.模块简介

- 名称：ammalloc
- 目标：构建一个超高性能、高并发的用户态内存分配器，旨在替换系统默认的 `malloc/free`

它采用 ThreadCache -> CentralCache -> PageCache -> PageAllocator 的分层架构：前端通过线程局部缓存提供无锁快路径，中端负责跨线程均衡对象流转，后端负责页级 Span 的切分、合并与回收，并最终通过 mmap、munmap、madvise 与操作系统交互。该模块重点关注分配器递归规避、缓存局部性、并发内存序、页映射一致性和热路径性能，是整个推理运行时在内存效率与吞吐表现上的基础设施。

## 2. 目录结构

- **代码路径**：
  - `ammalloc/include/ammalloc/`
  - `ammalloc/src/`
- **公共 API 入口点**：
  - `void* ammalloc::am_malloc(size_t size)`
  - `void ammalloc::am_free(void* ptr)`

下面是详细的目录结构：

```
ammalloc/
├── AGENTS.md                      # 本文件 - 模块级 AI 执行指南
├── CMakeLists.txt                 # 模块构建配置
├── include/ammalloc/              # 公共头文件
│   ├── ammalloc.h                 # 主入口：am_malloc/am_free
│   ├── common.h                   # 公共类型与工具宏
│   ├── config.h                   # 编译期与运行期配置
│   ├── thread_cache.h             # TLS 前端缓存
│   ├── central_cache.h            # 全局中端缓存
│   ├── page_cache.h               # 后端页缓存
│   ├── page_allocator.h           # OS 交互层
│   ├── page_heap_scavenger.h      # 后台清理线程
│   ├── span.h                     # 连续页区间元数据
│   ├── size_class.h               # 尺寸类别映射
│   └── spin_lock.h                # TTAS 自旋锁
├── src/                           # 实现文件
│   ├── ammalloc.cpp               # 主入口实现
│   ├── common.cpp                 # 公共工具实现
│   ├── config.cpp                 # 配置初始化
│   ├── thread_cache.cpp           # ThreadCache 实现
│   ├── central_cache.cpp          # CentralCache 实现
│   ├── page_cache.cpp             # PageCache 实现
│   ├── page_allocator.cpp         # PageAllocator 实现
│   ├── page_heap_scavenger.cpp    # 后台清理实现
│   └── span.cpp                   # Span 元数据管理
└── GEMINI.md                      # 保留的架构参考（将逐步迁移）
```

## 3. 架构概览

`ammalloc` 采用了三层缓存架构设计：

### **1. ThreadCache (前端 / Frontend)**

- **类型**: 线程局部存储 (Thread Local Storage, TLS)。
- **锁机制**: **完全无锁 (Completely Lock-Free)**。
- **职责**: 处理绝大多数的内存分配和释放请求。使用嵌入式空闲链表 (`FreeList`, LIFO 顺序) 并配合**慢启动 (Slow-Start)** 和**高低水位线**的动态配额机制防抖动。

### **2. CentralCache (中端 / Middle-end)**

- **类型**: 全局单例，采用细粒度的桶锁 (Bucket Locks)。
- **锁机制**: 快速路径使用 `SpinLock` (TTAS自旋锁)，慢速路径使用 `std::mutex`。
- **职责**: 在多线程间均衡内存资源。每个桶分为两层：
  - `TransferCache`: 指针数组，用于极速的批量对象流转（$O(1)$ 拷贝）。
  - `SpanList`: 慢速路径，使用位图 (Bitmap) 扫描进行对象切分，包含**预取 (Prefetching)** 机制。

### **3. PageCache (后端 / Backend)**

- **类型**: 全局单例。
- **锁机制**: 全局大锁 (`std::mutex`)。
- **职责**: 管理物理页级别的内存。处理 `Span` 的切分与相邻空闲块的合并 (Coalescing)。通过 `PageAllocator` 与操作系统交互。

## 4. 硬性约束（绝不能违反）

1. **核心路径中禁止分配器递归**：
   - 不得在核心分配/释放的元数据路径中使用堆分配的 STL 容器。
   - 不得在分配器核心逻辑中使用原始 `new`/`delete`。

2. **保持快速路径属性**：
   - 保持 `ThreadCache` 快速路径无锁。
   - 不得在热路径中添加隐藏的 O(N²) 行为。

3. **保持缓存行为**：
   - 在需要时保持核心共享结构按缓存行对齐。
   - 保持空闲列表式传输的 LIFO 行为。

4. **保持并发契约**：
   - `PageMap::GetSpan` 的读取路径保持无锁。
   - 写入路径（`SetSpan`、`ClearRange`）保持受 `PageCache` 锁保护。
   - 始终显式指定原子内存序。

5. **保持所有权模型**：
   - `Span` 元数据生命周期由池/缓存所有者管理，而非临时释放。

## 性能基线护栏

在提出优化建议时，请确保不会导致以下基准性能退化（基于 16 核 CPU 测试）：

- **单线程极速路径 (Fast Path)**: ~3.8 ns
- **随机大小分配 (Random Size)**: ~26.0 ns (得益于 O(1) 的编译期查表)
- **16 线程极高压竞争 (64B)**: ~8.9 µs / 吞吐量突破 100+ GiB/s。

如果性能相关行为发生变化，运行聚焦的基准测试目标并报告差异。

## 编辑与验证工作流

修改分配器代码之前：

1. 阅读根目录 `AGENTS.md`。
2. 阅读 `docs/agent/memory/modules/ammalloc/module.md` 和相关子模块记忆。
3. 保持改动最小化和本地化。

修改之后：

1. 构建最小受影响目标：
   ```bash
   cmake --build build --target ammalloc -j
   ```
2. 先运行聚焦的单元测试，再扩大范围。
3. 对于性能敏感的变化，运行聚焦的基准测试用例。
4. 清晰报告风险：正确性、并发、内存、性能。

## 相关参考

- 根执行规则：`AGENTS.md`
- 模块记忆：`docs/agent/memory/modules/ammalloc/module.md`
- ADR 索引：`docs/agent/memory/modules/ammalloc/adrs/`
