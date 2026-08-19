# 第 11 章：可观测性与 Profiling

> [总索引](README.md) · [上一章](09-numa-and-aethermind.md) · [下一章](11-security-hardening.md)  
> **本章目标**：建设不递归、低开销、可归因的统计、采样与诊断体系。  
> **适用范围**：stats、control ABI、sampling、heap profile、event ring 与 exporter。  
> **核心 invariant**：默认关闭的观测能力近零开销；控制与采集路径不得递归进入 allocator。

可观测性必须先回答“字节在哪里、为什么保留、谁在竞争、哪个调用栈长期存活”，再提供动态调参。控制面是 allocator ABI 的一部分，错误设计会引入递归、全局锁、ABI 漂移和不可复现的生产行为。

## 11.1 目标与原则

- 默认计数成本可预测；
- 普通 Frontend fast path 不进行共享原子统计写；
- 所有接口可在 malloc interposition 下无递归运行；
- 快照明确 relaxed/epoch/quiescent 一致性；
- 指标名、类型、单位和版本稳定；
- 高基数数据通过 sampling，不按每请求精确记账；
- 控制变更可审计、可回退；
- 输出由调用方提供 buffer；
- profiling 可动态启停；
- 统计关闭时核心结构布局和性能仍受测试。

## 11.2 当前基线与差距

当前只有 `PageAllocatorStats` relaxed 原子计数，能够统计 mmap/huge/free/failure，但：

- 没有 Frontend/Central/PageCache 字节统计；
- 没有统一快照或导出接口；
- 无 requested/usable/live；
- 无 lock、fragmentation、RSS、NUMA；
- counters 是多个独立 atomic，不能构成一致快照；
- 无 profiling/sampling；
- 无控制 namespace；
- RuntimeConfig 只从三个环境变量一次性读取；
- core 仍依赖 spdlog；
- 没有版本/size negotiation；
- 测试宏作为 PUBLIC compile definition 传播，可能改变公共头布局；
- 无统计开销基准。

## 11.3 指标层级

### 11.3.1 Level 0：始终开启

- fatal/OOM/syscall failure；
- mapped/unmapped bytes；
- current cache bytes（能以 owner-local 普通字段维护时）；
- high-water；
- allocator state/version；
- pressure level；
- active shard/node/arena 数。

### 11.3.2 Level 1：低成本详细统计

- per-size-class hit/miss/batch；
- lock contention sample；
- split/coalesce/refill；
- purge/refault；
- remote free；
- NUMA fallback；
- large size histogram；
- metadata arena。

通过构建或运行期开关启用，更新在已进入的慢路径或 shard-local 数据中完成。

### 11.3.3 Level 2：Allocation Sampling

- sampled requested/usable；
- allocation time、thread/CPU/node/arena/tag；
- sampled stack id；
- lifetime；
- live/peak bytes；
- sampled fragmentation；
- sampled realloc path。

### 11.3.4 Level 3：Guarded/Hardened Sampling

- guard pages；
- quarantine；
- redzone/canary；
- delayed reuse；
- crash record；
- 高成本 stack capture。

默认关闭或极低采样。

## 11.4 指标数据模型

指标描述包含：

~~~text
id, canonical_name, type, unit, scope, consistency,
monotonic_or_gauge, introduced_version, feature_requirement
~~~

Scope：

- process；
- thread/CPU；
- size class；
- Central/PageCache shard；
- NUMA node；
- arena；
- hugepage/region；
- sampled tag。

不通过字符串拼接动态生成内部指标；使用 numeric id + 固定 schema，导出层再格式化。

## 11.5 Counter、Gauge 与 Histogram

### 11.5.1 Counter

单调事件：alloc calls、miss、syscalls、failures。使用 `uint64_t` relaxed，考虑溢出自然回绕并在快照层计算 delta；控制接口标明是否从启动累计。

### 11.5.2 Gauge

当前 bytes/objects/spans。若由单 owner 维护，用普通字段并在 snapshot 锁/epoch下读取；不要为了在线读取把所有字段变成 atomic。

### 11.5.3 High-water

慢路径 CAS max 或 owner-local 更新。普通 fast path 的 ThreadCache high-water 可在线程退出/采样时汇总。

### 11.5.4 Histogram

