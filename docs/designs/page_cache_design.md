# PageCache 详细设计文档

> ammalloc 后端页缓存（PageCache + PageMap）设计与并发优化方案

---

## 1. 目标与范围

### 1.1 模块职责

`PageCache` 是 ammalloc 的后端页级管理层，负责：

- 管理空闲 Span 桶（按页数 `1..MAX_PAGE_NUM` 分类）
- 为 `CentralCache` 提供 Span（精确命中 / 拆分 / 系统回填）
- 释放 Span 时执行左右合并（coalescing）
- 通过 `PageMap` 维护 `page_id -> Span*` 映射

### 1.2 当前实现边界

基于当前代码（`ammalloc/include/ammalloc/page_cache.h`、`ammalloc/src/page_cache.cpp`）：

- `PageCache` 采用单例，核心状态由一把全局 `std::mutex` 保护
- `PageMap::GetSpan` 为无锁读路径（acquire load）
- `PageMap::SetSpan/ClearRange/Reset` 由 `PageCache` 写路径串行调用
- `Span` 元数据由 `ObjectPool<Span>` 管理

---

## 2. 现状设计

### 2.1 核心数据结构

```cpp
class PageCache {
  std::mutex mutex_;
  std::array<SpanList, PageConfig::MAX_PAGE_NUM + 1> span_lists_;
  ObjectPool<Span> span_pool_;
};

class PageMap {
  static std::atomic<RadixRootNode*> root_;
  static RadixRootNode radix_root_storage_;
  static ObjectPool<RadixNode> radix_node_pool_;
};
```

`RadixRootNode` 和 `RadixNode` 的 `children` 采用固定大小 `std::array<std::atomic<void*>, N>`，不触发动态分配。

### 2.4 已落地实现变更（2026-03-17）

`PageMap` root 初始化路径已从对象池切换为静态 root：

- 变更前：`root_ == nullptr` 时通过 `radix_root_pool_.New()` 分配 root。
- 变更后：`root_ == nullptr` 时使用 `radix_root_storage_`，先清空 `children`，再 `release store` 发布到 `root_`。

对应收益与影响：

- 去掉“单实例 root 仍走池分配”的冗余路径
- 降低初始化复杂度，避免 root pool 的管理开销
- 保持 `GetSpan` 无锁读与内存序语义不变
- `Reset()` 不再调用 root pool 释放，仅保留 `root_=nullptr` 与 `radix_node_pool_` 释放

### 2.2 分配流程（AllocSpanLocked）

1. `page_num > MAX_PAGE_NUM`：直接系统分配并建图
2. 精确桶命中：`span_lists_[page_num]` 取出
3. 从更大桶拆分：头部分配 + 尾部回桶 + PageMap 更新
4. 若无可用 Span：向 OS 申请 `MAX_PAGE_NUM`，回桶后重试

### 2.3 释放流程（ReleaseSpan）

1. 超大 Span 直接 `SystemFree`
2. 左合并循环
3. 右合并循环
4. 合并后回桶，更新 `PageMap`

---

## 3. 并发瓶颈分析（全局大锁）

### 3.1 问题本质

当前 `PageCache::mutex_` 覆盖了以下操作：

- 桶访问（exact hit / split / insert）
- 合并循环（多次 PageMap 查询 + 多桶 erase/push）
- 系统分配/释放（慢系统调用路径）
- PageMap 写入（SetSpan/ClearRange）

在高并发下，这会导致：

- 热路径与慢路径互相阻塞（Head-of-Line Blocking）
- 线程数增加时吞吐扩展受限
- 持锁区间过长，锁竞争放大

### 3.2 不可破坏的架构契约

优化时必须保持：

1. `PageMap::GetSpan` 继续无锁读取
2. `PageMap` 发布语义正确（写 release，读 acquire）
3. Span 生命周期继续由池管理，不引入外部 delete
4. 不引入分配器递归（核心路径无堆分配容器）

---

## 4. 全局大锁优化方案

## 4.1 总体思路

将“一个大锁”拆成“多域锁”，把慢系统调用和复杂合并从热临界区剥离。

目标：

- 缩短临界区
- 降低不相关路径互斥
- 保持实现可回归、可灰度上线

### 4.2 锁域拆分方案（推荐）

从：

- `PageCache::mutex_`（单全局锁）

到：

- `bucket_locks_[K]`：桶分段锁（K 建议 8/16）
- `pagemap_write_lock_`：PageMap 写锁（`SetSpan/ClearRange/Reset`）
- `span_pool_lock_`：若 `ObjectPool` 非并发安全时保护池操作

