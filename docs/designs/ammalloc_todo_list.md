本文档记录了 AetherMind 内存池项目的已知缺陷修复计划、性能优化点以及未来向工业级功能对齐的演进路线。

> **🔗 相关资源**: [📅 查看开发进度日志 (Development Log)](../../logs/development_log.md)

### 🔴 P0: 紧急修复 (Critical Fixes)

*涉及内存安全和核心正确性，必须立即修复。*

- [ ] #### **解决“自举（Bootstrapping）”与“递归死锁”问题 [Bug/Safety]**

- 背景：当 `ammalloc` 接管系统 `malloc` 后，分配器内部使用的 STL 容器、`std::jthread` 或日志库（如 `spdlog`）可能会反过来调用 `malloc`。这会导致无限递归和死锁。

- 方案：
  1. 在 `am_malloc` 入口处实现 `thread_local` 递归守卫 (`in_malloc` flag)。
     -  如果 `in_malloc == true`，说明是分配器内部（例如 std::jthread 或 spdlog）触发了二次分配
     - 对策：此时直接走“最底层路径”，绕过 `PageCache`，直接通过 `PageAllocator::SystemAlloc (mmap) `返回内存，或者返回一个预先分配好的小静态缓冲区。
  2. 如果 `in_malloc == false`，将其设为 `true`，执行正常逻辑，结束后设为 `false`。
  3. 在递归路径（`in_malloc == true`）下，由于 `mmap` 分配的内存无法通过 `PageMap::GetSpan` 正常释放（因为我们还没把它封装成 `Span` 注册到树里），这会导致分配器内部泄露（虽然很少）。为了完美解决，可以在递归路径下使用一个简单的“静态紧急缓冲区”或者也将其注册为普通的 `SystemAlloc` 并在 `am_free` 中通过检查 `PageMap` 是否为空来处理。
  
- 优先级：最高。这是实现 `PageHeapScavenger` (基于 `jthread`) 的先决条件。

- [x] #### **HugePageCache 使用 std::vector 违反自举约束 [Bug/Safety]**

- 背景：`PageAllocator` 底层 `HugePageCache` 使用 `std::vector` 作为缓存容器。若 `am_malloc` 已通过 `LD_PRELOAD` 替换系统 `malloc`，`std::vector` 的 `reserve/push_back` 会触发系统 `malloc`，导致无限递归 → 栈溢出。

- 方案：将 `std::vector<void*>` 替换为定长原生数组，并采用 leaky singleton 模式：
  ```cpp
  static constexpr size_t kMaxCacheCapacity = 16;
  void* cache_[kMaxCacheCapacity]{};
  size_t cache_size_{0};
  ```
- 实现细节：使用 placement new + 静态存储避免静态析构顺序问题；LIFO 栈式操作（`cache_[--cache_size_]` 和 `cache_[cache_size_++]`）；提供 `ReleaseAllForTesting()` 接口供 ASan 使用。
- 文件位置：`ammalloc/src/page_allocator.cpp:12-59`
- 状态：已修复（2026-03-06）

- [x] #### **Span::FreeObject 缺少 double-free 防护 [Bug/Safety]**

- 背景：当前实现直接设置 bitmap 位并递减 `use_count`，无重复释放检查。若用户 double-free，`use_count` 会下溢为 `SIZE_MAX`，此后 `AllocObject()` 永远返回 `nullptr`。

- 方案：在 `FreeObject` 中检查 bitmap 位状态，若已释放则触发 `AM_DCHECK`：
  ```cpp
  uint64_t mask = 1ULL << bit_pos;
  AM_DCHECK((bitmap[bitmap_idx] & mask) == 0, "double free detected.");
  bitmap[bitmap_idx] |= mask;
  --use_count;
  ```
- 文件位置：`ammalloc/src/span.cpp:105-107`
- 状态：已修复（2026-03-06）
- 相关测试：`test_span.cpp:DoubleFreeCorruption`

- [x] #### **GetOneSpan Release 模式空指针解引用 [Bug/Safety]**

