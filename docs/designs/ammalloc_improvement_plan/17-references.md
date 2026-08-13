# 第 18 章：参考资料

> [总索引](README.md) · [上一章](16-implementation-principles.md) · [下一章](18-final-recommendations.md)  
> **本章目标**：维护设计决策所依赖的一手资料和仓库事实来源。  
> **适用范围**：TCMalloc、jemalloc、Linux、标准、论文及内部事实。  
> **核心 invariant**：仓库已验证事实优先；外部结论优先采用官方文档、标准和论文。

参考资料用于理解设计空间，不表示 ammalloc 应直接复制实现。任何采用都需结合本项目 4 KiB page、PageMap、bitmap、ABI 和 aethermind workload 验证。

## 18.1 TCMalloc

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

## 18.2 jemalloc

- [jemalloc Manual](https://jemalloc.net/jemalloc.3.html)：mallctl namespace、arena、tcache、dirty/muzzy decay、background thread、profiling和 extent hooks。

借鉴重点：

- typed control namespace；
- stats epoch；
- per-arena decay；
- background thread 动态控制；
- allocation sampling；
- arena/extent 策略。

注意其启动后台线程文档明确提示初始化循环依赖风险，ammalloc 必须坚持 bootstrap 后启用和确定性生命周期。

## 18.3 Linux 内核

- [Transparent Hugepage Support](https://docs.kernel.org/admin-guide/mm/transhuge.html)：`MADV_HUGEPAGE`、THP 策略、collapse/defrag 与内存放大。
- [HugeTLB Pages](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html)：显式 hugetlb pool、预留和限制。
- [Control Group v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)：`memory.high/max/events/stat/pressure/reclaim`。
- [NUMA Memory Policy](https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html)：default/preferred/bind/interleave 语义。
- Linux man pages：`mmap(2)`、`munmap(2)`、`madvise(2)`、`mbind(2)`、`set_mempolicy(2)`、`get_mempolicy(2)`、`rseq(2)`、`pthread_atfork(3)`。

## 18.4 标准与 ABI

- ISO C malloc family 和 alignment/zero-size 语义；
- ISO C++ replaceable global allocation/deallocation functions；
- Itanium C++ ABI（适用平台）；
- System V AMD64 ABI；
- AArch64 ELF ABI；
- glibc malloc hooks/interposition 现状与动态链接器文档。

实现前应引用项目支持工具链对应版本，而不是仅依赖摘要。

## 18.5 并发与内存模型

- ISO C++ memory model；
- Linux rseq ABI；
- acquire/release publication；
- epoch/RCU 文献；
- ABA/tagged pointer；
- lock-order verification；
- allocator-specific non-moving reclamation 研究。

## 18.6 内部事实来源

优先级：

1. 当前代码与可复现实验；
2. AGENTS.md 硬约束；
3. current-state design；
4. tests/benchmarks；
5. ADR；
6. future plan；
7. 外部资料。

文档与代码冲突时先记录冲突，再按已验证代码事实修正文档。

## 18.7 资料维护

- 记录访问日期/版本；
- 对内核特性标最低版本；
- 外部链接定期检查；
- ADR 引用具体资料和采纳/拒绝原因；
- 不复制大段外部文本；
- 性能数字只引用本项目可复现实验。

