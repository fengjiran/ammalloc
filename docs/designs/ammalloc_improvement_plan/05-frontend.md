# 第 6 章：Frontend 提升

> [总索引](README.md) · [上一章](04-pagemap-and-span-lifecycle.md) · [下一章](06-middle-end.md)  
> **本章目标**：在保持无锁 LIFO 快路径的前提下提升容量治理、生命周期和可扩展性。  
> **适用范围**：ThreadCache、FreeList、TLS、batch refill/release、remote free 与可选 per-CPU cache。  
> **核心 invariant**：ThreadCache 热路径无锁、O(1)、LIFO，且不引入共享写和分配器递归。

Frontend 是 ammalloc 中直接承载普通小对象分配和释放的最热层。它的目标不是缓存尽可能多的对象，而是在严格内存预算内，让绝大多数请求只访问线程或 CPU 本地状态，同时将 refill、trim、GC、跨线程归还和策略调整移入可控慢路径。

本节采用渐进路线：第一阶段把 per-thread ThreadCache 做到生命周期可靠、预算可控和可观测；第二阶段加入 sized free、增量 GC 与 remote free；只有这些机制稳定后，才实验 per-CPU/rseq Frontend。

## 6.1 职责、边界与性能目标

### 6.1.1 Frontend 的职责

Frontend 应负责：

- 将普通小对象请求映射到 size class；
- 从本地 LIFO FreeList 或 pointer array 完成无锁分配；
- 将普通小对象释放到本地缓存；
- 在本地缓存 underflow 时批量向 Middle-end refill；
- 在 overflow、idle、内存压力或线程退出时批量归还；
- 在总字节预算内动态调整不同 size class 的容量；
- 提供 per-thread/per-CPU cache flush、统计和诊断；
- 在支持 sized free/delete 时跳过不必要的 PageMap 查询。

Frontend 不应负责：

- 直接执行 mmap、munmap 或 madvise；
- 直接管理 Span bitmap；
- 修改 PageMap；
- split/coalesce Span；
- 在热路径中执行全局策略计算；
- 缓存大对象或任意过对齐 extent；
- 用无限扩大本地缓存换取表面吞吐。

### 6.1.2 层间边界

```text
Public allocation/free API
          |
          v
Frontend
  - local object cache
  - byte budget
  - slow-start / trim / GC
          |
          | batched object transfer
          v
Middle-end
  - TransferCache
  - Central SpanList / bitmap
          |
          | Span allocation/release
          v
PageCache
```

Frontend 只持有用户对象指针，不拥有 Span descriptor，也不直接修改 Span bitmap。对象位于 Frontend 时，对应 bitmap bit 仍保持 allocated/out-of-bitmap 状态，直到对象真正回到 CentralCache Span bitmap。

### 6.1.3 目标指标

建议同时定义延迟、扩展性和内存目标：

| 指标 | 目标方向 | 说明 |
|---|---|---|
| 稳态小对象 allocate/free | 维持纳秒级、无锁、O(1) | 重点观察 8B/64B 和随机小对象 |
| 热路径共享写 | per-thread 模式为零 | 统计也不应无条件产生共享原子写 |
| Frontend miss rate | 按 workload 可观测并可调 | 不能只靠扩大缓存降低 miss |
| 单线程 cache 上限 | 严格受总字节预算约束 | 避免所有 class 独立膨胀 |
| 总 Frontend RSS | 随活跃线程或 CPU 数受控增长 | 支持 idle/pressure 回收 |
| 扩展性 | 固定工作集下接近线性 | Middle-end 竞争需单独归因 |
| 尾延迟 | refill/trim/GC 有单次工作上限 | 监控 p99/p999，而非只看均值 |

## 6.2 Frontend 核心不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 目的 |
|---|---|---|
| FE-1 | per-thread ThreadCache 只有所属线程读写本地 FreeList | 保证快路径无需锁和 atomic |
| FE-2 | 每个本地 FreeList 只包含同一 size class 的合法对象 | 防止错误 class 复用导致越界 |
| FE-3 | FreeList 始终保持 LIFO | 优先重用最近访问对象，提高局部性 |
| FE-4 | 本地缓存对象仍对应 Span bitmap 中的 cleared bit | 保证 Span 不会过早回 PageCache |
| FE-5 | `cached_bytes` 等于所有本地 FreeList 对象的 class-size 加权和 | 保证预算和统计守恒 |
| FE-6 | refill 失败不破坏原 FreeList、quota 或 Middle-end 所有权 | OOM 失败原子性 |
| FE-7 | trim 只归还已经从本地 FreeList 摘除的完整对象链 | 防止双重所有权 |
| FE-8 | ThreadCache 销毁前先 drain 全部对象 | 避免线程退出泄漏和 Span 永久占用 |
| FE-9 | 全局依赖销毁后禁止 TLS late drain 进入 Middle-end | 避免静态析构 UAF |
| FE-10 | 普通小对象快路径不执行系统调用或全局锁操作 | 保持延迟基线 |
| FE-11 | remote free 对象在任一时刻只属于发送方、remote queue 或目标 cache 之一 | 防止并发 double ownership |
| FE-12 | per-CPU 模式的提交只在确认当前 CPU 未变化的 rseq 临界区内发生 | 防止线程迁移破坏数组状态 |
| FE-13 | sized free 提供的 size 不能未经验证直接污染目标 class | 防止错误调用导致 freelist 混类 |
| FE-14 | Frontend 不缓存大于配置上限或不能满足基础对齐的对象 | 保持路由和 ABI 一致 |
| FE-15 | 线程/CPU cache 的 soft/hard budget 始终有界 | 防止高线程数内存放大 |

FE-2、FE-5、FE-7 和 FE-13 建议在 Debug/Hardened 构建中增加深度检查；FE-4、FE-8、FE-9 属于生命周期正确性，不能只依赖断言。

## 6.3 当前实现基线与差距

### 6.3.1 已有基础

当前实现已经具备：

- `ThreadCache` 按 cache line 对齐；
- 每 size class 一个侵入式 `FreeList`；
- allocate 快路径为 size-class lookup + `FreeList::pop()`；
- deallocate 快路径为 `FreeList::push()` + quota 判断；
- slow-start quota；
- overflow 时单 batch 归还；
- overages 驱动 quota 衰减；
- ThreadCache/CentralCache 间使用对象内嵌 next 指针；
- LIFO 批量构链；
- 慢路径通过 `AM_NOINLINE` 与快路径分离。

这些结构应作为性能基线保留，后续预算、GC 和 remote free 不应侵入每次 allocate/free 的核心指令序列。

### 6.3.2 主要差距

