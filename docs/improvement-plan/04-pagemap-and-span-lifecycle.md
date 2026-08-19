# 第 5 章：PageMap 与 Span 生命周期

> [总索引](README.md) · [上一章](03-correctness-bootstrap-and-abi.md) · [下一章](05-frontend.md)  
> **本章目标**：建立无锁 PageMap reader 可依赖的稳定 Span 生命周期与发布协议。  
> **适用范围**：RadixTree、PageMap、Span descriptor、owner shard 与回收协议。  
> **核心 invariant**：GetSpan 无锁；写路径串行化；读者可见 descriptor 不得过早结束生命周期或复用。

PageMap 是普通 `free`、跨线程释放、Span 拆分/合并、大对象回收和 `malloc_usable_size` 的基础索引。它必须同时满足两个看似矛盾的目标：读取路径不加锁，写入和回收路径仍然保持严格的对象生命周期安全。

本节的核心结论是：**atomic leaf 的 acquire/release 只能保证指针发布前的初始化可见性，不能保证 leaf 指向的 Span 在读取后继续存活。** PageMap 的正确设计必须同时包含发布协议、写者串行化协议和 Span 延迟回收协议。

## 5.1 职责、术语与所有权边界

### 5.1.1 PageMap 的职责

PageMap 负责将任意受管 CPU heap 地址转换为描述其所属页区间的 Span：

```text
user pointer
  -> page id
  -> PageMap radix lookup
  -> Span descriptor
  -> allocation kind / size class / owner shard / usable size
```

PageMap 应负责：

- 从 page id 查找当前负责该页的 Span；
- 为新 Span 发布连续的 `page_id -> Span*` 映射；
- 在 split、coalesce、unmap 时原子地切换逻辑所有者；
- 拒绝超出受支持虚拟地址位数的 page id；
- 为 `free`、`realloc`、`malloc_usable_size` 和诊断路径提供稳定查询。

PageMap 不应负责：

- 分配或释放用户对象；
- 决定 ThreadCache/CentralCache 策略；
- 直接拥有 Span descriptor；
- 在无锁读路径中修复不一致映射；
- 猜测未知指针是否属于 libc、GPU 或其他 allocator domain。

### 5.1.2 四类所有权

必须区分以下四类所有权，避免用“Span 属于 PageCache”掩盖不同生命周期：

| 所有权类型 | 所有者 | 含义 |
|---|---|---|
| Descriptor ownership | PageCache shard 的 metadata arena/pool | 决定谁创建、retire 和最终销毁 Span descriptor |
| Address-range ownership | 某个 Span descriptor | 决定一段虚拟页当前由哪个 Span 描述 |
| Page backing ownership | PageAllocator/PageCache extent | 决定虚拟映射和物理 backing 的保留、purge、unmap |
| Object ownership | 用户或 Frontend/Middle-end cache | 决定小对象当前是 live、缓存中还是已回 Span bitmap |

CentralCache 从 PageCache 借用一个小对象 Span 时，不取得 descriptor ownership。它只在该 Span 的小对象分配生命周期内维护 bitmap 和 `use_count`。PageCache 仍是 descriptor 的最终所有者，但只有当所有对象都已回到 Span bitmap，CentralCache 才能把 Span 返还 PageCache。

### 5.1.3 借用指针

`PageMap::GetSpan()` 返回的是 borrowed pointer，不转移所有权。借用必须有明确有效期：

- **当前实现隐含契约**：调用者读取 leaf 后立即访问 Span，但没有显式 read-side guard；该契约不足以支持 descriptor 立即复用。
- **第一阶段推荐契约**：已经发布的 descriptor 在 allocator 正常运行期间保持稳定地址，不单独回收；borrow 在一次 API 调用内天然安全。
- **未来 epoch 契约**：调用者进入 PageMap read-side critical section，退出前 descriptor 不得回收。

在没有 stable metadata 或 epoch guard 之前，任何 `ObjectPool::Delete(span)` 都不能被视为安全的无锁读者回收协议。

## 5.2 核心生命周期不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 说明 |
|---|---|---|
| PM-1 | `GetSpan` 不获取 PageCache、PageMap 或 metadata pool 锁 | 保持普通 free 的无锁查询路径 |
| PM-2 | 每个已映射 page leaf 在任一时刻最多指向一个逻辑 owner Span | 禁止重叠 Span 同时拥有同一页 |
| PM-3 | leaf 发布前 Span 的身份字段和本 allocation epoch 的分类字段已完整初始化 | acquire reader 才能看到完整描述 |
| PM-4 | leaf 指向的 descriptor 在所有潜在读者退出前不得结束生命周期或被复用 | 解决 UAF/ABA，而非只解决可见性 |
| PM-5 | RadixNode 在正常并发运行期间只增不减 | 读者不需要保护中间节点生命周期 |
| PM-6 | 所有 PageMap 写入由明确的 writer 协议串行化 | 分片锁本身不足以保护全局 radix tree |
| PM-7 | Span 的 owner shard 在发布前确定，并在整个 descriptor allocation epoch 内不变 | release 必须能回到唯一 shard |
| PM-8 | 一个 Span 同时最多属于一个侵入式 SpanList | 避免重复 erase、链表环和跨桶污染 |
| PM-9 | free-list bucket 索引与 `span->page_num` 一致 | split/coalesce 后必须先更新字段再入正确桶 |
| PM-10 | 小对象 Span 只有在 `use_count == 0` 且无缓存对象时才能返还 PageCache | 防止 ThreadCache/TransferCache 悬垂指针 |
| PM-11 | unmap 前先撤销对即将失效地址范围的 PageMap 映射 | 防止 free 查到已解除映射的内存 |
| PM-12 | ClearRange 不等同于 descriptor 可立即回收 | 仍可能有读者已经取得旧指针 |
| PM-13 | split/coalesce 的部分失败不改变已发布所有权 | 保持失败原子性 |
| PM-14 | Detached Scavenger Span 既不在 free list，也不可被分配或合并 | 锁外 madvise 期间保持独占 |
| PM-15 | Reset/Destroy 只能在已证明无并发读写者的静默期执行 | 测试清理不能冒充运行时安全操作 |

