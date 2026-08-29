# 第 8 章：Backend、PageCache 与大对象管理

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](06-middle-end.md) · [下一章](08-rss-fragmentation-and-scavenging.md)  
> **本章目标**：建立可事务化的页级分配、回收、大对象和 OS 交互路径。  
> **适用范围**：PageCache、PageAllocator、Region、LargeExtent、split/coalesce、THP 与 hugepage。  
> **核心 invariant**：所有状态转换有唯一 owner；OS 操作不在核心锁内；发布与回滚顺序明确。

Backend 是 ammalloc 中管理虚拟地址区间、页级空闲空间和 OS 映射的最后一层。它既为 Middle-end 提供承载小对象的 run，也直接承载超过 Frontend 上限的大对象。Backend 的首要目标是保持地址范围所有权和 PageMap 一致；在此基础上，再优化分片扩展性、外部碎片、系统调用频率、RSS 和 hugepage 利用率。

本节与[第 5 章：PageMap 与 Span 生命周期](04-pagemap-and-span-lifecycle.md)、[第 9 章：RSS、碎片与后台回收](08-rss-fragmentation-and-scavenging.md)的边界如下：第 5 章定义 PageMap 发布、Span 生命周期和 descriptor 延迟回收协议；本章定义 PageCache 的数据结构、分配/释放事务、大对象路径和 PageAllocator 接口；第 9 章进一步定义 dirty/purged decay、RSS 压力和后台回收策略。三章必须共享同一 Span 状态机和统计口径。

## 8.1 职责、边界与性能目标

### 8.1.1 Backend 的职责

Backend 应负责：

- 管理 allocator 所拥有的虚拟地址 region、extent 和 run；
- 为 Middle-end 按页数提供 small-object Span；
- 为大对象提供 page-aligned 或显式 over-aligned 地址范围；
- 对 free extent 执行 exact hit、fit selection、split 和 owner-local coalesce；
- 维护 extent 的 committed、purged、retained、direct-mapped 等状态；
- 在 PageMap 中原子发布和撤销地址范围到 Span descriptor 的映射；
- 通过 PageAllocator 执行 mmap、munmap、madvise、prefault 和 hugepage hint；
- 在 NUMA 模式下保持 region、PageCache shard 与物理放置策略一致；
- 在 OOM、内存压力、shutdown 和 fork 时提供可预测的降级与清理行为；
- 提供可验证的页、extent、VMA、碎片和系统调用统计。

Backend 不应负责：

- 普通小对象的逐对象分配和 bitmap 管理；
- Frontend 或 TransferCache 的对象缓存策略；
- 在 PageCache shard 锁内执行不可控时长的系统调用；
- 使用 `std::map`、`std::vector` 等堆分配容器维护 extent；
- 将“2 MiB 对齐”或 `MADV_HUGEPAGE` 成功等同于物理 hugepage 已建立；
- 将 device memory、CUDA pinned memory 或其他非普通 CPU heap 资源混入 PageCache；
- 通过随机 shard 哈希破坏相邻地址的统一所有权。

### 8.1.2 分层结构

```text
Middle-end small-object Span request
Large-object allocation request
                 |
                 v
        Backend routing policy
                 |
      +----------+-----------+
      |                      |
      v                      v
SmallRunCache          LargeExtentSet
exact page buckets     size + address index
      |                      |
      +----------+-----------+
                 |
                 v
        Region / owner shard
                 |
       retained/purged/direct
                 |
                 v
          PageAllocator
      mmap/munmap/madvise
```

`SmallRunCache`、`LargeExtentSet` 和 `DirectMapped` 是不同策略层，不要求第一阶段立即成为三个独立 C++ 类型，但状态、指标和路由语义必须先区分。

### 8.1.3 性能与内存目标

| 指标 | 目标方向 | 说明 |
|---|---|---|
| Exact bucket hit | O(1) 且无 OS 调用 | 仅操作 owner shard 本地状态 |
| Split candidate lookup | O(1) 或 O(log N)，有界 | 禁止锁内扫描全部 extent |
| Coalesce | 通过地址索引或 PageMap O(log N)/O(1) 定位邻居 | 不跨 owner region |
| Shard 扩展性 | 后端 miss/churn 下随 shard 数改善 | 普通 Frontend fast path 不应受影响 |
| OS 调用锁占用 | shard 锁内为零 | mmap/munmap/madvise 均在锁外 |
| PageMap 更新 | 与修改的页/leaf 数成正比且可测量 | 大 extent 不得形成不可见长尾 |
| 外部碎片 | 按 region、size tier 可观测并受策略控制 | 不能只报告总 free bytes |
| RSS/VA 放大 | 分别预算 resident 与 retained VA | 保留 VA 不等于保留物理页 |
| 大对象尾延迟 | 分离 cache hit、split、mmap 和 purge | 报告 p99/p999，而非只有平均值 |
| Hugepage 效率 | 以实际 backing 和 breakage 评估 | 对齐/hint 只能作为过程指标 |

Backend 优化必须继续满足整体护栏：单线程 fast path 约 3.8 ns、随机大小约 26.0 ns、16 线程 64B 压力场景约 8.9 us 且吞吐 100+ GiB/s。Backend 通常位于慢路径，若这些基线退化，优先排查 PageMap 读路径、Span 布局、公共路由分支和新增共享统计。

## 8.2 Backend 核心不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 目的 |
|---|---|---|
| BE-1 | 每个受管虚拟页在任一时刻最多属于一个逻辑 Span/Extent | 防止重叠分配 |
| BE-2 | 每个 active/free extent 恰好属于一个 region 和一个 PageCache owner shard | 保证唯一锁域 |
| BE-3 | `owner_shard_id` 在第一次 PageMap 发布前确定，descriptor epoch 内不可改变 | release 可稳定路由 |
| BE-4 | 一个 free extent 同时只在一种 free 数据结构和一个状态集合中 | 防止重复分配 |
| BE-5 | bucket 或 size index 中记录的大小与 descriptor 地址范围完全一致 | 保持 fit 正确性 |
| BE-6 | address index 中相邻节点的地址范围不重叠且严格有序 | 支持可靠 coalesce |
| BE-7 | split 后 allocated 与 remainder 的并集严格等于原 extent | 防止页遗失或重叠 |
| BE-8 | coalesce 后结果范围严格等于参与 extent 的并集 | 保证地址守恒 |
| BE-9 | 只有逻辑空闲且不再被 Frontend/Middle-end/用户引用的 Span 才能进入 PageCache free state | 防止 UAF |
| BE-10 | unmap 前先撤销对应 PageMap 映射；purge 保留 VA 时映射和状态必须一致 | 防止 stale lookup |
| BE-11 | PageMap reader 可能观察到的 descriptor 不得立即复用 | 防止 ABA/UAF |
| BE-12 | split/coalesce/PageMap 发布是失败原子的 | OOM 不破坏旧所有权 |
| BE-13 | mmap、munmap、madvise、mbind、prefault 不在 PageCache shard 锁内执行 | 控制锁尾延迟 |
| BE-14 | 不同时持有两个普通 PageCache shard 锁完成一次分配或合并 | 避免跨 shard 死锁 |
| BE-15 | coalesce 不跨 region、owner shard、page policy 或不兼容 backing 类型 | 保持释放协议正确 |
| BE-16 | DirectMapped 释放使用原始 mapping base/length，而非仅使用用户可见地址和 usable size | 正确 munmap |
| BE-17 | requested、usable、mapped、resident、retained 等字节口径不重复计算 | 保证指标可信 |
| BE-18 | extent index、metadata arena 和 region registry 不依赖被 ammalloc 拦截的堆分配 | 避免递归 |
| BE-19 | 所有 size/page/alignment 加法、乘法和向上取整先检查溢出 | 防止小映射承载超大请求 |
| BE-20 | 2 MiB alignment、THP eligibility、实际 THP backing 和 hugetlb backing 分别记录 | 避免错误优化结论 |
| BE-21 | committed/purged 状态转换不改变逻辑地址所有权 | 保证复用与 coalesce 稳定 |
| BE-22 | destructive reset 只在所有 allocator 用户、Scavenger 和 PageMap reader 静止后执行 | 防止并发生命周期破坏 |

建议增加 Backend 守恒式：

```text
region_reserved_bytes
  = active_small_span_bytes
  + active_large_bytes
  + free_committed_bytes
  + free_purged_bytes
  + metadata_or_guard_bytes
  + unassigned_region_bytes
```

DirectMapped、显式 hugetlb 和 region 外特殊映射应单独计入 `direct_mapped_bytes`，不能重复包含在 region 总量中。

## 8.3 当前实现基线与差距

