# 第 14 章：工程化与发布

> [总索引](README.md) · [上一章](12-testing-and-validation.md) · [下一章](14-implementation-roadmap.md)  
> **本章目标**：建立可构建、可安装、可发布、可灰度和可回滚的工程体系。  
> **适用范围**：CMake target、依赖、符号、ABI 版本、安装包、CI、release train 与 runbook。  
> **核心 invariant**：发布工件可复现、ABI 可追踪、配置可灰度、故障可回滚。

allocator 是进程级基础设施，发布工程必须比普通工具库更严格：符号、初始化、配置、依赖、回滚和诊断任何一项失误都可能使目标进程无法启动。

## 14.1 当前工程基线与差距

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

## 14.2 Target 分层

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

## 14.3 核心依赖治理

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

## 14.4 符号与可见性

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

## 14.5 ABI 与版本

采用 SemVer 但明确：

- public ABI major；
- control schema version；
- stats schema version；
- arena handle version；
- build feature bitmap；
- allocator config fingerprint。

设置 library VERSION/SOVERSION；兼容 minor 只追加能力，不改变 struct 已有字段语义。C struct 使用 `struct_size`/`version`，opaque handle 不暴露 C++ layout。

## 14.6 构建 Profile

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

## 14.7 配置体系

### 14.7.1 类型

- compile capability；
- immutable startup geometry；
- runtime policy；
- action command；
- test-only failpoint。

### 14.7.2 解析

- 只在 bootstrap-safe 初始化期解析环境；
- 固定 key table；
- checked numeric/size/bool/enum；
- unknown/invalid 记录；
- 不使用 string/map；
- 配置快照不可变；
- 敏感生产配置允许禁用环境变量并由 API 注入。

### 14.7.3 输出

可查询 effective value、source、default、range、mutable、generation。性能报告必须包含 config fingerprint。

## 14.8 安装与包

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

## 14.9 平台能力探测

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

## 14.10 可复现构建

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

## 14.11 文档与仓库一致性

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

## 14.12 ADR 与不变量治理

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

## 14.13 Code review 与变更门禁

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

## 14.14 Release Pipeline

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

## 14.15 灰度与回滚

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

## 14.16 安全与供应链

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

## 14.17 运行手册

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

## 14.18 发布验收清单

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

## 14.19 分阶段实施

### 阶段 A：可维护开发库

移除 core spdlog、隔离 test macro、target/profile 分层、manifest 和文档检查。

### 阶段 B：稳定显式 API 包

visibility、VERSION/SOVERSION、install/export、ABI test、signed artifacts。

### 阶段 C：Preload-ready

version script、constructor/bootstrap、compat matrix、symbol audit、rollback。

### 阶段 D：生产基础设施

release train、aethermind canary、SBOM、安全响应、长期性能趋势。

每阶段风险：兼容性、工程、性能、运维。