建议为 Span 增加 Debug-only list owner/bucket 标记，并在入链、出链和状态转换时校验 PM-8、PM-9。生产构建中保留低成本的边界和 owner 检查。

## 5.3 Span 状态机

### 5.3.1 推荐状态

当前 `used/committed` 两个标志不足以表达 CentralCache 借用、Scavenger 摘除和 descriptor retire 等状态。建议引入概念上的显式状态机：

```cpp
enum class SpanState : uint8_t {
    kFresh = 0,
    kFreeCommitted,
    kFreePurged,
    kInUseSmall,
    kInUseLarge,
    kDetachedScavenging,
    kCoalescing,
    kRetired,
};
```

该枚举不一定必须在第一版直接作为原子字段实现，但设计、断言和测试必须按这些状态理解 Span 生命周期。

### 5.3.2 状态定义

| 状态 | 所属容器/域 | 允许的主要字段 | 合法操作 |
|---|---|---|---|
| `kFresh` | metadata owner 私有，尚未发布 | identity 正在初始化 | 初始化、发布或失败回收 |
| `kFreeCommitted` | PageCache shard 的 page-count bucket | 无小对象 bitmap 所有权，物理页仍 committed | exact hit、split、coalesce、scavenge |
| `kFreePurged` | PageCache shard 的 page-count bucket | 虚拟地址保留，物理页已 purge | exact hit、split、coalesce、重新触页 |
| `kInUseSmall` | CentralCache bucket 借用 | size class、bitmap、capacity、use_count 有效 | object alloc/free、归还 PageCache |
| `kInUseLarge` | 用户大对象 | page range 和 usable size 有效 | free、realloc grow/shrink |
| `kDetachedScavenging` | Scavenger 私有临时链 | 不在 PageCache bucket，地址仍映射 | madvise、失败回滚、重新入桶 |
| `kCoalescing` | PageCache shard 临界区私有 | 正在组合地址范围 | 邻居摘除、映射切换、旧 descriptor retire |
| `kRetired` | metadata retire list | 不再拥有页，不得重新发布 | 等待进程结束或 epoch 安全回收 |

### 5.3.3 主要转换

```text
kFresh
  +--> kFreeCommitted       OS refill 后先作为 free extent 发布
  +--> kInUseLarge          大对象直接分配并发布

kFreeCommitted / kFreePurged
  +--> kInUseSmall          CentralCache 取得并初始化
  +--> kInUseLarge          大页级请求取得
  +--> kCoalescing          与相邻 free Span 合并
  +--> kDetachedScavenging  后台摘除并 purge

kInUseSmall
  +--> kFreeCommitted       use_count 归零并交回 PageCache

kInUseLarge
  +--> kFreeCommitted       可缓存页数范围内释放
  +--> kRetired             direct mapping 清图/unmap 后 descriptor retire

kDetachedScavenging
  +--> kFreePurged          madvise 成功
  +--> kFreeCommitted       madvise 失败

kCoalescing
  +--> kFreeCommitted       合并结果入桶
  +--> kRetired             被吸收的旧 descriptor
```

### 5.3.4 禁止转换

- `kInUseSmall -> kRetired`：只要 ThreadCache/TransferCache/用户仍可能持有对象指针，就不能回收 descriptor。
- `kDetachedScavenging -> kInUse*`：必须先回 shard bucket，再由正常分配路径取得。
- `kRetired -> kFresh`：在 stable metadata 方案中 descriptor 不复用；epoch 方案也必须增加 generation 后才能复用。
- `kInUseLarge -> kInUseSmall`：同一个 allocation epoch 内不能直接重新解释字段。
- `kFreePurged -> unmap` 同时保留 PageMap leaf：解除映射前必须先撤销或切换映射。

## 5.4 Span 字段分类与访问协议

### 5.4.1 身份字段

建议将以下字段视为 descriptor allocation epoch 内的身份：

- `start_page_idx`；
- `page_num`；
- `owner_shard_id`；
- `generation`；
- allocator domain/region id。

其中 `start_page_idx/page_num` 在 split/coalesce 时会变化，因此更准确的规则是：**每次地址范围变化都创建新的映射版本，旧身份在发布切换后进入 retired 状态。** 第一阶段为减少 descriptor 数量，可以允许 shard 锁内修改 survivor Span，但必须保证：

1. 读者不会把同一个 descriptor 的旧地址范围与新字段混合使用；
2. 旧 leaf 在 descriptor 字段改变前已经受 writer 协议保护地切换；
3. descriptor 不被立即复用；
4. PageMap 查询结果经过必要的一致性验证。

从可证明性角度，推荐 split/coalesce 尽可能使用“新 descriptor 发布、旧 descriptor retire”，把 identity 变更转化为版本替换。

### 5.4.2 小对象分类字段

以下字段只在 `kInUseSmall` 有效：

- `size_class_idx`；
- `aligned_obj_size`；
- `capacity`；
- `obj_offset`；
- bitmap 地址/长度；
- `use_count`；
- `scan_cursor`。

发布规则：

- CentralCache 在 Span 尚未进入其 `span_list` 前完成布局初始化；
- PageMap leaf 若已指向该 Span，则普通 free 只有在对象已经交给用户后才会读取分类字段；
- 对象发布给 ThreadCache/用户之前，分类字段必须对该线程可见；
- `aligned_obj_size` 在整个 `kInUseSmall` epoch 内不可改变；
- `capacity/obj_offset` 初始化后不可改变；
- bitmap、`use_count` 和 `scan_cursor` 由对应 CentralCache bucket lock 保护。

### 5.4.3 回收字段

以下字段描述物理 backing 与回收策略：

- committed/purged 状态；
- `last_used_time_ms`；
- decay generation；
- purge failure/retry 信息。

这些字段由 PageCache shard lock 或 Detached Scavenger 独占权保护。Scavenger 锁外执行 madvise 时，只有持有 `kDetachedScavenging` 状态的线程可以修改 committed 状态。

### 5.4.4 建议的字段访问表

