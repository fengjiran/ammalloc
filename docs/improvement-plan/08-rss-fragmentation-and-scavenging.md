# 第 9 章：RSS、碎片与后台回收

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](07-backend-pagecache-large-object.md) · [下一章](09-numa-and-aethermind.md)  
> **本章目标**：在复用延迟、RSS、碎片、系统调用和尾延迟之间建立反馈控制。  
> **适用范围**：allocator 各层缓存、Scavenger、decay、purge、cgroup/PSI 与内存预算。  
> **核心 invariant**：回收工作量有界，统计口径守恒，降低 RSS 不以不可见的 p99/refault 代价换取。

RSS 治理不是简单地“尽快 madvise”。分配器必须在复用延迟、物理内存、虚拟地址、系统调用、page fault、THP 完整性和应用尾延迟之间建立可观测的反馈控制。Backend 定义 extent 状态和锁外 OS 事务，本节负责决定何时回收、回收多少以及如何证明 RSS 确实回落。

当前 Scavenger 已具备 `std::jthread`、stop-aware wait、锁内 detach 和锁外 `MADV_DONTNEED` 的基本形态，但仍是单 shard、固定 1 秒周期、10 秒阈值和全桶扫描。后续改进应先修复生命周期与多 shard 正确性，再引入 decay 和压力反馈。

## 9.1 职责、边界与目标

### 9.1.1 职责

RSS/回收子系统应负责：

- 统一统计 allocator 各层持有的逻辑字节、映射字节和可回收字节；
- 对 free committed extent 执行有界、增量的 purge；
- 对长期无复用价值的 retained extent 执行 unmap；
- 响应 allocator 内部预算、cgroup 事件、PSI 和上层显式压力通知；
- 协调 Frontend、Middle-end、PageCache 和 hugepage cache 的分层排空；
- 控制同步回收的最大工作量，避免 OOM 路径形成延迟风暴；
- 在 shutdown/fork 前建立确定性静止边界；
- 量化 purge 后 RSS、fault、THP breakage 和吞吐的真实变化。

它不应：

- 在后台线程直接访问未受 owner shard 保护的 SpanList；
- 在 PageCache 锁内调用 `madvise`、`munmap`、日志或用户回调；
- 将 allocator 内部 `purged_bytes` 当成进程 RSS 的精确值；
- 每轮全量扫描所有 shard、bucket 和 extent；
- 在每次 free 上读取 cgroup 文件或 `/proc`；
- 把内存压力策略放入 3.8 ns Frontend fast path；
- 为了降低稳态 RSS 破坏峰值吞吐和 p99，而不报告权衡。

### 9.1.2 目标

| 目标 | 判定方式 |
|---|---|
| Burst 后可预测回落 | 报告 1s/5s/10s/30s RSS 与 allocator retained 曲线 |
| 回收工作有界 | 单轮 pages、syscalls、wall time 和 shard 数均有上限 |
| 无锁序反转 | detach/reinsert 遵循[第 5 章](04-pagemap-and-span-lifecycle.md)和[第 8 章](07-backend-pagecache-large-object.md)的状态机 |
| 压力响应及时 | `memory.events`/PSI/显式通知到 cache shrink 的延迟可测 |
| 避免回收振荡 | purge 后短期 refault 与再次 refill 比例受控 |
| 统计守恒 | allocator 各层字节快照能解释 mapped/retained 差异 |
| 不伤热路径 | fast-path 和 random-size 基线无可测共享写退化 |

## 9.2 内存统计口径

### 9.2.1 基础字节

至少定义：

| 指标 | 定义 |
|---|---|
| `requested_bytes` | 当前用户请求的逻辑字节，采样或精确取决于 metadata 能力 |
| `usable_bytes` | 当前用户对象可安全访问的总字节 |
| `active_span_bytes` | 承载 live/缓存对象的 Span/Extent 页字节 |
| `frontend_cached_bytes` | ThreadCache/per-CPU 中对象按 class size 加权 |
| `transfer_cached_bytes` | TransferCache 中对象字节 |
| `central_bitmap_free_bytes` | active Central Span bitmap 中可用对象字节 |
| `pagecache_free_committed_bytes` | PageCache 可复用且可能仍驻留的空闲 extent |
| `pagecache_free_purged_bytes` | VA 保留但已请求内核丢弃物理页的 extent |
| `retained_va_bytes` | allocator 保留的可复用 VA |
| `direct_mapped_bytes` | 独立大对象 mapping 长度 |
| `metadata_mapped/used/retired_bytes` | metadata arena 的映射、使用和延迟回收字节 |
| `guard_bytes` | guard page、redzone mapping 等安全开销 |
| `mapped_bytes` | allocator 仍拥有的虚拟映射总长度 |
| `observed_rss_bytes` | 由 OS 低频观测的进程 RSS，不等同 allocator 专属 RSS |

