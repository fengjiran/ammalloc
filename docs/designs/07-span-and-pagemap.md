# Span 与 PageMap 模块设计

- **状态**: Current（描述已验证实现）
- **版本**: 1.0
- **日期**: 2026-08-19
- **关联代码**: [include/ammalloc/span.h](../../include/ammalloc/span.h) / [src/span.cpp](../../src/span.cpp) / [include/ammalloc/page_cache.h](../../include/ammalloc/page_cache.h)（`PageMap`/`RadixNode`） / [src/page_cache.cpp](../../src/page_cache.cpp)
- **上游依赖**: `PageAllocator`（Span 元数据池、页面）、`SizeClass`（对象尺寸）
- **下游消费者**: `PageCache`（Span 切分/合并/回收）、`CentralCache`（Span bitmap 对象分配）、`am_free`（PageMap 定位）
- **关联测试**: [tests/unit/test_span.cpp](../../tests/unit/test_span.cpp)、[tests/unit/test_page_cache.cpp](../../tests/unit/test_page_cache.cpp)
- **架构总览**: [ammalloc_design.md §4/§5.3/§5.6](ammalloc_design.md)
- **演进提案**: [improvement-plan/04-pagemap-and-span-lifecycle.md](../improvement-plan/04-pagemap-and-span-lifecycle.md)

## 1. 背景与目标

Span 描述一段连续页区间及其对象分配状态，是 PageCache 切分/合并与 PageMap 索引的基本单元；PageMap 是 4 层基数树，将 `PageID → Span*` 发布给所有无锁读者。目标：

- Span 元数据严格 64B（单缓存行，`static_assert` 强制），热字段一次载入。
- PageMap 读取路径完全无锁（仅原子 load），`am_free` 在 O(1) 时间定位归属 Span。

## 2. 职责与边界

- **所有权**：Span 元数据由所属 PageCache 分片的 `ObjectPool` 唯一持有（owner-shard-local）；CentralCache 桶只借用；`SpanList` 不拥有节点。
- **生命周期**：PageMap 的 RadixNode 运行期只增不减，绝不单独释放；仅 `Reset` 或进程退出时经 ObjectPool 统一回收。
- **写入保护**：`PageMap::SetSpan`/`ClearRange` 必须在所属分片锁内调用（生产默认单分片即全局串行化）。

## 3. 关键数据结构

| 成员 | 含义 | 同步机制 / 备注 |
|---|---|---|
| `Span::next/prev` | 侵入式链表指针 | 持锁访问（分片锁或桶锁） |
| `Span::start_page_idx/page_num` | 起始页号 / 页数 | 同上 |
| `Span::flags` | `kUsedMask`/`kCommittedMask` 打包位 | 经 `IsUsed/SetUsed/IsCommitted/SetCommitted` 访问 |
| `Span::size_class_idx` | CentralCache 桶索引 | 释放时避免重映射尺寸 |
| `Span::aligned_obj_size/capacity/use_count` | 对象分配元数据 | CentralCache 桶锁下访问 |
| `Span::scan_cursor` | 首个可能含空闲位的 bitmap 字 | 减少分配扫描范围 |
| `Span::obj_offset/owner_shard_id` | 数据区偏移 / 属主分片 | 初始化后不可变 |
| `Span::last_used_time_ms` | 最近归还时间戳（冷数据） | Scavenger 读取 |
| `RadixRootNode/RadixNode` | 基数树节点（页对齐） | `children` 为 `std::atomic<void*>`，release 安装 / acquire 读取 |

## 4. 并发模型

- **Span 字段非原子**：所有访问者必须持有对应锁（PageCache 分片锁或 CentralCache 桶锁），锁域在头文件注释中显式声明。
- **PageMap 读路径**：`GetSpan` 逐层 `acquire` load，与写者的 `release` store 配对；任一空层立即返回 null；不获取任何锁。
- **PageMap 写路径**：`SetSpan` 懒分配中间节点（`radix_node_pool_.New()`，页对齐），release store 安装；`ClearRange` 对空子树按覆盖范围整体跳过；root 首次发布前从静态 `root_storage_` 初始化。
- **内存序契约**：发布用 `release`，读取用 `acquire`，计数/初始化用 `relaxed`；禁止默认 `seq_cst`（硬性约束 §4.4）。

## 5. 接口定义