映射策略：

- `stripe = page_num % K`

收益：

- 小对象高频页请求主要冲突在少数桶，不再阻塞全部桶
- PageMap 批量写入与桶操作解耦

### 4.3 两阶段分配（Alloc）

#### 阶段 A：桶内快路径

- 仅持有目标 bucket 锁
- 处理 exact hit 和 split（仅桶内可完成部分）

#### 阶段 B：慢路径补给

- `SystemAlloc` 尽量移出 bucket 热锁区
- 仅在“发布到结构（回桶/建图）”时短持相关锁

设计要点：

- 避免在长系统调用期间占用 bucket 锁
- 发布顺序保持可见性与一致性

### 4.4 释放路径优化（Release）

分两级推进：

1. **同步精简版（先落地）**
   - 保留即时合并语义
   - 采用有序多锁获取（见 4.6）缩短冲突范围
2. **延迟合并版（后续）**
   - 快路径先回桶并打标
   - 后台线程批量 coalesce
   - 以碎片率换吞吐，需 benchmark 驱动阈值

### 4.5 PageMap 写路径优化

- 维持读无锁不变
- 写路径在 `pagemap_write_lock_` 下批量更新
- `SetSpan/ClearRange` 支持分段批量提交，减少锁切换

### 4.6 锁顺序与死锁规约（强约束）

统一顺序：

1. `span_pool_lock_`
2. `bucket_locks_[i]`（多把时按索引升序）
3. `pagemap_write_lock_`

禁止逆序获取；跨桶操作先收集目标 stripe，再按序一次性加锁。

---

## 5. 渐进式实施计划

### Phase 1（低风险，首选）

- 引入 bucket 分段锁
- 保持现有语义，不改算法
- 验证正确性与回归稳定性

### Phase 2（中风险，高收益）

- 拆分 Alloc 慢路径，系统调用移出热锁区
- 优化 PageMap 写路径批量化

### Phase 3（可选）

- 延迟合并（后台 coalesce）
- 引入回收策略参数化（空闲阈值、批次）

### Phase 4（增强）

- 非空桶 hint（位图/计数）
- 降低空桶探测与无效加锁

---

## 6. 正确性与性能验收

### 6.1 正确性验收

- 单测：`PageCache`、`CentralCache`、`Span` 相关全部通过
- 关键不变量：
  - `PageMap::GetSpan` 始终可见正确 Span
  - 合并后页映射连续一致
  - 无 UAF / 双删 / 锁顺序死锁

### 6.2 性能验收（建议阈值）

基准建议：

- `aethermind_benchmark --benchmark_filter="PageCache"`
- `aethermind_benchmark --benchmark_filter="Malloc_Deep_Churn"`

目标（建议）：

- `PageCache SameBucket threads:16` 吞吐提升 >= 15%
- `PageCache MixedBuckets threads:16` 吞吐提升 >= 10%
- 单线程 `ExactBucketHit` 不退化超过 3%

---

## 7. 风险与对策

### 风险 1：锁拆分导致状态一致性缺口

- 对策：引入明确发布点（结构更新 -> release store）
- 对策：增加 Debug 断言（Span 所属桶、页映射连续性）

### 风险 2：多锁顺序错误引发死锁

- 对策：统一锁顺序，封装锁获取 helper，禁止手写分散加锁

### 风险 3：延迟合并导致碎片升高

- 对策：先同步精简版，再灰度启用延迟合并并用基准评估

---

## 8. 与当前代码的对照关系

- 当前全局锁：`ammalloc/include/ammalloc/page_cache.h`
- 当前分配主逻辑：`ammalloc/src/page_cache.cpp` `AllocSpanLocked`
- 当前释放与合并：`ammalloc/src/page_cache.cpp` `ReleaseSpan`
- 当前无锁读映射：`ammalloc/src/page_cache.cpp` `PageMap::GetSpan`

本设计文档优先作为 PageCache 并发重构的评审基线，后续代码改动应按“Phase 1 -> Phase 2 -> ...”顺序推进。

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.1 | 2026-03-17 | 已落地：PageMap root 从 `ObjectPool<RadixRootNode>` 切换为静态 `radix_root_storage_`，同步更新 SetSpan/Reset 语义 |
| v1.0 | 2026-03-17 | 初版：补齐 PageCache 现状设计，并新增全局大锁优化方案与实施计划 |
