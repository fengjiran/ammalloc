# 第 7 章：Middle-end 提升

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](05-frontend.md) · [下一章](07-backend-pagecache-large-object.md)  
> **本章目标**：提升批量对象流转、跨线程均衡和 CentralCache 并发扩展能力。  
> **适用范围**：TransferCache、CentralCache、Span bitmap、ObjectBatch、分片与预取。  
> **核心 invariant**：批量流转保持 LIFO/有界工作量，桶级同步不泄漏到 ThreadCache 快路径。

Middle-end 位于 Frontend 与 PageCache 之间：向上以批次为单位吸收不同线程或 CPU 的短期供需波动，向下管理承载小对象的 active Span。它既不能成为所有线程争用的全局瓶颈，也不能通过无限缓存对象掩盖后端成本。

本节采取渐进路线：先建立严格的对象所有权、批量接口、测试和统计，再优化当前单 Bucket 的锁内工作量；随后根据竞争数据引入 size-class shard 和 NUMA-local CentralCache；批次描述符、动态容量和更复杂的反馈控制只能在基准证明收益后启用。

## 7.1 职责、边界与性能目标

### 7.1.1 Middle-end 的职责

Middle-end 应负责：

- 在 Frontend cache 之间以批次形式平衡同一 size class 的对象；
- 通过 TransferCache 为 refill 和 trim 提供短临界区快速交换；
- 管理每个 size class 的 active Span 集合及对象 bitmap；
- 在 TransferCache miss 时从 partial Span 批量提取对象；
- 在对象真正返回 Span bitmap 后识别 empty Span；
- 在不持有 CentralCache 锁的前提下向 PageCache 申请或归还 Span；
- 对共享缓存字节、Span 利用率、锁竞争和跨 NUMA 流量实施预算与观测；
- 在 shutdown、测试 reset、fork 和内存压力阶段提供确定性的 drain 行为。

Middle-end 不应负责：

- 执行普通请求的逐对象 fast path；
- 修改 ThreadCache/per-CPU cache 的本地链表；
- split、coalesce 或回收 PageCache free Span；
- 直接调用 `mmap`、`munmap` 或 `madvise`；
- 在共享锁内执行系统调用、日志格式化或动态内存分配；
- 为大对象、任意 over-aligned extent 或专用 tensor arena 提供对象缓存；
- 用复杂的 lock-free bitmap CAS 网络替代可验证的短临界区。

### 7.1.2 层间边界

```text
ThreadCache / per-CPU Frontend
        |
        | ObjectBatch(size_class, count)
        v
Middle-end shard
  +-------------------------+
  | TransferCache fast tier |  pointer/batch slots
  +-------------------------+
        | hit/miss/overflow
  +-------------------------+
  | Central Span tier       |  partial/full/empty + bitmap
  +-------------------------+
        |
        | Span request/release, no Central lock held
        v
PageCache owner shard
```

Frontend 只交换对象；Middle-end 借用 PageCache 所拥有的 `Span` descriptor，并在 Central 状态下独占其对象 bitmap；PageCache 只接收已经没有任何外借对象的 empty Span。

### 7.1.3 目标指标

| 指标 | 目标方向 | 说明 |
|---|---|---|
| TransferCache hit | O(batch)、短临界区 | 不查询 PageMap，不访问 Span bitmap |
| TransferCache miss | 工作量有界 | bitmap 扫描、Span 获取和回滚均有上限 |
| 同 class 扩展性 | 随 shard 数近似扩展 | 重点验证 8B/64B 高竞争负载 |
| 锁持有时间 | 分布可观测 | 同时报告平均值、p99 和最大值 |
| Central cached bytes | 受 node/process 预算约束 | 不允许按线程数无界增长 |
| Span 利用率 | 避免大量低占用 partial Span | 同时控制后端碎片与 miss rate |
| PageCache 往返 | 按批次摊销 | 防止多线程 miss 引起 refill 惊群 |
| NUMA locality | 本地流量优先 | 跨 node 对象漂移必须可测量 |

Middle-end 优化不能以破坏现有整体护栏为代价：单线程 fast path 约 3.8 ns、随机大小约 26.0 ns，以及 16 线程 64B 压力场景约 8.9 us、100+ GiB/s。由于普通 fast path 不进入 Middle-end，任何稳定退化通常意味着增加了共享统计、改变了 Frontend 批次行为或引入了布局副作用。

## 7.2 核心不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 目的 |
|---|---|---|
| ME-1 | 任一小对象在任一时刻只有一个逻辑所有者 | 防止重复分配和双重归还 |
| ME-2 | TransferCache 中的对象 bitmap bit 保持为 0 | 表示对象已从 Span 提取，Span 不能提前回收 |
| ME-3 | `Span::use_count` 统计所有已从 bitmap 提取的对象，包括用户、Frontend 和 TransferCache 持有者 | 维持 empty 判断正确性 |
| ME-4 | 只有 `use_count == 0` 且 bitmap 全部有效位为 1 的 Span 才能归还 PageCache | 防止 UAF |
| ME-5 | 一个 active Span 在任一时刻只属于一个 Central size-class shard | 保证 bitmap 的单锁域写入 |
| ME-6 | Span 的 `size_class_idx` 与所在 bucket 一致且生命周期内不变 | 防止错误尺寸复用 |
| ME-7 | TransferCache 只保存同一 size class、同一 shard 契约下的对象 | 防止跨桶污染 |
| ME-8 | Fetch 成功移出的对象数、目标 batch 数和所有权计数完全一致 | 防止遗漏或重复交付 |
| ME-9 | Release 只有在对象成功进入 TransferCache 或恢复 bitmap 后才算完成 | 保证失败原子性 |
| ME-10 | TransferCache 锁与 SpanList 锁不嵌套 | 缩小死锁状态空间 |
| ME-11 | 持有 Central 锁时不得进入 PageCache 或执行 OS 调用 | 保持全局锁顺序 |
| ME-12 | PageMap 读路径无锁，Middle-end 不调用 PageMap 写接口 | 遵守 PageMap 契约 |
| ME-13 | 共享缓存总字节和 batch slot 数受显式预算约束 | 控制 RSS 放大 |
| ME-14 | 共享路径不使用堆分配 STL 容器或原始 owning `new/delete` | 避免分配器递归 |
| ME-15 | LIFO 顺序的定义在每次批量转换中一致 | 保持对象缓存局部性 |
| ME-16 | Reset/shutdown 只在并发请求静止后破坏共享 backing | 防止 teardown UAF |
| ME-17 | OOM、partial fetch 或发布竞争不会遗失已经从 bitmap 提取的对象 | 保证错误路径守恒 |
| ME-18 | 所有 shard 选择均可从 Span 或稳定路由信息重建 | 确保 cross-thread free 回到正确所有者 |