### 9.2.2 避免重复计算

Frontend/TransferCache 对象仍位于 active Span 页中，因此：

- `frontend_cached_bytes` 和 `transfer_cached_bytes` 是 active 页内部分类；
- 它们不能再次加到 `active_span_bytes` 上计算 mapped bytes；
- `central_bitmap_free_bytes` 也是 active Span 内部未使用容量；
- `pagecache_free_*` 才是从 active Span 退出后的页级空闲；
- metadata 和 direct mapping 单独计入；
- OS RSS 包含 allocator 之外的栈、代码、文件映射和其他匿名区，不能与 allocator accounting 强行守恒。

### 9.2.3 派生指标

建议同时报告比率和浪费字节：

~~~text
internal_fragmentation_bytes = usable_bytes - requested_bytes
internal_fragmentation_ratio = internal_fragmentation_bytes / max(requested_bytes, 1)

active_slack_bytes =
    active_span_bytes - usable_live_object_bytes
active_amplification = active_span_bytes / max(usable_live_object_bytes, 1)

allocator_retained_bytes =
    pagecache_free_committed_bytes
  + pagecache_free_purged_bytes
  + retained_metadata_slack

mapped_amplification = mapped_bytes / max(requested_bytes, 1)
observed_rss_amplification = observed_rss_bytes / max(requested_bytes, 1)
~~~

原 `usable_bytes / requested_bytes` 更适合称为 usable amplification，而不是“内部碎片率”。离峰时以当前 live bytes 为分母会产生很大比率，必须同时报告 peak-demand realized fragmentation，避免把正常缓存回落误判为峰值内存问题。

### 9.2.4 一致性等级

- **Relaxed live counters**：无全局停顿，适合在线趋势；
- **Epoch snapshot**：读取前后 generation，相同则近似一致；
- **Quiescent exact snapshot**：测试/shutdown 时冻结各层并逐项校验；
- **OS observation**：低频读取 RSS、PSI、cgroup，时间点不与内部快照严格同步。

控制接口必须标注快照类型。

## 9.3 页状态与可回收性

| 状态 | VA | 物理页候选 | 可直接分配 | 可 coalesce | 回收动作 |
|---|---|---|---|---|---|
| Active | 保留 | 可能驻留 | 否 | 否 | 等待对象归还 |
| FreeCommitted/Dirty | 保留 | 可能驻留 | 是 | 是 | decay 后 purge |
| DetachedForPurge | 保留 | 变化中 | 否 | 否 | 锁外 madvise |
| FreePurged/Muzzy | 保留 | 可按需重新 fault | 是 | 是 | 复用或继续 retain |
| Retained | 保留 | 策略相关 | 取决于索引 | 同 policy 可合并 | budget 超限 unmap |
| DetachedForUnmap | 即将撤销 | 不可交付 | 否 | 否 | clear PageMap + munmap |
| Unmapped | 已归还 | 无 | 否 | 否 | descriptor retire |

`MADV_DONTNEED` 成功只表示内核接受了建议；实际 RSS 回落量和时间仍应通过 OS 观测验证。对 THP-backed 范围，部分 purge 可能造成 hugepage split，应单独计入 breakage。

## 9.4 当前 Scavenger 基线与差距

已有基础：

- `std::jthread` 和 stop token；
- `Stop()` 立即 join，形成明确静止点；
- wait mutex 不跨 PageCache 扫描；
- Span 在锁下从 bucket 摘除并标记 reserved；
- `madvise` 在 PageCache 锁外；
- 成功后更新 committed flag，再重新入桶。

主要差距：