| 差距 | 影响 | 改进方向 |
|---|---|---|
| TLS pointer 与 cleaner 生命周期没有形成可靠 holder | 线程退出不 drain，产生映射和对象缓存泄漏 | 合并为真实访问的 TLS RAII holder |
| ThreadCache 独占完整 OS page | 高线程数下 metadata 浪费 | 使用专用 metadata slab/pool |
| quota 只按每 class 对象数 | 总缓存字节可能跨 class 放大 | 增加全 cache byte budget |
| 没有增量 GC/idle flush | 长期休眠线程保留峰值缓存 | 活动驱动 GC + 显式 idle/pressure flush |
| 普通 free 总是 PageMap lookup | 已知 size 的 C++ delete 仍付查询成本 | sized free/delete |
| 跨线程 free 无 owner-aware 路径 | NUMA 漂移、释放线程 cache 污染 | remote queue 或 owner-routing 实验 |
| SizeClass 固定手工策略 | 难以适配 page size 和真实分布 | 自动生成并以 trace 验证 |
| 统计不足 | 无法解释 miss、trim、RSS 和尾延迟 | 分层近似统计 |
| 运行期配置未真正改变路由上限 | 文档、配置与行为不一致 | 明确编译期 class 几何和运行期 cache policy |

## 6.4 分配与释放请求路由

### 6.4.1 分配路由

```text
malloc(size)
  |
  +-- allocator not ready / recursive --> BootstrapAllocator
  |
  +-- zero size ------------------------> minimum ABI-compatible class
  |
  +-- ordinary small size -------------> Frontend
  |
  +-- over-aligned request -------------> aligned slow path
  |
  +-- size > frontend limit ------------> PageCache/LargeExtent backend
```

要求：

- 公共入口只执行最少状态读取、边界比较和 TLS/CPU cache 访问；
- zero-size、递归、过对齐和大对象进入 `AM_NOINLINE` 冷路径；
- `SizeClass::Index/RoundUp` 必须为 checked、O(1) 且不访问可变共享状态；
- Frontend runtime soft limit 不能改变编译期 size-class 几何，只能选择是否缓存某些 class；
- 请求超过运行期 Frontend limit 时可以绕过本地缓存，但仍可由 CentralCache 或 PageCache 按明确策略处理。

### 6.4.2 释放路由

```text
free(ptr)
  |
  +-- null -----------------------------> return
  |
  +-- sized + trusted small class ------> Frontend fast free
  |
  +-- ordinary pointer -----------------> PageMap lookup
          |
          +-- InUseSmall ---------------> local/remote Frontend
          +-- InUseLarge ---------------> PageCache/backend
          +-- bootstrap domain ---------> BootstrapAllocator
          +-- invalid/transition state -> hardened failure / UB policy
```

`free(nullptr)` 不应触发 allocator 初始化或 TLS holder 创建。普通 free 必须先取得稳定 Span descriptor，再读取 allocation kind 和 size class。

## 6.5 ThreadCache TLS RAII 设计

### 6.5.1 Holder 模型

建议将 TLS 指针、初始化状态和清理逻辑合并：

```cpp
struct ThreadCacheHolder {
    ThreadCache* cache{nullptr};
    uint32_t epoch_slot{kInvalidSlot};
    bool teardown_started{false};

    ThreadCache* GetOrCreate() noexcept;
    void Drain() noexcept;
    ~ThreadCacheHolder();
};

thread_local ThreadCacheHolder tls_frontend;
```

关键点：

- allocate/free 实际访问 `tls_frontend`，确保构造和析构注册真实发生；
- `GetOrCreate()` 在 normal allocator ready 后才从 ThreadCache metadata pool 获取对象；
- 创建失败返回 null，由调用方退回 Middle-end direct path 或 OOM；
- holder 析构不抛异常、不记录高层日志；
- `Drain()` 幂等，析构、显式 flush 和 fork child 重建可以安全调用；
- epoch slot、remote queue registration 等线程级资源由同一 holder 管理。

### 6.5.2 首次使用

```text
Frontend request
  -> tls holder already has cache: fast path
  -> no cache:
       - allocator ready?
       - obtain ThreadCache metadata
       - initialize all FreeLists and budget state
       - publish only to current TLS holder
       - execute slow refill
```

ThreadCache 完全属于当前线程，不需要发布给其他线程。全局 registry 如仅用于统计、flush 或 epoch，注册信息必须独立同步，不能让其他线程直接操作 ThreadCache 本地 FreeList。

### 6.5.3 TLS 析构顺序

ThreadCacheHolder 的 destructor 可能发生在其他 thread_local 对象析构期间，而这些析构函数也可能分配内存。因此：

- `teardown_started` 后当前线程新的分配走 BootstrapAllocator 或无 ThreadCache 的降级路径；
- drain 前全局 allocator runtime 必须仍处于 `Ready` 或受支持的 `ShuttingDown` 阶段；
- 全局核心默认采用进程生命周期常驻，避免 TLS 晚于静态单例析构；
- ThreadCache drain 期间发生递归分配时不得重新创建同一个 ThreadCache；
- 线程退出测试必须覆盖多个 TLS 对象交叉析构。

### 6.5.4 Thread pool 与 idle worker

长期线程池不会触发 TLS 析构，因此还需要显式接口：

- `am_thread_cache_flush()`：归还当前线程全部缓存对象；
- `am_thread_cache_idle()`：按 idle policy 缩减，而不一定完全清空；
- aethermind worker 在进入长期阻塞或模型卸载阶段调用 idle/flush；
- 接口只影响调用线程，不从外部线程直接遍历和修改其 FreeList。

## 6.6 ThreadCache 元数据分配

### 6.6.1 候选方案

| 方案 | 分配成本 | 空间效率 | 递归安全 | 回收复杂度 | 结论 |
|---|---:|---:|---:|---:|---|
| 每 ThreadCache 独立 mmap 整页 | 高 | 低 | 高 | 低 | 当前简单方案，仅保留 fallback |
| 全局 `ObjectPool<ThreadCache>` | 中 | 高 | 高 | 中 | 第一阶段可用 |
| 专用 cache-line aligned slab | 低 | 高 | 高 | 中高 | 稳定后推荐 |
| 普通 `new/delete` | 低 | 高 | 否 | 低 | 禁止 |

第一阶段可以使用 PageAllocator-backed ObjectPool，但要注意：

- pool lock 只在 ThreadCache 创建/销毁时使用，不进入对象快路径；
- slot 满足 `alignas(CACHE_LINE_SIZE)`；
- 一个 slab 内相邻 ThreadCache 虽然各自 cache-line 对齐，但结构总大小也应向 cache line 取整；
- pool 元数据不能使用系统 heap；
- final destroy 只在全局静默期执行。