固定指数 buckets：

- allocation size；
- batch；
- lock wait/hold；
- syscall latency；
- allocation latency sample；
- lifetime；
- refault interval。

避免 HDR histogram 等动态分配结构进入核心。

## 11.6 缓存行与聚合

- 每线程/CPU 热计数与对象指针数组隔离；
- per-shard counters 与 lock state 分离，避免读 exporter 干扰写热点；
- process global counters 按 CPU/shard 分片；
- exporter 聚合，不在更新时全局 fetch_add；
- 低频项可直接 global relaxed atomic；
- 结构用 `static_assert(sizeof/alignof/offsetof)` 验证；
- perf c2c 验证统计开关的伪共享。

## 11.7 快照协议

### 11.7.1 Relaxed

逐字段读取，允许跨时间。适合监控。

### 11.7.2 Generation/Epoch

~~~text
g0 = stats_generation acquire
read shard snapshots
g1 = stats_generation acquire
if g0 == g1: approximately consistent
else retry at most once or mark inconsistent
~~~

控制线程在结构级变化后递增 generation。不能因反复变化无限重试。

### 11.7.3 Quiescent

测试、诊断或 shutdown：

- 停止新请求；
- drain/冻结各层；
- 按锁序读取；
- 可遍历对象/Span；
- 执行守恒；
- 不用于在线监控。

### 11.7.4 Stats epoch

类似“刷新统计视图”的 epoch 不应清空真实累计 counter。可维护 snapshot cache generation，调用者显式 refresh 后查询同一代数据。

## 11.8 无分配控制 ABI

### 11.8.1 Typed C ABI

建议：

~~~text
am_control_get(id, scope, out, inout_size)
am_control_set(id, scope, value, value_size)
am_stats_snapshot(request, caller_buffer, inout_size)
am_stats_schema(caller_buffer, inout_size)
~~~

返回：

- OK；
- BUFFER_TOO_SMALL（同时返回所需大小）；
- NOT_SUPPORTED；
- INVALID_ARGUMENT；
- READ_ONLY；
- BUSY；
- OUT_OF_MEMORY；
- VERSION_MISMATCH。

### 11.8.2 字符串 namespace

可提供 mallctl 风格薄适配，但内部先将固定名称解析为 numeric id。解析使用 caller string_view 和无分配 trie/table，不构造 `std::string`。

### 11.8.3 JSON/Text

- 调用方传 buffer + writer callback；
- 支持 dry-run size estimate；
- 输出被截断时返回明确状态；
- writer callback 必须声明是否允许重入 allocator；
- 最安全默认是 allocator 只写 caller buffer；
- JSON schema 带版本；
- 人类文本不是机器稳定 ABI。

## 11.9 配置控制面

配置分三类：

| 类型 | 示例 | 变更时机 |
|---|---|---|
| Structural | max shards、region geometry、PageMap mode | 构建/启动期 |
| Policy | cache budget、decay、sampling | 运行期 |
| Action | purge、flush CPU、dump profile | 命令 |

运行期 set 流程：

1. 校验类型、范围、feature；
2. 创建不可变 config snapshot；
3. release 发布 generation；
4. 慢路径 acquire 观察；
5. 记录 old/new、时间和来源；
6. 失败不修改旧配置；
7. 提供恢复默认值。

不能用任意字符串环境变量直接修改结构性配置。

## 11.10 Allocation Sampling

### 11.10.1 采样方法

推荐基于分配字节的几何间隔，而非每 N 次固定采样：

- 每线程/CPU 保存 `bytes_until_sample`；
- fast path 只减本地计数并检查分支；
- 到期进入 noinline sample slow path；
- 下一个间隔由轻量 PRNG 产生；
- 平均 interval 可配置，如 512 KiB 起步；
- sized free 或 PageMap metadata 用于结束 sample 生命周期。

### 11.10.2 Sample record

- allocation id/generation；
- ptr 或稳定 sample key；
- requested/usable；
- timestamp/CPU/thread/node/arena/tag；
- stack id；
- large/small/class；
- live/freed；
- weight。

records 来自预分配 ring/pool，耗尽时 drop 并计数，不回退普通 heap。

### 11.10.3 偏差校正

