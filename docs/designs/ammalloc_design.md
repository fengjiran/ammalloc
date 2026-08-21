# ammalloc 设计文档

**版本**: v2.0.2
**日期**: 2026-08-19
**作者**: AetherMind Team

> **文档定位**：本文档描述 **当前已验证实现**（current state）的架构与设计。所有内容均对照 `include/ammalloc/`、`src/` 中的代码事实编写；当本文档与代码冲突时，以经过测试验证的仓库事实为准。
>
> **未来架构**（per-CPU、NUMA、标准 ABI 替换、BootstrapAllocator 等）属于演进方向，见第 12 章及 [`docs/improvement-plan/`](../improvement-plan/README.md)。

---

## 目录

1. [概述](#1-概述)
2. [设计目标](#2-设计目标)
3. [整体架构](#3-整体架构)
4. [核心数据结构](#4-核心数据结构)
5. [子模块详细设计](#5-子模块详细设计)
   - 5.1 [ThreadCache（线程缓存）](#51-threadcache线程缓存)
   - 5.2 [CentralCache（中心缓存）](#52-centralcache中心缓存)
   - 5.3 [PageCache（页缓存）](#53-pagecache页缓存)
   - 5.4 [SizeClass（尺寸分级）](#54-sizeclass尺寸分级)
   - 5.5 [SpinLock（自旋锁）](#55-spinlock自旋锁)
   - 5.6 [PageMap（页映射基数树）](#56-pagemap页映射基数树)
   - 5.7 [PageAllocator（OS 交互层）](#57-pageallocatoros-交互层)
   - 5.8 [PageHeapScavenger（后台回收）](#58-pageheapscavenger后台回收)
6. [内存分配/释放流程](#6-内存分配释放流程)
7. [性能优化策略](#7-性能优化策略)
8. [线程安全与并发控制](#8-线程安全与并发控制)
9. [内存碎片与 RSS 管理](#9-内存碎片与-rss-管理)
10. [配置参数](#10-配置参数)
11. [性能基准测试](#11-性能基准测试)
12. [演进方向](#12-演进方向)

## 附录

- [A. 文件结构](#a-文件结构)
- [B. 相关设计文档与参考资料](#b-相关设计文档与参考资料)
- [版本历史](#版本历史)

---

## 1. 概述

### 1.1 项目背景

ammalloc 是一个高性能、多线程友好的用户态内存分配器。其核心设计理念源于 Google TCMalloc，并结合现代 C++（C++20）特性与底层微架构优化，目标是成为系统默认 `malloc/free` 的**显式替代路径**：业务通过 `ammalloc::am_malloc` / `ammalloc::am_free` 显式调用，不拦截系统符号。

当前实现已具备：

- ThreadCache → CentralCache → PageCache → PageAllocator 分层架构，前端 TLS 无锁快路径；
- 分片 PageCache（shard 化）与 owner-shard-local Span 合并；
- 无锁读、受保护写的 4 层基数树 PageMap（支持 48-bit / 57-bit 虚拟地址空间）；
- 后台 PageHeapScavenger 通过 `MADV_DONTNEED` 回收空闲 Span 的物理页（RSS 治理）；
- 基于 `mmap`/`munmap`/`madvise` 的 PageAllocator，含 2 MiB 映射缓存与透明大页提示；
- 基于 `ObjectPool` 的固定大小元数据池，核心路径零堆分配、无分配器递归。

### 1.2 设计哲学

| 原则 | 描述 |
|------|------|
| **分层缓存** | ThreadCache → CentralCache → PageCache → PageAllocator 四级架构，热点数据局部化访问 |
| **锁粒度优化** | TLS 无锁 → 桶锁（SpinLock + Mutex）→ 分片锁，逐层降低竞争 |
| **批量操作** | 批量搬运（batch transfer）摊薄跨层交互与锁开销 |
| **零拷贝元数据** | 嵌入式链表（FreeList/SpanList）复用对象自身空间；元数据用 `ObjectPool` 定长池分配 |
| **递归规避** | 核心路径禁止 STL 堆容器与裸 `new`/`delete`，杜绝回落到系统 malloc 的递归死锁 |
| **缓存友好** | 缓存行对齐、预取指令、LIFO 策略最大化 CPU 缓存命中率 |
| **Owner 模型** | Span 元数据由所属 PageCache 分片的 ObjectPool 唯一持有，生命周期明确 |
| **显式内存序** | 所有 `std::atomic` 操作显式指定内存序，不依赖默认 `seq_cst` |

### 1.3 硬性约束

- 核心分配/释放元数据路径中禁止使用 STL 堆分配容器与原始 `new`/`delete`（详见 4.5）。
- 被多线程高频并发访问的核心结构（ThreadCache、CentralCache::Bucket、PageCacheShard）必须缓存行对齐。
- 缓存间转移对象始终保持 LIFO 顺序。
- `PageMap::GetSpan` 读取路径保持无锁；写入路径受所属分片锁保护。
- 基数树节点运行期只增不减，绝不单独释放。

---

## 2. 设计目标

### 2.1 性能目标（实测基线）

基于 16 核 CPU 的 Release 构建实测护栏（详见第 11 章与 `AGENTS.md`）：

| 指标 | 基线 | 说明 |
|------|------|------|
| 单线程快路径 | ~3.8 ns | 绝大多数小对象分配/释放完全无锁 |
| 随机大小分配 | ~26.0 ns | 得益于 O(1) 编译期查表 |
| 16 线程极高压竞争（64B） | ~8.9 µs / 100+ GiB/s | 吞吐量突破 100 GiB/s |

性能相关改动必须守住以上护栏，不得以正确性、RSS 或尾延迟为代价换取平均延迟。

### 2.2 功能目标

| 能力 | 状态 |
|------|------|
| 小对象（≤ 32 KiB）高吞吐分配 | 已实现（TLS 无锁快路径） |
| 大对象（> 32 KiB）页对齐分配 | 已实现（直接走 PageCache） |
| 多线程并发分配无全局锁竞争 | 已实现（TLS + 桶锁 + 分片锁） |
| Span 切分与 owner-shard-local 合并 | 已实现 |
| 后台 RSS 回收（Scavenger + MADV_DONTNEED） | 已实现 |
| 48-bit / 57-bit 虚拟地址空间支持 | 已实现（编译选项 `USE_57BIT_VA`） |
| 2 MiB 映射缓存与透明大页提示 | 已实现 |
| 替换系统 `malloc/free` 符号 | 不支持（显式 API 模式，见 2.3） |

### 2.3 API 边界与语义契约

公共 API 仅两个入口（`include/ammalloc/ammalloc.h`）：

```cpp
void* am_malloc(size_t original_size);
void  am_free(void* ptr);
```

语义契约（README 与实现共同确认）：

| 场景 | 行为 |
|------|------|
| `am_malloc(0)` | 映射到最小 size class（`ALIGNMENT` 字节），不返回 null |
| `am_malloc(size)` 失败 | 返回 `nullptr` |
| `am_free(nullptr)` | no-op |
| `am_free(未识别指针)` | PageMap 查询 miss 后静默忽略（no-op） |
| `am_free(合法指针)` | 按 Span 归属走大对象/小对象路径 |
| 系统 `malloc`/`free`、全局 `new`/`delete` | **不替换、不拦截**；与 `am_*` 混用指针是未定义行为 |

只允许将 `am_malloc` 返回的原始活动指针传给 `am_free`；内部指针释放、double-free 均不支持（Hardened 检测属演进方向，见第 12 章）。

### 2.4 质量目标

- **可测试性**：GoogleTest 单元测试 + Google Benchmark 基准，单可执行文件 + 过滤器子集运行。
- **可观测性**：`PageAllocatorStats` 原子计数器、scavenger 日志、测试专用接口（`GetMaxSizeForTest` 等）。
- **可配置性**：运行时环境变量（见 10.2）与构建选项（见 10.3）。
- **正确性护栏**：`AMMALLOC_CHECK`（Release 生效的致命检查）与 `AMMALLOC_DCHECK`（Debug 检查）分层。

---

## 3. 整体架构

### 3.1 分层架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Application Layer                              │
│                     am_malloc() / am_free()                              │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
┌─────────────────────────────────▼───────────────────────────────────────┐
│                     ThreadCache (TLS，前端 Frontend)                      │
│  FreeList[0]  FreeList[1]  ...  FreeList[N-1]      N = kNumSizeClasses  │
│  (16B)        (32B)            (32KB)                                   │
│                                                                          │
│  特性: 完全无锁 | 线程私有 | LIFO 嵌入式链表 | O(1)                        │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ 慢路径：批量 refill / trim
┌─────────────────────────────────▼───────────────────────────────────────┐
│                    CentralCache (中端 Middle-end)                         │
│  Bucket[0]    Bucket[1]    ...    Bucket[N-1]     每 size class 一桶      │
│  ┌──────────────────┐  ┌──────────────────┐                              │
│  │ TransferCache    │  │ TransferCache    │  ← SpinLock，O(1) 指针数组   │
│  │ (指针数组, 快路径)│  │ (指针数组, 快路径)│                              │
│  ├──────────────────┤  ├──────────────────┤                              │
│  │ SpanList         │  │ SpanList         │  ← std::mutex，Bitmap 切分   │
│  │ (Span 双链表)     │  │ (Span 双链表)     │                              │
│  └──────────────────┘  └──────────────────┘                              │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ Span 分配 / 归还
┌─────────────────────────────────▼───────────────────────────────────────┐
│                PageCache (后端 Backend，分片结构)                          │
│  Shard[0]   Shard[1]   ...  Shard[kMaxShardCount-1]   (生产默认 1 片)     │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │ 每片: std::mutex | SpanList[1..128] | ObjectPool<Span>           │   │
│  │       Span 切分 | owner-shard-local 合并 | PageMap 写保护         │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ 页级分配 / 释放
┌─────────────────────────────────▼───────────────────────────────────────┐
│                       PageAllocator (OS 交互层)                          │
│  mmap() / munmap() / madvise() | 2 MiB 映射缓存 | MAP_POPULATE | THP 提示 │
└─────────────────────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────────────────┐
  │              PageHeapScavenger (后台线程，侧路)                        │
  │  每 1s 扫描空闲 Span → 空闲 >10s 者摘除 → 锁外 madvise(MADV_DONTNEED) │
  │  → 成功则标记 purged 并归还所属分片（RSS 治理）                        │
  └─────────────────────────────────────────────────────────────────────┘
```

### 3.2 数据流向

```
                     am_malloc(size)
                          │
              ┌───────────▼───────────┐
              │ size > 32KiB ?        │  MAX_TC_SIZE
              └───┬───────────────┬───┘
           Yes    │               │ No
              ┌───▼───────────┐  ┌▼──────────────────┐
              │ 页对齐 + page_num │ │ ThreadCache      │
              │ PageCache.AllocSpan│ │ FreeList.pop()   │  ← 无锁快路径
              └───┬───────────┘  └─┬─────────────────┘
                  │                │ miss → FetchFromCentralCache
                  │           ┌────▼─────────────────┐
                  │           │ CentralCache.FetchRange│
                  │           │  ├ TransferCache (SpinLock)
                  │           │  └ SpanList (Mutex + Bitmap)
                  │           └────┬─────────────────┘
                  │           ┌────▼─────────────────┐
                  │           │ 需新 Span: GetOneSpan │
                  │           └────┬─────────────────┘
                  │           ┌────▼─────────────────┐
                  └───────────┤ PageCache.AllocSpan  │
                              │  ├ exact hit / split │
                              │  └ OS refill (128 页)│
                              └────┬─────────────────┘
                              ┌────▼─────────────────┐
                              │ PageAllocator.SystemAlloc │
                              └──────────────────────┘

                     am_free(ptr)
                          │
              ┌───────────▼───────────┐
              │ ptr == nullptr ?      │──Yes──→ return
              └───────────┬───────────┘
              ┌───────────▼───────────┐
              │ PageMap.GetSpan(ptr)  │──miss──→ return（未识别指针忽略）
              └───────────┬───────────┘
              ┌───────────▼───────────┐
              │ aligned_obj_size==0 ? │──Yes──→ PageCache.ReleaseSpan（大对象）
              └───────────┬───────────┘                    ├ 超大: ClearRange + munmap
                          │ No                            └ ≤128页: 合并左右邻居后入桶
              ┌───────────▼───────────┐
              │ ThreadCache.Deallocate │
              │  FreeList.push (无锁)   │
              └───────────┬───────────┘
              ┌───────────▼───────────┐
              │ size > max_size ?     │──No──→ return
              └───────────┬───────────┘
              ┌───────────▼───────────┐
              │ DeallocateSlowPath    │
              │  CentralCache.ReleaseListToSpans
              │   ├ TransferCache (未满)  → 存入指针数组
              │   └ 满 → Span.FreeObject (Bitmap)
              │        └ Span 空 → PageCache.ReleaseSpan → 合并
              └───────────────────────┘
```

### 3.3 线程模型与后台任务

- **分配线程**：普通业务线程通过 TLS 持有各自 ThreadCache，互不干扰。
- **后台 Scavenger**：`std::jthread` 单线程，懒启动（首次慢路径），见 5.8。
- **TLS 生命周期**：每个线程的 `ThreadCacheCleaner` 在析构时 drain 全部对象回 CentralCache 并释放 ThreadCache 元数据（见 5.1）。

---

## 4. 核心数据结构

### 4.1 FreeList（自由链表）

```cpp
class FreeList {
    FreeBlock* head_;    // 嵌入式链表头
    size_t    size_;     // 当前对象数
    size_t    max_size_; // 水位线（慢启动增长 / 超配衰减）
    size_t    overages_; // 连续溢出裁剪计数（衰减信号）
};

struct FreeBlock {
    FreeBlock* next;     // 复用对象体前 8 字节
};
```

**设计要点**：

- **嵌入式链表**：空闲对象自身内存存储 `next` 指针，零元数据分配。
- **LIFO**：最近释放的对象最先被重新分配，提升缓存局部性。
- **慢启动水位线**：`max_size_` 初始为 1，refill 压力下指数热身到 1 个 batch、再线性增长到 batch 的有界倍数。
- **超配衰减**：连续溢出裁剪（`overages_`）使 `max_size_` 批量下降，防止瞬时突发把长期水位线钉在高位。
- **预取**：`pop()` 对下一节点发出 `prefetch` 指令隐藏内存延迟。
- **线程归属**：FreeList 非线程安全；ThreadCache 内线程私有，CentralCache 临时列表由调用方加锁。

### 4.2 Span（连续页区间元数据）

```cpp
struct alignas(CACHE_LINE_SIZE) Span {
    Span*  next;      // 侵入式链表（SpanList / Scavenger 临时链）
    Span*  prev;
    uint64_t start_page_idx;  // 起始全局页号
    uint32_t page_num;        // 页数量
    uint16_t flags;           // kUsedMask | kCommittedMask
    uint16_t size_class_idx;  // CentralCache 桶索引（小对象）
    uint32_t aligned_obj_size;// 对齐后对象大小；0 = 大对象
    uint32_t capacity;        // Span 可容纳对象数
    uint32_t use_count;       // 已从 bitmap 取出的对象数
    uint32_t scan_cursor;     // bitmap 扫描游标（首个可能空闲的 word）
    uint32_t obj_offset;      // 页首到首个对象的偏移
    uint32_t owner_shard_id;  // 所属 PageCache 分片
    uint64_t last_used_time_ms; // 最近一次空闲时间（Scavenger 判定用）
};
static_assert(sizeof(Span) == 64);  // 严格单缓存行
```

**设计要点**：

- **单缓存行**：整个 Span 紧凑为 64 字节，消除跨缓存行访问；字段非原子，访问必须持有相应锁。
- **位图内嵌**：bitmap 存放在 Span 对应页区间的**首部**（`GetBitmap()` 返回页基址），`obj_offset` 记录 bitmap 之后、按对齐要求填充后的数据区偏移，避免存储指针。
- **状态标志**：`used`（是否处于活跃分配路径）与 `committed`（虚拟区间是否有物理页 backing）。
- **大对象判定**：`aligned_obj_size == 0` 表示页级大对象（不经过 CentralCache），free 时直接归还 PageCache。
- **Scavenger 协作**：`last_used_time_ms` 记录进入空闲态的时间，供后台回收判定。

### 4.3 SpanList（Span 双向链表）

```cpp
class alignas(CACHE_LINE_SIZE) SpanList {
    Span head_;   // 内联哨兵（环形链表）
};
```

**设计要点**：

- **内联哨兵**：`head_` 即哨兵节点，插入/删除无空指针分支。
- **不持有所有权**：SpanList 只维护链接，Span 元数据归 PageCache 分片 ObjectPool 所有；调用方必须提供对应的 shard/bucket 锁。
- 提供 `push_front` / `push_back` / `pop_front` / `erase` 等 O(1) 操作。

### 4.4 Bucket（CentralCache 桶结构）

```cpp
struct alignas(CACHE_LINE_SIZE) Bucket {
    // Tier 1: TransferCache（快路径）
    SpinLock transfer_cache_lock;
    size_t   transfer_cache_count;
    size_t   transfer_cache_capacity;
    void**   transfer_cache;      // 借用自连续 backing 的切片

    // Tier 2: SpanList（慢路径）
    std::mutex span_list_lock;
    SpanList   span_list;
};
```

**设计要点**：

- **双层架构**：TransferCache 处理批量对象流转（O(1) 数组拷贝、无 bitmap、无 PageMap 查询）；SpanList 处理 Span 内对象切分（bitmap 扫描）。
- **锁分离**：SpinLock 服务短临界区，Mutex 服务复杂操作。
- **缓存行对齐**：消除相邻桶之间的 false sharing。

### 4.5 ObjectPool（定长元数据池）

```cpp
template<typename T, size_t CHUNK_SIZE = 64 * 1024>
class ObjectPool {
    // chunk_list_ / free_list_ / data_ / remain_bytes_ / mutex_
};
```

**设计要点**：

- **递归规避基础设施**：Span、RadixNode 等元数据全部由 ObjectPool 分配，chunk 来自 `PageAllocator::SystemAlloc`，绝不进入 `am_malloc`。
- **chunk 式管理**：每次向 OS 申请一个约 64 KiB 的页对齐 chunk，内部按定长对象切分；空闲对象经侵入式 free list 复用。
- **生命周期**：池持有全部 chunk 直到 `ReleaseMemory()` 或析构统一归还 OS；`New()` 失败抛 `std::bad_alloc`（调用方必须回滚已申请的映射）。
- **注意**：`Delete()` 后槽位可被立即复用——这适用于内部受锁保护的场景；PageMap 无锁读者可能持有的已发布 descriptor 的延迟回收协议属演进方向（见第 12 章）。

### 4.6 PageMap 树形结构

```cpp
struct alignas(PAGE_SIZE) RadixRootNode {  // 静态存储，页对齐
    std::array<std::atomic<void*>, RADIX_ROOT_SIZE> children;
};
struct alignas(PAGE_SIZE) RadixNode {       // ObjectPool 分配，页对齐
    std::array<std::atomic<void*>, RADIX_NODE_SIZE> children;  // 512
};
```

- **四层结构**：一个胖 root + 三层 9-bit RadixNode（`RADIX_NODE_BITS = 9`，每节点 512 个原子 child）。
- **地址分解**：`page_id = 地址 >> PAGE_SHIFT`，按 `[root bits][9][9][9]` 分解；`RADIX_ROOT_BITS = PAGE_ID_BITS - 27`（48-bit VA 时 root 9 bit；57-bit VA 时 root 18 bit）。
- **节点生命周期**：root 为静态存储；普通节点由 `ObjectPool<RadixNode>` 分配，运行期只增不减，仅 `Reset()`/进程结束统一回收。
- **叶子语义**：leaf 保存 `Span*`（acquire/release 发布），中间节点保存 `RadixNode*`。

---

## 5. 子模块详细设计

### 5.1 ThreadCache（线程缓存）

#### 5.1.1 设计目标

前端缓存，处理绝大多数分配/释放请求，热路径**完全无锁**。

#### 5.1.2 核心实现

```cpp
class alignas(CACHE_LINE_SIZE) ThreadCache {
    std::array<FreeList, kNumSizeClasses> free_lists_;  // 线程私有

    AM_ALWAYS_INLINE void* Allocate(size_t aligned_size) noexcept {
        size_t idx = SizeClass::Index(aligned_size);
        auto& list = free_lists_[idx];
        if (!list.empty()) AM_LIKELY {
            return list.pop();              // 快路径：单次 pop
        }
        return FetchFromCentralCache(list, aligned_size);  // AM_NOINLINE 冷路径
    }

    AM_ALWAYS_INLINE void Deallocate(void* ptr, size_t aligned_size) {
        size_t idx = SizeClass::Index(aligned_size);
        auto& list = free_lists_[idx];
        list.push(ptr);                     // 快路径：单次 push
        if (list.size() > list.max_size()) AM_UNLIKELY {
            DeallocateSlowPath(list, aligned_size);       // AM_NOINLINE 冷路径
        }
    }
};
```

#### 5.1.3 关键特性

| 特性 | 实现方式 | 优势 |
|------|----------|------|
| TLS 存储 | `thread_local ThreadCache*`（initial-exec 模型） | 每线程独立实例，无竞争 |
| 内联优化 | `AM_ALWAYS_INLINE` 快路径 / `AM_NOINLINE` 慢路径 | 消除调用开销且不污染 I-cache |
| 分支预测 | `AM_LIKELY` / `AM_UNLIKELY` | 优化 CPU 流水线 |
| 配额策略 | 慢启动 + 超配衰减（见 4.1） | 自适应缓存容量，防抖动 |

#### 5.1.4 TLS 生命周期

- TLS 指针由 `ThreadCacheCleaner`（thread_local 析构）管理：线程退出时先 `ReleaseAll()` 将所有 FreeList 对象批量归还 CentralCache，再销毁 ThreadCache 元数据（`SystemFree` 归还 OS）。
- `g_ThreadCacheAlreadyDestructed` 防止 TLS 析构阶段的递归分配重建缓存。
- ThreadCache 元数据本身页对齐分配（`PageAllocator::SystemAlloc`），避免递归。

### 5.2 CentralCache（中心缓存）

#### 5.2.1 设计目标

中端缓存：跨线程均衡对象流转，管理各 size class 的 Span 集合，与 PageCache 交互。

#### 5.2.2 双层桶架构

```
                        Bucket[i]
  ┌─────────────────────────────────────────────────────────┐
  │ TransferCache（快路径）                                    │
  │   void* ptr_array[capacity=8×batch]  | SpinLock          │
  │   操作: O(1) 数组访问 | 无位图 | 无 PageMap 查询            │
  ├─────────────────────────────────────────────────────────┤
  │ SpanList（慢路径）                                         │
  │   Span 双链表 | std::mutex                                │
  │   操作: Span 切分 | Bitmap 扫描 | PageMap 查询             │
  └─────────────────────────────────────────────────────────┘
```

#### 5.2.3 关键操作

- **`FetchRange(block_list, batch_num, aligned_size)`**：先从 TransferCache 抓取（SpinLock 短临界区），不足部分再从 SpanList 的 Span 上按 bitmap 切分对象；两阶段返回总数可能小于请求数。
- **`ReleaseListToSpans(start, aligned_size)`**：对象链先入 TransferCache，溢出时逐个归还所属 Span bitmap（`Span::FreeObject`）。
- **`GetOneSpan`（锁协议）**：SpanList 锁内发现无可用 Span 时，**先释放桶锁**再进入 PageCache 获取新 Span，取得后重新加锁插入——保证锁顺序"桶锁内不进 PageCache"，避免死锁。

#### 5.2.4 TransferCache 初始化

- 初始化时一次性从 `PageAllocator` 申请连续 backing，按 `capacity = kCapScale × batch_size = 8 × batch` 切分给每个桶的 `transfer_cache`。
- **单次系统调用 + 连续内存布局**：避免多次调用系统 malloc 的初始化死锁，同时提升缓存局部性。

### 5.3 PageCache（页缓存）

#### 5.3.1 分片结构

```cpp
class PageCache {
    static constexpr uint16_t kMaxShardCount = 4;
    uint16_t active_shard_count_{1};        // 生产默认仅启用 1 片
    std::array<PageCacheShard, kMaxShardCount> shards_{};

    uint16_t SelectShardForAlloc(size_t page_num) noexcept { return 0; }  // 当前恒选 0
};

class alignas(CACHE_LINE_SIZE) PageCacheShard {
    std::mutex mutex_;
    std::array<SpanList, MAX_PAGE_NUM + 1> span_lists_;  // 按页数索引，0 未用
    ObjectPool<Span> span_pool_;
};
```

**设计要点**：

- **每片独立同步**：一片一把 `std::mutex` + 独立空闲链表数组 + 独立 Span 元数据池。
- **Owner 模型**：`AllocSpan` 选定分片后记录 `span->owner_shard_id`；`ReleaseSpan` 按 owner 回到唯一分片，**合并只在所属分片内进行**（owner-shard-local）。
- **路由策略**：当前 `SelectShardForAlloc` 恒返回 0（生产单分片）；测试可通过 `SetActiveShardCountForTest` 扩展分片数。
- 保留无分片的 legacy 实现（`USE_PAGECACHE_SHARD` 未定义时），仅作对照。

#### 5.3.2 Span 分配（AllocSpanLocked）

```
while (true) {
    1. 超大请求（page_num > 128）
         PageAllocator::SystemAlloc → 池化 Span 元数据 → PageMap::SetSpan → 返回
         （超大步进不进入页数桶，但保留池化元数据，free 可定位）
    2. 精确匹配：span_lists_[page_num] 非空 → pop_front，标记 used+committed
    3. 切分：从第一个非空的大桶 span_lists_[i] (i > page_num) pop
         - 池化分配小 Span 元数据（失败则大 Span 原样放回，失败原子）
         - 原地收缩大 Span（start_page_idx += n, page_num -= n）并回对应桶
         - PageMap::SetSpan 发布两个 Span
    4. OS refill：SystemAlloc(128 页) → 池化元数据 → 入桶[128] → 继续循环
}
```

- 元数据分配失败（`std::bad_alloc`）时：已摘除的 Span 放回原桶、已申请的 OS 映射归还系统，**不发布任何半初始化状态**。

#### 5.3.3 Span 释放与合并（ReleaseSpanLocked）

```
1. 超大 Span（> 128 页）：
     PageMap::ClearRange → PageAllocator::SystemFree → span_pool_.Delete
     （从不保留在页数桶中）
2. 可缓存 Span（≤ 128 页）：
     向左合并：左邻居存在且 !used 且 owner_shard_id 相同
              且合并后 ≤ MAX_PAGE_NUM → 摘除邻居、吸收页数、Delete 邻居元数据
     向右合并：同上
     重置小对象字段（aligned_obj_size/use_count/obj_offset/capacity = 0）
     标记 !used + committed，更新 last_used_time_ms
     push_front 到 span_lists_[page_num]
     PageMap::SetSpan（合并使邻居元数据失效，重写全部映射）
```

**合并约束**：合并绝不跨越 owner-shard 边界；合并上限 128 页；只有空闲（!used）Span 才参与合并。

#### 5.3.4 Reset

- 依次锁定活动分片：释放全部空闲 Span 映射、`span_pool_.ReleaseMemory()`、`PageMap::Reset()`。
- 仅用于测试隔离与受控销毁，调用方必须保证分配器处于静默期。

### 5.4 SizeClass（尺寸分级）

> 详细设计与验证（算法推演、编译期校验清单、测试矩阵）见 [01-size-class.md](01-size-class.md)。

#### 5.4.1 分级策略

混合映射：**线性区 + 几何步进区**，全部推导基于 `ALIGNMENT = alignof(std::max_align_t) = 16`：

```
线性区 [1, 128]：每 16B 一档，共 8 档（kLinearBucketCount = 128/16 = 8）
   16   32   48   64   80   96   112   128

几何区 (128, 32KiB]：每个 2 的幂区间分 4 档（kStepsPerGroup = 4）
   160  192  224  256 | 320  384  448  512 | ... | 24KiB ... 32KiB
```

#### 5.4.2 数学模型

```cpp
// Index(original_size)：请求 → 桶索引
if (size <= 128)      return (size - 1) >> kAlignShift;        // 线性区
else {
    msb         = bit_width(size - 1) - 1;                     // 区间编号
    group_idx   = msb - kLinearMsb;                            // 7 = log2(128)
    base_idx    = kLinearBucketCount + group_idx * kStepsPerGroup;
    shift       = msb - kStepShift;                            // 档内步长 = 2^shift
    group_offset = ((size - 1) >> shift) & (kStepsPerGroup - 1);
    return base_idx + group_offset;
}
// Size(idx)：桶索引 → 对象大小（Index 的右逆，保证 Size(Index(s)) >= s）
// 线性区: (idx + 1) << kAlignShift
// 几何区: 2^msb + (step_idx + 1) * 2^(msb - kStepShift)
```

#### 5.4.3 编译期查表

- `small_index_table_`：`[0, 1024]` 的 Index 查表（最热区间 O(1) 单次内存访问）；
- `size_table_`：Index → Size 全表；
- `batch_table_`：每 class 的单次搬运对象数 = `clamp(32KiB / class_size, 2, 512)`；
- `move_page_table_`：每 class 的 Span 页数 ≈ 8×batch 对象所需页数，下限 32 KiB、上限 128 页。

所有表均为 `consteval` 编译期生成，附大量 `static_assert`（单调性、逆映射、对齐倍数、MAX_TC_SIZE 幂次边界）。

#### 5.4.4 碎片率分析

| 区间 | 步长 | 最坏内部碎片 |
|------|------|--------------|
| [1, 128]（线性，16B 档） | 16 B | 93.75%（1B 请求→16B）；[1,128] 均匀分布累计 ≈ 10.4% |
| (128, 256]（几何组 0） | 32 B | ≈ 25% |
| (256, 512]（几何组 1） | 64 B | ≈ 25% |
| ... | 每档 = 区间上限的 1/4 | ≈ 25% |
| (16KiB, 32KiB] | 4 KiB | ≈ 25% |

几何区平均碎片 ≈ 12.5%（阶梯映射采用 jemalloc 风格，4 档/组，见 `01-size-class.md` §2.4）；同时所有 class size 均为 `ALIGNMENT` 的倍数，保证每个对象 16B 对齐。

#### 5.4.5 对齐保证

- 基础对齐：所有对象的对齐 = `alignof(std::max_align_t)`（x86-64/aarch64 为 16B）。
- Span 数据区起点按对齐填充（`obj_offset`），且所有 class size 为 16B 倍数，因此 16B 步进的对象序列天然保持 16B 对齐。

### 5.5 SpinLock（自旋锁）

#### 5.5.1 TTAS 算法

```cpp
class SpinLock {
    std::atomic<bool> locked_{false};

    void lock() noexcept {
        size_t spin_cnt = 0;
        while (true) {
            if (!locked_.load(std::memory_order_relaxed)) {   // Phase 1: 乐观读
                if (!locked_.exchange(true, std::memory_order_acquire)) {
                    return;                                    // Phase 2: 抢占成功
                }
            }
            detail::CPUPause();                                // Phase 3: 退避
            ++spin_cnt;
            if (spin_cnt > 2000) AM_UNLIKELY {                 // Phase 4: 让出 CPU
                std::this_thread::yield();
                spin_cnt = 0;
            }
        }
    }
    bool try_lock() noexcept { /* 乐观读 + acquire exchange，不等待 */ }
    void unlock() noexcept { locked_.store(false, std::memory_order_release); }
};
```

#### 5.5.2 性能优化点

| 优化技术 | 作用 |
|----------|------|
| Relaxed 读 | 避免缓存行失效时的总线风暴 |
| CPU Pause（x86 `_mm_pause` / aarch64 `yield`） | 提示 CPU 处于自旋，优化流水线 |
| 自适应退避 | 长时间等待让出时间片，避免饥饿 |
| Acquire/Release 语义 | 建立临界区内存边界 |

#### 5.5.3 与 std::mutex 对比

| 特性 | SpinLock | std::mutex |
|------|----------|------------|
| 适用场景 | 短临界区（< 100ns） | 长临界区 |
| 等待策略 | 忙等待 | 内核调度 |
| 上下文切换 | 无 | 有 |
| 适用层级 | TransferCache | SpanList、PageCacheShard |

### 5.6 PageMap（页映射基数树）

#### 5.6.1 查询（GetSpan，完全无锁）

```cpp
Span* PageMap::GetSpan(size_t page_id) {
    auto* curr = root_.load(std::memory_order_acquire);
    if (!curr) return nullptr;
    // [root bits][9][9][9] 逐层 acquire 加载
    const size_t i0 = page_id >> 27;                    // 越界检查 RADIX_ROOT_SIZE
    const size_t i1 = (page_id >> 18) & 0x1FF;
    const size_t i2 = (page_id >> 9)  & 0x1FF;
    const size_t i3 = page_id & 0x1FF;
    ...
    return static_cast<Span*>(p3->children[i3].load(std::memory_order_acquire));
}
```

**契约**：每个 acquire 与写者对相应 child 的 release store 配对，保证 reader 看到节点构造与 Span 发布前的初始化数据；任何一层为空即返回 null。读路径不获取任何锁。

#### 5.6.2 写入（SetSpan / ClearRange）

- **SetSpan**：懒分配中间节点（`radix_node_pool_.New()`，页对齐），release store 安装；叶子按连续段批量 release store。root 首次使用前从静态 `root_storage_` 发布。
- **ClearRange**：按叶子段 release store null；遇到空子树按覆盖范围整体跳过。
- **Reset**：root 置空 + `radix_node_pool_.ReleaseMemory()`。

**写入保护**：`SetSpan`/`ClearRange` 必须在所属 PageCache 分片锁内调用（当前生产单分片即全局串行化；多分片并发写属演进方向 R02/R08）。

#### 5.6.3 节点生命周期与内存序

- 普通 RadixNode 运行期**只增不减**，读者无需保护中间节点生命周期；仅 `Reset` 或进程结束时经 ObjectPool 统一回收。
- 发布使用 `memory_order_release`，查询使用 `memory_order_acquire`，计数/初始化使用 `relaxed`，禁止默认 `seq_cst`。

### 5.7 PageAllocator（OS 交互层）

#### 5.7.1 职责

封装全部 OS 内存原语，供 PageCache 与 ObjectPool 使用：

| 能力 | 说明 |
|------|------|
| `SystemAlloc(page_num)` | `mmap` 页对齐映射；可带 `MAP_POPULATE`（运行时开关）；失败重试（`MAX_ALLOC_RETRIES = 3`）；`mmap` 后按需 `madvise(MADV_HUGEPAGE)` 透明大页提示 |
| `SystemFree(ptr, page_num)` | 精确 2 MiB 映射经 `MADV_DONTNEED` 后进入**无锁双栈映射缓存**（容量 `HUGE_PAGE_CACHE_SIZE = 16`）；其余直接 `munmap` |
| 统计 | `PageAllocatorStats`：normal/huge 分配计数与字节、缓存命中/未命中、mmap/munmap/madvise 失败计数（relaxed 原子） |

#### 5.7.2 2 MiB 映射缓存

- 仅缓存**精确等于 2 MiB**（`HUGE_PAGE_SIZE`）且对齐的映射；madvise 后物理页已归还内核，地址空间留作复用，显著降低高频大对象分配的 mmap/munmap 系统调用与 VMA 抖动。
- 缓存为无锁双栈（lock-free dual-stack），`ReleaseHugePageCache()` 供测试/受控销毁。

### 5.8 PageHeapScavenger（后台回收）

#### 5.8.1 职责

周期性对 PageCache 中**长期空闲**的 Span 执行 `MADV_DONTNEED`，把物理页归还内核、保留虚拟地址（虚拟地址不 unmap，避免 VMA 抖动）。

#### 5.8.2 三阶段"摘除-回收-归还"协议

```
ScavengeOnePass（每 kScavengeIntervalMs = 1s 一轮，按桶 128 → 1 扫描）：
  [Phase 1 摘除] 持分片锁：
     遍历桶内空闲 Span，跳过 used / 未 committed 者；
     now - last_used_time_ms >= kIdleThresholdMs (10s) 者：
       从桶中 erase，SetUsed(true) 作为"detached 预留"，连入私有临时链
     释放锁
  [Phase 2 回收] 锁外执行：
     对每个 detached Span 调 madvise(MADV_DONTNEED)
     成功 → SetCommitted(false)；失败 → 保持 committed（best-effort）
  [Phase 3 归还] 重新持锁：
     逐 Span SetUsed(false)、更新 last_used_time_ms、push_back 回原桶
```

**不变量**：

- detached Span 不在任何 PageCache 桶中；`used` 标志阻止其他线程将其视为可合并邻居或可分配。
- `madvise` 系统调用在**锁外**执行（`GetMutex`/`GetSpanList` 为单分片下的 legacy 访问接口）。
- 失败不破坏状态：Span 回到原 owner 分片，committed 状态准确。
- 归还必须回原 owner 分片（当前单分片下即分片 0）。

#### 5.8.3 启动与生命周期

- **懒启动**：首次分配慢路径（`EnsureScavengerStarted`）才创建 `std::jthread`，避免进程启动成本；`AM_ENABLE_SCAVENGER=0` 可禁用。
- **停止**：`Stop()` 请求停止并 join，建立静默边界；等待使用 stop-aware `condition_variable_any`。

---

## 6. 内存分配/释放流程

### 6.1 分配流程

```
am_malloc(size)
  │
  ├─ 快路径: pTLSThreadCache != null 且 size <= 32KiB
  │    └─ ThreadCache::Allocate(SizeClass::RoundUp(size))
  │         ├─ FreeList.pop() ── 命中即返回（无锁，~3.8ns）
  │         └─ miss → FetchFromCentralCache（慢路径）
  │
  └─ am_malloc_slow_path(size)
       ├─ EnsureScavengerStarted()（懒启动后台回收）
       ├─ size > 32KiB（大对象）
       │    └─ AlignUp 到页 → page_num
       │         └─ PageCache::AllocSpan(page_num)
       │              ├─ 超大（>128 页）→ SystemAlloc 直接映射
       │              ├─ exact hit → pop 空闲 Span
       │              ├─ split → 切分大 Span
       │              └─ OS refill → SystemAlloc(128 页) 后循环
       │         └─ 返回 span->GetPageBaseAddr()
       ├─ TLS 未初始化 → CreateThreadCache()（SystemAlloc + placement new）
       └─ ThreadCache::Allocate(RoundUp(size))
```

### 6.2 释放流程

```
am_free(ptr)
  │
  ├─ ptr == nullptr → return
  ├─ PageMap::GetSpan(ptr) → miss → return（未识别指针忽略）
  │
  ├─ span->aligned_obj_size == 0（大对象）
  │    └─ PageCache::ReleaseSpan(span)
  │         ├─ > 128 页 → ClearRange + munmap + 元数据回收
  │         └─ ≤ 128 页 → 合并左右空闲邻居（同 owner、≤128 页）→ 入桶 → 重写映射
  │
  └─ 小对象
       ├─ TLS 未初始化 → CreateThreadCache()（失败则直接 ReleaseListToSpans 兜底）
       └─ ThreadCache::Deallocate(ptr, aligned_size)
            ├─ FreeList.push()（无锁快路径）
            └─ size > max_size → DeallocateSlowPath
                 └─ 批量归还 CentralCache::ReleaseListToSpans
                      ├─ TransferCache 未满 → 入指针数组
                      └─ 满 → Span::FreeObject（bitmap 置位，use_count--）
                           └─ Span 全空 → PageCache::ReleaseSpan → 合并
```

### 6.3 后台回收流程

```
PageHeapScavenger 线程（jthread，1s 周期）
  └─ ScavengeOnePass
       ├─ [锁] 扫描各页数桶，摘除空闲 >10s 的 Span（used 置位作预留）
       ├─ [无锁] madvise(MADV_DONTNEED) 逐个回收物理页
       └─ [锁] 回原桶并刷新 last_used_time_ms
```

---

## 7. 性能优化策略

| 优化技术 | 实现位置 | 效果 |
|----------|----------|------|
| 缓存行对齐 | ThreadCache、Bucket、PageCacheShard、Span（64B） | 消除 false sharing，单缓存行元数据 |
| 预取指令 | `FreeList::pop()` | 隐藏链表下一跳的内存延迟 |
| LIFO 策略 | FreeList、SpanList | 提升 L1/L2 命中率 |
| 编译期查表 | `small_index_table_`/`size_table_`/`batch_table_`/`move_page_table_` | 随机大小 O(1)，~26ns |
| 连续内存布局 | TransferCache backing、ObjectPool chunk | 减少 TLB miss |
| 分支预测 | `[[likely]]/[[unlikely]]`（AM_LIKELY/AM_UNLIKELY） | 热路径流水线优化 |
| 内联分工 | `AM_ALWAYS_INLINE` 快路径 / `AM_NOINLINE` 冷路径 | 消除调用开销且不污染 I-cache |
| 批量搬运 | FetchRange / ReleaseListToSpans | 摊薄锁开销，Span 一次服务多次 refill |
| Relaxed 统计 | `PageAllocatorStats` 计数器 | 观测不阻塞热路径 |
| 惰性初始化 | RuntimeConfig/PageCache/Scavenger 静态存储 + placement new | 避免单例构造递归 |

---

## 8. 线程安全与并发控制

### 8.1 锁层次结构

```
┌─────────────────────────────────────────────────────────────┐
│                     Lock Hierarchy                          │
├─────────────────────────────────────────────────────────────┤
│  Level 0: ThreadCache     → 无锁 (TLS)                     │
│  Level 1: TransferCache   → SpinLock (per bucket)          │
│  Level 2: SpanList        → std::mutex (per bucket)        │
│  Level 3: PageCacheShard  → std::mutex (per shard)         │
│  （PageMap 读：无锁；写：受所属分片锁保护）                    │
└─────────────────────────────────────────────────────────────┘
```

### 8.2 锁顺序规约与死锁预防

- **桶锁内不进 PageCache**：CentralCache 需新 Span 时先释放 SpanList 锁（`GetOneSpan` 的 lock-unlock-reacquire 协议），再进入 PageCache。
- **分片锁保护 PageMap 写**：`SetSpan`/`ClearRange` 只在持有所属分片锁时调用。
- **合并仅在本分片内**：`ReleaseSpan` 按 `owner_shard_id` 路由，不会跨分片加锁。
- 当前实现中部分 OS 系统调用（超大 Span 的 munmap、refill 的 mmap）仍在分片锁内执行；将系统调用移出锁（OS-out-of-lock）已列入演进方向（风险 R17）。

### 8.3 内存序选择

| 操作 | 内存序 | 原因 |
|------|--------|------|
| PageMap 中间节点/叶子读取 | acquire | 看到发布前的完整初始化数据 |
| PageMap 节点安装/叶子写入 | release | 发布 Span/节点数据 |
| SpinLock 等待轮询 | relaxed | 减少总线流量 |
| SpinLock 获取/释放 | acquire / release | 建立临界区边界 |
| 统计计数器 | relaxed | 纯观测，不参与同步 |
| Scavenger 启动标志 | acquire / acq_rel CAS | 保证单次启动 |

---

## 9. 内存碎片与 RSS 管理

### 9.1 内部碎片

**定义**：size class 分配大小与实际请求大小的差值。

**控制策略**：

- 几何区每档步长 = 区间上限的 1/4，最坏 ≈ 25%，平均 ≈ 12.5%；
- 线性区 16B 档仅影响 ≤128B 的极小请求（最坏 93.75% 只出现在 1B 请求）；
- 所有 class size 为 16B 倍数，杜绝对齐引入的额外碎片。

### 9.2 外部碎片

**定义**：空闲内存无法合并为大块连续内存。

**控制策略**：

- PageCache 释放时左右双向合并（owner-shard-local，上限 128 页）；
- 合并失败原子：元数据 OOM 时原 Span 原样回桶，不发布半状态；
- 超大 Span（> 128 页 = 512 KiB）从不驻留缓存，直接归还 OS，避免大块空洞。

### 9.3 RSS 治理

- **Scavenger**：空闲超过 10s 的 Span 周期性 `MADV_DONTNEED`，物理页归还内核、虚拟地址保留；
- **2 MiB 映射缓存**：大对象释放经 madvise 后地址空间复用，减少 VMA 数量与 RSS 残留；
- **内存观感**：分配器持有的地址空间（virtual）可能大于常驻物理内存（RSS），属预期行为。

### 9.4 统计与监控

```cpp
struct PageAllocatorStats {
    std::atomic<size_t> normal_alloc_count / _success / _bytes;   // 普通页分配
    std::atomic<size_t> huge_alloc_count / _success / _bytes;     // 2MiB 分配
    std::atomic<size_t> huge_cache_hit_count / huge_cache_miss_count; // 缓存命中
    std::atomic<size_t> free_count / free_bytes;
    std::atomic<size_t> normal_alloc_failed_count / huge_alloc_failed_count;
    std::atomic<size_t> mmap_enomem_count / mmap_other_error_count;
    std::atomic<size_t> munmap_failed_count / madvise_failed_count;
};
```

计数器均为 relaxed 原子，提供近似快照；精确到字节的分配器级 accounting 属演进方向（R16）。

---

## 10. 配置参数

### 10.1 编译期配置

```cpp
struct SystemConfig {
    constexpr static size_t PAGE_SIZE = 4096;
    constexpr static size_t PAGE_SHIFT = 12;
    constexpr static size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
    constexpr static size_t CACHE_LINE_SIZE = 64;
    constexpr static size_t BITMAP_BITS = 64;
    constexpr static size_t ALIGNMENT = alignof(std::max_align_t);  // 16
    static constexpr size_t VA_BITS = AM_USE_57BIT_VA ? 57 : 48;
    constexpr static size_t PAGE_ID_BITS = VA_BITS - PAGE_SHIFT;
};

struct SizeConfig {
    constexpr static size_t MAX_TC_SIZE = 32 * 1024;   // ThreadCache 上限
    constexpr static int kStepsPerGroup = 4;           // 每组档数
    constexpr static int kStepShift = 2;
    constexpr static size_t kSmallSizeThreshold = 1024;// 查表区间上限
};

struct PageConfig {
    constexpr static size_t MAX_PAGE_NUM = 128;        // 缓存 Span 最大页数
    constexpr static size_t RADIX_NODE_BITS = 9;       // 每节点位数
    constexpr static size_t RADIX_NODE_SIZE = 512;
    constexpr static size_t RADIX_ROOT_BITS = PAGE_ID_BITS - 27;
    constexpr static size_t MAX_ALLOC_RETRIES = 3;
    constexpr static size_t HUGE_PAGE_CACHE_SIZE = 16; // 2MiB 缓存容量
};
```

### 10.2 运行时配置

环境变量在首次初始化时读取一次，之后不可变；必须在第一次调用 `am_malloc` 前设置：

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `AM_USE_MAP_POPULATE` | false | 普通 mmap 请求附加 `MAP_POPULATE`（平台支持时） |
| `AM_ENABLE_SCAVENGER` | true | 是否在首次分配慢路径懒启动后台回收线程 |
| `AM_TC_SIZE` | 32KiB | 解析并上限 32KiB；当前仅暴露于 RuntimeConfig，**不影响固定的 32KiB 路由阈值** |

布尔值接受 `1/true/on/yes`（大小写不敏感、容忍空白）；字节数接受 `B/K/M/G/T` 二进制后缀（如 `16K`）。

### 10.3 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTS` | ON | 构建单元测试并启用测试辅助符号（`AMMALLOC_TEST`） |
| `BUILD_BENCHMARKS` | ON | 构建基准测试 |
| `USE_57BIT_VA` | OFF | 定义 `AM_USE_57BIT_VA`，PageMap 覆盖 57-bit 虚拟地址空间 |

---

## 11. 性能基准测试

### 11.1 测试场景（`tests/benchmark/` 实际用例）

| 分组 | 用例示例 | 关注指标 |
|------|----------|----------|
| 分配/释放 | `BM_Malloc_Churn`、`BM_Malloc_Deep_Churn`、`BM_am_malloc_free_pair_random_size` | 纯 malloc/free 延迟 |
| 多线程 | `BM_am_malloc_multithread`、`BM_am_malloc_multithread_random` | 扩展性、竞争 |
| PageMap | `BM_PageMap_GetSpan_Hit/Miss/Mixed90_10` | 无锁查询延迟 |
| PageCache | `BM_PageCache_Alloc_ExactBucketHit`、`BM_PageCache_Alloc_SplitFromLarger`、`BM_PageCache_Release_MergeLeftRight`、Contention 系列 | 后端路径 |
| PageAllocator | `BM_PageAllocator_*`（2M 缓存命中/冷 miss/多线程竞争、4K 路径） | OS 交互 |
| SizeClass | `BM_SizeClass_Index_Small/Large`、`BM_SizeClass_CalculateBatchSize` 等 | 映射开销 |

### 11.2 当前基线（16 核 CPU，Release 构建实测）

| 场景 | 数值 |
|------|------|
| 单线程快路径 | ~3.8 ns |
| 随机大小分配 | ~26.0 ns |
| 16 线程 64B 极高压竞争 | ~8.9 µs / 100+ GiB/s |

### 11.3 对比方法

- 性能结果强依赖 CPU、编译器、构建类型、内核与机器负载：**同一机器、Release 构建、一致运行时环境**下进行前后对比；
- 使用 `--benchmark_filter` 选择聚焦子集；性能相关改动必须跑聚焦基准并报告差异。

---

## 12. 演进方向

本文档描述的是**当前已验证实现**。`docs/improvement-plan/`（18 个专题 + 路线图）定义了将 ammalloc 建设为可安全接管进程内存、对标 TCMalloc/jemalloc 的工业级分配器的目标架构与分阶段路线，要点如下：

| 阶段 | 主题 | 代表性工作 |
|------|------|-----------|
| Phase 0-1 | 基线冻结与显式分配器正确性 | 测试假覆盖修复、TLS RAII/OOM、PageMap stable metadata、split/coalesce 事务化、fault injection |
| Phase 2 | 完整 ABI 与自举 | C malloc 族 / C++ new-delete 族、BootstrapAllocator、LD_PRELOAD 目标、core 移除 spdlog |
| Phase 3 | 内存效率与控制面 | Frontend byte budget、incremental decay、stats/control schema、cgroup 压力感知 |
| Phase 4 | 并发分片与 LargeExtent | Central size-class shard、PageMap multi-writer、region ownership、LargeExtent 索引 |
| Phase 5 | NUMA / per-CPU / Hugepage 实验 | rseq Frontend、NUMA-local pipeline、HugepageFiller（默认关闭、独立基准、一键回退） |
| Phase 6 | aethermind 内存基础设施 | versioned arena、KV/request arena、压力优先级、模型级采样 |
| Phase 7 | 安全与生产成熟 | ReleaseChecked、pointer encoding、guarded sampling、quarantine、ABI 稳定、SLO |

关键原则：正确性与生命周期安全先于极限性能；后续阶段不得绕过前置门禁；高阶优化（per-CPU、NUMA、HugepageFiller、epoch 回收）在基础正确性建立前只做隔离研究。详见 [`improvement-plan/README.md`](../improvement-plan/README.md)。

---

## 附录

### A. 文件结构

```
ammalloc/
├── AGENTS.md                    # 模块级 AI 执行指南
├── CMakeLists.txt               # 顶层构建配置
├── include/ammalloc/            # 公共头文件
│   ├── ammalloc.h               # 公共 API：am_malloc / am_free
│   ├── assert.h                 # AMMALLOC_CHECK / AMMALLOC_DCHECK
│   ├── attributes.h             # 编译器属性与 builtin 包装宏
│   ├── common.h                 # 地址/对齐/CPU/配置解析工具 + ObjectPool
│   ├── config.h                 # 编译期与运行期配置
│   ├── free_list.h              # 嵌入式 LIFO 空闲链表（FreeBlock/FreeList）
│   ├── thread_cache.h           # TLS 前端缓存
│   ├── central_cache.h          # 全局中端缓存
│   ├── page_cache.h             # 分片页缓存 + PageMap 基数树
│   ├── page_allocator.h         # OS 交互层 + ObjectPool
│   ├── page_heap_scavenger.h    # 后台回收线程
│   ├── span.h                   # Span 元数据与 SpanList
│   ├── size_class.h             # 尺寸类别映射
│   └── spin_lock.h              # TTAS 自旋锁
├── src/                         # 实现文件
│   ├── ammalloc.cpp             # 主入口实现
│   ├── common.cpp               # 公共工具实现
│   ├── config.cpp               # 环境变量解析
│   ├── thread_cache.cpp         # ThreadCache 实现
│   ├── central_cache.cpp        # CentralCache 实现
│   ├── page_cache.cpp           # PageCache 与 PageMap 实现
│   ├── page_allocator.cpp       # PageAllocator 实现
│   ├── page_heap_scavenger.cpp  # 后台回收实现
│   └── span.cpp                 # Span bitmap 管理
├── tests/
│   ├── unit/                    # GoogleTest 单元测试（单可执行）
│   └── benchmark/               # Google Benchmark（单可执行）
└── docs/
    ├── README.md                # 文档索引与术语表
    ├── issues.md                # 问题与待办跟踪
    ├── designs/                 # 架构与模块设计（NN- 编号）
    │   └── research/            # 调研备忘
    ├── improvement-plan/        # 演进提案（18 个专题 + README）
    ├── guides/                  # 编码/注释/测试/文档规范
    ├── api/                     # 公共 API 参考
    ├── decisions/               # 架构决策记录（ADR）
    └── templates/               # 文档模板
```

### B. 相关设计文档与参考资料

- 本仓库子系统设计（与当前实现同步维护，完整索引见 [docs/README.md](../README.md)）：
  - `docs/designs/01-size-class.md`
  - `docs/designs/02-thread-cache.md`
  - `docs/designs/03-central-cache.md`
  - `docs/designs/04-page-cache.md`
  - `docs/designs/05-page-allocator.md`
  - `docs/designs/06-page-heap-scavenger.md`
  - `docs/designs/07-span-and-pagemap.md`
- 演进规划：`docs/improvement-plan/`（18 个专题文档 + README）
- 文档系统规范：`docs/guides/documentation-guide.md`；公共 API 参考：`docs/api/public-api.md`；架构决策记录：`docs/decisions/`
- 外部参考：
  1. **TCMalloc**: Google's Thread-Caching Malloc — https://github.com/google/tcmalloc
  2. **jemalloc**: A General Purpose malloc Implementation — https://github.com/jemalloc/jemalloc
  3. **mimalloc**: Microsoft's Malloc — https://github.com/microsoft/mimalloc
  4. **TCMalloc Design Document** — https://goog-perftools.sourceforge.net/doc/tcmalloc.html

---

**文档版本历史**:

| 版本 | 日期 | 作者 | 变更说明 |
|------|------|------|----------|
| v1.0 | 2026-02-26 | AetherMind Team | 初始版本 |
| v2.0 | 2026-08-19 | AetherMind Team | 全量对齐当前实现：分片 PageCache、四层 PageMap（48/57-bit）、Span 64B 布局、ObjectPool、PageAllocator 2MiB 缓存、PageHeapScavenger；更新 SizeClass 映射/碎片率/对齐契约、配置参数、性能基线；清理重复图表；新增"演进方向"章链接改进计划 |
| v2.0.1 | 2026-08-19 | AetherMind Team | §5.4 对齐 `01-size-class.md` v2.3：阶梯映射设计依据修正为 jemalloc 风格；线性区 [1,128] 均匀分布累计碎片修正为 ≈10.4%；增加指向子文档的链接 |
| v2.0.2 | 2026-08-19 | AetherMind Team | `FreeList`/`FreeBlock` 从 central_cache.h 迁出至独立头文件 `free_list.h`（上层头文件不再依赖中端头文件）；附录 A 文件结构同步 |