### 6.6.2 Metadata 大小预算

需要报告：

```text
sizeof(ThreadCache)
bytes per active thread
metadata slab utilization
total ThreadCache metadata mapped/resident bytes
```

如果 FreeList 数量或统计字段持续增长，应将冷字段移到可选 side state，避免每个线程为从未使用的 class 支付过高固定成本。

## 6.7 FreeList 数据布局与操作

### 6.7.1 Intrusive LIFO

空闲对象的首个 pointer-sized word 存储 next：

```text
head -> most recently freed object -> ... -> oldest object -> null
```

优点：

- 不分配额外 node；
- push/pop O(1)；
- 最近释放对象更可能仍在 L1/L2；
- 批量传输可以原地构链。

要求最小 size class 至少能容纳一个指针，并满足公开 ABI 的基础对齐契约。

### 6.7.2 建议操作集

- `Push(ptr)`；
- `Pop()`；
- `PushRange(head, tail, count)`；
- `PopRange(max_count, &head, &tail)`；
- `ClearMetadataOnly()`，仅在对象所有权已转移后使用；
- `VisitForDebug()`，仅 Debug/静默期检查。

`PopRange` 应一次更新 head 和 size，避免 slow path 循环调用公开 `Pop()` 产生重复分支；但必须保持链顺序和 batch LIFO 语义。

### 6.7.3 字段布局

每 class 热字段建议紧凑排列：

```text
head pointer
current count
dynamic capacity
slow-path policy state
```

考虑把以下冷字段移入低频 side state 或压缩：

- overages；
- underflow history；
- last GC epoch；
- sampled statistics；
- remote drain hints。

需要通过 `static_assert(sizeof(FreeList) ...)` 和整体 ThreadCache cache-line 分析评估数组扫描、TLB 和 L1 footprint。

### 6.7.4 Pointer encoding

Hardened 模式可对 freelist next 使用进程/线程 cookie 编码，降低简单 UAF 覆盖 next 形成任意链的风险。默认性能模式是否启用必须基于基准；编码不应引入共享状态或额外内存访问。

ThreadCache 本地链不存在并发 ABA，因为只有所属线程操作。ABA 只在未来 remote lock-free queue 或 per-CPU 并发协议中需要单独处理。

## 6.8 分配快路径

### 6.8.1 目标形态

```text
aligned/classified small request
  -> load TLS ThreadCache pointer
  -> index local FreeList
  -> load head
  -> if non-null:
       load head->next
       store new head
       decrement count/cached bytes
       return old head
  -> cold refill
```

稳定命中路径应满足：

- 无锁；
- 无 atomic RMW；
- 无 PageMap；
- 无系统调用；
- 无统计共享写；
- 无函数调用或仅完全内联 helper；
- 常见 size 的 Index/RoundUp O(1)；
- 只访问 TLS、一个 FreeList 和对象头。

### 6.8.2 `cached_bytes` 更新

每次 pop/push 更新精确 `cached_bytes` 会增加一到两个 TLS 算术操作，但可以使总预算严格可见。候选方案：

- 精确更新：逻辑简单，慢路径决策准确；
- 分 class count，在慢路径按 class size 聚合：热路径更小，但计算和预算响应延迟增加；
- 分配减少缓存时不更新，仅 free/trim 更新近似值：容易破坏守恒，不推荐。

建议先实现精确 TLS `cached_bytes +=/-= class_size` 并通过汇编和基准决定是否需要优化。TLS 本地算术不会产生跨核共享流量。

### 6.8.3 Prefetch

- pop 后可预取新 head，用于连续分配；
- prefetch 的局部性级别和读/写意图需按架构封装；
- 单次 malloc/free pair 的 window=1 场景可能不受益；
- 必须分别测量 steady sequence、random class 和深窗口；
- 不把硬编码 prefetch 当作普遍优化。

### 6.8.4 Debug 与 Release

Release 快路径保留：

- 必须的路由边界；
- 防止数组越界的可信内部前置条件；
- 对 null holder/head 的分支。

Debug/Hardened 可增加：

- class index 和 aligned size 对应检查；
- pointer alignment；
- Span domain/class 抽样交叉验证；
- freelist cookie；
- count/cached bytes 守恒。

## 6.9 释放快路径

### 6.9.1 普通 free

普通 `free(ptr)` 必须通过稳定 PageMap descriptor 获得 allocation kind：

```text
ptr -> Span
  -> InUseLarge: backend free
  -> InUseSmall: obtain class size -> Frontend deallocate
```

小对象 local free：

1. 获取当前线程 holder；
2. 如果 holder 已 teardown 或创建失败，直接批量/单对象回 Middle-end；
3. push 到 class FreeList；
4. 增加 count/cached bytes；
5. 超过 class capacity 或 hard byte budget 时进入 trim 慢路径。

### 6.9.2 不创建 ThreadCache 的释放

一个从未分配过的小线程可能只负责释放其他线程创建的对象。为一次 free 创建完整 ThreadCache 可能不划算。建议策略：

- 如果当前线程已有 ThreadCache，正常缓存；
- 如果没有，直接将单对象或小批对象归还 Middle-end；
- 只有在观察到重复小对象 free/alloc 活动后才创建 ThreadCache；
- 为 free-only 线程记录轻量活动计数，但不能依赖堆分配。

这可减少短生命周期回收线程的 metadata 和缓存膨胀。

### 6.9.3 Overflow 判断

进入 trim 的条件可包括：

- `list.count > list.capacity`；
- `cached_bytes > hard_budget`；
- 当前 class 超过 per-class hard cap；
- 内存压力 generation 已变化；
- idle/flush 显式请求。

普通稳定 free 只应检查最常见的一到两个条件。低频压力/GC 信号可在采样计数达到阈值后检查，避免每次 free 读取共享原子。

## 6.10 Refill 慢路径

### 6.10.1 基本事务

```text
local FreeList empty
  -> calculate desired fetch count
  -> request batch from Middle-end
  -> receive [head, tail, count]
  -> retain one object for caller
  -> push remainder into local FreeList
  -> update cached bytes and policy state
```

Middle-end 接口应直接返回链和 count，避免 Frontend 使用固定大栈数组重新构链。

### 6.10.2 Fetch count

fetch count 受以下约束：

```text
desired = min(
    size_class_batch,
    class_dynamic_capacity - current_count + 1,
    bytes_available_in_soft_budget / class_size + 1,
    implementation_batch_cap)
```