- 背景：`AM_DCHECK(span != nullptr)` 在 Release 构建（`NDEBUG`）下编译为空。若 `AllocSpan` 返回 `nullptr`（OOM），下一行 `span->Init(size)` 直接崩溃。

- 方案：将 `AM_DCHECK` 替换为运行时检查：
  ```cpp
  auto* span = PageCache::GetInstance().AllocSpan(page_num, size);
  if (!span) return nullptr;
  span->Init(size);
  ```
- 文件位置：`ammalloc/src/central_cache.cpp:303-306`
- 状态：已修复（2026-03-06）

- [x] #### **修复 ObjectPool 内存对齐失效问题 [Bug]**

- 背景：当前 ObjectPool::New 直接在 ChunkHeader (16 Bytes) 之后分配对象。对于 Span (alignas 64) 和 RadixNode (alignas 4096)，这会导致严重的未对齐访问。

- 后果：
  1. RadixNode 跨页导致基数树物理映射错乱。
  2. Span 跨缓存行导致伪共享和原子操作性能下降。
  
- 方案：
  1. 在 ObjectPool::New 中，计算 ChunkHeader 之后的地址。
  2. 使用 alignof(T) 计算需要的 Padding。
  3. 调整 data_ 到对齐后的地址，并确保剩余空间计算扣除了 Padding。
  
- 状态：已完成，见 `ammallloc/page_allocator.h`。

- [x] #### **修复 Radix Tree 地址空间限制 (48-bit vs 57-bit) [Bug]** 

- 背景：当前 4 层基数树仅覆盖 48 位虚拟地址空间 (9 bits * 4 levels + 12 bits offset)。在支持 5-level paging (57-bit) 的现代 CPU (如 Intel Ice Lake+) 上，mmap 可能返回高位地址，导致 `PageMap::GetSpan` 数组越界崩溃。

- 方案：实现"胖根节点"（Fat Root Node）：
  1. 引入 `RadixRootNode` 结构，其大小根据虚拟地址位宽动态计算
  2. 48-bit 模式：`RADIX_ROOT_SIZE = 512`（4KB 根节点）
  3. 57-bit 模式：`RADIX_ROOT_SIZE = 262144`（2MB 根节点）
  4. 通过 CMake 选项 `USE_57BIT_VA` 启用 57-bit 支持（默认 48-bit）
  5. 保持原有的 4 层 9-bit 主树结构不变

- 关键改动：
  ```cpp
  // config.h
  #ifdef AM_USE_57BIT_VA
      static constexpr size_t VA_BITS = 57;
  #else
      static constexpr size_t VA_BITS = 48;
  #endif
  constexpr static size_t RADIX_ROOT_BITS = PAGE_ID_BITS - 3 * RADIX_NODE_BITS;
  constexpr static size_t RADIX_ROOT_SIZE = 1 << RADIX_ROOT_BITS;
  ```

- 文件位置：`ammalloc/include/ammalloc/config.h`, `ammalloc/include/ammalloc/page_cache.h`
- 状态：**已修复（2026-03-07）**
- 设计决策：选择胖根节点而非增加第 5 层，避免热路径多一次解引用；默认 48-bit 兼容绝大多数系统。

- [x] #### **Span v2 64B 重构 [Perf/Architecture]**
- 背景：当前 `Span` 结构体 112 字节，跨越两个缓存行，多线程场景下导致 False Sharing 和 Cache Miss。
- 方案：
  1. **删除字段**: `bitmap`、`data_base_ptr`、`bitmap_num`、`is_used`、`is_committed` → 改为内联计算或位域打包
  2. **类型降级**: `size_t` → `uint32_t` (page_num, obj_size, capacity, use_count, scan_cursor)
  3. **新增字段**: `uint16_t flags` (打包状态位), `uint32_t obj_offset` (替代 data_base_ptr)
  4. **布局优化**: 按 64B 缓存行对齐，分 4 个 16B 区域组织字段
  5. **API 迁移**: 所有 `is_used`/`is_committed` 直接访问改为 `IsUsed()`/`SetUsed()`/`IsCommitted()`/`SetCommitted()` 访问器方法