| 字段组 | 正常读者 | 写者保护 | 是否允许无锁读取 |
|---|---|---|---|
| owner/domain/generation | `free`、PageCache | 发布前初始化；之后稳定 | 是，前提是 descriptor 生命周期稳定 |
| start/page count | PageMap 诊断、PageCache | shard + PageMap writer 事务 | 仅在版本稳定协议下 |
| size class/object size | `free`、CentralCache | CentralCache 发布前初始化；epoch 内只读 | 是，前提是小对象 epoch 未结束 |
| capacity/offset | CentralCache、诊断 | 初始化后不可变 | 是 |
| bitmap/use_count/cursor | CentralCache | bucket mutex | 否，除非仅作近似统计 |
| state/list links | PageCache/CentralCache/Scavenger | 对应状态 owner 的锁 | 否 |
| committed/last-used | PageCache/Scavenger | shard lock 或 detached 独占 | 统计可近似无锁读，决策不可 |

## 5.5 PageMap 四层基数树

### 5.5.1 地址分解

设 OS page shift 为 `PAGE_SHIFT=12`，page id 位数为：

```text
PAGE_ID_BITS = VA_BITS - PAGE_SHIFT
```

当前四层结构包含一个胖 root 和三个 9-bit RadixNode 层：

```text
page_id
  [root bits][9 bits][9 bits][9 bits]
      i0        i1      i2      i3
```

- 48-bit VA：page id 为 36 bit，root 使用 9 bit；
- 57-bit VA：page id 为 45 bit，root 使用 18 bit；
- 每个普通 RadixNode 包含 512 个 atomic child；
- leaf 保存 `Span*`，中间层保存 `RadixNode*`。

必须通过编译期断言验证：

- `RADIX_ROOT_BITS + 3 * RADIX_NODE_BITS == PAGE_ID_BITS`；
- root/node 数组大小不会发生位移溢出；
- `page_id` 高于支持范围时 Get/Set/Clear 均明确拒绝；
- 57-bit 构建的胖 root 内存开销经过预算评估。

### 5.5.2 中间节点生命周期

RadixRoot 使用静态存储。普通 RadixNode 由独立 metadata pool 分配，并遵循：

- 构造时将所有 child relaxed 初始化为空；
- 节点完全初始化后通过 release store/CAS 发布；
- reader 使用 acquire load 获取中间节点；
- 正常运行期间不删除单个节点；
- 仅在全局静默、PageMap 不再有读者时统一释放节点池。

中间节点“只增不减”会产生 metadata 增长，因此必须统计节点数量、覆盖地址范围和每 GiB managed memory 的 radix metadata 成本，但不能为了减少这点开销破坏无锁读者安全。

### 5.5.3 Leaf 语义

leaf 指针表示“该 page 当前逻辑上属于哪个 Span descriptor”，而不是单纯表示物理页是否 committed：

- `kFreePurged` Span 仍保留 leaf，便于相邻合并和后续复用；
- direct mapping unmap 后 leaf 必须为空；
- split 后 allocated 和 remainder 页分别指向对应 descriptor；
- coalesce 后整个合并区间指向 survivor/new descriptor；
- detached Scavenger Span 仍可以保留 leaf，但其 state 阻止分配和合并。

## 5.6 无锁读取契约

### 5.6.1 Acquire 链

典型读取路径为：

```text
acquire root
  -> acquire level-1 node
  -> acquire level-2 node
  -> acquire level-3 node
  -> acquire Span leaf
```

每个 acquire 与写者对相应 child 的 release 发布配对，保证 reader 看到节点构造和 Span 发布前写入的初始化数据。

### 5.6.2 可见性不等于生命周期

以下代码即使所有 atomic 内存序都正确，仍可能发生 UAF：

```text
Reader                          Writer
------                          ------
span = leaf.load(acquire)
                                leaf.store(nullptr, release)
                                pool.Delete(span)
read span->aligned_obj_size
```

acquire 只保证 reader 看到 writer 在最初发布 Span 前的写入；它不能阻止另一个 writer 清除 leaf 后结束对象生命周期，也不能阻止对象池在相同地址构造另一个 Span，从而形成 ABA。

因此无锁读必须配合以下至少一种机制：

- descriptor 正常运行期间不回收、不复用；
- epoch/read-side critical section；
- hazard pointer；
- 其他具有严格证明的延迟回收协议。

### 5.6.3 Lookup 结果验证

在 Hardened 模式中，`GetSpan(ptr)` 后建议验证：

- descriptor state 是允许 free/usable-size 的状态；
- `ptr_page` 位于 `[start_page_idx, start_page_idx + page_num)`；
- domain/generation 合法；
- 小对象指针位于 data area 且满足 object boundary；
- 大对象普通 free 只接受 allocation base。

默认模式可以减少部分检查，但 descriptor 生命周期和地址范围一致性不能省略。

## 5.7 PageMap 多写者协议

### 5.7.1 当前单写者边界

当前生产默认只使用 shard 0，因此 shard mutex 在事实上串行化 PageMap 写入。但这个性质来自路由策略，而不是 PageMap 自身。只要多个 PageCache shard 可以并发调用 `SetSpan/ClearRange`，分片锁便不再构成全局 writer lock。

### 5.7.2 丢失节点安装风险

两个 writer 对同一个空 child 执行以下序列时可能丢失更新：

```text
Writer A                        Writer B
load child == null              load child == null
allocate node A                 allocate node B
store node A                    store node B
publish leaves under node A     publish leaves under node B
```

最终 root 只指向 node B，node A 及其 leaves 不可达。这会造成映射缺失和 metadata 泄漏。

### 5.7.3 候选方案

| 方案 | 正确性复杂度 | 写入并发 | 读路径影响 | 建议 |
|---|---:|---:|---:|---|
| 全局 PageMap writer mutex | 低 | 中低 | 无 | 第一阶段推荐 |
| root/层级分段 writer lock | 中 | 中高 | 无 | 写入成为瓶颈后评估 |
| child CAS 安装 + loser 回收/保留 | 中高 | 高 | 无 | 可作为后续优化 |
| 完全 lock-free writer | 很高 | 高 | 可能增加元数据协议 | 暂不采用 |

### 5.7.4 推荐锁顺序

