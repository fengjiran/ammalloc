# SizeClass 详细设计文档

> ammalloc 内存分配器的尺寸分级系统设计

---

## 1. 设计目标与背景

### 1.1 问题背景

内存分配器面临的核心挑战：**如何高效地将任意大小的内存请求映射到有限数量的固定尺寸桶中？**

- **内部碎片**: 分配的内存块大于请求的大小，造成浪费
- **外部碎片**: 空闲内存分散，无法满足大内存请求
- **管理开销**: 过多的桶数量会增加管理成本（内存、查找时间）

### 1.2 设计目标

| 目标 | 指标 | 说明 |
|------|------|------|
| **低内部碎片** | 平均约 12.5% | 最坏相对碎片率受 step 配置约束，平均碎片率保持较低 |
| **快速查找** | O(1) | 尺寸到桶索引的映射时间恒定 |
| **合理桶数量** | < 100 | 控制 ThreadCache 中 FreeList 的数量 |
| **缓存友好** | 局部性好 | 常用尺寸集中在前几个桶 |

### 1.3 设计参考

本设计基于 **Google TCMalloc** 的尺寸分级策略：
- 小对象（≤128B）：线性映射，8字节对齐
- 大对象（>128B）：对数步进，每 2^k 区间分成 4 步

---

## 2. 算法设计

size的分级策略如下，主要分为三个阶段：边界防御、线性映射区、对数阶梯映射区。

```
┌─────────────────────────────────────────────────────────────┐
│                     尺寸分级总览                             │
├──────────────┬─────────────────┬────────────┬───────────────┤
│   范围       │    对齐粒度      │   桶数量   │    索引计算   │
├──────────────┼─────────────────┼────────────┼───────────────┤
|  1-128 B     │     8 B         │    16      │  (size-1)>>3  │
│  129-256 B   │    32 B         │     4      │  数学计算     │
│  257-512 B   │    64 B         │     4      │  数学计算     │
│  513-1024 B  │   128 B         │     4      │  数学计算     │
│  ...         │   ...           │   ...      │  数学计算     │
│  16KB-32KB   │    4 KB         │     4      │  数学计算     │
└──────────────┴─────────────────┴────────────┴───────────────┘
```

### 2.1 边界语义 (Boundary Semantics)

```cpp
if (size > SizeConfig::MAX_TC_SIZE) return std::numeric_limits<size_t>::max();
```

* **`size == 0`**：当前实现采用**分层语义**，而不是统一按非法输入处理：
  * 映射接口：`Index(0) -> 0`，`RoundUp(0) -> 8`
  * 策略接口：`CalculateBatchSize(0) -> 0`，`GetMovePageNum(0) -> 0`
  * 这样既支持上层 `malloc(0)` 风格语义，又避免 batch/page 策略对零尺寸做无意义计算。
* **越界检查**：超过 `ThreadCache` 阈值的直接交由 `PageCache` 处理。

### 2.2 线性映射区 (1 ~ 128 Bytes)

```cpp
if (size <= 128) {
    return (size - 1) >> 3;
}
```

对于极小对象，底层公式采用 **8 字节固定步长**。

* **为什么是 `size - 1`？**：这是对齐算法的精髓。如果 `size = 8`，`(8-1)>>3 = 0`（落入第 0 号桶：8B）。如果不用 `size-1`，`8>>3 = 1`，就会错误地落入第 1 号桶（16B）。
* **桶范围**：Index `[0, 15]`，对应容量 `[8B, 16B, ..., 128B]`。
* **实现注意**：这里描述的是 `details::CalculateIndex()` 的线性映射段。`SizeClass::Index()` 在工程实现上对 `[0, kSmallSizeThreshold]` 使用查表优化；当前 `kSmallSizeThreshold = 1024`。

- **内部碎片**: 最大 7/8 = 12.5%

- **优点**: 计算简单，O(1)，分支预测友好

### 2.3 对数阶梯映射区 (> 128 Bytes)

将内存按 **2 的幂次方进行分组（Group）**，然后在每个组内再均匀切分为 **4 个阶梯（Step）**。

*配置参数*: `kStepsPerGroup = 4`, `kStepShift = 2` (因为 $2^2 = 4$)

**步骤 A：确定所属的 2 的幂次方组 (`group_idx`)**

```cpp
int msb = std::bit_width(size - 1) - 1;
int group_idx = msb - 7;
```

