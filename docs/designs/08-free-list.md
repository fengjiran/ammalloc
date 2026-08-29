# FreeList 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.0
- **日期**: 2026-08-21
- **关联代码**: [include/ammalloc/free_list.h](../../include/ammalloc/free_list.h)（全部实现位于头文件：`FreeBlock`/`FreeChain`/`FreeList`，无 `.cpp` 实现文件）
- **上游依赖**: 仅依赖 [assert.h](../../include/ammalloc/assert.h)、[attributes.h](../../include/ammalloc/attributes.h)、[config.h](../../include/ammalloc/config.h) 的定义与标准库，无 ammalloc 模块依赖（叶子原语）
- **下游消费者**: [ThreadCache](../../include/ammalloc/thread_cache.h)（每尺寸类别持有一个 `FreeList`）、[CentralCache](../../include/ammalloc/central_cache.h)（`FetchRange` 通过 `push_range` 回填对象）
- **关联测试**: [tests/unit/test_free_list.cpp](../../tests/unit/test_free_list.cpp)
- **架构总览**: [ammalloc_design.md §4.1](ammalloc_design.md)

## 1. 背景与目标

`FreeList` 是分配器前端（ThreadCache）最底层的空闲对象容器，承接快路径的绝大多数分配与释放请求。

- **为什么存在**：ThreadCache 需要按尺寸类别保存空闲对象，而每次进出都必须满足纳秒级延迟的高性能基线（单线程极速路径约 3.8 ns，见架构总览）。
- **量化目标**：单次 `push`/`pop` 为 O(1) 常数时间、零元数据堆分配、零系统调用；对象链通过复用空闲对象自身内存实现，不引入任何独立元数据节点。

## 2. 职责与边界

- **提供**：嵌入式 LIFO 空闲对象链，及其上的单对象 (`push`/`pop`) 与批量 (`push_range`/`pop_range`) 操作；同时承载每类的配额状态（`max_size_`/`overages_`），供 ThreadCache 慢启动与超配衰减使用。
- **不做**：不负责对象切分、跨线程均衡、页管理；不持有/释放 Span 或页。
- **所有权**：`FreeList` 持有的对象是分配器系统的借用对象，所有权始终属于分配器系统，不再属于调用线程或释放线程。
- **线程模型**：`FreeList` 非线程安全。ThreadCache 内的实例线程私有（唯一 mutator 是拥有线程）；CentralCache 的 `FetchRange` 接收 `FreeList&` 时，由调用方持有的桶锁提供外部同步。

## 3. 关键数据结构

### 3.1 类型一览

| 类型 | 含义 | 关键点 |
|---|---|---|
| `FreeBlock` | 嵌入式链接节点，仅含一个 `FreeBlock* next` | 存储在空闲对象体内，`static_assert(sizeof(FreeBlock) <= SystemConfig::ALIGNMENT)` 保证能塞进最小槽位 |
| `FreeChain` | 从 FreeList 摘下的独立对象链（`head`/`tail`/`count`） | 用于批量流转，O(1) 拷贝进出 |
| `FreeList` | LIFO 空闲链 + 每类配额状态 | 见下表成员 |

### 3.2 FreeList 成员

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| `head_` (`FreeBlock*`) | 嵌入式链头 | 仅所属线程读写（CentralCache 回填时由桶锁保护） |
| `size_` (`size_t`) | 当前缓存对象数 | 与 `head_` 强一致：`head_ == nullptr` 当且仅当 `size_ == 0` |
| `max_size_` (`size_t`) | ThreadCache 高水位配额，初值 1 | 慢启动增长 / 超配批量衰减；`set_max_size` 钳到至少 1 |
| `overages_` (`size_t`) | 连续溢出裁剪计数 | 作为配额衰减信号；refill 成功时由 ThreadCache 清零 |

## 4. 并发模型