### 8.3.1 已有基础

当前实现已经具备：

- 固定容量 `PageCacheShard[4]` 和每 shard 独立 mutex、页数桶、Span ObjectPool；
- 生产默认启用一个 shard，release 通过 `owner_shard_id` 回到 owner；
- 1～128 页的精确桶、向上寻找较大桶及 head split；
- 左右相邻 free Span 的 owner-local coalesce；
- 超过 128 页的 Span 直接通过 PageAllocator 映射并在 free 时直接解除映射；
- PageMap 四层 radix tree 的无锁读路径；
- Span 和 RadixNode 使用 PageAllocator-backed ObjectPool，未使用堆分配 STL 容器；
- 2 MiB 对齐、`MADV_HUGEPAGE` hint、普通页 fallback 和固定容量 2 MiB VMA cache；
- PageAllocator 溢出保护、mmap/munmap/madvise 失败计数；
- PageCache exact hit、split、merge、PageMap 和同桶争用基准；
- Scavenger 在锁下摘除 Span、锁外 `madvise`、再重新入桶的基本模式。

这些能力可以作为单 shard 正确性基线，但不能直接推导出真正多 shard、NUMA、hugepage-aware 或完整 large extent 管理已经成立。

### 8.3.2 关键差距

当前主要差距包括：

- `SelectShardForAlloc()` 固定返回 0，生产没有实际分片；
- `AllocSpanLocked()` 将 OS refill 和 oversized Span 的 owner 先写成 0，并在 PageMap 中发布；外层返回后才覆盖 owner，直接开启非零 shard 会产生 owner、free-list 和 PageMap 发布时序不一致；
- test-only `SetActiveShardCountForTest()` 没有被现有单元测试调用，多 shard 路径缺少验证；
- 每 shard 都可能并发安装 PageMap radix node，但没有独立全局 writer 协议；
- mmap、munmap 以及 metadata chunk 的 SystemAlloc 发生在 shard mutex 临界区；
- 所有 1～128 页 bucket miss 都线性扫描到 128，当前上限尚小，但未来扩大阈值会变为锁内线性成本；
- 超过 128 页全部 direct map，缺少 LargeExtentSet，导致中大型对象反复 mmap/munmap；
- 128 页上限同时承担“精确桶上限”“coalesce 上限”和“OS refill 大小”，策略耦合；
- large allocation 只保存页数，没有 requested size、alignment、mapping base/user offset、arena/node 或 page policy；
- 大对象 PageMap 发布逐页执行 atomic store，映射规模增大时延迟与 metadata 成本线性增长；
- 当前 ObjectPool `Delete(span)` 会立即复用 descriptor，不能满足[第 5 章](04-pagemap-and-span-lifecycle.md)规定的无锁 reader 生命周期；
- 全局 2 MiB cache 缓存的是 `MADV_DONTNEED` 后保留的 VMA，不代表物理 hugepage 仍存在；
- 所有达到 1 MiB 的请求进入 huge alignment 路径，但阈值与收益尚未通过大对象专项基准证明；
- `MADV_HUGEPAGE` 仅是 THP hint，当前统计却容易被理解为 hugepage 实际成功；
- 没有 `MAP_HUGETLB`/memfd-hugetlb 等显式 hugepage 模式及其严格失败语义；
- PageAllocator 失败路径仍使用可能分配内存的 `spdlog`，不满足完整 malloc interposition 的自举契约；
- `ResetLocked()` 在每个 shard 内调用全局 `PageMap::Reset()`，未来多 shard reset 需要统一事务而不是逐 shard 重置全局索引；
- PageCache 测试依赖单线程地址连续性和内部桶状态，尚未覆盖 region、owner、OS unlock/relock 竞争及故障回滚。

## 8.4 分配类型与路由矩阵

### 8.4.1 路由输入

Backend 路由不能只读取页数，还应考虑：

- requested size；
- usable size；
- alignment；
- small-object Span 请求还是用户大对象请求；
- NUMA node/arena；
- hot/cold、short/long lifetime hint；
- 是否允许 retained VA；
- THP/hugetlb policy；
- guard page 或 hardened mode；
- 当前内存压力和 shard/region budget。

普通 `am_malloc` 可以只使用默认策略；扩展 API 再显式传入 node、alignment 和 lifetime，避免污染标准 ABI。

### 8.4.2 建议路由层级

| 类型 | 典型来源 | 建议管理结构 | 释放策略 |
|---|---|---|---|
| Small run | CentralCache 请求若干页 | SmallRunCache | owner-local coalesce，按状态保留或 purge |
| Medium/large cached extent | 大于 Frontend 上限但适合复用 | LargeExtentSet | 地址合并、size best-fit、decay |
| Direct mapped | 极大、特殊 alignment、guarded 或低复用请求 | DirectMapped registry + PageMap | clear mapping 后直接 unmap 或保留专用 extent |
| Hugepage filler run | 小于 2 MiB 且需要 THP 聚合 | HugepageFiller | 回到所属 hugepage，按 breakage 决定 purge |
| Huge region extent | 多个 2 MiB 单元的大对象/模型内存 | HugeRegion | hugepage-aware split/coalesce |
| Explicit hugetlb | 用户显式要求且系统配置允许 | 独立 hugetlb arena | 严格对应 hugetlb release，不与匿名页混合 |

### 8.4.3 阈值解耦

至少拆分以下配置：

- `small_run_max_pages`：精确桶上限；
- `region_refill_pages`：一次 OS/region 补货规模；
- `large_extent_cache_max_bytes`：LargeExtentSet 最大可缓存 extent；
- `direct_map_threshold`：超过后优先直接映射；
- `retain_max_bytes`：可保留 VA/extent 上限；
- `hugepage_filler_max_pages`：允许进入 filler 的 run 上限；
- `hugetlb_min_bytes`：显式大页适用阈值。

这些阈值不能继续全部隐含绑定到 `MAX_PAGE_NUM=128`。

### 8.4.4 溢出与零请求

路由前统一使用 checked arithmetic：

1. 处理 zero-size 的公共 ABI 语义；
2. 检查 `size + alignment - 1`；
3. 检查 guard/header/offset 加法；
4. 检查 byte-to-page 向上取整；
5. 检查 page count 能否表示在 descriptor 字段中；
6. 检查 PageMap 地址覆盖是否超出支持的 VA bits。

任何失败都必须在调用 mmap 或修改缓存前返回。

## 8.5 Page、Extent 与 Region 状态模型

### 8.5.1 状态定义

建议统一使用以下逻辑状态：

```text
kFresh
kInUseSmall
kInUseLargeCached
kInUseDirect
kFreeCommitted
kFreePurged
kRetained
kSplitOrCoalesce
kDetachedForPurge
kDetachedForUnmap
kRetiredDescriptor
```

状态可以编码在 Span flags 与冷 side metadata 中，不要求全部放入 64B Span；但所有实现和测试必须按同一状态机验证。

### 8.5.2 虚拟地址与物理页分离

必须区分：

- **Mapped/Reserved**：虚拟地址范围仍存在；
- **Committed/Resident candidate**：匿名映射可按需提供物理页；
- **Purged**：已 `MADV_DONTNEED`/等价处理，VA 保留，物理页可被内核回收；
- **Retained**：allocator 保留该 VA/extent 用于未来复用；
- **Unmapped**：VA 已归还内核；
- **Huge-backed**：经系统证据确认由 THP 或 hugetlb backing，而不是根据对齐推断。

匿名 mmap 在 Linux 上通常采用按需分配；因此“mmap 成功”不等于全部页 resident，“committed”也不能直接等同 RSS。

### 8.5.3 主要状态转换

```text
OS/region acquire
  -> kFresh
  -> publish free extent
  -> kFreeCommitted
       -> allocate -> kInUseSmall / kInUseLargeCached
       -> purge    -> kDetachedForPurge -> kFreePurged
       -> unmap    -> kDetachedForUnmap -> kRetiredDescriptor

kFreePurged
  -> allocate/fault-on-write -> active
  -> retain                  -> kRetained
  -> unmap                   -> retired

kInUseDirect
  -> clear PageMap
  -> unmap mapping
  -> retire descriptor
```

所有 split/coalesce 通过暂态 `kSplitOrCoalesce` 完成，不能让中间结果进入可分配索引。

### 8.5.4 Backing policy 兼容性

只有下列属性兼容时才能合并：

- 相同 region 和 owner shard；
- 相同匿名/hugetlb/file-backed 类型；
- guard page 边界允许合并；
- NUMA policy 相容；
- committed/purged 状态存在明确定义的合并结果；
- encryption/pinning/device 等特殊属性一致。

