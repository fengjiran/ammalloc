
> **[AI 助手指令]**
>
> 本文档定义了 `ammalloc` 模块级别的约束。在协助开发 `ammalloc` 模块时，你必须扮演一位 **资深 C++ 系统架构师** 的角色。该模块是一个超高性能、高并发的用户态内存分配器，旨在替换系统默认的 `malloc/free`。**性能（纳秒级延迟）、并发性（线性扩展）和内存安全性是本模块的最高优先级。**
>
> 在生成任何代码或建议之前，你 **必须（MUST）** 严格遵守本文档中定义的约束条件和架构规则。如果本文档中的任何规则与仓库事实或经过验证的代码行为冲突，以仓库事实和已验证的代码行为为准。本文档中的预审批门：如果需要生成或者修改源文件，先给出具体的实现方案供我审核，审核通过之后再开始写代码。

## 1. 模块简介

- 名称：ammalloc
- 目标：构建一个超高性能、高并发的用户态内存分配器，旨在替换系统默认的 `malloc/free`

它采用 ThreadCache -> CentralCache -> PageCache -> PageAllocator 的分层架构：前端通过线程局部缓存提供无锁快路径，中端负责跨线程均衡对象流转，后端负责页级 Span 的切分、合并与回收，并最终通过 mmap、munmap、madvise 与操作系统交互。该模块重点关注分配器递归规避、缓存局部性、并发内存序、页映射一致性和热路径性能，是整个推理运行时在内存效率与吞吐表现上的基础设施。

## 2. 目录结构

- **代码路径**：
  - `include/ammalloc/`
  - `src/`
- **公共 API 入口点**：
  - `void* ammalloc::am_malloc(size_t size)`
  - `void ammalloc::am_free(void* ptr)`

下面是详细的目录结构：