- **无锁、无原子**：`FreeList` 不含任何 `std::atomic`，也没有加锁逻辑；全部状态通过普通成员读写，正确性完全依赖"线程私有/外部加锁"的所有权契约。
- **TLS 使用范围**：ThreadCache 以 `std::array<FreeList, SizeClass::kNumSizeClasses> free_lists_` 持有全部实例，线程受限；避免共享链头的锁、原子与 Cache line bouncing。
- **跨线程间接访问**：CentralCache::`FetchRange(FreeList&)` 仅在桶锁（快路径 SpinLock / 慢路径 `std::mutex`）保护下向该 `FreeList` 回填对象，因此不需要 `FreeList` 自身做同步。
- **生命周期**：`FreeList` 作为 ThreadCache 的成员随 TLS 一起销毁；销毁前 ThreadCache 通过 `ReleaseAll` 把所有实例中的对象批量归还 CentralCache，确保没有悬空对象。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `empty` | `bool empty() const noexcept` | `head_ == nullptr` | ✅ |
| `size` | `size_t size() const noexcept` | 当前对象数 | ✅ |
| `push` | `void push(void* ptr) noexcept` | `nullptr` 直接忽略；`block->next = head_` 后前插，`++size_` | ✅ |
| `push_range` | `void push_range(const FreeChain& chain) noexcept` | `head`/`tail`/`count` 任一为空/零则拒绝；debug 下校验链长与尾可达；`tail->next = head_` 后整体前插，`size_ += count` | ❌ |
| `pop_range` | `FreeChain pop_range(size_t n) noexcept` | 摘取链头最多 `n` 个，保持原序；产出链被终结（`tail->next == nullptr`），不足时摘取全部；O(n) | ❌ |
| `pop_range_tail` | `FreeChain pop_range_tail(size_t n) noexcept` | 摘取链尾最多 `n` 个（驱逐最旧、保留链头最新对象供本地复用）；`n >= size_` 时整链摘取；**O(`size_`) 遍历**，非 O(n) | ❌ |
| `pop` | `void* pop() noexcept` | 空表返回 `nullptr`；对下一节点发预取；`head_ = head_->next`，`--size_` | ✅ |
| `max_size` / `set_max_size` | `size_t () const` / `void (size_t n)` | 读/写高水位；`set_max_size` debug 下要求 `n >= 1`，并钳到至少 1 | ❌ |
| `overages` / `set_overages` | `size_t () const` / `void (size_t n)` | 读/写连续溢出计数 | ❌ |

## 6. 算法与流程

### 6.1 单对象入链 `push(ptr)`

```
block->next = head_; head_ = block; ++size_;
```

- 复用对象体前 `sizeof(FreeBlock)` 字节存 `next`，不分配元数据。
- `nullptr` 走 `AM_UNLIKELY` 分支提前返回，不进主序。

### 6.2 单对象出链 `pop()`

```
block = head_;
if (block->next) prefetch(block->next);   // 隐藏下一跳延迟
head_ = head_->next; --size_;
```

- 命中率高的下一对象预取（`AM_BUILTIN_PREFETCH`，参数 `(addr, 0, 3)`），隐藏链表遍历的内存延迟。
- 空表走 `AM_UNLIKELY` 分支返回 `nullptr`。

### 6.3 批量出链 `pop_range(n)`

沿 `head_` 前进最多 `n` 步，记录 `tail` 与 `count`；随后 `head_` 指向剩余节点、`size_ -= count`，并把产出链尾的 `next` 置 `nullptr` 终结，避免遍历者误入仍留在链上的对象。

### 6.4 批量入链 `push_range(chain)`

debug 下调用静态私有助手 `CountChain(head, tail)` 校验链长与尾可达（不一致则 `AM_DCHECK` 中止），随后 `tail->next = head_`、`head_ = chain.head`、`size_ += chain.count`。

### 6.5 尾部批量出链 `pop_range_tail(n)`

取 `pop_count = min(n, size_)`、`keep = size_ - pop_count`，保留区间 `[0, keep)`、驱逐后缀 `[keep, size_)`：先沿链走 `keep - 1` 步定位保留尾并断链，再走完后缀求 `tail`。两趟合计 ≈ `size_` 步**串行依赖的指针 load**，所以代价由**链深**决定而非 `n`。`keep == 0`（整链驱逐）时第一趟为零步，退化成单趟。

语义目标是"驱逐最旧、留住最新"，与 `pop_range` 的表头摘取互补，专供 ThreadCache 溢出 trim 使用（见 [02-thread-cache.md](02-thread-cache.md) §6.3）。