- 关键改动：
  ```cpp
  struct alignas(64) Span {
      Span* next{nullptr};            // 8B
      Span* prev{nullptr};            // 8B
      uint64_t start_page_idx{0};     // 8B
      uint32_t page_num{0};           // 4B
      uint16_t flags{0};              // 2B: is_used, is_committed
      uint16_t size_class_idx{0};     // 2B
      uint32_t obj_size{0};           // 4B
      uint32_t capacity{0};           // 4B
      uint32_t use_count{0};          // 4B
      uint32_t scan_cursor{0};        // 4B
      uint32_t obj_offset{0};         // 4B: 替代 data_base_ptr
      uint32_t padding{0};            // 4B
      uint64_t last_used_time_ms{0};  // 8B
      // 计算方法替代字段
      [[nodiscard]] AM_ALWAYS_INLINE uint64_t* GetBitmap() const noexcept {
          return static_cast<uint64_t*>(GetPageBaseAddr());
      }
      [[nodiscard]] AM_ALWAYS_INLINE void* GetDataBasePtr() const noexcept {
          return static_cast<char*>(GetPageBaseAddr()) + obj_offset;
      }
  };
  ```
- 涉及文件：
  - `ammalloc/include/ammalloc/span.h` - Span 结构体重写
  - `ammalloc/src/span.cpp` - Init/AllocObject/FreeObject 适配
  - `ammalloc/src/central_cache.cpp` - cleanup 逻辑适配
  - `ammalloc/src/page_cache.cpp` - is_used/is_committed 访问适配
  - `ammalloc/src/page_heap_scavenger.cpp` - 同上
  - `ammalloc/src/ammalloc.cpp` - obj_size 类型适配
  - `tests/unit/test_page_cache.cpp` - 测试断言适配
- 性能预期：消除 False Sharing，多线程场景预期提升 10-20%
- 验证：
  - 单元测试：`--gtest_filter=*Span*:*PageCache*:*CentralCache*:*ThreadCache*` 46/46 通过
  - 多线程压力测试：`MultiThreadStress` 20 线程 1000000 ops 通过
  - 代码清理：删除过时注释代码（central_cache.cpp, page_cache.cpp, page_heap_scavenger.cpp）
- 修复问题：
  - `is_committed` 初始化语义：`AllocSpanLocked` 统一调用 `SetCommitted(true)`
  - `page_num` 上限防护：`size_t → uint32_t` 截断检查
  - `object_size` 范围检查：`Span::Init` 添加 `AM_DCHECK`
- 状态：**已完成 (2026-03-16)**
- Handoff: `ammalloc__page_cache/20260316T120000Z--ses_page_cache_v2--sisyphus.md`, `ammalloc__page_cache/20260316T183731Z--ses_page_cache_review--sisyphus.md`
- 优先级：P0 → P1

- [x] #### **PageCache 代码审查与测试完善 [Code Quality]**
- 背景：`PageCache` 作为 ammalloc 后端核心组件，需要全面的代码审查和测试覆盖。
- 完成工作：
  - 代码审查报告生成 (`docs/reviews/code_review/20260315_page_cache_code_review.md`)
  - 修复 P0 问题：`PageMap::GetSpan` 边界检查缺失
  - 新增 8 个测试用例（5 个增量 + 3 个失败路径）
  - 性能基准测试建设（修复段错误，覆盖单线程/多线程）
  - 注释规范改造 (`page_cache.h`)
- 状态：**已完成 (2026-03-15 ~ 2026-03-16)**

### 🟠 P1: 性能微调 (Performance Tuning)
*基于 Benchmark 数据和 Review 意见的针对性优化，性价比极高。*

- [x] #### **实现 CentralCache 预取机制 (Prefetching) [Perf]**
- 背景：当前 FetchRange 的慢速路径（查 SpanList Bitmap）只获取 batch_num 个对象返回给 ThreadCache，此时 TransferCache 依然是空的。下一次请求大概率又要走慢速路径。
- 方案：
  1. 在 Slow Path 中，一次性从 Span 申请 batch_num + N 个对象（或填满 tc_capacity）。
  2. 将一部分返回给 ThreadCache，剩下的直接填入 TransferCache 数组。