不兼容 extent 即使虚拟地址连续也必须保持分离。

## 8.6 Region-based PageCache 分片

### 8.6.1 为什么需要 region ownership

仅把每次申请按 CPU 或线程 hash 到不同 shard，会让相邻 OS 映射随机归属不同 shard。结果是：

- 释放时无法跨 shard 合并；
- address space 被切成大量小岛；
- PageMap writer 仍全局共享；
- 线程迁移导致同一工作集漂移；
- 多锁 coalesce 设计复杂且容易死锁。

正确模型是先确定 region owner，再从该 region 内完成所有 split、allocate、free 和 coalesce。

### 8.6.2 Region 描述

每个 Region 至少包含：

- base address 和 reserved size；
- region id、owner shard id、NUMA node id；
- backing/page policy；
- free committed/purged/retained bytes；
- SmallRunCache 和 LargeExtentSet 入口；
- region generation/closing 状态；
- PageMap/region registry 发布状态；
- decay/scavenge cursor。

Region metadata 由固定上限数组或 metadata arena 管理，不使用堆容器。

### 8.6.3 Region 获取方式

候选方式：

1. **独立 mmap region**：一次映射较大匿名 VA，内部切分；实现简单；
2. **PROT_NONE reserve + 按需映射/提交**：更清晰地区分 VA reservation 与物理使用，但 VMA 和 commit 协议更复杂；
3. **固定地址 arena**：通过 `MAP_FIXED_NOREPLACE` 等机制获取预定区间，适用于高级 arena，但需要严格兼容性和失败回退；
4. **OS extent 直接归属**：不预留大 region，但每个新 mapping 整体归给一个 shard；短期最容易落地。

第一阶段建议采用“OS extent 整体归属 shard”，先建立 owner 正确性；中期再评估大 VA region reservation。

### 8.6.4 Region 边界

- split 不改变 region id；
- coalesce 不跨 region；
- region 只有在全部 extent 空闲且无 PageMap reader 生命周期风险时才能整体 unmap；
- region closing 后禁止新分配；
- region metadata 只有在 PageMap leaf 撤销并满足[第 5 章](04-pagemap-and-span-lifecycle.md)的 retire 协议后才能复用；
- region 大小应为 hugepage 和 SmallRun refill 单位的公倍数，但不得盲目预留过量 VA。

## 8.7 Shard 选择与 owner 发布协议

### 8.7.1 当前多 shard 阻塞点

当前内部 refill/split 路径把 `owner_shard_id` 写为 0并发布 PageMap，外层 `AllocSpan()` 返回前才把对象改为选定 shard。对于 shard 1～3，这会导致 remainder 留在非零 shard 的 list 中却声明 owner 0，后续 release/coalesce 进入错误锁域。因此在修复发布协议前，生产必须继续固定 shard 0。

### 8.7.2 正确发布顺序

```text
select owner shard/region
  -> prepare descriptor with owner id
  -> initialize address, page count, backing policy, state
  -> acquire owner shard + PageMap writer protocol
  -> validate target range/index state
  -> publish PageMap range
  -> insert free remainder or return active Span
  -> release locks
```

owner id 必须作为 `AllocSpanLocked(owner_context, page_num)` 的输入，而不是在函数返回后修补。

### 8.7.3 Owner 继承

- exact hit 保持原 owner；
- split 的 allocated 和 remainder 都继承 region owner；
- coalesce 结果保持共同 owner；
- direct mapping 在第一次发布前确定 owner/arena；
- NUMA fallback 到其他 node 时记录实际 owner，而非保留请求 node；
- active Span 不允许通过简单字段写入迁移 shard。

### 8.7.4 路由策略

推荐优先级：

1. 调用方显式 arena/node；
2. Frontend/Middle-end 稳定 route；
3. 当前 CPU 所属 node/shard；
4. shard 压力/容量有限回退；
5. shard 0 兼容模式。

选择结果在一次 allocation transaction 内固定。不能在 split 或 OS refill 返回后因线程迁移重新选择 shard。

### 8.7.5 验证

每次 release 至少校验：

- owner id 在 active range 内；
- descriptor 确实由该 shard metadata arena 创建；
- region id 与 shard 一致；
- PageMap 对首尾及采样页指向相同 descriptor/generation；
- free-list membership 为空。

## 8.8 SmallRunCache 精确桶

### 8.8.1 适用范围

SmallRunCache 服务 CentralCache 请求的较小连续页区间，也可服务高复用的中小型直接对象 extent。建议 `small_run_max_pages` 独立于 OS refill 大小和 DirectMapped 阈值。

### 8.8.2 数据结构

基础结构仍可使用：

```text
SpanList committed_buckets[1..N]
SpanList purged_buckets[1..N]      // 可选
non_empty_bitmap
```

`non_empty_bitmap` 在 shard 锁内维护，不需要 atomic。若 N ≤ 128，可用两个 64-bit word；查找下一个非空 bucket 使用 mask + `countr_zero`，避免未来阈值扩大后线性扫描。

### 8.8.3 Exact hit

exact hit 事务只应：

1. 从 bucket 头摘除一个 Span；
2. 清除对应 non-empty bit（若 bucket 变空）；
3. 验证 page count、owner、region、状态；
4. 转为 active 状态；
5. 保持 PageMap 指向稳定 descriptor；
6. 返回调用方。

不执行 OS 调用，不分配 descriptor，不重写整个 PageMap range。

### 8.8.4 Bucket 内顺序

候选顺序：

- recently freed LIFO：局部性高；
- oldest first：更容易把冷页用于 purge；
- committed 优先、purged 次之：减少首次触页；
- 根据 hot/cold hint 分两条链。

短期保留 LIFO committed bucket，并让 Scavenger 从冷端扫描；若 SpanList 只支持单端访问，可增加 cold cursor，而不是每次全表遍历。

### 8.8.5 Purged Span 复用

purged Span 仍拥有 VA 和 PageMap 映射。重新分配时通常不需要显式“commit”，首次写会重新建立匿名页；但状态和统计必须从 purged 转 active，不能继续计入可回收 RSS。

## 8.9 LargeExtentSet 双索引

### 8.9.1 设计目的

对于超过精确桶上限、但具有明显复用价值的中大型 extent，全部 direct mmap 会增加：

- mmap/munmap 次数和内核锁竞争；
- VMA 创建/销毁成本；
- PageMap node 反复创建；
- 对齐 over-map/trim 成本；
- 大对象 p99/p999 延迟。

LargeExtentSet 应同时支持按大小寻找 fit 和按地址寻找邻居。

### 8.9.2 双索引语义

- **Size index**：键为 `(page_count, address)`，用于 exact/best-fit；
- **Address index**：键为 `start_page_id`，用于前驱/后继和 overlap 检查；
- 同一个 free extent 同时位于两个索引；
- active、detached 或 direct extent 不在 free size index；
- 插入/删除两个索引必须在同一 shard transaction 内完成。

### 8.9.3 无递归实现

禁止使用 `std::map`/`std::multimap`。候选实现：

- intrusive red-black tree；
- intrusive treap（priority 由稳定无分配 PRNG/hash 生成）；
- 分级 size bins + intrusive address tree；
- radix/segment tree，仅在 region 地址结构适合时采用。

Extent descriptor 需要两套 tree hooks。若 64B Span 放不下，应使用 PageCache-only side metadata 或独立 `ExtentDescriptor`，不能无评估地膨胀所有小对象 Span。

### 8.9.4 Fit 查找

默认建议：

1. exact size 优先；
2. size tree lower_bound 找最小足够 extent；
3. 在有限候选窗口内按 committed、NUMA、alignment 和碎片代价排序；
4. 超出搜索预算时接受第一个合格 best-fit；
5. 没有候选再进入 OS/region refill。

禁止为追求理论最优在 shard 锁内扫描所有 free extent。

### 8.9.5 Index 守恒

诊断模式验证：

- 两棵树节点集合完全一致；
- address tree 无重叠；
- size key 与 descriptor page count 一致；
- 所有节点 owner/region 相同；
- 索引节点不在 SmallRun bucket；
- free bytes 等于索引逐项求和。

## 8.10 DirectMapped 大对象路径

### 8.10.1 适用条件

以下请求优先 DirectMapped：

- 大于 `direct_map_threshold`；
- alignment 明显大于普通 region 单元；
- guarded/hardened 分配；
- 显式 hugetlb、file-backed 或特殊 page policy；
- workload 表明复用概率很低；
- LargeExtentSet 预算不足且内存压力较高。

### 8.10.2 分配事务