至少获取一个返回给调用方的对象；若 byte budget 不允许缓存额外对象，可请求一个或从 Middle-end batch 中把剩余对象立即留在 Middle-end。

### 6.10.3 Partial refill

Middle-end 可以因 OOM 或库存不足返回少于 desired 的对象：

- `count == 0`：返回 null，FreeList 和 quota 不改变，记录 miss/failure；
- `count > 0`：一个对象返回用户，剩余对象缓存；
- policy growth 基于实际取得数量和 underflow 压力，而非假设完整 batch；
- 接收到的链必须完整属于当前 size class；
- push-range 后所有权一次性转移到 ThreadCache。

### 6.10.4 OOM 与异常边界

- Frontend refill API 为 `noexcept`；
- Middle-end 的 metadata OOM 被转换为空 batch；
- 不允许空指针 placement new；
- 失败路径不调用分配型日志；
- public malloc wrapper 负责设置 `errno` 或 C++ new 失败语义；
- quota、overages 和 GC cursor 在 OOM 后保持合法。

### 6.10.5 LIFO 顺序

假设 Middle-end 提供链 `A -> B -> C`，且 A 是最希望优先重用的对象：

- caller 应得到 A；
- remainder `B -> C` 以 B 为本地 head；
- 不应无意反转两次导致最冷对象先返回；
- TransferCache 数组 pop/push 和链转换必须通过顺序单测验证。

## 6.11 Overflow 与 Trim 慢路径

### 6.11.1 基本策略

当 class capacity 溢出时：

1. 从本地 FreeList 头部 pop 一个有界 batch；
2. 更新 count 和 `cached_bytes`；
3. 构造完整 LIFO 链；
4. 批量归还 Middle-end；
5. 更新 overage/overflow policy；
6. 如总缓存仍超过 hard budget，继续选择其他 class 做有界 trim。

单次普通 class overflow 应只归还一个 batch，避免一次 free 遭遇全 ThreadCache 扫描。hard budget 超限可触发额外 trim，但必须有最大对象数/字节工作预算。

### 6.11.2 工作集保留

- class capacity 小于一个 batch 时，可以保留 slow-start 的小容量；
- 达到稳态后通常至少保留一个近期工作 batch；
- 内存压力或 explicit flush 可以打破该下限；
- 大 size class 的保留对象数应更少，但按字节衡量不能完全固定为 1；
- 不能将全部 class 同时增长到 `batch * 8` 而忽略总预算。

### 6.11.3 失败处理

Middle-end release 正常不应失败，因为对象已经存在；如果内部诊断发现错误：

- 对象不能同时保留在本地链和提交到 Middle-end；
- Hardened 模式终止并输出无分配诊断；
- 生产模式不能把非法对象链继续传播到其他 Span；
- Release API 应明确所有权是在调用前、调用成功点还是始终无失败转移。

## 6.12 每线程总字节预算

### 6.12.1 为什么必须按字节预算

只限制每 class 对象数会产生以下放大：

```text
thread count * active size classes * per-class max objects * class size
```

高线程数和宽尺寸分布下，即使每个 class 看似合理，总 RSS 也可能远超预期。Frontend 必须同时控制：

- 每 class capacity；
- 每 ThreadCache 总 cached bytes；
- 全进程 Frontend 总预算；
- idle 与内存压力下的回收速度。

### 6.12.2 预算字段

每个 ThreadCache 建议维护：

- `cached_bytes`：当前实际缓存对象字节；
- `soft_budget_bytes`：正常 slow-start/容量竞争的目标上限；
- `hard_budget_bytes`：任何稳定状态不得长期超过的上限；
- `assigned_capacity_bytes`：所有 class dynamic capacity 的字节总和；
- `last_pressure_generation`；
- `gc_cursor`。

关系约束：

```text
cached_bytes <= assigned_capacity_bytes + bounded_transient
assigned_capacity_bytes <= hard_budget_bytes
soft_budget_bytes <= hard_budget_bytes
```

refill/trim 期间可出现有界 transient，但慢路径返回前必须恢复约束。

### 6.12.3 预算来源

预算可由以下因素决定：

- 默认 per-thread 上限；
- 活跃线程数量；
- 线程类型或 aethermind worker role；
- 进程级 Frontend 总上限；
- cgroup/系统内存压力；
- latency/memory 运行模式；
- NUMA node 局部预算。

第一阶段使用固定 per-thread soft/hard budget；活跃线程自适应和全进程再平衡放到后续慢路径控制器，避免初始化复杂度进入热路径。

### 6.12.4 容量竞争

一个 class 需要增长容量时：

1. 检查 `assigned_capacity_bytes + growth_bytes <= soft_budget`；
2. 若超限，从低收益 class 回收 assigned capacity；
3. 优先选择长时间无命中、缓存占比高、对象较大的 class；
4. 回收 capacity 不一定立即扫描/归还对象；若 current count 超过新 capacity，安排有界 trim；
5. hard budget 超限时直接执行实际对象回收。

## 6.13 Slow-start 与自适应容量

### 6.13.1 输入信号

每 class 慢路径状态可基于：

- underflow/refill 次数；
- overflow/trim 次数；
- underflow 与 overflow 交替频率；
- refill 后对象被消费的速度；
- 长期 idle epoch；
- 当前 class cached bytes；
- 全 ThreadCache budget pressure；
- remote drain 贡献。

### 6.13.2 增长策略

推荐原则：

- 冷启动容量为 1 或一个很小值；
- 连续 underflow 表明缓存过小，先指数增长到 batch；
- 达到 batch 后按小步线性增长；
- underflow/overflow 频繁交替说明 capacity 接近工作集边界，可适度增长以消除抖动；
- 任何增长都受 class hard cap 和 ThreadCache byte budget 限制。

### 6.13.3 衰减策略

- 连续 overflow 表明 class 缓存大于近期需求；
- 多个 GC epoch 未命中时减少 assigned capacity；
- memory pressure 下按字节收益优先回收；
- capacity 不应每次事件都剧烈变化，使用 hysteresis 和最小保持 epoch；
- 衰减决策在 slow path/GC 中执行，不进入每次 pop/push。

### 6.13.4 稳定性约束

- growth/shrink 步长有上限；
- policy counter 使用饱和算术，避免长时间运行回绕；
- 参数变化不会让 `capacity < current_count` 后长期不 trim；
- 运行时调参发布使用 generation，在慢路径采样，不要求热路径实时读取；
- 所有策略都有静态保守 fallback。

## 6.14 增量 GC、Idle 与压力回收

### 6.14.1 活动驱动 GC

长期线程不退出，需要由分配活动周期性触发小步 GC：

