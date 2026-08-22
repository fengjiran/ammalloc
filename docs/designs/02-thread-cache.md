# ThreadCache 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.0
- **日期**: 2026-08-19
- **关联代码**: [include/ammalloc/thread_cache.h](../../include/ammalloc/thread_cache.h) / [src/thread_cache.cpp](../../src/thread_cache.cpp) / [include/ammalloc/free_list.h](../../include/ammalloc/free_list.h)（`FreeList`/`FreeBlock`）
- **上游依赖**: `SizeClass`（Index/Size/CalculateBatchSize）、`CentralCache`（FetchRange/ReleaseListToSpans）
- **下游消费者**: `ammalloc.cpp`（`am_malloc`/`am_free` 主入口）
- **关联测试**: [tests/unit/test_thread_cache.cpp](../../tests/unit/test_thread_cache.cpp) / [tests/unit/test_ammalloc.cpp](../../tests/unit/test_ammalloc.cpp) / [tests/unit/test_quota_policy.cpp](../../tests/unit/test_quota_policy.cpp)
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
- **跨线程 free**：`Deallocate` 直接 push 到释放线程自己的 FreeList，不触碰分配线程缓存，快路径保持无锁；代价是缓存归属漂移（对象可能留在非分配线程），由 CentralCache 水位线与 trim 回收平衡（见 §6.3）。
- **析构顺序**：线程退出 → `ThreadCacheCleaner` 析构 → `ReleaseAll()` 归还 CentralCache → `SystemFree` 释放元数据页；`g_ThreadCacheAlreadyDestructed` 置位防止析构期递归重建。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `Allocate` | `void* Allocate(size_t original_size) noexcept` | `@pre original_size <= MAX_TC_SIZE`；size=0 走最小类；快路径单次 pop，空列表走 `FetchFromCentralCache` | ✅ |
| `Deallocate` | `void Deallocate(void* ptr, size_t idx)` | `@pre ptr != nullptr`；`@pre idx < kNumSizeClasses`；用 Span 记录的 `size_class_idx` 避免重新映射；超配额走 `DeallocateSlowPath` | ✅ |
| `ReleaseAll` | `void ReleaseAll()` | 清空所有 FreeList 归还 CentralCache；重置配额 | ❌ |
| `GetMaxSizeForTest` / `GetOveragesForTest` | `size_t (size_t idx) const` | 测试专用观测接口 | ❌ |

## 6. 算法与流程

### 6.1 慢启动与配额衰减（Slow-Start & Quota Decay）

每个尺寸类的高水位配额 `max_size_` 与连续溢出计数 `overages_`（见 §3）驱动一个
「慢上车 → 高水位 → 慢下车」的状态机：增长由 `quota_policy::NextAfterRefill` 决定，
衰减由 `quota_policy::NextAfterOverflow` 决定，二者均为无 I/O 的纯函数（
`include/ammalloc/thread_cache.h` 的 `quota_policy` 命名空间），可被
`test_quota_policy.cpp` 独立穷尽。

**增长策略 `NextAfterRefill`（`kMaxQuotaBatches = 8`）**

```text
if (current < batch)     return min(batch, current + max(1, current));       // ① 指数预热
if (current < batch * kMaxQuotaBatches) return min(batch * kMaxQuotaBatches, current + max(1, batch / kMaxQuotaBatches)); // ② 线性增长
return current;                                                             // ③ kMaxQuotaBatches×batch 封顶
```

- ① 配额低于一个 batch 时翻倍（`max(1, current)` 兜住 `current == 0` 边界），快速收敛。
- ② 达到 batch 后每步 `+batch/kMaxQuotaBatches`（`max(1, …)` 兜住小 batch），线性上探至 kMaxQuotaBatches×batch。
- ③ 封顶后配额不再增长。

**衰减策略 `NextAfterOverflow`（`kMaxOverages = 3`）**

```text
if (current <= batch)             return {current, 0};                      // 下限：不留衰减状态
if (overages + 1 >= kMaxOverages) return {max(current - batch, batch), 0};  // 连续 3 次：降一个 batch
return {current, overages + 1};                                             // 只累计计数
```

- 下限分支用 `<=`：配额等于 batch 即触底，清零计数、不再衰减。
- 连续 3 次无 refill 的溢出才降一个 batch；`max(current - batch, batch)` 在
  `batch < current < 2*batch` 区间是直接压到 batch 下限，而非严格减一个 batch。

**数值演化（16 字节类，`batch = 512`）**

- 增长：配额 `1 → 2 → 4 → … → 512`（翻倍恰达 batch），随后以步长 `+512/kMaxQuotaBatches = 64`
  线性上探，封顶 `4096`（kMaxQuotaBatches×batch）。
