# ThreadCache 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.2
- **日期**: 2026-08-31
- **关联代码**: [include/ammalloc/thread_cache.h](../../include/ammalloc/thread_cache.h) / [src/thread_cache.cpp](../../src/thread_cache.cpp) / [include/ammalloc/free_list.h](../../include/ammalloc/free_list.h)（`FreeList`/`FreeBlock`）
- **上游依赖**: `SizeClass`（Index/Size/CalculateBatchSize）、`CentralCache`（FetchRange/ReleaseListToSpans）
- **下游消费者**: `ammalloc.cpp`（`am_malloc`/`am_free` 主入口）
- **关联测试**: [tests/unit/test_thread_cache.cpp](../../tests/unit/test_thread_cache.cpp) / [tests/unit/test_ammalloc.cpp](../../tests/unit/test_ammalloc.cpp) / [tests/unit/test_quota_policy.cpp](../../tests/unit/test_quota_policy.cpp)
- **架构总览**: [ammalloc_design.md §5.1](ammalloc_design.md)

## 1. 背景与目标

ThreadCache 是分配器的前端缓存：每个线程一个 TLS 实例，处理绝大多数分配/释放请求。目标：

- 快路径（一次 FreeList pop/push）完全无锁、无系统调用，单线程极速路径约 3.8 ns（见架构总览性能基线）。
- 通过慢启动与水位线动态调节每类配额，并以 aggregate quota-capacity budget 限制跨类总额，防止线程长期钉住突发期缓存。
- 在不触碰常见 push/pop 路径的前提下，提供 owner-thread trim/purge 和 cooperative pressure request，使空 Span 最终能返回 PageCache。

## 2. 职责与边界

- **提供**：`Allocate`/`Deallocate`（快路径）+ `ReleaseAll`（TLS 销毁时全量归还）。
- **请求**：`CentralCache::FetchRange`（refill）、`CentralCache::ReleaseListToSpans`（trim/全量归还）。
- **所有权**：FreeList 中的对象所有权始终属于分配器系统；ThreadCache 只是借用。TLS 析构时 `ReleaseAll` 把全部对象归还 CentralCache 后再销毁 ThreadCache 元数据（`PageAllocator::SystemFree`）。
- **生命周期**：TLS 指针由 `thread_local ThreadCacheCleaner` 管理；`g_ThreadCacheAlreadyDestructed` 防止 TLS 析构阶段的递归分配重建缓存。

## 3. 关键数据结构

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| `ThreadCache::free_lists_` | 每个尺寸类别一个 LIFO FreeList | TLS 私有，无同步；`alignas(64)` 防伪共享 |
| `FreeList::head_` | 嵌入式空闲链头（对象自身存 next） | 仅所属线程读写 |
| `FreeList::size_` | 当前对象数（size_t） | 同上 |
| `FreeList::max_size_` | 该类高水位配额，初值 1 | 慢启动增长 / 超配衰减 |
| `FreeList::overages_` | 连续溢出 trim 计数 | 配额衰减信号 |
| `ThreadCache::reserved_quota_bytes_` | `Σ(max_size × class_size)` | 只在 quota 变更慢路径维护；是 capacity，不是当前 cached bytes |
| `ThreadCache::cache_budget_bytes_` | aggregate quota-capacity ceiling | 默认 2 MiB/thread；不在 push/pop 路径读取 |
| `ThreadCache::observed_trim_epoch_` | owner 已处理的 cooperative 请求 | 仅 refill/overflow/safepoint 读取全局 epoch |

## 4. 并发模型

