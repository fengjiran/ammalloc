# CentralCache 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.0
- **日期**: 2026-08-19
- **关联代码**: [include/ammalloc/central_cache.h](../../include/ammalloc/central_cache.h) / [src/central_cache.cpp](../../src/central_cache.cpp) / [include/ammalloc/spin_lock.h](../../include/ammalloc/spin_lock.h)
- **上游依赖**: `SizeClass`（batch/span 页数策略）、`FreeList`（FetchRange 批量传输容器）、`PageCache`（AllocSpan/ReleaseSpan）、`PageMap`（`GetSpan`）、`PageAllocator`（TransferCache backing）
- **下游消费者**: `ThreadCache`（FetchRange/ReleaseListToSpans）
- **关联测试**: [tests/unit/test_central_cache.cpp](../../tests/unit/test_central_cache.cpp)
- **架构总览**: [ammalloc_design.md §5.2](ammalloc_design.md)

## 1. 背景与目标

CentralCache 是中端缓存：在 ThreadCache 与 PageCache 之间均衡小对象流转。目标：

- 批量搬运（batch transfer）摊薄跨层交互与锁开销。
- 快速路径（TransferCache 指针数组）O(1) 拷贝，慢速路径（SpanList + bitmap）仅在快路径不足时进入。

## 2. 职责与边界

- **提供**：`FetchRange`（向 ThreadCache 供批）、`ReleaseListToSpans`（回收对象链）。
- **请求**：`PageCache::AllocSpan` / `PageCache::ReleaseSpan`；`PageMap::GetSpan`（释放时定位归属 Span）。
- **所有权**：Span 元数据归 PageCache 所有，CentralCache 桶只借用（`span_list` 为借用链表）；对象的所有权按 bitmap 记录归 Span。
- **TransferCache 初始化**：构造时一次性从 `PageAllocator::SystemAlloc` 申请连续 backing，按 `capacity = 8 × batch` 切分给各桶——单次系统调用避免递归初始化，连续布局提升缓存局部性。
- **FreeList 配额字段**：`max_size_`/`overages_` 仅 ThreadCache 解释为策略；CentralCache 拥有的临时列表不使用。

## 3. 关键数据结构

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| `Bucket::transfer_cache` | 该类的指针数组（借自全局连续映射） | `SpinLock`（`transfer_cache_lock`）保护 |
| `Bucket::transfer_cache_count/capacity` | 有效指针数 / 容量 | 同上 |
| `Bucket::span_list` | 该类可分配 Span 的借用链表 | `std::mutex`（`span_list_lock`）保护；与 TransferCache 域分处不同缓存行 |
| `FreeBlock` | 空闲对象体内的侵入式 next 指针 | 无额外分配 |
| `Bucket` | 每尺寸类别一个桶 | `alignas(64)`；三缓存行分区，双锁与相邻桶均不共享缓存行（见 §3.1） |

### 3.1 Bucket 缓存行布局

`Bucket` 大小为 192B（3 个缓存行），每个缓存行承载一个独立热域：

| 缓存行 | 内容 | 保护 |
|---|---|---|
| 行 0 | `transfer_cache_lock` + `transfer_cache_count/capacity` + `transfer_cache` | `transfer_cache_lock` |
| 行 1 | `span_list_lock`（成员级 `alignas(64)` 起始） | `span_list_lock` 自身 |
| 行 2 | `span_list`（64 对齐内联哨兵） | `span_list_lock` |

- 两把桶锁可被不同线程**独立持有**（快路径锁与慢路径锁分时获取、可并发并存），分处不同缓存行避免跨核伪共享乒乓。
- `span_list_lock` 采用成员级 `alignas(64)` 而非手工 pad：由编译器计算填充量，不依赖 `std::mutex` 的具体大小（libstdc++ 40B / libc++ 8B 均适用）。
- 编译期不变量：`static_assert(offsetof(Bucket, span_list_lock) >= 64)`（双锁不同行）、`static_assert(sizeof(Bucket) % 64 == 0)`（`std::array` 连续排布下相邻桶零重叠）。

## 4. 并发模型

- **双锁分层**：`SpinLock` 保护 TransferCache（短临界区，快路径）；`std::mutex` 保护 SpanList 与 bitmap 操作（慢路径）。
- **锁顺序（allocator lock order）**：**桶锁内不进 PageCache**。`GetOneSpan` 在 `span_list_lock` 内发现无可用 Span 时先 `unlock()`，再进入 PageCache，取回后重新加锁插入；`ReleaseListToSpans` 在空 Span 归还 PageCache 前同样先释放桶锁。此顺序避免 CentralCache 桶锁 → PageCache 分片锁的倒置死锁。
- **预取发布**：SpanList 中预取的指针在**离开 Span bitmap 锁域后**才写入 TransferCache；TransferCache 满时多余对象走 `ReleaseListToSpans` 正常归还路径。
- **伪共享防护**：`Bucket` 三缓存行分区（§3.1），`transfer_cache_lock` 与 `span_list_lock` 分处不同行；相邻桶因 `sizeof(Bucket) % 64 == 0` 亦零重叠。
- **单例**：进程级单例，构造即初始化 TransferCache。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `FetchRange` | `size_t FetchRange(FreeList&, size_t batch_num, size_t aligned_size)` | `@pre batch_num <= kMaxBatchSize`；先 TransferCache 后 SpanList；返回可能小于请求（OOM 或 Span 不足） | ✅（跨层必经） |
| `ReleaseListToSpans` | `void ReleaseListToSpans(void* start, size_t aligned_size)` | `@pre` 链上对象同属一个 `aligned_size` 的 Span；先吸收入 TransferCache，溢出按 bitmap 归还 | ✅ |
| `Reset` | `void Reset() noexcept` | 测试/受控 teardown：还原 bitmap、归还 Span、释放并**重建** TransferCache backing（重建失败则优雅降级慢路径、不 abort） | ❌ |
| `GetOneSpan` | `static Span* (Bucket&, size_t, std::unique_lock&)` | 私有；持有桶锁进入，PageCache 期间释放，返回前无论成败均重新持锁 | ❌ |
| `InitTransferCache` | `void InitTransferCache()` | 私有；构造期单次 SystemAlloc | ❌ |
| `CalculateTotalTransferPtrs` | `static size_t () noexcept` | 私有；TransferCache 总指针容量单一来源，分配与释放共用 | ❌ |