* `std::bit_width(x)` 返回表示 `x` 所需的最小位数（底层编译为 `BSR` 或 `LZCNT` 硬件指令，1 个时钟周期完成）。
* **`msb` (最高有效位)**：例如 `size = 129`，`size-1 = 128` (二进制 `10000000`)，`bit_width = 8`，`msb = 7`。
* **`- 7` 的含义**：因为线性区处理到了 128 字节（$2^7$）。所以大于 128 的第一个组，其 `msb` 必然是 7。减去 7 就是为了让 `group_idx` 从 `0` 开始。
  * Group 0: 129B ~ 256B (`msb=7`)
  * Group 1: 257B ~ 512B (`msb=8`)

**步骤 B：计算该组的起始桶下标 (`base_idx`)**

```cpp
size_t base_idx = 16 + (group_idx << SizeConfig::kStepShift);
```

* **`16`**：因为前面的线性区已经占用了 `0~15` 号桶，所以大对象的桶从 `16` 开始。
* **`<< kStepShift` (即乘以 4)**：因为每个 Group 内被划分成了 4 个阶梯（桶），所以每跨越一个 Group，基础下标就要增加 4。

**步骤 C：计算组内的步长 (`shift`)**

```cpp
int shift = msb - SizeConfig::kStepShift;
```

* **物理意义**：当前组的总跨度是 $2^{msb}$。要把它切成 4 份（$2^2$），每份的大小就是 $2^{msb} / 2^2 = 2^{msb-2}$。
* 例如 Group 0 (128~256B)：`msb=7`，`shift = 7-2=5`。步长为 $2^5 = 32$ 字节。（即 160B, 192B, 224B, 256B）。

**步骤 D：计算组内偏移量 (`group_offset`)**

```cpp
size_t group_offset = ((size - 1) >> shift) & (SizeConfig::kStepsPerGroup - 1);
```

* `(size - 1) >> shift`：计算当前大小包含了多少个"步长"。
* `& (4 - 1)`：即 `% 4`。屏蔽掉高位信息，只保留在当前 Group 内部的相对索引 `[0, 1, 2, 3]`。

**步骤 E：合成最终 Index**

```cpp
return base_idx + group_offset;
```

#### 算法推演实例

让我们手动推演一次 **`size = 320`** 的分配过程：

1. **边界与线性区**：`320 > 128`，进入阶梯映射区。
2. **算 `msb`**：`size - 1 = 319` (二进制 `1 0011 1111`)。`bit_width = 9`，`msb = 8`。
3. **算 `group_idx`**：`msb - 7 = 8 - 7 = 1`。（属于 Group 1：256B ~ 512B）。
4. **算 `base_idx`**：`16 + (1 << 2) = 16 + 4 = 20`。（Group 1 的桶从 20 开始）。
5. **算 `shift`**：`msb - 2 = 8 - 2 = 6`。（步长为 $2^6 = 64$ 字节）。
6. **算 `offset`**：`(319 >> 6) & 3 = 4 & 3 = 0`。
7. **结果**：`base_idx + offset = 20 + 0 = 20`。

**反向验证**：第 20 号桶的容量 = 基准 256B + (0 + 1) * 64B = **320B**。完美契合！

### 2.4 内部碎片分析

#### 小对象（≤128B）

```
最大内部碎片 = 对齐粒度 - 1 = 7B
相对碎片率 = 7 / 8 = 12.5%
```

#### 大对象（>128B）

```
每组的步长 = (2^(k+1) - 2^k) / 4 = 2^(k-2)
最大内部碎片 = 步长 - 1 ≈ 2^(k-2)
相对碎片率 = 2^(k-2) / 2^k = 25%

实际最大碎片率出现在组边界：
- 129B 映射到 160B 桶：碎片率 (160-129)/129 = 24%
- 257B 映射到 320B 桶：碎片率 (320-257)/257 = 24.5%
```

**说明**：这里的 25% 是按组阶梯化估算的最坏相对碎片上界；全范围统计下的平均内部碎片率约为 12.5%。

---

## 3. 架构设计

### 3.1 类层次结构

```
namespace aethermind {
    namespace details {
        // 内部计算函数
        constexpr size_t CalculateIndex(size_t size) noexcept;
        constexpr size_t CalculateSize(size_t idx) noexcept;
    }
    
    class SizeClass {
    public:
        // 核心接口
        static size_t Index(size_t size) noexcept;     // Size -> Index
        static size_t Size(size_t idx) noexcept;       // Index -> bucket upper bound
        static size_t SafeSize(size_t idx) noexcept;   // Checked Size()
        static size_t RoundUp(size_t size) noexcept;   // 向上取整到桶尺寸
        
        // 批量策略
        static size_t CalculateBatchSize(size_t size) noexcept;
        static size_t GetMovePageNum(size_t size) noexcept;
        
    private:
        // 编译期表
        static auto small_index_table_;  // [0, kSmallSizeThreshold] 快速查表
        static auto size_table_;         // index -> class size
        static auto batch_table_;        // index -> batch size
        static auto move_page_table_;    // index -> span page count
    };
}
```

