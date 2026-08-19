# 第 4 章：正确性、自举与 ABI

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](02-target-architecture.md) · [下一章](04-pagemap-and-span-lifecycle.md)  
> **本章目标**：建立显式 allocator、标准 ABI 和动态拦截共同依赖的正确性与自举契约。  
> **适用范围**：公共 API、C/C++ ABI、初始化、TLS、OOM、对齐、realloc、fork 与 shutdown。  
> **核心 invariant**：失败原子、自举不递归、ownership 唯一且 ABI 语义完整。

本节定义 ammalloc 从“显式调用的 C++ 分配 API”演进到“可安全接管整个进程内存”的基础契约。这里的工作不是普通功能补齐，而是后续 PageCache 分片、per-CPU cache、NUMA 和 hugepage-aware backend 的前置门禁。任何违反本节契约的实现，即使在微基准中更快，也不能进入系统 malloc 替换模式。

## 4.1 目标、适用范围与硬性不变量

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

## 4.2 当前发布阻断项

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

## 4.3 初始化状态机

### 4.3.1 状态定义

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

### 4.3.2 状态转换

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

### 4.3.3 递归守卫

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

### 4.3.4 并发首次初始化

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

### 4.3.5 原子内存序

- 初始化者通过 CAS 从 `kUninitialized` 进入 `kInitializing`，成功序使用 `std::memory_order_acq_rel`，失败序使用 `std::memory_order_acquire`。
- 初始化完成后使用 `std::memory_order_release` 发布 `kReady` 或 `kDegraded`。
- 热路径使用 `std::memory_order_acquire` 读取 `kReady`，确保看到完整初始化的全局结构。
- 纯统计计数使用 `std::memory_order_relaxed`。
- 禁止依赖默认 `seq_cst`。

如果实测 acquire 状态读取对 3～5 ns 热路径有可见影响，可在证明对象发布关系仍成立的前提下设计独立的只读 ready fast flag，但不能先降级内存序再补正确性论证。

## 4.4 BootstrapAllocator 设计

### 4.4.1 职责与边界

BootstrapAllocator 只解决以下问题：

- allocator 核心初始化期间的递归分配；
- 其他线程与初始化并发时的临时分配；
- 核心初始化永久失败后的可控降级；
- shutdown 后不能再访问正常缓存时的必要分配。

它不是正常小对象快路径，也不承担长期高吞吐目标。因此可以接受每次分配一次 `mmap` 或使用简单锁，但必须正确、可释放、线程安全且不递归。

### 4.4.2 候选方案

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

### 4.4.3 Bootstrap block 元数据

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

### 4.4.4 所有权判定顺序

`free(ptr)` 建议使用以下逻辑：

1. `ptr == nullptr`：直接返回。
2. 快速检查正常 PageMap 是否存在合法 Span，存在则走正常释放。
3. 检查是否为 bootstrap block，存在则回 BootstrapAllocator。
4. 如果两者都不识别：
   - 默认兼容模式按“非法 free”为未定义行为，可在 Hardened 模式终止并输出无分配诊断；
   - 不建议自动转发给 libc `free`，否则容易掩盖 allocator 混用并重新引入动态解析递归。

若 bootstrap block 所在地址恰好被正常 PageMap 覆盖，必须通过 allocator domain/generation 消除歧义。最简单的做法是让 bootstrap region 与正常 allocator region 完全分离，并保证 PageMap 不注册 bootstrap 地址。

### 4.4.5 并发与信号边界

- BootstrapAllocator 至少需要支持多线程并发初始化场景。
- 可使用一个独立 TTAS lock 或原子 bump pointer；该锁不能与正常 allocator 锁形成嵌套。
- 不要求默认实现 async-signal-safe；如果未来支持信号处理器分配，需要单独设计固定容量的 signal-safe arena。
- 锁竞争、region 耗尽和 mmap 失败只能进入无分配失败报告，不能调用 spdlog 或 iostream。

## 4.5 初始化依赖和发布顺序

### 4.5.1 初始化依赖图

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

### 4.5.2 发布粒度

