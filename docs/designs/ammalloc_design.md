# AetherMind Memory Allocator (ammalloc) 技术设计文档

**版本**: v1.0  
**日期**: 2026-02-26  
**作者**: AetherMind Team

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
6. [内存分配流程](#6-内存分配流程)
7. [性能优化策略](#7-性能优化策略)
8. [线程安全与并发控制](#8-线程安全与并发控制)
9. [内存碎片管理](#9-内存碎片管理)
10. [配置参数](#10-配置参数)
11. [性能基准测试](#11-性能基准测试)
12. [未来优化方向](#12-未来优化方向)

---

## 1. 概述

### 1.1 项目背景

AetherMind Memory Allocator (ammalloc) 是一个高性能、多线程友好的内存分配器。其核心设计理念源于 Google TCMalloc，并结合现代 C++ 特性与底层微观架构优化，目标是在保持高吞吐量的同时，最小化内存碎片和锁竞争。

### 1.2 设计哲学

ammalloc 的设计遵循以下核心原则：

| 原则 | 描述 |
|------|------|
| **分层缓存** | 通过 ThreadCache → CentralCache → PageCache 三级架构，实现热点数据的局部化访问 |
| **锁粒度优化** | 从全局锁到桶锁，再到自旋锁，逐层降低锁竞争 |
| **批量操作** | 通过批量搬运减少跨层交互频率，摊薄锁获取开销 |
| **零拷贝设计** | 利用嵌入式链表（Embedded List）避免额外的内存分配 |
| **缓存友好** | 数据结构对齐、预取指令、LIFO 策略最大化 CPU 缓存命中率 |

---

## 2. 设计目标

### 2.1 性能目标

| 指标 | 目标值 | 说明 |
|------|--------|------|
| **极致的单线程性能** | < 5ns | 将绝大多数小内存分配/释放操作的指令数压榨到 10 条以内，实现纳秒级（~5ns）的分配延迟 |
| **无锁并发扩展性** | 近似线性扩展 | 通过线程局部存储（TLS）和流转缓存（TransferCache）隔离线程竞争，确保多核并发下吞吐量呈线性增长 |
| **抗内存碎片** | ~12.5% | 内部碎片：采用对数阶梯式（Logarithmic Stepped）的 SizeClass 映射，将内部碎片率严格控制在 12.5% 左右。外部碎片：底层采用基数树（Radix Tree）和伙伴算法变种，实现 O(1) 的连续物理页合并（Coalescing）。|
| **零外部依赖** | 无 | 核心分配逻辑不依赖任何 STL 堆分配容器，杜绝 malloc 递归死锁（Bootstrapping Problem） |

### 2.2 功能目标

- ✅ 支持小对象（≤ 32KB）的高效分配
- ✅ 支持大对象（> 32KB）的直接系统分配
- ✅ 支持多线程并发分配，无全局锁竞争
- ✅ 支持内存回收与 Span 合并
- ✅ 支持大页（Huge Page）优化

### 2.3 质量目标

- **可测试性**: 提供完整的单元测试和性能基准测试
- **可观测性**: 提供详细的统计信息和调试接口
- **可配置性**: 支持运行时参数调整

---

## 3. 整体架构

### 3.1 三层缓存架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Application Layer                              │
│                      am_malloc() / am_free()                            │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │
┌─────────────────────────────────▼───────────────────────────────────────┐
│                          ThreadCache (TLS)                               │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  FreeList[0]  FreeList[1]  FreeList[2]  ...  FreeList[N-1]       │   │
│  │     (8B)        (16B)        (24B)            (32KB)             │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  特性: Lock-Free | 线程私有 | O(1) 访问                                  │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ Slow Path (Cache Miss / Overflow)
                                  │
┌─────────────────────────────────▼───────────────────────────────────────┐
│                           CentralCache                                   │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  Bucket[0]      Bucket[1]      Bucket[2]      ...  Bucket[N-1]   │   │
│  │  ┌──────────┐   ┌──────────┐   ┌──────────┐       ┌──────────┐   │   │
│  │  │Transfer  │   │Transfer  │   │Transfer  │       │Transfer  │   │   │
│  │  │ Cache    │   │ Cache    │   │ Cache    │       │ Cache    │   │   │
│  │  │(SpinLock)│   │(SpinLock)│   │(SpinLock)│       │(SpinLock)│   │   │
│  │  ├──────────┤   ├──────────┤   ├──────────┤       ├──────────┤   │   │
│  │  │SpanList  │   │SpanList  │   │SpanList  │       │SpanList  │   │   │
│  │  │(Mutex)   │   │(Mutex)   │   │(Mutex)   │       │(Mutex)   │   │   │
│  │  └──────────┘   └──────────┘   └──────────┘       └──────────┘   │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  特性: 桶锁隔离 | TransferCache 快速路径 | Span 管理慢速路径              │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ Span Allocation / Release
                                  │
┌─────────────────────────────────▼───────────────────────────────────────┐
│                            PageCache                                     │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  SpanList[1]  SpanList[2]  SpanList[3]  ...  SpanList[128]       │   │
│  │   (1 page)    (2 pages)   (3 pages)        (128 pages)           │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  特性: 全局锁 | Span 合并 | 页级内存管理                                  │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ System Allocation
                                  │
┌─────────────────────────────────▼───────────────────────────────────────┐
│                          PageAllocator                                   │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  mmap() / munmap() | madvise() | Huge Page Support               │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  特性: 系统调用封装 | 大页优化 | 内存对齐                                 │
└─────────────────────────────────────────────────────────────────────────┘
```



1.1 三层缓存架构概览
┌─────────────────────────────────────────────────────────────────┐
│                         am_malloc/am_free                        │
└─────────────────────┬───────────────────────────────────────────┘
                      │
    ┌─────────────────▼──────────────────┐
    │       ThreadCache (TLS)            │  ← 完全无锁，Fast Path ~3.8ns
    │  ┌─────────┐ ┌─────────┐          │
    │  │FreeList │ │FreeList │ ...      │  LIFO 嵌入式链表
    │  │ (8B)    │ │ (16B)   │          │
    │  └─────────┘ └─────────┘          │
    └─────────────────┬──────────────────┘
                      │ Slow Path (空/满)
    ┌─────────────────▼──────────────────┐
    │       CentralCache (全局)          │  ← 细粒度桶锁
    │  ┌─────────────────────────┐       │
    │  │   Bucket[SizeClass]     │       │
    │  │  ┌───────────────┐      │       │
    │  │  │ TransferCache │ ←────┼───────┼── SpinLock，O(1) 数组
    │  │  │ (指针数组)     │      │       │
    │  │  └───────────────┘      │       │
    │  │  ┌───────────────┐      │       │
    │  │  │   SpanList    │ ←────┼───────┼── Mutex，Bitmap 扫描
    │  │  │ (Span 双链表)  │      │       │
    │  │  └───────────────┘      │       │
    │  └─────────────────────────┘       │
    └─────────────────┬──────────────────┘
                      │ Slow Path (Span 耗尽)
    ┌─────────────────▼──────────────────┐
    │        PageCache (全局)            │  ← 全局大锁
    │  ┌─────────────────────────┐       │
    │  │   SpanList[1..128]      │       │  空闲 Span 按页数分类
    │  │  (Span 双链表数组)       │       │
    │  └─────────────────────────┘       │
    │  ┌─────────────────────────┐       │
    │  │      PageMap            │       │  4层 RadixTree
    │  │   (PageID → Span*)      │       │  读无锁，写受保护
    │  └─────────────────────────┘       │
    └─────────────────┬──────────────────┘
                      │
    ┌─────────────────▼──────────────────┐
    │      PageAllocator (OS 接口)        │
    │   ┌─────────────┐ ┌─────────────┐   │
    │   │ Normal Page │ │  Huge Page  │   │  mmap/munmap
    │   │  (4KB)      │ │   (2MB)     │   │  HugePageCache
    │   └─────────────┘ └─────────────┘   │
    └─────────────────────────────────────┘



### 3.2 数据流向

```
                    ┌─────────────────────┐
                    │   am_malloc(size)   │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │   size > 32KB ?     │
                    └──────────┬──────────┘
                      Yes │         │ No
                          │         │
              ┌───────────▼───┐  ┌──▼────────────────┐
              │ PageAllocator │  │ ThreadCache      │
              │ (直接系统分配) │  │ FreeList.Pop()   │
              └───────────────┘  └──┬────────────────┘
                                     │
                          ┌──────────▼──────────┐
                          │   Cache Hit ?       │
                          └──────────┬──────────┘
                            Yes │         │ No
                                │         │
                   ┌────────────┘  ┌──────▼────────────┐
                   │               │ CentralCache      │
                   │               │ TransferCache     │
                   ▼               │ Fetch (SpinLock)  │
              ┌─────────┐         └──┬────────────────┘
              │ Return  │            │
              │ Object  │     ┌──────▼────────────┐
              └─────────┘     │ Cache Hit ?       │
                              └──────────┬────────┘
                                Yes │         │ No
                                    │         │
                       ┌────────────┘  ┌──────▼────────────┐
                       │               │ SpanList          │
                       ▼               │ AllocObject()     │
                  ┌─────────┐         │ (Mutex + Bitmap)  │
                  │ Return  │         └──┬────────────────┘
                  │ Objects │            │
                  └─────────┘     ┌──────▼────────────┐
                                  │ PageCache         │
                                  │ AllocSpan()       │
                                  │ (Global Mutex)    │
                                  └──┬────────────────┘
                                     │
                              ┌──────▼────────────┐
                              │ PageAllocator     │
                              │ SystemAlloc()     │
                              └───────────────────┘
```

---

## 4. 核心数据结构

### 4.1 FreeList（自由链表）

```cpp
class FreeList {
    FreeBlock* head_;      // 链表头指针
    uint32_t size_;        // 当前元素数量
    uint32_t max_size_;    // 最大容量（动态调整）
};

struct FreeBlock {
    FreeBlock* next;       // 嵌入式链表节点
};
```

**设计要点**：
- **嵌入式链表**: 利用对象自身的内存空间存储 `next` 指针，避免额外分配
- **动态水位线**: `max_size_` 根据使用模式动态增长，平衡内存占用与命中率
- **LIFO 策略**: 最近释放的对象最先被重新分配，提升缓存局部性

### 4.2 Span（内存跨度）

```cpp
struct Span {
    // 双向链表节点
    Span* next;
    Span* prev;
    
    // 页缓存信息
    size_t start_page_idx;    // 起始页 ID
    size_t page_num;          // 页数量
    
    // 对象分配信息
    size_t obj_size;          // 对象大小
    size_t use_count;         // 已分配对象数
    size_t capacity;          // 对象容量
    void* data_base_ptr;      // 数据起始地址
    
    // 位图管理
    uint64_t* bitmap;         // 分配位图
    size_t bitmap_num;        // 位图块数量
    size_t scan_cursor;       // 扫描游标
    
    // 状态标记
    bool is_used;             // 是否在使用中
};
```

**设计要点**：
- **位图分配**: 使用 64 位整数数组追踪对象分配状态
- **游标优化**: `scan_cursor` 记录上次扫描位置，避免每次从头开始
- **内嵌位图**: 位图数据直接存储在 Span 的起始地址，减少一次间接访问

### 4.3 SpanList（Span 双向链表）

```cpp
class SpanList {
    Span head_;               // 哨兵节点
    std::mutex mutex_;        // 桶锁
};
```

**设计要点**：
- **哨兵节点**: 简化边界条件处理，避免空指针检查
- **循环双向链表**: 支持高效的头部/尾部插入删除
- **缓存行对齐**: `alignas(64)` 避免 False Sharing

### 4.4 Bucket（CentralCache 桶结构）

```cpp
struct alignas(64) Bucket {
    // Tier 1: Transfer Cache (Fast Path)
    SpinLock transfer_cache_lock;
    size_t transfer_cache_count;
    size_t transfer_cache_capacity;
    void** transfer_cache;
    
    // Tier 2: Span List (Slow Path)
    std::mutex span_list_lock;
    SpanList span_list;
};
```

**设计要点**：
- **双层架构**: Transfer Cache 处理快速路径，Span List 处理慢速路径
- **锁分离**: SpinLock 用于快速操作，Mutex 用于复杂操作
- **缓存行对齐**: 消除不同桶之间的 False Sharing

---

## 5. 子模块详细设计

### 5.1 ThreadCache（线程缓存）

#### 5.1.1 设计目标

ThreadCache 是内存分配的**前端（Frontend）**，负责处理绝大多数的分配请求。其核心目标是实现**无锁、O(1) 时间复杂度**的内存操作。

#### 5.1.2 核心实现

```cpp
class alignas(64) ThreadCache {
    std::array<FreeList, kNumSizeClasses> free_lists_;
    
public:
    AM_ALWAYS_INLINE void* Allocate(size_t size) noexcept {
        size_t idx = SizeClass::Index(size);
        auto& list = free_lists_[idx];
        
        // Fast Path: Lock-Free Pop
        if (!list.empty()) AM_LIKELY {
            return list.pop();
        }
        
        // Slow Path: Fetch from CentralCache
        return FetchFromCentralCache(list, SizeClass::RoundUp(size));
    }
    
    AM_ALWAYS_INLINE void Deallocate(void* ptr, size_t size) {
        size_t idx = SizeClass::Index(size);
        auto& list = free_lists_[idx];
        
        // Fast Path: Lock-Free Push
        list.push(ptr);
        
        // Slow Path: Return to CentralCache
        if (list.size() >= list.max_size()) AM_UNLIKELY {
            DeallocateSlowPath(list, size);
        }
    }
};
```

#### 5.1.3 关键特性

| 特性 | 实现方式 | 优势 |
|------|----------|------|
| **TLS 存储** | `thread_local` 关键字 | 每线程独立实例，无竞争 |
| **内联优化** | `AM_ALWAYS_INLINE` 宏 | 消除函数调用开销 |
| **分支预测** | `AM_LIKELY` / `AM_UNLIKELY` | 优化 CPU 流水线 |
| **动态水位线** | Slow Start 策略 | 自适应调整缓存大小 |

#### 5.1.4 动态水位线策略

```cpp
void* FetchFromCentralCache(FreeList& list, size_t size) {
    // ... fetch logic ...
    
    // Slow Start: 类似 TCP 拥塞控制
    if (list.max_size() < batch_num * 16) {
        list.set_max_size(list.max_size() + batch_num / 4);
    }
    
    return list.pop();
}
```

**策略说明**：
- 初始 `max_size = 1`
- 每次从 CentralCache 获取对象时，按 `batch_num / 4` 增长
- 最大不超过 `batch_num * 16`
- 避免一次性占用过多内存

---

### 5.2 CentralCache（中心缓存）

#### 5.2.1 设计目标

CentralCache 是内存分配的**中间层（Middle-end）**，负责：
1. 在多个 ThreadCache 之间平衡内存资源
2. 管理特定 Size Class 的 Span 集合
3. 与 PageCache 交互获取/释放内存页

#### 5.2.2 双层桶架构

```
┌─────────────────────────────────────────────────────────────┐
│                         Bucket[i]                           │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Transfer Cache (Fast Path)              │   │
│  │  ┌─────────────────────────────────────────────┐    │   │
│  │  │ void* ptr_array[capacity]                   │    │   │
│  │  │ SpinLock lock                               │    │   │
│  │  │ size_t count                                │    │   │
│  │  └─────────────────────────────────────────────┘    │   │
│  │                                                      │   │
│  │  操作: O(1) 数组访问 | 无位图操作 | 无 PageMap 查询   │   │
│  └─────────────────────────────────────────────────────┘   │
│                            │                               │
│                     Cache Miss                             │
│                            ▼                               │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Span List (Slow Path)                   │   │
│  │  ┌─────────────────────────────────────────────┐    │   │
│  │  │ SpanList span_list                          │    │   │
│  │  │ std::mutex lock                             │    │   │
│  │  └─────────────────────────────────────────────┘    │   │
│  │                                                      │   │
│  │  操作: Span 切分 | Bitmap 扫描 | PageMap 查询        │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

#### 5.2.3 FetchRange 实现

```cpp
size_t CentralCache::FetchRange(FreeList& block_list, size_t batch_num, size_t obj_size) {
    auto& bucket = buckets_[SizeClass::Index(obj_size)];
    
    // ========================================
    // Phase 1: Transfer Cache (Fast Path)
    // ========================================
    bucket.transfer_cache_lock.lock();
    size_t grab_count = std::min(batch_num, bucket.transfer_cache_count);
    for (size_t i = 0; i < grab_count; ++i) {
        local_ptrs[i] = bucket.transfer_cache[--bucket.transfer_cache_count];
    }
    bucket.transfer_cache_lock.unlock();
    
    // ========================================
    // Phase 2: Span List (Slow Path)
    // ========================================
    if (fetched < batch_num) {
        std::unique_lock<std::mutex> lock(bucket.span_list_lock);
        // 从 Span 分配对象（涉及 Bitmap 操作）
        // ...
    }
    
    return fetched;
}
```

#### 5.2.4 TransferCache 初始化

```cpp
void CentralCache::InitTransferCache() {
    // 1. 计算所有桶需要的总指针数量
    size_t total_ptrs = 0;
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        size_t batch_num = SizeClass::CalculateBatchSize(SizeClass::Size(i));
        total_ptrs += kCapScale * batch_num;  // 容量 = 8 * batch_size
    }
    
    // 2. 一次性从系统分配大块内存
    void* p = PageAllocator::SystemAlloc(page_num);
    
    // 3. 按需分割给各个桶
    auto** cur_ptr = static_cast<void**>(p);
    for (size_t i = 0; i < kNumSizeClasses; ++i) {
        buckets_[i].transfer_cache = cur_ptr;
        cur_ptr += buckets_[i].transfer_cache_capacity;
    }
}
```

**设计亮点**：
- **单次系统调用**: 避免多次调用 `malloc` 导致的初始化死锁
- **连续内存布局**: 提升缓存局部性
- **容量策略**: `capacity = 8 * batch_size`，提供充足的缓冲空间

---

### 5.3 PageCache（页缓存）

#### 5.3.1 设计目标

PageCache 是内存分配的**后端（Backend）**，负责：
1. 管理以页为单位的内存块（Span）
2. 实现 Span 的合并（Coalescing）以减少外部碎片
3. 与操作系统交互获取/释放内存

#### 5.3.2 Span 分配算法

```cpp
Span* PageCache::AllocSpanLocked(size_t page_num, size_t obj_size) {
    while (true) {
        // 1. 超大请求: 直接走系统
        if (page_num > MAX_PAGE_NUM) {
            return AllocFromSystem(page_num, obj_size);
        }
        
        // 2. 精确匹配: 从对应桶获取
        if (!span_lists_[page_num].empty()) {
            return span_lists_[page_num].pop_front();
        }
        
        // 3. 分割: 从更大的 Span 切分
        for (size_t i = page_num + 1; i <= MAX_PAGE_NUM; ++i) {
            if (!span_lists_[i].empty()) {
                return SplitSpan(i, page_num, obj_size);
            }
        }
        
        // 4. 系统补充: 分配 128 页的大块
        void* ptr = PageAllocator::SystemAlloc(MAX_PAGE_NUM);
        // 插入到 span_lists_[128]，下次循环会触发分割
    }
}
```

#### 5.3.3 Span 合并算法

```cpp
void PageCache::ReleaseSpan(Span* span) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 向左合并
    while (true) {
        Span* left = PageMap::GetSpan(span->start_page_idx - 1);
        if (!left || left->is_used || 
            span->page_num + left->page_num > MAX_PAGE_NUM) {
            break;
        }
        MergeLeft(span, left);
    }
    
    // 2. 向右合并
    while (true) {
        Span* right = PageMap::GetSpan(span->start_page_idx + span->page_num);
        if (!right || right->is_used ||
            span->page_num + right->page_num > MAX_PAGE_NUM) {
            break;
        }
        MergeRight(span, right);
    }
    
    // 3. 插入回空闲列表
    span_lists_[span->page_num].push_front(span);
}
```

#### 5.3.4 PageMap（基数树）

```cpp
// 三层基数树，支持 48 位虚拟地址空间
// 每层 9 位，共 27 位，覆盖 2^27 * 4KB = 512GB 地址空间

struct RadixNode {
    std::atomic<void*> children[512];  // 2^9 = 512
};

class PageMap {
    static std::atomic<RadixNode*> root_;
    
public:
    static Span* GetSpan(void* ptr) {
        size_t page_id = reinterpret_cast<uintptr_t>(ptr) >> 12;
        
        // 三层查找
        RadixNode* n1 = root_->children[page_id >> 27];
        RadixNode* n2 = n1->children[(page_id >> 18) & 0x1FF];
        RadixNode* n3 = n2->children[(page_id >> 9) & 0x1FF];
        
        return static_cast<Span*>(n3->children[page_id & 0x1FF]);
    }
};
```

**设计优势**：
- **无锁读取**: 使用原子操作，读取路径完全无锁
- **O(1) 查找**: 固定 3 次指针跳转
- **内存效率**: 按需分配节点，稀疏地址空间不占用内存

---

### 5.4 SizeClass（尺寸分级）

#### 5.4.1 分级策略

ammalloc 采用**混合分级策略**，结合线性映射和对数步进：

```
Size Class 分级示意图:

  8B   16B   24B   32B   40B   48B   56B   64B   ...  128B
  │    │     │     │     │     │     │     │          │
  └────┴─────┴─────┴─────┴─────┴─────┴─────┴──────────┘
                    线性映射 (8字节步长)
                    
 160B  192B  224B  256B  320B  384B  448B  512B  ...  32KB
  │    │     │     │     │     │     │     │          │
  └────┴─────┴─────┴─────┴─────┴─────┴─────┴──────────┘
                    对数步进 (每 2 倍区间分 4 档)
```

#### 5.4.2 数学模型

**线性区间** ($size \le 128$):
$$Index = \lfloor (size - 1) / 8 \rfloor$$

**对数步进区间** ($size > 128$):
$$msb = \lfloor \log_2(size - 1) \rfloor$$
$$group\_idx = msb - 7$$
$$base\_idx = 16 + group\_idx \times 4$$
$$step = \lfloor (size - 1) / 2^{msb - 2} \rfloor \mod 4$$
$$Index = base\_idx + step$$

#### 5.4.3 碎片率分析

| Size Range | Step Size | Max Fragmentation |
|------------|-----------|-------------------|
| 1 - 128 B | 8 B | 53% (8B 桶分配 1B) |
| 129 - 256 B | 32 B | 20% |
| 257 - 512 B | 64 B | 20% |
| 513 - 1024 B | 128 B | 20% |
| 1025 - 2048 B | 256 B | 20% |
| ... | ... | ... |
| 16KB - 32KB | 4KB | 25% |

**平均碎片率**: < 12.5%（符合 TCMalloc 设计目标）

---

### 5.5 SpinLock（自旋锁）

#### 5.5.1 TTAS 算法

ammalloc 采用 **Test-and-Test-and-Set (TTAS)** 自旋锁：

```cpp
class SpinLock {
    std::atomic<bool> locked_{false};
    
public:
    void lock() noexcept {
        size_t spin_cnt = 0;
        while (true) {
            // Phase 1: Test (乐观读)
            if (!locked_.load(std::memory_order_relaxed)) {
                // Phase 2: Test-and-Set (尝试抢占)
                if (!locked_.exchange(true, std::memory_order_acquire)) {
                    return;  // 成功获取锁
                }
            }
            
            // Phase 3: Backoff
            details::CPUPause();
            ++spin_cnt;
            
            // Phase 4: Yield (避免 CPU 饥饿)
            if (spin_cnt > 2000) AM_UNLIKELY {
                std::this_thread::yield();
                spin_cnt = 0;
            }
        }
    }
    
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }
};
```

#### 5.5.2 性能优化点

| 优化技术 | 作用 |
|----------|------|
| **Relaxed 读** | 避免缓存行失效时的总线风暴 |
| **CPU Pause** | 提示 CPU 处于自旋状态，优化流水线 |
| **自适应退避** | 长时间等待时让出 CPU，避免饥饿 |
| **Acquire/Release 语义** | 保证内存可见性 |

#### 5.5.3 与 std::mutex 对比

| 特性 | SpinLock | std::mutex |
|------|----------|------------|
| 适用场景 | 短临界区 (< 100ns) | 长临界区 |
| 等待策略 | 忙等待 | 内核调度 |
| 上下文切换 | 无 | 有 |
| 内存开销 | 1 byte | ~40 bytes |
| 适用层级 | TransferCache | SpanList, PageCache |

---

## 6. 内存分配流程

### 6.1 分配流程详解

```
am_malloc(size)
    │
    ├─ size == 0 → 返回 nullptr
    │
    ├─ size > 32KB (MAX_TC_SIZE)
    │   │
    │   ├─ size > 512KB (MAX_PAGE_NUM * PAGE_SIZE)
    │   │   └─ PageAllocator::SystemAlloc() → 直接 mmap
    │   │
    │   └─ size ≤ 512KB
    │       └─ PageCache::AllocSpan() → 大对象 Span
    │
    └─ size ≤ 32KB
        │
        ├─ ThreadCache::Allocate(size)
        │   │
        │   ├─ FreeList 非空 (Fast Path)
        │   │   └─ list.pop() → 返回对象
        │   │
        │   └─ FreeList 为空 (Slow Path)
        │       │
        │       ├─ CentralCache::FetchRange()
        │       │   │
        │       │   ├─ TransferCache 非空 (Fast Path)
        │       │   │   └─ 从指针数组批量获取
        │       │   │
        │       │   └─ TransferCache 为空 (Slow Path)
        │       │       │
        │       │       ├─ SpanList 非空
        │       │       │   └─ Span::AllocObject() (Bitmap 扫描)
        │       │       │
        │       │       └─ SpanList 为空
        │       │           └─ PageCache::AllocSpan()
        │       │               └─ PageAllocator::SystemAlloc()
        │       │
        │       └─ 返回第一个对象，其余存入 FreeList
        │
        └─ 返回对象指针
```

### 6.2 释放流程详解

```
am_free(ptr)
    │
    ├─ ptr == nullptr → 直接返回
    │
    ├─ 大对象 (size > 32KB)
    │   │
    │   ├─ size > 512KB
    │   │   └─ PageAllocator::SystemFree() → 直接 munmap
    │   │
    │   └─ size ≤ 512KB
    │       └─ PageCache::ReleaseSpan() → 尝试合并
    │
    └─ 小对象 (size ≤ 32KB)
        │
        └─ ThreadCache::Deallocate(ptr, size)
            │
            ├─ FreeList 未满 (Fast Path)
            │   └─ list.push(ptr) → 直接插入
            │
            └─ FreeList 已满 (Slow Path)
                │
                ├─ 批量取出 batch_num 个对象
                │
                └─ CentralCache::ReleaseListToSpans()
                    │
                    ├─ TransferCache 未满 (Fast Path)
                    │   └─ 存入指针数组
                    │
                    └─ TransferCache 已满 (Slow Path)
                        │
                        ├─ PageMap::GetSpan(ptr) → 找到所属 Span
                        │
                        ├─ Span::FreeObject(ptr) → 更新 Bitmap
                        │
                        └─ Span 完全空闲
                            └─ PageCache::ReleaseSpan() → 合并
```

---

1.3 分配/释放流程
分配流程 (am_malloc)
am_malloc(size)
  ├── [Fast] size ≤ 32KB && TLS 存在?
  │     └── ThreadCache::Allocate()
  │           ├── SizeClass::Index(size) → O(1) 查表
  │           └── FreeList::pop() → 无锁，~3.8ns
  │
  └── [Slow] 大块内存或 TLS 未初始化
        └── am_malloc_slow_path()
              ├── [大块] PageCache::AllocSpan() → 直接系统分配
              └── [小块] 创建 ThreadCache → FetchFromCentralCache()
                        └── CentralCache::FetchRange()
                              ├── [Fast] TransferCache (SpinLock)
                              └── [Slow] Span::AllocObject() (Mutex + Bitmap)
                                    └── [需新 Span] PageCache::AllocSpan()
                                          ├── 查找空闲列表
                                          ├── 切分大 Span
                                          └── 系统分配 (mmap)
释放流程 (am_free)
am_free(ptr)
  ├── PageMap::GetSpan(ptr) → 无锁 RadixTree 查询
  ├── [大块] span->obj_size == 0 → PageCache::ReleaseSpan()
  │
  └── [小块] ThreadCache::Deallocate()
        ├── FreeList::push() → 无锁
        └── [缓存满] DeallocateSlowPath()
              └── CentralCache::ReleaseListToSpans()
                    ├── [Fast] TransferCache (SpinLock)
                    └── [满] Span::FreeObject()
                          └── [Span 空] PageCache::ReleaseSpan()
                                ├── 合并左邻居
                                ├── 合并右邻居
                                └── 插入空闲列表



分配流程 (am_malloc)
am_malloc(size)
  ↓
[pTLSThreadCache null?] ──Yes──→ am_malloc_slow_path()
  ↓ No                                   ↓
[size > MAX_TC_SIZE?] ──Yes──→ PageCache.AllocSpan() → PageAllocator.SystemAlloc()
  ↓ No                                   ↓
ThreadCache.Allocate(size)           CreateThreadCache()
  ↓                                    ↓
SizeClass.Index(size) ──→ free_lists_[idx].pop() [Fast Path, Lock-Free]
  ↓
[empty?] ──Yes──→ FetchFromCentralCache()
                    ↓
              CentralCache.FetchRange()
                ├── Fast: TransferCache [SpinLock]
                └── Slow: Span.AllocObject() [Mutex + Bitmap]
                      ↓
                [need new Span?] ──Yes──→ PageCache.AllocSpan()
                                              ↓
                                        [oversized?] ──Yes──→ SystemAlloc
                                              ↓ No
                                        SpanList 查找/切分
释放流程 (am_free)
am_free(ptr)
  ↓
[ptr == null?] ──Yes──→ Return
  ↓
PageMap.GetSpan(ptr) ──→ [not found?] ──Yes──→ Return
  ↓
span.obj_size == 0? ──Yes──→ PageCache.ReleaseSpan() [大块内存]
  ↓
[pTLSThreadCache null?] ──Yes──→ am_free_slow_path()
  ↓                                ↓
ThreadCache.Deallocate()      CreateThreadCache()
  ↓                                ↓
free_lists_[idx].push(ptr)    CentralCache.ReleaseListToSpans()
  ↓                                ↓
[size >= max_size?] ──Yes──→   TransferCache [Fast Path]
DeallocateSlowPath()           Span.FreeObject() [Slow Path]
  ↓                                ↓
Return batch to CentralCache   [span empty?] ──Yes──→ PageCache.ReleaseSpan()
                                    ↓
                              [Coalescing: 合并左右邻居]

## 7. 性能优化策略

### 7.1 缓存优化

| 优化技术 | 实现位置 | 效果 |
|----------|----------|------|
| **缓存行对齐** | Bucket, SpanList | 消除 False Sharing |
| **预取指令** | FreeList::pop() | 隐藏内存延迟 |
| **LIFO 策略** | FreeList, SpanList | 提升缓存命中率 |
| **连续内存** | TransferCache 初始化 | 减少TLB Miss |

### 7.2 分支预测优化

```cpp
// 使用 [[likely]] / [[unlikely]] 指导编译器
if (!list.empty()) AM_LIKELY {
    return list.pop();  // 热路径
}

if (list.size() >= list.max_size()) AM_UNLIKELY {
    DeallocateSlowPath(list, size);  // 冷路径
}
```

### 7.3 内联优化

```cpp
// 关键路径强制内联
AM_ALWAYS_INLINE void* Allocate(size_t size) noexcept;

// 冷路径禁止内联
AM_NOINLINE static void* FetchFromCentralCache(...);
```

### 7.4 编译期计算

```cpp
// Size -> Index 查找表在编译期生成
constexpr static auto small_index_table_ = []() consteval {
    std::array<uint8_t, 1025> table{};
    for (size_t sz = 0; sz <= 1024; ++sz) {
        table[sz] = CalculateIndex(sz);
    }
    return table;
}();
```

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
│  Level 3: PageCache       → std::mutex (global)            │
└─────────────────────────────────────────────────────────────┘

锁获取顺序: PageCache → SpanList → TransferCache
         (必须从下往上获取，避免死锁)
```

### 8.2 死锁预防

```cpp
// CentralCache::GetOneSpan() 中的锁释放策略
Span* CentralCache::GetOneSpan(Bucket& bucket, ...) {
    // 必须先释放 SpanList 锁
    lock.unlock();
    
    // 再获取 PageCache 锁
    Span* span = PageCache::GetInstance().AllocSpan(...);
    
    // 重新获取 SpanList 锁
    lock.lock();
    bucket.span_list.push_front(span);
    return span;
}
```

### 8.3 内存序选择

| 操作 | 内存序 | 原因 |
|------|--------|------|
| PageMap 读取 | acquire | 确保看到完整的 Span 数据 |
| PageMap 写入 | release | 确保 Span 数据对其他线程可见 |
| SpinLock 等待 | relaxed | 减少总线流量 |
| SpinLock 获取 | acquire | 建立临界区边界 |
| SpinLock 释放 | release | 刷新写缓冲区 |

---

## 9. 内存碎片管理

### 9.1 内部碎片

**定义**: 分配大小与实际使用大小的差值

**控制策略**:
- Size Class 分级设计，最大碎片率 < 25%
- 小对象（≤ 128B）使用 8 字节对齐，碎片率可控
- 大对象使用对数步进，平衡碎片率与桶数量

### 9.2 外部碎片

**定义**: 空闲内存无法合并为大块连续内存

**控制策略**:
- PageCache 实现 Span 合并
- Span 完全空闲时自动归还给 PageCache
- 合并算法支持左右双向合并

### 9.3 碎片监控

```cpp
struct MemoryStats {
    size_t total_allocated;      // 总分配量
    size_t total_requested;      // 用户请求量
    size_t internal_fragmentation; // 内部碎片
    size_t free_spans;           // 空闲 Span 数量
    size_t largest_free_span;    // 最大连续空闲块
};
```

---

## 10. 配置参数

### 10.1 编译期配置

```cpp
struct SystemConfig {
    constexpr static size_t PAGE_SIZE = 4096;        // 页大小
    constexpr static size_t CACHE_LINE_SIZE = 64;    // 缓存行大小
    constexpr static size_t ALIGNMENT = 16;          // 默认对齐
};

struct SizeConfig {
    constexpr static size_t MAX_TC_SIZE = 32 * 1024; // ThreadCache 上限
    constexpr static int kStepsPerGroup = 4;         // 每组步数
};

struct PageConfig {
    constexpr static size_t MAX_PAGE_NUM = 128;      // 最大页数
    constexpr static size_t RADIX_BITS = 9;          // 基数树位数
};
```

### 10.2 运行时配置

```cpp
class RuntimeConfig {
    size_t max_tc_size_;           // 可调整的 TC 上限
    size_t huge_page_cache_size_;  // 大页缓存数量
    bool use_map_populate_;        // 是否预分配物理页
};
```

**环境变量支持**:
- `AMMALLOC_MAX_TC_SIZE`: 设置 ThreadCache 最大对象大小
- `AMMALLOC_HUGE_PAGE_CACHE`: 设置大页缓存数量
- `AMMALLOC_USE_MAP_POPULATE`: 启用物理页预分配

---

## 11. 性能基准测试

### 11.1 测试场景

| 测试名称 | 描述 | 关注指标 |
|----------|------|----------|
| `BM_Malloc_Churn` | 固定窗口分配释放 | 纯 malloc/free 延迟 |
| `BM_Malloc_Deep_Churn` | 大批量分配释放 | CentralCache 性能 |
| `BM_am_malloc_multithread` | 多线程并发 | 扩展性 |
| `BM_*_random_size` | 随机大小分配 | 综合性能 |

### 11.2 预期性能

| 场景 | ammalloc | std::malloc | 提升 |
|------|----------|-------------|------|
| 单线程 8B 分配 | ~8ns | ~25ns | 3x |
| 单线程 64B 分配 | ~8ns | ~30ns | 3.5x |
| 4 线程 8B 分配 | ~10ns | ~80ns | 8x |
| 8 线程 8B 分配 | ~12ns | ~150ns | 12x |

### 11.3 Benchmark 代码示例

```cpp
template<size_t AllocSize, size_t WindowSize>
void BM_Malloc_Churn(benchmark::State& state) {
    std::array<void*, WindowSize> window{};
    size_t i = 0;
    
    for (auto _ : state) {
        size_t idx = i & (WindowSize - 1);
        void* old_ptr = window[idx];
        
        window[idx] = am_malloc(AllocSize);
        benchmark::DoNotOptimize(window[idx]);
        
        if (old_ptr) {
            am_free(old_ptr);
        }
        ++i;
    }
    
    // Cleanup
    for (void* ptr : window) {
        if (ptr) am_free(ptr);
    }
}
```

---

## 12. 未来优化方向

### 12.1 短期优化

| 优化项 | 描述 | 预期收益 |
|--------|------|----------|
| **NUMA 感知** | 根据 CPU 节点选择内存区域 | 多插槽服务器 20%+ |
| **大页支持** | 使用 2MB Huge Page | 减少TLB Miss |
| **更细粒度锁** | PageCache 分区锁 | 提升大对象并发 |

### 12.2 中期优化

| 优化项 | 描述 | 预期收益 |
|--------|------|----------|
| **采样分析** | 动态调整 Size Class | 减少碎片 |
| **热路径追踪** | 识别热点 Size Class | 优化缓存布局 |
| **内存压力感知** | 系统内存紧张时主动释放 | 避免OOM |

### 12.3 长期优化

| 优化项 | 描述 | 预期收益 |
|--------|------|----------|
| **分层后端** | 支持多种内存后端 | 灵活性 |
| **持久内存支持** | PMEM 优化 | 新硬件支持 |
| **容器化隔离** | cgroup 感知 | 云原生场景 |

---

## 附录

### A. 文件结构

```
include/ammalloc/
├── ammalloc.h          # 公共接口
├── config.h            # 配置参数
├── size_class.h        # 尺寸分级
├── thread_cache.h      # 线程缓存
├── central_cache.h     # 中心缓存
├── page_cache.h        # 页缓存
├── page_allocator.h    # 系统分配器
├── span.h              # Span 定义
├── spin_lock.h         # 自旋锁
└── common.h            # 公共工具

src/ammalloc/
├── ammalloc.cpp        # 接口实现
├── thread_cache.cpp    # ThreadCache 实现
├── central_cache.cpp   # CentralCache 实现
├── page_cache.cpp      # PageCache 实现
├── page_allocator.cpp  # PageAllocator 实现
├── span.cpp            # Span 实现
└── config.cpp          # 配置解析

tests/
├── unit/               # 单元测试
└── benchmark/          # 性能测试
    └── benchmark_memory_pool.cpp
```

### B. 参考资料

1. **TCMalloc**: Google's Thread-Caching Malloc
   - https://github.com/google/tcmalloc

2. **jemalloc**: A General Purpose malloc Implementation
   - https://github.com/jemalloc/jemalloc

3. **mimalloc**: Microsoft's Malloc
   - https://github.com/microsoft/mimalloc

4. **TCMalloc Design Document**
   - https://goog-perftools.sourceforge.net/doc/tcmalloc.html

---

**文档版本历史**:

| 版本 | 日期 | 作者 | 变更说明 |
|------|------|------|----------|
| v1.0 | 2026-02-26 | AetherMind Team | 初始版本 |