### 3.2 分层设计 rationale

| 层级 | 职责 | 理由 |
|------|------|------|
| `details::` | 数学计算 | 分离算法实现，便于测试和验证 |
| `SizeClass::` | 公共接口 + 优化 | 封装实现细节，提供优化后的访问 |
| 编译期表 | 运行时加速 | 用空间换时间，热路径 O(1) |

### 3.3 快速路径 vs 慢速路径

```
SizeClass::Index(size)
    │
    ├── [size ≤ 1024B] AM_LIKELY ─→ 查表 small_index_table_[size] ──→ O(1)
    │
    └── [size > 1024B] ───────────→ 计算 details::CalculateIndex(size) ──→ O(1)
```

**设计决策**:
- 小对象（高频）走查表，消除热路径上的重复算术
- 大对象（低频）走计算，节省内存（不需要 32KB 的表）

---

## 4. 性能优化

### 4.1 编译期计算

```cpp
// 使用 C++20 consteval IIFE 生成查找表
constexpr static auto size_table_ = []() consteval {
    std::array<uint32_t, kNumSizeClasses> size_table{};
    for (size_t idx = 0; idx < kNumSizeClasses; ++idx) {
        size_table[idx] = static_cast<uint32_t>(details::CalculateSize(idx));
    }
    return size_table;
}();
```

**优势**:
- 零运行时开销
- 保证表的正确性（编译期验证）
- 缓存友好（连续内存）

### 4.2 极致的工程优化

虽然上述位运算已经非常快（无分支、纯寄存器计算，约 10~15 个时钟周期），但在 AetherMind 中，我们将其进一步推向了物理极限：

```cpp
static constexpr auto small_index_table_ =[]() consteval { ... }();

static constexpr size_t Index(size_t size) noexcept {
    if (size <= SizeConfig::kSmallSizeThreshold) AM_LIKELY {
        return small_index_table_[size]; // O(1) 查表
    }
    return details::CalculateIndex(size); // 慢速路径算术
}
```

1. **编译期常量表 (`consteval`)**：利用上述纯数学算法，在**编译期**直接生成 1024 字节以内的查找表。
2. **热路径降维**：95% 以上的内存分配都在 1KB 以内。通过 `AM_LIKELY` 提示，CPU 会直接执行一条内存读取指令 `movzx eax, byte ptr [table + size]`。
3. **耗时对比**：将原本需要 ~15ns 的位运算，降维打击至 **~3ns**（L1 Cache 命中）。

### 4.3 分支预测提示

```cpp
AM_ALWAYS_INLINE constexpr static size_t Index(size_t size) noexcept {
    if (size > SizeConfig::MAX_TC_SIZE) AM_UNLIKELY {
        return std::numeric_limits<size_t>::max();
    }
    
    if (size <= SizeConfig::kSmallSizeThreshold) AM_LIKELY {
        return small_index_table_[size];
    }
    
    return details::CalculateIndex(size);
}
```

**使用**:
- `AM_LIKELY`: 标注热路径（小对象）
- `AM_UNLIKELY`: 标注冷路径（超大对象、错误）

### 4.4 强制内联

```cpp
AM_ALWAYS_INLINE static constexpr size_t Size(size_t idx) noexcept;
AM_ALWAYS_INLINE static constexpr size_t RoundUp(size_t size) noexcept;
```

**理由**: 这些函数在分配/释放热路径上被频繁调用，内联消除函数调用开销。

### 4.5 位运算优化

```cpp
// 使用 std::bit_width (C++20) 替代循环查找 MSB
int msb = std::bit_width(size - 1) - 1;

// 使用位运算替代除法/乘法
size_t page_num = (total_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;  // 代替 / PAGE_SIZE
```

---

## 5. 批量策略设计

### 5.1 CalculateBatchSize

**目标**: 确定 ThreadCache 和 CentralCache 之间一次传输多少个对象

**策略**: 以 **size class** 为单位预计算，等价于对齐后尺寸的反比策略

```cpp
static constexpr size_t CalculateBatchSize(size_t size) noexcept {
    if (size == 0 || size > SizeConfig::MAX_TC_SIZE) return 0;
    return BatchByIndex(Index(size));
}
```

其背后的表生成逻辑本质上仍然是：

```cpp
for each size class idx:
    class_size = Size(idx)
    batch = MAX_TC_SIZE / class_size
    batch = clamp(batch, 2, 512)
```