```text
every N slow-path events or allocated bytes
  -> inspect K size classes from gc_cursor
  -> trim stale excess
  -> advance cursor
```

要求：

- 不在每次 fast allocation/free 检查 wall clock；
- 使用本地事件或字节计数触发；
- 单次最多扫描 K 个 class、归还 M 个对象或 B 字节；
- 完整一轮覆盖所有 class，但工作分摊到多个慢路径；
- 没有活动的线程依赖显式 idle 或全局压力通知。

### 6.14.2 Staleness

每 class 可维护近似 activity epoch：

- refill/pop 说明 allocation demand；
- overflow/push 说明 deallocation pressure；
- 多个 GC epoch 无 demand 且 count 较高，判定 stale；
- 不需要每个 fast hit 都更新时间戳，可通过采样计数或慢路径事件近似。

### 6.14.3 Idle 接口

`am_thread_cache_idle()` 建议：

- 保留少量最热 class 工作集；
- 显著缩减冷 class capacity；
- 将 cached bytes 降到 idle budget；
- 不销毁 ThreadCache metadata，便于线程快速恢复；
- 幂等且不抛异常。

`am_thread_cache_flush()` 则归还全部对象并把 class capacity 恢复初始值，适合模型卸载、线程角色切换和测试隔离。

### 6.14.4 全局内存压力

全局控制器只递增 relaxed/release pressure generation，不直接跨线程修改 FreeList：

- ThreadCache 在下一次慢路径或采样点观察 generation；
- 执行有界 pressure trim；
- 长期完全 idle 的 worker 由 aethermind 调度器显式调用 idle/flush；
- per-CPU 模式可由管理线程安全地 drain 指定 CPU cache，但必须使用其专用协议。

## 6.15 跨线程释放与 Remote Free

### 6.15.1 当前“释放线程缓存”模型

对象在任意线程 free 时进入释放线程 ThreadCache，优点是：

- 实现简单；
- free 快路径只访问本地 TLS；
- 不需要记录 allocation thread owner；
- 无 remote queue 原子操作。

缺点是：

- producer 分配、consumer 释放时对象逐渐迁移到 consumer cache；
- 原 producer 随后 refill，引入 Middle-end 流量；
- NUMA first-touch 页面可能在远端线程缓存中长期保留；
- 释放专用线程可能囤积大量它不会重新分配的 class。

第一阶段仍可保留该模型，同时通过“不为单次 free 创建 ThreadCache”、byte budget 和 idle GC 限制问题。

### 6.15.2 Owner-aware remote queue

后续可在 Span/region 记录 logical owner（CPU、Frontend shard 或 NUMA node），跨 owner free 进入 remote queue：

```text
freeing thread
  -> determine Span logical owner
  -> enqueue object/batch to owner remote queue
  -> owner allocate/slow path drains queue into local cache or Middle-end
```

owner 不应是易失的原始 thread id，因为线程可能退出。更稳妥的是：

- per-CPU cache id；
- NUMA-local Frontend shard；
- 可重定向的 ThreadCache registry slot + generation；
- owner 失效时退回 Middle-end。

### 6.15.3 Queue 候选

| 方案 | free 成本 | drain 成本 | 风险 |
|---|---:|---:|---|
| MPSC intrusive stack | 一个原子 exchange/CAS | 批量取出后可能需反转 | ABA、顺序、共享热点 |
| MPSC queue | 较高 | 保持 FIFO，但局部性未必更好 | metadata/算法复杂 |
| per-owner locked batch | 低频锁 | 简单批量 | 高竞争 owner 可能阻塞 |
| 直接回 Middle-end | 获取 bucket lock | 无 owner queue | Middle-end 竞争增加 |

remote free 的目标是降低整体跨层流量和 NUMA 漂移，而不是追求 free 形式上的 lock-free。应先用 producer/consumer trace 证明收益。

### 6.15.4 Drain 与退化

- owner 在本地 underflow 前优先 drain remote queue；
- 每次 drain 有对象数/字节上限；
- remote 链按 LIFO/FIFO 特性转换为本地 LIFO，顺序必须测试；
- owner 线程退出时先标记 slot closing，再把 queue drain 到 Middle-end；
- enqueue 观察到 closing/generation mismatch 时直接回 Middle-end；
- remote queue bytes 计入 owner 或全局 Frontend 预算，不能成为隐藏缓存。

### 6.15.5 NUMA 策略

对于 aethermind 固定 NUMA worker，更可能有价值的是“回 NUMA-local Middle-end”而不是“回原线程”。建议依次实验：

1. 当前释放线程本地缓存；
2. free-only 线程直接回 NUMA-local Middle-end；
3. per-CPU remote queue；
4. 原 owner ThreadCache queue。

按 remote access、Middle-end lock、RSS 和 tokens/s 综合决定。

## 6.16 ThreadCache 退出、Shutdown 与 Fork

### 6.16.1 线程退出

正确顺序：

1. holder 标记 `teardown_started`，阻止重新创建；
2. 从 remote registry 标记 owner closing；
3. drain remote incoming queue；
4. 遍历本地 size class，按 batch 归还 Middle-end；
5. 释放 epoch/registry slot；
6. 销毁 ThreadCache；
7. 将 metadata slot 回专用 pool。

整个过程不得抛异常。Middle-end 若已不可用，则必须走 allocator shutdown 明确定义的降级路径，而不是静默泄漏或 UAF。

### 6.16.2 进程 shutdown

推荐全局核心常驻到进程退出。若支持受控 destroy：

- 先进入 `kShuttingDown`；
- 停止创建新 ThreadCache；
- 停止 Scavenger；
- 等待/协调 worker 退出并 drain；
- 再清 Middle-end/PageCache；
- 不允许未知业务线程继续 malloc/free。

外部线程不能安全地直接 flush 另一个仍运行线程的普通 FreeList；需要 cooperative generation、stop-the-world 或 per-CPU 专用协议。

### 6.16.3 Fork

`pthread_atfork` child 路径中：

- 只保留当前线程 TLS holder；
- 重置消失线程的 registry/remote owner slot；
- 把无法确认的 remote queue 标记为需要回收；
- 当前 holder 本地 FreeList 可以保留，前提是 Middle-end/PageMap 状态已按全局 atfork 协议恢复；
- per-CPU/rseq registration 在 child 中重新注册；
- 后台线程状态重置为未启动。

## 6.17 Per-CPU Frontend 目标架构

### 6.17.1 采用动机

per-thread cache 的内存随活跃线程数增长。大量短线程、协程调度线程或线程数显著高于 CPU 数时，per-CPU cache 可以把 Frontend 内存规模限制在 logical CPU 数量，并让同 CPU 上的线程共享工作集。

