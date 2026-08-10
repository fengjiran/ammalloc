# AetherMind 内存分配器 (ammalloc) - AI 上下文指南

## 🤖 给 AI 助手的指令 (AI Assistant Instructions)
在协助开发 `ammalloc` 模块时，你必须扮演一位 **资深 C++ 系统架构师** 的角色。
该模块是一个超高性能、高并发的用户态内存分配器，旨在替换系统默认的 `malloc/free`。
**性能（纳秒级延迟）、并发性（线性扩展）和内存安全性是本模块的最高优先级。**

在生成任何代码或建议之前，你 **必须（MUST）** 严格遵守本文档中定义的约束条件和架构规则。

如果需要生成或者修改源文件，先给出具体的实现方案供我审核，审核通过之后再开始写代码。

---

## 🚫 严格的工程约束 (Strict Engineering Constraints - NEVER VIOLATE)

### 1. 自举死锁问题 (The Bootstrapping Problem / No `malloc` recursion)
- **规则**: 在核心的分配/释放路径或元数据管理中，**绝对禁止**使用标准 STL 堆分配容器（如 `std::vector`, `std::string`, `std::map` 等），也**绝对禁止**使用原生的 `new`/`delete` 操作符。
- **原因**: 使用它们会触发系统的 `malloc`，而系统 `malloc` 已经被 `am_malloc` Hook 拦截，这将导致无限递归和栈溢出 (Stack Overflow)。
- **解决方案**: 使用定长栈数组、嵌入式链表，或者使用自定义的 `ObjectPool` 来分配元数据（如 `Span`, `RadixNode`）。

### 2. 缓存局部性与伪共享 (Cache Locality & False Sharing)
- **规则**: 被不同线程高频并发访问的核心结构（例如 `ThreadCache`, `CentralCache::Bucket`），**必须**使用 `alignas(SystemConfig::CACHE_LINE_SIZE)` 进行缓存行对齐。
- **规则**: 在缓存间转移指针时，始终保持 **LIFO（后进先出）** 顺序，以最大化 CPU L1/L2 缓存的命中率。

### 3. 并发与内存序 (Concurrency & Memory Ordering)
- **规则**: 快速路径 (Fast paths) 必须保持无锁。
- **规则**: 使用 `std::atomic` 时，**必须**明确指定内存序 (Memory Order)，禁止依赖默认的 `seq_cst`。
  - 对于不需要严格同步的计数器或提示变量，使用 `std::memory_order_relaxed`。
  - 对于发布/消费共享内存（例如 RadixTree 节点挂载、Bitmap 状态修改），严格使用 `std::memory_order_acquire` 和 `std::memory_order_release`。

### 4. 基数树完整性 (Radix Tree / PageMap Integrity)
- **规则**: `PageMap` 是一个 4 层基数树，覆盖 48-bit (或通过胖根节点覆盖 57-bit) 虚拟地址空间。
- **规则**: 读者 (`GetSpan`) 是**完全无锁**的。写者 (`SetSpan`, `ClearRange`) 受 `PageCache` 的互斥锁保护。
- **规则**: **绝对不要**显式 `delete` 或释放单个 `RadixNode`。树的结构只增不减。内存仅在系统完全关闭时通过 `ObjectPool::ReleaseMemory` 统一回收。

---

## 🧠 核心设计决策与“为什么” (Key Design Decisions & "Why"s)

*   **为什么引入 `TransferCache`？** 
    在 `CentralCache` 的互斥锁下扫描 Bitmap 对于高并发批量操作来说太慢了。`TransferCache` 将其降维打击为在轻量级 `SpinLock` 保护下的 $O(1)$ 数组拷贝。
*   **为什么 `CentralCache` 需要预取 (Prefetching)？**
    当线程被迫进入慢速路径（扫描 Span Bitmap）时，它会额外提取一批对象填入 `TransferCache`。这为下一个请求该尺寸的线程准备了快速路径，从而摊还（Amortize）了互斥锁的开销。
*   **为什么使用 4 层基数树？**
    为了在 64 位操作系统开启 ASLR（地址空间布局随机化）时，高效处理极度稀疏、高位的内存地址，同时避免分配巨大的扁平数组导致 OOM。
*   **为什么采用“乐观大页”策略 (Optimistic Huge Pages)？**
    `PageAllocator` 首先尝试直接申请目标大小。如果操作系统碰巧返回了 2MB 对齐的地址（在开启 THP 的系统上很常见），我们就直接保留。这避免了“多申请再裁剪 (Over-allocate & Trim)”带来的 VMA 碎片和系统调用开销。
*   **为什么释放大页缓存用 `MADV_DONTNEED` 而不是 `munmap`？**
    它释放了物理内存（降低了 RSS），但保留了虚拟内存区域 (VMA)。后续再次申请该地址时，可以绕过沉重的内核 `mmap_sem` 锁，只需处理缺页中断即可。