第一阶段建议使用独立 PageMap writer mutex，并固定顺序：

```text
CentralCache bucket lock
  - 禁止持有时进入 PageCache

PageCache shard lock（跨 shard 时按 shard id 升序）
  -> PageMap writer lock
     -> RadixNode metadata pool lock
```

说明：

- 正常 PageCache 操作持有一个 owner shard lock，再取得 PageMap writer lock；
- PageMap writer lock 不反向请求 shard lock；
- Reset 如需锁定多个 shard，先按 id 升序取得所有 shard，再取得 PageMap writer lock；
- OS mmap/munmap/madvise 尽量不在这些锁内执行；
- CentralCache 释放空 Span 时必须先释放 bucket lock，再进入 PageCache。

如果未来采用 CAS 安装，中间节点 loser 也不能立即回 pool，除非证明没有读者看到它；最简单的做法是把 loser node 保留到 pool/region 最终销毁。

## 5.8 Span descriptor 回收方案比较

| 方案 | 热路径开销 | 回收及时性 | metadata 开销 | 实现/证明复杂度 |
|---|---:|---:|---:|---:|
| 已发布 descriptor 永不单独复用 | 最低 | 进程/region 结束 | 中高 | 最低 |
| Region-lifetime metadata arena | 最低 | region 释放时 | 中 | 低 |
| Epoch-based reclamation | 每次 API 少量 reader 标记 | 延迟但可持续回收 | 低中 | 中高 |
| Hazard pointer | 每次 lookup 发布 hazard | 较及时 | 中 | 高 |
| Span 原子引用计数 | 每次 lookup/free 共享原子写 | 及时 | 低 | 中，但性能风险高 |

推荐路线：

1. 先实现 region-lifetime/stable descriptor；
2. 统计 `metadata_bytes/managed_bytes`、retired descriptor 数和峰值；
3. 只有 metadata 放大超过目标阈值，才设计 epoch；
4. 不把引用计数放入普通 free 热路径。

## 5.9 推荐的稳定元数据方案

### 5.9.1 Metadata arena

每个 PageCache shard/region 拥有独立 metadata arena：

- arena 使用 PageAllocator raw mapping 获取 chunk；
- Span descriptor 地址在 arena 生命周期内稳定；
- 新 descriptor 从 bump area 或“从未发布过”的 free slot 取得；
- 一旦 descriptor 发布到 PageMap，retire 后不返回普通可复用 free list；
- region 最终销毁且全局无读者时统一释放整个 arena。

这与普通 ObjectPool 的关键差异是：`Delete()` 不代表已发布 Span slot 可以立刻被 `New()` 重用。

### 5.9.2 Retire list

被 split/coalesce 吸收或 direct-unmap 的 descriptor 进入 owner shard 的 retire list：

```text
active/free Span
  -> PageMap 不再引用它
  -> state = Retired
  -> append retire list
  -> process/region teardown 才释放 storage
```

retired descriptor 应保留：

- generation；
- 原地址范围或诊断摘要；
- retire reason；
- 可选 retire epoch；
- Hardened 模式 poison state。

### 5.9.3 Generation

即使第一阶段不复用 descriptor，也建议引入单调 generation 用于：

- 检测 stale Span 引用；
- 诊断同一地址范围的 split/coalesce 历史；
- 为未来 epoch 复用建立 ABA 防护；
- 在测试中确认 leaf 切换到预期版本。

generation 不应仅使用容易短期回绕的 8/16-bit 字段。若受 64B Span 尺寸限制，可以使用 32-bit generation 并明确回绕策略，或把冷诊断字段移入 side metadata。

### 5.9.4 内存预算

必须持续统计：

- live Span descriptor 数；
- retired descriptor 数；
- metadata arena mapped/resident bytes；
- RadixNode 数；
- 每 GiB active/mapped heap 对应的 metadata bytes；
- split/coalesce 产生 descriptor 的速率。

如果稳定 descriptor 产生过高开销，优先减少不必要的 descriptor 创建或按 region 批量管理，而不是直接牺牲生命周期安全。

## 5.10 Epoch 延迟回收备选设计

Epoch 只作为第二阶段候选，本节用于规定未来评审边界。

### 5.10.1 Reader 协议

每个可能调用 PageMap 的线程维护 read-side 状态：

```text
EnterPageMapRead()
  -> publish active + observed global epoch
  -> lookup and consume Span fields
ExitPageMapRead()
  -> publish inactive
```

要求：

- enter/exit 不分配、不加全局锁；
- ThreadCache TLS 退出会从 epoch registry 安全注销；
- 嵌套查询使用深度计数，不能过早退出；
- free、usable-size、realloc 和 PageCache 无锁邻居查询都必须纳入协议；
- reader 不能在 read-side section 内阻塞或执行长系统调用。

### 5.10.2 Writer 协议

writer 从 PageMap 撤销旧 descriptor 后：

1. 记录当前 global epoch；
2. 将 descriptor 放入 per-shard retire queue；
3. 周期性推进 epoch；
4. 确认所有在 retire epoch 前进入的 reader 已退出；
5. 才允许 destructor 和 slot 复用。

### 5.10.3 生命周期交互

- Scavenger 不需要 retire descriptor，但其 PageMap/Span 读取仍必须遵循状态锁。
- Thread 退出必须标记 epoch slot inactive，防止永久阻塞回收。
- `fork()` child 必须重建只包含当前线程的 epoch registry。
- shutdown 必须等待所有 reader inactive 后清空 retire queue。
- 信号处理器若可进入 allocator，必须单独解决嵌套 epoch 和异步安全问题。

### 5.10.4 启用门禁

引入 epoch 前必须证明：

- stable metadata 已成为显著内存问题；
- reader enter/exit 对 fast-path 基准的影响可接受；
- TSan 和模型化测试覆盖 unregister、epoch rollover 和 stalled reader；
- 回收收益大于额外 TLS、registry 和 retire queue 开销。

## 5.11 PageMap 更新事务

所有映射变更统一采用以下逻辑模型：