建议为调试和诊断构建增加守恒检查：

```text
valid_objects_in_active_spans
  = bitmap_free_objects
  + transfer_cached_objects
  + frontend_cached_objects
  + user_outstanding_objects
  + remote_queued_objects
```

生产构建不逐次维护右侧所有共享计数；可通过采样、停机快照或测试 hook 验证。

## 7.3 当前实现基线与差距

### 7.3.1 已有基础

当前实现已经具备：

- 每个 size class 一个缓存行对齐的 `CentralCache::Bucket`；
- `SpinLock` 保护的连续指针数组 TransferCache；
- 独立 `std::mutex` 保护的 `SpanList` 和非原子 bitmap；
- 一次 `PageAllocator::SystemAlloc` 建立全部 TransferCache backing，避免普通堆递归；
- TransferCache miss 后多提取一个 batch 的预取机制；
- `Span::scan_cursor`、`std::countr_zero` 和按 bitmap word 扫描；
- full Span 后移、新ly non-full Span 前移的候选维护；
- 进入 PageCache 前释放 bucket mutex 的锁序约束；
- TransferCache overflow 对象恢复到 Span bitmap 的回滚路径；
- intrusive `FreeList` 与 `SpanList`，核心路径没有堆容器。

因此，下一步重点不是重新实现已存在的 `scan_cursor`，而是建立批量 bitmap 操作、明确所有权事务、减少重复 PageMap 查询，并用分片和预算解决真实竞争。

### 7.3.2 主要差距

当前差距包括：

- 每个 size class 只有一个全局 Bucket，热点 class 最终集中到一把 TransferCache 锁和一把 SpanList 锁；
- TransferCache 容量固定为 `8 * batch_size`，没有全局字节预算、动态借额或工作集衰减；
- TransferCache 存储逐对象指针，批量 push/pop 仍执行 O(N) 指针复制；
- 预取固定为请求 batch，未受空闲 slot、miss rate、Span 利用率或内存压力约束；
- `AllocObject()` 每次只提取一个对象，锁内函数调用和元数据更新次数与 batch 成正比；
- release overflow 对每个对象执行一次 PageMap lookup 和 bitmap 更新；
- SpanList 只有一条链表，partial、full、empty 状态通过位置约定隐式表达；
- `GetOneSpan()` 的 unlock/refill/relock 窗口允许多个 miss 线程并行向 PageCache 取 Span，可能过度回填；
- 当前 release API 信任调用方传入的 `aligned_size`，缺少显式 shard/class 一致性校验；
- 无 TransferCache hit/miss、lock wait、Span occupancy 或 PageCache refill 专项统计；
- `Reset()` 释放 backing 后不重新初始化，而单元测试在每个用例开始调用 `Reset()`，导致大量测试没有覆盖 TransferCache fast tier；
- OOM 日志路径使用 `spdlog`，未来拦截系统 malloc 后不属于可证明的 bootstrap-safe 失败路径；
- 现有 benchmark 主要通过端到端 churn 间接触发 CentralCache，不能单独归因 TransferCache、bitmap 或 PageCache refill 成本。

## 7.4 对象所有权状态机

### 7.4.1 状态定义

一个 Central small-object slot 可处于以下状态：

```text
SpanBitmapFree
    |
    | bitmap bit 1 -> 0, use_count++
    v
TransferCached
    |
    | pop batch
    v
FrontendCached
    |
    | local allocation
    v
UserOwned
    |
    | local free / remote free
    v
FrontendCached or RemoteQueued
    |
    | trim batch
    v
TransferCached
    |
    | TransferCache overflow/drain
    v
SpanBitmapFree
```

`TransferCached`、`FrontendCached`、`RemoteQueued` 和 `UserOwned` 对 bitmap 而言都是“已提取”状态。不能因为对象在 allocator 内部缓存就提前把 bit 设回 1，否则 Span 可能在对象仍可被重新分配时归还 PageCache。

### 7.4.2 Span 状态

建议显式区分：

- **Partial**：bitmap 至少有一个 free slot，且 `use_count > 0`；
- **Full**：bitmap 没有 free slot，`use_count == capacity`；
- **EmptyCandidate**：`use_count == 0`，等待从 Central shard 脱链；
- **RefillInProgress**：PageCache 已交付但尚未发布到 shard 的独占 Span；
- **Detached**：已从 Central 移除，准备归还 PageCache。

状态转换必须在 owning shard 的 Span 锁下完成；`RefillInProgress` 和 `Detached` 是线程私有状态，不允许其他 Central 操作访问。

### 7.4.3 所有权提交点

- bitmap allocate 的提交点：有效 bit 被清零且 `use_count` 增加；
- TransferCache publish 的提交点：对象写入受锁 slot 且 count 更新；
- Frontend fetch 的提交点：batch 从 TransferCache/Span tier 摘除并交给调用者；
- bitmap free 的提交点：对象有效 bit 从 0 变 1 且 `use_count` 减少；
- PageCache release 的提交点：empty Span 从 Central 列表脱链，Central 锁释放后进入 PageCache。

任何提交点后的失败都必须继续向下一合法所有者交付，或者按逆事务恢复原状态，不能通过忽略指针结束路径。

## 7.5 显式 ObjectBatch 接口

### 7.5.1 设计目的

当前接口用 `FreeList&` 和裸链表头分别表达 fetch 与 release，数量、尾指针、size class 和 shard 信息分散在调用约定中。建议引入不分配内存的内部批次描述：

```cpp
struct ObjectBatch {
    void* head;
    void* tail;
    uint32_t count;
    uint16_t size_class_idx;
    uint16_t shard_id;
};
```

该示例只是语义草图，具体字段宽度和布局必须通过 `static_assert` 与 benchmark 决定。它不拥有额外 metadata allocation；链表链接仍存储在 free object body 中。

### 7.5.2 接口语义

建议形成对称接口：

```text
FetchBatch(class, preferred_count, route) -> ObjectBatch
ReleaseBatch(ObjectBatch, route)
DrainTransferCache(class, shard, limit) -> ObjectBatch
```

接口应明确：

- `count == 0` 时 `head == tail == nullptr`；
- 非空 batch 中 `tail` 可从 `head` 在 `count - 1` 步内到达；
- 所有对象属于相同 size class；
- batch 交付是 move-only 所有权转移；
- partial fetch 合法，OOM 时可能返回小于 preferred count；
- release 接口返回后调用方不再访问对象链；
- shard hint 不是可信所有权证明，必要时以 Span metadata 为准。