```text
checked request geometry
  -> allocate descriptor outside shard lock
  -> mmap/align/trim outside shard lock
  -> initialize mapping_base, mapping_size, user_ptr, usable_size
  -> acquire PageMap writer protocol
  -> publish address range
  -> mark kInUseDirect
  -> return user_ptr
```

如果 PageMap node/descriptor 准备失败，必须 unmap 完整原始 mapping，不能留下无法释放的 VMA。

### 8.10.3 释放事务

```text
PageMap lookup + pointer validation
  -> mark/detach direct descriptor
  -> clear entire mapping range from PageMap
  -> release writer/shard locks
  -> munmap(mapping_base, mapping_size)
  -> retire descriptor
```

如果 munmap 失败，不能重新把已经交还用户语义的对象发布为 active。需要记录 leaked-mapping/quarantine 状态并提供 fail-fast 或诊断策略。

### 8.10.4 PageMap 成本

当前 `SetSpan` 为每个页 leaf 执行 store，大型映射的发布/清除成本为 O(page count)。改进方向包括：

- 按 leaf coverage 批量 fill/clear；
- 为同一 Span 的完整 radix leaf 使用专用 range marker；
- 缓存已存在的 radix path；
- 评估大对象 header/side registry 与 PageMap 的组合，但不得让普通 free 变为加锁查树；
- 继续保证 `GetSpan` 固定深度和无锁。

任何压缩方案都必须与[第 5 章](04-pagemap-and-span-lifecycle.md)的 reader 生命周期和多 writer 协议共同设计。

## 8.11 大对象 metadata、usable size 与 alignment

### 8.11.1 必要字段

大对象至少需要保存：

- requested size；
- usable size；
- user pointer 或 user offset；
- original mapping base 和 mapping length；
- logical extent start/page count；
- requested/effective alignment；
- owner shard、region、NUMA node；
- page policy、guard flags、direct/retained 类型；
- lifecycle generation；
- allocation tag/profile sample（可选）。

这些字段不应全部塞进热 Span。推荐将公共身份字段保留在 64B Span，将 large-only 冷字段放进 allocator-owned `LargeExtentMetadata`。

### 8.11.2 普通大对象

普通 `malloc` 大对象至少返回 `max_align_t` 对齐；当前 page alignment 已满足。usable size 可以是页向上取整后的长度，但 `malloc_usable_size` 必须返回用户可用范围，而非包含 guard/header/trim 区域的 mapping length。

### 8.11.3 Over-aligned 请求

对 `aligned_alloc/posix_memalign`：

1. 验证 alignment 是合法 2 的幂和 ABI 要求；
2. checked 计算 `size + alignment - 1`；
3. 优先从对齐合适的 region extent split；
4. fallback 才执行 over-map + trim；
5. 保存 original mapping geometry；
6. 将所有可接受 free 地址页正确映射到 descriptor；
7. free 时验证传入的是原始 user pointer，而非任意 interior pointer。

### 8.11.4 Realloc

大对象 realloc 的演进顺序：

- usable size 足够时原地返回；
- shrink 时可选择保留尾部、拆分回 LargeExtentSet 或直接 unmap 尾部；
- 相邻 free extent 足够时在 owner shard 内原地扩展；
- DirectMapped 可在 Linux 上实验 `mremap`，但必须处理地址移动后的 PageMap 原子切换；
- 否则 allocate-copy-free；
- 失败保持原对象有效。

### 8.11.5 零填充与安全

新匿名 mmap 初始为零，但缓存/retained extent 复用不保证满足 calloc 语义。calloc 必须显式依赖状态：purged/新映射可利用内核零页语义，committed dirty extent 必须清零。不能仅以“来自 PageAllocator”推断内容为零。

## 8.12 Exact-hit 分配事务

### 8.12.1 前置条件

- request geometry 已校验；
- owner shard/region 已确定；
- bucket/index key 合法；
- 调用线程没有持有 CentralCache 锁；
- PageMap reader contract 已建立。

### 8.12.2 事务步骤

1. 获取 owner shard 锁；
2. 从 committed exact bucket 优先摘取；
3. 若无 committed，再从 purged exact bucket摘取；
4. 校验 state、page count、region、owner 和 index membership；
5. 标记暂态/active，更新索引位图和字节计数；
6. 若 descriptor/range 不变，不重写 PageMap；
7. 释放 shard 锁；
8. 对 purged extent 仅在策略需要时执行预取/预触页；
9. 初始化 small-object Span 或 large metadata 后再向上层发布。

### 8.12.3 失败语义

exact hit 不应因额外 metadata allocation 失败而丢失 Span。所需 large side metadata 应在摘取前准备，或采用可回滚的 reserved metadata slot。失败时 extent 回到原状态和原索引位置。

## 8.13 Split 分配事务

### 8.13.1 Candidate 选择

- SmallRun 使用 non-empty bucket bitmap 找最近较大 bucket；
- LargeExtentSet 使用 size lower_bound；
- alignment 可能产生 prefix/suffix 两个 remainder；
- candidate backing policy 必须兼容请求；
- 分裂后的碎片必须达到最小可管理粒度。

### 8.13.2 Metadata 预留

在从 free index 摘除 candidate 前准备好最坏情况下所需 descriptor：

- 普通 head/tail split：allocated + 一个 remainder；
- alignment split：prefix + allocated + suffix；
- guard page：可能再增加保护区 metadata；
- 若采用新 descriptor 发布协议，还需为旧 descriptor retire 准备节点。

metadata OOM 时原 candidate 保持不变。

### 8.13.3 提交步骤

1. 获取 owner shard 与 PageMap writer 协议；
2. 重新验证 candidate 仍在预期索引；
3. 从 size/address index 或 bucket 摘除；
4. 计算所有子区间，验证无溢出、无空洞、无重叠；
5. 初始化子 descriptor 的 owner/region/state；
6. 批量切换 PageMap range；
7. 将 remainder 插入正确 bucket/双索引；
8. 将 allocated extent 标记 active；
9. retire 不再使用的旧 descriptor；
10. 更新 committed/purged/active/fragmentation 计数；
11. 释放锁并向上层发布。

### 8.13.4 碎片抑制

- prefix/suffix 小于最小 extent 时并入 usable size或换候选；
- 对大 alignment 计算 `waste_bytes`；
- 避免不断从同一 hugepage 中间切出导致 breakage；
- fit score 同时考虑 remainder 数量和大小；
- 不为减少几个页浪费而引入锁内无界候选搜索。

## 8.14 OS/Region refill 事务

### 8.14.1 当前问题

当前 `AllocSpanLocked()` 在持有 shard mutex 时执行 SystemAlloc；mmap 重试、VMA 操作、THP hint 及 metadata chunk 分配都会扩大同 shard 等待时间。多线程 miss 时还可能形成串行内核调用或重复补货。

### 8.14.2 Prepare 阶段

在 shard 锁内：

- 再次确认没有合格 free extent；
- 检查 region/shard budget；
- 尝试取得 refill single-flight ownership；
- 记录 request class、期望 pages 和 generation；
- 预留可用 descriptor/region slot；
- 随即释放锁。

### 8.14.3 OS 阶段

锁外执行：

- mmap/reserve/commit；
- alignment trimming；
- NUMA policy 或 mbind；
- hugepage hint；
- 可选 prefault；
- 初始化尚未共享的 descriptor。

该阶段产生的 mapping 由当前线程独占，失败可以直接清理，不接触共享索引。

### 8.14.4 Publish 阶段

重新获取 owner shard和 PageMap writer 协议后：

- 验证 shard/region 未 closing；
- 验证 single-flight generation；
- 重新检查是否已有其他可用 extent；
- 将新 mapping 作为完整 free extent 发布；
- 设置 owner 后再写 PageMap；
- 插入 bucket/双索引；
- 清除 refill 状态；
- 重试 exact/split 事务。

如果发布时已不需要该 mapping，可将其作为预算允许的 free extent 缓存，或在锁外 unmap；不能在持锁状态直接释放。

### 8.14.5 Refill 大小

refill 大小应由以下因素决定：

- request pages；
- SmallRun 常用 page count；
- region/large extent 最小单元；
- hugepage 边界；
- 最近 split remainder 利用率；
- shard 当前 free bytes；
- 内存压力与 NUMA node budget。

固定补 128 页可作为初始策略，但必须与 exact bucket 上限解耦。

## 8.15 Release 与 Coalesce 事务

### 8.15.1 接收前验证

PageCache 接收 Span/Extent 前验证：

- descriptor 非空且 active；
- owner shard/region 合法；
- 小对象 Span `use_count == 0` 且 bitmap 全 free；
- large/direct pointer 是 allocation base；
- descriptor 不在任何 free index；
- PageMap 首尾及必要采样 leaf 与 descriptor/generation 一致；
- mapping geometry 和页数没有溢出；
- 非 guarded 页范围允许合并。