- 通过 legacy `GetSpanList/GetMutex` 只支持 active shard 为 1；
- 固定扫描全部 128 个 bucket，单轮工作无 pages/time 上限；
- 固定 1 秒周期和 10 秒 idle threshold；
- 每个符合条件的 Span 都同步 `MADV_DONTNEED`，未批量合并相邻范围；
- detached 状态复用 `IsUsed=true`，与真实 active allocation 语义混淆；
- `last_used_time_ms` 在重新入桶时重置，会掩盖 purge 后长期空闲年龄；
- madvise 成功后在锁外写非原子 Span flags，必须由显式 detached 独占协议证明；
- 失败路径用 spdlog，未来 interposition 不自举安全；
- 无 cgroup、PSI、allocator budget 或上层通知；
- 无 dirty/purged/retained 字节预算和 refault feedback；
- Start 的一次失败永久禁止重试，但没有状态/诊断控制；
- 测试无法精确触发单 pass、模拟时钟或注入 madvise 结果。

## 9.5 Scavenger 生命周期

### 9.5.1 状态机

~~~text
kNotStarted
  -> Start -> kStarting
  -> success -> kRunning
  -> failure -> kDisabled or kRetryableFailure

kRunning
  -> pressure wake / timer wake / explicit wake
  -> kStopping -> joined -> kStopped

fork prepare -> kForkFrozen
parent -> previous state
child -> kNotStarted or kDisabled
~~~

状态发布使用显式 acquire/release；只有控制路径访问，不进入普通 allocation fast path。

### 9.5.2 启动策略

- 显式 API 模式可按配置 lazy start；
- malloc interposition 模式必须在 bootstrap 安全后启动；
- 创建后台线程失败不影响基本分配，但要记录可查询状态；
- 是否允许退避重试由控制面决定，不能每次慢路径重试；
- 默认最多一个全局协调线程，后续可按 NUMA node 分 worker；
- worker 数上限与 shard/node 数解耦，避免大型机器线程爆炸。

### 9.5.3 Shutdown

顺序：

1. 禁止新的后台 wake；
2. request stop 并唤醒；
3. join worker；
4. 等待所有 detached purge work item 提交/回滚；
5. 再执行 PageCache/CentralCache destructive drain；
6. 最终释放控制 metadata。

不得在 PageCache 已销毁后让 TLS 或后台线程重新进入 allocator。

### 9.5.4 Fork

- prepare 阶段冻结 worker 并等待当前 pass 结束；
- parent 恢复之前的运行状态；
- child 清除继承的 jthread/condvar 运行假象；
- child 只在显式重新启用时创建新 worker；
- atfork handler 不分配、不记录复杂日志。

## 9.6 唤醒源与压力等级

### 9.6.1 唤醒源

- decay deadline；
- PageCache free committed bytes 超软限；
- Frontend/Middle-end budget 超限；
- cgroup `memory.events(.local)` 变化；
- PSI `memory.pressure` 阈值触发；
- 上层 `NotifyMemoryPressure(level)`；
- 显式 `PurgeArena/PurgeAll`；
- OOM 前的有界同步回收请求；
- shutdown/fork。

文件监控和 epoll 由独立控制线程执行，allocator 热路径只观察压力 generation。

### 9.6.2 压力等级

| 等级 | 动作 |
|---|---|
| Normal | 正常 decay、保留复用价值高的 committed extent |
| Soft | 停止预取/容量增长，缩 Frontend/Middle-end，提前 purge |
| Hard | 大幅 drain cache，bypass retained cache，优先 unmap 大冷 extent |
| Critical | 有界同步协助回收，禁用可选 profiling/quarantine 增长 |
| OOM recovery | 只执行可证明、有限且不分配 metadata 的回收，然后最多重试一次 |

状态升级快、降级慢，并设置滞回与最小保持时间。

### 9.6.3 信号合并

多个压力源合并为单调 generation + 当前最高等级。worker 处理完一轮后重新读取；不为每个事件排队动态对象，也不丢失更高等级。

## 9.7 Dirty/Purged Decay

### 9.7.1 Decay 目的

固定“空闲 10 秒后全部 purge”会在突发 workload 中形成批量 syscalls。建议采用近似 decay：

- 为 free committed bytes 按时间窗口分桶；
- 每个 epoch 只 purge 已到期比例；
- 曲线首尾速率较低，减少突发；
- reuse 时从对应 age bucket 撤销；
- pressure 可提升 decay 速率；
- decay=-1 表示禁用自动 purge，0 表示尽快 purge。

