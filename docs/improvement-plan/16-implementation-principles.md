# 第 17 章：关键实施原则

> **状态**: Draft（规划草案，未实施）

> [总索引](README.md) · [上一章](15-priority-and-risk-matrix.md) · [下一章](17-references.md)  
> **本章目标**：固化跨模块设计、实现、评审和发布必须遵循的原则。  
> **适用范围**：correctness、concurrency、performance、memory、bootstrap、API 与 release。  
> **核心 invariant**：任何优化都必须维持 ownership、并发和失败语义，并以测量证明收益。

## 17.1 正确性原则

1. **先定义状态和所有权，再选择锁或 atomic。**
2. **PageMap acquire/release 不替代 descriptor 生命周期。**
3. **每个对象、Span、Extent、Region 同时只有一个逻辑所有者。**
4. **失败原子：提交前旧状态完整，提交后新状态完整。**
5. **unmap 前撤销可发现性，metadata 回收晚于最后 reader。**
6. **Release 构建保留必要安全检查。**
7. **未知输入不能静默污染 heap。**

## 17.2 并发原则

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

## 17.3 热路径原则

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

## 17.4 内存与碎片原则

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

## 17.5 自举原则

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

## 17.6 API 与兼容原则

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

## 17.7 观测与验证原则

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

## 17.8 发布原则

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

## 17.9 决策问题清单

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

