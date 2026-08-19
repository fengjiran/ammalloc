# PageHeap Scavenger 设计方案 (ammalloc 内存归还机制)

## 1. 背景与目标 (Context & Goals)
`ammalloc` 目前采用三层缓存架构，虽然实现了高效的内存分配与复用，但缺乏主动将物理内存归还给操作系统的机制。长期运行后，进程的 RSS (Resident Set Size) 会持续处于高位。

**核心目标**:
- 实现后台异步清理机制。
- 将长期闲置的物理内存归还给 OS (Decommit)。
- **架构解耦**: 采用独立的 `PageHeapScavenger` 类负责清理逻辑，避免污染 `PageCache` 核心分配代码。

 **设计难点与挑战**  

在实现后台清理线程时，必须解决以下几个痛点：

- **锁竞争（Lock Contention）**：PageCache 是全局单例，受 std::mutex mutex_ 保护。如果后台线程频繁地锁住 PageCache 去扫描 128 个桶，会严重阻塞前端的正常 Alloc/Free 业务。
- **如何定义“闲置久了”？（Aging Mechanism）**：不能把刚还回来的 Span 立刻还给 OS，否则下一秒业务又申请，会导致频繁的缺页中断（Page Fault），性能雪崩。需要给 Span 加上“时间戳”或“代数（Generation）”。
- **如何释放？（MADV_DONTNEED vs munmap）**：对于小于等于 128 页的 Span，它们在 PageCache 的桶里。我们**不能**用munmap，因为这会破坏虚拟地址的连续性，导致以后无法再和相邻的 Span 合并。必须使用madvise(MADV_DONTNEED)。它会告诉内核释放物理内存，但保留虚拟地址映射。当业务再次访问这块地址时，内核会自动分配新的全零物理页。

## 2. 核心技术：MADV_DONTNEED
我们使用 `madvise(addr, length, MADV_DONTNEED)`。它保留虚拟地址空间 (VMA)，仅释放物理页，下次访问时通过缺页中断自动找回。

## 3. 架构设计 (Architecture)

### 3.1 核心组件：`PageHeapScavenger` (New Class)
该类负责管理后台线程的生命周期和清理逻辑。

- **职责**:
  1. 维护后台清理线程。
  2. 按照 `kScavengeIntervalMs` 定期被唤醒。
  3. 执行 `Scavenge()` 核心逻辑。
- **与 PageCache 交互**:
  - `Scavenge()` 需要持有 `PageCache::mutex_` 进行扫描。
  - 采用 **"Extract-Release-Return"** 策略以最小化持锁时间。
- 配置项：
  - `kScavengeIntervalMs` (清理间隔，默认 500ms) 。
  -  `kIdleThresholdMs` (闲置阈值，默认 5000ms)。

### 3.2 `Span` 结构体扩展
```cpp
struct Span {
    // ... 原有字段 ...
    int64_t last_used_ms{0};  // 最后一次归还到 PageCache 的时间戳 (毫秒)
    bool is_committed{true};  // 物理内存是否已提交 (false 表示已调用 MADV_DONTNEED)
};
```

## 4. 清理算法：摘除-释放-归还 (Extract-Release-Return)

为了避免在调用耗时的 `madvise` 时阻塞分配请求，`PageHeapScavenger` 执行以下循环：

1. **Lock PageCache**: 获取 `PageCache::mutex_`。
2. **Scan & Extract**: 遍历所有 `span_lists_` 桶。
   - 寻找 `is_used == false && is_committed == true && Now() - last_used_ms > kIdleThresholdMs` 的 Span。
   - 将符合条件的 Span 从 `PageCache` 的链表中**彻底摘除**，放入本地临时列表。
3. **Unlock PageCache**: 释放全局锁。此时其他线程可以正常进行分配，但不会用到正在释放的 Span。
4. **Physical Release**: 对摘除的 Span 调用 `madvise(..., MADV_DONTNEED)`，并将 `is_committed` 设为 `false`。
5. **Lock PageCache**: 重新获取全局锁。
6. **Return**: 将这些 Span 放回原来的 `span_lists_` 桶中。

## 5. 整合逻辑 (Integration)

### 5.1 PageCache 端的配合
- **ReleaseSpan**: 归还 Span 时，必须设置 `span->last_used_ms = Now()`。如果发生合并，合并后的新 Span 同样更新时间戳。
- **AllocSpan**: 从桶中取回 Span 时，如果其 `is_committed == false`，则将其标记为 `true`。

### 5.2 生命周期管理
`PageHeapScavenger` 作为一个单例或由 `PageCache` 持有的成员变量，在 `PageCache` 首次初始化时启动线程。支持平滑退出 (StopThread)。

## 6. 实现计划 (Implementation Plan)

1. **Phase 1 (Infrastructure)**: 
   - 修改 `Span` 结构。
   - 在 `PageAllocator` 中封装 `SystemDecommit`。
2. **Phase 2 (Scavenger Class)**: 
   - 实现 `PageHeapScavenger` 的线程循环和扫描逻辑。
3. **Phase 3 (Integration)**: 
   - 修改 `PageCache::ReleaseSpan` 更新时间戳。
   - 修改 `PageCache::AllocSpan` 重置 `is_committed`。
4. **Phase 4 (Validation)**: 
   - 编写测试用例，通过 `/proc/self/status` 观察 RSS 的动态回落。

---
*文档版本: 1.1 (Architectural Decoupling Update)*
*日期: 2026-03-02*
