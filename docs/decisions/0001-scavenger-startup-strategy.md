# ADR-0001: PageHeapScavenger 启动时机策略

- **状态**: Accepted
- **日期**: 2026-08-19（由 `docs/designs/page_heap_scavenger_start.md` 转化，原文档日期 2026-03）
- **作者**: AetherMind Team
- **关联代码**: [src/ammalloc.cpp](../../src/ammalloc.cpp)（`EnsureScavengerStarted`）、[include/ammalloc/page_heap_scavenger.h](../../include/ammalloc/page_heap_scavenger.h)、[src/page_heap_scavenger.cpp](../../src/page_heap_scavenger.cpp)、[include/ammalloc/config.h](../../include/ammalloc/config.h)（`AM_ENABLE_SCAVENGER`）

## 背景

`PageHeapScavenger` 用于回收长期闲置 Span 的物理页（`madvise(MADV_DONTNEED)`）。启动策略必须同时满足：

1. 不污染 hot path（`ThreadCache` / 高频 `free`）。
2. 不引入 bootstrap 递归风险（分配器路径禁止读环境变量、禁止堆分配）。
3. 多线程仅启动一次。
4. 关闭顺序可证明安全（避免静态析构 UAF）。

## 决策

采用 **"首次 slow-path 启动 + 单原子状态机 + ReleaseSpan 轻量 hint"**：

- 启动入口为 `am_malloc_slow_path()` 开头（[src/ammalloc.cpp:88-91](../../src/ammalloc.cpp)）；fast path 无分支新增。
- `EnsureScavengerStarted()` 先读 `RuntimeConfig::EnableScavenger()`（环境变量仅在 `RuntimeConfig::InitFromEnv()` 缓存，无 getenv），再用 `static std::atomic<bool> started` + `compare_exchange_strong(acq_rel)` 保证多线程仅启动一次；启动失败时保持 `started` 置位，避免重试风暴（分配功能不受影响）。
- `ReleaseSpan` 只允许 O(1) 提示位写入，不允许阈值判断 + `Start()`（污染释放热路径）。

已实现参数（以代码事实为准，[include/ammalloc/page_heap_scavenger.h:51-53](../../include/ammalloc/page_heap_scavenger.h)）：

- `kScavengeIntervalMs = 1000`（清理间隔）
- `kIdleThresholdMs = 10000`（闲置阈值）

## 权衡

| 备选方案 | 结论 | 原因 |
|---|---|---|
| `__attribute__((constructor))` 自动启动 | 否 | 初始化顺序与库加载时机不可控 |
| 首次 fast-path 启动 | 否 | 污染 hot path |
| `ReleaseSpan` 阈值触发启动 | 否 | 污染 free 热路径、启动抖动 |
| 首次 slow-path 启动 | 是 | 性能/安全/复杂度最佳平衡 |

## 后果

- 正面：RSS 治理对热路径零影响；线程惰性创建，进程不使用 ammalloc 时无启动开销；失败降级安全。
- 负面：首次 slow path 有一次线程创建成本（可接受）；`AM_ENABLE_SCAVENGER` 默认开启意味着首次分配即建线程（短生命周期进程可显式关闭）。
- 生命周期契约：`Stop()` 由显式 shutdown 流程调用，先于 `PageCache` teardown；析构函数仅防御式兜底，不作为主流程依赖。

## 关联

- 模块设计：[06-page-heap-scavenger.md](../designs/06-page-heap-scavenger.md)
- 架构总览：[ammalloc_design.md §5.8](../designs/ammalloc_design.md)
- 调研参考：[designs/research/allocator-background-thread.md](../designs/research/allocator-background-thread.md)
- 运行配置：README.md "Runtime Configuration"（`AM_ENABLE_SCAVENGER`）

## 未落地项（来自原启动方案文档的实施清单）

以下条目在决策落地时尚未实现，属后续演进，跟踪于 [docs/issues.md](../issues.md)：

1. ScavengeLoop 双阈值自适应频率（hysteresis）：当前 `kScavengeIntervalMs` 为固定间隔，尚无压力感知调速。
2. 显式 C API `am_malloc_enable_scavenger()` / `am_malloc_disable_scavenger()`：当前仅支持环境变量控制。
3. fast/slow 清理阈值与间隔参数的正式标定（当前参数：interval 1000ms / idle 10000ms）。
