# 第 12 章：安全加固

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](10-observability-and-profiling.md) · [下一章](12-testing-and-validation.md)  
> **本章目标**：按运行 Profile 提供分层内存安全检测与利用缓解能力。  
> **适用范围**：pointer validation、freelist hardening、redzone、guard、quarantine 与安全配置。  
> **核心 invariant**：安全检查不破坏默认 fast path；高成本能力按 Profile 或采样启用。

分配器安全设计必须区分“始终成立的内存安全基线”和“可选的漏洞检测/利用缓解”。默认模式不能为了 3.8 ns 牺牲生命周期、整数安全或所有权；Hardened 模式则可以用可控的内存和延迟成本提高检测覆盖。

## 12.1 威胁模型

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

## 12.2 安全 Profile

| Profile | 目标 | 典型能力 |
|---|---|---|
| ReleaseFast | 生产性能默认 | 必要边界/生命周期/checked arithmetic |
| ReleaseChecked | 低成本增强 | sampled validation、pointer encoding、poison sampling |
| Hardened | 高风险服务 | canary、quarantine、guard sampling、严格 invalid free |
| Debug | 开发诊断 | 全量断言、bitmap/list 校验、poison、慢速守恒 |
| Sanitizer | 动态工具 | ASan/UBSan/TSan 专用布局与禁用冲突优化 |

Profile 由构建期能力和运行期策略共同决定。不能用 `NDEBUG` 单一宏隐式改变公共 ABI 或 descriptor 布局。

## 12.3 始终成立的安全不变量

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

## 12.4 Pointer 分类与 Free 验证

### 12.4.1 分类流程

~~~text
null -> success
  -> PageMap lookup
  -> validate descriptor lifetime/generation/state
  -> validate ptr within user allocation domain
  -> small: object-start/alignment/class validation
  -> large: exact user pointer / mapping metadata validation
  -> route owner
~~~

### 12.4.2 Unknown pointer

策略必须按部署模式区分：

- 显式 `am_free`：unknown pointer 是 API 违反，可 fail-fast/错误回调；
- preload fully-owned process：通常 fail-fast，防止跨 allocator domain；
- transition/mixed mode：只有存在可靠 ownership registry 时才允许系统 free fallback；
- 当前“PageMap miss 直接忽略”会掩盖泄漏和 domain bug，不应作为最终语义。

### 12.4.3 Interior pointer

小对象验证：

- ptr >= data base；
- ptr < data base + capacity * class size；
- offset % class size == 0；
- object index < capacity。

大对象要求 ptr 等于 recorded user pointer；page-aligned interior address不能被当成 allocation base。

### 12.4.4 Validation 分层

- fast release：依赖 trusted internal route + 必要字段；
- public free slow path：低成本边界；
- sampled checked：全 PageMap range/generation；
- Debug：bitmap、链、owner、arena 全验证。

## 12.5 Double Free 检测

### 12.5.1 Small object

对象从用户 free 到 Frontend/TransferCache 时 bitmap 仍为 allocated，不能依靠 Span bitmap立即检测重复 free。候选：

- ThreadCache/remote queue sampled membership check；
- object header/tag（有空间和 profile 时）；
- per-Span secondary state bitmap（Hardened）；
- quarantine membership；
- 最终回 Span bitmap 时检测 bit 已为 1。

默认模式至少在最终 bitmap free 检测；Hardened 模式提供更早检测。

### 12.5.2 Large object

- free 开始时通过 descriptor state CAS/owner lock 转 closing；
- 第二次 free 观察非 active state并 fail；
- PageMap clear 后 stale free 由 generation/retired metadata 识别；
- descriptor 不立即复用，避免 ABA 把旧 pointer 解释成新对象。

### 12.5.3 错误处置

默认推荐 fail-fast，而不是继续操作损坏 heap。提供固定 error code、地址、Span generation、thread/cpu 到 crash/event ring；禁止在损坏状态做复杂 heap dump。

## 12.6 Freelist Hardening

### 12.6.1 Pointer encoding

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

### 12.6.2 Safe-linking 边界

encoding 提高利用难度但不证明 pointer 合法。必须配合 range/alignment/owner 验证；若每次 fast path 都做完整 PageMap lookup，性能成本可能不可接受，应在 ReleaseChecked/Hardened 或 refill/trim 批量边界使用。

### 12.6.3 链完整性

- batch count 上限；
- tail 可达性 sampled 验证；
- Floyd cycle 检测仅 Debug/异常路径；
- object next 不指向自身；
- chain 对象同 class/owner；
- bulk 转换时不重复；
- corrupted chain 不继续写任意地址。

## 12.7 Poison、Junk Fill 与 Zeroing

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

## 12.8 Redzone 与 Canary

### 12.8.1 Small object

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

### 12.8.2 Canary

canary 由 secret、object address、generation、requested size 混合，避免全局固定值。保存位置不能覆盖用户合法 alignment/size；大对象可放 header/side metadata + tail。

### 12.8.3 限制

redzone 只能在 free/check 时发现，无法阻止越界发生；大幅增加内部碎片。默认采用 sampling 或 Hardened 专用 class。

## 12.9 Guarded Sampling

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

## 12.10 Quarantine

### 12.10.1 目标

延迟重用提高 UAF 可检测窗口，但会增加 RSS。必须同时限制：

- bytes；
- object count；
- per-class；
- per-thread/CPU；
- age；
- process/node/arena。

### 12.10.2 数据结构

使用固定 ring、intrusive FIFO 或 arena-backed bounded queue；不使用 deque/vector。对象在 quarantine 时仍不回 bitmap/PageCache，并计入独立 bytes。

### 12.10.3 压力

Soft pressure 缩短 quarantine；Hard/Critical drain，但 guard `PROT_NONE` sample 的元数据仍需安全释放。安全 profile 不得导致 cgroup OOM。

## 12.11 Metadata 保护

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

## 12.12 随机化

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

## 12.13 Reentrancy 与故障隔离

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

## 12.14 安全控制与默认值

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

## 12.15 安全可观测性

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

## 12.16 测试与 Fuzz

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

## 12.17 分阶段实施与验收

### 阶段 A：默认安全基线

1. checked arithmetic；
2. public free pointer/class/base validation；
3. stable descriptor generation；
4. 明确 unknown pointer 策略；
5. bootstrap-safe fail-fast；
6. 错误事件记录。

退出条件：已知 invalid/double/stale 路径不静默污染 heap；fast-path 护栏通过。

风险类型：正确性、安全、性能。

### 阶段 B：ReleaseChecked

1. pointer encoding；
2. sampled chain validation；
3. poison sampling；
4. large canary；
5. bounded quarantine；
6. 配置/统计。

退出条件：攻击面明显收窄；默认开销和 RSS 达标；pressure 可 drain。

风险类型：安全、性能、内存。

### 阶段 C：Hardened/Guarded

1. redzone/canary class；
2. guarded sampling；
3. metadata protection；
4. stack capture；
5. UAF crash record；
6. production canary。

退出条件：典型 overflow/UAF 可稳定捕获；sample/drop/成本可观测。

风险类型：安全、内存、性能、运维。

### 阶段 D：持续安全验证

1. fuzz farm；
2. sanitizer/nightly；
3. corpus regression；
4. aethermind profile 灰度；
5. 安全响应和版本策略。

退出条件：漏洞样本进入回归；安全 profile 可回滚；默认 ABI 不漂移。

风险类型：安全、工程、兼容性。