不建议分别发布多个“看起来已经可用”的全局单例。正常分配入口只能通过统一的 `kReady` 判断进入分层 allocator；在此之前，即使 PageMap 或 PageCache 已构造完成，也仍走 bootstrap 路径。

这种全有或全无的发布方式可以避免：

- PageMap 已可查询但 Span pool 尚未初始化；
- CentralCache 已暴露但 TransferCache backing 尚未准备；
- ThreadCache 已创建但其析构依赖尚未稳定；
- 初始化失败后留下部分可见的共享状态。

## 4.6 TLS、关闭与进程生命周期

### 4.6.1 ThreadCache RAII 所有权

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

### 4.6.2 生产进程退出策略

对系统 allocator，推荐以下策略：

- PageMap、PageCache、CentralCache 和 BootstrapAllocator 采用进程生命周期常驻对象；
- 不依赖 C++ 静态析构自动释放它们；OS 会在进程退出时回收地址空间；
- 显式停止 Scavenger，或者让其 `jthread`/pthread 生命周期有受控的退出钩子；
- 只有测试 runtime 或明确的嵌入式场景执行完整 destroy。

“退出时不逐页 munmap”不是内存泄漏缺陷，而是避免静态析构顺序 UAF 的有意策略。测试环境仍需要独立 runtime 或受控 Reset 来验证资源释放。

### 4.6.3 受控 shutdown 顺序

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

## 4.7 `fork()`、动态加载与静态链接

### 4.7.1 `fork()` 处理

多线程进程 `fork()` 后，子进程只保留调用 fork 的线程；其他线程持有的 allocator 锁不会自动恢复。因此需要注册 `pthread_atfork`：

- **prepare**：停止或冻结 Scavenger，按全局固定顺序取得 allocator 管理锁；
- **parent**：按逆序释放锁并恢复 Scavenger；
- **child**：重置锁、后台线程状态和仅属于消失线程的 cache 注册信息，只保留当前线程 TLS；
- child 中首次分配前重新建立必要的 runtime 状态。

如果第一阶段不支持多线程 fork，必须明确记录限制并在测试中检测，而不能静默死锁。

### 4.7.2 链接模式

建议输出两个清晰分离的目标：

- `libammalloc.so`：只导出 `am_*` 显式 API，适合早期接入和双 allocator 对比；
- `libammalloc_proxy.so`：导出标准 C/C++ 符号并链接相同核心，适合链接替换或 `LD_PRELOAD`。

这样可以避免为了 interposition 而让普通开发、单测和基准始终处于高风险符号环境。

### 4.7.3 动态加载限制

- 不支持在进程已经使用另一个 allocator 创建大量 live object 后再动态加载并接管 free。
- 不建议对未知指针通过 `dlsym(RTLD_NEXT, "free")` 自动转发；动态符号解析本身可能分配并递归。
- interposer 必须在进程启动早期加载，并保持到进程结束。
- allocator DSO 默认应设置为不可安全卸载；若确需 `dlclose`，必须证明所有 live allocation 和后台线程都已清空。

## 4.8 标准 C ABI 语义

### 4.8.1 接口集合

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

### 4.8.2 语义矩阵

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

### 4.8.3 `calloc` 清零策略

- 新 mmap 区域由内核保证零页，可避免重复 `memset`，但必须有明确的“fresh mapping”标志。
- 从 ThreadCache、CentralCache 或 retained extent 复用的对象必须显式清零请求可见区域。
- 清零超出 requested size 到 usable size 是否有收益，应由安全策略和基准决定。
- Hardened 模式的 free poisoning 与 calloc 清零必须协调，不能把 poison 当作已经清零。

### 4.8.4 `realloc` 原地优化

小对象：

- 新大小仍落在同一 size class 时可直接返回原地址；
- 新大小变小但跨 class 时，第一阶段也可保留原对象，避免迁移；
- 需要保证 `malloc_usable_size` 和统计能够反映实际 usable size。

大对象：

- 可尝试利用右侧连续 free extent 原地扩展；
- 可尝试释放尾部完整页完成原地收缩；
- PageMap 更新、extent 拆分和回滚必须在统一事务内完成；
- 第一阶段可以只实现“新分配 + copy + free”，优先保证正确性。

## 4.9 对齐契约

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