- 衰减：配额 24（`batch = 8`）时连续 3 次溢出 `24 → 16`，再连续 3 次 `16 → 8` 触底。

**设计意图**

- `max_size = 1` 起步 + `fetch = min(batch, max_size)`：冷尺寸类不囤积，稀疏负载下
  per-thread RSS 最小。
- 翻倍增长以 O(log batch) 次 refill 收敛满 batch，摊薄 CentralCache 锁开销。
- `kMaxQuotaBatches×batch` 封顶硬性锁定每类内存上限 ≈ `kMaxQuotaBatches × 32KB = 256KB`（极小对象因 batch 封顶 512 更低）。
- 增长快 / 衰减慢（连续 3 次才降一格）的非对称：换取稳定、避免抖动，代价是突发期
  配额短暂偏高（见 §8）。

### 6.2 Refill（FetchFromCentralCache，慢路径）

- 列表为空时从 CentralCache 补货；取货量 `fetch_num = min(batch, max_size)`（配额策略见 §6.1）。
- 成功 refill 后调用 `NextAfterRefill` 上调配额并清零 `overages`（新需求抵消衰减趋势）。
- `FetchRange` 返回 0 表示 OOM，`Allocate` 返回 `nullptr`。

### 6.3 Trim（DeallocateSlowPath，慢路径）

- `Deallocate` 在 `size > max_size` 时进入；通过 `pop_range_tail` 从尾部（最旧）归还一个 batch 对象给 CentralCache（让最近释放的留在本地复用），
  避免一次清空本地缓存。
- 归还对象后调用 `NextAfterOverflow` 更新配额与衰减计数（阈值与 floor 见 §6.1）。

### 6.4 复杂度

所有路径 O(1)；慢路径每事件固定工作量（≤ 1 batch），无隐藏 O(N²)。

## 7. 边界条件与错误处理

- `size=0`：映射到最小尺寸类别（`Index(0)=0`）。
- `FetchRange` 返回 0：表示 OOM，`Allocate` 返回 nullptr。
- 配额下限：`max_size` 不低于一个 batch，避免衰减到 0。

## 8. 风险与权衡

- **per-thread 内存占用**：每类配额上限 kMaxQuotaBatches×batch，活跃线程多时会持有较多空闲对象；这是无锁快路径的代价，由 CentralCache 水位线与 trim 机制平衡。
- **TLS 生命周期成本**：线程退出时 `ThreadCacheCleaner` 析构触发全量归还（`ReleaseAll`），线程频繁创建/退出的场景会产生归还风暴；`g_ThreadCacheAlreadyDestructed` 防护析构期递归重建（生命周期问题分析见 [research/thread-local-and-thread-cache.md](research/thread-local-and-thread-cache.md)）。
- **快路径不含策略**：所有配额决策推迟到冷路径，保证常见分支轻。

## 9. 测试要点

`tests/unit/test_thread_cache.cpp`：

- 功能：`BasicAllocate`、`AllocateZero`、`BasicDeallocate`、`EdgeCases`、`DifferentSizeClasses`、`ReleaseAll`
- 配额：`SlowStartGrowthThenOveragesShrinkMaxSize`（增长与衰减）、`TriggerReleaseTooLongList`、`SlowStartAndScavenge`、`MaxSizeStaysBoundedUnderSustainedLoad`（持续负载下有界收敛）
- 并发：`MultiThreadStress`、`MultiThreadedAllocation`、`MultiThreadedDifferentSizes`

`tests/unit/test_ammalloc.cpp`：

- TLS 生命周期：`AmMallocThreadExitTest.ThreadExitDrainsCacheToCentralCache`（线程退出经 `ThreadCacheCleaner → ReleaseAll → ReleaseThreadCache` 归还对象并释放元数据页）
- 跨线程 free：`AmMallocCrossThreadFreeTest.FreeOnDifferentThread`（释放线程重读 `span->size_class_idx` 并 push 到自身 FreeList，覆盖 `am_free_slow_path → CreateThreadCache` 与归属漂移）

`tests/unit/test_quota_policy.cpp`：`quota_policy` 纯函数（`NextAfterRefill`/`NextAfterOverflow`）的增长/衰减分支与封顶/floor 边界。

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| 2026-08-19 | 初版（由架构总览 §5.1 拆分扩展） | 文档系统落地 | — |
| 2026-08-21 | 补充 §6.1 慢启动与配额衰减策略（增长/衰减纯函数、数值演化、设计意图） | 沉淀慢启动实现逻辑 | — |
| 2026-08-21 | 补充 §9 三个测试用例（TLS 生命周期、跨线程 free、配额有界收敛） | 覆盖 I1/I2 测试缺口 | — |