- 收益：大幅减少 Span Bitmap 的扫描频率和 span_lock (Mutex) 的争抢。Benchmark 显示多线程小对象分配吞吐量提升 3-5 倍。
- 状态：已完成，逻辑包含在 `FetchRange` 中。

- [x] #### **优化 SizeClass 边界测试 [Test]**
- 背景：确保索引计算逻辑在跨组边界（如 128B, 129B, 256B）时的绝对正确性。
- 方案：增加针对 SizeClass::Index 和 SizeClass::Size 的互逆性单元测试，覆盖所有 Size Class 的边界值。
- 状态：已完成，见 `tests/unit/test_size_class.cpp`。

- [ ] #### **优化 CentralCache 锁粒度与 Bitmap 扫描 [Perf]** 

- 背景：`FetchRange` 持有 `span_list_lock` 调用 `AllocObject`。对于大 Span (128页)，`AllocObject` 内部的 Bitmap 扫描可能耗时较长，阻塞其他线程。
- 方案：在 `Span` 中维护 `scan_cursor` 提示，记录上次扫描位置，避免每次从头扫描 Bitmap。

- [ ] #### **补齐 OOM 错误处理与 `errno=ENOMEM` 语义 [Correctness]**

- 背景：当前分配失败路径主要依赖返回 `nullptr`，但尚未系统性对齐 POSIX/`malloc` 家族的错误语义。调用方若依赖 `errno == ENOMEM` 判断内存不足，行为会与系统分配器不一致。
- 方案：在 `am_malloc` 及后续补齐的 `am_calloc`/`am_realloc`/`am_memalign` 失败路径统一设置 `errno = ENOMEM`，并梳理参数校验与失败传播，避免遗漏慢路径和系统分配回退分支。

- [ ] #### **移除 SpinLock 硬编码参数瓶颈 [Perf/Maintainability]**

- 背景：当前 SpinLock 的自旋次数、退避策略或相关阈值依赖硬编码常量，难以针对不同核心数、负载和编译配置调优，容易在高并发场景形成隐藏瓶颈。
- 方案：将 SpinLock 关键参数集中到 `config.h` 或独立调优配置，提供编译期/运行期可控入口，并用 benchmark 驱动默认值选择。

#### **优化 ThreadCache 对象分配内存占用 [Mem]**

- 背景：`ThreadCache` 对象本身约 1KB，当前直接使用 `SystemAlloc` 分配一个 4KB 页，每个线程浪费约 3KB 内存。在数千线程的高并发场景下浪费显著。
- 方案：使用全局 `ObjectPool<ThreadCache>` 替代裸页分配。

- [x] #### **PageMap Root 静态化 [Optimize]**
  - 背景：`PageMap` 的 radix tree root 节点通过 `ObjectPool<RadixRootNode>` 动态分配，但整个生命周期只会有一个 root 实例，使用对象池过于冗余。
  - 方案：
    1. 移除 `radix_root_pool_`，改为静态 `RadixRootNode radix_root_storage_`
    2. `SetSpan` 中初始化时直接使用 `&radix_root_storage_`，先清空 children 再 release store 到 `root_`
    3. `Reset()` 不再释放 root pool，仅保留 `root_=nullptr` 和 `radix_node_pool_.ReleaseMemory()`
  - 涉及文件：
    - `ammalloc/include/ammalloc/page_cache.h` - 字段声明变更
    - `ammalloc/src/page_cache.cpp` - SetSpan/Reset 逻辑调整
  - 状态：**已完成 (2026-03-17)**
  - 验证：20/20 单测通过，PageCache 基准测试正常