### 8.15.2 邻居发现

- SmallRun 可通过 PageMap 查左末页和右首页面；
- LargeExtentSet 优先通过 address tree predecessor/successor；
- 两种方式结果应在诊断构建交叉校验；
- 邻居必须 free、同 owner、同 region、状态兼容且不处于 detached/scavenge；
- 不跨 DirectMapped、guard 或 hugetlb 边界。

### 8.15.3 Coalesce 提交

1. 获取 owner shard与 PageMap writer 协议；
2. 将释放 extent 标记 `kSplitOrCoalesce`；
3. 从索引摘除合格左/右邻居；
4. 准备 survivor/new descriptor；
5. 计算合并范围并再次验证不溢出；
6. 统一 committed/purged policy；
7. 将结果 range 发布到 survivor/new descriptor；
8. 将旧 descriptor 放入 retire list而非立即复用；
9. 插入 SmallRun bucket 或 LargeExtentSet；
10. 更新 free、active、coalesced 和碎片统计；
11. 释放锁。

### 8.15.4 跨 size tier 合并

合并结果可能从 SmallRun 升入 LargeExtentSet。SmallRun 最大页数不应阻止地址连续空闲区继续合并；否则 128 页上限会永久制造外部碎片。tier 转换必须作为同一 shard transaction 完成。

### 8.15.5 DirectMapped 例外

默认 DirectMapped 不与 region extent 合并，释放时直接 clear + unmap。若未来允许 direct mapping 进入 retained LargeExtentSet，必须在 allocation epoch 开始时明确其 region/backing policy，不能在 free 时临时改变所有权模型。

## 8.16 将 OS 调用移出 shard 锁

### 8.16.1 通用 Detach 模式

```text
lock shard
  -> remove extent from allocatable index
  -> mark kDetachedForPurge/kDetachedForUnmap
  -> record generation and private work item
unlock shard
  -> perform OS syscall
lock shard
  -> validate generation/closing state
  -> publish result or retire descriptor
unlock shard
```

detached extent 不可被 allocate、split 或 coalesce，也不能仍挂在普通 free list。

### 8.16.2 mmap

mmap 返回的是尚未共享的新范围，不需要在 OS 调用期间持 shard 锁。发布时若策略已变化，安全地缓存或 unmap 新范围。

### 8.16.3 madvise

purge 时通常保留 PageMap leaf，但 Span state 必须让其他路径知道它 detached。成功后转 `kFreePurged`，失败则恢复 `kFreeCommitted`。两种结果都重新进入正确索引。

### 8.16.4 munmap

unmap 前必须：

- 从 free/address/size index 移除；
- 撤销 PageMap range；
- 确认没有合法用户对象；
- 将 descriptor 置 detached/retired；
- 释放所有 allocator 锁。

munmap 失败时该 VA 可能仍映射，但逻辑上不能重新交给用户，除非能够证明完整回滚安全。推荐进入 quarantined mapping 统计并触发受控诊断。

### 8.16.5 日志和回调

OS 错误处理不得在 shard 锁内格式化日志，也不得调用用户 OOM 回调。事件先写入固定结构/原子计数，锁外使用 bootstrap-safe 方式报告。

## 8.17 PageMap 集成边界

### 8.17.1 规范来源

PageMap 的 reader 生命周期、writer 串行化、节点只增不减、descriptor retire 和 split/coalesce 发布顺序以[第 5 章](04-pagemap-and-span-lifecycle.md)为准。本节只规定 Backend 在何时调用 range publish/clear，不建立第二套较弱协议。

### 8.17.2 Writer 锁序

推荐：

```text
owner PageCache shard lock
  -> global PageMap writer lock
  -> radix node metadata pool lock（如仍需要）
```

PageMap writer 不反向取得 shard 锁。OS 调用不在任何上述锁内。

### 8.17.3 Range API

建议将逐 Span 操作抽象为：

- `PrepareRange(start, pages)`：预分配可能需要的 radix node；
- `PublishRange(start, pages, span)`；
- `ReplaceRange(start, pages, expected, replacement)`；
- `ClearRange(start, pages, expected)`；
- `ValidateRangeSample(...)`；
- quiescent-only `ResetAll()`。

`expected` 能防止错误 writer 静默覆盖不属于自己的 mapping。

### 8.17.4 Large range 优化

针对数百 MiB/GiB extent，记录：

- leaf stores 数；
- 新建 radix node 数；
- publish/clear cycles；
- PageMap metadata bytes；
- direct allocation 总耗时占比。

只有这些数据证明逐页 leaf 是瓶颈后，才引入 uniform leaf/range marker；普通 `GetSpan` 仍必须固定深度、无锁和无动态分支爆炸。

### 8.17.5 Reset

PageMap 是全局索引，只能在所有 active shard 清空、reader 静止后统一 reset 一次。不得在逐 shard `ResetLocked()` 内重复释放全局 radix pool。

## 8.18 Span/Extent metadata arena

### 8.18.1 当前 ObjectPool 的局限

当前 ObjectPool 解决了递归分配，但：

- `Delete()` 立即把 slot 放回 free list；这只适用于当前由 object ownership pin 住的
  PageMap 调用点，不满足任意无锁 PageMap reader 的延迟回收；
- pool 依赖外层 shard/structure mutex 串行化；
- Span 与大型 extent 冷 metadata 需求不同；
- `ReleaseMemory()` 只适合全局 quiescent reset。

### 8.18.2 推荐分层

- `SpanArena`：每 shard 管理 64B hot Span descriptor；
- `LargeMetadataArena`：requested/alignment/mapping/guard 等冷字段；
- `RegionArena`：固定上限 region descriptor；
- `RadixNodeArena`：PageMap 节点，只增不减直到 quiescent reset；
- `RetireList`：已撤销映射但尚不可复用的 descriptor。

所有 arena 使用 PageAllocator-backed chunk 和 intrusive free/retire list。

### 8.18.3 锁策略

如果 arena 严格 shard-local，可由 shard lock 外部保护，移除内部重复 mutex；PageMap node arena需要独立 writer 锁。若保留内部锁，必须文档化固定锁序并统计 metadata allocation 慢路径。

### 8.18.4 Stable metadata 优先

第一阶段采用[第 5 章](04-pagemap-and-span-lifecycle.md)推荐的 stable metadata：已发布 descriptor 退休后不复用，直到受控 shutdown。只有 metadata RSS 数据证明不可接受，才实现 epoch reclamation。不要为了节省少量 descriptor 立即给 free 路径增加引用计数。

### 8.18.5 Metadata budget

记录：

- live/free/retired Span 数；
- arena mapped/used/wasted bytes；
- split/coalesce descriptor 产生率；
- large side metadata 数；
- radix node bytes；
- 每 region/shard metadata 放大。

## 8.19 无递归的 extent 索引结构

### 8.19.1 禁止项

以下实现即使方便也禁止用于核心 Backend：

- `std::map`/`std::multimap` 的 size/address tree；
- `std::vector` 保存动态 region 或 free extent；
- `std::priority_queue` 保存 purge 候选；
- raw owning `new/delete` 创建 tree node；
- 依赖系统 malloc 的第三方容器。

### 8.19.2 Intrusive hooks

Large extent descriptor 可包含：

```text
size_parent/left/right/color
addr_parent/left/right/color
```

或者把 hooks 放在 PageCache side metadata。Tree 不拥有 descriptor，只维护链接；descriptor ownership 属于对应 arena。

### 8.19.3 复杂度边界

- insert/remove/lower_bound：O(log N)；
- predecessor/successor：O(log N) 或已有节点 O(1) 邻接；
- exact small bucket：O(1)；
- purge 候选：增量 cursor 或分层时间桶；
- diagnostics 全遍历：只在停机/采样路径。

不得使用线性链表加“通常 extent 不多”的假设构建大对象默认路径。

### 8.19.4 Tree 正确性测试

使用固定 seed 随机插入、删除、split、coalesce，并与测试进程中的参考模型比较。参考模型可以在测试代码使用 STL，但 allocator 核心实现不能。

## 8.20 Fit 策略与外部碎片控制

### 8.20.1 Fit 候选

| 策略 | 优点 | 风险 | 推荐用途 |
|---|---|---|---|
| Exact fit | 无 remainder、快 | 命中率有限 | SmallRun 与 LargeExtent 首选 |
| Best fit | 降低即时 remainder | 可能反复制造小碎片 | LargeExtent 默认基线 |
| First fit | 搜索简单 | 地址/大小碎片不稳定 | 有界候选回退 |
| Hugepage-aware fit | 保护完整 hugepage | 可能牺牲少量页 | 推理 workload |
| Decay-aware fit | 优先复用 committed 热页 | 可能降低最佳尺寸匹配 | RSS/延迟平衡 |