它也带来新的复杂度：线程可能迁移和被抢占，同一 CPU cache 需要无锁但不是线程私有的提交协议。

### 6.17.2 Slab 布局

建议一次预留 per-CPU slab：

```text
CPU section
  +----------------------------+
  | per-class headers          |
  | begin/current/end/capacity |
  +----------------------------+
  | pointer array segments     |
  | class 0 | class 1 | ...    |
  +----------------------------+
  | cold stats / control       |
  +----------------------------+
```

要求：

- CPU section 按 cache line 或更大边界对齐；
- 每 class pointer segment 的静态上限在初始化时确定；
- dynamic capacity 不超过 segment 静态上限；
- 当前长度和提交字段位于同一 CPU 本地 cache line；
- 相邻 CPU 高频写字段不共享 cache line；
- slab 来自 PageAllocator/Bootstrap-safe mapping，不使用系统 heap。

### 6.17.3 CPU 总预算

per-CPU 模式按 CPU 控制总 assigned capacity：

- class 扩容需要从同 CPU 其他冷 class 借用容量；
- 不要求指针 segment 物理移动，可以只改变 logical capacity；
- CPU hard budget 决定整个 section 能缓存的对象字节或指针槽；
- 未在线 CPU 和长期 idle CPU 可以释放其缓存对象；
- CPU hotplug 需要明确支持或静态禁用策略。

### 6.17.4 与 Middle-end 共用

per-thread 和 per-CPU Frontend 应共用同一批量 Middle-end API：

- `FetchBatch(class, max_count)`；
- `ReleaseBatch(class, head, tail, count)`；
- NUMA/shard hint 可作为附加参数；
- Middle-end 不依赖 Frontend 实现细节。

这保证可在相同 benchmark 中切换两种 Frontend，并保留快速回滚。

## 6.18 Linux rseq 协议

### 6.18.1 基本原理

restartable sequences 允许用户态定义短临界区：线程若在临界区中被迁移、抢占到另一 CPU 或收到需要重启的事件，内核将执行流跳到 abort handler，操作从头重试。提交点之前的局部计算可以重做，提交点之后状态必须完整一致。

### 6.18.2 Pop 提交

概念流程：

```text
rseq begin
  -> read current cpu id
  -> locate CPU/class header
  -> read current length
  -> if zero: abort to slow path
  -> load pointer[length - 1]
  -> commit new length = length - 1   <-- single commit point
rseq end
  -> return pointer
```

Push 类似：先验证 capacity、写入 pointer slot，再以长度更新作为提交点。具体写入顺序必须遵循 rseq ABI 和目标架构要求，不能用普通 C++ 原子推测实现。

### 6.18.3 限制

- rseq 临界区必须非常短，不能调用函数、加锁或触发 page fault 风险较高的操作；
- underflow/overflow 转入普通慢路径；
- 信号、抢占和 CPU migration 必须由 rseq abort/retry 覆盖；
- 编译器生成、汇编约束和链接段需要专门测试；
- 不支持的平台必须可靠回退到 per-thread；
- sanitizer 可能无法完整理解手写 rseq，需要额外模型测试。

### 6.18.4 注册与回退

运行时检测：

1. kernel/architecture 是否支持 rseq；
2. libc 是否已经注册当前线程；
3. ammalloc 与其他库是否存在 rseq ABI 冲突；
4. 当前进程配置是否允许 per-CPU；
5. 失败则选择 per-thread，不影响正确性。

不得在 malloc 热路径反复探测能力。选择结果在线程/runtime 初始化时缓存。

## 6.19 Frontend 模式选择

建议支持三种模式：

| 模式 | 适用场景 | 优点 | 主要风险 |
|---|---|---|---|
| Per-thread | 通用平台、固定少量 worker、第一阶段默认 | 简单、快路径极小 | 内存随线程数增长 |
| Per-CPU | Linux、高线程数、稳定 CPU 拓扑 | 内存按 CPU 有界、共享工作集 | rseq、迁移、CPU hotplug 复杂 |
| Frontend disabled/minimal | 内存敏感、调试、基准隔离 | 最小缓存和易诊断 | Middle-end 锁流量高 |

选择可以是编译期能力 + 启动期策略：

- 编译期决定是否包含 rseq/per-CPU 实现；
- 启动期根据平台能力、环境配置和 workload profile 选择；
- runtime ready 后不建议在任意线程活跃时切换模式；
- 测试和灰度必须保留一键回退 per-thread。

对 aethermind：

- 固定绑核、线程数接近 CPU 数时同时测试 per-thread/per-CPU；
- 每 worker 有强私有工作集时 per-thread 可能更优；
- 大量任务在线程间迁移时 per-CPU 可能减少缓存漂移；
- 最终以 tokens/s、p99、RSS、remote NUMA access 决定。

## 6.20 Sized free 与 Sized delete

### 6.20.1 接口

内部接口建议包含：

```cpp
void am_free_sized(void* ptr, size_t size) noexcept;
void am_free_aligned_sized(void* ptr, size_t size, size_t alignment) noexcept;
```

C++ sized delete 直接调用这些接口。编译器已知对象大小时可省略 PageMap pointer-to-size lookup。

### 6.20.2 路由

```text
ptr + supplied size
  -> validate size arithmetic
  -> size <= Frontend max and ordinary alignment?
       -> calculate class
       -> optional sampled/Hardened PageMap cross-check
       -> Frontend deallocate
  -> otherwise
       -> ordinary PageMap/backend free
```

### 6.20.3 信任边界

标准 sized delete 要求调用方提供匹配大小；错误 size 属于调用方未定义行为。但 allocator 应防止容易发生的灾难性污染：

- Debug/Hardened 始终与 Span class 交叉验证；
- 默认模式可以按低概率采样验证；
- size 超出小对象范围时退回普通 free；
- alignment 不普通时走 aligned 路径；
- 不能仅凭 size 把大对象内部指针推入小对象 FreeList。

### 6.20.4 指标

- sized free/delete 调用比例；
- 成功跳过 PageMap 的比例；
- cross-check failure；
- 普通 free 与 sized free 的指令数和延迟差；
- binary size 和 I-cache 影响。

## 6.21 SizeClass 自动生成

### 6.21.1 生成器输入

- `max_frontend_size`；
- 基础 alignment 和 over-alignment 策略；
- allocator logical page size；
- OS page/hugepage size；
- 每 Span 目标对象数；
- 允许的最大内部碎片率；
- 允许的 Span 尾部浪费；
- batch 字节上下限；
- Frontend per-thread/per-CPU budget；
- PageMap/Span metadata 成本；
- 真实 allocation trace 的 size 频率和生命周期。

