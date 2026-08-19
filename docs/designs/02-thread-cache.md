# ThreadCache 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.0
- **日期**: 2026-08-19
- **关联代码**: [include/ammalloc/thread_cache.h](../../include/ammalloc/thread_cache.h) / [src/thread_cache.cpp](../../src/thread_cache.cpp) / [include/ammalloc/free_list.h](../../include/ammalloc/free_list.h)（`FreeList`/`FreeBlock`）
- **上游依赖**: `SizeClass`（Index/Size/CalculateBatchSize）、`CentralCache`（FetchRange/ReleaseListToSpans）
- **下游消费者**: `ammalloc.cpp`（`am_malloc`/`am_free` 主入口）
- **关联测试**: [tests/unit/test_thread_cache.cpp](../../tests/unit/test_thread_cache.cpp)
- **架构总览**: [ammalloc_design.md §5.1](ammalloc_design.md)

## 1. 背景与目标

ThreadCache 是分配器的前端缓存：每个线程一个 TLS 实例，处理绝大多数分配/释放请求。目标：

- 快路径（一次 FreeList pop/push）完全无锁、无系统调用，单线程极速路径约 3.8 ns（见架构总览性能基线）。
- 通过慢启动与水位线动态调节每类配额，防止线程长期钉住突发期缓存。

## 2. 职责与边界

- **提供**：`Allocate`/`Deallocate`（快路径）+ `ReleaseAll`（TLS 销毁时全量归还）。
- **请求**：`CentralCache::FetchRange`（refill）、`CentralCache::ReleaseListToSpans`（trim/全量归还）。
- **所有权**：FreeList 中的对象所有权始终属于分配器系统；ThreadCache 只持有借用。TLS 析构时 `ReleaseAll` 把全部对象归还 CentralCache 后再销毁 ThreadCache 元数据（`PageAllocator::SystemFree`）。
- **生命周期**：TLS 指针由 `thread_local ThreadCacheCleaner` 管理；`g_ThreadCacheAlreadyDestructed` 防止 TLS 析构阶段的递归分配重建缓存。

## 3. 关键数据结构

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| `ThreadCache::free_lists_` | 每个尺寸类别一个 LIFO FreeList | TLS 私有，无同步；`alignas(64)` 防伪共享 |
| `FreeList::head_` | 嵌入式空闲链头（对象自身存 next） | 仅所属线程读写 |
| `FreeList::size_` | 当前对象数（uint32） | 同上 |
| `FreeList::max_size_` | 该类高水位配额，初值 1 | 慢启动增长 / 超配衰减 |
| `FreeList::overages_` | 连续溢出 trim 计数 | 配额衰减信号 |

## 4. 并发模型

- **无锁**：ThreadCache 及其 FreeList 线程私有，唯一 mutator 是拥有线程。
- **TLS 模型**：`thread_local ThreadCache*`，initial-exec 模型。
- **无锁前提与代价**：快路径无锁依赖 TLS 私有实例（唯一 mutator 是拥有线程），避免共享 FreeList 的锁/原子/Cache line bouncing；代价是 TLS 访问本身（initial-exec 下为 FS 基址 + 偏移，成本远低于锁与原子竞争，背景见 [research/thread-local-and-thread-cache.md](research/thread-local-and-thread-cache.md)）。
- **跨线程 free**：`Deallocate` 直接 push 到释放线程自己的 FreeList，不触碰分配线程缓存，快路径保持无锁；代价是缓存归属漂移（对象可能留在非分配线程），由 CentralCache 水位线与 trim 回收平衡（见 §6.2）。
- **析构顺序**：线程退出 → `ThreadCacheCleaner` 析构 → `ReleaseAll()` 归还 CentralCache → `SystemFree` 释放元数据页；`g_ThreadCacheAlreadyDestructed` 置位防止析构期递归重建。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `Allocate` | `void* Allocate(size_t original_size) noexcept` | `@pre original_size <= MAX_TC_SIZE`；size=0 走最小类；快路径单次 pop，空列表走 `FetchFromCentralCache` | ✅ |
| `Deallocate` | `void Deallocate(void* ptr, size_t idx)` | `@pre ptr != nullptr`；`@pre idx < kNumSizeClasses`；用 Span 记录的 `size_class_idx` 避免重新映射；超配额走 `DeallocateSlowPath` | ✅ |
| `ReleaseAll` | `void ReleaseAll()` | 清空所有 FreeList 归还 CentralCache；重置配额 | ❌ |
| `GetMaxSizeForTest` / `GetOveragesForTest` | `size_t (size_t idx) const` | 测试专用观测接口 | ❌ |

## 6. 算法与流程

### 6.1 Refill（FetchFromCentralCache，慢路径）

- 按 `batch = CalculateBatchSize(aligned_size)`，只取 `min(batch, max_size)`（慢启动保持早期 refill 小批量）。
- 两阶段增长：`max_size < batch` 时指数预热（`inc = max(1, max_size)`）；`< batch*8` 时线性增长（`inc = batch/8`），上限 `batch*8`。
- 成功 refill 重置 `overages`（新需求抵消衰减趋势）。

### 6.2 Trim（DeallocateSlowPath，慢路径）

- 每次溢出最多归还一个 batch（`kMaxBatchSize` 内），避免一次清空本地缓存。
- 连续溢出（`overages >= 3`）且 `max_size > batch` 时，按一个 batch 衰减配额并重置计数；配额回到 batch 下限后不再衰减。

### 6.3 复杂度

所有路径 O(1)；慢路径每事件固定工作量（≤ 1 batch），无隐藏 O(N²)。

## 7. 边界条件与错误处理

- `size=0`：映射到最小尺寸类别（`Index(0)=0`）。
- `FetchRange` 返回 0：表示 OOM，`Allocate` 返回 nullptr。
- 配额下限：`max_size` 不低于一个 batch，避免衰减到 0。

## 8. 风险与权衡

- **per-thread 内存占用**：每类配额上限 8×batch，活跃线程多时会持有较多空闲对象；这是无锁快路径的代价，由 CentralCache 水位线与 trim 机制平衡。
- **TLS 生命周期成本**：线程退出时 `ThreadCacheCleaner` 析构触发全量归还（`ReleaseAll`），线程频繁创建/退出的场景会产生归还风暴；`g_ThreadCacheAlreadyDestructed` 防护析构期递归重建（生命周期问题分析见 [research/thread-local-and-thread-cache.md](research/thread-local-and-thread-cache.md)）。
- **快路径不含策略**：所有配额决策推迟到冷路径，保证常见分支轻。

## 9. 测试要点

`tests/unit/test_thread_cache.cpp`：

- 功能：`BasicAllocate`、`AllocateZero`、`BasicDeallocate`、`EdgeCases`、`DifferentSizeClasses`、`ReleaseAll`
- 配额：`SlowStartGrowthThenOveragesShrinkMaxSize`（增长与衰减）、`TriggerReleaseTooLongList`、`SlowStartAndScavenge`
- 并发：`MultiThreadStress`、`MultiThreadedAllocation`、`MultiThreadedDifferentSizes`

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| 2026-08-19 | 初版（由架构总览 §5.1 拆分扩展） | 文档系统落地 | — |
