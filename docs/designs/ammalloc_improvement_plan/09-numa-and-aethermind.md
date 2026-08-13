# 第 10 章：NUMA 与 aethermind 集成

> [总索引](README.md) · [上一章](08-rss-fragmentation-and-scavenging.md) · [下一章](10-observability-and-profiling.md)  
> **本章目标**：为 NUMA-local 分配和 aethermind 生命周期化内存提供可落地接口。  
> **适用范围**：NUMA topology/policy、node-local arena、模型/KV/workspace 和 device memory 边界。  
> **核心 invariant**：内存 policy 可兑现、可回退，allocation/free 始终由原 owner domain 正确处理。

ammalloc 作为 aethermind 的底层分配器，应同时提供“普通进程堆”和“显式推理内存设施”，但两者不能混成一套隐含策略。标准 malloc ABI 保持可移植、安全默认值；NUMA、生命周期、hugepage、arena reset 和 allocation tag 通过独立扩展 API 提供。

## 10.1 集成目标与非目标

目标：

- 普通 C/C++ 对象可透明使用标准 ABI；
- 模型、请求、KV cache、workspace 和 runtime metadata 按生命周期治理；
- CPU worker、CentralCache、PageCache region 和 first touch 尽量 node-local；
- 提供显式 alignment、page policy、purge/reset 和统计；
- CPU heap 与 device/pinned/IPC memory 具有清晰 allocator domain；
- 所有扩展能力可回退默认 heap；
- 用 tokens/s、TTFT、TPOT、p99 和 RSS 验收，而非只看 malloc ns。

非目标：

- 用普通 malloc 管理 GPU device memory；
- 在标准 free 中自动猜测模型或请求生命周期；
- 自动迁移已交付用户的 live object；
- 不考虑 worker affinity，仅靠 mbind 解决 NUMA；
- 默认要求 hugetlb；
- 将 arena reset 用于仍有异步任务持有对象的生命周期。

## 10.2 集成不变量

| 编号 | 不变量 |
|---|---|
| AI-1 | 标准 ABI 分配可由普通 `free` 释放，扩展 domain 释放规则明确 |
| AI-2 | arena/domain id 在 allocation epoch 内稳定 |
| AI-3 | request/model reset 前所有使用者和异步任务已静止 |
| AI-4 | NUMA 请求 node 与实际 owner node 分别记录，fallback 不伪装成功 |
| AI-5 | free 返回 allocation owner，而非释放线程所在 node |
| AI-6 | CPU heap 不接管 CUDA/device/pinned allocator 的原始指针 |
| AI-7 | first-touch、worker affinity 和 memory policy 共同决定 locality |
| AI-8 | model/request/tag 统计不在普通 fast path 做高基数共享更新 |
| AI-9 | 生命周期 hint 只影响策略，不放宽正确性和 ABI |
| AI-10 | arena reset 是批量生命周期终止，不逐对象运行任意析构 |
| AI-11 | 所有扩展 metadata 由 ammalloc 自有 arena 管理 |
| AI-12 | 功能不可用时有明确 unsupported/fallback 结果 |

## 10.3 当前仓库基线与差距

当前 ammalloc 只有 `am_malloc/am_free`，没有：

- arena/domain handle；
- NUMA topology 或 node policy；
- requested vs actual node；
- alignment/page policy 扩展；
- batch/reset/purge API；
- allocation tag；
- profiling/control API；
- device/pinned domain 识别；
- aethermind 集成代码和真实 trace 基准。

生产 PageCache 仍固定 shard 0，物理页依赖 first touch；因此 NUMA 接口必须在[第 7 章：Middle-end](06-middle-end.md)和[第 8 章：Backend](07-backend-pagecache-large-object.md)的多 shard/region owner 稳定后启用，不能先暴露无法兑现的 API。

## 10.4 NUMA 拓扑发现

### 10.4.1 启动快照

初始化时在非递归阶段读取：

- online CPUs；
- possible/online NUMA nodes；
- CPU -> node 映射；
- node memory availability；
- cpuset allowed CPUs/mems；
- cgroup/cpuset 约束；
- hugepage availability；
- 页大小与 THP policy。