### 9.7.2 无分配时间轮

每 shard 使用固定数量 age buckets：

~~~text
epoch 0 ... epoch N-1
  intrusive list of free extent candidates
~~~

Span 只能同时位于 size/address 索引和一个 age 结构时，需要独立 hooks 或 side metadata；不能复用唯一 `next/prev` 导致双重链表。另一方案是仅存 last-free epoch，在 size index 上使用有界 cursor 扫描。

### 9.7.3 Decay 参数

- dirty decay ms；
- retained/unmap decay ms；
- minimum purge extent bytes；
- per-pass pages/syscalls/time；
- pressure multiplier；
- refault penalty window；
- per-node overrides。

参数动态更新后使用 generation 生效，不回溯修改所有 Span。

### 9.7.4 Refault 反馈

统计 purge 后在短窗口内重新分配/触页的字节。高 refault 说明 decay 太激进；低 refault 且 RSS 高说明可加快。调整在控制线程进行，每周期限幅。

## 9.8 Purge 模式与系统语义

### 9.8.1 `MADV_DONTNEED`

- 成功后后续匿名页读取呈零；
- 通常适合需要确定零语义和明确回收的 free extent；
- 会产生后续 fault；
- 部分 THP 范围可能破坏 hugepage；
- 仅在逻辑 free、detached 的页上使用。

### 9.8.2 `MADV_FREE`（可选）

- 允许内核延迟回收，页在真正回收前可能仍保留旧内容；
- 复用前不能据此满足 calloc 零语义；
- RSS 可能不立即下降；
- 需要单独 `muzzy` 状态和平台探测；
- 初始实现可以不启用，避免状态语义混淆。

### 9.8.3 Unmap

适用于：

- DirectMapped 释放；
- retained VA 超限；
- 大而冷的完整 region；
- VMA 数与地址空间策略允许；
- pressure hard/critical。

unmap 前遵循[第 8 章](07-backend-pagecache-large-object.md)的 PageMap clear 和 descriptor retire。

### 9.8.4 相邻范围批量

worker 在锁下摘取多个相邻、同 policy extent，锁外合并为有限 work item，减少 madvise 调用。批量不能跨 active、guard、region、node 或 backing policy 边界。

## 9.9 Detached 回收事务

### 9.9.1 Detach

在 owner shard 锁下：

1. 选择符合 age/pressure 的 free extent；
2. 从 allocatable index 和 decay candidate 中移除；
3. 标记 `kDetachedForPurge`；
4. 保存 start/pages/generation/原状态；
5. 将 descriptor 放入栈上或固定 work array；
6. 释放锁。

### 9.9.2 OS 操作

- 仅操作 work item 指定范围；
- 不读取可能变化的 Span 链；
- 捕获 errno 到固定结果；
- 记录 syscall time；
- 相邻范围按上限合并；
- 不调用普通 allocator。

### 9.9.3 Commit/Rollback

重新持 owner shard 锁：

- 验证 descriptor generation 和 detached 状态；
- 成功：转 `kFreePurged`，插入相应 index；
- 失败：恢复 `kFreeCommitted`；
- shutdown closing：保持 detached，交给 unmap/drain；
- 更新 bytes 和 failure counters；
- 不把 purge 时间误写成 free/reuse 年龄。

### 9.9.4 并发约束

detached extent 不能：

- 被 exact/split 分配；
- 被相邻 free coalesce；
- 被另一轮 Scavenger 重复选择；
- 被 PageCache reset 释放而 worker 仍在 syscall；
- 改变 owner shard。

## 9.10 增量扫描与工作预算

每个 shard 保留：

- next size bucket；
- next region/extent cursor；
- decay epoch；
- last scan time；
- pending pressure generation；
- committed/purged bytes hint。

单轮预算建议同时限制：

| 预算 | 目的 |
|---|---|
| max shards | 避免大型机器全扫描 |
| max extents | 限制锁内摘取 |
| max pages/bytes | 限制 fault/RSS影响 |
| max syscalls | 限制内核开销 |
| max wall time | 控制后台 CPU 和 shutdown 响应 |
| max lock hold per shard | 控制分配尾延迟 |

预算耗尽后保存 cursor 并再次调度，不从头扫描。

## 9.11 分层 Cache 排空

### 9.11.1 顺序