| 接口 | 签名 | 语义要点 | Hot path |
|---|---|---|---|
| `Span::Init` | `void Init(size_t aligned_object_size)` | `@pre` Span 归当前 CentralCache 桶独占；计算 bitmap+数据布局并置位空闲 bitmap | ❌ |
| `Span::AllocObject` | `void* AllocObject()` | 清一个空闲位；满时返回 null；`scan_cursor` 推进 | ✅ |
| `Span::FreeObject` | `void FreeObject(void* ptr)` | 置回空闲位；`AM_DCHECK` 检出 double-free；回退 `scan_cursor` | ✅ |
| `SpanList::insert/erase/push_front/push_back/pop_front` | 静态/成员 | 循环哨兵免空分支；不持有元数据 | ✅ |
| `PageMap::GetSpan` | `static Span* (size_t page_id / void* ptr)` | 无锁；未映射返回 null；`ptr` 版本右移页偏移 | ✅ |
| `PageMap::SetSpan` | `static void (Span* span)` | `@pre` 分片锁已持；按连续段批量 release store | ❌ |
| `PageMap::ClearRange` | `static void (size_t start, size_t page_num)` | `@pre` 分片锁已持；空子树整块跳过 | ❌ |
| `PageMap::Reset` | `static void Reset()` | `@pre` 所有用户静止；root 置空 + 池回收 | ❌ |

## 6. 算法与流程

### 6.1 Span::Init 布局计算

```
bitmap 放页区起始（1 bit/对象），数据区按 ALIGNMENT 对齐其后：
  max_objs  ≈ (总字节 × BITS_PER_BYTE) / (obj_size × BITS_PER_BYTE + 1)
  capacity  = 对齐后 (data_end - data_start) / obj_size（可能小于 max_objs）
```
- bitmap 字几何常量统一取自 `SystemConfig::BITMAP_BITS/SHIFT/MASK` 与 `BITS_PER_BYTE`（config.h，分别派生自 `std::countr_zero` 与 `std::numeric_limits<unsigned char>::digits`），代码中不出现 64/63/6/8 字面量；字数按 `(max_objs + BITMAP_MASK) >> BITMAP_SHIFT` 取整。
- 不存数据指针，`obj_offset` 派生存取，保证 Span 保持 64B。

### 6.2 AllocObject / FreeObject

- 分配：从 `scan_cursor` 起扫描 `BITMAP_BITS` 位字，`std::countr_zero` 取首个空闲位；字满则推进游标。
- 释放：按对象偏移反算全局索引（2 的幂尺寸走移位），置位并回退 `scan_cursor`；double-free 触发 `AM_HCHECK`。

### 6.3 PageMap 查询（48-bit 模式）

```
[root][9][9][9] 四级索引：i0 = page_id >> 27（越界检查 RADIX_ROOT_SIZE），
i1/i2/i3 各取 9 bit；逐层 acquire load，任一空层返回 null。
```

### 6.4 PageMap 写入

- `SetSpan`：逐页段索引；中间节点缺失时经 `ObjectPool` 懒分配并以 release store 挂载，叶子批量 store。
- `ClearRange`：按页段清除叶子；遇到空子树按该层覆盖跨度整体跳过（O(子树跨度) 变 O(层数)）。

## 7. 边界条件与错误处理

- `page_id` 越界（`i0 >= RADIX_ROOT_SIZE`）→ `GetSpan` 返回 null（对未知地址的释放被 `am_free` 忽略）。
- `FreeObject` 越界/错位指针：`AM_HCHECK` 下溢、对齐与溢出检查（debug 构建及 `AM_HARDENED` release 崩溃，其余 release 交由调用契约）。
- `Init` 时容量为 0（对齐开销超过页区）：`AllocObject` 恒 null，该 Span 不被分配使用。

## 8. 风险与权衡

- **bitmap 前置在页区**：分配对象前必须先读 bitmap 页，牺牲少量缓存；换取 Span 64B 与 O(1) 定位。
- **节点只增不减**：长期运行地址空间稀疏时 RadixNode 内存不回收；这是无锁读的代价，演进方向见 improvement-plan 04（Span 延迟回收协议）。
- **写路径串行化**：单分片下 `SetSpan`/`ClearRange` 全局串行；多写者并发属演进方向（PageMap multi-writer）。

## 9. 测试要点

- `test_span.cpp`：`SpanTest.DoubleFreeCorruption`（double-free 防护）。
- `test_page_cache.cpp`：`PageMapConsistency`、`SplitRemainderIsMappedInPageMap`、`UnknownAddressReturnsNullFromPageMap`、`ClearRangeOnEmptyPageMapKeepsLookupNull`、`ClearRangeAfterUnmapMakesLookupNull`、`ResetClearsMappingsAndIsIdempotent`、`MergeLogic`、`RandomStress`。

## 10. 变更记录

| 日期 | 变更 | 原因 | 关联 PR / ADR |
|---|---|---|---|
| 2026-08-19 | 初版（由架构总览 §4/§5.3/§5.6 拆分扩展） | 文档系统落地 | — |