### 6.21.2 生成器输出

每个 class 输出：

- class size；
- alignment；
- batch count；
- Span logical page count；
- 初始/最大 Frontend capacity；
- TransferCache 建议容量；
- bitmap/object layout 参数；
- 可选 trace 权重和预计浪费。

生成结果编译为 constexpr table，运行时 hot path 不执行搜索。

### 6.21.3 优化目标

可定义加权成本：

```text
cost = W1 * expected_internal_fragmentation
     + W2 * expected_span_tail_waste
     + W3 * middle_end_refill_rate
     + W4 * frontend_cached_bytes
     + W5 * metadata_bytes
     + W6 * TLB/pageheap pressure
```

权重应为 profile 配置的一部分：latency profile 更重视 refill rate，memory profile 更重视碎片和 cached bytes。

### 6.21.4 验证

- 对全部输入 size 验证 `Size(Index(s)) >= s`；
- class size 单调且 index 唯一；
- 所有 class 满足 alignment；
- bitmap + objects 不超过 Span；
- round-up/乘法无溢出；
- 与旧 table 比较碎片、batch 和 benchmark；
- 使用 aethermind trace 离线重放预计 RSS 和 refill 次数。

### 6.21.5 Logical page 解耦

allocator logical page 可以由多个 OS page 组成，也可以根据构建 profile 选择 4/8/32 KiB。影响包括：

- Span 页数和 PageMap 粒度；
- 小对象尾部浪费；
- 后端 refill 频率；
- TLB 和 hugepage packing；
- metadata 大小。

这属于跨 Frontend/PageMap/PageCache 的架构变更，不能只修改 SizeClass 常量。第一阶段保持 4 KiB，先建立生成器和对比工具。

## 6.22 对齐与特殊请求

### 6.22.1 基础对齐

如果公开 malloc ABI 要求 16B：

- 每个普通小 size class 的 stride 必须是 16 的倍数，或通过布局确保每个对象起点都满足 16B；
- 数据区 base 单独对齐不足以修复 8B stride；
- 最小对象仍需容纳 freelist pointer；
- SizeClass table 和 Span layout 都要有静态/运行时测试。

可以选择像部分主流 allocator 一样对极小请求提供较低对齐，但必须严格符合目标 C/C++ ABI、编译器默认 new alignment 和项目兼容策略，不能与 `SystemConfig::ALIGNMENT` 自相矛盾。

### 6.22.2 Over-aligned

- alignment 大于普通 class 能力时不进入普通 ThreadCache；
- 第一阶段通过 PageCache/extent 分配额外 padding；
- metadata 记录 user base、mapping base 和 alignment；
- sized-aligned delete 走对应路径；
- 只有 trace 证明某些 over-aligned 小请求很常见时，才增加专用 class。

### 6.22.3 Zero-size

项目可以继续把 zero-size 映射到最小 class，但要满足：

- 返回指针可正常 free；
- alignment 与普通 malloc 一致；
- 每次是否返回唯一地址不作额外保证；
- zero-size 统计单独记录，避免污染真实 requested-bytes 碎片计算；
- OOM 时仍允许返回 null。

## 6.23 缓存局部性与伪共享

### 6.23.1 ThreadCache 布局

- `ThreadCache` 起始地址 cache-line aligned；
- 总大小向 cache line 取整，避免 slab 中相邻实例共享尾部 line；
- FreeList 数组按常用 class 顺序连续排列；
- 极热 class header 可以靠近对象首部，冷策略字段放后部/side state；
- 当前线程独占 ThreadCache，因此其内部不同 FreeList 无需逐个 cache-line 对齐，否则会造成巨大 metadata 膨胀。

### 6.23.2 Per-CPU 布局

- 不同 CPU section 严格隔离 cache line；
- 高频 length/capacity 字段不与其他 CPU 共享 line；
- 同 CPU 不同 class 可以紧凑，但需分析并行 slow-path 管理线程访问；
- cold global control 和 stats 不放入 push/pop 的 line；
- NUMA 上 per-CPU slab first-touch 到所属 node。

### 6.23.3 LIFO 与对象局部性

- local free 后近期 allocate 优先取得同一对象；
- refill/trim 的链转换不应破坏 LIFO；
- remote queue 若反转链，需要测量对局部性的影响；
- poison/zeroing 可能使刚释放对象 cache line 保持热，需分别测量安全与性能模式。

## 6.24 并发与内存序

### 6.24.1 Per-thread 模式

本地 FreeList、count、capacity、cached bytes 和 policy state 全部线程私有，不使用 atomic。线程私有性是同步契约，不应为了“看起来线程安全”增加无意义原子。

全局 registry/pressure generation 使用原子：

- 纯计数器：`memory_order_relaxed`；
- pressure generation 发布：writer release，ThreadCache 慢路径 acquire 或 relaxed + 独立策略论证；
- holder closing/generation：remote enqueue 需要 acquire/release；
- 禁止默认 seq_cst。

### 6.24.2 Remote queue

若使用 MPSC intrusive stack：

- producer 在对象 next 完成后以 release CAS/exchange 发布；
- consumer 以 acquire exchange 取走整条链；
- next 在发布后不得由 producer 修改；
- queue head 需要 ABA/tag 或算法本身证明不会复用节点造成 ABA；
- 对象节点生命周期由 Span out-of-bitmap 语义保证，但 double free 仍需 Hardened 检测。

### 6.24.3 Per-CPU/rseq

rseq 提供 CPU 本地提交原子性，不等价于 C++ memory model 的跨线程同步。Middle-end refill/drain、管理线程回收 CPU cache 时仍需明确的锁、停止协议或 acquire/release publication。

## 6.25 可观测性

### 6.25.1 每 ThreadCache/CPU 指标

- cached bytes；
- assigned capacity bytes；
- soft/hard budget；
- active class 数；
- hit/miss/refill/partial-refill/OOM；
- overflow/trim/GC returned bytes；
- idle/flush 次数；
- remote enqueue/drain/fallback；
- holder create/destroy；
- last activity/pressure generation。

### 6.25.2 每 SizeClass 指标

- allocate/free request；
- local hit；
- refill batch 平均/分布；
- current/max cached objects；
- capacity growth/shrink；
- GC age；
- sized-free 命中；
- remote free 比例。

### 6.25.3 热路径统计策略

不能为每个 fast allocation 更新全局 atomic。候选方式：

- TLS 本地计数，慢路径或显式 snapshot 聚合；
- 固定采样频率；
- build-time stats 开关；
- per-CPU 模式在 CPU-local cache line 中累计；
- 聚合允许近似，避免 stop-the-world。