### 6.6 复杂度与不变量

- `push` / `pop`：O(1)。
- `push_range`：O(1)（`CountChain` 仅存在于 debug 构建）。
- `pop_range`：O(n)，`n` 为摘取数量，在 ThreadCache 中受 `batch` 有界。
- `pop_range_tail`：**O(`size_`)，与 `n` 无关，不受 `batch` 有界**。ThreadCache 调用点处 `size_ == max_size + 1`，而配额上限是 `kMaxQuotaBatches × batch`，因此该遍历的代价上限由**配额策略参数**隐式决定：16B/64B 类当前为 `8 × 512 + 1 = 4097` 步。
- 实测（本机 x86_64 / `-O2`，真实 16B 对象、64 KiB 足迹、链深 4097）：单次 trim 事件 ≈ 8.2 µs，摊销 ≈ 16 ns/free。由于跳数 / `batch` 恒为 `kMaxQuotaBatches`，摊销成本近似等于 `8 × 单跳依赖延迟`，与尺寸类别无关；`batch` 更小的中间类别（如 256B，`batch=128`）反而更差。
- 无隐藏 O(N²)：每次事件只扫一遍链，不随事件次数累积。
- 核心不变量（debug 下经 `AM_DCHECK` 校验）：`head_ == nullptr` 当且仅当 `size_ == 0`；`pop_range` / `pop_range_tail` 产出链尾 `next == nullptr`。

## 7. 边界条件与错误处理

- `push(nullptr)` / `push_range` 空链（`head`/`tail`/`count` 任一为空/零）：静默忽略，状态不变。
- `pop()` 空表、`pop_range(0)`：返回空（`nullptr` / 空链），状态不变。
- `pop_range(n)` 当链上对象少于 `n`：摘取全部，`count < n` 属正常行为，非错误。
- `set_max_size(0)`：debug 下 `AM_DCHECK` 中止；release 下钳到 1，避免配额归零导致 refill 永久失败。
- 链结构损坏（`push_range` 的 `count` 与链长不符 / 尾不可达）：debug 下 `AM_DCHECK` 中止，错误信息匹配 `"Check failed"`（对应 death test）。

## 8. 风险与权衡

- **线程受限的代价**：无锁快路径建立在"线程私有"之上；代价是线程退出时需 `ReleaseAll` 全量归还，且跨线程 `free` 会使对象滞留在非分配线程的 FreeList 中，由 CentralCache 水位线与 trim 机制平衡（见 02-thread-cache.md §6）。
- **嵌入式节点的前提**：`next` 必须塞进最小对象槽，依赖 `static_assert(sizeof(FreeBlock) <= SystemConfig::ALIGNMENT)` 与 SizeClass 的最小类对齐保证，二者必须始终一致。
- **LIFO 顺序**：刻意采用 LIFO 以最大化 L1/L2 命中率，代价是迭代顺序不确定，因此 `pop_range` 必须显式终结链尾，防止遍历越界。

## 9. 测试要点

关联套件 `FreeListTest`（[tests/unit/test_free_list.cpp](../../tests/unit/test_free_list.cpp)）：

- 空输入语义：`PushNullIsIgnored`、`PopOnEmptyReturnsNull`、`PushRangeRejectsNull`、`PushRangeRejectsZeroCount`、`PopRangeOnEmptyList`
- 链语义：`PopRangeReturnsOrderedChain`、`PopRangePartialOnShortList`、`PopRangeKeepsRemainder`、`PopRangePushRangeRoundTrip`
- 配额计数：`SetAndGetOverages`
- debug 中止：`PushRangeCountMismatchDeath`、`PushRangeEndUnreachableDeath`（`#ifndef NDEBUG` 守卫，匹配 `"Check failed"`）

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| 2026-08-21 | 初版（FreeList 独立成文，自 02-thread-cache.md 拆分细化） | FreeList 拥有独立头文件与独立测试套件，单列模块设计 | — |
| 2026-08-28 | 接口表补 `pop_range_tail`、新增 §6.5 算法小节；§6.6 逐项改写复杂度并给出实测值 | 该成员此前无文档，导致 `pop_range` 的 `batch` 上界被误推广到 O(`size_`) 的尾部遍历 | S-3 |