这意味着 **129B 和 160B 这类落入同一 class 的请求，会得到同样的 batch size**。

**示例**:
- 8B 对象: 32768 / 8 = 4096 → clamp 到 512
- 1KB 对象: 32768 / 1024 = 32
- 32KB 对象: 32768 / 32768 = 1 → 提升到 2

### 5.2 GetMovePageNum

**目标**: 确定 CentralCache 从 PageCache 申请多少页

**策略**: 一个 Span 满足约 8 个 batch 请求，且同一 size class 的 page 策略固定一致

```cpp
static constexpr size_t GetMovePageNum(size_t size) noexcept {
    if (size == 0 || size > SizeConfig::MAX_TC_SIZE) return 0;
    return MovePagesByIndex(Index(size));
}
```

其背后的表生成逻辑可概括为：

```cpp
for each size class idx:
    class_size = Size(idx)
    batch = BatchByIndex(idx)
    total_objs = batch * 8
    total_bytes = max(total_objs * class_size, 32KB)
    pages = ceil(total_bytes / PAGE_SIZE)
    pages = min(pages, MAX_PAGE_NUM)
```

**理由**: 减少 PageCache 全局锁的竞争频率

---

## 6. 验证与测试

### 6.1 编译期验证

```cpp
// 验证小对象边界
static_assert(details::CalculateSize(0) == 8);
static_assert(details::CalculateSize(15) == 128);

// 验证大对象第 0 组
static_assert(details::CalculateSize(16) == 160);  // 128 + 32
static_assert(details::CalculateSize(17) == 192);  // 160 + 32
static_assert(details::CalculateSize(19) == 256);  // 组边界

// 验证互逆性（近似）
static_assert(details::CalculateIndex(129) == 16);
static_assert(details::CalculateIndex(160) == 16);  // 同桶

// Round-trip 检查
static_assert(SizeClass::Index(SizeClass::Size(20)) == 20);

// 关键边界约束
static_assert(std::has_single_bit(SizeConfig::MAX_TC_SIZE));
static_assert(SizeClass::Size(SizeClass::kNumSizeClasses - 1) == SizeConfig::MAX_TC_SIZE);
```

除了代表点 `static_assert` 之外，当前实现还增加了采样式 consteval 校验，用于验证：

- `Index(s)` 在 class 边界上可映射到合法索引
- `Size(Index(s)) >= s`
- `Index(Size(Index(s))) == Index(s)`
- `Size(idx)` 单调递增
- `RoundUp(s)` 单调不减

### 6.2 运行时测试

```cpp
TEST(SizeClassTest, ComprehensiveRoundTrip) {
    for (size_t sz = 1; sz <= SizeConfig::MAX_TC_SIZE; ++sz) {
        size_t idx = SizeClass::Index(sz);
        size_t bucket_size = SizeClass::Size(idx);
        
        // 验证: bucket_size >= sz
        EXPECT_GE(bucket_size, sz);
        
        // 验证: 前一个桶的尺寸 < sz
        if (idx > 0) {
            EXPECT_LT(SizeClass::Size(idx - 1), sz);
        }
    }
}
```

此外，当前单测还覆盖了 invalid-input 契约：

- `RoundUp(size > MAX_TC_SIZE)` 原样透传
- `CalculateBatchSize(0) == 0`
- `GetMovePageNum(0) == 0`
- `Index(size > MAX_TC_SIZE) == max()`

### 6.3 内部碎片统计

```cpp
TEST(SizeClassTest, FragmentationAnalysis) {
    double total_waste = 0;
    size_t total_alloc = 0;
    
    for (size_t sz = 1; sz <= 32768; ++sz) {
        size_t bucket = SizeClass::Size(SizeClass::Index(sz));
        total_waste += (bucket - sz);
        total_alloc += bucket;
    }
    
    double avg_fragmentation = total_waste / total_alloc;
    EXPECT_LT(avg_fragmentation, 0.125);  // < 12.5%
}
```

---

## 7. 使用示例

### 7.1 基本使用

```cpp
// 获取尺寸的桶索引
size_t idx = SizeClass::Index(100);     // idx = 12
size_t idx = SizeClass::Index(200);     // idx = 18

// 获取桶的最大尺寸
size_t size = SizeClass::Size(12);      // size = 104
size_t size = SizeClass::Size(18);      // size = 192

// 向上取整到桶尺寸
size_t aligned = SizeClass::RoundUp(150);  // aligned = 160
```

### 7.2 在分配器中的使用