```
ammalloc/
├── AGENTS.md                      # 本文件 - 模块级 AI 执行指南
├── CMakeLists.txt                 # 顶层构建配置（选项与子目录调度）
├── include/ammalloc/              # 公共头文件
│   ├── ammalloc.h                 # 主入口：am_malloc/am_free
│   ├── assert.h                   # 断言宏：AMMALLOC_CHECK / AMMALLOC_DCHECK
│   ├── attributes.h               # 编译器属性与 builtin 包装宏
│   ├── common.h                   # 公共类型与工具宏
│   ├── config.h                 # 编译期与运行期配置
│   ├── free_list.h              # 嵌入式 LIFO 空闲链表（FreeList/FreeBlock）
│   ├── thread_cache.h           # TLS 前端缓存
│   ├── central_cache.h            # 全局中端缓存
│   ├── page_cache.h               # 后端页缓存
│   ├── page_allocator.h           # OS 交互层
│   ├── page_heap_scavenger.h      # 后台清理线程
│   ├── span.h                     # 连续页区间元数据
│   ├── size_class.h               # 尺寸类别映射
│   └── spin_lock.h                # TTAS 自旋锁
├── src/                           # 实现文件
│   ├── CMakeLists.txt             # 库目标配置（GLOB_RECURSE 收集源文件、依赖获取）
│   ├── ammalloc.cpp               # 主入口实现
│   ├── common.cpp                 # 公共工具实现
│   ├── config.cpp                 # 配置初始化
│   ├── thread_cache.cpp           # ThreadCache 实现
│   ├── central_cache.cpp          # CentralCache 实现
│   ├── page_cache.cpp             # PageCache 实现
│   ├── page_allocator.cpp         # PageAllocator 实现
│   ├── page_heap_scavenger.cpp    # 后台清理实现
│   └── span.cpp                   # Span 元数据管理
└── tests/
    ├── unit/
    │   ├── CMakeLists.txt         # googletest 依赖 + 单可执行 ammalloc_unit_tests
    │   └── test_*.cpp             # 单元测试源文件（GLOB_RECURSE 自动收集）
    └── benchmark/
        ├── CMakeLists.txt         # google benchmark 依赖 + 单可执行 ammalloc_benchmarks
        └── benchmark_*.cpp        # 基准测试源文件（GLOB_RECURSE 自动收集）
└── docs/                           # 文档系统（索引见 docs/README.md）
    ├── README.md                   # 文档索引与术语表
    ├── issues.md                   # 问题与待办跟踪
    ├── designs/                    # 架构与模块设计（NN- 编号，描述已验证实现）
    │   └── research/               # 调研备忘
    ├── improvement-plan/           # 演进提案与路线图（18 个编号专题）
    ├── guides/                     # 编码/注释/测试/文档规范
    ├── api/                        # 公共 API 参考
    ├── decisions/                  # 架构决策记录（ADR）
    └── templates/                  # 文档模板
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

- **类型**: 全局单例，内部采用分片结构（`PageCacheShard`，容量 `kMaxShardCount = 4`，生产默认仅启用 1 个分片）。
- **锁机制**: 每分片一把 `std::mutex`（分片锁）；当前默认只使用 shard 0，测试可通过 `SetActiveShardCountForTest` 扩展分片数。
- **职责**: 管理物理页级别的内存。处理 `Span` 的切分与相邻空闲块的合并 (Coalescing)（合并仅在所属分片内进行，owner-shard-local）。通过 `PageAllocator` 与操作系统交互。

## 4. 硬性约束（绝不能违反）

### 4.1 核心路径中禁止分配器递归

- 不得在核心分配/释放的元数据路径中使用堆分配的 STL 容器（如 `std::vector`、`std::string`、`std::map` 等）。
- 不得在分配器核心逻辑中使用原始 `new`/`delete`。
- **原因**: 使用它们会退回到系统 `malloc`/`free`，把元数据分配暴露在分配器管理之外，破坏递归规避契约；若未来替换（拦截）系统符号，将直接导致无限递归和栈溢出 (Stack Overflow)。当前公共 API 为 `ammalloc::am_malloc`/`am_free`，并未 Hook 系统 `malloc`。
- **解法**: 使用定长栈数组、嵌入式链表，或使用自定义的 `ObjectPool` 来分配元数据（如 `Span`、`RadixNode`）。

### 4.2 保持缓存局部性，避免伪共享

- 被不同线程高频并发访问的核心结构（如 `ThreadCache`、`CentralCache::Bucket`），**必须**使用 `alignas(SystemConfig::CACHE_LINE_SIZE)` 进行缓存行对齐。
- 在缓存间转移指针时，始终保持 **LIFO（后进先出）** 顺序，以最大化 CPU L1/L2 缓存的命中率。

### 4.3 保持快速路径属性

- 保持 `ThreadCache` 快速路径无锁。
- 不得在热路径中添加隐藏的 O(N²) 行为。

### 4.4 保持并发契约与内存序

- `PageMap::GetSpan` 的读取路径保持无锁。
- 写入路径（`SetSpan`、`ClearRange`）保持受所属 `PageCache` 分片锁保护。
- 使用 `std::atomic` 时，**必须**明确指定内存序 (Memory Order)，禁止依赖默认的 `seq_cst`：
  - 对于不需要严格同步的计数器或提示变量，使用 `std::memory_order_relaxed`。
  - 对于发布/消费共享内存（如 RadixTree 节点挂载、Bitmap 状态修改），严格使用 `std::memory_order_acquire` 和 `std::memory_order_release`。

### 4.5 基数树（PageMap）完整性

- `PageMap` 是一个 4 层基数树，覆盖 48-bit（或通过胖根节点覆盖 57-bit）虚拟地址空间。
- 读者 (`GetSpan`) 是**完全无锁**的；写者 (`SetSpan`、`ClearRange`) 受所属 `PageCache` 分片的互斥锁保护。
- **绝对不要**显式 `delete` 或释放单个 `RadixNode`。树的结构只增不减，内存仅在系统完全关闭时通过 `ObjectPool::ReleaseMemory` 统一回收。

### 4.6 保持所有权模型

- `Span` 元数据生命周期由池/缓存所有者管理，而非临时释放。

## 5. 代码风格规范

- prefer modern C++ adopted by the project (C++20)
- prefer RAII and standard library facilities
- avoid raw owning pointers
- preserve const-correctness
- make ownership and lifetime explicit
- avoid hidden O(N²) behavior in hot paths
- prefer simple control flow and early returns
- prefer small, focused functions

See full coding style:

- `docs/guides/cpp_coding_style_guidelines.md`

## 6. 注释风格规范

Write comments only when they add real value.
Comment:

- intent
- assumptions
- invariants
- ownership and lifetime
- thread-safety expectations
- non-obvious tradeoffs
- performance-sensitive choices

Do not:

- comment obvious code
- leave commented-out code
- keep stale comments

For non-trivial public APIs in headers, use documentation comments.
For implementation details, use `//`.