### 8.20.2 综合评分

慢路径候选评分可考虑：

- size waste；
- prefix/suffix 数量；
- committed/purged 状态；
- hugepage breakage 增量；
- NUMA locality；
- region occupancy；
- age/decay；
- alignment waste。

只检查固定数量候选，避免锁内复杂全局优化。

### 8.20.3 Coalesce 策略

普通 free 应积极 owner-local coalesce，因为地址索引已可快速定位邻居；但 detached、不同 backing 或不同 region 不合并。对于 hugepage filler 内部 run，先在 filler 粒度合并，只有完整 hugepage 空闲后才提升到上层 HugeRegion。

### 8.20.4 碎片指标

至少报告：

- total free bytes；
- largest free extent；
- `1 - largest_free_extent / total_free_bytes`；
- size histogram；
- split remainder bytes/count；
- failed fit while total free sufficient；
- alignment waste；
- region stranded bytes；
- hugepage broken/partially used bytes。

### 8.20.5 Trace 驱动调优

使用真实 aethermind 请求、模型加载、KV cache 和临时 workspace trace 离线重放，比较 fit 策略。只优化合成均匀随机大小容易得到错误阈值。

## 8.21 Retained VA、Purge 与 Recommit

### 8.21.1 Retained 的价值

保留虚拟地址并 purge 物理页可以：

- 减少 mmap/munmap 和 VMA 变化；
- 保留 region/NUMA/对齐结构；
- 降低 PageMap 节点反复创建；
- 为 hugepage filler/region 提供稳定边界；
- 改善大对象重复申请尾延迟。

代价是 VA 放大、PageMap metadata 常驻和潜在 VMA/地址空间压力。

### 8.21.2 三种释放层级

- **Cache committed**：VA 与物理页候选均保留，最快复用；
- **Purge retained**：`MADV_DONTNEED`，VA 保留，RSS 可下降；
- **Unmap**：撤销 PageMap 和 VA，最彻底回收。

策略由 extent size、age、pressure、region occupancy 和复用率决定。

### 8.21.3 Recommit 语义

匿名 `MADV_DONTNEED` extent 通常可直接重新交付并在首次写时 fault；如果 API 需要确定性预触页，再在锁外执行 `MADV_WILLNEED` 或显式 touch。Recommit 统计需区分：

- logical reuse；
- minor/major faults；
- prefault bytes/time；
- THP collapse latency；
- actual resident growth。

### 8.21.4 VA budget

为 48-bit 和 57-bit 模式分别设置：

- process retained VA soft/hard limit；
- per-node/per-region limit；
- 最大 region 数；
- PageMap metadata limit；
- unmap 优先级。

不能因为 64-bit VA 看似充足而无界保留。

### 8.21.5 与第 9 章边界

本节定义状态和 Backend 操作；具体 dirty/muzzy decay 时间、压力信号、后台扫描预算和 RSS 控制在[第 9 章](08-rss-fragmentation-and-scavenging.md)统一制定。

## 8.22 Hugepage 术语与模式分离

### 8.22.1 四个不同概念

| 概念 | 含义 | 是否保证物理 hugepage |
|---|---|---|
| 2 MiB aligned | 虚拟地址和长度满足 2 MiB 边界 | 否 |
| THP eligible | `MADV_HUGEPAGE` 或系统策略允许 collapse | 否 |
| THP backed | `/proc`、smaps 或内核计数显示实际 AnonHugePages | 是，可能随时 split |
| Explicit hugetlb | `MAP_HUGETLB`/hugetlbfs 等预留大页 | 是，资源和失败语义不同 |

代码、指标和文档必须使用准确术语。

### 8.22.2 当前模式

当前 PageAllocator：

- 对达到 1 MiB 的映射尝试 2 MiB 对齐；
- 通过 `MADV_HUGEPAGE` 提示 THP；
- 对恰好 2 MiB 的 mapping 在 free 时 `MADV_DONTNEED` 并缓存 VMA；
- huge 路径失败时回退普通匿名页。

这属于“THP-friendly aligned anonymous mapping”，不是显式 hugepage allocator。

### 8.22.3 建议策略枚举

```text
PagePolicy::kNormal
PagePolicy::kThpPrefer
PagePolicy::kThpAvoid
PagePolicy::kHugetlbRequire
PagePolicy::kHugetlbPrefer
```

- `kHugetlbRequire` 失败必须返回失败，不能静默降级；
- `kHugetlbPrefer` 才允许回退；
- small metadata、冷页和频繁 purge extent 可用 `kThpAvoid`；
- 默认 malloc 以 availability-first 的 normal/THP prefer 为主。

### 8.22.4 实际 backing 观测

在线热路径不解析 `/proc/self/smaps`。通过低频诊断、`/proc` 汇总、`perf`/内核接口或抽样 `mincore` 等方式测量；对外报告“hint issued”“aligned mapping”“observed THP bytes”三个独立指标。

## 8.23 Hugepage Filler

### 8.23.1 目标

Hugepage filler 将一个 2 MiB 区间划分为多个 small run，使活跃 run 尽量集中在少量 hugepage 内，让完全空闲 hugepage 可以整体 purge/unmap，同时避免随机 SmallRun split 破坏所有 hugepage。

### 8.23.2 Filler metadata

每个 filler hugepage 至少记录：

- hugepage base、owner node/shard；
- 已分配/空闲/已 purge 的 4 KiB 页 bitmap；
- longest free run 或分级空闲提示；
- live run count 和 used pages；
- hugepage broken/eligible/backed 状态；
- last allocation/free epoch；
- intrusive fullness list hooks。

metadata 必须来自专用 arena，不能嵌在用户页中影响对齐或 usable capacity。

### 8.23.3 分配策略

- 优先已有 partially used hugepage，填充空洞；
- 根据所需 run pages 选择可容纳且 remainder 最小的 filler；
- 避免把单个极小长期存活 run 分散到多个 hugepage；
- 对 hot/short-lived workload 使用不同 fullness 策略；
- filler 内部操作由 owner shard/filler lock 保护，不使用逐页 atomic CAS 网络。

### 8.23.4 释放策略

- run free 后更新 filler bitmap；
- 相邻页在 filler 内自然合并；
- hugepage 完全空闲后提升为完整 hugepage cache/region extent；
- 若只有极少活跃页阻止回收，计入 breakage；
- 不迁移 live user objects来“整理”hugepage，除非上层 arena 明确支持 relocation。

### 8.23.5 采用门槛

只有在实际 THP 覆盖率、TLB miss、hugepage breakage 和 aethermind 性能表明收益后启用。Filler 增加 metadata、选择成本和状态复杂度，不应作为第一阶段默认路径。

## 8.24 HugeRegion 与完整 Hugepage Cache

### 8.24.1 HugeRegion

HugeRegion 以 2 MiB 单元管理连续地址范围，适合：

- 模型权重 CPU staging；
- KV cache 大块；
- 推理 workspace；
- 需要 NUMA locality 的大型长期 allocation；
- 多个 hugepage 的普通大对象。

它应支持完整 hugepage 的 split/coalesce，而不是退化为 4 KiB extent tree 后再猜测 huge 边界。

### 8.24.2 完整大页缓存

缓存对象必须区分：

- 2 MiB 对齐 retained VMA；
- 当前实际 THP-backed 区间；
- explicit hugetlb page；
- purged 后仅保留 VA 的区间。

这些资源不能放进同一无类型 cache。显式 hugetlb 必须回到相同 hugetlb pool；普通匿名 VMA 可按 node/policy 分片缓存。

### 8.24.3 当前 lock-free cache 的定位

现有固定 16-slot 双栈 cache 可继续作为“全局 2 MiB retained anonymous VMA cache”的实验基线，但需要：

- 按 NUMA node 或 owner arena 分区；
- 记录 current slots/bytes/high-water；
- 区分 hit 后重新 fault 与真正 resident reuse；
- 在 pressure/shutdown 时可确定性 drain；
- 对 ABA tag wrap、concurrent drain 和 fork 增加测试；
- 不把 cache hit 记作 hugepage backing hit。

### 8.24.4 Hugepage breakage

至少统计：

- total huge-aligned regions；
- observed huge-backed bytes；
- full/partial/empty hugepage 数；
- partial hugepage 内 live/free pages；
- 因少量 live pages 无法整体 purge 的 stranded bytes；
- THP split/collapse 事件（可观测时）；
- filler allocation success/fallback；
- explicit hugetlb success/failure/fallback。