## 4.10 完整 C++ ABI

### 4.10.1 必需符号

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

### 4.10.2 失败语义

标准兼容模式建议遵循 C++ 语义：

- throwing new 在分配失败时调用当前 `new_handler`；
- handler 返回后重试，handler 抛出或为空时抛出 `std::bad_alloc`；
- nothrow new 捕获分配失败并返回 null；
- `operator delete(nullptr)` 为 no-op；
- delete 系列不得向外抛异常。

如果 aethermind 希望 OOM 直接终止以减少异常路径，可以提供独立构建策略或显式 `am_malloc_or_die`，但不能让标准 ABI 在未声明的情况下偏离语义。

### 4.10.3 Sized delete

Sized delete 是重要热路径优化：

1. 验证 size 是否落入合法 size class；
2. 直接计算 aligned size 和目标 ThreadCache bucket；
3. 必要时在 Debug/Hardened 模式与 PageMap 中记录的 class 交叉验证；
4. Release 默认路径跳过完整 pointer-to-size 查找；
5. 大对象或不可信 size 退回普通 PageMap 释放。

不能在调用方 size 错误时静默把对象放入错误 freelist。标准上 mismatched sized delete 属于调用方错误，但 Hardened 构建应尽可能检测。

## 4.11 符号、可见性与 allocator domain

### 4.11.1 符号导出

- 核心实现默认 hidden visibility；
- 只导出标准 ABI、稳定 `am_*` API 和必要控制接口；
- 使用 linker version script 固定符号集合；
- C API 使用 `extern "C"`；
- C++ ABI 符号由专用 translation unit 提供；
- 测试故障注入符号不得进入生产动态库。

### 4.11.2 allocator domain

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

## 4.12 整数安全与失败原子性

### 4.12.1 统一 checked arithmetic

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

### 4.12.2 各层失败契约

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

### 4.12.3 无分配错误报告

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

## 4.13 非法释放与安全策略分层

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

## 4.14 测试矩阵

### 4.14.1 自举与递归测试

- 在 RuntimeConfig、PageMap、CentralCache 和 Scavenger 初始化的每个阶段注入递归 malloc/free；
- 多线程同时执行第一次分配；
- 初始化线程暂停时，其他线程使用 BootstrapAllocator 并成功释放；
- 核心初始化失败后进入 `kDegraded`，基本 malloc/free 仍按策略工作；
- BootstrapAllocator region 耗尽和 mmap OOM；
- 错误报告路径自身不递归；
- 已有 bootstrap block 在 allocator ready 后仍能 realloc/free。

### 4.14.2 C ABI 测试

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

### 4.14.3 C++ ABI 测试

- 普通、数组、nothrow、sized、aligned new/delete；
- 构造函数抛出后对应 delete 路径正确；
- `new_handler` 重试、抛出和置空行为；
- over-aligned 类型；
- 编译器在不同优化级别下选择的 delete 符号；
- GCC/Clang 与 libstdc++/libc++ 组合的符号完整性。

### 4.14.4 生命周期与集成测试

- 大量短生命周期线程退出后缓存和 metadata 回落；
- Scavenger start/stop 与 TLS 析构并发；
- 多线程 `fork()` 后父子进程继续分配；
- 链接替换和 `LD_PRELOAD` 运行真实程序；
- 静态链接启动阶段的全局构造分配；
- DSO 构造/析构中的分配；
- 进程退出阶段的 late free；
- allocator 不同 domain 混用的 Hardened 检测。

### 4.14.5 动态工具

- ASan：越界、UAF 和错误回滚；
- UBSan：整数、位移、对齐和对象生命周期；
- TSan：状态机、PageMap、TLS 和 Scavenger；
- LeakSanitizer：线程退出和 bootstrap block；
- ABI symbol checker：导出符号与版本；
- 自定义 fault injection：mmap、munmap、madvise、线程创建和 metadata pool OOM。

## 4.15 分阶段实施与验收门禁

### 阶段 A：显式 API 正确性

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

### 阶段 B：自举安全核心

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

### 阶段 C：完整标准 ABI

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

### 阶段 D：动态拦截与生产灰度

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

## 4.16 性能护栏

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