- **无锁**：ThreadCache 及其 FreeList 线程私有，唯一 mutator 是拥有线程。
- **TLS 模型**：`thread_local ThreadCache*`，initial-exec 模型。
- **无锁前提与代价**：快路径无锁依赖 TLS 私有实例（唯一 mutator 是拥有线程），避免共享 FreeList 的锁/原子/Cache line bouncing；代价是 TLS 访问本身（initial-exec 下为 FS 基址 + 偏移，成本远低于锁与原子竞争，背景见 [research/thread-local-and-thread-cache.md](research/thread-local-and-thread-cache.md)）。
- **跨线程 free**：`Deallocate` 直接 push 到释放线程自己的 FreeList，不触碰分配线程缓存，快路径保持无锁；代价是缓存归属漂移（对象可能留在非分配线程），由 CentralCache 水位线与 trim 回收平衡（见 §6.3）。
- **析构顺序**：线程退出 → `ThreadCacheCleaner` 析构 → `ReleaseAll()` 归还 CentralCache → `SystemFree` 释放元数据页；`g_ThreadCacheAlreadyDestructed` 置位防止析构期递归重建。
- **GC 边界**：后台线程不得遍历或修改其他 TLS FreeList，只能发布 trim epoch；owner 在 refill/overflow 慢路径或显式 safepoint 观察。存在两类观察盲区：(a) 完全 idle 线程不进入任何分配路径；(b) 稳定工作集、收支平衡的稳态线程——缓存永不清空且永不过限，长期只走快路径而不触发慢路径（实测：cooperative purge 请求发布后此类线程持续分配/释放，cached bytes 保持不变）。两类线程都需 scheduler/event loop 以周期性 owner-thread safepoint 调用 `am_thread_cache_trim()` 或 `am_thread_cache_purge()` 才能立即回收。自动触发策略（Scavenger wiring）另行跟踪，见 `docs/issues.md`「retention 回收的自动触发策略」。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `Allocate` | `void* Allocate(size_t original_size) noexcept` | `@pre original_size <= MAX_TC_SIZE`；size=0 走最小类；快路径单次 pop，空列表走 `FetchFromCentralCache` | ✅ |
| `Deallocate` | `void Deallocate(void* ptr, size_t idx)` | `@pre ptr != nullptr`；`@pre idx < kNumSizeClasses`；用 Span 记录的 `size_class_idx` 避免重新映射；超配额走 `DeallocateSlowPath` | ✅ |
| `ReleaseAll` | `void ReleaseAll()` | 清空所有 FreeList 归还 CentralCache；重置配额 | ❌ |
| `Trim` | `void Trim(ThreadCacheTrimMode, size_t)` | owner-thread 固定 `kNumSizeClasses` 类（当前 40）扫描；soft 保留 warm batch，hard direct-release 到 Span bitmap | ❌ |
| `RequestGlobalTrim` / `ObserveGlobalTrimRequest` | `static void ...` / `void ...` | publish/consume cooperative epoch；只在慢路径或 safepoint 生效 | ❌ |
| `GetMaxSizeForTest` / `GetOveragesForTest` | `size_t (size_t idx) const` | 测试专用观测接口 | ❌ |

## 6. 算法与流程

### 6.1 慢启动与配额衰减（Slow-Start & Quota Decay）

每个尺寸类的高水位配额 `max_size_` 与连续溢出计数 `overages_`（见 §3）驱动一个
「慢上车 → 高水位 → 慢下车」的状态机：增长由 `quota_policy::NextAfterRefill` 决定，
衰减由 `quota_policy::NextAfterOverflow` 决定，二者均为无 I/O 的纯函数（
`include/ammalloc/thread_cache.h` 的 `quota_policy` 命名空间），可被
`test_quota_policy.cpp` 独立穷尽。

**增长策略 `NextAfterRefill`（`kMaxQuotaBatches = 8`）**

```c++
if (current < batch) return min(batch, current + max(1, current));       // ① 指数预热
if (current < batch * kMaxQuotaBatches) return min(batch * kMaxQuotaBatches, current + max(1, batch / kMaxQuotaBatches)); // ② 线性增长
return current;                    // ③ kMaxQuotaBatches×batch 封顶
```

- ① 配额低于一个 batch 时翻倍（`max(1, current)` 兜住 `current == 0` 边界），快速收敛。
- ② 达到 batch 后每步 `+batch/kMaxQuotaBatches`（`max(1, …)` 兜住小 batch），线性上探至 kMaxQuotaBatches×batch。
- ③ 封顶后配额不再增长。

**衰减策略 `NextAfterOverflow`（`kMaxOverages = 3`）**