数据写入固定上限 topology snapshot；不使用动态 STL。

### 10.4.2 动态变化

CPU/node hotplug、cpuset 调整和容器迁移通过低频 generation 更新。普通 fast path 只读取稳定映射或回退默认 node；更新不能释放仍被 reader 使用的 snapshot。

### 10.4.3 不可用回退

- 单 node：所有 route 为 node 0；
- 无权限：使用 CPU affinity + first touch，标记 policy unknown；
- CPU 不在 snapshot：回退默认 arena；
- 请求 node 不在 allowed mems：返回 fallback 或 strict failure；
- 非 Linux：扩展 API 返回 unsupported，标准 allocator 正常工作。

## 10.5 NUMA policy 语义

建议支持：

| Policy | 语义 |
|---|---|
| Default | 由默认 arena/first touch 决定 |
| Preferred(node) | 优先 node，允许实际 fallback |
| Bind(nodes) | 只允许 nodemask，失败语义严格 |
| Interleave(nodes) | 页在 node 集合间交错 |
| Local | 以调用时 CPU node 为 preferred |
| InheritArena | 使用 arena 固定 policy |

`MPOL_BIND`、preferred 与 interleave 的内核语义不同；API 必须返回实际应用的 policy/fallback。不要通过并发不安全地临时修改进程级 policy 包围 mmap。

## 10.6 Arena 模型

### 10.6.1 ArenaDescriptor

至少包含：

- arena id + generation；
- kind：default/model/request/KV/workspace/metadata；
- NUMA policy 和 allowed nodes；
- page/hugepage policy；
- soft/hard byte budget；
- lifetime/reset policy；
- Frontend/Middle-end cache profile；
- Backend region set；
- pressure priority；
- statistics handle；
- closing/quiescent state。

Arena metadata 来自固定 registry/arena pool。

### 10.6.2 Arena ownership

- allocation 返回的 Span/Extent 记录 arena id；
- free 通过 PageMap 恢复 arena/owner；
- arena 关闭后拒绝新分配；
- closing 期间逐步 drain Frontend/Middle-end；
- reset 只有在无 live/borrowed allocation 时提交；
- destroy 后 generation 变化，旧 handle 失效；
- default arena 可选择进程生命周期常驻。

### 10.6.3 Arena 数量

不要为每个请求创建完整 PageCache/CentralCache。建议：

- 少量长期 arena 拥有独立 region/budget；
- request arena 使用轻量子 arena/segment；
- 高基数 request id 作为 tag/sample，不成为全套 shard；
- arena 上限固定，可配置 exhaustion fallback。

## 10.7 Worker 与内存亲和性

### 10.7.1 Worker route

aethermind worker 初始化时：

1. 固定或确认 CPU affinity；
2. 获取 CPU node；
3. 绑定 Frontend route；
4. 选择 node-local Central/PageCache arena；
5. 由实际访问线程完成 first touch；
6. worker 迁移时按策略保留原 route 或在安全点重绑定。

### 10.7.2 First touch

仅 mmap/mbind 不保证已驻留页。模型加载可由目标 node worker 并行触页；请求热路径避免大范围同步 prefault。记录 fault time 和实际 NUMA placement。

### 10.7.3 线程迁移

- 固定 worker 推荐保持 route；
- 短暂调度迁移不立即重绑定；
- 持续迁移超过阈值在慢路径更新 Frontend route；
- active object/Span owner 不迁移；
- remote access 与 cache-to-cache 数据决定是否值得更新。

## 10.8 Model Arena

适合模型权重索引、CPU staging、长期元数据和可按模型整体卸载的资源。

设计：

- 每模型一个逻辑 arena handle，但可共享 node-local Backend；
- model id 只在采样/聚合层保存，不为每次分配写高基数全局表；
- 初始化阶段允许较大 batch、THP prefer、有限 prefault；
- steady phase 收缩临时 cache；
- unload 先停止请求和异步任务，再 reset/purge；
- 权重本体若来自 mmap 文件或专用 tensor runtime，不强行复制进匿名 heap；
- 多副本按 node 分配并由对应 worker first touch；
- model arena hard limit 与进程 budget 联动。