~~~text
stop growth/prefetch
  -> Frontend trim/flush idle caches
  -> remote queues drain
  -> TransferCache drain to Span bitmap
  -> empty Central Span return PageCache
  -> PageCache committed purge
  -> retained extent/region unmap
  -> huge VMA cache drain
~~~

跳过上层直接 purge PageCache 不能回收仍被 Frontend/Middle-end 占用的 Span。

### 9.11.2 同步与异步

- Soft pressure：全部异步；
- Hard：调用线程最多协助有限 batch/page；
- Critical/OOM：执行不需要新 metadata 的同步 drain；
- 显式 purge API 可提供 async token 或 blocking 模式；
- blocking 模式必须注明不会保证 OS RSS 立即下降。

### 9.11.3 防振荡

- pressure 下 Frontend refill batch 和 Middle-end prefetch 同步收缩；
- 回收后保留 cooldown；
- cache 重新增长采用 slow-start；
- 记录 `purged_then_refilled_bytes`；
- 只有压力解除且命中收益持续才恢复容量。

## 9.12 cgroup、PSI 与容器环境

### 9.12.1 cgroup v2 输入

建议低频解析：

- `memory.current`；
- `memory.high`；
- `memory.max`；
- `memory.events.local` 的 high/max/oom/oom_kill；
- `memory.stat` 的 anon、pgfault、pgmajfault 等；
- `memory.pressure`。

解析按 key，不依赖行顺序；处理 `max`、权限失败、容器迁移和 cgroup 路径变化。

### 9.12.2 解释

- 超过 `memory.high` 会触发节流和重回收，不等于 OOM；
- `memory.events` 计数变化是事件，不是当前压力等级；
- PSI 衡量 stall，不等于 allocator 可回收字节；
- `memory.current` 包含进程/cgroup 其他内存，不应全部归因 allocator；
- 管理器提供的显式预算可比自动猜测更可靠。

### 9.12.3 读取架构

后台 monitor 使用预打开 fd、固定 buffer 和无分配 parser。热路径只读取 cache-line 隔离的 pressure generation/level。不可从 malloc/free 内调用 fopen、fstream 或构造 string。

### 9.12.4 主动回收接口

如果支持 cgroup `memory.reclaim`，它是外部管理面能力，不应由 allocator 默认写入；allocator 自身只回收自己拥有的缓存。aethermind 管理器可在更高层协调 allocator purge 与系统 reclaim。

## 9.13 预算与策略控制

### 9.13.1 预算层级

- process allocator budget；
- NUMA node budget；
- arena/model/request budget；
- Frontend total budget；
- Middle-end budget；
- PageCache committed/retained budget；
- metadata budget；
- hugepage/hugetlb budget。

父级预算必须覆盖子级，不允许每层独立扩大后总和失控。

### 9.13.2 Soft/Hard limit

- soft limit 触发渐进收缩；
- hard limit 禁止增长并触发同步有限回收；
- limit 以 bytes 为主，slot/Span 数为辅助；
- 配置小于不可回收 live bytes 时报告 unsatisfiable，不循环 purge；
- 预算变更使用 generation 发布。

### 9.13.3 受益评分

缓存保留优先级可依据：

- 最近命中次数/每字节；
- refill/syscall 避免成本；
- refault 代价；
- NUMA locality；
- hugepage 完整性；
- extent age；
- workload tag/lifetime。

复杂评分仅在控制路径运行。

## 9.14 可观测性与报警

必须报告：

- 各层 cached/active/free committed/purged/retained bytes；
- purge eligible、detached、quarantined bytes；
- decay runs、pages、syscalls、time；
- explicit/pressure/OOM-triggered reclaim；
- madvise/unmap success/failure；
- purge-to-reuse/refault 时间分布；
- RSS before/after（带采样时间）；
- cgroup current/high/max 与 events delta；
- PSI some/full；
- Scavenger state、last run、last progress、stalls；
- per-shard cursor 和 lock hold samples；
- THP breakage 与 huge cache drain。

报警条件包括：

- hard pressure 下多轮无 reclaim progress；
- detached bytes 长期不归零；
- madvise/munmap 连续失败；
- purge 后高 refault；
- retained VA 超 hard limit；
- Scavenger 已启用但线程不运行；
- shutdown 超时；
- allocator accounting 明显不守恒。

## 9.15 测试与故障注入

