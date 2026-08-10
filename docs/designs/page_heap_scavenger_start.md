# PageHeapScavenger 启动时机设计方案（可实施版本）

## 背景

`PageHeapScavenger` 用于回收长期闲置 Span 的物理页（`madvise(MADV_DONTNEED)`）。
启动策略必须同时满足：
1. 不污染 hot path（`ThreadCache` / 高频 `free`）
2. 不引入 bootstrap 递归风险
3. 多线程仅启动一次
4. 关闭顺序可证明安全（避免静态析构 UAF）

---

## 结论

采用 **“首次 slow-path 启动 + Scavenger 内部自适应频率 + ReleaseSpan 轻量 hint”**。

不采用：
- `__attribute__((constructor))` 自动启动（初始化顺序不可控）
- `ReleaseSpan` 阈值触发启动（污染释放热路径）

---

## 核心风险与对应约束

### P0-1: bootstrap 风险（禁止在 allocator 路径读环境变量）

`std::getenv` 不应放在 `am_malloc_slow_path`/`am_free` 相关路径中。

**约束**：
- 环境变量只在 `RuntimeConfig::InitFromEnv()` 阶段读取并缓存
- `EnsureScavengerStarted()` 只能读取缓存后的布尔配置（无 IO、无 getenv）

### P0-2: 生命周期顺序风险

Scavenger 线程运行时访问 `PageCache`，必须保证停止顺序先于 `PageCache` teardown。

**约束**：
- `Stop()` 由显式 shutdown 流程调用，不能只依赖静态析构顺序
- 析构函数中仅做防御式 `if (joinable()) Stop();`，但不作为主流程依赖

### P1-1: 启动状态竞争

使用单一状态机，替代分散的 `checked/started` 标志。

**状态机**：
```cpp
enum class ScavengerState : uint8_t {
    kDisabled = 0,
    kNotStarted,
    kStarting,
    kRunning,
};
```

---

## 启动方案（实施细节）

### 1) 启动入口：首次 slow path

在 `am_malloc_slow_path()` 开头调用 `EnsureScavengerStarted()`。

```cpp
void EnsureScavengerStarted() noexcept {
    // 仅使用原子状态 + 已缓存配置，禁止 getenv
}

AM_NOINLINE void* am_malloc_slow_path(size_t size) {
    EnsureScavengerStarted();
    // existing slow path logic
}
```

说明：
- fast path 完全无分支新增
- 首次 slow path 启动一次线程，可接受

### 2) 启动流程：CAS 状态迁移

```cpp
// pseudo
if (state == kDisabled || state == kRunning) return;
if (CAS(kNotStarted -> kStarting)) {
    Start();
    state = kRunning;
}
```

失败线程直接返回，不阻塞。

### 3) 禁止在 ReleaseSpan 启动线程

`ReleaseSpan` 只允许做 O(1) 提示位写入：

```cpp
suggest_scavenge_.store(true, std::memory_order_relaxed);
```

不允许：阈值判断 + `Start()`。

---

## 自适应频率策略（Scavenger 内部）

### 目标

在内存压力大时加速清理；压力小时降低 CPU 与锁占用。

### 实施原则

1. 压力指标必须低成本（优先原子计数，避免每轮全表加锁统计）
2. 使用双阈值（hysteresis）避免频繁抖动
3. `suggest_scavenge_` 仅作提示，不作为正确性依赖

### 推荐参数（初始值）

```cpp
kIntervalSlowMs = 5000;
kIntervalFastMs = 200;
kHighPressurePages = X;
kLowPressurePages = Y;   // Y < X
```

---

## 环境变量策略

### 配置项

- `AM_ENABLE_SCAVENGER=1` 启用
- `AM_ENABLE_SCAVENGER=0` 禁用

### 读取时机

仅在 `RuntimeConfig::InitFromEnv()`。

### 默认值

默认建议：`enabled`（生产环境更需要 RSS 控制）。

短生命周期场景可通过环境变量显式关闭。

---

## 线程停止与关闭顺序

### 必须满足

1. 停止流程先于 `PageCache` 销毁
2. `Stop()` 必须可立即唤醒 sleep（`stop_token` + `cv`）
3. `join()` 不能无限等待

### 最小要求

```cpp
~PageHeapScavenger() {
    if (scavenge_thread_.joinable()) {
        Stop();
    }
}
```

备注：该析构逻辑只作为兜底，主停止路径应是显式 shutdown。

---

## 方案取舍结论

| 方案 | 结论 | 原因 |
|------|------|------|
| constructor 自动启动 | 否 | 初始化顺序与库加载时机不可控 |
| 首次 fast-path 启动 | 否 | 污染 hot path |
| ReleaseSpan 阈值启动 | 否 | 污染 free 热路径、启动抖动 |
| 首次 slow-path 启动 | 是 | 性能/安全/复杂度最佳平衡 |

---

## 实施清单

1. 在 `RuntimeConfig` 增加 `enable_scavenger`，并在 `InitFromEnv()` 一次性读取 `AM_ENABLE_SCAVENGER`
2. 在 `ammalloc.cpp` 新增 `EnsureScavengerStarted()`，采用单一原子状态机
3. 在 `am_malloc_slow_path()` 开头调用 `EnsureScavengerStarted()`
4. `ReleaseSpan` 中仅保留轻量 `suggest_scavenge_` 提示（不启动线程）
5. 在 `ScavengeLoop` 增加双阈值自适应频率（hysteresis）
6. 明确并实现显式 shutdown 顺序：先 `PageHeapScavenger::Stop()`，后 `PageCache` teardown
7. 补充测试：
   - 多线程并发启动仅一次
   - `AM_ENABLE_SCAVENGER=0/1` 生效
   - 停止时无死锁、无长时间阻塞

---

## 待确认项

1. 默认是否启用 `AM_ENABLE_SCAVENGER`（当前建议：启用）
2. fast/slow 清理阈值与间隔的初始参数
3. 是否提供显式 C API：`am_malloc_enable_scavenger()` / `am_malloc_disable_scavenger()`