- [ ] #### **拆分 MAP_POPULATE 与 MADV_WILLNEED 配置 [Config/Perf]**
  - 背景：当前 `use_map_populate_` 同时控制：
    - Normal Page 路径的 `MAP_POPULATE`（mmap 时预分配）
    - Huge Page 路径的 `MADV_WILLNEED`（预取提示）
  - 问题：
    - 两者语义不同：`MAP_POPULATE` 是强制预分配，`MADV_WILLNEED` 是建议性提示
    - 延迟影响不同：Normal Page 通常 4KB-1MB，Huge Page 可能 2MB-256MB+
    - 无法独立控制不同路径的延迟策略
  - 方案：
    ```cpp
    class RuntimeConfig {
        bool use_map_populate_ = false;           // 兼容旧配置
        std::optional<bool> populate_for_normal_; // 独立控制 Normal Page
        std::optional<bool> willneed_for_huge_;   // 独立控制 Huge Page
    };
    ```
  - 优先级：P2（当前共用配置够用，精细化调优场景需求不明确）
  - 参考：Code Review P2-3

### 🟡 P2: 功能对齐 (Feature Parity)
*对齐 TCMalloc/Jemalloc 的核心功能，使其具备在生产环境长期运行的能力。*

- [x] #### **实现 PageHeapScavenger (后台清理线程) [Feature]**
- 背景：目前内存只进不出（除非合并出超大 Span）。长期运行会导致 RSS 虚高。
- 方案：
  1. 启动一个后台守护线程 (`std::jthread` + `stop_token`)。
  2. 定期扫描 PageCache 中闲置超过 `kIdleThresholdMs` (如 5s) 的 Span。
  3. 调用 `madvise(MADV_DONTNEED)` 释放物理内存（保留虚拟地址）。
  4. 维护 Span 的 `is_committed` 状态。
- 状态：**已实现并修复所有 P0 问题**（启动集成、并发安全、析构顺序、时间戳语义）。详见下方关联条目。

- [x] #### **修复 PageHeapScavenger 并发安全风险 [Bug]**
- 背景：`ScavengeOnePass()` 将 Span 从 `span_lists_` 摘下后释放锁执行 `madvise`，此时 `ReleaseSpan()` 可能通过 `PageMap` 找到该 Span 并尝试 `erase`，导致链表损坏。
- 方案：摘下 Span 前临时设置 `span->is_used = true` 使其对合并逻辑不可见，挂回后恢复 `false`。
- 严重级：P0
- 状态：已修复（见 `page_heap_scavenger.cpp:86, 123`）

- [x] #### **修复 PageHeapScavenger 析构顺序 UAF 风险 [Bug]**
- 背景：`PageHeapScavenger` 和 `PageCache` 均为函数内静态单例，析构顺序跨 TU 不确定。
- 方案：改为显式生命周期管理（`ammalloc_init`/`ammalloc_shutdown` 钩子）或采用 leaky singleton 策略。
- 严重级：P0
- 状态：已修复（采用 placement new + 静态存储的 leaky singleton 模式，见 `page_cache.h:127-132`、`page_heap_scavenger.h:16-19`）

- [x] #### **修复 `last_used_time_ms` 语义不一致 [Bug]**
- 背景：新通过系统补货进入 PageCache 的 Span 未初始化 `last_used_time_ms`（默认 0），会被立即误判为 idle。
- 方案：在所有 Span 进入 free list 的路径统一写入当前时间戳，或特判 0 为“不参与清理”。
- 严重级：P2
- 状态：已修复（在 `ReleaseSpan`、`AllocSpanLocked` 的切分路径和系统补货路径统一初始化，见 `page_cache.cpp:227, 256, 332`）

- [ ] #### **实现 ThreadCache 垃圾回收 (GC) [Feature]**
- 背景：线程如果从忙碌转为闲置，其 ThreadCache 中囤积的内存无法被其他线程复用。
- 方案：
  1. 建立全局 ThreadCacheRegistry 链表。
  2. 配合 Scavenger 线程，定期检查 ThreadCache 的使用活跃度。
  3. 强制回收闲置 ThreadCache 的 FreeList 到 CentralCache。

- [ ] #### **补齐 POSIX API 对齐能力 (`calloc` / `realloc` / `memalign`) [Parity]**
- 背景：当前 TODO 仍缺少对 `calloc`、`realloc`、`memalign` 的正式追踪，导致 `ammalloc` 与 POSIX/常见分配器接口能力存在缺口，难以作为通用替换器落地。
- 方案：补齐零初始化分配、扩缩容重分配、显式对齐分配三类接口，并明确与现有 `PageCache`/`CentralCache`/大对象路径的交互语义及回退策略。