### 7.5.3 数组与链表边界

Frontend 适合 intrusive chain；TransferCache 当前适合 pointer array；bitmap 批量提取适合定长栈数组。转换函数必须统一顺序语义，并使用 `kMaxBatchSize` 定长存储，不得在慢路径创建 `std::vector`。

## 7.6 LIFO 语义与批量顺序

### 7.6.1 统一定义

定义 batch 的 `head` 为下一次应优先分配的对象，即整个系统中“最热”的一端。对任意链：

```text
head -> next -> ... -> tail
```

`head` 必须首先被 Frontend pop。TransferCache push/pop、数组和链表互转都应保持这一语义，除非基准证明某个层级采用相反顺序更优，并在接口中明确标注。

### 7.6.2 当前风险

逐对象链表转数组后再从数组尾部 pop，容易发生一次或两次隐式反转。仅验证“FreeList 自身是 LIFO”不足以证明跨层批量流转仍为 LIFO。

应加入带有稳定对象编号的顺序测试，分别覆盖：

- Frontend chain -> TransferCache -> Frontend；
- Span bitmap -> prefetch array -> TransferCache -> Frontend；
- TransferCache overflow -> bitmap -> 再次 fetch；
- partial batch 和满 batch；
- 两个 producer 交替 release 后的局部顺序。

### 7.6.3 顺序与公平性

LIFO 有利于 cache locality，但可能让冷对象长期停留在 TransferCache。容量衰减和 drain 应优先归还较冷的一端；不应为了公平性改变普通 fetch 的热端优先行为。

## 7.7 TransferCache 数据结构路线

### 7.7.1 路线 A：连续指针栈

保留当前模型：

```text
void* slots[capacity]
size_t count
SpinLock lock
```

优点：

- 实现简单，所有权容易验证；
- 支持任意 1..N 个对象的 partial batch；
- backing 可一次性向 PageAllocator 申请；
- push/pop 临界区短且无 bitmap 操作。

代价：

- 每批执行 O(N) 指针复制；
- 大 batch 会延长自旋锁持有时间；
- 容量调整需要额外 slot 管理；
- 单 Bucket 下 cache line 在高并发时持续迁移。

该路线应作为正确性基线和可靠回退模式。

### 7.7.2 路线 B：固定批次描述符

类似主流分配器的 transfer cache，可缓存已经链接好的完整 batch：

```text
BatchSlot { head, tail, count }
```

一次 push/pop 只移动描述符，而不是复制每个指针。要求：

- BatchSlot backing 预分配且不依赖系统 malloc；
- 完整 batch 走 descriptor fast tier；
- partial batch 走小型 pointer stack 或直接 Span tier；
- descriptor 中的链保持既定 LIFO；
- 不能让同一对象链同时被两个 slot 引用；
- reset/drain 能遍历所有 slot 并恢复 bitmap 所有权。

代价是状态机和碎片化策略更复杂。只有在 `batch_ptr_copy_cycles`、锁持有时间和多线程吞吐数据表明逐指针复制是主要瓶颈时才引入。

### 7.7.3 推荐决策

短期继续使用 pointer stack，但抽象出 `ObjectBatch`、bulk push/pop 和一致顺序；中期增加 descriptor 实验开关，针对小对象大 batch 场景 A/B 测试。不要一次同时引入 descriptor、shard 和 NUMA，以免性能变化无法归因。

## 7.8 TransferCache 批量操作

### 7.8.1 Bulk pop

建议流程：

1. 在锁外计算 `wanted = min(requested, kMaxBatchSize)`；
2. 获取 TransferCache 锁；
3. 计算 `n = min(wanted, count)`；
4. 将热端连续 `n` 个 slot 复制到栈数组或生成 batch；
5. 一次更新 `count`；
6. 释放锁；
7. 在锁外完成数组到 intrusive chain 的转换。

锁内不得执行 FreeList 遍历、PageMap lookup、bitmap 修改或共享日志。

### 7.8.2 Bulk push

建议流程：

1. 在锁外把输入 chain 的有限前缀转换为栈数组，或保留 descriptor；
2. 获取 TransferCache 锁；
3. 计算可接收 slot 数；
4. 连续复制并一次更新 `count`；
5. 释放锁；
6. 将剩余对象交给 Span tier。

如果调用方已经提供定长 pointer array，不应先构链再拆链。

### 7.8.3 临界区上限

即使 `kMaxBatchSize` 允许 512 个对象，也应测量单次锁内复制的最大周期。必要时将 bulk 操作按 32/64 个指针分段，但分段会增加锁获取次数，必须根据 p99 lock hold 和吞吐选择，而非凭经验固定。

## 7.9 容量、字节预算与动态借额

### 7.9.1 为什么不能只用固定倍数

`capacity = 8 * batch_size` 简单，但不同 class 的实际流量、对象字节和 Span 成本差异很大：一个冷 class 可能长期占据 slot，而热点 class 频繁溢出；按对象数等比例也不等于按字节公平。

建议至少维护：

- process/node 级 CentralCache byte budget；
- 每 class/shard 的 minimum slots；
- 当前 capacity slots/bytes；
- occupancy 高水位和低水位；
- fetch hit、partial hit、push overflow；
- 最近一个控制窗口的收益分数。

### 7.9.2 预算口径

需要区分：

- `transfer_object_bytes`：TransferCache 内对象按 class size 加权；
- `transfer_metadata_bytes`：pointer slots 或 BatchSlot backing；
- `central_span_free_bytes`：active Span bitmap 中尚未提取的对象容量；
- `central_span_active_bytes`：Central 持有的全部 Span 页；
- `empty_span_retained_bytes`：暂存但可立即归还 PageCache 的空 Span。

对象从 Span bitmap 提取到 TransferCache 时，不增加 active Span 页数，但会改变对象缓存归属；统计不能重复计入 allocator 总 active bytes。

### 7.9.3 动态借额

容量调整只在慢路径或控制周期执行，并以 batch 为最小单位：

- 高频 miss 且高 occupancy 的 class 可申请一个 batch slot；
- 长期低 occupancy 的 class 归还一个 batch slot；
- push overflow 与紧随其后的 fetch miss 同时较高，说明容量可能不足；
- 只有 overflow 而没有后续 hit，说明对象应更快回 bitmap/PageCache；
- 总预算超限时按“每字节避免的慢路径次数”从低收益 class 回收。

动态 slot pool 必须使用预分配数组、位图或 intrusive free list，不能使用 `std::vector` 扩容。