```text
Prepare
  -> 在共享结构外申请 descriptor/node/OS mapping
Lock
  -> 取得 owner shard + PageMap writer lock
Validate
  -> 验证旧 leaf、状态、owner、邻居和 bucket membership
Publish
  -> 更新 leaf 和共享状态
Retire
  -> 旧 descriptor 进入延迟回收
Unlock
  -> 释放 writer/shard lock
Cleanup
  -> 锁外处理可安全释放的局部资源
```

### 5.11.1 `SetSpan`

前置条件：

- Span 非空；
- `page_num > 0`；
- `start + page_num` checked arithmetic 成功；
- owner shard 已确定；
- descriptor 处于可发布状态；
- writer 协议已持有。

发布顺序：

1. 预创建所有可能缺失的 RadixNode；
2. 初始化 descriptor 的身份和分类字段；
3. 通过 release CAS/store 安装中间节点；
4. 对连续 leaf 执行 release store；
5. 更新 Span state/list membership；
6. 对外返回或发布对象指针。

如果中间节点分配失败，不能只发布区间的一部分。应在写 leaf 前完成节点准备，或记录已经发布范围并具备严格回滚。推荐“先准备全部路径，再发布 leaf”。

### 5.11.2 `ClearRange`

前置条件与行为：

- 使用 checked end 计算，拒绝 page count wrap；
- 只清 leaf，不删除中间节点；
- writer lock 下验证 leaf 指向预期 descriptor，避免清掉后来重用的范围；
- 支持 `ClearRangeExpected(start, count, expected_span, generation)` 形式；
- release store null 只撤销映射，不授权 descriptor 立即复用。

### 5.11.3 批量发布一致性

连续多个 leaf 的 atomic store 无法让读者看到“全区间瞬时原子切换”。在发布循环期间，读者可能看到新旧映射混合。设计必须保证每一个中间状态都安全：

- 新旧 descriptor 在整个切换期间都保持存活；
- 每个 leaf 始终指向一个能合法描述该 page 的 descriptor；
- 不能先修改旧 descriptor 的范围，使尚未切换的 leaf 指向不再覆盖该页的 descriptor；
- 必要时使用新 descriptor 表示最终范围，待所有 leaf 切换后再 retire 旧 descriptor。

这也是推荐“版本化 descriptor”而非原地修改 identity 的主要原因。

## 5.12 Span 分配与拆分事务

### 5.12.1 Exact bucket hit

```text
lock owner shard
  -> 从 bucket[page_num] 摘除 Span
  -> 验证 state 为 FreeCommitted/FreePurged
  -> 验证 page_num 和 bucket 一致
  -> 转为 InUseLarge 或交给 CentralCache 初始化
  -> 保持 PageMap leaf 指向同一稳定 descriptor
unlock
```

如果 free Span 已 purged，重新分配时不需要先主动 commit；第一次写入会由内核重新提供物理页。state/统计应反映该转换，避免把 purged bytes 继续计为可回收 RSS。

### 5.12.2 从大 Span 拆分

推荐使用两个最终 descriptor：allocated Span 与 remainder Span。事务为：

1. 在共享结构外准备 allocated descriptor；
2. 取得 owner shard 和 PageMap writer lock；
3. 验证 big Span 仍位于预期 bucket、状态 free 且大小足够；
4. 从原 bucket 摘除 big Span；
5. 计算 allocated/remainder 范围并检查所有算术；
6. 初始化 allocated descriptor；
7. 将 big Span 作为 remainder survivor，或为 remainder 创建新 descriptor；
8. 在整个 leaf 切换期间保持原 big descriptor 有效且覆盖旧区间；
9. 先发布 allocated 范围的新 leaf；
10. 更新 remainder descriptor identity 并发布 remainder leaf；
11. remainder 入对应 bucket；
12. allocated 转入使用状态；
13. 不再拥有页的旧 descriptor 进入 retire list。

更容易证明的方案是为 allocated 和 remainder 都创建新 descriptor，原 big Span 在全部 leaf 切换后 retire。这样 metadata 稍多，但不会出现原地缩小 big Span 时旧 leaf 瞬间指向“不覆盖该页”的 descriptor。

### 5.12.3 Metadata OOM

- descriptor 分配应在摘除 big Span 前完成；
- 分配失败直接返回 null，原 big Span 和 PageMap 不变；
- 不能在 split 一半后因第二个 descriptor OOM 留下部分映射；
- 如果采用 survivor 方案，只需一个新 descriptor，但仍必须满足批量发布安全性。

### 5.12.4 OS refill

OS refill 应尽量锁外进行：

1. shard 锁内确认没有可用 Span；
2. 解锁并向 PageAllocator 申请 region；
3. 准备 descriptor 和 RadixNode；
4. 重新取得 shard/writer lock；
5. 再次检查 bucket，若其他线程已补货，可将新 region 作为额外 free extent 发布，而不是丢弃；
6. 完整发布 PageMap 后入桶；
7. 重试 exact/split。

## 5.13 Span 释放与合并事务

### 5.13.1 释放前验证

PageCache 接收 Span 前必须验证：

- descriptor 属于当前 allocator domain；
- owner shard id 合法；
- state 是 `kInUseSmall` 或 `kInUseLarge` 的合法返还状态；
- 小对象 Span 的 `use_count == 0`；
- Span 不在任何链表；
- PageMap 对其地址范围仍指向该 descriptor/generation；
- page range 算术没有 wrap。

### 5.13.2 邻居资格

左右邻居只有同时满足以下条件才能合并：

- PageMap 查到非空 descriptor；
- state 是 `kFreeCommitted` 或 `kFreePurged`；
- owner shard 与当前 Span 相同；
- 地址严格相邻而不是仅 leaf 恰好指向同一对象；
- 邻居确实位于对应 page-count bucket；
- 合并大小不超过当前缓存/region 策略限制；
- committed/purged 状态组合有定义。

owner-shard-local 是硬约束。不同 shard 的相邻 extent 不应在释放热路径中跨锁合并；正确的 region ownership 应从地址分配阶段避免这种边界碎片。

### 5.13.3 合并发布顺序

推荐事务：

