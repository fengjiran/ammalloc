# 第 13 章：测试与验证体系

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](11-security-hardening.md) · [下一章](13-engineering-and-release.md)  
> **本章目标**：建立覆盖功能、并发、失败、兼容性和性能回归的验证体系。  
> **适用范围**：单元测试、模型测试、并发、fault injection、fuzz、sanitizer、ABI 与 benchmark。  
> **核心 invariant**：测试必须覆盖声称路径，性能结论必须来自可复现实验而非主观判断。

验证体系必须证明四件事：API 行为正确、并发状态可证明、内存长期守恒、性能结论可复现。单元测试“运行未崩溃”不能证明分配器正确；单个平均 ns 也不能证明可部署。

## 13.1 验证金字塔

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

## 13.2 Compile-time 验证

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

## 13.3 单元测试组织

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

## 13.4 公共 ABI 测试

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

## 13.5 层级所有权与守恒测试

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

## 13.6 状态模型测试

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

## 13.7 并发测试

### 13.7.1 场景

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

### 13.7.2 同步控制

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

### 13.7.3 线性化与结果

对 lock-free/rseq/remote queue 定义线性化点；测试允许的结果集合。无法说明线性化点的结构不能因“压力测试没崩”而上线。

## 13.8 故障注入框架

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

## 13.9 Fuzz 与随机压力

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

## 13.10 Sanitizer 与动态工具

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

## 13.11 平台与工具链矩阵

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

## 13.12 Preload 与真实程序兼容

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

## 13.13 内存与碎片测试

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

## 13.14 性能基准体系

### 13.14.1 微基准

- SizeClass；
- ThreadCache hit；
- Transfer hit/miss；
- bitmap batch；
- PageMap hit/miss/range publish；
- PageCache exact/split/coalesce；
- PageAllocator mmap/huge cache；
- stats/sampling；
- hardening。

### 13.14.2 合成 workload

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

### 13.14.3 Trace replay

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

### 13.14.4 竞争者

相同 workload 比较：

- ammalloc；
- glibc malloc；
- TCMalloc；
- jemalloc。

严格记录版本、配置、page size、THP、background thread、per-CPU/arena 等，不使用默认配置差异得出不公平结论。

## 13.15 性能测量规范

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

## 13.16 性能回归门禁

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

## 13.17 CI 分层

### Presubmit

- format/build；
- focused unit；
- ABI compile；
- Debug/Release；
- 快速 ASan/UBSan；
- diff-based microbench smoke。

### Postsubmit

- full unit；
- TSan；
- fuzz short；
- 48/57-bit compile；
- GCC/Clang；
- benchmark stable host。

### Nightly

- long stress/fuzz；
- cgroup pressure；
- fork/preload；
- multi-shard；
- NUMA/THP；
- leak/accounting；
- aethermind trace。

### Release qualification

- 全平台矩阵；
- ABI compatibility；
- competitive benchmark；
- staged preload；
- failure injection；
- rollback rehearsal；
- artifact/signing。

## 13.18 结果工件与可追溯性

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

## 13.19 失败分级

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

## 13.20 分阶段实施

### 阶段 A

整理测试 reset、failpoint、ABI exerciser、ASan/UBSan、基准环境 manifest。

### 阶段 B

状态模型、deterministic concurrency、TSan、fuzz、accounting checker。

### 阶段 C

preload/平台/cgroup/NUMA/THP、trace replay、竞争者比较。

### 阶段 D

aethermind staging、生产 canary、长期回归趋势和自动二分。

每阶段退出条件是相应风险可自动复现和阻断，而不是测试数量达到某个数字。