### 7.9.4 控制稳定性

- 设置最小观察窗口；
- 增长与衰减采用不同阈值形成滞回；
- 每周期最多调整有限 batch；
- 内存压力事件可绕过普通衰减直接收缩；
- 配置变化只影响慢路径，热路径不读取复杂全局结构。

## 7.10 Size-class Bucket 分片

### 7.10.1 分片目标

单 size-class Bucket 是热点 class 的扩展性上限。目标结构为：

```text
CentralCache[node]
  Class[0]
    Shard[0..S0)
  Class[1]
    Shard[0..S1)
  ...
```

不同 class 可使用不同 shard 数：8B/64B 等热点 class 可以更多，冷门大 class 保持 1 个，避免 metadata 和空 Span 放大。

### 7.10.2 稳定路由

Frontend refill 应使用稳定 route，例如：

- per-thread 模式：初始化时选择 node/shard，并在一段时间内保持；
- per-CPU 模式：由当前 CPU 映射到本 node 的固定 shard；
- remote free：优先回到 Span 的 Central owner shard，而不是释放线程随机选择的 shard。

每次请求随机 hash 会让对象和 Span 在 shard 间漂移，破坏局部性并增加无法合并的低占用 Span。

### 7.10.3 Span owner 标识

真正分片后，release 必须从对象对应 Span 恢复 Central owner。需要稳定的 `central_shard_id` 或等价信息：

- 可在 `Span` 的紧凑状态字段中编码；
- 可使用由 PageMap/region 推导的稳定 arena id；
- 或使用 allocator-owned side metadata。

`Span` 当前严格为一个缓存行，不能未经布局与性能评估直接增加字段。任何编码都必须验证 shard id、size class、generation 和 PageCache owner 不混淆。

### 7.10.4 不允许跨 shard 共管一个 Span

一个 Span 的 bitmap 只能由一个 Central shard 锁保护。多个 shard 共享同一 Span 会把简单互斥重新变成 bitmap 原子竞争，并使 empty 判定和归还协议复杂化，应明确禁止。

## 7.11 NUMA-local Middle-end

### 7.11.1 拓扑

建议每个 NUMA node 拥有独立 CentralCache 组，并优先从同 node PageCache arena 获取 Span：

```text
CPU/thread -> local Frontend -> local Central shard -> local PageCache region
```

这要求 PageCache 的 region ownership 先稳定；Middle-end 不能单独通过 node hash 承诺物理页本地性。

### 7.11.2 Cross-node free

对象在哪个 node 分配与在哪个 node 释放可能不同。候选策略：

1. 直接回 owner node/shard，保持 Span 聚合，但产生远端写；
2. 先进入释放 node 的 bounded remote queue，再批量发送 owner；
3. 在高漂移 workload 下允许有限 Span reassignment，但只能在 Span 完全 drain 后执行。

默认推荐前两种，不允许活跃 Span 直接换 owner。

### 7.11.3 NUMA 统计

至少记录：

- local/remote Central fetch；
- local/remote release；
- owner-node queue depth；
- 跨 node batch 数和字节；
- node 间 Span 获取/归还；
- `perf` NUMA remote access 或等价硬件计数；
- aethermind worker/socket 对应的 tokens/s 与尾延迟。

NUMA 模式必须提供单 node 和不可识别拓扑时的透明回退。

## 7.12 FetchRange 事务化流程

### 7.12.1 阶段 1：路由与校验

- 将 aligned size 映射为 class index；
- 校验请求数不超过 `kMaxBatchSize`；
- 选择稳定 node/shard；
- 读取只在慢路径更新的 batch/capacity hint；
- 不创建动态容器，不触发日志格式化。

### 7.12.2 阶段 2：TransferCache probe

- bulk pop 最多 requested 个对象；
- 命中完整 batch 时直接返回；
- partial hit 合法，剩余部分进入 Span tier；
- 释放 TransferCache 锁后再构造输出链；
- hit/miss 统计采用采样或 shard-local relaxed counter。

### 7.12.3 阶段 3：Span tier 提取

在 shard Span 锁下：

1. 从 partial list 选择候选 Span；
2. 使用 bulk bitmap API 提取调用方缺少的对象；
3. 在预算允许时额外提取 prefetch 对象；
4. 更新 use_count、scan cursor 和 Span 状态列表；
5. 将 caller batch 与 prefetch batch 分开；
6. 释放 Span 锁。

如果没有 partial Span，进入独立 refill 协议，而不是持锁调用 PageCache。

### 7.12.4 阶段 4：发布预取对象

预取对象在 bitmap 中已经提交为 extracted，必须满足下列之一：

- 成功发布到 TransferCache；
- 直接并入调用方 batch且不超过 requested 语义；
- 恢复到对应 Span bitmap；
- 交给另一个明确所有者。

TransferCache 在 bitmap 扫描期间可能被其他线程填满，因此发布失败不是异常。回滚路径必须有界且可测量。

### 7.12.5 Partial fetch 与 OOM

PageCache OOM 时：

- 已从 TransferCache 或现有 Span 取得的对象仍可返回；
- 没有取得任何对象时返回 empty batch；
- 不修改原调用方 FreeList 的既有对象；
- errno 等公共 ABI 语义由上层统一处理；
- 不在 OOM 路径调用可能分配内存的 logger。

## 7.13 ReleaseRange 事务化流程

### 7.13.1 输入规范化

- 空 batch 直接返回；
- batch count、head/tail 和 class 必须一致；
- Release 构建至少保证内部可信调用不会静默丢对象；
- hardening/debug 模式校验链长度、循环、对齐和 class；
- 不可信公共 free 的指针验证策略由 ABI/安全章节定义。

### 7.13.2 阶段 1：TransferCache absorb

优先将 batch 热端放入匹配 shard 的 TransferCache：

- 锁内只复制指针/描述符并更新 count；
- 满容量时产生 leftover batch；
- 若内存压力要求 bypass，可直接把整批送往 Span tier；
- 不允许为了避免 overflow 临时扩大未预算的 backing。

### 7.13.3 阶段 2：按 Span 恢复 bitmap

对 leftover：

1. 使用 PageMap 无锁读取解析 Span；
2. 验证 Span class 与 Central owner shard；
3. 在 owner shard 的 Span 锁下批量设置 bitmap bit；
4. 更新 use_count 和 Span 状态；
5. 将变为 empty 的 Span 脱链到线程私有 release list；
6. 释放 Central 锁后逐个或批量归还 PageCache。