1. 取得 owner shard 和 PageMap writer lock；
2. 将释放 Span 标记为 `kCoalescing`，但尚不入桶；
3. 查询并验证左邻居；
4. 从左邻居 bucket 摘除并标记 `kCoalescing`；
5. 查询并验证右邻居；
6. 从右邻居 bucket 摘除并标记 `kCoalescing`；
7. 准备一个覆盖最终范围的新/survivor descriptor；
8. 将最终范围的每个 leaf 切换到结果 descriptor；
9. 只有在 leaf 完成切换后，才把被吸收 descriptor 标记 retired；
10. 结果 Span 清除小对象字段，设置正确 committed/purged 策略；
11. 结果 Span 进入对应 bucket 并转为 free state。

### 5.13.4 Committed 状态合并

若一个邻居 purged、另一个 committed，合并结果不能用单个 bool 精确描述逐页物理 backing。可选策略：

- 第一阶段把合并结果视为 committed，统计上保守，不影响正确性；
- 维护区间级 purge bitmap，复杂度较高；
- 合并前/后将整个结果区间统一 madvise，系统调用应在安全 detached 状态下锁外进行。

建议第一阶段使用保守状态并把精细物理页状态留给 hugepage/extent backend 重构。

### 5.13.5 禁止立即 `ObjectPool::Delete`

被左/右合并吸收的 descriptor 即使 leaf 已全部切换，也可能仍被早先的 reader 持有。因此它们必须进入 retire list，不能立即回到 ObjectPool free list。

## 5.14 小对象 Span 生命周期

### 5.14.1 从 PageCache 到 CentralCache

```text
PageCache Free Span
  -> CentralCache 请求 page_num
  -> PageCache 返回稳定 descriptor
  -> Span::Init(size_class)
  -> 初始化 bitmap/capacity/offset/use_count
  -> state = InUseSmall
  -> 插入对应 CentralCache bucket SpanList
```

初始化应在对其他 CentralCache 线程可见前完成。若 PageMap leaf 早已指向该 descriptor，合法用户 free 仍不会发生，因为尚未有对象被发布；但是错误指针查询可能看到过渡状态，因此 Hardened 检查需要明确识别 `kFresh/kCoalescing`。

### 5.14.2 `use_count` 的准确语义

当前 bitmap 模型中，bit cleared 表示对象已从 Span bitmap 取出。对象可能位于：

- 用户手中；
- ThreadCache FreeList；
- TransferCache；
- CentralCache 批量提取的临时数组。

因此 `use_count` 更准确的名称是 `objects_out_of_span_bitmap`，而不是仅指“用户当前 live allocation”。对象从用户释放到 ThreadCache 或 TransferCache 时不递减；只有最终回到 Span bitmap 时才递减。

这一语义保证：只要任何 Frontend/Middle-end 仍持有该 Span 的对象指针，`use_count` 就不会归零，Span 也不能返还 PageCache。

### 5.14.3 TransferCache 不变量

- TransferCache 中每个指针对应 Span bitmap 中一个 cleared bit；
- 从 TransferCache 取出交给 ThreadCache 不改变 `use_count`；
- ThreadCache 归还到 TransferCache 不改变 `use_count`；
- 只有 TransferCache overflow 或 Reset 把对象真正回 bitmap 时递减；
- CentralCache Reset 必须先恢复所有 TransferCache object 的 bitmap 所有权，再释放空 Span；
- TransferCache backing 的 clear 与 destroy 必须分离，避免测试绕过真实路径。

### 5.14.4 Span 归还 PageCache

在 CentralCache bucket lock 下处理对象回 bitmap。当 `use_count` 变为零：

1. 从 CentralCache SpanList 摘除；
2. 记录 descriptor 和必要状态；
3. 释放 bucket lock；
4. 调用 PageCache 返还 owner shard；
5. PageCache 清除小对象分类字段并进入 coalesce/free 流程。

禁止持 CentralCache bucket lock 调用 PageCache，以免与 refill 路径形成锁顺序反转。

### 5.14.5 Double free 与跨线程 free

- 跨线程 free 是合法场景，对象进入释放线程的 Frontend 或 owner-aware remote queue；
- 两个线程并发 free 同一对象属于用户错误，但 Hardened 模式应在对象最终回 bitmap 或更早阶段检测；
- 仅在 bitmap 层检测可能被 ThreadCache/TransferCache 延迟，应考虑 sampled state/cookie；
- 无论用户错误如何，descriptor 生命周期协议不能导致 PageMap UAF。

## 5.15 大对象 Span 生命周期

### 5.15.1 分配

大于 Frontend 上限的请求按 checked round-up 转为 page count：

1. 验证 `size + page_size - 1` 不溢出；
2. 从 PageCache small-run bucket、LargeExtentSet 或 direct mapping 获取区间；
3. 初始化 `kInUseLarge` descriptor；
4. 记录 requested/usable size、alignment 和 owner domain；
5. 完整发布 PageMap；
6. 返回 allocation base。

不建议继续用 `aligned_obj_size == 0` 作为大对象的唯一身份标志。应使用显式 SpanState/allocation kind，防止字段清理或过渡状态被误判为大对象。

### 5.15.2 释放

普通大对象 free 只接受 allocation base。事务：

- lookup stable descriptor；
- 验证 state/domain/base；
- 进入 owner shard；
- cacheable extent 转为 free/coalesce；
- direct mapping 先通过 expected-span ClearRange 撤销 leaf；
- 经过 read-side grace period 后 descriptor 才可回收；
- OS unmap 可以在映射撤销和状态隔离后锁外执行，但必须防止地址被错误重用。

### 5.15.3 Realloc

大对象原地扩展需要：

- 检查右邻居是否 free、同 owner 且足够大；
- 在 shard/writer 事务中摘除或拆分右邻居；
- 发布扩展范围 leaf；
- 最后更新用户可见 usable size；
- 失败时原 allocation 完全不变。

原地收缩需要：

- 生成 tail remainder descriptor；
- 将 tail leaf 从原 Span 切换到 remainder；
- 原 descriptor 的 usable range 更新必须与 leaf 切换协议一致；
- remainder 进入 PageCache 或直接 madvise/unmap。

第一阶段优先采用 allocate-copy-free，待事务模型稳定后再实现原地 grow/shrink。