- [ ] #### **增强内存安全特性（Poisoning / Double-Free 检测） [Safety]**
- 背景：虽然 `Span::FreeObject` 已补上基础 double-free 防护，但 allocator 仍缺少面向调试和线上排障的系统化内存安全机制，例如释放后填毒、跨层级重复释放检测和非法归属校验。
- 方案：为 debug/asan 友好场景增加 freed-object poisoning、重复释放检测和必要的 ownership 校验开关，在不显著拖慢热路径的前提下提升问题暴露能力。

- [ ] #### **移除 Scavenger 硬编码策略与阈值 [Feature/Maintainability]**
- 背景：`PageHeapScavenger` 的扫描周期、idle 阈值、批处理策略等若长期保持硬编码，会限制不同部署场景下的 RSS/延迟权衡，也不利于后续实验和回归验证。
- 方案：将 Scavenger 的时间阈值、批量大小和启停策略收敛到统一配置层，支持通过编译选项或运行时参数调优。

- [ ] #### **实现统计监控模块 (Statistics) [Observability]**
- 背景：缺乏运行时观测手段，难以排查内存泄漏或碎片化问题。
- 方案：
  1. 完善 PageAllocatorStats。
  2. 实现 GetStats() 接口，输出类似 TCMalloc 的文本报表（包含各层级缓存占用、碎片率、系统申请量等）。

### 🔵 P3: 架构演进 (Architecture Evolution)
*面向超大规模 NUMA 架构或特殊场景的长期规划。*

- [ ] **实现 Page 来源标记机制 (Source Tagging) [Architecture/Correctness]**
  - 背景：`SystemFree` 当前通过 `size == HUGE_PAGE_SIZE && IsHugePageAligned(ptr)` 判断是否可放入 HugePageCache。这个启发式规则过于脆弱：
    - `AllocNormalPage` 理论上也可能返回 2MB 对齐地址
    - 非大页路径分配的内存也可能满足条件
    - 无法区分内存来源，可能错误缓存非大页来源的内存
  - 方案：
    1. **短期**：在 `page_allocator.h` 文档化当前策略是临时启发式，明确局限性
    2. **中期**：引入 `FreeFlags` 参数区分来源：
       ```cpp
       enum class FreeFlags : uint8_t {
           kNone = 0,
           kFromHugePage = 1 << 0,    // 来自大页分配路径
           kFromNormalPage = 1 << 1,  // 来自普通页分配路径
       };
       void SystemFree(void* ptr, size_t page_num, FreeFlags flags = FreeFlags::kNone);
       ```
    3. **长期**：由上层 `Span` 元数据携带来源标记，`PageCache` 在释放时传递给 `PageAllocator`
  - 风险：不修复可能导致缓存污染，后续 `Get()` 返回的内存块不符合预期属性
  - 优先级：P3（当前启发式在 v1 可用，但应在架构演进中解决）
  - 相关文件：`ammalloc/include/ammalloc/page_allocator.h`、`ammalloc/src/page_allocator.cpp`
  - 参考：Code Review P1-5、ADR-003 (乐观大页策略)

