# ammalloc 全面提升与演进方案

> 文档状态：规划草案  
> 适用范围：ammalloc 核心分配器、系统分配接口、测试基准及 aethermind 集成  
> 总体目标：将 ammalloc 建设为可安全接管进程内存、可作为后续项目底层基础设施，并在功能、性能、内存效率和可观测性方面对标 TCMalloc、jemalloc 的工业级用户态内存分配器。

## 1. 总体结论

ammalloc 现有的 `ThreadCache -> CentralCache -> PageCache -> PageAllocator` 分层架构是一个良好起点，但距离“可替换系统 malloc、可作为 aethermind 基础设施、综合对标 TCMalloc/jemalloc”还需要跨越三个阶段：

1. 达到工业级正确性、自举安全性和 ABI 完整性。
2. 解决并发扩展、内存碎片和 RSS 回收问题。
3. 建设 NUMA、采样分析、安全加固及推理引擎专用扩展能力。

实施过程中应首先保证正确性和生命周期安全，再推进无锁化与极限性能优化。当前 TLS 生命周期、OOM 处理、对齐、PageMap/Span 生命周期以及分配器递归等基础问题没有解决前，不宜扩大并发结构的复杂度。

## 2. 产品定位与运行模式

TCMalloc 采用 Frontend/Middle-end/Backend 分层，现代实现支持 per-CPU cache、动态缓存容量、hugepage-aware backend 和低开销采样。jemalloc 的主要优势包括多 arena、tcache、dirty/muzzy decay、后台 purge、extent hook、运行时控制和完整统计体系。

ammalloc 不必逐项复制两者，而应形成以下产品定位：

- **默认模式**：在低延迟与合理 RSS 之间保持均衡。
- **Latency 模式**：扩大前端缓存、降低 purge 频率，面向在线推理。
- **Memory 模式**：缩小缓存、积极 decay，面向多模型或多租户部署。
- **Hardened 模式**：启用抽样防护、指针校验和 quarantine。
- **Aethermind 扩展模式**：提供 NUMA、生命周期、热冷和 arena hint，但不污染标准 malloc ABI。

## 3. 目标架构

```text
标准 C/C++ ABI                    ammalloc 扩展 API
malloc/free/new/delete            arena/NUMA/alignment/lifetime hint
          |                                  |
          +---------------+------------------+
                          v
                       Frontend
                 +--------+--------+
                 |                 |
          Per-thread Cache   Per-CPU Cache
            第一阶段          Linux/rseq 可选
                 |                 |
                 +--------+--------+
                          v
               NUMA-local Middle-end
          +---------------+----------------+
          |                                |
  Sharded TransferCache       Central Free Lists
                                     |
                                     v
                          PageHeap / Extent Manager
                 +-------------------+------------------+
                 |                   |                  |
          Small-run buckets   Large extent tree   Hugepage regions
                 +-------------------+------------------+
                                     |
                         mmap/madvise/munmap

横向基础设施：
- PageMap + 稳定的 Span 生命周期
- 统计、采样、Profiling 和内存压力反馈
- 无分配的自举、错误处理与配置系统
```

## 4. 第一优先级：正确性、自举与 ABI

本节定义 ammalloc 从“显式调用的 C++ 分配 API”演进到“可安全接管整个进程内存”的基础契约。这里的工作不是普通功能补齐，而是后续 PageCache 分片、per-CPU cache、NUMA 和 hugepage-aware backend 的前置门禁。任何违反本节契约的实现，即使在微基准中更快，也不能进入系统 malloc 替换模式。

### 4.1 目标、适用范围与硬性不变量

本节同时覆盖三种部署形态：

1. **显式 API 模式**：业务只调用 `ammalloc::am_malloc/am_free`，系统 malloc 仍由 libc 提供。
2. **链接替换模式**：程序在链接期选择 ammalloc 提供的标准 C/C++ 分配符号。
3. **动态拦截模式**：通过 `LD_PRELOAD` 或等价机制拦截进程内标准分配符号。

三种模式共享分配器核心，但风险等级不同。显式 API 模式可以容忍进程中同时存在多个 allocator；链接替换和动态拦截模式则必须假设初始化、日志、动态加载器、线程库和第三方依赖都可能递归进入 ammalloc。

以下不变量必须在 Release 构建中成立，不能只依赖 `AMMALLOC_DCHECK`：

| 类别 | 硬性不变量 | 违反后的后果 |
|---|---|---|
| 地址 | 成功返回的指针满足对应 API 的对齐要求 | 未对齐访问、ABI 不兼容、未定义行为 |
| 大小 | 返回区域的 usable size 不小于请求大小 | 用户写越界和相邻元数据破坏 |
| 算术 | 加法、乘法、round-up、page-count 换算均不可溢出 | 小映射承载大请求、越界和任意内存破坏 |
| 所有权 | 每个 live allocation 恰好由一个 allocator domain 和一个 Span/extent 拥有 | double ownership、重复回收或泄漏 |
| 失败原子性 | 分配失败不发布半初始化对象；realloc 失败不改变原对象 | PageMap 悬挂、原数据丢失 |
| 生命周期 | PageMap 读者持有的 Span 在读者退出前不能结束生命周期或被复用 | Use-After-Free、错误 size class |
| 并发 | 同一 live allocation 的元数据状态转换具有唯一串行化点 | bitmap、use_count 和 freelist 损坏 |
| 自举 | 初始化及递归路径不调用可能回到标准 malloc 的设施 | 无限递归、死锁、栈溢出 |
| TLS | 线程退出时 ThreadCache 可安全归还对象；进程关闭时不访问已销毁全局状态 | 线程级泄漏、静态析构 UAF |
| ABI | C 边界不传播 C++ 异常；标准符号的返回值、`errno` 和异常语义稳定 | 调用方状态损坏或兼容性失败 |
| 回收 | 已发布到无锁读结构的元数据只能延迟回收 | 无锁读者观察已复用对象 |
| 可诊断性 | 失败路径可在不分配内存的前提下报告最小错误信息 | 错误处理本身递归崩溃 |

建议把这些不变量转化为三类工程资产：

- 头文件中的静态断言和 API 文档；
- 实现中的 Release 运行时检查及 Debug 深度检查；
- 单元测试、故障注入、Sanitizer 和 ABI 兼容测试。

### 4.2 当前发布阻断项

基于当前实现，至少存在以下进入系统 malloc 替换模式前必须关闭的问题：

| 问题 | 当前表现 | 目标行为 | 主要位置 |
|---|---|---|---|
| TLS 清理未被可靠激活 | 独立的 `thread_local` cleaner 未形成可靠 RAII 所有权 | 线程退出时先 drain FreeList，再释放 ThreadCache metadata | `src/ammalloc.cpp` |
| ThreadCache OOM | `SystemAlloc` 失败后可能在空地址 placement new | 返回空指针并设置正确失败状态 | `src/ammalloc.cpp` |
| CentralCache OOM 锁状态 | 解锁后失败返回可能破坏调用方 `unique_lock` 假设 | 所有返回路径维持统一的锁所有权契约 | `src/central_cache.cpp` |
| 小对象基础对齐 | 8B size class 可能产生非 16B 对齐的后续对象 | 遵循公开 ABI 的基础对齐矩阵 | `include/ammalloc/size_class.h`、`src/span.cpp` |
| Span 回收 | PageMap 无锁读取后，Span 可能较快回对象池复用 | 采用稳定 metadata 或 epoch 延迟回收 | `src/page_cache.cpp` |
| Scavenger 生命周期 | 缺少完整、幂等且全局有序的 shutdown | 停止后台线程后才能进入 allocator teardown | `page_heap_scavenger.*` |
| 错误日志递归 | spdlog、iostream、`std::format` 可能分配 | 核心失败路径使用无分配日志 | `src/page_allocator.cpp`、`include/ammalloc/assert.h` |
| 标准 ABI 缺失 | 仅提供命名空间内 `am_malloc/am_free` | 提供完整 C/C++ 符号与语义测试 | `include/ammalloc/ammalloc.h` |
| 测试清理与生产销毁混用 | `Reset()` 会破坏可继续使用的 backing state | 分离 clear、purge、test reset 和 final destroy | CentralCache、PageCache |

这些问题应作为 P0/P1 阶段的验收清单，而不是分散的“后续优化项”。

### 4.3 初始化状态机

#### 4.3.1 状态定义

建议使用单一原子状态机管理全局 allocator runtime：

```cpp
enum class AllocatorState : uint8_t {
    kUninitialized = 0,
    kInitializing,
    kReady,
    kDegraded,
    kShuttingDown,
    kShutdown,
};
```

状态含义如下：

| 状态 | 含义 | 允许的分配路径 |
|---|---|---|
| `kUninitialized` | 尚未开始全局初始化 | 当前线程尝试成为初始化者 |
| `kInitializing` | 一个线程正在初始化核心结构 | 初始化线程递归和其他并发线程走 BootstrapAllocator |
| `kReady` | 正常分层分配器可用 | ThreadCache/CentralCache/PageCache 正常路径 |
| `kDegraded` | 核心初始化失败，但 bootstrap 后端仍可工作 | 仅 BootstrapAllocator 或直接映射路径 |
| `kShuttingDown` | 已阻止新后台工作，正在清理受控资源 | 只允许可安全释放的路径；新分配走降级路径或按策略失败 |
| `kShutdown` | 受控测试/卸载结束 | 不再进入已销毁的缓存结构 |

生产 interposer 更适合使用“核心单例常驻到进程退出”的策略，避免依赖 C++ 静态析构顺序；`kShuttingDown/kShutdown` 主要用于测试、显式关闭或确实支持 DSO 卸载的构建。

#### 4.3.2 状态转换

```text
kUninitialized
      |
      | CAS 成功，当前线程成为初始化者
      v
kInitializing ------------------------+
      |                               |
      | 初始化成功                    | 初始化失败
      v                               v
   kReady                         kDegraded
      |                               |
      +---------------+---------------+
                      | 显式 shutdown
                      v
               kShuttingDown
                      |
                      v
                 kShutdown
```

不允许的转换包括：

- `kReady -> kInitializing`：正常运行期间不能原地重建核心单例；
- `kShutdown -> kReady`：除非构建了独立的测试 runtime 对象，否则不支持原地复活；
- 多线程同时从 `kUninitialized` 安装不同的全局结构；
- 初始化失败后仍把部分结构发布为 `kReady`。

#### 4.3.3 递归守卫

状态机不能替代线程级递归守卫。初始化线程在 `kInitializing` 状态下如果因为动态加载器、线程库或错误处理再次调用 `malloc`，等待全局状态会形成自死锁。

建议使用不会动态初始化的初始执行模型 TLS 标志：

```cpp
thread_local uint32_t allocator_entry_depth = 0;
```

入口规则：

1. 进入标准分配符号时先读取状态和递归深度。
2. `allocator_entry_depth != 0` 时直接走 BootstrapAllocator。
3. 首次进入时递增深度，通过 RAII 或无异常的 scope guard 确保所有返回路径恢复。
4. 初始化者在执行任何全局初始化前已经设置递归深度。
5. `free` 同样需要识别 bootstrap allocation，不能假设只有分配会递归。

递归深度而非单个布尔值的好处是可以诊断多层递归，并避免异常或早返回导致标志永久卡住。

#### 4.3.4 并发首次初始化

推荐流程：

```text
malloc(size)
  |
  +-- state == Ready && depth == 0 --> 正常快路径
  |
  +-- depth != 0 -------------------> BootstrapAllocator
  |
  +-- state == Uninitialized
  |      |
  |      +-- CAS 成功 --> 初始化 --> release-store Ready/Degraded
  |      |
  |      +-- CAS 失败 --> 重新读取状态
  |
  +-- state == Initializing --------> BootstrapAllocator
  |
  +-- state == Degraded ------------> BootstrapAllocator
```

其他线程观察到 `kInitializing` 时不应无限自旋等待。动态加载器锁、`pthread_create` 内部锁或信号处理可能让初始化者依赖当前等待线程。更稳妥的策略是让并发线程暂时使用线程安全的 BootstrapAllocator；初始化成功后，新分配自动转入正常路径，已经返回的 bootstrap allocation 保持原所有权直到释放。

#### 4.3.5 原子内存序

- 初始化者通过 CAS 从 `kUninitialized` 进入 `kInitializing`，成功序使用 `std::memory_order_acq_rel`，失败序使用 `std::memory_order_acquire`。
- 初始化完成后使用 `std::memory_order_release` 发布 `kReady` 或 `kDegraded`。
- 热路径使用 `std::memory_order_acquire` 读取 `kReady`，确保看到完整初始化的全局结构。
- 纯统计计数使用 `std::memory_order_relaxed`。
- 禁止依赖默认 `seq_cst`。

如果实测 acquire 状态读取对 3～5 ns 热路径有可见影响，可在证明对象发布关系仍成立的前提下设计独立的只读 ready fast flag，但不能先降级内存序再补正确性论证。

### 4.4 BootstrapAllocator 设计

#### 4.4.1 职责与边界

BootstrapAllocator 只解决以下问题：

- allocator 核心初始化期间的递归分配；
- 其他线程与初始化并发时的临时分配；
- 核心初始化永久失败后的可控降级；
- shutdown 后不能再访问正常缓存时的必要分配。

它不是正常小对象快路径，也不承担长期高吞吐目标。因此可以接受每次分配一次 `mmap` 或使用简单锁，但必须正确、可释放、线程安全且不递归。

#### 4.4.2 候选方案

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| 固定静态紧急缓冲区 | 实现简单、无系统调用 | 容量有限、难以 free、并发和对齐复杂 | 只适合最终错误报告，不作为通用 bootstrap heap |
| 每次分配独立 mmap | 生命周期清晰、容易 munmap | 小分配 VMA 和 syscall 成本高 | 适合作为最早期和大块 fallback |
| 预留 bootstrap region + 简单子分配 | 小对象效率较好、VMA 数量受控 | 需要独立元数据、回收和并发设计 | 推荐作为稳定实现 |
| 直接借用正常 PageCache | 复用现有代码 | 初始化环和递归无法消除 | 禁止 |

推荐采用“两级方案”：

1. 最早期和超出 region 能力的请求使用独立 mmap；
2. 初始化首次成功取得 bootstrap region 后，小请求使用 region 内的简单 size class 或 free list；
3. 所有 bootstrap block 均带有独立、可验证的 header；
4. 正常 allocator ready 后不迁移已有 block，释放时仍回原 bootstrap domain。

#### 4.4.3 Bootstrap block 元数据

建议的概念布局：

```text
mapping base
  +------------------------+
  | BootstrapHeader        |
  | magic / version        |
  | mapping base + length  |
  | requested / usable     |
  | alignment / flags      |
  | state / checksum       |
  +------------------------+
  | alignment padding      |
  +------------------------+
  | user memory            |  <-- 返回给调用方
  +------------------------+
```

设计要求：

- header 自身不能由正常 allocator 分配；
- `mapping_size`、padding 和 user offset 的计算全部检查溢出；
- user pointer 满足基础或显式 alignment；
- header 包含固定 magic 和版本，Hardened 模式可增加 cookie/checksum；
- free 能从 user pointer 恢复 header 和原始映射范围；
- double free 在 Debug/Hardened 模式中可通过状态位识别；
- 释放独立 mmap block 时直接 `munmap(mapping_base, mapping_size)`；
- region block 归还 bootstrap free list，不进入 ThreadCache/CentralCache。

标准 C 对无效 free 的行为本身是未定义的，因此不要求默认模式安全探测任意随机地址；但对所有由 BootstrapAllocator 返回的合法指针必须稳定识别，不能因为 PageMap 已经 ready 就把它误判为外部指针。

#### 4.4.4 所有权判定顺序

`free(ptr)` 建议使用以下逻辑：

1. `ptr == nullptr`：直接返回。
2. 快速检查正常 PageMap 是否存在合法 Span，存在则走正常释放。
3. 检查是否为 bootstrap block，存在则回 BootstrapAllocator。
4. 如果两者都不识别：
   - 默认兼容模式按“非法 free”为未定义行为，可在 Hardened 模式终止并输出无分配诊断；
   - 不建议自动转发给 libc `free`，否则容易掩盖 allocator 混用并重新引入动态解析递归。

若 bootstrap block 所在地址恰好被正常 PageMap 覆盖，必须通过 allocator domain/generation 消除歧义。最简单的做法是让 bootstrap region 与正常 allocator region 完全分离，并保证 PageMap 不注册 bootstrap 地址。

#### 4.4.5 并发与信号边界

- BootstrapAllocator 至少需要支持多线程并发初始化场景。
- 可使用一个独立 TTAS lock 或原子 bump pointer；该锁不能与正常 allocator 锁形成嵌套。
- 不要求默认实现 async-signal-safe；如果未来支持信号处理器分配，需要单独设计固定容量的 signal-safe arena。
- 锁竞争、region 耗尽和 mmap 失败只能进入无分配失败报告，不能调用 spdlog 或 iostream。

### 4.5 初始化依赖和发布顺序

#### 4.5.1 初始化依赖图

建议按以下顺序构建和发布：

```text
Raw OS primitives
  -> BootstrapAllocator
  -> RuntimeConfig snapshot
  -> PageAllocator runtime
  -> PageMap static root state
  -> PageCache shards and metadata pools
  -> CentralCache backing storage
  -> Frontend/TLS registration capability
  -> state = Ready
  -> lazy Scavenger start
```

关键原则：

- `RuntimeConfig` 读取环境变量时不得构造 `std::string` 或进行日志输出；可直接遍历 `environ` 或使用已经证明不分配的解析路径。
- PageMap root、PageCache 和 CentralCache 构造期间需要的元数据只能来自静态存储、BootstrapAllocator 或 PageAllocator raw mapping。
- CentralCache TransferCache backing 完整切分并初始化后才能发布 ready。
- Scavenger 线程不能成为 allocator ready 的必要条件；它应在 ready 后的慢路径延迟启动。
- 初始化失败必须逆序回滚尚未发布的资源，或把它们转为 BootstrapAllocator 明确拥有的常驻映射。

#### 4.5.2 发布粒度

不建议分别发布多个“看起来已经可用”的全局单例。正常分配入口只能通过统一的 `kReady` 判断进入分层 allocator；在此之前，即使 PageMap 或 PageCache 已构造完成，也仍走 bootstrap 路径。

这种全有或全无的发布方式可以避免：

- PageMap 已可查询但 Span pool 尚未初始化；
- CentralCache 已暴露但 TransferCache backing 尚未准备；
- ThreadCache 已创建但其析构依赖尚未稳定；
- 初始化失败后留下部分可见的共享状态。

### 4.6 TLS、关闭与进程生命周期

#### 4.6.1 ThreadCache RAII 所有权

ThreadCache 指针和 cleaner 应合并为一个 TLS holder：

```text
ThreadCacheHolder
  - ThreadCache* cache
  - bool initialized
  - destructor: Drain -> Destroy -> SystemFree
```

要求：

- 首次访问 holder 时必须实际触发其 TLS 初始化；
- holder 析构前全局 CentralCache/PageCache 必须仍然可用；
- 如果进程已经进入 `kShuttingDown`，holder 使用专门的安全 drain 策略，不能访问已释放 backing；
- TLS 析构发生递归分配时走 BootstrapAllocator；
- ThreadCache 中对象按 size class 批量归还，不逐对象获取全局锁；
- 线程退出路径不得抛出异常。

#### 4.6.2 生产进程退出策略

对系统 allocator，推荐以下策略：

- PageMap、PageCache、CentralCache 和 BootstrapAllocator 采用进程生命周期常驻对象；
- 不依赖 C++ 静态析构自动释放它们；OS 会在进程退出时回收地址空间；
- 显式停止 Scavenger，或者让其 `jthread`/pthread 生命周期有受控的退出钩子；
- 只有测试 runtime 或明确的嵌入式场景执行完整 destroy。

“退出时不逐页 munmap”不是内存泄漏缺陷，而是避免静态析构顺序 UAF 的有意策略。测试环境仍需要独立 runtime 或受控 Reset 来验证资源释放。

#### 4.6.3 受控 shutdown 顺序

如果必须支持完整关闭，顺序应固定为：

1. 原子状态进入 `kShuttingDown`，禁止启动新的后台工作；
2. 请求 Scavenger 停止并 join；
3. 阻止新线程进入正常分配路径；
4. 等待 allocator 活跃操作进入静默期；
5. drain 当前可控的 ThreadCache/per-CPU cache；
6. drain CentralCache TransferCache 和 SpanList；
7. PageCache 释放 free extent；
8. PageMap 在无读者条件下清理 radix metadata；
9. 释放 BootstrapAllocator 可释放资源；
10. 发布 `kShutdown`。

不能在仍允许任意业务线程调用 malloc/free 时执行第 4 步之后的操作。

### 4.7 `fork()`、动态加载与静态链接

#### 4.7.1 `fork()` 处理

多线程进程 `fork()` 后，子进程只保留调用 fork 的线程；其他线程持有的 allocator 锁不会自动恢复。因此需要注册 `pthread_atfork`：

- **prepare**：停止或冻结 Scavenger，按全局固定顺序取得 allocator 管理锁；
- **parent**：按逆序释放锁并恢复 Scavenger；
- **child**：重置锁、后台线程状态和仅属于消失线程的 cache 注册信息，只保留当前线程 TLS；
- child 中首次分配前重新建立必要的 runtime 状态。

如果第一阶段不支持多线程 fork，必须明确记录限制并在测试中检测，而不能静默死锁。

#### 4.7.2 链接模式

建议输出两个清晰分离的目标：

- `libammalloc.so`：只导出 `am_*` 显式 API，适合早期接入和双 allocator 对比；
- `libammalloc_proxy.so`：导出标准 C/C++ 符号并链接相同核心，适合链接替换或 `LD_PRELOAD`。

这样可以避免为了 interposition 而让普通开发、单测和基准始终处于高风险符号环境。

#### 4.7.3 动态加载限制

- 不支持在进程已经使用另一个 allocator 创建大量 live object 后再动态加载并接管 free。
- 不建议对未知指针通过 `dlsym(RTLD_NEXT, "free")` 自动转发；动态符号解析本身可能分配并递归。
- interposer 必须在进程启动早期加载，并保持到进程结束。
- allocator DSO 默认应设置为不可安全卸载；若确需 `dlclose`，必须证明所有 live allocation 和后台线程都已清空。

### 4.8 标准 C ABI 语义

#### 4.8.1 接口集合

建议第一批稳定导出：

```c
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t new_size);
void* reallocarray(void* ptr, size_t count, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);
size_t malloc_usable_size(void* ptr);
```

项目自有命名空间保留等价的 `am_*` 接口，标准符号包装层只负责 ABI 语义、错误码和进入核心 allocator。

#### 4.8.2 语义矩阵

| API/场景 | 推荐语义 | 错误报告 | 关键实现要求 |
|---|---|---|---|
| `malloc(size > 0)` | 成功返回至少 `size` 字节 | 失败返回 null，`errno=ENOMEM` | 所有 round-up 检查溢出 |
| `malloc(0)` | 沿用项目策略，返回可被 free 的最小 allocation；失败仍可返回 null | 失败时 `errno=ENOMEM` | 文档和测试固定该策略 |
| `free(nullptr)` | no-op | 不修改 errno | 不触发初始化和锁 |
| `free(valid)` | 回原 allocator domain | 无返回值 | 支持 normal/bootstrap 两类所有权 |
| `free(invalid)` | C 标准下为未定义行为 | Hardened 模式中终止并诊断 | 默认模式不能误伤合法 bootstrap block |
| `calloc(n, s)` | 分配 `n*s` 并将可见请求区清零 | 溢出或 OOM 返回 null，`errno=ENOMEM` | 乘法溢出必须先于 round-up |
| `calloc(0, s)` | 与项目 zero-size policy 一致，返回可 free 的最小零填充块或 null | 失败时 `errno=ENOMEM` | 行为必须稳定 |
| `realloc(nullptr, n)` | 等价于 `malloc(n)` | 同 malloc | 复用统一入口 |
| `realloc(p, 0)` | 推荐定义为释放 `p` 并返回 null | 不将策略性 null 视为 OOM | 作为 ammalloc 明确兼容策略记录 |
| `realloc(p, n)` 原 class 可容纳 | 可原地返回原地址 | 成功不修改 errno | usable size 必须覆盖新请求 |
| `realloc(p, n)` 需要迁移 | 新分配、复制、再释放旧块 | 失败返回 null、`errno=ENOMEM`，旧块保持有效 | 复制 `min(old_usable, n)`；失败原子性 |
| `reallocarray` | 先检查 `count*size` 再执行 realloc | 溢出返回 null，`errno=ENOMEM` | 不允许乘法 wrap |
| `aligned_alloc(a, n)` | `a` 为有效 2 的幂，且 `n` 满足标准倍数要求 | 非法参数返回 null；建议 `errno=EINVAL` | 返回地址满足 `a` |
| `posix_memalign(&p,a,n)` | 成功返回 0 并写入 `p` | 非法对齐返回 `EINVAL`；OOM 返回 `ENOMEM` | 失败时不得修改 `*memptr` |
| `malloc_usable_size(p)` | 返回当前 allocation 可安全访问的 usable size | 仅接受合法 live pointer | 不可作为 realloc 替代手段 |

`errno` 处理必须只在失败时写入；成功路径不应为了恢复 errno 增加 TLS 访问。`free` 不应修改调用方 errno。

#### 4.8.3 `calloc` 清零策略

- 新 mmap 区域由内核保证零页，可避免重复 `memset`，但必须有明确的“fresh mapping”标志。
- 从 ThreadCache、CentralCache 或 retained extent 复用的对象必须显式清零请求可见区域。
- 清零超出 requested size 到 usable size 是否有收益，应由安全策略和基准决定。
- Hardened 模式的 free poisoning 与 calloc 清零必须协调，不能把 poison 当作已经清零。

#### 4.8.4 `realloc` 原地优化

小对象：

- 新大小仍落在同一 size class 时可直接返回原地址；
- 新大小变小但跨 class 时，第一阶段也可保留原对象，避免迁移；
- 需要保证 `malloc_usable_size` 和统计能够反映实际 usable size。

大对象：

- 可尝试利用右侧连续 free extent 原地扩展；
- 可尝试释放尾部完整页完成原地收缩；
- PageMap 更新、extent 拆分和回滚必须在统一事务内完成；
- 第一阶段可以只实现“新分配 + copy + free”，优先保证正确性。

### 4.9 对齐契约

建议定义以下对齐矩阵：

| 接口或大小 | 最低对齐要求 |
|---|---|
| 标准 `malloc/calloc/realloc` | 至少满足平台 `alignof(std::max_align_t)`，目标平台通常为 16B |
| 普通 C++ `operator new` | 满足 `__STDCPP_DEFAULT_NEW_ALIGNMENT__` |
| `aligned_alloc/posix_memalign` | 满足调用方请求且不低于接口规定最小值 |
| C++ aligned new | 满足 `std::align_val_t` 指定值 |
| PageAllocator 映射 | OS page 对齐 |
| Hugepage 专用接口 | 对应 hugepage 大小或明确的降级语义 |

SizeClass 生成和 Span 布局必须共同保证对齐：仅将数据区起点对齐到 16B 不足以保证后续 8B stride 对象也保持 16B 对齐。

实现需要统一使用 checked alignment helper：

```text
CheckedAlignUp(size, alignment)
  - alignment 必须为合法值
  - size + alignment - 1 不得溢出
  - 返回值不得小于 size
```

过对齐分配可以采用独立 extent、在大块内部增加 padding 并记录原始 base，或建立专用 aligned size class。第一阶段优先使用独立 extent，避免污染普通小对象热路径。

### 4.10 完整 C++ ABI

#### 4.10.1 必需符号

至少覆盖：

```cpp
void* operator new(std::size_t);
void* operator new[](std::size_t);
void* operator new(std::size_t, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t, const std::nothrow_t&) noexcept;

void operator delete(void*) noexcept;
void operator delete[](void*) noexcept;
void operator delete(void*, std::size_t) noexcept;
void operator delete[](void*, std::size_t) noexcept;

void* operator new(std::size_t, std::align_val_t);
void* operator new[](std::size_t, std::align_val_t);
void* operator new(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept;
void* operator new[](std::size_t, std::align_val_t, const std::nothrow_t&) noexcept;

void operator delete(void*, std::align_val_t) noexcept;
void operator delete[](void*, std::align_val_t) noexcept;
void operator delete(void*, std::size_t, std::align_val_t) noexcept;
void operator delete[](void*, std::size_t, std::align_val_t) noexcept;
```

还应通过 ABI 符号测试确认编译器实际引用的 mangled symbol 全部存在。

#### 4.10.2 失败语义

标准兼容模式建议遵循 C++ 语义：

- throwing new 在分配失败时调用当前 `new_handler`；
- handler 返回后重试，handler 抛出或为空时抛出 `std::bad_alloc`；
- nothrow new 捕获分配失败并返回 null；
- `operator delete(nullptr)` 为 no-op；
- delete 系列不得向外抛异常。

如果 aethermind 希望 OOM 直接终止以减少异常路径，可以提供独立构建策略或显式 `am_malloc_or_die`，但不能让标准 ABI 在未声明的情况下偏离语义。

#### 4.10.3 Sized delete

Sized delete 是重要热路径优化：

1. 验证 size 是否落入合法 size class；
2. 直接计算 aligned size 和目标 ThreadCache bucket；
3. 必要时在 Debug/Hardened 模式与 PageMap 中记录的 class 交叉验证；
4. Release 默认路径跳过完整 pointer-to-size 查找；
5. 大对象或不可信 size 退回普通 PageMap 释放。

不能在调用方 size 错误时静默把对象放入错误 freelist。标准上 mismatched sized delete 属于调用方错误，但 Hardened 构建应尽可能检测。

### 4.11 符号、可见性与 allocator domain

#### 4.11.1 符号导出

- 核心实现默认 hidden visibility；
- 只导出标准 ABI、稳定 `am_*` API 和必要控制接口；
- 使用 linker version script 固定符号集合；
- C API 使用 `extern "C"`；
- C++ ABI 符号由专用 translation unit 提供；
- 测试故障注入符号不得进入生产动态库。

#### 4.11.2 allocator domain

进程中可能同时存在：

- libc allocator；
- ammalloc normal domain；
- ammalloc bootstrap domain；
- aethermind 专用 arena；
- GPU/pinned-memory allocator。

每个指针必须回到原 domain。推荐原则：

- 标准 `free` 只接受标准 ammalloc/BootstrapAllocator 返回的 CPU heap 指针；
- arena 指针由兼容的 arena free 或能够恢复 owner 的统一 free 处理；
- GPU/pinned memory 绝不注册为普通 PageMap Span；
- 禁止猜测未知指针属于 libc 并自动转发；
- Debug/Hardened 模式记录 domain 和 generation 以检测错误混用。

### 4.12 整数安全与失败原子性

#### 4.12.1 统一 checked arithmetic

建议提供无异常的内部 helper：

- `CheckedAdd(a, b, &out)`；
- `CheckedMul(a, b, &out)`；
- `CheckedAlignUp(size, align, &out)`；
- `BytesToPagesRoundUp(bytes, &pages)`；
- `PagesToBytes(pages, &bytes)`。

覆盖范围包括：

- `calloc/reallocarray` 乘法；
- alignment padding；
- bitmap bytes + object bytes；
- page count 左移；
- Span 起止 page id；
- hugepage over-allocation；
- TransferCache backing 总指针数；
- ObjectPool chunk layout。

不能依赖 Debug 断言承担来自用户输入的溢出检查。

#### 4.12.2 各层失败契约

| 层级 | 失败返回 | 必须保持的状态 |
|---|---|---|
| Public C API | null/错误码，并设置约定 errno | 不传播 C++ 异常，不发布半成品 |
| C++ new | handler/`bad_alloc` 或 nothrow null | 符合所选 ABI 策略 |
| ThreadCache | null | 原 FreeList 和 quota 保持合法 |
| CentralCache | 0/null | 调用方 lock ownership 符合文档；不丢失 Span/object |
| PageCache | null | 原 free lists、PageMap、owner 信息保持一致 |
| ObjectPool | 推荐返回 null/status；若内部抛出必须在边界捕获 | 新 chunk 失败不改变旧 pool |
| PageAllocator | null | 所有部分映射被正确回滚或记录为明确所有权 |
| Scavenger | best-effort 失败 | Span 重新回原 owner list，committed 状态准确 |

失败原子性要求状态更新遵循“准备—验证—发布”：

1. 在不可见的局部状态中完成计算和资源申请；
2. 取得目标锁后重新验证前提；
3. 一次性更新 owner/list/PageMap 发布点；
4. 失败则回滚局部资源，不修改已发布结构。

#### 4.12.3 无分配错误报告

核心错误路径应提供类似以下接口：

```text
RawLog(severity, literal_message, integer_fields...)
```

实现要求：

- 只使用栈上定长缓冲区；
- 手工或 `to_chars` 格式化整数和地址；
- 通过 `write`/`writev` 写入预先确定的文件描述符；
- 不使用 iostream、locale、`std::format`、spdlog 或动态字符串；
- 致命错误输出后使用 `_exit`/abort 策略，避免复杂析构再次分配。

### 4.13 非法释放与安全策略分层

默认性能模式不应为每次 free 增加昂贵的完整校验，但必须清楚区分：

| 情况 | 默认模式 | Debug/Hardened 模式 |
|---|---|---|
| `free(nullptr)` | no-op | no-op |
| 合法首地址 | 正常释放 | 正常释放并校验 domain/class |
| 同一对象 double free | 未定义行为，但尽量避免静默破坏全局结构 | 立即检测并终止 |
| Span 内部指针 | 未定义行为 | 校验 object boundary 并终止 |
| 不属于 ammalloc 的地址 | 未定义行为或明确拒绝 | 输出 domain/page 信息后终止 |
| stale generation | 不保证检测 | generation/cookie 检测 |

无论模式如何，allocator 自身的内部错误不得被归类为“用户未定义行为”。例如 PageMap 返回已复用 Span、错误 OOM 回滚或线程退出访问销毁全局状态，必须由实现消除。

### 4.14 测试矩阵

#### 4.14.1 自举与递归测试

- 在 RuntimeConfig、PageMap、CentralCache 和 Scavenger 初始化的每个阶段注入递归 malloc/free；
- 多线程同时执行第一次分配；
- 初始化线程暂停时，其他线程使用 BootstrapAllocator 并成功释放；
- 核心初始化失败后进入 `kDegraded`，基本 malloc/free 仍按策略工作；
- BootstrapAllocator region 耗尽和 mmap OOM；
- 错误报告路径自身不递归；
- 已有 bootstrap block 在 allocator ready 后仍能 realloc/free。

#### 4.14.2 C ABI 测试

- zero-size 行为；
- `SIZE_MAX` 和相邻边界；
- `calloc/reallocarray` 乘法溢出；
- realloc 原地、迁移、收缩和失败原子性；
- 所有合法/非法 alignment；
- `posix_memalign` 失败时不修改输出指针；
- `errno` 成功保持和失败设置；
- `malloc_usable_size` 与真实可写范围一致；
- `free(nullptr)` 不触发初始化；
- 跨线程 alloc/free。

#### 4.14.3 C++ ABI 测试

- 普通、数组、nothrow、sized、aligned new/delete；
- 构造函数抛出后对应 delete 路径正确；
- `new_handler` 重试、抛出和置空行为；
- over-aligned 类型；
- 编译器在不同优化级别下选择的 delete 符号；
- GCC/Clang 与 libstdc++/libc++ 组合的符号完整性。

#### 4.14.4 生命周期与集成测试

- 大量短生命周期线程退出后缓存和 metadata 回落；
- Scavenger start/stop 与 TLS 析构并发；
- 多线程 `fork()` 后父子进程继续分配；
- 链接替换和 `LD_PRELOAD` 运行真实程序；
- 静态链接启动阶段的全局构造分配；
- DSO 构造/析构中的分配；
- 进程退出阶段的 late free；
- allocator 不同 domain 混用的 Hardened 检测。

#### 4.14.5 动态工具

- ASan：越界、UAF 和错误回滚；
- UBSan：整数、位移、对齐和对象生命周期；
- TSan：状态机、PageMap、TLS 和 Scavenger；
- LeakSanitizer：线程退出和 bootstrap block；
- ABI symbol checker：导出符号与版本；
- 自定义 fault injection：mmap、munmap、madvise、线程创建和 metadata pool OOM。

### 4.15 分阶段实施与验收门禁

#### 阶段 A：显式 API 正确性

实施内容：

1. 修复 TLS RAII、OOM、对齐和锁所有权问题；
2. 建立 checked arithmetic；
3. 明确 PageMap/Span 生命周期；
4. 分离 reset、purge 和 destroy；
5. 建立无分配 RawLog；
6. 补齐故障注入和 Sanitizer。

退出条件：

- `am_malloc/am_free` 在长时间、多线程和 OOM 场景下无已知崩溃或泄漏；
- ASan、UBSan、TSan 通过；
- 所有 size class 满足已定义对齐契约；
- ThreadCache 在线程退出后可观测地完成 drain。

#### 阶段 B：自举安全核心

实施内容：

1. 实现 AllocatorState 和递归深度；
2. 实现 BootstrapAllocator；
3. 重构全局初始化依赖和统一 ready 发布；
4. 消除初始化及错误路径中的递归依赖；
5. 实现幂等 Scavenger shutdown。

退出条件：

- 初始化任意阶段递归分配均可完成或按策略失败；
- 多线程首次初始化无死锁；
- bootstrap block 可以跨 ready 状态释放；
- 核心初始化失败不会发布半初始化结构。

#### 阶段 C：完整标准 ABI

实施内容：

1. 实现 C API 语义矩阵；
2. 实现完整 C++ new/delete 符号；
3. 加入 sized free/delete；
4. 建立 version script、visibility 和 ABI 测试；
5. 输出显式库与 interposer 库两个目标。

退出条件：

- 标准 C/C++ API 测试全部通过；
- 链接期替换可稳定运行代表性程序；
- ABI 符号在支持的编译器/标准库矩阵中完整；
- OOM、errno、new-handler 和 alignment 语义与文档一致。

#### 阶段 D：动态拦截与生产灰度

实施内容：

1. 完成 `LD_PRELOAD` 启动测试；
2. 实现或明确限制 `fork()` 行为；
3. 验证动态加载器和第三方库初始化递归；
4. 建立运行时识别、统计和回滚开关；
5. 在 aethermind 非关键进程中灰度。

退出条件：

- 真实应用 preload 压力运行无 allocator 死锁、递归和泄漏；
- 可以通过构建或启动配置快速回退 libc/TCMalloc/jemalloc；
- allocator domain 和错误诊断足以定位混用问题；
- 性能收益不以显著 RSS 或尾延迟退化为代价。

### 4.16 性能护栏

正确性与 ABI 补齐可能增加入口分支、状态读取和所有权判断，因此每一阶段都应监控：

- `am_malloc/am_free` steady-state 指令数；
- TLS lookup 次数；
- PageMap 查询次数；
- sized delete 命中率；
- BootstrapAllocator 只在初始化/递归路径出现，ready 后命中应接近零；
- zero-size、aligned 和 realloc 冷路径不能污染普通小对象 I-cache；
- interposer 包装层是否被编译器内联或形成额外 PLT 跳转；
- ThreadCache 析构和 shutdown 只影响线程/进程生命周期，不进入正常热路径。

建议把公开入口拆成极小 fast wrapper 和 `AM_NOINLINE` 冷路径：

```text
malloc(size)
  -> ready + non-recursive + ordinary alignment
       -> ThreadCache fast path
  -> otherwise
       -> malloc_slow_dispatch(size)
```

最终验收不能只比较平均纳秒数，还应同时检查 p99/p999、线程扩展性、RSS、VMA 数量和故障路径延迟。

## 5. PageMap 与 Span 生命周期

PageMap 是普通 `free`、跨线程释放、Span 拆分/合并、大对象回收和 `malloc_usable_size` 的基础索引。它必须同时满足两个看似矛盾的目标：读取路径不加锁，写入和回收路径仍然保持严格的对象生命周期安全。

本节的核心结论是：**atomic leaf 的 acquire/release 只能保证指针发布前的初始化可见性，不能保证 leaf 指向的 Span 在读取后继续存活。** PageMap 的正确设计必须同时包含发布协议、写者串行化协议和 Span 延迟回收协议。

### 5.1 职责、术语与所有权边界

#### 5.1.1 PageMap 的职责

PageMap 负责将任意受管 CPU heap 地址转换为描述其所属页区间的 Span：

```text
user pointer
  -> page id
  -> PageMap radix lookup
  -> Span descriptor
  -> allocation kind / size class / owner shard / usable size
```

PageMap 应负责：

- 从 page id 查找当前负责该页的 Span；
- 为新 Span 发布连续的 `page_id -> Span*` 映射；
- 在 split、coalesce、unmap 时原子地切换逻辑所有者；
- 拒绝超出受支持虚拟地址位数的 page id；
- 为 `free`、`realloc`、`malloc_usable_size` 和诊断路径提供稳定查询。

PageMap 不应负责：

- 分配或释放用户对象；
- 决定 ThreadCache/CentralCache 策略；
- 直接拥有 Span descriptor；
- 在无锁读路径中修复不一致映射；
- 猜测未知指针是否属于 libc、GPU 或其他 allocator domain。

#### 5.1.2 四类所有权

必须区分以下四类所有权，避免用“Span 属于 PageCache”掩盖不同生命周期：

| 所有权类型 | 所有者 | 含义 |
|---|---|---|
| Descriptor ownership | PageCache shard 的 metadata arena/pool | 决定谁创建、retire 和最终销毁 Span descriptor |
| Address-range ownership | 某个 Span descriptor | 决定一段虚拟页当前由哪个 Span 描述 |
| Page backing ownership | PageAllocator/PageCache extent | 决定虚拟映射和物理 backing 的保留、purge、unmap |
| Object ownership | 用户或 Frontend/Middle-end cache | 决定小对象当前是 live、缓存中还是已回 Span bitmap |

CentralCache 从 PageCache 借用一个小对象 Span 时，不取得 descriptor ownership。它只在该 Span 的小对象分配生命周期内维护 bitmap 和 `use_count`。PageCache 仍是 descriptor 的最终所有者，但只有当所有对象都已回到 Span bitmap，CentralCache 才能把 Span 返还 PageCache。

#### 5.1.3 借用指针

`PageMap::GetSpan()` 返回的是 borrowed pointer，不转移所有权。借用必须有明确有效期：

- **当前实现隐含契约**：调用者读取 leaf 后立即访问 Span，但没有显式 read-side guard；该契约不足以支持 descriptor 立即复用。
- **第一阶段推荐契约**：已经发布的 descriptor 在 allocator 正常运行期间保持稳定地址，不单独回收；borrow 在一次 API 调用内天然安全。
- **未来 epoch 契约**：调用者进入 PageMap read-side critical section，退出前 descriptor 不得回收。

在没有 stable metadata 或 epoch guard 之前，任何 `ObjectPool::Delete(span)` 都不能被视为安全的无锁读者回收协议。

### 5.2 核心生命周期不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 说明 |
|---|---|---|
| PM-1 | `GetSpan` 不获取 PageCache、PageMap 或 metadata pool 锁 | 保持普通 free 的无锁查询路径 |
| PM-2 | 每个已映射 page leaf 在任一时刻最多指向一个逻辑 owner Span | 禁止重叠 Span 同时拥有同一页 |
| PM-3 | leaf 发布前 Span 的身份字段和本 allocation epoch 的分类字段已完整初始化 | acquire reader 才能看到完整描述 |
| PM-4 | leaf 指向的 descriptor 在所有潜在读者退出前不得结束生命周期或被复用 | 解决 UAF/ABA，而非只解决可见性 |
| PM-5 | RadixNode 在正常并发运行期间只增不减 | 读者不需要保护中间节点生命周期 |
| PM-6 | 所有 PageMap 写入由明确的 writer 协议串行化 | 分片锁本身不足以保护全局 radix tree |
| PM-7 | Span 的 owner shard 在发布前确定，并在整个 descriptor allocation epoch 内不变 | release 必须能回到唯一 shard |
| PM-8 | 一个 Span 同时最多属于一个侵入式 SpanList | 避免重复 erase、链表环和跨桶污染 |
| PM-9 | free-list bucket 索引与 `span->page_num` 一致 | split/coalesce 后必须先更新字段再入正确桶 |
| PM-10 | 小对象 Span 只有在 `use_count == 0` 且无缓存对象时才能返还 PageCache | 防止 ThreadCache/TransferCache 悬垂指针 |
| PM-11 | unmap 前先撤销对即将失效地址范围的 PageMap 映射 | 防止 free 查到已解除映射的内存 |
| PM-12 | ClearRange 不等同于 descriptor 可立即回收 | 仍可能有读者已经取得旧指针 |
| PM-13 | split/coalesce 的部分失败不改变已发布所有权 | 保持失败原子性 |
| PM-14 | Detached Scavenger Span 既不在 free list，也不可被分配或合并 | 锁外 madvise 期间保持独占 |
| PM-15 | Reset/Destroy 只能在已证明无并发读写者的静默期执行 | 测试清理不能冒充运行时安全操作 |

建议为 Span 增加 Debug-only list owner/bucket 标记，并在入链、出链和状态转换时校验 PM-8、PM-9。生产构建中保留低成本的边界和 owner 检查。

### 5.3 Span 状态机

#### 5.3.1 推荐状态

当前 `used/committed` 两个标志不足以表达 CentralCache 借用、Scavenger 摘除和 descriptor retire 等状态。建议引入概念上的显式状态机：

```cpp
enum class SpanState : uint8_t {
    kFresh = 0,
    kFreeCommitted,
    kFreePurged,
    kInUseSmall,
    kInUseLarge,
    kDetachedScavenging,
    kCoalescing,
    kRetired,
};
```

该枚举不一定必须在第一版直接作为原子字段实现，但设计、断言和测试必须按这些状态理解 Span 生命周期。

#### 5.3.2 状态定义

| 状态 | 所属容器/域 | 允许的主要字段 | 合法操作 |
|---|---|---|---|
| `kFresh` | metadata owner 私有，尚未发布 | identity 正在初始化 | 初始化、发布或失败回收 |
| `kFreeCommitted` | PageCache shard 的 page-count bucket | 无小对象 bitmap 所有权，物理页仍 committed | exact hit、split、coalesce、scavenge |
| `kFreePurged` | PageCache shard 的 page-count bucket | 虚拟地址保留，物理页已 purge | exact hit、split、coalesce、重新触页 |
| `kInUseSmall` | CentralCache bucket 借用 | size class、bitmap、capacity、use_count 有效 | object alloc/free、归还 PageCache |
| `kInUseLarge` | 用户大对象 | page range 和 usable size 有效 | free、realloc grow/shrink |
| `kDetachedScavenging` | Scavenger 私有临时链 | 不在 PageCache bucket，地址仍映射 | madvise、失败回滚、重新入桶 |
| `kCoalescing` | PageCache shard 临界区私有 | 正在组合地址范围 | 邻居摘除、映射切换、旧 descriptor retire |
| `kRetired` | metadata retire list | 不再拥有页，不得重新发布 | 等待进程结束或 epoch 安全回收 |

#### 5.3.3 主要转换

```text
kFresh
  +--> kFreeCommitted       OS refill 后先作为 free extent 发布
  +--> kInUseLarge          大对象直接分配并发布

kFreeCommitted / kFreePurged
  +--> kInUseSmall          CentralCache 取得并初始化
  +--> kInUseLarge          大页级请求取得
  +--> kCoalescing          与相邻 free Span 合并
  +--> kDetachedScavenging  后台摘除并 purge

kInUseSmall
  +--> kFreeCommitted       use_count 归零并交回 PageCache

kInUseLarge
  +--> kFreeCommitted       可缓存页数范围内释放
  +--> kRetired             direct mapping 清图/unmap 后 descriptor retire

kDetachedScavenging
  +--> kFreePurged          madvise 成功
  +--> kFreeCommitted       madvise 失败

kCoalescing
  +--> kFreeCommitted       合并结果入桶
  +--> kRetired             被吸收的旧 descriptor
```

#### 5.3.4 禁止转换

- `kInUseSmall -> kRetired`：只要 ThreadCache/TransferCache/用户仍可能持有对象指针，就不能回收 descriptor。
- `kDetachedScavenging -> kInUse*`：必须先回 shard bucket，再由正常分配路径取得。
- `kRetired -> kFresh`：在 stable metadata 方案中 descriptor 不复用；epoch 方案也必须增加 generation 后才能复用。
- `kInUseLarge -> kInUseSmall`：同一个 allocation epoch 内不能直接重新解释字段。
- `kFreePurged -> unmap` 同时保留 PageMap leaf：解除映射前必须先撤销或切换映射。

### 5.4 Span 字段分类与访问协议

#### 5.4.1 身份字段

建议将以下字段视为 descriptor allocation epoch 内的身份：

- `start_page_idx`；
- `page_num`；
- `owner_shard_id`；
- `generation`；
- allocator domain/region id。

其中 `start_page_idx/page_num` 在 split/coalesce 时会变化，因此更准确的规则是：**每次地址范围变化都创建新的映射版本，旧身份在发布切换后进入 retired 状态。** 第一阶段为减少 descriptor 数量，可以允许 shard 锁内修改 survivor Span，但必须保证：

1. 读者不会把同一个 descriptor 的旧地址范围与新字段混合使用；
2. 旧 leaf 在 descriptor 字段改变前已经受 writer 协议保护地切换；
3. descriptor 不被立即复用；
4. PageMap 查询结果经过必要的一致性验证。

从可证明性角度，推荐 split/coalesce 尽可能使用“新 descriptor 发布、旧 descriptor retire”，把 identity 变更转化为版本替换。

#### 5.4.2 小对象分类字段

以下字段只在 `kInUseSmall` 有效：

- `size_class_idx`；
- `aligned_obj_size`；
- `capacity`；
- `obj_offset`；
- bitmap 地址/长度；
- `use_count`；
- `scan_cursor`。

发布规则：

- CentralCache 在 Span 尚未进入其 `span_list` 前完成布局初始化；
- PageMap leaf 若已指向该 Span，则普通 free 只有在对象已经交给用户后才会读取分类字段；
- 对象发布给 ThreadCache/用户之前，分类字段必须对该线程可见；
- `aligned_obj_size` 在整个 `kInUseSmall` epoch 内不可改变；
- `capacity/obj_offset` 初始化后不可改变；
- bitmap、`use_count` 和 `scan_cursor` 由对应 CentralCache bucket lock 保护。

#### 5.4.3 回收字段

以下字段描述物理 backing 与回收策略：

- committed/purged 状态；
- `last_used_time_ms`；
- decay generation；
- purge failure/retry 信息。

这些字段由 PageCache shard lock 或 Detached Scavenger 独占权保护。Scavenger 锁外执行 madvise 时，只有持有 `kDetachedScavenging` 状态的线程可以修改 committed 状态。

#### 5.4.4 建议的字段访问表

| 字段组 | 正常读者 | 写者保护 | 是否允许无锁读取 |
|---|---|---|---|
| owner/domain/generation | `free`、PageCache | 发布前初始化；之后稳定 | 是，前提是 descriptor 生命周期稳定 |
| start/page count | PageMap 诊断、PageCache | shard + PageMap writer 事务 | 仅在版本稳定协议下 |
| size class/object size | `free`、CentralCache | CentralCache 发布前初始化；epoch 内只读 | 是，前提是小对象 epoch 未结束 |
| capacity/offset | CentralCache、诊断 | 初始化后不可变 | 是 |
| bitmap/use_count/cursor | CentralCache | bucket mutex | 否，除非仅作近似统计 |
| state/list links | PageCache/CentralCache/Scavenger | 对应状态 owner 的锁 | 否 |
| committed/last-used | PageCache/Scavenger | shard lock 或 detached 独占 | 统计可近似无锁读，决策不可 |

### 5.5 PageMap 四层基数树

#### 5.5.1 地址分解

设 OS page shift 为 `PAGE_SHIFT=12`，page id 位数为：

```text
PAGE_ID_BITS = VA_BITS - PAGE_SHIFT
```

当前四层结构包含一个胖 root 和三个 9-bit RadixNode 层：

```text
page_id
  [root bits][9 bits][9 bits][9 bits]
      i0        i1      i2      i3
```

- 48-bit VA：page id 为 36 bit，root 使用 9 bit；
- 57-bit VA：page id 为 45 bit，root 使用 18 bit；
- 每个普通 RadixNode 包含 512 个 atomic child；
- leaf 保存 `Span*`，中间层保存 `RadixNode*`。

必须通过编译期断言验证：

- `RADIX_ROOT_BITS + 3 * RADIX_NODE_BITS == PAGE_ID_BITS`；
- root/node 数组大小不会发生位移溢出；
- `page_id` 高于支持范围时 Get/Set/Clear 均明确拒绝；
- 57-bit 构建的胖 root 内存开销经过预算评估。

#### 5.5.2 中间节点生命周期

RadixRoot 使用静态存储。普通 RadixNode 由独立 metadata pool 分配，并遵循：

- 构造时将所有 child relaxed 初始化为空；
- 节点完全初始化后通过 release store/CAS 发布；
- reader 使用 acquire load 获取中间节点；
- 正常运行期间不删除单个节点；
- 仅在全局静默、PageMap 不再有读者时统一释放节点池。

中间节点“只增不减”会产生 metadata 增长，因此必须统计节点数量、覆盖地址范围和每 GiB managed memory 的 radix metadata 成本，但不能为了减少这点开销破坏无锁读者安全。

#### 5.5.3 Leaf 语义

leaf 指针表示“该 page 当前逻辑上属于哪个 Span descriptor”，而不是单纯表示物理页是否 committed：

- `kFreePurged` Span 仍保留 leaf，便于相邻合并和后续复用；
- direct mapping unmap 后 leaf 必须为空；
- split 后 allocated 和 remainder 页分别指向对应 descriptor；
- coalesce 后整个合并区间指向 survivor/new descriptor；
- detached Scavenger Span 仍可以保留 leaf，但其 state 阻止分配和合并。

### 5.6 无锁读取契约

#### 5.6.1 Acquire 链

典型读取路径为：

```text
acquire root
  -> acquire level-1 node
  -> acquire level-2 node
  -> acquire level-3 node
  -> acquire Span leaf
```

每个 acquire 与写者对相应 child 的 release 发布配对，保证 reader 看到节点构造和 Span 发布前写入的初始化数据。

#### 5.6.2 可见性不等于生命周期

以下代码即使所有 atomic 内存序都正确，仍可能发生 UAF：

```text
Reader                          Writer
------                          ------
span = leaf.load(acquire)
                                leaf.store(nullptr, release)
                                pool.Delete(span)
read span->aligned_obj_size
```

acquire 只保证 reader 看到 writer 在最初发布 Span 前的写入；它不能阻止另一个 writer 清除 leaf 后结束对象生命周期，也不能阻止对象池在相同地址构造另一个 Span，从而形成 ABA。

因此无锁读必须配合以下至少一种机制：

- descriptor 正常运行期间不回收、不复用；
- epoch/read-side critical section；
- hazard pointer；
- 其他具有严格证明的延迟回收协议。

#### 5.6.3 Lookup 结果验证

在 Hardened 模式中，`GetSpan(ptr)` 后建议验证：

- descriptor state 是允许 free/usable-size 的状态；
- `ptr_page` 位于 `[start_page_idx, start_page_idx + page_num)`；
- domain/generation 合法；
- 小对象指针位于 data area 且满足 object boundary；
- 大对象普通 free 只接受 allocation base。

默认模式可以减少部分检查，但 descriptor 生命周期和地址范围一致性不能省略。

### 5.7 PageMap 多写者协议

#### 5.7.1 当前单写者边界

当前生产默认只使用 shard 0，因此 shard mutex 在事实上串行化 PageMap 写入。但这个性质来自路由策略，而不是 PageMap 自身。只要多个 PageCache shard 可以并发调用 `SetSpan/ClearRange`，分片锁便不再构成全局 writer lock。

#### 5.7.2 丢失节点安装风险

两个 writer 对同一个空 child 执行以下序列时可能丢失更新：

```text
Writer A                        Writer B
load child == null              load child == null
allocate node A                 allocate node B
store node A                    store node B
publish leaves under node A     publish leaves under node B
```

最终 root 只指向 node B，node A 及其 leaves 不可达。这会造成映射缺失和 metadata 泄漏。

#### 5.7.3 候选方案

| 方案 | 正确性复杂度 | 写入并发 | 读路径影响 | 建议 |
|---|---:|---:|---:|---|
| 全局 PageMap writer mutex | 低 | 中低 | 无 | 第一阶段推荐 |
| root/层级分段 writer lock | 中 | 中高 | 无 | 写入成为瓶颈后评估 |
| child CAS 安装 + loser 回收/保留 | 中高 | 高 | 无 | 可作为后续优化 |
| 完全 lock-free writer | 很高 | 高 | 可能增加元数据协议 | 暂不采用 |

#### 5.7.4 推荐锁顺序

第一阶段建议使用独立 PageMap writer mutex，并固定顺序：

```text
CentralCache bucket lock
  - 禁止持有时进入 PageCache

PageCache shard lock（跨 shard 时按 shard id 升序）
  -> PageMap writer lock
     -> RadixNode metadata pool lock
```

说明：

- 正常 PageCache 操作持有一个 owner shard lock，再取得 PageMap writer lock；
- PageMap writer lock 不反向请求 shard lock；
- Reset 如需锁定多个 shard，先按 id 升序取得所有 shard，再取得 PageMap writer lock；
- OS mmap/munmap/madvise 尽量不在这些锁内执行；
- CentralCache 释放空 Span 时必须先释放 bucket lock，再进入 PageCache。

如果未来采用 CAS 安装，中间节点 loser 也不能立即回 pool，除非证明没有读者看到它；最简单的做法是把 loser node 保留到 pool/region 最终销毁。

### 5.8 Span descriptor 回收方案比较

| 方案 | 热路径开销 | 回收及时性 | metadata 开销 | 实现/证明复杂度 |
|---|---:|---:|---:|---:|
| 已发布 descriptor 永不单独复用 | 最低 | 进程/region 结束 | 中高 | 最低 |
| Region-lifetime metadata arena | 最低 | region 释放时 | 中 | 低 |
| Epoch-based reclamation | 每次 API 少量 reader 标记 | 延迟但可持续回收 | 低中 | 中高 |
| Hazard pointer | 每次 lookup 发布 hazard | 较及时 | 中 | 高 |
| Span 原子引用计数 | 每次 lookup/free 共享原子写 | 及时 | 低 | 中，但性能风险高 |

推荐路线：

1. 先实现 region-lifetime/stable descriptor；
2. 统计 `metadata_bytes/managed_bytes`、retired descriptor 数和峰值；
3. 只有 metadata 放大超过目标阈值，才设计 epoch；
4. 不把引用计数放入普通 free 热路径。

### 5.9 推荐的稳定元数据方案

#### 5.9.1 Metadata arena

每个 PageCache shard/region 拥有独立 metadata arena：

- arena 使用 PageAllocator raw mapping 获取 chunk；
- Span descriptor 地址在 arena 生命周期内稳定；
- 新 descriptor 从 bump area 或“从未发布过”的 free slot 取得；
- 一旦 descriptor 发布到 PageMap，retire 后不返回普通可复用 free list；
- region 最终销毁且全局无读者时统一释放整个 arena。

这与普通 ObjectPool 的关键差异是：`Delete()` 不代表已发布 Span slot 可以立刻被 `New()` 重用。

#### 5.9.2 Retire list

被 split/coalesce 吸收或 direct-unmap 的 descriptor 进入 owner shard 的 retire list：

```text
active/free Span
  -> PageMap 不再引用它
  -> state = Retired
  -> append retire list
  -> process/region teardown 才释放 storage
```

retired descriptor 应保留：

- generation；
- 原地址范围或诊断摘要；
- retire reason；
- 可选 retire epoch；
- Hardened 模式 poison state。

#### 5.9.3 Generation

即使第一阶段不复用 descriptor，也建议引入单调 generation 用于：

- 检测 stale Span 引用；
- 诊断同一地址范围的 split/coalesce 历史；
- 为未来 epoch 复用建立 ABA 防护；
- 在测试中确认 leaf 切换到预期版本。

generation 不应仅使用容易短期回绕的 8/16-bit 字段。若受 64B Span 尺寸限制，可以使用 32-bit generation 并明确回绕策略，或把冷诊断字段移入 side metadata。

#### 5.9.4 内存预算

必须持续统计：

- live Span descriptor 数；
- retired descriptor 数；
- metadata arena mapped/resident bytes；
- RadixNode 数；
- 每 GiB active/mapped heap 对应的 metadata bytes；
- split/coalesce 产生 descriptor 的速率。

如果稳定 descriptor 产生过高开销，优先减少不必要的 descriptor 创建或按 region 批量管理，而不是直接牺牲生命周期安全。

### 5.10 Epoch 延迟回收备选设计

Epoch 只作为第二阶段候选，本节用于规定未来评审边界。

#### 5.10.1 Reader 协议

每个可能调用 PageMap 的线程维护 read-side 状态：

```text
EnterPageMapRead()
  -> publish active + observed global epoch
  -> lookup and consume Span fields
ExitPageMapRead()
  -> publish inactive
```

要求：

- enter/exit 不分配、不加全局锁；
- ThreadCache TLS 退出会从 epoch registry 安全注销；
- 嵌套查询使用深度计数，不能过早退出；
- free、usable-size、realloc 和 PageCache 无锁邻居查询都必须纳入协议；
- reader 不能在 read-side section 内阻塞或执行长系统调用。

#### 5.10.2 Writer 协议

writer 从 PageMap 撤销旧 descriptor 后：

1. 记录当前 global epoch；
2. 将 descriptor 放入 per-shard retire queue；
3. 周期性推进 epoch；
4. 确认所有在 retire epoch 前进入的 reader 已退出；
5. 才允许 destructor 和 slot 复用。

#### 5.10.3 生命周期交互

- Scavenger 不需要 retire descriptor，但其 PageMap/Span 读取仍必须遵循状态锁。
- Thread 退出必须标记 epoch slot inactive，防止永久阻塞回收。
- `fork()` child 必须重建只包含当前线程的 epoch registry。
- shutdown 必须等待所有 reader inactive 后清空 retire queue。
- 信号处理器若可进入 allocator，必须单独解决嵌套 epoch 和异步安全问题。

#### 5.10.4 启用门禁

引入 epoch 前必须证明：

- stable metadata 已成为显著内存问题；
- reader enter/exit 对 fast-path 基准的影响可接受；
- TSan 和模型化测试覆盖 unregister、epoch rollover 和 stalled reader；
- 回收收益大于额外 TLS、registry 和 retire queue 开销。

### 5.11 PageMap 更新事务

所有映射变更统一采用以下逻辑模型：

```text
Prepare
  -> 在共享结构外申请 descriptor/node/OS mapping
Lock
  -> 取得 owner shard + PageMap writer lock
Validate
  -> 验证旧 leaf、状态、owner、邻居和 bucket membership
Publish
  -> 更新 leaf 和共享状态
Retire
  -> 旧 descriptor 进入延迟回收
Unlock
  -> 释放 writer/shard lock
Cleanup
  -> 锁外处理可安全释放的局部资源
```

#### 5.11.1 `SetSpan`

前置条件：

- Span 非空；
- `page_num > 0`；
- `start + page_num` checked arithmetic 成功；
- owner shard 已确定；
- descriptor 处于可发布状态；
- writer 协议已持有。

发布顺序：

1. 预创建所有可能缺失的 RadixNode；
2. 初始化 descriptor 的身份和分类字段；
3. 通过 release CAS/store 安装中间节点；
4. 对连续 leaf 执行 release store；
5. 更新 Span state/list membership；
6. 对外返回或发布对象指针。

如果中间节点分配失败，不能只发布区间的一部分。应在写 leaf 前完成节点准备，或记录已经发布范围并具备严格回滚。推荐“先准备全部路径，再发布 leaf”。

#### 5.11.2 `ClearRange`

前置条件与行为：

- 使用 checked end 计算，拒绝 page count wrap；
- 只清 leaf，不删除中间节点；
- writer lock 下验证 leaf 指向预期 descriptor，避免清掉后来重用的范围；
- 支持 `ClearRangeExpected(start, count, expected_span, generation)` 形式；
- release store null 只撤销映射，不授权 descriptor 立即复用。

#### 5.11.3 批量发布一致性

连续多个 leaf 的 atomic store 无法让读者看到“全区间瞬时原子切换”。在发布循环期间，读者可能看到新旧映射混合。设计必须保证每一个中间状态都安全：

- 新旧 descriptor 在整个切换期间都保持存活；
- 每个 leaf 始终指向一个能合法描述该 page 的 descriptor；
- 不能先修改旧 descriptor 的范围，使尚未切换的 leaf 指向不再覆盖该页的 descriptor；
- 必要时使用新 descriptor 表示最终范围，待所有 leaf 切换后再 retire 旧 descriptor。

这也是推荐“版本化 descriptor”而非原地修改 identity 的主要原因。

### 5.12 Span 分配与拆分事务

#### 5.12.1 Exact bucket hit

```text
lock owner shard
  -> 从 bucket[page_num] 摘除 Span
  -> 验证 state 为 FreeCommitted/FreePurged
  -> 验证 page_num 和 bucket 一致
  -> 转为 InUseLarge 或交给 CentralCache 初始化
  -> 保持 PageMap leaf 指向同一稳定 descriptor
unlock
```

如果 free Span 已 purged，重新分配时不需要先主动 commit；第一次写入会由内核重新提供物理页。state/统计应反映该转换，避免把 purged bytes 继续计为可回收 RSS。

#### 5.12.2 从大 Span 拆分

推荐使用两个最终 descriptor：allocated Span 与 remainder Span。事务为：

1. 在共享结构外准备 allocated descriptor；
2. 取得 owner shard 和 PageMap writer lock；
3. 验证 big Span 仍位于预期 bucket、状态 free 且大小足够；
4. 从原 bucket 摘除 big Span；
5. 计算 allocated/remainder 范围并检查所有算术；
6. 初始化 allocated descriptor；
7. 将 big Span 作为 remainder survivor，或为 remainder 创建新 descriptor；
8. 在整个 leaf 切换期间保持原 big descriptor 有效且覆盖旧区间；
9. 先发布 allocated 范围的新 leaf；
10. 更新 remainder descriptor identity 并发布 remainder leaf；
11. remainder 入对应 bucket；
12. allocated 转入使用状态；
13. 不再拥有页的旧 descriptor 进入 retire list。

更容易证明的方案是为 allocated 和 remainder 都创建新 descriptor，原 big Span 在全部 leaf 切换后 retire。这样 metadata 稍多，但不会出现原地缩小 big Span 时旧 leaf 瞬间指向“不覆盖该页”的 descriptor。

#### 5.12.3 Metadata OOM

- descriptor 分配应在摘除 big Span 前完成；
- 分配失败直接返回 null，原 big Span 和 PageMap 不变；
- 不能在 split 一半后因第二个 descriptor OOM 留下部分映射；
- 如果采用 survivor 方案，只需一个新 descriptor，但仍必须满足批量发布安全性。

#### 5.12.4 OS refill

OS refill 应尽量锁外进行：

1. shard 锁内确认没有可用 Span；
2. 解锁并向 PageAllocator 申请 region；
3. 准备 descriptor 和 RadixNode；
4. 重新取得 shard/writer lock；
5. 再次检查 bucket，若其他线程已补货，可将新 region 作为额外 free extent 发布，而不是丢弃；
6. 完整发布 PageMap 后入桶；
7. 重试 exact/split。

### 5.13 Span 释放与合并事务

#### 5.13.1 释放前验证

PageCache 接收 Span 前必须验证：

- descriptor 属于当前 allocator domain；
- owner shard id 合法；
- state 是 `kInUseSmall` 或 `kInUseLarge` 的合法返还状态；
- 小对象 Span 的 `use_count == 0`；
- Span 不在任何链表；
- PageMap 对其地址范围仍指向该 descriptor/generation；
- page range 算术没有 wrap。

#### 5.13.2 邻居资格

左右邻居只有同时满足以下条件才能合并：

- PageMap 查到非空 descriptor；
- state 是 `kFreeCommitted` 或 `kFreePurged`；
- owner shard 与当前 Span 相同；
- 地址严格相邻而不是仅 leaf 恰好指向同一对象；
- 邻居确实位于对应 page-count bucket；
- 合并大小不超过当前缓存/region 策略限制；
- committed/purged 状态组合有定义。

owner-shard-local 是硬约束。不同 shard 的相邻 extent 不应在释放热路径中跨锁合并；正确的 region ownership 应从地址分配阶段避免这种边界碎片。

#### 5.13.3 合并发布顺序

推荐事务：

1. 取得 owner shard 和 PageMap writer lock；
2. 将释放 Span 标记为 `kCoalescing`，但尚不入桶；
3. 查询并验证左邻居；
4. 从左邻居 bucket 摘除并标记 `kCoalescing`；
5. 查询并验证右邻居；
6. 从右邻居 bucket 摘除并标记 `kCoalescing`；
7. 准备一个覆盖最终范围的新/survivor descriptor；
8. 将最终范围的每个 leaf 切换到结果 descriptor；
9. 只有在 leaf 完成切换后，才把被吸收 descriptor 标记 retired；
10. 结果 Span 清除小对象字段，设置正确 committed/purged 策略；
11. 结果 Span 进入对应 bucket 并转为 free state。

#### 5.13.4 Committed 状态合并

若一个邻居 purged、另一个 committed，合并结果不能用单个 bool 精确描述逐页物理 backing。可选策略：

- 第一阶段把合并结果视为 committed，统计上保守，不影响正确性；
- 维护区间级 purge bitmap，复杂度较高；
- 合并前/后将整个结果区间统一 madvise，系统调用应在安全 detached 状态下锁外进行。

建议第一阶段使用保守状态并把精细物理页状态留给 hugepage/extent backend 重构。

#### 5.13.5 禁止立即 `ObjectPool::Delete`

被左/右合并吸收的 descriptor 即使 leaf 已全部切换，也可能仍被早先的 reader 持有。因此它们必须进入 retire list，不能立即回到 ObjectPool free list。

### 5.14 小对象 Span 生命周期

#### 5.14.1 从 PageCache 到 CentralCache

```text
PageCache Free Span
  -> CentralCache 请求 page_num
  -> PageCache 返回稳定 descriptor
  -> Span::Init(size_class)
  -> 初始化 bitmap/capacity/offset/use_count
  -> state = InUseSmall
  -> 插入对应 CentralCache bucket SpanList
```

初始化应在对其他 CentralCache 线程可见前完成。若 PageMap leaf 早已指向该 descriptor，合法用户 free 仍不会发生，因为尚未有对象被发布；但是错误指针查询可能看到过渡状态，因此 Hardened 检查需要明确识别 `kFresh/kCoalescing`。

#### 5.14.2 `use_count` 的准确语义

当前 bitmap 模型中，bit cleared 表示对象已从 Span bitmap 取出。对象可能位于：

- 用户手中；
- ThreadCache FreeList；
- TransferCache；
- CentralCache 批量提取的临时数组。

因此 `use_count` 更准确的名称是 `objects_out_of_span_bitmap`，而不是仅指“用户当前 live allocation”。对象从用户释放到 ThreadCache 或 TransferCache 时不递减；只有最终回到 Span bitmap 时才递减。

这一语义保证：只要任何 Frontend/Middle-end 仍持有该 Span 的对象指针，`use_count` 就不会归零，Span 也不能返还 PageCache。

#### 5.14.3 TransferCache 不变量

- TransferCache 中每个指针对应 Span bitmap 中一个 cleared bit；
- 从 TransferCache 取出交给 ThreadCache 不改变 `use_count`；
- ThreadCache 归还到 TransferCache 不改变 `use_count`；
- 只有 TransferCache overflow 或 Reset 把对象真正回 bitmap 时递减；
- CentralCache Reset 必须先恢复所有 TransferCache object 的 bitmap 所有权，再释放空 Span；
- TransferCache backing 的 clear 与 destroy 必须分离，避免测试绕过真实路径。

#### 5.14.4 Span 归还 PageCache

在 CentralCache bucket lock 下处理对象回 bitmap。当 `use_count` 变为零：

1. 从 CentralCache SpanList 摘除；
2. 记录 descriptor 和必要状态；
3. 释放 bucket lock；
4. 调用 PageCache 返还 owner shard；
5. PageCache 清除小对象分类字段并进入 coalesce/free 流程。

禁止持 CentralCache bucket lock 调用 PageCache，以免与 refill 路径形成锁顺序反转。

#### 5.14.5 Double free 与跨线程 free

- 跨线程 free 是合法场景，对象进入释放线程的 Frontend 或 owner-aware remote queue；
- 两个线程并发 free 同一对象属于用户错误，但 Hardened 模式应在对象最终回 bitmap 或更早阶段检测；
- 仅在 bitmap 层检测可能被 ThreadCache/TransferCache 延迟，应考虑 sampled state/cookie；
- 无论用户错误如何，descriptor 生命周期协议不能导致 PageMap UAF。

### 5.15 大对象 Span 生命周期

#### 5.15.1 分配

大于 Frontend 上限的请求按 checked round-up 转为 page count：

1. 验证 `size + page_size - 1` 不溢出；
2. 从 PageCache small-run bucket、LargeExtentSet 或 direct mapping 获取区间；
3. 初始化 `kInUseLarge` descriptor；
4. 记录 requested/usable size、alignment 和 owner domain；
5. 完整发布 PageMap；
6. 返回 allocation base。

不建议继续用 `aligned_obj_size == 0` 作为大对象的唯一身份标志。应使用显式 SpanState/allocation kind，防止字段清理或过渡状态被误判为大对象。

#### 5.15.2 释放

普通大对象 free 只接受 allocation base。事务：

- lookup stable descriptor；
- 验证 state/domain/base；
- 进入 owner shard；
- cacheable extent 转为 free/coalesce；
- direct mapping 先通过 expected-span ClearRange 撤销 leaf；
- 经过 read-side grace period 后 descriptor 才可回收；
- OS unmap 可以在映射撤销和状态隔离后锁外执行，但必须防止地址被错误重用。

#### 5.15.3 Realloc

大对象原地扩展需要：

- 检查右邻居是否 free、同 owner 且足够大；
- 在 shard/writer 事务中摘除或拆分右邻居；
- 发布扩展范围 leaf；
- 最后更新用户可见 usable size；
- 失败时原 allocation 完全不变。

原地收缩需要：

- 生成 tail remainder descriptor；
- 将 tail leaf 从原 Span 切换到 remainder；
- 原 descriptor 的 usable range 更新必须与 leaf 切换协议一致；
- remainder 进入 PageCache 或直接 madvise/unmap。

第一阶段优先采用 allocate-copy-free，待事务模型稳定后再实现原地 grow/shrink。

### 5.16 Scavenger 的 Detached 生命周期

Scavenger 使用“摘除—回收物理页—归还”三阶段协议：

```text
shard lock
  -> 从 free bucket 摘除候选
  -> state = DetachedScavenging
unlock
  -> madvise(MADV_DONTNEED)
lock same owner shard
  -> success: FreePurged
  -> failure: FreeCommitted
  -> 更新时间并回原 page-count bucket
unlock
```

关键不变量：

- Detached Span 不在任何 PageCache bucket；
- state 阻止其他线程把它视为可合并邻居；
- PageMap leaf 可以继续指向 descriptor，因为虚拟地址仍保留；
- Scavenger 私有临时链不能覆盖 descriptor 的 PageCache/CentralCache list link 而破坏状态；建议提供独立临时链接或严格保证完全摘除；
- madvise 失败必须恢复 `kFreeCommitted`；
- 归还必须使用原 owner shard，而不是默认 shard 0；
- shutdown 必须等待所有 Detached Span 回归或进入受控终止状态。

多 shard 实现应逐 shard 扫描，每次只持一把 shard lock。不能依赖 `GetSpanList/GetMutex` 的 shard-0 legacy 接口。

### 5.17 Reset、Purge 与最终销毁

当前一个 `Reset()` 容易混淆不同语义，建议拆分：

| 操作 | 并发条件 | 行为 | 是否可继续使用 runtime |
|---|---|---|---|
| `ClearCachesForTest` | 测试线程独占，所有 worker 停止 | drain Frontend/Middle-end，保留 backing | 是 |
| `PurgeFreePages` | 正常运行，可与受控分配并发 | madvise free extent，不清 PageMap metadata | 是 |
| `ReleaseUnusedExtents` | 明确 writer 协议 | unmap 完全 free 的大 extent，retire descriptor | 是 |
| `DestroyRuntimeForTest` | 全局静默，无任何读写者 | 清 PageMap、释放 RadixNode/metadata arena | 否 |
| 进程退出 | OS 接管 | 停止后台线程，可让核心 metadata 常驻 | 不适用 |

#### 5.17.1 静默期证明

最终销毁前必须满足：

- allocator state 已阻止新正常入口；
- Scavenger 已停止并 join；
- 所有 ThreadCache/CPU cache 已 drain 或线程已终止；
- 所有 PageMap reader 已退出；
- 所有 shard writer 已退出；
- CentralCache 不再持有 Span；
- retire queue 已达到安全回收条件。

仅仅依次取得每个 shard mutex 不足以证明无锁 PageMap reader 已退出。

#### 5.17.2 Root 重用

静态 root 在测试 Reset 后若要重新使用：

- 必须处于全局静默；
- 先让 root 对新 reader 不可见；
- 等待旧 reader 退出；
- 才能清空 root child 和释放 RadixNode pool；
- 下一次初始化完整构造后再 release 发布 root。

### 5.18 非法 free、边界校验与 generation

#### 5.18.1 小对象边界

Hardened 模式下，从 Span 恢复对象索引时验证：

```text
data_base <= ptr < data_base + capacity * object_size
(ptr - data_base) % object_size == 0
object_index < capacity
bitmap bit 当前为 allocated/out-of-bitmap
```

检查和减 `use_count` 必须在同一个 CentralCache bucket lock 临界区内完成。

#### 5.18.2 大对象边界

- 普通 free 要求 `ptr == Span::GetPageBaseAddr()` 或记录的 aligned user base；
- aligned large allocation 需要从 user pointer 恢复原 allocation base；
- interior pointer 不得因为 PageMap 能找到 Span 就被当作合法 free；
- `malloc_usable_size` 同样只接受合法 allocation pointer。

#### 5.18.3 Generation 校验

PageMap leaf 若只保存裸 `Span*`，generation 不能直接与 leaf 原子一致读取。可选方案：

- descriptor 不复用时，generation 主要用于诊断；
- leaf 保存 tagged pointer/index + generation；
- PageMap side table 保存稳定 descriptor id；
- epoch 安全回收后仍在 descriptor 内检查 generation，但需防 ABA。

第一阶段不复用 descriptor，可以避免把 tagged pointer 引入热路径；未来复用前必须重新评审 ABA 方案。

### 5.19 锁顺序与死锁规约

#### 5.19.1 锁域

- Frontend：线程/CPU 私有，不持共享锁；
- TransferCache：每 bucket SpinLock；
- Central SpanList：每 bucket mutex；
- PageCache：每 shard mutex；
- PageMap：独立 writer mutex，reader 无锁；
- metadata arena：内部 pool lock；
- Scavenger wait state：独立 mutex，不与 PageCache 锁嵌套等待。

#### 5.19.2 强制顺序

```text
CentralCache bucket lock
  -> 不得进入 PageCache；需要时先 unlock

PageCache shard lock(s), shard id ascending
  -> PageMap writer lock
     -> PageMap RadixNode pool lock

PageCache shard lock
  -> owner Span metadata arena lock（若仍需要独立锁）
```

禁止：

- 持 PageMap writer lock 再取得未持有的 shard lock；
- 持 CentralCache bucket lock 调用 PageCache；
- 持 Scavenger wait mutex 等待 PageCache 操作；
- 持 shard lock 执行可阻塞的 mmap/munmap/madvise，除非当前阶段为保证正确性暂时接受并有明确基准；
- 未按 shard id 排序获取多 shard 锁。

#### 5.19.3 锁断言

建议在 Debug 构建增加轻量 lock-rank 检查，记录当前线程持有的最高锁等级。它只用于测试和开发，不进入 Release 热路径。

### 5.20 故障注入与测试矩阵

#### 5.20.1 PageMap 基础测试

- 48-bit 和 57-bit 各层索引边界；
- root first/last entry；
- 每个 RadixNode 边界跨越；
- unknown address miss；
- SetSpan/ClearRange 全区间；
- page id/end range overflow；
- purged Span 仍可 lookup；
- unmap 后 leaf 为空。

#### 5.20.2 Split/coalesce 测试

- exact hit 不改变无关 leaf；
- split 的 allocated/remainder 每页映射正确；
- 左合并、右合并、左右同时合并；
- 达到最大合并大小时停止；
- 跨 owner shard 不合并；
- committed + purged 状态组合；
- 被吸收 descriptor 进入 retire list而不是立即复用；
- 任一 metadata 分配失败时原 Span/list/PageMap 不变。

#### 5.20.3 并发测试

- 多 reader 与同范围 leaf 切换；
- reader 在 load leaf 后暂停，writer clear/retire，再恢复 reader；
- 多 writer 并发安装相同中间节点；
- 不同 shard 并发 SetSpan/ClearRange；
- CentralCache 归还空 Span与 PageCache 分配并发；
- Scavenger detached 与相邻 Span release 并发；
- Reset/Destroy 只在静默期成功；
- TLS 退出与 epoch/retire queue 并发。

#### 5.20.4 故障注入

- RadixNode pool OOM；
- Span descriptor arena OOM；
- OS refill OOM；
- split 准备阶段失败；
- madvise 失败；
- munmap 失败；
- writer 在批量 leaf 发布中途被调度暂停；
- epoch reader 长时间停滞；
- generation 接近回绕。

#### 5.20.5 工具与方法

- ASan 检查 descriptor 和用户内存 UAF；
- UBSan 检查 page id 位移、范围计算和对齐；
- TSan 检查状态字段、root reset 和 Scavenger；
- 模型化测试枚举 split/coalesce/reader interleaving；
- 固定种子随机 Span 操作序列并在每步验证 PageMap 全区间；
- 长时间 cross-thread free 与 PageCache churn；
- Debug invariant walker 验证“bucket/list/PageMap/owner”四方一致。

### 5.21 可观测性与诊断

建议增加以下低成本或慢路径统计：

- `pagemap_lookup_hit/miss`；
- 每层 RadixNode 数量；
- PageMap writer lock wait/hold time；
- `set_span_pages/clear_range_pages`；
- live/retired Span descriptor 数；
- split/coalesce 次数和页数；
- owner-shard mismatch 拒绝次数；
- detached Scavenger Span 数和停留时间；
- metadata mapped/resident bytes；
- epoch retire queue 长度和最大 grace period；
- invariant failure 分类。

统计读取接口应允许近似快照，不能为了全局一致统计阻塞所有 PageMap reader。Debug 工具可以在显式静默期输出完整地址范围和 descriptor 状态。

### 5.22 性能与内存护栏

#### 5.22.1 Reader 基准

必须持续跟踪：

- `PageMap::GetSpan` hit/miss/mixed 延迟；
- 普通 `am_free` 中 PageMap 占比；
- sized free 跳过 PageMap 后的收益；
- 48-bit 与 57-bit root 对 cache/TLB 的影响；
- stable metadata、epoch reader enter/exit 的额外指令；
- 不同访问分布下的 L1/LLC/TLB miss。

#### 5.22.2 Writer 基准

- 单页与多页 SetSpan；
- ClearRange；
- exact bucket、split、coalesce；
- 1/2/4/8/16 shard 并发建图；
- writer mutex contention；
- CAS 节点安装与全局 writer lock 的比较；
- OS 系统调用移出锁前后的 p99。

#### 5.22.3 Metadata 护栏

建议初始目标以实测确定，但至少报告：

```text
metadata amplification = metadata resident bytes / active user bytes
retired ratio          = retired descriptors / all descriptors
radix density          = mapped leaves / radix leaf capacity
```

任何回收优化必须同时给出 fast-path 延迟、metadata RSS、UAF 证明和复杂度变化，不能只报告减少了多少 descriptor。

### 5.23 分阶段实施与验收

#### 阶段 A：建立稳定生命周期基线

实施内容：

1. 定义显式 SpanState 和字段有效期；
2. 已发布 descriptor 改为 stable/retire，不立即回 ObjectPool；
3. 引入 generation 和 Debug list/bucket invariant；
4. `ClearRangeExpected` 验证 expected descriptor；
5. 分离 Reset/Purge/Destroy；
6. 补充 reader-pause/retire 回归测试。

退出条件：

- PageMap reader 不可能访问已析构或已复用 descriptor；
- RadixNode 正常运行期间永不回收；
- ASan/TSan 和并发生命周期测试通过；
- metadata 开销已可观测。

风险类型：正确性、并发、内存。

#### 阶段 B：事务化 split/coalesce

实施内容：

1. 将 split/coalesce 重写为 prepare/validate/publish/retire；
2. 避免原地 identity 修改造成混合 leaf；
3. OS refill 移出 shard 锁；
4. 建立失败注入和全区间 invariant walker；
5. 明确 committed/purged 合并策略。

退出条件：

- 任意中途失败保持原 PageMap 和 bucket 状态；
- 每个 leaf 在发布过渡期间始终指向合法、存活 descriptor；
- split/coalesce 聚焦基准无不可接受退化。

风险类型：正确性、并发、性能。

#### 阶段 C：支持真正的多 shard writer

实施内容：

1. 引入独立 PageMap writer mutex；
2. 固化 shard/writer/pool 锁顺序；
3. owner shard 在首次发布前确定；
4. Scavenger 改为逐 shard；
5. 实现真实 shard 路由和 region ownership；
6. 增加并发 node install 和 writer contention 基准。

退出条件：

- 多 shard SetSpan/ClearRange 无丢失节点或映射；
- owner-shard-local coalesce 始终成立；
- 16 线程 PageCache 扩展性优于 shard-0 基线；
- writer mutex 尚未成为主要瓶颈，或已得到量化。

风险类型：并发、性能、内存。

#### 阶段 D：按数据决定是否引入 epoch

触发条件：

- retired/stable descriptor 的 RSS 已成为目标 workload 的显著开销；
- descriptor 创建速率无法通过 region 和 split 策略降低；
- 有完整的 reader 注册、TLS 退出、fork 和 shutdown 设计。

退出条件：

- epoch reader 开销满足 fast-path 护栏；
- stalled reader、线程退出和 epoch rollover 测试通过；
- metadata RSS 的实际收益显著高于复杂度和延迟成本；
- 仍保留可切回 stable metadata 的构建选项，便于诊断和回滚。

风险类型：并发、内存、性能。

## 6. Frontend 提升

Frontend 是 ammalloc 中直接承载普通小对象分配和释放的最热层。它的目标不是缓存尽可能多的对象，而是在严格内存预算内，让绝大多数请求只访问线程或 CPU 本地状态，同时将 refill、trim、GC、跨线程归还和策略调整移入可控慢路径。

本节采用渐进路线：第一阶段把 per-thread ThreadCache 做到生命周期可靠、预算可控和可观测；第二阶段加入 sized free、增量 GC 与 remote free；只有这些机制稳定后，才实验 per-CPU/rseq Frontend。

### 6.1 职责、边界与性能目标

#### 6.1.1 Frontend 的职责

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

#### 6.1.2 层间边界

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

#### 6.1.3 目标指标

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

### 6.2 Frontend 核心不变量

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

### 6.3 当前实现基线与差距

#### 6.3.1 已有基础

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

#### 6.3.2 主要差距

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

### 6.4 分配与释放请求路由

#### 6.4.1 分配路由

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

#### 6.4.2 释放路由

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

### 6.5 ThreadCache TLS RAII 设计

#### 6.5.1 Holder 模型

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

#### 6.5.2 首次使用

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

#### 6.5.3 TLS 析构顺序

ThreadCacheHolder 的 destructor 可能发生在其他 thread_local 对象析构期间，而这些析构函数也可能分配内存。因此：

- `teardown_started` 后当前线程新的分配走 BootstrapAllocator 或无 ThreadCache 的降级路径；
- drain 前全局 allocator runtime 必须仍处于 `Ready` 或受支持的 `ShuttingDown` 阶段；
- 全局核心默认采用进程生命周期常驻，避免 TLS 晚于静态单例析构；
- ThreadCache drain 期间发生递归分配时不得重新创建同一个 ThreadCache；
- 线程退出测试必须覆盖多个 TLS 对象交叉析构。

#### 6.5.4 Thread pool 与 idle worker

长期线程池不会触发 TLS 析构，因此还需要显式接口：

- `am_thread_cache_flush()`：归还当前线程全部缓存对象；
- `am_thread_cache_idle()`：按 idle policy 缩减，而不一定完全清空；
- aethermind worker 在进入长期阻塞或模型卸载阶段调用 idle/flush；
- 接口只影响调用线程，不从外部线程直接遍历和修改其 FreeList。

### 6.6 ThreadCache 元数据分配

#### 6.6.1 候选方案

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

#### 6.6.2 Metadata 大小预算

需要报告：

```text
sizeof(ThreadCache)
bytes per active thread
metadata slab utilization
total ThreadCache metadata mapped/resident bytes
```

如果 FreeList 数量或统计字段持续增长，应将冷字段移到可选 side state，避免每个线程为从未使用的 class 支付过高固定成本。

### 6.7 FreeList 数据布局与操作

#### 6.7.1 Intrusive LIFO

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

#### 6.7.2 建议操作集

- `Push(ptr)`；
- `Pop()`；
- `PushRange(head, tail, count)`；
- `PopRange(max_count, &head, &tail)`；
- `ClearMetadataOnly()`，仅在对象所有权已转移后使用；
- `VisitForDebug()`，仅 Debug/静默期检查。

`PopRange` 应一次更新 head 和 size，避免 slow path 循环调用公开 `Pop()` 产生重复分支；但必须保持链顺序和 batch LIFO 语义。

#### 6.7.3 字段布局

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

#### 6.7.4 Pointer encoding

Hardened 模式可对 freelist next 使用进程/线程 cookie 编码，降低简单 UAF 覆盖 next 形成任意链的风险。默认性能模式是否启用必须基于基准；编码不应引入共享状态或额外内存访问。

ThreadCache 本地链不存在并发 ABA，因为只有所属线程操作。ABA 只在未来 remote lock-free queue 或 per-CPU 并发协议中需要单独处理。

### 6.8 分配快路径

#### 6.8.1 目标形态

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

#### 6.8.2 `cached_bytes` 更新

每次 pop/push 更新精确 `cached_bytes` 会增加一到两个 TLS 算术操作，但可以使总预算严格可见。候选方案：

- 精确更新：逻辑简单，慢路径决策准确；
- 分 class count，在慢路径按 class size 聚合：热路径更小，但计算和预算响应延迟增加；
- 分配减少缓存时不更新，仅 free/trim 更新近似值：容易破坏守恒，不推荐。

建议先实现精确 TLS `cached_bytes +=/-= class_size` 并通过汇编和基准决定是否需要优化。TLS 本地算术不会产生跨核共享流量。

#### 6.8.3 Prefetch

- pop 后可预取新 head，用于连续分配；
- prefetch 的局部性级别和读/写意图需按架构封装；
- 单次 malloc/free pair 的 window=1 场景可能不受益；
- 必须分别测量 steady sequence、random class 和深窗口；
- 不把硬编码 prefetch 当作普遍优化。

#### 6.8.4 Debug 与 Release

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

### 6.9 释放快路径

#### 6.9.1 普通 free

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

#### 6.9.2 不创建 ThreadCache 的释放

一个从未分配过的小线程可能只负责释放其他线程创建的对象。为一次 free 创建完整 ThreadCache 可能不划算。建议策略：

- 如果当前线程已有 ThreadCache，正常缓存；
- 如果没有，直接将单对象或小批对象归还 Middle-end；
- 只有在观察到重复小对象 free/alloc 活动后才创建 ThreadCache；
- 为 free-only 线程记录轻量活动计数，但不能依赖堆分配。

这可减少短生命周期回收线程的 metadata 和缓存膨胀。

#### 6.9.3 Overflow 判断

进入 trim 的条件可包括：

- `list.count > list.capacity`；
- `cached_bytes > hard_budget`；
- 当前 class 超过 per-class hard cap；
- 内存压力 generation 已变化；
- idle/flush 显式请求。

普通稳定 free 只应检查最常见的一到两个条件。低频压力/GC 信号可在采样计数达到阈值后检查，避免每次 free 读取共享原子。

### 6.10 Refill 慢路径

#### 6.10.1 基本事务

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

#### 6.10.2 Fetch count

fetch count 受以下约束：

```text
desired = min(
    size_class_batch,
    class_dynamic_capacity - current_count + 1,
    bytes_available_in_soft_budget / class_size + 1,
    implementation_batch_cap)
```

至少获取一个返回给调用方的对象；若 byte budget 不允许缓存额外对象，可请求一个或从 Middle-end batch 中把剩余对象立即留在 Middle-end。

#### 6.10.3 Partial refill

Middle-end 可以因 OOM 或库存不足返回少于 desired 的对象：

- `count == 0`：返回 null，FreeList 和 quota 不改变，记录 miss/failure；
- `count > 0`：一个对象返回用户，剩余对象缓存；
- policy growth 基于实际取得数量和 underflow 压力，而非假设完整 batch；
- 接收到的链必须完整属于当前 size class；
- push-range 后所有权一次性转移到 ThreadCache。

#### 6.10.4 OOM 与异常边界

- Frontend refill API 为 `noexcept`；
- Middle-end 的 metadata OOM 被转换为空 batch；
- 不允许空指针 placement new；
- 失败路径不调用分配型日志；
- public malloc wrapper 负责设置 `errno` 或 C++ new 失败语义；
- quota、overages 和 GC cursor 在 OOM 后保持合法。

#### 6.10.5 LIFO 顺序

假设 Middle-end 提供链 `A -> B -> C`，且 A 是最希望优先重用的对象：

- caller 应得到 A；
- remainder `B -> C` 以 B 为本地 head；
- 不应无意反转两次导致最冷对象先返回；
- TransferCache 数组 pop/push 和链转换必须通过顺序单测验证。

### 6.11 Overflow 与 Trim 慢路径

#### 6.11.1 基本策略

当 class capacity 溢出时：

1. 从本地 FreeList 头部 pop 一个有界 batch；
2. 更新 count 和 `cached_bytes`；
3. 构造完整 LIFO 链；
4. 批量归还 Middle-end；
5. 更新 overage/overflow policy；
6. 如总缓存仍超过 hard budget，继续选择其他 class 做有界 trim。

单次普通 class overflow 应只归还一个 batch，避免一次 free 遭遇全 ThreadCache 扫描。hard budget 超限可触发额外 trim，但必须有最大对象数/字节工作预算。

#### 6.11.2 工作集保留

- class capacity 小于一个 batch 时，可以保留 slow-start 的小容量；
- 达到稳态后通常至少保留一个近期工作 batch；
- 内存压力或 explicit flush 可以打破该下限；
- 大 size class 的保留对象数应更少，但按字节衡量不能完全固定为 1；
- 不能将全部 class 同时增长到 `batch * 8` 而忽略总预算。

#### 6.11.3 失败处理

Middle-end release 正常不应失败，因为对象已经存在；如果内部诊断发现错误：

- 对象不能同时保留在本地链和提交到 Middle-end；
- Hardened 模式终止并输出无分配诊断；
- 生产模式不能把非法对象链继续传播到其他 Span；
- Release API 应明确所有权是在调用前、调用成功点还是始终无失败转移。

### 6.12 每线程总字节预算

#### 6.12.1 为什么必须按字节预算

只限制每 class 对象数会产生以下放大：

```text
thread count * active size classes * per-class max objects * class size
```

高线程数和宽尺寸分布下，即使每个 class 看似合理，总 RSS 也可能远超预期。Frontend 必须同时控制：

- 每 class capacity；
- 每 ThreadCache 总 cached bytes；
- 全进程 Frontend 总预算；
- idle 与内存压力下的回收速度。

#### 6.12.2 预算字段

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

#### 6.12.3 预算来源

预算可由以下因素决定：

- 默认 per-thread 上限；
- 活跃线程数量；
- 线程类型或 aethermind worker role；
- 进程级 Frontend 总上限；
- cgroup/系统内存压力；
- latency/memory 运行模式；
- NUMA node 局部预算。

第一阶段使用固定 per-thread soft/hard budget；活跃线程自适应和全进程再平衡放到后续慢路径控制器，避免初始化复杂度进入热路径。

#### 6.12.4 容量竞争

一个 class 需要增长容量时：

1. 检查 `assigned_capacity_bytes + growth_bytes <= soft_budget`；
2. 若超限，从低收益 class 回收 assigned capacity；
3. 优先选择长时间无命中、缓存占比高、对象较大的 class；
4. 回收 capacity 不一定立即扫描/归还对象；若 current count 超过新 capacity，安排有界 trim；
5. hard budget 超限时直接执行实际对象回收。

### 6.13 Slow-start 与自适应容量

#### 6.13.1 输入信号

每 class 慢路径状态可基于：

- underflow/refill 次数；
- overflow/trim 次数；
- underflow 与 overflow 交替频率；
- refill 后对象被消费的速度；
- 长期 idle epoch；
- 当前 class cached bytes；
- 全 ThreadCache budget pressure；
- remote drain 贡献。

#### 6.13.2 增长策略

推荐原则：

- 冷启动容量为 1 或一个很小值；
- 连续 underflow 表明缓存过小，先指数增长到 batch；
- 达到 batch 后按小步线性增长；
- underflow/overflow 频繁交替说明 capacity 接近工作集边界，可适度增长以消除抖动；
- 任何增长都受 class hard cap 和 ThreadCache byte budget 限制。

#### 6.13.3 衰减策略

- 连续 overflow 表明 class 缓存大于近期需求；
- 多个 GC epoch 未命中时减少 assigned capacity；
- memory pressure 下按字节收益优先回收；
- capacity 不应每次事件都剧烈变化，使用 hysteresis 和最小保持 epoch；
- 衰减决策在 slow path/GC 中执行，不进入每次 pop/push。

#### 6.13.4 稳定性约束

- growth/shrink 步长有上限；
- policy counter 使用饱和算术，避免长时间运行回绕；
- 参数变化不会让 `capacity < current_count` 后长期不 trim；
- 运行时调参发布使用 generation，在慢路径采样，不要求热路径实时读取；
- 所有策略都有静态保守 fallback。

### 6.14 增量 GC、Idle 与压力回收

#### 6.14.1 活动驱动 GC

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

#### 6.14.2 Staleness

每 class 可维护近似 activity epoch：

- refill/pop 说明 allocation demand；
- overflow/push 说明 deallocation pressure；
- 多个 GC epoch 无 demand 且 count 较高，判定 stale；
- 不需要每个 fast hit 都更新时间戳，可通过采样计数或慢路径事件近似。

#### 6.14.3 Idle 接口

`am_thread_cache_idle()` 建议：

- 保留少量最热 class 工作集；
- 显著缩减冷 class capacity；
- 将 cached bytes 降到 idle budget；
- 不销毁 ThreadCache metadata，便于线程快速恢复；
- 幂等且不抛异常。

`am_thread_cache_flush()` 则归还全部对象并把 class capacity 恢复初始值，适合模型卸载、线程角色切换和测试隔离。

#### 6.14.4 全局内存压力

全局控制器只递增 relaxed/release pressure generation，不直接跨线程修改 FreeList：

- ThreadCache 在下一次慢路径或采样点观察 generation；
- 执行有界 pressure trim；
- 长期完全 idle 的 worker 由 aethermind 调度器显式调用 idle/flush；
- per-CPU 模式可由管理线程安全地 drain 指定 CPU cache，但必须使用其专用协议。

### 6.15 跨线程释放与 Remote Free

#### 6.15.1 当前“释放线程缓存”模型

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

#### 6.15.2 Owner-aware remote queue

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

#### 6.15.3 Queue 候选

| 方案 | free 成本 | drain 成本 | 风险 |
|---|---:|---:|---|
| MPSC intrusive stack | 一个原子 exchange/CAS | 批量取出后可能需反转 | ABA、顺序、共享热点 |
| MPSC queue | 较高 | 保持 FIFO，但局部性未必更好 | metadata/算法复杂 |
| per-owner locked batch | 低频锁 | 简单批量 | 高竞争 owner 可能阻塞 |
| 直接回 Middle-end | 获取 bucket lock | 无 owner queue | Middle-end 竞争增加 |

remote free 的目标是降低整体跨层流量和 NUMA 漂移，而不是追求 free 形式上的 lock-free。应先用 producer/consumer trace 证明收益。

#### 6.15.4 Drain 与退化

- owner 在本地 underflow 前优先 drain remote queue；
- 每次 drain 有对象数/字节上限；
- remote 链按 LIFO/FIFO 特性转换为本地 LIFO，顺序必须测试；
- owner 线程退出时先标记 slot closing，再把 queue drain 到 Middle-end；
- enqueue 观察到 closing/generation mismatch 时直接回 Middle-end；
- remote queue bytes 计入 owner 或全局 Frontend 预算，不能成为隐藏缓存。

#### 6.15.5 NUMA 策略

对于 aethermind 固定 NUMA worker，更可能有价值的是“回 NUMA-local Middle-end”而不是“回原线程”。建议依次实验：

1. 当前释放线程本地缓存；
2. free-only 线程直接回 NUMA-local Middle-end；
3. per-CPU remote queue；
4. 原 owner ThreadCache queue。

按 remote access、Middle-end lock、RSS 和 tokens/s 综合决定。

### 6.16 ThreadCache 退出、Shutdown 与 Fork

#### 6.16.1 线程退出

正确顺序：

1. holder 标记 `teardown_started`，阻止重新创建；
2. 从 remote registry 标记 owner closing；
3. drain remote incoming queue；
4. 遍历本地 size class，按 batch 归还 Middle-end；
5. 释放 epoch/registry slot；
6. 销毁 ThreadCache；
7. 将 metadata slot 回专用 pool。

整个过程不得抛异常。Middle-end 若已不可用，则必须走 allocator shutdown 明确定义的降级路径，而不是静默泄漏或 UAF。

#### 6.16.2 进程 shutdown

推荐全局核心常驻到进程退出。若支持受控 destroy：

- 先进入 `kShuttingDown`；
- 停止创建新 ThreadCache；
- 停止 Scavenger；
- 等待/协调 worker 退出并 drain；
- 再清 Middle-end/PageCache；
- 不允许未知业务线程继续 malloc/free。

外部线程不能安全地直接 flush 另一个仍运行线程的普通 FreeList；需要 cooperative generation、stop-the-world 或 per-CPU 专用协议。

#### 6.16.3 Fork

`pthread_atfork` child 路径中：

- 只保留当前线程 TLS holder；
- 重置消失线程的 registry/remote owner slot；
- 把无法确认的 remote queue 标记为需要回收；
- 当前 holder 本地 FreeList 可以保留，前提是 Middle-end/PageMap 状态已按全局 atfork 协议恢复；
- per-CPU/rseq registration 在 child 中重新注册；
- 后台线程状态重置为未启动。

### 6.17 Per-CPU Frontend 目标架构

#### 6.17.1 采用动机

per-thread cache 的内存随活跃线程数增长。大量短线程、协程调度线程或线程数显著高于 CPU 数时，per-CPU cache 可以把 Frontend 内存规模限制在 logical CPU 数量，并让同 CPU 上的线程共享工作集。

它也带来新的复杂度：线程可能迁移和被抢占，同一 CPU cache 需要无锁但不是线程私有的提交协议。

#### 6.17.2 Slab 布局

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

#### 6.17.3 CPU 总预算

per-CPU 模式按 CPU 控制总 assigned capacity：

- class 扩容需要从同 CPU 其他冷 class 借用容量；
- 不要求指针 segment 物理移动，可以只改变 logical capacity；
- CPU hard budget 决定整个 section 能缓存的对象字节或指针槽；
- 未在线 CPU 和长期 idle CPU 可以释放其缓存对象；
- CPU hotplug 需要明确支持或静态禁用策略。

#### 6.17.4 与 Middle-end 共用

per-thread 和 per-CPU Frontend 应共用同一批量 Middle-end API：

- `FetchBatch(class, max_count)`；
- `ReleaseBatch(class, head, tail, count)`；
- NUMA/shard hint 可作为附加参数；
- Middle-end 不依赖 Frontend 实现细节。

这保证可在相同 benchmark 中切换两种 Frontend，并保留快速回滚。

### 6.18 Linux rseq 协议

#### 6.18.1 基本原理

restartable sequences 允许用户态定义短临界区：线程若在临界区中被迁移、抢占到另一 CPU 或收到需要重启的事件，内核将执行流跳到 abort handler，操作从头重试。提交点之前的局部计算可以重做，提交点之后状态必须完整一致。

#### 6.18.2 Pop 提交

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

#### 6.18.3 限制

- rseq 临界区必须非常短，不能调用函数、加锁或触发 page fault 风险较高的操作；
- underflow/overflow 转入普通慢路径；
- 信号、抢占和 CPU migration 必须由 rseq abort/retry 覆盖；
- 编译器生成、汇编约束和链接段需要专门测试；
- 不支持的平台必须可靠回退到 per-thread；
- sanitizer 可能无法完整理解手写 rseq，需要额外模型测试。

#### 6.18.4 注册与回退

运行时检测：

1. kernel/architecture 是否支持 rseq；
2. libc 是否已经注册当前线程；
3. ammalloc 与其他库是否存在 rseq ABI 冲突；
4. 当前进程配置是否允许 per-CPU；
5. 失败则选择 per-thread，不影响正确性。

不得在 malloc 热路径反复探测能力。选择结果在线程/runtime 初始化时缓存。

### 6.19 Frontend 模式选择

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

### 6.20 Sized free 与 Sized delete

#### 6.20.1 接口

内部接口建议包含：

```cpp
void am_free_sized(void* ptr, size_t size) noexcept;
void am_free_aligned_sized(void* ptr, size_t size, size_t alignment) noexcept;
```

C++ sized delete 直接调用这些接口。编译器已知对象大小时可省略 PageMap pointer-to-size lookup。

#### 6.20.2 路由

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

#### 6.20.3 信任边界

标准 sized delete 要求调用方提供匹配大小；错误 size 属于调用方未定义行为。但 allocator 应防止容易发生的灾难性污染：

- Debug/Hardened 始终与 Span class 交叉验证；
- 默认模式可以按低概率采样验证；
- size 超出小对象范围时退回普通 free；
- alignment 不普通时走 aligned 路径；
- 不能仅凭 size 把大对象内部指针推入小对象 FreeList。

#### 6.20.4 指标

- sized free/delete 调用比例；
- 成功跳过 PageMap 的比例；
- cross-check failure；
- 普通 free 与 sized free 的指令数和延迟差；
- binary size 和 I-cache 影响。

### 6.21 SizeClass 自动生成

#### 6.21.1 生成器输入

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

#### 6.21.2 生成器输出

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

#### 6.21.3 优化目标

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

#### 6.21.4 验证

- 对全部输入 size 验证 `Size(Index(s)) >= s`；
- class size 单调且 index 唯一；
- 所有 class 满足 alignment；
- bitmap + objects 不超过 Span；
- round-up/乘法无溢出；
- 与旧 table 比较碎片、batch 和 benchmark；
- 使用 aethermind trace 离线重放预计 RSS 和 refill 次数。

#### 6.21.5 Logical page 解耦

allocator logical page 可以由多个 OS page 组成，也可以根据构建 profile 选择 4/8/32 KiB。影响包括：

- Span 页数和 PageMap 粒度；
- 小对象尾部浪费；
- 后端 refill 频率；
- TLB 和 hugepage packing；
- metadata 大小。

这属于跨 Frontend/PageMap/PageCache 的架构变更，不能只修改 SizeClass 常量。第一阶段保持 4 KiB，先建立生成器和对比工具。

### 6.22 对齐与特殊请求

#### 6.22.1 基础对齐

如果公开 malloc ABI 要求 16B：

- 每个普通小 size class 的 stride 必须是 16 的倍数，或通过布局确保每个对象起点都满足 16B；
- 数据区 base 单独对齐不足以修复 8B stride；
- 最小对象仍需容纳 freelist pointer；
- SizeClass table 和 Span layout 都要有静态/运行时测试。

可以选择像部分主流 allocator 一样对极小请求提供较低对齐，但必须严格符合目标 C/C++ ABI、编译器默认 new alignment 和项目兼容策略，不能与 `SystemConfig::ALIGNMENT` 自相矛盾。

#### 6.22.2 Over-aligned

- alignment 大于普通 class 能力时不进入普通 ThreadCache；
- 第一阶段通过 PageCache/extent 分配额外 padding；
- metadata 记录 user base、mapping base 和 alignment；
- sized-aligned delete 走对应路径；
- 只有 trace 证明某些 over-aligned 小请求很常见时，才增加专用 class。

#### 6.22.3 Zero-size

项目可以继续把 zero-size 映射到最小 class，但要满足：

- 返回指针可正常 free；
- alignment 与普通 malloc 一致；
- 每次是否返回唯一地址不作额外保证；
- zero-size 统计单独记录，避免污染真实 requested-bytes 碎片计算；
- OOM 时仍允许返回 null。

### 6.23 缓存局部性与伪共享

#### 6.23.1 ThreadCache 布局

- `ThreadCache` 起始地址 cache-line aligned；
- 总大小向 cache line 取整，避免 slab 中相邻实例共享尾部 line；
- FreeList 数组按常用 class 顺序连续排列；
- 极热 class header 可以靠近对象首部，冷策略字段放后部/side state；
- 当前线程独占 ThreadCache，因此其内部不同 FreeList 无需逐个 cache-line 对齐，否则会造成巨大 metadata 膨胀。

#### 6.23.2 Per-CPU 布局

- 不同 CPU section 严格隔离 cache line；
- 高频 length/capacity 字段不与其他 CPU 共享 line；
- 同 CPU 不同 class 可以紧凑，但需分析并行 slow-path 管理线程访问；
- cold global control 和 stats 不放入 push/pop 的 line；
- NUMA 上 per-CPU slab first-touch 到所属 node。

#### 6.23.3 LIFO 与对象局部性

- local free 后近期 allocate 优先取得同一对象；
- refill/trim 的链转换不应破坏 LIFO；
- remote queue 若反转链，需要测量对局部性的影响；
- poison/zeroing 可能使刚释放对象 cache line 保持热，需分别测量安全与性能模式。

### 6.24 并发与内存序

#### 6.24.1 Per-thread 模式

本地 FreeList、count、capacity、cached bytes 和 policy state 全部线程私有，不使用 atomic。线程私有性是同步契约，不应为了“看起来线程安全”增加无意义原子。

全局 registry/pressure generation 使用原子：

- 纯计数器：`memory_order_relaxed`；
- pressure generation 发布：writer release，ThreadCache 慢路径 acquire 或 relaxed + 独立策略论证；
- holder closing/generation：remote enqueue 需要 acquire/release；
- 禁止默认 seq_cst。

#### 6.24.2 Remote queue

若使用 MPSC intrusive stack：

- producer 在对象 next 完成后以 release CAS/exchange 发布；
- consumer 以 acquire exchange 取走整条链；
- next 在发布后不得由 producer 修改；
- queue head 需要 ABA/tag 或算法本身证明不会复用节点造成 ABA；
- 对象节点生命周期由 Span out-of-bitmap 语义保证，但 double free 仍需 Hardened 检测。

#### 6.24.3 Per-CPU/rseq

rseq 提供 CPU 本地提交原子性，不等价于 C++ memory model 的跨线程同步。Middle-end refill/drain、管理线程回收 CPU cache 时仍需明确的锁、停止协议或 acquire/release publication。

### 6.25 可观测性

#### 6.25.1 每 ThreadCache/CPU 指标

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

#### 6.25.2 每 SizeClass 指标

- allocate/free request；
- local hit；
- refill batch 平均/分布；
- current/max cached objects；
- capacity growth/shrink；
- GC age；
- sized-free 命中；
- remote free 比例。

#### 6.25.3 热路径统计策略

不能为每个 fast allocation 更新全局 atomic。候选方式：

- TLS 本地计数，慢路径或显式 snapshot 聚合；
- 固定采样频率；
- build-time stats 开关；
- per-CPU 模式在 CPU-local cache line 中累计；
- 聚合允许近似，避免 stop-the-world。

### 6.26 测试与故障注入

#### 6.26.1 ThreadCache 基础

- 每个 size class allocate/free；
- LIFO 单对象和 range 顺序；
- count/cached bytes 守恒；
- slow-start 到 batch、线性增长和上限；
- overflow 单 batch trim；
- hard budget 多 class trim；
- partial refill 和 OOM；
- zero-size 和 alignment；
- 不为单次 free-only 线程创建 cache。

#### 6.26.2 生命周期

- TLS holder 实际构造/析构；
- 大量短线程退出后 metadata/cached bytes 回落；
- destructor 内递归 allocation；
- explicit idle/flush 幂等；
- shutdown 与 thread exit 并发；
- fork child registry/TLS 重建；
- ThreadCache pool OOM 和回收。

#### 6.26.3 自适应策略

- 连续 underflow 扩容；
- overflow 衰减；
- underflow/overflow 交替消抖；
- capacity byte 总和不超过 hard budget；
- GC cursor 最终覆盖所有 class；
- 单次 GC 工作量有界；
- pressure generation 触发回收；
- 长期 idle class 收缩。

#### 6.26.4 Cross-thread/remote

- producer 分配、consumer 释放；
- 多 producer 单 owner；
- owner 退出与 enqueue 竞争；
- generation mismatch fallback；
- remote queue bytes 计入预算；
- 链 drain 顺序；
- NUMA-local fallback；
- double free Hardened 检测。

#### 6.26.5 Per-CPU/rseq

- rseq 可用/不可用检测；
- 临界区 underflow/overflow；
- 模拟 CPU migration/abort/retry；
- signal interruption；
- CPU offline/online 策略；
- 管理线程 drain 与 fast path 协调；
- per-thread fallback 行为一致。

#### 6.26.6 工具

- ASan：对象链、range transfer、TLS teardown；
- UBSan：class index、cached byte 算术、alignment；
- TSan：registry、remote queue、pressure generation；
- 模型化测试：owner closing/enqueue/drain 状态交错；
- 固定种子随机 allocate/free/flush/pressure；
- 汇编检查：fast path 指令和意外函数调用。

### 6.27 性能护栏与基准矩阵

#### 6.27.1 微基准

- 8B、64B、256B、4KiB allocate/free pair，window=1；
- window=256/1024 稳态；
- random size；
- refill underflow；
- overflow trim；
- sized free vs ordinary free；
- ThreadCache holder 已存在/首次创建；
- stats 开启/关闭；
- pointer encoding 开启/关闭。

#### 6.27.2 并发基准

- 1/2/4/8/16/32 线程固定 size；
- 线程数远高于 CPU 数；
- 高频短生命周期线程；
- producer/consumer cross-thread free；
- 多 producer 单 consumer；
- per-thread vs per-CPU；
- NUMA local/remote；
- idle/pressure 后恢复。

#### 6.27.3 指标

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

#### 6.27.4 回归门禁

- 所有 before/after 使用相同 Release 工具链、affinity、governor 和 THP 设置；
- 保存 Google Benchmark JSON 和 perf counter；
- fast path 性能变化超过预设阈值必须解释到指令或 cache 行为；
- 降低 miss rate不能以超预算 RSS 为代价；
- per-CPU 模式只有在目标 workload 综合优于 per-thread 时才默认启用；
- 与 TCMalloc/jemalloc 比较必须使用相同 API、工作集和字节统计口径。

### 6.28 分阶段实施与验收

#### 阶段 A：ThreadCache 正确性基线

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

#### 阶段 B：预算、GC、Sized free 与 SizeClass 生成

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

#### 阶段 C：Remote free 与 NUMA-aware 路由

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

#### 阶段 D：Per-CPU/rseq 实验与灰度

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

## 7. Middle-end 提升

Middle-end 位于 Frontend 与 PageCache 之间：向上以批次为单位吸收不同线程或 CPU 的短期供需波动，向下管理承载小对象的 active Span。它既不能成为所有线程争用的全局瓶颈，也不能通过无限缓存对象掩盖后端成本。

本节采取渐进路线：先建立严格的对象所有权、批量接口、测试和统计，再优化当前单 Bucket 的锁内工作量；随后根据竞争数据引入 size-class shard 和 NUMA-local CentralCache；批次描述符、动态容量和更复杂的反馈控制只能在基准证明收益后启用。

### 7.1 职责、边界与性能目标

#### 7.1.1 Middle-end 的职责

Middle-end 应负责：

- 在 Frontend cache 之间以批次形式平衡同一 size class 的对象；
- 通过 TransferCache 为 refill 和 trim 提供短临界区快速交换；
- 管理每个 size class 的 active Span 集合及对象 bitmap；
- 在 TransferCache miss 时从 partial Span 批量提取对象；
- 在对象真正返回 Span bitmap 后识别 empty Span；
- 在不持有 CentralCache 锁的前提下向 PageCache 申请或归还 Span；
- 对共享缓存字节、Span 利用率、锁竞争和跨 NUMA 流量实施预算与观测；
- 在 shutdown、测试 reset、fork 和内存压力阶段提供确定性的 drain 行为。

Middle-end 不应负责：

- 执行普通请求的逐对象 fast path；
- 修改 ThreadCache/per-CPU cache 的本地链表；
- split、coalesce 或回收 PageCache free Span；
- 直接调用 `mmap`、`munmap` 或 `madvise`；
- 在共享锁内执行系统调用、日志格式化或动态内存分配；
- 为大对象、任意 over-aligned extent 或专用 tensor arena 提供对象缓存；
- 用复杂的 lock-free bitmap CAS 网络替代可验证的短临界区。

#### 7.1.2 层间边界

```text
ThreadCache / per-CPU Frontend
        |
        | ObjectBatch(size_class, count)
        v
Middle-end shard
  +-------------------------+
  | TransferCache fast tier |  pointer/batch slots
  +-------------------------+
        | hit/miss/overflow
  +-------------------------+
  | Central Span tier       |  partial/full/empty + bitmap
  +-------------------------+
        |
        | Span request/release, no Central lock held
        v
PageCache owner shard
```

Frontend 只交换对象；Middle-end 借用 PageCache 所拥有的 `Span` descriptor，并在 Central 状态下独占其对象 bitmap；PageCache 只接收已经没有任何外借对象的 empty Span。

#### 7.1.3 目标指标

| 指标 | 目标方向 | 说明 |
|---|---|---|
| TransferCache hit | O(batch)、短临界区 | 不查询 PageMap，不访问 Span bitmap |
| TransferCache miss | 工作量有界 | bitmap 扫描、Span 获取和回滚均有上限 |
| 同 class 扩展性 | 随 shard 数近似扩展 | 重点验证 8B/64B 高竞争负载 |
| 锁持有时间 | 分布可观测 | 同时报告平均值、p99 和最大值 |
| Central cached bytes | 受 node/process 预算约束 | 不允许按线程数无界增长 |
| Span 利用率 | 避免大量低占用 partial Span | 同时控制后端碎片与 miss rate |
| PageCache 往返 | 按批次摊销 | 防止多线程 miss 引起 refill 惊群 |
| NUMA locality | 本地流量优先 | 跨 node 对象漂移必须可测量 |

Middle-end 优化不能以破坏现有整体护栏为代价：单线程 fast path 约 3.8 ns、随机大小约 26.0 ns，以及 16 线程 64B 压力场景约 8.9 us、100+ GiB/s。由于普通 fast path 不进入 Middle-end，任何稳定退化通常意味着增加了共享统计、改变了 Frontend 批次行为或引入了布局副作用。

### 7.2 核心不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 目的 |
|---|---|---|
| ME-1 | 任一小对象在任一时刻只有一个逻辑所有者 | 防止重复分配和双重归还 |
| ME-2 | TransferCache 中的对象 bitmap bit 保持为 0 | 表示对象已从 Span 提取，Span 不能提前回收 |
| ME-3 | `Span::use_count` 统计所有已从 bitmap 提取的对象，包括用户、Frontend 和 TransferCache 持有者 | 维持 empty 判断正确性 |
| ME-4 | 只有 `use_count == 0` 且 bitmap 全部有效位为 1 的 Span 才能归还 PageCache | 防止 UAF |
| ME-5 | 一个 active Span 在任一时刻只属于一个 Central size-class shard | 保证 bitmap 的单锁域写入 |
| ME-6 | Span 的 `size_class_idx` 与所在 bucket 一致且生命周期内不变 | 防止错误尺寸复用 |
| ME-7 | TransferCache 只保存同一 size class、同一 shard 契约下的对象 | 防止跨桶污染 |
| ME-8 | Fetch 成功移出的对象数、目标 batch 数和所有权计数完全一致 | 防止遗漏或重复交付 |
| ME-9 | Release 只有在对象成功进入 TransferCache 或恢复 bitmap 后才算完成 | 保证失败原子性 |
| ME-10 | TransferCache 锁与 SpanList 锁不嵌套 | 缩小死锁状态空间 |
| ME-11 | 持有 Central 锁时不得进入 PageCache 或执行 OS 调用 | 保持全局锁顺序 |
| ME-12 | PageMap 读路径无锁，Middle-end 不调用 PageMap 写接口 | 遵守 PageMap 契约 |
| ME-13 | 共享缓存总字节和 batch slot 数受显式预算约束 | 控制 RSS 放大 |
| ME-14 | 共享路径不使用堆分配 STL 容器或原始 owning `new/delete` | 避免分配器递归 |
| ME-15 | LIFO 顺序的定义在每次批量转换中一致 | 保持对象缓存局部性 |
| ME-16 | Reset/shutdown 只在并发请求静止后破坏共享 backing | 防止 teardown UAF |
| ME-17 | OOM、partial fetch 或发布竞争不会遗失已经从 bitmap 提取的对象 | 保证错误路径守恒 |
| ME-18 | 所有 shard 选择均可从 Span 或稳定路由信息重建 | 确保 cross-thread free 回到正确所有者 |

建议为调试和诊断构建增加守恒检查：

```text
valid_objects_in_active_spans
  = bitmap_free_objects
  + transfer_cached_objects
  + frontend_cached_objects
  + user_outstanding_objects
  + remote_queued_objects
```

生产构建不逐次维护右侧所有共享计数；可通过采样、停机快照或测试 hook 验证。

### 7.3 当前实现基线与差距

#### 7.3.1 已有基础

当前实现已经具备：

- 每个 size class 一个缓存行对齐的 `CentralCache::Bucket`；
- `SpinLock` 保护的连续指针数组 TransferCache；
- 独立 `std::mutex` 保护的 `SpanList` 和非原子 bitmap；
- 一次 `PageAllocator::SystemAlloc` 建立全部 TransferCache backing，避免普通堆递归；
- TransferCache miss 后多提取一个 batch 的预取机制；
- `Span::scan_cursor`、`std::countr_zero` 和按 bitmap word 扫描；
- full Span 后移、新ly non-full Span 前移的候选维护；
- 进入 PageCache 前释放 bucket mutex 的锁序约束；
- TransferCache overflow 对象恢复到 Span bitmap 的回滚路径；
- intrusive `FreeList` 与 `SpanList`，核心路径没有堆容器。

因此，下一步重点不是重新实现已存在的 `scan_cursor`，而是建立批量 bitmap 操作、明确所有权事务、减少重复 PageMap 查询，并用分片和预算解决真实竞争。

#### 7.3.2 主要差距

当前差距包括：

- 每个 size class 只有一个全局 Bucket，热点 class 最终集中到一把 TransferCache 锁和一把 SpanList 锁；
- TransferCache 容量固定为 `8 * batch_size`，没有全局字节预算、动态借额或工作集衰减；
- TransferCache 存储逐对象指针，批量 push/pop 仍执行 O(N) 指针复制；
- 预取固定为请求 batch，未受空闲 slot、miss rate、Span 利用率或内存压力约束；
- `AllocObject()` 每次只提取一个对象，锁内函数调用和元数据更新次数与 batch 成正比；
- release overflow 对每个对象执行一次 PageMap lookup 和 bitmap 更新；
- SpanList 只有一条链表，partial、full、empty 状态通过位置约定隐式表达；
- `GetOneSpan()` 的 unlock/refill/relock 窗口允许多个 miss 线程并行向 PageCache 取 Span，可能过度回填；
- 当前 release API 信任调用方传入的 `aligned_size`，缺少显式 shard/class 一致性校验；
- 无 TransferCache hit/miss、lock wait、Span occupancy 或 PageCache refill 专项统计；
- `Reset()` 释放 backing 后不重新初始化，而单元测试在每个用例开始调用 `Reset()`，导致大量测试没有覆盖 TransferCache fast tier；
- OOM 日志路径使用 `spdlog`，未来拦截系统 malloc 后不属于可证明的 bootstrap-safe 失败路径；
- 现有 benchmark 主要通过端到端 churn 间接触发 CentralCache，不能单独归因 TransferCache、bitmap 或 PageCache refill 成本。

### 7.4 对象所有权状态机

#### 7.4.1 状态定义

一个 Central small-object slot 可处于以下状态：

```text
SpanBitmapFree
    |
    | bitmap bit 1 -> 0, use_count++
    v
TransferCached
    |
    | pop batch
    v
FrontendCached
    |
    | local allocation
    v
UserOwned
    |
    | local free / remote free
    v
FrontendCached or RemoteQueued
    |
    | trim batch
    v
TransferCached
    |
    | TransferCache overflow/drain
    v
SpanBitmapFree
```

`TransferCached`、`FrontendCached`、`RemoteQueued` 和 `UserOwned` 对 bitmap 而言都是“已提取”状态。不能因为对象在 allocator 内部缓存就提前把 bit 设回 1，否则 Span 可能在对象仍可被重新分配时归还 PageCache。

#### 7.4.2 Span 状态

建议显式区分：

- **Partial**：bitmap 至少有一个 free slot，且 `use_count > 0`；
- **Full**：bitmap 没有 free slot，`use_count == capacity`；
- **EmptyCandidate**：`use_count == 0`，等待从 Central shard 脱链；
- **RefillInProgress**：PageCache 已交付但尚未发布到 shard 的独占 Span；
- **Detached**：已从 Central 移除，准备归还 PageCache。

状态转换必须在 owning shard 的 Span 锁下完成；`RefillInProgress` 和 `Detached` 是线程私有状态，不允许其他 Central 操作访问。

#### 7.4.3 所有权提交点

- bitmap allocate 的提交点：有效 bit 被清零且 `use_count` 增加；
- TransferCache publish 的提交点：对象写入受锁 slot 且 count 更新；
- Frontend fetch 的提交点：batch 从 TransferCache/Span tier 摘除并交给调用者；
- bitmap free 的提交点：对象有效 bit 从 0 变 1 且 `use_count` 减少；
- PageCache release 的提交点：empty Span 从 Central 列表脱链，Central 锁释放后进入 PageCache。

任何提交点后的失败都必须继续向下一合法所有者交付，或者按逆事务恢复原状态，不能通过忽略指针结束路径。

### 7.5 显式 ObjectBatch 接口

#### 7.5.1 设计目的

当前接口用 `FreeList&` 和裸链表头分别表达 fetch 与 release，数量、尾指针、size class 和 shard 信息分散在调用约定中。建议引入不分配内存的内部批次描述：

```cpp
struct ObjectBatch {
    void* head;
    void* tail;
    uint32_t count;
    uint16_t size_class_idx;
    uint16_t shard_id;
};
```

该示例只是语义草图，具体字段宽度和布局必须通过 `static_assert` 与 benchmark 决定。它不拥有额外 metadata allocation；链表链接仍存储在 free object body 中。

#### 7.5.2 接口语义

建议形成对称接口：

```text
FetchBatch(class, preferred_count, route) -> ObjectBatch
ReleaseBatch(ObjectBatch, route)
DrainTransferCache(class, shard, limit) -> ObjectBatch
```

接口应明确：

- `count == 0` 时 `head == tail == nullptr`；
- 非空 batch 中 `tail` 可从 `head` 在 `count - 1` 步内到达；
- 所有对象属于相同 size class；
- batch 交付是 move-only 所有权转移；
- partial fetch 合法，OOM 时可能返回小于 preferred count；
- release 接口返回后调用方不再访问对象链；
- shard hint 不是可信所有权证明，必要时以 Span metadata 为准。

#### 7.5.3 数组与链表边界

Frontend 适合 intrusive chain；TransferCache 当前适合 pointer array；bitmap 批量提取适合定长栈数组。转换函数必须统一顺序语义，并使用 `kMaxBatchSize` 定长存储，不得在慢路径创建 `std::vector`。

### 7.6 LIFO 语义与批量顺序

#### 7.6.1 统一定义

定义 batch 的 `head` 为下一次应优先分配的对象，即整个系统中“最热”的一端。对任意链：

```text
head -> next -> ... -> tail
```

`head` 必须首先被 Frontend pop。TransferCache push/pop、数组和链表互转都应保持这一语义，除非基准证明某个层级采用相反顺序更优，并在接口中明确标注。

#### 7.6.2 当前风险

逐对象链表转数组后再从数组尾部 pop，容易发生一次或两次隐式反转。仅验证“FreeList 自身是 LIFO”不足以证明跨层批量流转仍为 LIFO。

应加入带有稳定对象编号的顺序测试，分别覆盖：

- Frontend chain -> TransferCache -> Frontend；
- Span bitmap -> prefetch array -> TransferCache -> Frontend；
- TransferCache overflow -> bitmap -> 再次 fetch；
- partial batch 和满 batch；
- 两个 producer 交替 release 后的局部顺序。

#### 7.6.3 顺序与公平性

LIFO 有利于 cache locality，但可能让冷对象长期停留在 TransferCache。容量衰减和 drain 应优先归还较冷的一端；不应为了公平性改变普通 fetch 的热端优先行为。

### 7.7 TransferCache 数据结构路线

#### 7.7.1 路线 A：连续指针栈

保留当前模型：

```text
void* slots[capacity]
size_t count
SpinLock lock
```

优点：

- 实现简单，所有权容易验证；
- 支持任意 1..N 个对象的 partial batch；
- backing 可一次性向 PageAllocator 申请；
- push/pop 临界区短且无 bitmap 操作。

代价：

- 每批执行 O(N) 指针复制；
- 大 batch 会延长自旋锁持有时间；
- 容量调整需要额外 slot 管理；
- 单 Bucket 下 cache line 在高并发时持续迁移。

该路线应作为正确性基线和可靠回退模式。

#### 7.7.2 路线 B：固定批次描述符

类似主流分配器的 transfer cache，可缓存已经链接好的完整 batch：

```text
BatchSlot { head, tail, count }
```

一次 push/pop 只移动描述符，而不是复制每个指针。要求：

- BatchSlot backing 预分配且不依赖系统 malloc；
- 完整 batch 走 descriptor fast tier；
- partial batch 走小型 pointer stack 或直接 Span tier；
- descriptor 中的链保持既定 LIFO；
- 不能让同一对象链同时被两个 slot 引用；
- reset/drain 能遍历所有 slot 并恢复 bitmap 所有权。

代价是状态机和碎片化策略更复杂。只有在 `batch_ptr_copy_cycles`、锁持有时间和多线程吞吐数据表明逐指针复制是主要瓶颈时才引入。

#### 7.7.3 推荐决策

短期继续使用 pointer stack，但抽象出 `ObjectBatch`、bulk push/pop 和一致顺序；中期增加 descriptor 实验开关，针对小对象大 batch 场景 A/B 测试。不要一次同时引入 descriptor、shard 和 NUMA，以免性能变化无法归因。

### 7.8 TransferCache 批量操作

#### 7.8.1 Bulk pop

建议流程：

1. 在锁外计算 `wanted = min(requested, kMaxBatchSize)`；
2. 获取 TransferCache 锁；
3. 计算 `n = min(wanted, count)`；
4. 将热端连续 `n` 个 slot 复制到栈数组或生成 batch；
5. 一次更新 `count`；
6. 释放锁；
7. 在锁外完成数组到 intrusive chain 的转换。

锁内不得执行 FreeList 遍历、PageMap lookup、bitmap 修改或共享日志。

#### 7.8.2 Bulk push

建议流程：

1. 在锁外把输入 chain 的有限前缀转换为栈数组，或保留 descriptor；
2. 获取 TransferCache 锁；
3. 计算可接收 slot 数；
4. 连续复制并一次更新 `count`；
5. 释放锁；
6. 将剩余对象交给 Span tier。

如果调用方已经提供定长 pointer array，不应先构链再拆链。

#### 7.8.3 临界区上限

即使 `kMaxBatchSize` 允许 512 个对象，也应测量单次锁内复制的最大周期。必要时将 bulk 操作按 32/64 个指针分段，但分段会增加锁获取次数，必须根据 p99 lock hold 和吞吐选择，而非凭经验固定。

### 7.9 容量、字节预算与动态借额

#### 7.9.1 为什么不能只用固定倍数

`capacity = 8 * batch_size` 简单，但不同 class 的实际流量、对象字节和 Span 成本差异很大：一个冷 class 可能长期占据 slot，而热点 class 频繁溢出；按对象数等比例也不等于按字节公平。

建议至少维护：

- process/node 级 CentralCache byte budget；
- 每 class/shard 的 minimum slots；
- 当前 capacity slots/bytes；
- occupancy 高水位和低水位；
- fetch hit、partial hit、push overflow；
- 最近一个控制窗口的收益分数。

#### 7.9.2 预算口径

需要区分：

- `transfer_object_bytes`：TransferCache 内对象按 class size 加权；
- `transfer_metadata_bytes`：pointer slots 或 BatchSlot backing；
- `central_span_free_bytes`：active Span bitmap 中尚未提取的对象容量；
- `central_span_active_bytes`：Central 持有的全部 Span 页；
- `empty_span_retained_bytes`：暂存但可立即归还 PageCache 的空 Span。

对象从 Span bitmap 提取到 TransferCache 时，不增加 active Span 页数，但会改变对象缓存归属；统计不能重复计入 allocator 总 active bytes。

#### 7.9.3 动态借额

容量调整只在慢路径或控制周期执行，并以 batch 为最小单位：

- 高频 miss 且高 occupancy 的 class 可申请一个 batch slot；
- 长期低 occupancy 的 class 归还一个 batch slot；
- push overflow 与紧随其后的 fetch miss 同时较高，说明容量可能不足；
- 只有 overflow 而没有后续 hit，说明对象应更快回 bitmap/PageCache；
- 总预算超限时按“每字节避免的慢路径次数”从低收益 class 回收。

动态 slot pool 必须使用预分配数组、位图或 intrusive free list，不能使用 `std::vector` 扩容。

#### 7.9.4 控制稳定性

- 设置最小观察窗口；
- 增长与衰减采用不同阈值形成滞回；
- 每周期最多调整有限 batch；
- 内存压力事件可绕过普通衰减直接收缩；
- 配置变化只影响慢路径，热路径不读取复杂全局结构。

### 7.10 Size-class Bucket 分片

#### 7.10.1 分片目标

单 size-class Bucket 是热点 class 的扩展性上限。目标结构为：

```text
CentralCache[node]
  Class[0]
    Shard[0..S0)
  Class[1]
    Shard[0..S1)
  ...
```

不同 class 可使用不同 shard 数：8B/64B 等热点 class 可以更多，冷门大 class 保持 1 个，避免 metadata 和空 Span 放大。

#### 7.10.2 稳定路由

Frontend refill 应使用稳定 route，例如：

- per-thread 模式：初始化时选择 node/shard，并在一段时间内保持；
- per-CPU 模式：由当前 CPU 映射到本 node 的固定 shard；
- remote free：优先回到 Span 的 Central owner shard，而不是释放线程随机选择的 shard。

每次请求随机 hash 会让对象和 Span 在 shard 间漂移，破坏局部性并增加无法合并的低占用 Span。

#### 7.10.3 Span owner 标识

真正分片后，release 必须从对象对应 Span 恢复 Central owner。需要稳定的 `central_shard_id` 或等价信息：

- 可在 `Span` 的紧凑状态字段中编码；
- 可使用由 PageMap/region 推导的稳定 arena id；
- 或使用 allocator-owned side metadata。

`Span` 当前严格为一个缓存行，不能未经布局与性能评估直接增加字段。任何编码都必须验证 shard id、size class、generation 和 PageCache owner 不混淆。

#### 7.10.4 不允许跨 shard 共管一个 Span

一个 Span 的 bitmap 只能由一个 Central shard 锁保护。多个 shard 共享同一 Span 会把简单互斥重新变成 bitmap 原子竞争，并使 empty 判定和归还协议复杂化，应明确禁止。

### 7.11 NUMA-local Middle-end

#### 7.11.1 拓扑

建议每个 NUMA node 拥有独立 CentralCache 组，并优先从同 node PageCache arena 获取 Span：

```text
CPU/thread -> local Frontend -> local Central shard -> local PageCache region
```

这要求 PageCache 的 region ownership 先稳定；Middle-end 不能单独通过 node hash 承诺物理页本地性。

#### 7.11.2 Cross-node free

对象在哪个 node 分配与在哪个 node 释放可能不同。候选策略：

1. 直接回 owner node/shard，保持 Span 聚合，但产生远端写；
2. 先进入释放 node 的 bounded remote queue，再批量发送 owner；
3. 在高漂移 workload 下允许有限 Span reassignment，但只能在 Span 完全 drain 后执行。

默认推荐前两种，不允许活跃 Span 直接换 owner。

#### 7.11.3 NUMA 统计

至少记录：

- local/remote Central fetch；
- local/remote release；
- owner-node queue depth；
- 跨 node batch 数和字节；
- node 间 Span 获取/归还；
- `perf` NUMA remote access 或等价硬件计数；
- aethermind worker/socket 对应的 tokens/s 与尾延迟。

NUMA 模式必须提供单 node 和不可识别拓扑时的透明回退。

### 7.12 FetchRange 事务化流程

#### 7.12.1 阶段 1：路由与校验

- 将 aligned size 映射为 class index；
- 校验请求数不超过 `kMaxBatchSize`；
- 选择稳定 node/shard；
- 读取只在慢路径更新的 batch/capacity hint；
- 不创建动态容器，不触发日志格式化。

#### 7.12.2 阶段 2：TransferCache probe

- bulk pop 最多 requested 个对象；
- 命中完整 batch 时直接返回；
- partial hit 合法，剩余部分进入 Span tier；
- 释放 TransferCache 锁后再构造输出链；
- hit/miss 统计采用采样或 shard-local relaxed counter。

#### 7.12.3 阶段 3：Span tier 提取

在 shard Span 锁下：

1. 从 partial list 选择候选 Span；
2. 使用 bulk bitmap API 提取调用方缺少的对象；
3. 在预算允许时额外提取 prefetch 对象；
4. 更新 use_count、scan cursor 和 Span 状态列表；
5. 将 caller batch 与 prefetch batch 分开；
6. 释放 Span 锁。

如果没有 partial Span，进入独立 refill 协议，而不是持锁调用 PageCache。

#### 7.12.4 阶段 4：发布预取对象

预取对象在 bitmap 中已经提交为 extracted，必须满足下列之一：

- 成功发布到 TransferCache；
- 直接并入调用方 batch且不超过 requested 语义；
- 恢复到对应 Span bitmap；
- 交给另一个明确所有者。

TransferCache 在 bitmap 扫描期间可能被其他线程填满，因此发布失败不是异常。回滚路径必须有界且可测量。

#### 7.12.5 Partial fetch 与 OOM

PageCache OOM 时：

- 已从 TransferCache 或现有 Span 取得的对象仍可返回；
- 没有取得任何对象时返回 empty batch；
- 不修改原调用方 FreeList 的既有对象；
- errno 等公共 ABI 语义由上层统一处理；
- 不在 OOM 路径调用可能分配内存的 logger。

### 7.13 ReleaseRange 事务化流程

#### 7.13.1 输入规范化

- 空 batch 直接返回；
- batch count、head/tail 和 class 必须一致；
- Release 构建至少保证内部可信调用不会静默丢对象；
- hardening/debug 模式校验链长度、循环、对齐和 class；
- 不可信公共 free 的指针验证策略由 ABI/安全章节定义。

#### 7.13.2 阶段 1：TransferCache absorb

优先将 batch 热端放入匹配 shard 的 TransferCache：

- 锁内只复制指针/描述符并更新 count；
- 满容量时产生 leftover batch；
- 若内存压力要求 bypass，可直接把整批送往 Span tier；
- 不允许为了避免 overflow 临时扩大未预算的 backing。

#### 7.13.3 阶段 2：按 Span 恢复 bitmap

对 leftover：

1. 使用 PageMap 无锁读取解析 Span；
2. 验证 Span class 与 Central owner shard；
3. 在 owner shard 的 Span 锁下批量设置 bitmap bit；
4. 更新 use_count 和 Span 状态；
5. 将变为 empty 的 Span 脱链到线程私有 release list；
6. 释放 Central 锁后逐个或批量归还 PageCache。

对象不能因为 PageMap 返回空或 class 不一致而被简单忽略。对于内部不变量破坏，debug/hardening 应立即报告；普通 Release 构建也应采用明确 fail-fast 或 quarantine 策略，而不是泄漏后继续运行。

#### 7.13.4 跨 shard batch

如果输入链可能包含多个 Central owner shard，不能持有 shard A 锁再获取 shard B。应在定长栈数组中分组，或按对象生成 bounded 子批次，依次处理单一 owner shard。

### 7.14 SpanList 状态分组

#### 7.14.1 显式列表

建议将单条隐式排序 SpanList 演进为：

- `partial`: 有 free bitmap bit，优先用于 fetch；
- `full`: 没有 free bit，不参与扫描；
- 可选 `empty`: 短期保留的全空 Span；
- `refill_pending`: 不是 SpanList，而是 shard 状态/计数。

由于 `Span` 只有一组 intrusive `next/prev`，同一时刻只能位于一条 Central 或 PageCache 链表，状态转换必须先 erase 再 insert。

#### 7.14.2 Partial 选择策略

候选策略需要在局部性和碎片之间权衡：

- 优先高占用 partial Span：更快填满，有利于其他低占用 Span 变空；
- 优先最近使用 Span：更高 cache/TLB locality；
- round-robin：更公平，但可能扩大活跃工作集。

默认建议使用 occupancy bucket 或有限候选窗口，不引入全局平衡树。遍历必须有上限，禁止在锁内为寻找“最优”Span 执行 O(number_of_spans) 全扫描。

#### 7.14.3 Full -> Partial

对象从 full Span 真正恢复到 bitmap 时，Span 必须立即进入 partial 候选；对象仅进入 TransferCache 时 bitmap 仍为 0，因此不会触发该转换。

#### 7.14.4 Partial -> Empty

`use_count` 降为 0 后：

- 校验有效 bitmap bits 全为 1；
- 从 partial 列表摘除；
- 根据 empty retention 策略保留或脱离；
- Central 锁外归还 PageCache。

### 7.15 Bitmap word 级批量化

#### 7.15.1 Bulk allocate

为 Span 增加语义等价的 bulk API：

```text
AllocBatch(out[], max_count) -> actual_count
```

每个 bitmap word 的处理可为：

1. 加载 `word`；
2. 循环 `countr_zero(word)` 提取多个 bit；
3. 在局部寄存器清除 bit；
4. 一次写回 bitmap word；
5. 批量增加 `use_count`；
6. 只在 word 变空时推进 `scan_cursor`。

这减少逐对象函数调用、bitmap load/store 和 cursor 写入次数。bitmap 仍由 shard mutex 保护，不需要 atomic。

#### 7.15.2 Bulk free

将同一 Span 的对象按 bitmap word 聚合为 mask：

```text
free_mask[word_index] |= 1 << bit
```

然后：

- 验证 `bitmap[word] & free_mask == 0`；
- 一次 OR 写回；
- 按 `popcount(free_mask)` 减少 use_count；
- 将 cursor 更新为最小受影响 word。

必须检测同一 batch 内重复对象；仅使用 OR mask 会吞掉重复项，使 use_count 与 bitmap 不一致。可通过输入计数与 mask popcount 对比、固定哈希表或诊断模式逐项验证。

#### 7.15.3 边界校验

bulk API 必须覆盖：

- capacity 不是 64 整数倍；
- 尾 word 的无效 bit 永远为 0；
- 对象地址位于 data region 且严格按 class size 对齐；
- `global_obj_idx < capacity`，而不只是 bitmap index 有效；
- use_count 不上溢/下溢；
- scan cursor 永不超过 bitmap word count 的合法终止值。

#### 7.15.4 不引入无锁 bitmap

多线程 CAS 同一 bitmap word 会导致 cache line bouncing、复杂 ABA/empty 协议和难以验证的 Span 归还。优先通过 shard 降低竞争，并保持 bitmap 在短 mutex 临界区内批量更新。

### 7.16 PageMap 查询摊销与按 Span 分组

#### 7.16.1 当前成本

TransferCache overflow 后，当前实现为每个对象执行一次四层 PageMap 读取。读路径虽无锁，但会产生多级依赖 load；一个 batch 中多个对象通常来自少量 Span，可以摊销。

#### 7.16.2 低复杂度优化

先实施：

- 记忆 `last_span`，指针仍落在其 page range 时直接复用；
- 对相邻链节点预取 PageMap 路径或 Span metadata；
- Frontend 构建 trim batch 时尽量保留同 Span 局部性；
- sized release 仍需 Span 来恢复 owner，不应误认为 class size 能完全替代 PageMap。

#### 7.16.3 定长分组表

在低命中情况下，可使用栈上固定大小的 open-addressed table：

```text
Span* -> pointer sub-batch / masks
```

表容量由 `kMaxBatchSize` 上界决定，不能动态扩容。溢出时退化为逐对象处理，禁止隐藏 O(N²) 链表查找。

#### 7.16.4 何时值得实施

记录 `unique_spans_per_release_batch`、PageMap lookup cycles 和 bulk-free cycles。只有 batch 中对象确实高度聚集且 PageMap 占比较高时，分组表才可能覆盖其初始化成本。

### 7.17 自适应预取

#### 7.17.1 输入信号

预取量不应恒等于调用 batch。建议使用：

- TransferCache 当前 free slots；
- 最近 miss/partial-hit rate；
- push overflow rate；
- Span tier lock wait；
- 当前 class occupancy；
- node/process Central byte budget；
- memory pressure 和 empty Span 数；
- Frontend 请求 batch 的移动平均。

#### 7.17.2 基本策略

```text
prefetch = min(
  available_transfer_slots,
  class_prefetch_limit,
  budget_remaining / class_size,
  span_extractable_objects)
```

低 miss 或高 overflow 时将 prefetch 衰减到 0；持续 miss 且发布后很快被消费时逐步增加。调节只在慢路径进行。

#### 7.17.3 发布竞争

扫描 Span 时不应持有 TransferCache 锁，因此 free slots 只是快照。三种方案：

- **容忍竞争并回滚**：最简单，保留当前模型；
- **预留 slot credit**：扫描前预留容量，状态更复杂；
- **直接把 extra 交给等待者**：需要请求合并机制。

短期建议保留有界回滚并统计 `prefetch_publish_failure`；只有该比例显著时才评估 credit reservation。

#### 7.17.4 防止反馈振荡

- miss 和 overflow 使用不同阈值；
- 增长慢、压力收缩快；
- 单次最多改变一个 batch；
- 至少经过一个统计窗口再反向调整；
- benchmark 必须覆盖潮汐型 producer/consumer，而不仅是稳态循环。

### 7.18 PageCache refill 惊群抑制

#### 7.18.1 问题

当前获取新 Span 时释放 `span_list_lock` 再进入 PageCache。这避免锁序反转，但多个线程可同时观察到无可用 Span，各自完成后端分配，导致：

- 短时间创建过多 Span；
- PageCache 锁竞争放大；
- 低占用 partial Span 增多；
- OOM/压力场景产生级联慢路径。

#### 7.18.2 Single-flight 状态

每 shard/class 可维护轻量状态：

- `refill_in_progress`；
- `refill_generation`；
- 可选有限 waiter hint；
- 最近失败时间或 backoff 状态。

首个线程成为 refiller，锁外进入 PageCache；其他线程可：

- 重新检查 TransferCache；
- 尝试同 class 另一 shard；
- 在短暂自旋后让出；
- 或在允许 partial fetch 时直接返回已有对象。

不要在 allocator 内部无界等待条件变量，尤其不能持有其他缓存锁等待。

#### 7.18.3 发布协议

refiller 从 PageCache 得到 Span 后在锁外 `Init`，随后重新获得 shard 锁：

- 重新验证 shard 仍运行且 class 匹配；
- 发布到 partial list；
- 清除 in-progress 状态并递增 generation；
- 若 shutdown 已开始，则不发布，直接在锁外归还 PageCache。

#### 7.18.4 Refill 数量

默认一次获取一个 Span。只有 PageCache 往返被证明是瓶颈且 class 持续高压时，才允许一次预取多个 Span；额外 Span 必须计入 Central budget 和 empty retention，不能形成隐性 arena。

### 7.19 Empty Span 保留与归还

#### 7.19.1 即时归还

当前语义是 `use_count == 0` 后立即归还 PageCache。优点是内存聚合快、Central 状态简单；缺点是潮汐 workload 可能在 Central/PageCache 间反复移动 Span。

#### 7.19.2 有界保留

可为热点 class/shard 保留极少量 empty Span：

- 按 class 或 shard 设置 0..N 个上限；
- 记录变空时间或 epoch；
- 后续 miss 可直接重新初始化/复用；
- 内存压力、idle 或预算超限立即 drain；
- retained empty bytes 纳入独立统计。

由于 Span bitmap 已全 free，保留不会产生对象所有权歧义，但页仍占用 Central active working set。

#### 7.19.3 Decay

使用慢路径事件或后台增量扫描，每轮只处理有限 shard/class。不要让普通 free 因 empty Span 而同步扫描整个 CentralCache。

#### 7.19.4 采用门槛

仅当 `empty_span_bounce`、PageCache lock wait 和系统整体 p99 表明即时归还造成明显抖动时启用；同时验证 RSS/active bytes 不显著恶化。

### 7.20 锁域与锁顺序

#### 7.20.1 锁域

| 锁 | 保护数据 | 禁止在锁内执行 |
|---|---|---|
| TransferCache SpinLock | slots、count、capacity/credit 的热状态 | PageMap、bitmap、PageCache、日志、动态分配 |
| Central shard Span mutex | Span lists、bitmap、use_count、scan cursor、refill 状态 | PageCache、OS syscall、阻塞日志 |
| PageCache shard mutex | free Span、split/coalesce、PageMap 写 | Central shard 操作、Frontend 操作 |

#### 7.20.2 全局锁序

正常运行期推荐约束：

```text
Frontend local state
  -> TransferCache lock          (随后释放)
  -> Central Span lock           (随后释放)
  -> PageCache shard lock
  -> PageAllocator/OS
```

箭头表示调用层次，不表示这些锁可以同时持有。TransferCache lock 与 Span lock 不嵌套；Central 与 PageCache 锁不嵌套；OS 调用不在任何自旋锁内。

#### 7.20.3 多 shard 操作

普通 fetch/release 一次只处理一个 Central shard。若 drain/reset 必须遍历多个 shard，应逐 shard 摘取到线程私有 intrusive list，再在无 Central 锁状态执行下层归还，不同时持有两把 shard 锁。

#### 7.20.4 SpinLock 使用边界

TTAS SpinLock 适合极短且不会调度阻塞的 pointer slot 操作。需要：

- 将自旋/让出阈值集中配置；
- 统计 try-lock failure 或采样 wait cycles；
- 在 oversubscription 场景评估 mutex/futex 或分片是否更优；
- 不因“自旋锁更快”的假设忽略 p99 CPU 消耗。

### 7.21 原子变量与内存序

#### 7.21.1 锁内状态

`transfer_cache_count`、Span bitmap、`use_count` 和 SpanList link 在对应锁内访问，不需要改为 atomic。锁的 acquire/release 已提供发布关系。

#### 7.21.2 统计与提示

以下无同步语义的计数器可使用 `memory_order_relaxed`：

- hit/miss/overflow 次数；
- requested/returned objects；
- lock contention samples；
- refill/prefetch 次数；
- bytes high-water hint。

近似统计不得参与对象所有权或 empty 判定。

#### 7.21.3 Single-flight

若 `refill_in_progress` 只在 Span mutex 下访问，则保持普通字段；若设计为锁外快速提示，使用显式 acquire/release 或 relaxed hint + 锁内重新验证。不能仅凭 relaxed load 决定对象所有权转换。

#### 7.21.4 配置发布

动态容量、压力模式或采样率可通过 generation/config snapshot 发布：控制线程 release store，慢路径 acquire load。普通 Frontend fast path不应为此增加 acquire load。

### 7.22 内存压力与主动排空

#### 7.22.1 压力等级

建议定义：

- **Normal**：按命中率维护容量和有限 empty retention；
- **Soft pressure**：停止预取，缩减低收益 TransferCache；
- **Hard pressure**：旁路 TransferCache release，drain cached objects 和 empty Span；
- **Emergency/OOM**：同步完成有限关键回收，禁止递归日志与复杂控制逻辑。

#### 7.22.2 Drain 顺序

1. 停止新 prefetch/容量增长；
2. 从低收益 class 的 TransferCache 摘取有界 batch；
3. 恢复对应 Span bitmap；
4. 将 newly empty Span 在 Central 锁外归还 PageCache；
5. 再由 Backend/Scavenger 决定 madvise 或 unmap。

Middle-end drain 只改变 allocator 内部所有权，不直接调用 `madvise`。

#### 7.22.3 有界工作量

每次前台协助回收应有 `max_batches` 或 `max_bytes` 上限；后台控制线程按 shard 增量推进。高压信号不能让每个分配线程同时全量扫描所有 bucket。

#### 7.22.4 与 Frontend 协同

压力 generation 由慢路径观察：Frontend 先 trim 本地冗余，Middle-end 再 drain shared cache。必须避免两层同时振荡式清空又立即大批 refill。

### 7.23 Reset、Shutdown 与 Fork

#### 7.23.1 Reset 语义

需要明确区分：

- `DrainForTest()`：清空对象和 Span，但保留/重新初始化 TransferCache backing，可继续使用；
- `DestroyForShutdown()`：释放 backing，之后禁止普通请求；
- `ReinitializeForTest()`：在确认全局静止后重建完整 Central 状态。

当前 `Reset()` 释放 backing 后仍允许测试继续 fetch，实际退化为没有 TransferCache 的路径。应通过 API 语义和测试消除这种模糊状态。

#### 7.23.2 Quiescence

破坏性 reset 前必须保证：

- 所有 Frontend 已 drain；
- 没有正在执行 fetch/release；
- 没有 remote batch 等待 Central owner；
- refill single-flight 已结束；
- scavenger 不会并发访问相关 Span。

Reset 不应试图在未知并发调用存在时“尽量工作”。

#### 7.23.3 Shutdown 顺序

```text
stop new public allocations
 -> drain/close remote queues
 -> drain Frontend
 -> drain TransferCaches into Span bitmaps
 -> detach empty/active Central Spans
 -> release to PageCache
 -> destroy TransferCache metadata backing
 -> shutdown PageCache/PageAllocator
```

若生产策略选择进程退出时故意保留 allocator singleton，也应明确记录，而不是依赖未定义静态析构顺序。

#### 7.23.4 Fork

`pthread_atfork` 策略应：

- prepare 阶段按固定顺序冻结控制线程和 Central shard；
- parent 阶段按逆序恢复；
- child 阶段重置只存在于其他线程的 refill/owner 状态；
- child 中保留的锁不得继承为永久 locked；
- atfork handler 不分配内存、不输出复杂日志。

### 7.24 元数据自举与递归规避

#### 7.24.1 Backing 分配

TransferCache slots、BatchSlot、shard 数组和统计结构必须：

- 编译期定长内嵌；或
- 一次性通过 PageAllocator/SystemAlloc 建立；或
- 由 allocator-owned metadata arena/ObjectPool 管理。

禁止在初始化、扩容、NUMA node hotplug 或统计注册时使用会回到系统 malloc 的 STL 容器。

#### 7.24.2 动态容量不等于动态分配

容量借额应在预分配 slot backing 内重新分区。可以使用固定 freelist、bitmap 或 index stack 管理空闲 segment；不能在热点 class 扩容时调用 `new[]`。

#### 7.24.3 OOM 失败路径

- metadata backing 建立失败时返回可定义的初始化失败或进入明确降级模式；
- 如果核心 singleton 无法工作，使用 bootstrap-safe raw write 后 fail-fast；
- 不调用 `spdlog`、`std::string`、iostream 或可能初始化 locale 的设施；
- 不允许“TransferCache 初始化失败但半初始化 Bucket 继续运行”而没有显式模式标志。

#### 7.24.4 NUMA 拓扑存储

最大 node/shard 数建议使用构建期上限与运行期 active count；超出上限时回退共享 node，而不是动态创建 `std::vector<CentralCache>`。

### 7.25 可观测性与守恒统计

#### 7.25.1 每 class/shard 指标

至少包括：

- fetch requests/objects；
- full hit、partial hit、miss；
- release requests/objects；
- push accepted/overflow；
- current/capacity/high-water slots；
- prefetch extracted/published/rolled-back；
- bitmap words scanned、objects extracted/freed；
- partial/full/empty Span 数；
- PageCache refill/release 数；
- refill single-flight collisions；
- lock wait/spin samples；
- local/remote node batch。

#### 7.25.2 字节指标

- transfer cached bytes；
- Central active Span bytes；
- bitmap-free usable bytes；
- extracted outstanding bytes；
- empty retained bytes；
- metadata backing bytes；
- budget assigned/used/reclaimable bytes。

#### 7.25.3 守恒快照

提供仅用于测试/诊断的 stop-the-world snapshot：遍历 TransferCache、SpanList 和 bitmap，验证：

- slot 中无重复指针；
- slot 指针对应 Span/class/shard 正确；
- 每 Span `use_count == capacity - free_bit_count`；
- full/partial/empty 列表与 bitmap 状态一致；
- PageMap 仍指向相同 Span；
- Central cached object bytes 与逐项求和一致。

该功能可以较慢，但不能用于在线热路径。

#### 7.25.4 统计开销

- 普通 fast path 不写 Central 统计；
- Middle-end 已是慢路径，可使用 shard-local relaxed counter；
- 纳秒级锁等待直方图采用采样；
- 导出时聚合，不在更新时获取全局锁；
- benchmark 同时运行 statistics on/off，量化观测成本。

### 7.26 测试与故障注入

#### 7.26.1 TransferCache 基础测试

- 空/满/partial bulk push-pop；
- 精确 capacity 边界；
- batch 为 1 和 `kMaxBatchSize`；
- 指针数组与 intrusive chain 双向转换；
- 端到端 LIFO 顺序；
- overflow leftover 无遗漏；
- backing 对齐、分区不重叠。

#### 7.26.2 所有权与 bitmap 测试

- bitmap bit、use_count 与对象状态逐步对应；
- TransferCache 中对象不恢复 free bit；
- full -> partial -> empty 转换；
- capacity 非 64 倍数的尾 bit；
- 同 batch 重复 free；
- 错误 class、错误 shard、非对象起始地址；
- empty Span 归还后不再出现在 Central 列表。

#### 7.26.3 并发测试

- 多线程同 class fetch/release；
- TransferCache hit 与 Span miss 并发；
- prefetch publish 时其他线程填满 cache；
- full Span 在多个 release batch 下转 partial/empty；
- refill single-flight 与 PageCache OOM；
- 多 shard cross-thread free；
- NUMA owner 线程退出和 remote batch drain；
- TSan 下验证锁域，无 data race。

#### 7.26.4 生命周期测试

- drain 后继续使用；
- destroy 后拒绝请求；
- 重复 init/drain 的幂等性；
- reset 与未 drain Frontend 的负向测试；
- fork 前存在 contended shard/refill；
- shutdown 时存在 empty retained Span 和 remote batch。

#### 7.26.5 故障注入

在确定点注入：

- TransferCache backing 分配失败；
- PageCache refill 返回 null；
- partial fetch；
- prefetch 发布容量竞争；
- owner shard closing；
- pressure generation 切换；
- NUMA topology 不可用；
- metadata pool 耗尽。

每次失败后运行守恒快照，确认没有对象丢失、重复、Span 悬挂或锁未释放。

#### 7.26.6 修复测试盲区

CentralCache fixture 应保证两类模式分别覆盖：

- TransferCache backing 存在且启用的正常模式；
- 显式禁用 TransferCache 的 Span-only 降级模式。

不能通过一个会永久释放 backing 的 `Reset()` 无意中让全部用例只测试降级路径。测试还应直接读取受控 diagnostics，确认 hit、prefetch、overflow 分支确实执行。

### 7.27 性能基准与回归门禁

#### 7.27.1 专项微基准

建议新增独立目标或过滤项：

- `BM_Central_TransferFetchHit/{8,64,256,4096}`；
- `BM_Central_TransferReleaseHit/...`；
- `BM_Central_TransferPartialHit/...`；
- `BM_Central_BitmapAllocBatch/...`；
- `BM_Central_BitmapFreeBatch/...`；
- `BM_Central_PrefetchMiss/...`；
- `BM_Central_PageRefill/...`；
- `BM_Central_EmptySpanBounce/...`。

微基准必须把准备/清理移出计时区，分别报告 ns/object、ns/batch 和 cycles/object。

#### 7.27.2 并发矩阵

变量至少包括：

- 线程数：1、2、4、8、16、32、oversubscribed；
- 大小：8B、64B、256B、4KiB、接近 `MAX_TC_SIZE`；
- batch：1、自然 batch、最大 batch、partial batch；
- 模式：单 Bucket、sharded、NUMA-local；
- 流量：balanced、allocate-heavy、free-heavy、producer/consumer、潮汐；
- 工作集：L1/L2 可容纳、跨 LLC、内存压力；
- affinity：固定 CPU、跨 socket、允许迁移。

#### 7.27.3 采集指标

- TransferCache hit/miss/overflow；
- lock acquisitions、wait cycles、hold cycles；
- bitmap words/object；
- PageMap lookup/object；
- PageCache refill/release rate；
- cycles、instructions、branches、branch misses；
- L1/LLC miss、cache-to-cache、NUMA remote access；
- active/resident/Central cached bytes；
- p50/p99/p999 batch latency；
- 端到端 ammalloc 吞吐及 aethermind tokens/s。

#### 7.27.4 与现有基准衔接

继续运行：

- `BM_Malloc_Churn` window 1024；
- `BM_Malloc_Deep_Churn`；
- 8B/64B 多线程 batch workload；
- 随机大小多线程 workload；
- 3.8 ns fast path 与 26.0 ns random-size 护栏。

专项基准用于归因，端到端基准用于验收。不能只因 Central 微基准改善就接受 Frontend fast path 或 RSS 退化。

#### 7.27.5 回归判定

- 使用相同 Release 编译器、CPU affinity、governor、THP 和 NUMA policy；
- 保存 Google Benchmark JSON、allocator stats 和 perf 数据；
- 至少进行多轮交错 before/after，报告中位数和波动；
- 单线程 fast path 的噪声外退化必须阻断；
- 16 线程 64B 不得低于既有护栏，除非先证明原统计口径错误；
- 吞吐收益不能以无预算 Central RSS 或严重 p999 退化换取；
- shard/NUMA 默认开启前必须在真实 aethermind trace 上验证。

### 7.28 分阶段实施与验收

#### 阶段 A：所有权、生命周期与测试基线

实施内容：

1. 定义 ME 不变量和 `ObjectBatch` 语义；
2. 统一批量 LIFO 顺序；
3. 区分 drain、destroy 与 reinitialize；
4. 修复 TransferCache 测试被 Reset 绕过的问题；
5. 增加守恒快照和 OOM/failure injection；
6. 将 bootstrap OOM 路径改为不分配日志策略；
7. 建立 Central 专项 benchmark。

退出条件：

- TransferCache normal 与 disabled 两种模式均被真实覆盖；
- 所有 batch 顺序和对象守恒测试通过；
- reset/shutdown 不产生 UAF、泄漏或半初始化状态；
- ASan/UBSan/TSan 通过；
- 现有 3.8 ns、26.0 ns 和 16 线程护栏无不可接受退化。

风险类型：正确性、内存、并发、性能。

#### 阶段 B：批量 bitmap、预取与预算

实施内容：

1. pointer-stack bulk push/pop；
2. Span `AllocBatch/FreeBatch`；
3. last-span 或定长分组实验；
4. 自适应预取和发布失败统计；
5. process/node Central byte budget；
6. 有界 empty Span retention 实验；
7. 内存压力 drain。

退出条件：

- bitmap words/object 和锁持有周期显著下降；
- duplicate/free 边界不会破坏 use_count；
- 预取 publish rollback 有界且守恒；
- Central cached/retained bytes 始终受预算约束；
- churn workload 的 PageCache 往返或尾延迟获得可量化改善；
- fast path 和 RSS 护栏通过。

风险类型：正确性、内存、性能。

#### 阶段 C：Size-class shard 与 refill single-flight

实施内容：

1. 将 Bucket 拆为独立缓存行的 Transfer 与 Span shard 状态；
2. 热点 size class 启用可配置 shard；
3. 建立稳定 Frontend route 和 Span central-owner 标识；
4. 实现单 shard、无跨锁的 cross-thread release；
5. 增加 refill single-flight/backoff；
6. A/B 实验批次描述符 TransferCache。

退出条件：

- 同 class 高竞争下 lock wait/cache-to-cache 显著下降；
- 一个 active Span 永远只被一个 shard 管理；
- cross-shard free 无 ABA、UAF 或对象漂移；
- PageCache 过度 refill 和低占用 Span 数得到控制；
- descriptor 模式只有综合收益明确时保留；
- 单线程和冷 class 不因分片 metadata 显著退化。

风险类型：并发、正确性、内存、性能。

#### 阶段 D：NUMA-local Middle-end 与生产灰度

实施内容：

1. 每 NUMA node 建立有界 CentralCache 组；
2. 与 PageCache region/node ownership 对齐；
3. owner-node release 和 bounded remote batch；
4. node 级预算、压力回收和统计；
5. CPU/node hotplug、拓扑不可用与单 node 回退；
6. 在 aethermind 固定 worker 和真实 cross-thread trace 灰度。

退出条件：

- 本地分配的物理页与 Central route 一致；
- remote release 不丢对象且队列受预算控制；
- 跨 socket remote access、吞吐或尾延迟获得明确收益；
- tokens/s、请求 p99、RSS 三项综合不劣于单 node 基线；
- 支持运行时或构建期开关快速回退单 Central 模式。

风险类型：并发、内存、性能、兼容性。

## 8. Backend、PageCache 与大对象管理

Backend 是 ammalloc 中管理虚拟地址区间、页级空闲空间和 OS 映射的最后一层。它既为 Middle-end 提供承载小对象的 run，也直接承载超过 Frontend 上限的大对象。Backend 的首要目标是保持地址范围所有权和 PageMap 一致；在此基础上，再优化分片扩展性、外部碎片、系统调用频率、RSS 和 hugepage 利用率。

本节与第 5 节、第 9 节的边界如下：第 5 节定义 PageMap 发布、Span 生命周期和 descriptor 延迟回收协议；本节定义 PageCache 的数据结构、分配/释放事务、大对象路径和 PageAllocator 接口；第 9 节进一步定义 dirty/purged decay、RSS 压力和后台回收策略。三节必须共享同一 Span 状态机和统计口径。

### 8.1 职责、边界与性能目标

#### 8.1.1 Backend 的职责

Backend 应负责：

- 管理 allocator 所拥有的虚拟地址 region、extent 和 run；
- 为 Middle-end 按页数提供 small-object Span；
- 为大对象提供 page-aligned 或显式 over-aligned 地址范围；
- 对 free extent 执行 exact hit、fit selection、split 和 owner-local coalesce；
- 维护 extent 的 committed、purged、retained、direct-mapped 等状态；
- 在 PageMap 中原子发布和撤销地址范围到 Span descriptor 的映射；
- 通过 PageAllocator 执行 mmap、munmap、madvise、prefault 和 hugepage hint；
- 在 NUMA 模式下保持 region、PageCache shard 与物理放置策略一致；
- 在 OOM、内存压力、shutdown 和 fork 时提供可预测的降级与清理行为；
- 提供可验证的页、extent、VMA、碎片和系统调用统计。

Backend 不应负责：

- 普通小对象的逐对象分配和 bitmap 管理；
- Frontend 或 TransferCache 的对象缓存策略；
- 在 PageCache shard 锁内执行不可控时长的系统调用；
- 使用 `std::map`、`std::vector` 等堆分配容器维护 extent；
- 将“2 MiB 对齐”或 `MADV_HUGEPAGE` 成功等同于物理 hugepage 已建立；
- 将 device memory、CUDA pinned memory 或其他非普通 CPU heap 资源混入 PageCache；
- 通过随机 shard 哈希破坏相邻地址的统一所有权。

#### 8.1.2 分层结构

```text
Middle-end small-object Span request
Large-object allocation request
                 |
                 v
        Backend routing policy
                 |
      +----------+-----------+
      |                      |
      v                      v
SmallRunCache          LargeExtentSet
exact page buckets     size + address index
      |                      |
      +----------+-----------+
                 |
                 v
        Region / owner shard
                 |
       retained/purged/direct
                 |
                 v
          PageAllocator
      mmap/munmap/madvise
```

`SmallRunCache`、`LargeExtentSet` 和 `DirectMapped` 是不同策略层，不要求第一阶段立即成为三个独立 C++ 类型，但状态、指标和路由语义必须先区分。

#### 8.1.3 性能与内存目标

| 指标 | 目标方向 | 说明 |
|---|---|---|
| Exact bucket hit | O(1) 且无 OS 调用 | 仅操作 owner shard 本地状态 |
| Split candidate lookup | O(1) 或 O(log N)，有界 | 禁止锁内扫描全部 extent |
| Coalesce | 通过地址索引或 PageMap O(log N)/O(1) 定位邻居 | 不跨 owner region |
| Shard 扩展性 | 后端 miss/churn 下随 shard 数改善 | 普通 Frontend fast path 不应受影响 |
| OS 调用锁占用 | shard 锁内为零 | mmap/munmap/madvise 均在锁外 |
| PageMap 更新 | 与修改的页/leaf 数成正比且可测量 | 大 extent 不得形成不可见长尾 |
| 外部碎片 | 按 region、size tier 可观测并受策略控制 | 不能只报告总 free bytes |
| RSS/VA 放大 | 分别预算 resident 与 retained VA | 保留 VA 不等于保留物理页 |
| 大对象尾延迟 | 分离 cache hit、split、mmap 和 purge | 报告 p99/p999，而非只有平均值 |
| Hugepage 效率 | 以实际 backing 和 breakage 评估 | 对齐/hint 只能作为过程指标 |

Backend 优化必须继续满足整体护栏：单线程 fast path 约 3.8 ns、随机大小约 26.0 ns、16 线程 64B 压力场景约 8.9 us 且吞吐 100+ GiB/s。Backend 通常位于慢路径，若这些基线退化，优先排查 PageMap 读路径、Span 布局、公共路由分支和新增共享统计。

### 8.2 Backend 核心不变量

以下不变量必须在 Release 构建中成立：

| 编号 | 不变量 | 目的 |
|---|---|---|
| BE-1 | 每个受管虚拟页在任一时刻最多属于一个逻辑 Span/Extent | 防止重叠分配 |
| BE-2 | 每个 active/free extent 恰好属于一个 region 和一个 PageCache owner shard | 保证唯一锁域 |
| BE-3 | `owner_shard_id` 在第一次 PageMap 发布前确定，descriptor epoch 内不可改变 | release 可稳定路由 |
| BE-4 | 一个 free extent 同时只在一种 free 数据结构和一个状态集合中 | 防止重复分配 |
| BE-5 | bucket 或 size index 中记录的大小与 descriptor 地址范围完全一致 | 保持 fit 正确性 |
| BE-6 | address index 中相邻节点的地址范围不重叠且严格有序 | 支持可靠 coalesce |
| BE-7 | split 后 allocated 与 remainder 的并集严格等于原 extent | 防止页遗失或重叠 |
| BE-8 | coalesce 后结果范围严格等于参与 extent 的并集 | 保证地址守恒 |
| BE-9 | 只有逻辑空闲且不再被 Frontend/Middle-end/用户引用的 Span 才能进入 PageCache free state | 防止 UAF |
| BE-10 | unmap 前先撤销对应 PageMap 映射；purge 保留 VA 时映射和状态必须一致 | 防止 stale lookup |
| BE-11 | PageMap reader 可能观察到的 descriptor 不得立即复用 | 防止 ABA/UAF |
| BE-12 | split/coalesce/PageMap 发布是失败原子的 | OOM 不破坏旧所有权 |
| BE-13 | mmap、munmap、madvise、mbind、prefault 不在 PageCache shard 锁内执行 | 控制锁尾延迟 |
| BE-14 | 不同时持有两个普通 PageCache shard 锁完成一次分配或合并 | 避免跨 shard 死锁 |
| BE-15 | coalesce 不跨 region、owner shard、page policy 或不兼容 backing 类型 | 保持释放协议正确 |
| BE-16 | DirectMapped 释放使用原始 mapping base/length，而非仅使用用户可见地址和 usable size | 正确 munmap |
| BE-17 | requested、usable、mapped、resident、retained 等字节口径不重复计算 | 保证指标可信 |
| BE-18 | extent index、metadata arena 和 region registry 不依赖被 ammalloc 拦截的堆分配 | 避免递归 |
| BE-19 | 所有 size/page/alignment 加法、乘法和向上取整先检查溢出 | 防止小映射承载超大请求 |
| BE-20 | 2 MiB alignment、THP eligibility、实际 THP backing 和 hugetlb backing 分别记录 | 避免错误优化结论 |
| BE-21 | committed/purged 状态转换不改变逻辑地址所有权 | 保证复用与 coalesce 稳定 |
| BE-22 | destructive reset 只在所有 allocator 用户、Scavenger 和 PageMap reader 静止后执行 | 防止并发生命周期破坏 |

建议增加 Backend 守恒式：

```text
region_reserved_bytes
  = active_small_span_bytes
  + active_large_bytes
  + free_committed_bytes
  + free_purged_bytes
  + metadata_or_guard_bytes
  + unassigned_region_bytes
```

DirectMapped、显式 hugetlb 和 region 外特殊映射应单独计入 `direct_mapped_bytes`，不能重复包含在 region 总量中。

### 8.3 当前实现基线与差距

#### 8.3.1 已有基础

当前实现已经具备：

- 固定容量 `PageCacheShard[4]` 和每 shard 独立 mutex、页数桶、Span ObjectPool；
- 生产默认启用一个 shard，release 通过 `owner_shard_id` 回到 owner；
- 1～128 页的精确桶、向上寻找较大桶及 head split；
- 左右相邻 free Span 的 owner-local coalesce；
- 超过 128 页的 Span 直接通过 PageAllocator 映射并在 free 时直接解除映射；
- PageMap 四层 radix tree 的无锁读路径；
- Span 和 RadixNode 使用 PageAllocator-backed ObjectPool，未使用堆分配 STL 容器；
- 2 MiB 对齐、`MADV_HUGEPAGE` hint、普通页 fallback 和固定容量 2 MiB VMA cache；
- PageAllocator 溢出保护、mmap/munmap/madvise 失败计数；
- PageCache exact hit、split、merge、PageMap 和同桶争用基准；
- Scavenger 在锁下摘除 Span、锁外 `madvise`、再重新入桶的基本模式。

这些能力可以作为单 shard 正确性基线，但不能直接推导出真正多 shard、NUMA、hugepage-aware 或完整 large extent 管理已经成立。

#### 8.3.2 关键差距

当前主要差距包括：

- `SelectShardForAlloc()` 固定返回 0，生产没有实际分片；
- `AllocSpanLocked()` 将 OS refill 和 oversized Span 的 owner 先写成 0，并在 PageMap 中发布；外层返回后才覆盖 owner，直接开启非零 shard 会产生 owner、free-list 和 PageMap 发布时序不一致；
- test-only `SetActiveShardCountForTest()` 没有被现有单元测试调用，多 shard 路径缺少验证；
- 每 shard 都可能并发安装 PageMap radix node，但没有独立全局 writer 协议；
- mmap、munmap 以及 metadata chunk 的 SystemAlloc 发生在 shard mutex 临界区；
- 所有 1～128 页 bucket miss 都线性扫描到 128，当前上限尚小，但未来扩大阈值会变为锁内线性成本；
- 超过 128 页全部 direct map，缺少 LargeExtentSet，导致中大型对象反复 mmap/munmap；
- 128 页上限同时承担“精确桶上限”“coalesce 上限”和“OS refill 大小”，策略耦合；
- large allocation 只保存页数，没有 requested size、alignment、mapping base/user offset、arena/node 或 page policy；
- 大对象 PageMap 发布逐页执行 atomic store，映射规模增大时延迟与 metadata 成本线性增长；
- 当前 ObjectPool `Delete(span)` 会立即复用 descriptor，不能满足第 5 节规定的无锁 reader 生命周期；
- 全局 2 MiB cache 缓存的是 `MADV_DONTNEED` 后保留的 VMA，不代表物理 hugepage 仍存在；
- 所有达到 1 MiB 的请求进入 huge alignment 路径，但阈值与收益尚未通过大对象专项基准证明；
- `MADV_HUGEPAGE` 仅是 THP hint，当前统计却容易被理解为 hugepage 实际成功；
- 没有 `MAP_HUGETLB`/memfd-hugetlb 等显式 hugepage 模式及其严格失败语义；
- PageAllocator 失败路径仍使用可能分配内存的 `spdlog`，不满足完整 malloc interposition 的自举契约；
- `ResetLocked()` 在每个 shard 内调用全局 `PageMap::Reset()`，未来多 shard reset 需要统一事务而不是逐 shard 重置全局索引；
- PageCache 测试依赖单线程地址连续性和内部桶状态，尚未覆盖 region、owner、OS unlock/relock 竞争及故障回滚。

### 8.4 分配类型与路由矩阵

#### 8.4.1 路由输入

Backend 路由不能只读取页数，还应考虑：

- requested size；
- usable size；
- alignment；
- small-object Span 请求还是用户大对象请求；
- NUMA node/arena；
- hot/cold、short/long lifetime hint；
- 是否允许 retained VA；
- THP/hugetlb policy；
- guard page 或 hardened mode；
- 当前内存压力和 shard/region budget。

普通 `am_malloc` 可以只使用默认策略；扩展 API 再显式传入 node、alignment 和 lifetime，避免污染标准 ABI。

#### 8.4.2 建议路由层级

| 类型 | 典型来源 | 建议管理结构 | 释放策略 |
|---|---|---|---|
| Small run | CentralCache 请求若干页 | SmallRunCache | owner-local coalesce，按状态保留或 purge |
| Medium/large cached extent | 大于 Frontend 上限但适合复用 | LargeExtentSet | 地址合并、size best-fit、decay |
| Direct mapped | 极大、特殊 alignment、guarded 或低复用请求 | DirectMapped registry + PageMap | clear mapping 后直接 unmap 或保留专用 extent |
| Hugepage filler run | 小于 2 MiB 且需要 THP 聚合 | HugepageFiller | 回到所属 hugepage，按 breakage 决定 purge |
| Huge region extent | 多个 2 MiB 单元的大对象/模型内存 | HugeRegion | hugepage-aware split/coalesce |
| Explicit hugetlb | 用户显式要求且系统配置允许 | 独立 hugetlb arena | 严格对应 hugetlb release，不与匿名页混合 |

#### 8.4.3 阈值解耦

至少拆分以下配置：

- `small_run_max_pages`：精确桶上限；
- `region_refill_pages`：一次 OS/region 补货规模；
- `large_extent_cache_max_bytes`：LargeExtentSet 最大可缓存 extent；
- `direct_map_threshold`：超过后优先直接映射；
- `retain_max_bytes`：可保留 VA/extent 上限；
- `hugepage_filler_max_pages`：允许进入 filler 的 run 上限；
- `hugetlb_min_bytes`：显式大页适用阈值。

这些阈值不能继续全部隐含绑定到 `MAX_PAGE_NUM=128`。

#### 8.4.4 溢出与零请求

路由前统一使用 checked arithmetic：

1. 处理 zero-size 的公共 ABI 语义；
2. 检查 `size + alignment - 1`；
3. 检查 guard/header/offset 加法；
4. 检查 byte-to-page 向上取整；
5. 检查 page count 能否表示在 descriptor 字段中；
6. 检查 PageMap 地址覆盖是否超出支持的 VA bits。

任何失败都必须在调用 mmap 或修改缓存前返回。

### 8.5 Page、Extent 与 Region 状态模型

#### 8.5.1 状态定义

建议统一使用以下逻辑状态：

```text
kFresh
kInUseSmall
kInUseLargeCached
kInUseDirect
kFreeCommitted
kFreePurged
kRetained
kSplitOrCoalesce
kDetachedForPurge
kDetachedForUnmap
kRetiredDescriptor
```

状态可以编码在 Span flags 与冷 side metadata 中，不要求全部放入 64B Span；但所有实现和测试必须按同一状态机验证。

#### 8.5.2 虚拟地址与物理页分离

必须区分：

- **Mapped/Reserved**：虚拟地址范围仍存在；
- **Committed/Resident candidate**：匿名映射可按需提供物理页；
- **Purged**：已 `MADV_DONTNEED`/等价处理，VA 保留，物理页可被内核回收；
- **Retained**：allocator 保留该 VA/extent 用于未来复用；
- **Unmapped**：VA 已归还内核；
- **Huge-backed**：经系统证据确认由 THP 或 hugetlb backing，而不是根据对齐推断。

匿名 mmap 在 Linux 上通常采用按需分配；因此“mmap 成功”不等于全部页 resident，“committed”也不能直接等同 RSS。

#### 8.5.3 主要状态转换

```text
OS/region acquire
  -> kFresh
  -> publish free extent
  -> kFreeCommitted
       -> allocate -> kInUseSmall / kInUseLargeCached
       -> purge    -> kDetachedForPurge -> kFreePurged
       -> unmap    -> kDetachedForUnmap -> kRetiredDescriptor

kFreePurged
  -> allocate/fault-on-write -> active
  -> retain                  -> kRetained
  -> unmap                   -> retired

kInUseDirect
  -> clear PageMap
  -> unmap mapping
  -> retire descriptor
```

所有 split/coalesce 通过暂态 `kSplitOrCoalesce` 完成，不能让中间结果进入可分配索引。

#### 8.5.4 Backing policy 兼容性

只有下列属性兼容时才能合并：

- 相同 region 和 owner shard；
- 相同匿名/hugetlb/file-backed 类型；
- guard page 边界允许合并；
- NUMA policy 相容；
- committed/purged 状态存在明确定义的合并结果；
- encryption/pinning/device 等特殊属性一致。

不兼容 extent 即使虚拟地址连续也必须保持分离。

### 8.6 Region-based PageCache 分片

#### 8.6.1 为什么需要 region ownership

仅把每次申请按 CPU 或线程 hash 到不同 shard，会让相邻 OS 映射随机归属不同 shard。结果是：

- 释放时无法跨 shard 合并；
- address space 被切成大量小岛；
- PageMap writer 仍全局共享；
- 线程迁移导致同一工作集漂移；
- 多锁 coalesce 设计复杂且容易死锁。

正确模型是先确定 region owner，再从该 region 内完成所有 split、allocate、free 和 coalesce。

#### 8.6.2 Region 描述

每个 Region 至少包含：

- base address 和 reserved size；
- region id、owner shard id、NUMA node id；
- backing/page policy；
- free committed/purged/retained bytes；
- SmallRunCache 和 LargeExtentSet 入口；
- region generation/closing 状态；
- PageMap/region registry 发布状态；
- decay/scavenge cursor。

Region metadata 由固定上限数组或 metadata arena 管理，不使用堆容器。

#### 8.6.3 Region 获取方式

候选方式：

1. **独立 mmap region**：一次映射较大匿名 VA，内部切分；实现简单；
2. **PROT_NONE reserve + 按需映射/提交**：更清晰地区分 VA reservation 与物理使用，但 VMA 和 commit 协议更复杂；
3. **固定地址 arena**：通过 `MAP_FIXED_NOREPLACE` 等机制获取预定区间，适用于高级 arena，但需要严格兼容性和失败回退；
4. **OS extent 直接归属**：不预留大 region，但每个新 mapping 整体归给一个 shard；短期最容易落地。

第一阶段建议采用“OS extent 整体归属 shard”，先建立 owner 正确性；中期再评估大 VA region reservation。

#### 8.6.4 Region 边界

- split 不改变 region id；
- coalesce 不跨 region；
- region 只有在全部 extent 空闲且无 PageMap reader 生命周期风险时才能整体 unmap；
- region closing 后禁止新分配；
- region metadata 只有在 PageMap leaf 撤销并满足第 5 节 retire 协议后才能复用；
- region 大小应为 hugepage 和 SmallRun refill 单位的公倍数，但不得盲目预留过量 VA。

### 8.7 Shard 选择与 owner 发布协议

#### 8.7.1 当前多 shard 阻塞点

当前内部 refill/split 路径把 `owner_shard_id` 写为 0并发布 PageMap，外层 `AllocSpan()` 返回前才把对象改为选定 shard。对于 shard 1～3，这会导致 remainder 留在非零 shard 的 list 中却声明 owner 0，后续 release/coalesce 进入错误锁域。因此在修复发布协议前，生产必须继续固定 shard 0。

#### 8.7.2 正确发布顺序

```text
select owner shard/region
  -> prepare descriptor with owner id
  -> initialize address, page count, backing policy, state
  -> acquire owner shard + PageMap writer protocol
  -> validate target range/index state
  -> publish PageMap range
  -> insert free remainder or return active Span
  -> release locks
```

owner id 必须作为 `AllocSpanLocked(owner_context, page_num)` 的输入，而不是在函数返回后修补。

#### 8.7.3 Owner 继承

- exact hit 保持原 owner；
- split 的 allocated 和 remainder 都继承 region owner；
- coalesce 结果保持共同 owner；
- direct mapping 在第一次发布前确定 owner/arena；
- NUMA fallback 到其他 node 时记录实际 owner，而非保留请求 node；
- active Span 不允许通过简单字段写入迁移 shard。

#### 8.7.4 路由策略

推荐优先级：

1. 调用方显式 arena/node；
2. Frontend/Middle-end 稳定 route；
3. 当前 CPU 所属 node/shard；
4. shard 压力/容量有限回退；
5. shard 0 兼容模式。

选择结果在一次 allocation transaction 内固定。不能在 split 或 OS refill 返回后因线程迁移重新选择 shard。

#### 8.7.5 验证

每次 release 至少校验：

- owner id 在 active range 内；
- descriptor 确实由该 shard metadata arena 创建；
- region id 与 shard 一致；
- PageMap 对首尾及采样页指向相同 descriptor/generation；
- free-list membership 为空。

### 8.8 SmallRunCache 精确桶

#### 8.8.1 适用范围

SmallRunCache 服务 CentralCache 请求的较小连续页区间，也可服务高复用的中小型直接对象 extent。建议 `small_run_max_pages` 独立于 OS refill 大小和 DirectMapped 阈值。

#### 8.8.2 数据结构

基础结构仍可使用：

```text
SpanList committed_buckets[1..N]
SpanList purged_buckets[1..N]      // 可选
non_empty_bitmap
```

`non_empty_bitmap` 在 shard 锁内维护，不需要 atomic。若 N ≤ 128，可用两个 64-bit word；查找下一个非空 bucket 使用 mask + `countr_zero`，避免未来阈值扩大后线性扫描。

#### 8.8.3 Exact hit

exact hit 事务只应：

1. 从 bucket 头摘除一个 Span；
2. 清除对应 non-empty bit（若 bucket 变空）；
3. 验证 page count、owner、region、状态；
4. 转为 active 状态；
5. 保持 PageMap 指向稳定 descriptor；
6. 返回调用方。

不执行 OS 调用，不分配 descriptor，不重写整个 PageMap range。

#### 8.8.4 Bucket 内顺序

候选顺序：

- recently freed LIFO：局部性高；
- oldest first：更容易把冷页用于 purge；
- committed 优先、purged 次之：减少首次触页；
- 根据 hot/cold hint 分两条链。

短期保留 LIFO committed bucket，并让 Scavenger 从冷端扫描；若 SpanList 只支持单端访问，可增加 cold cursor，而不是每次全表遍历。

#### 8.8.5 Purged Span 复用

purged Span 仍拥有 VA 和 PageMap 映射。重新分配时通常不需要显式“commit”，首次写会重新建立匿名页；但状态和统计必须从 purged 转 active，不能继续计入可回收 RSS。

### 8.9 LargeExtentSet 双索引

#### 8.9.1 设计目的

对于超过精确桶上限、但具有明显复用价值的中大型 extent，全部 direct mmap 会增加：

- mmap/munmap 次数和内核锁竞争；
- VMA 创建/销毁成本；
- PageMap node 反复创建；
- 对齐 over-map/trim 成本；
- 大对象 p99/p999 延迟。

LargeExtentSet 应同时支持按大小寻找 fit 和按地址寻找邻居。

#### 8.9.2 双索引语义

- **Size index**：键为 `(page_count, address)`，用于 exact/best-fit；
- **Address index**：键为 `start_page_id`，用于前驱/后继和 overlap 检查；
- 同一个 free extent 同时位于两个索引；
- active、detached 或 direct extent 不在 free size index；
- 插入/删除两个索引必须在同一 shard transaction 内完成。

#### 8.9.3 无递归实现

禁止使用 `std::map`/`std::multimap`。候选实现：

- intrusive red-black tree；
- intrusive treap（priority 由稳定无分配 PRNG/hash 生成）；
- 分级 size bins + intrusive address tree；
- radix/segment tree，仅在 region 地址结构适合时采用。

Extent descriptor 需要两套 tree hooks。若 64B Span 放不下，应使用 PageCache-only side metadata 或独立 `ExtentDescriptor`，不能无评估地膨胀所有小对象 Span。

#### 8.9.4 Fit 查找

默认建议：

1. exact size 优先；
2. size tree lower_bound 找最小足够 extent；
3. 在有限候选窗口内按 committed、NUMA、alignment 和碎片代价排序；
4. 超出搜索预算时接受第一个合格 best-fit；
5. 没有候选再进入 OS/region refill。

禁止为追求理论最优在 shard 锁内扫描所有 free extent。

#### 8.9.5 Index 守恒

诊断模式验证：

- 两棵树节点集合完全一致；
- address tree 无重叠；
- size key 与 descriptor page count 一致；
- 所有节点 owner/region 相同；
- 索引节点不在 SmallRun bucket；
- free bytes 等于索引逐项求和。

### 8.10 DirectMapped 大对象路径

#### 8.10.1 适用条件

以下请求优先 DirectMapped：

- 大于 `direct_map_threshold`；
- alignment 明显大于普通 region 单元；
- guarded/hardened 分配；
- 显式 hugetlb、file-backed 或特殊 page policy；
- workload 表明复用概率很低；
- LargeExtentSet 预算不足且内存压力较高。

#### 8.10.2 分配事务

```text
checked request geometry
  -> allocate descriptor outside shard lock
  -> mmap/align/trim outside shard lock
  -> initialize mapping_base, mapping_size, user_ptr, usable_size
  -> acquire PageMap writer protocol
  -> publish address range
  -> mark kInUseDirect
  -> return user_ptr
```

如果 PageMap node/descriptor 准备失败，必须 unmap 完整原始 mapping，不能留下无法释放的 VMA。

#### 8.10.3 释放事务

```text
PageMap lookup + pointer validation
  -> mark/detach direct descriptor
  -> clear entire mapping range from PageMap
  -> release writer/shard locks
  -> munmap(mapping_base, mapping_size)
  -> retire descriptor
```

如果 munmap 失败，不能重新把已经交还用户语义的对象发布为 active。需要记录 leaked-mapping/quarantine 状态并提供 fail-fast 或诊断策略。

#### 8.10.4 PageMap 成本

当前 `SetSpan` 为每个页 leaf 执行 store，大型映射的发布/清除成本为 O(page count)。改进方向包括：

- 按 leaf coverage 批量 fill/clear；
- 为同一 Span 的完整 radix leaf 使用专用 range marker；
- 缓存已存在的 radix path；
- 评估大对象 header/side registry 与 PageMap 的组合，但不得让普通 free 变为加锁查树；
- 继续保证 `GetSpan` 固定深度和无锁。

任何压缩方案都必须与第 5 节 reader 生命周期和多 writer 协议共同设计。

### 8.11 大对象 metadata、usable size 与 alignment

#### 8.11.1 必要字段

大对象至少需要保存：

- requested size；
- usable size；
- user pointer 或 user offset；
- original mapping base 和 mapping length；
- logical extent start/page count；
- requested/effective alignment；
- owner shard、region、NUMA node；
- page policy、guard flags、direct/retained 类型；
- lifecycle generation；
- allocation tag/profile sample（可选）。

这些字段不应全部塞进热 Span。推荐将公共身份字段保留在 64B Span，将 large-only 冷字段放进 allocator-owned `LargeExtentMetadata`。

#### 8.11.2 普通大对象

普通 `malloc` 大对象至少返回 `max_align_t` 对齐；当前 page alignment 已满足。usable size 可以是页向上取整后的长度，但 `malloc_usable_size` 必须返回用户可用范围，而非包含 guard/header/trim 区域的 mapping length。

#### 8.11.3 Over-aligned 请求

对 `aligned_alloc/posix_memalign`：

1. 验证 alignment 是合法 2 的幂和 ABI 要求；
2. checked 计算 `size + alignment - 1`；
3. 优先从对齐合适的 region extent split；
4. fallback 才执行 over-map + trim；
5. 保存 original mapping geometry；
6. 将所有可接受 free 地址页正确映射到 descriptor；
7. free 时验证传入的是原始 user pointer，而非任意 interior pointer。

#### 8.11.4 Realloc

大对象 realloc 的演进顺序：

- usable size 足够时原地返回；
- shrink 时可选择保留尾部、拆分回 LargeExtentSet 或直接 unmap 尾部；
- 相邻 free extent 足够时在 owner shard 内原地扩展；
- DirectMapped 可在 Linux 上实验 `mremap`，但必须处理地址移动后的 PageMap 原子切换；
- 否则 allocate-copy-free；
- 失败保持原对象有效。

#### 8.11.5 零填充与安全

新匿名 mmap 初始为零，但缓存/retained extent 复用不保证满足 calloc 语义。calloc 必须显式依赖状态：purged/新映射可利用内核零页语义，committed dirty extent 必须清零。不能仅以“来自 PageAllocator”推断内容为零。

### 8.12 Exact-hit 分配事务

#### 8.12.1 前置条件

- request geometry 已校验；
- owner shard/region 已确定；
- bucket/index key 合法；
- 调用线程没有持有 CentralCache 锁；
- PageMap reader contract 已建立。

#### 8.12.2 事务步骤

1. 获取 owner shard 锁；
2. 从 committed exact bucket 优先摘取；
3. 若无 committed，再从 purged exact bucket摘取；
4. 校验 state、page count、region、owner 和 index membership；
5. 标记暂态/active，更新索引位图和字节计数；
6. 若 descriptor/range 不变，不重写 PageMap；
7. 释放 shard 锁；
8. 对 purged extent 仅在策略需要时执行预取/预触页；
9. 初始化 small-object Span 或 large metadata 后再向上层发布。

#### 8.12.3 失败语义

exact hit 不应因额外 metadata allocation 失败而丢失 Span。所需 large side metadata 应在摘取前准备，或采用可回滚的 reserved metadata slot。失败时 extent 回到原状态和原索引位置。

### 8.13 Split 分配事务

#### 8.13.1 Candidate 选择

- SmallRun 使用 non-empty bucket bitmap 找最近较大 bucket；
- LargeExtentSet 使用 size lower_bound；
- alignment 可能产生 prefix/suffix 两个 remainder；
- candidate backing policy 必须兼容请求；
- 分裂后的碎片必须达到最小可管理粒度。

#### 8.13.2 Metadata 预留

在从 free index 摘除 candidate 前准备好最坏情况下所需 descriptor：

- 普通 head/tail split：allocated + 一个 remainder；
- alignment split：prefix + allocated + suffix；
- guard page：可能再增加保护区 metadata；
- 若采用新 descriptor 发布协议，还需为旧 descriptor retire 准备节点。

metadata OOM 时原 candidate 保持不变。

#### 8.13.3 提交步骤

1. 获取 owner shard 与 PageMap writer 协议；
2. 重新验证 candidate 仍在预期索引；
3. 从 size/address index 或 bucket 摘除；
4. 计算所有子区间，验证无溢出、无空洞、无重叠；
5. 初始化子 descriptor 的 owner/region/state；
6. 批量切换 PageMap range；
7. 将 remainder 插入正确 bucket/双索引；
8. 将 allocated extent 标记 active；
9. retire 不再使用的旧 descriptor；
10. 更新 committed/purged/active/fragmentation 计数；
11. 释放锁并向上层发布。

#### 8.13.4 碎片抑制

- prefix/suffix 小于最小 extent 时并入 usable size或换候选；
- 对大 alignment 计算 `waste_bytes`；
- 避免不断从同一 hugepage 中间切出导致 breakage；
- fit score 同时考虑 remainder 数量和大小；
- 不为减少几个页浪费而引入锁内无界候选搜索。

### 8.14 OS/Region refill 事务

#### 8.14.1 当前问题

当前 `AllocSpanLocked()` 在持有 shard mutex 时执行 SystemAlloc；mmap 重试、VMA 操作、THP hint 及 metadata chunk 分配都会扩大同 shard 等待时间。多线程 miss 时还可能形成串行内核调用或重复补货。

#### 8.14.2 Prepare 阶段

在 shard 锁内：

- 再次确认没有合格 free extent；
- 检查 region/shard budget；
- 尝试取得 refill single-flight ownership；
- 记录 request class、期望 pages 和 generation；
- 预留可用 descriptor/region slot；
- 随即释放锁。

#### 8.14.3 OS 阶段

锁外执行：

- mmap/reserve/commit；
- alignment trimming；
- NUMA policy 或 mbind；
- hugepage hint；
- 可选 prefault；
- 初始化尚未共享的 descriptor。

该阶段产生的 mapping 由当前线程独占，失败可以直接清理，不接触共享索引。

#### 8.14.4 Publish 阶段

重新获取 owner shard和 PageMap writer 协议后：

- 验证 shard/region 未 closing；
- 验证 single-flight generation；
- 重新检查是否已有其他可用 extent；
- 将新 mapping 作为完整 free extent 发布；
- 设置 owner 后再写 PageMap；
- 插入 bucket/双索引；
- 清除 refill 状态；
- 重试 exact/split 事务。

如果发布时已不需要该 mapping，可将其作为预算允许的 free extent 缓存，或在锁外 unmap；不能在持锁状态直接释放。

#### 8.14.5 Refill 大小

refill 大小应由以下因素决定：

- request pages；
- SmallRun 常用 page count；
- region/large extent 最小单元；
- hugepage 边界；
- 最近 split remainder 利用率；
- shard 当前 free bytes；
- 内存压力与 NUMA node budget。

固定补 128 页可作为初始策略，但必须与 exact bucket 上限解耦。

### 8.15 Release 与 Coalesce 事务

#### 8.15.1 接收前验证

PageCache 接收 Span/Extent 前验证：

- descriptor 非空且 active；
- owner shard/region 合法；
- 小对象 Span `use_count == 0` 且 bitmap 全 free；
- large/direct pointer 是 allocation base；
- descriptor 不在任何 free index；
- PageMap 首尾及必要采样 leaf 与 descriptor/generation 一致；
- mapping geometry 和页数没有溢出；
- 非 guarded 页范围允许合并。

#### 8.15.2 邻居发现

- SmallRun 可通过 PageMap 查左末页和右首页面；
- LargeExtentSet 优先通过 address tree predecessor/successor；
- 两种方式结果应在诊断构建交叉校验；
- 邻居必须 free、同 owner、同 region、状态兼容且不处于 detached/scavenge；
- 不跨 DirectMapped、guard 或 hugetlb 边界。

#### 8.15.3 Coalesce 提交

1. 获取 owner shard与 PageMap writer 协议；
2. 将释放 extent 标记 `kSplitOrCoalesce`；
3. 从索引摘除合格左/右邻居；
4. 准备 survivor/new descriptor；
5. 计算合并范围并再次验证不溢出；
6. 统一 committed/purged policy；
7. 将结果 range 发布到 survivor/new descriptor；
8. 将旧 descriptor 放入 retire list而非立即复用；
9. 插入 SmallRun bucket 或 LargeExtentSet；
10. 更新 free、active、coalesced 和碎片统计；
11. 释放锁。

#### 8.15.4 跨 size tier 合并

合并结果可能从 SmallRun 升入 LargeExtentSet。SmallRun 最大页数不应阻止地址连续空闲区继续合并；否则 128 页上限会永久制造外部碎片。tier 转换必须作为同一 shard transaction 完成。

#### 8.15.5 DirectMapped 例外

默认 DirectMapped 不与 region extent 合并，释放时直接 clear + unmap。若未来允许 direct mapping 进入 retained LargeExtentSet，必须在 allocation epoch 开始时明确其 region/backing policy，不能在 free 时临时改变所有权模型。

### 8.16 将 OS 调用移出 shard 锁

#### 8.16.1 通用 Detach 模式

```text
lock shard
  -> remove extent from allocatable index
  -> mark kDetachedForPurge/kDetachedForUnmap
  -> record generation and private work item
unlock shard
  -> perform OS syscall
lock shard
  -> validate generation/closing state
  -> publish result or retire descriptor
unlock shard
```

detached extent 不可被 allocate、split 或 coalesce，也不能仍挂在普通 free list。

#### 8.16.2 mmap

mmap 返回的是尚未共享的新范围，不需要在 OS 调用期间持 shard 锁。发布时若策略已变化，安全地缓存或 unmap 新范围。

#### 8.16.3 madvise

purge 时通常保留 PageMap leaf，但 Span state 必须让其他路径知道它 detached。成功后转 `kFreePurged`，失败则恢复 `kFreeCommitted`。两种结果都重新进入正确索引。

#### 8.16.4 munmap

unmap 前必须：

- 从 free/address/size index 移除；
- 撤销 PageMap range；
- 确认没有合法用户对象；
- 将 descriptor 置 detached/retired；
- 释放所有 allocator 锁。

munmap 失败时该 VA 可能仍映射，但逻辑上不能重新交给用户，除非能够证明完整回滚安全。推荐进入 quarantined mapping 统计并触发受控诊断。

#### 8.16.5 日志和回调

OS 错误处理不得在 shard 锁内格式化日志，也不得调用用户 OOM 回调。事件先写入固定结构/原子计数，锁外使用 bootstrap-safe 方式报告。

### 8.17 PageMap 集成边界

#### 8.17.1 规范来源

PageMap 的 reader 生命周期、writer 串行化、节点只增不减、descriptor retire 和 split/coalesce 发布顺序以第 5 节为准。本节只规定 Backend 在何时调用 range publish/clear，不建立第二套较弱协议。

#### 8.17.2 Writer 锁序

推荐：

```text
owner PageCache shard lock
  -> global PageMap writer lock
  -> radix node metadata pool lock（如仍需要）
```

PageMap writer 不反向取得 shard 锁。OS 调用不在任何上述锁内。

#### 8.17.3 Range API

建议将逐 Span 操作抽象为：

- `PrepareRange(start, pages)`：预分配可能需要的 radix node；
- `PublishRange(start, pages, span)`；
- `ReplaceRange(start, pages, expected, replacement)`；
- `ClearRange(start, pages, expected)`；
- `ValidateRangeSample(...)`；
- quiescent-only `ResetAll()`。

`expected` 能防止错误 writer 静默覆盖不属于自己的 mapping。

#### 8.17.4 Large range 优化

针对数百 MiB/GiB extent，记录：

- leaf stores 数；
- 新建 radix node 数；
- publish/clear cycles；
- PageMap metadata bytes；
- direct allocation 总耗时占比。

只有这些数据证明逐页 leaf 是瓶颈后，才引入 uniform leaf/range marker；普通 `GetSpan` 仍必须固定深度、无锁和无动态分支爆炸。

#### 8.17.5 Reset

PageMap 是全局索引，只能在所有 active shard 清空、reader 静止后统一 reset 一次。不得在逐 shard `ResetLocked()` 内重复释放全局 radix pool。

### 8.18 Span/Extent metadata arena

#### 8.18.1 当前 ObjectPool 的局限

当前 ObjectPool 解决了递归分配，但：

- `Delete()` 立即把 slot 放回 free list，不满足无锁 PageMap reader 的延迟回收；
- pool 内部 mutex 与外层 shard mutex 形成嵌套锁；
- Span 与大型 extent 冷 metadata 需求不同；
- `ReleaseMemory()` 只适合全局 quiescent reset。

#### 8.18.2 推荐分层

- `SpanArena`：每 shard 管理 64B hot Span descriptor；
- `LargeMetadataArena`：requested/alignment/mapping/guard 等冷字段；
- `RegionArena`：固定上限 region descriptor；
- `RadixNodeArena`：PageMap 节点，只增不减直到 quiescent reset；
- `RetireList`：已撤销映射但尚不可复用的 descriptor。

所有 arena 使用 PageAllocator-backed chunk 和 intrusive free/retire list。

#### 8.18.3 锁策略

如果 arena 严格 shard-local，可由 shard lock 外部保护，移除内部重复 mutex；PageMap node arena需要独立 writer 锁。若保留内部锁，必须文档化固定锁序并统计 metadata allocation 慢路径。

#### 8.18.4 Stable metadata 优先

第一阶段采用第 5 节推荐的 stable metadata：已发布 descriptor 退休后不复用，直到受控 shutdown。只有 metadata RSS 数据证明不可接受，才实现 epoch reclamation。不要为了节省少量 descriptor 立即给 free 路径增加引用计数。

#### 8.18.5 Metadata budget

记录：

- live/free/retired Span 数；
- arena mapped/used/wasted bytes；
- split/coalesce descriptor 产生率；
- large side metadata 数；
- radix node bytes；
- 每 region/shard metadata 放大。

### 8.19 无递归的 extent 索引结构

#### 8.19.1 禁止项

以下实现即使方便也禁止用于核心 Backend：

- `std::map`/`std::multimap` 的 size/address tree；
- `std::vector` 保存动态 region 或 free extent；
- `std::priority_queue` 保存 purge 候选；
- raw owning `new/delete` 创建 tree node；
- 依赖系统 malloc 的第三方容器。

#### 8.19.2 Intrusive hooks

Large extent descriptor 可包含：

```text
size_parent/left/right/color
addr_parent/left/right/color
```

或者把 hooks 放在 PageCache side metadata。Tree 不拥有 descriptor，只维护链接；descriptor ownership 属于对应 arena。

#### 8.19.3 复杂度边界

- insert/remove/lower_bound：O(log N)；
- predecessor/successor：O(log N) 或已有节点 O(1) 邻接；
- exact small bucket：O(1)；
- purge 候选：增量 cursor 或分层时间桶；
- diagnostics 全遍历：只在停机/采样路径。

不得使用线性链表加“通常 extent 不多”的假设构建大对象默认路径。

#### 8.19.4 Tree 正确性测试

使用固定 seed 随机插入、删除、split、coalesce，并与测试进程中的参考模型比较。参考模型可以在测试代码使用 STL，但 allocator 核心实现不能。

### 8.20 Fit 策略与外部碎片控制

#### 8.20.1 Fit 候选

| 策略 | 优点 | 风险 | 推荐用途 |
|---|---|---|---|
| Exact fit | 无 remainder、快 | 命中率有限 | SmallRun 与 LargeExtent 首选 |
| Best fit | 降低即时 remainder | 可能反复制造小碎片 | LargeExtent 默认基线 |
| First fit | 搜索简单 | 地址/大小碎片不稳定 | 有界候选回退 |
| Hugepage-aware fit | 保护完整 hugepage | 可能牺牲少量页 | 推理 workload |
| Decay-aware fit | 优先复用 committed 热页 | 可能降低最佳尺寸匹配 | RSS/延迟平衡 |

#### 8.20.2 综合评分

慢路径候选评分可考虑：

- size waste；
- prefix/suffix 数量；
- committed/purged 状态；
- hugepage breakage 增量；
- NUMA locality；
- region occupancy；
- age/decay；
- alignment waste。

只检查固定数量候选，避免锁内复杂全局优化。

#### 8.20.3 Coalesce 策略

普通 free 应积极 owner-local coalesce，因为地址索引已可快速定位邻居；但 detached、不同 backing 或不同 region 不合并。对于 hugepage filler 内部 run，先在 filler 粒度合并，只有完整 hugepage 空闲后才提升到上层 HugeRegion。

#### 8.20.4 碎片指标

至少报告：

- total free bytes；
- largest free extent；
- `1 - largest_free_extent / total_free_bytes`；
- size histogram；
- split remainder bytes/count；
- failed fit while total free sufficient；
- alignment waste；
- region stranded bytes；
- hugepage broken/partially used bytes。

#### 8.20.5 Trace 驱动调优

使用真实 aethermind 请求、模型加载、KV cache 和临时 workspace trace 离线重放，比较 fit 策略。只优化合成均匀随机大小容易得到错误阈值。

### 8.21 Retained VA、Purge 与 Recommit

#### 8.21.1 Retained 的价值

保留虚拟地址并 purge 物理页可以：

- 减少 mmap/munmap 和 VMA 变化；
- 保留 region/NUMA/对齐结构；
- 降低 PageMap 节点反复创建；
- 为 hugepage filler/region 提供稳定边界；
- 改善大对象重复申请尾延迟。

代价是 VA 放大、PageMap metadata 常驻和潜在 VMA/地址空间压力。

#### 8.21.2 三种释放层级

- **Cache committed**：VA 与物理页候选均保留，最快复用；
- **Purge retained**：`MADV_DONTNEED`，VA 保留，RSS 可下降；
- **Unmap**：撤销 PageMap 和 VA，最彻底回收。

策略由 extent size、age、pressure、region occupancy 和复用率决定。

#### 8.21.3 Recommit 语义

匿名 `MADV_DONTNEED` extent 通常可直接重新交付并在首次写时 fault；如果 API 需要确定性预触页，再在锁外执行 `MADV_WILLNEED` 或显式 touch。Recommit 统计需区分：

- logical reuse；
- minor/major faults；
- prefault bytes/time；
- THP collapse latency；
- actual resident growth。

#### 8.21.4 VA budget

为 48-bit 和 57-bit 模式分别设置：

- process retained VA soft/hard limit；
- per-node/per-region limit；
- 最大 region 数；
- PageMap metadata limit；
- unmap 优先级。

不能因为 64-bit VA 看似充足而无界保留。

#### 8.21.5 与第 9 节边界

本节定义状态和 Backend 操作；具体 dirty/muzzy decay 时间、压力信号、后台扫描预算和 RSS 控制在第 9 节统一制定。

### 8.22 Hugepage 术语与模式分离

#### 8.22.1 四个不同概念

| 概念 | 含义 | 是否保证物理 hugepage |
|---|---|---|
| 2 MiB aligned | 虚拟地址和长度满足 2 MiB 边界 | 否 |
| THP eligible | `MADV_HUGEPAGE` 或系统策略允许 collapse | 否 |
| THP backed | `/proc`、smaps 或内核计数显示实际 AnonHugePages | 是，可能随时 split |
| Explicit hugetlb | `MAP_HUGETLB`/hugetlbfs 等预留大页 | 是，资源和失败语义不同 |

代码、指标和文档必须使用准确术语。

#### 8.22.2 当前模式

当前 PageAllocator：

- 对达到 1 MiB 的映射尝试 2 MiB 对齐；
- 通过 `MADV_HUGEPAGE` 提示 THP；
- 对恰好 2 MiB 的 mapping 在 free 时 `MADV_DONTNEED` 并缓存 VMA；
- huge 路径失败时回退普通匿名页。

这属于“THP-friendly aligned anonymous mapping”，不是显式 hugepage allocator。

#### 8.22.3 建议策略枚举

```text
PagePolicy::kNormal
PagePolicy::kThpPrefer
PagePolicy::kThpAvoid
PagePolicy::kHugetlbRequire
PagePolicy::kHugetlbPrefer
```

- `kHugetlbRequire` 失败必须返回失败，不能静默降级；
- `kHugetlbPrefer` 才允许回退；
- small metadata、冷页和频繁 purge extent 可用 `kThpAvoid`；
- 默认 malloc 以 availability-first 的 normal/THP prefer 为主。

#### 8.22.4 实际 backing 观测

在线热路径不解析 `/proc/self/smaps`。通过低频诊断、`/proc` 汇总、`perf`/内核接口或抽样 `mincore` 等方式测量；对外报告“hint issued”“aligned mapping”“observed THP bytes”三个独立指标。

### 8.23 Hugepage Filler

#### 8.23.1 目标

Hugepage filler 将一个 2 MiB 区间划分为多个 small run，使活跃 run 尽量集中在少量 hugepage 内，让完全空闲 hugepage 可以整体 purge/unmap，同时避免随机 SmallRun split 破坏所有 hugepage。

#### 8.23.2 Filler metadata

每个 filler hugepage 至少记录：

- hugepage base、owner node/shard；
- 已分配/空闲/已 purge 的 4 KiB 页 bitmap；
- longest free run 或分级空闲提示；
- live run count 和 used pages；
- hugepage broken/eligible/backed 状态；
- last allocation/free epoch；
- intrusive fullness list hooks。

metadata 必须来自专用 arena，不能嵌在用户页中影响对齐或 usable capacity。

#### 8.23.3 分配策略

- 优先已有 partially used hugepage，填充空洞；
- 根据所需 run pages 选择可容纳且 remainder 最小的 filler；
- 避免把单个极小长期存活 run 分散到多个 hugepage；
- 对 hot/short-lived workload 使用不同 fullness 策略；
- filler 内部操作由 owner shard/filler lock 保护，不使用逐页 atomic CAS 网络。

#### 8.23.4 释放策略

- run free 后更新 filler bitmap；
- 相邻页在 filler 内自然合并；
- hugepage 完全空闲后提升为完整 hugepage cache/region extent；
- 若只有极少活跃页阻止回收，计入 breakage；
- 不迁移 live user objects来“整理”hugepage，除非上层 arena 明确支持 relocation。

#### 8.23.5 采用门槛

只有在实际 THP 覆盖率、TLB miss、hugepage breakage 和 aethermind 性能表明收益后启用。Filler 增加 metadata、选择成本和状态复杂度，不应作为第一阶段默认路径。

### 8.24 HugeRegion 与完整 Hugepage Cache

#### 8.24.1 HugeRegion

HugeRegion 以 2 MiB 单元管理连续地址范围，适合：

- 模型权重 CPU staging；
- KV cache 大块；
- 推理 workspace；
- 需要 NUMA locality 的大型长期 allocation；
- 多个 hugepage 的普通大对象。

它应支持完整 hugepage 的 split/coalesce，而不是退化为 4 KiB extent tree 后再猜测 huge 边界。

#### 8.24.2 完整大页缓存

缓存对象必须区分：

- 2 MiB 对齐 retained VMA；
- 当前实际 THP-backed 区间；
- explicit hugetlb page；
- purged 后仅保留 VA 的区间。

这些资源不能放进同一无类型 cache。显式 hugetlb 必须回到相同 hugetlb pool；普通匿名 VMA 可按 node/policy 分片缓存。

#### 8.24.3 当前 lock-free cache 的定位

现有固定 16-slot 双栈 cache 可继续作为“全局 2 MiB retained anonymous VMA cache”的实验基线，但需要：

- 按 NUMA node 或 owner arena 分区；
- 记录 current slots/bytes/high-water；
- 区分 hit 后重新 fault 与真正 resident reuse；
- 在 pressure/shutdown 时可确定性 drain；
- 对 ABA tag wrap、concurrent drain 和 fork 增加测试；
- 不把 cache hit 记作 hugepage backing hit。

#### 8.24.4 Hugepage breakage

至少统计：

- total huge-aligned regions；
- observed huge-backed bytes；
- full/partial/empty hugepage 数；
- partial hugepage 内 live/free pages；
- 因少量 live pages 无法整体 purge 的 stranded bytes；
- THP split/collapse 事件（可观测时）；
- filler allocation success/fallback；
- explicit hugetlb success/failure/fallback。

### 8.25 PageAllocator OS 抽象与失败语义

#### 8.25.1 从二元接口演进

当前 `SystemAlloc(page_num)`/`SystemFree(ptr, page_num)` 无法表达 retained、NUMA、alignment、page policy 和错误详情。建议内部演进为：

```text
Map(request) -> MappingResult
Unmap(mapping) -> OsResult
Purge(range, mode) -> OsResult
Advise(range, policy) -> OsResult
BindNuma(range, node, policy) -> OsResult
Prefault(range, budget) -> OsResult
```

现有接口可作为 bootstrap/兼容薄封装。

#### 8.25.2 MappingResult

返回结构至少包含：

- base、size；
- effective alignment；
- actual mapping flags/policy；
- requested NUMA 与实际 fallback；
- errno/error category；
- 是否经过 over-map/trim；
- 是否获得显式 hugetlb；
- 是否只是 THP hint；
- cleanup responsibility。

结构由值返回或调用方提供 buffer，不分配内存。

#### 8.25.3 Retry 策略

当前 ENOMEM 最多重试 3 次并固定 sleep 50 us。应重新评估：

- ENOMEM 在 cgroup/overcommit 下通常不是短暂事件；
- 每次 allocation thread sleep 会直接放大尾延迟；
- 可在首次失败后触发有界 cache purge，再重试一次；
- retry 次数和 backoff 应配置化、可观测；
- 非 ENOMEM 错误通常立即返回；
- `MAP_HUGETLB` 失败是否 fallback 由 page policy 决定。

#### 8.25.4 errno 与公共 ABI

PageAllocator 保存原始 errno/category，上层标准 ABI 统一设置 `errno=ENOMEM` 或参数错误。内部成功路径是否保留调用者 errno 由第 4 节规定，不能让 madvise hint 失败意外污染成功的 malloc errno。

#### 8.25.5 Bootstrap-safe 诊断

PageAllocator、metadata arena 和 direct-map OOM 路径禁止依赖 spdlog、iostream、`std::string` 或 locale。使用：

- relaxed 原子计数；
- 固定大小错误记录环；
- 必要时 `write(2)` 输出固定文本/整数格式；
- 上层显式拉取诊断。

#### 8.25.6 Prefault 与 MAP_POPULATE

`MAP_POPULATE` 可能把缺页成本集中到分配延迟并长时间阻塞。必须：

- 默认关闭；
- 与 THP policy 解耦；
- 对大范围设置单次 prefault byte/time budget；
- 在 aethermind 模型加载与请求热路径分别测试；
- 报告 mmap time、fault time、minor/major faults 和首次访问延迟。

### 8.26 锁序、并发与内存序

#### 8.26.1 锁域

| 锁 | 保护内容 | 禁止事项 |
|---|---|---|
| PageCache shard lock | region、bucket、extent index、Span state、refill state | OS syscall、Central lock、动态日志 |
| PageMap writer lock | radix node 安装和 range publish/clear | 反向获取其他 shard、OS syscall |
| Metadata arena lock | 非 shard-local chunk/free/retire 状态 | 用户回调、OS 调用（尽量预分配） |
| Huge cache atomic/lock | 固定 slot ownership | PageMap/extent transaction |
| Scavenger control lock | 线程等待与停止状态 | PageCache 全量扫描期间长期持有 |

#### 8.26.2 全局顺序

```text
CentralCache lock（释放后）
  -> PageCache owner shard lock
  -> PageMap writer lock
  -> metadata arena lock（若无法 shard-local）
```

OS 调用发生在锁序之外。Reset 若必须冻结所有 shard，按 shard id 升序获取，并在统一点进入 PageMap reset；普通请求禁止多 shard 同持锁。

#### 8.26.3 内存序

- PageMap node/leaf publish：release store；reader：acquire load；
- PageMap root reset：仅 quiescent，可使用明确 relaxed/release 语义并记录前置条件；
- statistics/hints：`memory_order_relaxed`；
- lock-free HugePageCache slot 发布/消费：release/acquire；
- refill/closing 若在锁内访问，保持普通字段；若作为锁外 hint，relaxed load 后必须锁内重验；
- 不用 atomic `Span::IsUsed` 代替 descriptor 生命周期协议。

#### 8.26.4 Shard cache-line 布局

PageCacheShard 需隔离：

- mutex/lock state；
- hottest exact-bucket bitmap；
- refill state；
- frequently updated counters；
- cold tree roots和统计快照。

仅对外层类型 `alignas(64)` 不保证内部热点字段不会与相邻冷字段/其他 shard 伪共享；需通过 `sizeof/offsetof` 和 cache-to-cache profiling 验证。

#### 8.26.5 Fork 与 shutdown

- atfork prepare 固定顺序冻结 PageAllocator 控制线程和 shard；
- child 重建 refill、Scavenger、huge cache 并发状态；
- shutdown 先停止上层请求和 Scavenger，再 drain cache、清 PageMap、unmap；
- lock-free cache 的 drain 只能在无并发 Put/Get 或具备专门协议时执行；
- placement-new singleton 的有意常驻策略需要明确记录。

### 8.27 NUMA-local Region 与大对象放置

#### 8.27.1 路由不等于物理本地性

把请求路由到 node-local PageCache 只能决定逻辑 owner。匿名页的物理 node 通常由 first touch 决定；线程随后迁移可能仍产生远端访问。因此需要同时设计：

- CPU/worker affinity；
- region owner node；
- first-touch 执行线程；
- 可选 `mbind`/NUMA policy；
- cross-node free owner；
- pressure fallback。

#### 8.27.2 Node-local Region

- 每个 region 固定 owner node；
- region 内 shard 属于同一 node；
- SmallRun/LargeExtent 优先 node-local fit；
- fallback 到其他 node 时记录实际 node，并让 free 返回实际 owner；
- coalesce 不跨 node region；
- node budget 按 active/resident/retained 分别限制。

#### 8.27.3 大对象放置

扩展 API 可支持：

- preferred node；
- interleave policy；
- bind/strict bind；
- first-touch callback/worker；
- model/request/temporary lifetime；
- THP/hugetlb preference。

标准 malloc ABI 不暴露这些策略，继续使用安全默认值。

#### 8.27.4 不推荐的做法

- 在进程级临时调用 `set_mempolicy` 后 mmap，再恢复；并发线程可能观察到错误策略；
- 每次分配迁移已有物理页；
- 释放线程决定 extent 新 owner；
- 活跃 region 在 node 间迁移；
- NUMA 不可用时返回失败而没有默认 arena 回退。

#### 8.27.5 验证

使用固定 CPU/socket affinity，采集：

- local/remote pages；
- numa faults/migrations；
- remote LLC/内存访问；
- node-local hit/fallback；
- model load time、tokens/s、请求 p99；
- 每 node active/resident/retained bytes。

### 8.28 可观测性、守恒与诊断

#### 8.28.1 每 shard/region 指标

- alloc request/hit/miss pages；
- exact hit、split hit、large fit、OS refill；
- release、left/right/both coalesce；
- SmallRun bucket occupancy；
- LargeExtent count/bytes/largest extent；
- direct map/unmap count/bytes；
- committed/purged/retained bytes；
- refill single-flight collision；
- shard lock wait/hold samples；
- PageMap publish/clear pages和 cycles；
- metadata live/free/retired bytes；
- NUMA local/fallback。

#### 8.28.2 OS 指标

- mmap/munmap/madvise 调用、字节和 latency histogram；
- ENOMEM/其他 errno；
- retry/purge-before-retry；
- over-map/trim head/tail waste；
- MAP_POPULATE/prefault bytes/time；
- VMA cache hit/miss；
- munmap/purge failure quarantine bytes。

#### 8.28.3 大对象指标

- requested/usable/mapped bytes；
- internal alignment/page-rounding waste；
- cached/direct allocation counts；
- realloc in-place/move/failure；
- lifetime/size histogram（采样）；
- direct PageMap publish cost；
- guard/hugetlb/THP policy 分类。

#### 8.28.4 Hugepage 指标

- aligned mapping bytes；
- THP hint success/failure；
- observed THP bytes；
- explicit hugetlb success/failure/fallback；
- full/partial/empty hugepage；
- breakage/stranded bytes；
- filler hit/miss；
- retained VMA hit 与 resident reuse 分离。

#### 8.28.5 Backend 守恒快照

quiescent diagnostics 应验证：

- 所有 free extent 在 address space 中无重叠；
- SmallRun、LargeExtent 和 DirectMapped 集合互斥；
- size/address 双索引节点一致；
- owner/region/PageMap 一致；
- active 与 free/retained 字节总和等于 region accounting；
- PageMap leaf 不指向已复用/释放 metadata；
- purged/committed/retained 状态与索引一致；
- metadata arena live + free + retired 守恒。

#### 8.28.6 统计开销

- PageMap `GetSpan` 不新增共享统计写；
- shard 慢路径使用 shard-local relaxed counter；
- syscall latency 直接在 Backend 慢路径计时；
- 大型直方图采样更新；
- snapshot/export 由调用方提供 buffer，不在 allocator 内构造 JSON string；
- 统计开关 on/off 分别跑整体性能护栏。

### 8.29 测试、故障注入与性能基准

#### 8.29.1 SmallRun/PageCache 单元测试

- exact hit 1、边界页数和最大 SmallRun；
- non-empty bitmap 与 bucket 一致；
- committed/purged exact hit；
- head/tail/alignment split；
- 左、右、双侧 coalesce；
- SmallRun 合并升级 LargeExtent；
- region 边界禁止合并；
- owner shard 在首次 PageMap 发布前正确；
- page count、地址和所有算术溢出。

#### 8.29.2 多 shard 测试

- 显式启用 2～4 shard，确认每个 shard 实际获得 allocation；
- refill remainder 与 allocated Span owner 一致；
- release 回到实际 owner；
- 相邻但不同 region/shard 不合并；
- 多 shard 并发安装 PageMap node；
- shard closing/refill publish 竞争；
- reset 只统一释放一次 PageMap；
- Scavenger 遍历所有 active shard；
- TSan 下无 writer race。

#### 8.29.3 LargeExtent/DirectMapped 测试

- size/address tree 随机模型验证；
- exact/best-fit 和 bounded candidate；
- prefix/suffix/双 remainder；
- direct threshold 上下边界；
- requested/usable/mapping size；
- over-aligned user pointer 与 original mapping free；
- realloc shrink/grow/move/OOM；
- gigabyte-scale PageMap range 的首尾和随机页；
- interior pointer、double free、stale descriptor 的 hardening 行为。

#### 8.29.4 Retained/Hugepage 测试

- committed -> purged -> reuse；
- purge failure 回滚；
- retained VA budget 超限转 unmap；
- exact 2 MiB VMA cache capacity/concurrent drain；
- THP hint 与 actual backing 指标不混淆；
- hugetlb require/prefer 两种失败语义；
- filler full/partial/empty 和 breakage；
- NUMA policy fallback；
- fork 后 cache/refill 状态恢复。

#### 8.29.5 故障注入

在每个事务提交点注入：

- Span/LargeMetadata/RadixNode arena OOM；
- mmap ENOMEM/非 ENOMEM；
- alignment head/tail munmap 失败；
- PageMap prepare/publish 失败；
- refill publish 时 shard closing；
- madvise/mbind/prefault 失败；
- direct unmap 失败；
- hugetlb unavailable；
- pressure generation 变化。

失败后运行守恒快照，确保旧 extent 仍完整，或新 mapping 被明确清理/quarantine。

#### 8.29.6 Backend 专项基准

建议包括：

- `BM_PageCache_ExactHit/{1,8,32,128}`；
- `BM_PageCache_Split/{small,aligned,large}`；
- `BM_PageCache_Coalesce/{left,right,both,tier-cross}`；
- `BM_PageCache_Refill_Cold`；
- `BM_PageCache_SameShard_Contention`；
- `BM_PageCache_MultiShard_Contention`；
- `BM_LargeExtent_Exact/BestFit/Split`；
- `BM_DirectMap/{1MiB,2MiB,16MiB,1GiB}`；
- `BM_PageMap_PublishRange/...`；
- `BM_Retained_Reuse/Purged_Reuse`；
- `BM_HugepageFiller/...`；
- `BM_Numa_LocalRemote/...`。

分别报告 ns/op、ns/page、cycles、lock wait/hold、syscalls、minor faults、VMA count、RSS/VA 和碎片。

#### 8.29.7 端到端门禁

- 继续运行 3.8 ns 单线程 fast path；
- 随机大小约 26.0 ns；
- 16 线程 64B 约 8.9 us 和 100+ GiB/s；
- Deep Churn 和 Central/PageCache contention；
- 大对象随机分配/释放/realloc；
- aethermind model load、steady inference、KV cache churn；
- 固定编译器、affinity、governor、THP、NUMA 和 cgroup 环境；
- 保存 benchmark JSON、perf、smaps/RSS 与 allocator stats。

### 8.30 分阶段实施与验收

#### 阶段 A：Backend 正确性与 owner 基线

实施内容：

1. 固化 BE 不变量和统一 Span/Extent 状态；
2. 将 owner shard 作为内部 allocation/refill 的显式输入；
3. 保证 owner 在第一次 PageMap 发布前确定；
4. 建立 PageMap writer 协议并引用第 5 节 stable metadata；
5. 区分 PageCache per-shard reset 与全局 PageMap reset；
6. 增加 2～4 shard 实际分配、release、split、coalesce 测试；
7. 建立 Backend 守恒快照和事务故障注入；
8. 建立 PageCache/large/direct 专项基准基线。

退出条件：

- non-zero shard 的 allocated/remainder/free Span owner、list 和 PageMap 始终一致；
- 多 shard writer 无 radix node 泄漏、覆盖或 data race；
- split/coalesce OOM 保持失败原子；
- descriptor 不在无锁 reader 仍可能访问时复用；
- reset/shutdown 只在 quiescent 下统一清理；
- ASan/UBSan/TSan 和整体性能护栏通过。

风险类型：正确性、并发、内存、性能。

#### 阶段 B：锁外 OS 事务与大对象语义

实施内容：

1. 引入 refill single-flight 和 prepare/OS/publish 三阶段事务；
2. 将 mmap、munmap、madvise 移出 shard lock；
3. 解耦 SmallRun 上限、refill 大小和 direct threshold；
4. 增加 large-only metadata：requested、usable、alignment、mapping geometry；
5. 完善 over-aligned、calloc、realloc 与 direct free；
6. 将 PageAllocator 失败诊断改为 bootstrap-safe；
7. 增加 syscall latency、PageMap range 和大对象指标。

退出条件：

- shard lock hold histogram 中不再包含 OS syscall；
- publish 竞争、OOM 和 syscall failure 无 mapping/descriptor 泄漏；
- 大对象 alignment、usable size、realloc 失败语义符合 ABI；
- direct map/unmap 与 PageMap 清理顺序通过并发测试；
- PageCache contention p99 获得可量化改善；
- 小对象 fast path 和现有吞吐护栏无退化。

风险类型：正确性、并发、内存、性能、兼容性。

#### 阶段 C：LargeExtentSet、Region 与碎片治理

实施内容：

1. 引入无递归 intrusive size/address 双索引；
2. SmallRun 合并结果可提升 LargeExtentSet；
3. exact/best-fit、alignment-aware split 和碎片指标；
4. 建立 region descriptor 与稳定 owner；
5. retained VA、purged reuse 和 VA budget；
6. 每 shard/node 增量 purge/unmap；
7. 用真实 aethermind trace 调优 threshold 和 fit policy。

退出条件：

- 双索引随机模型和守恒测试长期通过；
- 中大型重复 workload 的 mmap/munmap 次数和尾延迟显著下降；
- total free 足够却无法 fit 的比例下降；
- region retained VA、PageMap metadata 和 RSS 均受预算约束；
- 不出现跨 region/shard 合并；
- 端到端 RSS、p99 和吞吐综合不劣于阶段 B。

风险类型：正确性、内存、性能、并发。

#### 阶段 D：Hugepage、NUMA 与 aethermind 灰度

实施内容：

1. 精确区分 aligned、THP eligible/backed 和 explicit hugetlb；
2. 按 node 分片完整 2 MiB retained VMA/hugepage cache；
3. 实验 HugepageFiller 和 HugeRegion；
4. 实现 normal/THP/hugetlb page policy；
5. node-local region、first-touch 和可选 mbind；
6. 为 model/KV/workspace 提供扩展 arena API；
7. 在 aethermind 固定 worker、跨 socket 和压力场景分阶段灰度。

退出条件：

- observed THP/hugetlb 指标证明真实 backing 收益，而非仅对齐/hint；
- hugepage breakage、stranded bytes 和 filler fallback 受控；
- NUMA local access、TLB miss、tokens/s 或请求 p99 获得明确收益；
- hugetlb require/prefer 失败语义和普通页 fallback 正确；
- model load、steady inference、KV churn 的 RSS/VA 无不可接受放大；
- 所有新模式具有构建期或运行时快速回退到普通 region/PageCache 的能力。

风险类型：并发、内存、性能、兼容性、运维。

## 9. RSS、碎片与后台回收

RSS 治理不是简单地“尽快 madvise”。分配器必须在复用延迟、物理内存、虚拟地址、系统调用、page fault、THP 完整性和应用尾延迟之间建立可观测的反馈控制。Backend 定义 extent 状态和锁外 OS 事务，本节负责决定何时回收、回收多少以及如何证明 RSS 确实回落。

当前 Scavenger 已具备 `std::jthread`、stop-aware wait、锁内 detach 和锁外 `MADV_DONTNEED` 的基本形态，但仍是单 shard、固定 1 秒周期、10 秒阈值和全桶扫描。后续改进应先修复生命周期与多 shard 正确性，再引入 decay 和压力反馈。

### 9.1 职责、边界与目标

#### 9.1.1 职责

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

#### 9.1.2 目标

| 目标 | 判定方式 |
|---|---|
| Burst 后可预测回落 | 报告 1s/5s/10s/30s RSS 与 allocator retained 曲线 |
| 回收工作有界 | 单轮 pages、syscalls、wall time 和 shard 数均有上限 |
| 无锁序反转 | detach/reinsert 遵循第 5、8 节状态机 |
| 压力响应及时 | `memory.events`/PSI/显式通知到 cache shrink 的延迟可测 |
| 避免回收振荡 | purge 后短期 refault 与再次 refill 比例受控 |
| 统计守恒 | allocator 各层字节快照能解释 mapped/retained 差异 |
| 不伤热路径 | fast-path 和 random-size 基线无可测共享写退化 |

### 9.2 内存统计口径

#### 9.2.1 基础字节

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

#### 9.2.2 避免重复计算

Frontend/TransferCache 对象仍位于 active Span 页中，因此：

- `frontend_cached_bytes` 和 `transfer_cached_bytes` 是 active 页内部分类；
- 它们不能再次加到 `active_span_bytes` 上计算 mapped bytes；
- `central_bitmap_free_bytes` 也是 active Span 内部未使用容量；
- `pagecache_free_*` 才是从 active Span 退出后的页级空闲；
- metadata 和 direct mapping 单独计入；
- OS RSS 包含 allocator 之外的栈、代码、文件映射和其他匿名区，不能与 allocator accounting 强行守恒。

#### 9.2.3 派生指标

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

#### 9.2.4 一致性等级

- **Relaxed live counters**：无全局停顿，适合在线趋势；
- **Epoch snapshot**：读取前后 generation，相同则近似一致；
- **Quiescent exact snapshot**：测试/shutdown 时冻结各层并逐项校验；
- **OS observation**：低频读取 RSS、PSI、cgroup，时间点不与内部快照严格同步。

控制接口必须标注快照类型。

### 9.3 页状态与可回收性

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

### 9.4 当前 Scavenger 基线与差距

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

### 9.5 Scavenger 生命周期

#### 9.5.1 状态机

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

#### 9.5.2 启动策略

- 显式 API 模式可按配置 lazy start；
- malloc interposition 模式必须在 bootstrap 安全后启动；
- 创建后台线程失败不影响基本分配，但要记录可查询状态；
- 是否允许退避重试由控制面决定，不能每次慢路径重试；
- 默认最多一个全局协调线程，后续可按 NUMA node 分 worker；
- worker 数上限与 shard/node 数解耦，避免大型机器线程爆炸。

#### 9.5.3 Shutdown

顺序：

1. 禁止新的后台 wake；
2. request stop 并唤醒；
3. join worker；
4. 等待所有 detached purge work item 提交/回滚；
5. 再执行 PageCache/CentralCache destructive drain；
6. 最终释放控制 metadata。

不得在 PageCache 已销毁后让 TLS 或后台线程重新进入 allocator。

#### 9.5.4 Fork

- prepare 阶段冻结 worker 并等待当前 pass 结束；
- parent 恢复之前的运行状态；
- child 清除继承的 jthread/condvar 运行假象；
- child 只在显式重新启用时创建新 worker；
- atfork handler 不分配、不记录复杂日志。

### 9.6 唤醒源与压力等级

#### 9.6.1 唤醒源

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

#### 9.6.2 压力等级

| 等级 | 动作 |
|---|---|
| Normal | 正常 decay、保留复用价值高的 committed extent |
| Soft | 停止预取/容量增长，缩 Frontend/Middle-end，提前 purge |
| Hard | 大幅 drain cache，bypass retained cache，优先 unmap 大冷 extent |
| Critical | 有界同步协助回收，禁用可选 profiling/quarantine 增长 |
| OOM recovery | 只执行可证明、有限且不分配 metadata 的回收，然后最多重试一次 |

状态升级快、降级慢，并设置滞回与最小保持时间。

#### 9.6.3 信号合并

多个压力源合并为单调 generation + 当前最高等级。worker 处理完一轮后重新读取；不为每个事件排队动态对象，也不丢失更高等级。

### 9.7 Dirty/Purged Decay

#### 9.7.1 Decay 目的

固定“空闲 10 秒后全部 purge”会在突发 workload 中形成批量 syscalls。建议采用近似 decay：

- 为 free committed bytes 按时间窗口分桶；
- 每个 epoch 只 purge 已到期比例；
- 曲线首尾速率较低，减少突发；
- reuse 时从对应 age bucket 撤销；
- pressure 可提升 decay 速率；
- decay=-1 表示禁用自动 purge，0 表示尽快 purge。

#### 9.7.2 无分配时间轮

每 shard 使用固定数量 age buckets：

~~~text
epoch 0 ... epoch N-1
  intrusive list of free extent candidates
~~~

Span 只能同时位于 size/address 索引和一个 age 结构时，需要独立 hooks 或 side metadata；不能复用唯一 `next/prev` 导致双重链表。另一方案是仅存 last-free epoch，在 size index 上使用有界 cursor 扫描。

#### 9.7.3 Decay 参数

- dirty decay ms；
- retained/unmap decay ms；
- minimum purge extent bytes；
- per-pass pages/syscalls/time；
- pressure multiplier；
- refault penalty window；
- per-node overrides。

参数动态更新后使用 generation 生效，不回溯修改所有 Span。

#### 9.7.4 Refault 反馈

统计 purge 后在短窗口内重新分配/触页的字节。高 refault 说明 decay 太激进；低 refault 且 RSS 高说明可加快。调整在控制线程进行，每周期限幅。

### 9.8 Purge 模式与系统语义

#### 9.8.1 `MADV_DONTNEED`

- 成功后后续匿名页读取呈零；
- 通常适合需要确定零语义和明确回收的 free extent；
- 会产生后续 fault；
- 部分 THP 范围可能破坏 hugepage；
- 仅在逻辑 free、detached 的页上使用。

#### 9.8.2 `MADV_FREE`（可选）

- 允许内核延迟回收，页在真正回收前可能仍保留旧内容；
- 复用前不能据此满足 calloc 零语义；
- RSS 可能不立即下降；
- 需要单独 `muzzy` 状态和平台探测；
- 初始实现可以不启用，避免状态语义混淆。

#### 9.8.3 Unmap

适用于：

- DirectMapped 释放；
- retained VA 超限；
- 大而冷的完整 region；
- VMA 数与地址空间策略允许；
- pressure hard/critical。

unmap 前遵循第 8 节 PageMap clear 和 descriptor retire。

#### 9.8.4 相邻范围批量

worker 在锁下摘取多个相邻、同 policy extent，锁外合并为有限 work item，减少 madvise 调用。批量不能跨 active、guard、region、node 或 backing policy 边界。

### 9.9 Detached 回收事务

#### 9.9.1 Detach

在 owner shard 锁下：

1. 选择符合 age/pressure 的 free extent；
2. 从 allocatable index 和 decay candidate 中移除；
3. 标记 `kDetachedForPurge`；
4. 保存 start/pages/generation/原状态；
5. 将 descriptor 放入栈上或固定 work array；
6. 释放锁。

#### 9.9.2 OS 操作

- 仅操作 work item 指定范围；
- 不读取可能变化的 Span 链；
- 捕获 errno 到固定结果；
- 记录 syscall time；
- 相邻范围按上限合并；
- 不调用普通 allocator。

#### 9.9.3 Commit/Rollback

重新持 owner shard 锁：

- 验证 descriptor generation 和 detached 状态；
- 成功：转 `kFreePurged`，插入相应 index；
- 失败：恢复 `kFreeCommitted`；
- shutdown closing：保持 detached，交给 unmap/drain；
- 更新 bytes 和 failure counters；
- 不把 purge 时间误写成 free/reuse 年龄。

#### 9.9.4 并发约束

detached extent 不能：

- 被 exact/split 分配；
- 被相邻 free coalesce；
- 被另一轮 Scavenger 重复选择；
- 被 PageCache reset 释放而 worker 仍在 syscall；
- 改变 owner shard。

### 9.10 增量扫描与工作预算

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

### 9.11 分层 Cache 排空

#### 9.11.1 顺序

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

#### 9.11.2 同步与异步

- Soft pressure：全部异步；
- Hard：调用线程最多协助有限 batch/page；
- Critical/OOM：执行不需要新 metadata 的同步 drain；
- 显式 purge API 可提供 async token 或 blocking 模式；
- blocking 模式必须注明不会保证 OS RSS 立即下降。

#### 9.11.3 防振荡

- pressure 下 Frontend refill batch 和 Middle-end prefetch 同步收缩；
- 回收后保留 cooldown；
- cache 重新增长采用 slow-start；
- 记录 `purged_then_refilled_bytes`；
- 只有压力解除且命中收益持续才恢复容量。

### 9.12 cgroup、PSI 与容器环境

#### 9.12.1 cgroup v2 输入

建议低频解析：

- `memory.current`；
- `memory.high`；
- `memory.max`；
- `memory.events.local` 的 high/max/oom/oom_kill；
- `memory.stat` 的 anon、pgfault、pgmajfault 等；
- `memory.pressure`。

解析按 key，不依赖行顺序；处理 `max`、权限失败、容器迁移和 cgroup 路径变化。

#### 9.12.2 解释

- 超过 `memory.high` 会触发节流和重回收，不等于 OOM；
- `memory.events` 计数变化是事件，不是当前压力等级；
- PSI 衡量 stall，不等于 allocator 可回收字节；
- `memory.current` 包含进程/cgroup 其他内存，不应全部归因 allocator；
- 管理器提供的显式预算可比自动猜测更可靠。

#### 9.12.3 读取架构

后台 monitor 使用预打开 fd、固定 buffer 和无分配 parser。热路径只读取 cache-line 隔离的 pressure generation/level。不可从 malloc/free 内调用 fopen、fstream 或构造 string。

#### 9.12.4 主动回收接口

如果支持 cgroup `memory.reclaim`，它是外部管理面能力，不应由 allocator 默认写入；allocator 自身只回收自己拥有的缓存。aethermind 管理器可在更高层协调 allocator purge 与系统 reclaim。

### 9.13 预算与策略控制

#### 9.13.1 预算层级

- process allocator budget；
- NUMA node budget；
- arena/model/request budget；
- Frontend total budget；
- Middle-end budget；
- PageCache committed/retained budget；
- metadata budget；
- hugepage/hugetlb budget。

父级预算必须覆盖子级，不允许每层独立扩大后总和失控。

#### 9.13.2 Soft/Hard limit

- soft limit 触发渐进收缩；
- hard limit 禁止增长并触发同步有限回收；
- limit 以 bytes 为主，slot/Span 数为辅助；
- 配置小于不可回收 live bytes 时报告 unsatisfiable，不循环 purge；
- 预算变更使用 generation 发布。

#### 9.13.3 受益评分

缓存保留优先级可依据：

- 最近命中次数/每字节；
- refill/syscall 避免成本；
- refault 代价；
- NUMA locality；
- hugepage 完整性；
- extent age；
- workload tag/lifetime。

复杂评分仅在控制路径运行。

### 9.14 可观测性与报警

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

### 9.15 测试与故障注入

#### 9.15.1 确定性测试设施

- 注入 monotonic clock；
- 暴露 test-only `RunOnePass(budget)`；
- mock madvise/munmap 结果；
- 禁止测试依赖真实等待 10 秒；
- 可查询 detached 和 index 状态；
- reset 前自动 stop/join worker。

#### 9.15.2 单元测试

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

#### 9.15.3 并发测试

- allocate/split/coalesce 与 purge 并发；
- Central empty Span 返回与 Scavenger detach；
- reset/stop/wake 竞争；
- fork prepare 时 worker 正在 syscall；
- cgroup monitor 更新与控制线程读取；
- TSan 验证 Span flags/state 不在锁外竞态访问。

#### 9.15.4 压力测试

- burst 分配后 idle；
- 周期性 burst，检查振荡；
- cgroup `memory.high`；
- 低 memory.max/OOM；
- THP on/off；
- NUMA node pressure；
- 长时间 churn；
- aethermind model load -> steady inference -> unload。

### 9.16 性能与内存基准

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

### 9.17 分阶段实施与验收

#### 阶段 A：状态、生命周期与统计基线

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

#### 阶段 B：增量 Decay 与有界工作

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

#### 阶段 C：压力控制与分层排空

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

#### 阶段 D：aethermind 自适应治理

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

## 10. NUMA 与 aethermind 集成

ammalloc 作为 aethermind 的底层分配器，应同时提供“普通进程堆”和“显式推理内存设施”，但两者不能混成一套隐含策略。标准 malloc ABI 保持可移植、安全默认值；NUMA、生命周期、hugepage、arena reset 和 allocation tag 通过独立扩展 API 提供。

### 10.1 集成目标与非目标

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

### 10.2 集成不变量

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

### 10.3 当前仓库基线与差距

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

生产 PageCache 仍固定 shard 0，物理页依赖 first touch；因此 NUMA 接口必须在第 7、8 节多 shard/region owner 稳定后启用，不能先暴露无法兑现的 API。

### 10.4 NUMA 拓扑发现

#### 10.4.1 启动快照

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

#### 10.4.2 动态变化

CPU/node hotplug、cpuset 调整和容器迁移通过低频 generation 更新。普通 fast path 只读取稳定映射或回退默认 node；更新不能释放仍被 reader 使用的 snapshot。

#### 10.4.3 不可用回退

- 单 node：所有 route 为 node 0；
- 无权限：使用 CPU affinity + first touch，标记 policy unknown；
- CPU 不在 snapshot：回退默认 arena；
- 请求 node 不在 allowed mems：返回 fallback 或 strict failure；
- 非 Linux：扩展 API 返回 unsupported，标准 allocator 正常工作。

### 10.5 NUMA policy 语义

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

### 10.6 Arena 模型

#### 10.6.1 ArenaDescriptor

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

#### 10.6.2 Arena ownership

- allocation 返回的 Span/Extent 记录 arena id；
- free 通过 PageMap 恢复 arena/owner；
- arena 关闭后拒绝新分配；
- closing 期间逐步 drain Frontend/Middle-end；
- reset 只有在无 live/borrowed allocation 时提交；
- destroy 后 generation 变化，旧 handle 失效；
- default arena 可选择进程生命周期常驻。

#### 10.6.3 Arena 数量

不要为每个请求创建完整 PageCache/CentralCache。建议：

- 少量长期 arena 拥有独立 region/budget；
- request arena 使用轻量子 arena/segment；
- 高基数 request id 作为 tag/sample，不成为全套 shard；
- arena 上限固定，可配置 exhaustion fallback。

### 10.7 Worker 与内存亲和性

#### 10.7.1 Worker route

aethermind worker 初始化时：

1. 固定或确认 CPU affinity；
2. 获取 CPU node；
3. 绑定 Frontend route；
4. 选择 node-local Central/PageCache arena；
5. 由实际访问线程完成 first touch；
6. worker 迁移时按策略保留原 route 或在安全点重绑定。

#### 10.7.2 First touch

仅 mmap/mbind 不保证已驻留页。模型加载可由目标 node worker 并行触页；请求热路径避免大范围同步 prefault。记录 fault time 和实际 NUMA placement。

#### 10.7.3 线程迁移

- 固定 worker 推荐保持 route；
- 短暂调度迁移不立即重绑定；
- 持续迁移超过阈值在慢路径更新 Frontend route；
- active object/Span owner 不迁移；
- remote access 与 cache-to-cache 数据决定是否值得更新。

### 10.8 Model Arena

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

### 10.9 Request Arena

#### 10.9.1 适用对象

- 解析/调度临时结构；
- shape/plan scratch metadata；
- 生命周期严格包含在请求内的小对象；
- 可批量销毁且不需要逐对象 free 的 POD/平凡析构对象。

#### 10.9.2 结构

推荐 segment/bump + fallback：

- 从 node-local Backend 获取 segment；
- bump pointer 提供极短分配路径；
- 大或 over-aligned 请求转 Backend；
- reset 时批量归还 segment；
- segment 内不需要逐对象 PageMap size-class 元数据；
- 可设置 max retained segments；
- request 结束时 generation 失效。

#### 10.9.3 限制

- 非平凡 C++ 对象仍需上层运行析构；
- 指针不得逃逸到请求结束后；
- 异步 GPU kernel、通信或 callback 完成前不能 reset；
- 跨 request cache 不能存 request arena 指针；
- debug 模式 reset 后 poison/protect；
- fallback allocation 的归属必须随 arena reset 正确处理。

### 10.10 KV Cache 与 Tensor Workspace

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

### 10.11 Runtime Metadata

运行时小型长期对象可使用：

- default small-object allocator；
- 专用 size-class profile；
- subsystem arena/tag；
- stable-address metadata arena。

不要为所有 metadata 一律 request arena；需要跨请求存活的 scheduler、graph、kernel cache 和 communicator 对象必须有明确长期 domain。

### 10.12 Device、Pinned 与共享内存边界

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

### 10.13 Cross-thread/Node Free

- PageMap 恢复 allocation owner arena/node/shard；
- 普通 local free 走 Frontend；
- free-only/remote 流量先按第 6、7 节 bounded queue/batch；
- owner closing 时回退其 Middle-end drain path；
- request arena 通常不允许任意逐对象跨线程 free；
- model/KV block 返回专用 owner pool；
- remote bytes 计入实际 owner budget；
- 不能由释放线程把页重新标记为本 node owner。

### 10.14 内存压力优先级

建议默认回收顺序：

1. 已结束 request arena；
2. idle workspace 和 staging；
3. Frontend/Middle-end 冗余；
4. PageCache free committed；
5. idle KV free blocks；
6. retained model temporary region；
7. live model/KV 只由上层 eviction/unload 决定。

allocator 不得自行释放仍属 live model/request 的用户对象。pressure callback 只通知上层做业务级 eviction。

### 10.15 扩展 API 草图

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

### 10.16 配置与灰度

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

### 10.17 可观测性

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

### 10.18 测试与基准

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

### 10.19 分阶段实施与验收

#### 阶段 A：Domain 与显式 Arena 基础

1. allocator domain 文档和 RAII wrapper；
2. versioned arena handle；
3. default/model/request 基础配置；
4. caller-buffer stats；
5. reset/quiescence 协议；
6. aethermind 小范围显式 API。

退出条件：domain mismatch 可检测；arena reset 无 UAF；默认 malloc 不受影响。

风险类型：正确性、兼容性、内存。

#### 阶段 B：Request/Model 生命周期治理

1. request segment arena；
2. model arena budget/load/unload；
3. workspace pool；
4. allocation tag/sampling；
5. pressure priority；
6. 真实 trace。

退出条件：请求分配成本和模型 unload RSS 有明确收益；无 pointer escape。

风险类型：正确性、内存、性能。

#### 阶段 C：NUMA-local 链路

1. topology snapshot；
2. worker route；
3. node-local Frontend/Central/PageCache；
4. first touch/mbind policy；
5. remote free；
6. per-node stats。

退出条件：实际页 placement 与 owner 一致；remote access、tokens/s 或 p99 改善；fallback 正确。

风险类型：并发、性能、兼容性。

#### 阶段 D：KV/Hugepage 与默认化评估

1. KV block/HugeRegion；
2. pinned/device domain wrapper；
3. hugetlb prefer/require；
4. multi-model pressure；
5. 生产灰度和自动回滚。

退出条件：真实推理综合收益明确；domain 安全；运维指标和回滚成熟。

风险类型：性能、内存、运维、兼容性。

## 11. 可观测性与 Profiling

可观测性必须先回答“字节在哪里、为什么保留、谁在竞争、哪个调用栈长期存活”，再提供动态调参。控制面是 allocator ABI 的一部分，错误设计会引入递归、全局锁、ABI 漂移和不可复现的生产行为。

### 11.1 目标与原则

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

### 11.2 当前基线与差距

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

### 11.3 指标层级

#### 11.3.1 Level 0：始终开启

- fatal/OOM/syscall failure；
- mapped/unmapped bytes；
- current cache bytes（能以 owner-local 普通字段维护时）；
- high-water；
- allocator state/version；
- pressure level；
- active shard/node/arena 数。

#### 11.3.2 Level 1：低成本详细统计

- per-size-class hit/miss/batch；
- lock contention sample；
- split/coalesce/refill；
- purge/refault；
- remote free；
- NUMA fallback；
- large size histogram；
- metadata arena。

通过构建或运行期开关启用，更新在已进入的慢路径或 shard-local 数据中完成。

#### 11.3.3 Level 2：Allocation Sampling

- sampled requested/usable；
- allocation time、thread/CPU/node/arena/tag；
- sampled stack id；
- lifetime；
- live/peak bytes；
- sampled fragmentation；
- sampled realloc path。

#### 11.3.4 Level 3：Guarded/Hardened Sampling

- guard pages；
- quarantine；
- redzone/canary；
- delayed reuse；
- crash record；
- 高成本 stack capture。

默认关闭或极低采样。

### 11.4 指标数据模型

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

### 11.5 Counter、Gauge 与 Histogram

#### 11.5.1 Counter

单调事件：alloc calls、miss、syscalls、failures。使用 `uint64_t` relaxed，考虑溢出自然回绕并在快照层计算 delta；控制接口标明是否从启动累计。

#### 11.5.2 Gauge

当前 bytes/objects/spans。若由单 owner 维护，用普通字段并在 snapshot 锁/epoch下读取；不要为了在线读取把所有字段变成 atomic。

#### 11.5.3 High-water

慢路径 CAS max 或 owner-local 更新。普通 fast path 的 ThreadCache high-water 可在线程退出/采样时汇总。

#### 11.5.4 Histogram

固定指数 buckets：

- allocation size；
- batch；
- lock wait/hold；
- syscall latency；
- allocation latency sample；
- lifetime；
- refault interval。

避免 HDR histogram 等动态分配结构进入核心。

### 11.6 缓存行与聚合

- 每线程/CPU 热计数与对象指针数组隔离；
- per-shard counters 与 lock state 分离，避免读 exporter 干扰写热点；
- process global counters 按 CPU/shard 分片；
- exporter 聚合，不在更新时全局 fetch_add；
- 低频项可直接 global relaxed atomic；
- 结构用 `static_assert(sizeof/alignof/offsetof)` 验证；
- perf c2c 验证统计开关的伪共享。

### 11.7 快照协议

#### 11.7.1 Relaxed

逐字段读取，允许跨时间。适合监控。

#### 11.7.2 Generation/Epoch

~~~text
g0 = stats_generation acquire
read shard snapshots
g1 = stats_generation acquire
if g0 == g1: approximately consistent
else retry at most once or mark inconsistent
~~~

控制线程在结构级变化后递增 generation。不能因反复变化无限重试。

#### 11.7.3 Quiescent

测试、诊断或 shutdown：

- 停止新请求；
- drain/冻结各层；
- 按锁序读取；
- 可遍历对象/Span；
- 执行守恒；
- 不用于在线监控。

#### 11.7.4 Stats epoch

类似“刷新统计视图”的 epoch 不应清空真实累计 counter。可维护 snapshot cache generation，调用者显式 refresh 后查询同一代数据。

### 11.8 无分配控制 ABI

#### 11.8.1 Typed C ABI

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

#### 11.8.2 字符串 namespace

可提供 mallctl 风格薄适配，但内部先将固定名称解析为 numeric id。解析使用 caller string_view 和无分配 trie/table，不构造 `std::string`。

#### 11.8.3 JSON/Text

- 调用方传 buffer + writer callback；
- 支持 dry-run size estimate；
- 输出被截断时返回明确状态；
- writer callback 必须声明是否允许重入 allocator；
- 最安全默认是 allocator 只写 caller buffer；
- JSON schema 带版本；
- 人类文本不是机器稳定 ABI。

### 11.9 配置控制面

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

### 11.10 Allocation Sampling

#### 11.10.1 采样方法

推荐基于分配字节的几何间隔，而非每 N 次固定采样：

- 每线程/CPU 保存 `bytes_until_sample`；
- fast path 只减本地计数并检查分支；
- 到期进入 noinline sample slow path；
- 下一个间隔由轻量 PRNG 产生；
- 平均 interval 可配置，如 512 KiB 起步；
- sized free 或 PageMap metadata 用于结束 sample 生命周期。

#### 11.10.2 Sample record

- allocation id/generation；
- ptr 或稳定 sample key；
- requested/usable；
- timestamp/CPU/thread/node/arena/tag；
- stack id；
- large/small/class；
- live/freed；
- weight。

records 来自预分配 ring/pool，耗尽时 drop 并计数，不回退普通 heap。

#### 11.10.3 偏差校正

按采样概率加权估算 live bytes 和调用栈分布；对超大对象可强制采样。输出同时报告 sample count、drop 和置信限制，不能把估计值伪装为精确值。

### 11.11 调用栈与符号化

- allocator 内只采集原始 PC；
- unwind 可选 frame-pointer 或预验证的无分配 unwinder；
- 不在分配路径符号化；
- stack depot 使用固定/arena-backed hash table；
- 表满后降级 drop/aggregate；
- 符号解析在外部工具或安全控制线程；
- 避免 loader lock/reentrant malloc；
- build id、binary mapping 在 profile 导出时关联。

### 11.12 Heap Profile 与泄漏诊断

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

### 11.13 Event Ring 与错误诊断

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

### 11.14 Export 与生态集成

优先级：

1. C ABI snapshot；
2. 文本/JSON caller buffer；
3. Prometheus/OpenTelemetry 由 aethermind adapter 读取；
4. profile 文件由外部工具符号化；
5. debugger pretty-printer/diagnostic command。

allocator 不直接启动 HTTP server、不依赖 protobuf/JSON 库、不在核心链接日志框架。

### 11.15 观测开销预算

设定门禁：

- Level 0 对 3.8 ns fast path 无统计显著退化；
- Level 1 在 Middle/Backend 慢路径开销可量化；
- sampling 默认间隔下整体吞吐退化目标 <1%（最终以实测阈值为准）；
- stack capture 单独报告；
- exporter 不应阻塞 allocator shard；
- stats reader 高频运行时不引起 cache-to-cache 激增；
- 内存开销受 metadata/profile budget 限制。

### 11.16 测试

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

### 11.17 分阶段实施与验收

#### 阶段 A：统一统计 Schema

1. 定义字节口径；
2. numeric metric id/schema；
3. Level 0 counters/gauges；
4. caller-buffer snapshot；
5. quiescent守恒；
6. 移除核心日志依赖。

退出条件：各层字节可解释；接口无分配；stats-off 基线通过。

风险类型：正确性、性能、兼容性。

#### 阶段 B：控制面与在线快照

1. typed get/set；
2. config snapshot/generation；
3. purge/flush action；
4. epoch snapshot；
5. fixed event ring；
6. aethermind exporter adapter。

退出条件：并发控制无全局停顿；错误/版本语义稳定；可回滚。

风险类型：并发、兼容性、运维。

#### 阶段 C：Allocation Sampling

1. byte-geometric sampler；
2. record pool；
3. stack depot；
4. live/lifetime profile；
5. tag/arena；
6. overhead benchmark。

退出条件：sample 估计可验证；drop/预算可见；默认开销满足门禁。

风险类型：性能、内存、可观测性。

#### 阶段 D：Guarded Profiling 与生产诊断

1. guarded sampling；
2. crash/event record；
3. external symbolization；
4. peak/growth profile；
5. 自动告警与灰度。

退出条件：真实问题可定位；安全模式不污染默认 fast path；生产可动态关闭。

风险类型：安全、性能、运维。

## 12. 安全加固

分配器安全设计必须区分“始终成立的内存安全基线”和“可选的漏洞检测/利用缓解”。默认模式不能为了 3.8 ns 牺牲生命周期、整数安全或所有权；Hardened 模式则可以用可控的内存和延迟成本提高检测覆盖。

### 12.1 威胁模型

需要覆盖：

- accidental double free、invalid free、interior free；
- size/alignment overflow；
- use-after-free；
- freelist next 被用户越界写破坏；
- Span/PageMap stale descriptor；
- metadata corruption；
- cross-thread/remote queue ABA；
- buffer overflow/underflow；
- uninitialized/calloc 语义错误；
- allocator recursion 和 reentrant corruption；
- shutdown/fork 生命周期竞态；
- 攻击者可控 heap 数据试图伪造 freelist 指针；
- sampled production detection。

不承诺：

- 默认模式检测全部堆越界；
- 对任意野指针 `free` 完全恢复并继续运行；
- 对 data race 导致的用户内存破坏提供隔离；
- 替代 ASan/HWASan；
- 在损坏 allocator 核心 metadata 后保证进程可继续安全服务。

### 12.2 安全 Profile

| Profile | 目标 | 典型能力 |
|---|---|---|
| ReleaseFast | 生产性能默认 | 必要边界/生命周期/checked arithmetic |
| ReleaseChecked | 低成本增强 | sampled validation、pointer encoding、poison sampling |
| Hardened | 高风险服务 | canary、quarantine、guard sampling、严格 invalid free |
| Debug | 开发诊断 | 全量断言、bitmap/list 校验、poison、慢速守恒 |
| Sanitizer | 动态工具 | ASan/UBSan/TSan 专用布局与禁用冲突优化 |

Profile 由构建期能力和运行期策略共同决定。不能用 `NDEBUG` 单一宏隐式改变公共 ABI 或 descriptor 布局。

### 12.3 始终成立的安全不变量

- 所有 byte/page/count/alignment 计算 checked；
- PageMap 读无锁但 descriptor 生命周期安全；
- owner shard、region、size class 在发布前确定；
- free object 进入 freelist 前至少能存放 pointer；
- 用户指针满足 API alignment；
- calloc 乘法和清零语义正确；
- realloc 失败保留旧对象；
- OOM 不留下半提交 split/coalesce；
- unmap 前 clear PageMap；
- metadata 不来自普通 heap；
- lock-free 结构的 ABA/generation 明确；
- free(nullptr) no-op；
- 外部指针策略明确，不静默污染 allocator；
- core failure reporting 不递归；
- Release 构建保留关键状态校验，不能全部依赖 DCHECK。

### 12.4 Pointer 分类与 Free 验证

#### 12.4.1 分类流程

~~~text
null -> success
  -> PageMap lookup
  -> validate descriptor lifetime/generation/state
  -> validate ptr within user allocation domain
  -> small: object-start/alignment/class validation
  -> large: exact user pointer / mapping metadata validation
  -> route owner
~~~

#### 12.4.2 Unknown pointer

策略必须按部署模式区分：

- 显式 `am_free`：unknown pointer 是 API 违反，可 fail-fast/错误回调；
- preload fully-owned process：通常 fail-fast，防止跨 allocator domain；
- transition/mixed mode：只有存在可靠 ownership registry 时才允许系统 free fallback；
- 当前“PageMap miss 直接忽略”会掩盖泄漏和 domain bug，不应作为最终语义。

#### 12.4.3 Interior pointer

小对象验证：

- ptr >= data base；
- ptr < data base + capacity * class size；
- offset % class size == 0；
- object index < capacity。

大对象要求 ptr 等于 recorded user pointer；page-aligned interior address不能被当成 allocation base。

#### 12.4.4 Validation 分层

- fast release：依赖 trusted internal route + 必要字段；
- public free slow path：低成本边界；
- sampled checked：全 PageMap range/generation；
- Debug：bitmap、链、owner、arena 全验证。

### 12.5 Double Free 检测

#### 12.5.1 Small object

对象从用户 free 到 Frontend/TransferCache 时 bitmap 仍为 allocated，不能依靠 Span bitmap立即检测重复 free。候选：

- ThreadCache/remote queue sampled membership check；
- object header/tag（有空间和 profile 时）；
- per-Span secondary state bitmap（Hardened）；
- quarantine membership；
- 最终回 Span bitmap 时检测 bit 已为 1。

默认模式至少在最终 bitmap free 检测；Hardened 模式提供更早检测。

#### 12.5.2 Large object

- free 开始时通过 descriptor state CAS/owner lock 转 closing；
- 第二次 free 观察非 active state并 fail；
- PageMap clear 后 stale free 由 generation/retired metadata 识别；
- descriptor 不立即复用，避免 ABA 把旧 pointer 解释成新对象。

#### 12.5.3 错误处置

默认推荐 fail-fast，而不是继续操作损坏 heap。提供固定 error code、地址、Span generation、thread/cpu 到 crash/event ring；禁止在损坏状态做复杂 heap dump。

### 12.6 Freelist Hardening

#### 12.6.1 Pointer encoding

可编码：

~~~text
encoded_next = next XOR process_secret XOR Rotate(object_address)
~~~

要求：

- secret 在 bootstrap 安全初始化；
- decode 后校验 canonical address、alignment、PageMap/size class；
- 不能只用固定常量；
- fork child 是否刷新 secret 必须与继承 heap兼容，通常不能直接刷新旧对象编码；
- crash dump 工具能按受控方式解码。

#### 12.6.2 Safe-linking 边界

encoding 提高利用难度但不证明 pointer 合法。必须配合 range/alignment/owner 验证；若每次 fast path 都做完整 PageMap lookup，性能成本可能不可接受，应在 ReleaseChecked/Hardened 或 refill/trim 批量边界使用。

#### 12.6.3 链完整性

- batch count 上限；
- tail 可达性 sampled 验证；
- Floyd cycle 检测仅 Debug/异常路径；
- object next 不指向自身；
- chain 对象同 class/owner；
- bulk 转换时不重复；
- corrupted chain 不继续写任意地址。

### 12.7 Poison、Junk Fill 与 Zeroing

策略：

- allocation fill：Debug 可写 `0xA5`；
- free fill：Debug/Hardened 写 `0x5A`，保留编码 next 区；
- sampled poison：ReleaseChecked；
- calloc：必须全零；
- purged/new anonymous mapping 可利用内核零语义；
- dirty cache reuse 必须显式清零；
- sensitive arena 可 secure-zero，但须防编译器消除；
- large poison 设置 byte/time 上限，避免巨大同步 memset。

统计 fill bytes/time；默认 fast path 不全量 poison。

### 12.8 Redzone 与 Canary

#### 12.8.1 Small object

选择：

- 使用 class slack 放置 tail canary；
- Hardened 专用 size-class 表增加 redzone；
- sample object 使用独立 guarded allocation。

检查时机：

- free；
- quarantine release；
- Span drain；
- explicit heap check。

不能把用户 `usable_size` 报告到包含 canary 的范围。

#### 12.8.2 Canary

canary 由 secret、object address、generation、requested size 混合，避免全局固定值。保存位置不能覆盖用户合法 alignment/size；大对象可放 header/side metadata + tail。

#### 12.8.3 限制

redzone 只能在 free/check 时发现，无法阻止越界发生；大幅增加内部碎片。默认采用 sampling 或 Hardened 专用 class。

### 12.9 Guarded Sampling

- 以低概率将分配路由到 guard allocator；
- mapping 前后 guard page；
- 可随机左/右对齐以检测 under/overflow；
- free 后 `PROT_NONE` 并进入有界 quarantine；
- metadata 独立保存 requested/stack/generation；
- guard address range 在独立 registry/PageMap policy 中；
- sample pool 耗尽时回退普通分配并计数；
- 大对象可复用已有 page granularity；
- crash handler 输出固定 record；
- 不保证所有分配被采样。

### 12.10 Quarantine

#### 12.10.1 目标

延迟重用提高 UAF 可检测窗口，但会增加 RSS。必须同时限制：

- bytes；
- object count；
- per-class；
- per-thread/CPU；
- age；
- process/node/arena。

#### 12.10.2 数据结构

使用固定 ring、intrusive FIFO 或 arena-backed bounded queue；不使用 deque/vector。对象在 quarantine 时仍不回 bitmap/PageCache，并计入独立 bytes。

#### 12.10.3 压力

Soft pressure 缩短 quarantine；Hard/Critical drain，但 guard `PROT_NONE` sample 的元数据仍需安全释放。安全 profile 不得导致 cgroup OOM。

### 12.11 Metadata 保护

- Span/Radix/Region arena 与用户 mapping 分离；
- metadata page 不交付用户；
- stable metadata 减少 UAF/ABA；
- Debug 在 chunk 完成初始化后可将只读表 `mprotect` 为 RO；
- mutable metadata 可加 guard page 或随机 canary；
- free-list node poison；
- index/tree invariant sampled check；
- sensitive secret 位于独立 cache line/page；
- core dump 暴露 secret 的威胁需文档化；
- metadata error 使用 fail-fast，不尝试复杂自修复。

### 12.12 随机化

可选：

- Span 内 allocation 起点随机化；
- 同 size bucket 有界随机候选；
- guard side 随机；
- quarantine delay 抖动；
- process secret；
- region base 依赖 ASLR。

随机化不能：

- 破坏 LIFO 默认局部性而无数据；
- 使用系统 `rand()` 或会分配的设施；
- 使 benchmark 不可复现；
- 影响所有权/锁序；
- 作为正确性替代。

测试允许固定 seed；生产 secret 使用 OS entropy，失败时明确降级。

### 12.13 Reentrancy 与故障隔离

- TLS recursion depth；
- bootstrap allocator 独立 domain；
- signal/crash 路径只用 async-signal-safe fixed writer；
- OOM callback 不允许普通 heap，或明确 guarded reentry；
- profiler/unwinder 不符号化；
- invalid free 不调用用户 logger；
- internal background thread 使用 allocator-safe primitives；
- loader/constructor/destructor 阶段状态机；
- fork child 清理 inherited locks；
- corruption 时停止当前 heap mutation。

### 12.14 安全控制与默认值

配置项应是 typed policy：

- pointer encoding；
- poison rate；
- canary/redzone；
- quarantine bytes；
- guarded sample rate；
- invalid free action；
- stack capture；
- metadata protection；
- abort callback（严格约束）。

结构性能力构建期启用；运行期只能在安全状态转换内改变。每个配置报告预计内存/性能成本。

### 12.15 安全可观测性

- invalid/interior/double free；
- canary/redzone failure；
- freelist decode/range failure；
- stale generation；
- quarantine bytes/drop；
- guard samples/coverage；
- poison bytes；
- metadata invariant failure；
- recursion/bootstrap fallback；
- last N fixed security events；
- profile/config fingerprint。

地址导出可按安全策略哈希/脱敏。

### 12.16 测试与 Fuzz

- 每种 invalid free；
- 同线程/跨线程 double free；
- freelist next corruption；
- chain cycle；
- canary/redzone；
- UAF quarantine；
- guard under/overflow；
- descriptor reuse/generation；
- corrupted size/owner/region；
- integer overflow；
- calloc/realloc；
- recursion/OOM；
- fork/shutdown；
- fuzz public ABI、control parser、batch chain、extent tree；
- ASan/HWASan 与 allocator hardening 组合矩阵；
- death test 统一匹配 fail-fast；
- ReleaseFast/Checked/Hardened 性能和 RSS 对比。

### 12.17 分阶段实施与验收

#### 阶段 A：默认安全基线

1. checked arithmetic；
2. public free pointer/class/base validation；
3. stable descriptor generation；
4. 明确 unknown pointer 策略；
5. bootstrap-safe fail-fast；
6. 错误事件记录。

退出条件：已知 invalid/double/stale 路径不静默污染 heap；fast-path 护栏通过。

风险类型：正确性、安全、性能。

#### 阶段 B：ReleaseChecked

1. pointer encoding；
2. sampled chain validation；
3. poison sampling；
4. large canary；
5. bounded quarantine；
6. 配置/统计。

退出条件：攻击面明显收窄；默认开销和 RSS 达标；pressure 可 drain。

风险类型：安全、性能、内存。

#### 阶段 C：Hardened/Guarded

1. redzone/canary class；
2. guarded sampling；
3. metadata protection；
4. stack capture；
5. UAF crash record；
6. production canary。

退出条件：典型 overflow/UAF 可稳定捕获；sample/drop/成本可观测。

风险类型：安全、内存、性能、运维。

#### 阶段 D：持续安全验证

1. fuzz farm；
2. sanitizer/nightly；
3. corpus regression；
4. aethermind profile 灰度；
5. 安全响应和版本策略。

退出条件：漏洞样本进入回归；安全 profile 可回滚；默认 ABI 不漂移。

风险类型：安全、工程、兼容性。

## 13. 测试与验证体系

验证体系必须证明四件事：API 行为正确、并发状态可证明、内存长期守恒、性能结论可复现。单元测试“运行未崩溃”不能证明分配器正确；单个平均 ns 也不能证明可部署。

### 13.1 验证金字塔

~~~text
Compile-time invariants
  -> deterministic unit tests
  -> state-model / fault-injection tests
  -> sanitizer / fuzz / stress
  -> ABI & preload compatibility
  -> microbenchmarks
  -> workload/trace benchmarks
  -> aethermind staging & production canary
~~~

低层失败阻断高层；性能优化必须同时通过相关正确性层。

### 13.2 Compile-time 验证

- C++20 feature；
- `sizeof/alignof` Span、Bucket、ThreadCache、shard；
- cache-line offsets；
- atomic lock-free 要求；
- VA/PageMap 几何；
- size-class 完整互逆；
- batch/page table；
- enum/flag 不重叠；
- public C struct size/version；
- feature macro 一致；
- exception/RTTI/visibility profile；
- no-copy/no-move；
- checked max constants；
- 48/57-bit 构建。

### 13.3 单元测试组织

现有单个 `ammalloc_unit_tests` 可继续，但建议按 filter/tag 建立逻辑套件：

- `SizeClass*`；
- `FreeList/ThreadCache*`；
- `Central/Transfer*`；
- `Span/PageMap*`；
- `PageCache/Extent*`；
- `PageAllocator/Scavenger*`；
- `Abi/Bootstrap*`；
- `Stats/Profile*`；
- `Security*`；
- `Numa/Arena*`。

测试 fixture 必须恢复 singleton 状态；reset/drain/destroy 语义分离；测试不依赖执行顺序。

### 13.4 公共 ABI 测试

覆盖 C：

- malloc/free/calloc/realloc/reallocarray；
- aligned_alloc/posix_memalign/memalign/valloc/pvalloc（按支持范围）；
- malloc_usable_size；
- zero-size；
- SIZE_MAX/overflow；
- errno；
- alignment；
- realloc failure atomicity；
- foreign/interior/double pointer 策略。

覆盖 C++：

- scalar/array new/delete；
- nothrow；
- sized delete；
- aligned new/delete；
- constructor throw cleanup；
- `new_handler`；
- DSO 边界；
- exception 跨库；
- static constructor/destructor。

ABI 测试使用独立 C/C++ 可执行文件，不只从库内部调用。

### 13.5 层级所有权与守恒测试

每次操作后可选 exact checker：

- object 在唯一状态；
- FreeList count/chain；
- bitmap/use_count；
- TransferCache/Frontend bytes；
- Span list membership；
- PageMap range；
- PageCache bucket/index；
- region accounting；
- metadata live/free/retired；
- mapped/unmapped；
- arena/node owner。

测试 checker 可以慢且使用 STL reference model，但核心库不能。

### 13.6 状态模型测试

为 Span/Extent 建小状态机模型：

操作：

- allocate exact/split/refill；
- small init/object alloc/free；
- cache transfer；
- release/coalesce；
- purge/reuse/unmap；
- reset/shutdown；
- failure at every commit point。

模型随机生成短序列并与实现快照比较。失败输出 seed 和最小操作序列。

### 13.7 并发测试

#### 13.7.1 场景

- same/different size class；
- same/different Central shard；
- same/different PageCache shard；
- PageMap read vs split/coalesce/unmap；
- ThreadCache exit vs remote free；
- refill single-flight；
- Scavenger detach；
- stats snapshot/config update；
- huge cache Put/Get/drain；
- fork freeze；
- shutdown。

#### 13.7.2 同步控制

用 barrier/latch/test hook 将线程暂停在：

- leaf publish 前后；
- descriptor retire 前；
- shard unlock/OS call/relock；
- TransferCache publish；
- owner closing；
- TLS destructor；
- madvise detached；
- config generation。

不要只依赖随机调度碰撞竞态。

#### 13.7.3 线性化与结果

对 lock-free/rseq/remote queue 定义线性化点；测试允许的结果集合。无法说明线性化点的结构不能因“压力测试没崩”而上线。

### 13.8 故障注入框架

统一 failpoint id：

- metadata chunk N；
- RadixNode N；
- Span descriptor N；
- ThreadCache N；
- Transfer backing；
- mmap errno；
- munmap/madvise/mbind；
- thread creation；
- rseq registration；
- profile record；
- topology read；
- config publish。

支持：

- fail next；
- fail Nth；
- fail range；
- fail probability（固定 seed）；
- delay/barrier；
- error code；
- per-thread scope。

failpoint 自身无分配、显式 atomic memory order，生产构建完全移除或默认不可达。

### 13.9 Fuzz 与随机压力

Fuzz targets：

- size/alignment/API sequence；
- realloc/calloc；
- FreeList/ObjectBatch；
- bitmap bulk；
- Span split/coalesce；
- extent tree；
- PageMap range；
- control parser；
- stats exporter；
- arena lifecycle；
- invalid pointer hardening。

每个 corpus 保存 allocator config、seed、VA mode。长时间 stress 使用固定 seed 列表 + 新随机 seed；失败自动保存最小复现。

### 13.10 Sanitizer 与动态工具

矩阵：

- ASan：allocator 内部越界/UAF；
- UBSan：overflow、shift、alignment、enum；
- TSan：锁外状态、PageMap writer、shutdown；
- LSan 或自有 mapping/accounting leak；
- MSan（可用时）：未初始化 metadata；
- valgrind/helgrind 作为补充而非主门禁；
- clang static analyzer；
- clang-tidy 仅作为质量信号；
- perf/ftrace/bpf/syscall trace；
- debug page protection。

自定义 allocator 与 ASan interposition 可能冲突，应分别测试“库内部 ASan”和“allocator 接管目标进程”配置。

### 13.11 平台与工具链矩阵

最低：

| 维度 | 配置 |
|---|---|
| Compiler | GCC、Clang，明确最低版本 |
| Arch | x86-64、AArch64 |
| VA | 48-bit、57-bit compile；57-bit runtime 环境另测 |
| Build | Debug、Release、ReleaseChecked、Sanitizer |
| Link | shared、static（支持后）、preload |
| Kernel | 最低支持版本、当前 LTS、生产版本 |
| libc | glibc 版本矩阵；其他 libc 明确 unsupported/experimental |
| Page | 4 KiB；AArch64 其他 base page size需能力探测或拒绝 |
| THP | never/madvise/always |
| NUMA | single、dual socket |
| cgroup | unlimited、memory.high、memory.max |

不能在 `SystemConfig::PAGE_SIZE=4096` 时宣称支持非 4KiB 基页平台。

### 13.12 Preload 与真实程序兼容

测试：

- 小型 C/C++ ABI exerciser；
- shell 工具和多线程程序；
- DSO/plugin load/unload；
- Python/Java 等复杂 runtime（仅在支持范围）；
- aethermind；
- early constructor；
- dlopen allocator；
- fork/exec；
- signal handler；
- static TLS；
- mixed allocator 负向场景。

用 `LD_DEBUG`、符号表和 backtrace 验证实际命中 ammalloc，而不是测试误用 glibc。

### 13.13 内存与碎片测试

- requested/usable/active/mapped/RSS 守恒；
- 所有 size class 内部碎片；
- random/adversarial large extent；
- total free 足够但 fit 失败；
- burst 后 decay；
- retained VA；
- metadata growth；
- high thread count cache；
- hugepage breakage；
- model/request arena reset；
- peak realized fragmentation。

输出时间序列和 heap snapshots。

### 13.14 性能基准体系

#### 13.14.1 微基准

- SizeClass；
- ThreadCache hit；
- Transfer hit/miss；
- bitmap batch；
- PageMap hit/miss/range publish；
- PageCache exact/split/coalesce；
- PageAllocator mmap/huge cache；
- stats/sampling；
- hardening。

#### 13.14.2 合成 workload

- fixed-size churn；
- random size；
- deep churn；
- multithread scaling；
- producer/consumer；
- short threads；
- mixed small/large；
- realloc；
- burst/idle；
- remote NUMA；
- pressure；
- hugepage filler。

#### 13.14.3 Trace replay

trace 记录：

- relative time；
- alloc/free/realloc；
- size/alignment；
- thread/CPU/node；
- allocation id；
- arena/tag；
- lifetime；
- phase marker。

脱敏且不保存原始地址/用户数据。Replay 保持依赖关系，可缩放时间/线程，但报告变换。

#### 13.14.4 竞争者

相同 workload 比较：

- ammalloc；
- glibc malloc；
- TCMalloc；
- jemalloc。

严格记录版本、配置、page size、THP、background thread、per-CPU/arena 等，不使用默认配置差异得出不公平结论。

### 13.15 性能测量规范

- 固定 CPU affinity；
- 固定/记录 governor、turbo、SMT；
- 预热；
- 多轮交错 A/B；
- 报告 median、MAD/置信区间；
- Google Benchmark JSON；
- raw perf stat；
- kernel、BIOS、microcode；
- 环境温度/后台负载；
- 分离 wall time 与 CPU time；
- p50/p95/p99/p999；
- 吞吐与 bytes/s；
- RSS/VSS/page faults/syscalls；
- perf c2c/LLC/TLB；
- compiler flags/LTO。

3.8 ns、26.0 ns、8.9 us/100+ GiB/s 只有在原始机器和统计口径完整时才是有效护栏；迁移机器后建立归一化新基线，旧值保留历史对照。

### 13.16 性能回归门禁

每个 benchmark 定义：

- primary metric；
- allowed regression；
- noise floor；
- min iterations/time；
- machine class；
- blocking/advisory；
- related memory cap。

建议：

- fast path：最严格，任何噪声外退化阻断；
- slow path：按场景阈值；
- throughput：同时检查 p99；
- RSS：同时检查 latency；
- sampling/hardened：与对应 profile 基线比；
- 竞争者比较不作为每 commit 门禁，只做周期报告。

### 13.17 CI 分层

#### Presubmit

- format/build；
- focused unit；
- ABI compile；
- Debug/Release；
- 快速 ASan/UBSan；
- diff-based microbench smoke。

#### Postsubmit

- full unit；
- TSan；
- fuzz short；
- 48/57-bit compile；
- GCC/Clang；
- benchmark stable host。

#### Nightly

- long stress/fuzz；
- cgroup pressure；
- fork/preload；
- multi-shard；
- NUMA/THP；
- leak/accounting；
- aethermind trace。

#### Release qualification

- 全平台矩阵；
- ABI compatibility；
- competitive benchmark；
- staged preload；
- failure injection；
- rollback rehearsal；
- artifact/signing。

### 13.18 结果工件与可追溯性

每次关键验证保存：

- git commit/config hash；
- build manifest；
- compiler/kernel/hardware；
- test list/result；
- sanitizer log；
- benchmark JSON；
- perf data/summary；
- RSS trace；
- allocator stats snapshot；
- known deviations；
- random seeds；
- aethermind workload version。

工件有保留期和对比工具，不只在 PR 评论粘贴摘要。

### 13.19 失败分级

| 类型 | 处理 |
|---|---|
| 内存安全/数据竞争 | P0，阻断合入/发布 |
| ABI/递归/初始化失败 | P0 |
| 守恒/泄漏 | P0/P1，按可达性 |
| Fast-path 回归 | P1，阻断性能变更 |
| 尾延迟/RSS 回归 | P1 |
| 统计不一致 | P1/P2 |
| 可选 profile failure | P2，但不能影响默认模式 |
| benchmark noise | 重新测量，不直接下结论 |

### 13.20 分阶段实施

#### 阶段 A

整理测试 reset、failpoint、ABI exerciser、ASan/UBSan、基准环境 manifest。

#### 阶段 B

状态模型、deterministic concurrency、TSan、fuzz、accounting checker。

#### 阶段 C

preload/平台/cgroup/NUMA/THP、trace replay、竞争者比较。

#### 阶段 D

aethermind staging、生产 canary、长期回归趋势和自动二分。

每阶段退出条件是相应风险可自动复现和阻断，而不是测试数量达到某个数字。

## 14. 工程化与发布

allocator 是进程级基础设施，发布工程必须比普通工具库更严格：符号、初始化、配置、依赖、回滚和诊断任何一项失误都可能使目标进程无法启动。

### 14.1 当前工程基线与差距

当前：

- C++20 shared library；
- `GLOB_RECURSE` 自动收集源码/测试/基准；
- FetchContent 获取 spdlog/gtest/benchmark；
- 测试和基准各一个可执行文件；
- BUILD_TESTS/BUILD_BENCHMARKS/USE_57BIT_VA；
- 无 install/export/package；
- 无 VERSION/SOVERSION/SONAME 策略；
- 无 hidden visibility/export macro；
- `AMMALLOC_TEST` 作为 PUBLIC definition 传播给库和消费者；
- core 链接 spdlog/std::format；
- 配置环境变量少且无 schema；
- 无 C ABI、pkg-config/CMake package；
- 无静态库/preload 专用 target；
- 无 sanitizer/hardening build profiles；
- 无发布流水线和 ABI 检查。

### 14.2 Target 分层

建议：

| Target | 用途 |
|---|---|
| `ammalloc_core` | 内部实现对象库/静态内部 target |
| `ammalloc` | 显式 C/C++ API shared library |
| `ammalloc_preload` | 导出 malloc family/interposition |
| `ammalloc_static` | 明确支持时提供静态链接 |
| `ammalloc_test_support` | failpoint/introspection，仅测试 |
| `ammalloc_unit_tests` | 单元 |
| `ammalloc_benchmarks` | 基准 |
| `ammalloc_abi_tests` | 独立 ABI executables |

避免生产库因 BUILD_TESTS 改变公共头或 ABI。test hooks 通过独立内部接口和 target。

### 14.3 核心依赖治理

核心运行时只依赖：

- libc/syscall；
- pthread/必要 C++ runtime；
- 项目自身无分配组件。

移除：

- spdlog/std::format；
- 可能懒初始化/分配的 iostream/locale；
- 核心路径异常依赖（按选定政策）；
- 非必要第三方 runtime。

测试/工具可继续依赖 gtest/benchmark/格式化库。依赖版本锁定，支持离线/系统包/镜像，不能让发布构建强依赖网络 FetchContent。

### 14.4 符号与可见性

- 默认 `CXX_VISIBILITY_PRESET hidden`；
- `VISIBILITY_INLINES_HIDDEN`；
- `AMMALLOC_API` 显式导出；
- preload target 使用 version script；
- 只导出批准的 C ABI 和 operator new/delete；
- 内部符号不泄露；
- `nm/readelf` 自动检查；
- 避免与 libc 私有符号冲突；
- symbol versioning/compat policy；
- static link duplicate allocator 检测；
- LTO 不删除/合并必须导出的符号。

### 14.5 ABI 与版本

采用 SemVer 但明确：

- public ABI major；
- control schema version；
- stats schema version；
- arena handle version；
- build feature bitmap；
- allocator config fingerprint。

设置 library VERSION/SOVERSION；兼容 minor 只追加能力，不改变 struct 已有字段语义。C struct 使用 `struct_size`/`version`，opaque handle 不暴露 C++ layout。

### 14.6 构建 Profile

| Profile | 主要选项 |
|---|---|
| ReleaseFast | O3/LTO 可选、NDEBUG、minimal stats |
| ReleaseChecked | 低成本 hardening/stats |
| Debug | assertions、poison、invariants |
| ASan/UBSan | sanitizer compatible |
| TSan | 禁用不兼容 rseq/asm 或提供替代 |
| Fuzz | libFuzzer、failpoint |
| Benchmark | Release、符号保留、稳定 flags |

禁止测试 profile 的宏泄漏到安装头。编译器优化和 frame pointer 设置写入 build manifest。

### 14.7 配置体系

#### 14.7.1 类型

- compile capability；
- immutable startup geometry；
- runtime policy；
- action command；
- test-only failpoint。

#### 14.7.2 解析

- 只在 bootstrap-safe 初始化期解析环境；
- 固定 key table；
- checked numeric/size/bool/enum；
- unknown/invalid 记录；
- 不使用 string/map；
- 配置快照不可变；
- 敏感生产配置允许禁用环境变量并由 API 注入。

#### 14.7.3 输出

可查询 effective value、source、default、range、mutable、generation。性能报告必须包含 config fingerprint。

### 14.8 安装与包

提供：

- versioned shared library；
- public headers；
- CMake config/targets；
- pkg-config；
- license/notice；
- symbol version script；
- optional preload library；
- debug symbols 分离；
- install RPATH 策略；
- uninstall manifest；
- package checksum/SBOM；
- Debian/RPM/容器产物按需要。

安装不包含 test-only headers/macros。

### 14.9 平台能力探测

CMake configure 检查：

- architecture；
- base page size假设；
- 64-bit atomic lock-free；
- rseq headers/syscall；
- `MADV_FREE/HUGEPAGE/COLLAPSE`；
- `MAP_FIXED_NOREPLACE/HUGETLB`；
- NUMA policy syscall/lib；
- pthread_atfork；
- symbol versioning；
- visibility；
- sanitizer；
- 48/57-bit。

运行时再次检查内核/权限。编译成功不等于功能可用。

### 14.10 可复现构建

- 固定依赖 tag/hash；
- source archive vendor/lock；
- deterministic archive；
- SOURCE_DATE_EPOCH；
- build-id；
- compiler/linker manifest；
- no build path 泄漏或 prefix-map；
- CI/container toolchain image；
- artifact checksum/signature；
- release commit clean；
- generated size-class/schema 文件可重现并在 CI diff。

### 14.11 文档与仓库一致性

每个发布检查：

- AGENTS.md 目录/命令/目标；
- docs/designs 与实际状态；
- docs/guides；
- README/API；
- CMake options/target names；
- test filters；
- benchmark names；
- environment/control keys；
- platform matrix；
- aethermind 名称与集成边界；
- 无陈旧 aethermind 旧仓库链接/target；
- 已完成任务的状态标记与代码一致。

使用脚本检查链接、target、选项和文件引用；设计文档区分 current/future。

### 14.12 ADR 与不变量治理

关键决策建立 ADR：

- Span stable metadata vs epoch；
- PageMap writer；
- per-thread vs per-CPU；
- Transfer pointer vs descriptor；
- region/extent tree；
- THP/hugetlb；
- invalid free；
- exception policy；
- preload/static；
- NUMA；
- stats/control ABI。

每个并发模块在 header/design 中维护：

- ownership；
- state machine；
- locks/order；
- atomics/order；
- linearization points；
- reset/shutdown；
- failure atomicity；
- performance invariants。

### 14.13 Code review 与变更门禁

PR 模板要求：

- scope；
- affected invariants；
- recursion audit；
- lock/memory-order audit；
- failure paths；
- tests；
- benchmark before/after；
- RSS/metadata；
- ABI/symbol impact；
- config/doc；
- rollout/rollback。

高风险模块需要至少一名 allocator/concurrency reviewer。机械文档改动无需性能基准，但必须过链接/一致性检查。

### 14.14 Release Pipeline

阶段：

1. source/build verification；
2. full CI/sanitizer/fuzz；
3. ABI/symbol diff；
4. benchmark qualification；
5. package/sign；
6. explicit API staging；
7. aethermind opt-in；
8. selected preload canary；
9. wider rollout；
10. release notes/known issues。

每阶段有停止条件和 artifact。

### 14.15 灰度与回滚

回滚开关：

- 构建时 allocator choice；
- 进程启动选择；
- explicit API vs preload；
- per-CPU/NUMA/hugepage/profile；
- background reclaim；
- model/request arena；
- hardening。

约束：

- 同一指针必须由原 domain free；
- 进程运行中不能无协议切换系统 allocator；
- 配置回退不改变已分配对象 metadata 解释；
- crash-loop 检测可由部署系统回退；
- 保存 last-known-good config；
- canary 指标覆盖 crash、OOM、RSS、latency、throughput。

### 14.16 安全与供应链

- SBOM；
- 依赖 CVE；
- signed release；
- reproducible hash；
- fuzz/sanitizer 结果；
- hardening flags；
- symbol exposure；
- no unexpected constructors；
- control API 权限；
- env injection 风险；
- debug/profile 数据隐私；
- 漏洞响应和 backport policy。

### 14.17 运行手册

包含：

- 启用/禁用；
- preload 诊断；
- 收集 stats/profile；
- OOM/RSS 高；
- Scavenger 不工作；
- NUMA remote 高；
- THP/hugetlb；
- invalid free crash；
- fork/deadlock；
- performance regression；
- core dump；
- 回滚；
- 版本/config fingerprint。

### 14.18 发布验收清单

- P0/P1 缺陷清零或有批准豁免；
- ABI/symbol stable；
- sanitizer/TSan/fuzz；
- baseline/competitive/aethermind；
- peak/steady RSS；
- fault injection；
- cgroup/NUMA/THP；
- docs/current state；
- package/install；
- rollback rehearsal；
- on-call/runbook；
- artifact archived。

### 14.19 分阶段实施

#### 阶段 A：可维护开发库

移除 core spdlog、隔离 test macro、target/profile 分层、manifest 和文档检查。

#### 阶段 B：稳定显式 API 包

visibility、VERSION/SOVERSION、install/export、ABI test、signed artifacts。

#### 阶段 C：Preload-ready

version script、constructor/bootstrap、compat matrix、symbol audit、rollback。

#### 阶段 D：生产基础设施

release train、aethermind canary、SBOM、安全响应、长期性能趋势。

每阶段风险：兼容性、工程、性能、运维。

## 15. 分阶段实施路线图

路线图以依赖关系和风险消减排序，不按功能吸引力排序。per-CPU、NUMA、HugepageFiller 等高阶优化不得绕过 ABI、自举、生命周期和基准基础。

### 15.1 总体依赖图

~~~text
Baseline & test truth
  -> ABI correctness / bootstrap
  -> PageMap + Span stable lifetime
  -> Frontend/Central lifecycle
  -> PageCache transactional correctness
  -> stats/accounting/fault injection
  -> preload-ready explicit allocator
  -> cache budgets + decay + OS-out-of-lock
  -> Central/PageCache sharding + region
  -> NUMA / per-CPU / hugepage experiments
  -> aethermind arenas + production default evaluation
~~~

旁路依赖：

- 安全基线贯穿所有阶段；
- 工程发布从第一阶段开始；
- sampling 在统计 schema 稳定后开始；
- Hardened/guarded 在默认生命周期正确后开始；
- competitive benchmark 在可复现环境建立后持续运行。

### 15.2 Phase 0：基线冻结与事实修正

目标：让后续结果可相信。

工作包：

1. 修复 CentralCache Reset 导致 TransferCache 测试假覆盖；
2. 区分 drain/reset/destroy；
3. 整理测试 fixture 和 singleton 生命周期；
4. 建立 Debug/Release/ASan/UBSan/TSan；
5. 建立 benchmark manifest/JSON/perf；
6. 校准 3.8 ns、26 ns、8.9 us/100+ GiB/s；
7. 标记 docs current/future；
8. 建立 failpoint 基础和 accounting snapshot；
9. 清理构建 target/宏不一致；
10. 冻结当前 public API 行为。

退出门：

- 测试实际覆盖声称路径；
- baseline 多轮可复现；
- 工作区/构建说明一致；
- 所有已知阻塞性审核问题有 owner 和方案。

风险：工程、性能测量、正确性。

### 15.3 Phase 1：显式分配器正确性基线

目标：`am_malloc/am_free` 可安全作为显式 allocator 使用。

工作包：

1. TLS RAII、创建 OOM、线程退出；
2. PageMap writer lock；
3. stable Span metadata；
4. split/coalesce 失败原子；
5. public pointer/large base validation；
6. Scavenger explicit detached state、stop/reset；
7. bootstrap-safe core diagnostics；
8. ThreadCache/Central/PageCache byte accounting；
9. multi-shard 保持关闭但修正 owner 输入；
10. fuzz/state-model 初版。

退出门：

- 无已知 reachable UAF、double allocation、mapping stale；
- OOM/failpoint 守恒；
- ASan/UBSan/TSan；
- 短线程/fork/shutdown；
- fast path 与随机大小护栏；
- 显式 API 长时间 stress。

交付：v0.x explicit-preview。

### 15.4 Phase 2：完整 ABI 与自举

目标：形成可测试的 malloc replacement，但不默认生产启用。

工作包：

1. C malloc family；
2. C++ new/delete family；
3. errno/zero/alignment/realloc；
4. BootstrapAllocator/recursion state；
5. sized free；
6. symbol visibility/version script；
7. preload target；
8. constructor/destructor/fork；
9. core 移除 spdlog；
10. ABI exerciser/real-program smoke。

退出门：

- ABI 测试全通过；
- early/late/reentrant allocation；
- preload 主流样本；
- domain mismatch 明确；
- public symbols准确；
- 可通过进程启动配置回退系统 allocator；
- 生产仍 opt-in。

交付：v0.x preload-preview。

### 15.5 Phase 3：内存效率与控制面

目标：在不改变架构并发复杂度的前提下治理 RSS 和缓存。

工作包：

1. Frontend total byte budget/GC；
2. Middle-end budget/adaptive prefetch；
3. bulk bitmap；
4. incremental Scavenger/decay；
5. retained/purged state；
6. stats/control schema；
7. cgroup/pressure；
8. large metadata/aligned/realloc；
9. OS syscall 移出 shard lock；
10. sampling 初版。

退出门：

- burst RSS 回落；
- refault/latency 可控；
- stats accounting 可信；
- pressure 无振荡；
- slow-path p99 改善或不退化；
- sampling 默认开销达标。

交付：v1.0 explicit-stable 候选；preload 仍灰度。

### 15.6 Phase 4：并发分片与 LargeExtent

目标：移除热点 Middle/Backend 单锁上限并降低中大型 mmap churn。

工作包：

1. Central size-class shard；
2. ObjectBatch/稳定 route；
3. refill single-flight；
4. PageCache multi-shard owner 发布；
5. PageMap multi-writer；
6. region ownership；
7. SmallRun non-empty bitmap；
8. intrusive LargeExtent size/address index；
9. fit/fragmentation；
10. multi-shard Scavenger。

退出门：

- 2～4 shard correctness/TSan；
- same-class/Backend contention 缩放；
- 无跨 shard coalesce；
- mmap/munmap 和外部碎片下降；
- metadata/retained budget；
- 16/32 thread 综合护栏。

交付：v1.x scalable-backend。

### 15.7 Phase 5：NUMA、per-CPU 与 Hugepage 实验

目标：在稳定架构上进行可归因的高阶性能实验。

并行但独立 feature flags：

- per-CPU/rseq Frontend；
- NUMA-local Central/PageCache region；
- remote free queue；
- HugepageFiller；
- HugeRegion；
- THP/hugetlb policy；
- descriptor TransferCache；
- logical page size profile。

每个实验：

1. 独立设计/不变量；
2. 独立基准；
3. 默认关闭；
4. 与现有模式 A/B；
5. 失败/unsupported 回退；
6. 不同时合并多个不可归因变化。

退出门：

- 真实 workload 综合收益；
- 尾延迟、RSS、metadata 不显著恶化；
- 平台兼容；
- 一键回退。

### 15.8 Phase 6：aethermind 内存基础设施

目标：按推理生命周期而不是通用 malloc 猜测来优化。

工作包：

1. versioned arena/domain；
2. request segment arena；
3. model arena；
4. KV/HugeRegion block pool；
5. worker/node route与 first touch；
6. pressure priority；
7. model/request sampling；
8. device/pinned domain wrapper；
9. aethermind exporter；
10. unload/reset/runbook。

退出门：

- tokens/s、TTFT/TPOT、p99、peak/steady RSS 综合收益；
- domain/reset 无 UAF；
- multi-model；
- cgroup pressure；
- canary 和 rollback；
- 运维可诊断。

交付：aethermind opt-in -> selected default。

### 15.9 Phase 7：安全与生产成熟

工作包：

- ReleaseChecked；
- pointer encoding；
- guarded sampling；
- quarantine；
- metadata protection；
- fuzz/sanitizer farm；
- ABI stability；
- package/sign/SBOM；
- LTS/backport；
- production SLO；
- incident response。

退出门：

- 默认与 Hardened profile 都有稳定 SLO；
- 漏洞/崩溃可定位；
- release qualification 自动化；
- allocator 可作为批准范围内的默认基础设施。

### 15.10 工作流与 WIP 限制

- 同一时间最多一个 P0 生命周期重构处于未稳定状态；
- 性能 feature 不与 ABI/metadata layout 大改混合；
- 每个 phase 保持可构建、可测试；
- feature flag 不是永久技术债，决策后删除无用分支；
- milestone 结束做 docs/current-state 更新；
- benchmark 数据与 commit 绑定；
- 阻塞问题不以新增优化绕过。

### 15.11 每阶段统一 DoD

- 设计不变量；
- 实现方案获批；
- 单元/并发/故障测试；
- sanitizer；
- before/after benchmark；
- RSS/metadata；
- ABI/config/docs；
- observability；
- rollout/rollback；
- known limitations；
- reviewer sign-off。

## 16. 优先级与风险矩阵

优先级由“错误可导致什么”和“后续工作是否依赖它”共同决定。P0/P1 未关闭前，高阶性能项仅允许隔离研究，不进入默认实现。

### 16.1 定义

| 优先级 | 定义 |
|---|---|
| P0 | 可能导致 UAF、heap corruption、递归崩溃、错误 unmap、进程无法启动 |
| P1 | 阻碍安全部署、ABI 兼容、可靠 OOM、验证或关键性能护栏 |
| P2 | 扩展性、RSS、碎片和生产可观测性的主要缺口 |
| P3 | 高阶/平台特定优化，需数据证明 |

### 16.2 详细矩阵

| ID | 优先级 | 工作项 | 风险类型 | 依赖 | 主要验收 |
|---|---|---|---|---|---|
| R01 | P0 | PageMap reader 与 Span stable lifetime | 并发、内存 | 无 | stale reader/UAF 模型测试 |
| R02 | P0 | 多 writer 协议 | 并发 | R01 | multi-shard TSan/发布事务 |
| R03 | P0 | split/coalesce failure atomicity | 正确性、内存 | R01 | 每提交点 failpoint |
| R04 | P0 | TLS RAII/OOM/退出 | 正确性、内存 | 无 | 短线程/OOM/late TLS |
| R05 | P0 | Bootstrap/递归安全诊断 | 正确性、兼容性 | 无 | early/reentrant/preload |
| R06 | P0 | unknown/interior/double free 策略 | 安全、正确性 | R01 | ABI/hardening |
| R07 | P0 | Scavenger detached/stop/reset | 并发、正确性 | R01 | purge/reset/fork |
| R08 | P0 | owner 在首次发布前确定 | 正确性、并发 | R02 | non-zero shard |
| R09 | P0 | OS unmap 与 PageMap 顺序 | 内存、并发 | R01 | direct/large race |
| R10 | P1 | C/C++ ABI 完整性 | 兼容性、正确性 | R04-R06 | ABI exerciser |
| R11 | P1 | Central reset 测试假覆盖 | 测试、正确性 | 无 | Transfer hit diagnostics |
| R12 | P1 | 故障注入/状态模型 | 测试 | R01-R09 | deterministic reproduction |
| R13 | P1 | sanitizer/TSan/CI | 工程、正确性 | 基线 | 自动门禁 |
| R14 | P1 | 可复现性能基线 | 性能 | 无 | manifest/JSON/noise |
| R15 | P1 | core 移除 spdlog | 递归、工程 | R05 | symbol/dependency audit |
| R16 | P1 | stats/accounting schema | 内存、可观测性 | 生命周期 | 守恒/snapshot |
| R17 | P1 | OS syscall 移出 shard lock | 并发、性能 | R03/R08 | lock hold/p99 |
| R18 | P1 | release/install/visibility/ABI version | 兼容性、工程 | R10 | symbol/ABI/package |
| R19 | P2 | Frontend byte budget/GC | 内存、性能 | R16 | RSS/miss |
| R20 | P2 | Middle bulk bitmap/budget | 性能、内存 | R16 | cycles/lock |
| R21 | P2 | Incremental decay/pressure | 内存、性能 | R07/R16/R17 | burst RSS/refault |
| R22 | P2 | Central sharding | 并发、性能 | R02/R08 | same-class scaling |
| R23 | P2 | PageCache region/multi-shard | 并发、碎片 | R02/R08/R17 | region owner/scaling |
| R24 | P2 | LargeExtentSet | 内存、性能 | R03/R16/R23 | mmap/fragmentation |
| R25 | P2 | Sampling/control API | 可观测性、性能 | R16/R18 | overhead/profile |
| R26 | P2 | ReleaseChecked | 安全、性能 | R06/R16 | exploit checks/cost |
| R27 | P3 | per-CPU/rseq | 并发、性能 | R20/R22 | target workload |
| R28 | P3 | NUMA-local pipeline | 性能、兼容性 | R22-R24 | placement/remote |
| R29 | P3 | HugepageFiller/Region | 性能、内存 | R21/R23/R24 | THP/breakage |
| R30 | P3 | aethermind arenas | 正确性、性能 | R16/R18/R23 | lifecycle/E2E |
| R31 | P3 | Guarded sampling | 安全、内存 | R25/R26 | detection/overhead |

### 16.3 风险登记模板

每项记录：

- risk id/owner；
- affected invariant；
- trigger/workload；
- impact；
- likelihood；
- detection；
- mitigation；
- fallback；
- test/metric；
- status；
- target milestone。

### 16.4 优先级调整规则

- 任一真实 UAF/corruption 升 P0；
- benchmark 数字不可复现，相关性能结论降为未验证；
- 若功能阻断 preload/aethermind 灰度，至少 P1；
- 平台特定优化无真实 workload 收益保持 P3；
- 安全 profile 不得挤占默认生命周期 P0；
- P0 连续三次无法修复时重新评估架构，不以 workaround 掩盖。

## 17. 关键实施原则

### 17.1 正确性原则

1. **先定义状态和所有权，再选择锁或 atomic。**
2. **PageMap acquire/release 不替代 descriptor 生命周期。**
3. **每个对象、Span、Extent、Region 同时只有一个逻辑所有者。**
4. **失败原子：提交前旧状态完整，提交后新状态完整。**
5. **unmap 前撤销可发现性，metadata 回收晚于最后 reader。**
6. **Release 构建保留必要安全检查。**
7. **未知输入不能静默污染 heap。**

### 17.2 并发原则

1. ThreadCache 真正 thread-confined；
2. shared bitmap 采用 shard 锁 + 批量操作；
3. 不嵌套 Transfer/Central/PageCache 锁；
4. 普通操作只持一个 owner shard；
5. OS 调用在 allocator 锁外；
6. atomic 显式 memory order；
7. relaxed hint 必须锁内重验；
8. lock-free/rseq 必须给出线性化点和 fallback；
9. reset/shutdown 需要 quiescence；
10. fork 需要显式冻结协议。

### 17.3 热路径原则

1. 快路径 O(1)、无锁、少分支；
2. 不写共享统计；
3. 不调用配置解析、日志、系统调用；
4. SizeClass constexpr/table；
5. LIFO 保持 locality；
6. 复杂策略 noinline slow path；
7. 结构布局用反汇编和 perf 验证；
8. 禁止隐藏 O(N²)；
9. 新安全能力默认不无条件进入 3～5 ns 路径；
10. 每个额外 load/store 有基准依据。

### 17.4 内存与碎片原则

1. cache 有 bytes budget；
2. requested/usable/active/mapped/resident 分离；
3. retained VA 与 RSS 分离；
4. peak realized fragmentation 与 off-peak retention 分离；
5. split/coalesce owner-local；
6. LargeExtent 有 size/address 索引；
7. pressure 分层 drain；
8. purge 评估 refault；
9. hugepage 评估实际 backing/breakage；
10. metadata 也有预算。

### 17.5 自举原则

1. 核心 metadata 不用普通 heap；
2. 不用堆分配 STL；
3. placement new 不是 owning raw new；
4. core 诊断 async/bootstrap-safe；
5. 配置固定 buffer 解析；
6. profiler 不在线符号化；
7. background thread bootstrap 后启动；
8. recursion domain 明确；
9. OOM recovery 不分配新 metadata；
10. 第三方依赖不进入 core runtime。

### 17.6 API 与兼容原则

1. 标准 ABI 与扩展 arena API 分离；
2. C ABI versioned/opaque；
3. C++ 全套匹配 new/delete；
4. domain 不混用；
5. errno/zero/realloc/alignment 文档化；
6. config structural/policy/action 分离；
7. unsupported/fallback 明确；
8. public symbol 最小；
9. ABI minor 只兼容扩展；
10. preload 不允许运行中无协议切换。

### 17.7 观测与验证原则

1. 先 schema/口径，再堆 counter；
2. relaxed snapshot 不伪装强一致；
3. 高基数用 sampling；
4. exporter caller-buffer；
5. 每个优化有专项基准和端到端；
6. 数字带硬件/工具链/配置；
7. before/after 交错多轮；
8. 吞吐、尾延迟、RSS 同时验收；
9. failure injection 覆盖提交点；
10. 测试必须证明路径真实执行。

### 17.8 发布原则

1. 显式 API -> preload canary -> 受控默认；
2. feature 默认关闭再灰度；
3. 回滚不改变已有对象解释；
4. config fingerprint；
5. ABI/symbol 检查；
6. sanitizer/fuzz/benchmark 工件；
7. current/future docs 分离；
8. ADR 记录关键取舍；
9. production runbook；
10. allocator crash/RSS/SLO 可告警。

### 17.9 决策问题清单

每次设计评审必须回答：

- 谁拥有该对象/metadata？
- 哪把锁或哪段 thread/CPU confinement？
- reader 在返回后多久有效？
- 线性化/提交点在哪？
- OOM 在每一步发生会怎样？
- 会否递归 malloc？
- reset/fork/shutdown 如何处理？
- 内存/metadata 上限？
- 快路径多了哪些指令/cache line？
- 用什么测试和基准证明？
- 功能失败如何降级？
- 如何灰度和回滚？

## 18. 参考资料

参考资料用于理解设计空间，不表示 ammalloc 应直接复制实现。任何采用都需结合本项目 4 KiB page、PageMap、bitmap、ABI 和 aethermind workload 验证。

### 18.1 TCMalloc

- [TCMalloc Design](https://google.github.io/tcmalloc/design.html)：Frontend/Middle-end/Backend、per-CPU、TransferCache、PageHeap 和 HPAA 总览。
- [TCMalloc Performance Tuning](https://google.github.io/tcmalloc/tuning.html)：cache 与 memory release 调优边界。
- [TCMalloc Basic Reference](https://google.github.io/tcmalloc/reference.html)：C/C++ API、alignment、sized delete。
- [TCMalloc Statistics](https://google.github.io/tcmalloc/stats.html)：各层字节、metadata、PageMap 和 realized fragmentation 口径。
- [TCMalloc Restartable Sequences](https://google.github.io/tcmalloc/rseq.html)：per-CPU push/pop 约束。
- [TCMalloc GWP-ASan](https://google.github.io/tcmalloc/gwp-asan.html)：guarded sampling。

借鉴重点：

- per-thread/per-CPU mode 隔离；
- per-CPU total budget；
- Transfer/Central 分工；
- PageHeap small/large；
- hugepage filler/region/cache；
- low-overhead sampling；
- stats 分类。

不应盲目复制：

- TCMalloc logical page size；
- Linux/Google 内部平台假设；
- region 尺寸；
- size-class 表；
- rseq 默认化；
- metadata 回收细节。

### 18.2 jemalloc

- [jemalloc Manual](https://jemalloc.net/jemalloc.3.html)：mallctl namespace、arena、tcache、dirty/muzzy decay、background thread、profiling和 extent hooks。

借鉴重点：

- typed control namespace；
- stats epoch；
- per-arena decay；
- background thread 动态控制；
- allocation sampling；
- arena/extent 策略。

注意其启动后台线程文档明确提示初始化循环依赖风险，ammalloc 必须坚持 bootstrap 后启用和确定性生命周期。

### 18.3 Linux 内核

- [Transparent Hugepage Support](https://docs.kernel.org/admin-guide/mm/transhuge.html)：`MADV_HUGEPAGE`、THP 策略、collapse/defrag 与内存放大。
- [HugeTLB Pages](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html)：显式 hugetlb pool、预留和限制。
- [Control Group v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)：`memory.high/max/events/stat/pressure/reclaim`。
- [NUMA Memory Policy](https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html)：default/preferred/bind/interleave 语义。
- Linux man pages：`mmap(2)`、`munmap(2)`、`madvise(2)`、`mbind(2)`、`set_mempolicy(2)`、`get_mempolicy(2)`、`rseq(2)`、`pthread_atfork(3)`。

### 18.4 标准与 ABI

- ISO C malloc family 和 alignment/zero-size 语义；
- ISO C++ replaceable global allocation/deallocation functions；
- Itanium C++ ABI（适用平台）；
- System V AMD64 ABI；
- AArch64 ELF ABI；
- glibc malloc hooks/interposition 现状与动态链接器文档。

实现前应引用项目支持工具链对应版本，而不是仅依赖摘要。

### 18.5 并发与内存模型

- ISO C++ memory model；
- Linux rseq ABI；
- acquire/release publication；
- epoch/RCU 文献；
- ABA/tagged pointer；
- lock-order verification；
- allocator-specific non-moving reclamation 研究。

### 18.6 内部事实来源

优先级：

1. 当前代码与可复现实验；
2. AGENTS.md 硬约束；
3. current-state design；
4. tests/benchmarks；
5. ADR；
6. future plan；
7. 外部资料。

文档与代码冲突时先记录冲突，再按已验证代码事实修正文档。

### 18.7 资料维护

- 记录访问日期/版本；
- 对内核特性标最低版本；
- 外部链接定期检查；
- ADR 引用具体资料和采纳/拒绝原因；
- 不复制大段外部文本；
- 性能数字只引用本项目可复现实验。

## 19. 最终建议

### 19.1 核心结论

ammalloc 当前已经具备有价值的高性能骨架：ThreadCache LIFO、TransferCache、Span bitmap、PageMap、PageCache split/coalesce、PageAllocator 和初步 Scavenger。但它仍更接近“高性能显式 allocator 原型”，尚未达到可安全替换系统 malloc、也未达到与 TCMalloc/jemalloc 全面比较的工程成熟度。

最先决定项目成败的不是再增加一个无锁结构，而是：

1. PageMap/Span descriptor 生命周期；
2. 自举和完整 C/C++ ABI；
3. TLS/Reset/Shutdown/Fork；
4. owner 发布、锁外 OS 事务；
5. 测试真实覆盖、故障注入和统计守恒；
6. 可复现的 latency/throughput/RSS 基线。

### 19.2 推荐架构终态

~~~text
Standard malloc/new ABI        aethermind explicit arena API
          |                              |
          +--------------+---------------+
                         v
        Frontend: per-thread baseline / per-CPU optional
                         |
        Middle-end: ObjectBatch + Transfer + Central shards
                         |
        Backend: node-local region owner shards
          | SmallRun | LargeExtent | DirectMapped |
          | HugeFiller/Region optional             |
                         |
        PageAllocator: map/purge/unmap/NUMA policy

Cross-cutting:
  stable PageMap lifetime
  bootstrap-safe metadata
  accounting/control/sampling
  pressure/decay
  hardening
  test/benchmark/release
~~~

### 19.3 最近三个里程碑

#### 里程碑 1：可信显式 allocator

- 修复测试假覆盖；
- TLS/OOM；
- PageMap stable metadata/writer；
- split/coalesce transaction；
- Scavenger lifecycle；
- accounting/failpoint；
- baseline。

成功定义：显式 API 在 sanitizer、并发、OOM、长时间 churn 下可靠，且现有性能护栏不退化。

#### 里程碑 2：Preload-ready

- 完整 ABI；
- bootstrap；
- core 无高层依赖；
- symbols/visibility；
- fork/teardown；
- real-program compatibility；
- rollback。

成功定义：可在批准测试程序安全 preload，但仍不是全局默认。

#### 里程碑 3：可扩展与可治理

- cache budgets；
- incremental decay；
- OS-out-of-lock；
- Central/PageCache shards；
- LargeExtent/region；
- stats/control/sampling；
- aethermind explicit arenas。

成功定义：吞吐、尾延迟、RSS 和诊断能力达到可与主流 allocator 公平比较。

### 19.4 暂不默认投入的项目

在前三个里程碑完成前，以下只做隔离研究：

- per-CPU/rseq；
- lock-free bitmap；
- epoch descriptor reuse；
- HugepageFiller/HugeRegion；
- explicit hugetlb 默认路径；
- 复杂 remote-owner queue；
- 每请求完整 arena；
- 运行中 allocator domain 切换。

原因不是这些方向无价值，而是其正确性、验证和收益依赖尚未建立。

### 19.5 与主流分配器的对标方法

“对标”定义为：

- API/ABI 兼容范围明确；
- 同硬件、工具链、workload、THP/NUMA；
- 相同 sampling/background 配置；
- latency p50/p99/p999；
- throughput；
- peak/steady RSS；
- internal/external/realized fragmentation；
- metadata；
- syscalls/faults/locks/TLB；
- aethermind tokens/s、TTFT/TPOT；
- 完整原始工件。

不要求每个微基准都第一；目标是针对 aethermind 和通用服务 workload 的综合 Pareto 改善。

### 19.6 组织与执行建议

- 为 P0 lifecycle/ABI 指定明确 owner；
- 性能、并发、ABI、安全评审角色分离；
- 每个阶段只引入可归因复杂度；
- 维护风险登记和 ADR；
- 建立稳定 benchmark host；
- 真实 workload trace 由 aethermind 团队共同维护；
- 发布与 on-call 在 preload 前介入；
- 文档每 milestone 更新 current state；
- 所有大改先设计评审再写代码，遵守 AGENTS.md 预审批。

### 19.7 最终判定

最合理的路线不是从当前实现直接跳到“默认替换 malloc + per-CPU + NUMA + hugepage”，而是：

~~~text
先可信
  -> 再兼容
  -> 再可观测和可治理
  -> 再扩展
  -> 最后针对 aethermind 做高阶优化和默认化
~~~

只有当正确性、ABI、自举、失败处理、生命周期、测试和发布体系共同成立，3.8 ns 级快路径才真正具有生产价值；只有当延迟、吞吐、RSS 和真实推理指标同时被测量，ammalloc 才能有意义地对标 TCMalloc、jemalloc，并成为 aethermind 的长期内存基础设施。