验收：

- model load time；
- first-token latency；
- steady RSS；
- observed THP/NUMA placement；
- unload 后 RSS/retained 回落；
- 多模型并存的隔离性。

## 10.9 Request Arena

### 10.9.1 适用对象

- 解析/调度临时结构；
- shape/plan scratch metadata；
- 生命周期严格包含在请求内的小对象；
- 可批量销毁且不需要逐对象 free 的 POD/平凡析构对象。

### 10.9.2 结构

推荐 segment/bump + fallback：

- 从 node-local Backend 获取 segment；
- bump pointer 提供极短分配路径；
- 大或 over-aligned 请求转 Backend；
- reset 时批量归还 segment；
- segment 内不需要逐对象 PageMap size-class 元数据；
- 可设置 max retained segments；
- request 结束时 generation 失效。

### 10.9.3 限制

- 非平凡 C++ 对象仍需上层运行析构；
- 指针不得逃逸到请求结束后；
- 异步 GPU kernel、通信或 callback 完成前不能 reset；
- 跨 request cache 不能存 request arena 指针；
- debug 模式 reset 后 poison/protect；
- fallback allocation 的归属必须随 arena reset 正确处理。

## 10.10 KV Cache 与 Tensor Workspace

KV cache 具有大块、长寿命、扩缩容、NUMA/GPU 协同特征，不应简单走小对象 cache。

建议：

- CPU KV 使用 LargeExtent/HugeRegion API；
- 以 block/page 为单位管理，不为每个 token 调 malloc；
- 支持 2 MiB alignment 和 node-local placement；
- block pool 与请求/会话生命周期分离；
- eviction 返回专用 pool，再按压力 purge；
- GPU KV 由 device allocator 管理；
- host staging/pinned memory 使用独立 domain；
- tensor workspace 采用可复用高水位 buffer 或 execution arena；
- 记录 reserved/used/committed 和 fragmentation。

## 10.11 Runtime Metadata

运行时小型长期对象可使用：

- default small-object allocator；
- 专用 size-class profile；
- subsystem arena/tag；
- stable-address metadata arena。

不要为所有 metadata 一律 request arena；需要跨请求存活的 scheduler、graph、kernel cache 和 communicator 对象必须有明确长期 domain。

## 10.12 Device、Pinned 与共享内存边界

定义 allocator domain：

| Domain | 分配/释放者 |
|---|---|
| CPU anonymous heap | ammalloc |
| File-backed weights | mapping/weight manager |
| CUDA/HIP device | device allocator |
| Pinned host | CUDA/HIP pinned allocator 或专用 wrapper |
| Shared memory/IPC | shm/memfd manager |
| RDMA registered | communication allocator |

扩展 metadata 可记录 domain，但 `am_free` 不应猜测并调用第三方 free。统一 RAII wrapper 在 aethermind 层按 domain 分派。

## 10.13 Cross-thread/Node Free

- PageMap 恢复 allocation owner arena/node/shard；
- 普通 local free 走 Frontend；
- free-only/remote 流量先按[第 6 章：Frontend](05-frontend.md)和[第 7 章：Middle-end](06-middle-end.md)的 bounded queue/batch；
- owner closing 时回退其 Middle-end drain path；
- request arena 通常不允许任意逐对象跨线程 free；
- model/KV block 返回专用 owner pool；
- remote bytes 计入实际 owner budget；
- 不能由释放线程把页重新标记为本 node owner。

## 10.14 内存压力优先级

建议默认回收顺序：

1. 已结束 request arena；
2. idle workspace 和 staging；
3. Frontend/Middle-end 冗余；
4. PageCache free committed；
5. idle KV free blocks；
6. retained model temporary region；
7. live model/KV 只由上层 eviction/unload 决定。

allocator 不得自行释放仍属 live model/request 的用户对象。pressure callback 只通知上层做业务级 eviction。

## 10.15 扩展 API 草图

内部 C ABI 草图：

~~~text
am_arena_create(config, out_handle)
am_arena_close(handle)
am_arena_reset(handle, flags)
am_arena_purge(handle, flags, out_result)