- [ ] **大页策略重构 (Huge Page Strategy Overhaul) [Architecture/Perf]**
  - 背景：当前大页策略存在多个可优化点：
    - **乐观对齐命中率未知**：exact-size mmap 返回 2MB 对齐地址的概率极低（ASLR 下约 0.005%），但缺乏统计数据支撑
    - **THP 合并延迟不可控**：`MADV_HUGEPAGE` 依赖 khugepaged 异步合并，可能存在 10ms+ 延迟尖刺
    - **MAP_HUGETLB 未利用**：显式大页可提供严格延迟保证，但需要系统配置
    - **缺少大页来源追踪**：无法区分内存块是来自 THP 还是预留大页
  - 方案：
    1. **短期（数据驱动）**：增加对齐命中率统计
       ```cpp
       // 在 AllocHugePage 中增加
       if (IsHugePageAligned(ptr)) {
           stats_.huge_exact_align_hit_count.fetch_add(1, ...);
       } else {
           stats_.huge_exact_align_miss_count.fetch_add(1, ...);
       }
       ```
    2. **中期（可选 MAP_HUGETLB）**：作为可配置的后端选项
       ```cpp
       struct RuntimeConfig {
           bool use_explicit_huge_pages_{false};  // 是否尝试 MAP_HUGETLB
       };
       // AllocHugePage 中：先尝试 MAP_HUGETLB（若启用），失败则 fallback 到 THP
       ```
    3. **长期（大页策略抽象层）**：
       ```cpp
       class HugePageBackend {
       public:
           virtual ~HugePageBackend() = default;
           virtual void* Allocate(size_t size) = 0;
           virtual void Deallocate(void* ptr, size_t size) = 0;
       };
       // 可插拔实现：THPBackend, HugeTLBBackend, HybridBackend
       ```
  - 技术对比：
    | 策略 | 优点 | 缺点 | 适用场景 |
    |------|------|------|---------|
    | THP (MADV_HUGEPAGE) | 零配置、灵活 | 异步合并、延迟不可控 | 通用场景 |
    | MAP_HUGETLB | 严格延迟保证、立即分配 | 需预留、不可换出、SIGBUS 风险 | HFT、DPDK |
  - 风险：
    - MAP_HUGETLB 需 root 预留大页，池耗尽时触发 SIGBUS
    - 大页内存不可换出，可能导致其他进程 OOM
    - 内部碎片：分配 2MB 但只用部分会浪费物理内存
  - 优先级：P3（当前 THP 策略在 v1 可用，演进提升可配置性和延迟确定性）
  - 相关文件：`ammalloc/src/page_allocator.cpp`、`ammalloc/include/ammalloc/config.h`
  - 参考：Code Review P2-2、ADR-003、业界实践（TCMalloc Temeraire、Jemalloc HPA）
  - 子任务：
    - [ ] 增加对齐命中率统计（`huge_exact_align_hit_count`、`huge_exact_align_miss_count`）
    - [ ] 评估是否保留乐观对齐策略或直接 over-allocate + trim
    - [ ] 实现 MAP_HUGETLB 可选后端
    - [ ] 设计大页来源追踪机制（THP vs HugeTLB）
    - [ ] 文档化大页配置指南（Docker/Kubernetes 环境）

- [ ] **PageCache 分片 (Sharding) [Scalability]**
  - 背景：在超多核（>64 Core）场景下，PageCache 的全局大锁可能成为瓶颈。
  - 方案：将 PageCache 拆分为多个独立的 PageHeap（例如按 CPU ID 取模），降低锁粒度。

- [ ] **NUMA 感知 (NUMA Awareness) [Perf]**

- 背景：跨 NUMA 节点访问内存延迟较高。
- 方案：为每个 NUMA 节点维护独立的 CentralCache 和 PageCache 实例，优先分配本地内存。

- [ ] **Malloc Hooks [Feature]**

- 背景：用户需要自定义内存分配行为（如统计、故障注入）。
- 方案：提供 AddMallocHook / RemoveMallocHook 接口。

### 📝 变更日志 (Changelog)

- 2026-03-19
  - **PageAllocator SystemFree 审查与大页策略演进**：
    - 完成 `SystemFree` 代码审查，识别 4 个待修复问题：
      - P1: madvise 失败后仍尝试缓存
      - P1: madvise 失败未捕获 errno
      - P2: 缺少 `huge_cache_put_*` 统计
      - P3: huge cache 资格判断脆弱
    - 完成 `HugePageCache` 实现审核，识别 3 个待修复问题：
      - P1: Pop CAS success 内存序 (`acquire` → `acq_rel`)
      - P2: ReleaseAllForTesting 缺少 errno 捕获
      - P2: Put 缺少 debug 校验
    - 完成 Code Review P2-2/P2-3 建议合理性评估
    - 完成 MAP_HUGETLB vs THP 技术调研
    - 新增 3 个演进条目（见 P1/P3 章节）
    - 更新头文件注释：`page_allocator.h` SystemFree 接口文档