See full comment rules:

- `docs/guides/cpp_comment_guidelines.md`

## 7. 测试编写规范

Tests use GoogleTest and live under `tests/unit/` (see §9 for run commands).

- suite and test names use PascalCase; no `DISABLED_`, no commented-out `GTEST_SKIP()`
- one behavior per test; prefer `EXPECT_*`, use `ASSERT_*` only when continuing would crash
- death tests match `"Check failed"` (the `AMMALLOC_CHECK` abort message) and guard debug-only checks with `#ifndef NDEBUG`
- shared helpers go in `tests/unit/.../test_*_helpers.h` as `inline` functions, not in `include/`
- reset global singletons in `TEST_F` `SetUp`/`TearDown`; seed RNGs explicitly

See full test writing rules:

- `docs/guides/test_writing_guidelines.md`

## 8. 性能基线护栏

在提出优化建议时，请确保不会导致以下基准性能退化（基于 16 核 CPU 测试）：

- **单线程极速路径 (Fast Path)**: ~3.8 ns
- **随机大小分配 (Random Size)**: ~26.0 ns (得益于 O(1) 的编译期查表)
- **16 线程极高压竞争 (64B)**: ~8.9 µs / 吞吐量突破 100+ GiB/s。

如果性能相关行为发生变化，运行聚焦的基准测试用例并报告差异。

## 9. 构建与验证工作流

修改分配器代码之前：

1. 阅读本文件（`AGENTS.md`）。
2. 保持改动最小化和本地化。

修改之后：

1. 构建最小受影响目标：
   ```bash
   cmake --build build --target ammalloc -j
   ```
2. 先运行聚焦的单元测试，再扩大范围：
   ```bash
   ./build/tests/unit/ammalloc_unit_tests --gtest_filter=<Suite>.*
   ```
3. 对于性能敏感的变化，运行聚焦的基准测试用例：
   ```bash
   ./build/tests/benchmark/ammalloc_benchmarks --benchmark_filter=BM_<Group>.*
   ```
4. 涉及文档时运行漂移检测（链接有效性 / 符号可追溯 / 索引覆盖）：
   ```bash
   python3 scripts/verify_docs.py
   ```
5. 清晰报告风险：正确性、并发、内存、性能。

构建说明：

- 依赖（`spdlog`、`googletest`、`google benchmark`）由 `FetchContent` 自动获取。
- 测试为**纯 gtest 直跑模式，无 ctest**：单元测试全部用例合并为单可执行 `ammalloc_unit_tests`，基准全部用例合并为单可执行 `ammalloc_benchmarks`；`--gtest_filter` / `--benchmark_filter` 用于选择子集。
- 源文件收集统一使用 `GLOB_RECURSE` + `CONFIGURE_DEPENDS`：`src/`、`tests/unit/`、`tests/benchmark/` 内**不要**放置非本目录用途的 `.cpp`/`.cc`（会被自动纳入目标）。
- 顶层选项：`-DBUILD_TESTS=ON/OFF`、`-DBUILD_BENCHMARKS=ON/OFF`、`-DUSE_57BIT_VA=ON/OFF`（开启后定义 `AM_USE_57BIT_VA`，将 VA 空间扩展至 57-bit）。

## 10. 相关参考

- 根执行规则：本文件（`AGENTS.md`）