### 9.15.1 确定性测试设施

- 注入 monotonic clock；
- 暴露 test-only `RunOnePass(budget)`；
- mock madvise/munmap 结果；
- 禁止测试依赖真实等待 10 秒；
- 可查询 detached 和 index 状态；
- reset 前自动 stop/join worker。

### 9.15.2 单元测试

- age threshold 前后；
- cursor continuation；
- committed -> detached -> purged；
- madvise 失败回滚；
- shutdown 时 detached work；
- 多 shard round-robin；
- 相邻范围批量但不跨 region；
- soft/hard/critical 动作；
- budget 不可满足；
- purged extent reuse；
- pressure generation 滞回；
- byte accounting 守恒。

### 9.15.3 并发测试

- allocate/split/coalesce 与 purge 并发；
- Central empty Span 返回与 Scavenger detach；
- reset/stop/wake 竞争；
- fork prepare 时 worker 正在 syscall；
- cgroup monitor 更新与控制线程读取；
- TSan 验证 Span flags/state 不在锁外竞态访问。

### 9.15.4 压力测试

- burst 分配后 idle；
- 周期性 burst，检查振荡；
- cgroup `memory.high`；
- 低 memory.max/OOM；
- THP on/off；
- NUMA node pressure；
- 长时间 churn；
- aethermind model load -> steady inference -> unload。

## 9.16 性能与内存基准

场景：

- Scavenger disabled/enabled idle overhead；
- 1/4/16 shard incremental scan；
- 1KiB～1GiB purge range；
- batched vs per-extent madvise；
- committed reuse vs purged refault；
- immediate、1s、10s、adaptive decay；
- retained vs unmap；
- cgroup soft/hard pressure；
- THP breakage；
- Frontend/Middle-end drain；
- aethermind burst/RSS 回落。

采集：

- allocation p50/p99/p999；
- background CPU；
- lock hold/wait；
- purge syscalls和 bytes/s；
- minor/major faults；
- RSS/VSS 时间序列；
- retained/committed/purged；
- refault ratio；
- tokens/s 与请求尾延迟。

## 9.17 分阶段实施与验收

### 阶段 A：状态、生命周期与统计基线

1. 引入显式 detached/purged 状态；
2. 修复多 shard Scavenger 路由；
3. 可注入时钟和单 pass；
4. 确定性 Stop/fork/reset；
5. 建立字节口径与 quiescent 守恒快照；
6. 移除后台失败路径对普通 allocator 日志的依赖。

退出条件：

- 多 shard allocate/release/purge 无竞态；
- detached extent 不可被分配/合并；
- shutdown/fork 无悬挂线程；
- ASan/TSan 和 accounting 测试通过；
- disabled 模式对性能基线无影响。

风险类型：并发、正确性、内存。

### 阶段 B：增量 Decay 与有界工作

1. per-shard cursor；
2. dirty/purged decay；
3. pages/syscalls/time budget；
4. 相邻范围批量；
5. refault feedback；
6. retained/unmap 阈值。

退出条件：

- 单轮工作严格受限；
- burst 后 RSS 按目标曲线回落；
- purge syscall 和 p999 无尖峰；
- refault/吞吐权衡优于固定 10 秒全扫；
- THP breakage 可解释。

风险类型：性能、内存、并发。

### 阶段 C：压力控制与分层排空

1. pressure generation；
2. Frontend/Middle-end/PageCache 协同 drain；
3. cgroup events/PSI monitor；
4. hard/critical 有界同步回收；
5. node/arena budgets；
6. 显式 purge/control API。

退出条件：

- `memory.high` 下缓存主动下降且无回收风暴；
- OOM retry 路径无递归和无界等待；
- pressure 解除后容量平稳恢复；
- allocator 可回收量与实际 RSS 改善具有关联数据。

风险类型：内存、性能、兼容性。

### 阶段 D：aethermind 自适应治理

1. model/request/KV arena pressure priority；
2. model unload 主动 purge；
3. worker/node 局部回收；
4. workload-aware decay；
5. 管理面告警和自动回滚。

退出条件：

- 真实服务 burst 后 RSS、tokens/s、p99 综合收益明确；
- 不同模型/arena 的回收互不错误干扰；
- pressure 策略可灰度、可关闭、可回退。

风险类型：内存、性能、运维。