对象不能因为 PageMap 返回空或 class 不一致而被简单忽略。对于内部不变量破坏，debug/hardening 应立即报告；普通 Release 构建也应采用明确 fail-fast 或 quarantine 策略，而不是泄漏后继续运行。

### 7.13.4 跨 shard batch

如果输入链可能包含多个 Central owner shard，不能持有 shard A 锁再获取 shard B。应在定长栈数组中分组，或按对象生成 bounded 子批次，依次处理单一 owner shard。

## 7.14 SpanList 状态分组

### 7.14.1 显式列表

建议将单条隐式排序 SpanList 演进为：

- `partial`: 有 free bitmap bit，优先用于 fetch；
- `full`: 没有 free bit，不参与扫描；
- 可选 `empty`: 短期保留的全空 Span；
- `refill_pending`: 不是 SpanList，而是 shard 状态/计数。

由于 `Span` 只有一组 intrusive `next/prev`，同一时刻只能位于一条 Central 或 PageCache 链表，状态转换必须先 erase 再 insert。

### 7.14.2 Partial 选择策略

候选策略需要在局部性和碎片之间权衡：

- 优先高占用 partial Span：更快填满，有利于其他低占用 Span 变空；
- 优先最近使用 Span：更高 cache/TLB locality；
- round-robin：更公平，但可能扩大活跃工作集。

默认建议使用 occupancy bucket 或有限候选窗口，不引入全局平衡树。遍历必须有上限，禁止在锁内为寻找“最优”Span 执行 O(number_of_spans) 全扫描。

### 7.14.3 Full -> Partial

对象从 full Span 真正恢复到 bitmap 时，Span 必须立即进入 partial 候选；对象仅进入 TransferCache 时 bitmap 仍为 0，因此不会触发该转换。

### 7.14.4 Partial -> Empty

`use_count` 降为 0 后：

- 校验有效 bitmap bits 全为 1；
- 从 partial 列表摘除；
- 根据 empty retention 策略保留或脱离；
- Central 锁外归还 PageCache。

## 7.15 Bitmap word 级批量化

### 7.15.1 Bulk allocate

为 Span 增加语义等价的 bulk API：

```text
AllocBatch(out[], max_count) -> actual_count
```

每个 bitmap word 的处理可为：

1. 加载 `word`；
2. 循环 `countr_zero(word)` 提取多个 bit；
3. 在局部寄存器清除 bit；
4. 一次写回 bitmap word；
5. 批量增加 `use_count`；
6. 只在 word 变空时推进 `scan_cursor`。

这减少逐对象函数调用、bitmap load/store 和 cursor 写入次数。bitmap 仍由 shard mutex 保护，不需要 atomic。

### 7.15.2 Bulk free

将同一 Span 的对象按 bitmap word 聚合为 mask：

```text
free_mask[word_index] |= 1 << bit
```

然后：

- 验证 `bitmap[word] & free_mask == 0`；
- 一次 OR 写回；
- 按 `popcount(free_mask)` 减少 use_count；
- 将 cursor 更新为最小受影响 word。

必须检测同一 batch 内重复对象；仅使用 OR mask 会吞掉重复项，使 use_count 与 bitmap 不一致。可通过输入计数与 mask popcount 对比、固定哈希表或诊断模式逐项验证。

### 7.15.3 边界校验

bulk API 必须覆盖：

- capacity 不是 64 整数倍；
- 尾 word 的无效 bit 永远为 0；
- 对象地址位于 data region 且严格按 class size 对齐；
- `global_obj_idx < capacity`，而不只是 bitmap index 有效；
- use_count 不上溢/下溢；
- scan cursor 永不超过 bitmap word count 的合法终止值。

### 7.15.4 不引入无锁 bitmap

多线程 CAS 同一 bitmap word 会导致 cache line bouncing、复杂 ABA/empty 协议和难以验证的 Span 归还。优先通过 shard 降低竞争，并保持 bitmap 在短 mutex 临界区内批量更新。

## 7.16 PageMap 查询摊销与按 Span 分组

### 7.16.1 当前成本

TransferCache overflow 后，当前实现为每个对象执行一次四层 PageMap 读取。读路径虽无锁，但会产生多级依赖 load；一个 batch 中多个对象通常来自少量 Span，可以摊销。

### 7.16.2 低复杂度优化

先实施：

- 记忆 `last_span`，指针仍落在其 page range 时直接复用；
- 对相邻链节点预取 PageMap 路径或 Span metadata；
- Frontend 构建 trim batch 时尽量保留同 Span 局部性；
- sized release 仍需 Span 来恢复 owner，不应误认为 class size 能完全替代 PageMap。

### 7.16.3 定长分组表

在低命中情况下，可使用栈上固定大小的 open-addressed table：

```text
Span* -> pointer sub-batch / masks
```

表容量由 `kMaxBatchSize` 上界决定，不能动态扩容。溢出时退化为逐对象处理，禁止隐藏 O(N²) 链表查找。

### 7.16.4 何时值得实施

记录 `unique_spans_per_release_batch`、PageMap lookup cycles 和 bulk-free cycles。只有 batch 中对象确实高度聚集且 PageMap 占比较高时，分组表才可能覆盖其初始化成本。

## 7.17 自适应预取

### 7.17.1 输入信号

预取量不应恒等于调用 batch。建议使用：

- TransferCache 当前 free slots；
- 最近 miss/partial-hit rate；
- push overflow rate；
- Span tier lock wait；
- 当前 class occupancy；
- node/process Central byte budget；
- memory pressure 和 empty Span 数；
- Frontend 请求 batch 的移动平均。

### 7.17.2 基本策略

```text
prefetch = min(
  available_transfer_slots,
  class_prefetch_limit,
  budget_remaining / class_size,
  span_extractable_objects)
```

低 miss 或高 overflow 时将 prefetch 衰减到 0；持续 miss 且发布后很快被消费时逐步增加。调节只在慢路径进行。

### 7.17.3 发布竞争

扫描 Span 时不应持有 TransferCache 锁，因此 free slots 只是快照。三种方案：

- **容忍竞争并回滚**：最简单，保留当前模型；
- **预留 slot credit**：扫描前预留容量，状态更复杂；
- **直接把 extra 交给等待者**：需要请求合并机制。

短期建议保留有界回滚并统计 `prefetch_publish_failure`；只有该比例显著时才评估 credit reservation。

### 7.17.4 防止反馈振荡

- miss 和 overflow 使用不同阈值；
- 增长慢、压力收缩快；
- 单次最多改变一个 batch；
- 至少经过一个统计窗口再反向调整；
- benchmark 必须覆盖潮汐型 producer/consumer，而不仅是稳态循环。