```cpp
void* am_malloc(size_t size) {
    // 1. 超过 ThreadCache 阈值的请求直接走慢路径
    if (size > SizeConfig::MAX_TC_SIZE) {
        return am_malloc_slow_path(size);
    }

    // 2. 小对象走 ThreadCache 快路径
    return ThreadCache::GetCurrentThreadCache()->Allocate(size);
}
```

也就是说，顶层 `am_malloc()` 本身并不直接决定 batch/page 策略；
这些策略在 ThreadCache / CentralCache 的慢路径里，通过 `SizeClass` 继续生效。

### 7.3 在 CentralCache 中的使用

```cpp
Span* CentralCache::GetOneSpan(size_t class_size) {
    // class_size 通常已经过 RoundUp() 或来自 Size(idx)
    size_t page_num = SizeClass::GetMovePageNum(class_size);
    
    // 2. 从 PageCache 申请 Span
    Span* span = PageCache::GetInstance().AllocSpan(page_num, class_size);
    
    // 3. 初始化 Span...
}
```

---

## 8. 关键设计决策

### 8.1 为什么使用 8 字节对齐（小对象）？

- **硬件原因**: x86-64 架构的最小对齐要求
- **内存效率**: 8 字节是 `void*` 的大小，覆盖大多数小对象
- **碎片平衡**: 12.5% 的最大碎片是可接受的

### 8.2 为什么大对象使用 4 步/组？

- **碎片控制**: 4 步/组将最坏相对碎片率控制在约 25% 量级，平均碎片明显更低
- **桶数量控制**: 32KB 范围当前只需 48 个桶
- **计算简单**: 4 步 = 2^2，位运算友好

### 8.3 为什么小对象用查表，大对象用计算？

| 方案 | 内存占用 | 查找时间 | 缓存友好性 |
|------|----------|----------|------------|
| 全查表（32KB） | 32KB | O(1) | 一般 |
| 全计算 | 0 | O(1) | 好 |
| **混合（当前）** | 多张小型只读表 | O(1) | **好** |

**权衡**: 小对象查表（消除分支），大对象计算（节省内存）

### 8.4 为什么 batch size 近似反比于对象所属 class 的大小？

- **小对象**: 传输 512 个 8B 对象 = 4KB，摊销锁开销
- **大对象**: 传输 2 个 16KB 对象 = 32KB，防止 ThreadCache 囤积
- **内存平衡**: ThreadCache 总占用约 32KB-64KB

---

## 9. 性能特征

### 9.1 时间复杂度

| 操作 | 复杂度 | 实际开销 |
|------|--------|----------|
| `Index(size ≤ 1024)` | O(1) | 1 次数组访问 |
| `Index(size > 1024)` | O(1) | 若干位运算 |
| `Size(idx)` | O(1) | 1 次数组访问 |
| `RoundUp(size)` | O(1) | Index + Size |

### 9.2 空间占用

| 表 | 大小 | 说明 |
|----|------|------|
| `small_index_table_` | 1025 bytes | size 0-1024 的索引 |
| `size_table_` | 48 × 4 bytes | index -> class size |
| `batch_table_` | 48 × 2 bytes | index -> batch size |
| `move_page_table_` | 48 × 2 bytes | index -> page count |
| **总计** | **约 1.4 KB** | 代码段，只读 |

### 9.3 缓存行为

- `small_index_table_`: 适合 L1 cache（1025B）
- `size_table_` / `batch_table_` / `move_page_table_`: 均为小型只读表
- 无动态内存分配

---

## 10. 扩展性考虑

### 10.1 修改对齐粒度

如果要改为 16 字节对齐：

```cpp
// 修改 kSmallSizeThreshold 相关的计算
// 桶数量从 16 变为 8
// 需要重新生成 static_assert 验证
```

### 10.2 支持更大尺寸

如果要支持 64KB 对象：

```cpp
// 修改 SizeConfig::MAX_TC_SIZE
// kNumSizeClasses 会自动计算
// 重新验证 static_assert
```

### 10.3 调整步数

如果要改为 8 步/组（更细粒度）：

```cpp
// 修改 kStepShift = 3
// 桶数量增加，碎片降低
// 需要权衡 ThreadCache 内存
```

---

## 11. 参考资料

1. **TCMalloc**: `https://goog-perftools.sourceforge.net/doc/tcmalloc.html`
2. **jemalloc**: `https://jemalloc.net/`
3. **C++ 标准**: `std::bit_width` (C++20)
4. **内存分配器设计**: "Scalable Memory Allocation" - Google

---

**文档版本**: 2.1  
**最后更新**: 2026-03-24  
**设计状态**: 已实施并验证  
**相关文件**: `ammalloc/include/ammalloc/size_class.h`