## 8.25 PageAllocator OS 抽象与失败语义

### 8.25.1 从二元接口演进

当前 `SystemAlloc(page_num)`/`SystemFree(ptr, page_num)` 无法表达 retained、NUMA、alignment、page policy 和错误详情。建议内部演进为：

```text
Map(request) -> MappingResult
Unmap(mapping) -> OsResult
Purge(range, mode) -> OsResult
Advise(range, policy) -> OsResult
BindNuma(range, node, policy) -> OsResult
Prefault(range, budget) -> OsResult
```

现有接口可作为 bootstrap/兼容薄封装。

### 8.25.2 MappingResult

返回结构至少包含：

- base、size；
- effective alignment；
- actual mapping flags/policy；
- requested NUMA 与实际 fallback；
- errno/error category；
- 是否经过 over-map/trim；
- 是否获得显式 hugetlb；
- 是否只是 THP hint；
- cleanup responsibility。

结构由值返回或调用方提供 buffer，不分配内存。

### 8.25.3 Retry 策略

当前 ENOMEM 最多重试 3 次并固定 sleep 50 us。应重新评估：

- ENOMEM 在 cgroup/overcommit 下通常不是短暂事件；
- 每次 allocation thread sleep 会直接放大尾延迟；
- 可在首次失败后触发有界 cache purge，再重试一次；
- retry 次数和 backoff 应配置化、可观测；
- 非 ENOMEM 错误通常立即返回；
- `MAP_HUGETLB` 失败是否 fallback 由 page policy 决定。

### 8.25.4 errno 与公共 ABI

PageAllocator 保存原始 errno/category，上层标准 ABI 统一设置 `errno=ENOMEM` 或参数错误。内部成功路径是否保留调用者 errno 由[第 4 章](03-correctness-bootstrap-and-abi.md)规定，不能让 madvise hint 失败意外污染成功的 malloc errno。

### 8.25.5 Bootstrap-safe 诊断

PageAllocator、metadata arena 和 direct-map OOM 路径禁止依赖 spdlog、iostream、`std::string` 或 locale。使用：

- relaxed 原子计数；
- 固定大小错误记录环；
- 必要时 `write(2)` 输出固定文本/整数格式；
- 上层显式拉取诊断。

### 8.25.6 Prefault 与 MAP_POPULATE

`MAP_POPULATE` 可能把缺页成本集中到分配延迟并长时间阻塞。必须：

- 默认关闭；
- 与 THP policy 解耦；
- 对大范围设置单次 prefault byte/time budget；
- 在 aethermind 模型加载与请求热路径分别测试；
- 报告 mmap time、fault time、minor/major faults 和首次访问延迟。

## 8.26 锁序、并发与内存序

### 8.26.1 锁域

| 锁 | 保护内容 | 禁止事项 |
|---|---|---|
| PageCache shard lock | region、bucket、extent index、Span state、refill state | OS syscall、Central lock、动态日志 |
| PageMap writer lock | radix node 安装和 range publish/clear | 反向获取其他 shard、OS syscall |
| Metadata arena lock | 非 shard-local chunk/free/retire 状态 | 用户回调、OS 调用（尽量预分配） |
| Huge cache atomic/lock | 固定 slot ownership | PageMap/extent transaction |
| Scavenger control lock | 线程等待与停止状态 | PageCache 全量扫描期间长期持有 |

### 8.26.2 全局顺序

```text
CentralCache lock（释放后）
  -> PageCache owner shard lock
  -> PageMap writer lock
  -> metadata arena lock（若无法 shard-local）
```

OS 调用发生在锁序之外。Reset 若必须冻结所有 shard，按 shard id 升序获取，并在统一点进入 PageMap reset；普通请求禁止多 shard 同持锁。

### 8.26.3 内存序

- PageMap node/leaf publish：release store；reader：acquire load；
- PageMap root reset：仅 quiescent，可使用明确 relaxed/release 语义并记录前置条件；
- statistics/hints：`memory_order_relaxed`；
- lock-free HugePageCache slot 发布/消费：release/acquire；
- refill/closing 若在锁内访问，保持普通字段；若作为锁外 hint，relaxed load 后必须锁内重验；
- 不用 atomic `Span::IsUsed` 代替 descriptor 生命周期协议。

### 8.26.4 Shard cache-line 布局

PageCacheShard 需隔离：

- mutex/lock state；
- hottest exact-bucket bitmap；
- refill state；
- frequently updated counters；
- cold tree roots和统计快照。

仅对外层类型 `alignas(64)` 不保证内部热点字段不会与相邻冷字段/其他 shard 伪共享；需通过 `sizeof/offsetof` 和 cache-to-cache profiling 验证。

### 8.26.5 Fork 与 shutdown

- atfork prepare 固定顺序冻结 PageAllocator 控制线程和 shard；
- child 重建 refill、Scavenger、huge cache 并发状态；
- shutdown 先停止上层请求和 Scavenger，再 drain cache、清 PageMap、unmap；
- lock-free cache 的 drain 只能在无并发 Put/Get 或具备专门协议时执行；
- placement-new singleton 的有意常驻策略需要明确记录。

## 8.27 NUMA-local Region 与大对象放置

### 8.27.1 路由不等于物理本地性

把请求路由到 node-local PageCache 只能决定逻辑 owner。匿名页的物理 node 通常由 first touch 决定；线程随后迁移可能仍产生远端访问。因此需要同时设计：

- CPU/worker affinity；
- region owner node；
- first-touch 执行线程；
- 可选 `mbind`/NUMA policy；
- cross-node free owner；
- pressure fallback。

### 8.27.2 Node-local Region

- 每个 region 固定 owner node；
- region 内 shard 属于同一 node；
- SmallRun/LargeExtent 优先 node-local fit；
- fallback 到其他 node 时记录实际 node，并让 free 返回实际 owner；
- coalesce 不跨 node region；
- node budget 按 active/resident/retained 分别限制。

### 8.27.3 大对象放置

扩展 API 可支持：

- preferred node；
- interleave policy；
- bind/strict bind；
- first-touch callback/worker；
- model/request/temporary lifetime；
- THP/hugetlb preference。

标准 malloc ABI 不暴露这些策略，继续使用安全默认值。

### 8.27.4 不推荐的做法

- 在进程级临时调用 `set_mempolicy` 后 mmap，再恢复；并发线程可能观察到错误策略；
- 每次分配迁移已有物理页；
- 释放线程决定 extent 新 owner；
- 活跃 region 在 node 间迁移；
- NUMA 不可用时返回失败而没有默认 arena 回退。

### 8.27.5 验证

使用固定 CPU/socket affinity，采集：

- local/remote pages；
- numa faults/migrations；
- remote LLC/内存访问；
- node-local hit/fallback；
- model load time、tokens/s、请求 p99；
- 每 node active/resident/retained bytes。

## 8.28 可观测性、守恒与诊断

### 8.28.1 每 shard/region 指标

- alloc request/hit/miss pages；
- exact hit、split hit、large fit、OS refill；
- release、left/right/both coalesce；
- SmallRun bucket occupancy；
- LargeExtent count/bytes/largest extent；
- direct map/unmap count/bytes；
- committed/purged/retained bytes；
- refill single-flight collision；
- shard lock wait/hold samples；
- PageMap publish/clear pages和 cycles；
- metadata live/free/retired bytes；
- NUMA local/fallback。

### 8.28.2 OS 指标

- mmap/munmap/madvise 调用、字节和 latency histogram；
- ENOMEM/其他 errno；
- retry/purge-before-retry；
- over-map/trim head/tail waste；
- MAP_POPULATE/prefault bytes/time；
- VMA cache hit/miss；
- munmap/purge failure quarantine bytes。

### 8.28.3 大对象指标

- requested/usable/mapped bytes；
- internal alignment/page-rounding waste；
- cached/direct allocation counts；
- realloc in-place/move/failure；
- lifetime/size histogram（采样）；
- direct PageMap publish cost；
- guard/hugetlb/THP policy 分类。

### 8.28.4 Hugepage 指标

- aligned mapping bytes；
- THP hint success/failure；
- observed THP bytes；
- explicit hugetlb success/failure/fallback；
- full/partial/empty hugepage；
- breakage/stranded bytes；
- filler hit/miss；
- retained VMA hit 与 resident reuse 分离。

### 8.28.5 Backend 守恒快照

quiescent diagnostics 应验证：

- 所有 free extent 在 address space 中无重叠；
- SmallRun、LargeExtent 和 DirectMapped 集合互斥；
- size/address 双索引节点一致；
- owner/region/PageMap 一致；
- active 与 free/retained 字节总和等于 region accounting；
- PageMap leaf 不指向已复用/释放 metadata；
- purged/committed/retained 状态与索引一致；
- metadata arena live + free + retired 守恒。