## 7.18 PageCache refill 惊群抑制

### 7.18.1 问题

当前获取新 Span 时释放 `span_list_lock` 再进入 PageCache。这避免锁序反转，但多个线程可同时观察到无可用 Span，各自完成后端分配，导致：

- 短时间创建过多 Span；
- PageCache 锁竞争放大；
- 低占用 partial Span 增多；
- OOM/压力场景产生级联慢路径。

### 7.18.2 Single-flight 状态

每 shard/class 可维护轻量状态：

- `refill_in_progress`；
- `refill_generation`；
- 可选有限 waiter hint；
- 最近失败时间或 backoff 状态。

首个线程成为 refiller，锁外进入 PageCache；其他线程可：

- 重新检查 TransferCache；
- 尝试同 class 另一 shard；
- 在短暂自旋后让出；
- 或在允许 partial fetch 时直接返回已有对象。

不要在 allocator 内部无界等待条件变量，尤其不能持有其他缓存锁等待。

### 7.18.3 发布协议

refiller 从 PageCache 得到 Span 后在锁外 `Init`，随后重新获得 shard 锁：

- 重新验证 shard 仍运行且 class 匹配；
- 发布到 partial list；
- 清除 in-progress 状态并递增 generation；
- 若 shutdown 已开始，则不发布，直接在锁外归还 PageCache。

### 7.18.4 Refill 数量

默认一次获取一个 Span。只有 PageCache 往返被证明是瓶颈且 class 持续高压时，才允许一次预取多个 Span；额外 Span 必须计入 Central budget 和 empty retention，不能形成隐性 arena。

## 7.19 Empty Span 保留与归还

### 7.19.1 即时归还

当前语义是 `use_count == 0` 后立即归还 PageCache。优点是内存聚合快、Central 状态简单；缺点是潮汐 workload 可能在 Central/PageCache 间反复移动 Span。

### 7.19.2 有界保留

可为热点 class/shard 保留极少量 empty Span：

- 按 class 或 shard 设置 0..N 个上限；
- 记录变空时间或 epoch；
- 后续 miss 可直接重新初始化/复用；
- 内存压力、idle 或预算超限立即 drain；
- retained empty bytes 纳入独立统计。

由于 Span bitmap 已全 free，保留不会产生对象所有权歧义，但页仍占用 Central active working set。

### 7.19.3 Decay

使用慢路径事件或后台增量扫描，每轮只处理有限 shard/class。不要让普通 free 因 empty Span 而同步扫描整个 CentralCache。

### 7.19.4 采用门槛

仅当 `empty_span_bounce`、PageCache lock wait 和系统整体 p99 表明即时归还造成明显抖动时启用；同时验证 RSS/active bytes 不显著恶化。

## 7.20 锁域与锁顺序

### 7.20.1 锁域

| 锁 | 保护数据 | 禁止在锁内执行 |
|---|---|---|
| TransferCache SpinLock | slots、count、capacity/credit 的热状态 | PageMap、bitmap、PageCache、日志、动态分配 |
| Central shard Span mutex | Span lists、bitmap、use_count、scan cursor、refill 状态 | PageCache、OS syscall、阻塞日志 |
| PageCache shard mutex | free Span、split/coalesce、PageMap 写 | Central shard 操作、Frontend 操作 |

### 7.20.2 全局锁序

正常运行期推荐约束：

```text
Frontend local state
  -> TransferCache lock          (随后释放)
  -> Central Span lock           (随后释放)
  -> PageCache shard lock
  -> PageAllocator/OS
```

箭头表示调用层次，不表示这些锁可以同时持有。TransferCache lock 与 Span lock 不嵌套；Central 与 PageCache 锁不嵌套；OS 调用不在任何自旋锁内。

### 7.20.3 多 shard 操作

普通 fetch/release 一次只处理一个 Central shard。若 drain/reset 必须遍历多个 shard，应逐 shard 摘取到线程私有 intrusive list，再在无 Central 锁状态执行下层归还，不同时持有两把 shard 锁。

### 7.20.4 SpinLock 使用边界

TTAS SpinLock 适合极短且不会调度阻塞的 pointer slot 操作。需要：

- 将自旋/让出阈值集中配置；
- 统计 try-lock failure 或采样 wait cycles；
- 在 oversubscription 场景评估 mutex/futex 或分片是否更优；
- 不因“自旋锁更快”的假设忽略 p99 CPU 消耗。

## 7.21 原子变量与内存序

### 7.21.1 锁内状态

`transfer_cache_count`、Span bitmap、`use_count` 和 SpanList link 在对应锁内访问，不需要改为 atomic。锁的 acquire/release 已提供发布关系。

### 7.21.2 统计与提示

以下无同步语义的计数器可使用 `memory_order_relaxed`：

- hit/miss/overflow 次数；
- requested/returned objects；
- lock contention samples；
- refill/prefetch 次数；
- bytes high-water hint。

近似统计不得参与对象所有权或 empty 判定。

### 7.21.3 Single-flight

若 `refill_in_progress` 只在 Span mutex 下访问，则保持普通字段；若设计为锁外快速提示，使用显式 acquire/release 或 relaxed hint + 锁内重新验证。不能仅凭 relaxed load 决定对象所有权转换。

### 7.21.4 配置发布

动态容量、压力模式或采样率可通过 generation/config snapshot 发布：控制线程 release store，慢路径 acquire load。普通 Frontend fast path不应为此增加 acquire load。

## 7.22 内存压力与主动排空

### 7.22.1 压力等级

建议定义：

- **Normal**：按命中率维护容量和有限 empty retention；
- **Soft pressure**：停止预取，缩减低收益 TransferCache；
- **Hard pressure**：旁路 TransferCache release，drain cached objects 和 empty Span；
- **Emergency/OOM**：同步完成有限关键回收，禁止递归日志与复杂控制逻辑。

### 7.22.2 Drain 顺序

1. 停止新 prefetch/容量增长；
2. 从低收益 class 的 TransferCache 摘取有界 batch；
3. 恢复对应 Span bitmap；
4. 将 newly empty Span 在 Central 锁外归还 PageCache；
5. 再由 Backend/Scavenger 决定 madvise 或 unmap。

Middle-end drain 只改变 allocator 内部所有权，不直接调用 `madvise`。

### 7.22.3 有界工作量

每次前台协助回收应有 `max_batches` 或 `max_bytes` 上限；后台控制线程按 shard 增量推进。高压信号不能让每个分配线程同时全量扫描所有 bucket。

### 7.22.4 与 Frontend 协同