```c++
if (current <= batch) return {current, 0};                      // 下限：不留衰减状态
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
- `kMaxQuotaBatches×batch` 封顶每类对象数；大多数类别单 batch 约 32 KiB，
  但最小 batch 为 2，故 32 KiB 类单 batch 为 64 KiB、单类 quota 上限为 512 KiB。
  各类别的 batch、封顶对象数与封顶字节见 §6.7。
- 增长快 / 衰减慢（连续 3 次才降一格）的非对称：换取稳定、避免抖动，代价是突发期
  配额短暂偏高（见 §8）。

### 6.2 Refill（FetchFromCentralCache，慢路径）

- 列表为空时从 CentralCache 补货；取货量 `fetch_num = min(batch, max_size)`（配额策略见 §6.1）。
- 成功 refill 后调用 `NextAfterRefill` 上调配额并清零 `overages`（新需求抵消衰减趋势）。
- 部分 refill（`0 < fetched < fetch_num`）视为内存压力：保持当前配额与 `overages` 不变、
  不增长不清零，避免压力正反馈为更大的后续请求。
- quota 增长前检查 `reserved_quota_bytes + delta <= cache_budget_bytes`；预算不足只拒绝
  增长，不把正常 allocation 伪装成 OOM。
- `FetchRange` 返回 0 表示 OOM，`Allocate` 返回 `nullptr`。

### 6.3 Trim（DeallocateSlowPath，慢路径）

- `Deallocate` 在 `size > max_size` 时进入；通过 `pop_range_tail` 从尾部（最旧）归还一个 batch 对象给 CentralCache（让最近释放的留在本地复用），
  避免一次清空本地缓存。
- 归还对象后调用 `NextAfterOverflow` 更新配额与衰减计数（阈值与 floor 见 §6.1）。

### 6.4 Aggregate Budget 与显式 Trim

- 默认 `cache_budget_bytes = 2 MiB`，约束 `Σ(max_size × class_size)`；它是 per-thread
  quota capacity 的上界，正常对象缓存至多比该配额多一个刚 push 的对象。全部类别单类封顶之和
  ≈ 10.0 MiB，故多类同时热时该预算才是绑定约束（数值见 §6.7）。
- 所有 aggregate accounting 仅发生在 refill、overflow quota decay、`Trim` 和
  `ReleaseAll`；常见 `Allocate` hit / `Deallocate` push 不读共享 atomic、不维护 exact
  cached bytes、也不做跨 `kNumSizeClasses` 类扫描。
- `Trim(kReuse, target)` 从大类向小类驱逐最旧对象，最多保留一个现有 batch/class，故在
  warm floor 高于 target 时是 best-effort；`Trim(kRelease, 0)` 直接归还 bitmap 并重置全部
  class quota 到 1。
- hard trim 选择 direct bitmap release；`ReleaseAll` 保持普通 TransferCache reuse，避免
  多线程同时退出时将 TLS destructor 变成 bitmap/PageCache purge storm。需要 RSS 回收的
  worker 应在退出前显式调用 `am_thread_cache_purge()`；已有 TransferCache retention 再由
  CentralCache drain 处理。

### 6.5 Cooperative Pressure Request

控制方以 release store 发布 `{mode, epoch}`；TLS owner 在 `FetchFromCentralCache`、
`DeallocateSlowPath` 或显式 safepoint 执行一次 fixed-size trim。普通 local hit 完全不读
该共享 cache line，因此保留 ThreadCache 的无锁、无共享写热路径。请求不是对完全 idle
线程的中断机制，后者必须由上层 cooperative scheduler 处理。

`GetThreadCacheStats()` 提供 aggregate `reserved_quota_bytes`、trim count、trim 前观察到的
cached-object bytes、trimmed bytes 与 budget-denied growth；CentralCache 的 `GetStats()`
提供 transfer drained bytes 和 direct release 解钉 Span 的计数。这些都是 slow-path
telemetry：不能将 cached-object bytes 或 Span count 直接解释成 RSS。

### 6.6 复杂度

- `Allocate` / `Deallocate` 快路径：O(1)。
- Refill 慢路径：O(batch)，取货量与 `FetchRange` 搬运均以 `batch` 为界。
- **Trim 慢路径：O(`max_size`)，不是 O(1)。** `Deallocate` 先 `push` 再判 `size() > max_size()`，故进入时链深恰为 `max_size + 1`；`pop_range_tail(batch)` 归还的对象数受 `batch` 有界，但**定位驱逐点需要走完整条链**（≈ `size_ - 2` 步串行依赖 load），此外还要走完后缀求尾。"每事件 ≤ 1 batch" 只约束搬运对象数，不约束遍历代价。
- 上限由配额策略参数决定：`max_size ≤ kMaxQuotaBatches × batch`，故 16B/64B 类最坏单次 trim ≈ 4095 步（实测 ≈ 8.2 µs，摊销 ≈ 16 ns/free）。详见 [08-free-list.md](08-free-list.md) §6.6。
- 无隐藏 O(N²)：每个事件只扫一遍链，不随事件次数累积。

### 6.7 容量限制算法与参数表

ThreadCache 的容量限制不是一段独立的策略代码，而是**五道分布在不同路径上的闸门**。本节先把
它们合成一张分层视图，再给出全部输入旋钮与派生上限（数值由
[include/ammalloc/config.h](../../include/ammalloc/config.h)、
[include/ammalloc/size_class.h](../../include/ammalloc/size_class.h)、
[include/ammalloc/free_list.h](../../include/ammalloc/free_list.h)、
[include/ammalloc/thread_cache.h](../../include/ammalloc/thread_cache.h) 的编译期常量推导，
修改任一旋钮后必须重新核算本表）。

**五道闸门**

| 层 | 限制对象 | 上限 | 生效路径 | 代价 | 机制细节 |
|---|---|---|---|---|---|
| L0 入口尺寸 | 哪些对象能进 ThreadCache | `SizeConfig::MAX_TC_SIZE` = 32 KiB | `am_malloc_slow_path`：更大请求直连 `PageCache::AllocSpan` | 一次 `>` 比较 | §1、§5 |
| L1 单类配额 | 每个尺寸类可驻留的**对象个数** | `max_size ≤ kMaxQuotaBatches × batch` | 满额 refill 涨、部分 refill 保持、连续溢出衰减 | 冷路径纯函数 | §6.1、§6.2 |
| L2 溢出即裁 | 超出配额的驻留部分 | 每事件至多归还 1 个 batch | 每次 `Deallocate` push 后判 `size() > max_size()` | 快路径一次比较；慢路径 O(`max_size`) | §6.3、§6.6 |
| L3 聚合预算 | 跨类**总配额容量** | `cache_budget_bytes_` = 2 MiB/thread，`CanSetQuota` 逐一否决 | 每次配额增长前 | 慢路径 `Σ` 增量记账 | §6.4 |
| L4 主动收敛 | 已驻留对象 + 配额本身 | `kReuse` → 1 MiB 软目标；`kRelease` → 0 字节 | owner `Trim`/`purge`、cooperative epoch、线程退出 `ReleaseAll` | 固定 `kNumSizeClasses` 类扫描 | §4、§6.4、§6.5 |

- L0–L3 是**常驻的被动约束**（随分配/释放自然生效）；L4 是**事件驱动的主动收敛**，用于把
  被动约束允许的驻留提前交还给中端/后端。
- L1/L3 只限制"允许占多少"，L2/L4 才真正移动对象；两者正交：任一类别既受自身封顶，又与其余
  `kNumSizeClasses − 1` 类共享同一线程总额。
- 快路径只为闸门付出**一次比较**的代价（L2 的 `size() > max_size()`），所有 `Σ` 记账、扫描与
  CentralCache 搬运都落在冷路径（见 §6.4 第二条）。

**常驻不变量**

- 每类：`1 ≤ max_size_ ≤ kMaxQuotaBatches × batch`，且瞬时 `size_ ≤ max_size_ + 1`
  （`Deallocate` 先 push 后判；`FreeList::set_max_size` 把下限钳到 1，0 配额会让 refill 永久失败）。
- 取回量：`fetch_num = min(batch, max_size_)`，refill 不会把列表填到配额以上。
- 每线程：`Σ(max_size_ × class_size) = reserved_quota_bytes_ ≤ cache_budget_bytes_`，
  下界为构造时预扣的 `InitialReservedQuotaBytes()` = 207.75 KiB。
- 闸门之间是**单向**的：`Trim` 只收缩配额、绝不为了达成 target 而扩张（否则 L4 可以绕过 L3 的
  预算否决）。

**全局旋钮**

| 参数 | 值 | 来源 | 在容量限制中的作用 |
|---|---|---|---|
| `SizeConfig::MAX_TC_SIZE` | 32 KiB | `SizeConfig`（config.h） | 能进 ThreadCache 的对象尺寸上限；更大请求直接走 `PageCache::AllocSpan` |
| `SystemConfig::ALIGNMENT` | 16 B | `SystemConfig`（config.h） | 线性区步长，决定类别数量 |
| `SizeConfig::kStepsPerGroup` / `kStepShift` | 4 / 2 | `SizeConfig`（config.h） | ≥128 B 后每个 2 次幂区间分 4 桶 |
| `SizeClass::kNumSizeClasses` | 40 | `SizeClass`（size_class.h） | `free_lists_` 数组长度 = 参与配额预约的类别数 |
| `SizeClass::kMaxBatchSize` | 512 | `SizeClass`（size_class.h） | batch 上界截断，防小对象类单次搬运过大 |
| `batch` | `clamp(MAX_TC_SIZE / class_size, 2, 512)` | `SizeClass::CalculateBatchSize` | 配额与搬运的基本单位（对象个数） |
| `quota_policy::kMaxQuotaBatches` | 8 | `quota_policy`（thread_cache.h） | 单类配额封顶 = 8 × batch |
| `quota_policy::kMaxOverages` | 3 | `quota_policy`（thread_cache.h） | 连续 3 次溢出触发一次「降一个 batch」的衰减 |
| `FreeList::max_size_` 初值 / 下限 | 1 / 1 | `FreeList`（free_list.h） | slow-start 起点；下限 1 防止 refill 永久失败 |
| `FreeList::overages_` | 0 起步，成功 refill 清零 | `FreeList`（free_list.h） | 衰减信号：新需求取消衰减趋势 |
| `ThreadCache::kDefaultCacheBudgetBytes` | 2 MiB | `ThreadCache`（thread_cache.h） | 单线程 aggregate quota-capacity ceiling |
| `ThreadCache::kDefaultTrimTargetBytes` | 1 MiB | `ThreadCache`（thread_cache.h） | `kReuse` 软 trim 目标（best-effort） |
| `ThreadCache::InitialReservedQuotaBytes()` | 207.75 KiB = Σ `kNumSizeClasses` 类 class_size（当前 40） | `ThreadCache`（thread_cache.h） | 构造即预扣的「每类 1 对象」地板，同时是 budget 的下限 |
| budget 可增长余量 | 1840.25 KiB | 派生：2 MiB − 207.75 KiB | 全部类别共享的增长头寸，由 `CanSetQuota` 逐一否决 |

**每类配额上限**（`idx` 为 `SizeClass::Index` 的类别编号）

- 请求区间：映射到该 class 的原始请求尺寸（左开右闭；`size = 0` 经 `Index(0) = 0` 归入 idx 0）。
- 线性步长：§6.1 ② 中达到一个 batch 后每次 refill 的增量 `max(1, batch / kMaxQuotaBatches)`。
- 对象上限：§6.1 ③ 的封顶值 `kMaxQuotaBatches × batch`。
- 字节上限：对象上限 × class_size，即该类**单独**热起时的 quota 预约量。

| idx | 请求区间 (B) | class_size (B) | batch | 线性步长 | 对象上限 | 字节上限 |
|--:|---|--:|--:|--:|--:|--:|
| 0 | [0, 16] | 16 | 512 | 64 | 4096 | 64 KiB |
| 1 | (16, 32] | 32 | 512 | 64 | 4096 | 128 KiB |
| 2 | (32, 48] | 48 | 512 | 64 | 4096 | 192 KiB |
| 3 | (48, 64] | 64 | 512 | 64 | 4096 | 256 KiB |
| 4 | (64, 80] | 80 | 409 | 51 | 3272 | 256 KiB |
| 5 | (80, 96] | 96 | 341 | 42 | 2728 | 256 KiB |
| 6 | (96, 112] | 112 | 292 | 36 | 2336 | 256 KiB |
| 7 | (112, 128] | 128 | 256 | 32 | 2048 | 256 KiB |
| 8 | (128, 160] | 160 | 204 | 25 | 1632 | 255 KiB |
| 9 | (160, 192] | 192 | 170 | 21 | 1360 | 255 KiB |
| 10 | (192, 224] | 224 | 146 | 18 | 1168 | 256 KiB |
| 11 | (224, 256] | 256 | 128 | 16 | 1024 | 256 KiB |
| 12 | (256, 320] | 320 | 102 | 12 | 816 | 255 KiB |
| 13 | (320, 384] | 384 | 85 | 10 | 680 | 255 KiB |
| 14 | (384, 448] | 448 | 73 | 9 | 584 | 256 KiB |
| 15 | (448, 512] | 512 | 64 | 8 | 512 | 256 KiB |
| 16 | (512, 640] | 640 | 51 | 6 | 408 | 255 KiB |
| 17 | (640, 768] | 768 | 42 | 5 | 336 | 252 KiB |
| 18 | (768, 896] | 896 | 36 | 4 | 288 | 252 KiB |
| 19 | (896, 1024] | 1024 | 32 | 4 | 256 | 256 KiB |
| 20 | (1024, 1280] | 1280 | 25 | 3 | 200 | 250 KiB |
| 21 | (1280, 1536] | 1536 | 21 | 2 | 168 | 252 KiB |
| 22 | (1536, 1792] | 1792 | 18 | 2 | 144 | 252 KiB |
| 23 | (1792, 2048] | 2048 | 16 | 2 | 128 | 256 KiB |
| 24 | (2048, 2560] | 2560 | 12 | 1 | 96 | 240 KiB |
| 25 | (2560, 3072] | 3072 | 10 | 1 | 80 | 240 KiB |
| 26 | (3072, 3584] | 3584 | 9 | 1 | 72 | 252 KiB |
| 27 | (3584, 4096] | 4096 | 8 | 1 | 64 | 256 KiB |
| 28 | (4096, 5120] | 5120 | 6 | 1 | 48 | 240 KiB |
| 29 | (5120, 6144] | 6144 | 5 | 1 | 40 | 240 KiB |
| 30 | (6144, 7168] | 7168 | 4 | 1 | 32 | 224 KiB |
| 31 | (7168, 8192] | 8192 | 4 | 1 | 32 | 256 KiB |
| 32 | (8192, 10240] | 10240 | 3 | 1 | 24 | 240 KiB |
| 33 | (10240, 12288] | 12288 | 2 | 1 | 16 | 192 KiB |
| 34 | (12288, 14336] | 14336 | 2 | 1 | 16 | 224 KiB |
| 35 | (14336, 16384] | 16384 | 2 | 1 | 16 | 256 KiB |
| 36 | (16384, 20480] | 20480 | 2 | 1 | 16 | 320 KiB |
| 37 | (20480, 24576] | 24576 | 2 | 1 | 16 | 384 KiB |
| 38 | (24576, 28672] | 28672 | 2 | 1 | 16 | 448 KiB |
| 39 | (28672, 32768] | 32768 | 2 | 1 | 16 | 512 KiB |

**读表结论**

1. **单类天花板被刻意压平在 ~256 KiB**：`batch × class_size ≈ MAX_TC_SIZE`（32 KiB），乘
   `kMaxQuotaBatches = 8` 即 256 KiB。表中两处偏离都来自 clamp 而非策略意图——idx 0–2 被
   `kMaxBatchSize = 512` 截到 64/128/192 KiB，idx 36–39 被 `batch ≥ 2` 抬到
   320/384/448/512 KiB，与 §6.1「设计意图」末条的 512 KiB 单类上限一致。
2. **2 MiB aggregate budget 必然成为绑定约束**：`kNumSizeClasses` 类字节上限之和 ≈ 10.0 MiB，远大于单线程
   2 MiB 总额。任一类别单独热起可在 1840.25 KiB 余量内触顶，但多类同时热时后续增长被
   `CanSetQuota` 拒绝并累加 `budget_denied_growth`（见 §6.4）。
3. **实际驻留对象数比「对象上限」多 1**：`Deallocate` 先 `push` 后判 `size() > max_size()`，
   故瞬时 `size_ ≤ max_size_ + 1`；refill 侧 `fetch_num = min(batch, max_size)` 不再额外扩张
   （见 §6.2）。这也是 §6.6 中 `pop_range_tail` 遍历长度为 `max_size + 1` 的来源。

## 7. 边界条件与错误处理

- `size=0`：映射到最小尺寸类别（`Index(0)=0`）。
- `FetchRange` 返回 0：表示 OOM，`Allocate` 返回 nullptr。
- overflow decay 不会把已经增长到 batch 的 quota 降到一个 batch 以下；冷类别可保持初始
  quota 1，显式 hard trim 也会把 quota 重置为 1。

## 8. 风险与权衡

- **per-thread 内存占用**：每类仍有 kMaxQuotaBatches×batch 的局部上限，但跨类总量受
  aggregate budget 约束；完全 idle 线程仍依赖 owner-thread safepoint 执行 trim/purge。
- **RSS 语义**：cached-object bytes 不是 RSS；一个对象足以钉住整个 active Span。只有
  owner hard purge/direct release、TransferCache drain、Span `use_count == 0`、PageCache
  release 和后续 scavenging 的完整链条才能使物理页可回收。
- **TLS 生命周期成本**：线程退出时 `ThreadCacheCleaner` 析构触发全量归还（`ReleaseAll`），线程频繁创建/退出的场景会产生归还风暴；`g_ThreadCacheAlreadyDestructed` 防护析构期递归重建（生命周期问题分析见 [research/thread-local-and-thread-cache.md](research/thread-local-and-thread-cache.md)）。
- **快路径不含策略**：所有配额决策推迟到冷路径，保证常见分支轻。

## 9. 测试要点

`tests/unit/test_thread_cache.cpp`：

- 功能：`BasicAllocate`、`AllocateZero`、`BasicDeallocate`、`EdgeCases`、`DifferentSizeClasses`、`ReleaseAll`
- 配额：`SlowStartGrowthThenOveragesShrinkMaxSize`（增长与衰减）、`TriggerReleaseTooLongList`、`SlowStartAndScavenge`、`MaxSizeStaysBoundedUnderSustainedLoad`（持续负载下有界收敛）、`PartialRefillHoldsQuotaAndOverage`（部分 refill 保持配额与衰减信号）
- 并发：`MultiThreadStress`、`MultiThreadedAllocation`、`MultiThreadedDifferentSizes`
- retention：`AggregateQuotaBudgetBoundsAllSizeClasses`、`QuotaReservationTracksGrowthAndHardTrim`、
  `SoftTrimKeepsOneBatchAndRetractsBurstQuota`、`CooperativeHardTrimIsObservedOnlyAtSlowPath`

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
| 2026-08-21 | 补充 §9 `PartialRefillHoldsQuotaAndOverage`（部分 refill 分支） | 覆盖 G1 测试缺口 | — |
| 2026-08-28 | §6.4 撤销"所有路径 O(1)"，改为逐路径复杂度并标注 trim 为 O(`max_size`) | 原"≤ 1 batch"混淆了归还对象数与定位代价；`pop_range_tail` 需遍历整条链 | S-3 |
| 2026-08-30 | aggregate capacity budget、owner trim/purge、cooperative epoch 与 direct bitmap release | 解决跨类缓存无总额、idle retention 钉住 Span 的 I-2 风险 | I-2 |
| 2026-08-31 | 新增 §6.7 容量限制算法与参数表（L0–L4 五道闸门分层视图、常驻不变量、全局旋钮表、40 类 batch/封顶对象数/封顶字节表），并在 §6.1、§6.4 加 §6.7 交叉引用 | 容量约束此前只以文字散在 §6.1/§6.4，既无"限制分几层、各层管什么"的单一视图，也无法核对单类封顶与 2 MiB 总额的关系；成节成表后可随任一旋钮重新核算 | — |
| 2026-08-31 | 修正 §3 `size_` 类型标注（uint32→size_t）；§6.2/§6.7 补部分 refill 分支；将「40 类」口径统一为 `kNumSizeClasses` 引用 | 文档-实现核对（F1/F2/S1） | — |