## 5.16 Scavenger 的 Detached 生命周期

Scavenger 使用“摘除—回收物理页—归还”三阶段协议：

```text
shard lock
  -> 从 free bucket 摘除候选
  -> state = DetachedScavenging
unlock
  -> madvise(MADV_DONTNEED)
lock same owner shard
  -> success: FreePurged
  -> failure: FreeCommitted
  -> 更新时间并回原 page-count bucket
unlock
```

关键不变量：

- Detached Span 不在任何 PageCache bucket；
- state 阻止其他线程把它视为可合并邻居；
- PageMap leaf 可以继续指向 descriptor，因为虚拟地址仍保留；
- Scavenger 私有临时链不能覆盖 descriptor 的 PageCache/CentralCache list link 而破坏状态；建议提供独立临时链接或严格保证完全摘除；
- madvise 失败必须恢复 `kFreeCommitted`；
- 归还必须使用原 owner shard，而不是默认 shard 0；
- shutdown 必须等待所有 Detached Span 回归或进入受控终止状态。

多 shard 实现应逐 shard 扫描，每次只持一把 shard lock。不能依赖 `GetSpanList/GetMutex` 的 shard-0 legacy 接口。

## 5.17 Reset、Purge 与最终销毁

当前一个 `Reset()` 容易混淆不同语义，建议拆分：

| 操作 | 并发条件 | 行为 | 是否可继续使用 runtime |
|---|---|---|---|
| `ClearCachesForTest` | 测试线程独占，所有 worker 停止 | drain Frontend/Middle-end，保留 backing | 是 |
| `PurgeFreePages` | 正常运行，可与受控分配并发 | madvise free extent，不清 PageMap metadata | 是 |
| `ReleaseUnusedExtents` | 明确 writer 协议 | unmap 完全 free 的大 extent，retire descriptor | 是 |
| `DestroyRuntimeForTest` | 全局静默，无任何读写者 | 清 PageMap、释放 RadixNode/metadata arena | 否 |
| 进程退出 | OS 接管 | 停止后台线程，可让核心 metadata 常驻 | 不适用 |

### 5.17.1 静默期证明

最终销毁前必须满足：

- allocator state 已阻止新正常入口；
- Scavenger 已停止并 join；
- 所有 ThreadCache/CPU cache 已 drain 或线程已终止；
- 所有 PageMap reader 已退出；
- 所有 shard writer 已退出；
- CentralCache 不再持有 Span；
- retire queue 已达到安全回收条件。

仅仅依次取得每个 shard mutex 不足以证明无锁 PageMap reader 已退出。

### 5.17.2 Root 重用

静态 root 在测试 Reset 后若要重新使用：

- 必须处于全局静默；
- 先让 root 对新 reader 不可见；
- 等待旧 reader 退出；
- 才能清空 root child 和释放 RadixNode pool；
- 下一次初始化完整构造后再 release 发布 root。

## 5.18 非法 free、边界校验与 generation

### 5.18.1 小对象边界

Hardened 模式下，从 Span 恢复对象索引时验证：

```text
data_base <= ptr < data_base + capacity * object_size
(ptr - data_base) % object_size == 0
object_index < capacity
bitmap bit 当前为 allocated/out-of-bitmap
```

检查和减 `use_count` 必须在同一个 CentralCache bucket lock 临界区内完成。

### 5.18.2 大对象边界

- 普通 free 要求 `ptr == Span::GetPageBaseAddr()` 或记录的 aligned user base；
- aligned large allocation 需要从 user pointer 恢复原 allocation base；
- interior pointer 不得因为 PageMap 能找到 Span 就被当作合法 free；
- `malloc_usable_size` 同样只接受合法 allocation pointer。

### 5.18.3 Generation 校验

PageMap leaf 若只保存裸 `Span*`，generation 不能直接与 leaf 原子一致读取。可选方案：

- descriptor 不复用时，generation 主要用于诊断；
- leaf 保存 tagged pointer/index + generation；
- PageMap side table 保存稳定 descriptor id；
- epoch 安全回收后仍在 descriptor 内检查 generation，但需防 ABA。

第一阶段不复用 descriptor，可以避免把 tagged pointer 引入热路径；未来复用前必须重新评审 ABA 方案。

## 5.19 锁顺序与死锁规约

### 5.19.1 锁域

- Frontend：线程/CPU 私有，不持共享锁；
- TransferCache：每 bucket SpinLock；
- Central SpanList：每 bucket mutex；
- PageCache：每 shard mutex；
- PageMap：独立 writer mutex，reader 无锁；
- metadata arena：内部 pool lock；
- Scavenger wait state：独立 mutex，不与 PageCache 锁嵌套等待。

### 5.19.2 强制顺序

```text
CentralCache bucket lock
  -> 不得进入 PageCache；需要时先 unlock

PageCache shard lock(s), shard id ascending
  -> PageMap writer lock
     -> PageMap RadixNode pool lock

PageCache shard lock
  -> owner Span metadata arena lock（若仍需要独立锁）
```

禁止：

- 持 PageMap writer lock 再取得未持有的 shard lock；
- 持 CentralCache bucket lock 调用 PageCache；
- 持 Scavenger wait mutex 等待 PageCache 操作；
- 持 shard lock 执行可阻塞的 mmap/munmap/madvise，除非当前阶段为保证正确性暂时接受并有明确基准；
- 未按 shard id 排序获取多 shard 锁。

### 5.19.3 锁断言

建议在 Debug 构建增加轻量 lock-rank 检查，记录当前线程持有的最高锁等级。它只用于测试和开发，不进入 Release 热路径。

## 5.20 故障注入与测试矩阵

### 5.20.1 PageMap 基础测试

- 48-bit 和 57-bit 各层索引边界；
- root first/last entry；
- 每个 RadixNode 边界跨越；
- unknown address miss；
- SetSpan/ClearRange 全区间；
- page id/end range overflow；
- purged Span 仍可 lookup；
- unmap 后 leaf 为空。

### 5.20.2 Split/coalesce 测试