按采样概率加权估算 live bytes 和调用栈分布；对超大对象可强制采样。输出同时报告 sample count、drop 和置信限制，不能把估计值伪装为精确值。

## 11.11 调用栈与符号化

- allocator 内只采集原始 PC；
- unwind 可选 frame-pointer 或预验证的无分配 unwinder；
- 不在分配路径符号化；
- stack depot 使用固定/arena-backed hash table；
- 表满后降级 drop/aggregate；
- 符号解析在外部工具或安全控制线程；
- 避免 loader lock/reentrant malloc；
- build id、binary mapping 在 profile 导出时关联。

## 11.12 Heap Profile 与泄漏诊断

支持：

- live sampled bytes by stack/tag/arena；
- allocation count/bytes；
- lifetime histogram；
- peak snapshot；
- growth between epochs；
- model/request scope；
- dump to caller fd/buffer；
- thread-level profiling enable；
- reset profile epoch；
- dropped samples。

泄漏判断基于长期增长和生命周期，而不是“进程退出未 free”。stable metadata 和 intentional singleton bytes 单独分类。

## 11.13 Event Ring 与错误诊断

固定容量 lock-free/per-shard ring 记录低频关键事件：

- OOM；
- mmap/madvise/munmap failure；
- invalid free/hardening；
- config change；
- pressure transition；
- Scavenger stall；
- invariant violation；
- fallback/feature disable。

每条固定大小，包含 timestamp、thread/cpu、code、最多若干整数；不保存动态 string。覆盖旧事件并计 drop/overwrite。

## 11.14 Export 与生态集成

优先级：

1. C ABI snapshot；
2. 文本/JSON caller buffer；
3. Prometheus/OpenTelemetry 由 aethermind adapter 读取；
4. profile 文件由外部工具符号化；
5. debugger pretty-printer/diagnostic command。

allocator 不直接启动 HTTP server、不依赖 protobuf/JSON 库、不在核心链接日志框架。

## 11.15 观测开销预算

设定门禁：

- Level 0 对 3.8 ns fast path 无统计显著退化；
- Level 1 在 Middle/Backend 慢路径开销可量化；
- sampling 默认间隔下整体吞吐退化目标 <1%（最终以实测阈值为准）；
- stack capture 单独报告；
- exporter 不应阻塞 allocator shard；
- stats reader 高频运行时不引起 cache-to-cache 激增；
- 内存开销受 metadata/profile budget 限制。

## 11.16 测试

- counter/gauge 守恒；
- histogram 边界；
- atomic rollover；
- relaxed/epoch snapshot；
- buffer-too-small；
- schema/version；
- unknown/read-only control；
- concurrent get/set；
- sampling interval 分布；
- sample free 生命周期；
- stack depot exhaustion；
- profile drop；
- fork/reset；
- reentrant writer callback 负向测试；
- stats on/off 性能；
- TSan；
- fuzz control parser 和 exporter。

## 11.17 分阶段实施与验收

### 阶段 A：统一统计 Schema

1. 定义字节口径；
2. numeric metric id/schema；
3. Level 0 counters/gauges；
4. caller-buffer snapshot；
5. quiescent守恒；
6. 移除核心日志依赖。

退出条件：各层字节可解释；接口无分配；stats-off 基线通过。

风险类型：正确性、性能、兼容性。

### 阶段 B：控制面与在线快照

1. typed get/set；
2. config snapshot/generation；
3. purge/flush action；
4. epoch snapshot；
5. fixed event ring；
6. aethermind exporter adapter。

退出条件：并发控制无全局停顿；错误/版本语义稳定；可回滚。

风险类型：并发、兼容性、运维。

### 阶段 C：Allocation Sampling

1. byte-geometric sampler；
2. record pool；
3. stack depot；
4. live/lifetime profile；
5. tag/arena；
6. overhead benchmark。

退出条件：sample 估计可验证；drop/预算可见；默认开销满足门禁。

风险类型：性能、内存、可观测性。

### 阶段 D：Guarded Profiling 与生产诊断

1. guarded sampling；
2. crash/event record；
3. external symbolization；
4. peak/growth profile；
5. 自动告警与灰度。

退出条件：真实问题可定位；安全模式不污染默认 fast path；生产可动态关闭。

风险类型：安全、性能、运维。