## 6. 算法与流程

### 6.1 FetchRange（两阶段）

1. TransferCache 快路径：锁内取 `min(batch_num, count)` 个指针，LIFO 顺序组装对象链。
2. 不足部分进入 SpanList：持 `span_list_lock`，从队首 Span 用 `AllocObject()` 切分对象；Span 满则移到队尾；无 Span 时 `GetOneSpan` 补货。
3. **预取**：除请求数外额外提取一个 batch（`prefetch_target = batch_num`），写入 TransferCache 供下一个请求者；锁外发布。

### 6.2 ReleaseListToSpans

1. 按 `kMaxBatchSize` 分段吸收到 TransferCache（不碰 Span 元数据）。
2. 溢出部分：`PageMap::GetSpan` 定位归属 Span → `FreeObject` 还原 bitmap；平移为非满的 Span 移到队首（成为下一个分配候选，`use_count == capacity - 1`）；空 Span（`use_count == 0`）摘除后收集进侵入式链表，批次处理完后统一释放桶锁、批量归还 PageCache（每次批次仅一次 unlock，避免逐 Span 的锁往返）。

### 6.3 复杂度

快路径 O(batch)；SpanList 切分均摊 O(batch)（span 内 bitmap 扫描有 `scan_cursor` 推进）。预取量当前无上界修正（见 §8 风险）。

## 7. 边界条件与错误处理

- `FetchRange` 中 `GetOneSpan` 失败（OOM）→ 返回已取数量（可能 0），不重试。
- TransferCache 满时预取剩余对象走 ReleaseListToSpans，保证所有权一致。
- `PageMap::GetSpan` 返回 null（对象不属于 ammalloc）→ 跳过该对象（不崩溃，保持分配器鲁棒）。
- 测试注入（`AMMALLOC_TEST`）：`g_mock_fetch_range_cap` 设 `0 < cap < batch_num` 时，`FetchRange` 至多返回 `cap` 个对象，用于确定性构造「部分 refill」。

## 8. 风险与权衡

- **预取无界**：`TODO(ammalloc): Bound prefetching by the observed TransferCache capacity`——极端批量下可能产生多余预取与回退开销。
- **双锁开销**：快路径仍需 SpinLock；换取的收益是 SpanList 慢路径不阻塞 TransferCache 操作。
- **布局刚性**：缓存行分区假设 `std::mutex` 尺寸 ≤ 64B（libstdc++ 40B / libc++ 8B 均满足）；若未来替换为 ≥64B 的锁实现，`span_list` 将推入更后缓存行，需复查 §3.1 的 static_assert。

## 9. 测试要点

`tests/unit/test_central_cache.cpp`：

- 功能：`BasicFetchRange`、`MultipleFetchRange`、`BasicReleaseListToSpans`、`DifferentSizeClasses`、`LargeBatchAllocation`
- 生命周期：`Reset`、`ReallocateAfterRelease`
- 并发：`MultiThreadedAllocation`、`StressTest`、`FreeListOperations`

此外，`tests/unit/test_thread_cache.cpp` 的 `PartialRefillHoldsQuotaAndOverage` 经 `g_mock_fetch_range_cap` 覆盖了 `FetchRange` 的部分返回分支。

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| 2026-08-19 | 初版（由架构总览 §5.2 拆分扩展） | 文档系统落地 | — |
| 2026-08-21 | 补充 §7 测试注入 `g_mock_fetch_range_cap` 及 §9 部分 refill 测试引用 | 覆盖 FetchRange 部分返回 | — |
| 2026-08-21 | §6.2 空 Span 归还改为批次收集、锁外统一 ReleaseSpan；空/非满分支互斥化 | 减少锁往返 | — |
| 2026-08-21 | §5 `Reset` 末尾重建 TransferCache（`TryInitTransferCache` 容忍失败；构造期 `InitTransferCache` 保持 abort 语义） | 修复 Reset 后快路径永久失效 | — |
| 2026-08-26 | §3.1 Bucket 缓存行布局：`span_list_lock` 成员级 `alignas(64)` 隔离双锁，static_assert 固化不变量 | 消除桶内双锁伪共享（C-01 复查） | — |
| 2026-08-26 | §5 新增 `CalculateTotalTransferPtrs`：提取 TransferCache 总指针容量计算 | 消除 InitTransferCache/Reset 重复循环 | — |
| 2026-08-26 | §5 `GetOneSpan` 锁协议自洽：失败路径亦重新持锁返回 | 调用点无需处理锁恢复，消除隐藏约定 | — |