压力 generation 由慢路径观察：Frontend 先 trim 本地冗余，Middle-end 再 drain shared cache。必须避免两层同时振荡式清空又立即大批 refill。

## 7.23 Reset、Shutdown 与 Fork

### 7.23.1 Reset 语义

需要明确区分：

- `DrainForTest()`：清空对象和 Span，但保留/重新初始化 TransferCache backing，可继续使用；
- `DestroyForShutdown()`：释放 backing，之后禁止普通请求；
- `ReinitializeForTest()`：在确认全局静止后重建完整 Central 状态。

当前 `Reset()` 释放 backing 后仍允许测试继续 fetch，实际退化为没有 TransferCache 的路径。应通过 API 语义和测试消除这种模糊状态。

### 7.23.2 Quiescence

破坏性 reset 前必须保证：

- 所有 Frontend 已 drain；
- 没有正在执行 fetch/release；
- 没有 remote batch 等待 Central owner；
- refill single-flight 已结束；
- scavenger 不会并发访问相关 Span。

Reset 不应试图在未知并发调用存在时“尽量工作”。

### 7.23.3 Shutdown 顺序

```text
stop new public allocations
 -> drain/close remote queues
 -> drain Frontend
 -> drain TransferCaches into Span bitmaps
 -> detach empty/active Central Spans
 -> release to PageCache
 -> destroy TransferCache metadata backing
 -> shutdown PageCache/PageAllocator
```

若生产策略选择进程退出时故意保留 allocator singleton，也应明确记录，而不是依赖未定义静态析构顺序。

### 7.23.4 Fork

`pthread_atfork` 策略应：

- prepare 阶段按固定顺序冻结控制线程和 Central shard；
- parent 阶段按逆序恢复；
- child 阶段重置只存在于其他线程的 refill/owner 状态；
- child 中保留的锁不得继承为永久 locked；
- atfork handler 不分配内存、不输出复杂日志。

## 7.24 元数据自举与递归规避

### 7.24.1 Backing 分配

TransferCache slots、BatchSlot、shard 数组和统计结构必须：

- 编译期定长内嵌；或
- 一次性通过 PageAllocator/SystemAlloc 建立；或
- 由 allocator-owned metadata arena/ObjectPool 管理。

禁止在初始化、扩容、NUMA node hotplug 或统计注册时使用会回到系统 malloc 的 STL 容器。

### 7.24.2 动态容量不等于动态分配

容量借额应在预分配 slot backing 内重新分区。可以使用固定 freelist、bitmap 或 index stack 管理空闲 segment；不能在热点 class 扩容时调用 `new[]`。

### 7.24.3 OOM 失败路径

- metadata backing 建立失败时返回可定义的初始化失败或进入明确降级模式；
- 如果核心 singleton 无法工作，使用 bootstrap-safe raw write 后 fail-fast；
- 不调用 `spdlog`、`std::string`、iostream 或可能初始化 locale 的设施；
- 不允许“TransferCache 初始化失败但半初始化 Bucket 继续运行”而没有显式模式标志。

### 7.24.4 NUMA 拓扑存储

最大 node/shard 数建议使用构建期上限与运行期 active count；超出上限时回退共享 node，而不是动态创建 `std::vector<CentralCache>`。

## 7.25 可观测性与守恒统计

### 7.25.1 每 class/shard 指标

至少包括：

- fetch requests/objects；
- full hit、partial hit、miss；
- release requests/objects；
- push accepted/overflow；
- current/capacity/high-water slots；
- prefetch extracted/published/rolled-back；
- bitmap words scanned、objects extracted/freed；
- partial/full/empty Span 数；
- PageCache refill/release 数；
- refill single-flight collisions；
- lock wait/spin samples；
- local/remote node batch。

### 7.25.2 字节指标

- transfer cached bytes；
- Central active Span bytes；
- bitmap-free usable bytes；
- extracted outstanding bytes；
- empty retained bytes；
- metadata backing bytes；
- budget assigned/used/reclaimable bytes。

### 7.25.3 守恒快照

提供仅用于测试/诊断的 stop-the-world snapshot：遍历 TransferCache、SpanList 和 bitmap，验证：

- slot 中无重复指针；
- slot 指针对应 Span/class/shard 正确；
- 每 Span `use_count == capacity - free_bit_count`；
- full/partial/empty 列表与 bitmap 状态一致；
- PageMap 仍指向相同 Span；
- Central cached object bytes 与逐项求和一致。

该功能可以较慢，但不能用于在线热路径。

### 7.25.4 统计开销

- 普通 fast path 不写 Central 统计；
- Middle-end 已是慢路径，可使用 shard-local relaxed counter；
- 纳秒级锁等待直方图采用采样；
- 导出时聚合，不在更新时获取全局锁；
- benchmark 同时运行 statistics on/off，量化观测成本。

## 7.26 测试与故障注入

### 7.26.1 TransferCache 基础测试

- 空/满/partial bulk push-pop；
- 精确 capacity 边界；
- batch 为 1 和 `kMaxBatchSize`；
- 指针数组与 intrusive chain 双向转换；
- 端到端 LIFO 顺序；
- overflow leftover 无遗漏；
- backing 对齐、分区不重叠。

### 7.26.2 所有权与 bitmap 测试

- bitmap bit、use_count 与对象状态逐步对应；
- TransferCache 中对象不恢复 free bit；
- full -> partial -> empty 转换；
- capacity 非 64 倍数的尾 bit；
- 同 batch 重复 free；
- 错误 class、错误 shard、非对象起始地址；
- empty Span 归还后不再出现在 Central 列表。

### 7.26.3 并发测试

- 多线程同 class fetch/release；
- TransferCache hit 与 Span miss 并发；
- prefetch publish 时其他线程填满 cache；
- full Span 在多个 release batch 下转 partial/empty；
- refill single-flight 与 PageCache OOM；
- 多 shard cross-thread free；
- NUMA owner 线程退出和 remote batch drain；
- TSan 下验证锁域，无 data race。

### 7.26.4 生命周期测试

- drain 后继续使用；
- destroy 后拒绝请求；
- 重复 init/drain 的幂等性；
- reset 与未 drain Frontend 的负向测试；
- fork 前存在 contended shard/refill；
- shutdown 时存在 empty retained Span 和 remote batch。

### 7.26.5 故障注入

在确定点注入：

- TransferCache backing 分配失败；
- PageCache refill 返回 null；
- partial fetch；
- prefetch 发布容量竞争；
- owner shard closing；
- pressure generation 切换；
- NUMA topology 不可用；
- metadata pool 耗尽。