- exact hit 不改变无关 leaf；
- split 的 allocated/remainder 每页映射正确；
- 左合并、右合并、左右同时合并；
- 达到最大合并大小时停止；
- 跨 owner shard 不合并；
- committed + purged 状态组合；
- 被吸收 descriptor 进入 retire list而不是立即复用；
- 任一 metadata 分配失败时原 Span/list/PageMap 不变。

### 5.20.3 并发测试

- 多 reader 与同范围 leaf 切换；
- reader 在 load leaf 后暂停，writer clear/retire，再恢复 reader；
- 多 writer 并发安装相同中间节点；
- 不同 shard 并发 SetSpan/ClearRange；
- CentralCache 归还空 Span与 PageCache 分配并发；
- Scavenger detached 与相邻 Span release 并发；
- Reset/Destroy 只在静默期成功；
- TLS 退出与 epoch/retire queue 并发。

### 5.20.4 故障注入

- RadixNode pool OOM；
- Span descriptor arena OOM；
- OS refill OOM；
- split 准备阶段失败；
- madvise 失败；
- munmap 失败；
- writer 在批量 leaf 发布中途被调度暂停；
- epoch reader 长时间停滞；
- generation 接近回绕。

### 5.20.5 工具与方法

- ASan 检查 descriptor 和用户内存 UAF；
- UBSan 检查 page id 位移、范围计算和对齐；
- TSan 检查状态字段、root reset 和 Scavenger；
- 模型化测试枚举 split/coalesce/reader interleaving；
- 固定种子随机 Span 操作序列并在每步验证 PageMap 全区间；
- 长时间 cross-thread free 与 PageCache churn；
- Debug invariant walker 验证“bucket/list/PageMap/owner”四方一致。

## 5.21 可观测性与诊断

建议增加以下低成本或慢路径统计：

- `pagemap_lookup_hit/miss`；
- 每层 RadixNode 数量；
- PageMap writer lock wait/hold time；
- `set_span_pages/clear_range_pages`；
- live/retired Span descriptor 数；
- split/coalesce 次数和页数；
- owner-shard mismatch 拒绝次数；
- detached Scavenger Span 数和停留时间；
- metadata mapped/resident bytes；
- epoch retire queue 长度和最大 grace period；
- invariant failure 分类。

统计读取接口应允许近似快照，不能为了全局一致统计阻塞所有 PageMap reader。Debug 工具可以在显式静默期输出完整地址范围和 descriptor 状态。

## 5.22 性能与内存护栏

### 5.22.1 Reader 基准

必须持续跟踪：

- `PageMap::GetSpan` hit/miss/mixed 延迟；
- 普通 `am_free` 中 PageMap 占比；
- sized free 跳过 PageMap 后的收益；
- 48-bit 与 57-bit root 对 cache/TLB 的影响；
- stable metadata、epoch reader enter/exit 的额外指令；
- 不同访问分布下的 L1/LLC/TLB miss。

### 5.22.2 Writer 基准

- 单页与多页 SetSpan；
- ClearRange；
- exact bucket、split、coalesce；
- 1/2/4/8/16 shard 并发建图；
- writer mutex contention；
- CAS 节点安装与全局 writer lock 的比较；
- OS 系统调用移出锁前后的 p99。

### 5.22.3 Metadata 护栏

建议初始目标以实测确定，但至少报告：

```text
metadata amplification = metadata resident bytes / active user bytes
retired ratio          = retired descriptors / all descriptors
radix density          = mapped leaves / radix leaf capacity
```

任何回收优化必须同时给出 fast-path 延迟、metadata RSS、UAF 证明和复杂度变化，不能只报告减少了多少 descriptor。

## 5.23 分阶段实施与验收

### 阶段 A：建立稳定生命周期基线

实施内容：

1. 定义显式 SpanState 和字段有效期；
2. 已发布 descriptor 改为 stable/retire，不立即回 ObjectPool；
3. 引入 generation 和 Debug list/bucket invariant；
4. `ClearRangeExpected` 验证 expected descriptor；
5. 分离 Reset/Purge/Destroy；
6. 补充 reader-pause/retire 回归测试。

退出条件：

- PageMap reader 不可能访问已析构或已复用 descriptor；
- RadixNode 正常运行期间永不回收；
- ASan/TSan 和并发生命周期测试通过；
- metadata 开销已可观测。

风险类型：正确性、并发、内存。

### 阶段 B：事务化 split/coalesce

实施内容：

1. 将 split/coalesce 重写为 prepare/validate/publish/retire；
2. 避免原地 identity 修改造成混合 leaf；
3. OS refill 移出 shard 锁；
4. 建立失败注入和全区间 invariant walker；
5. 明确 committed/purged 合并策略。

退出条件：

- 任意中途失败保持原 PageMap 和 bucket 状态；
- 每个 leaf 在发布过渡期间始终指向合法、存活 descriptor；
- split/coalesce 聚焦基准无不可接受退化。

风险类型：正确性、并发、性能。

### 阶段 C：支持真正的多 shard writer

实施内容：

1. 引入独立 PageMap writer mutex；
2. 固化 shard/writer/pool 锁顺序；
3. owner shard 在首次发布前确定；
4. Scavenger 改为逐 shard；
5. 实现真实 shard 路由和 region ownership；
6. 增加并发 node install 和 writer contention 基准。

退出条件：

- 多 shard SetSpan/ClearRange 无丢失节点或映射；
- owner-shard-local coalesce 始终成立；
- 16 线程 PageCache 扩展性优于 shard-0 基线；
- writer mutex 尚未成为主要瓶颈，或已得到量化。

风险类型：并发、性能、内存。

### 阶段 D：按数据决定是否引入 epoch

触发条件：

- retired/stable descriptor 的 RSS 已成为目标 workload 的显著开销；
- descriptor 创建速率无法通过 region 和 split 策略降低；
- 有完整的 reader 注册、TLS 退出、fork 和 shutdown 设计。

退出条件：

- epoch reader 开销满足 fast-path 护栏；
- stalled reader、线程退出和 epoch rollover 测试通过；
- metadata RSS 的实际收益显著高于复杂度和延迟成本；
- 仍保留可切回 stable metadata 的构建选项，便于诊断和回滚。

风险类型：并发、内存、性能。