## 6.26 测试与故障注入

### 6.26.1 ThreadCache 基础

- 每个 size class allocate/free；
- LIFO 单对象和 range 顺序；
- count/cached bytes 守恒；
- slow-start 到 batch、线性增长和上限；
- overflow 单 batch trim；
- hard budget 多 class trim；
- partial refill 和 OOM；
- zero-size 和 alignment；
- 不为单次 free-only 线程创建 cache。

### 6.26.2 生命周期

- TLS holder 实际构造/析构；
- 大量短线程退出后 metadata/cached bytes 回落；
- destructor 内递归 allocation；
- explicit idle/flush 幂等；
- shutdown 与 thread exit 并发；
- fork child registry/TLS 重建；
- ThreadCache pool OOM 和回收。

### 6.26.3 自适应策略

- 连续 underflow 扩容；
- overflow 衰减；
- underflow/overflow 交替消抖；
- capacity byte 总和不超过 hard budget；
- GC cursor 最终覆盖所有 class；
- 单次 GC 工作量有界；
- pressure generation 触发回收；
- 长期 idle class 收缩。

### 6.26.4 Cross-thread/remote

- producer 分配、consumer 释放；
- 多 producer 单 owner；
- owner 退出与 enqueue 竞争；
- generation mismatch fallback；
- remote queue bytes 计入预算；
- 链 drain 顺序；
- NUMA-local fallback；
- double free Hardened 检测。

### 6.26.5 Per-CPU/rseq

- rseq 可用/不可用检测；
- 临界区 underflow/overflow；
- 模拟 CPU migration/abort/retry；
- signal interruption；
- CPU offline/online 策略；
- 管理线程 drain 与 fast path 协调；
- per-thread fallback 行为一致。

### 6.26.6 工具

- ASan：对象链、range transfer、TLS teardown；
- UBSan：class index、cached byte 算术、alignment；
- TSan：registry、remote queue、pressure generation；
- 模型化测试：owner closing/enqueue/drain 状态交错；
- 固定种子随机 allocate/free/flush/pressure；
- 汇编检查：fast path 指令和意外函数调用。

## 6.27 性能护栏与基准矩阵

### 6.27.1 微基准

- 8B、64B、256B、4KiB allocate/free pair，window=1；
- window=256/1024 稳态；
- random size；
- refill underflow；
- overflow trim；
- sized free vs ordinary free；
- ThreadCache holder 已存在/首次创建；
- stats 开启/关闭；
- pointer encoding 开启/关闭。

### 6.27.2 并发基准

- 1/2/4/8/16/32 线程固定 size；
- 线程数远高于 CPU 数；
- 高频短生命周期线程；
- producer/consumer cross-thread free；
- 多 producer 单 consumer；
- per-thread vs per-CPU；
- NUMA local/remote；
- idle/pressure 后恢复。

### 6.27.3 指标

- ns/op 与 cycles/op；
- instructions、branches、branch miss；
- L1/LLC/TLB miss；
- p50/p95/p99/p999；
- Middle-end batch calls；
- cached/assigned bytes；
- peak/steady RSS；
- thread exit 回收时间；
- remote atomic contention；
- aethermind tokens/s 与请求尾延迟。

### 6.27.4 回归门禁

- 所有 before/after 使用相同 Release 工具链、affinity、governor 和 THP 设置；
- 保存 Google Benchmark JSON 和 perf counter；
- fast path 性能变化超过预设阈值必须解释到指令或 cache 行为；
- 降低 miss rate不能以超预算 RSS 为代价；
- per-CPU 模式只有在目标 workload 综合优于 per-thread 时才默认启用；
- 与 TCMalloc/jemalloc 比较必须使用相同 API、工作集和字节统计口径。

## 6.28 分阶段实施与验收

### 阶段 A：ThreadCache 正确性基线

实施内容：

1. 引入真实 TLS `ThreadCacheHolder`；
2. 修复创建 OOM、析构和 late allocation；
3. 使用专用 ObjectPool/slab 管理 ThreadCache metadata；
4. 增加 cached-bytes 守恒；
5. 明确 free-only 线程降级；
6. 补齐生命周期和故障注入测试。

退出条件：

- 大量短线程退出无 Frontend 对象或 metadata 泄漏；
- fast path 仍为无锁 O(1)；
- OOM 不崩溃且状态不损坏；
- ASan/UBSan/TSan 通过；
- 8B/64B 基线无不可接受退化。

风险类型：正确性、内存、性能。

### 阶段 B：预算、GC、Sized free 与 SizeClass 生成

实施内容：

1. per-thread soft/hard byte budget；
2. slow-start 容量受总预算控制；
3. 增量 GC、idle 和 flush；
4. sized free/delete；
5. SizeClass table 生成器和 trace 分析；
6. 完整 Frontend 统计。

退出条件：

- cached/assigned bytes 始终满足预算；
- idle 和 memory profile 下 RSS 可预测回落；
- sized delete 获得可量化收益；
- 新 SizeClass table 在碎片、吞吐和 refill 之间优于或不劣于旧表；
- GC p99 工作量有界。

风险类型：内存、性能、正确性。

### 阶段 C：Remote free 与 NUMA-aware 路由

实施内容：

1. 先优化 free-only 线程直返 NUMA-local Middle-end；
2. 定义稳定 owner id/generation；
3. 实验 remote queue；
4. owner closing、fallback 和预算统计；
5. producer/consumer 与 aethermind trace 验证。

退出条件：

- remote queue 不产生 UAF、ABA 或 owner-exit 丢对象；
- cross-thread workload 的 Middle-end 流量或 NUMA remote access 显著下降；
- 普通 local free 不因 remote 支持出现不可接受退化；
- remote cached bytes 受统一预算控制。

风险类型：并发、内存、性能。

### 阶段 D：Per-CPU/rseq 实验与灰度

实施内容：

1. 建立独立 per-CPU slab；
2. 实现 rseq push/pop 和可靠 fallback；
3. CPU 总预算及 class 容量竞争；
4. idle CPU 回收和 CPU hotplug 策略；
5. 与 per-thread 共用 Middle-end；
6. 在 aethermind 固定 worker 小范围灰度。

退出条件：

- rseq abort/migration/signal 测试通过；
- 不支持 rseq 时无缝回退；
- 高线程数下 RSS 和扩展性显著优于 per-thread；
- 目标推理 workload 的 tokens/s、p99 和 RSS 综合收益明确；
- 保留运行时/构建期快速回退 per-thread。

风险类型：并发、性能、兼容性。