- 2026-03-17
  - **PageMap Root 静态化优化**：
    - 移除 `ObjectPool<RadixRootNode> radix_root_pool_`，改为静态 `RadixRootNode radix_root_storage_`
    - 更新 `SetSpan` 初始化路径：先清空 children，再 release store 发布到 `root_`
    - 更新 `Reset` 逻辑：不再调用 root pool 释放，仅保留 `root_=nullptr`
    - 保持 `GetSpan` 无锁读路径与内存序语义不变
    - 文件变更：`page_cache.h:91-93`, `page_cache.cpp:54-60, 158-161`
    - 验证：20/20 单测通过，设计文档同步更新

- 2026-03-14
  - 完成 PageAllocator P0 修复：
    - 降级大页污染缓存：添加对齐校验门禁（`page_allocator.cpp:285-290, 327-332`）
    - 页数转换溢出保护：三处溢出 guard（`page_allocator.cpp:181-186, 264-268, 315-319`）
    - nullptr cache-hit 回归：添加 `ptr &&` 非空判断（`page_allocator.cpp:285`）
    - 缓存容量配置化：改用 `PageConfig::HUGE_PAGE_CACHE_SIZE`（`page_allocator.cpp:43, 66`）
  - 新增边界回归测试：`OverflowGuard_SystemAlloc`、`OverflowGuard_SystemFree`、`NonHugeSizedFreeDoesNotPopulateHugeCache`、`AdjacentHugeBoundaryDoesNotPopulateHugeCache`
  - **TODO 列表扩展**：基于代码审查补充未记录的功能缺失与技术债：
    - P1 新增：POSIX 错误处理 (`errno ENOMEM`)、SpinLock/TransferCache 硬编码参数移入 RuntimeConfig
    - P2 新增：POSIX API 补齐 (`calloc`/`realloc`/`memalign`)、内存安全防护 (poisoning/red zones/quarantine)、Scavenger/MAX_TC_SIZE 硬编码消除

- 2026-3-5
  - 完成 PageHeapScavenger 所有 P0 问题修复：
    - Off-list Span 并发安全：`is_used` 标记保护（`page_heap_scavenger.cpp:86, 123`）
    - 静态析构顺序 UAF：placement new + BSS 段静态存储的 leaky singleton（`page_cache.h`、`page_heap_scavenger.h`）
    - `last_used_time_ms` 语义：在切分、系统补货、释放三处统一初始化（`page_cache.cpp:227, 256, 332`）
  - 启动集成完成：`EnsureScavengerStarted()` 在 `am_malloc_slow_path` 中调用，支持 `AM_ENABLE_SCAVENGER` 环境变量控制
  - 更新开发日志记录完整修复过程。
  - **PageHeapScavenger 功能现在完整可用**。

- 2026-3-4
  - 完成 `PageHeapScavenger` 核心实现，标记为完成（待修复并发安全问题后可用）。
  - 同步 `docs/reviews/code_review/` 中的 Code Review 问题到本 TODO 列表。
  - 新增 P0 级别问题：HugePageCache std::vector 自举违反、Span::FreeObject double-free 漏洞、GetOneSpan Release 空指针解引用。
  - 新增 P1 级别问题：ThreadCache 初始化锁、CentralCache unlock-relock 窗口、TLS 析构顺序。
  - 新增 P2/P3 级别功能需求：am_realloc/calloc/memalign、data 区域验证、ObjectPool 归属检查等。
  - 更新开发日志 (`docs/logs/development_log.md`) 记录 PageHeapScavenger 架构决策。

- 2026-3-1
  -  增加 Code Review 发现的 5 个新优化项 (Radix Tree Fix, CentralCache Lock, ThreadCache Pool, Safety)。
- 2026-2-27: 
  - 初始化 TODO 列表。
  - 修复 ObjectPool 内存对齐 Bug。
  - 增加 SizeClass 边界单元测试。
  - 实现 CentralCache 预取机制，性能大幅提升 (Benchmark: +350% ~ +500% in multi-threaded small object allocs)。