每次失败后运行守恒快照，确认没有对象丢失、重复、Span 悬挂或锁未释放。

### 7.26.6 修复测试盲区

CentralCache fixture 应保证两类模式分别覆盖：

- TransferCache backing 存在且启用的正常模式；
- 显式禁用 TransferCache 的 Span-only 降级模式。

不能通过一个会永久释放 backing 的 `Reset()` 无意中让全部用例只测试降级路径。测试还应直接读取受控 diagnostics，确认 hit、prefetch、overflow 分支确实执行。

## 7.27 性能基准与回归门禁

### 7.27.1 专项微基准

建议新增独立目标或过滤项：

- `BM_Central_TransferFetchHit/{8,64,256,4096}`；
- `BM_Central_TransferReleaseHit/...`；
- `BM_Central_TransferPartialHit/...`；
- `BM_Central_BitmapAllocBatch/...`；
- `BM_Central_BitmapFreeBatch/...`；
- `BM_Central_PrefetchMiss/...`；
- `BM_Central_PageRefill/...`；
- `BM_Central_EmptySpanBounce/...`。

微基准必须把准备/清理移出计时区，分别报告 ns/object、ns/batch 和 cycles/object。

### 7.27.2 并发矩阵

变量至少包括：

- 线程数：1、2、4、8、16、32、oversubscribed；
- 大小：8B、64B、256B、4KiB、接近 `MAX_TC_SIZE`；
- batch：1、自然 batch、最大 batch、partial batch；
- 模式：单 Bucket、sharded、NUMA-local；
- 流量：balanced、allocate-heavy、free-heavy、producer/consumer、潮汐；
- 工作集：L1/L2 可容纳、跨 LLC、内存压力；
- affinity：固定 CPU、跨 socket、允许迁移。

### 7.27.3 采集指标

- TransferCache hit/miss/overflow；
- lock acquisitions、wait cycles、hold cycles；
- bitmap words/object；
- PageMap lookup/object；
- PageCache refill/release rate；
- cycles、instructions、branches、branch misses；
- L1/LLC miss、cache-to-cache、NUMA remote access；
- active/resident/Central cached bytes；
- p50/p99/p999 batch latency；
- 端到端 ammalloc 吞吐及 aethermind tokens/s。

### 7.27.4 与现有基准衔接

继续运行：

- `BM_Malloc_Churn` window 1024；
- `BM_Malloc_Deep_Churn`；
- 8B/64B 多线程 batch workload；
- 随机大小多线程 workload；
- 3.8 ns fast path 与 26.0 ns random-size 护栏。

专项基准用于归因，端到端基准用于验收。不能只因 Central 微基准改善就接受 Frontend fast path 或 RSS 退化。

### 7.27.5 回归判定

- 使用相同 Release 编译器、CPU affinity、governor、THP 和 NUMA policy；
- 保存 Google Benchmark JSON、allocator stats 和 perf 数据；
- 至少进行多轮交错 before/after，报告中位数和波动；
- 单线程 fast path 的噪声外退化必须阻断；
- 16 线程 64B 不得低于既有护栏，除非先证明原统计口径错误；
- 吞吐收益不能以无预算 Central RSS 或严重 p999 退化换取；
- shard/NUMA 默认开启前必须在真实 aethermind trace 上验证。

## 7.28 分阶段实施与验收

### 阶段 A：所有权、生命周期与测试基线

实施内容：

1. 定义 ME 不变量和 `ObjectBatch` 语义；
2. 统一批量 LIFO 顺序；
3. 区分 drain、destroy 与 reinitialize；
4. 修复 TransferCache 测试被 Reset 绕过的问题；
5. 增加守恒快照和 OOM/failure injection；
6. 将 bootstrap OOM 路径改为不分配日志策略；
7. 建立 Central 专项 benchmark。

退出条件：

- TransferCache normal 与 disabled 两种模式均被真实覆盖；
- 所有 batch 顺序和对象守恒测试通过；
- reset/shutdown 不产生 UAF、泄漏或半初始化状态；
- ASan/UBSan/TSan 通过；
- 现有 3.8 ns、26.0 ns 和 16 线程护栏无不可接受退化。

风险类型：正确性、内存、并发、性能。

### 阶段 B：批量 bitmap、预取与预算

实施内容：

1. pointer-stack bulk push/pop；
2. Span `AllocBatch/FreeBatch`；
3. last-span 或定长分组实验；
4. 自适应预取和发布失败统计；
5. process/node Central byte budget；
6. 有界 empty Span retention 实验；
7. 内存压力 drain。

退出条件：

- bitmap words/object 和锁持有周期显著下降；
- duplicate/free 边界不会破坏 use_count；
- 预取 publish rollback 有界且守恒；
- Central cached/retained bytes 始终受预算约束；
- churn workload 的 PageCache 往返或尾延迟获得可量化改善；
- fast path 和 RSS 护栏通过。

风险类型：正确性、内存、性能。

### 阶段 C：Size-class shard 与 refill single-flight

实施内容：

1. 将 Bucket 拆为独立缓存行的 Transfer 与 Span shard 状态；
2. 热点 size class 启用可配置 shard；
3. 建立稳定 Frontend route 和 Span central-owner 标识；
4. 实现单 shard、无跨锁的 cross-thread release；
5. 增加 refill single-flight/backoff；
6. A/B 实验批次描述符 TransferCache。

退出条件：

- 同 class 高竞争下 lock wait/cache-to-cache 显著下降；
- 一个 active Span 永远只被一个 shard 管理；
- cross-shard free 无 ABA、UAF 或对象漂移；
- PageCache 过度 refill 和低占用 Span 数得到控制；
- descriptor 模式只有综合收益明确时保留；
- 单线程和冷 class 不因分片 metadata 显著退化。

风险类型：并发、正确性、内存、性能。

### 阶段 D：NUMA-local Middle-end 与生产灰度

实施内容：

1. 每 NUMA node 建立有界 CentralCache 组；
2. 与 PageCache region/node ownership 对齐；
3. owner-node release 和 bounded remote batch；
4. node 级预算、压力回收和统计；
5. CPU/node hotplug、拓扑不可用与单 node 回退；
6. 在 aethermind 固定 worker 和真实 cross-thread trace 灰度。

退出条件：

- 本地分配的物理页与 Central route 一致；
- remote release 不丢对象且队列受预算控制；
- 跨 socket remote access、吞吐或尾延迟获得明确收益；
- tokens/s、请求 p99、RSS 三项综合不劣于单 node 基线；
- 支持运行时或构建期开关快速回退单 Central 模式。

风险类型：并发、内存、性能、兼容性。