am_arena_malloc(handle, size)
am_arena_aligned_alloc(handle, alignment, size)
am_arena_usable_size(ptr)

am_set_thread_arena(handle)
am_get_thread_arena()
am_thread_idle()

am_notify_memory_pressure(level)
am_get_stats(query, caller_buffer)
~~~

要求：

- handle 包含 id/generation，不暴露裸内部指针；
- 所有返回码稳定、无异常穿过 C ABI；
- API 本身不依赖普通堆；
- caller-buffer 支持 size negotiation；
- reset/purge 明确 sync/async；
- unsupported 与 OOM 分开；
- 标准 malloc 不需要调用这些扩展。

## 10.16 配置与灰度

配置层级：

- 编译期能力：NUMA、hugetlb、rseq、profiling；
- 启动期结构：max node/shard/arena、region geometry；
- 运行期策略：budget、decay、pressure、sampling；
- per-arena：node/page/lifetime；
- per-request：只允许轻量 hint。

灰度开关：

- default heap only；
- explicit arena API；
- selected model arena；
- selected worker/node；
- selected request percentage；
- preload on/off；
- system allocator fallback（仅在 domain 明确且无混合 free 时）。

## 10.17 可观测性

按 arena/node/model 聚合：

- requested/usable/active/resident/retained；
- alloc/free/reset/purge；
- node requested/actual/fallback；
- remote free/access；
- THP/hugetlb；
- segment/block utilization；
- model load/unload；
- request high-water；
- KV reserved/used/free；
- pressure reclaim；
- allocation failure；
- tokens/s、TTFT、TPOT、p99 的关联时间序列。

高基数 model/request tag 通过采样或上层 registry 聚合，allocator 不维护无界字符串 map。

## 10.18 测试与基准

正确性：

- topology/cpuset 变化；
- Preferred/Bind/Interleave fallback；
- arena handle generation；
- reset 时 live async task；
- cross-node free；
- model unload；
- request pointer escape hardening；
- device pointer误传；
- pressure priority；
- fork/shutdown；
- unsupported platform。

性能：

- local vs remote memory；
- pinned/unpinned worker；
- first touch node；
- model load/prefault；
- request bump arena vs general malloc；
- KV block churn；
- multi-model；
- cross-socket producer/consumer；
- THP/hugetlb on/off；
- allocator stats off/on。

端到端必须同时报告 tokens/s、TTFT、TPOT、p99/p999、peak/steady RSS、NUMA remote、page faults 和 allocator fragmentation。

## 10.19 分阶段实施与验收

### 阶段 A：Domain 与显式 Arena 基础

1. allocator domain 文档和 RAII wrapper；
2. versioned arena handle；
3. default/model/request 基础配置；
4. caller-buffer stats；
5. reset/quiescence 协议；
6. aethermind 小范围显式 API。

退出条件：domain mismatch 可检测；arena reset 无 UAF；默认 malloc 不受影响。

风险类型：正确性、兼容性、内存。

### 阶段 B：Request/Model 生命周期治理

1. request segment arena；
2. model arena budget/load/unload；
3. workspace pool；
4. allocation tag/sampling；
5. pressure priority；
6. 真实 trace。

退出条件：请求分配成本和模型 unload RSS 有明确收益；无 pointer escape。

风险类型：正确性、内存、性能。

### 阶段 C：NUMA-local 链路

1. topology snapshot；
2. worker route；
3. node-local Frontend/Central/PageCache；
4. first touch/mbind policy；
5. remote free；
6. per-node stats。

退出条件：实际页 placement 与 owner 一致；remote access、tokens/s 或 p99 改善；fallback 正确。

风险类型：并发、性能、兼容性。

### 阶段 D：KV/Hugepage 与默认化评估

1. KV block/HugeRegion；
2. pinned/device domain wrapper；
3. hugetlb prefer/require；
4. multi-model pressure；
5. 生产灰度和自动回滚。

退出条件：真实推理综合收益明确；domain 安全；运维指标和回滚成熟。

风险类型：性能、内存、运维、兼容性。