### 8.28.6 统计开销

- PageMap `GetSpan` 不新增共享统计写；
- shard 慢路径使用 shard-local relaxed counter；
- syscall latency 直接在 Backend 慢路径计时；
- 大型直方图采样更新；
- snapshot/export 由调用方提供 buffer，不在 allocator 内构造 JSON string；
- 统计开关 on/off 分别跑整体性能护栏。

## 8.29 测试、故障注入与性能基准

### 8.29.1 SmallRun/PageCache 单元测试

- exact hit 1、边界页数和最大 SmallRun；
- non-empty bitmap 与 bucket 一致；
- committed/purged exact hit；
- head/tail/alignment split；
- 左、右、双侧 coalesce；
- SmallRun 合并升级 LargeExtent；
- region 边界禁止合并；
- owner shard 在首次 PageMap 发布前正确；
- page count、地址和所有算术溢出。

### 8.29.2 多 shard 测试

- 显式启用 2～4 shard，确认每个 shard 实际获得 allocation；
- refill remainder 与 allocated Span owner 一致；
- release 回到实际 owner；
- 相邻但不同 region/shard 不合并；
- 多 shard 并发安装 PageMap node；
- shard closing/refill publish 竞争；
- reset 只统一释放一次 PageMap；
- Scavenger 遍历所有 active shard；
- TSan 下无 writer race。

### 8.29.3 LargeExtent/DirectMapped 测试

- size/address tree 随机模型验证；
- exact/best-fit 和 bounded candidate；
- prefix/suffix/双 remainder；
- direct threshold 上下边界；
- requested/usable/mapping size；
- over-aligned user pointer 与 original mapping free；
- realloc shrink/grow/move/OOM；
- gigabyte-scale PageMap range 的首尾和随机页；
- interior pointer、double free、stale descriptor 的 hardening 行为。

### 8.29.4 Retained/Hugepage 测试

- committed -> purged -> reuse；
- purge failure 回滚；
- retained VA budget 超限转 unmap；
- exact 2 MiB VMA cache capacity/concurrent drain；
- THP hint 与 actual backing 指标不混淆；
- hugetlb require/prefer 两种失败语义；
- filler full/partial/empty 和 breakage；
- NUMA policy fallback；
- fork 后 cache/refill 状态恢复。

### 8.29.5 故障注入

在每个事务提交点注入：

- Span/LargeMetadata/RadixNode arena OOM；
- mmap ENOMEM/非 ENOMEM；
- alignment head/tail munmap 失败；
- PageMap prepare/publish 失败；
- refill publish 时 shard closing；
- madvise/mbind/prefault 失败；
- direct unmap 失败；
- hugetlb unavailable；
- pressure generation 变化。

失败后运行守恒快照，确保旧 extent 仍完整，或新 mapping 被明确清理/quarantine。

### 8.29.6 Backend 专项基准

建议包括：

- `BM_PageCache_ExactHit/{1,8,32,128}`；
- `BM_PageCache_Split/{small,aligned,large}`；
- `BM_PageCache_Coalesce/{left,right,both,tier-cross}`；
- `BM_PageCache_Refill_Cold`；
- `BM_PageCache_SameShard_Contention`；
- `BM_PageCache_MultiShard_Contention`；
- `BM_LargeExtent_Exact/BestFit/Split`；
- `BM_DirectMap/{1MiB,2MiB,16MiB,1GiB}`；
- `BM_PageMap_PublishRange/...`；
- `BM_Retained_Reuse/Purged_Reuse`；
- `BM_HugepageFiller/...`；
- `BM_Numa_LocalRemote/...`。

分别报告 ns/op、ns/page、cycles、lock wait/hold、syscalls、minor faults、VMA count、RSS/VA 和碎片。

### 8.29.7 端到端门禁

- 继续运行 3.8 ns 单线程 fast path；
- 随机大小约 26.0 ns；
- 16 线程 64B 约 8.9 us 和 100+ GiB/s；
- Deep Churn 和 Central/PageCache contention；
- 大对象随机分配/释放/realloc；
- aethermind model load、steady inference、KV cache churn；
- 固定编译器、affinity、governor、THP、NUMA 和 cgroup 环境；
- 保存 benchmark JSON、perf、smaps/RSS 与 allocator stats。

## 8.30 分阶段实施与验收

### 阶段 A：Backend 正确性与 owner 基线

实施内容：

1. 固化 BE 不变量和统一 Span/Extent 状态；
2. 将 owner shard 作为内部 allocation/refill 的显式输入；
3. 保证 owner 在第一次 PageMap 发布前确定；
4. 建立 PageMap writer 协议并引用[第 5 章](04-pagemap-and-span-lifecycle.md)的 stable metadata；
5. 区分 PageCache per-shard reset 与全局 PageMap reset；
6. 增加 2～4 shard 实际分配、release、split、coalesce 测试；
7. 建立 Backend 守恒快照和事务故障注入；
8. 建立 PageCache/large/direct 专项基准基线。

退出条件：

- non-zero shard 的 allocated/remainder/free Span owner、list 和 PageMap 始终一致；
- 多 shard writer 无 radix node 泄漏、覆盖或 data race；
- split/coalesce OOM 保持失败原子；
- descriptor 不在无锁 reader 仍可能访问时复用；
- reset/shutdown 只在 quiescent 下统一清理；
- ASan/UBSan/TSan 和整体性能护栏通过。

风险类型：正确性、并发、内存、性能。

### 阶段 B：锁外 OS 事务与大对象语义

实施内容：

1. 引入 refill single-flight 和 prepare/OS/publish 三阶段事务；
2. 将 mmap、munmap、madvise 移出 shard lock；
3. 解耦 SmallRun 上限、refill 大小和 direct threshold；
4. 增加 large-only metadata：requested、usable、alignment、mapping geometry；
5. 完善 over-aligned、calloc、realloc 与 direct free；
6. 将 PageAllocator 失败诊断改为 bootstrap-safe；
7. 增加 syscall latency、PageMap range 和大对象指标。

退出条件：

- shard lock hold histogram 中不再包含 OS syscall；
- publish 竞争、OOM 和 syscall failure 无 mapping/descriptor 泄漏；
- 大对象 alignment、usable size、realloc 失败语义符合 ABI；
- direct map/unmap 与 PageMap 清理顺序通过并发测试；
- PageCache contention p99 获得可量化改善；
- 小对象 fast path 和现有吞吐护栏无退化。

风险类型：正确性、并发、内存、性能、兼容性。

### 阶段 C：LargeExtentSet、Region 与碎片治理

实施内容：

1. 引入无递归 intrusive size/address 双索引；
2. SmallRun 合并结果可提升 LargeExtentSet；
3. exact/best-fit、alignment-aware split 和碎片指标；
4. 建立 region descriptor 与稳定 owner；
5. retained VA、purged reuse 和 VA budget；
6. 每 shard/node 增量 purge/unmap；
7. 用真实 aethermind trace 调优 threshold 和 fit policy。

退出条件：

- 双索引随机模型和守恒测试长期通过；
- 中大型重复 workload 的 mmap/munmap 次数和尾延迟显著下降；
- total free 足够却无法 fit 的比例下降；
- region retained VA、PageMap metadata 和 RSS 均受预算约束；
- 不出现跨 region/shard 合并；
- 端到端 RSS、p99 和吞吐综合不劣于阶段 B。

风险类型：正确性、内存、性能、并发。

### 阶段 D：Hugepage、NUMA 与 aethermind 灰度

实施内容：

1. 精确区分 aligned、THP eligible/backed 和 explicit hugetlb；
2. 按 node 分片完整 2 MiB retained VMA/hugepage cache；
3. 实验 HugepageFiller 和 HugeRegion；
4. 实现 normal/THP/hugetlb page policy；
5. node-local region、first-touch 和可选 mbind；
6. 为 model/KV/workspace 提供扩展 arena API；
7. 在 aethermind 固定 worker、跨 socket 和压力场景分阶段灰度。

退出条件：

- observed THP/hugetlb 指标证明真实 backing 收益，而非仅对齐/hint；
- hugepage breakage、stranded bytes 和 filler fallback 受控；
- NUMA local access、TLB miss、tokens/s 或请求 p99 获得明确收益；
- hugetlb require/prefer 失败语义和普通页 fallback 正确；
- model load、steady inference、KV churn 的 RSS/VA 无不可接受放大；
- 所有新模式具有构建期或运行时快速回退到普通 region/PageCache 的能力。

风险类型：并发、内存、性能、兼容性、运维。
